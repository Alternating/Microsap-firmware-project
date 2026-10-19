// oled.c — SSD1306 128x64 status display + VMU LCD mirror. Reconstructed from pico2maple
// flipped-bt (0x10002c18 oled_dma_init, 0x10002cd4 ssd1306_send, 0x10002d64 oled_init,
// 0x10002eb8 oled_refresh_kick, 0x10002ef4 vmu_lcd_upscale, 0x10003188/33b8/3544/3800/3938
// glyph painters). Alt layout wires I2C to GPIO4 (SDA) / GPIO5 (SCL).
//
// Framebuffer: heap block of 0x80E bytes. Bytes [0..0xD] hold the DMA command prefix; the
// display data follows as 1024 uint16_t entries (one per SSD1306 byte). The low byte of each
// entry is the pixel data; the high byte carries I2C control flags, so the DMA can stream the
// whole frame to I2C0->IC_DATA_CMD with a STOP flag (0x200) set on the final entry (byte 0x80C).
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "oled.h"
#include "../assets/p2m_assets.h"
#include "../assets/bootlogo.h"

#define SSD1306_ADDR 0x3C
#define OLED_I2C     i2c0            // TODO(verify @0x20002b38: i2c instance global)
#define FB_SIZE      0x80E
#define FB_DATA_OFF  0x0E            // first uint16_t data entry
#define FB_LAST_ENTRY 0x80C         // byte offset of the last data entry (gets STOP flag)
#define I2C_STOP_FLAG 0x200

// Board-config globals (resolved by the board-init module; alt layout = 4/5).
extern uint8_t g_oled_sda_pin;      // GPIO4
extern uint8_t g_oled_scl_pin;      // GPIO5

static uint8_t *g_oled_fb;          // @*DAT_10002e80 / *DAT_10002ca8
static int      g_oled_dma_ch;      // @*DAT_100022cc-ish
static volatile bool g_oled_enabled;   // set by core1 at the end of oled_init; read by core0

// Store one display byte into framebuffer entry `index` (low byte = data).
static inline void fb_put(unsigned index, uint8_t v)
{
    ((uint16_t *)(g_oled_fb + FB_DATA_OFF))[index] = v;
}

// Blit a glyph strip of n bytes into the framebuffer at raw byte offset `fb_off`
// (each source byte becomes one uint16_t entry). Mirrors the icon-painter inner loops.
static void fb_blit(uint32_t fb_off, const uint8_t *glyph, int n)
{
    if (!g_oled_fb) return;
    uint16_t *dst = (uint16_t *)(g_oled_fb + fb_off);
    for (int i = 0; i < n; i++) dst[i] = glyph[i];
}

// from 0x10002cd4 — push a command+data block to the panel over blocking I2C.
// Sends {0x21,col0,col1, 0x22,page0,page1} as six {0x80,cmd} control writes, then a single
// {0x40, data...} data write.
void ssd1306_send(const uint8_t *data, size_t len,
                  uint8_t col0, uint8_t col1, uint8_t page0, uint8_t page1)
{
    const uint8_t cmds[6] = { 0x21, col0, col1, 0x22, page0, page1 };
    for (int i = 0; i < 6; i++) {
        uint8_t pkt[2] = { 0x80, cmds[i] };
        i2c_write_blocking(OLED_I2C, SSD1306_ADDR, pkt, 2, false);
    }
    uint8_t *buf = malloc(len + 1);
    buf[0] = 0x40;
    memcpy(buf + 1, data, len);
    i2c_write_blocking(OLED_I2C, SSD1306_ADDR, buf, len + 1, false);
    free(buf);
}

// from 0x10002c18 — set up the framebuffer command prefix and the refresh DMA channel.
static void oled_dma_init(void)
{
    memset(g_oled_fb, 0, FB_SIZE);
    // Command prefix bytes the DMA replays before pixel data (col/page window = full screen).
    g_oled_fb[0] = 0x21;            // (DAT_10002cb0) column-address command stream header
    *(uint16_t *)(g_oled_fb + 3) = 0x40;
    // Preload the two bank-label glyph strips (static chrome) — see oled_dma_init @0x10002c18.
    fb_blit(0xCC, g_icon_attach_off_a, 0x20);   // right-column chrome, page 0
    fb_blit(0x1CC, g_icon_attach_off_b, 0x40);  // page 1

    g_oled_dma_ch = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(g_oled_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, i2c_get_dreq(OLED_I2C, true));
    dma_channel_configure(g_oled_dma_ch, &c,
                          &i2c_get_hw(OLED_I2C)->data_cmd,   // write: IC_DATA_CMD
                          (uint16_t *)(g_oled_fb + FB_DATA_OFF),
                          0, false);
}

// from 0x10002d64 — panel bring-up. `rotated` flips COM/segment scan (the "rotated_screen"
// builds). Command bytes transcribed from the init-table literals (DAT_10002e94..0x10002ea8).
void oled_init(bool rotated)
{
    g_oled_fb = malloc(FB_SIZE);

    i2c_init(OLED_I2C, 800000);        // 0x000c3500 baud config
    gpio_set_function(g_oled_sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(g_oled_scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(g_oled_sda_pin);
    gpio_pull_up(g_oled_scl_pin);
    sleep_ms(50);                       // 0x32

    // Init command sequence (values dumped from the binary init table).
    const uint8_t init_seq[] = {
        0xAE,                           // display off
        0x20, 0x00,                     // memory addressing mode: horizontal
        0x40,                           // display start line 0
        0xD5, 0x80,                     // clock divide / osc freq
        0xD9, 0xF1,                     // pre-charge period
        0xDB, 0x30,                     // VCOMH deselect
        0x81, 0xFF,                     // contrast max
        0xD3, 0x00,                     // display offset 0
        0xDA, 0x12,                     // COM pins config
        0xA4,                           // resume to RAM content
        0xA6,                           // normal (non-inverted)
        0x8D, 0x14,                     // charge pump enable
        0xA8, 0x3F,                     // multiplex ratio 63 (= 64 rows)
        (uint8_t)(rotated ? 0xC0 : 0xC8),   // COM output scan direction
        (uint8_t)(rotated ? 0xA0 : 0xA1),   // segment re-map
        0x2E,                           // deactivate scroll
        0xAF,                           // display on
    };
    for (unsigned i = 0; i < sizeof init_seq; i++) {
        uint8_t pkt[2] = { 0x80, init_seq[i] };
        i2c_write_blocking(OLED_I2C, SSD1306_ADDR, pkt, 2, false);
    }
    sleep_ms(50);

    // draw the boot splash (full-screen 128x64 1bpp) — replaces the original "Pico2Maple" logo
    ssd1306_send(g_bootlogo, 1024, 0, 0x7F, 0, 7);

    oled_dma_init();
    g_oled_enabled = true;

    // Periodic refresh: alarm pool repeating timer re-arms the DMA (DAT_10002eb0 µs period).
    // from 0x10002d64 tail: FUN_10009ec8(alarm, ..., oled_refresh_cb)   // TODO(verify period)
}

// Push the current framebuffer to the panel over blocking I2C. The framebuffer stores each
// display byte as a uint16_t (low byte = pixel data); extract the low bytes and send the full
// 128x64 frame. Simpler and more reliable than the DMA path for now.  (replaces 0x10002eb8)
void oled_present(void)
{
    if (!g_oled_fb) return;
    static uint8_t line[1024];
    const uint16_t *e = (const uint16_t *)(g_oled_fb + FB_DATA_OFF);
    for (int i = 0; i < 1024; i++) line[i] = (uint8_t)e[i];
    ssd1306_send(line, 1024, 0, 0x7F, 0, 7);
}

// from 0x10002eb8 — in the original a DMA kick: the caller never waited for the panel.
// oled_present is a blocking I2C push instead, ~11.5 ms for a frame at 800 kHz, and the kick's
// callers include core1's Maple reply path (VMU LCD and block writes, the flush before a
// GET_CONDITION reply), where the budget is ~100 us. So a kick only marks the frame stale and
// core0's loop pushes it (oled_service). That also makes core0 the only user of the I2C bus and
// of oled_present's staging buffer, which core0's layout toggle and core1 used to share.
static volatile bool s_present_pending;
void oled_refresh_kick(void)
{
    __dmb();                     // framebuffer writes before the flag
    s_present_pending = true;
}

// Core0 main loop: push the framebuffer if a kick is pending. A kick during the push re-arms it.
void oled_service(void)
{
    if (!g_oled_enabled || !s_present_pending) return;
    s_present_pending = false;
    __dmb();
    oled_present();
}

// from 0x10002ef4 — upscale the 48x32 1bpp VMU LCD into the framebuffer's main area.
// Functional reconstruction: the original is a dense bit-packing loop. The VMU LCD is 48x32,
// 6 bytes/row (MSB = leftmost pixel). Here each VMU pixel is written as an SSD1306 page bit.
// The console writes VMU art already turned 180°, because a VMU sits upside down in the
// controller; the framebuffer here is upright (the panel's own flip is oled_init's), so the
// frame is read turned back: pixel (row, col) comes from (31 - row, 47 - col). Bench, 2026-10-19:
// without it the game's art showed upside down.
// TODO(verify @0x10002ef4): exact scale factor and target offset against hardware.
void vmu_lcd_upscale(const uint8_t *lcd_fb)
{
    if (!g_oled_fb) return;
    for (int row = 0; row < 32; row++) {
        for (int col = 0; col < 48; col++) {
            int sr = 31 - row, sc = 47 - col;
            int bit = (lcd_fb[sr * 6 + (sc >> 3)] >> (7 - (sc & 7))) & 1;
            unsigned page = (unsigned)row >> 3;          // 32 rows -> 4 pages
            unsigned x = (unsigned)col;
            uint16_t *e = &((uint16_t *)(g_oled_fb + FB_DATA_OFF))[page * 128 + x];
            if (bit) *e |= (uint16_t)(1u << (row & 7));
            else     *e &= (uint16_t)~(1u << (row & 7));
        }
    }
}

// --- status glyph painters (write into fixed framebuffer page rows) ---

// from 0x10003188 — 0..9 digit, 10 = "GID". Banks 0/1 use label strips; 2..10 use digit glyphs.
void oled_draw_bank(unsigned bank)
{
    if (!g_oled_fb) return;
    switch (bank) {
    case 0:  fb_blit(0x2CC, g_glyph_bank1_a, 0x20); fb_blit(0x3CC, g_glyph_bank1_b, 0x40); break;
    case 1:  fb_blit(0x2CC, g_glyph_bank0_a, 0x20); fb_blit(0x3CC, g_glyph_bank0_b, 0x40); break;
    default: {
        // banks 2..10: 0x40-byte digit glyph split into two 0x20 strips on pages 2 and 3
        static const uint8_t *const digit[11] = {
            0, 0, g_glyph_bank2, g_glyph_bank3, g_glyph_bank4, g_glyph_bank5,
            g_glyph_bank6, g_glyph_bank7, g_glyph_bank8, g_glyph_bank9, g_glyph_bank10_gid,
        };
        if (bank > 10 || !digit[bank]) return;
        fb_blit(0x2CC, digit[bank], 0x20);
        fb_blit(0x3CC, digit[bank] + 0x20, 0x20);
        break;
    }
    }
}

// from 0x100033b8 — SD dirty/flush indicator (two 0x20 strips at pages 6/7, right column).
void oled_draw_sd_icon(bool dirty)
{
    if (!g_oled_fb) return;
    if (dirty) {
        fb_blit(0x6CE, g_icon_sd_state, 0x20);
        fb_blit(0x7CE, g_icon_sd_state, 0x20);   // TODO(verify @0x100033b8: second strip source)
    } else {
        memset(g_oled_fb + 0x6CE, 0, 0x20 * 2);
        memset(g_oled_fb + 0x7CE, 0, 0x20 * 2);
    }
}

// from 0x10003544 — storage medium icon (internal flash vs SD).
void oled_draw_storage_icon(bool internal)
{
    if (!g_oled_fb) return;
    fb_blit(0x6F6, internal ? g_icon_storage_int : g_icon_storage_sd, 0x20);
}

// from 0x10003800 — active-layout icon (pages 4/5). layout: 1 std, 2 twin-stick, 3 alt, 4 racing.
void oled_draw_layout_icon(unsigned layout)
{
    if (!g_oled_fb) return;
    const uint8_t *a, *b;
    switch (layout) {
    case 1:  a = g_icon_layout_std_a;    b = g_icon_layout_std_b;    break;
    case 2:  a = g_icon_layout_ts_a;     b = g_icon_layout_ts_b;     break;
    case 4:  a = g_icon_layout_racing_a; b = g_icon_layout_racing_b; break;
    default: a = g_icon_layout_alt;      b = NULL;                   break;
    }
    fb_blit(0x4CC, a, 0x20);
    if (b) fb_blit(0x5CC, b, 0x40);
}

// from 0x10003938 — device-attached indicator (pages 0/1).
void oled_draw_attached_icon(bool attached)
{
    if (!g_oled_fb) return;
    if (attached) {
        fb_blit(0xCC, g_icon_attach_on_a, 0x20);
    } else {
        fb_blit(0xCC, g_icon_attach_off_a, 0x20);
        fb_blit(0x1CC, g_icon_attach_off_b, 0x40);
    }
}

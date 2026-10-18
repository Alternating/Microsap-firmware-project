// maple.c — Maple bus engine, reconstructed from pico2maple flipped-bt (build "Mar 4 2026").
// Primary sources: FUN_10000298 (core1 main, LTO-inlined giant), FUN_10001550 (rx),
// FUN_10001660 (tx), FUN_100008f0/0x1000090c helpers; PIO programs @0x1005e4b4/0x1005e4f8
// (reconstructed byte-exact in maple.pio).
//
// Topology (verified from literal pools):
//   RX engine: PIO0 (0x50200000), SM0, program maple_rx (24 instr), clkdiv 1.0 (0x10000),
//              SHIFTCTRL=0x80890000 (FJOIN_RX | PUSH_THRESH=8 | OUT_SHIFTDIR_R | AUTOPUSH),
//              PINCTRL = IN_BASE=pin.  Wrap: offset+0 .. offset+23.
//   TX engine: PIO1 (0x50300000), SM0, program maple_tx (30 instr), clkdiv 0x0008e700
//              (= 8 + 231/256 ≈ 8.902 → ~16.85 MHz at 150 MHz sysclk → ~2 Mbps Maple cell),
//              SHIFTCTRL=0x50060000 (FJOIN_TX | PULL_THRESH=8 | AUTOPULL | OUT_SHIFTDIR_R
//              TODO(verify @0x10001bb0 const 0x50060000: exact field decode)),
//              PINCTRL = OUT_BASE=pin,OUT_COUNT=2, SET_BASE=pin,SET_COUNT=2, (SIDESET_BASE=pin+1
//              set but side-set unused).  Wrap: offset+0 .. offset+29.
//   Both Maple GPIOs carry function PIO1 (TX) by default; PIO0 reads raw input levels, so RX
//   needs no gpio muxing.  Pin base comes from the config global @0x20002a01 (alternate layout
//   build: GPIO6=SDCKA "Maple 1", GPIO7=SDCKB "Maple 2").

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "maple.h"
#include "maple.pio.h"   // generated: maple_tx_program / maple_rx_program
#include "maple_frames.h"   // decoded Maple response headers

// Controller GET_CONDITION reply: DATA_TRANSFER (cmd 8), 3 words (func + 8-byte condition),
// src AP 0x20 (main peripheral). Verified from the original: header DAT_10001b6c = 0x08002003.
#define MAPLE_HDR_DATA_COND_CONTROLLER 0x08002003u

// ---- Globals (addresses from the binary; names ours) --------------------------
// @0x20002a01: Maple GPIO base pin (u8, part of the board-config struct in .data)
extern uint8_t g_maple_pin;                       // = 6 in this flipped build
extern uint8_t g_storage_status;                  // 1 = no SD, 2 = SD mounted
// @0x2000fd4b: Maple port/unit number captured from the last host frame (bits 7:6 of AP)
static uint8_t g_maple_port;
// set to 1 by core1 once the Maple engine has finished initializing (core0 waits on it)
volatile uint8_t g_core1_ready;
// diagnostics: how many Maple frames the Dreamcast has sent us (printed by core0 heartbeat)
volatile uint32_t g_maple_frames, g_maple_dev_req, g_maple_get_cond;
// @0x20001668: attached sub-peripheral mask (P2M_SUBPERIPH_*); also patches sender AP on TX
uint8_t g_subperiph_mask;
// @0x2000e970: RX frame buffer (0x400 bytes: len,src,dst,cmd + payload + xor)
static uint8_t g_rx_buf[0x400];
// @0x2000f98c: TX staging buffer (header + byte-swapped payload + checksum)
static uint8_t g_tx_buf[0x400 + 8];
// @0x20005e2c / @0x20005e68: controller / mouse condition blocks.
// Controller cond is defined in dc_mapping.c (its producer); the rest live here (the engine owns them).
extern dc_controller_cond_t g_dc_controller_cond;
dc_mouse_cond_t g_dc_mouse_cond;
// @0x20006b20 (+ body) : current DEVICE_STATUS payload, rebuilt on layout switch
uint8_t g_devinfo_payload[112];
// keyboard condition block + feed queue (queue_init(q, 12, 5) with claimed spinlock, @core1 init)
uint8_t g_dc_kbd_cond[16];
queue_t g_kbd_queue;

// Cross-module (other reconstruction units)
extern void sd_ensure_dirs(void);                           // 0x100039d0
extern void maple_build_device_info_blocks(void *hdr, void *body, int func); // 0x1000066c
extern void vmu_build_device_info(void *hdr, void *body);   // 0x10000824
extern void vmu_init(uint8_t *vmu_buf);                     // 0x10002568
extern void vmu_load_bank(uint8_t *vmu_buf, int bank, int gameid, ...); // 0x10002948
extern void vmu_load_gameid_bank(uint8_t *vmu_buf, int flags);          // 0x10002aa0
extern void vmu_bank_cycle(uint8_t *vmu_buf, bool forward, uint32_t t_lo, uint32_t t_hi); // 0x10002ac4
extern void vmu_flush_dirty(uint8_t *vmu_buf, ...);         // 0x1000278c
extern void vmu_block_read(uint8_t *vmu_buf, uint32_t addr);// 0x10002748
extern void vmu_block_write(uint8_t *vmu_buf, uint32_t addr, const void *data, int words); // 0x100028a4
extern void vmu_lcd_write(uint8_t *vmu_buf, uint32_t addr, const void *data, int words);   // 0x1000290c
extern int  vmu_set_condition(uint8_t *vmu_buf, const void *cond, int len); // 0x10002bbc (buzzer)
extern int  vmu_gameid_store(uint8_t *vmu_buf, const void *payload, int len); // 0x10002b44
extern void vmu_storage_enable(int en);                     // 0x10003938
extern void vmu_invalidate(void);                           // 0x10002eb8
extern void vmu_media_info_build(void *v);                  // 0x10002250
extern int  vmu_lcd_render_notify(void *v);                 // 0x100028f4
extern void oled_init(bool rotated);          // 0x10002d64
extern int  rumble_get_cond(void *a, void *b);              // 0x10007c30
extern void rumble_set_block(void *a, void *b);             // 0x10007c14 (BLOCK_WRITE func 0x100)
extern void rumble_start(void *a, void *b);                 // 0x10007b60 (SET_CONDITION func 0x100)
extern void rumble_stop(void);                              // 0x10007c4c
extern void usb_announce(void *x);                          // 0x10007b24 TODO(verify semantics)
// settings (storage module)
extern int  settings_load(void *s, int x);                  // 0x10003e94
extern int  settings_save(void *s, int x);                  // 0x10003f48
extern void settings_defaults(void *s);                     // 0x10003fe4
extern void settings_set(void *s, int field, const void *v);// 0x10004020
extern int  settings_get(void *s, int field, void *out);    // 0x100040d0
extern int  settings_update_field(int field, const void *v, int x); // 0x10004120
extern int  settings_read_field(int field, void *v, int x); // 0x10004218
// config (mapping module)

// ---- Hotkey-side requests ------------------------------------------------------
// @0x2000????: pending bank-switch request (0 = none, +1/-1) — set by mapper hotkeys.
static volatile int8_t g_bank_switch_req;

// from 0x100008f0 (FUN_100008f0)
void maple_request_bank_switch(int dir)
{
    if ((dir == 1 || dir == -1) && g_bank_switch_req == 0)
        g_bank_switch_req = (int8_t)dir;
}

// from 0x1000090c (FUN_1000090c): every g_hotplug_interval_us, toggle storage attach state —
// detaches/attaches the VMU storage function so the DC re-enumerates after SD insert/remove.
static uint64_t g_hotplug_deadline;      // @DAT_1000097c
static uint8_t  g_storage_state;         // @DAT_10000980 (bit1 = attached)
extern uint32_t g_hotplug_interval_us;   // @DAT_10000988
void maple_storage_hotplug_tick(void)
{
    if (time_us_64() < g_hotplug_deadline) return;
    if (g_storage_state & 2) {
        g_storage_state &= ~2;
        rumble_stop();                   // TODO(verify @0x1000090c): call is 0x10007c4c
        vmu_storage_enable(0);
        vmu_invalidate();
    } else {
        usb_announce(/*@DAT_10000984*/ 0);
        g_storage_state |= 2;
        vmu_storage_enable(1);
        vmu_invalidate();
    }
    g_hotplug_deadline = time_us_64() + g_hotplug_interval_us;
}

// ---- RX -----------------------------------------------------------------------
// from 0x10001550 (FUN_10001550).  Frame arrives as PIO-autopushed 8-bit groups; the start
// pattern leaves the stream misaligned by 2 bits, so bytes are rebuilt as
// (prev & 3) << 6 | cur >> 2.  First synchronized byte whose FIFO word matches
// (w & 0xfc) == 0x84 begins the frame.  Layout: [len][src][dst][cmd] + len*4 + xor.
int maple_rx_frame(maple_pio_ctx_t *rx)
{
    PIO pio = rx->pio;
    uint sm = rx->sm;

    // restart RX SM at program start and clear its FIFO (XOR-alias FJOIN_RX toggle ×2)
    pio->sm[sm].instr = pio_encode_jmp(rx->offset) & 0x1f;    // raw: SM_INSTR = offset&0x1f
    hw_xor_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
    hw_xor_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
    hw_set_bits(&pio->ctrl, 1u << (PIO_CTRL_SM_RESTART_LSB + sm));
    pio->sm[sm].instr = pio_encode_jmp(rx->offset) & 0x1f;

    // release SDCKB back to input with pull-up (host drives the bus now)
    gpio_set_pulls(g_maple_pin + 1, true, false);
    gpioc_bit_oe_put(g_maple_pin + 1, 0);   // RP2350 GPIO coprocessor single-bit OE clear
                                            // TODO(verify @0x10001594): OUT vs OE register

    int idx = 0, expect = 5;
    uint8_t prev = 0, xorsum = 0;
    bool synced = false;
    for (;;) {
        while (pio->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + sm)))
            ;                                            // busy-poll (core1 dedicated)
        uint32_t w = pio->rxf[sm];
        uint8_t raw = (uint8_t)w;
        uint8_t byte = (uint8_t)(((prev & 3) << 6) | (raw >> 2));
        if (!synced) {
            if ((w & 0xfc) == 0x84) {                   // start-pattern residue signature
                synced = true; idx = 0; expect = 5; prev = raw;
            } else {                                     // garbage: clear FIFO, resync
                hw_xor_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
                hw_xor_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
                hw_set_bits(&pio->ctrl, 1u << (PIO_CTRL_SM_RESTART_LSB + sm));
            }
            continue;
        }
        g_rx_buf[idx++] = byte;
        if (idx == 1)
            expect = (int16_t)byte * 4 + 5;             // len*4 payload + 4 hdr + 1 xor
        else if (idx == expect)
            return (xorsum == byte) ? 0 : -1;           // final byte is the checksum
        xorsum ^= byte;
        prev = raw;
    }
}

// ---- TX -----------------------------------------------------------------------
// from 0x10001660 (FUN_10001660)
void maple_tx_frame(maple_pio_ctx_t *tx, const maple_frame_t *frame)
{
    PIO pio = tx->pio;
    uint sm = tx->sm;
    uint pin = g_maple_pin;
    uint8_t *hdr = (uint8_t *)&((maple_frame_t *)frame)->header;

    // Patch AP bytes: port number into bits 7:6 of src+dst; if dst requests sub-peripherals
    // (bit6 of original src byte — TODO(verify @0x10001660 bit test)), OR in our mask.
    uint8_t port_bits = (uint8_t)(g_maple_port << 6);
    uint8_t src = hdr[1];
    hdr[2] = port_bits | (hdr[2] & 0x3f);
    hdr[1] = port_bits | (src & 0x3f);
    if (src & 0x20)                                    // "I am a sub-peripheral" flag path
        hdr[1] = port_bits | (src & 0x20) | g_subperiph_mask;

    // Route both pins to the TX PIO's gpio function (func computed from PIO base),
    // preset both lines high as outputs, enable the SM.
    uint func = (((uintptr_t)pio + 0xafe00000u) >> 20) + 6;  // PIO0→6, PIO1→7 (RP2350)
    gpio_set_function(pin + 1, func);
    pio_sm_set_pins_with_mask(pio, sm, (1u << pin) | (1u << (pin + 1)),
                                        (1u << pin) | (1u << (pin + 1)));
    pio_sm_set_pindirs_with_mask(pio, sm, (1u << pin) | (1u << (pin + 1)),
                                           (1u << pin) | (1u << (pin + 1)));
    hw_set_bits(&pio->ctrl, 1u << sm);

    // Stage: header word, then payload words byte-reversed, then XOR checksum byte.
    uint32_t *stage = (uint32_t *)g_tx_buf;
    uint32_t hdrw = frame->header;
    stage[0] = hdrw;
    uint len = hdrw & 0xff;
    uint8_t x = (uint8_t)(hdrw ^ (hdrw >> 8) ^ (hdrw >> 16) ^ (hdrw >> 24));
    int total;
    if (len == 0) {
        g_tx_buf[4] = x;
        total = 5;
    } else {
        const uint8_t *p = frame->payload;
        for (uint i = 0; i < len; i++, p += 4) {
            stage[1 + i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                           (uint32_t)p[2] << 8  | p[3];
            x ^= p[0] ^ p[1] ^ p[2] ^ p[3];
        }
        g_tx_buf[4 + len * 4] = x;
        total = 5 + len * 4;
    }

    // Feed one byte per FIFO word (<<24; TX program consumes 8 bits per word via autopull
    // threshold 8), then wait for true completion via the FDEBUG TXSTALL handshake.
    for (int i = 0; i < total; i++) {
        while (pio->fstat & (1u << (PIO_FSTAT_TXFULL_LSB + sm)))
            ;
        pio->txf[sm] = (uint32_t)g_tx_buf[i] << 24;
    }
    uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
    while (!(pio->fdebug & stall)) ;
    pio->fdebug = stall;                 // clear
    while (!(pio->fdebug & stall)) ;     // set again = SM drained and stalled on pull

    // Return the bus to idle/RX: SDCKB driven high momentarily then released to SIO input
    // with pull, both pins back to inputs, SM disabled.
    gpioc_bit_out_put(pin + 1, 1);       // TODO(verify @0x100017a4): OUT=1 then OE=0 order
    gpioc_bit_oe_put(pin + 1, 0);
    gpio_set_function(pin + 1, GPIO_FUNC_SIO);
    gpio_set_pulls(pin + 1, false, true); // TODO(verify @0x100017d8): args read as pull-DOWN
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 2, false);
    hw_clear_bits(&pio->ctrl, 1u << sm);
}

// ---- Loopback self-test ---------------------------------------------------------
// TX and RX share the Maple pins (RX taps raw input levels), so a transmitted frame should be
// captured by our own RX engine. Validates both PIO programs + framing + checksum on real
// hardware without a Dreamcast. Expected decode for the ACK test frame: 00 01 00 07 06.
static void maple_loopback_test(maple_pio_ctx_t *rx, maple_pio_ctx_t *tx)
{
    PIO pio = rx->pio;
    uint sm = rx->sm;

    // restart RX at program start + clear its FIFO (same prologue as maple_rx_frame)
    pio->sm[sm].instr = pio_encode_jmp(rx->offset) & 0x1f;
    hw_xor_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
    hw_xor_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
    hw_set_bits(&pio->ctrl, 1u << (PIO_CTRL_SM_RESTART_LSB + sm));
    pio->sm[sm].instr = pio_encode_jmp(rx->offset) & 0x1f;
    gpio_set_pulls(g_maple_pin + 1, true, false);

    maple_frame_t f = { .header = MAPLE_HDR_ACK_MAIN, .payload = 0 };
    maple_tx_frame(tx, &f);                       // blocking; RX FIFO (8 deep) holds the echo

    uint32_t w[16];
    int n = 0;
    uint64_t dl = time_us_64() + 50000;
    while (n < 16 && time_us_64() < dl) {
        if (!(pio->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + sm))))
            w[n++] = pio->rxf[sm];
    }
    printf("[selftest] loopback raw words (%d):", n);
    for (int i = 0; i < n; i++) printf(" %02x", (unsigned)(w[i] & 0xff));
    printf("\n");
    if (n >= 6 && (w[0] & 0xfc) == 0x84) {
        static const uint8_t expect[5] = { 0x00, 0x01, 0x00, 0x07, 0x06 };
        uint8_t prev = (uint8_t)w[0], b[15];
        int k = 0;
        for (int i = 1; i < n && k < 15; i++) {
            uint8_t raw = (uint8_t)w[i];
            b[k++] = (uint8_t)(((prev & 3) << 6) | (raw >> 2));
            prev = raw;
        }
        printf("[selftest] decoded:");
        for (int i = 0; i < k; i++) printf(" %02x", b[i]);
        printf("  -> %s\n", (k >= 5 && memcmp(b, expect, 5) == 0)
               ? "PASS (Maple RX+TX verified)" : "MISMATCH (framing/bit-order bug)");
    } else {
        puts("[selftest] FAIL: RX captured nothing/no sync - Maple PIO RX or TX broken");
    }
}

// ---- Core1: engine init + protocol loop ----------------------------------------
// from 0x10000298 (FUN_10000298).  The 128KB VMU image + response staging buffers live on
// core1's stack (frame ≈ 0x20630 bytes).
void maple_core1_main(void)
{
    static maple_pio_ctx_t rx_ctx = { pio0, 0, 0 };
    static maple_pio_ctx_t tx_ctx = { pio1, 0, 0 };
    // 128KB VMU image: static (.bss) rather than on core1's stack (the original kept it on the
    // stack, which needs a custom core1 stack region; a static buffer is cleaner for our build).
    static uint8_t vmu_buf[131264];      // auStack_20468
    uint8_t settings[256];               // auStack_20568
    uint8_t lcd_info[15], media_hdr[28], blk_resp[524], allstat[192];

    // (core0 is the lockout victim — set up in main before launch — so core1 can pause it
    // around flash writes; core1 itself is not a victim.)
#ifndef DEBUG_UART_OLED              // OLED shares GPIO4/5 with the debug UART; skip it when debugging
    oled_init(true);                 // 180° flip so the display reads upright when plugged into
                                     // the Dreamcast (connector at the bottom). // 0x10002d64
#else
    puts("[core1] OLED disabled (debug UART on GPIO4/5)");
#endif
    sd_ensure_dirs();
    printf("[core1] SD status=%u (1=no SD, 2=mounted)\n", g_storage_status);

    // Maple pins: pull-ups on, SDCKB SIO-output-disabled (bus idles pulled high)
    uint pin = g_maple_pin;
    gpio_pull_up(pin + 1);               // 0x10008fc4(pin+1) TODO(verify single-arg identity)
    gpio_set_pulls(pin, true, false);
    gpio_set_pulls(pin + 1, true, false);

    // Device info blocks: controller (per active layout) + VMU.
    // NOTE: pass NULL for the ref pair — our dispatch uses explicit headers; the original wrote
    // the pair at devinfo-8 (adjacent globals), which here would underflow into unrelated .bss.
    // The VMU body is 112 bytes and needs a real static home (a 28-byte stack buffer overflowed).
    static uint8_t vmu_devinfo_body[112];
    static uint8_t vmu_devinfo_ref[8];           // {header, body*} pair (vmu_devinfo_ref_t)
    maple_build_device_info_blocks(NULL, g_devinfo_payload, MAPLE_FUNC_CONTROLLER);
    vmu_build_device_info(vmu_devinfo_ref, vmu_devinfo_body);

    // Condition blocks
    g_dc_controller_cond.func = 0x01000000;          // FUNC_CONTROLLER big-endian
    g_dc_controller_cond.buttons = 0xffff;
    memset(&g_dc_controller_cond.joyx, 0x80, 4);
    g_dc_mouse_cond.func = 0x00020000;               // FUNC_MOUSE big-endian (0x200)
    g_dc_mouse_cond.buttons = 0xff;
    g_dc_mouse_cond.dx = g_dc_mouse_cond.dy = g_dc_mouse_cond.wheel = 0x200;
    *(uint32_t *)g_dc_kbd_cond = 0x40000000;         // FUNC_KEYBOARD big-endian

    // keyboard event queue: 12-byte elements, depth 5, own spinlock
    queue_init_with_spinlock(&g_kbd_queue, 12, 5, spin_lock_claim_unused(true));

    puts("[core1] VMU init...");
    vmu_init(vmu_buf);
    puts("[core1] VMU ready");
    // NOTE: the status screen (bank/INT/SD glyphs) is drawn into the framebuffer but NOT
    // presented yet — our reconstructed glyph offsets wrap across page boundaries and render
    // garbled. Keep the clean splash until the OLED layout is corrected with hardware feedback.
    // TODO(oled): fix glyph offsets / page wrapping, then present the status screen here.
    g_subperiph_mask |= P2M_SUBPERIPH_VMU;
    usb_announce(/*DAT_10001b80*/ 0);

    // Settings load + custom-map load now live on core0 in control_init(); core1 just loads the
    // default VMU bank. The gameid/bank selection re-runs once control_init() sets the prefs and
    // bumps g_devinfo_rebuild. (See control.c; this split is a Path-B change from the original,
    // which did all of this on core1.)
    (void)settings;
    vmu_load_bank(vmu_buf, 0, false);

    // ---- PIO bring-up (exact register values preserved) ----
    rx_ctx.offset = pio_add_program(pio0, &maple_rx_program);
    pio_sm_set_consecutive_pindirs(pio0, 0, pin, 2, false);
    {
        pio_sm_config c = {0};
        c.clkdiv    = 0x00010000;                                   // 1.0
        c.execctrl  = (rx_ctx.offset << 7) | ((rx_ctx.offset + 23) << 12);
        c.shiftctrl = 0x80890000;  // FJOIN_RX | PUSH_THRESH=8 | OUT_SHIFTDIR | AUTOPUSH
        c.pinctrl   = (uint)pin << 15;                              // IN_BASE
        pio_sm_init(pio0, 0, rx_ctx.offset, &c);
    }
    hw_set_bits(&pio0->sm[0].pinctrl, 0);  // raw decomp: sm[0].pinctrl=(…&~0x1f)+2 — see note
    // (original also does: pio0->sm[0].reg34 adjust + pio0->ctrl |= 1 — RX SM enabled)
    hw_set_bits(&pio0->ctrl, 1u << 0);

    tx_ctx.offset = pio_add_program(pio1, &maple_tx_program);
    pio_sm_set_consecutive_pindirs(pio1, 0, pin, 2, false);
    gpio_set_function(pin, 7);             // both pins muxed to PIO1 (TX) permanently
    gpio_set_function(pin + 1, 7);
    {
        pio_sm_config c = {0};
        c.clkdiv    = 0x0008e700;                                   // ≈8.902
        c.execctrl  = ((tx_ctx.offset << 7) | ((tx_ctx.offset + 29) << 12)) & 0xe0ffffff
                      | ((uint)pin << 24);  // TODO(verify): low 5 bits of EXECCTRL hi-byte
        c.shiftctrl = 0x50060000;  // FJOIN_TX | PULL_THRESH=8 | AUTOPULL | shiftdir
        c.pinctrl   = ((uint)(pin + 1) << 10) /*side base (unused)*/ |
                      ((uint)pin << 5) /*SET_BASE*/ | pin /*OUT_BASE*/ |
                      (2u << 26) /*SET_COUNT*/ | (2u << 20) /*OUT_COUNT*/;
        pio_sm_init(pio1, 0, tx_ctx.offset, &c);
    }
    hw_set_bits(&pio1->ctrl, 1u << 0);
    hw_clear_bits(&pio1->ctrl, 1u << 0);   // enable+disable: TX armed but idle

    memset(g_rx_buf, 0, 0x400);
    static uint8_t g_storage_announce;     // @DAT_10001bb0
    g_storage_announce = 1;
    printf("[core1] Maple PIO up on GPIO%u/%u\n", pin, pin+1);
    maple_loopback_test(&rx_ctx, &tx_ctx);       // hear our own TX: validates RX+TX end-to-end
    puts("[core1] entering protocol loop (waiting for Dreamcast)");
    g_core1_ready = 1;              // signal core0 that the Maple engine is up

    // ---- Protocol loop ----
    uint32_t frames_lo = 0, frames_hi = 0;
    uint64_t bank_debounce_deadline = 0, autosave_deadline = 0;
    bool autosave_armed = false;

    for (;;) {
        // (a) pending VMU bank switch (debounced DAT_10001bc4 µs)
        if (g_bank_switch_req == 0) {
            // (b) SD flush tick: after >99 frames idle-ish, flush dirty VMU blocks
            //     (raw: if (*DAT_10001bc0 && frames>99) { vmu_flush_dirty(); hotplug_tick(); })
        } else if (/* debounce deadline reached */ time_us_64() >= bank_debounce_deadline) {
            vmu_bank_cycle(vmu_buf, g_bank_switch_req == 1, 0, 0);
            char b; settings_get(settings, 1, &b);
            settings_set(settings, 8, /* gid flag according to b=='\n' check */ 0);
            settings_save(settings, 1);
            g_bank_switch_req = 0;
        }

        // (c) layout switched? → rebuild controller device-info after a short delay
        extern volatile int16_t g_active_layout, g_devinfo_rebuild;
        if (g_devinfo_rebuild) {
            // delayed by DAT_10001bc4 µs then:
            maple_build_device_info_blocks(NULL, g_devinfo_payload, g_devinfo_rebuild);
            g_devinfo_rebuild = 0;
        }

        // (d) receive one frame; on error send nothing (original: tx "resend"? — raw shows
        //     FUN_10001660 with DAT_10001ee8 header on rx error TODO(verify @0x10001c06))
        if (maple_rx_frame(&rx_ctx) == -1) {
            maple_frame_t err = { .header = /*DAT_10001ee8*/ 0, .payload = 0 };
            maple_tx_frame(&tx_ctx, &err);
            continue;
        }
        frames_lo++;
        g_maple_frames++;

        const uint8_t *f = g_rx_buf;     // [len][src][dst][cmd]
        uint8_t len = f[0], dst = f[2], cmd = f[3];
        uint32_t func = *(const uint32_t *)(f + 4);   // first payload word (host order)
        bool to_vmu    = (dst & 1) && (g_subperiph_mask & P2M_SUBPERIPH_VMU);
        bool to_rumble = (dst & 2) && (g_subperiph_mask & P2M_SUBPERIPH_RUMBLE);
        maple_frame_t out = {0};

        switch (cmd) {
        case MAPLE_CMD_DEVICE_REQUEST:
            g_maple_dev_req++;
            if (dst & 0x40) g_maple_port = dst >> 6;   // capture port addressing
            if (!(dst & 3)) {                           // main device
                out.header = MAPLE_HDR_DEVICE_STATUS_MAIN;   // cmd 5, 28 words (112B devinfo)
                out.payload = g_devinfo_payload;
                maple_tx_frame(&tx_ctx, &out);
            } else if (to_vmu) {
                maple_tx_frame(&tx_ctx, /*vmu devinfo frame*/ &out);
            } else if (to_rumble) {
                maple_tx_frame(&tx_ctx, /*purupuru devinfo @0x100a2994*/ &out);
            }
            break;
        case MAPLE_CMD_ALL_STATUS_REQ:
            if (to_vmu || to_rumble) break;            // sub-devices don't answer (raw gate)
            out.header = MAPLE_HDR_ALL_STATUS; out.payload = allstat;
            maple_tx_frame(&tx_ctx, &out);
            break;
        case MAPLE_CMD_GET_CONDITION:
            g_maple_get_cond++;
            if (frames_lo > 100) vmu_flush_dirty(vmu_buf, frames_lo, 0);
            if (dst & 0x20) {                          // addressed to main device
                if (func == MAPLE_FUNC_CONTROLLER) {
                    out.header = MAPLE_HDR_DATA_COND_CONTROLLER;
                    out.payload = (uint8_t *)&g_dc_controller_cond;
                    maple_tx_frame(&tx_ctx, &out);
                } else if (func == MAPLE_FUNC_MOUSE) {
                    maple_tx_frame(&tx_ctx, /*mouse cond*/ &out);
                    g_dc_mouse_cond.dx = g_dc_mouse_cond.dy = 0x200;
                    g_dc_mouse_cond.wheel = 0x200;
                } else if (func == MAPLE_FUNC_KEYBOARD) {
                    // pull latest kbd report from queue into the condition block (0x100013c0)
                    maple_tx_frame(&tx_ctx, /*kbd cond*/ &out);
                }
            }
            break;
        case MAPLE_CMD_GET_MEDIA_INFO:
            if (to_vmu && func == MAPLE_FUNC_STORAGE) {
                vmu_media_info_build((void*)vmu_buf);
                out.payload = media_hdr; maple_tx_frame(&tx_ctx, &out);
            } else if (to_vmu && func == MAPLE_FUNC_LCD) {
                out.payload = lcd_info; maple_tx_frame(&tx_ctx, &out);
            } else if (to_rumble && func == MAPLE_FUNC_VIBRATION) {
                maple_tx_frame(&tx_ctx, /*static rumble media info*/ &out);
            }
            break;
        case MAPLE_CMD_BLOCK_READ:
            if (to_vmu && func == MAPLE_FUNC_STORAGE) {
                vmu_block_read(vmu_buf, *(const uint32_t *)(f + 8));
                out.payload = blk_resp; maple_tx_frame(&tx_ctx, &out);
                frames_lo = 0;
            } else if (to_rumble && func == MAPLE_FUNC_VIBRATION) {
                if (rumble_get_cond(0, 0)) maple_tx_frame(&tx_ctx, /*err*/ &out);
                maple_tx_frame(&tx_ctx, /*rumble cond*/ &out);
            }
            break;
        case MAPLE_CMD_BLOCK_WRITE:
            if (to_vmu && func == MAPLE_FUNC_STORAGE) {
                vmu_block_write(vmu_buf, *(const uint32_t *)(f + 8), f + 12, len - 2);
                goto ack;
            } else if (to_vmu && func == MAPLE_FUNC_LCD) {
                vmu_lcd_write(vmu_buf, *(const uint32_t *)(f + 8), f + 12, len - 2);
                vmu_lcd_render_notify((void*)vmu_buf);
                goto ack;
            } else if (to_rumble && func == MAPLE_FUNC_VIBRATION) {
                rumble_set_block(0, 0);
                goto ack;
            }
            break;
        case MAPLE_CMD_GET_LAST_ERROR:                  // write sync
            if (to_vmu && func == MAPLE_FUNC_STORAGE) { frames_lo = 0; goto ack; }
            break;
        case MAPLE_CMD_SET_CONDITION:
            if (to_vmu) {                               // clock function: buzzer
                if (vmu_set_condition(vmu_buf, f + 8, len)) break;
                goto ack;
            } else if (to_rumble && func == MAPLE_FUNC_VIBRATION) {
                rumble_start(0, 0);
                goto ack;
            }
            break;
        case MAPLE_CMD_GAMEID:                          // OpenMenu/GDEMU GameID (custom)
            if (!to_vmu) break;
            if (vmu_gameid_store(vmu_buf, f + 4, len) != 0)
                goto ack;
            // ack_then_gid: acknowledge, then switch to the per-game VMU bank if gameid mode is on
            maple_tx_frame(&tx_ctx, /*ACK 0x07000100*/ &out);
            { char en = 0;
              if (settings_read_field(8, &en, true) && en) vmu_load_gameid_bank(vmu_buf, 1); }
            break;
        ack:
            out.header = 0x07000100; out.payload = 0;
            maple_tx_frame(&tx_ctx, &out);
            break;
        default:
            break;
        }

        // reset for next frame: clear cmd byte, clear RX FIFO (XOR-alias toggle)
        g_rx_buf[0] = 0;
        hw_xor_bits(&pio0->sm[0].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
        hw_xor_bits(&pio0->sm[0].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
    }
}

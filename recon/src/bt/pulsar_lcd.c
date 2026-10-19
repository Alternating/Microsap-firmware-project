// pulsar_lcd.c — slot 1's LCD frames, out to the screen of the VMU docked in a Pulsar.
// Story 025. Protocol per embedded_dreamcast/docs/host_integration.md, "LCD frame channel".
#include <string.h>
#include "btstack.h"
#include "hardware/sync.h"
#include "pulsar_lcd.h"

#define LCD_BYTES      0xC0
#define LCD_CHUNK_ROWS 8
#define LCD_CHUNK      (LCD_CHUNK_ROWS * 6)        // 8 rows of 6 bytes, behind a row offset
#define LCD_WHOLE_MTU  (LCD_BYTES + 3)             // one 192-byte write plus the ATT header
// The Pulsar draws one frame every ~105 ms and keeps the newest. Sending faster only lengthens
// the connection events the HID reports share, so a frame goes at most every 50 ms.
#define LCD_MIN_GAP_MS 50

// Mailbox: written on core1 from the Maple reply, read on core0. One memcpy under the lock.
static spin_lock_t *s_lock;
static uint8_t s_box[LCD_BYTES];
static bool s_full;

// Core0: the frame in flight, frozen once its first write goes.
static uint8_t s_frame[LCD_BYTES];
static bool s_have;                 // s_frame holds a frame (sent or in flight)
static int  s_next = -1;            // next chunk 0-3; -1 none in flight
static bool s_whole;
static uint32_t s_last_ms;

volatile uint32_t g_pulsar_lcd_in, g_pulsar_lcd_sent;

void pulsar_lcd_init(void)
{
    s_lock = spin_lock_init(spin_lock_claim_unused(true));
}

// Their instance's lcd op: frames exactly as the console wrote them. The spec wants them
// unrotated; the Pulsar turns them for the VMU sitting upside down in it.
static void sink_lcd(void *ctx, const uint8_t fb[LCD_BYTES])
{
    (void)ctx;
    uint32_t irq = spin_lock_blocking(s_lock);
    memcpy(s_box, fb, LCD_BYTES);
    s_full = true;
    spin_unlock(s_lock, irq);
    g_pulsar_lcd_in++;
}

const vmu_display_t g_vmu_display_pulsar = { .lcd = sink_lcd };

void pulsar_lcd_service(uint16_t con, uint16_t value_handle, uint16_t mtu)
{
    if (s_next < 0) {
        uint32_t now = btstack_run_loop_get_time_ms();
        if (!s_full || now - s_last_ms < LCD_MIN_GAP_MS) return;
        uint32_t irq = spin_lock_blocking(s_lock);
        memcpy(s_frame, s_box, LCD_BYTES);
        s_full = false;
        spin_unlock(s_lock, irq);
        s_have = true;
        s_whole = mtu >= LCD_WHOLE_MTU;
        s_next = 0;
        s_last_ms = now;
    }
    while (s_next >= 0) {
        uint8_t chunk[1 + LCD_CHUNK];
        uint8_t *p = s_frame;
        uint16_t n = LCD_BYTES;
        if (!s_whole) {
            chunk[0] = (uint8_t)(s_next * LCD_CHUNK_ROWS);     // offset 0 starts a frame
            memcpy(chunk + 1, s_frame + s_next * LCD_CHUNK, LCD_CHUNK);
            p = chunk;
            n = sizeof chunk;
        }
        uint8_t st = gatt_client_write_value_of_characteristic_without_response(con, value_handle, n, p);
        if (st == GATT_CLIENT_BUSY) return;                     // the rest at the next tick
        // Any other failure drops the frame: the screen is decoration, never worth an input.
        if (st != ERROR_CODE_SUCCESS) { s_next = -1; return; }
        if (s_whole || ++s_next == 4) { s_next = -1; g_pulsar_lcd_sent++; }
    }
}

void pulsar_lcd_reset(void)
{
    s_next = -1;
    // The Pulsar's own animation resumes while no host draws; a static game screen sends no new
    // frame, so the next link gets the last one again unless a newer one is waiting.
    uint32_t irq = spin_lock_blocking(s_lock);
    if (!s_full && s_have) { memcpy(s_box, s_frame, LCD_BYTES); s_full = true; }
    spin_unlock(s_lock, irq);
}

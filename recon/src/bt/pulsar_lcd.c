// pulsar_lcd.c — slot 1's LCD frames, out to the screen of the VMU docked in a Pulsar.
// Story 025. Protocol per embedded_dreamcast/docs/host_integration.md, "LCD frame channel".
#include <string.h>
#include "btstack.h"
#include "hardware/sync.h"
#include "pulsar_lcd.h"
#include "pulsar_screen.h"
#include "maple/pulsar_card.h"

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
static int  s_next = -1;            // next chunk 0-3; -1 none in flight
static bool s_whole;
static uint32_t s_last_ms;
// Core0: the console's newest frame, kept once taken. A game that has parked its art writes no
// fresh frame when the loading screen gives the screen back, so this is what goes back up.
static uint8_t s_art[LCD_BYTES];
static bool s_have_art;
// The pull's progress as last drawn, and as the link now reports it.
static unsigned s_done, s_total, s_shown_done, s_shown_total;
static bool s_pending;              // something new to put up
static bool s_ours;                 // the screen holds one we drew, and no console art since

volatile uint32_t g_pulsar_lcd_in, g_pulsar_lcd_sent;

void pulsar_lcd_init(void)
{
    s_lock = spin_lock_init(spin_lock_claim_unused(true));
}

// Their instance's lcd op: frames exactly as the console wrote them; they are turned on the way
// out (rotate_180).
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

void pulsar_lcd_progress(unsigned done, unsigned total)
{
    s_done = total ? done : 0;
    s_total = total;
}

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b & 0xf0) >> 4 | (b & 0x0f) << 4);
    b = (uint8_t)((b & 0xcc) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xaa) >> 1 | (b & 0x55) << 1);
}

// The console writes VMU art turned 180 degrees, for a VMU that sits upside down in a controller,
// and the Pulsar turns what it receives for its own docked VMU: sent untouched, a game's art
// arrives inverted (bench, 2026-10-19, as the bridge found 2026-09-10). Our own screens are drawn
// as viewed and go as drawn.
static void rotate_180(uint8_t *dst, const uint8_t *src)
{
    for (unsigned i = 0; i < LCD_BYTES; i++) dst[i] = rev8(src[LCD_BYTES - 1 - i]);
}

// Choose the next frame into s_frame. False when nothing needs to go.
static bool next_frame(void)
{
    // Taken even while the loading screen has the Pulsar, so s_art is the newest when it ends.
    // Tested under the lock too: core1 sets the flag under it, and only the lock orders the two.
    uint32_t irq = spin_lock_blocking(s_lock);
    if (s_full) {
        memcpy(s_art, s_box, LCD_BYTES);
        s_full = false;
        s_have_art = true;
        s_pending = true;
    }
    spin_unlock(s_lock, irq);
    // Redraw per block: the count moves even when the bar's next pixel has not.
    if (s_done != s_shown_done || s_total != s_shown_total) {
        s_shown_done = s_done;
        s_shown_total = s_total;
        s_pending = true;
    }
    if (!s_pending) return false;
    if (s_total) {
        pulsar_screen_loading(s_frame, s_done, s_total);
        s_ours = true;
    } else if (s_have_art) {
        rotate_180(s_frame, s_art);
        s_ours = false;
    } else if (s_ours) {
        // The pull ended and the console has drawn nothing (the BIOS, a game without VMU art).
        // The Pulsar holds a host's screen until the link drops, so the bar must be replaced:
        // READY if the card came up, blank if the pull failed. The first console frame takes it.
        if (pulsar_card_present(NULL)) pulsar_screen_message(s_frame, "READY");
        else memset(s_frame, 0, LCD_BYTES);
    } else {
        s_pending = false;          // nothing of ours up and no art: the Pulsar's own screen stays
        return false;
    }
    s_pending = false;
    return true;
}

void pulsar_lcd_service(uint16_t con, uint16_t value_handle, uint16_t mtu)
{
    if (s_next < 0) {
        uint32_t now = btstack_run_loop_get_time_ms();
        if (now - s_last_ms < LCD_MIN_GAP_MS || !next_frame()) return;
        s_whole = mtu >= LCD_WHOLE_MTU;
        s_next = 0;
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
        // Any other failure drops the frame but not the change: the end of a pull or the art's
        // return may be the last change for minutes, with nothing new coming to retry with.
        if (st != ERROR_CODE_SUCCESS) { s_next = -1; s_pending = true; return; }
        // The gap runs from a frame's last write: one held up by BUSY must not let the next
        // follow it at once.
        if (s_whole || ++s_next == 4) {
            s_next = -1;
            s_last_ms = btstack_run_loop_get_time_ms();
            g_pulsar_lcd_sent++;
        }
    }
}

void pulsar_lcd_reset(void)
{
    // The Pulsar's own animation resumes while no host draws; a static game screen sends no new
    // frame, so the next link redraws what belongs there: the loading screen, the last art, or
    // READY.
    s_next = -1;
    s_pending = true;
}

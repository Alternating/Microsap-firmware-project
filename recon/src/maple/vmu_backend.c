// vmu_backend.c — their VMU's store (internal flash banks, or bank files on SD) and display
// (the OLED), behind vmu.h's seams. The bodies are the original vmu.c's backend calls, moved;
// v->storage_mode still selects the medium, as it did inline.
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "hardware/flash.h"
#include "vmu.h"

// oled.c
extern void oled_draw_bank(unsigned bank);
extern void oled_draw_sd_icon(bool on);
extern void oled_draw_storage_icon(bool internal);
extern void oled_refresh_kick(void);
extern void vmu_lcd_upscale(const uint8_t *lcd_fb);
// storage module
extern int  sd_write_file(const char *path, const void *data, size_t len, uint32_t offset); // 0x10003b60
extern int  vmu_bank_file_read(uint8_t bank, void *buf);                               // 0x10003abc
extern int  vmu_bank_file_write(uint8_t bank, uint32_t offset, const void *buf, size_t len); // 0x10003bc8
extern int  gameid_vmu_read(const char gameid[12], uint32_t bank_index, void *buf);    // 0x10003ae0
extern int  gameid_vmu_write(const char gameid[12], uint32_t bank_index, uint32_t offset,
                             const void *buf, size_t len);                             // 0x10003bf4
extern uint8_t g_settings_storage_pref;                                  // @0x2000fd6b
extern queue_t g_sd_write_queue;                                         // @0x2000fc28

// Internal VMU banks live in flash from offset 0xB9000, one 128KB bank each.
#define INTERNAL_BANKS_FLASH_OFF 0xB9000u
static const uint8_t *const g_internal_banks = (const uint8_t *)(XIP_BASE + INTERNAL_BANKS_FLASH_OFF);
// Root-block template, 0x400 bytes. rodata @0x1005e528 (extract verbatim for byte-exactness).
extern const uint8_t g_vmu_root_template[0x400];

// thin adapters so the VMU engine can load/save using its own state struct
static int vmu_bank_file_load(vmu_state_t *v) { return vmu_bank_file_read(v->bank, v->image); }
static int gameid_vmu_load(vmu_state_t *v)    { return gameid_vmu_read(v->gameid, 0, v->image); }

// ---- store: internal flash (mode 0) or SD bank files (mode 1); bank 10 is the GameID file ----

static uint8_t fs_mode(void *ctx)
{
    (void)ctx;
    return (g_settings_storage_pref == 2) ? 1 : 0;
}

// from 0x10002354 (the load half of vmu_load_bank_image)
static int fs_load(void *ctx, vmu_state_t *v)
{
    (void)ctx;
    if (v->bank == 10)            return gameid_vmu_load(v) ? 0xfe : 0;
    if (v->storage_mode == 1)     return vmu_bank_file_load(v) ? 0xfe : 0;
    if (v->storage_mode == 0) {
        memcpy(v->image, g_internal_banks + v->bank * 0x20000, 0x20000);
        return 0;
    }
    return 0xff;
}

// from 0x100022d0 (vmu_format_bank) + the re-read vmu_load_bank_image did after it
static int fs_format(void *ctx, vmu_state_t *v)
{
    (void)ctx;
    if (v->storage_mode == 0) {
        // Format the internal VMU in RAM (not flash) — the Dreamcast sees a blank formatted
        // VMU immediately. We deliberately do NOT erase/program flash here: doing that from
        // core1 while core0 runs from XIP flash faults core0 and resets the board. Flash
        // persistence of the internal VMU is deferred.  // TODO: persist via flash_safe_execute
        memset(v->image, 0, 0x20000);
        memcpy(&v->image[0x1FC00], g_vmu_root_template, 0x400);
    } else if (v->storage_mode == 1) {
        memset(v->image, 0, 0x20000);
        if (v->bank != 10) {
            vmu_bank_file_write(v->bank, 0, v->image, 0x20000);
            vmu_bank_file_write(v->bank, 0x1FC00, g_vmu_root_template, 0x400);
        } else {
            gameid_vmu_write(v->gameid, 0, 0, v->image, 0x20000);
            gameid_vmu_write(v->gameid, 0, 0x1FC00, g_vmu_root_template, 0x400);
        }
    }
    // mode 0: the format already populated the RAM image (no flash re-read).
    if (v->bank == 10)            { if (gameid_vmu_load(v)) return 0xfe; }
    else if (v->storage_mode == 1) { if (vmu_bank_file_load(v)) return 0xfe; }
    return 0;
}

// from 0x1000278c (the per-medium half of vmu_flush_dirty)
static bool fs_flush_page(void *ctx, vmu_state_t *v, unsigned page)
{
    (void)ctx;
    if (v->storage_mode == 0) {
        // Internal VMU persistence is deferred (flashing from core1 while core0 runs from XIP
        // faults the board). For now the internal VMU is RAM-only — it works but saves don't
        // survive a power cycle. SD-backed VMU (mode 1) does persist (queued to core0 below).
        // TODO: persist via flash_safe_execute() so it survives reboots.
        return true;
    }
    if (v->storage_mode == 1) {
        static sd_write_msg_t m;                 // 4KB+ message; original builds on stack
        memset(&m, 0, sizeof m);
        m.op = 1;                                // TODO(verify @0x1000278c op byte)
        m.bank = v->bank;
        if (v->gameid[0] != 0 && v->bank == 10) {
            memcpy(m.gameid, v->gameid, 12);
            m.gameid_valid = 1;
        }
        m.offset = page * 0x1000;
        memcpy(m.data, &v->image[page * 0x1000], 0x1000);
        queue_add_blocking(&g_sd_write_queue, &m);   // consumed on core0
        return true;
    }
    return false;
}

// from 0x10002568: export internal flash banks to SD once (stop at first error — e.g. file exists)
static void fs_opened(void *ctx, vmu_state_t *v)
{
    (void)ctx;
    if (v->storage_mode != 1) return;
    for (int i = 0; i < 10; i++) {
        char p[40];
        snprintf(p, sizeof p, "internal/vmu_internal%02d.bin", i);
        if (sd_write_file(p, g_internal_banks + i * 0x20000, 0x20000, 0)) break;
    }
}

const vmu_store_t g_vmu_store_flash_sd = {
    .mode = fs_mode, .load = fs_load, .format = fs_format,
    .flush_page = fs_flush_page, .opened = fs_opened,
};

// ---- display: the OLED ----

static void oled_lcd(void *ctx, const uint8_t fb[0xC0])
{
    (void)ctx;
    vmu_lcd_upscale(fb);
    oled_refresh_kick();
}
static void oled_bank(void *ctx, unsigned bank)      { (void)ctx; oled_draw_bank(bank); }
static void oled_storage(void *ctx, bool internal)   { (void)ctx; oled_draw_storage_icon(internal); }
static void oled_sd_icon(void *ctx, bool on)         { (void)ctx; oled_draw_sd_icon(on); }
static void oled_refresh(void *ctx)                  { (void)ctx; oled_refresh_kick(); }

const vmu_display_t g_vmu_display_oled = {
    .lcd = oled_lcd, .bank = oled_bank, .storage = oled_storage,
    .sd_icon = oled_sd_icon, .refresh = oled_refresh,
};

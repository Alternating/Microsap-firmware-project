// settings.c — persistent settings blob (flash 0x3FC000 + optional SD copy).
// Reconstructed from pico2maple flipped-bt; draft by local coder, corrected against decomp.
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "storage.h"

// from 0x10003f48/0x10003e94 (shared checksum): XOR of words 0..0x3e folded with the three
// bytes 0xfc..0xfe and 0xaa.
static uint8_t settings_checksum(const uint8_t *b)
{
    uint32_t x = 0;
    for (int i = 0; i < 0x3f; i++) {
        uint32_t w;
        memcpy(&w, b + i * 4, 4);
        x ^= w;
    }
    return (uint8_t)(b[0xfc] ^ b[0xfd] ^ (x >> 24) ^ b[0xfe] ^ x ^ (x >> 8) ^ (x >> 16) ^ 0xaa);
}

static bool blob_valid(const uint8_t *b)
{
    uint32_t mask;
    memcpy(&mask, b + 0x92, 4);
    if (mask == 0xffffffff) return false;        // erased flash
    return b[0xff] == settings_checksum(b);
}

static void load_raw(uint8_t *dst, bool use_sd, bool *ok)
{
    if (use_sd && g_storage_status == 2) {
        *ok = sd_read_file("pico2maple_settings.bin", dst, 0x100) == 0;
    } else {
        memcpy(dst, (const uint8_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET), 0x100);
        *ok = true;
    }
}

// from 0x10003e94
bool settings_load(p2m_settings_t *s, bool use_sd)
{
    bool ok;
    load_raw(s->raw, use_sd, &ok);
    if (!ok) return false;
    return blob_valid(s->raw);
}

// from 0x10003f48
bool settings_save(p2m_settings_t *s, bool use_sd)
{
    s->raw[0xff] = settings_checksum(s->raw);
    if (use_sd && g_storage_status == 2)
        return sd_write_file("pico2maple_settings.bin", s->raw, 0x100, 0) == 0;
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, 0x1000);
    flash_range_program(SETTINGS_FLASH_OFFSET, s->raw, 0x100);
    restore_interrupts(irq);
    return true;
}

// from 0x10003fe4 — zeroed blob with valid checksum (fields all unset)
void settings_defaults(p2m_settings_t *s)
{
    memset(s->raw, 0, 0x100);
    s->raw[0xff] = settings_checksum(s->raw);
}

// from 0x10004020 — exact-field switch (not a bitmask loop), then re-checksum
void settings_set(p2m_settings_t *s, uint32_t field, const void *val)
{
    uint32_t mask;
    switch (field) {
    case SETTINGS_FIELD_BANK:        s->raw[0] = *(const uint8_t *)val; break;
    case SETTINGS_FIELD_MAPPING:     memmove(s->raw + 1, val, MAPPING_SIZE); break;
    case SETTINGS_FIELD_GAMEID:      memmove(s->raw + 0x96, val, 12); break;
    case SETTINGS_FIELD_GAMEID_MODE: s->raw[0xa2] = *(const uint8_t *)val != 0; break;
    default: break;
    }
    memcpy(&mask, s->raw + 0x92, 4);
    mask |= field;
    memcpy(s->raw + 0x92, &mask, 4);
    s->raw[0xff] = settings_checksum(s->raw);
}

// from 0x100040d0
bool settings_get(const p2m_settings_t *s, uint32_t field, void *out)
{
    uint32_t mask;
    memcpy(&mask, s->raw + 0x92, 4);
    if (!(mask & field)) return false;
    switch (field) {
    case SETTINGS_FIELD_BANK:        *(uint8_t *)out = s->raw[0]; break;
    case SETTINGS_FIELD_MAPPING:     memmove(out, s->raw + 1, MAPPING_SIZE); break;
    case SETTINGS_FIELD_GAMEID:      memmove(out, s->raw + 0x96, 12); break;
    case SETTINGS_FIELD_GAMEID_MODE: *(uint8_t *)out = s->raw[0xa2]; break;
    default: break;
    }
    return true;
}

// from 0x10004120 — reload, verify, write only when the value actually changed
void settings_update_field(uint32_t field, const void *val, bool use_sd)
{
    p2m_settings_t fresh;
    bool ok;
    load_raw(fresh.raw, use_sd, &ok);
    if (!ok || !blob_valid(fresh.raw)) return;   // original aborts on invalid store

    bool differs = true;
    if (field == SETTINGS_FIELD_BANK)
        differs = fresh.raw[0] != *(const uint8_t *)val;
    else if (field == SETTINGS_FIELD_MAPPING)
        differs = memcmp(val, fresh.raw + 1, MAPPING_SIZE) != 0;
    if (!differs) {
        uint32_t mask;
        memcpy(&mask, fresh.raw + 0x92, 4);
        if (mask & field) return;                // identical and already present: skip write
    }
    settings_set(&fresh, field, val);
    settings_save(&fresh, use_sd);
}

// from 0x10004218
bool settings_read_field(uint32_t field, void *out, bool use_sd)
{
    p2m_settings_t fresh;
    bool ok;
    load_raw(fresh.raw, use_sd, &ok);
    if (!ok || !blob_valid(fresh.raw)) return false;
    return settings_get(&fresh, field, out);
}

// from 0x1000431c — identity-less default mapping: every output with 4 empty input slots
void mapping_defaults(uint8_t *m)
{
    for (int i = 0; i < 28; i++) {
        m[i * 5] = (uint8_t)i;
        memset(m + i * 5 + 1, 0xff, 4);
    }
    memset(m + 0x8c, 0, 4);  // options word
    m[0x90] = 0;             // autoload flag
}

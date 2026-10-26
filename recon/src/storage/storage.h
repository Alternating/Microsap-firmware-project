// storage.h — SD/FatFs layer, settings blob, button helper. Reconstructed from
// pico2maple flipped-bt; draft by local coder, reviewed against the decompilation.
#ifndef P2M_STORAGE_H
#define P2M_STORAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "pico/util/queue.h"

// ---- settings blob (256 bytes; flash slot 0x3FC000 = the picotool-loadable slot) ----
// [0x00] bank; [0x01..0x91] mapping table (28 outputs × {out_id, 4 input slots} + options
// u32 at +0x8c + autoload byte at +0x90); [0x92] u32 field mask; [0x96] gameid[12];
// [0xa2] gameid_mode; [0xff] checksum.
typedef struct { uint8_t raw[256]; } p2m_settings_t;

#define SETTINGS_FIELD_BANK        1u
#define SETTINGS_FIELD_MAPPING     2u
#define SETTINGS_FIELD_GAMEID      4u
#define SETTINGS_FIELD_GAMEID_MODE 8u
#define MAPPING_SIZE 0x91
#define SETTINGS_FLASH_OFFSET 0x3FC000u

bool settings_load(p2m_settings_t *s, bool use_sd);                       // 0x10003e94
bool settings_save(p2m_settings_t *s, bool use_sd);                       // 0x10003f48
void settings_defaults(p2m_settings_t *s);                                // 0x10003fe4
void settings_set(p2m_settings_t *s, uint32_t field, const void *val);    // 0x10004020
bool settings_get(const p2m_settings_t *s, uint32_t field, void *out);    // 0x100040d0
void settings_update_field(uint32_t field, const void *val, bool use_sd); // 0x10004120
bool settings_read_field(uint32_t field, void *out, bool use_sd);         // 0x10004218
void mapping_defaults(uint8_t *mapping);                                  // 0x1000431c

// ---- SD layer ----
extern queue_t g_sd_write_queue;   // element 0x1014 bytes, depth 16; consumed on core0
extern uint8_t g_storage_status;   // 1 = no SD, 2 = SD mounted (@0x2000fd6b)

int sd_ensure_dirs(void);                                                  // 0x100039d0
int sd_read_file(const char *name, void *buf, size_t exact_len);           // 0x10003a50
int sd_write_file(const char *name, const void *buf, size_t len, uint32_t offset); // 0x10003b60
int vmu_bank_file_read(uint8_t bank, void *buf);                           // 0x10003abc
int vmu_bank_file_write(uint8_t bank, uint32_t offset, const void *buf, size_t len); // 0x10003bc8
int gameid_vmu_read(const char gameid[12], uint32_t bank_index, void *buf); // 0x10003ae0
int gameid_vmu_write(const char gameid[12], uint32_t bank_index, uint32_t offset,
                     const void *buf, size_t len);                         // 0x10003bf4
int sd_read_lines(const char *name, char *linebuf, size_t maxlen,
                  void (*cb)(char *line, void *arg), void *arg);           // 0x10003c50
int sd_file_exists(const char *name);                                      // 0x10003d14
int sd_write_queue_service(void);      // core0 main loop: write one queued VMU page

// ---- physical button (active-low, debounced; short press → 1, long hold → 2) ----
typedef struct {
    uint32_t pin;
    uint8_t  state;        // 0 idle, 1 debouncing, 2 held
    uint64_t t_debounce;
    uint64_t t_hold;
    uint8_t  long_fired;
    uint8_t  last_level;
} p2m_button_t;

void button_init(p2m_button_t *b, uint32_t pin);                           // 0x10003d50
int  button_poll(p2m_button_t *b);                                         // 0x10003d78

int axis_deadzone_scale(int value, float dz_frac, int range);              // 0x10003e3c

#endif

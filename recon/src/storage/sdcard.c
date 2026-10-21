// sdcard.c — SD/FatFs path layer + button helper. Reconstructed from pico2maple flipped-bt;
// draft by local coder, corrected against the decompilation.
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "hardware/gpio.h"
#include "ff.h"
#include "storage.h"

queue_t g_sd_write_queue;
uint8_t g_storage_status;      // 1 = no SD, 2 = SD mounted  (@0x2000fd6b)
static FATFS g_fs;

// Debounce/press thresholds (values dumped from the binary literal pools).
#define BTN_DEBOUNCE_US   30001u      // 0x7531
#define BTN_SHORT_MAX_US  0x1e8868u   // 2,000,0?? µs ≈ 2.0 s (@0x10003e34)
#define BTN_LONG_HOLD_US  0x2dc6bfu   // ≈ 3.0 s (@0x10003e38)

// from 0x100039d0
int sd_ensure_dirs(void)
{
    queue_init_with_spinlock(&g_sd_write_queue, 0x1014, 16,
                             spin_lock_claim_unused(true));
    bool ok = f_mount(&g_fs, "", 1) == FR_OK;
    // original accepts FR_OK or FR_EXIST on each mkdir ((res & 0xf7) == 0, FR_EXIST == 8)
    ok = ok && ((f_mkdir("/Microsap") & 0xf7) == 0);
    ok = ok && ((f_mkdir("/Microsap/internal") & 0xf7) == 0);
    ok = ok && ((f_mkdir("/Microsap/gameid") & 0xf7) == 0);
    ok = ok && (f_chdir("/Microsap") == FR_OK);
    g_storage_status = ok ? 2 : 1;
    return ok ? 0 : -1;
}

// from 0x10003a50 — strict-size read; mode 0x13 creates missing files (size check then
// fails and the caller reformats/regenerates).
int sd_read_file(const char *name, void *buf, size_t exact_len)
{
    FIL f;
    if (f_open(&f, name, FA_READ | FA_WRITE | FA_OPEN_ALWAYS) != FR_OK)
        return -1;
    int rc = -1;
    UINT br = 0;
    if (f_lseek(&f, exact_len) == FR_OK && f_size(&f) == exact_len &&
        f_lseek(&f, 0) == FR_OK &&
        f_read(&f, buf, exact_len, &br) == FR_OK && br == exact_len)
        rc = 0;
    f_close(&f);
    return rc;
}

// from 0x10003b60 — ensure file size == len, then write len bytes at offset.
// TODO(verify @0x10003b60): size==len gate vs the 0x400-at-0x1FC00 partial writes seen in
// vmu_format_bank (possible that f_lseek grows the file and the check is tell==len).
int sd_write_file(const char *name, const void *buf, size_t len, uint32_t offset)
{
    FIL f;
    if (f_open(&f, name, FA_READ | FA_WRITE | FA_OPEN_ALWAYS) != FR_OK)
        return -1;
    int rc = -1;
    UINT bw = 0;
    if (f_lseek(&f, len) == FR_OK && f_size(&f) == len &&
        f_lseek(&f, offset) == FR_OK &&
        f_write(&f, buf, len, &bw) == FR_OK)
        rc = 0;
    f_close(&f);
    return rc;
}

// from 0x10003abc
int vmu_bank_file_read(uint8_t bank, void *buf)
{
    char p[16];
    snprintf(p, sizeof p, "vmu%02d.bin", bank);
    return sd_read_file(p, buf, 0x20000);
}

// from 0x10003bc8
int vmu_bank_file_write(uint8_t bank, uint32_t offset, const void *buf, size_t len)
{
    char p[16];
    snprintf(p, sizeof p, "vmu%02d.bin", bank);
    return sd_write_file(p, buf, len, offset);
}

static void gameid_trim(const char gameid[12], char out[13])
{
    memcpy(out, gameid, 12);
    out[12] = '\0';
    for (int i = 11; i >= 0 && (out[i] == '\0' || out[i] == ' '); i--)
        out[i] = '\0';
}

// from 0x10003ae0
int gameid_vmu_read(const char gameid[12], uint32_t bank_index, void *buf)
{
    char id[13], dir[24], path[48];
    gameid_trim(gameid, id);
    snprintf(dir, sizeof dir, "gameid/%s", id);
    if ((f_mkdir(dir) & 0xf7) != 0) {
        g_storage_status = 1;
        return -1;
    }
    snprintf(path, sizeof path, "%s/%s-%u.bin", dir, id, (unsigned)bank_index);
    return sd_read_file(path, buf, 0x20000);
}

// from 0x10003bf4
int gameid_vmu_write(const char gameid[12], uint32_t bank_index, uint32_t offset,
                     const void *buf, size_t len)
{
    char id[13], path[48];
    gameid_trim(gameid, id);
    snprintf(path, sizeof path, "gameid/%s/%s-%u.bin", id, id, (unsigned)bank_index);
    return sd_write_file(path, buf, len, offset);
}

// from 0x10003c50 — line reader for the controller-map cfg; strips \r, delivers non-empty
// lines (truncated at maxlen) to cb.  FR_NO_FILE → -2.
int sd_read_lines(const char *name, char *linebuf, size_t maxlen,
                  void (*cb)(char *line, void *arg), void *arg)
{
    FIL f;
    FRESULT r = f_open(&f, name, FA_READ);
    if (r == FR_NO_FILE) return -2;
    if (r != FR_OK) return -1;

    size_t len = 0;
    for (;;) {
        char c;
        UINT br;
        if (f_read(&f, &c, 1, &br) != FR_OK) { f_close(&f); return -1; }
        if (br == 0) {                       // EOF
            linebuf[len] = '\0';
            if (len) cb(linebuf, arg);
            f_close(&f);
            return 0;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            linebuf[len] = '\0';
            if (len) cb(linebuf, arg);
            len = 0;
            continue;
        }
        if (len < maxlen) {
            linebuf[len++] = c;
        } else {                             // overflow: drain to newline, deliver truncated
            do {
                if (f_read(&f, &c, 1, &br) != FR_OK) { f_close(&f); return -1; }
            } while (br == 1 && c != '\n');
            linebuf[len] = '\0';
            cb(linebuf, arg);
            len = 0;
            if (br == 0) { f_close(&f); return 0; }
        }
    }
}

// from 0x10003d14
int sd_file_exists(const char *name)
{
    FIL f;
    FRESULT r = f_open(&f, name, FA_READ);
    if (r == FR_OK) { f_close(&f); return 0; }
    return (r == FR_NO_FILE) ? -2 : -1;
}

// from 0x10003d50 — original also clears the SIO output-enable via the GPIO coprocessor
void button_init(p2m_button_t *b, uint32_t pin)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_set_pulls(pin, true, false);
    b->pin = pin;
    b->state = 0;
    b->long_fired = 0;
    b->last_level = 1;
}

// from 0x10003d78 — returns 0 none, 1 short press (on release), 2 long hold (once)
int button_poll(p2m_button_t *b)
{
    uint32_t level = gpio_get(b->pin);       // active low
    int ev = 0;
    switch (b->state) {
    case 0:
        if (level == 0 && b->last_level != 0) {
            b->state = 1;
            b->t_debounce = time_us_64();
        }
        break;
    case 1:
        if (time_us_64() - b->t_debounce >= BTN_DEBOUNCE_US) {
            if (level == 0) { b->state = 2; b->t_hold = time_us_64(); }
            else b->state = 0;
        }
        break;
    case 2: {
        uint64_t held = time_us_64() - b->t_hold;
        if (level != 0 && !b->long_fired) {
            b->state = 0;
            ev = (held < BTN_SHORT_MAX_US) ? 1 : 0;
        } else if (level != 0) {
            b->state = 0;
            b->long_fired = 0;
        } else if (held > BTN_LONG_HOLD_US && !b->long_fired) {
            b->long_fired = 1;
            ev = 2;
        }
        break;
    }
    }
    b->last_level = (uint8_t)(level != 0);
    return ev;
}

// from 0x10003e3c
int axis_deadzone_scale(int value, float dz_frac, int range)
{
    float r = (float)range;
    float dz = r * dz_frac;
    float v = (float)value;
    if (-dz <= v && v <= dz) return 0;
    v += (value < 1) ? dz : -dz;
    return (int16_t)((v / (r - dz)) * r);
}

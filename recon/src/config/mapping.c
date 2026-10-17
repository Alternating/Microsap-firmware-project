// mapping.c — custom controller mapping engine, reconstructed from pico2maple flipped-bt.
// Draft by local coder, reviewed/corrected against the decompilation.
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "mapping.h"
#include "../storage/storage.h"

static const char *const s_names[28] = {
    "A","B","C","D","X","Y","Z","START","LB","RB",
    "DPAD_UP","DPAD_DOWN","DPAD_LEFT","DPAD_RIGHT",
    "DPAD2_UP","DPAD2_DOWN","DPAD2_LEFT","DPAD2_RIGHT",
    "LT","RT",
    "A1_UP","A1_DOWN","A1_LEFT","A1_RIGHT",
    "A2_UP","A2_DOWN","A2_LEFT","A2_RIGHT",
};

// Per-output "pressed" magnitude for digital inputs (@0x200016ac, abs() applied):
static const int16_t s_weights[28] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    255,255,-128,127,-128,127,-128,127,-128,127,
};

#define INPUT_MASK  0x0ffc3fb3u   // valid left-side ids (@0x10004c5c)
#define OUTPUT_MASK 0x00fc3cb3u   // "meaningful mapping" outputs (@0x10004988)

const char *p2m_button_name(unsigned id) { return id < 28 ? s_names[id] : NULL; }

// from 0x1000498c
int p2m_button_id(const char *name)
{
    for (int i = 0; i < 28; i++)
        if (strcmp(name, s_names[i]) == 0) return i;
    return -1;
}

// Digital input id → gamepad byte offset (ids 0..0x11). from 0x10004344 switch
static const uint8_t s_din_off[18] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,   // A..START
    0x10, 0x0f,                                        // LB, RB
    0x09, 0x0a, 0x0b, 0x0c,                            // DPAD
    0x15, 0x16, 0x17, 0x18,                            // DPAD2
};

// Analog input source byte per id 0x12..0x1b (pairs share a byte).
static const uint8_t s_ain_off[10] = {
    0x0e, 0x0d,          // 0x12 LT, 0x13 RT
    0x12, 0x12,          // 0x14 A1_UP, 0x15 A1_DOWN
    0x11, 0x11,          // 0x16 A1_LEFT, 0x17 A1_RIGHT
    0x14, 0x14,          // 0x18 A2_UP, 0x19 A2_DOWN
    0x13, 0x13,          // 0x1a A2_LEFT, 0x1b A2_RIGHT
};

// from 0x10004344 — rewrite the gamepad state through the user mapping table.
void apply_custom_mapping(const uint8_t *mapping, p2m_gamepad_t *gpad)
{
    const uint8_t *g = gpad->g;            // inputs read from the pre-mapping state
    uint8_t  dig[18] = {0};                // outputs 0..0x11 (clamped 0/1)
    uint16_t lt = 0, rt = 0;               // outputs 0x12/0x13 (max, clamp 0xff)
    int16_t a1v = 0x80, a1h = 0x80, a2v = 0x80, a2h = 0x80;

    for (int row = 0; row < 28; row++) {
        const uint8_t *r = mapping + row * 5;
        int out = r[0];
        int16_t val = 0;                   // max contribution over the 4 slots

        for (int s = 0; s < 4; s++) {
            uint8_t in = r[1 + s];
            if (in == 0xff) break;
            int16_t c = 0;
            if (in < 0x12) {               // digital input
                if (g[s_din_off[in]] != 0) {
                    c = s_weights[out < 28 ? out : 0];
                    if (c < 0) c = (int16_t)-c;
                }
            } else if (in < 0x1c) {
                uint8_t v = g[s_ain_off[in - 0x12]];
                if (out < 0x12) {          // analog input feeding a digital output: threshold
                    switch (in) {
                    case 0x12: case 0x13: c = v > 0xf4; break;             // triggers
                    case 0x14: case 0x16: case 0x18: case 0x1a:            // "low" halves
                        c = v < 0x34; break;
                    default:                                               // "high" halves
                        c = v > 0xcc; break;
                    }
                } else {                   // analog → analog: magnitude
                    switch (in) {
                    case 0x12: case 0x13:                                  // trigger value
                        c = (out >= 0x14) ? (v >> 1) : v;
                        break;
                    case 0x14: case 0x16: case 0x18: case 0x1a: {          // low half: 0x80-v
                        int16_t m = (int16_t)(0x80 - v);
                        if (m < 0) m = 0;
                        c = (out < 0x14) ? (int16_t)(m << 1) : m;
                        break;
                    }
                    default: {                                             // high half: v-0x80
                        if (v <= 0x80) { c = 0; break; }
                        c = (out < 0x14) ? (int16_t)((v - 0x80) * 2)
                                         : (int16_t)(v - 0x80);
                        break;
                    }
                    }
                }
            }
            if (c > val) val = c;
        }

        // accumulate by output id (from the big switch at 0x100046xx)
        if (out < 0x12) {
            if (val > dig[out]) dig[out] = 1;       // OR semantics, clamp to 1
            else if (dig[out]) dig[out] = 1;
        } else if (out == 0x12) { if (val > (int16_t)lt) lt = val > 0xff ? 0xff : (uint16_t)val; }
        else if (out == 0x13)   { if (val > (int16_t)rt) rt = val > 0xff ? 0xff : (uint16_t)val; }
        else switch (out) {                          // axis halves: subtract/add around 0x80
        case 0x14: a1v = (int16_t)(a1v - val); if (a1v > 0xfe) a1v = 0xff; break;
        case 0x15: a1v = (int16_t)(a1v + val); if (a1v > 0xfe) a1v = 0xff; break;
        case 0x16: a1h = (int16_t)(a1h - val); if (a1h > 0xfe) a1h = 0xff; break;
        case 0x17: a1h = (int16_t)(a1h + val); if (a1h > 0xfe) a1h = 0xff; break;
        case 0x18: a2v = (int16_t)(a2v - val); if (a2v > 0xfe) a2v = 0xff; break;
        case 0x19: a2v = (int16_t)(a2v + val); if (a2v > 0xfe) a2v = 0xff; break;
        case 0x1a: a2h = (int16_t)(a2h - val); if (a2h > 0xfe) a2h = 0xff; break;
        case 0x1b: a2h = (int16_t)(a2h + val); if (a2h > 0xfe) a2h = 0xff; break;
        default: break;
        }
    }

    // pack outputs back into the gamepad struct (word stores in the original)
    uint8_t *o = gpad->g;
    for (int i = 0; i < 8; i++) o[i] = dig[i];
    o[0x08] = 0;
    o[0x09] = dig[10]; o[0x0a] = dig[11]; o[0x0b] = dig[12]; o[0x0c] = dig[13];
    o[0x0d] = (uint8_t)rt;                  // RT output (id 0x13) → byte +0xd
    o[0x0e] = (uint8_t)lt;                  // LT output (id 0x12) → byte +0xe
    o[0x0f] = 0;                            // RB/LB are inputs only
    o[0x10] = 0;
    o[0x11] = (uint8_t)a1h;
    o[0x12] = (uint8_t)a1v;
    o[0x13] = (uint8_t)a2h;
    o[0x14] = (uint8_t)a2v;
    o[0x15] = dig[14]; o[0x16] = dig[15]; o[0x17] = dig[16]; o[0x18] = dig[17];
}

// from 0x10004950
bool mapping_has_assignments(const uint8_t *mapping)
{
    for (int row = 0; row < 28; row++) {
        uint8_t out = mapping[row * 5], in0 = mapping[row * 5 + 1];
        if (out != 0xff && in0 != 0xff &&
            (out > 0x17 || (OUTPUT_MASK >> out) & 1))
            return true;
    }
    return false;
}

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

// from 0x10004cb8 — parse one "<input>: <output>" line into the mapping table
void cfg_line_parse(char *line, void *mapping_arg)
{
    uint8_t *mapping = (uint8_t *)mapping_arg;
    trim(line);
    if (line[0] == '#' || line[0] == '\0') return;
    char *colon = strchr(line, ':');
    if (!colon) return;
    *colon = '\0';
    char *left = line, *right = colon + 1;
    trim(left);
    trim(right);
    if (left[0] == '\0' || right[0] == '\0') return;

    int in_id = p2m_button_id(left);
    if (in_id == -1) {
        // not a button: maybe an option line
        if (strcmp(left, "OPTION_AUTOLOAD") == 0) {
            if (strcmp(right, "FALSE") == 0)      mapping[0x90] = 0;
            else if (strcmp(right, "TRUE") == 0)  mapping[0x90] = 1;
            else                                  mapping[0x90] = strtol(right, NULL, 10) != 0;
        }
        return;
    }
    int out_id = p2m_button_id(right);
    if (out_id == -1) return;
    if (!(in_id < 0x1c && ((INPUT_MASK >> in_id) & 1))) return;
    if (!(out_id < 8 || (unsigned)(out_id - 10) < 0x12)) return;   // outputs: 0..7 or 10..27

    for (int row = 0; row < 28; row++) {
        if (mapping[row * 5] != (uint8_t)out_id) continue;
        for (int s = 0; s < 4; s++) {
            if (mapping[row * 5 + 1 + s] == 0xff) {
                mapping[row * 5 + 1 + s] = (uint8_t)in_id;
                return;
            }
        }
        return;                                                    // all 4 slots full
    }
}

// from 0x10004d60 — read cfg from SD into mapping; create the sample file when missing
int cfg_load_custom_mapping(uint8_t *mapping, const char *cfg_name, bool create_sample)
{
    if (g_storage_status != 2) return -1;
    char linebuf[0x32];
    int r = sd_read_lines(cfg_name, linebuf, sizeof linebuf, cfg_line_parse, mapping);
    if (r == -2) {
        if (create_sample &&
            sd_file_exists("pico2maple_controller_map_sample.cfg") == -2)
            sd_write_file("pico2maple_controller_map_sample.cfg",
                          g_cfg_sample_text, strlen(g_cfg_sample_text), 0);
        return -1;
    }
    return (r < 0) ? -1 : 0;
}

// from 0x10004dc4
int mapping_autoload_flag(const uint8_t *mapping, int must_be_zero)
{
    if (must_be_zero != 0) return -1;
    return mapping[0x90];
}

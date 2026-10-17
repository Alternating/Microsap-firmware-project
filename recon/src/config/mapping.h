// mapping.h — custom controller mapping, reconstructed from pico2maple flipped-bt.
// Draft by local coder, reviewed/corrected against decompilation (0x10004344 et al).
#ifndef P2M_MAPPING_H
#define P2M_MAPPING_H

#include <stdint.h>
#include <stdbool.h>

// Normalized gamepad state (filled by USB/BT ingestion, rewritten in place by
// apply_custom_mapping, consumed by the Dreamcast layout mapper 0x1000098c).
// Byte offsets (verified against 0x10004344 reads + 0x10004dd4 writes):
//   +0x00..07  digital: A,B,C,D,X,Y,Z,START
//   +0x08      (unused, 0)
//   +0x09..0c  digital: DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT
//   +0x0d      RT analog 0..255
//   +0x0e      LT analog 0..255
//   +0x0f      RB digital
//   +0x10      LB digital
//   +0x11      A1 horizontal (0x80 center)   +0x12  A1 vertical
//   +0x13      A2 horizontal                  +0x14  A2 vertical
//   +0x15..18  digital: DPAD2_UP, DPAD2_DOWN, DPAD2_LEFT, DPAD2_RIGHT
//   +0x19      misc buttons (NOT seen by the mapper): bit0 Select, bit1 System/Home, bit2 Capture
typedef struct __attribute__((packed)) {
    uint8_t g[0x20];
} p2m_gamepad_t;

#define P2M_MISC 0x19           // index of the misc-buttons byte
#define P2M_MISC_SELECT 0x01
#define P2M_MISC_SYSTEM 0x02
#define P2M_MISC_CAPTURE 0x04

// Button/axis IDs (order = the firmware's name table @0x10004b60):
// 0 A, 1 B, 2 C, 3 D, 4 X, 5 Y, 6 Z, 7 START, 8 LB, 9 RB,
// 10..13 DPAD_U/D/L/R, 14..17 DPAD2_U/D/L/R, 18 LT, 19 RT,
// 20..23 A1_UP/DOWN/LEFT/RIGHT, 24..27 A2_UP/DOWN/LEFT/RIGHT
#define P2M_ID_LT 18
#define P2M_ID_RT 19

const char *p2m_button_name(unsigned id);
int  p2m_button_id(const char *name);                                    // 0x1000498c
void apply_custom_mapping(const uint8_t *mapping, p2m_gamepad_t *gp);    // 0x10004344
bool mapping_has_assignments(const uint8_t *mapping);                    // 0x10004950
void cfg_line_parse(char *line, void *mapping_arg);                      // 0x10004cb8
int  cfg_load_custom_mapping(uint8_t *mapping, const char *cfg_name,
                             bool create_sample);                        // 0x10004d60
int  mapping_autoload_flag(const uint8_t *mapping, int must_be_zero);    // 0x10004dc4

extern const char g_cfg_sample_text[];   // embedded sample cfg (rodata 0x100a16f4)

#endif

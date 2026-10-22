// Auto-generated from pico2maple_flipped-bt (tools/gen_assets.py). Do not edit.
#ifndef P2M_ASSETS_H
#define P2M_ASSETS_H
#include <stdint.h>

extern const uint8_t g_vmu_root_template[0x400];  // VMU root-block template (@0x1005e528, used by vmu_format_bank 0x100022d0)
extern const uint8_t g_glyph_bank10_gid[0x40];  // bank glyph: GID/bank10 strip a (DAT_10003394)
extern const uint8_t g_glyph_bank9[0x40];  // bank glyph 9 (DAT_10003398)
extern const uint8_t g_glyph_bank8[0x40];  // bank glyph 8 (DAT_10003390)
extern const uint8_t g_glyph_bank7[0x40];  // bank glyph 7 (DAT_1000338c)
extern const uint8_t g_glyph_bank6[0x40];  // bank glyph 6 (DAT_10003388)
extern const uint8_t g_glyph_bank5[0x40];  // bank glyph 5 (DAT_10003384)
extern const uint8_t g_glyph_bank4[0x40];  // bank glyph 4 (DAT_100033b4)
extern const uint8_t g_glyph_bank3[0x40];  // bank glyph 3 (DAT_100033b0)
extern const uint8_t g_glyph_bank2[0x3f];  // bank glyph 2 (DAT_100033ac)
extern const uint8_t g_glyph_bank0_a[0x20];  // bank 0 label strip a (DAT_100033a4)
extern const uint8_t g_glyph_bank0_b[0x20];  // bank 0 label strip b (DAT_100033a8)
extern const uint8_t g_glyph_bank1_a[0x20];  // bank 1 label strip a (DAT_1000339c)
extern const uint8_t g_glyph_bank1_b[0x40];  // bank 1 label strip b (DAT_100033a0)
extern const uint8_t g_icon_storage_int[0x20];  // storage icon internal (DAT_100037f8)
extern const uint8_t g_icon_storage_sd[0x20];  // storage icon SD (DAT_100037fc)
extern const uint8_t g_icon_sd_state[0x20];  // SD state icon (DAT_10003540)
extern const uint8_t g_icon_layout_racing_a[0x20];  // layout icon strips (DAT_10003920/24: mode 4 racing)
extern const uint8_t g_icon_layout_racing_b[0x40];  // layout icon mode 4 strip b (DAT_10003924)
extern const uint8_t g_icon_layout_std_a[0x20];  // layout icon mode 1 strip a (DAT_10003928)
extern const uint8_t g_icon_layout_std_b[0x40];  // layout icon mode 1 strip b (DAT_1000392c)
extern const uint8_t g_icon_layout_ts_a[0x20];  // layout icon mode 2 strip a (DAT_10003930)
extern const uint8_t g_icon_layout_ts_b[0x40];  // layout icon mode 2 strip b (DAT_10003934)
extern const uint8_t g_icon_layout_alt[0x20];  // layout icon mode 3/default strip (DAT_1000391c)
extern const uint8_t g_icon_attach_on_a[0x20];  // device-attached icon on strip a (DAT_100039b4)
extern const uint8_t g_icon_attach_off_a[0x20];  // device-attached icon off strip a (DAT_100039ac)
extern const uint8_t g_icon_attach_off_b[0x40];  // device-attached icon off strip b (DAT_100039b0)

// connected-input-device icons (Path B addition; 24x16, 24B page0 + 24B page1) — see
// tools/gen_device_icons.py and oled_draw_device_icon.
extern const uint8_t g_icon_dev_bt[48];     // Bluetooth pad
extern const uint8_t g_icon_dev_pad[48];    // USB/wired gamepad
extern const uint8_t g_icon_dev_kbd[48];    // keyboard
extern const uint8_t g_icon_dev_mouse[48];  // mouse

extern const char g_cfg_sample_text[];  // embedded sample cfg (@0x100a16f4)

#endif

# Config / OLED / board / main notes

## config/mapping.c (0x10004344–0x10004dc4) — confidence HIGH on structure

- Button/axis name table (0x1000498c name list) in ID order 0..27: A,B,C,D,X,Y,Z,START,LB,RB,
  DPAD_U/D/L/R, DPAD2_U/D/L/R, LT, RT, A1_UP/DOWN/LEFT/RIGHT, A2_UP/DOWN/LEFT/RIGHT.
- apply_custom_mapping (0x10004344): rewrites the normalized gamepad in place through the 28-row
  table (each row: output_id + up to 4 input slots). Weights @0x200016ac: buttons=1, triggers
  LT/RT=255, axis halves = ±128/127. Analog inputs feeding digital outputs use thresholds
  (>0xf4 triggers, <0x34 / >0xcc axis halves); analog→analog uses magnitude with a <<1 for
  digital-ish outputs. Outputs 0..0x11 clamp to bool, 0x12/0x13 (triggers) clamp 0xff, axis
  pairs accumulate around 0x80. **HIGH** but the exact int16 wrap/clamp on axis pairs is MED —
  verify against hardware.
- mapping_has_assignments (0x10004950): valid row = output!=0xff && input0!=0xff && (output>0x17
  || bit set in 0x00fc3cb3).
- cfg_line_parse (0x10004cb8): "<input>: <output>" grammar; OPTION_AUTOLOAD: TRUE/FALSE/int;
  input validity mask 0x0ffc3fb3, output must be 0..7 or 10..27; inserts input into first free
  slot of the row whose output_id matches.
- cfg_load_custom_mapping (0x10004d60): needs g_storage_status==2; reads
  /pico2maple/pico2maple_controller_map.cfg via sd_read_lines; if missing and create_sample,
  writes pico2maple_controller_map_sample.cfg from g_cfg_sample_text (embedded, assets).
- mapping_autoload_flag (0x10004dc4): returns mapping[0x90] (autoload) when arg2==0.
- NOT yet reconstructed: 0x10004dd4 (per-controller HID axis->trigger normalizer, uses a
  descriptor table) and 0x10004e94 (1348B, likely the per-controller HID report field extractor
  feeding the normalized gamepad). These belong with the USB module. See usb.md (pending).

## oled/oled.c (0x10002c18–0x10003938) — confidence HIGH on glyphs, MED on LCD upscale

- SSD1306 128x64 over I2C0 @0x3C, baud 800000, alt-layout pins SDA=GPIO4/SCL=GPIO5.
- Framebuffer 0x80E bytes: cmd prefix + 1024 u16 entries (low byte=data, high byte=I2C flags).
  Refreshed by a DMA channel pacing to the I2C TX DREQ; the last entry (byte 0x80C) gets the
  I2C STOP flag (0x200). A repeating alarm re-arms the DMA.
- Init command bytes transcribed from the init table (0x10002e94..0x10002ea8). `rotated`
  flips COM scan (0xC8/0xC0) and segment remap (0xA1/0xA0) — this is what the repo's
  "rotated_screen" builds change, and is a candidate meaning for the local file's "flipped"
  tag IF it is not the pinout (but board.c shows the pinout IS switched, so "flipped" = pinout).
- Status glyphs live in the right column across the 8 pages: attached icon (p0/1), bank digit
  (p2/3), layout icon (p4/5), SD/storage icon (p6/7). Glyph bitmaps extracted to assets.
  NOTE: g_glyph_bank0_* / g_glyph_bank1_* asset names are swapped vs their role (bank 0 draw
  uses the "bank1" strips); contents are correct, oled.c uses them in the right role.
- vmu_lcd_upscale (0x10002ef4): dense bit-packing; reimplemented functionally (48x32 1bpp →
  framebuffer). **MED** — verify scale/orientation/offset on hardware.

## board.c (0x10008140) — confidence HIGH
See memory note. GPIO15 grounded → alternate pinout (Maple GPIO6/7, etc.). This build's
default use is the alternate layout.

## main.c (0x10000608 + core0 loop 0x10000308) — confidence MED (structural)
Entry sets up stdio, pins (GPIO15 sense), USB host, kbd queue, launches core1 (Maple engine),
then runs the core0 loop: wait core1, bt_init, wait bt, cyw43 poll init, 300ms USB settle, then
loop {usb task, controller poll, rumble out, mode-button → bank switch / layout toggle}. The
exact controller_poll internals and rumble routing are in the USB/BT modules (pending).

# Storage + settings notes (0x100039bc–0x1000431c)

Files: recon/src/storage/sdcard.c, settings.c, storage.h. Confidence HIGH unless noted.

## Settings blob (256 B, flash 0x3FC000 = picotool mapping slot; optional SD copy)
- [0x00] bank, [0x01..0x91] 0x91-byte mapping table, [0x92] u32 field mask, [0x96] gameid[12],
  [0xa2] gameid_mode, [0xff] checksum.
- Checksum (0x10003e94/f48): XOR of 63 LE words (0..0xfb) folded with bytes 0xfc/0xfd/0xfe and
  0xaa. Blob invalid if mask==0xffffffff (erased) or checksum mismatch. **HIGH**.
- Field IDs: 1 bank, 2 mapping, 4 gameid, 8 gameid_mode. set/get are exact-field (switch), not
  a bitmask loop — they touch one field and OR it into the mask.
- settings_save: SD when use_sd && g_storage_status==2, else flash erase(0x3FC000,0x1000)+
  program(0x100) under save_and_disable_interrupts.
- settings_update_field (0x10004120): reload fresh, verify, write only if value changed or field
  absent (write-avoidance for flash wear). settings_read_field (0x10004218): reload+verify+get.

## SD layer (carlk3 no-OS-FatFS, FatFs)
- sd_ensure_dirs (0x100039d0): queue_init_with_spinlock(g_sd_write_queue, 0x1014, 16); f_mount;
  mkdir /pico2maple, /pico2maple/internal, /pico2maple/gameid (FR_OK or FR_EXIST ok); chdir.
  g_storage_status = 2 (ok) / 1 (fail).
- sd_read_file/sd_write_file (0x10003a50/0x10003b60): open mode 0x13 (FA_READ|FA_WRITE|
  FA_OPEN_ALWAYS), strict size==len gate, then read-all / lseek(offset)+write. **MED** on the
  size gate vs the 0x400@0x1FC00 partial writes from vmu_format_bank — verify.
- vmu_bank_file_* "vmu%02d.bin"; gameid_vmu_* "gameid/<id>/<id>-<n>.bin" (id trimmed of trailing
  spaces/NULs; mkdir gameid/<id>).
- sd_read_lines (0x10003c50): 1-byte-at-a-time line reader for the cfg parser, strips \r,
  truncates at 0x32, FR_NO_FILE→-2.

## Button + axis helpers
- button_init/poll (0x10003d50/0x10003d78): active-low, debounce 30001µs, short-press event on
  release if held < 0x1e8868µs (~2.0s), long-press event once past 0x2dc6bf µs (~3.0s). The
  original also toggles the GPIO coprocessor OE. **HIGH** (thresholds dumped from binary).
- axis_deadzone_scale (0x10003e3c): float deadzone frac × range, rescales outside the zone.

## Open items
- sd_write_file size/offset semantics (see above).
- g_storage_status side-effect in gameid_vmu_read mkdir-fail path (TODO in code).

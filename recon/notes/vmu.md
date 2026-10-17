# VMU + OLED module notes (functions 0x10000824, 0x10002250–0x10003938)

## vmu_state buffer layout (single big struct, passed as `uint8_t *vmu` — lives on core1 stack, 131264 B)

| Offset | Size | Field |
|---|---|---|
| 0x00000 | 0x20000 | VMU flash image (256 blocks × 512 B; root block 255 @0x1FE00 must start with 16×'U') |
| 0x20000 | 0xC0 | VMU LCD framebuffer (48×32 1bpp) |
| 0x200c0 | 4 | block-read response: func code 0x02000000 (storage, BE) |
| 0x200c4 | 4 | block-read response: block number (byte-swapped) |
| 0x200c8 | 0x200 | block-read response: block data |
| 0x202c8 | 4 | dirty-page bitmask (32 × 4 KB pages) |
| 0x202cc | 1 | current bank (0–9; 10 = GameID bank) |
| 0x202cd | 12 | GameID string (3 words, byte-swapped from wire) |
| 0x202d9 | 0x80 | GameID aux buffer (cleared on init/new id) |
| 0x20359 | 0x30 | VMU device-info: func 0x0E (storage+LCD+clock); fdata 0x403f7e7e / 0x00100500-ish / 0x00410f00 (bytes written individually: 0xe,0x7e,0x7e,0x3f,0x40, 5,0x10, 0xf,0x41) |
| 0x2036b | 30 | product name "Visual Memory" (copied from rodata ptr) |
| 0x20389 | 60+ | license string + power words (DAT_10002734 at +0x203c5) |
| 0x203c9 | 16 | media-info template copy (DAT_10002738) |
| 0x203d9 | 0x40 | cleared staging area |
| 0x20419 | 0x1c | GET_MEDIA_INFO response: func BE, 0xff..., words DAT_100022c8/cc; +0x20429=0xd (dirsz 13), +0x2042b=u16 icon (from image byte 0x1FE4E), +0x2042d=200 (user blocks), +0x2042f=0x1f, +0x20433=0x80 |
| 0x20435 | 4+ | LCD media info: +0x20438=4, +0x20439=u32 DAT_1000273c (48×32 desc) |
| 0x2043d | 1 | storage mode: 0 = internal flash, 1 = SD card |

## Functions

- **vmu_build_device_info (0x824)** — fills a {hdr,ptr} pair + 112-byte device-info body:
  fdata bytes as above, name+license copies, power word DAT_100008e8. HIGH.
- **vmu_media_info_build (0x2250)** — (re)build GET_MEDIA_INFO response fields (values above),
  icon shape = u16 from image[0x1FE4E]. HIGH.
- **vmu_format_bank (0x22d0)** — storage mode 0: flash_range_erase(bank*0x20000 + 0xD7000, 0x2000)
  i.e. system area (image offs 0x1E000) of internal bank at flash offset 0xB9000+bank*0x20000;
  program 7×0x400 zero chunks then root-block template (DAT_1000224c) at +0x1FC00.
  All under save_and_disable_interrupts. Mode 1: memset image, write vmu file (bank file or
  gameid path), root template at 0x1FC00. flash fns are RAM-resident veneers
  (flash_range_erase=thunk 0x200007b0, flash_range_program=0x2000085c). HIGH.
- **vmu_load_bank_image (0x2354)(buf, bank)** — bank>10 → -1. Sets bank; OLED bank glyph
  (0x3188). bank==10 → load gameid file; mode 1 → load bank file; mode 0 → memcpy from XIP
  ptr *DAT_10002564 (=0x100B9000) + bank*0x20000. Validate 16×'U' root marker at 0x1FE00; if
  bad → format (0x22d0) and reload/recopy. Rebuild media info. Returns 0 / 0xfe (file err) /
  0xff (bad mode). HIGH.
- **vmu_init (0x2568)(buf)** — storage pref from settings global (*DAT_10002728: 2 = SD → mode 1
  else mode 0); draw storage icon (0x3544); load bank 1 (!) — on -2 falls back to internal.
  Builds device-info block at +0x20359 (name/license from rodata), LCD media info, clears
  gameid. If SD mode: export all 10 internal banks to SD as "internal/vmu_internal%02d.bin"
  (snprintf fmt DAT_10002744; source XIP *DAT_10002740 + i*0x20000; stop on first write error —
  note: likely skips existing files inside 0x3b60). HIGH (export-direction MED).
- **vmu_block_read (0x2748)(buf, blockspec)** — blockspec u32: hi byte(s) phase/block; rejects
  if (spec & 0xff0000)!=0; copies block (spec&0xff)*0x200 into response @+0x200c8; stores
  byte-swapped spec @+0x200c4, func BE @+0x200c0. HIGH.
- **vmu_flush_dirty (0x278c)(buf)** — pop lowest dirty page; mode 0: flash erase+program 4 KB at
  0xB9000+bank*0x20000+page*0x1000 (IRQ-save); mode 1: build 0x1014-byte message {1, bank,
  gameid[12], flag, offset, 4 KB data} and queue_add_blocking(sd_write_queue DAT_100028a0) —
  SD writes happen on core0. When mask reaches 0: redraw SD icon (0x33b8) + OLED refresh.
  Returns 1 if a page was flushed, 0 if none. HIGH.
- **vmu_block_write (0x28a4)(buf, spec, src, words)** — dst = ((spec>>16 &0xff) + (spec&0xff)*4)
  *0x80 (phase-of-4 × 128 B); words byte-swapped; set dirty bit (block>>3); redraw icons. HIGH.
- **vmu_lcd_render_notify (0x28f4)** — timestamp + upscale LCD (0x2ef4 on buf+0x20000) + OLED
  DMA kick (0x2eb8). HIGH.
- **vmu_lcd_write (0x290c)(buf, spec, src, words)** — byte-swapped copy into LCD fb
  (+0x20000), zero-pad to 0xC0. HIGH.
- **vmu_load_bank (0x2948)(buf, bank, notify)** — flush all dirty; load image; rebuild
  device-info + media info (byte stores as in init); clear staging; bank glyph (0x3188);
  optional OLED kick. HIGH.
- **vmu_load_gameid_bank (0x2aa0)** — if mode SD && gameid set → load bank 10. HIGH.
- **vmu_bank_cycle (0x2ac4)(buf, forward, ...)** — cycle 0..9 with wrap; if gameid available,
  wrap goes through bank 10 (gameid) at the 0↔9 boundary. HIGH.
- **vmu_gameid_store (0x2b44)(buf, frame, len)** — len<4 → -1; rejects a specific blacklisted
  id (DAT_10002bb4/b8 — dump: likely all-zero or menu id); stores 12-byte byte-swapped id;
  extra words continue into +0x202d9 area; len==4 clears aux. HIGH.
- **vmu_set_condition (0x2bbc)(buf, frame…)** — only func==8 (clock): buzzer via
  buzzer_set(0x1000811c)(b7, b6, 8) — PWM beep (freq/duty bytes). HIGH.
  (0x2bc6 variant passes explicit arg3.)
- **oled_dma_init (0x2c18)** — framebuffer *DAT_10002ca8 (0x80E B: cmd prefix + 0x400 u16
  entries later pushed as 16-bit I2C writes); claims DMA ch; CTRL: DREQ i2c tx (0x58/0x5c
  select by i2c0/i2c1), 16-bit, write to I2C_DATA_CMD (+0x10). Also preloads two 0x20/0x40
  glyph strips (bank label chars) into fb rows +0x33*4/+0x73*4. HIGH.
- **ssd1306_send (0x2cd4)(data, hdr)** — i2c_write_blocking(i2c, 0x3C, {0x80,cmd}, 2) for cmds
  0x21 col-range, 0x22 page-range (from hdr); then malloc len+1 buffer {0x40, data…} single
  blocking write; free. HIGH.
- **oled_init (0x2d64)(rotation)** — malloc 0x80E fb; i2c_init(baud DAT_10002e8c); 2 pins func
  I2C + pullups (OLED pins; alt layout GPIO4/5); sleep 50 ms; cmd sequence incl. 0xA8 0x3F,
  (rotation? 0xC8:0xC0... local_2d), segremap (rot^1)|0xA0, …, 0x2E, 0xAF; sleep; clear via
  ssd1306_send; oled_dma_init; claim hardware alarm + set repeating callback (DAT_10002eb4)
  every DAT_10002eb0 µs → periodic OLED DMA refresh. HIGH (exact cmd bytes: dump DAT_10002e94..a8).
- **oled_refresh_kick (0x2eb8)** — if enabled && DMA idle: fb[0x80c] |= 0x200 marker;
  DMA READ_ADDR=fb, TRANS_COUNT_TRIG=0x407. HIGH.
- **vmu_lcd_upscale (0x2ef4)(lcd)** — bit-twiddling 48×32 → OLED (2× scale + rotation) into
  fb+0xE. Faithful port = transcribe; MED (regenerate from decomp when building).
- **oled_draw_bank (0x3188)(bank)** — draws bank glyph strips (banks 0/1 label chars; 2–10
  digit bitmaps incl. "GID" for 10) into fb rows +0x2cc/+0x3cc from rodata tables
  (DAT_10003384–0x100033b4). HIGH.
- **oled_draw_sd_icon (0x33b8)(on)** — 2×0x20-byte glyph at +0x6ce/+0x7ce or clear. HIGH.
- **oled_draw_storage_icon (0x3544)(mode)** — same pattern at +0x6f6/… internal-vs-SD glyph. HIGH.
- **0x3800 / 0x3938** — further icon draw (rumble?) / vmu_storage_enable(flag) — storage
  attach flag + icon. MED (confirm in decomp when porting).

## TODO dumps needed before rebuild
- Glyph/bitmap rodata tables: resolve DAT_10003384..0x100033b4, DAT_1000339c/33a0/33a4/33a8,
  DAT_100037f8/fc, oled cmd words DAT_10002e94..0x10002ea8, root-block template DAT_1000224c,
  media-info words DAT_100022c8/cc, 0x1000273c, blacklist id DAT_10002bb4/b8, snprintf fmt
  DAT_10002744 ("internal/vmu_internal%02d.bin"), XIP base DAT_10002740/0x10002564 (0x100B9000).
- sd_write_queue element format (0x1014 B) must match storage module's core0 consumer.

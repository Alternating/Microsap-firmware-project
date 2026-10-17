# Pico2Maple Reconstruction — Shared Agent Brief

You are reconstructing clean, buildable C from a Ghidra decompilation of the closed-source
pico2maple firmware (Dreamcast Maple-bus peripheral emulator, RP2350 / Pico 2 W, pico-sdk 2.2.0).

## Inputs (all on disk — READ THESE, do not guess)

- `/projects/Pico2maple-decomp/export/decompiled_all.c` — full decompilation, functions sorted by
  address, each preceded by `// ==== <addr> <name> (size N) ====`. Extract your functions with awk:
  `awk '/^\/\/ ==== 10003e94 /,/^\/\/ ==== 10003f48 /' decompiled_all.c`
- `/projects/Pico2maple-decomp/export/functions.csv` — address,name,size,callers,callees
- `/projects/Pico2maple-decomp/export/strings.csv` — address|string|referencing_functions
- `/projects/Pico2maple-decomp/pico2maple_flipped-bt_rp2350-arm-s_0x10000000.bin` — raw image,
  base 0x10000000 (to read rodata tables: `python3` + struct at offset addr-0x10000000)
- `/projects/Pico2maple-decomp/LEDGER.md` — binary map and anchors
- `/projects/Pico2maple-decomp/recon/src/maple.pio` — already-reconstructed PIO programs
- References: `/app/data/home/refs/{pico-sdk,bluepad32-cluoma,bluepad32,sdlib,tinyusb,tusb_xinput,MapleSyrup}`
  (pico-sdk is tag 2.2.0 — use its headers for exact API signatures; bluepad32-cluoma is the
  author's own fork actually linked into this firmware — uni_* structs there are authoritative)

## Architecture (established facts)

- main() = FUN_10000608 @0x10000608 (flash). Core1 runs FUN_10000298 = "maple_core1_main":
  Maple PIO init + the whole Maple protocol loop (giant LTO-inlined function, disjoint body).
- Maple bus: 2 GPIOs (base pin from a global byte; alternate layout = GPIO6/7). RX PIO program
  (24 instr) and TX (30 instr) on TWO different PIO instances. RX: IN_BASE=pin, autopush. TX:
  SET_BASE=pin,SET_COUNT=2,OUT_COUNT=2.
- Devices emulated: controller (subtypes standard/twinstick/racing/fightstick/custom), keyboard
  (func 0x40), mouse (0x200), VMU (storage 0x002|LCD 0x004|clock 0x008), Puru Puru (0x100,
  static device-info block @0x100a2994 in .data).
- Storage: SD card (carlk3 lib, SPI GPIO10-13) with FatFs; paths /pico2maple, /pico2maple/internal,
  /pico2maple/gameid, vmu%02d.bin, gameid/<id>/<id>-<n>.bin, pico2maple_settings.bin,
  pico2maple_controller_map.cfg(+_sample). Internal VMU banks also in flash
  (internal/vmu_internal%02d.bin mirrors + flash_range_erase/program).
- Custom mapping also loadable from flash 0x103FC000 (picotool slot; global ptr @0x100a16b8).
  BTstack TLV (pairing keys) at flash 0x103FD000+.
- BT: Bluepad32 fork + BTstack; uni_platform struct @0x100a2a18, callbacks p2m_bp32_* (named in
  decomp). USB: TinyUSB host + tusb_xinput.
- OLED: SSD1306 128x64 I2C on GPIO4/5 (i2c calls around FUN_10005c30/FUN_1000718c).
- Config text format + sample file: `/projects/Pico2maple-decomp/recovered/pico2maple_controller_map_sample.cfg`

## Library callee identification (verified or high-confidence; verify borderline ones yourself)

| Address | Identity |
|---|---|
| 0x1000a314 | time_us_64 |
| 0x10052bd0 | memset |
| 0x10053d7c | memcpy |
| 0x100532a0 | strcmp (verify — maybe strncmp) |
| 0x10054678 | strlen (verify) |
| 0x10053174 | memcmp (verify) |
| 0x1000d404 | snprintf/sprintf family |
| 0x1000d8d4 / 0x1000d9d8 | stdio puts / printf |
| 0x1000bdd0 | malloc (panics "Out of memory") |
| 0x1000a9d0 | hard_assert/panic helper |
| 0x1000a1ac | sleep_ms or busy_wait (verify) |
| 0x1000a468 | queue_try_remove (pico_util queue; verify) |
| 0x1000a3d0 | queue_init-ish / alarm-pool-ish (verify) |
| 0x10008e4c | gpio_set_function |
| 0x10008e88 | gpio_set_pulls (pin, up, down) |
| 0x10008fc4 | gpio pull/init single-arg (verify) |
| 0x10009d88 / 0x10009ec8 | irq handler set / irq_set_enabled (verify) |
| 0x10011e8c | pio_add_program |
| 0x1001217c | pio_sm_init |
| 0x100120f0 | pio_sm_set_consecutive_pindirs |
| 0x1000df50 / 0x1000dfe0 | flash_range_erase / flash_range_program |
| 0x10011524 | i2c_write_blocking (OLED) |
| 0x100180c4 | f_open (FatFs; 0x10017290 f_read-ish, 0x10018200 f_write-ish, 0x100179b8 f_close-ish, 0x100186b0 f_mkdir, verify each by arg patterns vs refs/sdlib + pico-sdk FatFs) |
| 0x1000f420, 0x1001137c, 0x100113b0, 0x1001363c, 0x10012794, 0x100114a8 | TinyUSB host (tuh_*) — verify against refs/tinyusb |
| 0x1002ba78 / 0x1002ba8c / 0x1002bab4 | btstack_run_loop_set_timer / add / remove (verify) |
| 0x10048xxx cluster | Bluepad32 core (uni_*) — verify against refs/bluepad32-cluoma |
| FUN_200012bc (RAM) | __aeabi_uldivmod |

## Output conventions (MANDATORY)

1. Write clean C to your assigned file(s) under `/projects/Pico2maple-decomp/recon/src/`.
2. Every reconstructed function: doc comment with `// from 0x<addr> (<old FUN_ name>)`.
3. Unresolved semantics: keep code faithful, add `// TODO(verify @0x<addr>): <question>`.
   NEVER invent behavior. If a Ghidra artifact is unreadable, transcribe conservatively and flag.
4. Globals: name them meaningfully, declare at top with `// @0x<ram/flash addr>` comments.
   DAT_10001xxx literal-pool references in decomp are POINTERS to globals — the real global
   address is the value stored there (read it from the .bin if needed).
5. Use pico-sdk 2.2.0 APIs & types (refs/pico-sdk), Bluepad32 types (refs/bluepad32-cluoma).
6. Magic numbers for Maple protocol: use named constants (MAPLE_CMD_DEVICE_REQUEST=0x01, etc. —
   standard Maple command set: 01 dev req, 02 all status, 03 reset, 04 shutdown, 05 dev status,
   06 all status, 07 ack, 08 data xfer, 09 get cond, 0A get media info, 0B block read, 0C block
   write, 0D get last error/sync, 0E set cond; negative/0xFB-0xFF errors).
7. Also write `/projects/Pico2maple-decomp/recon/notes/<module>.md`: what each function does,
   confidence level per function (HIGH/MED/LOW), open questions, and a list of every global you
   named with its address. This is the review artifact — be precise.
8. Do NOT touch files outside your module's src file(s) + notes file.

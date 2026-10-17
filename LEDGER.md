# Pico2Maple Reconstruction Ledger

Target: `pico2maple_flipped-bt.uf2` — pico2maple, Pico 2 W (RP2350 ARM-S), pico-sdk 2.2.0, Release, build "Mar 4 2026".
Raw image: `pico2maple_flipped-bt_rp2350-arm-s_0x10000000.bin` @ 0x10000000 (668,672 bytes). Ghidra project: `ghidra/P2M`.

## Binary map

| Region | Contents | Status |
|---|---|---|
| 0x10000000–0x110 | Vector table (SP=0x20082000, reset=0x1000015d) | identified |
| 0x10000124 | binary_info header | parsed |
| 0x10000200–0x10006000 | **APP: main, Maple engine, devices, VMU, storage, settings, config** | reconstructing |
| 0x10006000–0x10009000 | **APP: TinyUSB host glue (0x73f4), Bluepad32 platform (0x82dc–0x8b68), BT main (0x8d14)** | reconstructing |
| 0x10009000–0x10019000 | pico-sdk runtime + TinyUSB | library (open source) |
| 0x10019000–0x1001c000 | carlk3 SDIO/FatFs | library (open source) |
| 0x1001c000–0x10057000 | cyw43-driver, BTstack, Bluepad32 core | library (open source) |
| 0x10057000–0x1005e4ac | .rodata: app strings @0x57000–0x575c8, lib strings | extracted |
| 0x1005e4ac/0x1005e4f0 | **maple_tx/maple_rx PIO programs (v1)** | decoded → `recovered/maple_pio_disasm.txt` |
| 0x1005fb4c–0x1005fb84 | SDIO PIO programs (carlk3 lib) | library |
| 0x10060f88 | cyw43 gSPI PIO program (pico-sdk) | library |
| ~0x10060000–0x1009960f | CYW43439 WiFi/BT firmware + CLM blobs | vendor blob |
| 0x100a16f4–0x100a1da7 | sample controller map cfg text | extracted verbatim |
| 0x100a2a18 | uni_platform struct (9 callbacks) | identified |
| 0x100a3400 | image end (.data load image at tail) | — |

## Key anchors

| Address | Name | Evidence |
|---|---|---|
| 0x10000608 | main | "Starting pico2maple" |
| 0x1000066c | maple_build_device_info_blocks | all device-info strings |
| 0x10000824 | vmu_build_device_info | "Visual Memory" + SEGA license |
| 0x1000098c | (2444-byte fn) TBD — likely maple protocol dispatch | size/position |
| ~0x10001a00–0x2250 | maple engine init + IRQ handlers (PIO struct literal pool @0x1b98/0x1ba0) | gap recovered via prologue scan |
| 0x10002568/0x2948 | VMU maple cmd handlers | "Pico2Maple USBBT"+VMU strings |
| 0x100039d0–0x3bf4 | SD dirs / vmu bank / gameid paths | path format strings |
| 0x10003e94–0x4218 | settings load/save (pico2maple_settings.bin) | settings string |
| 0x1000498c | cfg_parse_mapping_file | button-name strings |
| 0x10004d60 | cfg_write_sample_file | sample cfg string |
| 0x100073f4/0x7400 | tuh_mount_cb/tuh_umount_cb | TinyUSB mount strings |
| 0x100082dc–0x8b68 | Bluepad32 platform callbacks | uni_platform struct @0x100a2a18 |

## Module reconstruction status

- [x] maple.pio — TX/RX PIO source, **byte-exact** (tools/pio_check.py passes)
- [x] maple engine C (init, frame TX/RX, protocol dispatch) — recon/src/maple/maple.{c,h} (HIGH; response-header constants still placeholders)
- [x] VMU emulation (block r/w, LCD, bank/gameid, buzzer) — recon/src/maple/vmu.{c,h} (HIGH)
- [x] storage: SD mount/dirs/banks/gameid + settings.bin — recon/src/storage/{sdcard,settings}.c (HIGH)
- [x] config parser + mapping engine + sample writer — recon/src/config/mapping.{c,h} (HIGH)
- [x] OLED (SSD1306) driver + status glyphs — recon/src/oled/oled.{c,h} (HIGH; LCD upscale MED)
- [x] board init + alt pinout (GPIO15 sense) — recon/src/board.{c,h} (HIGH, verified)
- [x] assets (glyphs, root template, sample cfg) — recon/src/assets/p2m_assets.{c,h} (generated, exact)
- [x] main + core0 loop skeleton — recon/src/main.c (MED, structural)
- [~] DC layout mapper — recon/src/maple/dc_mapping.c (standard layout HIGH; variants 2/4/5/6 pending byte-exact tables)
- [x] TinyUSB host glue (Path B clean) — recon/src/usb/usb_host.{c,h} (per-controller offsets TODO(verify); xinput stubbed)
- [x] Bluepad32 platform (Path B clean) — recon/src/bt/bp32_platform.{c,h} (uni_* -> p2m_gamepad_t + rumble forward)
- [x] Maple response-header constants — decoded & inlined in recon/src/maple/maple_frames.h
- [x] Control flow integration — recon/src/control.{c,h}: USB+BT merge -> hotkeys (Select+combos)
      -> custom map -> dc layout mapper -> DC condition; rumble forward; Puru Puru hooks. Wired into main.
- [~] Puru Puru rumble: device hooks done (control.c); waveform parse TODO(verify @0x10007c14)
- [ ] Vendor deps (lib/ submodules) + CMake wiring -> first compile for pico2_w
- [ ] bt_main.c (cyw43/btstack init: bt_init/bt_ready) + usb xinput glue — stubs referenced by main
- [x] CMakeLists / build scaffold (pico-sdk 2.2.0, pico2_w) — recon/CMakeLists.txt
- See recon/PATH_B.md for the own-firmware architecture and milestones.

## Response-header constants still to dump/inline (maple.c TODOs)
DAT_10001eb8 (DEVICE_STATUS hdr), 0x1edc (ALL_STATUS), 0x1ec4 (=0x07000100 ACK), 0x1ec8/0x1ecc/
0x1ed0/0x1ed4/0x1ed8/0x1ee0/0x1ee8, and the 0x2188..0x21cc rumble/gameid frame cluster.

## Provenance notes

- File matches no published UF2 (repo `firmware/` checked by size+hash). Internal date Mar 4 2026. Likely out-of-repo "flipped" build.
- Author research: repo active to 2026-06-15; source deliberately proprietary (blog 2025-02); death not confirmed. Rights caution applies to redistribution; see report.
- Open references: Soopahfly/MapleSyrup (MIT), OrangeFox86/DreamPicoPort, mackieks/MaplePad (CC BY 4.0), charcole/Dreamcast-PopnMusic (CC BY-NC), cluoma/bluepad32 fork (Apache-2.0, cloned at /app/data/home/refs).

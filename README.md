# Microsap — Dreamcast Maple Adapter Firmware

*A from-binary reconstruction of cluoma's Pico2Maple ("Path B"), branded for Midwest Mods.
Bridges USB & Bluetooth controllers to a Sega Dreamcast, emulates dual VMU memory cards, and
runs on an RP2350 / Raspberry Pi Pico 2 W. Personal use only — not for sale or distribution.*

## Contents

- [Overview](#overview)
- [Status at a glance](#status-at-a-glance)
- [Hardware](#hardware)
- [Architecture](#architecture)
- [How it works](#how-it-works)
- [Building](#building)
- [Feature status](#feature-status)
- [Known controllers](#known-controllers)
- [Development history / key fixes](#development-history--key-fixes)
- [Developer workflow, diagnostics & remaining work](#developer-workflow-diagnostics--remaining-work)
- [Attribution & credits](#attribution--credits)
- [Third-party components](#third-party-components)
- [Use & distribution](#use--distribution)


## Overview

This repository contains **Path B**, a clean, buildable firmware reconstruction of cluoma's proprietary Pico2Maple controller adapter for the Sega Dreamcast. Built on the RP2350 (Raspberry Pi Pico 2 W) with pico-sdk 2.2.0, it bridges USB/Bluetooth game controllers, keyboards, and mice to a Dreamcast over the Maple bus, with full VMU memory card emulation (dual slot support), rumble feedback, and OLED status display. The firmware is reconstructed from a reverse-engineered binary decompilation and reuses byte-exact implementations of the hard-won Maple protocol core (PIO programs, frame headers, device info) while rebuilding I/O glue atop open-source libraries (TinyUSB, Bluepad32, BTstack, no-OS-FatFS). **This is personal-use firmware only** — designed for developers collaborating on the reconstruction; not for distribution or sale. It has been verified working on real Dreamcast hardware: controllers drive the console, the VMU appears in the BIOS and retail games save to it, and persistent storage survives power cycles.

## Status at a glance

This is an active reconstruction that **works on real Dreamcast hardware**. As of the latest
build: Bluetooth and USB controllers drive the console, two VMU memory cards (internal flash +
SD) appear in the BIOS and save real games, and controller saves persist across power cycles.
It is not a finished product — see the remaining-work list — but the core adapter function is
done and proven.

## Hardware

**Target board:** RP2350 / Raspberry Pi Pico 2 W

**Pinout (Maple Tap v1, standard layout — the default active layout for this build):**
- Maple bus (GPIO16/17, SDCKA/SDCKB)
- Buzzer: GPIO3 (PWM tone)
- CYW43 Bluetooth module: GPIO24 (DI), GPIO25 (CS), GPIO29 (SCLK), GPIO23 (BT-ON)
- OLED display (SSD1306, I2C): GPIO4 (SDA), GPIO5 (SCL)
- microSD card (SPI1): GPIO10 (SCK), GPIO11 (MOSI), GPIO12 (MISO), GPIO13 (CS)

**Layout selection:** GPIO15 is pulled up internally. If grounded at boot, the firmware switches to the alternate ("flipped") layout (Maple on GPIO6/7, buzzer GPIO26, CYW43 on different pins, 180° OLED rotation). The naming "flipped" in the original binary refers to the OLED panel orientation, not the electrical pinout.

## Architecture

The firmware splits responsibilities across the RP2350's two cores:

**Core0 (USB/Bluetooth input processing):** Polls USB HID devices and Bluetooth controllers via Bluepad32, normalizes input into a unified `p2m_gamepad_t` struct, applies custom button remapping, applies layout-specific Dreamcast condition building (standard, racing, twin-stick, fight-stick), drives rumble feedback back to the active pad, and handles hotkey combos (Select+button). Core0 builds the shared controller condition that Core1 broadcasts to the Dreamcast.

**Core1 (Maple bus engine & storage):** Runs a PIO-based Maple protocol engine (RX on PIO0, TX on PIO1) handling the Dreamcast's requests, emulates VMU storage blocks and LCD frames, manages SD card writes, handles the OLED refresh, and dispatches storage (block read/write, media info, get condition) requests to VMU banks (internal flash or SD-backed). Maple handshake is verified on real hardware.

**Source modules:**
- `maple/maple.c` / `maple.pio` — Maple protocol frame TX/RX (PIO programs, byte-exact; frame dispatch)
- `maple/vmu.c` — VMU emulation (block read/write, LCD, internal banks, GameID, Puru Puru buzzer)
- `maple/dc_mapping.c` — Convert normalized gamepad state into Dreamcast controller condition (standard layout verified; variants pending byte-exact tables)
- `usb/usb_host.c` — TinyUSB host glue for HID pads, keyboards, mice
- `usb/xinput.c` — XInput (Xbox) controller normalization via tusb_xinput
- `bt/bp32_platform.c` — Bluepad32 platform adapter (uni_controller_t → p2m_gamepad_t, rumble forward)
- `config/mapping.c` — Custom button-remap parser and runtime application
- `control.c` — Core0 integration: input merge, hotkey dispatch, layout switching, rumble tick
- `storage/sdcard.c` / `storage/settings.c` — SD card mount, directory management, VMU bank files, settings blob
- `oled/oled.c` — SSD1306 I2C driver, framebuffer, DMA refresh, status glyphs
- `board.c` — GPIO15 layout sensing, pin assignment struct population

## How it works

Pico2Maple runs as a two-core RP2350 firmware. **Core 1** is the Maple-bus engine: it owns the
PIO state machines, decodes host frames, and answers the Dreamcast in real time
(`recon/src/maple/maple.c`, `maple_core1_main`). **Core 0** runs the input stack: TinyUSB host,
Bluepad32/BTstack, the hotkey/mapping logic, and rumble output (`recon/src/control.c`). The two
cores communicate through a few shared globals (the controller/VMU condition blocks, the
sub-peripheral mask, bank-switch and device-info rebuild flags) and a `pico/util` queue for SD
writes. Everything below is reconstructed from the shipped binary; byte-exact pieces and
`TODO(verify)` gaps are called out in the source and in `recon/notes/`.

### Maple bus engine

**PIO transport.** The bus uses two PIO programs, reconstructed byte-exact from the binary in
`recon/src/maple.pio` (the PIO blobs live at flash `0x1005e4b4` (TX, 30 instructions) and
`0x1005e4f8` (RX, 24 instructions), both `pio_version=1`; `tools/pio_check.py` confirms they
re-assemble identically — see `recon/notes/maple.md`).

- **RX** is PIO0 SM0, `clkdiv = 1.0` (`0x00010000`), `SHIFTCTRL = 0x80890000`
  (FJOIN_RX | PUSH_THRESH=8 | autopush), `JMP_PIN` set to SDCKA, `IN_BASE = pin`
  (`maple.c`, PIO bring-up around lines 473-488). The RX program (`maple.pio`,
  `.program maple_rx`) is an edge/state-transition decoder: `x` holds the previous 2-pin state,
  `y` the new state, and it pushes one data bit per detected clock edge.
- **TX** is PIO1 SM0, `clkdiv = 0x0008e700` (≈ 8.902, giving ~16.85 MHz at a 150 MHz sysclk,
  i.e. a ~2 Mbps Maple cell), `SHIFTCTRL = 0x50060000` (FJOIN_TX | PULL_THRESH=8 | autopull),
  with `OUT_BASE`/`SET_BASE = pin`, 2-pin group (`maple.c` lines 490-505). The TX program emits
  the Maple start pattern (4 SDCKB pulses with SDCKA low), then two line phases per bit pair, then
  the end pattern (SDCKA pulsed low twice).

Both Maple GPIOs carry the PIO1 (TX) function permanently; PIO0 reads the raw input levels, so RX
needs no muxing. The pin base comes from `g_maple_pin` (board-config `@0x20002a01`); on this
"flipped"/alternate layout that is GPIO6 = SDCKA ("Maple 1") and GPIO7 = SDCKB ("Maple 2").
SDCKB's direction is flipped between PIO-output (during TX) and SIO-input (idle) via the RP2350
GPIO coprocessor single-bit ops (`gpioc_bit_*`).

**Frame format.** A frame is a little-endian header word
`[byte0 = length in 32-bit words][byte1 = source AP][byte2 = destination AP][byte3 = command]`,
followed by `length` payload words and a trailing XOR-checksum byte. Payload words are
**byte-swapped** on the wire.

- **RX** (`maple_rx_frame`, from `0x10001550`): the PIO autopushes 8-bit groups, but the start
  pattern leaves the stream misaligned by 2 bits, so each byte is rebuilt as
  `((prev & 3) << 6) | (cur >> 2)`. Sync is the first FIFO word satisfying `(w & 0xfc) == 0x84`.
  Byte 0 is the length; `expect = len*4 + 5` (4-byte header + `len*4` payload + 1 checksum). The
  final byte is compared to the running XOR; the function returns `0` on match, `-1` on checksum
  failure (`maple.c` lines 152-209).
- **TX** (`maple_tx_frame`, from `0x10001660`): stages the header word, then each payload word
  byte-reversed (`p[0]<<24 | p[1]<<16 | p[2]<<8 | p[3]`), then the XOR byte. It feeds one byte per
  FIFO word (left-justified `<<24`; the program consumes 8 bits per word via autopull) and waits
  for true drain using the `FDEBUG` TXSTALL handshake (clear, then wait for it to re-assert)
  before releasing the bus back to idle/RX (`maple.c` lines 213-280).

**Source-AP convention.** `0x20` is the main peripheral (the controller itself); sub-peripherals
identify by slot bits — `0x01` for slot 1, `0x02` for slot 2. On every TX the engine patches the
AP bytes: the live port number (`g_maple_port`, captured from a `DEVICE_REQUEST`'s `dst >> 6` when
`dst & 0x40`) is shifted into bits 7:6 of both src and dst, and if the template src byte has bit
`0x20` set (the "I am a sub-peripheral" path) the attached-sub-peripheral mask `g_subperiph_mask`
(`@0x20001668`) is OR-ed in (`maple.c` lines 220-227). For per-slot replies the dispatch uses the
`VMU_HDR(base, src)` macro to rewrite the header's source-AP byte (bits 8..15) to the addressed
slot's nibble (`0x01`/`0x02`).

**Command dispatch** (the `switch` in `maple_core1_main`, `maple.c` lines 570-723):

- **`DEVICE_REQUEST`** — captures the port, then: main device (`!(dst & 3)`) replies
  `DEVICE_STATUS` (cmd 05) with the 112-byte controller device-info body `g_devinfo_payload`; a
  VMU slot replies `DEVICE_STATUS` with the shared "Visual Memory" body and the slot's src-AP.
- **`ALL_STATUS_REQ`** — answered **only** when addressed to a VMU (the main device stays silent;
  a comment records that the console interrogates the VMU with ALL_STATUS and that silence here
  made the console drop the whole port). Payload is device-info + 80 zero bytes (192 B total).
- **`GET_CONDITION`** — to the main device (`dst & 0x20`): controller function
  (`0x01000000`) returns `g_dc_controller_cond`; mouse (`0x200`) and keyboard (`0x40000000`) are
  also handled. To a VMU it echoes the requested function with an all-zero condition. On idle
  windows (`frames_lo > 100`) this case also drives `vmu_flush_dirty` for the active VMU(s).
- **`GET_MEDIA_INFO`** — storage function rebuilds and returns the media-info block; the LCD
  function returns a short geometry reply.
- **`BLOCK_READ`** / **`BLOCK_WRITE`** — storage reads/writes a VMU block; LCD writes go to the
  VMU framebuffer and trigger an OLED refresh. Writes (and `GET_LAST_ERROR` write-sync,
  `SET_CONDITION`) reply with an ACK.
- **`SET_CONDITION`** — the clock function drives the VMU buzzer (`vmu_set_condition`).
- **`GAMEID`** (cmd `0x21`, an OpenMenu/GDEMU extension) — stores the game id and, if GameID mode
  is enabled in settings, switches to the per-game VMU bank.

**Reply headers** are decoded into named constants in `recon/src/maple/maple_frames.h`; each bakes
the payload length into byte 0 and carries a template src/dst (`01` = main, `02` = first
sub-peripheral) that the TX path then patches. Key ones:
`MAPLE_HDR_ACK_MAIN 0x07000100`, `MAPLE_HDR_DEVICE_STATUS_MAIN 0x0500201c` (28 words, src AP
`0x20`), `MAPLE_HDR_ALL_STATUS 0x06000130` (48 words/192 B), `MAPLE_HDR_DEVICE_STATUS_VMU
0x0500011c` (src `0x01`), `MAPLE_HDR_DATA_BLOCK_READ 0x08000182` (130 words),
`MAPLE_HDR_DATA_MEDIA_INFO 0x08000107` (7 words), `MAPLE_HDR_DATA_SHORT_VMU 0x08000102` (2 words),
and the error header `0xfc000200` (sub-peripheral file/function error).

### Controller input path

All three front-ends normalize into one `p2m_gamepad_t` — a packed 0x20-byte array `g[]`
(`recon/src/config/mapping.h`). The byte layout (verified against the mapper reads/writes):
digital `A,B,C,D,X,Y,Z,START` at `0x00..0x07`; D-pad at `0x09..0x0c`; RT analog at `0x0d`, LT
analog at `0x0e`; RB at `0x0f`, LB at `0x10`; analog stick 1 X/Y at `0x11`/`0x12`, stick 2 X/Y at
`0x13`/`0x14` (center `0x80`); second D-pad at `0x15..0x18`; and a misc byte at `0x19` holding
Select (bit 0), System/Home (bit 1), and Capture (bit 2). The misc byte is deliberately **not**
seen by the layout mapper — it is the hotkey channel.

- **USB HID** (`recon/src/usb/usb_host.c`) uses the public TinyUSB host API. `parse_gamepad`
  branches on VID/PID (Sony DualShock 4 / DualSense `0x054C`, Switch Pro `0x057E/0x2009`, plus a
  generic HID-gamepad fallback) and fills `g[]`; the same file routes HID keyboards into an 8-byte
  report buffer and HID mice into a delta accumulator. (Per-controller report offsets are marked
  `TODO(verify)`.)
- **USB XInput** (`recon/src/usb/xinput.c`) registers the Ryzee119 `tusb_xinput` driver via
  `usbh_app_driver_get_cb`; `map_pad` converts the `xinput_gamepad_t` (wButtons, bLeft/RightTrigger,
  signed thumbsticks) into `g[]`, scaling the 16-bit sticks to 0..255 and inverting Y for the
  Dreamcast convention. The pattern is derived from joypad-os / USBRetro (Robert Dale Smith,
  Apache-2.0). Back → Select, Guide → System.
- **Bluetooth** (`recon/src/bt/bp32_platform.c`) is a Bluepad32 platform whose
  `p2m_on_controller_data` callback converts `uni_controller_t` → `p2m_gamepad_t`
  (`gamepad_to_p2m`), with `axis_to_u8`/`trig_to_u8` scaling Bluepad32's signed ~[-512,511] sticks
  and [0,1023] triggers down to bytes.

**Priority and hotkeys** live in `controller_poll` (`control.c` lines 53-105). The active pad is
chosen in a fixed priority order: USB HID first (`usb_get_gamepad()`), then USB XInput
(`xinput_get_gamepad()`), then Bluetooth (`bp32_get_gamepad()`); if none is present the state is
forced neutral with sticks centered. When **Select is held** (`g[P2M_MISC] & P2M_MISC_SELECT`),
gameplay input is suppressed (a neutral condition is sent) and a combo bitmask is edge-detected:
Select+A → standard layout (3), Select+B → all-buttons (2), Select+X → twin-stick (5), Select+Y →
fight-stick (6), Select+DpadRight → custom (7), Select+DpadLeft → racing (4), Select+RB / Select+LB
→ VMU bank +1 / -1 (`maple_request_bank_switch`), Select+DpadDown → toggle rumble. Outside the
hotkey layer, the custom layout (7) first runs `apply_custom_mapping(g_custom_map, &gp)`, then
`dc_build_condition` (`recon/src/maple/dc_mapping.c`) selects the layout builder and writes
`g_dc_controller_cond` — the active-low 16-bit button word plus the analog triggers and sticks
(with a `snap_center` deadzone around `0x80`). The standard builder (case 3) is transcribed
exactly; the other built-in layouts are structurally reconstructed and flagged for byte-exact
verification.

### VMU emulation & storage

A VMU is a `vmu_state_t` (`recon/src/maple/vmu.c`) whose first `0x20000` bytes are the **128 KB
flash image** — 256 blocks × 512 B — with the root block (block 255, image offset `0x1FE00`)
required to begin with sixteen `'U'` (`0x55`) bytes. The full state struct also carries the LCD
framebuffer, the pre-built block-read/media-info response areas, the dirty-page bitmask, the
current bank, and a GameID buffer (full layout in `recon/notes/vmu.md`). In this reconstruction the
image lives in a static `.bss` buffer (`vmu_buf[0x20600]` in `maple_core1_main`) rather than on
core 1's stack as the original did.

- **Media-info geometry** (`vmu_media_info_build`): function `0x02` (storage), total 255 blocks,
  FAT at 254 (size 1), directory at 253 (size 13 / `0x0d`), icon shape read from image byte
  `0x1FE4E`, and 200 user blocks.
- **Block read** (`vmu_block_read`): rejects phased reads (`spec & 0xff0000`), copies
  `(spec & 0xff) * 0x200` bytes into the response area, and stamps the byte-swapped block number.
- **Block write** (`vmu_block_write`): the Maple protocol writes a 512 B block in 4 phases of 128
  bytes; the destination offset is `(((spec>>16)&0xff) + (spec&0xff)*4) * 0x80`, words are
  byte-swapped, and the page's dirty bit (`block >> 3`) is set.

**Internal-flash backing.** The internal VMU banks live in on-board flash starting at offset
`0xB9000` (`INTERNAL_BANKS_FLASH_OFF`), one 128 KB bank each; reads map directly through XIP
(`g_internal_banks = XIP_BASE + 0xB9000`). Dirty pages are persisted one 4 KB page per call by
`vmu_flush_dirty` in storage mode 0, using `flash_safe_execute` to erase+program the page while the
other core is parked (both cores opt in at boot: `flash_safe_execute_core_init` runs in
`maple_core1_main`, and core 0 is the lockout victim). Flushing runs only in idle windows
(`GET_CONDITION` with `frames_lo > 100`).

**SD-card backing.** Storage mode 1 uses FatFs (carlk3 no-OS-FatFS). `sd_ensure_dirs`
(`recon/src/storage/sdcard.c`) mounts the card, creates `/Microsap`, `/Microsap/internal`, and
`/Microsap/gameid`, `chdir`s into `/Microsap`, and sets `g_storage_status` to 2 (mounted) or 1
(no SD). Bank files are `vmu%02d.bin`; game-id saves go under `gameid/<id>/`; settings persist to
`microsap_settings.bin` (`recon/src/storage/settings.c`). Because flash erase/program cannot run
freely while core 0 executes from XIP, SD writes from the VMU engine are marshaled through
`g_sd_write_queue` (a `pico/util` queue of ~0x1014-byte `sd_write_msg_t` messages) and performed on
core 0.

**Dual VMU.** When an SD card is present (`g_storage_status == 2`), `maple_core1_main` brings up a
second `vmu_state_t` on controller **slot 2** via `vmu_init_slot2(vmu_buf2)`: that instance is
always SD-backed (storage mode 1), loads bank 2, and therefore uses backing file `vmu02.bin`
(mediainfo tag "Microsap SD VMU2"), while slot 1 stays internal-flash-backed. The dispatch routes
each frame by the destination slot bit: `to_vmu = (dst & 1) && (mask & VMU)`,
`to_vmu2 = (dst & 2) && g_dual_vmu`; `vp` selects the instance and `vsrc = (dst & 2) ? 0x02 : 0x01`
is patched into every reply's source-AP (`maple.c` lines 561-567). Dirty flushing runs for both
instances. Slot 2 is also the slot the Puru Puru rumble pack would occupy, so enabling dual-VMU
sets `g_subperiph_mask |= P2M_SUBPERIPH_RUMBLE` but answers that slot as a VMU — **rumble is off
while dual-VMU is active** (`maple.c` lines 456-462; `to_rumble` is gated on `!g_dual_vmu`).

### Bluetooth notes

Bluetooth is built on **upstream ricardoquesada/bluepad32 5.x** (beta) rather than cluoma's fork:
per `recon/BUILD.md`, cluoma's fork at HEAD is mid-migration (its code calls `hids_host_*` but its
CMake/btstack pin still names `hids_client`), so it does not build cleanly; its extra controller
parsers can be dropped in individually later. Bluepad32 5.x in turn needs BTstack v1.8.2+ (which
renamed `hids_client` → `hids_host`), newer than the one pico-sdk 2.2.0 bundles, so the build
points `PICO_BTSTACK_PATH` at bluepad32's pinned `external/btstack` and adds a one-line
`hids_client.c → #include "hids_host.c"` shim.

The one local Bluepad32 change is `recon/patches/bluepad32-xbox-ble-descriptor-at-setup.patch`
(ported from cluoma's fork, commit `9f888c9`). It modifies
`src/components/bluepad32/bt/uni_bt_le.c` to collect the HID descriptor **at setup time** — when
the HID service connects — instead of lazily on the first report. Xbox BLE pads need the descriptor
before the report parser is chosen; collecting it lazily races `set_ready` and produces a
connected-but-silent pad (the "connected but no input" stall).

Finally, `CYW43_ENABLE_BLUETOOTH=1` is required in the build (`recon/CMakeLists.txt`): it loads the
CYW43 Bluetooth firmware, and without it the HCI power-on fails.

## Building

**Prerequisites:**
- **pico-sdk 2.2.0** with submodules: `git -C <pico-sdk> submodule update --init lib/tinyusb lib/btstack lib/cyw43-driver lib/lwip`
- **ricardoquesada/bluepad32** 5.x (upstream; cluoma's fork does not build cleanly without mid-refactor fixes), with its `external/btstack` submodule initialized
- **BTstack v1.8.2+** (newer than pico-sdk 2.2.0's bundled copy; bluepad32's pinned btstack is used via `PICO_BTSTACK_PATH`)
- **carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico**
- **Ryzee119/tusb_xinput** (XInput host driver)
- **arm-gnu-toolchain 13.2.rel1** (arm-none-eabi-gcc)
- **cmake ≥ 3.29** and Make or Ninja

**Build commands:**
```sh
export PICO_SDK_PATH=/path/to/pico-sdk-2.2.0
export BLUEPAD32_ROOT=/path/to/bluepad32  # upstream, not cluoma's
export SDFATFS_ROOT=/path/to/no-OS-FatFS-SD
export PICO_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain

cmake -S recon -B recon/build -DCMAKE_BUILD_TYPE=Release
cmake --build recon/build -j3
```

**BTstack shim (required):** Create `<bluepad32>/external/btstack/src/ble/gatt-service/hids_client.c` containing:
```c
#include "hids_host.c"
```

This allows pico-sdk's source list (which still references `hids_client.c`) to compile against BTstack v1.8.2+.

**Build options:**
- Default output: `recon/build/midwestmods-2350-v1.00.uf2` (FW_VERSION defaults to 1.00; override with `-DFW_VERSION=1.01`)
- Debug serial console variant: `cmake -S recon -B recon/build-debug -DDEBUG_UART_OLED=ON` routes UART1 to GPIO4(TX)/GPIO5(RX) instead of the OLED header, disabling the display but allowing console logging if GPIO0 is not available on the board

**Flash:** Hold BOOTSEL on the Pico and copy the `.uf2` file to the RPI-RP2 mass storage drive.

## Feature Status

| Feature | Status | Notes |
|---------|--------|-------|
| Bluetooth controllers (Xbox, PS4, etc.) | Working | Bluepad32 + BTstack; verified on real hardware |
| USB HID controllers (generic) | Working | TinyUSB host; tested with various pads |
| USB XInput (Xbox wired) | Built | Integrated via tusb_xinput; controller offsets TODO(verify on hardware) |
| USB keyboard & mouse | Built | Boot-protocol parse + DC condition plumbing; not yet verified against a console |
| Dreamcast Maple handshake | Working | PIO-based RX/TX; frame dispatch; verified on real console |
| VMU internal (slot 1) | Working | RAM-backed with flash persistence (128 KB, sector-aligned via flash_safe_execute) |
| VMU SD-backed (slot 2) | Working | Loads vmu02.bin from SD; dual VMU both visible in BIOS |
| VMU block read/write | Working | Retail games (SNK vs Capcom, Crazy Taxi) save and load verified |
| VMU LCD (display) | Built | 48×32 upscale reconstructed; not verified on the OLED (status screen disabled) |
| Rumble (Puru Puru) | Working | DC vibration → USB/BT pad; **off while dual-VMU is active** (shares slot 2) |
| OLED splash | Working | Midwest Mods splash shown at boot, correct orientation |
| OLED status screen | Partial | Glyphs drawn but not presented — reconstructed offsets wrap page boundaries and garble; splash shown instead |
| Custom button remapping | Working | Loaded from SD config file or flash settings blob; applied at input |
| Layout variants (standard) | Working | Standard layout (case 3) verified; condition mapping byte-exact |
| Layout variants (racing/twin-stick/fight-stick) | Partial | Mapped (cases 4/5/6); switch cases reconstructed; TODO(verify @dc_mapping.c) byte-exact tables |
| Hotkey combos (Select+button) | Working | Trigger layout switches, bank cycling, VM invalidation |
| GameID support | Working | Loads game-specific saves from /Microsap/gameid/ folders |

## Building on Vendor Conflicts

If BTstack path conflicts occur or bluepad32 submodule updates are incomplete, verify:
1. `PICO_BTSTACK_PATH` environment variable points to `${BLUEPAD32_ROOT}/external/btstack` (v1.8.2+)
2. The hids_client.c shim is in place
3. `CYW43_ENABLE_BLUETOOTH=1` compile flag is set (required for BT firmware load)

If builds fail on old pico-sdk, verify you are using **2.2.0 with all submodules** — earlier versions bundle an incompatible BTstack and lack ARM-S (RP2350) support.

## Known controllers

Controllers tested against this firmware, with their quirks. Bluetooth pads go through
bluepad32; USB pads through TinyUSB host (HID) or tusb_xinput (XInput).

| Controller | Link | Status | Notes |
|---|---|---|---|
| Xbox Wireless Controller | BLE | Working, verified | Full standard-layout mapping verified bit-for-bit (`recon/notes/verified-button-map.md`). Fast link (~110–125 reports/s). Needed the BLE descriptor-at-setup patch to avoid a connected-but-silent stall. |
| "CECHZC2U PS3 clone" (generic BLE gamepad, VID `1949:0402`) | BLE | Working, but laggy | Not actually a DualShock 3 — a generic BLE HID gamepad advertising Class-of-Device **joystick** (`0x04`). Needed the joystick-CoD acceptance fix to connect at all. Streams at only ~15 reports/s and **ignores** the host's connection-interval request, so it has inherent input lag we cannot tune out (a limitation of this cheap pad, not the firmware). Details: `recon/notes/controller-cechzc2u-ble.md`. |
| Sony DualShock 4 / DualSense | USB | Parsed | `usb_host.c` VID/PID branch; report offsets marked `TODO(verify)` on hardware. |
| Nintendo Switch Pro | USB | Parsed | `usb_host.c`; offsets `TODO(verify)`. |
| Generic USB HID gamepad | USB | Fallback parse | Generic descriptor fallback in `usb_host.c`. |
| Xbox wired (XInput) | USB | Built | `usb/xinput.c` via tusb_xinput; not yet hardware-verified. |

### Lessons for adding controllers

- **Bluetooth Class-of-Device filter.** `p2m_on_device_discovered` (`bt/bp32_platform.c`) must
  accept the pad's CoD minor class or it is discovered-but-never-connected. The mask is `0x4c`
  (gamepad `0x08` | joystick `0x04` | keyboard `0x40`); widen it if a controller advertises a
  different class.
- **BLE input latency = connection interval.** The firmware requests a fast 7.5–15 ms interval
  at connect (local bluepad32 patch), but a peripheral may ignore it. Measure the real rate with
  the debug build's `bt N/s` / `age=` heartbeat fields during continuous stick motion: a healthy
  pad sustains 60–125/s; a slow one that ignores the request sits near 15/s.
- **"PS3 clone" is a label, not a protocol.** Many cheap pads sold as DS3 clones are plain BLE
  HID gamepads with borrowed VID/PIDs — analyze the actual HID report descriptor (e.g. on a
  Linux host via `usbhid-dump` / sysfs) before assuming DualShock 3 behavior.

## Development history / key fixes

This firmware went from "doesn't talk to the console at all" to "Bluetooth and USB
controllers work, and two VMU memory cards (internal flash plus SD) save and persist" over
many bench iterations. What follows is the condensed story, grouped by theme; every item is
drawn from a commit in this repository's history.

### Reconstruction and the Path B foundation

The project began as a decompilation of `pico2maple_flipped-bt.uf2` (a Dreamcast Maple-bus
adapter on the RP2350 / Pico 2 W) into clean, buildable C. The first commit landed the
toolchain (UF2 unpacker, Ghidra headless export, a PIO verifier, asset generators), the full
Ghidra inventory under `export/`, and nine reconstructed modules with evidence addresses. The
Maple TX/RX PIO programs were reconstructed byte-exact, and the alternate "flipped" pinout was
confirmed: grounding GPIO15 selects Maple on GPIO6/7.

From there the direction was set as **Path B** — an own-firmware build that reuses the
hard-won, byte-exact Maple engine, VMU, storage, config, OLED, and board code, but rebuilds
USB and Bluetooth input on open libraries (TinyUSB host and Bluepad32), both feeding one
normalized `p2m_gamepad_t`. The control layer (`control.c`) then tied input to hotkeys, custom
mapping, layout selection, Puru Puru rumble hooks, and the Dreamcast condition builder. The
whole thing first compiled and linked to a valid RP2350 UF2 against pico-sdk 2.2.0, bluepad32
5.x, BTstack v1.8.2, cyw43, TinyUSB host, and no-OS-FatFS — a build that took real wiring work
(TinyUSB host + BT + SD coexisting), plus moving the 128 KB VMU image off the core1 stack into
`.bss` to avoid a SCRATCH_X overflow, and renaming `board_init` to `p2m_board_init` to dodge a
TinyUSB BSP clash. Branding (the Midwest Mods splash and the `midwestmods-2350-v<FW_VERSION>.uf2`
output name) and a 180° OLED flip for the Dreamcast mounting orientation came in alongside.

### Getting it to boot: the core0-XIP flash crash loop

The first hardware boots reset-looped at `[core1] VMU init...` with garbled serial — the
classic signature of core0 faulting on XIP flash while core1 erased that same flash, because
the multicore lockout was not actually pausing core0. Two root causes were found: in the debug
build the OLED is disabled so its framebuffer is never allocated, and `vmu_init` dereferenced
it (fixed by guarding every OLED draw with `if (!g_oled_fb) return;`); and on a blank board the
internal-VMU region has no format markers, so core1 erased/programmed flash while core0 ran
from XIP. The decisive fix made the **internal VMU RAM-backed** — all flash writes removed from
core1, formatting and dirty-flush done in RAM — which stopped the crash loop at the cost of
internal-VMU persistence (deferred until the multicore flash path was sanctioned later). Boot
diagnostics over UART, non-fatal (timed-out) waits for core1 and Bluetooth, and the
`-DDEBUG_UART_OLED=ON` console on GPIO4/5 were all added here so the firmware would show where
it stalled instead of hanging silently.

Once the loop reached the main loop, Bluetooth still failed with "Failed to power on HCI": the
cyw43 driver was never told to load the BT firmware. Defining `CYW43_ENABLE_BLUETOOTH=1` let
the HCI transport power on the radio.

### Making the console hear us: the Maple handshake and a deaf RX

Controller input wasn't reaching the Dreamcast because the key Maple replies still carried
**placeholder headers** (`out.header = 0`), which are malformed frames the console can't parse.
The decoded headers were wired in (DEVICE_STATUS `0x0500011c`, ALL_STATUS `0x06000130`,
controller condition `0x08000103`), and frame/dev-req/get-cond counters were added so the
serial log could show whether the console was polling at all. Then a subtler protocol bug: the
main-device DEVICE_STATUS and controller GET_CONDITION headers needed **source AP `0x20`** (the
"main peripheral present" bit the Dreamcast requires); the reconstruction had used `0x01`, so
the console rejected both the device info and the condition.

Two init-time memory-corruption bugs were also flushed out around here. `maple_build_device_info_blocks`
was handed `g_devinfo_payload - 8`, writing 8 bytes *before* the array into adjacent `.bss` on
every init and layout rebuild; and `vmu_build_device_info` wrote a 112-byte body into a 28-byte
stack buffer, smashing 84 bytes of core1 stack locals. A boot-time **Maple loopback self-test**
(RX taps the pins while TX drives them, capturing our own ACK frame) was added so framing,
checksum, and both PIO engines could be verified on hardware without a console.

That self-test then captured **zero words**: the RX engine was completely deaf. Re-decoding the
original core1 init revealed two RX config fields the reconstruction had lost — EXECCTRL must
set `JMP_PIN = SDCKA` (`(pin & 0x1f) << 24`), because the RX program polls the line with
`jmp pin` (without it RX watched GPIO0); and the raw write at byte offset `0xD0` is
RP2350 **SHIFTCTRL.IN_COUNT = 2** (expose SDCKA/SDCKB to IN PINS), previously misread as a
PINCTRL no-op that left the IN pins masked. Either alone silences RX; together they fully
explained the dead exchange.

### The hardware finding: a broken SDCKB solder wire

With loopback passing and RX diagnostics (`rxw`/`rsync`) in place, the first console-connected
session showed `rxw` exploding into the thousands but `rsync == rxw` **exactly** — we heard the
console but never matched the `0x84` start residue. Rather than keep theorizing (the RX C
logic, PIO programs, and configs were all verified identical to the original, and the decoder is
provably A/B-swap-agnostic), the next build captured reality: a one-shot dump of the first
rejected RX FIFO words, and a raw SIO line-state probe of the first console frame. The rejected
words all came back **`0xFF`** — a pure bit-1 stream, meaning one line toggles while the other
never goes low with it — and the probe saw **SDCKB sitting low**. The conclusion: only SDCKA
reached the board; the SDCKB path (TP2 → GPIO17) was electrically **open**, a broken solder
wire, so no frame could ever decode. This was the final blocker to the console ever hearing us.
(The probe was hardened to pull both lines up first — a leftover TX pulldown on SDCKB had
masked the earlier reading — and to always print per-line edge counts, so a dead wire shows
zero edges unambiguously.)

### Making input register: the capability-mask byte order

With the wire repaired, the console enumerated us and polled GET_CONDITION at 30 Hz but ignored
all input. The culprit was the devinfo function-data word. The original stores the words
*verbatim* (e.g. `0xffff3f00`), and the Maple TX word-byte reversal delivers them to the console
as the canonical capability masks (`0x003fffff` etc.). The reconstruction had `memcpy`'d the
already-canonical value little-endian, so it arrived byte-reversed as garbage capabilities, and
the BIOS treated the controller function as having no usable inputs. Storing the binary-verbatim
words (with per-layout selection and the verbatim Twin Stick / Racing names, and the mouse word)
fixed it. A follow-up tuned layout 3 specifically: in Dolphin Blue (an Atomiswave conversion
that parses the capability mask) a real Sega pad's Start worked but ours didn't, because our
layout-3 word advertised the expanded mask instead of a real pad's `0x000f06fe`. Reporting the
canonical word (`0xfe060f00` stored) **made Dolphin Blue's Start button work**.

### The VMU saga: from rejected to dual memory cards

The VMU was the longest thread. It "worked in menus, died in games" because games — unlike the
BIOS menu — interrogate the VMU we advertise, and every sub-device reply still had placeholder
headers; games also poll the VMU's functions with GET_CONDITION, which the firmware silently
ignored (285 unanswered polls before the console dropped the port). Completing the **VMU
sub-device protocol** wired the real replies resolved from the binary: VMU DEVICE_STATUS
`0x0500011c` with the 112-byte devinfo body, storage GET_MEDIA_INFO `0x08000107` with the real
media response (previously an empty stack buffer), LCD media info, BLOCK_READ `0x08000182` with
a file-error fallback `0xfc000200`, and a VMU GET_CONDITION that echoes the function with an
all-zero condition.

Even then the console could reject a *well-formed but wrong* device. An A/B test disabling VMU
advertising confirmed the console was parsing replies yet rejecting the enumeration result and
restarting the port; the prime suspect was a garbage ALL_STATUS reply (192 bytes of
uninitialized stack under a valid header). That was rebuilt properly at init. The real
breakthrough: re-reading the original dispatch showed **ALL_STATUS belongs to the VMU, not the
main device** — it replies only when addressed to the VMU, with header `0x06000130` whose source
AP is `0x01` (slot 1); the reconstruction had it inverted, so the console's VMU interrogation
met silence.

A separate identity bug nearly undid everything after boot: the core1 rebuild path passed a
*layout id* (e.g. 3) where the device-info builder expected a function code (1 / 0x40 / 0x200),
so a rebuilt block described a device with **zero functions**. The first enumeration used the
good init-time info, so sessions started fine, but any later enumeration (console power cycle,
reconnect) read the corrupted block and ignored the controller. Passing `MAPLE_FUNC_CONTROLLER`
(the builder reads the layout internally) made the device survive re-enumeration.

With saves landing but the BIOS card view sometimes showing nothing, VMU inspection was added —
a write counter with the last raw write spec, a `P2M_VMU_DIR` serial dump of the directory
entries and FAT occupancy, then read-side telemetry (reads, file-error replies, last read spec,
media-info count) — to split "blocks never arrived" from "written but not displayed/read back."
The RAM-backed era then ended: dirty 4 KB pages now **flush to the internal flash bank** via
`flash_safe_execute` during idle windows, so slot-1 saves survive power cycles. Finally, **dual
VMU** landed — beyond the original firmware, which presented only one card: when an SD card is
mounted, controller slot 2 hosts a second, independent VMU backed by `vmu02.bin`, so the BIOS
shows two memory cards (A1 + A2). It occupies the rumble slot, so rumble is off while dual-VMU
is active; the dispatch selects the target VMU per frame by the destination slot bit and patches
each reply's source-AP byte to match.

### Bluetooth reliability and input latency

Bonds originally lived only in RAM: btstack's link-key store writes flash through
`flash_safe_execute()` on core0, which requires the *other* core to have called
`flash_safe_execute_core_init()`. Neither core had opted in, so every key save silently failed
and a power cycle wiped the pairing. Having both cores opt in (and routing `settings_save`
through the same sanctioned path) made **Bluetooth pairings persist across power cycles** and
removed the old core1-writes-flash-while-core0-runs-XIP landmine on VMU bank switches.

Latency work was driven by measurement, not guessing: the heartbeat gained a pad report rate
and the age of the freshest report. An attempt to forbid classic-BT SNIFF mode (which adds
10-30 ms of latency) backfired — Xbox pads *require* sniff support and refuse to connect without
it — so that was reverted and the default link policy restored. The infamous **zombie-stream
bug** (link up, zero input reports, no stack errors, striking on reconnect and mid-session) was
fixed by porting cluoma's own targeted bluepad32 patch: collect the BLE HID descriptor at
`GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED` (setup) rather than lazily on the first report,
*after* the parser has already been chosen (recorded as
`recon/patches/bluepad32-xbox-ble-descriptor-at-setup.patch`). Along the way the BT stack's
info-level logging was quieted to errors-only, because btstack and bluepad32 printf from async
context and raced the serial diagnostics, garbling every log line that mattered.

### USB XInput and input mapping verification

USB Xbox (XInput) support filled a long-standing stub using Ryzee119/tusb_xinput, normalizing
`xinput_gamepad_t` into `p2m_gamepad_t` (DC stick convention with Y inversion, analog triggers,
Back→Select hotkey modifier), with a `USB HID → XInput → Bluetooth` priority in `controller_poll`
and rumble dispatch to the active pad.

Input mapping was validated against real hardware throughout. The heartbeat was extended to
print the live Dreamcast controller condition (button mask, triggers, both sticks) so the full
input→mapping→condition chain could be watched from the bench without a Dreamcast; a neutral
no-pad / hotkey-suppressed condition was corrected to 0x80 on all four stick axes (a zeroed
neutral pad otherwise read as full up-left because the center-snap only snaps near 0x80). That
tooling culminated in a 16/16 guided button-map sweep verifying every standard-layout input from
an Xbox BT pad through to the exact Dreamcast condition bits — including the faithful LB→full
right-trigger and RT-full-pull→C cross-routing — recorded in
`recon/notes/verified-button-map.md`. One deliberate, documented deviation followed: in Crazy
Taxi the gas pedal reads the analog right trigger, so Xbox RT's analog value now also feeds
`rtrig` (max of the two sources) while keeping the original cross-routing.

### The phantom mode button

A late field bug: mid-game the console would flash "controller removed / memory card
recognized" while controls stalled. The reconstructed mode button had been *guessed* onto
GPIO15 — which on the Maple Tap is the **floating layout-sense pin**. Noise read as presses,
causing random VMU bank switches (the on-screen messages) and `settings_save` flash writes whose
`flash_safe_execute` parked core0 (Bluetooth) mid-write (the input stall). The button was
disabled until its real pin can be read from the board schematic; controller hotkeys provide the
same functions.

## Developer workflow, diagnostics & remaining work

### The debug serial console

The Maple Tap v1 board does not break out GPIO0/1 (the normal UART0 console), so the debug build
(`-DDEBUG_UART_OLED=ON`) routes stdio to **UART1 on GPIO4 (TX) / GPIO5 (RX)** — the OLED SDA/SCL
pads, next to GND — at **115200 baud**. The OLED is disabled in this build because it shares
those pins. Wire a 3.3 V USB-serial adapter's RX to GPIO4 and GND to GND to read the log. The
companion `tools/flash.sh` drives a full reflash over this link without touching BOOTSEL.

**Serial console commands** (type the token and press Enter; parsed in the core0 loop in
`recon/src/main.c`):

| Command | Effect |
|---|---|
| `P2M_BOOTSEL` | Reboots into the USB bootloader (RP2 mass-storage drive) so a new `.uf2` can be copied; `tools/flash.sh` uses this. |
| `P2M_REBOOT` | Plain watchdog reboot — re-runs boot and the Maple loopback self-test. |
| `P2M_BT_CLEAR` | Wipes all stored Bluetooth bonds (`uni_bt_del_keys_safe`) for a clean re-sync after key-state confusion, without disabling bond persistence. |
| `P2M_BT_SCAN` | Forces the pairing/scanning window open (`uni_bt_start_scanning_and_autoconnect_safe`) instead of guessing at the stack's scan state. |
| `P2M_VMU_DIR` | Dumps VMU diagnostics: write counter + last write spec, read counter + file-error count + last read spec + media-info request count, the first directory entries, and the FAT non-free-entry count — straight from the card image, so you can see whether a game's save actually landed. |

### Heartbeat telemetry

Every ~3 s the core0 loop prints a `[hb]` line so the log shows the loop is alive and what the
whole chain is doing. The fields and what they diagnose:

- **`sd=`** — storage mount state (`1` = no SD / not mounted, `2` = SD mounted). A recurring
  field because the one-shot boot status line kept getting shredded by serial noise right when
  the first SD test needed it. `sd=2` is also what enables the second (dual) VMU.
- **`usb_pad` / `xpad` / `bt_pad`** — which input source is connected (USB HID, USB XInput,
  Bluetooth), plus a `[pad active]` marker when any normalized gamepad is live.
- **`layout=`** — the active Dreamcast layout id (standard / all-buttons / twin-stick /
  fight-stick / custom / racing).
- **`DC rxw=`** — raw Maple RX FIFO words heard from the bus. Zero means no line activity (dead
  RX or a dead line); nonzero means we're hearing the console.
- **`rsync=`** — RX resyncs. The telltale `rsync == rxw` state means we hear the console but
  never match the frame start — the symptom that exposed the open SDCKB wire.
- **`frames=`** — decoded Maple frames. The jump from "activity but no sync" to real frames is
  how you know RX is actually decoding.
- **`devreq=recv/sent`** — DEVICE_STATUS requests received vs. replies transmitted. A ratio that
  climbs while `cond` is frozen is a re-enumeration storm (console rejecting our enumeration);
  splitting recv from sent tells you whether the problem is unanswered requests or rejected
  replies.
- **`cond=recv/sent`** — GET_CONDITION polls received vs. answered. A growing gap (sent lagging
  recv) is how a console decides a device is dead — it marks functions it polls that we fail to
  answer.
- **`miss=`** — a packed code for the first unanswered condition poll (destination AP + function
  + length), so a gap can be attributed to a specific function code (e.g. a sub-peripheral the
  BIOS never polls but a game does).
- **`bt N/s` and `age=Nms`** — Bluetooth pad report rate and the age of the freshest report, so
  input latency is measured rather than guessed.
- **`out: btn=… lt=… rt=… stick=… s2=…`** — the live Dreamcast controller condition being sent:
  the active-low button mask, both analog triggers, the main stick, and the second stick
  (`s2`, used by the twin-stick layout). This lets the full input→mapping→condition chain be
  verified from the bench without a Dreamcast.

### Remaining work and known issues

Gathered from the `TODO(verify)` markers in `recon/src` and the notes:

- **OLED status screen is splash-only.** The bank/INT/SD status glyphs are drawn into the
  framebuffer but never presented, because the reconstructed glyph offsets wrap across page
  boundaries and render garbled (`maple/maple.c`, `oled/oled.c`). The clean splash is shown
  instead until the glyph offsets, scale, and orientation are corrected against hardware.
- **Per-Bluetooth-connection variance.** On some connections the upper face buttons (X/Y) and
  the shoulders/triggers are absent from the parsed report entirely, and latency varies with
  whether the pad enters sniff mode (which must be allowed, because Xbox pads refuse to connect
  otherwise). Reconnecting the pad is the current workaround. The descriptor-at-setup fix was
  applied only to the **BLE** path; the classic (BR/EDR) lazy-descriptor path still awaits the
  same treatment, which is the suspected cause of the dual-mode parse difference.
- **Layout-variant condition builders are partial.** The standard layout (case 3) is verified
  byte-exact, but the twin-stick, fight-stick, and racing variants
  (cases 2/4/5/6 in `dc_mapping.c`) still need their exact mask tables reconstructed from the
  binary.
- **Dual VMU vs. rumble is mutually exclusive.** Dual-VMU occupies the slot the rumble pack
  would use, so rumble is off whenever a second (SD-backed) VMU is active. This could be exposed
  as a user toggle rather than being driven purely by SD presence.
- **The physical mode button is disabled** pending its real GPIO assignment from the board
  schematic (see the phantom-button story above); controller hotkeys cover its functions
  meanwhile.
- **Assorted `TODO(verify)` hardware-detail markers** remain across the tree: USB controller
  report byte offsets (DS4/DualSense, Switch Pro, generic, XInput) in `usb/`; Maple GPIO
  OUT/OE ordering, pull direction, and EXECCTRL field decode in `maple/maple.c`; VMU write-op
  byte and head field order in `maple/vmu.c`; the SD partial-write size gate in
  `storage/sdcard.c`; the BTstack run-loop integration in `bt/bt_main.c`; the Puru Puru
  waveform parse in `control.c`; and the cfg_a/b roles and buzzer freq/duty derivation in
  `board.c`.

## Attribution & credits

- **Garpike / cluoma** — original author of the closed-source Pico2Maple firmware this project
  reconstructs. The hard-won Maple-bus protocol work (PIO programs, frame headers, device-info
  layout, VMU geometry) is his; this firmware reuses it byte-for-byte where it was recovered
  from the binary, and his open-source [bluepad32 fork](https://github.com/cluoma) commits were
  the key reference for the Xbox Bluetooth reliability fix.
- Reverse engineering and reconstruction performed with Ghidra against the original UF2.
- Dreamcast Maple reference cross-checks: the open-source **MapleSyrup** and
  **joypad-ai/joypad-os** (formerly USBRetro) projects — the USB XInput host glue is ported
  from joypad-os (Apache-2.0), and the generic-HID parsing approach is informed by it.

## Third-party components

| Component | Role | License |
|---|---|---|
| Raspberry Pi **pico-sdk** 2.2.0 | RP2350 platform, TinyUSB host, BTstack, CYW43 | BSD-3 |
| ricardoquesada/**bluepad32** 5.x | Bluetooth gamepad stack (over BTstack) | Apache-2.0 |
| **BTstack** 1.8.x | Bluetooth host stack | BlueKitchen (dual; free for non-commercial) |
| carlk3/**no-OS-FatFS-SD** | microSD FatFs layer | Apache-2.0 |
| Ryzee119/**tusb_xinput** | USB XInput (Xbox) host driver | MIT |
| joypad-ai/**joypad-os** | XInput glue reference (ported, attributed in-file) | Apache-2.0 |
| LUFA HID parser (via joypad-os) | HID report-descriptor parsing reference | MIT/ISC |

Local patch `recon/patches/bluepad32-xbox-ble-descriptor-at-setup.patch` must be applied to the
bluepad32 checkout; it is a port of cluoma's fork fix and carries that attribution.

## Use & distribution

**Personal use only.** This firmware is a collaborative reconstruction built to keep the
project alive after the original author went quiet; it is **not to be sold or distributed**.
The reused protocol work belongs to the original author, and the third-party components carry
their own license terms (above) — honor all of them. The "Midwest Mods" / "Microsap" branding
marks personal builds and is not an endorsement by, or affiliation with, the original author.

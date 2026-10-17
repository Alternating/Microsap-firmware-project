# Path B — our own Pico2Maple-compatible firmware

Goal: a clean, buildable firmware (personal use, not for distribution) that behaves like
pico2maple — bridges USB/Bluetooth controllers, keyboards and mice to a Sega Dreamcast over the
Maple bus, with VMU, GameID, rumble and an OLED status display. We reuse the parts we reverse-
engineered where they are the hard-won value, and build on open libraries everywhere else.

## Architecture

```
          Dreamcast  <--Maple bus (GPIO6/7)-->  [ core1: Maple engine ]   <-- OURS (byte-exact PIO)
                                                       |
                            controller condition / VMU / device-info frames
                                                       |
   USB pads/kbd/mouse --TinyUSB host--> [ usb_host ] --+
   BT controllers ------bluepad32------> [ bp32_platform ]--> p2m_gamepad_t --> custom mapping
                                                                              --> layout mapper
                                                                              --> DC condition
   microSD (FatFs) <--> [ storage ]  (VMU saves, gameid, settings, cfg)
   SSD1306 OLED    <--> [ oled ]     (VMU LCD mirror + status glyphs)
```

## What we keep verbatim (the valuable reverse-engineered core)
- `maple.pio` — TX/RX PIO programs, byte-exact. This is the crown jewel.
- `maple.c` / `maple_frames.h` — Maple protocol engine + decoded response headers.
- `vmu.c` — VMU emulation (block r/w, LCD, banks, GameID, buzzer), struct layout verified.
- `mapping.c` — the custom controller-map parser + remap algorithm (exact).
- `board.c` — pinout incl. the GPIO15 alternate-layout sense (this build's default).
- `assets` — glyphs, FAT+root template, sample cfg (extracted exact).

## What we build clean on open libraries (don't need to be byte-identical)
- `usb_host.c` — TinyUSB host (hathach/tinyusb) + XInput (Ryzee119/tusb_xinput). Produces the
  normalized `p2m_gamepad_t`. Reference: DreamPicoPort, tusb_xinput examples.
- `bp32_platform.c` — a thin Bluepad32 platform (ricardoquesada/bluepad32, or cluoma's fork which
  already carries his controller additions) that maps `uni_controller_t` -> `p2m_gamepad_t`.
- `storage` uses carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico directly.

## Normalized controller struct (the integration contract)
`p2m_gamepad_t` (mapping.h): digital buttons + two analog sticks + two triggers + dpads, at the
byte offsets the custom-map and layout mappers expect. BOTH usb_host and bp32_platform fill this
same struct, so everything downstream is source-agnostic.

## Milestones
1. [done] Maple engine + VMU + storage + config + OLED + board, with decoded frame headers.
2. usb_host.c: enumerate + normalize USB HID / XInput pads, keyboard, mouse -> p2m_gamepad_t.
3. bp32_platform.c: Bluepad32 glue -> p2m_gamepad_t; rumble forwarding back to the pad.
4. Vendor deps as submodules (lib/), wire CMake, first compile for pico2_w.
5. Flash + bring-up: UART log, then Maple handshake on a real Dreamcast, then VMU/OLED.
6. Fill remaining layout-variant tables (twin-stick / fight-stick / racing) + verify TODOs.

## Rights
Personal, non-distribution continuation of Garpike/cluoma's project (collaborator, author silent).
Keep the repo private; the reverse-engineered binary is the reference, the shipping firmware is
our own clean build on open-source libraries.

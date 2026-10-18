# Building the firmware

Path B own-firmware for the Pico 2 W (RP2350). Target output: `pico2maple.uf2`.

## Prerequisites
- `arm-none-eabi-gcc` (tested with Arm GNU Toolchain 13.2.rel1)
- CMake ≥ 3.13 and Make/Ninja
- Checkouts of the external components (any location; point the env vars at them):
  - **pico-sdk 2.2.0** with submodules initialized:
    `git -C <pico-sdk> submodule update --init lib/tinyusb lib/btstack lib/cyw43-driver lib/lwip`
  - **ricardoquesada/bluepad32** 5.x (`src/components/bluepad32` builds as `libbluepad32`),
    with its `external/btstack` submodule initialized. We build on upstream bluepad32 because
    cluoma's fork at HEAD is mid-migration (its code calls `hids_host_*` but its CMake/btstack
    pin still name `hids_client`), so it doesn't build cleanly; cluoma's extra controller
    parsers (`uni_hid_parser_*.c`) can be dropped in individually later.
  - **carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico**
  - (optional) **Ryzee119/tusb_xinput** once the xinput glue is written

### btstack version note (important)
Bluepad32 5.x needs **BTstack v1.8.2+** (which renamed `hids_client` → `hids_host`), newer than
the one pico-sdk 2.2.0 bundles. The build points `PICO_BTSTACK_PATH` at bluepad32's pinned
`external/btstack`. pico-sdk 2.2.0's `pico_btstack` source list still names `hids_client.c`, so
add a one-line shim in that btstack: create
`external/btstack/src/ble/gatt-service/hids_client.c` containing `#include "hids_host.c"`.
(Equivalent to the pico-sdk patch the bluepad32 pico_w example documents.)

## Configure + build
```sh
export PICO_SDK_PATH=/path/to/pico-sdk          # must be 2.2.0 with submodules
export BLUEPAD32_ROOT=/path/to/bluepad32        # cluoma's fork
export SDFATFS_ROOT=/path/to/no-OS-FatFS-SD
export PICO_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain

cmake -S recon -B recon/build -DCMAKE_BUILD_TYPE=Release
cmake --build recon/build -j3        # btstack is large; -j3 avoids fork exhaustion on small hosts
```
Result: `recon/build/pico2maple.uf2`. Hold BOOTSEL and copy it to the RPI-RP2 drive to flash.

## Hardware notes
- Console log is on **UART0 (GPIO0 TX / GPIO1 RX)** at 115200 — USB stays free for host mode.
- Ground **GPIO15** to select the alternate ("flipped") pinout this project uses
  (Maple on GPIO6/7, etc. — see `src/board.c`).
- SD card SPI wiring is in `src/hw_config.c` (SPI1, SCK=10/TX=11/RX=12/CS=13).

## Config headers (in `src/`)
- `tusb_config.h` — TinyUSB **host** mode (USB controllers/kbd/mouse)
- `btstack_config.h`, `sdkconfig.h` — BTstack + Bluepad32 (from cluoma's pico_w example)

## Known gaps before it's flash-ready
See `../LEDGER.md`. Highlights: the BTstack run-loop integration in `bt_main.c`, the xinput
glue, and the ~30 `TODO(verify)` markers (controller report offsets, Maple timing, VMU offsets)
that need confirmation on real hardware.

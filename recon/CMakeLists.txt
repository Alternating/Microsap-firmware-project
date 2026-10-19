# Pico2Maple-compatible firmware (Path B) — RP2350 / Pico 2 W, pico-sdk 2.2.0.
# Integrates: TinyUSB host (USB pads/kbd/mouse), Bluepad32 + BTstack + cyw43 (BT),
# no-OS-FatFS-SD (microSD), and our Maple engine / VMU / OLED / config.
cmake_minimum_required(VERSION 3.13)

# --- external component roots (override on the command line for your own checkout) ---
set(PICO_SDK_PATH   "$ENV{PICO_SDK_PATH}"   CACHE PATH "pico-sdk 2.2.0")
set(BLUEPAD32_ROOT  "$ENV{BLUEPAD32_ROOT}"  CACHE PATH "ricardoquesada/bluepad32 5.x (+ external/btstack)")
set(SDFATFS_ROOT    "$ENV{SDFATFS_ROOT}"    CACHE PATH "carlk3 no-OS-FatFS-SD")
set(TUSB_XINPUT_ROOT "$ENV{TUSB_XINPUT_ROOT}" CACHE PATH "Ryzee119/tusb_xinput")
if(NOT TUSB_XINPUT_ROOT)
    set(TUSB_XINPUT_ROOT /app/data/home/refs/tusb_xinput)
endif()
if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH to a pico-sdk 2.2.0 checkout")
endif()
# Bluepad32 5.x needs BTstack v1.8.2+ (has hids_host); pico-sdk bundles an older one.
# Use the btstack that bluepad32 pins (its external/btstack submodule) via PICO_BTSTACK_PATH.
if(NOT BTSTACK_ROOT)
    set(BTSTACK_ROOT ${BLUEPAD32_ROOT}/external/btstack)
endif()
set(PICO_BTSTACK_PATH ${BTSTACK_ROOT})
# Files new in btstack v1.8 that pico-sdk's source list doesn't yet include.
set(BTSTACK_MISSING_SOURCES ${BTSTACK_ROOT}/src/hci_event_builder.c)

set(PICO_BOARD pico2_w)
set(PICO_PLATFORM rp2350-arm-s)

include(pico_sdk_import.cmake)
project(pico2maple C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# Firmware identity: output is  midwestmods-2350-v<FW_VERSION>.uf2
# (name = midwestmods, platform = 2350 = RP2350, version = FW_VERSION). Bump per release, e.g.
#   cmake -S recon -B recon/build -DFW_VERSION=1.01
set(FW_VERSION "1.00" CACHE STRING "firmware version (goes into the .uf2 filename)")

add_executable(pico2maple
    src/main.c
    src/control.c
    src/maple/maple.c
    src/maple/vmu.c
    src/maple/vmu_backend.c
    src/maple/dc_mapping.c
    src/maple/maple_devices.c
    src/maple/pulsar_card.c
    src/storage/sdcard.c
    src/storage/settings.c
    src/config/mapping.c
    src/oled/oled.c
    src/usb/usb_host.c
    src/bt/bp32_platform.c
    src/bt/pulsar_link.c
    src/board.c
    src/assets/p2m_assets.c
    src/assets/bootlogo.c
    src/hw_config.c
    src/bt/bt_main.c
    src/usb/xinput.c
    ${TUSB_XINPUT_ROOT}/xinput_host.c
    ${BTSTACK_MISSING_SOURCES}   # hci_event_builder.c, new in btstack v1.8
)

pico_generate_pio_header(pico2maple ${CMAKE_CURRENT_LIST_DIR}/src/maple.pio)

# tusb_xinput ae4f3a9 declares xinputh_open as uint16_t (it returns the consumed descriptor
# length) where TinyUSB 0.18's driver table wants bool. GCC >= 14 makes that initializer a hard
# error; keep it the warning it was under the 13.2 toolchain BUILD.md was tested with. TinyUSB
# never calls that pointer: src/usb/xinput.c hands it a bool-typed adapter instead.
set_source_files_properties(${TUSB_XINPUT_ROOT}/xinput_host.c PROPERTIES
    COMPILE_OPTIONS "-Wno-error=incompatible-pointer-types")

target_include_directories(pico2maple PRIVATE
    src
    ${TUSB_XINPUT_ROOT}
    ${BLUEPAD32_ROOT}/src/components/bluepad32/include
    ${BTSTACK_ROOT}/src
    ${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include
    ${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include
)

# TinyUSB host needs the app's tusb_config.h on the include path (src/).
target_compile_definitions(pico2maple PRIVATE
    PICO_STACK_SIZE=0x1000
    PICO_CORE1_STACK_SIZE=0x1000     # VMU image is now static (.bss), so default stack is fine
    CFG_TUSB_CONFIG_FILE="tusb_config.h"
    CYW43_ENABLE_BLUETOOTH=1         # load the CYW43 BT firmware; without it HCI power-on fails
)

# Debug console on the OLED header (Maple Tap v1 doesn't break out GPIO0). Routes stdio to
# UART1 on GPIO4(TX)/GPIO5(RX) — the OLED SDA/SCL pads — and disables the OLED (shared pins).
# Enable with:  cmake -S recon -B recon/build -DDEBUG_UART_OLED=ON   then read GPIO4 @115200.
option(DEBUG_UART_OLED "Route serial console to UART1 on the OLED header (GPIO4/5); disables OLED" OFF)
if(DEBUG_UART_OLED)
    target_compile_definitions(pico2maple PRIVATE
        DEBUG_UART_OLED=1
        PICO_DEFAULT_UART=1
        PICO_DEFAULT_UART_TX_PIN=4
        PICO_DEFAULT_UART_RX_PIN=5
    )
    message(STATUS "DEBUG_UART_OLED: console on UART1 GPIO4(TX)/GPIO5(RX); OLED disabled")
endif()

target_link_libraries(pico2maple PRIVATE
    pico_stdlib
    pico_multicore
    pico_flash
    hardware_watchdog
    hardware_pio
    hardware_pwm
    hardware_dma
    hardware_flash
    hardware_i2c
    hardware_spi
    tinyusb_host
    tinyusb_board
    pico_cyw43_arch_none
    pico_btstack_classic
    pico_btstack_ble
    pico_btstack_cyw43
    bluepad32
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

# Bluepad32 + SD library as sub-builds. libbluepad32 and BTstack both #include our
# btstack_config.h / sdkconfig.h, so src/ must be a global include dir before the sub-build.
include_directories(${CMAKE_CURRENT_LIST_DIR}/src)
add_subdirectory(${BLUEPAD32_ROOT}/src/components/bluepad32 libbluepad32)
add_subdirectory(${SDFATFS_ROOT}/src build_sdfatfs)

# Console on UART0 (GPIO0/1); USB peripheral stays free for host mode.
pico_enable_stdio_uart(pico2maple 1)
pico_enable_stdio_usb(pico2maple 0)

# Brand + version the output artifacts: midwestmods-2350-v<FW_VERSION>.{uf2,elf,bin}
set_target_properties(pico2maple PROPERTIES OUTPUT_NAME "midwestmods-2350-v${FW_VERSION}")

pico_add_extra_outputs(pico2maple)   # produces midwestmods-2350-v<FW_VERSION>.uf2

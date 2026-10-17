# Reconstruction build scaffold for pico2maple (RP2350 / Pico 2 W, "flipped-bt" variant)
# Matches the original's stack: pico-sdk 2.2.0, TinyUSB host, Bluepad32 (cluoma fork),
# BTstack, cyw43-driver, carlk3 no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w)
set(PICO_PLATFORM rp2350-arm-s)

include(pico_sdk_import.cmake)          # pico-sdk 2.2.0
project(pico2maple C CXX ASM)
pico_sdk_init()

# External libs (populate as git submodules in lib/)
#   lib/bluepad32        -> https://github.com/cluoma/bluepad32 (the author's own public fork)
#   lib/tusb_xinput      -> https://github.com/Ryzee119/tusb_xinput
#   lib/no-OS-FatFS      -> https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico

add_executable(pico2maple
    src/main.c
    src/maple/maple.c           # Maple bus engine (PIO + protocol)
    src/maple/maple_devices.c   # device-info blocks, controller/mouse/kbd/twinstick/racing
    src/maple/vmu.c             # Visual Memory emulation (block r/w, LCD, clock)
    src/maple/purupuru.c        # rumble pack emulation
    src/storage/sdcard.c        # SD mount, dirs, vmu banks, gameid
    src/storage/settings.c      # pico2maple_settings.bin
    src/config/mapping.c        # controller map parser + sample writer
    src/oled/oled.c             # SSD1306 status display
    src/usb/usb_host.c          # TinyUSB host glue (incl. xinput)
    src/bt/bp32_platform.c      # Bluepad32 uni_platform ("BP32 Pico2Maple Platform")
)

pico_generate_pio_header(pico2maple ${CMAKE_CURRENT_LIST_DIR}/src/maple.pio)

target_include_directories(pico2maple PRIVATE src)

target_link_libraries(pico2maple
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_pwm
    hardware_i2c
    hardware_flash
    tinyusb_host
    tinyusb_board
    pico_cyw43_arch_none        # CYW43 for BT only (btstack run loop)
    pico_btstack_classic
    pico_btstack_ble
    pico_btstack_cyw43
    # bluepad32 + tusb_xinput + no-OS-FatFS targets added once vendored
)

# Original build: UART stdio on GPIO0/1, Release
pico_enable_stdio_uart(pico2maple 1)
pico_enable_stdio_usb(pico2maple 0)

pico_add_extra_outputs(pico2maple)

// tusb_config.h — TinyUSB HOST mode for USB controllers/keyboard/mouse on the RP2350.
#ifndef P2M_TUSB_CONFIG_H
#define P2M_TUSB_CONFIG_H

#define CFG_TUSB_MCU          OPT_MCU_RP2040   // RP2350 uses the rp2040 port in pico-sdk TinyUSB
#define CFG_TUSB_OS           OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_HOST | OPT_MODE_DEFAULT_SPEED)

#define BOARD_TUH_RHPORT 0
#define BOARD_TUH_MAX_SPEED OPT_MODE_DEFAULT_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

// ---- HOST stack ----
#define CFG_TUH_ENABLED       1
#define CFG_TUH_RPI_PIO_USB   0           // native RP2350 USB, not PIO-USB

#define CFG_TUH_HUB           1           // allow a USB hub (multiple controllers)
#define CFG_TUH_DEVICE_MAX    (CFG_TUH_HUB ? 4 : 1)
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUH_HID           4           // up to 4 HID interfaces (pads/kbd/mouse)
#define CFG_TUH_HID_EPIN_BUFSIZE  64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#endif

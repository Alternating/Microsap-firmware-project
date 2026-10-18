// board.h — pin assignment + layout selection. Reconstructed from pico2maple flipped-bt
// board init (FUN_10008140). The pin config is a byte struct in .data (RAM 0x200029f4);
// each field below lists its RAM address. GPIO15 (pulled up) selects the layout at boot:
// HIGH/floating = standard, grounded = alternate ("flipped").
//
//                         STANDARD (GPIO15 high)   ALTERNATE (GPIO15 grounded)
//   Maple bus 1 (SDCKA)   GPIO16                   GPIO6      <- this build's default use
//   Maple bus 2 (SDCKB)   GPIO17                   GPIO7
//   UART console TX/RX    GPIO0 / GPIO1            GPIO0 / GPIO1
//   OLED I2C SDA / SCL    GPIO4 / GPIO5            GPIO4 / GPIO5
//   microSD SPI SCK       GPIO10                   GPIO10
//   microSD SPI TX/RX/CS  GPIO11 / GPIO12 / GPIO13 GPIO11 / GPIO12 / GPIO13
//   VMU buzzer PWM        GPIO3                    GPIO26
//   CYW43 gSPI DI/DO/IRQ  GPIO24                   GPIO2
//   CYW43 gSPI CS         GPIO25                   GPIO3
//   CYW43 gSPI SCLK       GPIO29                   GPIO29
//   CYW43 BT-ON           GPIO23                   GPIO14
//   layout select         GPIO15 (internal pull-up; ground to enable alternate)
#ifndef P2M_BOARD_H
#define P2M_BOARD_H

#include <stdint.h>
#include <stdbool.h>

// Pin configuration, populated by p2m_board_init() from the GPIO15 sense.
// Field order and RAM addresses mirror the original .data struct exactly.
typedef struct {
    uint8_t buzzer;      // @0x200029f4  (3 std / 26 alt)
    uint8_t cfg_a;       // @0x200029f5  (9)   -- CYW43 power/aux (p2m_bp32_init)
    uint8_t cfg_b;       // @0x200029f6  (8)
    uint8_t cyw_sclk;    // @0x200029f7  (29)
    uint8_t cyw_cs;      // @0x200029f8  (25 std / 3 alt)
    uint8_t cyw_di;      // @0x200029f9  (24 std / 2 alt)
    uint8_t cyw_bt_on;   // @0x200029fa  (23 std / 14 alt)
    uint8_t oled_scl;    // @0x200029fb  (5)
    uint8_t oled_sda;    // @0x200029fc  (4)
    uint8_t sd_cs;       // @0x200029fd  (13)
    uint8_t sd_rx;       // @0x200029fe  (12)
    uint8_t sd_tx;       // @0x200029ff  (11)
    uint8_t sd_sck;      // @0x20002a00  (10)
    uint8_t maple_pin;   // @0x20002a01  (16 std / 6 alt) -- base; Maple 2 = maple_pin + 1
    uint8_t misc_a;      // @0x20002a02  (0x40)
    uint8_t misc_b;      // @0x20002a03  (0x20)
    uint8_t misc_c;      // @0x20002a04  (0x10)
} p2m_pins_t;

extern p2m_pins_t g_pins;
extern bool g_layout_alternate;   // true when GPIO15 was grounded at boot

// from 0x10008140 — sense GPIO15 and populate g_pins; also start BTstack/cyw43 bring-up prep.
void p2m_board_init(void);

#endif

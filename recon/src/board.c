// board.c — pin assignment + layout selection. from 0x10008140 (FUN_10008140).
// The pico2maple flipped-bt build boots with the STANDARD pin table baked into .data, then
// reads GPIO15: if it is grounded the ALTERNATE ("flipped") layout is written over g_pins.
// This project's hardware grounds GPIO15, so the active layout is the alternate one.
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "board.h"

// Baked-in standard layout (mirrors the .data initializer at RAM 0x200029f4).
p2m_pins_t g_pins = {
    .buzzer = 3,  .cfg_a = 9,  .cfg_b = 8,
    .cyw_sclk = 29, .cyw_cs = 25, .cyw_di = 24, .cyw_bt_on = 23,
    .oled_scl = 5, .oled_sda = 4,
    .sd_cs = 13, .sd_rx = 12, .sd_tx = 11, .sd_sck = 10,
    .maple_pin = 16,
    .misc_a = 0x40, .misc_b = 0x20, .misc_c = 0x10,
};
bool g_layout_alternate;

// Aliases the maple engine / oled / sd modules read directly (same storage as g_pins fields).
// Declared extern there; defined here so the whole firmware shares one pin map.
uint8_t g_maple_pin;      // = g_pins.maple_pin
uint8_t g_oled_sda_pin;   // = g_pins.oled_sda
uint8_t g_oled_scl_pin;   // = g_pins.oled_scl

// from 0x10008140
void p2m_board_init(void)
{
    // Configure GPIO15 as a pulled-up input and let it settle.
    gpio_init(15);
    gpio_set_dir(15, GPIO_IN);
    gpio_set_pulls(15, true, false);
    sleep_ms(1);

    // Original tests bit 15 of SIO GPIO_IN via (gpio_in << 16) < 0 (sign of bit 31).
    // HIGH (pulled up, not grounded) -> standard; LOW (grounded) -> alternate.
    bool grounded = ((sio_hw->gpio_in >> 15) & 1) == 0;
    g_layout_alternate = grounded;

    if (grounded) {
        g_pins.maple_pin = 6;     // Maple 1 = GPIO6, Maple 2 = GPIO7
        g_pins.cyw_di    = 2;
        g_pins.cyw_cs    = 3;
        g_pins.cyw_sclk  = 29;
        g_pins.cyw_bt_on = 14;
        g_pins.oled_sda  = 4;
        g_pins.oled_scl  = 5;
        g_pins.sd_sck    = 10;
        g_pins.sd_tx     = 11;
        g_pins.sd_rx     = 12;
        g_pins.sd_cs     = 13;
        g_pins.buzzer    = 26;
        g_pins.cfg_a     = 9;     // *DAT_100082d4 = 9
        g_pins.cfg_b     = 8;     // *DAT_100082d0 = 8  // TODO(verify @0x10008140 roles of cfg_a/b)
    }

    g_maple_pin    = g_pins.maple_pin;
    g_oled_sda_pin = g_pins.oled_sda;
    g_oled_scl_pin = g_pins.oled_scl;

    // The remainder of FUN_10008140 seeds the cyw43/BTstack gpio descriptor from these pins
    // (see bp32 module). Not reproduced here — that belongs to bp32_platform.c.
}

// VMU buzzer (0x1000811c): drive a PWM tone on the buzzer pin. Called from vmu_set_condition
// with the SET_CONDITION bytes. TODO(verify @0x1000811c): exact freq/duty derivation.
#include "hardware/pwm.h"
void buzzer_set(uint8_t a, uint8_t b, int mode)
{
    (void)mode;
    uint pin = g_pins.buzzer;
    if (a == 0 && b == 0) {                    // stop
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_put(pin, 0);
        return;
    }
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(pin);
    // Puru Puru buzzer: byte pair encodes a tone. Map to a rough PWM frequency.
    uint32_t div = 1 + a;                      // coarse pitch from the first byte
    pwm_set_clkdiv(slice, (float)div);
    pwm_set_wrap(slice, 1000);
    pwm_set_gpio_level(pin, 500);              // ~50% duty
    pwm_set_enabled(slice, true);
}

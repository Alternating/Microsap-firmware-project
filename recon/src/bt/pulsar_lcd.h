// pulsar_lcd.h — slot 1's LCD frames, out to the screen of the VMU docked in a Pulsar.
// Core1 drops each of slot 1's frames into a one-slot mailbox, beside the OLED's copy, and the
// BLE context on core0 writes the newest to the Pulsar's LCD characteristic.
// Frames replace, never queue: games draw at 30-60 Hz and the link cannot.
#ifndef P2M_PULSAR_LCD_H
#define P2M_PULSAR_LCD_H

#include <stdbool.h>
#include <stdint.h>

void pulsar_lcd_init(void);                  // claims the mailbox lock; core1 init, before bt_init
// Core1: slot 1's frame, 48x32 1 bpp, exactly as the console wrote it (turned on the way out).
void pulsar_lcd_frame(const uint8_t fb[192]);

// BLE context only. The pull's progress, set by the link before each service: done of total
// blocks, total 0 when no pull runs. While a pull runs the Pulsar's screen is the loading screen
// (pulsar_screen.h), not the console's art, which is a card the player cannot use yet.
void pulsar_lcd_progress(unsigned done, unsigned total);
// BLE context only. Called on the link's tick: writes the frame in flight, or the newest from the
// mailbox; returns at the first busy write, to carry on at the next tick.
void pulsar_lcd_service(uint16_t con, uint16_t value_handle, uint16_t mtu);
// The link dropped: abandon the frame in flight and send the last frame again on the next link.
void pulsar_lcd_reset(void);

extern volatile uint32_t g_pulsar_lcd_in, g_pulsar_lcd_sent;

#endif

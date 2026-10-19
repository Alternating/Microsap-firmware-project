// pulsar_lcd.h — slot 1's LCD frames, out to the screen of the VMU docked in a Pulsar.
// A second display sink on their VMU instance (vmu.h): core1 drops each frame into a one-slot
// mailbox, and the BLE context on core0 writes the newest to the Pulsar's LCD characteristic.
// Frames replace, never queue: games draw at 30-60 Hz and the link cannot.
#ifndef P2M_PULSAR_LCD_H
#define P2M_PULSAR_LCD_H

#include <stdbool.h>
#include <stdint.h>
#include "maple/vmu.h"

void pulsar_lcd_init(void);                  // claims the mailbox lock; core1 init, before bt_init
extern const vmu_display_t g_vmu_display_pulsar;

// BLE context only. Called on the link's tick: writes the frame in flight, or the newest from the
// mailbox; returns at the first busy write, to carry on at the next tick.
void pulsar_lcd_service(uint16_t con, uint16_t value_handle, uint16_t mtu);
// The link dropped: abandon the frame in flight and send the last frame again on the next link.
void pulsar_lcd_reset(void);

extern volatile uint32_t g_pulsar_lcd_in, g_pulsar_lcd_sent;

#endif

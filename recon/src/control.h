// control.h — core0 integration layer: controller input -> hotkeys/mapping -> DC condition,
// plus DC rumble -> pad. Path B own-firmware glue. State here is read by the Maple engine (core1).
#ifndef P2M_CONTROL_H
#define P2M_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

// Active built-in layout: 2 all-buttons, 3 standard, 4 racing, 5 twin-stick, 6 fight-stick, 7 custom.
extern volatile int16_t g_active_layout;
// Set to the new layout id when the layout changes; core1 rebuilds the device-info then clears it.
extern volatile int16_t g_devinfo_rebuild;
extern volatile bool    g_rumble_enabled;
extern uint8_t          g_custom_map[0x91];

void control_init(void);       // load settings + custom map; set default layout
void controller_poll(void);    // once per core0 loop: input -> hotkeys/mapping -> DC condition
void rumble_tick(void);        // forward a pending DC rumble to the active pad
void slot2_tick(void);         // the first Pulsar takes slot 2; the slot-2 chord's beeps

// Puru Puru device hooks the Maple engine (core1) calls on DC vibration commands.
void rumble_start(void *a, void *b);
void rumble_set_block(void *a, void *b);
int  rumble_get_cond(void *a, void *b);
void rumble_stop(void);
void usb_announce(void *x);

#endif

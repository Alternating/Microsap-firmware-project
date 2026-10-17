// usb_host.h — TinyUSB host: USB controllers / keyboard / mouse -> normalized gamepad.
// Path B clean module (own firmware). Produces the shared p2m_gamepad_t that the custom
// mapping and Dreamcast layout mapper consume.
#ifndef P2M_USB_HOST_H
#define P2M_USB_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include "../config/mapping.h"   // p2m_gamepad_t

void usb_host_init(void);
void usb_host_task(void);                 // poll once per main-loop iteration (non-blocking)
bool usb_pad_connected(void);             // any HID device mounted

const p2m_gamepad_t *usb_get_gamepad(void);   // latest merged pad state, or NULL if none
void usb_get_keyboard(uint8_t out[8]);        // boot-keyboard report: mod,res,6 keycodes
void usb_get_mouse(int8_t *dx, int8_t *dy, uint8_t *buttons, int8_t *wheel); // deltas, cleared on read

// XInput arrives via a separate driver (Ryzee119/tusb_xinput); this hook normalizes its report.
void usb_xinput_report(const void *xid_report, p2m_gamepad_t *gp);   // TODO: wire tusb_xinput

#endif

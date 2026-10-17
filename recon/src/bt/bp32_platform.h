// bp32_platform.h — Bluepad32 platform glue: BT controllers -> normalized gamepad.
// Path B clean module. "BP32 Pico2Maple Platform" in the original; here a thin platform that
// fills the same p2m_gamepad_t the USB path produces, so downstream mapping is source-agnostic.
#ifndef P2M_BP32_PLATFORM_H
#define P2M_BP32_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include "../config/mapping.h"   // p2m_gamepad_t

// Register this platform with Bluepad32 before uni_init(). from p2m_bp32_* (0x100082dc..)
void bp32_platform_register(void);

// Latest BT pad state (NULL if no controller connected).
const p2m_gamepad_t *bp32_get_gamepad(void);
bool bp32_pad_connected(void);

// Forward a Dreamcast rumble request (strength 0..255, duration ms) to the connected BT pad.
void bp32_set_rumble(uint8_t strength, uint16_t duration_ms);

#endif

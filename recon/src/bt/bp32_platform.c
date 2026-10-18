// bp32_platform.c — Bluepad32 platform for pico2maple (Path B, own firmware).
// Converts Bluepad32's uni_controller_t into our normalized p2m_gamepad_t and forwards rumble.
// Built against cluoma's public Bluepad32 fork (Apache-2.0), which already carries his extra
// controller support. Mirrors the original platform callbacks (p2m_bp32_*, 0x100082dc..0x10008b68).
#include <string.h>
#include "uni.h"
#include "bp32_platform.h"

#ifndef CONFIG_BLUEPAD32_MAX_DEVICES
#define CONFIG_BLUEPAD32_MAX_DEVICES 4
#endif

static p2m_gamepad_t s_gamepad;
static bool          s_connected;
static uni_hid_device_t *s_active;      // the controller we forward rumble to

static void gp_neutral(p2m_gamepad_t *gp)
{
    memset(gp->g, 0, sizeof gp->g);
    gp->g[0x11] = gp->g[0x12] = gp->g[0x13] = gp->g[0x14] = 0x80;
}

// Bluepad32 sticks are signed ~[-512,511]; triggers [0,1023]. Scale to 0..255 (center 0x80).
static inline uint8_t axis_to_u8(int32_t v)
{
    v = (v + 512) >> 2;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}
static inline uint8_t trig_to_u8(int32_t v)
{
    v >>= 2;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void gamepad_to_p2m(const uni_gamepad_t *gp, p2m_gamepad_t *out)
{
    gp_neutral(out);
    out->g[0x11] = axis_to_u8(gp->axis_x);
    out->g[0x12] = axis_to_u8(gp->axis_y);
    out->g[0x13] = axis_to_u8(gp->axis_rx);
    out->g[0x14] = axis_to_u8(gp->axis_ry);
    out->g[0x0e] = trig_to_u8(gp->brake);     // L2 -> LT
    out->g[0x0d] = trig_to_u8(gp->throttle);  // R2 -> RT

    if (gp->dpad & DPAD_UP)    out->g[0x9] = 0xff;
    if (gp->dpad & DPAD_DOWN)  out->g[0xa] = 0xff;
    if (gp->dpad & DPAD_LEFT)  out->g[0xb] = 0xff;
    if (gp->dpad & DPAD_RIGHT) out->g[0xc] = 0xff;

    if (gp->buttons & BUTTON_A)          out->g[0x0]  = 0xff;
    if (gp->buttons & BUTTON_B)          out->g[0x1]  = 0xff;
    if (gp->buttons & BUTTON_X)          out->g[0x4]  = 0xff;
    if (gp->buttons & BUTTON_Y)          out->g[0x5]  = 0xff;
    if (gp->buttons & BUTTON_SHOULDER_L) out->g[0x10] = 0xff;  // LB
    if (gp->buttons & BUTTON_SHOULDER_R) out->g[0x0f] = 0xff;  // RB
    if (gp->buttons & BUTTON_TRIGGER_L)  out->g[0x0e] = 0xff;  // digital L2 fallback
    if (gp->buttons & BUTTON_TRIGGER_R)  out->g[0x0d] = 0xff;  // digital R2 fallback

    if (gp->misc_buttons & MISC_BUTTON_START)   out->g[0x07] = 0xff;
    if (gp->misc_buttons & MISC_BUTTON_SELECT)  out->g[P2M_MISC] |= P2M_MISC_SELECT;  // hotkey mod
    if (gp->misc_buttons & MISC_BUTTON_SYSTEM)  out->g[P2M_MISC] |= P2M_MISC_SYSTEM;
    if (gp->misc_buttons & MISC_BUTTON_CAPTURE) out->g[P2M_MISC] |= P2M_MISC_CAPTURE;
}

// --- uni_platform callbacks (match p2m_bp32_* in the original) ---
static void p2m_init(int argc, const char **argv) { (void)argc; (void)argv; gp_neutral(&s_gamepad); }

static void p2m_on_init_complete(void)
{
    uni_bt_enable_new_connections_unsafe(true);   // accept pairings
    // optional: uni_bt_del_keys_unsafe() to clear old bonds
}

static uni_error_t p2m_on_device_discovered(bd_addr_t addr, const char *name,
                                            uint16_t cod, uint8_t rssi)
{
    (void)addr; (void)name; (void)rssi;
    // Original p2m_bp32_on_device_discovered (0x100082e0): accept only gamepad/joystick CoD.
    const uint32_t MASK = 0x48;   // (cod & 0x48)==0 -> reject (UNI_ERROR_IGNORE_DEVICE=5)
    return (cod & MASK) ? UNI_ERROR_SUCCESS : UNI_ERROR_IGNORE_DEVICE;
}

static void p2m_on_device_connected(uni_hid_device_t *d) { s_active = d; }

static void p2m_on_device_disconnected(uni_hid_device_t *d)
{
    if (d == s_active) { s_active = NULL; s_connected = false; gp_neutral(&s_gamepad); }
}

static uni_error_t p2m_on_device_ready(uni_hid_device_t *d)
{
    s_active = d; s_connected = true;
    return UNI_ERROR_SUCCESS;
}

// latency telemetry: report count + timestamp of the freshest pad data (read by the heartbeat)
volatile uint32_t g_bp32_reports;
volatile uint64_t g_bp32_last_report_us;

static void p2m_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl)
{
    g_bp32_reports++;
    g_bp32_last_report_us = time_us_64();
    s_active = d;
    switch (ctl->klass) {
    case UNI_CONTROLLER_CLASS_GAMEPAD:
        gamepad_to_p2m(&ctl->gamepad, &s_gamepad);
        s_connected = true;
        break;
    case UNI_CONTROLLER_CLASS_KEYBOARD:
    case UNI_CONTROLLER_CLASS_MOUSE:
        // TODO: route BT keyboard/mouse into the same kbd/mouse paths as USB.
        break;
    default: break;
    }
}

static const uni_property_t *p2m_get_property(uni_property_idx_t idx) { (void)idx; return NULL; }

static void p2m_on_oob_event(uni_platform_oob_event_t event, void *data)
{
    (void)event; (void)data;   // system (PS/Home) button long-press etc. — unused here
}

static struct uni_platform s_platform = {
    .name                   = "BP32 Pico2Maple Platform",
    .init                   = p2m_init,
    .on_init_complete       = p2m_on_init_complete,
    .on_device_discovered   = p2m_on_device_discovered,
    .on_device_connected    = p2m_on_device_connected,
    .on_device_disconnected = p2m_on_device_disconnected,
    .on_device_ready        = p2m_on_device_ready,
    .on_controller_data     = p2m_on_controller_data,
    .get_property           = p2m_get_property,
    .on_oob_event           = p2m_on_oob_event,
};

void bp32_platform_register(void) { uni_platform_set_custom(&s_platform); }

const p2m_gamepad_t *bp32_get_gamepad(void) { return s_connected ? &s_gamepad : NULL; }
bool bp32_pad_connected(void) { return s_connected; }

void bp32_set_rumble(uint8_t strength, uint16_t duration_ms)
{
    if (s_active && s_active->report_parser.play_dual_rumble)
        s_active->report_parser.play_dual_rumble(s_active, 0 /*delay*/, duration_ms,
                                                  strength, strength);
}

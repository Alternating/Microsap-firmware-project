// control.c — core0 integration: USB/BT input -> hotkeys + custom map + layout mapper ->
// Dreamcast controller condition, and DC rumble -> active pad. Path B own-firmware glue.
#include <string.h>
#include "pico/stdlib.h"
#include "control.h"
#include "config/mapping.h"
#include "storage/storage.h"
#include "usb/usb_host.h"
#include "bt/bp32_platform.h"

// cross-module
extern void apply_custom_mapping(const uint8_t *mapping, p2m_gamepad_t *gp);   // 0x10004344
extern void dc_build_condition(const uint8_t *gp, int active_layout);          // 0x1000098c
extern void maple_request_bank_switch(int dir);                                // 0x100008f0
extern int  cfg_load_custom_mapping(uint8_t *mapping, const char *name, bool create_sample);

// --- shared state ---
volatile int16_t g_active_layout = 3;     // standard by default
volatile int16_t g_devinfo_rebuild;
volatile bool    g_rumble_enabled = true;
uint8_t          g_custom_map[0x91];
uint8_t          g_settings_storage_pref;  // 2 = prefer SD (read by vmu_init); from settings

// --- pending rumble (set by the Maple engine's Puru Puru hooks, drained by rumble_tick) ---
static uint8_t  s_rumble_strength;
static uint16_t s_rumble_duration;
static bool     s_rumble_pending;

static uint32_t s_prev_combo;             // hotkey edge detection

static void set_layout(int id)
{
    g_active_layout   = (int16_t)id;
    g_devinfo_rebuild = (int16_t)id;      // core1 rebuilds the controller device-info
}

void control_init(void)
{
    bool use_sd = (g_storage_status == 2);

    // Seed the 28 output rows, then overlay the SD cfg file if present (its lines fill inputs).
    mapping_defaults(g_custom_map);
    // A previously-compiled map may also live in the settings blob (field 2).
    settings_read_field(SETTINGS_FIELD_MAPPING, g_custom_map, use_sd);
    if (use_sd)
        cfg_load_custom_mapping(g_custom_map, "pico2maple_controller_map.cfg", true);

    g_active_layout   = 3;                 // standard; layout is not persisted
    g_devinfo_rebuild = g_active_layout;   // force an initial device-info build on core1
    g_rumble_enabled  = true;
}

void controller_poll(void)
{
    const p2m_gamepad_t *active = usb_get_gamepad();
    if (!active) active = xinput_get_gamepad();
    if (!active) active = bp32_get_gamepad();

    p2m_gamepad_t gp;
    if (active) memcpy(&gp, active, sizeof gp);
    else {      // no pad: neutral — sticks must be explicitly centered (0 would read full up-left)
        memset(&gp, 0, sizeof gp);
        gp.g[0x11] = gp.g[0x12] = gp.g[0x13] = gp.g[0x14] = 0x80;
    }

    bool select = active && (gp.g[P2M_MISC] & P2M_MISC_SELECT);

    // Build the bitmask of combo buttons currently pressed (only meaningful while Select is held).
    uint32_t combo = 0;
    if (select) {
        if (gp.g[0x00]) combo |= 1u << 0;   // A
        if (gp.g[0x01]) combo |= 1u << 1;   // B
        if (gp.g[0x04]) combo |= 1u << 2;   // X
        if (gp.g[0x05]) combo |= 1u << 3;   // Y
        if (gp.g[0x0c]) combo |= 1u << 4;   // DPAD_RIGHT
        if (gp.g[0x0b]) combo |= 1u << 5;   // DPAD_LEFT
        if (gp.g[0x0f]) combo |= 1u << 6;   // RB
        if (gp.g[0x10]) combo |= 1u << 7;   // LB
        if (gp.g[0x0a]) combo |= 1u << 8;   // DPAD_DOWN
    }
    uint32_t rising = combo & ~s_prev_combo;   // edge: pressed this frame, not last
    s_prev_combo = combo;

    if (rising & (1u << 0)) set_layout(3);     // Select+A -> standard
    if (rising & (1u << 1)) set_layout(2);     // Select+B -> all buttons
    if (rising & (1u << 2)) set_layout(5);     // Select+X -> twin stick
    if (rising & (1u << 3)) set_layout(6);     // Select+Y -> fight stick
    if (rising & (1u << 4)) set_layout(7);     // Select+DpadRight -> custom
    if (rising & (1u << 5)) set_layout(4);     // Select+DpadLeft -> racing
    if (rising & (1u << 6)) maple_request_bank_switch(+1);  // Select+RB
    if (rising & (1u << 7)) maple_request_bank_switch(-1);  // Select+LB
    if (rising & (1u << 8)) g_rumble_enabled = !g_rumble_enabled;  // Select+DpadDown

    if (select) {
        // Suppress gameplay input while the hotkey modifier is held: send neutral.
        uint8_t neutral[0x20] = {0};
        neutral[0x11] = neutral[0x12] = neutral[0x13] = neutral[0x14] = 0x80;  // centered sticks
        dc_build_condition(neutral, g_active_layout);
        return;
    }

    if (g_active_layout == 7)               // custom layout: run the user map first
        apply_custom_mapping(g_custom_map, &gp);
    dc_build_condition(gp.g, g_active_layout);
}

void rumble_tick(void)
{
    if (!s_rumble_pending) return;
    if (g_rumble_enabled) {
        if (xinput_pad_connected()) xinput_set_rumble(s_rumble_strength);
        else bp32_set_rumble(s_rumble_strength, s_rumble_duration);
    }
    s_rumble_pending = false;
}

// --- Maple engine Puru Puru hooks ---
void rumble_start(void *a, void *b)        // DC SET_CONDITION (vibration)
{
    (void)a; (void)b;
    s_rumble_strength = 0xff; s_rumble_duration = 1000; s_rumble_pending = true;
}
void rumble_set_block(void *a, void *b)    // DC BLOCK_WRITE to the rumble function
{
    (void)a; (void)b;   // TODO(verify): parse the Puru Puru waveform (freq/amp/duration) from the block
    s_rumble_strength = 0xff; s_rumble_duration = 250; s_rumble_pending = true;
}
int  rumble_get_cond(void *a, void *b) { (void)a; (void)b; return 0; }
void rumble_stop(void) { s_rumble_pending = false; bp32_set_rumble(0, 0); }
void usb_announce(void *x) { (void)x; }    // TODO: USB hotplug re-announce

// control.c — core0 integration: USB/BT input -> hotkeys + custom map + layout mapper ->
// Dreamcast controller condition, and DC rumble -> active pad. Path B own-firmware glue.
#include <string.h>
#include "pico/stdlib.h"
#include "control.h"
#include "config/mapping.h"
#include "storage/storage.h"
#include "usb/usb_host.h"
#include "bt/bp32_platform.h"
#include "maple/pulsar_card.h"
#include "maple/slot2.h"

// cross-module
extern void apply_custom_mapping(const uint8_t *mapping, p2m_gamepad_t *gp);   // 0x10004344
extern void dc_build_condition(const uint8_t *gp, int active_layout);          // 0x1000098c
extern void maple_request_bank_switch(int dir);                                // 0x100008f0
extern void maple_request_slot2(uint8_t choice);                               // ours
extern void buzzer_set(uint8_t a, uint8_t b, int mode);                        // 0x1000811c
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

// Slot 2's backing: SETTINGS_FIELD_SLOT2, kept on the SD card, the only time there is
// a choice to make. Core1 moves the slot; this keeps the choice and tells it.
static uint8_t  s_slot2_choice;
static uint8_t  s_prev_slot2_dpad;        // chord edge detection: bit0 Up, bit1 Down
static uint8_t  s_beeps;                  // half-beeps still to play: odd on, even off
static uint64_t s_beep_at;

static void set_layout(int id)
{
    g_active_layout   = (int16_t)id;
    g_devinfo_rebuild = (int16_t)id;      // core1 rebuilds the controller device-info
}

// Without an SD card there is nothing to choose between: slot 2 is the Pulsar's.
// Core1 hears first; the settings write, an SD read and write, only stalls core0.
static void slot2_choose(uint8_t choice)
{
    if (g_storage_status != 2 || choice == s_slot2_choice) return;
    s_slot2_choice = choice;
    maple_request_slot2(choice);
    settings_update_field(SETTINGS_FIELD_SLOT2, &choice, true);
}

// The chord's feedback, played from the core0 loop: one beep for SD, two for the Pulsar.
static void beep(unsigned n)
{
    s_beeps = (uint8_t)(n * 2);
    s_beep_at = 0;
}

static void beep_tick(void)
{
    if (!s_beeps || time_us_64() < s_beep_at) return;
    s_beeps--;
    if (s_beeps & 1) buzzer_set(0x40, 1, 0);   // ~2.3 kHz (board.c's PWM divider)
    else             buzzer_set(0, 0, 0);
    s_beep_at = time_us_64() + 80000;
}

void control_init(void)
{
    bool use_sd = (g_storage_status == 2);

    // Seed the 28 output rows, then overlay the SD cfg file if present (its lines fill inputs).
    mapping_defaults(g_custom_map);
    // A previously-compiled map may also live in the settings blob (field 2).
    settings_read_field(SETTINGS_FIELD_MAPPING, g_custom_map, use_sd);
    if (use_sd)
        cfg_load_custom_mapping(g_custom_map, "microsap_controller_map.cfg", true);

    g_active_layout   = 3;                 // standard; layout is not persisted
    g_devinfo_rebuild = g_active_layout;   // force an initial device-info build on core1
    g_rumble_enabled  = true;

    // Until this runs core1 holds slot 2 on SD, as for "no Pulsar seen yet".
    uint8_t choice = SLOT2_CHOICE_NONE;
    if (use_sd && (!settings_read_field(SETTINGS_FIELD_SLOT2, &choice, true) ||
                   choice > SLOT2_CHOICE_PULSAR))
        choice = SLOT2_CHOICE_NONE;
    s_slot2_choice = choice;
    maple_request_slot2(choice);
}

// Once per core0 loop: the first Pulsar to connect takes slot 2, once, and the
// chord's beeps play out.
void slot2_tick(void)
{
    if (s_slot2_choice == SLOT2_CHOICE_NONE && pulsar_card_linked())
        slot2_choose(slot2_first_pulsar(s_slot2_choice));
    beep_tick();
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

    // L+R+Up/Down: both analog triggers down, then a D-pad edge, steps slot 2 to the
    // next or the previous backing. The game sees the press too: nothing is suppressed.
    uint8_t dpad = 0;
    if (active && gp.g[0x0e] >= 0xC0 && gp.g[0x0d] >= 0xC0)
        dpad = (gp.g[0x09] ? 1u : 0u) | (gp.g[0x0a] ? 2u : 0u);
    uint8_t pressed = dpad & ~s_prev_slot2_dpad;
    s_prev_slot2_dpad = dpad;
    if (pressed && g_storage_status == 2) {
        slot2_choose(slot2_cycle(s_slot2_choice, pressed & 1));
        beep(s_slot2_choice == SLOT2_CHOICE_PULSAR ? 2 : 1);
    }

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

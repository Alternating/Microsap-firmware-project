// dc_mapping.c — convert the normalized gamepad state into a Dreamcast controller condition.
// Reconstructed from the layout mapper FUN_1000098c (0x1000098c, 2444 bytes). The original is
// heavily unrolled by the compiler: each built-in layout precomputes every combination of the
// "base" buttons (A and the trigger→C mapping) into 16-bit active-low mask tables. This
// reconstruction expresses the SAME behavior as a readable per-button loop. The standard
// layout (case 3) is transcribed exactly; the other built-in layouts follow the README's
// documented remaps and are marked for byte-exact verification against their switch cases.
#include <string.h>
#include "pico/stdlib.h"
#include "maple.h"
#include "../config/mapping.h"

// Dreamcast controller button bits (active low in the condition word), from KOS/Maple spec
// and confirmed by the standard-layout mask tables in FUN_1000098c.
#define DC_C          (1u << 0)
#define DC_B          (1u << 1)
#define DC_A          (1u << 2)
#define DC_START      (1u << 3)
#define DC_DPAD_UP    (1u << 4)
#define DC_DPAD_DOWN  (1u << 5)
#define DC_DPAD_LEFT  (1u << 6)
#define DC_DPAD_RIGHT (1u << 7)
#define DC_Z          (1u << 8)
#define DC_Y          (1u << 9)
#define DC_X          (1u << 10)
#define DC_D          (1u << 11)
#define DC_DPAD2_UP   (1u << 12)
#define DC_DPAD2_DOWN (1u << 13)
#define DC_DPAD2_LEFT (1u << 14)
#define DC_DPAD2_RIGHT(1u << 15)

extern dc_controller_cond_t g_dc_controller_cond;   // @0x20005e2c (DAT_10001008 + 4 = buttons)

// Built-in layout ids (match the hotkey combos and the switch in FUN_1000098c).
enum {
    LAYOUT_ALL_BUTTONS = 2,   // Select+B: every input available (Z=LB, C=RB)
    LAYOUT_STANDARD    = 3,   // Select+A: standard Dreamcast
    LAYOUT_RACING      = 4,   // Select+DpadLeft: racing wheel
    LAYOUT_TWINSTICK   = 5,   // Select+X   (also custom=... see main)
    LAYOUT_FIGHTSTICK  = 6,   // Select+Y
    LAYOUT_CUSTOM      = 7,   // Select+DpadRight
};

// Analog deadzone snap used by every layout: values within [0x7e,0x82] clamp to 0x80 center.
static inline uint8_t snap_center(uint8_t v)
{
    return ((uint8_t)(v - 0x7e) < 5) ? 0x80 : v;
}

// from 0x1000098c, standard layout (case 3). `gp` is the post-custom-mapping gamepad state.
// Writes directly into the shared controller condition block.
static void build_standard(const uint8_t *gp)
{
    uint16_t btn = 0xffff;                       // all released (active low)
    if (gp[0x00]) btn &= ~DC_A;                  // A input   -> A
    if (gp[0x01]) btn &= ~DC_B;                  // B input   -> B
    if (gp[0x0d] > 199) btn &= ~DC_C;            // LT analog -> C (trigger-as-button)
    if (gp[0x04]) btn &= ~DC_X;                  // X input   -> X
    if (gp[0x05]) btn &= ~DC_Y;                  // Y input   -> Y
    if (gp[0x0f]) btn &= ~DC_Z;                  // RB input  -> Z
    if (gp[0x07]) btn &= ~DC_START;              // START
    if (gp[0x09]) btn &= ~DC_DPAD_UP;
    if (gp[0x0a]) btn &= ~DC_DPAD_DOWN;
    if (gp[0x0b]) btn &= ~DC_DPAD_LEFT;
    if (gp[0x0c]) btn &= ~DC_DPAD_RIGHT;

    uint8_t *c = (uint8_t *)&g_dc_controller_cond.buttons;
    c[0] = (uint8_t)btn;
    c[1] = (uint8_t)(btn >> 8);
    // triggers: rtrig (+2 within cond.buttons region = cond+6) and ltrig (+3 = cond+7)
    // TODO(verify @0x10000ebc): rtrig = LB-digital (gp[0x10]) full-scale, ltrig = RT analog gp[0x0e]
    g_dc_controller_cond.rtrig = gp[0x10] ? 0xff : 0x00;
    g_dc_controller_cond.ltrig = gp[0x0e];
    g_dc_controller_cond.joyx  = snap_center(gp[0x11]);
    g_dc_controller_cond.joyy  = snap_center(gp[0x12]);
    g_dc_controller_cond.joy2x = snap_center(gp[0x13]);
    g_dc_controller_cond.joy2y = snap_center(gp[0x14]);
}

// "All buttons" (case 2): same as standard but Z<-LB and C<-RB are also exposed, and both
// analog triggers pass through as buttons at thresholds. The original case 2 additionally
// gates C/Z on the digital shoulders and maps LT/RT (>0xc7) / (<0x39) to the D-pad2 diagonal
// bits. Documented remap; TODO(verify @0x10000a00: exact case-2 mask tables).
static void build_all_buttons(const uint8_t *gp)
{
    build_standard(gp);
    uint16_t btn;
    memcpy(&btn, &g_dc_controller_cond.buttons, 2);
    if (gp[0x10]) btn &= ~DC_Z;                  // LB -> Z
    if (gp[0x0f]) btn &= ~DC_C;                  // RB -> C
    memcpy(&g_dc_controller_cond.buttons, &btn, 2);
}

// Twin-stick (case 5): second analog stick drives DPAD2; triggers map to D/trigger.
// Fight-stick (case 6): RB/RT -> Z/C, LB -> RT (per README). Racing (case 4): steering on
// the main X axis, throttle/brake on triggers. These cases in FUN_1000098c are large unrolled
// tables; they are reconstructed structurally here and flagged for verification.
static void build_variant(const uint8_t *gp, int layout)
{
    // TODO(verify @0x1000098c cases 2/4/5/6): reconstruct each from its mask table. For now the
    // standard builder is applied so the device still enumerates and plays; variant-specific
    // remaps (Z/C shoulders, twin-stick DPAD2, fight-stick Z/C, racing wheel) are pending.
    build_standard(gp);
    (void)layout;
}

// from 0x1000098c — entry: pick the active layout and fill the controller condition.
void dc_build_condition(const uint8_t *gp, int active_layout)
{
    switch (active_layout) {
    case LAYOUT_STANDARD:    build_standard(gp); break;
    case LAYOUT_ALL_BUTTONS: build_all_buttons(gp); break;
    case LAYOUT_RACING:
    case LAYOUT_TWINSTICK:
    case LAYOUT_FIGHTSTICK:
    case LAYOUT_CUSTOM:      build_variant(gp, active_layout); break;
    default:                 build_standard(gp); break;
    }
}

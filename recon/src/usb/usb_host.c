// usb_host.c — TinyUSB host HID: USB pads / keyboard / mouse -> p2m_gamepad_t.
// Path B clean module (own firmware), built on the public TinyUSB host API only.
// Per-controller report offsets are marked TODO(verify) — they vary by mode and need a quick
// confirm against each real controller; the generic fallback handles standard HID gamepads.
#include <string.h>
#include "tusb.h"
#include "pico/stdlib.h"
#include "usb_host.h"

#define MAX_INSTANCES 8

typedef enum { DEV_NONE = 0, DEV_KEYBOARD, DEV_MOUSE, DEV_GAMEPAD } dev_kind_t;

typedef struct {
    uint8_t    dev_addr, instance;
    dev_kind_t kind;
    uint16_t   vid, pid;
    bool       used;
} dev_slot_t;

static dev_slot_t s_devs[MAX_INSTANCES];
static bool       s_has_gamepad;
static uint8_t    s_kb[8];
static struct { int16_t dx, dy, wheel; uint8_t buttons; } s_mouse;
static p2m_gamepad_t s_gamepad;

// Reset a gamepad struct to the neutral state (buttons released, sticks centered).
static void gp_neutral(p2m_gamepad_t *gp)
{
    memset(gp->g, 0, sizeof gp->g);
    gp->g[0x11] = gp->g[0x12] = gp->g[0x13] = gp->g[0x14] = 0x80;
}

static int slot_find(uint8_t dev_addr, uint8_t instance)
{
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (s_devs[i].used && s_devs[i].dev_addr == dev_addr && s_devs[i].instance == instance)
            return i;
    return -1;
}

static int slot_alloc(uint8_t dev_addr, uint8_t instance)
{
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (!s_devs[i].used) {
            s_devs[i] = (dev_slot_t){ .dev_addr = dev_addr, .instance = instance, .used = true };
            return i;
        }
    return -1;
}

// 8-direction HID hat (0=N,1=NE,..7=NW, >=8 center) -> the four DPAD bytes.
static void apply_hat(p2m_gamepad_t *gp, uint8_t hat)
{
    static const uint8_t dirs[8] = {
        /*N */ 0x1, /*NE*/ 0x1|0x8, /*E */ 0x8, /*SE*/ 0x8|0x2,
        /*S */ 0x2, /*SW*/ 0x2|0x4, /*W */ 0x4, /*NW*/ 0x4|0x1,
    };  // bit0=UP(g9) bit1=DOWN(ga) bit2=LEFT(gb) bit3=RIGHT(gc)
    if (hat >= 8) return;
    uint8_t m = dirs[hat];
    if (m & 0x1) gp->g[0x9] = 0xff;
    if (m & 0x2) gp->g[0xa] = 0xff;
    if (m & 0x4) gp->g[0xb] = 0xff;
    if (m & 0x8) gp->g[0xc] = 0xff;
}

// --- per-controller parsers (fill gp from a HID report) ---
static void parse_gamepad(uint16_t vid, uint16_t pid, const uint8_t *r, uint16_t len,
                          p2m_gamepad_t *gp)
{
    gp_neutral(gp);

    // Sony DualShock 4 / DualSense
    if (vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC || pid == 0x0CE6)) {
        // TODO(verify): DS4/DualSense report byte offsets (USB report 0x01)
        if (len < 10) return;
        gp->g[0x11] = r[1];                 // LS X (0x80 center)
        gp->g[0x12] = r[2];                 // LS Y  // TODO(verify): Dreamcast Y polarity
        gp->g[0x13] = r[3];                 // RS X
        gp->g[0x14] = r[4];                 // RS Y
        apply_hat(gp, r[5] & 0x0F);         // low nibble = dpad hat
        uint8_t face = r[5] >> 4;           // high nibble: 4=square,5=cross,6=circle,7=triangle
        if (face & 0x2) gp->g[0x0] = 0xff;  // cross   -> A
        if (face & 0x4) gp->g[0x1] = 0xff;  // circle  -> B
        if (face & 0x1) gp->g[0x4] = 0xff;  // square  -> X
        if (face & 0x8) gp->g[0x5] = 0xff;  // triangle-> Y
        uint8_t b2 = r[6];
        if (b2 & 0x01) gp->g[0x10] = 0xff;  // L1 -> LB
        if (b2 & 0x02) gp->g[0x0f] = 0xff;  // R1 -> RB
        if (b2 & 0x04) gp->g[0x0e] = r[8];  // L2 analog -> LT
        if (b2 & 0x08) gp->g[0x0d] = r[9];  // R2 analog -> RT
        if (b2 & 0x20) gp->g[0x07] = 0xff;  // Options -> START
        if (b2 & 0x10) gp->g[P2M_MISC] |= P2M_MISC_SELECT;  // Share -> Select (hotkey modifier)
        if (len > 7 && (r[7] & 0x01)) gp->g[P2M_MISC] |= P2M_MISC_SYSTEM;  // PS button
        return;
    }

    // Nintendo Switch Pro
    if (vid == 0x057E && pid == 0x2009) {
        // TODO(verify): Switch Pro report byte offsets (standard USB report)
        if (len < 8) return;
        gp->g[0x11] = r[1]; gp->g[0x12] = r[2];
        gp->g[0x13] = r[3]; gp->g[0x14] = r[4];
        uint8_t dpad = r[5] & 0x0F;         // bit0 up,1 down,2 left,3 right
        if (dpad & 0x1) gp->g[0x9] = 0xff;
        if (dpad & 0x2) gp->g[0xa] = 0xff;
        if (dpad & 0x4) gp->g[0xb] = 0xff;
        if (dpad & 0x8) gp->g[0xc] = 0xff;
        uint8_t sh = r[5] >> 4;
        if (sh & 0x1) gp->g[0x10] = 0xff;   // L  -> LB
        if (sh & 0x2) gp->g[0x0f] = 0xff;   // R  -> RB
        uint8_t face = r[6];
        if (face & 0x01) gp->g[0x0] = 0xff; // A
        if (face & 0x02) gp->g[0x1] = 0xff; // B
        if (face & 0x10) gp->g[0x4] = 0xff; // X
        if (face & 0x20) gp->g[0x5] = 0xff; // Y
        if (face & 0x40) gp->g[0x07] = 0xff;// + -> START
        if (face & 0x80) gp->g[P2M_MISC] |= P2M_MISC_SELECT;  // - -> Select
        return;
    }

    // Generic HID gamepad fallback
    // TODO(verify): assumes [0..3]=two sticks, [4]=hat, [5]=button bitfield.
    if (len < 6) return;
    gp->g[0x11] = r[0]; gp->g[0x12] = r[1];
    gp->g[0x13] = r[2]; gp->g[0x14] = r[3];
    apply_hat(gp, r[4]);
    uint8_t b = r[5];
    if (b & 0x01) gp->g[0x0]  = 0xff;  // A
    if (b & 0x02) gp->g[0x1]  = 0xff;  // B
    if (b & 0x04) gp->g[0x4]  = 0xff;  // X
    if (b & 0x08) gp->g[0x5]  = 0xff;  // Y
    if (b & 0x10) gp->g[0x10] = 0xff;  // LB
    if (b & 0x20) gp->g[0x0f] = 0xff;  // RB
    if (b & 0x40) gp->g[0x07] = 0xff;  // START
    if (b & 0x80) gp->g[P2M_MISC] |= P2M_MISC_SELECT;  // Select
}

// XInput normalization hook (driver provides the raw XID report elsewhere).
void usb_xinput_report(const void *xid_report, p2m_gamepad_t *gp)
{
    (void)xid_report; (void)gp;
    // TODO: wire Ryzee119/tusb_xinput: map wButtons, bLeftTrigger/bRightTrigger, thumb axes.
}

// --- TinyUSB host HID callbacks ---
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      const uint8_t *desc_report, uint16_t desc_len)
{
    (void)desc_report; (void)desc_len;
    int i = slot_find(dev_addr, instance);
    if (i < 0) i = slot_alloc(dev_addr, instance);
    if (i < 0) return;
    tuh_vid_pid_get(dev_addr, &s_devs[i].vid, &s_devs[i].pid);

    switch (tuh_hid_interface_protocol(dev_addr, instance)) {
    case HID_ITF_PROTOCOL_KEYBOARD: s_devs[i].kind = DEV_KEYBOARD; break;
    case HID_ITF_PROTOCOL_MOUSE:    s_devs[i].kind = DEV_MOUSE;    break;
    default:                        s_devs[i].kind = DEV_GAMEPAD; s_has_gamepad = true; break;
    }
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    int i = slot_find(dev_addr, instance);
    if (i < 0) return;
    if (s_devs[i].kind == DEV_GAMEPAD) gp_neutral(&s_gamepad);
    s_devs[i] = (dev_slot_t){0};
    s_has_gamepad = false;
    for (int j = 0; j < MAX_INSTANCES; j++)
        if (s_devs[j].used && s_devs[j].kind == DEV_GAMEPAD) s_has_gamepad = true;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                const uint8_t *report, uint16_t len)
{
    int i = slot_find(dev_addr, instance);
    if (i >= 0) {
        switch (s_devs[i].kind) {
        case DEV_KEYBOARD:
            memset(s_kb, 0, sizeof s_kb);
            memcpy(s_kb, report, len < 8 ? len : 8);
            break;
        case DEV_MOUSE:
            if (len >= 4) {
                s_mouse.buttons = report[0];
                s_mouse.dx    += (int8_t)report[1];
                s_mouse.dy    += (int8_t)report[2];
                s_mouse.wheel += (int8_t)report[3];
            }
            break;
        case DEV_GAMEPAD:
            parse_gamepad(s_devs[i].vid, s_devs[i].pid, report, len, &s_gamepad);
            break;
        default: break;
        }
    }
    tuh_hid_receive_report(dev_addr, instance);   // always re-arm
}

// --- public API ---
void usb_host_init(void)
{
    tuh_init(BOARD_TUH_RHPORT);
    memset(s_devs, 0, sizeof s_devs);
    memset(s_kb, 0, sizeof s_kb);
    memset(&s_mouse, 0, sizeof s_mouse);
    s_has_gamepad = false;
    gp_neutral(&s_gamepad);
}

void usb_host_task(void) { tuh_task(); }

bool usb_pad_connected(void)
{
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (s_devs[i].used) return true;
    return false;
}

const p2m_gamepad_t *usb_get_gamepad(void) { return s_has_gamepad ? &s_gamepad : NULL; }

void usb_get_keyboard(uint8_t out[8]) { memcpy(out, s_kb, 8); }

void usb_get_mouse(int8_t *dx, int8_t *dy, uint8_t *buttons, int8_t *wheel)
{
    if (dx)      *dx = (int8_t)s_mouse.dx;
    if (dy)      *dy = (int8_t)s_mouse.dy;
    if (buttons) *buttons = s_mouse.buttons;
    if (wheel)   *wheel = (int8_t)s_mouse.wheel;
    s_mouse.dx = s_mouse.dy = s_mouse.wheel = 0;
}

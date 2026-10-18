// maple_devices.c — Dreamcast device-info block builder + device-mode switch.
// from 0x1000066c (maple_build_device_info_blocks), 0x10001494 / 0x1000151c (device mode).
#include <string.h>
#include <stdint.h>
#include "maple.h"

extern volatile int16_t g_active_layout;   // selects the controller subtype for device info

// {header word, body pointer} pair the TX path hands out for DEVICE_STATUS replies.
typedef struct { uint32_t header; uint8_t *body; } devinfo_ref_t;

static const char dc_name_controller[30] = "Dreamcast Controller          ";
static const char dc_name_twinstick[30]  = "Twin Stick                    ";
static const char dc_name_racing[30]     = "Racing Controller             ";
static const char dc_name_keyboard[30]   = "Keyboard                      ";
static const char dc_name_mouse[30]      = "Dreamcast Mouse               ";
static const char dc_license[60] =
    "Produced By or Under License From SEGA ENTERPRISES,LTD.     ";

// from 0x1000066c — fill the 112-byte Maple device-info `body` for `func`, and point `ref`
// at it. Capability words below are the binary's verbatim literals (DAT_10000804/0x820 + the
// case-2/4 byte stores); the TX path reverses payload word bytes, so on the console they read
// as 0x003fffff (std/twin-stick), 0x00fe0007... (racing), 0x000f06fe (canonical controller).
void maple_build_device_info_blocks(void *ref_, void *body_, int func)
{
    devinfo_ref_t *ref = (devinfo_ref_t *)ref_;
    uint8_t *b = (uint8_t *)body_;
    memset(b, 0, 112);
    const char *name = dc_name_controller;
    uint32_t fdata0 = 0, fdata1 = 0, fdata2 = 0;
    uint32_t funccode = 0;

    if (func == MAPLE_FUNC_CONTROLLER) {
        funccode = 0x00000001;
        switch (g_active_layout) {
        // Layout 3 now reports the canonical real-pad capability word (console reads
        // 0x000f06fe). The original used the expanded 0xffff3f00 here, but Atomiswave
        // conversions (e.g. Dolphin Blue) parse the capability mask and misread Start
        // against the expanded claim; a genuine Sega pad reports the canonical word.
        case 1: case 3: fdata0 = 0xfe060f00; name = dc_name_controller; break;
        case 2:         fdata0 = 0xffff3f00; name = dc_name_twinstick;  break;
        case 4:         fdata0 = 0xfe000700; name = dc_name_racing;     break;
        default:        fdata0 = 0xfe060f00; name = dc_name_controller; break;
        }
    } else if (func == MAPLE_FUNC_KEYBOARD) {
        funccode = 0x00000040;
        fdata0 = 0x80000502;                  // keyboard (bytes 02 05 00 80, verbatim)
        name = dc_name_keyboard;
    } else if (func == MAPLE_FUNC_MOUSE) {
        funccode = 0x00000200;
        fdata0 = 0x00070e00;                  // mouse (bytes 00 0e 07 00, verbatim)
        name = dc_name_mouse;
    }

    // function codes (big-endian on the wire → store byte-reversed)
    b[0] = (uint8_t)(funccode >> 24); b[1] = (uint8_t)(funccode >> 16);
    b[2] = (uint8_t)(funccode >> 8);  b[3] = (uint8_t)funccode;
    memcpy(b + 4,  &fdata0, 4);
    memcpy(b + 8,  &fdata1, 4);
    memcpy(b + 12, &fdata2, 4);
    b[0x10] = 0xff;                            // area code
    b[0x11] = 0x00;                            // connector direction
    memcpy(b + 0x12, name, 30);
    memcpy(b + 0x30, dc_license, 60);
    uint16_t standby = 0x01ae, maxp = 0x01f4;  // ~43mA standby / ~50mA max
    memcpy(b + 0x6c, &standby, 2);
    memcpy(b + 0x6e, &maxp, 2);

    if (ref) { ref->header = 0; ref->body = b; }
}

// Device-mode switch (keyboard/mouse/controller announce). The original toggles which main
// device function is presented to the Dreamcast. from 0x10001494 / 0x1000151c.
static int s_device_mode = MAPLE_FUNC_CONTROLLER;

void dc_set_device_mode(int func, int sub)    // 0x10001494
{
    (void)sub;
    s_device_mode = func;
    // TODO(verify @0x10001494): rebuild device-info + flip the condition block the engine sends
}

void dc_reset_device_mode(void)               // 0x1000151c
{
    s_device_mode = MAPLE_FUNC_CONTROLLER;
    // TODO(verify @0x1000151c): restore standard controller presentation
}

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

// ---- Sub-device table -------------------------------------------------------------
// One entry per slot that can ever answer. The engine resolves a frame's recipient AP against
// it (maple.c); presence is snapshotted once per frame, so a handler never sees a slot appear
// or vanish mid-transaction.
#include "maple_subdev.h"
#include "maple_frames.h"
#include "vmu.h"
#include "control.h"     // rumble_start / rumble_set_block (drained on core0 by rumble_tick)

extern uint8_t g_subperiph_mask;

// Their VMU, slot 1: store{internal flash, SD} + display{OLED} (vmu_backend.c), wired in
// maple_subdevs_init. No initializer here: one would move the whole 132 KB state from .bss
// into .data, a flash copy of it made at every boot.
static vmu_t s_vmu;
static bool s_vmu_ready;

// Advertised again. When b09fd43 turned it off, the console looped re-enumerating the port.
// The device info was already right (it matches the bridge's byte for byte). The state was
// not: its buffer was 894 bytes short of vmu_state_t, so the block-read reply, the dirty mask
// and the bank lived past it, over whatever .bss came next. That is the likeliest cause.
static bool vmu_present(void *ctx) { (void)ctx; return s_vmu_ready; }

void maple_subdevs_init(void)
{
    s_vmu.store = &g_vmu_store_flash_sd;
    s_vmu.sinks[0] = &g_vmu_display_oled;
    vmu_init(&s_vmu);
    vmu_load_bank(&s_vmu, 0, false);   // core1 loads the default bank; control_init re-picks later
    s_vmu_ready = true;
}

void maple_subdev_vmu_bank_cycle(bool forward)
{
    vmu_bank_cycle(&s_vmu, forward);
}

// Their Puru Puru, slot 2. It has no device info yet (the original's lives at 0x100a2994 and
// was never reconstructed), so it is never present and the console never addresses it. The
// arms are kept so that giving it device info is the only step left; the ones that sent an
// empty frame now send nothing.
static bool rumble_present(void *ctx) { (void)ctx; return false; }

static int rumble_handle(void *ctx, const maple_req_t *rq, maple_frame_t *out)
{
    (void)ctx;
    if (rq->func != MAPLE_FUNC_VIBRATION) return 0;
    switch (rq->cmd) {
    case MAPLE_CMD_BLOCK_WRITE:   rumble_set_block(0, 0); break;
    case MAPLE_CMD_SET_CONDITION: rumble_start(0, 0);     break;
    default: return 0;   // DEVICE_REQUEST, GET_MEDIA_INFO, BLOCK_READ: no reply content yet
    }
    out->header = MAPLE_HDR_ACK_SUB;
    out->payload = 0;
    return MAPLE_SUBDEV_REPLY;
}

const maple_subdev_t g_maple_subdevs[] = {
    { .slot_bit = P2M_SUBPERIPH_VMU, .func = MAPLE_FUNC_STORAGE | MAPLE_FUNC_LCD | MAPLE_FUNC_CLOCK,
      .devinfo = (const uint8_t *)&g_vmu_identity, .ctx = &s_vmu, .present = vmu_present, .handle = vmu_subdev_handle,
      .after = vmu_subdev_after, .tick = vmu_subdev_tick },
    { .slot_bit = P2M_SUBPERIPH_RUMBLE, .func = MAPLE_FUNC_VIBRATION,
      .devinfo = NULL, .ctx = NULL, .present = rumble_present, .handle = rumble_handle },
};
const unsigned g_maple_subdev_count = sizeof g_maple_subdevs / sizeof g_maple_subdevs[0];

uint8_t maple_subdev_snapshot(void)
{
    uint8_t m = 0;
    for (unsigned i = 0; i < g_maple_subdev_count; i++)
        if (g_maple_subdevs[i].present(g_maple_subdevs[i].ctx)) m |= g_maple_subdevs[i].slot_bit;
    g_subperiph_mask = m;
    return m;
}

const maple_subdev_t *maple_subdev_find(uint8_t dst)
{
    uint8_t bit = dst & 0x1f;
    if (!bit || (bit & (bit - 1))) return NULL;   // one recipient: exactly one slot bit
    if (!(bit & g_subperiph_mask)) return NULL;
    for (unsigned i = 0; i < g_maple_subdev_count; i++)
        if (g_maple_subdevs[i].slot_bit == bit) return &g_maple_subdevs[i];
    return NULL;
}

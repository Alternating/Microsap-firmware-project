// maple_subdev.h — the Maple sub-devices behind the main controller, as one table.
// A frame's recipient AP names one device: bit 5 is the main device, bits 4:0 are the five
// sub-device slots. The engine resolves the slot bit against this table and calls the entry's
// handler; a new card is a table entry, not another boolean in the dispatch.
#ifndef P2M_MAPLE_SUBDEV_H
#define P2M_MAPLE_SUBDEV_H

#include <stdbool.h>
#include <stdint.h>
#include "maple.h"

// One received frame, as the handler sees it.
typedef struct {
    const uint8_t *frame;   // [len][src][dst][cmd] + len words of payload (host order)
    uint8_t len, dst, cmd;  // len in payload words
    uint32_t func;          // first payload word, host order (compare with MAPLE_FUNC_*)
} maple_req_t;

// Handler result flags.
#define MAPLE_SUBDEV_REPLY      0x01  // send *out; the engine sets its source AP to the slot
#define MAPLE_SUBDEV_IDLE_RESET 0x02  // restart the idle-frame count that paces vmu flushes
#define MAPLE_SUBDEV_AFTER      0x04  // call after() once the reply is on the wire

typedef struct {
    uint8_t slot_bit;              // 0x01..0x10: the recipient AP bit this device answers on
    uint32_t func;                 // MAPLE_FUNC_* it implements, OR'd (informational)
    const uint8_t *devinfo;        // 112-byte DEVICE_STATUS body; NULL = not advertisable
    void *ctx;
    // No I/O, never blocks: snapshotted once per frame into g_subperiph_mask.
    bool (*present)(void *ctx);
    // Called on core1 inside the reply budget. Returns MAPLE_SUBDEV_* flags.
    int  (*handle)(void *ctx, const maple_req_t *rq, maple_frame_t *out);
    void (*after)(void *ctx);      // may be NULL if the handler never returns _AFTER
    // Called on every GET_CONDITION with the idle-frame count; may be NULL.
    void (*tick)(void *ctx, uint32_t idle_frames);
} maple_subdev_t;

extern const maple_subdev_t g_maple_subdevs[];
extern const unsigned g_maple_subdev_count;

// Recompute g_subperiph_mask from every entry's present(). Once per frame, before dispatch.
uint8_t maple_subdev_snapshot(void);
// The present entry for a recipient AP's sub-device bits, or NULL.
const maple_subdev_t *maple_subdev_find(uint8_t dst);

// Core1 init for the sub-devices (their VMU); call before the protocol loop.
void maple_subdevs_init(void);
// Their VMU's bank cycle, for the mode button and hotkeys (core1 only).
void maple_subdev_vmu_bank_cycle(bool forward);

#endif

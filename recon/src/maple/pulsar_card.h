// pulsar_card.h — the VMU docked in a Pulsar, as a storage-only card in controller slot 2.
// A card, not a mirror: the console reads and writes a 128 KiB RAM image; the BLE link
// (pulsar_link.c) pulls it from the physical VMU at connect and pushes committed blocks back.
// The image is slot 2's buffer, which is free while no SD card holds slot 2 (maple.c). Two cores
// touch it: core1 answers Maple from it, core0 fills it and reads blocks to push. Every touch is
// one memcpy under one spinlock, never across a BLE call. Present only while link up, pull
// complete and card docked: absent, the console sees no slot 2, never a broken card.
#ifndef P2M_PULSAR_CARD_H
#define P2M_PULSAR_CARD_H

#include <stdbool.h>
#include <stdint.h>
#include "maple.h"

#define PULSAR_CARD_BLOCK      512u
#define PULSAR_CARD_PHASE      128u          // one BLOCK_WRITE phase, one link DATA/WRITE
#define PULSAR_CARD_DIR_FIRST  241u          // directory 241-253, FAT 254, root 255: metadata
#define PULSAR_CARD_FAT        254u
#define PULSAR_CARD_ROOT       255u

// ---- core1 ----
// Claims the lock; before core0 starts Bluetooth. image is slot 2's vmu_state_t, or NULL when
// slot 2 is the SD card's: then the card is never present and the link pulls nothing. void *,
// as maple.c declares the vmu functions over byte buffers and cannot include vmu.h.
void pulsar_card_init(void *image);
bool pulsar_card_present(void);
// One frame for slot 2, [len][src][dst][cmd] + payload (host order). Fills out with a reply
// whose source is slot 1's; the engine sets slot 2's. Returns false when nothing is sent.
bool pulsar_card_handle(const uint8_t *frame, maple_frame_t *out);

// DEVICE_STATUS is the first 112 bytes; ALL_STATUS is all 48 words, ext the version string.
// Each word is stored big-endian (the TX path reverses every payload word), the currents
// little-endian.
typedef struct __attribute__((packed)) {
    uint8_t func[4];          // MAPLE_FUNC_* OR'd, big-endian
    uint8_t fdata[3][4];      // function data, highest function code first
    uint8_t region;           // 0xff = all regions
    uint8_t direction;        // connector direction
    char    name[30];         // space padded, no NUL
    char    license[60];      // space padded, no NUL
    uint8_t standby[2];       // tenths of a mA, little-endian
    uint8_t max_power[2];
    char    ext[80];          // space padded, no NUL
} pulsar_card_identity_t;
_Static_assert(sizeof(pulsar_card_identity_t) == 192, "ALL_STATUS is 48 words");

// ---- core0, the BLE context ----
bool pulsar_card_available(void);            // slot 2 is ours to fill (no SD card holds it)
// Presence. Clearing it also drops core1's half-staged block, so a card re-docked mid-write
// cannot commit a block built from two cards.
void pulsar_card_set_present(bool present);
// Start a pull: absent, image zeroed, nothing pending. The caller fills the image.
void pulsar_card_begin_pull(void);
void pulsar_card_fill(uint8_t blk, unsigned phase, const uint8_t data[PULSAR_CARD_PHASE]);
void pulsar_card_read(uint8_t blk, uint8_t out[PULSAR_CARD_BLOCK]);

// The write queue: block numbers, never bytes; the bytes are read from the image as the block
// goes, so a block written twice before it drains goes once, newest. Data blocks (0-240)
// first; metadata only when no data block is pending and none is in flight unconfirmed, so a
// FAT or directory never reaches the card ahead of the data it points at.
// Snapshot the next block to push into out and clear its pending bit. A commit to it after this
// sets the bit again, so the newer bytes go too. -1 if nothing may go.
int  pulsar_card_take(bool data_in_flight, uint8_t out[PULSAR_CARD_BLOCK]);
void pulsar_card_requeue(uint8_t blk);       // its push failed or was not confirmed
void pulsar_card_discard(void);              // the card changed: nothing pending is for it
unsigned pulsar_card_pending(void);          // blocks pending (heartbeat)
static inline bool pulsar_card_is_metadata(uint8_t blk) { return blk >= PULSAR_CARD_DIR_FIRST; }

// Counters for the heartbeat; written on core1, read anywhere.
extern volatile uint32_t g_pulsar_card_commits, g_pulsar_card_refused;

#endif

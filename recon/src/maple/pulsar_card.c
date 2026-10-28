// pulsar_card.c — the VMU docked in a Pulsar, as a storage-only card in slot 2 (pulsar_card.h).
//
// Slot 1 writes its image as each BLOCK_WRITE phase lands (vmu.c) and persists pages later.
// This card stages the four phases and commits on GET_LAST_ERROR: only then is the block in the
// image and its number queued for the link, so a half-written block never goes to the
// physical card. The image is slot 2's vmu_state_t, so the block-read and media-info code is
// slot 1's; the store is the link, behind the queue below, not vmu_flush_dirty (bank- and
// page-shaped, and this card has neither). No flash: no new core pauses, and no stale copy that
// outlives the Pulsar it came from. Unsent blocks are RAM only.
#include <string.h>
#include "hardware/sync.h"
#include "maple_frames.h"
#include "vmu.h"
#include "pulsar_card.h"

// Slot 2's buffer. The card reaches it only while attached: otherwise it is the SD card's VMU.
// RAM has no room for a third image.
static vmu_state_t *s_card;

// The image and everything below are under s_lock, held for one memcpy at a time.
static spin_lock_t *s_lock;
static volatile bool s_attached;
static volatile uint32_t s_attachment;   // bumped on every attach and release
static uint32_t s_pull_attachment;       // the attachment the last pull began on
static volatile bool s_linked;
static volatile bool s_present;
static volatile uint32_t s_gen;          // bumped each time presence drops
static uint32_t s_pending[8];            // one bit per block
static bool s_out;                       // a block taken and not yet settled by the link

// Core1 only: the block being staged by BLOCK_WRITE.
// Aligned: stage() writes it a word at a time, and an -Os build need not word-align a byte array.
static uint8_t  s_stage[PULSAR_CARD_BLOCK] __attribute__((aligned(4)));
static int      s_stage_blk = -1;
static uint8_t  s_stage_phases;          // bit per phase received
static uint32_t s_stage_gen;
static int      s_done_blk = -1;         // the block the last commit took (see committed())
static uint32_t s_done_gen;

volatile uint32_t g_pulsar_card_commits, g_pulsar_card_refused;

// Slot 1's console-proven identity, storage alone: functions 0x00000002 and the storage
// function data in the first slot. The version string is ours, not Sega's or their USBBT one.
static const pulsar_card_identity_t s_identity = {
    .func      = { 0x00, 0x00, 0x00, 0x02 },
    .fdata     = { { 0x00, 0x0f, 0x41, 0x00 } },  // 512 B blocks, 4 writes, 1 read
    .region    = 0xff,
    .direction = 0x00,
    .name      = "Visual Memory                 ",
    .license   = "Produced By or Under License From SEGA ENTERPRISES,LTD.     ",
    .standby   = { 0x7c, 0x00 },
    .max_power = { 0x82, 0x00 },
    .ext       = "Pulsar VMU card                                                                 ",
};

#define DATA_BITS_W7 0x0001ffffu         // blocks 224-240; 241-255 are metadata

static inline uint32_t lock(void)       { return spin_lock_blocking(s_lock); }
static inline void unlock(uint32_t irq) { spin_unlock(s_lock, irq); }

void pulsar_card_init(void *image)
{
    s_lock = spin_lock_init(spin_lock_claim_unused(true));
    s_card = image;
}

void pulsar_card_attach(void)
{
    uint32_t irq = lock();
    s_attached = true;
    s_attachment++;
    unlock(irq);
}

void pulsar_card_release(void)
{
    uint32_t irq = lock();
    if (s_present) s_gen++;
    s_present = false;
    s_attached = false;
    s_attachment++;
    memset(s_pending, 0, sizeof s_pending);
    s_out = false;
    unlock(irq);
}

bool pulsar_card_drained(void)
{
    uint32_t irq = lock();
    bool owed = s_out;
    for (int w = 0; w < 8; w++) owed |= s_pending[w] != 0;
    unlock(irq);
    return !owed;
}

bool pulsar_card_linked(void) { return s_linked; }
bool pulsar_card_present(void) { return s_present; }

// ---- core1 ----

static uint32_t rd32(const uint8_t *p) { uint32_t w; memcpy(&w, p, 4); return w; }

// One phase of 32 words, in Maple word order, into the staging block in image order.
static bool stage(uint32_t loc, const uint32_t *src, int words)
{
    unsigned part = loc >> 24, phase = (loc >> 16) & 0xff, blk = loc & 0xffff;
    if (part != 0 || phase > 3 || blk > 255 || words != (int)(PULSAR_CARD_PHASE / 4)) return false;
    if (phase == 0 || (int)blk != s_stage_blk || s_stage_gen != s_gen) {
        if (phase != 0) return false;    // a later phase with no assembly open for it
        s_stage_blk = (int)blk;
        s_stage_phases = 0;
        s_stage_gen = s_gen;
    }
    uint32_t *dst = (uint32_t *)(s_stage + phase * PULSAR_CARD_PHASE);
    for (int i = 0; i < words; i++) dst[i] = __builtin_bswap32(src[i]);
    s_stage_phases |= 1u << phase;
    return true;
}

// Every phase has to be in: a commit after a dropped phase would put 128 bytes of zeros in
// the middle of a save, push them to the physical card, and answer ACK.
static bool commit(uint32_t loc)
{
    unsigned blk = loc & 0xffff;
    if ((int)blk != s_stage_blk || s_stage_phases != 0xf) return false;
    bool ok = false;
    uint32_t irq = lock();
    if (s_present && s_stage_gen == s_gen) {     // not a block for a card that has since left
        memcpy(&s_card->image[blk * PULSAR_CARD_BLOCK], s_stage, PULSAR_CARD_BLOCK);
        s_pending[blk >> 5] |= 1u << (blk & 31);
        ok = true;
    }
    unlock(irq);
    if (ok) { s_done_blk = (int)blk; s_done_gen = s_stage_gen; }
    s_stage_blk = -1;
    return ok;
}

// A second GET_LAST_ERROR for the block just committed, its ACK lost on the bus: ACK it again, as
// slot 1 does, rather than report a failed save for a block already in the image and queued.
static bool committed(uint32_t loc)
{
    return s_stage_blk < 0 && (int)(loc & 0xffff) == s_done_blk && s_done_gen == s_gen;
}

bool pulsar_card_handle(const uint8_t *f, maple_frame_t *out)
{
    uint8_t len = f[0], cmd = f[3];
    out->payload = 0;
    bool storage = rd32(f + 4) == MAPLE_FUNC_STORAGE;

    switch (cmd) {
    case MAPLE_CMD_DEVICE_REQUEST:
        out->header = MAPLE_HDR_DEVICE_STATUS_VMU;
        out->payload = (const uint8_t *)&s_identity;
        return true;

    case MAPLE_CMD_ALL_STATUS_REQ:
        out->header = MAPLE_HDR_ALL_STATUS;
        out->payload = (const uint8_t *)&s_identity;
        return true;

    case MAPLE_CMD_RESET:
        s_stage_blk = -1;
        s_done_blk = -1;
        out->header = MAPLE_HDR_ACK_SUB;
        return true;

    case MAPLE_CMD_GET_MEDIA_INFO: {
        if (!storage) break;
        // Built from the root block, which a pull clears and refills: under the lock, as reads are.
        bool ok;
        uint32_t irq = lock();
        if ((ok = s_present)) vmu_media_info_build(s_card);
        unlock(irq);
        out->header = ok ? MAPLE_HDR_DATA_MEDIA_INFO : MAPLE_HDR_ERR_SUB;
        out->payload = ok ? s_card->media_resp : 0;
        return true;
    }

    case MAPLE_CMD_BLOCK_READ: {
        if (!storage) break;
        int r = -1;
        uint32_t irq = lock();
        if (s_present) r = vmu_block_read(s_card, rd32(f + 8));   // phased reads refused
        unlock(irq);
        if (r == 0) {
            out->header = MAPLE_HDR_DATA_BLOCK_READ;
            out->payload = (const uint8_t *)&s_card->blk_resp_func;
        } else {
            out->header = MAPLE_HDR_ERR_SUB;
        }
        return true;
    }

    case MAPLE_CMD_BLOCK_WRITE:
        if (!storage) break;
        out->header = stage(rd32(f + 8), (const uint32_t *)(f + 12), len - 2)
                    ? MAPLE_HDR_ACK_SUB : MAPLE_HDR_ERR_SUB;
        return true;

    case MAPLE_CMD_GET_LAST_ERROR:
        if (!storage) break;
        if (commit(rd32(f + 8)))         { g_pulsar_card_commits++; out->header = MAPLE_HDR_ACK_SUB; }
        else if (committed(rd32(f + 8))) out->header = MAPLE_HDR_ACK_SUB;
        else                             { g_pulsar_card_refused++; out->header = MAPLE_HDR_ERR_SUB; }
        return true;

    case MAPLE_CMD_GET_CONDITION:
    case MAPLE_CMD_SET_CONDITION:
        break;                       // storage only: no clock, no buzzer, no condition

    default:
        out->header = MAPLE_HDR_UNKNOWN_CMD_SUB;
        return true;
    }
    out->header = MAPLE_HDR_FUNC_UNSUPP_SUB;
    return true;
}

// ---- core0 ----

bool pulsar_card_available(void) { return s_attached; }
uint32_t pulsar_card_attachment(void) { return s_attachment; }

void pulsar_card_set_linked(bool linked)
{
    uint32_t irq = lock();
    s_linked = linked;
    unlock(irq);
}

// Under the lock, the pull's image is the attached one: not released since its pull began.
static bool pulled_here(void) { return s_attached && s_attachment == s_pull_attachment; }

void pulsar_card_set_present(bool present)
{
    uint32_t irq = lock();
    present = present && pulled_here();
    if (s_present && !present) s_gen++;
    s_present = present;
    unlock(irq);
}

void pulsar_card_begin_pull(void)
{
    uint32_t irq = lock();
    if (s_present) s_gen++;
    s_present = false;
    memset(s_pending, 0, sizeof s_pending);
    s_pull_attachment = s_attachment;
    unlock(irq);
    // A page at a time under the lock: core1 may release the buffer to the SD card at any moment,
    // and its SD image must never come back with zeroed pages in it.
    for (unsigned off = 0; off < sizeof s_card->image; off += 4096) {
        irq = lock();
        if (pulled_here()) memset(&s_card->image[off], 0, 4096);
        unlock(irq);
    }
}

void pulsar_card_fill(uint8_t blk, unsigned phase, const uint8_t data[PULSAR_CARD_PHASE])
{
    uint32_t irq = lock();
    if (pulled_here())
        memcpy(&s_card->image[blk * PULSAR_CARD_BLOCK + (phase & 3) * PULSAR_CARD_PHASE], data,
               PULSAR_CARD_PHASE);
    unlock(irq);
}

void pulsar_card_read(uint8_t blk, uint8_t out[PULSAR_CARD_BLOCK])
{
    uint32_t irq = lock();
    if (s_attached) memcpy(out, &s_card->image[blk * PULSAR_CARD_BLOCK], PULSAR_CARD_BLOCK);
    else            memset(out, 0, PULSAR_CARD_BLOCK);
    unlock(irq);
}

int pulsar_card_take(bool data_in_flight, uint8_t out[PULSAR_CARD_BLOCK])
{
    int blk = -1;
    uint32_t irq = lock();
    for (int w = 0; w < 8 && blk < 0; w++) {
        uint32_t m = s_pending[w] & (w == 7 ? DATA_BITS_W7 : ~0u);
        if (m) blk = w * 32 + __builtin_ctz(m);
    }
    if (blk < 0 && !data_in_flight) {
        uint32_t m = s_pending[7] & ~DATA_BITS_W7;   // directory up, then FAT, then root
        if (m) blk = 7 * 32 + __builtin_ctz(m);
    }
    if (blk >= 0) {
        s_pending[blk >> 5] &= ~(1u << (blk & 31));
        memcpy(out, &s_card->image[blk * PULSAR_CARD_BLOCK], PULSAR_CARD_BLOCK);
        s_out = true;
    }
    unlock(irq);
    return blk;
}

void pulsar_card_settled(void)
{
    uint32_t irq = lock();
    s_out = false;
    unlock(irq);
}

// Released, the card has no image to owe a block from.
void pulsar_card_requeue(uint8_t blk)
{
    uint32_t irq = lock();
    if (s_attached) s_pending[blk >> 5] |= 1u << (blk & 31);
    unlock(irq);
}

void pulsar_card_discard(void)
{
    uint32_t irq = lock();
    memset(s_pending, 0, sizeof s_pending);
    unlock(irq);
}

unsigned pulsar_card_pending(void)
{
    unsigned n = 0;
    uint32_t irq = lock();
    for (int w = 0; w < 8; w++) n += (unsigned)__builtin_popcount(s_pending[w]);
    unlock(irq);
    return n;
}

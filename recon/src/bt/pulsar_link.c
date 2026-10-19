// pulsar_link.c — the Pulsar's vendor GATT service on the BLE link Bluepad32 holds.
//
// Bluepad32 owns the connection and already initialised gatt_client (uni_bt_le.c); this module
// never initialises it again and shares it. BTstack rejects overlapping queries on one
// connection, so every query goes through gatt_client_request_to_send_gatt_query and the
// state machine issues exactly one per grant, as hids_host.c does. Notifications come through a
// separate listener filtered by connection and value handle. One slot per Bluepad32 device, so a
// BLE pad that is not a Pulsar never blocks one that is. Protocol per
// embedded_dreamcast/docs/host_integration.md (fresh C; no code from that repo).
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pulsar_link.h"
#include "pulsar_lcd.h"
#include "maple/pulsar_card.h"

#ifndef CONFIG_BLUEPAD32_MAX_DEVICES
#define CONFIG_BLUEPAD32_MAX_DEVICES 4
#endif

// 7EDF00xx-3536-4A03-82D0-8AB9122016C6, xx = 01 service, 02 LCD, 03 storage up, 04 storage down.
// BTstack's uuid128 arrays are in the printed (big-endian) order.
static const uint8_t PULSAR_SVC_UUID[16] = {
    0x7E, 0xDF, 0x00, 0x01, 0x35, 0x36, 0x4A, 0x03, 0x82, 0xD0, 0x8A, 0xB9, 0x12, 0x20, 0x16, 0xC6 };
#define PULSAR_CHR_LCD   0x02
#define PULSAR_CHR_UP    0x03
#define PULSAR_CHR_DOWN  0x04
#define HAVE_SVC         0x01
#define HAVE_ALL_CHRS    ((1 << PULSAR_CHR_LCD) | (1 << PULSAR_CHR_UP) | (1 << PULSAR_CHR_DOWN))

#define PULSAR_OP_READ       0x01   // down: [01, blk]
#define PULSAR_OP_WRITE      0x02   // down: [02, blk, phase, seq] + 128 B
#define PULSAR_OP_STATUS_REQ 0x03   // down: [03]
#define PULSAR_OP_DATA       0x81   // up: [81, blk, phase, result] + 128 B, or 4 B when result != 0
#define PULSAR_OP_ACK        0x82   // up: [82, blk, seq, result]
#define PULSAR_OP_STATUS     0x83   // up: [83, proto, flags, queue_free, epoch u32 LE, card u32 LE]
#define PULSAR_OP_WRITTEN    0x84   // up: [84, blk, seq, result]
#define PULSAR_STATUS_LEN    12
#define PULSAR_FLAG_VMU      0x01
#define PULSAR_RES_OK        0
#define PULSAR_RES_FULL      1
#define PULSAR_RES_SUPERSEDED 6
#define PULSAR_QUEUE_SLOTS   8      // the Pulsar's staging queue
#define PULSAR_TICK_MS       10     // the store and LCD service period on the owning link
#define PULSAR_READ_RETRY_MS 2000   // a READ with no DATA since: resend (refused if still held)
#define PULSAR_ACK_RETRY_MS  500    // a WRITE with no ACK: resend the same (blk, seq)
#define PULSAR_READ_TRIES    3      // DATA … FAILED this often for one block ends the pull
#define PULSAR_WRITE_RETRY_MS 5     // STATUS? retry period while GATT_CLIENT_BUSY
#define PULSAR_WRITE_TRIES    200   // ~1 s of busy before it counts as a failure
#define PULSAR_STORAGE_MTU   135    // 132-byte DATA/WRITE + ATT header; the spec says do not use
                                    // the storage protocol below it, and STATUS is part of it

typedef enum {
    PL_IDLE = 0,
    PL_W4_SERVICE,      // primary service discovery by UUID queued/running
    PL_W4_CHARS,        // characteristic discovery queued/running
    PL_W4_CCC,          // CCC write (subscribe to storage-up) queued/running
    PL_W4_WRITE,        // STATUS? not yet sent: the ACL buffers were full, retry timer armed
    PL_READY,           // STATUS? sent; notifications flow
    PL_NOT_PULSAR,      // BLE pad without the service; left alone until it disconnects
    PL_FAILED,          // see fail_where/fail_code
} pl_state_t;

typedef struct {
    pl_state_t state;
    hci_con_handle_t con;
    gatt_client_service_t svc;
    gatt_client_characteristic_t chr_lcd, chr_up, chr_down;
    uint8_t have;                                   // HAVE_SVC | 1 << PULSAR_CHR_*
    gatt_client_notification_t listener;
    bool listening;
    btstack_context_callback_registration_t query;
    btstack_timer_source_t write_retry;             // STATUS? retry while the ACL buffers are full
    uint16_t write_tries;
    uint16_t mtu;
    const char *fail_where;                         // string literal
    uint8_t fail_code;
    // last STATUS seen
    uint32_t status_count;
    uint8_t  proto, flags, queue_free;
    uint32_t epoch, card;
    uint32_t other_count;                           // other notifications, counted for the log
} pl_link_t;

static pl_link_t s_links[CONFIG_BLUEPAD32_MAX_DEVICES];

// The card's pull and push (pulsar_card.h), on the one link that owns it: the first Pulsar to
// report a STATUS. One card, so one store; a second Pulsar gets no card and no screen.
typedef struct {
    pl_link_t *owner;
    btstack_timer_source_t tick;
    // The generation the image and the queue belong to: the STATUS epoch, and the card's
    // fingerprint once the Pulsar knows it (0 until it has read blocks 255 and 254).
    bool sess, pulled;
    uint32_t epoch, card;
    // Pull: blocks to READ in order, one outstanding.
    bool pulling, read_out;
    uint8_t order[256];
    uint16_t n, i;
    uint8_t phases, tries;
    uint32_t read_ms;
    // Push: one block going out (four WRITE phases, then its ACK), and the blocks acked but not
    // yet WRITTEN, which hold the Pulsar's queue slots.
    bool sending, full;
    uint8_t blk, seq, next_seq, phase;
    uint32_t sent_ms;
    uint8_t buf[PULSAR_CARD_BLOCK];
    struct { uint8_t blk, seq; } flight[PULSAR_QUEUE_SLOTS];
    unsigned nflight;
    uint32_t written, failed;
} pl_store_t;

static pl_store_t s_store;

static void store_tick(btstack_timer_source_t *ts);

static void request_next_query(pl_link_t *l);
static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static pl_link_t *link_for_handle(hci_con_handle_t con)
{
    for (int i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; i++)
        if (s_links[i].state != PL_IDLE && s_links[i].con == con) return &s_links[i];
    return NULL;
}

static void fail(pl_link_t *l, const char *where, uint8_t code)
{
    l->state = PL_FAILED;
    l->fail_where = where;
    l->fail_code = code;
    printf("[pulsar] 0x%04x FAILED at %s, code 0x%02x\n", l->con, where, code);
}

static bool is_pulsar_chr(const gatt_client_characteristic_t *c, uint8_t which)
{
    if (c->uuid128[2] != 0x00 || c->uuid128[3] != which) return false;
    return memcmp(c->uuid128, PULSAR_SVC_UUID, 2) == 0 &&
           memcmp(c->uuid128 + 4, PULSAR_SVC_UUID + 4, 12) == 0;
}

// ---- The store: the card's pull and push -----------------------------------------
// Session rule, decided before the pull: a STATUS whose epoch and card match the session's keeps
// the image and the pending queue (a reconnect while the Pulsar was still draining); anything
// else means a different card, or the same one after something the Pulsar could not vouch for,
// and discards both before a fresh pull. card reads 0 until the Pulsar has read 255 and 254 in
// this generation, so 0 matches anything and the first non-zero value is adopted.

static uint8_t store_write(pl_link_t *l, const uint8_t *v, uint16_t len)
{
    return gatt_client_write_value_of_characteristic_without_response(
        l->con, l->chr_down.value_handle, len, (uint8_t *)v);
}

static void store_forget_push(void)
{
    s_store.sending = false;
    s_store.nflight = 0;
    s_store.full = false;
}

// Root, FAT, directory from the top, then whatever the FAT says is in use.
static void pull_start(void)
{
    pulsar_card_begin_pull();
    pulsar_card_discard();
    store_forget_push();
    s_store.n = 0;
    s_store.order[s_store.n++] = PULSAR_CARD_ROOT;
    s_store.order[s_store.n++] = PULSAR_CARD_FAT;
    for (int b = PULSAR_CARD_FAT - 1; b >= (int)PULSAR_CARD_DIR_FIRST; b--) s_store.order[s_store.n++] = (uint8_t)b;
    s_store.i = 0;
    s_store.phases = 0;
    s_store.tries = 0;
    s_store.read_out = false;
    s_store.pulling = true;
    s_store.pulled = false;
    printf("[pulsar] card: pull started (epoch %lu)\n", (unsigned long)s_store.epoch);
}

// Free FAT entries read 0xfffc; entries are little-endian u16 in image order.
static void pull_add_used(void)
{
    pulsar_card_read(PULSAR_CARD_FAT, s_store.buf);
    for (unsigned b = 0; b < PULSAR_CARD_DIR_FIRST; b++)
        if (little_endian_read_16(s_store.buf, b * 2) != 0xfffc) s_store.order[s_store.n++] = (uint8_t)b;
}

// FNV-1a over 255 then 254, the Pulsar's card fingerprint: logged against its STATUS.
static uint32_t card_fingerprint(void)
{
    uint32_t h = 0x811c9dc5u;
    static const uint8_t blocks[2] = { PULSAR_CARD_ROOT, PULSAR_CARD_FAT };
    for (int k = 0; k < 2; k++) {
        pulsar_card_read(blocks[k], s_store.buf);
        for (unsigned i = 0; i < PULSAR_CARD_BLOCK; i++) { h ^= s_store.buf[i]; h *= 0x01000193u; }
    }
    return h;
}

static void store_on_status(pl_link_t *l)
{
    if (!s_store.owner) {
        s_store.owner = l;
        btstack_run_loop_set_timer_handler(&s_store.tick, store_tick);
        btstack_run_loop_set_timer_context(&s_store.tick, l);
        btstack_run_loop_set_timer(&s_store.tick, PULSAR_TICK_MS);
        btstack_run_loop_add_timer(&s_store.tick);
        printf("[pulsar] 0x%04x owns the card\n", l->con);
    }
    if (l != s_store.owner) return;
    s_store.full = false;                                   // queue_free may have moved

    if (!(l->flags & PULSAR_FLAG_VMU)) {                    // undocked: the card is gone
        if (s_store.sess) printf("[pulsar] card: undocked, %u pending dropped\n", pulsar_card_pending());
        pulsar_card_set_present(false);
        pulsar_card_discard();
        store_forget_push();
        s_store.sess = s_store.pulling = s_store.pulled = false;
        return;
    }
    bool same = s_store.sess && l->epoch == s_store.epoch &&
                (!s_store.card || !l->card || l->card == s_store.card);
    if (same) {
        if (!s_store.card && l->card) s_store.card = l->card;
        if (s_store.pulled) pulsar_card_set_present(true);  // reconnect: image and queue stand
        else if (!s_store.pulling) pull_start();            // the last pull did not finish
        return;
    }
    if (s_store.sess) printf("[pulsar] card: new generation, %u pending dropped\n", pulsar_card_pending());
    s_store.sess = true;
    s_store.epoch = l->epoch;
    s_store.card = l->card;
    pull_start();
}

static void store_on_data(pl_link_t *l, const uint8_t *v, uint16_t len)
{
    if (l != s_store.owner || !s_store.pulling || len < 4) return;
    uint8_t blk = v[1], phase = v[2], res = v[3];
    if (blk != s_store.order[s_store.i]) return;
    if (res != PULSAR_RES_OK) {
        // NO_VMU and DISCARDED come with a new generation, FAILED ends one: let the next
        // STATUS start over, and ask for it in case none is on its way.
        printf("[pulsar] card: READ %u result %u, pull stopped\n", blk, res);
        s_store.pulling = false;
        s_store.sess = false;
        uint8_t op = PULSAR_OP_STATUS_REQ;
        (void)store_write(l, &op, 1);
        return;
    }
    if (len != 4 + PULSAR_CARD_PHASE || phase > 3) return;
    pulsar_card_fill(blk, phase, v + 4);
    s_store.phases |= 1u << phase;
    s_store.read_ms = btstack_run_loop_get_time_ms();
    if (s_store.phases != 0xf) return;

    s_store.phases = 0;
    s_store.read_out = false;
    s_store.i++;
    if (s_store.i == 2)
        printf("[pulsar] card: fingerprint %08lx, STATUS says %08lx\n",
               (unsigned long)card_fingerprint(), (unsigned long)l->card);
    if (s_store.i == PULSAR_CARD_ROOT - PULSAR_CARD_DIR_FIRST + 1) pull_add_used();
    if (s_store.i < s_store.n) return;
    s_store.pulling = false;
    s_store.pulled = true;
    pulsar_card_set_present(true);
    printf("[pulsar] card: pulled %u blocks, present\n", s_store.n);
}

static void store_on_ack(pl_link_t *l, const uint8_t *v, uint16_t len)
{
    if (l != s_store.owner || len != 4 || !s_store.sending) return;
    if (v[1] != s_store.blk || v[2] != s_store.seq) return;
    s_store.sending = false;
    if (v[3] == PULSAR_RES_OK) {
        s_store.flight[s_store.nflight].blk = s_store.blk;
        s_store.flight[s_store.nflight].seq = s_store.seq;
        s_store.nflight++;
        return;
    }
    // FULL, NO_VMU, BAD: put it back and wait for a WRITTEN or a STATUS before the next.
    printf("[pulsar] card: WRITE %u seq %u result %u\n", s_store.blk, s_store.seq, v[3]);
    pulsar_card_requeue(s_store.blk);
    s_store.full = true;
}

static void store_on_written(pl_link_t *l, const uint8_t *v, uint16_t len)
{
    if (l != s_store.owner || len != 4) return;
    for (unsigned k = 0; k < s_store.nflight; k++) {
        if (s_store.flight[k].blk != v[1] || s_store.flight[k].seq != v[2]) continue;
        s_store.flight[k] = s_store.flight[--s_store.nflight];
        s_store.full = false;
        if (v[3] == PULSAR_RES_OK) {
            s_store.written++;
        } else if (v[3] != PULSAR_RES_SUPERSEDED) {         // superseded: a newer seq carries it
            s_store.failed++;
            printf("[pulsar] card: WRITTEN %u seq %u result %u, requeued\n", v[1], v[2], v[3]);
            pulsar_card_requeue(v[1]);
        }
        return;
    }
    // Not ours: a WRITTEN for a session already discarded.
}

static bool data_in_flight(void)
{
    if (s_store.sending && !pulsar_card_is_metadata(s_store.blk)) return true;
    for (unsigned k = 0; k < s_store.nflight; k++)
        if (!pulsar_card_is_metadata(s_store.flight[k].blk)) return true;
    return false;
}

static void pull_service(pl_link_t *l, uint32_t now)
{
    if (s_store.read_out && now - s_store.read_ms < PULSAR_READ_RETRY_MS) return;
    // A READ is held while the pad is in use, and refused in silence while one is outstanding,
    // so resending one that has seen no DATA for a while costs nothing and recovers a lost one.
    uint8_t rd[2] = { PULSAR_OP_READ, s_store.order[s_store.i] };
    if (store_write(l, rd, sizeof rd) != ERROR_CODE_SUCCESS) return;   // busy: next tick
    s_store.read_out = true;
    s_store.read_ms = now;
}

// A block goes out whole: snapshotted under the card's lock by pulsar_card_take, then four
// phases from that copy. A console write to it meanwhile marks it pending again.
static void push_service(pl_link_t *l, uint32_t now)
{
    if (!s_store.sending) {
        if (s_store.full || s_store.nflight >= PULSAR_QUEUE_SLOTS) return;
        int b = pulsar_card_take(data_in_flight(), s_store.buf);
        if (b < 0) return;
        s_store.sending = true;
        s_store.blk = (uint8_t)b;
        s_store.seq = s_store.next_seq++;
        s_store.phase = 0;
    }
    if (s_store.phase == 4) {
        if (now - s_store.sent_ms < PULSAR_ACK_RETRY_MS) return;
        s_store.phase = 0;                  // no ACK: the same (blk, seq) again, which is safe
    }
    while (s_store.phase < 4) {
        uint8_t w[4 + PULSAR_CARD_PHASE] = { PULSAR_OP_WRITE, s_store.blk, s_store.phase, s_store.seq };
        memcpy(w + 4, s_store.buf + s_store.phase * PULSAR_CARD_PHASE, PULSAR_CARD_PHASE);
        if (store_write(l, w, sizeof w) != ERROR_CODE_SUCCESS) return;  // busy: next tick
        s_store.phase++;
    }
    s_store.sent_ms = now;
}

static void store_tick(btstack_timer_source_t *ts)
{
    pl_link_t *l = btstack_run_loop_get_timer_context(ts);
    btstack_run_loop_set_timer(ts, PULSAR_TICK_MS);
    btstack_run_loop_add_timer(ts);
    if (l->state != PL_READY) return;
    uint32_t now = btstack_run_loop_get_time_ms();
    if (s_store.pulling) pull_service(l, now);
    else if (s_store.pulled && (l->flags & PULSAR_FLAG_VMU)) push_service(l, now);
    pulsar_lcd_service(l->con, l->chr_lcd.value_handle, l->mtu);
}

// The owner's link dropped. The card leaves the bus; the session, the image and what is still
// owed stay for a reconnect to the same generation. Acked blocks with no WRITTEN yet have an
// unknown fate: pending again, so they are resent (the same bytes, harmless) if it is the same.
static void store_drop(void)
{
    btstack_run_loop_remove_timer(&s_store.tick);
    pulsar_card_set_present(false);
    if (s_store.sending) pulsar_card_requeue(s_store.blk);
    for (unsigned k = 0; k < s_store.nflight; k++) pulsar_card_requeue(s_store.flight[k].blk);
    store_forget_push();
    s_store.pulling = false;
    s_store.owner = NULL;
    pulsar_lcd_reset();
}

static void decode_status(pl_link_t *l, const uint8_t *v, uint16_t len)
{
    if (len != PULSAR_STATUS_LEN) {
        printf("[pulsar] STATUS with len %u, want %u\n", len, PULSAR_STATUS_LEN);
        return;
    }
    l->proto      = v[1];
    l->flags      = v[2];
    l->queue_free = v[3];
    l->epoch      = little_endian_read_32(v, 4);
    l->card       = little_endian_read_32(v, 8);
    l->status_count++;
    printf("[pulsar] STATUS proto=%u flags=0x%02x (vmu=%u enabled=%u draining=%u idle=%u) "
           "queue_free=%u epoch=%lu card=%08lx\n",
           l->proto, l->flags, l->flags & 1, (l->flags >> 1) & 1, (l->flags >> 2) & 1,
           (l->flags >> 3) & 1, l->queue_free, (unsigned long)l->epoch, (unsigned long)l->card);
    store_on_status(l);
}

// Separate listener: BTstack filters on connection and value handle; checked again here.
static void notify_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;
    pl_link_t *l = link_for_handle(gatt_event_notification_get_handle(packet));
    if (!l || gatt_event_notification_get_value_handle(packet) != l->chr_up.value_handle) return;

    const uint8_t *v = gatt_event_notification_get_value(packet);
    uint16_t len = gatt_event_notification_get_value_length(packet);
    if (len == 0) return;
    switch (v[0]) {
    case PULSAR_OP_STATUS:  decode_status(l, v, len); break;
    case PULSAR_OP_DATA:    store_on_data(l, v, len); break;
    case PULSAR_OP_ACK:     store_on_ack(l, v, len); break;
    case PULSAR_OP_WRITTEN: store_on_written(l, v, len); break;
    default:
        l->other_count++;
        printf("[pulsar] notify op=0x%02x len=%u\n", v[0], len);
        break;
    }
}

// STATUS? is one write without response. It goes out directly and, while the ACL buffers are
// full, is retried on a BTstack timer. Not through gatt_client_request_to_write_without_response:
// BTstack has no call to take a registration back off that queue, so a Bluepad32 disconnect that
// arrives before HCI's would leave BTstack holding a callback into a slot we have zeroed. A timer
// can be removed. No remote acknowledgement exists, but the local call returns errors: check them.
static void send_status_request(btstack_timer_source_t *ts)
{
    pl_link_t *l = btstack_run_loop_get_timer_context(ts);
    if (l->state != PL_W4_WRITE) return;
    uint8_t op = PULSAR_OP_STATUS_REQ;
    uint8_t st = gatt_client_write_value_of_characteristic_without_response(
        l->con, l->chr_down.value_handle, 1, &op);
    if (st == GATT_CLIENT_BUSY && ++l->write_tries < PULSAR_WRITE_TRIES) {
        btstack_run_loop_set_timer(&l->write_retry, PULSAR_WRITE_RETRY_MS);
        btstack_run_loop_add_timer(&l->write_retry);
        return;
    }
    if (st != ERROR_CODE_SUCCESS) { fail(l, "status-write", st); return; }
    l->state = PL_READY;
    printf("[pulsar] 0x%04x STATUS? sent on 0x%04x\n", l->con, l->chr_down.value_handle);
}

static void request_status_write(pl_link_t *l)
{
    l->state = PL_W4_WRITE;
    l->write_tries = 0;
    btstack_run_loop_set_timer_handler(&l->write_retry, send_status_request);
    btstack_run_loop_set_timer_context(&l->write_retry, l);
    send_status_request(&l->write_retry);
}

static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    pl_link_t *l;

    switch (hci_event_packet_get_type(packet)) {
    case GATT_EVENT_SERVICE_QUERY_RESULT:
        l = link_for_handle(gatt_event_service_query_result_get_handle(packet));
        if (!l || l->state != PL_W4_SERVICE) return;
        gatt_event_service_query_result_get_service(packet, &l->svc);
        l->have |= HAVE_SVC;
        break;

    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
        l = link_for_handle(gatt_event_characteristic_query_result_get_handle(packet));
        if (!l || l->state != PL_W4_CHARS) return;
        gatt_client_characteristic_t c;
        gatt_event_characteristic_query_result_get_characteristic(packet, &c);
        if      (is_pulsar_chr(&c, PULSAR_CHR_LCD))  { l->chr_lcd  = c; l->have |= 1 << PULSAR_CHR_LCD; }
        else if (is_pulsar_chr(&c, PULSAR_CHR_UP))   { l->chr_up   = c; l->have |= 1 << PULSAR_CHR_UP; }
        else if (is_pulsar_chr(&c, PULSAR_CHR_DOWN)) { l->chr_down = c; l->have |= 1 << PULSAR_CHR_DOWN; }
        break;
    }

    case GATT_EVENT_QUERY_COMPLETE: {
        l = link_for_handle(gatt_event_query_complete_get_handle(packet));
        if (!l) return;
        uint8_t att = gatt_event_query_complete_get_att_status(packet);
        switch (l->state) {
        case PL_W4_SERVICE:
            if (att != ATT_ERROR_SUCCESS) { fail(l, "service-discovery", att); break; }
            if (!(l->have & HAVE_SVC)) {
                l->state = PL_NOT_PULSAR;
                printf("[pulsar] 0x%04x has no HostService: not a Pulsar\n", l->con);
                break;
            }
            // HIDS already ran queries, so BTstack's automatic MTU exchange has happened.
            gatt_client_get_mtu(l->con, &l->mtu);
            printf("[pulsar] 0x%04x HostService 0x%04x..0x%04x, MTU %u\n",
                   l->con, l->svc.start_group_handle, l->svc.end_group_handle, l->mtu);
            if (l->mtu < PULSAR_STORAGE_MTU) { fail(l, "mtu-below-135", (uint8_t)l->mtu); break; }
            l->state = PL_W4_CHARS;
            request_next_query(l);
            break;

        case PL_W4_CHARS:
            if (att != ATT_ERROR_SUCCESS) { fail(l, "char-discovery", att); break; }
            printf("[pulsar] 0x%04x chars: lcd=0x%04x up=0x%04x down=0x%04x (have=0x%02x)\n",
                   l->con, l->chr_lcd.value_handle, l->chr_up.value_handle,
                   l->chr_down.value_handle, l->have);
            if ((l->have & HAVE_ALL_CHRS) != HAVE_ALL_CHRS) { fail(l, "chars-missing", l->have); break; }
            l->state = PL_W4_CCC;
            request_next_query(l);
            break;

        case PL_W4_CCC:
            if (att != ATT_ERROR_SUCCESS) { fail(l, "ccc-write", att); break; }
            printf("[pulsar] 0x%04x subscribed to storage-up\n", l->con);
            request_status_write(l);
            break;

        default:
            break;
        }
        break;
    }

    default:
        break;
    }
}

// Runs when gatt_client grants us the connection: issue exactly one query for the state.
static void send_next_query(void *context)
{
    pl_link_t *l = context;
    uint8_t st;
    switch (l->state) {
    case PL_W4_SERVICE:
        st = gatt_client_discover_primary_services_by_uuid128(gatt_handler, l->con, PULSAR_SVC_UUID);
        if (st != ERROR_CODE_SUCCESS) fail(l, "service-query", st);
        break;
    case PL_W4_CHARS:
        st = gatt_client_discover_characteristics_for_service(gatt_handler, l->con, &l->svc);
        if (st != ERROR_CODE_SUCCESS) fail(l, "char-query", st);
        break;
    case PL_W4_CCC:
        // Nothing is notified, and writes are dropped in silence, on an unencrypted link.
        if (gap_encryption_key_size(l->con) == 0) { fail(l, "link-not-encrypted", 0); break; }
        gatt_client_listen_for_characteristic_value_updates(&l->listener, notify_handler, l->con, &l->chr_up);
        l->listening = true;
        st = gatt_client_write_client_characteristic_configuration(
            gatt_handler, l->con, &l->chr_up, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
        if (st != ERROR_CODE_SUCCESS) fail(l, "ccc-query", st);
        break;
    default:
        break;
    }
}

static void request_next_query(pl_link_t *l)
{
    l->query.callback = send_next_query;
    l->query.context = l;
    uint8_t st = gatt_client_request_to_send_gatt_query(&l->query, l->con);
    if (st != ERROR_CODE_SUCCESS) fail(l, "query-request", st);
}

void pulsar_link_attach(uni_hid_device_t *d)
{
    if (d->conn.protocol != UNI_BT_CONN_PROTOCOL_BLE) return;
    if (link_for_handle(d->conn.handle)) {
        printf("[pulsar] 0x%04x already attached\n", d->conn.handle);
        return;
    }
    pl_link_t *l = NULL;
    for (int i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES && !l; i++)
        if (s_links[i].state == PL_IDLE) l = &s_links[i];
    if (!l) { printf("[pulsar] no free link slot for 0x%04x\n", d->conn.handle); return; }
    memset(l, 0, sizeof *l);
    l->con = d->conn.handle;
    l->state = PL_W4_SERVICE;
    printf("[pulsar] attach: handle 0x%04x vid=%04x pid=%04x, discovering HostService\n",
           l->con, d->vendor_id, d->product_id);
    request_next_query(l);
}

// Bluepad32 calls the platform before it invalidates d->conn.handle (uni_hid_device_disconnect),
// and before BTstack's own gatt_client disconnect handling frees the context.
void pulsar_link_detach(uni_hid_device_t *d)
{
    if (d->conn.protocol != UNI_BT_CONN_PROTOCOL_BLE) return;
    pl_link_t *l = link_for_handle(d->conn.handle);
    if (!l) return;
    if (l->listening) gatt_client_stop_listening_for_characteristic_value_updates(&l->listener);
    (void)gatt_client_remove_gatt_query(&l->query, l->con);   // no-op if not queued
    btstack_run_loop_remove_timer(&l->write_retry);            // no-op if not armed
    if (l == s_store.owner) store_drop();
    printf("[pulsar] detach: handle 0x%04x (state %d, %lu STATUS, %lu other)\n",
           l->con, l->state, (unsigned long)l->status_count, (unsigned long)l->other_count);
    memset(l, 0, sizeof *l);
}

// Read from the core0 main loop while BTstack (the cyw43 async context) writes the links:
// snapshot the one link worth showing under the async-context lock, format after release.
const char *pulsar_link_heartbeat(void)
{
    static char buf[160];
    pl_link_t snap;
    struct { bool pulling, pulled; uint16_t i, n; unsigned nflight; uint32_t written, failed; } st;
    int rank_best = 0;                 // 1 not-pulsar, 2 failed, 3 discovering, 4 ready
    async_context_t *ctx = cyw43_arch_async_context();
    if (!ctx) return "none";           // cyw43 init failed: bt_init returned, nothing attaches
    async_context_acquire_lock_blocking(ctx);
    for (int i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; i++) {
        int rank;
        switch (s_links[i].state) {
        case PL_IDLE:       rank = 0; break;
        case PL_NOT_PULSAR: rank = 1; break;
        case PL_FAILED:     rank = 2; break;
        case PL_READY:      rank = 4; break;
        default:            rank = 3; break;
        }
        if (rank > rank_best) { rank_best = rank; snap = s_links[i]; }
    }
    st.pulling = s_store.pulling; st.pulled = s_store.pulled;       // not the block buffer:
    st.i = s_store.i; st.n = s_store.n; st.nflight = s_store.nflight; // core0's stack is 4 KB
    st.written = s_store.written; st.failed = s_store.failed;
    async_context_release_lock(ctx);

    switch (rank_best) {
    case 0: return "none";
    case 1: return "not-pulsar";
    case 2:
        snprintf(buf, sizeof buf, "fail:%s/0x%02x", snap.fail_where, snap.fail_code);
        return buf;
    case 4: {
        char card[48];
        if (st.pulling)     snprintf(card, sizeof card, "pull %u/%u", st.i, st.n);
        else if (st.pulled && (snap.flags & PULSAR_FLAG_VMU))
            snprintf(card, sizeof card, "on q%u f%u w%lu x%lu c%lu/%lu", pulsar_card_pending(),
                     st.nflight, (unsigned long)st.written, (unsigned long)st.failed,
                     (unsigned long)g_pulsar_card_commits, (unsigned long)g_pulsar_card_refused);
        else                snprintf(card, sizeof card, "off");
        snprintf(buf, sizeof buf, "ready mtu=%u st=f:%02x q:%u ep:%lu card:%08lx n=%lu | card %s lcd %lu/%lu",
                 snap.mtu, snap.flags, snap.queue_free, (unsigned long)snap.epoch,
                 (unsigned long)snap.card, (unsigned long)snap.status_count, card,
                 (unsigned long)g_pulsar_lcd_sent, (unsigned long)g_pulsar_lcd_in);
        return buf;
    }
    default:
        snprintf(buf, sizeof buf, "discover:%d", snap.state);
        return buf;
    }
}

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

#define PULSAR_OP_STATUS_REQ 0x03   // down: [03]
#define PULSAR_OP_STATUS     0x83   // up: [83, proto, flags, queue_free, epoch u32 LE, card u32 LE]
#define PULSAR_STATUS_LEN    12
#define PULSAR_STORAGE_MTU   135    // 132-byte DATA/WRITE + ATT header; the spec says do not use
                                    // the storage protocol below it, and STATUS is part of it

typedef enum {
    PL_IDLE = 0,
    PL_W4_SERVICE,      // primary service discovery by UUID queued/running
    PL_W4_CHARS,        // characteristic discovery queued/running
    PL_W4_CCC,          // CCC write (subscribe to storage-up) queued/running
    PL_W4_WRITE,        // waiting for a write-without-response grant for STATUS?
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
    btstack_context_callback_registration_t write_req;
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
    if (v[0] == PULSAR_OP_STATUS) {
        decode_status(l, v, len);
    } else {
        l->other_count++;
        printf("[pulsar] notify op=0x%02x len=%u\n", v[0], len);
    }
}

// Granted a write-without-response slot (queued API, shared fairly with hids_host's output
// reports). No remote acknowledgement exists, but the local call returns errors: check them.
static void send_status_request(void *context)
{
    pl_link_t *l = context;
    if (l->state != PL_W4_WRITE) return;
    uint8_t op = PULSAR_OP_STATUS_REQ;
    uint8_t st = gatt_client_write_value_of_characteristic_without_response(
        l->con, l->chr_down.value_handle, 1, &op);
    if (st != ERROR_CODE_SUCCESS) { fail(l, "status-write", st); return; }
    l->state = PL_READY;
    printf("[pulsar] 0x%04x STATUS? sent on 0x%04x\n", l->con, l->chr_down.value_handle);
}

static void request_status_write(pl_link_t *l)
{
    l->state = PL_W4_WRITE;   // before the request: the grant can fire inside it
    l->write_req.callback = send_status_request;
    l->write_req.context = l;
    uint8_t st = gatt_client_request_to_write_without_response(&l->write_req, l->con);
    if (st != ERROR_CODE_SUCCESS) fail(l, "write-request", st);
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
    printf("[pulsar] detach: handle 0x%04x (state %d, %lu STATUS, %lu other)\n",
           l->con, l->state, (unsigned long)l->status_count, (unsigned long)l->other_count);
    memset(l, 0, sizeof *l);
}

// Read from the core0 main loop while BTstack (the cyw43 async context) writes the links:
// snapshot the one link worth showing under the async-context lock, format after release.
const char *pulsar_link_heartbeat(void)
{
    static char buf[72];
    pl_link_t snap;
    int rank_best = 0;                 // 1 not-pulsar, 2 failed, 3 discovering, 4 ready
    async_context_t *ctx = cyw43_arch_async_context();
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
    async_context_release_lock(ctx);

    switch (rank_best) {
    case 0: return "none";
    case 1: return "not-pulsar";
    case 2:
        snprintf(buf, sizeof buf, "fail:%s/0x%02x", snap.fail_where, snap.fail_code);
        return buf;
    case 4:
        snprintf(buf, sizeof buf, "ready mtu=%u st=f:%02x q:%u ep:%lu card:%08lx n=%lu",
                 snap.mtu, snap.flags, snap.queue_free, (unsigned long)snap.epoch,
                 (unsigned long)snap.card, (unsigned long)snap.status_count);
        return buf;
    default:
        snprintf(buf, sizeof buf, "discover:%d", snap.state);
        return buf;
    }
}

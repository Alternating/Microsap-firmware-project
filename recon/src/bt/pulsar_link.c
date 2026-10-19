// pulsar_link.c — the Pulsar's vendor GATT service on the BLE link Bluepad32 holds.
//
// Bluepad32 owns the connection and already initialised gatt_client (uni_bt_le.c); this module
// never initialises it again and shares it. BTstack rejects overlapping queries on one
// connection, so every query goes through gatt_client_request_to_send_gatt_query and the
// state machine issues exactly one per grant, as hids_host.c does. Notifications come through a
// separate listener filtered by connection and value handle. Protocol per
// embedded_dreamcast/docs/host_integration.md (fresh C; no code from that repo).
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "pulsar_link.h"

// 7EDF00xx-3536-4A03-82D0-8AB9122016C6, xx = 01 service, 02 LCD, 03 storage up, 04 storage down.
// BTstack's uuid128 arrays are in the printed (big-endian) order.
static const uint8_t PULSAR_SVC_UUID[16] = {
    0x7E, 0xDF, 0x00, 0x01, 0x35, 0x36, 0x4A, 0x03, 0x82, 0xD0, 0x8A, 0xB9, 0x12, 0x20, 0x16, 0xC6 };
#define PULSAR_CHR_LCD   0x02
#define PULSAR_CHR_UP    0x03
#define PULSAR_CHR_DOWN  0x04

#define PULSAR_OP_STATUS_REQ 0x03   // down: [03]
#define PULSAR_OP_DATA       0x81
#define PULSAR_OP_ACK        0x82
#define PULSAR_OP_STATUS     0x83   // up: [83, proto, flags, queue_free, epoch u32 LE, card u32 LE]
#define PULSAR_OP_WRITTEN    0x84
#define PULSAR_STATUS_LEN    12
#define PULSAR_STORAGE_MTU   135    // 132-byte DATA/WRITE + ATT header

typedef enum {
    PL_IDLE = 0,
    PL_W4_SERVICE,      // primary service discovery by UUID queued/running
    PL_W4_CHARS,        // characteristic discovery queued/running
    PL_W4_CCC,          // CCC write (subscribe to storage-up) queued/running
    PL_W4_CAN_WRITE,    // waiting for a write-without-response slot for STATUS?
    PL_READY,           // STATUS? sent; notifications flow
    PL_NOT_PULSAR,      // BLE pad without the service; left alone
    PL_FAILED,          // see fail_where/fail_code
} pl_state_t;

static struct {
    pl_state_t state;
    hci_con_handle_t con;
    gatt_client_service_t svc;
    gatt_client_characteristic_t chr_lcd, chr_up, chr_down;
    uint8_t have;                                   // bitmask of PULSAR_CHR_* found
    gatt_client_notification_t listener;
    btstack_context_callback_registration_t query;
    uint16_t mtu;
    const char *fail_where;
    uint8_t fail_code;
    // last STATUS seen
    uint32_t status_count;
    uint8_t  proto, flags, queue_free;
    uint32_t epoch, card;
    // anything else notified, counted for the log
    uint32_t other_count;
} s;

static void request_next_query(void);
static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void fail(const char *where, uint8_t code)
{
    s.state = PL_FAILED;
    s.fail_where = where;
    s.fail_code = code;
    printf("[pulsar] FAILED at %s, code 0x%02x\n", where, code);
}

static bool is_pulsar_chr(const gatt_client_characteristic_t *c, uint8_t which)
{
    if (c->uuid128[2] != 0x00 || c->uuid128[3] != which) return false;
    return memcmp(c->uuid128, PULSAR_SVC_UUID, 2) == 0 &&
           memcmp(c->uuid128 + 4, PULSAR_SVC_UUID + 4, 12) == 0;
}

static void decode_status(const uint8_t *v, uint16_t len)
{
    if (len != PULSAR_STATUS_LEN) {
        printf("[pulsar] STATUS with len %u, want %u\n", len, PULSAR_STATUS_LEN);
        return;
    }
    s.proto      = v[1];
    s.flags      = v[2];
    s.queue_free = v[3];
    s.epoch      = little_endian_read_32(v, 4);
    s.card       = little_endian_read_32(v, 8);
    s.status_count++;
    printf("[pulsar] STATUS proto=%u flags=0x%02x (vmu=%u enabled=%u draining=%u idle=%u) "
           "queue_free=%u epoch=%lu card=%08lx\n",
           s.proto, s.flags, s.flags & 1, (s.flags >> 1) & 1, (s.flags >> 2) & 1,
           (s.flags >> 3) & 1, s.queue_free, (unsigned long)s.epoch, (unsigned long)s.card);
}

// Separate listener: only our connection and only storage-up's value handle reach here.
static void notify_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;
    if (gatt_event_notification_get_handle(packet) != s.con) return;
    if (gatt_event_notification_get_value_handle(packet) != s.chr_up.value_handle) return;

    const uint8_t *v = gatt_event_notification_get_value(packet);
    uint16_t len = gatt_event_notification_get_value_length(packet);
    if (len == 0) return;
    switch (v[0]) {
    case PULSAR_OP_STATUS: decode_status(v, len); break;
    default:
        s.other_count++;
        printf("[pulsar] notify op=0x%02x len=%u\n", v[0], len);
        break;
    }
}

// Write Without Response gets no remote acknowledgement, but the local call does return
// errors (oversize, busy): check them. Busy -> wait for the can-write event and retry once.
static void send_status_request(void)
{
    uint8_t op = PULSAR_OP_STATUS_REQ;
    uint8_t st = gatt_client_write_value_of_characteristic_without_response(
        s.con, s.chr_down.value_handle, 1, &op);
    if (st == ERROR_CODE_SUCCESS) {
        s.state = PL_READY;
        printf("[pulsar] STATUS? sent on 0x%04x\n", s.chr_down.value_handle);
        return;
    }
    if (st == GATT_CLIENT_BUSY && s.state != PL_W4_CAN_WRITE) {
        s.state = PL_W4_CAN_WRITE;   // set before the request: the event can fire inside it
        st = gatt_client_request_can_write_without_response_event(gatt_handler, s.con);
        if (st == ERROR_CODE_SUCCESS) return;
    }
    fail("status-write", st);
}

static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case GATT_EVENT_SERVICE_QUERY_RESULT:
        gatt_event_service_query_result_get_service(packet, &s.svc);
        s.have |= 0x01;
        break;

    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
        gatt_client_characteristic_t c;
        gatt_event_characteristic_query_result_get_characteristic(packet, &c);
        if      (is_pulsar_chr(&c, PULSAR_CHR_LCD))  { s.chr_lcd  = c; s.have |= 1 << PULSAR_CHR_LCD; }
        else if (is_pulsar_chr(&c, PULSAR_CHR_UP))   { s.chr_up   = c; s.have |= 1 << PULSAR_CHR_UP; }
        else if (is_pulsar_chr(&c, PULSAR_CHR_DOWN)) { s.chr_down = c; s.have |= 1 << PULSAR_CHR_DOWN; }
        break;
    }

    case GATT_EVENT_QUERY_COMPLETE: {
        uint8_t att = gatt_event_query_complete_get_att_status(packet);
        switch (s.state) {
        case PL_W4_SERVICE:
            if (att != ATT_ERROR_SUCCESS) { fail("service-discovery", att); break; }
            if (!(s.have & 0x01)) {
                s.state = PL_NOT_PULSAR;
                printf("[pulsar] no HostService on handle 0x%04x: not a Pulsar\n", s.con);
                break;
            }
            // HIDS already ran queries, so BTstack's automatic MTU exchange has happened.
            gatt_client_get_mtu(s.con, &s.mtu);
            printf("[pulsar] HostService 0x%04x..0x%04x, MTU %u%s\n",
                   s.svc.start_group_handle, s.svc.end_group_handle, s.mtu,
                   s.mtu < PULSAR_STORAGE_MTU ? " (below 135: storage protocol unusable)" : "");
            s.state = PL_W4_CHARS;
            request_next_query();
            break;

        case PL_W4_CHARS:
            if (att != ATT_ERROR_SUCCESS) { fail("char-discovery", att); break; }
            printf("[pulsar] chars: lcd=0x%04x up=0x%04x down=0x%04x (have=0x%02x)\n",
                   s.chr_lcd.value_handle, s.chr_up.value_handle, s.chr_down.value_handle, s.have);
            if ((s.have & 0x1c) != 0x1c) { fail("chars-missing", s.have); break; }
            s.state = PL_W4_CCC;
            request_next_query();
            break;

        case PL_W4_CCC:
            if (att != ATT_ERROR_SUCCESS) { fail("ccc-write", att); break; }
            printf("[pulsar] subscribed to storage-up\n");
            send_status_request();
            break;

        default:
            break;
        }
        break;
    }

    case GATT_EVENT_CAN_WRITE_WITHOUT_RESPONSE:
        if (s.state == PL_W4_CAN_WRITE) send_status_request();
        break;

    default:
        break;
    }
}

// Runs when gatt_client grants us the connection: issue exactly one query for the state.
static void send_next_query(void *context)
{
    (void)context;
    uint8_t st;
    switch (s.state) {
    case PL_W4_SERVICE:
        st = gatt_client_discover_primary_services_by_uuid128(gatt_handler, s.con, PULSAR_SVC_UUID);
        if (st != ERROR_CODE_SUCCESS) fail("service-query", st);
        break;
    case PL_W4_CHARS:
        st = gatt_client_discover_characteristics_for_service(gatt_handler, s.con, &s.svc);
        if (st != ERROR_CODE_SUCCESS) fail("char-query", st);
        break;
    case PL_W4_CCC:
        // Nothing is notified, and writes are dropped in silence, on an unencrypted link.
        if (gap_encryption_key_size(s.con) == 0) { fail("link-not-encrypted", 0); break; }
        gatt_client_listen_for_characteristic_value_updates(&s.listener, notify_handler, s.con, &s.chr_up);
        st = gatt_client_write_client_characteristic_configuration(
            gatt_handler, s.con, &s.chr_up, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
        if (st != ERROR_CODE_SUCCESS) fail("ccc-query", st);
        break;
    default:
        break;
    }
}

static void request_next_query(void)
{
    s.query.callback = send_next_query;
    s.query.context = NULL;
    uint8_t st = gatt_client_request_to_send_gatt_query(&s.query, s.con);
    if (st != ERROR_CODE_SUCCESS) fail("query-request", st);
}

void pulsar_link_attach(uni_hid_device_t *d)
{
    if (d->conn.protocol != UNI_BT_CONN_PROTOCOL_BLE) return;
    if (s.state != PL_IDLE) {
        printf("[pulsar] link busy (state %d, handle 0x%04x); ignoring handle 0x%04x\n",
               s.state, s.con, d->conn.handle);
        return;
    }
    memset(&s, 0, sizeof s);
    s.con = d->conn.handle;
    s.state = PL_W4_SERVICE;
    printf("[pulsar] attach: handle 0x%04x vid=%04x pid=%04x, discovering HostService\n",
           s.con, d->vendor_id, d->product_id);
    request_next_query();
}

void pulsar_link_detach(uni_hid_device_t *d)
{
    if (s.state == PL_IDLE) return;
    if (d->conn.protocol == UNI_BT_CONN_PROTOCOL_BLE && d->conn.handle != s.con) return;
    gatt_client_stop_listening_for_characteristic_value_updates(&s.listener);
    (void)gatt_client_remove_gatt_query(&s.query, s.con);   // context may already be gone
    printf("[pulsar] detach: handle 0x%04x (state %d, %lu STATUS, %lu other)\n",
           s.con, s.state, (unsigned long)s.status_count, (unsigned long)s.other_count);
    memset(&s, 0, sizeof s);
}

const char *pulsar_link_heartbeat(void)
{
    static char buf[64];
    switch (s.state) {
    case PL_IDLE:       return "none";
    case PL_NOT_PULSAR: return "not-pulsar";
    case PL_FAILED:
        snprintf(buf, sizeof buf, "fail:%s/0x%02x", s.fail_where, s.fail_code);
        return buf;
    case PL_READY:
        snprintf(buf, sizeof buf, "ready mtu=%u st=f:%02x q:%u ep:%lu card:%08lx n=%lu",
                 s.mtu, s.flags, s.queue_free, (unsigned long)s.epoch,
                 (unsigned long)s.card, (unsigned long)s.status_count);
        return buf;
    default:
        snprintf(buf, sizeof buf, "discover:%d", s.state);
        return buf;
    }
}

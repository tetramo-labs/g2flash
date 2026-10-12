/* SID-0xf0 message transport on the host: two simulated lenses (left is the
 * BLE ingress, right hangs off the frame bridge), a host zlib inflater and
 * stubbed stock entry points. Covers revision 41's subset-lens records: a
 * both-lens stream whose records each address one lens draws exactly one ACK
 * per lens, at the record's ordinal, and a record outside the stream's lens
 * set still NACKs.
 *
 *   cc -std=c11 -O1 -Wall -Ipatches -o /tmp/transport_host_test patches/host/transport_host_test.c -lz
 */
#include <stdint.h>
#include "utils.c"
#include "memory.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
#include "cfw_context.h"

/* ---- two lenses ------------------------------------------------------------ */
static customCfwContext g_ctx[2];               /* index = lens bit - 1 */
static uint8_t g_here = CFW_MESSAGE_LEFT;       /* the lens whose code is running */
static customCfwContext *peekCustomCfwContext(void) { return &g_ctx[g_here - 1]; }
static customCfwContext *getCustomCfwContext(void) { return &g_ctx[g_here - 1]; }
static uint32_t stub_lens_side(void) { return g_here == CFW_MESSAGE_RIGHT ? 1 : 2; }

static uint32_t stub_stock_receive(uint8_t pipe, const uint8_t *p, uint16_t n) { (void)pipe; (void)p; (void)n; return 77; }
static uint32_t stub_stock_bridge(uint32_t id, const uint8_t *p, uint32_t n, uint16_t e) { (void)id; (void)p; (void)n; (void)e; return 78; }

/* Bridge envelopes queued toward the other lens, delivered by the test. */
typedef struct { uint8_t body[300]; uint16_t len; uint8_t from; } envelope;
static envelope g_bridge[32];
static unsigned g_bridge_n;
static int stub_bridge_send(uint16_t sid, const uint8_t *body, uint16_t len, void *attr) {
    (void)attr;
    assert(sid == CFW_MESSAGE_SID && g_bridge_n < 32 && len <= 300);
    memcpy(g_bridge[g_bridge_n].body, body, len);
    g_bridge[g_bridge_n].len = len;
    g_bridge[g_bridge_n].from = g_here;
    g_bridge_n++;
    return 0;
}
/* Notifications the phone sees (only the left lens, the BLE ingress, sends). */
typedef struct { uint8_t body[300]; uint16_t len; } notification;
static notification g_ble[32];
static unsigned g_ble_n;
static int stub_ble_send(uint8_t type, uint8_t sid, const uint8_t *body, uint16_t len) {
    assert(type == 1 && sid == CFW_MESSAGE_SID && g_here == CFW_MESSAGE_LEFT && g_ble_n < 32 && len <= 300);
    memcpy(g_ble[g_ble_n].body, body, len);
    g_ble[g_ble_n].len = len;
    g_ble_n++;
    return 0;
}
#define CFW_STOCK_RECEIVE stub_stock_receive
#define CFW_STOCK_BRIDGE_RECEIVE stub_stock_bridge
#define CFW_LENS_SIDE stub_lens_side
#define CFW_BRIDGE_SEND stub_bridge_send
#define CFW_BLE_SEND stub_ble_send

static cfw_message_stream *stub_stream_state(uint8_t origin) { return &g_ctx[g_here - 1].message_streams[origin - 1]; }
static int g_live;
static void *stub_malloc(uint32_t size) { void *p = malloc(size); if (p) g_live++; return p; }
static void stub_free(void *p) { if (p) g_live--; free(p); }
#define CFW_STREAM_STATE stub_stream_state
#define CFW_MESSAGE_MALLOC stub_malloc
#define CFW_MESSAGE_FREE stub_free

#define CFW_ZSTREAM z_stream
#define CFW_ZINIT(s) inflateInit2((z_stream *)(s), 15)
#define CFW_ZINFLATE(s) inflate((z_stream *)(s), Z_NO_FLUSH)
#define CFW_ZEND(s) inflateEnd((z_stream *)(s))

/* What each lens's handler saw, in order. */
typedef struct { uint8_t body[64]; uint16_t len; } handled;
static handled g_handled[2][16];
static unsigned g_handled_n[2];
static int g_handler_result;
int cfw_message_received(const uint8_t *data, uint16_t size, uint16_t checksum) {
    (void)checksum;
    unsigned lens = g_here - 1;
    assert(g_handled_n[lens] < 16 && size <= 64);
    memcpy(g_handled[lens][g_handled_n[lens]].body, data, size);
    g_handled[lens][g_handled_n[lens]].len = size;
    g_handled_n[lens]++;
    return g_handler_result;
}

#include "message_transport.c"

/* ---- phone side ------------------------------------------------------------ */
static uint8_t g_stream[2048];
static unsigned g_stream_len;
static z_stream g_deflate;
static int g_deflate_ready;
static void phone_reset_context(void) {
    if (g_deflate_ready) deflateEnd(&g_deflate);
    g_deflate_ready = 0;
}
/* One record: [flags][len LE16][crc LE16][body]; compressed bodies end in a SYNC_FLUSH. */
static void record(uint8_t lenses, int compressed, int reset_context, const uint8_t *msg, uint16_t len) {
    uint8_t body[1024];
    uint32_t n = len;
    uint8_t flags = lenses;
    if (compressed) {
        if (reset_context) phone_reset_context();
        if (!g_deflate_ready) {
            bzero((uint8_t *)&g_deflate, sizeof g_deflate);
            assert(deflateInit(&g_deflate, Z_DEFAULT_COMPRESSION) == Z_OK);
            g_deflate_ready = 1;
        }
        g_deflate.next_in = (Bytef *)msg;
        g_deflate.avail_in = len;
        g_deflate.next_out = body;
        g_deflate.avail_out = sizeof body;
        assert(deflate(&g_deflate, Z_SYNC_FLUSH) == Z_OK && g_deflate.avail_in == 0);
        n = sizeof body - g_deflate.avail_out;
        flags |= CFW_MESSAGE_COMPRESSED;
    } else {
        memcpy(body, msg, len);
    }
    if (reset_context) flags |= CFW_MESSAGE_RESET_CONTEXT;
    uint16_t crc = cfw_message_crc(msg, len);
    uint8_t *out = g_stream + g_stream_len;
    out[0] = flags; out[1] = (uint8_t)n; out[2] = (uint8_t)(n >> 8);
    out[3] = (uint8_t)crc; out[4] = (uint8_t)(crc >> 8);
    memcpy(out + 5, body, n);
    g_stream_len += 5 + n;
}
/* The stream as SID-0xf0 packets of `chunk` stream bytes, delivered to the
 * left lens, with every bridge envelope relayed until both lenses are idle. */
static void deliver_bridge(void) {
    while (g_bridge_n) {
        envelope e = g_bridge[0];
        for (unsigned i = 1; i < g_bridge_n; i++) g_bridge[i - 1] = g_bridge[i];
        g_bridge_n--;
        g_here = e.from ^ CFW_MESSAGE_BOTH;
        uint32_t status = cfw_message_bridge_received(CFW_MESSAGE_SID, e.body, e.len, 0);
        assert(status == 0);
        g_here = CFW_MESSAGE_LEFT;
    }
}
static uint32_t g_last_status;
static void send_stream(uint8_t options, uint8_t stream_id, unsigned chunk) {
    unsigned offset = 0;
    uint8_t seq = stream_id;
    g_last_status = 0;
    do {
        unsigned end = offset + chunk < g_stream_len ? offset + chunk : g_stream_len;
        uint8_t packet[300];
        uint8_t flags = options;
        if (offset == 0) flags |= CFW_MESSAGE_RESET;
        if (end >= g_stream_len) flags |= CFW_MESSAGE_END;
        uint16_t body = (uint16_t)(1 + end - offset);
        packet[0] = 0xaa; packet[1] = 0x21; packet[2] = seq++; packet[3] = (uint8_t)(body + 2);
        packet[4] = 1; packet[5] = 1; packet[6] = CFW_MESSAGE_SID; packet[7] = 0;
        packet[8] = flags;
        memcpy(packet + 9, g_stream + offset, end - offset);
        uint16_t crc = cfw_message_crc(packet + 8, body);
        packet[9 + end - offset] = (uint8_t)crc;
        packet[10 + end - offset] = (uint8_t)(crc >> 8);
        g_here = CFW_MESSAGE_LEFT;
        uint32_t status = cfw_receive_packet(0, packet, (uint16_t)(body + 10));
        if (status) g_last_status = status;
        deliver_bridge();
        offset = end;
    } while (offset < g_stream_len);
    g_stream_len = 0;
}

/* ---- assertions ------------------------------------------------------------ */
static void reset(void) {
    for (unsigned l = 0; l < 2; l++) {
        g_here = (uint8_t)(l + 1);
        for (unsigned o = 0; o < 2; o++) {
            cfw_message_stream *s = &g_ctx[l].message_streams[o];
            cfw_inflate_reset(s);
            cfw_message_discard(s);
        }
        bzero((uint8_t *)&g_ctx[l], sizeof g_ctx[l]);
        g_handled_n[l] = 0;
    }
    g_here = CFW_MESSAGE_LEFT;
    g_ble_n = g_bridge_n = 0;
    g_handler_result = 0;
    phone_reset_context();
    g_stream_len = 0;
}
/* Index of the one reply matching, or -1. Reply order depends on packet
 * splits (the left lens answers its own records before the bridge delivers
 * the right lens's), so the assertions below look replies up. */
static int ack(uint8_t kind, uint8_t stream, uint16_t ordinal, uint8_t lens) {
    int found = -1;
    for (unsigned i = 0; i < g_ble_n; i++) {
        const uint8_t *b = g_ble[i].body;
        if (b[0] == kind && b[1] == stream && rd16(b + 2) == ordinal && b[4] == lens) {
            if (found >= 0) return -1;
            found = (int)i;
        }
    }
    return found;
}
static int saw(uint8_t lens, unsigned i, const uint8_t *msg, uint16_t len) {
    const handled *h = &g_handled[lens - 1][i];
    if (i >= g_handled_n[lens - 1] || h->len != len) return 0;
    for (unsigned k = 0; k < len; k++) if (h->body[k] != msg[k]) return 0;
    return 1;
}
static int clean(void) {
    for (unsigned l = 0; l < 2; l++)
        for (unsigned o = 0; o < 2; o++)
            if (g_ctx[l].message_streams[o].buffer || g_ctx[l].message_streams[o].length_bytes) return 0;
    return 1;
}

static const uint8_t LEFT_MSG[] = {38, 1, 2, 3, 4, 'L'};
static const uint8_t RIGHT_MSG[] = {38, 1, 2, 3, 4, 'R'};
static const uint8_t BOTH_MSG[] = {38, 1, 2, 3, 4, 'B', 'B'};

/* A both-lens stream of [left-only, right-only]: one ACK per lens, ordinals 0
 * and 1. The phone frames every exclusive record, and the record after it,
 * with RESET_CONTEXT (G2CfwTransport.Encoder), so the lens a record addresses
 * always sees a reset even when it skipped the stream's earlier records. */
static void subset_records(unsigned chunk) {
    reset();
    record(CFW_MESSAGE_LEFT, 0, 1, LEFT_MSG, sizeof LEFT_MSG);
    record(CFW_MESSAGE_RIGHT, 0, 1, RIGHT_MSG, sizeof RIGHT_MSG);
    send_stream(CFW_MESSAGE_BOTH, 0x10, chunk);
    assert(g_last_status == 0 && clean());
    assert(g_handled_n[0] == 1 && saw(CFW_MESSAGE_LEFT, 0, LEFT_MSG, sizeof LEFT_MSG));
    assert(g_handled_n[1] == 1 && saw(CFW_MESSAGE_RIGHT, 0, RIGHT_MSG, sizeof RIGHT_MSG));
    assert(g_ble_n == 2);
    assert(ack(CFW_MESSAGE_ACK, 0x10, 0, CFW_MESSAGE_LEFT) >= 0);
    assert(ack(CFW_MESSAGE_ACK, 0x10, 1, CFW_MESSAGE_RIGHT) >= 0);
    assert(g_ctx[0].nack_count == 0 && g_ctx[1].nack_count == 0);

    /* The ordinal keeps counting across skipped records: a both-lens record
     * after the pair is ordinal 2 on both lenses, in the same stream. */
    record(CFW_MESSAGE_RIGHT, 0, 1, RIGHT_MSG, sizeof RIGHT_MSG);
    record(CFW_MESSAGE_LEFT, 0, 1, LEFT_MSG, sizeof LEFT_MSG);
    record(CFW_MESSAGE_BOTH, 0, 1, BOTH_MSG, sizeof BOTH_MSG);
    g_ble_n = 0;
    send_stream(CFW_MESSAGE_BOTH, 0x20, chunk);
    assert(g_last_status == 0 && clean());
    assert(g_ble_n == 4);
    assert(ack(CFW_MESSAGE_ACK, 0x20, 1, CFW_MESSAGE_LEFT) >= 0);
    assert(ack(CFW_MESSAGE_ACK, 0x20, 0, CFW_MESSAGE_RIGHT) >= 0);
    assert(ack(CFW_MESSAGE_ACK, 0x20, 2, CFW_MESSAGE_LEFT) >= 0);
    assert(ack(CFW_MESSAGE_ACK, 0x20, 2, CFW_MESSAGE_RIGHT) >= 0);
    assert(g_handled_n[0] == 3 && g_handled_n[1] == 3);
    assert(saw(CFW_MESSAGE_LEFT, 1, LEFT_MSG, sizeof LEFT_MSG) && saw(CFW_MESSAGE_RIGHT, 1, RIGHT_MSG, sizeof RIGHT_MSG));
    assert(saw(CFW_MESSAGE_LEFT, 2, BOTH_MSG, sizeof BOTH_MSG) && saw(CFW_MESSAGE_RIGHT, 2, BOTH_MSG, sizeof BOTH_MSG));
    /* An ACK history never names a record its lens skipped. */
    for (unsigned i = 0; i < g_ble_n; i++) {
        const uint8_t *b = g_ble[i].body;
        uint16_t skipped = b[4] == CFW_MESSAGE_LEFT ? 0 : 1;
        for (unsigned e = CFW_ACK_SIZE; e + CFW_ACK_ENTRY_SIZE <= g_ble[i].len; e += CFW_ACK_ENTRY_SIZE)
            assert(!(b[e] == 0x20 && rd16(b + e + 1) == skipped));
    }
}

/* A record addressed to a lens outside the stream's options still NACKs, as does an empty lens set. */
static void outside_records(void) {
    reset();
    record(CFW_MESSAGE_RIGHT, 0, 1, RIGHT_MSG, sizeof RIGHT_MSG);
    send_stream(CFW_MESSAGE_LEFT, 0x30, 64);
    assert(clean() && g_handled_n[0] == 0 && g_handled_n[1] == 0);
    assert(g_ble_n == 1 && ack(CFW_MESSAGE_NACK, 0x30, 0, CFW_MESSAGE_LEFT) >= 0);
    assert(g_ctx[0].nack_count == 1 && g_ctx[0].nack_reason == CFW_NACK_FLAGS);

    reset();
    record(0, 0, 1, BOTH_MSG, sizeof BOTH_MSG);
    send_stream(CFW_MESSAGE_BOTH, 0x31, 64);
    assert(clean() && g_handled_n[0] == 0 && g_handled_n[1] == 0);
    assert(g_ble_n == 2);
    assert(ack(CFW_MESSAGE_NACK, 0x31, 0, CFW_MESSAGE_LEFT) >= 0 && ack(CFW_MESSAGE_NACK, 0x31, 0, CFW_MESSAGE_RIGHT) >= 0);
    assert(g_ctx[0].nack_reason == CFW_NACK_FLAGS && g_ctx[1].nack_reason == CFW_NACK_FLAGS);

    /* A left-only stream to a left-only record: unchanged. */
    reset();
    record(CFW_MESSAGE_LEFT, 0, 1, LEFT_MSG, sizeof LEFT_MSG);
    send_stream(CFW_MESSAGE_LEFT, 0x32, 64);
    assert(g_ble_n == 1 && ack(CFW_MESSAGE_ACK, 0x32, 0, CFW_MESSAGE_LEFT) >= 0 && g_handled_n[0] == 1 && g_handled_n[1] == 0);
}

/* Compressed exclusive records as the phone frames them (fresh context each),
 * then a shared context that the skipping lens never touched. */
static void compressed_records(unsigned chunk) {
    reset();
    record(CFW_MESSAGE_LEFT, 1, 1, LEFT_MSG, sizeof LEFT_MSG);
    record(CFW_MESSAGE_RIGHT, 1, 1, RIGHT_MSG, sizeof RIGHT_MSG);
    record(CFW_MESSAGE_BOTH, 1, 1, BOTH_MSG, sizeof BOTH_MSG);
    send_stream(CFW_MESSAGE_BOTH, 0x40, chunk);
    assert(g_last_status == 0 && clean() && g_ble_n == 4);
    assert(ack(CFW_MESSAGE_ACK, 0x40, 0, CFW_MESSAGE_LEFT) >= 0 && ack(CFW_MESSAGE_ACK, 0x40, 1, CFW_MESSAGE_RIGHT) >= 0);
    assert(ack(CFW_MESSAGE_ACK, 0x40, 2, CFW_MESSAGE_LEFT) >= 0 && ack(CFW_MESSAGE_ACK, 0x40, 2, CFW_MESSAGE_RIGHT) >= 0);
    assert(saw(CFW_MESSAGE_LEFT, 0, LEFT_MSG, sizeof LEFT_MSG) && saw(CFW_MESSAGE_LEFT, 1, BOTH_MSG, sizeof BOTH_MSG));
    assert(saw(CFW_MESSAGE_RIGHT, 0, RIGHT_MSG, sizeof RIGHT_MSG) && saw(CFW_MESSAGE_RIGHT, 1, BOTH_MSG, sizeof BOTH_MSG));

    /* A plain right-only record between two records of one shared context:
     * the left lens skips it and continues the context it was in. */
    g_ble_n = 0;
    record(CFW_MESSAGE_BOTH, 1, 0, BOTH_MSG, sizeof BOTH_MSG);
    record(CFW_MESSAGE_RIGHT, 0, 0, RIGHT_MSG, sizeof RIGHT_MSG);
    record(CFW_MESSAGE_BOTH, 1, 0, BOTH_MSG, sizeof BOTH_MSG);
    send_stream(CFW_MESSAGE_BOTH, 0x50, chunk);
    assert(g_last_status == 0 && clean() && g_ble_n == 5);
    assert(ack(CFW_MESSAGE_ACK, 0x50, 0, CFW_MESSAGE_LEFT) >= 0 && ack(CFW_MESSAGE_ACK, 0x50, 0, CFW_MESSAGE_RIGHT) >= 0);
    assert(ack(CFW_MESSAGE_ACK, 0x50, 1, CFW_MESSAGE_RIGHT) >= 0);
    assert(ack(CFW_MESSAGE_ACK, 0x50, 2, CFW_MESSAGE_LEFT) >= 0 && ack(CFW_MESSAGE_ACK, 0x50, 2, CFW_MESSAGE_RIGHT) >= 0);
    assert(g_handled_n[0] == 4 && g_handled_n[1] == 5);
    assert(saw(CFW_MESSAGE_LEFT, 3, BOTH_MSG, sizeof BOTH_MSG) && saw(CFW_MESSAGE_RIGHT, 4, BOTH_MSG, sizeof BOTH_MSG));
    assert(g_ctx[0].nack_count == 0 && g_ctx[1].nack_count == 0);
}

/* A skipped record leaves the lens's context alone: a fresh stream whose
 * first record the right lens skips still needs the right lens's own first
 * record to carry RESET_CONTEXT (the phone always does). */
static void skipped_reset_is_not_borrowed(void) {
    reset();
    record(CFW_MESSAGE_LEFT, 0, 1, LEFT_MSG, sizeof LEFT_MSG);
    record(CFW_MESSAGE_RIGHT, 0, 0, RIGHT_MSG, sizeof RIGHT_MSG);
    send_stream(CFW_MESSAGE_BOTH, 0x70, 64);
    assert(clean() && g_ble_n == 2 && g_handled_n[0] == 1 && g_handled_n[1] == 0);
    assert(ack(CFW_MESSAGE_ACK, 0x70, 0, CFW_MESSAGE_LEFT) >= 0 && ack(CFW_MESSAGE_NACK, 0x70, 1, CFW_MESSAGE_RIGHT) >= 0);
    assert(g_ctx[0].nack_count == 0 && g_ctx[1].nack_reason == CFW_NACK_CONTEXT);
}

/* A handler failure on the addressed lens NACKs only there; the other lens's record is unaffected. */
static void handler_failure(void) {
    reset();
    record(CFW_MESSAGE_LEFT, 0, 1, LEFT_MSG, sizeof LEFT_MSG);
    record(CFW_MESSAGE_RIGHT, 0, 1, RIGHT_MSG, sizeof RIGHT_MSG);
    g_handler_result = -1;
    send_stream(CFW_MESSAGE_BOTH, 0x60, 64);
    assert(clean() && g_ble_n == 2);
    assert(ack(CFW_MESSAGE_NACK, 0x60, 0, CFW_MESSAGE_LEFT) >= 0 && ack(CFW_MESSAGE_NACK, 0x60, 1, CFW_MESSAGE_RIGHT) >= 0);
    assert(g_ctx[0].nack_reason == CFW_NACK_HANDLER && g_ctx[1].nack_reason == CFW_NACK_HANDLER);
}

int main(void) {
    for (unsigned chunk = 3; chunk <= 64; chunk += 7) {
        subset_records(chunk);
        compressed_records(chunk);
    }
    subset_records(252);
    compressed_records(252);
    outside_records();
    skipped_reset_is_not_borrowed();
    handler_failure();
    reset();
    assert(g_live == 0);
    puts("PASS: subset-lens records ACK once per lens at their ordinals; outside-lens records NACK");
    return 0;
}

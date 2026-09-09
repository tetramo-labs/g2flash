/* Merge regressions: packet routing, settings size, ring cache and compass hooks.
 * run_vector_tests.py supplies the actual settings/gate functions in an include. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cfw_context.h"
#include "protobuf.c"

static customCfwContext ctx;
static int have_ctx = 1, side = 1;
static customCfwContext *peekCustomCfwContext(void) { return have_ctx ? &ctx : 0; }
static unsigned char sent[4][256];
static unsigned sent_len[4];
static int sent_sid[4], sends, send_result;
static int send_packet(int type, int sid, unsigned char *buf, unsigned len) {
    assert(type == 1 && sends < 4 && len <= sizeof(sent[0]));
    memcpy(sent[sends], buf, len); sent_len[sends] = len; sent_sid[sends++] = sid;
    return send_result;
}
static const uint8_t *field(const uint8_t *buf, unsigned len, unsigned number, unsigned *size) {
    for (unsigned i = 0; i < len;) {
        unsigned tag = 0, value = 0, shift = 0;
        uint8_t b;
        do { assert(i < len); b = buf[i++]; tag |= (b & 127u) << shift; shift += 7; } while (b & 128);
        shift = 0;
        do { assert(i < len); b = buf[i++]; value |= (b & 127u) << shift; shift += 7; } while (b & 128);
        if ((tag & 7) == 2) {
            assert(value <= len - i);
            if ((tag >> 3) == number) { *size = value; return buf + i; }
            i += value;
        } else assert((tag & 7) == 0);
    }
    return 0;
}
static uint16_t ring_cache;
static int ring_connected;
#define RING_BATTERY_CACHE ring_cache
#define RING_BATTERY_CONNECTED() ring_connected
#define RING_BATTERY_SIDE() side
#define RING_BATTERY_SEND(buf, len) send_packet(1, 9, buf, len)
#include "ring_battery.c"

typedef int (*send_fn)(int, int, unsigned char *, unsigned);
#define FW_SEND send_packet
#define SETTINGS_RESPONSE_CAPACITY 256u
static unsigned mic_append_status(unsigned char *buf, unsigned len, unsigned capacity) {
    const unsigned char mic[21] = {'M', 'C', 1};
    return pb_append_bytes_field(buf, len, capacity, 104, mic, sizeof(mic));
}
static int ancs_controls;
static void faceclaw_apply_control(const uint8_t *p, uint32_t n) { (void)p; (void)n; }
static void mic_apply_control(const uint8_t *p, uint32_t n) { (void)p; (void)n; }
static void ancs_apply_control(const uint8_t *p, uint32_t n) {
    assert(n == 4 && p[0] == 'A' && p[1] == 'N' && p[2] == 1 && p[3] == 3);
    ancs_controls++;
}
#include "upstream_functions.inc"

#define COMPASS_HOST_TEST 1
static _Alignas(4) uint8_t compass_ring[12 + 20 * 0x70];
static int decode_result, stock_events;
static int decode(void *d, const void *a, const void *b, uint8_t *g) {
    (void)d; (void)a; (void)b; (void)g; return decode_result;
}
static int stock_event(uint32_t e, int32_t h) { (void)e; (void)h; stock_events++; return 42; }
static int compass_side(void) { return side; }
#define COMPASS_DECODE decode
#define COMPASS_STOCK_EVENT stock_event
#define COMPASS_SEND send_packet
#define COMPASS_SIDE compass_side
#define COMPASS_RING compass_ring
#define COMPASS_MAG_SEEN 0
#define COMPASS_ACCURACY 2
#define COMPASS_ANOMALIES 1
#include "compass.c"

static void routing(void) {
    uint8_t p[64] = {0};
    assert(!is_shadow_message(0, 2) && !is_shadow_message(p, 0));
    /* Upstream sensor modes never become graphics, regardless of length. */
    for (unsigned mode = 16; mode <= 19; mode++) {
        p[0] = mode;
        for (unsigned len = 1; len <= sizeof(p); len++) assert(!is_shadow_message(p, len));
    }
    const uint8_t modes[] = {3,6,8,9,11,13,14,15,36,37,38};
    for (unsigned i = 0; i < sizeof(modes); i++) {
        p[0] = modes[i]; assert(is_shadow_message(p, 3));
        p[0] |= 0x80; assert(is_shadow_message(p, 3));
    }
    const uint8_t query[] = {0xf2, 0x07, 4, 'A', 'N', 1, 3};
    const uint8_t old_query[] = {0xd2, 0x06, 4, 'A', 'N', 1, 3};
    faceclaw_scan_settings_control(query, sizeof(query));
    assert(ancs_controls == 1);
    faceclaw_scan_settings_control(old_query, sizeof(old_query));
    assert(ancs_controls == 1); /* upstream field 106 is never ANCS control */
}
static void batteries(void) {
    const uint8_t query[] = {17,0}, scene[] = {37,0,0};
    unsigned size;
    for (int connected = 0; connected <= 1; connected++) {
        ring_connected = connected;
        for (int charging = 0; charging <= 1; charging++) {
            for (int level = 0; level <= 255; level++) {
                ring_cache = level | charging << 8; sends = 0;
                assert(ring_battery_control(query, 2) == 0 && sends == 1);
                const uint8_t *b = field(sent[0], sent_len[0], 106, &size);
                assert(b && size == 5 && b[0] == 'R' && b[1] == 'B' && b[2] == 1);
                int valid = connected && level <= 100;
                assert(b[3] == (connected | (valid ? 2 : 0) | (valid && charging ? 4 : 0)));
                assert(b[4] == (valid ? level : 255));
            }
        }
    }
    sends = 0; side = 0;
    assert(ring_battery_control(query, 2) == 0 && sends == 0);
    assert(ring_battery_control(scene, 3) == -1 && sends == 0);
    side = 1;
}
static void settings(void) {
    uint8_t buf[256] = {8,2,16,1,26,38}; /* 44-byte stock response */
    unsigned n;
    sends = 0;
    assert(settings_send_wrapper(1,9,buf,44) == 0 && sends == 2);
    const uint8_t *caps = field(sent[0],sent_len[0],100,&n);
    assert(caps && n == 151 && memcmp(caps,"EVENCFW/24 ",11) == 0);
    assert(sent_len[0] == 44 + 155 + 24); /* exactly the pre-merge reply size */
    assert(field(sent[0],sent_len[0],104,&n) && n == 21);
    assert(!field(sent[0],sent_len[0],106,&n));
    assert(field(sent[1],sent_len[1],106,&n) && n == 5 && sent_len[1] == 12);
    sends = 0; send_result = -1;
    assert(settings_send_wrapper(1,9,buf,44) == -1 && sends == 1);
    send_result = 0; sends = 0;
    assert(settings_send_wrapper(1,8,buf,4) == 0 && sends == 1 && sent_len[0] == 4);
}
static void headings(void) {
    uint8_t gaf[0x50] = {0};
    ctx.compass_forward = 1;
    gaf[0x4d] = gaf[8] = gaf[18] = gaf[22] = gaf[32] = gaf[36] = gaf[0x46] = 1;
    gaf[0x44] = 3; gaf[0x45] = 2;
    *(uint32_t *)(compass_ring + 12) = 1234;
    assert(compass_decode_capture(0,0,0,gaf) == 0);
    assert(ctx.compass_samples[0].source == 3 && ctx.compass_samples[0].flags == 0x8f);
    *(uint32_t *)(compass_ring + 8) = 1; compass_ring[16] = 0x20;
    for (int heading = 0; heading < 360; heading++) {
        sends = 0; unsigned n;
        assert(compass_report_event(9,heading) == 42 && sends == 1 && sent_sid[0] == 8);
        const uint8_t *b = field(sent[0],sent_len[0],100,&n);
        assert(b && n == 12 && b[3] == 3 && b[4] == 2 && b[5] == 3 && b[6] == 0x8f);
        assert(b[8] == (1234 & 255) && b[9] == (1234 >> 8));
        b = field(sent[0],sent_len[0],10,&n);
        assert(b && b[0] == 8 && ((b[1] & 127) | (n == 3 ? b[2] << 7 : 0)) == heading);
    }
    ctx.compass_samples[0].timestamp++; sends = 0; unsigned n;
    compass_report_event(9,123);
    const uint8_t *b = field(sent[0],sent_len[0],100,&n);
    assert(b && b[3] == 255 && b[5] == 0 && b[6] == 0);
    sends = 0; int before = stock_events;
    compass_report_event(8,0); compass_report_event(9,-1); compass_report_event(9,360);
    side = 0; compass_report_event(9,0); side = 1;
    ctx.compass_forward = 0; compass_report_event(9,0);
    have_ctx = 0; compass_report_event(9,0); have_ctx = 1;
    assert(sends == 0 && stock_events == before + 6);
}
int main(void) {
    routing(); batteries(); settings(); headings();
    puts("PASS: merged mode routing, bounded settings replies, ring battery and compass diagnostics");
    return 0;
}

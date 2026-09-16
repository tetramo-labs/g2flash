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

/* BLE link speed: stock globals and calls become counters. */
#define BLE_LINK_HOST_TEST 1
static uint8_t stock_fast_profile[16] = {0,0,0,0, 0x0c,0, 0x18,0, 0,0, 0x58,2, 5,0, 0,0};
static uint8_t stock_slow_profile[16] = {0,0,0,0, 0x24,0, 0x48,0, 4,0, 0x58,2, 5,0, 0,0};
static const uint8_t *ble_profile_slot;
static uint8_t ble_applied, ble_wanted;
static unsigned ble_cancels, ble_posts, ble_post_fn, ble_post_arg, ble_post_ms, ble_sends, ble_sent_mode;
static void *ble_sent_conn;
static unsigned ble_classifies;
static uint8_t ble_conn[0x20];          /* live connection record: +0x18 interval, +0x1a latency */
static const uint8_t *ble_conn_ptr;
static uint32_t ble_stock_classify(const void *conn) {
    assert(conn == ble_conn);
    ble_classifies++;
    return 0xa3;
}
#define BLE_PROFILE_SLOT ble_profile_slot
#define BLE_STOCK_FAST_PROFILE ((const uint8_t *)stock_fast_profile)
#define BLE_APPLIED_MODE ble_applied
#define BLE_WANTED_MODE ble_wanted
#define BLE_SET_WANTED(m) ((void)(ble_wanted = (uint8_t)(m)))
#define BLE_DEFER_CANCEL(fn) ((void)(ble_cancels++, assert((fn) == BLE_REQUEST_CB)))
#define BLE_DEFER_POST(fn, arg, ms) ((void)(ble_posts++, ble_post_fn = (fn), ble_post_arg = (arg), ble_post_ms = (ms)))
#define BLE_REQUEST_CB 0x0047b475u
#define BLE_SEND_REQUEST(mode, conn) (ble_sends++, ble_sent_mode = (mode), ble_sent_conn = (conn), 0)
#define BLE_CLASSIFY(conn) ble_stock_classify(conn)
#define BLE_CONN() ble_conn_ptr
#define BLE_SIDE() ((uint32_t)side)
#define BLE_PEEK() peekCustomCfwContext()
#define BLE_CTX() peekCustomCfwContext()
#include "ble_link.c"
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
    assert(caps && n == 13 && memcmp(caps,"GLASSLYCFW/29",13) == 0);
    assert(sent_len[0] == 44 + 16 + 24 + 20); /* revision string, microphone and BLE status */
    assert(sent_len[0] + 2 <= 232); /* payload plus CRC stays in one BLE frame */
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
static const uint8_t *ble_status(void) {
    uint8_t buf[256] = {8,2,16,1,26,38};
    unsigned n;
    sends = 0;
    assert(settings_send_wrapper(1,9,buf,44) == 0);
    const uint8_t *st = field(sent[0], sent_len[0], 128, &n);
    assert(st && n == 17 && st[0] == 'B' && st[1] == 'L' && st[2] == 1);
    return st;
}
static void ble_link(void) {
    const uint8_t fast[] = {0xfa, 0x07, 4, 'B', 'L', 1, 1};
    const uint8_t stock[] = {0xfa, 0x07, 4, 'B', 'L', 1, 0};
    const uint8_t query[] = {0xfa, 0x07, 4, 'B', 'L', 1, 2};
    const uint8_t bad[] = {0xfa, 0x07, 4, 'B', 'X', 1, 1};
    const uint8_t old_version[] = {0xfa, 0x07, 4, 'B', 'L', 2, 1};
    const uint8_t unknown_op[] = {0xfa, 0x07, 4, 'B', 'L', 1, 9};
    ctx.ble_fast = 0; side = 1;
    ble_profile_slot = stock_fast_profile; ble_applied = 0xa3; ble_wanted = 0xa3;
    ble_cancels = ble_posts = ble_sends = ble_classifies = 0;
    memset(ble_conn, 0, sizeof(ble_conn));
    ble_conn[0x18] = 12; ble_conn[0x1a] = 0;    /* a 15 ms / latency-0 link, the Mac's default */
    ble_conn_ptr = ble_conn;

    /* Default: every hook is a pass-through and the reply says stock. */
    assert(ble_filter_mode(0xa4) == 0xa4 && ble_filter_mode(0xa3) == 0xa3);
    assert(ble_hook_classify(ble_conn) == 0xa3 && ble_classifies == 1);
    assert(ble_hook_request(0xa3, (void *)0x1234) == 0 && ble_sends == 1);
    assert(ble_sent_mode == 0xa3 && ble_sent_conn == (void *)0x1234 && ble_profile_slot == stock_fast_profile);
    const uint8_t *st = ble_status();
    assert(st[3] == 0 && st[4] == 1 && st[5] == 0xa3 && st[6] == 0xa3);
    assert(st[7] == 0x0c && st[8] == 0 && st[9] == 0x18 && st[10] == 0 && st[11] == 0 && st[12] == 0);
    assert(st[13] == 12 && st[14] == 0 && st[15] == 0 && st[16] == 0); /* live link */
    ble_conn_ptr = 0;
    st = ble_status();
    assert(st[13] == 0 && st[14] == 0 && st[15] == 0 && st[16] == 0); /* no connection: zeros */
    ble_conn_ptr = ble_conn;
    faceclaw_scan_settings_control(bad, sizeof(bad));
    faceclaw_scan_settings_control(old_version, sizeof(old_version));
    faceclaw_scan_settings_control(unknown_op, sizeof(unknown_op));
    faceclaw_scan_settings_control(query, sizeof(query));
    faceclaw_scan_settings_control(stock, sizeof(stock));
    assert(ctx.ble_fast == 0 && ble_posts == 0 && ble_cancels == 0 && ble_applied == 0xa3);

    /* FAST: flag set, applied mode cleared, fast request queued the stock way. */
    faceclaw_scan_settings_control(fast, sizeof(fast));
    assert(ctx.ble_fast == 1 && ble_applied == 0 && ble_wanted == 0xa3);
    assert(ble_cancels == 1 && ble_posts == 1 && ble_post_fn == 0x0047b475u && ble_post_arg == 0xa3 && ble_post_ms == 0);
    faceclaw_scan_settings_control(fast, sizeof(fast));
    assert(ble_posts == 1); /* repeated request is a no-op */
    assert(ble_filter_mode(0xa4) == 0xa3 && ble_filter_mode(0xa3) == 0xa3 && ble_filter_mode(0) == 0xa3);
    /* The stock classifier would call a 15 ms link "already fast" and send
     * nothing; in fast mode only a 7.5 ms / latency-0 link is fast. */
    assert(ble_hook_classify(ble_conn) == 0xa4 && ble_classifies == 1);
    ble_conn[0x18] = 6;
    assert(ble_hook_classify(ble_conn) == 0xa3);
    ble_conn[0x1a] = 1;
    assert(ble_hook_classify(ble_conn) == 0xa4);
    ble_conn[0x1a] = 0;
    assert(ble_hook_classify(0) == 0xa4 && ble_classifies == 1);
    ble_sends = 0;
    assert(ble_hook_request(0xa3, 0) == 0 && ble_sends == 1 && ble_sent_mode == 0xa3);
    assert(ble_profile_slot == ctx.ble_fast_profile);
    const uint8_t *p = ctx.ble_fast_profile;
    assert(p[4] == 6 && p[5] == 0 && p[6] == 6 && p[7] == 0 && p[8] == 0 && p[9] == 0);
    assert(p[10] == 0x58 && p[11] == 2 && p[12] == 5 && p[13] == 0); /* timeout and retries copied */
    ble_profile_slot = stock_slow_profile;
    assert(ble_hook_request(0xa4, 0) == 0 && ble_profile_slot == stock_slow_profile); /* mode-gated */
    ble_profile_slot = ctx.ble_fast_profile; ble_applied = 0xa3;
    st = ble_status();
    assert(st[3] == 1 && st[7] == 6 && st[8] == 0 && st[9] == 6 && st[10] == 0 && st[11] == 0 && st[12] == 0);
    assert(st[13] == 6 && st[14] == 0 && st[15] == 0 && st[16] == 0);
    ble_conn[0x18] = 12;

    /* STOCK: restore the stock pointer, re-request what the stock machine wants. */
    ble_wanted = 0xa4;
    faceclaw_scan_settings_control(stock, sizeof(stock));
    assert(ctx.ble_fast == 0 && ble_profile_slot == stock_fast_profile && ble_applied == 0);
    assert(ble_cancels == 2 && ble_posts == 2 && ble_post_arg == 0xa4 && ble_wanted == 0xa4);
    faceclaw_scan_settings_control(fast, sizeof(fast));
    assert(ble_posts == 3 && ble_post_arg == 0xa3);
    ble_wanted = 0xa3; ble_profile_slot = stock_slow_profile;
    faceclaw_scan_settings_control(stock, sizeof(stock));
    assert(ble_posts == 4 && ble_post_arg == 0xa3 && ble_profile_slot == stock_slow_profile);

    /* Mode-11 cleanup leaves the link at stock; a second cleanup does nothing. */
    faceclaw_scan_settings_control(fast, sizeof(fast));
    assert(ctx.ble_fast == 1 && ble_posts == 5);
    ble_cleanup_session();
    assert(ctx.ble_fast == 0 && ble_posts == 6 && ble_post_arg == 0xa3);
    ble_cleanup_session();
    assert(ble_posts == 6);

    /* Without a context the hooks stay stock and the reply still parses. */
    have_ctx = 0;
    assert(ble_filter_mode(0xa4) == 0xa4);
    assert(ble_hook_classify(ble_conn) == 0xa3 && ble_classifies == 2);
    ble_profile_slot = stock_fast_profile;
    assert(ble_hook_request(0xa3, 0) == 0 && ble_profile_slot == stock_fast_profile);
    ble_cleanup_session();
    st = ble_status();
    assert(st[3] == 0 && ble_posts == 6);
    have_ctx = 1;
}
int main(void) {
    routing(); batteries(); settings(); headings(); ble_link();
    puts("PASS: merged mode routing, bounded settings replies, ring battery, compass diagnostics and BLE link control");
    return 0;
}

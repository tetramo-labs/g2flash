/*
 * Host harness for ancs_relay.c: drives the four hook wrappers and the control
 * ops with fake stock structures, runs the drain timer by hand, and checks the
 * field-125 records that reach the sender.
 *
 *   cc -std=c11 -Wall -Wextra -Wno-unused-function -I patches \
 *      -o obj/ancs_relay_host_test patches/host/ancs_relay_host_test.c && obj/ancs_relay_host_test
 */
#define ANCS_HOST_TEST 1
#include <stdint.h>
#include "utils.c"          /* defines its own strnlen/strlcpy/bzero, so <string.h> stays out */
#include <stdio.h>
#include <stdlib.h>

#include "cfw_context.h"

static void h_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
static int h_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = a, *y = b;
    for (uint32_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
static void h_memset(void *dst, int v, uint32_t n) {
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)v;
}
static uint32_t h_strlen(const char *s) { return strnlen(s, 0xffffu); }

/* ---- fakes ----------------------------------------------------------------- */

static uint8_t fake_cb[0x800];            /* anccCb image: +8 active, +0x35c app-attr buffer */
static uint32_t now_ms = 1000;
static uint32_t host_side = 1;
static customCfwContext ctx_storage;

static int stock_push_calls, stock_remove_calls, stock_attr_calls, stock_parse_calls;
static int perform_calls;
static uint32_t perform_uid, perform_action;
static uint16_t *perform_hdl;

static int stock_push(uint8_t *n) { (void)n; stock_push_calls++; return 1; }
static void stock_remove(uint8_t *n) { (void)n; stock_remove_calls++; }
static void stock_attr(uint8_t *a) { (void)a; stock_attr_calls++; }
static int stock_parse(void) { stock_parse_calls++; return 1; }
static void stock_perform(uint16_t *hdl, uint32_t uid, uint32_t action) {
    perform_calls++; perform_hdl = hdl; perform_uid = uid; perform_action = action;
}

#define SENT_MAX 64
static uint8_t sent[SENT_MAX][200];
static uint32_t sent_len[SENT_MAX];
static int sent_n;
static int send_fail;
static int host_send(const uint8_t *buf, uint32_t len) {
    if (send_fail) return -1;
    if (sent_n < SENT_MAX) { h_memcpy(sent[sent_n], buf, len); sent_len[sent_n] = len; }
    sent_n++;
    return 0;
}
static void sent_reset(void) { sent_n = 0; }

static void (*timer_cb)(void *);
static void *timer_arg;
static int timer_pending, timer_start_calls, timer_stop_calls, timer_delete_calls;
static uint32_t timer_ms;
static uint32_t host_timer_new(void *cb, void *arg) { timer_cb = (void (*)(void *))cb; timer_arg = arg; return 0x1234; }
static int host_timer_start(uint32_t t, uint32_t ms) {
    (void)t; timer_pending = 1; timer_ms = ms; timer_start_calls++; return 0;
}
static int host_timer_stop(uint32_t t) { (void)t; timer_pending = 0; timer_stop_calls++; return 0; }
static int host_timer_delete(uint32_t t) { (void)t; timer_delete_calls++; return 0; }
/* Fire the pending one-shot exactly like the RTOS timer thread would. */
static int run_timer(void) {
    if (!timer_pending) return 0;
    timer_pending = 0;
    timer_cb(timer_arg);
    return 1;
}
static void drain(void) { int guard = 0; while (run_timer() && ++guard < 1000) {} }

#define FW_ANCC_LIST_PUSH stock_push
#define FW_ANCC_REMOVE_CB stock_remove
#define FW_ANCC_ATTR_CB   stock_attr
#define FW_ANCC_PARSE_APP stock_parse
#define FW_ANCC_PERFORM   stock_perform
#define ANCS_CB           ((volatile uint8_t *)fake_cb)
#define ANCS_SEND(buf, len)     host_send((buf), (len))
#define ANCS_NOW()              now_ms
#define ANCS_SIDE()             host_side
#define ANCS_CTX()              (&ctx_storage)
#define ANCS_PEEK()             (&ctx_storage)
#define ANCS_ALLOC(n)           malloc(n)
#define ANCS_TIMER_NEW(cb, arg) host_timer_new((void *)(cb), (arg))
#define ANCS_TIMER_START(t, ms) host_timer_start((t), (ms))
#define ANCS_TIMER_STOP(t)      host_timer_stop(t)
#define ANCS_TIMER_DELETE(t)    host_timer_delete(t)

#include "ancs_relay.c"

/* ---- checks ---------------------------------------------------------------- */

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const uint8_t frame_hdr[6] = { 0x08, 0x03, 0x10, 0x00, 0xEA, 0x07 };

/* Returns the field-125 payload of message i (after the 7-byte protobuf frame). */
static const uint8_t *payload(int i, uint32_t *len) {
    CHECK(h_memcmp(sent[i], frame_hdr, 6) == 0);
    CHECK(sent[i][6] == sent_len[i] - 7);
    CHECK(sent[i][7] == 'A' && sent[i][8] == 'N' && sent[i][9] == ANCS_PROTO_VERSION);
    *len = sent_len[i] - 7;
    return sent[i] + 7;
}

static void control(uint8_t op, const uint8_t *extra, uint32_t extra_len) {
    uint8_t msg[16] = { 'A', 'N', ANCS_PROTO_VERSION, op };
    if (extra_len) h_memcpy(msg + 4, extra, extra_len);
    ancs_apply_control(msg, 4 + extra_len);
}

static uint8_t *active(void) { return fake_cb + ANCC_CB_ACTIVE; }

static void set_attr(uint8_t id, uint32_t uid, const uint8_t *data, uint32_t len, uint32_t parse_index) {
    uint8_t *a = active();
    a[ANCC_ACTIVE_PARSE_IDX] = (uint8_t)parse_index;
    a[ANCC_ACTIVE_PARSE_IDX + 1] = (uint8_t)(parse_index >> 8);
    a[ANCC_ACTIVE_ATTR_LEN] = (uint8_t)len;
    a[ANCC_ACTIVE_ATTR_LEN + 1] = (uint8_t)(len >> 8);
    a[ANCC_ACTIVE_ATTR_ID] = id;
    for (int i = 0; i < 4; i++) a[ANCC_ACTIVE_UID + i] = (uint8_t)(uid >> (8 * i));
    h_memcpy(a + ANCC_ACTIVE_ATTR_BUF + parse_index, data, len);
}

static void reset_all(void) {
    h_memset(&ctx_storage, 0, sizeof ctx_storage);
    ctx_storage.magic = CFW_CTX_MAGIC;
    h_memset(fake_cb, 0, sizeof fake_cb);
    sent_reset(); send_fail = 0;
    timer_pending = 0; timer_start_calls = timer_stop_calls = timer_delete_calls = 0;
    stock_push_calls = stock_remove_calls = stock_attr_calls = stock_parse_calls = perform_calls = 0;
    host_side = 1; now_ms = 1000;
}

static void test_left_lens_and_disabled(void) {
    reset_all();
    host_side = 2;
    control(ANCS_OP_ENABLE, 0, 0);
    CHECK(ctx_storage.ancs_ring == 0 && sent_n == 0);
    host_side = 1;
    uint8_t notif[8] = { 0, 0, 4, 1, 0x78, 0x56, 0x34, 0x12 };
    CHECK(ancs_hook_source(notif) == 1);
    CHECK(stock_push_calls == 1 && ctx_storage.ancs_head == 0);
    ancs_hook_attr(active());
    CHECK(stock_attr_calls == 1 && ctx_storage.ancs_head == 0);
    CHECK(ancs_hook_app() == 1 && stock_parse_calls == 1);
}

static void test_enable_status_and_source(void) {
    reset_all();
    control(ANCS_OP_ENABLE, 0, 0);
    CHECK(ctx_storage.ancs_ring != 0 && ctx_storage.ancs_timer == 0x1234);
    CHECK(ctx_storage.ancs_lease_deadline == now_ms + ANCS_LEASE_MS);
    CHECK(ctx_storage.ancs_epoch == 1);
    CHECK(sent_n == 1);
    uint32_t n; const uint8_t *p = payload(0, &n);
    CHECK(n == 11 && p[3] == ANCS_KIND_STATUS && p[4] == 1 && p[5] == 1);
    sent_reset();

    uint8_t notif[8] = { 0, 0x04, 4, 1, 0x78, 0x56, 0x34, 0x12 };
    CHECK(ancs_hook_source(notif) == 1);
    CHECK(stock_push_calls == 1);
    CHECK(timer_pending && timer_ms == ANCS_TICK_MS && ctx_storage.ancs_idle == 0);
    drain();
    CHECK(sent_n == 1);
    p = payload(0, &n);
    CHECK(n == 12 && p[3] == ANCS_KIND_SOURCE && h_memcmp(p + 4, notif, 8) == 0);
    CHECK(ctx_storage.ancs_idle == 1 && ctx_storage.ancs_seq == 1);
    sent_reset();

    uint8_t removed[8] = { 2, 0, 4, 0, 0x78, 0x56, 0x34, 0x12 };
    ancs_hook_remove(removed);
    CHECK(stock_remove_calls == 1);
    drain();
    CHECK(sent_n == 1);
    p = payload(0, &n);
    CHECK(n == 12 && p[3] == ANCS_KIND_SOURCE && p[4] == 2);
    sent_reset();

    /* A second ENABLE while the lease is live is a renewal: same epoch. */
    now_ms += 5000;
    control(ANCS_OP_ENABLE, 0, 0);
    CHECK(ctx_storage.ancs_epoch == 1 && ctx_storage.ancs_lease_deadline == now_ms + ANCS_LEASE_MS);
    control(ANCS_OP_RENEW, 0, 0);
    control(ANCS_OP_QUERY, 0, 0);
    CHECK(sent_n == 2);
}

static void test_attr_chunking(void) {
    reset_all();
    control(ANCS_OP_ENABLE, 0, 0);
    sent_reset();
    uint8_t body[300];
    for (int i = 0; i < 300; i++) body[i] = (uint8_t)(i * 7 + 1);
    set_attr(3, 0xdeadbeef, body, 300, 37);
    ancs_hook_attr(active());
    CHECK(stock_attr_calls == 1);
    drain();
    CHECK(sent_n == 3);
    uint32_t chunk_max = ANCS_MSG_MAX - ANCS_ATTR_FIXED;
    uint32_t off = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t n; const uint8_t *p = payload(i, &n);
        CHECK(p[3] == ANCS_KIND_ATTR);
        CHECK(rd32(p + 4) == 0xdeadbeef && p[8] == 3);
        CHECK(rd16(p + 9) == 300 && rd16(p + 11) == off);
        uint32_t chunk = n - ANCS_ATTR_FIXED;
        CHECK(chunk == (300 - off < chunk_max ? 300 - off : chunk_max));
        CHECK(h_memcmp(p + ANCS_ATTR_FIXED, body + off, chunk) == 0);
        off += chunk;
    }
    CHECK(off == 300);
    sent_reset();

    /* An empty attribute still produces one (empty) record. */
    set_attr(2, 0x11, body, 0, 5);
    ancs_hook_attr(active());
    drain();
    CHECK(sent_n == 1);
    uint32_t n; const uint8_t *p = payload(0, &n);
    CHECK(n == ANCS_ATTR_FIXED && p[8] == 2 && rd16(p + 9) == 0);
    sent_reset();

    /* Out-of-range lengths are ignored rather than read past the buffer. */
    set_attr(1, 0x22, body, 10, 0x3f8);
    ancs_hook_attr(active());
    drain();
    CHECK(sent_n == 0 && stock_attr_calls == 3);
}

static void test_app_attr(void) {
    reset_all();
    control(ANCS_OP_ENABLE, 0, 0);
    sent_reset();
    const char *id = "com.apple.MobileSMS";
    const char *name = "Messages";
    uint8_t *buf = fake_cb + ANCC_CB_ATTR_BUF;
    uint32_t n = 0;
    buf[n++] = 1;
    h_memcpy(buf + n, id, h_strlen(id) + 1); n += (uint32_t)h_strlen(id) + 1;
    buf[n++] = 0;
    buf[n++] = (uint8_t)h_strlen(name); buf[n++] = 0;
    h_memcpy(buf + n, name, h_strlen(name)); n += (uint32_t)h_strlen(name);

    /* First fragment: header complete, name missing -> no relay yet. */
    uint32_t partial = n - 3;
    fake_cb[ANCC_CB_BUF_INDEX] = (uint8_t)partial; fake_cb[ANCC_CB_BUF_INDEX + 1] = (uint8_t)(partial >> 8);
    CHECK(ancs_hook_app() == 1 && stock_parse_calls == 1);
    drain();
    CHECK(sent_n == 0);

    fake_cb[ANCC_CB_BUF_INDEX] = (uint8_t)n; fake_cb[ANCC_CB_BUF_INDEX + 1] = (uint8_t)(n >> 8);
    CHECK(ancs_hook_app() == 1 && stock_parse_calls == 2);
    drain();
    CHECK(sent_n == 1);
    uint32_t len; const uint8_t *p = payload(0, &len);
    CHECK(p[3] == ANCS_KIND_APP);
    CHECK(rd16(p + 4) == h_strlen(name) && rd16(p + 6) == 0 && p[8] == h_strlen(id));
    CHECK(h_memcmp(p + 9, id, h_strlen(id)) == 0);
    CHECK(len == ANCS_APP_FIXED + h_strlen(id) + h_strlen(name));
    CHECK(h_memcmp(p + 9 + h_strlen(id), name, h_strlen(name)) == 0);
    sent_reset();

    /* Unsupported attribute id or wrong command: left to stock. */
    buf[h_strlen(id) + 2] = 5;
    CHECK(ancs_hook_app() == 1);
    drain();
    CHECK(sent_n == 0);
}

static void test_ring_full_and_pacing(void) {
    reset_all();
    control(ANCS_OP_ENABLE, 0, 0);
    sent_reset();
    uint8_t notif[8] = { 0, 0, 1, 1, 1, 0, 0, 0 };
    /* Each SOURCE record costs 14 ring bytes; overfill without draining. */
    int pushed = 0;
    for (int i = 0; i < 400; i++) {
        uint32_t before = ctx_storage.ancs_head;
        ancs_hook_source(notif);
        if (ctx_storage.ancs_head != before) pushed++;
    }
    CHECK(pushed == (int)((ANCS_RING_BYTES) / 14u));
    CHECK(ctx_storage.ancs_drops == 400 - pushed);
    /* Two per tick, re-armed at the tick period until empty. */
    int ticks = 0;
    while (run_timer()) {
        ticks++;
        if (sent_n < pushed) CHECK(timer_ms == ANCS_TICK_MS);
    }
    CHECK(sent_n == pushed);
    CHECK(ticks == (pushed + (int)ANCS_BURST - 1) / (int)ANCS_BURST);
    CHECK(ctx_storage.ancs_head == ctx_storage.ancs_tail && ctx_storage.ancs_idle == 1);
    /* Space is back: the next record is queued and delivered. */
    sent_reset();
    ancs_hook_source(notif);
    drain();
    CHECK(sent_n == 1);
}

static void test_lease_expiry_and_epoch(void) {
    reset_all();
    control(ANCS_OP_ENABLE, 0, 0);
    sent_reset();
    uint8_t notif[8] = { 0, 0, 1, 1, 9, 0, 0, 0 };
    ancs_hook_source(notif);
    now_ms += ANCS_LEASE_MS + 1;
    ancs_hook_source(notif);            /* lease dead: nothing queued */
    CHECK(ctx_storage.ancs_drops == 0);
    drain();                            /* tick discards the earlier record */
    CHECK(sent_n == 0 && ctx_storage.ancs_lease_deadline == 0);
    CHECK(ctx_storage.ancs_head == ctx_storage.ancs_tail && ctx_storage.ancs_idle == 1);

    /* Records queued, then DISABLE and a fresh ENABLE before any tick: the
     * stale epoch is skipped, the new one delivered. */
    control(ANCS_OP_ENABLE, 0, 0);
    sent_reset();
    ancs_hook_source(notif);
    control(ANCS_OP_DISABLE, 0, 0);
    CHECK(ctx_storage.ancs_lease_deadline == 0 && timer_pending);
    sent_reset();
    control(ANCS_OP_ENABLE, 0, 0);
    CHECK(ctx_storage.ancs_epoch == 3);
    sent_reset();
    notif[4] = 10;
    ancs_hook_source(notif);
    drain();
    CHECK(sent_n == 1);
    uint32_t n; const uint8_t *p = payload(0, &n);
    CHECK(p[4 + 4] == 10);
}

static void test_send_failure(void) {
    reset_all();
    control(ANCS_OP_ENABLE, 0, 0);
    sent_reset();
    uint8_t notif[8] = { 0, 0, 1, 1, 1, 0, 0, 0 };
    ancs_hook_source(notif);
    ancs_hook_source(notif);
    send_fail = 1;
    run_timer();
    CHECK(ctx_storage.ancs_send_errs == 1 && timer_pending && timer_ms == ANCS_RETRY_MS);
    send_fail = 0;
    drain();
    CHECK(sent_n == 1 && ctx_storage.ancs_seq == 1);
    control(ANCS_OP_QUERY, 0, 0);
    uint32_t n; const uint8_t *p = payload(sent_n - 1, &n);
    CHECK(p[3] == ANCS_KIND_STATUS && rd16(p + 6) == 0 && rd16(p + 8) == 1 && p[10] == 1);
}

static void test_action(void) {
    reset_all();
    static uint16_t hdl_list[5] = { 0x10, 0x11, 0x12, 0x13, 0x14 };
    uint8_t arg[5] = { 0x78, 0x56, 0x34, 0x12, 1 };
    control(ANCS_OP_ACTION, arg, 5);
    CHECK(perform_calls == 0);          /* no connection */
    fake_cb[ANCC_CB_CONN_ID] = 1;
    uint32_t bad = 0x20001001u;         /* odd pointer */
    h_memcpy(fake_cb + ANCC_CB_HDL_LIST, &bad, 4);
    control(ANCS_OP_ACTION, arg, 5);
    CHECK(perform_calls == 0);
    uint32_t fake_ptr = 0x20001000u;
    h_memcpy(fake_cb + ANCC_CB_HDL_LIST, &fake_ptr, 4);
    control(ANCS_OP_ACTION, arg, 5);
    CHECK(perform_calls == 1 && perform_uid == 0x12345678 && perform_action == 1);
    CHECK((uintptr_t)perform_hdl == fake_ptr);
    (void)hdl_list;
    arg[4] = 2;
    control(ANCS_OP_ACTION, arg, 5);
    CHECK(perform_calls == 1);
    control(ANCS_OP_ACTION, arg, 3);    /* short */
    CHECK(perform_calls == 1);
}

static void test_cleanup(void) {
    reset_all();
    control(ANCS_OP_ENABLE, 0, 0);
    uint8_t notif[8] = { 0, 0, 1, 1, 1, 0, 0, 0 };
    ancs_hook_source(notif);
    ancs_cleanup_session();
    CHECK(ctx_storage.ancs_lease_deadline == 0 && ctx_storage.ancs_timer == 0);
    CHECK(ctx_storage.ancs_ring != 0 && ctx_storage.ancs_idle == 1);
    CHECK(timer_stop_calls == 1 && timer_delete_calls == 1);
    ancs_hook_source(notif);            /* off: not queued, stock still runs */
    CHECK(stock_push_calls == 2);
    sent_reset();
    control(ANCS_OP_ENABLE, 0, 0);      /* recreates the timer, keeps the ring */
    CHECK(ctx_storage.ancs_timer == 0x1234 && ctx_storage.ancs_epoch == 2);
    sent_reset();
    ancs_hook_source(notif);
    drain();
    CHECK(sent_n == 1);                 /* the pre-cleanup record was stale */
}

int main(void) {
    test_left_lens_and_disabled();
    test_enable_status_and_source();
    test_attr_chunking();
    test_app_attr();
    test_ring_full_and_pacing();
    test_lease_expiry_and_epoch();
    test_send_failure();
    test_action();
    test_cleanup();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    printf("ancs_relay host test: all checks passed\n");
    return 0;
}

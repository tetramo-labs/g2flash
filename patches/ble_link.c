#include <stdint.h>
#include "cfw_context.h"

/*
 * ble_link.c — phone-controlled BLE link speed (sid-0x09 fields 127/128).
 *
 * Upstream Faceclaw hard-codes the fast profile: it rewrites the flash interval
 * table to 7.5 ms and forces every connection-parameter request to "fast", so
 * the stock 60-second slow-mode timer can never throttle image traffic. That
 * costs battery whenever the phone is merely connected. Here the same two
 * effects are runtime-gated on one context flag; the default (flag clear) is
 * the untouched stock behaviour.
 *
 * STOCK MECHANISM (2.2.9.22, host side of the BLE stack)
 *
 *   _connectParamReq_impl(mode) @0x47c0e8   mode 0xa3 = fast, 0xa4 = slow
 *     movs r5,r0 ; bl 0x475814 (connection getter)          <- hook 1 (0x47c0ee)
 *     ... validation and a 45 s hold-off after the last update (0x47c2f4,
 *     flag 0x200773a7 / timestamp 0x200765c0) that reposts the deferred
 *     request every 2 s (0x47c45e); 0x47c468: if mode == applied_mode
 *     (0x20004d82) the repost waits 10 s instead
 *     0x47c4aa: bl 0x47bbf6(conn)                             <- hook 3
 *               classifies the LIVE link: 0xa3 ("already fast") when the
 *               connection record's interval (+0x18, 1.25 ms units) is below
 *               25 and its latency (+0x1a) equals the stock fast profile's,
 *               else 0xa4. A 0xa3 verdict ends the request right there:
 *               nothing is sent, the profile slot is not touched.
 *     0x47c5a8: profile = (mode == 0xa3) ? 0x7b286c : 0x7b285c
 *     0x47c5b4: *(0x200765ec) = profile                       (pointer, not a copy)
 *     0x47c6e0: bl 0x47b940(mode, conn)                       <- hook 2
 *               reads the profile through *(0x200765ec) and sends the request
 *     0x47c6e8: current_mode(0x20004d7f) = mode
 *
 *   Profile entry (16 bytes): +4 min, +6 max (1.25 ms units), +8 latency,
 *   +10 supervision timeout (10 ms units), +12 retries. Stock fast is
 *   15..30 ms / latency 0; stock slow is 45..90 ms / latency 4.
 *
 *   Requests are queued the same way the stock state machine does it:
 *   0x47b922(mode) records the wanted mode (0x20004d83), then the deferred
 *   call 0x47c710(mode) is (re)scheduled through 0x458ed2/0x458d82 and posts
 *   the message that reaches _connectParamReq_impl on the BLE task.
 *
 * WHAT THIS EXTENSION ADDS
 *
 *   Field 127 (phone -> glasses): ['B','L',1,op]
 *     op 0  STOCK  clear the flag; re-request the mode the stock machine wants
 *     op 1  FAST   set the flag; request fast now
 *     op 2  QUERY  no change (the reply below carries the state)
 *   Field 128 (appended to every settings READ reply):
 *     ['B','L',1, fast, side, wanted_mode, applied_mode, min16, max16, latency16,
 *      live_interval16, live_latency16]
 *     min/max/latency are the profile the last request used; the live pair is
 *     the connection as currently negotiated (0 when there is no connection).
 *
 *   With the flag set, hook 1 turns every requested mode into fast (so the
 *   delayed slow request is neutralised, as upstream does), hook 3 only calls
 *   the link "already fast" when it really runs at 7.5 ms / latency 0 (a phone
 *   or Mac link that starts at 15 or 30 ms otherwise satisfies the stock test
 *   and no request is ever sent), and hook 2 points the profile slot at a RAM
 *   copy of the stock fast entry with min = max = 7.5 ms and latency 0 before
 *   the request is sent. With the flag clear all three hooks are
 *   pass-throughs. Each lens has its own link, so the phone sends the control
 *   to both. Mode 11 cleanup returns the link to stock.
 *
 *   Field 128 also carries the interval and latency (+0x18/+0x1a) of the
 *   connection record the classifier sees (the object at 0x200765b8) after the
 *   profile, so the phone can see what the central actually granted rather
 *   than what was asked for.
 *
 *   The LE 2M feature bit stays enabled statically (patch_compress.py): it only
 *   exposes the PHY, the phone still has to request it.
 */

#ifndef BLE_LINK_HOST_TEST
#define BLE_PROFILE_SLOT        (*(volatile const uint8_t **)0x200765ecu)
#define BLE_STOCK_FAST_PROFILE  ((const uint8_t *)0x007b286cu)
#define BLE_APPLIED_MODE        (*(volatile uint8_t *)0x20004d82u)
#define BLE_WANTED_MODE         (*(volatile uint8_t *)0x20004d83u)
#define BLE_SET_WANTED(mode)    (((void (*)(uint32_t))0x0047b923u)(mode))
#define BLE_DEFER_CANCEL(fn)    (((void (*)(uint32_t))0x00458ed3u)(fn))
#define BLE_DEFER_POST(fn, arg, ms) \
    (((void (*)(uint32_t, uint32_t, uint32_t))0x00458d83u)((fn), (arg), (ms)))
#define BLE_REQUEST_CB          0x0047c711u
#define BLE_SEND_REQUEST(mode, conn) (((int (*)(uint32_t, void *))0x0047b941u)((mode), (conn)))
#define BLE_CLASSIFY(conn)      (((uint32_t (*)(const void *))0x0047bbf7u)(conn))
/* The connection record _connectParamReq_impl hands the classifier (0x47c4a4
 * loads it from 0x200765b8); the getter at 0x475814 returns a different object. */
#define BLE_CONN()              (*(const uint8_t *volatile *)0x200765b8u)
#define BLE_SIDE()              (((uint32_t (*)(void))0x0045d35du)())
#define BLE_PEEK()              peekCustomCfwContext()
#define BLE_CTX()               getCustomCfwContext()
#endif

#define BLE_MODE_FAST      0xa3u
#define BLE_MODE_SLOW      0xa4u
#define BLE_FAST_INTERVAL  6u      /* 6 x 1.25 ms = 7.5 ms */
#define BLE_PROTO_VERSION  1u
#define BLE_OP_STOCK       0u
#define BLE_OP_FAST        1u
#define BLE_OP_QUERY       2u
#define BLE_CONN_INTERVAL  0x18u   /* connection record: current interval, 1.25 ms units */
#define BLE_CONN_LATENCY   0x1au   /* connection record: current slave latency */

static void ble_write16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint32_t ble_read16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/* Hook 1 body: the mode _connectParamReq_impl is about to act on. */
__attribute__((used, noinline)) uint32_t ble_filter_mode(uint32_t mode) {
    customCfwContext *ctx = BLE_PEEK();
    if (ctx && ctx->ble_fast) return BLE_MODE_FAST;
    return mode;
}

/* Hook 3: replaces `bl 0x47bbf6` at 0x47c4aa. The stock classifier answers
 * "already fast" for any link under 31.25 ms with zero latency, and the
 * request ends there, so a link that came up at the central's default 15 or
 * 30 ms would never be asked for 7.5 ms. While fast mode is on, only a
 * 7.5 ms / latency-0 link counts as fast. */
__attribute__((used, noinline)) uint32_t ble_hook_classify(const uint8_t *conn) {
    customCfwContext *ctx = BLE_PEEK();
    if (ctx && ctx->ble_fast) {
        if (conn && ble_read16(conn + BLE_CONN_INTERVAL) == BLE_FAST_INTERVAL &&
            ble_read16(conn + BLE_CONN_LATENCY) == 0)
            return BLE_MODE_FAST;
        return BLE_MODE_SLOW;
    }
    return BLE_CLASSIFY(conn);
}

/* Hook 2: replaces `bl 0x47b940` once the stock code has stored the profile
 * pointer. Runs on the BLE task, so the RAM copy is written here and nowhere
 * else; the control path only flips the flag. */
__attribute__((used, noinline)) int ble_hook_request(uint32_t mode, void *conn) {
    customCfwContext *ctx = BLE_PEEK();
    if (ctx && ctx->ble_fast && (mode & 0xffu) == BLE_MODE_FAST) {
        uint8_t *p = ctx->ble_fast_profile;
        const uint8_t *stock = BLE_STOCK_FAST_PROFILE;
        for (uint32_t i = 0; i < sizeof(ctx->ble_fast_profile); i++) p[i] = stock[i];
        ble_write16(p + 4, BLE_FAST_INTERVAL);
        ble_write16(p + 6, BLE_FAST_INTERVAL);
        ble_write16(p + 8, 0);
        BLE_PROFILE_SLOT = p;
    }
    return BLE_SEND_REQUEST(mode, conn);
}

#ifndef BLE_LINK_HOST_TEST
/* Hook 1: replaces `bl 0x475814` at 0x47c0ee, right after `movs r5,r0` saved
 * the mode. r5 is the caller's copy of the mode for the rest of the function,
 * so it is rewritten here and the connection getter is tail-called. */
__attribute__((naked)) void ble_hook_mode(void) {
    __asm volatile(
        "push {r1-r3, lr}\n"
        "mov r0, r5\n"
        "bl ble_filter_mode\n"
        "mov r5, r0\n"
        "pop {r1-r3, lr}\n"
        "movw r12, #0x5815\n"   /* 0x00475814 | Thumb bit */
        "movt r12, #0x0047\n"
        "bx r12\n"
    );
}
#endif

/* Queue a connection-parameter request exactly as the stock state machine
 * does. The applied-mode byte is cleared first so a request for the mode the
 * link already reports is not dropped as a no-op (the profile behind that
 * mode is what changes); the stack rewrites it when the update completes. */
static void ble_request(uint32_t mode) {
    BLE_APPLIED_MODE = 0;
    BLE_SET_WANTED(mode);
    BLE_DEFER_CANCEL(BLE_REQUEST_CB);
    BLE_DEFER_POST(BLE_REQUEST_CB, mode, 0);
}

static void ble_set_fast(customCfwContext *ctx, int fast) {
    if (fast) {
        ctx->ble_fast = 1;
        ble_request(BLE_MODE_FAST);
        return;
    }
    ctx->ble_fast = 0;
    if (BLE_PROFILE_SLOT == ctx->ble_fast_profile) BLE_PROFILE_SLOT = BLE_STOCK_FAST_PROFILE;
    /* Back to whatever stock currently wants; the hooks are pass-throughs now. */
    uint32_t wanted = BLE_WANTED_MODE;
    ble_request(wanted == BLE_MODE_FAST ? BLE_MODE_FAST : BLE_MODE_SLOW);
}

void ble_apply_control(const uint8_t *data, uint32_t len) {
    if (len < 4u || data[0] != 'B' || data[1] != 'L' || data[2] != BLE_PROTO_VERSION) return;
    uint8_t op = data[3];
    if (op == BLE_OP_QUERY) return;
    if (op != BLE_OP_STOCK && op != BLE_OP_FAST) return;
    customCfwContext *ctx = BLE_CTX();
    if (!ctx) return;
    if ((op == BLE_OP_FAST) == (ctx->ble_fast != 0)) return;   /* already there */
    ble_set_fast(ctx, op == BLE_OP_FAST);
}

/* Field 128 on every settings READ reply. */
unsigned ble_append_status(unsigned char *buf, unsigned len, unsigned capacity) {
    customCfwContext *ctx = BLE_PEEK();
    uint8_t body[17] = {'B', 'L', BLE_PROTO_VERSION, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    body[3] = (ctx && ctx->ble_fast) ? 1u : 0u;
    body[4] = (uint8_t)BLE_SIDE();
    body[5] = BLE_WANTED_MODE;
    body[6] = BLE_APPLIED_MODE;
    const uint8_t *profile = BLE_PROFILE_SLOT;
    if (profile) {
        ble_write16(body + 7, ble_read16(profile + 4));
        ble_write16(body + 9, ble_read16(profile + 6));
        ble_write16(body + 11, ble_read16(profile + 8));
    }
    const uint8_t *conn = BLE_CONN();
    if (conn) {
        ble_write16(body + 13, ble_read16(conn + BLE_CONN_INTERVAL));
        ble_write16(body + 15, ble_read16(conn + BLE_CONN_LATENCY));
    }
    return pb_append_bytes_field(buf, len, capacity, 128u, body, sizeof(body));
}

/* Mode-11 session cleanup: a departing custom app leaves the link at stock speed. */
static void ble_cleanup_session(void) {
    customCfwContext *ctx = BLE_PEEK();
    if (ctx && ctx->ble_fast) ble_set_fast(ctx, 0);
}

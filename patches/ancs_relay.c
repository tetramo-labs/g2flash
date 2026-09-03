#include <stdint.h>
#include "cfw_context.h"
#include "malloc.h"

/*
 * ANCS relay: forward the iOS notifications the right lens already receives
 * through its on-firmware Apple Notification Center Service client to the
 * phone app, over the private sid-0x09 settings channel.
 *
 * WHY. iOS never lets a third-party app read other apps' notifications, but it
 * hands them to a paired BLE accessory over ANCS, and the stock G2 firmware
 * (AmbiqSuite's ANCC profile plus Even's service_ancc.c) already subscribes,
 * fetches every attribute, and reports only the source app to the phone
 * (NOTIFICATION_IOS). This taps the profile before Even's whitelist and its
 * 63-byte title/subtitle copies, so the phone gets the whole notification and
 * decides for itself what to show.
 *
 * STOCK SEAMS (2.2.9.22, platform/ble/profiles/ancc/profile_ancc.c; object
 * 0x4d3e5e..0x4d50e0; control block anccCb @ 0x20068c18). Four direct `bl`
 * sites inside that object are retargeted (patch_compress.py). Each wrapper
 * records the event and tail-calls the stock callee, so stock behaviour,
 * including the whitelist and the on-glass popup, is unchanged:
 *   0x4d438c  _anccNtfValueUpdate -> anccActionListPush(ancc_notif_t *)    ADDED / MODIFIED
 *   0x4d4384  _anccNtfValueUpdate -> _anccNotiRemoveCback(ancc_notif_t *)  REMOVED
 *   0x4d4c7c  _anccAttrHandler    -> _ancsAnccAttrCback(active_notif_t *)  one attribute
 *   0x4d4bf4, 0x4d4daa  _anccAttrHandler -> _anccParseAppAttributes()      app display name
 * ancc_notif_t is the 8-byte Notification Source record
 *   [event_id][event_flags][category_id][category_count][uid LE32].
 * active_notif_t (anccCb+8) as this build lays it out:
 *   +0x006 parseIndex u16, +0x008 attrLength u16, +0x00a attrId u8,
 *   +0x010 notiUid u32, +0x354 attrDataBuf[1024],
 * and the attribute body is attrDataBuf[parseIndex .. +attrLength) when the
 * callback runs. anccCb+0 is the Cordio connId, anccCb+4 the handle list,
 * anccCb+0xc the app-attribute buffer fill. Only the right lens discovers ANCS
 * (APP_BleAnccSvcDiscover returns early on the left) and only the right lens
 * may notify the phone, so the left lens ignores this extension entirely.
 *
 * THREADING. The callbacks run on the BLE stack (WSF) task. The stock protobuf
 * sender (Thread_MsgPbTxByBle) waits up to 500 ms on a full transmit queue whose
 * consumer needs that same task, so nothing here sends from a hook: the hooks
 * append fixed-format records to a lock-free single-producer / single-consumer
 * byte ring and, when the drain timer is parked, start it. ancs_relay_tick (RTOS
 * timer thread) pops a few records per tick, wraps each as a field-105 notify
 * and re-arms itself while the ring is non-empty. The ring is allocated on the
 * first ENABLE and kept for the life of the context, so no producer can race a
 * free; the control-point write used by ACTION posts a WSF message, which is
 * safe from the settings thread (the stock ANCC code does the same from its
 * event-loop thread).
 *
 * CONTRACT (sid 0x09 G2SettingPackage; unknown fields to stock decoders).
 *   field 106 (phone -> glasses) ['A','N', ver=1, op, <payload>]
 *     op 1 ENABLE   arm or renew the fail-open ANCS_LEASE_MS relay lease; answers STATUS
 *     op 2 DISABLE  clear the lease and discard queued records; answers STATUS
 *     op 3 QUERY    answer STATUS
 *     op 4 ACTION   [uid LE32][action u8]  perform the notification's positive (0)
 *                   or negative (1) action through the ANCS control point
 *     op 5 RENEW    renew a live lease silently
 *   field 105 (glasses -> phone) ['A','N', ver=1, kind, <body>], one record each:
 *     kind 1 SOURCE [event_id][event_flags][category_id][category_count][uid LE32]
 *                   as received on the Notification Source (event 0 added,
 *                   1 modified, 2 removed; flags bit0 silent, bit1 important,
 *                   bit2 pre-existing, bit3 positive action, bit4 negative action)
 *     kind 2 ATTR   [uid LE32][attr_id][total LE16][offset LE16][chunk]
 *                   one notification attribute (0 app id, 1 title, 2 subtitle,
 *                   3 message, 4 message size, 5 date, 6/7 action labels), split
 *                   into chunks when longer than the message ceiling; the phone
 *                   concatenates by (uid, attr_id, offset)
 *     kind 3 APP    [total LE16][offset LE16][id_len u8][app id][chunk]
 *                   the display name (app attribute 0) for that bundle id
 *     kind 4 STATUS [enabled][side][drops LE16][seq LE16][send_errs]
 *   Stock requests titles and subtitles up to 63 bytes and messages up to 512,
 *   so those are the relay's ceilings too. Records arrive in profile order:
 *   SOURCE, the eight ATTR records, then APP; a REMOVED SOURCE has no
 *   attributes. A record that does not fit the ring is dropped whole and
 *   counted in STATUS.
 */

#define ANCS_PROTO_VERSION 1u
#define ANCS_RELAY_FIELD   105u

#define ANCS_OP_ENABLE   1u
#define ANCS_OP_DISABLE  2u
#define ANCS_OP_QUERY    3u
#define ANCS_OP_ACTION   4u
#define ANCS_OP_RENEW    5u

#define ANCS_KIND_SOURCE 1u
#define ANCS_KIND_ATTR   2u
#define ANCS_KIND_APP    3u
#define ANCS_KIND_STATUS 4u

#define ANCS_LEASE_MS    90000u
#define ANCS_RING_BYTES  4096u                 /* power of two; divides the 16-bit index space */
#define ANCS_RING_MASK   (ANCS_RING_BYTES - 1u)
#define ANCS_MSG_MAX     150u                  /* field-105 payload bytes, 'A','N' header included */
#define ANCS_HDR_LEN     4u                    /* 'A','N', version, kind */
#define ANCS_ATTR_FIXED  (ANCS_HDR_LEN + 9u)   /* uid, attr id, total, offset */
#define ANCS_APP_FIXED   (ANCS_HDR_LEN + 5u)   /* total, offset, id_len */
#define ANCS_APP_ID_MAX  64u                   /* stock keeps bundle ids in a 64-byte field */
#define ANCS_TICK_MS     20u
#define ANCS_RETRY_MS    100u
#define ANCS_BURST       2u

#define ANCC_CB_CONN_ID        0x000u
#define ANCC_CB_HDL_LIST       0x004u
#define ANCC_CB_ACTIVE         0x008u
#define ANCC_CB_BUF_INDEX      0x00cu
#define ANCC_CB_ATTR_BUF       0x35cu
#define ANCC_ACTIVE_PARSE_IDX  0x006u
#define ANCC_ACTIVE_ATTR_LEN   0x008u
#define ANCC_ACTIVE_ATTR_ID    0x00au
#define ANCC_ACTIVE_UID        0x010u
#define ANCC_ACTIVE_ATTR_BUF   0x354u
#define ANCC_ATTR_BUF_BYTES    0x400u
#define ANCC_NOTIF_BYTES       8u

typedef int  (*ancs_push_fn)(uint8_t *notif);
typedef void (*ancs_notif_fn)(uint8_t *notif);
typedef void (*ancs_attr_fn)(uint8_t *active);
typedef int  (*ancs_parse_fn)(void);
typedef void (*ancs_perform_fn)(uint16_t *hdl_list, uint32_t uid, uint32_t action);

/* Firmware seams, overridable so patches/host/ancs_relay_host_test.c can run
 * the ring, chunking and lease logic on the host. */
#ifndef ANCS_HOST_TEST
#define FW_ANCC_LIST_PUSH ((ancs_push_fn)0x004d40a3u)     /* anccActionListPush */
#define FW_ANCC_REMOVE_CB ((ancs_notif_fn)0x004d428du)    /* _anccNotiRemoveCback */
#define FW_ANCC_ATTR_CB   ((ancs_attr_fn)0x004d459bu)     /* _ancsAnccAttrCback */
#define FW_ANCC_PARSE_APP ((ancs_parse_fn)0x004d474du)    /* _anccParseAppAttributes */
#define FW_ANCC_PERFORM   ((ancs_perform_fn)0x004d3fb1u)  /* AncsPerformNotiAction(hdlList, uid, action) */
#define ANCS_CB           ((volatile uint8_t *)0x20068c18u)
#define ANCS_SEND(buf, len)   ((send_fn)FW_SEND)(1, 9, (buf), (len))
#define ANCS_NOW()            FW_MS_TICK
#define ANCS_SIDE()           FW_SIDE_ID()
#define ANCS_CTX()            getCustomCfwContext()
#define ANCS_PEEK()           peekCustomCfwContext()
#define ANCS_ALLOC(n)         cfw_malloc(n)
#define ANCS_TIMER_NEW(cb, arg) FW_TIMER_NEW((void *)(cb), 0, (arg), 0)
#define ANCS_TIMER_START(t, ms) FW_TIMER_START((t), (ms))
#define ANCS_TIMER_STOP(t)      FW_TIMER_STOP(t)
#define ANCS_TIMER_DELETE(t)    FW_TIMER_DELETE(t)
#endif

void ancs_relay_tick(void *arg);

static int ancs_lease_live(const customCfwContext *ctx) {
    return ctx->ancs_lease_deadline != 0 &&
           (int32_t)(ctx->ancs_lease_deadline - ANCS_NOW()) > 0;
}

/* Producer side: true when a hook may append. Read-only on purpose; the hooks
 * never clear an expired deadline, the consumer does. */
static int ancs_relay_armed(const customCfwContext *ctx) {
    return ctx->ancs_ring != 0 && ancs_lease_live(ctx);
}

static void ancs_kick(customCfwContext *ctx) {
    if (!ctx->ancs_idle || ctx->ancs_timer == 0) return;
    ctx->ancs_idle = 0;
    if (ANCS_TIMER_START(ctx->ancs_timer, ANCS_TICK_MS) != 0) ctx->ancs_idle = 1;
}

/* Append one record [len][epoch][payload...]: the field-105 payload is
 * hdr+body. Space is checked before any byte is written and the head index is
 * published last, so the consumer only ever sees complete records. */
static int ancs_push(customCfwContext *ctx, const uint8_t *hdr, uint32_t hdr_len,
                     const uint8_t *body, uint32_t body_len) {
    uint32_t len = hdr_len + body_len;
    uint32_t used = (uint16_t)(ctx->ancs_head - ctx->ancs_tail);
    if (len > ANCS_MSG_MAX || ANCS_RING_BYTES - used < len + 2u) {
        ctx->ancs_drops++;
        return 0;
    }
    uint8_t *ring = ctx->ancs_ring;
    uint32_t w = ctx->ancs_head;
    ring[w++ & ANCS_RING_MASK] = (uint8_t)len;
    ring[w++ & ANCS_RING_MASK] = ctx->ancs_epoch;
    for (uint32_t i = 0; i < hdr_len; i++) ring[w++ & ANCS_RING_MASK] = hdr[i];
    for (uint32_t i = 0; i < body_len; i++) ring[w++ & ANCS_RING_MASK] = body[i];
    ctx->ancs_head = (uint16_t)w;
    ancs_kick(ctx);
    return 1;
}

static void ancs_relay_source(const uint8_t *notif) {
    customCfwContext *ctx = ANCS_PEEK();
    if (!ctx || !notif || !ancs_relay_armed(ctx)) return;
    uint8_t hdr[ANCS_HDR_LEN] = { 'A', 'N', ANCS_PROTO_VERSION, ANCS_KIND_SOURCE };
    ancs_push(ctx, hdr, sizeof hdr, notif, ANCC_NOTIF_BYTES);
}

static void ancs_relay_attr(const uint8_t *active) {
    customCfwContext *ctx = ANCS_PEEK();
    if (!ctx || !active || !ancs_relay_armed(ctx)) return;
    uint32_t parse = rd16(active + ANCC_ACTIVE_PARSE_IDX);
    uint32_t total = rd16(active + ANCC_ACTIVE_ATTR_LEN);
    if (parse > ANCC_ATTR_BUF_BYTES || total > ANCC_ATTR_BUF_BYTES - parse) return;
    const uint8_t *data = active + ANCC_ACTIVE_ATTR_BUF + parse;
    uint8_t hdr[ANCS_ATTR_FIXED] = { 'A', 'N', ANCS_PROTO_VERSION, ANCS_KIND_ATTR };
    for (uint32_t i = 0; i < 4; i++) hdr[4 + i] = active[ANCC_ACTIVE_UID + i];
    hdr[8] = active[ANCC_ACTIVE_ATTR_ID];
    hdr[9] = (uint8_t)total;
    hdr[10] = (uint8_t)(total >> 8);
    uint32_t off = 0;
    do {
        uint32_t chunk = total - off;
        if (chunk > ANCS_MSG_MAX - ANCS_ATTR_FIXED) chunk = ANCS_MSG_MAX - ANCS_ATTR_FIXED;
        hdr[11] = (uint8_t)off;
        hdr[12] = (uint8_t)(off >> 8);
        if (!ancs_push(ctx, hdr, sizeof hdr, data + off, chunk)) return;
        off += chunk;
    } while (off < total);
}

/* The app-attribute response accumulates in anccCb's attribute buffer as
 * [1][bundle id NUL][attr id 0][len LE16][name]; the stock parser is invoked
 * after every fragment and only acts once the record is complete, so the same
 * completeness test keeps this to exactly one relay per response. */
static void ancs_relay_app(void) {
    customCfwContext *ctx = ANCS_PEEK();
    if (!ctx || !ancs_relay_armed(ctx)) return;
    const uint8_t *buf = (const uint8_t *)ANCS_CB + ANCC_CB_ATTR_BUF;
    uint32_t n = rd16((const uint8_t *)ANCS_CB + ANCC_CB_BUF_INDEX);
    if (n > ANCC_ATTR_BUF_BYTES) n = ANCC_ATTR_BUF_BYTES;
    if (n < 5 || buf[0] != 1) return;
    uint32_t idx = 1;
    while (idx < n && buf[idx] != 0) idx++;
    uint32_t id_len = idx - 1;
    if (idx + 4 > n || buf[idx + 1] != 0 || id_len > ANCS_APP_ID_MAX) return;
    uint32_t total = rd16(buf + idx + 2);
    if (total > n - (idx + 4)) return;
    const uint8_t *data = buf + idx + 4;
    uint8_t hdr[ANCS_APP_FIXED + ANCS_APP_ID_MAX];
    hdr[0] = 'A'; hdr[1] = 'N'; hdr[2] = ANCS_PROTO_VERSION; hdr[3] = ANCS_KIND_APP;
    hdr[4] = (uint8_t)total;
    hdr[5] = (uint8_t)(total >> 8);
    hdr[8] = (uint8_t)id_len;
    for (uint32_t i = 0; i < id_len; i++) hdr[ANCS_APP_FIXED + i] = buf[1 + i];
    uint32_t hdr_len = ANCS_APP_FIXED + id_len;
    uint32_t off = 0;
    do {
        uint32_t chunk = total - off;
        if (chunk > ANCS_MSG_MAX - hdr_len) chunk = ANCS_MSG_MAX - hdr_len;
        hdr[6] = (uint8_t)off;
        hdr[7] = (uint8_t)(off >> 8);
        if (!ancs_push(ctx, hdr, hdr_len, data + off, chunk)) return;
        off += chunk;
    } while (off < total);
}

/* ---- retargeted stock call sites (BLE stack task) ------------------------ */

__attribute__((used, noinline)) int ancs_hook_source(uint8_t *notif) {
    ancs_relay_source(notif);
    return FW_ANCC_LIST_PUSH(notif);
}

__attribute__((used, noinline)) void ancs_hook_remove(uint8_t *notif) {
    ancs_relay_source(notif);
    FW_ANCC_REMOVE_CB(notif);
}

__attribute__((used, noinline)) void ancs_hook_attr(uint8_t *active) {
    ancs_relay_attr(active);
    FW_ANCC_ATTR_CB(active);
}

__attribute__((used, noinline)) int ancs_hook_app(void) {
    ancs_relay_app();
    return FW_ANCC_PARSE_APP();
}

/* ---- consumer (RTOS timer thread) ----------------------------------------- */

static void ancs_frame_notify(uint8_t *p, uint32_t body_len) {
    /* G2SettingPackage{commandId=3, magic=0, field 105}; tag 842 = ca 06. */
    p[0] = 0x08; p[1] = 0x03;
    p[2] = 0x10; p[3] = 0x00;
    p[4] = 0xCA; p[5] = 0x06; p[6] = (uint8_t)body_len;
}

void ancs_relay_tick(void *arg) {
    customCfwContext *ctx = (customCfwContext *)arg;
    if (!ctx || ctx->magic != CFW_CTX_MAGIC || !ctx->ancs_ring) return;
    uint8_t *ring = ctx->ancs_ring;
    if (!ancs_lease_live(ctx)) {
        /* Lease lapsed or DISABLE: discard everything queued and park. */
        ctx->ancs_lease_deadline = 0;
        ctx->ancs_tail = ctx->ancs_head;
        ctx->ancs_idle = 1;
        return;
    }
    uint32_t sent = 0;
    int failed = 0;
    while (sent < ANCS_BURST && !failed) {
        uint32_t r = ctx->ancs_tail;
        uint32_t used = (uint16_t)(ctx->ancs_head - r);
        if (used == 0) break;
        uint32_t len = ring[r & ANCS_RING_MASK];
        if (len == 0 || len > ANCS_MSG_MAX || used < len + 2u) {
            /* Not a record boundary: resynchronise on the producer's index. */
            ctx->ancs_tail = ctx->ancs_head;
            break;
        }
        uint8_t epoch = ring[(r + 1u) & ANCS_RING_MASK];
        uint8_t *p = ctx->ancs_notify_buf;
        for (uint32_t i = 0; i < len; i++) p[7 + i] = ring[(r + 2u + i) & ANCS_RING_MASK];
        ctx->ancs_tail = (uint16_t)(r + 2u + len);
        if (epoch != ctx->ancs_epoch) continue;   /* queued before the current ENABLE */
        ancs_frame_notify(p, len);
        if (ANCS_SEND(p, 7 + len) != 0) {
            if (ctx->ancs_send_errs != 0xffu) ctx->ancs_send_errs++;
            failed = 1;
        } else {
            ctx->ancs_seq++;
        }
        sent++;
    }
    if ((uint16_t)(ctx->ancs_head - ctx->ancs_tail) != 0) {
        ANCS_TIMER_START(ctx->ancs_timer, failed ? ANCS_RETRY_MS : ANCS_TICK_MS);
        return;
    }
    /* Park, then re-check: a push that landed between the emptiness test and
     * the park would otherwise wait for the next push to restart the timer. */
    ctx->ancs_idle = 1;
    if ((uint16_t)(ctx->ancs_head - ctx->ancs_tail) != 0) ancs_kick(ctx);
}

/* ---- control plane (settings thread) --------------------------------------- */

static void ancs_send_status(customCfwContext *ctx) {
    uint8_t *p = ctx->ancs_status_buf;
    ancs_frame_notify(p, ANCS_HDR_LEN + 7u);
    p[7] = 'A'; p[8] = 'N'; p[9] = ANCS_PROTO_VERSION; p[10] = ANCS_KIND_STATUS;
    p[11] = ancs_lease_live(ctx) ? 1u : 0u;
    p[12] = (uint8_t)ANCS_SIDE();
    p[13] = (uint8_t)ctx->ancs_drops;
    p[14] = (uint8_t)(ctx->ancs_drops >> 8);
    p[15] = (uint8_t)ctx->ancs_seq;
    p[16] = (uint8_t)(ctx->ancs_seq >> 8);
    p[17] = ctx->ancs_send_errs;
    ANCS_SEND(p, 18);
}

/* Allocate the ring and the drain timer before the lease can arm, so a hook
 * never has to create anything. Returns 0 (and leaves the relay off) on OOM. */
static int ancs_arm(customCfwContext *ctx) {
    if (ctx->ancs_ring == 0) {
        ctx->ancs_ring = (uint8_t *)ANCS_ALLOC(ANCS_RING_BYTES);
        if (ctx->ancs_ring == 0) return 0;
        ctx->ancs_head = 0;
        ctx->ancs_tail = 0;
    }
    if (ctx->ancs_timer == 0) {
        ctx->ancs_timer = ANCS_TIMER_NEW(&ancs_relay_tick, ctx);
        if (ctx->ancs_timer == 0) return 0;
        ctx->ancs_idle = 1;
    }
    return 1;
}

static void ancs_perform_action(uint32_t uid, uint32_t action) {
    if (action > 1u) return;
    if (ANCS_CB[ANCC_CB_CONN_ID] == 0) return;
    uint32_t hdl = rd32((const uint8_t *)ANCS_CB + ANCC_CB_HDL_LIST);
    if ((hdl & 1u) != 0 || hdl - 0x20000000u >= 0x00800000u) return;
    FW_ANCC_PERFORM((uint16_t *)(uintptr_t)hdl, uid, action);
}

/* Parse a field-106 record. Called from faceclaw_scan_settings_control for each
 * sid-0x09 settings WRITE, before the stock decoder runs. */
void ancs_apply_control(const uint8_t *data, uint32_t len) {
    if (len < 4u || data[0] != 'A' || data[1] != 'N' ||
        data[2] != ANCS_PROTO_VERSION) return;
    if (ANCS_SIDE() != 1u) return;
    customCfwContext *ctx = ANCS_CTX();
    if (!ctx) return;
    uint8_t op = data[3];
    if (op == ANCS_OP_ENABLE) {
        int fresh = !ancs_lease_live(ctx);
        if (ancs_arm(ctx)) {
            if (fresh) ctx->ancs_epoch++;
            ctx->ancs_lease_deadline = ANCS_NOW() + ANCS_LEASE_MS;
            ancs_kick(ctx);
        }
        ancs_send_status(ctx);
    } else if (op == ANCS_OP_DISABLE) {
        ctx->ancs_lease_deadline = 0;
        ancs_kick(ctx);            /* the tick discards whatever is queued */
        ancs_send_status(ctx);
    } else if (op == ANCS_OP_QUERY) {
        ancs_send_status(ctx);
    } else if (op == ANCS_OP_ACTION) {
        if (len >= 9u) ancs_perform_action(rd32(data + 4), data[8]);
    } else if (op == ANCS_OP_RENEW) {
        if (ancs_lease_live(ctx)) ctx->ancs_lease_deadline = ANCS_NOW() + ANCS_LEASE_MS;
    }
}

/* Mode-11 session cleanup. The ring stays allocated (a producer may be mid-push);
 * a timer whose delete fails stays in the context so a later cleanup can retry. */
static void ancs_cleanup_session(void) {
    customCfwContext *ctx = ANCS_PEEK();
    if (!ctx) return;
    ctx->ancs_lease_deadline = 0;
    if (ctx->ancs_timer) {
        ANCS_TIMER_STOP(ctx->ancs_timer);
        if (ANCS_TIMER_DELETE(ctx->ancs_timer) == 0) ctx->ancs_timer = 0;
    }
    ctx->ancs_idle = 1;
}

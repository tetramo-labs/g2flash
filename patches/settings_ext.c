#include "cfw_context.h"
#include "protobuf.h"
#include "scene.h"

// CFW firmware-version advertisement and Faceclaw wake-takeover lease.
//
// Appends one extra protobuf field to the sid=0x09 device-settings READ response
// (G2SettingPackage) right before it is framed and sent, so a connected app can
// detect this custom firmware and tell which revision of it is installed without
// any timeout-based probing. The field is:
//
//   field 100, wire type 2 (length-delimited string):
//     "GLASSLYCFW/<n>"
//
// <n> is the Glassly firmware revision. It is bumped every time the firmware
// contract changes (a new mode, field, event, or a behaviour change the phone
// app depends on) and is NOT kept in sync with the Glassly phone app version.
// The phone app requires a specific revision and offers to reflash whenever the
// installed one is older. Earlier builds advertised "GLASSLYCFW/<n> <feature
// tokens>" (and before that "EVENCFW/<n> <feature tokens>"); the tokens are
// gone, so clients discover features by revision alone. Upstream Faceclaw
// firmware uses the same scheme under the "Faceclaw/" prefix.
//
// Tag 100 is far above the stock message's fields (1..19), so stock decoders and
// the phone bridge skip it as an unknown field -- fully backward compatible.
//
// HOOK: the 2.2.9.22 settings responder ends with
//     r0=type(1) r1=sid(9) r2=buf r3=len ; bl FUN_0047d808   ; aa21 send
// We retarget that one `bl` to settings_send_wrapper. The 4 send args are already
// in r0..r3, so the wrapper appends to `buf` (a 256-byte static response buffer
// at 0x20072080 that only uses ~40 B) and tail-calls the real sender with the
// grown length. Only this call site is redirected, but we still guard on sid==9.

// The same sid remains subscribed while EvenHub is shut down, so field 101 is
// also used for a private control message:
//
//   field 101 bytes = ['F','C',version=1,op,nonceLo,nonceHi]
//     op 1 ACQUIRE/RENEW  -- arm a volatile 90-second lease
//     op 2 RELEASE        -- clear the lease; launch a pending dashboard now
//     op 3 WAKE_CLAIM     -- phone saw our wake notify; extend fallback to 5s
//     op 4 WAKE_READY     -- EvenHub frame is ready; cancel the fallback
//     op 5 FB_ACQUIRE      -- arm/renew a separate 90-second direct-framebuffer lease
//     op 6 FB_RELEASE      -- release that lease and restore stock compositor repaints
//     op 7 WEAR_QUERY      -- emit the current stock wear state on sid 0x10
//
// A deferred double tap is reported to the phone as field 102:
//
//   field 102 bytes = ['F','C',version=1,event=1,nonceLo,nonceHi]
//
// A deferred head-up (IMU head-tilt) wake has the same shape with event=5, so
// the phone can answer it with a lighter display than a double tap gets. The
// CLAIM/READY handshake is identical.
//
// While the wake lease is held, the idle gestures the stock display thread
// drops are reported through the same field with other event codes (see
// faceclaw_idle_input_forward below); their last two bytes are the raw touch
// source and 0 rather than a nonce:
//
//   field 102 bytes = ['F','C',version=1,event=2|3|4,source,0]
//     event 2 TAP, 3 LONG_PRESS, 4 LONG_PRESS_RELEASE; source 0/1 = temple
//     touchpads, 4 = R1 ring (the display thread's raw source values).
//
// Both are unknown fields to stock protobuf decoders and are therefore ignored
// by the official app and unmodified firmware.

typedef int  (*send_fn)(int type, int sid, unsigned char *buf, unsigned len);
typedef int  (*pb_decode_fn)(void *stream, const void *fields, void *dest);

/* Microphone-control extension (mic_control.c, same translation unit). Field 103
 * carries mic configuration (ops CONFIGURE/QUERY/STOP/RENEW); field 104 reads the
 * live config back, both appended to every settings READ and pushed as a
 * standalone notify on CONFIGURE/QUERY/STOP. Both ride the already-wired
 * sid-0x09 hooks below — no new patch sites. */
#define MIC_CONTROL_FIELD 103u
void mic_apply_control(const uint8_t *data, uint32_t len);
unsigned mic_append_status(unsigned char *buf, unsigned len, unsigned capacity);
/* ANCS relay (ancs_relay.c, same translation unit). Field 126 carries the
 * control ops (ENABLE/DISABLE/QUERY/ACTION/RENEW); relayed notification records
 * and the STATUS reply go out as field 125 notifies. */
#define ANCS_CONTROL_FIELD 126u
void ancs_apply_control(const uint8_t *data, uint32_t len);
/* BLE link speed (ble_link.c, same translation unit). Field 127 selects the
 * stock or the 7.5 ms fast connection profile; field 128 reports the state on
 * every settings READ. */
#define BLE_CONTROL_FIELD 127u
void ble_apply_control(const uint8_t *data, uint32_t len);
unsigned ble_append_status(unsigned char *buf, unsigned len, unsigned capacity);
typedef void (*display_start_fn)(unsigned app_id, void *arg, unsigned arg_len, void *cb);

#define FW_SEND 0x0047ef05 /* FUN_0047d808 | thumb bit */
#define FW_NOTIFY_SEND 0x0047f025u /* FUN_0047d90e | thumb bit */
#define FW_PB_DECODE ((pb_decode_fn)0x0049f1a5u)       /* FUN_0049da08 */
#define FW_DISPLAY_START ((display_start_fn)0x0046a673u) /* FUN_0046a39e */
#define FW_SIDE_ID ((lens_side_fn)0x00465d4du)         /* 1=right, 2=left */
typedef unsigned (*wear_status_fn)(void);
#define FW_WEAR_STATUS ((wear_status_fn)0x004adddbu)   /* cached WearDetect status: 1=off, 2=on */

#define FACECLAW_PROTO_VERSION 1u
#define FACECLAW_CONTROL_FIELD 101u
#define FACECLAW_EVENT_FIELD   102u
#define FACECLAW_OP_ACQUIRE    1u
#define FACECLAW_OP_RELEASE    2u
#define FACECLAW_OP_CLAIM      3u
#define FACECLAW_OP_READY      4u
#define FACECLAW_OP_FB_ACQUIRE 5u
#define FACECLAW_OP_FB_RELEASE 6u
#define FACECLAW_OP_WEAR_QUERY 7u
#define FACECLAW_EVENT_WAKE    1u
#define FACECLAW_EVENT_TAP     2u
#define FACECLAW_EVENT_LONG    3u
#define FACECLAW_EVENT_RELEASE 4u
#define FACECLAW_EVENT_WAKE_HEADUP 5u
#define FACECLAW_LEASE_MS      90000u
#define FACECLAW_FALLBACK_MS   400u
#define FACECLAW_CLAIMED_MS    5000u

static customCfwContext *faceclaw_context_if_valid(void) {
    customCfwContext *ctx = *(customCfwContext **)CFW_CTX_SLOT;
    if (((uintptr_t)ctx & 3u) != 0 ||
        (uintptr_t)ctx - 0x20000000u >= 0x00800000u) return 0;
    return ctx->magic == CFW_CTX_MAGIC ? ctx : 0;
}

/* Signed subtraction makes the comparison safe across the 32-bit millisecond
 * tick wrap, provided every deadline is less than 2^31 ms away (ours are). */
__attribute__((used, noinline)) int cfw_wake_lease_active(void) {
    customCfwContext *ctx = faceclaw_context_if_valid();
    if (!ctx || ctx->wake_lease_deadline == 0) return 0;
    if ((int32_t)(ctx->wake_lease_deadline - FW_MS_TICK) <= 0) {
        ctx->wake_lease_deadline = 0;
        return 0;
    }
    return 1;
}

/* Faceclaw acquires this lease for every display session, unlike the optional
 * wake-takeover lease. It is therefore also the session-ownership signal used
 * by compatibility-sensitive EvenHub hooks such as long-press forwarding. */
__attribute__((used, noinline)) int cfw_fb_lease_active(void) {
    customCfwContext *ctx = faceclaw_context_if_valid();
    if (!ctx || ctx->direct_lease_deadline == 0) return 0;
    if ((int32_t)(ctx->direct_lease_deadline - FW_MS_TICK) <= 0) {
        ctx->direct_lease_deadline = 0;
        ctx->direct_active = 0;
        cfw_scene_stop(ctx);
        cfw_texture_cache_release(ctx);
        return 0;
    }
    return 1;
}

static void faceclaw_launch_pending_dashboard(customCfwContext *ctx) {
    if (!ctx || !ctx->wake_dashboard_pending) return;
    ctx->wake_dashboard_pending = 0;
    ctx->wake_nonce = 0;
    if (ctx->wake_fallback_timer) FW_TIMER_STOP(ctx->wake_fallback_timer);
    FW_DISPLAY_START(1, 0, 0, 0);
}

/* Runs on the RTOS timer thread. REQUEST_DISPLAY_START_UP enqueues the normal
 * display lifecycle request, so the fail-open path remains the stock launch. */
void faceclaw_wake_fallback_tick(void *arg) {
    customCfwContext *ctx = (customCfwContext *)arg;
    if (!ctx || ctx->magic != CFW_CTX_MAGIC) return;
    if (!ctx->wake_dashboard_pending) return;
    ctx->wake_dashboard_pending = 0;
    ctx->wake_nonce = 0;
    FW_DISPLAY_START(1, 0, 0, 0);
}

static int faceclaw_arm_fallback(customCfwContext *ctx, uint32_t delay_ms) {
    if (ctx->wake_fallback_timer == 0) {
        ctx->wake_fallback_timer =
            FW_TIMER_NEW(CFW_FN_ADDR(faceclaw_wake_fallback_tick), 0, ctx, 0);
    }
    if (ctx->wake_fallback_timer == 0) return 0;
    FW_TIMER_STOP(ctx->wake_fallback_timer);
    return FW_TIMER_START(ctx->wake_fallback_timer, delay_ms) == 0;
}

static void faceclaw_send_wake_event(customCfwContext *ctx, unsigned event) {
    /* G2SettingPackage{commandId=3, magic=0, field102=<private event>}.
     * Tag 102/wire2 = 818 = b2 06. The buffer lives in the singleton because
     * the stock sender's copy/queue lifetime is intentionally treated as
     * opaque. Only the right/master lens notifies the phone. */
    if (!ctx || FW_SIDE_ID() != 1) return;
    unsigned char *p = ctx->wake_notify_buf;
    p[0] = 0x08; p[1] = 0x03;                       /* field 1: commandId=3 */
    p[2] = 0x10; p[3] = 0x00;                       /* field 2: magic=0 */
    p[4] = 0xb2; p[5] = 0x06; p[6] = 0x06;          /* field 102, len 6 */
    p[7] = 'F'; p[8] = 'C';
    p[9] = FACECLAW_PROTO_VERSION;
    p[10] = (unsigned char)event;
    p[11] = (unsigned char)ctx->wake_nonce;
    p[12] = (unsigned char)(ctx->wake_nonce >> 8);
    ((send_fn)FW_SEND)(1, 9, p, 13);
}

/* Revision 27: a tagged scene commit (mode-37 op 10) reports field 129 =
 * [tagLo][tagHi] once the scene has settled. Tag 129/wire2 = 1034 = 8a 08.
 * Called from the EvenHub task and the animation timer thread, like the
 * ANCS relay's sends. Only the right/master lens notifies the phone. */
static void cfw_scene_notify_settled(customCfwContext *ctx, uint16_t tag) {
    if (!ctx || FW_SIDE_ID() != 1) return;
    unsigned char *p = ctx->scene_notify_buf;
    p[0] = 0x08; p[1] = 0x03;
    p[2] = 0x10; p[3] = 0x00;
    p[4] = 0x8a; p[5] = 0x08; p[6] = 0x02;
    p[7] = (unsigned char)tag;
    p[8] = (unsigned char)(tag >> 8);
    ((send_fn)FW_SEND)(1, 9, p, 9);
}

/* Same G2SettingPackage shape as the wake event, with the gesture code and
 * raw source in place of the nonce. Own buffer: a wake notify may still be
 * queued when the tap or release that follows it arrives. */
static void faceclaw_send_gesture_event(customCfwContext *ctx, unsigned event, unsigned source) {
    if (!ctx || FW_SIDE_ID() != 1) return;
    unsigned char *p = ctx->gesture_notify_buf;
    p[0] = 0x08; p[1] = 0x03;                       /* field 1: commandId=3 */
    p[2] = 0x10; p[3] = 0x00;                       /* field 2: magic=0 */
    p[4] = 0xb2; p[5] = 0x06; p[6] = 0x06;          /* field 102, len 6 */
    p[7] = 'F'; p[8] = 'C';
    p[9] = FACECLAW_PROTO_VERSION;
    p[10] = (unsigned char)event;
    p[11] = (unsigned char)source;
    p[12] = 0;
    ((send_fn)FW_SEND)(1, 9, p, 13);
}

/* Idle-input forwarding. With no app on screen, the display thread's touch
 * branch (FUN_0045eb44, message type 7, after the idle gate FUN_0046f136
 * returned 1) launches the dashboard on a double tap (subtype 1) or a head-up
 * (6), the Menu on tap-then-long (0x11), and frees every other record
 * unhandled. Faceclaw's Glanceboard wants three of the dropped ones -- single
 * tap (0), long press (3) and its release (0xe) -- so while Faceclaw holds the
 * wake lease they are reported as field-102 events. Nothing stock is deferred,
 * so unlike the double-tap wake there is no CLAIM/fallback handshake, and the
 * stock branch still runs (and frees the record) exactly as before.
 *
 * 2.3.0 removed the old mode check. headup_gate_impl now calls this helper
 * only when the stock idle gate returns 1, before the record is freed:
 * u16 raw source at +2 (0/1 = temple touchpads, 4 = ring), u32 gesture subtype
 * at +4 -- the same record the UI dispatcher reads (from +2) while an app is
 * running. The caller preserves the stock gate result and register state. */
#define IDLE_GESTURE_TAP     0u
#define IDLE_GESTURE_LONG    3u
#define IDLE_GESTURE_RELEASE 0xeu

void faceclaw_idle_input_forward(const unsigned char *record) {
    if (!record) return;
    uint32_t subtype = (uint32_t)record[4] | ((uint32_t)record[5] << 8) |
                       ((uint32_t)record[6] << 16) | ((uint32_t)record[7] << 24);
    unsigned event = subtype == IDLE_GESTURE_TAP ? FACECLAW_EVENT_TAP
                   : subtype == IDLE_GESTURE_LONG ? FACECLAW_EVENT_LONG
                   : subtype == IDLE_GESTURE_RELEASE ? FACECLAW_EVENT_RELEASE : 0u;
    if (event == 0u || !cfw_wake_lease_active()) return;
    faceclaw_send_gesture_event(faceclaw_context_if_valid(), event, record[2]);
}

/* Send the stock OnboardingDataPackage EVENT/GLS_WEAR_STATUS wire shape
 * directly. The stock helper first checks the running app id and then routes
 * through onboarding's encoder state; using the generic notify sender removes
 * both lifecycle dependencies while retaining its right-arm/BLE guards.
 *
 *   field 1 commandId=3 (EVENT)
 *   field 2 magic=0
 *   field 5 { field 1 event=1, field 2 eventParam=wearing }
 */
__attribute__((used, noinline)) void faceclaw_send_wear_event(unsigned wearing) {
    customCfwContext *ctx = getCustomCfwContext();
    if (!ctx) return;
    unsigned char *p = ctx->wear_notify_buf;
    p[0] = 0x08; p[1] = 0x03;
    p[2] = 0x10; p[3] = 0x00;
    p[4] = 0x2a; p[5] = 0x04;
    p[6] = 0x08; p[7] = 0x01;
    p[8] = 0x10; p[9] = wearing ? 1u : 0u;
    ((send_fn)FW_NOTIFY_SEND)(1, 0x10, p, 10);
}

/* Replaces the two dashboard-start BLs in the idle policy: the double-tap site
 * (0x0045f4c6) through faceclaw_display_start and the head-up site (0x0045f586)
 * through faceclaw_display_start_headup, so the phone learns which gesture
 * woke it. A second double tap while a wake is pending is an emergency
 * stock-dashboard override; a head-up while one is pending is not (a head
 * lift right after the tap must not flash the stock dashboard), it just
 * leaves the pending wake alone. Any missing context/timer/lease takes the
 * exact stock path. */
static void faceclaw_display_start_with(unsigned app_id, void *arg, unsigned arg_len, void *cb,
                                        unsigned event) {
    customCfwContext *ctx = faceclaw_context_if_valid();
    if (app_id != 1 || !cfw_wake_lease_active() || !ctx) {
        FW_DISPLAY_START(app_id, arg, arg_len, cb);
        return;
    }
    if (ctx->wake_dashboard_pending) {
        if (event == FACECLAW_EVENT_WAKE) faceclaw_launch_pending_dashboard(ctx);
        return;
    }
    uint16_t nonce = (uint16_t)(ctx->wake_nonce + 1u);
    if (nonce == 0) nonce = 1;
    ctx->wake_nonce = nonce;
    ctx->wake_dashboard_pending = 1;
    if (!faceclaw_arm_fallback(ctx, FACECLAW_FALLBACK_MS)) {
        faceclaw_launch_pending_dashboard(ctx);
        return;
    }
    faceclaw_send_wake_event(ctx, event);
}

void faceclaw_display_start(unsigned app_id, void *arg, unsigned arg_len, void *cb) {
    faceclaw_display_start_with(app_id, arg, arg_len, cb, FACECLAW_EVENT_WAKE);
}

void faceclaw_display_start_headup(unsigned app_id, void *arg, unsigned arg_len, void *cb) {
    faceclaw_display_start_with(app_id, arg, arg_len, cb, FACECLAW_EVENT_WAKE_HEADUP);
}

static int faceclaw_read_varint(
    const uint8_t **cursor, const uint8_t *end, uint32_t *value
) {
    uint32_t out = 0;
    uint32_t shift = 0;
    const uint8_t *p = *cursor;
    while (p < end && shift < 32) {
        uint8_t byte = *p++;
        out |= (uint32_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            *cursor = p;
            *value = out;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

static void faceclaw_apply_control(const uint8_t *data, uint32_t len) {
    if (len < 6 || data[0] != 'F' || data[1] != 'C' ||
        data[2] != FACECLAW_PROTO_VERSION) return;
    customCfwContext *ctx = getCustomCfwContext();
    if (!ctx) return;
    uint8_t op = data[3];
    uint16_t nonce = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    if (op == FACECLAW_OP_ACQUIRE) {
        ctx->wake_lease_deadline = FW_MS_TICK + FACECLAW_LEASE_MS;
    } else if (op == FACECLAW_OP_RELEASE) {
        ctx->wake_lease_deadline = 0;
        faceclaw_launch_pending_dashboard(ctx);
    } else if (op == FACECLAW_OP_CLAIM) {
        if (cfw_wake_lease_active() && ctx->wake_dashboard_pending) {
            /* Align both lenses to the right/master's nonce before READY. */
            ctx->wake_nonce = nonce;
            faceclaw_arm_fallback(ctx, FACECLAW_CLAIMED_MS);
        }
    } else if (op == FACECLAW_OP_READY) {
        if (cfw_wake_lease_active() && ctx->wake_dashboard_pending &&
            ctx->wake_nonce == nonce) {
            ctx->wake_dashboard_pending = 0;
            ctx->wake_nonce = 0;
            if (ctx->wake_fallback_timer) FW_TIMER_STOP(ctx->wake_fallback_timer);
        }
    } else if (op == FACECLAW_OP_FB_ACQUIRE) {
        /* A fresh lease must earn preservation with a newly presented direct
         * frame; a renewal keeps the current one. */
        if (ctx->direct_lease_deadline == 0 ||
            (int32_t)(ctx->direct_lease_deadline - FW_MS_TICK) <= 0) {
            ctx->direct_active = 0;
            cfw_scene_stop(ctx);
            cfw_texture_cache_release(ctx);
        }
        ctx->direct_lease_deadline = FW_MS_TICK + FACECLAW_LEASE_MS;
    } else if (op == FACECLAW_OP_FB_RELEASE) {
        ctx->direct_lease_deadline = 0;
        ctx->direct_active = 0;
        cfw_scene_stop(ctx);
        cfw_texture_cache_release(ctx);
    } else if (op == FACECLAW_OP_WEAR_QUERY) {
        unsigned status = FW_WEAR_STATUS();
        if (status == 1u || status == 2u)
            faceclaw_send_wear_event(status == 2u ? 1u : 0u);
    }
}

static void faceclaw_scan_settings_control(const uint8_t *buf, uint32_t len) {
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    while (p < end) {
        uint32_t key;
        if (!faceclaw_read_varint(&p, end, &key)) return;
        uint32_t field = key >> 3;
        uint32_t wire = key & 7u;
        if (wire == 0) {
            uint32_t ignored;
            if (!faceclaw_read_varint(&p, end, &ignored)) return;
        } else if (wire == 1) {
            if ((uint32_t)(end - p) < 8) return;
            p += 8;
        } else if (wire == 2) {
            uint32_t item_len;
            if (!faceclaw_read_varint(&p, end, &item_len) ||
                item_len > (uint32_t)(end - p)) return;
            if (field == FACECLAW_CONTROL_FIELD)
                faceclaw_apply_control(p, item_len);
            else if (field == MIC_CONTROL_FIELD)
                mic_apply_control(p, item_len);
            else if (field == ANCS_CONTROL_FIELD)
                ancs_apply_control(p, item_len);
            else if (field == BLE_CONTROL_FIELD)
                ble_apply_control(p, item_len);
            p += item_len;
        } else if (wire == 5) {
            if ((uint32_t)(end - p) < 4) return;
            p += 4;
        } else {
            return;
        }
    }
}

/* Wrap nanopb's decoder at the sid-0x09 service call site. pb_istream_t is
 * {callback,state,bytes_left,errmsg}; state is the original byte buffer. Scan
 * before nanopb advances the stream, then let the stock decoder/handler see
 * the unchanged package. */
int settings_decode_wrapper(void *stream, const void *fields, void *dest) {
    uint32_t *words = (uint32_t *)stream;
    const uint8_t *buf = (const uint8_t *)words[1];
    uint32_t len = words[2];
    if (buf && len) faceclaw_scan_settings_control(buf, len);
    return FW_PB_DECODE(stream, fields, dest);
}

/* Entry trampoline for even_ai_display_ctrl. The first four stock bytes
 * (`push {r0-r6,lr}; mov r6,r0`) are replaced by a B.W here. Reproduce them,
 * suppress only START while a valid Faceclaw lease exists, and otherwise
 * resume the stock function at 0x004f79fe with every argument restored. */
__attribute__((naked)) void faceclaw_evenai_display_entry(void) {
    __asm volatile(
        "push {r0-r6, lr}\n"
        "mov r6, r0\n"
        "cmp r0, #0\n"
        "bne 1f\n"
        "bl cfw_wake_lease_active\n"
        "cmp r0, #0\n"
        "bne 2f\n"
        "ldmia sp, {r0-r3}\n"
        "mov r6, r0\n"
        "1:\n"
        "movw r12, #0x919b\n"   /* 0x004f79fe | Thumb bit; BX needs bit 0 set */
        "movt r12, #0x004f\n"
        "bx r12\n"
        "2:\n"
        "pop {r0-r6, pc}\n"
    );
}

// Firmware revision string "GLASSLYCFW/<n>" (see the header comment). Revision
// history, for reference when bumping:
//   37 -> rebased onto stock G2 2.3.0.24 (upstream jimrandomh/g2flash main, which
//         also brings ring touch-down forwarding as SysEvent 14). Every stock
//         address was re-derived for 2.3.0.24; the fork keeps its own ANCS relay
//         (fields 125/126) instead of upstream's raw-GATT relay.
//   36 -> revision 35's display-task hand-off is gone: the stock display task
//         has a 2 KiB stack and the scene dispatcher alone needs more, so every
//         mode-37/38 message hung the lens. Shadow messages run on the receiving
//         task again (16 KiB), with revision 29's display-gate discipline; the
//         shape text buffers moved off the stack for the display task's animation
//         renders. Settings field 107 carries the failure diagnostics.
//   35 -> shadow messages execute on the display task again (the transport
//         ran them on the BLE/bridge task; built-in font text goes through
//         stock LVGL, which crashed the lens under fast text updates). The
//         receiving task parks the record, wakes the display task and waits
//         on the display gate; control messages still run inline.
//   34 -> upstream's memory layout again (shadow and transport buffers on
//         heap 13, 256 KiB texture cache on the EvenHub heap): the retained
//         scene now renders into the owned shadow instead of a second 150 KiB
//         frame on heap 13, which is what starved LVGL in 31/32. Overlay line 4
//         reports NACK reasons, handler failures and failed mallocs.
//   33 -> heap budget restored to revision 29's shape: the owned shadow, the
//         64 KiB texture cache and the transport's transient decode buffer
//         live on the EvenHub heap (where the image container's buffers were);
//         heap 13 (LVGL's) keeps only the scene frame, transport record buffer,
//         inflater and mic relay. Revisions 31/32 put the shadow and transport
//         buffers on heap 13 and starved LVGL on hardware.
//   32 -> texture cache on heap 13 at 64 KiB (uint32 wire modes kept).
//   31 -> the stock EvenHub image path is no longer patched (snapshot FIFO,
//         deferred consumer, immediate ACK and the 576x288 lift are gone, as
//         upstream Faceclaw/8): custom commands arrive ONLY over the SID-0xf0
//         transport, modes 3/6 carry plain RLE, and the uint16 texture modes
//         12/13/14 are retired in favour of 18/19/20. Shape records draw cached
//         text with a mode-20 (uint32 table) font.
//   30 -> merged jimrandomh/g2flash main (Faceclaw/4..14, 2026-09-18): private
//         SID-0xf0 message transport (length-prefixed streams across packet
//         boundaries, transport-level zlib with a persistent inflater per
//         ingress lens, decoded CRC-16, per-lens selection with bridge
//         forwarding, ACK/NACK with a three-entry ACK history), custom modes
//         dispatched from that transport with no EvenHub container, a
//         256 KiB texture cache with uint32-offset modes 18/19/20 (the uint16
//         modes 12/13/14 stay accepted), magnetic-calibration accuracy kept
//         across IMU reconfiguration under the framebuffer lease, free/max
//         heap statistics in the debug overlay, and an owned 640x480 shadow
//         instead of the container's display buffer. Unlike upstream, the
//         stock EvenHub image path (snapshot FIFO, deferred consumer,
//         immediate ACK, 576x288 lift) is KEPT so existing phone apps still
//         work; both ingress paths feed one dispatcher under one mutex.
//   29 -> dirty-rect list folds overflow into a bounding box instead of
//         dropping it, so multi-line text updates refresh every line under
//         the revision-28 dirty-row panel refresh.
//   28 -> merged jimrandomh/g2flash PR #4 (kalanihelekunihi 2.2.10-CFW): stock
//         base 2.2.10.10, four-microphone relay, dirty-row partial panel
//         refresh, session-persistent inflate stream, LE Data Length request,
//         idle-input forwarding (field-102 events 2/3/4) and head-up wake
//         event 5 (gesture_fwd.c headup_gate). The fast BLE profile stays a
//         field-127 toggle (this fork's hooks) rather than forced on.
//   27 -> panel ownership: raster modes stop/freeze the scene and refresh the
//         shadow from the scene frame; mode-37 op 10 TAG + settled report in
//         field 129; modes 37/38 accepted inside mode-8 bundles.
//   26 -> BLE link speed control: field 127 selects stock (default) or the
//         7.5 ms fast profile at runtime, field 128 reports it on every
//         settings read. The fast profile is no longer forced on.
//   25 -> first revision using the bare numeric scheme. Same feature set as the
//         last token-based advertisement, "GLASSLYCFW/24 img640 imgz rle
//         wakelease directfb fbguard wearnotify cleanup11 texcache12 teximg13
//         texstr14 font15 micctl taplong11 shapes36 scene37 anim38", plus the
//         upstream fast BLE profile (LE 2M, 7.5 ms interval, no slow mode).
//         Ring battery (field 106, mode 17) and ALS (field 105, mode 16) come
//         from upstream; the ring report follows a settings read as a separate
//         notification.
//   24 -> local graphics moved to modes 36-38 and ANCS to fields 125/126.
//   19-23 -> inline text, ANCS relay, compiled paths and rotation.
//
// The string is a normal rodata literal now that build.py emits/relocates .rodata
// (earlier this had to be spelled out byte-by-byte to avoid a rodata section).
#define SETTINGS_RESPONSE_CAPACITY 256u

/* Revision 36: settings field 107 carries the failure diagnostics that the
 * debug overlay shows, so a phone or script can read them without a display.
 * Little-endian: [1][nack_count u16][nack_reason][worker_fail_mode]
 * [worker_fail_count u16][gate_timeouts u16][reserved 6 bytes, zero]
 * [direct_pending][direct_active][gate_held]
 * [alloc_fail_count u16][alloc_fail_heap][last_worker_us/100 u16][0]. */
static unsigned diag_append_status(unsigned char *buf, unsigned len, unsigned capacity) {
    customCfwContext *ctx = faceclaw_context_if_valid();
    if (!ctx) return len;
    unsigned char d[24];
    uint32_t gt = ctx->gate_timeouts > 0xffffu ? 0xffffu : ctx->gate_timeouts;
    uint32_t af = ctx->alloc_fail_count > 0xffffu ? 0xffffu : ctx->alloc_fail_count;
    uint32_t wu = ctx->last_worker_us / 100u;
    if (wu > 0xffffu) wu = 0xffffu;
    d[0] = 1;
    d[1] = (unsigned char)ctx->nack_count; d[2] = (unsigned char)(ctx->nack_count >> 8);
    d[3] = ctx->nack_reason;
    d[4] = ctx->worker_fail_mode;
    d[5] = (unsigned char)ctx->worker_fail_count; d[6] = (unsigned char)(ctx->worker_fail_count >> 8);
    d[7] = (unsigned char)gt; d[8] = (unsigned char)(gt >> 8);
    d[9] = d[10] = d[11] = d[12] = d[13] = d[14] = 0;   /* reserved */
    d[15] = ctx->direct_pending;
    d[16] = ctx->direct_active;
    d[17] = ctx->gate_held;
    d[18] = (unsigned char)af; d[19] = (unsigned char)(af >> 8);
    d[20] = ctx->alloc_fail_heap;
    d[21] = (unsigned char)wu; d[22] = (unsigned char)(wu >> 8);
    d[23] = 0;
    return pb_append_bytes_field(buf, len, capacity, 107u, d, (unsigned)sizeof(d));
}

int settings_send_wrapper(int type, int sid, unsigned char *buf, unsigned len) {
    if (sid == 9) {
        static const char caps[] = "GLASSLYCFW/37";
        len = pb_append_bytes_field(buf, len, SETTINGS_RESPONSE_CAPACITY,
                                    100u, (const unsigned char *)caps,
                                    (unsigned)sizeof(caps) - 1u);
        len = diag_append_status(buf, len, SETTINGS_RESPONSE_CAPACITY);
        len = mic_append_status(buf, len, SETTINGS_RESPONSE_CAPACITY);
        len = ble_append_status(buf, len, SETTINGS_RESPONSE_CAPACITY);
        /* Keep the settings reply inside one BLE frame. The ring report is
         * a separate notification, with the same field/body as upstream mode 17. */
        int result = ((send_fn)FW_SEND)(type, sid, buf, len);
        if (result == 0) {
            const uint8_t query[2] = {17, 0};
            ring_battery_control(query, sizeof(query));
        }
        return result;
    }
    return ((send_fn)FW_SEND)(type, sid, buf, len);
}

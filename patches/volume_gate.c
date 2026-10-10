#include <stdint.h>
#include "cfw_context.h"
#include "protobuf.h"
#include "volume_gate_core.h"

/*
 * Microphone volume gate (revision 40): stop streaming audio while the glasses
 * hear nothing louder than a phone-chosen floor, so the LC3 encoder and the
 * radio idle through silence. See volume_gate_core.h for the state machine.
 *
 * HOOK. thread.audio hands every captured PCM chunk to the stock dispatcher
 * SVC_PcmAppProcessData(source, pcm, bytes) (2.3.0.24: 0x00599780) from two
 * `bl` sites: 0x5596fe (source 0, codec DMIC/I2S) and 0x55973a (source 1,
 * Ambiq PDM). Both are retargeted at mic_gate_process below, which measures
 * the chunk, runs the gate and tail-calls the stock dispatcher only while the
 * gate is open. Everything downstream (the production callbacks, the stock
 * mono LC3 path, this fork's four-microphone array tap) sits behind the
 * dispatcher, so one gate covers every stream the temple can send. Nothing
 * else in the capture path is touched; a closed gate simply means the chunk
 * is never dispatched.
 *
 * CONTRACT (sid-0x09 settings hooks, no further patch sites):
 *   field 132 (RX)  ['V','G', ver=1, op, <payload>]
 *     op 1 CONFIGURE [enabled, thrLo, thrHi, holdLo, holdHi, openLo, openHi]
 *          thr   LE16 linear RMS amplitude the chunk must reach (0..32767)
 *          hold  LE16 ms of quiet before the gate closes
 *          open  LE16 ms the gate stays open at least once it opens
 *          Also renews the lease (below).
 *     op 2 QUERY     push the status now
 *   field 133 (TX)  status, appended to every settings READ reply and pushed
 *                   as a notify on CONFIGURE/QUERY:
 *     ['V','G',1, enabled, open, thrLo,thrHi, holdLo,holdHi, openLo,openHi,
 *      rmsLo,rmsHi, peakLo,peakHi, passed u32, dropped u32, side]
 *     `rms` is the last chunk, `peak` the loudest chunk since the previous
 *     status (cleared on read), `passed`/`dropped` count chunks while enabled.
 *     `side`: 1 = right temple, 2 = left temple.
 *
 * AUDIO-PATH RECORDS. The stock pb notify path drops settings notifications
 * on the LEFT temple, and the phone's mic watchdog restarts a mic that goes
 * quiet for 5 s, so gate state travels on the streaming-notify (audio
 * characteristic) path from both temples, where the phone already listens:
 *     ['V','G',1, event, side, open, rmsLo, rmsHi, tick u32]   (12 bytes)
 *       event 1 OPENED, 2 CLOSED, 3 HEARTBEAT (every 2 s while closed)
 *     ['V','G',1, 4, <status body from byte 3 on>]             (25 bytes)
 *       the CONFIGURE/QUERY status mirror
 * Stock LC3 chunks are 200+ bytes, so the lengths alone keep these apart.
 *
 * LEASE (fail-open, the house paradigm). The gate only drops audio while the
 * phone keeps configuring it: CONFIGURE pushes the deadline VG_LEASE_MS out,
 * and once it lapses chunks pass untouched (status still shows enabled=1 with
 * open=1 and the phone's next CONFIGURE re-arms it). An app that never sends
 * field 132 -- the stock Even app on a CFW temple -- therefore never loses
 * audio to a gate a previous app left behind.
 */

#define VG_LEASE_MS         600000u   /* 10 min; the phone renews every 4 */
#define VG_CONTROL_VERSION  1u
#define VG_OP_CONFIGURE     1u
#define VG_OP_QUERY         2u
#define VG_RECORD_STATUS    4u
#define VG_STATUS_LEN       24u

typedef void (*pcm_process_fn)(uint32_t source, const void *pcm, uint32_t bytes);
/* SVC_PcmAppProcessData (2.3.0.24, Thumb). ABI from the two callers: r0 = source
 * byte, r1 = PCM pointer, r2 = byte count; the return value is ignored by both. */
#define FW_PCM_PROCESS      ((pcm_process_fn)0x00599781U)
typedef int (*vg_notify_fn)(const void *buf, uint32_t len);
#define VG_AUDIO_NOTIFY     ((vg_notify_fn)0x0047f17dU)   /* Thread_MsgStreamingNotifyByBle facade */
typedef uint32_t (*vg_side_fn)(void);
#define VG_SIDE             ((vg_side_fn)0x00465d4dU)     /* 1 = right, 2 = left */
typedef int (*vg_send_fn)(int type, int sid, unsigned char *buf, unsigned len);
#define VG_SEND             ((vg_send_fn)0x0047ef05U)     /* settings notify sender (FW_SEND) */

static int vg_lease_live(const customCfwContext *ctx, uint32_t now) {
    return ctx->vg_lease_deadline != 0u && (int32_t)(ctx->vg_lease_deadline - now) > 0;
}

static void vg_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void vg_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Live event on the audio path. Runs on the audio thread; the stock fallback
 * hands this facade a stack buffer too, so no heap is needed. */
static void vg_send_event(const vg_state *s, uint8_t event, uint32_t now) {
    uint8_t r[12];
    r[0] = 'V'; r[1] = 'G'; r[2] = (uint8_t)VG_CONTROL_VERSION;
    r[3] = event;
    r[4] = (uint8_t)VG_SIDE();
    r[5] = s->open;
    vg_put16(r + 6, s->last_rms);
    vg_put32(r + 8, now);
    VG_AUDIO_NOTIFY(r, sizeof(r));
}

/* The retargeted dispatcher call. Non-static + noinline: patch_compress.py
 * looks it up by name as the `bl` target. */
__attribute__((used, noinline)) void mic_gate_process(uint32_t source, const void *pcm, uint32_t bytes) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (ctx && ctx->volume_gate.enabled && pcm && bytes >= 2u) {
        uint32_t now = FW_MS_TICK;
        if (!vg_lease_live(ctx, now)) {
            ctx->volume_gate.open = 1u;           /* lapsed: pass-through, no reports */
        } else {
            uint8_t event = VG_EVENT_NONE;
            int pass = vg_process(&ctx->volume_gate, (const int16_t *)pcm, bytes / 2u, now, &event);
            if (event != VG_EVENT_NONE) vg_send_event(&ctx->volume_gate, event, now);
            if (!pass) return;
        }
    }
    FW_PCM_PROCESS(source, pcm, bytes);
}

static unsigned vg_status_body(customCfwContext *ctx, uint8_t *b) {
    vg_state *s = &ctx->volume_gate;
    b[0] = 'V'; b[1] = 'G'; b[2] = (uint8_t)VG_CONTROL_VERSION;
    b[3] = s->enabled;
    b[4] = s->open;
    vg_put16(b + 5, s->threshold);
    vg_put16(b + 7, s->hold_ms);
    vg_put16(b + 9, s->min_open_ms);
    vg_put16(b + 11, s->last_rms);
    vg_put16(b + 13, s->peak_rms);
    vg_put32(b + 15, s->passed);
    vg_put32(b + 19, s->dropped);
    b[23] = (uint8_t)VG_SIDE();
    s->peak_rms = 0u;
    return VG_STATUS_LEN;
}

/* Field 133 on every settings READ reply. */
unsigned volume_gate_append_status(unsigned char *buf, unsigned len, unsigned capacity) {
    customCfwContext *ctx = peekCustomCfwContext();
    uint8_t body[VG_STATUS_LEN];
    if (ctx) {
        vg_status_body(ctx, body);
    } else {
        for (unsigned i = 0; i < sizeof(body); i++) body[i] = 0u;
        body[0] = 'V'; body[1] = 'G'; body[2] = (uint8_t)VG_CONTROL_VERSION;
        body[4] = 1u;
        body[23] = (uint8_t)VG_SIDE();
    }
    return pb_append_bytes_field(buf, len, capacity, 133u, body, sizeof(body));
}

/* Standalone status: a field-133 pb notify (right temple only, by stock
 * behaviour) plus the audio-path mirror from either temple. */
static void vg_push_status(customCfwContext *ctx) {
    uint8_t body[VG_STATUS_LEN];
    vg_status_body(ctx, body);
    {
        unsigned char *p = ctx->vg_notify_buf;
        p[0] = 0x08; p[1] = 0x03;                       /* field 1: commandId=3 */
        p[2] = 0x10; p[3] = 0x00;                       /* field 2: magic=0 */
        p[4] = 0xAA; p[5] = 0x08;                       /* field 133, wire type 2: tag 1066 */
        p[6] = (unsigned char)VG_STATUS_LEN;
        for (unsigned i = 0; i < VG_STATUS_LEN; i++) p[7 + i] = body[i];
        VG_SEND(1, 9, p, 7u + VG_STATUS_LEN);
    }
    {
        uint8_t m[VG_STATUS_LEN + 1u];
        m[0] = 'V'; m[1] = 'G'; m[2] = (uint8_t)VG_CONTROL_VERSION; m[3] = (uint8_t)VG_RECORD_STATUS;
        for (unsigned i = 3; i < VG_STATUS_LEN; i++) m[i + 1u] = body[i];
        VG_AUDIO_NOTIFY(m, sizeof(m));
    }
}

void volume_gate_apply_control(const uint8_t *data, uint32_t len) {
    if (len < 4u || data[0] != 'V' || data[1] != 'G' || data[2] != VG_CONTROL_VERSION) return;
    uint8_t op = data[3];
    customCfwContext *ctx = getCustomCfwContext();
    if (!ctx) return;
    if (op == VG_OP_CONFIGURE) {
        if (len < 11u) return;
        const uint8_t *c = data + 4;
        uint16_t thr  = (uint16_t)(c[1] | (c[2] << 8));
        uint16_t hold = (uint16_t)(c[3] | (c[4] << 8));
        uint16_t open = (uint16_t)(c[5] | (c[6] << 8));
        uint32_t now = FW_MS_TICK;
        vg_state *s = &ctx->volume_gate;
        /* A renewal with the same parameters keeps the running state; only a
         * changed configuration restarts the gate (open). */
        if (!s->enabled || !vg_lease_live(ctx, now) || s->threshold != thr ||
            s->hold_ms != hold || s->min_open_ms != open || (c[0] != 0u) != (s->enabled != 0u)) {
            vg_configure(s, c[0], thr, hold, open);
        }
        ctx->vg_lease_deadline = now + VG_LEASE_MS;
        if (ctx->vg_lease_deadline == 0u) ctx->vg_lease_deadline = 1u;
    } else if (op != VG_OP_QUERY) {
        return;
    }
    vg_push_status(ctx);
}

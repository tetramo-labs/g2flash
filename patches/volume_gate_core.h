#ifndef VOLUME_GATE_CORE_H
#define VOLUME_GATE_CORE_H
#include <stdint.h>

/*
 * Microphone volume gate (revision 40). Pure state machine shared by the
 * firmware (volume_gate.c) and the host test (host/volume_gate_host_test.c).
 *
 * Every PCM chunk the audio thread hands the stock dispatcher passes through
 * vg_process first. While the gate is OPEN the chunk is encoded and sent as
 * stock does; while it is CLOSED the chunk is dropped before any encode, so
 * neither the LC3 encoder nor the radio runs for silence. The gate is
 * level-driven: a chunk whose RMS reaches `threshold` is loud. A loud chunk
 * opens the gate (for at least `min_open_ms`); the gate closes once no loud
 * chunk has been seen for `hold_ms` AND the minimum open time has elapsed.
 *
 * A session restart (first chunk, or a gap of VG_SESSION_GAP_MS between
 * chunks) opens the gate unconditionally so the start of a capture is heard
 * and the codec driver's "audio check" (which reboots a codec whose frames
 * are not consumed in its first 2 s) is never tripped by the gate.
 *
 * The phone's mic watchdog restarts a mic that sends nothing for 5 s, so a
 * closed gate reports itself: VG_EVENT_CLOSED on the transition and
 * VG_EVENT_HEARTBEAT every VG_HEARTBEAT_MS while closed. The caller emits
 * those on the audio path (see volume_gate.c).
 *
 * Integer only: no division by a 64-bit value, no sqrt libcall.
 */

#define VG_SESSION_GAP_MS   1000u
#define VG_HEARTBEAT_MS     2000u
#define VG_MAX_HOLD_MS      60000u
#define VG_MAX_OPEN_MS      60000u

#define VG_EVENT_NONE       0u
#define VG_EVENT_OPENED     1u
#define VG_EVENT_CLOSED     2u
#define VG_EVENT_HEARTBEAT  3u

typedef struct {
    uint8_t  enabled;          /* 0 = pass everything, no reports */
    uint8_t  open;             /* current gate state (1 while disabled) */
    uint16_t threshold;        /* linear RMS amplitude, 0..32767 */
    uint16_t hold_ms;          /* silence before the gate may close */
    uint16_t min_open_ms;      /* minimum open time after an opening chunk */
    uint16_t last_rms;         /* RMS of the last chunk seen */
    uint16_t peak_rms;         /* loudest chunk since the last status read */
    uint32_t last_loud_tick;
    uint32_t open_until_tick;
    uint32_t last_frame_tick;  /* 0 = no chunk yet */
    uint32_t last_report_tick; /* last CLOSED/HEARTBEAT emission */
    uint32_t passed;           /* chunks forwarded while enabled */
    uint32_t dropped;          /* chunks dropped while enabled */
} vg_state;

static inline uint32_t vg_isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* 64-by-32 unsigned divide without the __aeabi_uldivmod libcall the injected
 * blob cannot link: the quotient is known to fit 32 bits (a mean square never
 * exceeds 32767^2). Restoring long division, 64 steps, once per chunk. */
static inline uint32_t vg_udiv64(uint64_t n, uint32_t d) {
    uint64_t rem = 0, q = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((n >> i) & 1u);
        q <<= 1;
        if (rem >= d) { rem -= d; q |= 1u; }
    }
    return q > 0xffffffffull ? 0xffffffffu : (uint32_t)q;
}

static inline void vg_configure(vg_state *s, uint8_t enabled, uint16_t threshold,
                                uint16_t hold_ms, uint16_t min_open_ms) {
    if (threshold > 32767u) threshold = 32767u;
    if (hold_ms > VG_MAX_HOLD_MS) hold_ms = (uint16_t)VG_MAX_HOLD_MS;
    if (min_open_ms > VG_MAX_OPEN_MS) min_open_ms = (uint16_t)VG_MAX_OPEN_MS;
    s->threshold = threshold;
    s->hold_ms = hold_ms;
    s->min_open_ms = min_open_ms;
    s->enabled = enabled ? 1u : 0u;
    /* A (re)configuration never starts closed: the next chunk decides. */
    s->open = 1u;
    s->last_frame_tick = 0u;
    s->last_report_tick = 0u;
}

/* RMS of `samples` int16 values (any channel layout; the level is over all of
 * them). 64-bit accumulate; chunks are measured on their first 4096 samples,
 * far above the 800-sample (50 ms) DMA chunk. */
static inline uint16_t vg_rms(const int16_t *pcm, uint32_t samples, uint64_t *sumsq_out) {
    if (samples > 4096u) samples = 4096u;
    uint64_t sumsq = 0;
    for (uint32_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        sumsq += (uint64_t)(uint32_t)(v * v);
    }
    if (sumsq_out) *sumsq_out = sumsq;
    if (samples == 0u) return 0u;
    uint32_t ms = sumsq < 0x100000000ull ? (uint32_t)sumsq / samples : vg_udiv64(sumsq, samples);
    uint32_t r = vg_isqrt(ms);
    return r > 32767u ? 32767u : (uint16_t)r;
}

/* Feed one chunk. Returns 1 when the chunk must be forwarded, 0 to drop it.
 * `*event` receives a VG_EVENT_* the caller should report (NONE otherwise). */
static inline int vg_process(vg_state *s, const int16_t *pcm, uint32_t samples,
                             uint32_t now, uint8_t *event) {
    *event = VG_EVENT_NONE;
    if (!s->enabled) { s->open = 1u; return 1; }

    uint64_t sumsq = 0;
    uint32_t measured = samples > 4096u ? 4096u : samples;
    uint16_t rms = vg_rms(pcm, samples, &sumsq);
    s->last_rms = rms;
    if (rms > s->peak_rms) s->peak_rms = rms;
    /* Compare mean square against threshold^2 without dividing: loud when
     * sumsq >= thr^2 * n. Threshold 0 is always loud (gate effectively off). */
    uint64_t floor_sq = (uint64_t)s->threshold * (uint64_t)s->threshold * (uint64_t)measured;
    int loud = measured == 0u ? 1 : (sumsq >= floor_sq);

    int fresh = s->last_frame_tick == 0u || (uint32_t)(now - s->last_frame_tick) > VG_SESSION_GAP_MS;
    s->last_frame_tick = now;
    if (fresh) {
        /* New capture session: start open regardless of level. */
        s->last_loud_tick = now;
        s->open_until_tick = now + s->min_open_ms;
        if (!s->open) *event = VG_EVENT_OPENED;
        s->open = 1u;
        s->passed++;
        return 1;
    }

    if (loud) {
        s->last_loud_tick = now;
        if (!s->open) {
            s->open = 1u;
            s->open_until_tick = now + s->min_open_ms;
            *event = VG_EVENT_OPENED;
        }
    } else if (s->open) {
        int min_open_done = (int32_t)(now - s->open_until_tick) >= 0;
        int held = (uint32_t)(now - s->last_loud_tick) >= s->hold_ms;
        if (min_open_done && held) {
            s->open = 0u;
            s->last_report_tick = now;
            *event = VG_EVENT_CLOSED;
        }
    }

    if (!s->open && *event == VG_EVENT_NONE &&
        (uint32_t)(now - s->last_report_tick) >= VG_HEARTBEAT_MS) {
        s->last_report_tick = now;
        *event = VG_EVENT_HEARTBEAT;
    }

    if (s->open) s->passed++; else s->dropped++;
    return s->open ? 1 : 0;
}

#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "volume_gate_core.h"

/* Offline checks for the revision-40 microphone volume gate state machine.
 * 50 ms chunks of 800 samples at 16 kHz, as thread.audio dispatches them. */

#define N 800u
static int16_t quiet[N], loud[N], mid[N];

static void fill_n(int16_t *buf, unsigned n, int16_t amplitude) {
    /* Square wave: RMS equals the amplitude exactly. */
    for (unsigned i = 0; i < n; i++) buf[i] = (i & 1u) ? amplitude : (int16_t)-amplitude;
}
static void fill(int16_t *buf, int16_t amplitude) { fill_n(buf, N, amplitude); }

static int feed(vg_state *s, const int16_t *pcm, uint32_t now, uint8_t *ev) {
    return vg_process(s, pcm, N, now, ev);
}

/* Run `chunks` 50 ms chunks of `pcm` starting at `*now`; count passes and the
 * events seen. */
static unsigned run(vg_state *s, const int16_t *pcm, unsigned chunks, uint32_t *now,
                    unsigned counts[4]) {
    unsigned passed = 0;
    for (unsigned i = 0; i < chunks; i++) {
        uint8_t ev;
        *now += 50u;
        if (feed(s, pcm, *now, &ev)) passed++;
        assert(ev <= VG_EVENT_HEARTBEAT);
        counts[ev]++;
    }
    return passed;
}

int main(void) {
    fill(quiet, 100);     /* -50 dBFS */
    fill(mid, 500);       /* -36 dBFS */
    fill(loud, 3000);     /* -21 dBFS */

    /* rms helper */
    assert(vg_rms(quiet, N, 0) == 100u);
    assert(vg_rms(loud, N, 0) == 3000u);
    assert(vg_rms(quiet, 0, 0) == 0u);
    assert(vg_isqrt(0) == 0 && vg_isqrt(1) == 1 && vg_isqrt(9) == 3 && vg_isqrt(1000000) == 1000);
    {
        int16_t full[N];
        fill(full, 32767);
        assert(vg_rms(full, N, 0) == 32767u);
    }

    vg_state s;
    memset(&s, 0, sizeof s);
    uint32_t now = 1000;
    unsigned c[4];

    /* Disabled: everything passes, nothing reported. */
    vg_configure(&s, 0, 1000, 3000, 2000);
    memset(c, 0, sizeof c);
    assert(run(&s, quiet, 100, &now, c) == 100);
    assert(c[VG_EVENT_OPENED] == 0 && c[VG_EVENT_CLOSED] == 0 && c[VG_EVENT_HEARTBEAT] == 0);
    assert(s.passed == 0 && s.dropped == 0);

    /* Enabled: a session starts open, closes after hold once min-open passed. */
    vg_configure(&s, 1, 1000, 3000, 2000);
    memset(c, 0, sizeof c);
    now += 5000;                           /* gap: fresh session */
    /* 2 s min-open + 3 s hold from the session start = 60 chunks pass, then closed. */
    unsigned passed = run(&s, quiet, 61, &now, c);
    assert(passed == 60);
    assert(!s.open && c[VG_EVENT_CLOSED] == 1 && c[VG_EVENT_OPENED] == 0);
    assert(s.dropped == 1);

    /* Closed: heartbeats every 2 s, nothing passes. */
    memset(c, 0, sizeof c);
    assert(run(&s, quiet, 80, &now, c) == 0);  /* 4 s */
    assert(c[VG_EVENT_HEARTBEAT] == 2 && c[VG_EVENT_CLOSED] == 0);

    /* A loud chunk opens immediately and the chunk itself passes. */
    memset(c, 0, sizeof c);
    assert(run(&s, loud, 1, &now, c) == 1);
    assert(s.open && c[VG_EVENT_OPENED] == 1);
    assert(s.peak_rms == 3000 && s.last_rms == 3000);

    /* Stays open for min_open (2 s = 40 chunks) even in silence, then hold (3 s)
     * must also elapse from the last loud chunk: closes at 3 s after the loud one. */
    memset(c, 0, sizeof c);
    passed = run(&s, quiet, 70, &now, c);
    assert(passed == 59);                  /* 3000 ms / 50 ms - 1 (the closing chunk is dropped) */
    assert(!s.open && c[VG_EVENT_CLOSED] == 1);

    /* Hold longer than min-open: loud chunks keep extending the hold. */
    vg_configure(&s, 1, 1000, 1000, 100);
    now += 5000;
    memset(c, 0, sizeof c);
    run(&s, quiet, 30, &now, c);           /* session start: open 1.5 s, closes at 1 s hold */
    assert(!s.open);
    memset(c, 0, sizeof c);
    for (unsigned i = 0; i < 20; i++) {
        /* alternate loud / quiet: never 1 s of silence */
        run(&s, loud, 1, &now, c);
        run(&s, quiet, 10, &now, c);
    }
    assert(s.open && c[VG_EVENT_OPENED] == 1 && c[VG_EVENT_CLOSED] == 0);

    /* Threshold is inclusive and compared on mean square. */
    vg_configure(&s, 1, 500, 100, 100);
    now += 5000;
    memset(c, 0, sizeof c);
    run(&s, quiet, 10, &now, c);
    assert(!s.open);
    memset(c, 0, sizeof c);
    assert(run(&s, mid, 1, &now, c) == 1 && s.open);
    vg_configure(&s, 1, 501, 100, 100);
    now += 5000;
    memset(c, 0, sizeof c);
    run(&s, quiet, 10, &now, c);
    assert(!s.open);
    memset(c, 0, sizeof c);
    assert(run(&s, mid, 1, &now, c) == 0 && !s.open);

    /* Threshold 0 never closes. */
    vg_configure(&s, 1, 0, 100, 100);
    now += 5000;
    memset(c, 0, sizeof c);
    assert(run(&s, quiet, 100, &now, c) == 100 && s.open);

    /* Session gap re-opens a closed gate so a fresh capture is heard. */
    vg_configure(&s, 1, 1000, 100, 100);
    now += 5000;
    memset(c, 0, sizeof c);
    run(&s, quiet, 10, &now, c);
    assert(!s.open);
    now += VG_SESSION_GAP_MS + 1;
    memset(c, 0, sizeof c);
    assert(run(&s, quiet, 1, &now, c) == 1 && s.open && c[VG_EVENT_OPENED] == 1);

    /* Tick wrap-around: the gate keeps working across 0xffffffff. */
    vg_configure(&s, 1, 1000, 1000, 100);
    now = 0xfffffff0u;
    memset(c, 0, sizeof c);
    run(&s, quiet, 40, &now, c);           /* crosses zero */
    assert(!s.open && c[VG_EVENT_CLOSED] == 1);
    memset(c, 0, sizeof c);
    assert(run(&s, loud, 1, &now, c) == 1 && s.open);

    /* Clamping. */
    vg_configure(&s, 1, 65535, 65535, 65535);
    assert(s.threshold == 32767 && s.hold_ms == VG_MAX_HOLD_MS && s.min_open_ms == VG_MAX_OPEN_MS);

    /* Chunks longer than 4096 samples are measured on their head. */
    {
        static int16_t big[9000];
        fill_n(big, 9000, 2000);
        assert(vg_rms(big, 9000, 0) == 2000u);
    }

    printf("PASS: volume gate host test\n");
    return 0;
}

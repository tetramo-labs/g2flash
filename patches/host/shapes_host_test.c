/*
 * Host-side exerciser for shapes.c / scene.c (no firmware needed).
 *
 * Compiles the rasterizer and the retained-scene logic against stubbed firmware
 * entry points, renders every primitive type plus a scene animation into
 * 640x480 packed-4bpp buffers, and writes them out as PGM images for eyeballing.
 * It also asserts the parsing/validation rules and the easing end points.
 *
 *   cc -std=c11 -O1 -Wall -I.. -o /tmp/shapes_host_test shapes_host_test.c && /tmp/shapes_host_test /tmp/out
 */
#include <stdint.h>
/* utils.c first: the host libc's fortified bzero/strlcpy macros would otherwise
 * collide with the blob's freestanding versions, so <string.h> is never used. */
#include "utils.c"
#include <stdio.h>
#include <stdlib.h>

/* ---- stand-ins for the parts of the CFW blob we are not testing ------------ */
#include "cfw_context.h"
#undef FW_MS_TICK
static uint32_t g_now;
#define FW_MS_TICK g_now
#include "debug.h"
#include "texture_cache.h"
#include "malloc.h"

#define IMAGE_W 640u
#define IMAGE_H 480u
#define IMAGE_STRIDE (IMAGE_W / 2u)
#define IMAGE_BYTES (IMAGE_STRIDE * IMAGE_H)

static customCfwContext g_ctx;
static int g_lease = 1;
static int g_timer_started, g_timer_stopped, g_gate_waits, g_gate_signals, g_presented;
static int g_fail_frame_alloc, g_presented_shadow;
static uint8_t g_container_shadow[640 * 480 / 2];

static customCfwContext *peekCustomCfwContext(void) { return &g_ctx; }
int cfw_fb_lease_active(void) { return g_lease; }
static void rl_add(cfw_rectlist *rl, uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    if (rl && rl->n < CFW_RECT_MAX) {
        rl->r[rl->n].l = (uint16_t)l; rl->r[rl->n].t = (uint16_t)t;
        rl->r[rl->n].w = (uint16_t)w; rl->r[rl->n].h = (uint16_t)h; rl->n++;
    }
}
static int g_alloc_fail_after=-1, g_alloc_live;
static void *cfw_heap13_malloc(uint32_t size) {
    if ((g_fail_frame_alloc && size == IMAGE_BYTES) || g_alloc_fail_after==0) return 0;
    if (g_alloc_fail_after>0)g_alloc_fail_after--;
    void *p=malloc(size);if(p)g_alloc_live++;return p;
}
static uint8_t *cfw_shadow_buffer(uint8_t *state) { return state ? g_container_shadow : 0; }
static void present_shadow(uint8_t *state, uint32_t w, uint32_t h, cfw_rectlist *rl) {
    (void)state; (void)w; (void)h; g_presented_shadow++; if (rl) rl->direct_submitted = 1;
}
static void cfw_heap13_free(void *p) { if(p)g_alloc_live--;free(p); }
static int  stub_timer_start(uint32_t h, uint32_t ms) { (void)h; (void)ms; g_timer_started++; return 0; }
static int  stub_timer_stop(uint32_t h) { (void)h; g_timer_stopped++; return 0; }
static int g_fail_timer;
static uint32_t stub_timer_new(void *cb, uint32_t t, void *a, void *attr) { (void)cb; (void)t; (void)a; (void)attr; return g_fail_timer?0:0x1234; }
static void stub_gate_wait(void) { g_gate_waits++; }
static void stub_gate_signal(void) { g_gate_signals++; }
#define FW_TIMER_START stub_timer_start
#define FW_TIMER_STOP stub_timer_stop
#define FW_TIMER_NEW stub_timer_new
#define FW_DISPLAY_WAIT stub_gate_wait
#define FW_DISPLAY_SIGNAL stub_gate_signal

/* texture stubs: draw a placeholder box so IMAGE/TEXT slots are visible */
static void stub_box(uint8_t *shadow, uint32_t stride, uint32_t w, uint32_t h, int32_t x, int32_t y, int32_t bw, int32_t bh, uint8_t c) {
    for (int32_t yy = y; yy < y + bh; yy++)
        for (int32_t xx = x; xx < x + bw; xx++) {
            if (xx < 0 || yy < 0 || xx >= (int32_t)w || yy >= (int32_t)h) continue;
            uint8_t *p = shadow + yy * stride + (xx >> 1);
            if (xx & 1) *p = (*p & 0xf0) | c; else *p = (*p & 0x0f) | (c << 4);
        }
}
static int cfw_texture_draw_image(uint8_t *shadow, uint32_t stride, uint32_t w, uint32_t h, const uint8_t *src, uint32_t len, cfw_rectlist *rl) {
    (void)rl; if (len != 7) return -1;
    stub_box(shadow, stride, w, h, (int16_t)rd16(src + 2), (int16_t)rd16(src + 4), 24, 24, src[6] & 15); return 0;
}
static int cfw_texture_draw_string(uint8_t *shadow, uint32_t stride, uint32_t w, uint32_t h, const uint8_t *src, uint32_t len, cfw_rectlist *rl) {
    (void)rl; uint32_t n = src[7]; if (len != 8 + n) return -1;
    stub_box(shadow, stride, w, h, (int16_t)rd16(src + 2), (int16_t)rd16(src + 4), 8 * n, 12, src[6] & 15); return 0;
}
static int cfw_builtin_draw_string_buf(uint8_t *shadow, uint32_t stride, uint32_t w, uint32_t h, const uint8_t *src, uint32_t len, cfw_rectlist *rl, uint32_t *tokens, uint32_t max_tokens) {
    (void)rl; (void)tokens; uint32_t n = src[5]; if (len != 6 + n || n > max_tokens) return -1;
    stub_box(shadow, stride, w, h, (int16_t)rd16(src), (int16_t)rd16(src + 2), 10 * n, 20, src[4] & 15); return 0;
}
static void present_buffer(customCfwContext *ctx, const uint8_t *buf, cfw_rectlist *rl) {
    (void)ctx; (void)buf; g_presented++; if (rl) rl->direct_submitted = 1;
}

#include "shapes.c"
#include "scene.c"

/* ---- helpers ------------------------------------------------------------------ */

static uint8_t g_shadow[IMAGE_BYTES];
static int g_fail;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void write_pgm(const char *dir, const char *name, const uint8_t *buf) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.pgm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P5\n%u %u\n255\n", IMAGE_W, IMAGE_H);
    for (uint32_t y = 0; y < IMAGE_H; y++)
        for (uint32_t x = 0; x < IMAGE_W; x++) {
            uint8_t b = buf[y * IMAGE_STRIDE + (x >> 1)];
            uint8_t v = (x & 1) ? (b & 15) : (b >> 4);
            fputc(v * 17, f);
        }
    fclose(f);
    printf("wrote %s\n", path);
}

static uint8_t pixel_at(const uint8_t *buf, int x, int y) {
    uint8_t b = buf[y * IMAGE_STRIDE + (x >> 1)];
    return (x & 1) ? (b & 15) : (b >> 4);
}

static uint8_t *rec(uint8_t *p, uint8_t type, uint8_t color, uint8_t width, const int16_t *v, int n) {
    p[0] = type; p[1] = CFW_SHAPE_FLAG_VISIBLE; p[2] = color; p[3] = width;
    for (int i = 0; i < 8; i++) {
        int16_t x = i < n ? v[i] : 0;
        p[4 + 2 * i] = (uint8_t)x; p[5 + 2 * i] = (uint8_t)((uint16_t)x >> 8);
    }
    return p + CFW_SHAPE_RECORD_BYTES;
}

static uint32_t count_color(const uint8_t *buf, uint8_t c) {
    uint32_t n = 0;
    for (uint32_t y = 0; y < IMAGE_H; y++)
        for (uint32_t x = 0; x < IMAGE_W; x++) if (pixel_at(buf, x, y) == c) n++;
    return n;
}

/* ---- tests --------------------------------------------------------------------- */

static void test_immediate(const char *dir) {
    uint8_t msg[1 + 24 * CFW_SHAPE_RECORD_BYTES];
    uint8_t *p = msg + 1;
    int n = 0;
#define R(type, color, width, ...) do { int16_t v[] = { __VA_ARGS__ }; p = rec(p, type, color, width, v, (int)(sizeof v / sizeof v[0])); n++; } while (0)
    R(CFW_SHAPE_RECT,        15, 2, 8, 8, 624, 464, 12);           /* frame */
    R(CFW_SHAPE_LINE,        15, 1, 20, 20, 200, 60);
    R(CFW_SHAPE_LINE,        12, 6, 20, 80, 200, 40);
    R(CFW_SHAPE_LINE,         9, 3, 30, 120, 30, 200);              /* vertical wide */
    R(CFW_SHAPE_RECT_FILL,   10, 0, 240, 20, 100, 60, 0);
    R(CFW_SHAPE_RECT_FILL,   15, 0, 360, 20, 100, 60, 16);
    R(CFW_SHAPE_RECT,        15, 4, 480, 20, 100, 60, 20);
    R(CFW_SHAPE_CIRCLE_FILL, 15, 0, 100, 300, 60);
    R(CFW_SHAPE_CIRCLE,      15, 1, 240, 300, 60);
    R(CFW_SHAPE_CIRCLE,      12, 8, 380, 300, 60);
    R(CFW_SHAPE_CIRCLE_FILL,  8, 0, 500, 300, 1);                   /* tiny */
    R(CFW_SHAPE_TRI_FILL,    15, 0, 40, 400, 120, 400, 80, 440);
    R(CFW_SHAPE_TRI,         15, 2, 140, 440, 220, 440, 180, 400);
    R(CFW_SHAPE_QUAD_FILL,   11, 0, 250, 400, 330, 410, 320, 460, 260, 450);
    R(CFW_SHAPE_QUAD,        15, 1, 350, 400, 430, 410, 420, 460, 360, 450);
    R(CFW_SHAPE_BEZIER2,     15, 1, 20, 260, 140, 100, 220, 260);
    R(CFW_SHAPE_BEZIER3,     15, 4, 240, 200, 300, 100, 360, 300, 440, 200);
    R(CFW_SHAPE_ARC,         15, 3, 560, 120, 50, 180, 360);         /* upper half */
    R(CFW_SHAPE_ARC,         12, 1, 560, 120, 50, 0, 90);
    R(CFW_SHAPE_PIE,         15, 0, 560, 300, 50, -90, 135);
    R(CFW_SHAPE_PIE,          6, 0, 560, 400, 40, 0, 360);           /* full disc */
    R(CFW_SHAPE_IMAGE,       15, 0, 460, 420, 100);
    R(CFW_SHAPE_TEXT,        15, 0, 470, 380, 200, 5);
    R(CFW_SHAPE_LINE,        15, 1, -50, 470, 700, 250);              /* clipped both ends */
#undef R
    msg[0] = (uint8_t)n;
    uint32_t len = 1 + n * CFW_SHAPE_RECORD_BYTES;
    bzero(g_shadow, sizeof g_shadow);
    cfw_rectlist rl = { 0 };
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, len, &rl) == 0);
    write_pgm(dir, "immediate", g_shadow);

    /* filled circle r=60 covers about pi*60^2 = 11310 px of color 15 at (100,300) */
    uint32_t disc = 0;
    for (int y = 230; y < 370; y++) for (int x = 30; x < 170; x++) if (pixel_at(g_shadow, x, y) == 15) disc++;
    CHECK(disc > 11000 && disc < 11600);
    CHECK(pixel_at(g_shadow, 100, 300) == 15);
    CHECK(pixel_at(g_shadow, 100, 239) == 0 && pixel_at(g_shadow, 100, 240) == 15);   /* top edge at cy - r */
    CHECK(pixel_at(g_shadow, 100, 360) == 15 && pixel_at(g_shadow, 100, 361) == 0);   /* bottom edge at cy + r */
    CHECK(pixel_at(g_shadow, 39, 300) == 0 && pixel_at(g_shadow, 40, 300) == 15 && pixel_at(g_shadow, 160, 300) == 15 && pixel_at(g_shadow, 161, 300) == 0);
    /* ring r=60 w=8: center empty, band exactly 8 px thick */
    CHECK(pixel_at(g_shadow, 380, 300) == 0 && pixel_at(g_shadow, 380, 240) == 12 && pixel_at(g_shadow, 380, 247) == 12 && pixel_at(g_shadow, 380, 248) == 0);
    /* rect fill exact size */
    CHECK(pixel_at(g_shadow, 240, 20) == 10 && pixel_at(g_shadow, 339, 79) == 10 && pixel_at(g_shadow, 340, 20) == 0 && pixel_at(g_shadow, 240, 80) == 0);
    /* rounded corner removed, straight edges kept */
    CHECK(pixel_at(g_shadow, 360, 20) == 0 && pixel_at(g_shadow, 410, 20) == 15 && pixel_at(g_shadow, 360, 50) == 15);
    /* tiny circle: r=1 -> a 3x3 block */
    CHECK(pixel_at(g_shadow, 499, 299) == 8 && pixel_at(g_shadow, 501, 301) == 8 && pixel_at(g_shadow, 498, 300) == 0 && pixel_at(g_shadow, 502, 300) == 0 && pixel_at(g_shadow, 500, 298) == 0);
    /* image + text slots went through the texture stubs */
    CHECK(pixel_at(g_shadow, 470, 430) == 15 && pixel_at(g_shadow, 480, 390) == 15);
    /* full pie == disc of radius 40: ~5026 px */
    uint32_t pie = 0;
    for (int y = 355; y < 445; y++) for (int x = 515; x < 605; x++) if (pixel_at(g_shadow, x, y) == 6) pie++;
    CHECK(pie > 4850 && pie < 5300);

    /* validation: wrong length, bad type, text too long */
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, len - 1, &rl) == -1);
    msg[1] = 99;
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, len, &rl) == -1);
    msg[1] = CFW_SHAPE_TEXT; msg[1 + 4 + 6] = 65; msg[1 + 4 + 7] = 0;   /* len 65 */
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, len, &rl) == -1);
}

static uint8_t *put16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)((uint16_t)v >> 8); return p + 2; }

static void test_easing(void) {
    const uint8_t linear[4] = { 0, 0, 255, 255 };
    const uint8_t ease[4] = { 107, 0, 148, 255 };      /* ease-in-out */
    CHECK(cfw_ease_progress(0, linear) == 0);
    CHECK(cfw_ease_progress(32768, linear) == 32768);
    uint32_t mid = cfw_ease_progress(16384, linear);
    CHECK(mid > 16384 - 64 && mid < 16384 + 64);
    uint32_t q = cfw_ease_progress(8192, ease);           /* ease-in-out starts slow */
    CHECK(q < 6000);
    uint32_t prev = 0;
    for (uint32_t u = 0; u <= 32768; u += 512) {          /* monotonic */
        uint32_t v = cfw_ease_progress(u, ease);
        CHECK(v >= prev);
        prev = v;
    }
}

static void test_scene(const char *dir) {
    uint8_t *cache = g_ctx.texture_cache;
    bzero((uint8_t *)&g_ctx, sizeof g_ctx);
    g_ctx.magic = CFW_CTX_MAGIC;
    g_ctx.texture_cache = cache;
    uint8_t msg[256];
    uint8_t *p = msg;
    *p++ = CFW_SCENE_FLAG_COMMIT | CFW_SCENE_FLAG_CLEAR;
    *p++ = 2;                                             /* bg gray 2 */
    /* slot 0: filled circle at (100,240) r 30 */
    *p++ = CFW_SCENE_OP_SET; *p++ = 0;
    { int16_t v[] = { 100, 240, 30 }; p = rec(p, CFW_SHAPE_CIRCLE_FILL, 15, 0, v, 3); }
    /* slot 1: progress bar rect fill w=10 */
    *p++ = CFW_SCENE_OP_SET; *p++ = 1;
    { int16_t v[] = { 40, 400, 10, 20, 0 }; p = rec(p, CFW_SHAPE_RECT_FILL, 12, 0, v, 5); }
    /* slot 5: arc gauge, end angle animates */
    *p++ = CFW_SCENE_OP_SET; *p++ = 5;
    { int16_t v[] = { 400, 240, 100, -90, -90 }; p = rec(p, CFW_SHAPE_ARC, 15, 6, v, 5); }
    /* glide slot 0 by (+400, 0) over 10 frames, ease-in-out */
    *p++ = CFW_SCENE_OP_GLIDE; *p++ = 0; p = put16(p, 400); p = put16(p, 0); *p++ = 10;
    *p++ = 107; *p++ = 0; *p++ = 148; *p++ = 255;
    /* tween slot 1: p2 (w) -> 560, color -> 15 over 20 frames linear */
    *p++ = CFW_SCENE_OP_TWEEN; *p++ = 1; p = put16(p, 0x104); *p++ = 20;
    *p++ = 0; *p++ = 0; *p++ = 255; *p++ = 255; p = put16(p, 560); p = put16(p, 15);
    /* tween slot 5: p4 (end angle) -> 180 over 20 frames */
    *p++ = CFW_SCENE_OP_TWEEN; *p++ = 5; p = put16(p, 0x10); *p++ = 20;
    *p++ = 0; *p++ = 0; *p++ = 255; *p++ = 255; p = put16(p, 180);
    uint32_t len = (uint32_t)(p - msg);

    cfw_rectlist rl = { 0 };
    CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 37, msg, len, 1, &rl) == 0);
    cfw_scene *sc = cfw_scene_peek(&g_ctx);
    CHECK(sc != 0 && sc->fb != 0);
    CHECK(sc->slot_hi == 6 && sc->anim_active == 1 && g_timer_started == 1 && g_presented == 1);
    CHECK(sc->slots[0].frames == 10 && sc->slots[1].frames == 20 && sc->slots[5].frames == 20);
    CHECK(pixel_at(sc->fb, 100, 240) == 15 && pixel_at(sc->fb, 300, 100) == 2);
    write_pgm(dir, "scene_f00", sc->fb);

    /* rejected: inside mode 8, unknown op, truncated tween, SET on slot 255 */
    CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 37, msg, len, 0, &rl) == -1);
    { uint8_t bad[] = { 0, 0, 9, 0 }; CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 37, bad, sizeof bad, 1, &rl) == -1); }
    { uint8_t bad[] = { 0, 0, CFW_SCENE_OP_TWEEN, 1, 0x04, 0x01, 20, 0, 0, 255, 255, 1 }; CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 37, bad, sizeof bad, 1, &rl) == -1); }
    { uint8_t bad[2 + 2 + CFW_SHAPE_RECORD_BYTES] = { 0, 0, CFW_SCENE_OP_SET, 255, CFW_SHAPE_LINE, 1 }; CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 37, bad, sizeof bad, 1, &rl) == -1); }
    CHECK(sc->slot_hi == 6);                              /* nothing above was applied */

    /* drive the timer callback like the RTOS would; the display hook renders */
    int16_t last_x = 100;
    int presented_before = g_presented;
    for (int f = 1; f <= 22; f++) {
        int armed = sc->anim_active;
        g_ctx.direct_pending = 0;
        scene_tick(&g_ctx);
        CHECK(sc->render_due == (armed ? 1 : 0));   /* the timer is not re-armed after the last frame */
        cfw_scene_render_if_due(&g_ctx, sc->fb);
        CHECK(sc->render_due == 0);
        CHECK(sc->slots[0].p[0] >= last_x);              /* glide is monotonic */
        last_x = sc->slots[0].p[0];
        if (f == 5) write_pgm(dir, "scene_f05", sc->fb);
        if (f == 10) {
            CHECK(sc->slots[0].p[0] == 500 && sc->slots[0].frames == 0);
            write_pgm(dir, "scene_f10", sc->fb);
        }
    }
    CHECK(sc->slots[1].p[2] == 560 && sc->slots[1].color == 15 && sc->slots[1].frames == 0);
    CHECK(sc->slots[5].p[4] == 180 && sc->slots[5].frames == 0);
    CHECK(sc->anim_active == 0);
    CHECK(g_presented - presented_before == 20);                     /* one frame per armed tick */
    CHECK(g_gate_waits == 20 && g_gate_signals == 0);                /* every wait handed the gate to a queued job */
    write_pgm(dir, "scene_f20", sc->fb);
    CHECK(pixel_at(sc->fb, 500, 240) == 15 && pixel_at(sc->fb, 100, 240) == 2);
    CHECK(pixel_at(sc->fb, 590, 410) == 15);            /* bar grew to x = 600 */

    /* composed glide: a second glide while moving continues to old target + delta */
    uint8_t g[] = { 0, 2, CFW_SCENE_OP_GLIDE, 0, (uint8_t)-100, 0xff, 0, 0, 10, 0, 0, 255, 255,
                    CFW_SCENE_OP_GLIDE, 0, (uint8_t)-100, 0xff, 0, 0, 10, 0, 0, 255, 255 };
    CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 37, g, sizeof g, 1, &rl) == 0);
    CHECK(sc->slots[0].to[0] == 300 && sc->slots[0].frames == 10);
    /* mode 38: finish snaps to the end and presents; release frees */
    { uint8_t m[] = { 3 }; CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 38, m, 1, 1, &rl) == 0); }
    CHECK(sc->slots[0].p[0] == 300 && sc->slots[0].frames == 0);
    { uint8_t m[] = { 1, 5 }; CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 38, m, 2, 1, &rl) == 0); CHECK(sc->period_ms == CFW_SCENE_MIN_PERIOD); }
    /* lease lapse stops the timer from inside the tick without touching memory */
    { uint8_t gl[] = { 0, 2, CFW_SCENE_OP_GLIDE, 0, 50, 0, 0, 0, 10, 0, 0, 255, 255 };
      CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 37, gl, sizeof gl, 1, &rl) == 0); }
    g_lease = 0;
    int stops = g_timer_stopped;
    scene_tick(&g_ctx);
    CHECK(sc->anim_active == 0 && g_timer_stopped == stops + 1);
    g_lease = 1;
    { uint8_t m[] = { 2 }; CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 38, m, 1, 1, &rl) == 0); }
    CHECK(g_ctx.scene == 0);

    /* no room for the CFW frame: a commit still draws into the container shadow
     * and a glide lands on its end value instead of animating */
    g_fail_frame_alloc = 1;
    bzero(g_container_shadow, sizeof g_container_shadow);
    uint8_t fb[2 + 2 + CFW_SHAPE_RECORD_BYTES + 11];
    fb[0] = CFW_SCENE_FLAG_COMMIT | CFW_SCENE_FLAG_CLEAR; fb[1] = 0; fb[2] = CFW_SCENE_OP_SET; fb[3] = 0;
    { int16_t v[] = { 100, 100, 10 }; rec(fb + 4, CFW_SHAPE_CIRCLE_FILL, 15, 0, v, 3); }
    uint8_t *q = fb + 4 + CFW_SHAPE_RECORD_BYTES;
    q[0] = CFW_SCENE_OP_GLIDE; q[1] = 0; put16(q + 2, 200); put16(q + 4, 0); q[6] = 10; q[7] = 0; q[8] = 0; q[9] = 255; q[10] = 255;
    int started = g_timer_started;
    CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 37, fb, sizeof fb, 1, &rl) == 0);
    sc = cfw_scene_peek(&g_ctx);
    CHECK(sc && sc->fb == 0 && sc->anim_active == 0 && g_timer_started == started);
    CHECK(sc->slots[0].p[0] == 300 && sc->slots[0].frames == 0);
    CHECK(g_presented_shadow == 1 && pixel_at(g_container_shadow, 300, 100) == 15);
    CHECK(cfw_scene_dispatch(&g_ctx, 0, 37, fb, sizeof fb, 1, &rl) == -1);   /* no shadow either */
    g_fail_frame_alloc = 0;
    cfw_scene_release(&g_ctx);
}

/* Inline-text records: variable length in mode 36, stored per slot in the scene. */
static void test_inline_text(void) {
    /* [count][rect 20 B][inline 13+5 B] */
    uint8_t msg[1 + 20 + 13 + 5];
    msg[0] = 2;
    { int16_t v[] = { 8, 8, 100, 40, 0 }; rec(msg + 1, CFW_SHAPE_RECT, 15, 1, v, 5); }
    uint8_t *t = msg + 21;
    t[0] = CFW_SHAPE_TEXT_INLINE; t[1] = 1; t[2] = 0x1F; t[3] = 0;
    put16(t + 4, 10); put16(t + 6, 12); put16(t + 8, 25); put16(t + 10, 40);   /* clip 25 px wide */
    t[12] = 5; t[13] = 'h'; t[14] = 'e'; t[15] = 'l'; t[16] = 'l'; t[17] = 'o';
    bzero(g_shadow, sizeof g_shadow);
    cfw_rectlist rl = { 0 };
    CHECK(cfw_shape_record_len(t, 18) == 18);
    CHECK(cfw_shape_record_len(t, 17) == 0);                         /* truncated */
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, sizeof msg, &rl) == 0);
    /* the stub draws 10 px per byte from (10,12); the 25 px clip leaves x = 10..34 lit */
    CHECK(pixel_at(g_shadow, 12, 20) == 15 && pixel_at(g_shadow, 34, 20) == 15 && pixel_at(g_shadow, 35, 20) == 0);
    t[12] = 0;
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, sizeof msg, &rl) == -1);
    t[12] = 5;
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, sizeof msg - 1, &rl) == -1);

    /* scene: SET an inline slot, then glide it; the bytes live in the slot */
    uint8_t *cache = g_ctx.texture_cache;
    bzero((uint8_t *)&g_ctx, sizeof g_ctx);
    g_ctx.magic = CFW_CTX_MAGIC;
    g_ctx.texture_cache = cache;
    uint8_t sm[2 + 2 + 18 + 11];
    sm[0] = CFW_SCENE_FLAG_COMMIT | CFW_SCENE_FLAG_CLEAR; sm[1] = 0;
    sm[2] = CFW_SCENE_OP_SET; sm[3] = 3;
    for (int i = 0; i < 18; i++) sm[4 + i] = t[i];
    uint8_t *g = sm + 22;
    g[0] = CFW_SCENE_OP_GLIDE; g[1] = 3; put16(g + 2, 100); put16(g + 4, 0); g[6] = 4; g[7] = 0; g[8] = 0; g[9] = 255; g[10] = 255;
    CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 37, sm, sizeof sm, 1, &rl) == 0);
    cfw_scene *sc = cfw_scene_peek(&g_ctx);
    CHECK(sc && sc->slots[3].type == CFW_SHAPE_TEXT_INLINE && sc->slots[3].p[4] == 5 && sc->slots[3].frames == 4);
    CHECK(sc->text[3][0] == 'h' && sc->text[3][4] == 'o');
    CHECK(pixel_at(sc->fb, 12, 20) == 15 && pixel_at(sc->fb, 35, 20) == 0);
    for (int f = 0; f < 4; f++) { g_ctx.direct_pending = 0; scene_tick(&g_ctx); cfw_scene_render_if_due(&g_ctx, sc->fb); }
    CHECK(sc->slots[3].p[0] == 110 && sc->slots[3].frames == 0);
    CHECK(pixel_at(sc->fb, 112, 20) == 15 && pixel_at(sc->fb, 12, 20) == 0);
    /* a SET with a bad length is rejected before anything is applied */
    sm[4 + 12] = 200;
    CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 37, sm, sizeof sm, 1, &rl) == -1);
    { uint8_t m[] = { 2 }; CHECK(cfw_scene_dispatch(&g_ctx, g_container_shadow, 38, m, 1, 1, &rl) == 0); }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    static uint8_t fake_cache[CFW_TEXTURE_CACHE_SIZE];
    g_ctx.texture_cache = fake_cache;
    test_immediate(dir);
    test_easing();
    test_scene(dir);
    test_inline_text();
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}

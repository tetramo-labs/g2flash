/*
 * Host-side exerciser for shapes.c / scene.c (no firmware needed).
 *
 * Compiles the rasterizer, the asset renderers and the retained object cache
 * against stubbed firmware entry points, renders every primitive type plus a
 * scene animation into 640x480 packed-4bpp buffers, and writes them out as PGM
 * images for eyeballing. It also asserts the parsing/validation rules, the
 * easing end points and the cache contract: identity, versions, LRU eviction,
 * pinning, transactions, asset dependencies, 32-bit offsets, the mutation
 * journal and the reply formats (revision 38).
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
#include "malloc.h"
#undef FW_FREE
#define FW_FREE free
#include "memory.h"

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
static customCfwContext *getCustomCfwContext(void) { return &g_ctx; }
int cfw_fb_lease_active(void) { return g_lease; }
static void rl_add(cfw_rectlist *rl, uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    if (rl && rl->n < CFW_RECT_MAX) {
        rl->r[rl->n].l = (uint16_t)l; rl->r[rl->n].t = (uint16_t)t;
        rl->r[rl->n].w = (uint16_t)w; rl->r[rl->n].h = (uint16_t)h; rl->n++;
    }
}
static int g_alloc_fail_after=-1, g_alloc_live, g_store_fail;
static void *cfw_heap13_malloc(uint32_t size) {
    if ((g_fail_frame_alloc && size == IMAGE_BYTES) || g_alloc_fail_after==0) return 0;
    if (g_alloc_fail_after>0)g_alloc_fail_after--;
    void *p=malloc(size);if(p)g_alloc_live++;return p;
}
static void cfw_heap13_free(void *p) { if(p)g_alloc_live--;free(p); }
/* the asset store lives on the EvenHub heap in the firmware */
/* g_store_fail 1: no EvenHub memory at all; 2: only requests of 64 KiB or less succeed */
static void *cfw_malloc(uint32_t size) {
    if (g_store_fail == 1 && size >= 64u * 1024u) return 0;
    if (g_store_fail == 2 && size > 64u * 1024u) return 0;
    void *p = malloc(size); if (p) g_alloc_live++; return p;
}
static void free_store(void *p) { if (p) g_alloc_live--; free(p); }
#undef FW_FREE
#define FW_FREE free_store
static int g_shadow_missing;   /* simulate a failed owned-shadow allocation */
static uint8_t *cfw_shadow_buffer(void) { return g_shadow_missing ? 0 : g_container_shadow; }
static void present_shadow(uint32_t w, uint32_t h, cfw_rectlist *rl) {
    (void)w; (void)h; g_presented_shadow++; if (rl) rl->direct_submitted = 1;
}
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

/* The real image/font renderers over asset regions; the built-in LVGL font
 * path calls stock entry points, so it is replaced by a box painter that draws
 * 10 px per byte from (x, y), clipped to the panel given. */
#define cfw_builtin_draw_string_buf real_builtin_draw_string_buf
#define cfw_builtin_draw_string real_builtin_draw_string
#include "texture_cache.c"
#undef cfw_builtin_draw_string_buf
#undef cfw_builtin_draw_string
static void stub_box(uint8_t *shadow, uint32_t stride, uint32_t w, uint32_t h, int32_t x, int32_t y, int32_t bw, int32_t bh, uint8_t c) {
    for (int32_t yy = y; yy < y + bh; yy++)
        for (int32_t xx = x; xx < x + bw; xx++) {
            if (xx < 0 || yy < 0 || xx >= (int32_t)w || yy >= (int32_t)h) continue;
            uint8_t *p = shadow + yy * stride + (xx >> 1);
            if (xx & 1) *p = (*p & 0xf0) | c; else *p = (*p & 0x0f) | (c << 4);
        }
}
static int cfw_builtin_draw_string_buf(uint8_t *shadow, uint32_t stride, uint32_t w, uint32_t h, const uint8_t *src, uint32_t len, cfw_rectlist *rl, uint32_t *tokens, uint32_t max_tokens) {
    (void)rl; (void)tokens; uint32_t n = src[5]; if (len != 6 + n || n > max_tokens) return -1;
    stub_box(shadow, stride, w, h, (int16_t)rd16(src), (int16_t)rd16(src + 2), 10 * n, 20, src[4] & 15); return 0;
}
static void present_buffer(customCfwContext *ctx, const uint8_t *buf, cfw_rectlist *rl) {
    (void)ctx; (void)buf; g_presented++; if (rl) rl->direct_submitted = 1;
}
static uint8_t g_hint;
static void cfw_message_reply_capacity_hint(uint8_t capacity) { g_hint = capacity; }
static int g_settled; static uint16_t g_settled_tag;
static void cfw_scene_notify_settled(customCfwContext *ctx, uint16_t tag) {
    (void)ctx; g_settled++; g_settled_tag = tag;
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

static uint32_t count_color(const uint8_t *buf, uint8_t c) {
    uint32_t n = 0;
    for (uint32_t y = 0; y < IMAGE_H; y++)
        for (uint32_t x = 0; x < IMAGE_W; x++) if (pixel_at(buf, x, y) == c) n++;
    return n;
}

static uint8_t *put16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)((uint16_t)v >> 8); return p + 2; }
static uint8_t *put32(uint8_t *p, uint32_t v) { put16(p, (int)(v & 0xffffu)); put16(p + 2, (int)(v >> 16)); return p + 4; }

/* geometric record */
static uint8_t *rec(uint8_t *p, uint8_t type, uint8_t color, uint8_t width, const int16_t *v, int n) {
    p[0] = type; p[1] = CFW_SHAPE_FLAG_VISIBLE; p[2] = color; p[3] = width;
    for (int i = 0; i < 8; i++) {
        int16_t x = i < n ? v[i] : 0;
        p[4 + 2 * i] = (uint8_t)x; p[5 + 2 * i] = (uint8_t)((uint16_t)x >> 8);
    }
    return p + CFW_SHAPE_RECORD_BYTES;
}
static uint8_t *rec_asset(uint8_t *p, uint8_t type, uint8_t options, int x, int y, uint32_t asset, uint32_t font) {
    p[0] = type; p[1] = CFW_SHAPE_FLAG_VISIBLE; p[2] = options; p[3] = 0;
    put16(p + 4, x); put16(p + 6, y); put32(p + 8, asset);
    if (type == CFW_SHAPE_TEXT_CACHED) { put32(p + 12, font); return p + 16; }
    return p + 12;
}
static uint8_t *rec_inline(uint8_t *p, uint8_t options, int x, int y, int w, int h, const char *s) {
    uint32_t n = strnlen(s, 255);
    p[0] = CFW_SHAPE_TEXT_INLINE; p[1] = CFW_SHAPE_FLAG_VISIBLE; p[2] = options; p[3] = 0;
    put16(p + 4, x); put16(p + 6, y); put16(p + 8, w); put16(p + 10, h); p[12] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) p[13 + i] = (uint8_t)s[i];
    return p + 13 + n;
}
static uint8_t *rec_path(uint8_t *p, uint8_t color, uint8_t rule, int x, int y, int scale, const uint8_t *cmds, uint32_t n) {
    p[0] = CFW_SHAPE_PATH; p[1] = CFW_SHAPE_FLAG_VISIBLE; p[2] = color; p[3] = rule;
    put16(p + 4, x); put16(p + 6, y); put16(p + 8, scale); put16(p + 10, (int)n);
    for (uint32_t i = 0; i < n; i++) p[12 + i] = cmds[i];
    return p + 12 + n;
}

/* A solid w x h image asset: [w][h][RLE]. */
static uint32_t mk_image(uint8_t *out, uint32_t w, uint32_t h, uint8_t color) {
    out[0] = (uint8_t)w; out[1] = (uint8_t)h;
    uint32_t left = w * h, n = 2;
    while (left) {
        if (left <= 15) { out[n++] = (uint8_t)((left << 4) | color); left = 0; }
        else if (left <= 255) { out[n++] = color; out[n++] = (uint8_t)left; left = 0; }
        else { uint32_t run = left > 65535 ? 65535 : left; out[n++] = color; out[n++] = 0; out[n++] = (uint8_t)run; out[n++] = (uint8_t)(run >> 8); left -= run; }
    }
    return n;
}

/* A w x h image written one token per pixel, so its byte size is 2 + w * h. */
static uint32_t mk_big_image(uint8_t *out, uint32_t w, uint32_t h, uint8_t color) {
    out[0] = (uint8_t)w; out[1] = (uint8_t)h;
    for (uint32_t i = 0; i < w * h; i++) out[2 + i] = (uint8_t)(0x10u | color);
    return 2 + w * h;
}

/* A font asset with glyphs for the characters in `chars`, each a solid 6x8 block. */
static uint32_t mk_font(uint8_t *out, const char *chars, uint8_t color) {
    bzero(out, CFW_FONT_TABLE_BYTES);
    uint32_t n = CFW_FONT_TABLE_BYTES;
    for (const char *c = chars; *c; c++) {
        put32(out + ((uint8_t)*c - 32u) * 4u, n);
        n += mk_image(out + n, 6, 8, color);
    }
    return n;
}

/* ---- message builders --------------------------------------------------------- */

static uint8_t g_msg[70000];
static uint32_t g_len;
static uint16_t g_epoch, g_request;

static void m_begin(uint16_t request) { g_len = 0; put16(g_msg, g_epoch); put16(g_msg + 2, request); g_len = 4; g_request = request; }
static void m_put_asset(uint32_t id, uint32_t ver, uint8_t kind, uint32_t total, uint32_t offset, const uint8_t *data, uint32_t len) {
    uint8_t *p = g_msg + g_len;
    p[0] = 0; put32(p + 1, id); put32(p + 5, ver); p[9] = kind; put32(p + 10, total); put32(p + 14, offset); put16(p + 18, (int)len);
    for (uint32_t i = 0; i < len; i++) p[20 + i] = data[i];
    g_len += 20 + len;
}
static void m_put_object(uint32_t id, uint32_t ver, const uint8_t *record, uint32_t reclen) {
    uint8_t *p = g_msg + g_len;
    p[0] = 1; put32(p + 1, id); put32(p + 5, ver);
    for (uint32_t i = 0; i < reclen; i++) p[9 + i] = record[i];
    g_len += 9 + reclen;
}
/* SHOW: header, flags, bg, puts (entries appended by m_put_* right after), then m_refs/ops */
static uint32_t g_puts_at;
static void m_show(uint16_t request, uint8_t flags, uint8_t bg) { m_begin(request); g_msg[g_len++] = flags; g_msg[g_len++] = bg; g_puts_at = g_len; g_msg[g_len++] = 0; }
static void m_put_count(uint8_t n) { g_msg[g_puts_at] = n; }
static void m_refs(const uint32_t *ids, const uint32_t *vers, uint32_t n) {
    put16(g_msg + g_len, (int)n); g_len += 2;
    for (uint32_t i = 0; i < n; i++) { put32(g_msg + g_len, ids[i]); put32(g_msg + g_len + 4, vers[i]); g_len += 8; }
}
static void m_glide(uint32_t id, int dx, int dy, uint8_t frames, const uint8_t *curve) {
    uint8_t *p = g_msg + g_len; p[0] = 4; put32(p + 1, id); put16(p + 5, dx); put16(p + 7, dy); p[9] = frames;
    for (int i = 0; i < 4; i++) p[10 + i] = curve[i]; g_len += 14;
}
static void m_tween(uint32_t id, uint32_t mask, uint8_t frames, const uint8_t *curve, const int16_t *values) {
    uint8_t *p = g_msg + g_len; p[0] = 5; put32(p + 1, id); put16(p + 5, (int)mask); p[7] = frames;
    for (int i = 0; i < 4; i++) p[8 + i] = curve[i];
    uint32_t n = 0; for (int k = 0; k < 10; k++) if (mask & (1u << k)) { put16(p + 12 + 2 * n, values[n]); n++; }
    g_len += 12 + 2 * n;
}
static void m_op1(uint8_t op, uint32_t id) { uint8_t *p = g_msg + g_len; p[0] = op; put32(p + 1, id); g_len += 5; }
static void m_visible(uint32_t id, uint8_t v) { uint8_t *p = g_msg + g_len; p[0] = 2; put32(p + 1, id); p[5] = v; g_len += 6; }
static void m_rotate(uint32_t id, int32_t angle, int px, int py, uint16_t ms) {
    uint8_t *p = g_msg + g_len; p[0] = 9; put32(p + 1, id); put32(p + 5, (uint32_t)angle); put16(p + 9, px); put16(p + 11, py); put16(p + 13, ms);
    p[15] = 0; p[16] = 0; p[17] = 255; p[18] = 255; g_len += 19;
}

static const uint8_t LINEAR[4] = { 0, 0, 255, 255 };
static const uint8_t EASE[4] = { 107, 0, 148, 255 };

/* dispatch with a 245-byte reply capacity, as a fast link would give */
static int send(uint8_t mode) {
    cfw_rectlist rl = { 0 };
    g_ctx.cache_reply_cap = 245;
    g_ctx.cache_reply_len = 0;
    g_ctx.direct_pending = 0;
    cfw_cache_prepare(&g_ctx, mode);            /* image_worker does this before the gate */
    return cfw_scene_dispatch(&g_ctx, mode, g_msg, g_len, 1, &rl);
}
static int reply_status(void) { return g_ctx.cache_reply_len >= 10 ? g_ctx.cache_reply[3] : -1; }
static int reply_mode(void) { return g_ctx.cache_reply_len >= 10 ? g_ctx.cache_reply[0] : -1; }
static uint32_t reply_request(void) { return rd16(g_ctx.cache_reply + 1); }
static uint16_t reply_epoch(void) { return (uint16_t)rd16(g_ctx.cache_reply + 4); }
static uint32_t reply_revision(void) { return rd32(g_ctx.cache_reply + 6); }
static const uint8_t *reply_extra(void) { return g_ctx.cache_reply + 10; }
static uint32_t reply_extra_len(void) { return g_ctx.cache_reply_len - 10u; }

static void tick(void) {
    g_ctx.direct_pending = 0;
    g_now += 33;
    scene_tick(&g_ctx);
    cfw_cache *sc = cfw_cache_peek(&g_ctx);
    if (sc && sc->fb) cfw_scene_render_if_due(&g_ctx, sc->fb);
}

/* Fresh context and cache; adopt the epoch a RESET reports. */
static void fresh(void) {
    cfw_scene_release(&g_ctx);
    bzero((uint8_t *)&g_ctx, sizeof g_ctx);
    g_ctx.magic = CFW_CTX_MAGIC;
    g_lease = 1; g_alloc_fail_after = -1; g_store_fail = 0; g_shadow_missing = 0; g_fail_timer = 0;
    bzero(g_container_shadow, sizeof g_container_shadow);
    g_epoch = 0;
    m_begin(1); g_msg[g_len++] = 0;                      /* RESET */
    CHECK(send(41) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    g_epoch = reply_epoch();
    CHECK(g_epoch != 0);
    CHECK(reply_extra_len() == 3 && rd16(reply_extra() + 1) == 256);   /* the store the heap gave, in KiB */
}

static cfw_object *obj(uint32_t id) { cfw_cache *sc = cfw_cache_peek(&g_ctx); int i = sc ? cfw_object_find(sc, id, 0) : -1; return i < 0 ? 0 : &sc->objects[i]; }
static cfw_asset *asset(uint32_t id) { cfw_cache *sc = cfw_cache_peek(&g_ctx); int i = sc ? cfw_asset_find(sc, id, 0) : -1; return i < 0 ? 0 : &sc->assets[i]; }
static uint32_t live_objects(void) { cfw_cache *sc = cfw_cache_peek(&g_ctx); uint32_t n = 0; for (uint32_t i = 0; sc && i < CFW_CACHE_OBJECTS; i++) if (sc->objects[i].id) n++; return n; }
static uint32_t live_assets(void) { cfw_cache *sc = cfw_cache_peek(&g_ctx); uint32_t n = 0; for (uint32_t i = 0; sc && i < CFW_CACHE_ASSETS; i++) if (sc->assets[i].id || sc->assets[i].length) n++; return n; }
static uint32_t store_used(void) { cfw_cache *sc = cfw_cache_peek(&g_ctx); uint32_t n = 0; for (uint32_t g = 0; sc && g < CFW_CACHE_GRANULES; g++) n += cfw_store_bit(sc, g); return n * CFW_CACHE_GRANULE; }

/* PUT one filled circle object at (x, y) r 20 */
static void put_circle(uint32_t id, uint32_t ver, int x, int y, uint8_t color) {
    uint8_t r[20]; int16_t v[] = { (int16_t)x, (int16_t)y, 20 }; rec(r, CFW_SHAPE_CIRCLE_FILL, color, 0, v, 3);
    m_begin(100); m_put_object(id, ver, r, 20);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
}
static int show1(uint16_t request, uint32_t id, uint32_t ver) {
    m_show(request, 0x01, 0); m_refs(&id, &ver, 1);
    return send(38);
}

/* ---- tests --------------------------------------------------------------------- */

static void test_immediate(const char *dir) {
    fresh();
    /* image + string + font assets for the asset-bearing immediate records */
    uint8_t img[64], str[] = "hello", font[2048];
    uint32_t il = mk_image(img, 24, 24, 15), fl = mk_font(font, "helo", 9);
    m_begin(2);
    m_put_asset(100, 1, CFW_ASSET_IMAGE, il, 0, img, il);
    m_put_asset(200, 1, CFW_ASSET_STRING, 5, 0, str, 5);
    m_put_asset(300, 1, CFW_ASSET_FONT, fl, 0, font, fl);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);

    uint8_t msg[1 + 26 * CFW_SHAPE_RECORD_BYTES];
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
    R(CFW_SHAPE_LINE,        15, 1, -50, 470, 700, 250);              /* clipped both ends */
#undef R
    p = rec_asset(p, CFW_SHAPE_IMAGE, 15, 460, 420, 100, 0); n++;
    p = rec_asset(p, CFW_SHAPE_TEXT, 15, 470, 380, 200, 0); n++;
    p = rec_asset(p, CFW_SHAPE_TEXT_CACHED, 15, 300, 360, 200, 300); n++;
    msg[0] = (uint8_t)n;
    uint32_t len = (uint32_t)(p - msg);
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
    /* the image asset (24x24 solid) and the built-in text stub */
    CHECK(pixel_at(g_shadow, 470, 430) == 15 && pixel_at(g_shadow, 483, 443) == 15 && pixel_at(g_shadow, 484, 420) == 0);
    CHECK(pixel_at(g_shadow, 480, 390) == 15);
    /* cached font: 5 glyphs of 6x8 at (300,360), gray 9 scaled by top color 15 -> 9 */
    CHECK(pixel_at(g_shadow, 300, 360) == 9 && pixel_at(g_shadow, 329, 367) == 9 && pixel_at(g_shadow, 330, 360) == 0 && pixel_at(g_shadow, 300, 368) == 0);
    /* full pie == disc of radius 40: ~5026 px */
    uint32_t pie = 0;
    for (int y = 355; y < 445; y++) for (int x = 515; x < 605; x++) if (pixel_at(g_shadow, x, y) == 6) pie++;
    CHECK(pie > 4850 && pie < 5300);

    /* validation: wrong length, bad type, unknown asset, path record */
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, len - 1, &rl) == -1);
    msg[1] = 99;
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, len, &rl) == -1);
    msg[1] = CFW_SHAPE_RECT;
    put32(msg + len - 16 + 8, 999);                        /* TEXT_CACHED string id unknown */
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, len, &rl) == -1);
    put32(msg + len - 16 + 8, 200); put32(msg + len - 16 + 12, 100);   /* font id names an image */
    CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, msg, len, &rl) == -1);
    { uint8_t cmds[] = { 0, 0, 0, 0, 0, 4 }; uint8_t pm[64]; pm[0] = 1; uint8_t *e = rec_path(pm + 1, 15, 0, 0, 0, 256, cmds, 6);
      CHECK(cfw_shapes_immediate(g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, pm, (uint32_t)(e - pm), &rl) == -1); }

    /* modes 19/20 by asset id */
    bzero(g_shadow, sizeof g_shadow);
    { uint8_t m[9]; put32(m, 100); put16(m + 4, 10); put16(m + 6, 10); m[8] = 15;
      CHECK(cfw_cache_immediate(&g_ctx, 19, g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, m, 9, &rl) == 0);
      CHECK(pixel_at(g_shadow, 10, 10) == 15 && pixel_at(g_shadow, 33, 33) == 15 && pixel_at(g_shadow, 34, 10) == 0);
      put32(m, 200); CHECK(cfw_cache_immediate(&g_ctx, 19, g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, m, 9, &rl) == -1); }
    { uint8_t m[13]; put32(m, 300); put16(m + 4, 100); put16(m + 6, 100); m[8] = 15; m[9] = 3; m[10] = 'h'; m[11] = 'l'; m[12] = 'o';
      CHECK(cfw_cache_immediate(&g_ctx, 20, g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, m, 13, &rl) == 0);
      CHECK(pixel_at(g_shadow, 100, 100) == 9 && pixel_at(g_shadow, 117, 107) == 9 && pixel_at(g_shadow, 118, 100) == 0);
      m[10] = 'x';                                          /* absent glyph: nothing drawn */
      CHECK(cfw_cache_immediate(&g_ctx, 20, g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, m, 13, &rl) == -1);
      m[10] = 'h'; m[9] = 4;
      CHECK(cfw_cache_immediate(&g_ctx, 20, g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, m, 13, &rl) == -1); }
}

static void test_easing(void) {
    CHECK(cfw_ease_progress(0, LINEAR) == 0);
    CHECK(cfw_ease_progress(32768, LINEAR) == 32768);
    uint32_t mid = cfw_ease_progress(16384, LINEAR);
    CHECK(mid > 16384 - 64 && mid < 16384 + 64);
    uint32_t q = cfw_ease_progress(8192, EASE);           /* ease-in-out starts slow */
    CHECK(q < 6000);
    uint32_t prev = 0;
    for (uint32_t u = 0; u <= 32768; u += 512) {          /* monotonic */
        uint32_t v = cfw_ease_progress(u, EASE);
        CHECK(v >= prev);
        prev = v;
    }
}

/* Define, show, animate, hide, reopen: the fast path. */
static void test_cache_basic(const char *dir) {
    fresh();
    g_presented = 0; g_settled = 0; g_timer_started = 0;
    cfw_cache *sc = cfw_cache_peek(&g_ctx);
    CHECK(sc && sc->revision == 0);

    /* an empty PUT and a STATE query of an empty cache */
    m_begin(5); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && reply_extra()[0] == 0 && reply_revision() == 0);
    m_begin(0); put32(g_msg + 2, 0); put16(g_msg + 6, 0); g_len = 8;
    CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_UNCHANGED && reply_epoch() == g_epoch);

    /* three objects in one PUT: circle, inline text, image via a shared asset */
    uint8_t img[64]; uint32_t il = mk_image(img, 24, 24, 12);
    uint8_t r1[20], r2[64], r3[12];
    { int16_t v[] = { 100, 240, 30 }; rec(r1, CFW_SHAPE_CIRCLE_FILL, 15, 0, v, 3); }
    uint8_t *e2 = rec_inline(r2, 0x1f, 40, 400, 0, 0, "hello");
    rec_asset(r3, CFW_SHAPE_IMAGE, 15, 500, 100, 100, 0);
    m_begin(10);
    m_put_asset(100, 7, CFW_ASSET_IMAGE, il, 0, img, il);
    m_put_object(1, 1, r1, 20);
    m_put_object(2, 1, r2, (uint32_t)(e2 - r2));
    m_put_object(3, 1, r3, 12);
    CHECK(send(37) == 0 && reply_mode() == 37 && reply_request() == 10 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    CHECK(reply_revision() == 1 && reply_extra_len() == 1 && reply_extra()[0] == 0);
    CHECK(obj(1) && obj(2) && obj(3) && asset(100) && asset(100)->refs == 1);
    CHECK(obj(2)->asset && (sc->assets[obj(2)->asset - 1].state & CFW_CACHE_ST_PRIVATE) && sc->assets[obj(2)->asset - 1].length == 5);
    CHECK(live_assets() == 2 && sc->active_count == 0 && g_presented == 0);
    CHECK(obj(1)->last_use == 1 && obj(2)->last_use == 2 && obj(3)->last_use == 3);   /* entry order */

    /* SHOW with a glide and a tween; TAG asks for the settled report */
    uint32_t ids[] = { 1, 2, 3 }, vers[] = { 1, 1, 1 };
    m_show(11, 0x01 | 0x04, 2);
    m_refs(ids, vers, 3);
    m_glide(1, 400, 0, 10, EASE);
    { int16_t v[] = { 140, 15 }; m_tween(2, 0x101, 20, LINEAR, v); }   /* x -> 140, options -> 15 */
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && reply_mode() == 38 && reply_request() == 11);
    CHECK(sc->active_count == 3 && sc->anim_active == 1 && g_timer_started == 1 && g_presented == 1 && g_settled == 0);
    CHECK((obj(1)->state & CFW_CACHE_ST_ACTIVE) && obj(1)->frames == 10 && obj(2)->frames == 20 && obj(3)->frames == 0);
    CHECK(pixel_at(sc->fb, 100, 240) == 15 && pixel_at(sc->fb, 300, 100) == 2 && pixel_at(sc->fb, 510, 110) == 12);
    CHECK(obj(1)->last_use == 4 && obj(2)->last_use == 5 && obj(3)->last_use == 6 && asset(100)->last_use == 6);
    CHECK(sc->revision == 1);                            /* a SHOW without PUTs changes no residency */
    write_pgm(dir, "cache_f00", sc->fb);

    /* rejected: unknown op, truncated tween, tween on an all-objects target, bad ref count */
    { m_show(12, 1, 0); m_refs(ids, vers, 3); g_msg[g_len++] = 11; put32(g_msg + g_len, 1); g_len += 4; CHECK(send(38) == -1); }
    { m_show(12, 1, 0); m_refs(ids, vers, 3); { int16_t v[] = { 1 }; m_tween(1, 0x1, 20, LINEAR, v); } g_len -= 1; CHECK(send(38) == -1); }
    { m_show(12, 1, 0); m_refs(ids, vers, 3); { int16_t v[] = { 1 }; m_tween(CFW_CACHE_ALL_OBJECTS, 0x1, 20, LINEAR, v); } CHECK(send(38) == -1); }
    { m_show(12, 1, 0); m_refs(ids, vers, 3); g_len -= 3; CHECK(send(38) == -1); }
    CHECK(sc->active_count == 3 && obj(1)->frames == 10);   /* nothing above was applied */

    /* drive the timer callback like the RTOS would; the display hook renders */
    int16_t last_x = 100;
    int presented_before = g_presented;
    for (int f = 1; f <= 22; f++) {
        int armed = sc->anim_active;
        g_ctx.direct_pending = 0;
        g_now += 33;
        scene_tick(&g_ctx);
        CHECK(sc->render_due == (armed ? 1 : 0));
        cfw_scene_render_if_due(&g_ctx, sc->fb);
        CHECK(sc->render_due == 0);
        CHECK(obj(1)->p[0] >= last_x);
        last_x = obj(1)->p[0];
        if (f == 10) { CHECK(obj(1)->p[0] == 500 && obj(1)->frames == 0); write_pgm(dir, "cache_f10", sc->fb); }
    }
    CHECK(obj(2)->p[0] == 140 && obj(2)->color == 15 && obj(2)->frames == 0);
    CHECK(sc->anim_active == 0 && g_presented - presented_before == 20);
    CHECK(g_settled == 1 && g_settled_tag == 11);
    CHECK(pixel_at(sc->fb, 500, 240) == 15 && pixel_at(sc->fb, 100, 240) == 2);

    /* a repeated SHOW (same request) applies nothing: the glide is not run twice */
    m_show(11, 0x01, 2); m_refs(ids, vers, 3); m_glide(1, 400, 0, 10, EASE);
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && obj(1)->frames == 0 && obj(1)->p[0] == 500);
    CHECK(obj(1)->last_use == 4);                        /* recency untouched by the retransmission */

    /* stale epoch: nothing applied, current epoch reported */
    g_epoch++;
    m_show(13, 1, 0); m_refs(ids, vers, 3); m_op1(7, CFW_CACHE_ALL_OBJECTS);
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_STALE && reply_epoch() == (uint16_t)(g_epoch - 1));
    g_epoch--;

    /* HIDE retains everything, blanks the panel, reports settled at once */
    m_begin(14); g_msg[g_len++] = 0x01 | 0x04; g_msg[g_len++] = 0;
    int blank_presents = g_presented;
    CHECK(send(39) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && g_settled == 2 && g_settled_tag == 14);
    CHECK(sc->active_count == 0 && obj(1) && obj(2) && obj(3) && !(obj(1)->state & CFW_CACHE_ST_ACTIVE));
    CHECK(g_presented == blank_presents + 1 && count_color(sc->fb, 15) == 0 && count_color(sc->fb, 12) == 0);
    CHECK(sc->revision == 1);

    /* warm reopen: one SHOW, no definitions, immediately drawn from the retained state */
    int shows = g_presented;
    m_show(15, 0x01, 2); m_refs(ids, vers, 3);
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && g_presented == shows + 1);
    CHECK(pixel_at(sc->fb, 500, 240) == 15 && pixel_at(sc->fb, 510, 110) == 12 && pixel_at(sc->fb, 150, 410) == 15);
    CHECK(sc->anim_active == 0);
    write_pgm(dir, "cache_reopen", sc->fb);

    /* paint order follows the list, not the definitions */
    uint8_t ra[20], rb[20];
    { int16_t v[] = { 200, 200, 100, 100, 0 }; rec(ra, CFW_SHAPE_RECT_FILL, 5, 0, v, 5); }
    { int16_t v[] = { 250, 250, 100, 100, 0 }; rec(rb, CFW_SHAPE_RECT_FILL, 9, 0, v, 5); }
    m_begin(16); m_put_object(20, 1, ra, 20); m_put_object(21, 1, rb, 20);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    { uint32_t o[] = { 21, 20 }, w[] = { 1, 1 }; m_show(17, 1, 0); m_refs(o, w, 2); CHECK(send(38) == 0 && pixel_at(sc->fb, 275, 275) == 5); }
    { uint32_t o[] = { 20, 21 }, w[] = { 1, 1 }; m_show(18, 1, 0); m_refs(o, w, 2); CHECK(send(38) == 0 && pixel_at(sc->fb, 275, 275) == 9); }
    CHECK(!(obj(1)->state & CFW_CACHE_ST_ACTIVE) && obj(1));   /* departed objects stay resident */

    /* KEEP: ops without resending the list; VISIBLE, MOVE, FINISH */
    m_show(19, 0x01 | 0x02, 0); m_refs(0, 0, 0); m_visible(20, 0);
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && pixel_at(sc->fb, 225, 225) == 0 && pixel_at(sc->fb, 275, 275) == 9);
    m_show(20, 0x01 | 0x02, 0); m_refs(0, 0, 0); { uint8_t *p = g_msg + g_len; p[0] = 3; put32(p + 1, 21); put16(p + 5, 100); put16(p + 7, 0); g_len += 9; }
    CHECK(send(38) == 0 && obj(21)->p[0] == 350 && pixel_at(sc->fb, 375, 275) == 9);
    { uint32_t o[] = { 20 }, w[] = { 1 }; m_show(21, 0x01 | 0x02, 0); m_refs(o, w, 1); CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_KEEP); }
    m_show(22, 0x01 | 0x02, 0); m_refs(0, 0, 0); m_glide(21, -100, 0, 10, LINEAR); m_op1(7, CFW_CACHE_ALL_OBJECTS);
    CHECK(send(38) == 0 && obj(21)->p[0] == 250 && obj(21)->frames == 0 && sc->anim_active == 0);
    /* an op on an object that is not on the list is refused as a whole */
    m_show(23, 0x01 | 0x02, 0); m_refs(0, 0, 0); m_glide(1, 10, 0, 10, LINEAR); m_glide(21, 10, 0, 10, LINEAR);
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_TARGET && obj(21)->frames == 0);
    /* tween masks are per type: an image only moves */
    { uint32_t o[] = { 3 }, w[] = { 1 }; m_show(24, 1, 0); m_refs(o, w, 1); { int16_t v[] = { 7 }; m_tween(3, 0x4, 5, LINEAR, v); }
      CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_MASK); }
    { uint32_t o[] = { 3 }, w[] = { 1 }; m_show(25, 1, 0); m_refs(o, w, 1); { int16_t v[] = { 520, 120 }; m_tween(3, 0x3, 2, LINEAR, v); }
      CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && obj(3)->frames == 2); }
    /* rotation of an image is refused before anything applies */
    { uint32_t o[] = { 3, 1 }, w[] = { 1, 1 }; m_show(26, 1, 0); m_refs(o, w, 2); m_rotate(3, 90 * 256, 0, 0, 100);
      CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && sc->active_count == 1); }
}

/* Missing and stale references never draw the wrong object. */
static void test_identity(void) {
    fresh();
    cfw_cache *sc = cfw_cache_peek(&g_ctx);
    put_circle(1, 1, 100, 100, 15);
    put_circle(2, 1, 200, 100, 15);
    uint32_t ids[] = { 1, 2 }, vers[] = { 1, 1 };
    m_show(1, 1, 0); m_refs(ids, vers, 2);
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    /* unknown id and stale version: MISSING lists both, scene unchanged */
    { uint32_t o[] = { 1, 7, 2 }, w[] = { 2, 1, 1 }; m_show(2, 1, 0); m_refs(o, w, 3);
      CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_MISSING);
      CHECK(reply_extra()[0] == 2 && reply_extra()[1] == 0 && rd32(reply_extra() + 2) == 1 && rd32(reply_extra() + 6) == 7);
      CHECK(sc->active_count == 2 && pixel_at(sc->fb, 100, 100) == 15 && obj(1)->version == 1); }
    /* same id, same version: a no-op PUT; new version replaces content in place, keeps its slot on the list */
    uint32_t rev = sc->revision;
    put_circle(1, 1, 999, 999, 15);
    CHECK(sc->revision == rev && obj(1)->p[0] == 100);
    put_circle(1, 2, 300, 300, 9);
    CHECK(sc->revision == rev + 1 && obj(1)->version == 2 && obj(1)->p[0] == 300 && (obj(1)->state & CFW_CACHE_ST_ACTIVE) && sc->active_count == 2);
    CHECK(live_objects() == 2);
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(3, 1, 0); m_refs(o, w, 1); CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_MISSING); }
    { uint32_t o[] = { 1 }, w[] = { 2 }; m_show(4, 1, 0); m_refs(o, w, 1); CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && pixel_at(sc->fb, 300, 300) == 9 && pixel_at(sc->fb, 200, 100) == 0); }
    /* a recycled descriptor cannot serve its former occupant */
    m_begin(5); g_msg[g_len++] = 1; g_msg[g_len++] = 1; put32(g_msg + g_len, 2); g_len += 4;     /* DROP 2 */
    CHECK(send(41) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && obj(2) == 0);
    put_circle(9, 1, 200, 100, 4);
    CHECK(obj(9) && obj(9) == &sc->objects[cfw_object_find(sc, 9, 0)]);
    { uint32_t o[] = { 2 }, w[] = { 1 }; m_show(6, 1, 0); m_refs(o, w, 1); CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_MISSING); }
    /* dropping an active object refuses the whole DROP; id/version 0 are refused */
    m_begin(7); g_msg[g_len++] = 1; g_msg[g_len++] = 2; put32(g_msg + g_len, 9); put32(g_msg + g_len + 4, 1); g_len += 8;
    CHECK(send(41) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_ACTIVE && obj(9));
    { uint8_t r[20]; int16_t v[] = { 1, 1, 1 }; rec(r, CFW_SHAPE_CIRCLE_FILL, 15, 0, v, 3);
      m_begin(8); m_put_object(0, 1, r, 20); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_ID);
      m_begin(8); m_put_object(5, 0, r, 20); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED);
      m_begin(8); m_put_object(5, 1, r, 20); m_put_object(5, 2, r, 20); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_DUPLICATE && obj(5) == 0); }
    /* RESET: new epoch, empty cache, the old epoch is stale from then on */
    uint16_t old = g_epoch;
    m_begin(9); put16(g_msg, 0); g_msg[g_len++] = 0;
    CHECK(send(41) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && reply_epoch() != old && live_objects() == 0 && sc->active_count == 0);
    m_begin(10); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_STALE);
    g_epoch = reply_epoch();
    m_begin(10); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    /* a RESET may carry the phone's session epoch, so both lenses share one */
    put_circle(1, 1, 1, 1, 15);
    m_begin(11); put16(g_msg, 0x4d2); g_msg[g_len++] = 0;
    CHECK(send(41) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && reply_epoch() == 0x4d2 && g_ctx.cache_epoch == 0x4d2 && live_objects() == 0);
    g_epoch = 0x4d2;
    put_circle(1, 1, 1, 1, 15); CHECK(obj(1));
    /* the reset's optional capacity byte raises the reply budget for this link */
    m_begin(12); g_msg[g_len++] = 0; g_msg[g_len++] = 200; g_ctx.cache_reply_cap = 20;
    { cfw_rectlist rl = { 0 }; CHECK(cfw_scene_dispatch(&g_ctx, 41, g_msg, g_len, 1, &rl) == 0 && g_hint == 200 && g_ctx.cache_reply_cap == 195); }
}

/* Deterministic LRU: shown objects are recent, hidden ones age, active ones are pinned. */
static void test_lru(void) {
    fresh();
    cfw_cache *sc = cfw_cache_peek(&g_ctx);
    for (uint32_t i = 1; i <= CFW_CACHE_OBJECTS; i++) put_circle(i, 1, (int)(i % 600), (int)(i % 400), 15);
    CHECK(live_objects() == CFW_CACHE_OBJECTS);
    /* show 1..8: they become the most recent; 9 is the oldest unpinned */
    uint32_t ids[8], vers[8];
    for (uint32_t i = 0; i < 8; i++) { ids[i] = i + 1; vers[i] = 1; }
    m_show(1, 1, 0); m_refs(ids, vers, 8);
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    /* a query does not refresh recency */
    m_begin(0); put32(g_msg + 2, 0); put16(g_msg + 6, 0); g_len = 8;
    CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_SNAPSHOT);
    uint32_t before9 = obj(9)->last_use;
    put_circle(1000, 1, 1, 1, 15);
    CHECK(reply_extra()[0] == 1 && obj(9) == 0 && obj(10) && obj(1000) && live_objects() == CFW_CACHE_OBJECTS);
    (void)before9;
    put_circle(1001, 1, 1, 1, 15);
    CHECK(obj(10) == 0 && obj(11));
    /* the journal reports the evictions as version 0 */
    m_begin(0); put32(g_msg + 2, sc->revision - 2); put16(g_msg + 6, 0); g_len = 8;
    CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_DELTA);
    { const uint8_t *x = reply_extra(); CHECK(x[0] == 4);
      CHECK(rd32(x + 2) == 9 && rd32(x + 6) == 0 && rd32(x + 11) == 1000 && rd32(x + 15) == 1);
      CHECK(rd32(x + 20) == 10 && rd32(x + 24) == 0 && rd32(x + 29) == 1001 && rd32(x + 33) == 1); }
    /* hide, then keep adding: the eight shown objects outlive everything older */
    m_begin(2); g_msg[g_len++] = 0; g_msg[g_len++] = 0; CHECK(send(39) == 0);
    for (uint32_t i = 0; i < 240; i++) put_circle(2000 + i, 1, 1, 1, 15);
    for (uint32_t i = 1; i <= 8; i++) CHECK(obj(i) != 0);
    CHECK(obj(11) == 0 && obj(250) == 0 && obj(2000));
    for (uint32_t i = 0; i < 14; i++) put_circle(3000 + i, 1, 1, 1, 15);   /* 251..256 first, then 1..8 */
    CHECK(obj(1) == 0 && obj(8) == 0 && obj(3000) && obj(2000) && obj(256) == 0);
    /* the active list is never evicted: fill with active objects and demand more */
    fresh(); sc = cfw_cache_peek(&g_ctx);
    uint32_t all[CFW_CACHE_OBJECTS], allv[CFW_CACHE_OBJECTS];
    for (uint32_t i = 0; i < CFW_CACHE_OBJECTS; i++) { put_circle(i + 1, 1, 1, 1, 15); all[i] = i + 1; allv[i] = 1; }
    m_show(3, 1, 0); m_refs(all, allv, CFW_CACHE_OBJECTS);
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && sc->active_count == CFW_CACHE_OBJECTS);
    { uint8_t r[20]; int16_t v[] = { 1, 1, 1 }; rec(r, CFW_SHAPE_CIRCLE_FILL, 15, 0, v, 3);
      m_begin(4); m_put_object(5000, 1, r, 20);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_CAPACITY && live_objects() == CFW_CACHE_OBJECTS && obj(5000) == 0); }
    /* replacing an active object under full occupancy needs no free descriptor beyond the transient one */
    m_begin(5); g_msg[g_len++] = 0; g_msg[g_len++] = 0; CHECK(send(39) == 0);   /* hide: all evictable */
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(6, 1, 0); m_refs(o, w, 1); CHECK(send(38) == 0); }
    put_circle(1, 2, 50, 50, 9);
    CHECK(obj(1)->version == 2 && (obj(1)->state & CFW_CACHE_ST_ACTIVE) && reply_extra()[0] == 1);   /* one LRU evicted for the staging slot */
}

/* Shared assets, replacement in place, chunked uploads and validation. */
static void test_assets(void) {
    fresh();
    cfw_cache *sc = cfw_cache_peek(&g_ctx);
    uint8_t font[4096], font2[4096], str[] = "hello", str2[] = "lohel";
    uint32_t fl = mk_font(font, "helo", 15), fl2 = mk_font(font2, "helo", 6);
    m_begin(1);
    m_put_asset(300, 1, CFW_ASSET_FONT, fl, 0, font, fl);
    m_put_asset(201, 1, CFW_ASSET_STRING, 5, 0, str, 5);
    m_put_asset(202, 1, CFW_ASSET_STRING, 5, 0, str2, 5);
    uint8_t ra[16], rb[16];
    rec_asset(ra, CFW_SHAPE_TEXT_CACHED, 15, 10, 10, 201, 300);
    rec_asset(rb, CFW_SHAPE_TEXT_CACHED, 15, 10, 100, 202, 300);
    m_put_object(1, 1, ra, 16); m_put_object(2, 1, rb, 16);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    CHECK(asset(300) && asset(300)->refs == 2 && asset(201)->refs == 1 && live_assets() == 3);
    uint32_t ids[] = { 1, 2 }, vers[] = { 1, 1 };
    m_show(2, 1, 0); m_refs(ids, vers, 2);
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    CHECK(pixel_at(sc->fb, 10, 10) == 15 && pixel_at(sc->fb, 39, 17) == 15 && pixel_at(sc->fb, 40, 10) == 0 && pixel_at(sc->fb, 10, 100) == 15);
    /* a new font version replaces in place: both objects now draw with it */
    m_begin(3); m_put_asset(300, 2, CFW_ASSET_FONT, fl2, 0, font2, fl2);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && asset(300)->version == 2 && asset(300)->refs == 2 && live_assets() == 3);
    m_show(4, 1, 0); m_refs(ids, vers, 2);
    CHECK(send(38) == 0 && pixel_at(sc->fb, 10, 10) == 6 && pixel_at(sc->fb, 10, 100) == 6);
    /* an object referencing an absent asset: MISSING names the asset, nothing is applied */
    { uint8_t rc[16]; rec_asset(rc, CFW_SHAPE_TEXT_CACHED, 15, 10, 200, 201, 301);
      m_begin(5); m_put_object(3, 1, rc, 16);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_MISSING && rd32(reply_extra() + 2) == 301 && obj(3) == 0 && live_objects() == 2); }
    /* kind mismatch: a string where a font is expected */
    { uint8_t rc[16]; rec_asset(rc, CFW_SHAPE_TEXT_CACHED, 15, 10, 200, 201, 202);
      m_begin(6); m_put_object(3, 1, rc, 16);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_KIND); }
    /* a shared asset survives its referrers leaving until nothing references it */
    m_begin(7); g_msg[g_len++] = 0; g_msg[g_len++] = 0; CHECK(send(39) == 0);
    m_begin(8); g_msg[g_len++] = 1; g_msg[g_len++] = 1; put32(g_msg + g_len, 1); g_len += 4;
    CHECK(send(41) == 0 && asset(300)->refs == 1 && asset(201) && asset(201)->refs == 0);
    /* unreferenced assets are the first to go under store pressure */
    { static uint8_t big[70000]; uint32_t bl = mk_big_image(big, 255, 255, 3);
      /* the store is 256 KiB: 4 x 65 KiB fits with ~2 KiB left, the 5th needs eviction */
      for (uint32_t i = 0; i < 4; i++) { m_begin(9); m_put_asset(400 + i, 1, CFW_ASSET_IMAGE, bl, 0, big, bl); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED); }
      CHECK(asset(201) && asset(400));
      m_begin(10); m_put_asset(404, 1, CFW_ASSET_IMAGE, bl, 0, big, bl);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && reply_extra()[0] == 0);
      CHECK(asset(201) == 0 && asset(400) == 0 && asset(401) && asset(404) && asset(300));   /* LRU unreferenced went, the referenced font stayed */
      CHECK(store_used() <= CFW_CACHE_STORE_BYTES); }
    /* chunked upload across messages; incomplete assets are not referenceable */
    fresh(); sc = cfw_cache_peek(&g_ctx);
    { uint8_t img[64]; uint32_t il = mk_image(img, 24, 24, 12);
      m_begin(11); m_put_asset(100, 1, CFW_ASSET_IMAGE, il, 0, img, 3);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && asset(100) && !(asset(100)->state & CFW_CACHE_ST_COMPLETE) && asset(100)->filled == 3);
      uint8_t r[12]; rec_asset(r, CFW_SHAPE_IMAGE, 15, 0, 0, 100, 0);
      m_begin(12); m_put_object(1, 1, r, 12);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_MISSING && rd32(reply_extra() + 2) == 100);
      m_begin(13); m_put_asset(100, 1, CFW_ASSET_IMAGE, il, 5, img + 5, il - 5);     /* wrong offset */
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_UPLOAD);
      m_begin(14); m_put_asset(100, 1, CFW_ASSET_IMAGE, il, 3, img + 3, il - 3); m_put_object(1, 1, r, 12);   /* completes + references in one message */
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && (asset(100)->state & CFW_CACHE_ST_COMPLETE) && obj(1) && asset(100)->refs == 1);
      /* invalid bytes: a 24x24 image whose RLE covers too few pixels */
      img[il - 1] = 0x11;
      m_begin(15); m_put_asset(101, 1, CFW_ASSET_IMAGE, il, 0, img, il);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_ASSET && asset(101) == 0 && live_assets() == 1);
      /* a font whose glyph offset points outside the asset */
      uint8_t bad[CFW_FONT_TABLE_BYTES + 8]; bzero(bad, sizeof bad); put32(bad + 4, 1000);
      m_begin(16); m_put_asset(102, 1, CFW_ASSET_FONT, sizeof bad, 0, bad, sizeof bad);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_ASSET);
      /* a string with a NUL, and one over 128 bytes */
      uint8_t s[130]; for (int i = 0; i < 130; i++) s[i] = 'a'; s[3] = 0;
      m_begin(17); m_put_asset(103, 1, CFW_ASSET_STRING, 5, 0, s, 5); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED);
      s[3] = 'a';
      m_begin(18); m_put_asset(103, 1, CFW_ASSET_STRING, 130, 0, s, 130); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED);
      /* private asset kind cannot be uploaded */
      m_begin(19); m_put_asset(104, 1, CFW_ASSET_EDGES, 16, 0, s, 16); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_KIND); }
    (void)sc;
}

/* The whole 256 KiB store is addressable: assets below, across and above 64 KiB. */
static void test_offsets(void) {
    fresh();
    cfw_cache *sc = cfw_cache_peek(&g_ctx);
    static uint8_t filler[70000], img[1024], font[4096], str[] = "ok";
    uint32_t il = mk_big_image(img, 24, 24, 13), fl = mk_font(font, "ok", 7);
    /* a font asset is table + glyphs + any trailing bytes, so it can be sized exactly:
     * 0x10000 - 16 bytes long, so the next asset starts at 0xfff0 and spans the boundary */
    uint32_t fill_len = 0x10000u - 16u;
    bzero(filler, fill_len);
    put32(filler + 0, CFW_FONT_TABLE_BYTES);
    mk_image(filler + CFW_FONT_TABLE_BYTES, 4, 4, 2);
    m_begin(1); m_put_asset(900, 1, CFW_ASSET_FONT, fill_len, 0, filler, 32000);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    m_begin(2); m_put_asset(900, 1, CFW_ASSET_FONT, fill_len, 32000, filler + 32000, fill_len - 32000);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && asset(900)->offset == 0 && (asset(900)->state & CFW_CACHE_ST_COMPLETE));
    m_begin(3);
    m_put_asset(100, 1, CFW_ASSET_IMAGE, il, 0, img, il);           /* starts just below 0x10000, ends above */
    m_put_asset(300, 1, CFW_ASSET_FONT, fl, 0, font, fl);           /* entirely above 0x10000 */
    m_put_asset(200, 1, CFW_ASSET_STRING, 2, 0, str, 2);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    CHECK(asset(100)->offset == 0xfff0u && asset(100)->offset + il > 0x10000u && asset(300)->offset >= 0x10000u);
    uint8_t ri[12], rt[16];
    rec_asset(ri, CFW_SHAPE_IMAGE, 15, 100, 100, 100, 0);
    rec_asset(rt, CFW_SHAPE_TEXT_CACHED, 15, 300, 300, 200, 300);
    m_begin(4); m_put_object(1, 1, ri, 12); m_put_object(2, 1, rt, 16);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
    { uint32_t o[] = { 1, 2 }, w[] = { 1, 1 }; m_show(5, 1, 0); m_refs(o, w, 2); CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED); }
    CHECK(pixel_at(sc->fb, 100, 100) == 13 && pixel_at(sc->fb, 123, 123) == 13 && pixel_at(sc->fb, 124, 100) == 0);
    CHECK(pixel_at(sc->fb, 300, 300) == 7 && pixel_at(sc->fb, 311, 307) == 7 && pixel_at(sc->fb, 312, 300) == 0);
    /* mode 19/20 reach them too */
    { cfw_rectlist rl = { 0 }; uint8_t m[9]; put32(m, 100); put16(m + 4, 0); put16(m + 6, 0); m[8] = 15; bzero(g_shadow, sizeof g_shadow);
      CHECK(cfw_cache_immediate(&g_ctx, 19, g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, m, 9, &rl) == 0 && pixel_at(g_shadow, 23, 23) == 13);
      uint8_t f[12]; put32(f, 300); put16(f + 4, 0); put16(f + 6, 40); f[8] = 15; f[9] = 2; f[10] = 'o'; f[11] = 'k';
      CHECK(cfw_cache_immediate(&g_ctx, 20, g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, f, 12, &rl) == 0 && pixel_at(g_shadow, 11, 47) == 7); }
    /* fill to exactly 256 KiB: the last allocation ends at the capacity */
    { static uint8_t blob[70000]; uint32_t bl = mk_big_image(blob, 255, 255, 1);
      uint32_t count = 0;
      while (CFW_CACHE_STORE_BYTES - store_used() >= ((bl + 15u) & ~15u) && count < 200) {
          m_begin(6); m_put_asset(1000 + count, 1, CFW_ASSET_IMAGE, bl, 0, blob, bl);
          CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && reply_extra()[0] == 0);
          count++;
      }
      CHECK(count == 3);
      uint32_t room = CFW_CACHE_STORE_BYTES - store_used();
      CHECK(room >= CFW_FONT_TABLE_BYTES + 16u && room % CFW_CACHE_GRANULE == 0 && room <= sizeof filler);
      bzero(filler, room); put32(filler, CFW_FONT_TABLE_BYTES); mk_image(filler + CFW_FONT_TABLE_BYTES, 4, 4, 2);
      uint32_t first = room > 32000u ? 32000u : room;
      m_begin(7); m_put_asset(1999, 1, CFW_ASSET_FONT, room, 0, filler, first);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
      if (first < room) { m_begin(7); m_put_asset(1999, 1, CFW_ASSET_FONT, room, first, filler + first, room - first);
                          CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED); }
      CHECK(asset(1999) && asset(1999)->offset + room == CFW_CACHE_STORE_BYTES);
      CHECK(store_used() == CFW_CACHE_STORE_BYTES);
      /* the store is full: the LRU unreferenced asset (the big filler) goes first, references stay */
      m_begin(8); m_put_asset(3000, 1, CFW_ASSET_IMAGE, bl, 0, blob, bl);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && asset(3000) && asset(900) == 0 && asset(1000) && asset(100) && asset(300));
      CHECK(asset(3000)->offset == 0); }
    /* out-of-range and wrapping lengths are refused, including UINT32_MAX */
    m_begin(9); m_put_asset(4000, 1, CFW_ASSET_IMAGE, CFW_CACHE_STORE_BYTES + 1u, 0, img, 10);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_UPLOAD);
    m_begin(9); m_put_asset(4000, 1, CFW_ASSET_IMAGE, 0xffffffffu, 0, img, 10); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED);
    m_begin(9); m_put_asset(4000, 1, CFW_ASSET_IMAGE, 100, 0xffffffffu, img, 10); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED);
    m_begin(9); m_put_asset(4000, 1, CFW_ASSET_IMAGE, 100, 96, img, 10); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED);
    m_begin(9); m_put_asset(4000, 1, CFW_ASSET_IMAGE, 100, 0, img, 0); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED);
    /* a truncated entry is malformed: NACK, no reply */
    m_begin(9); m_put_asset(4000, 1, CFW_ASSET_IMAGE, il, 0, img, il); g_len -= 1; CHECK(send(37) == -1 && g_ctx.cache_reply_len == 0);
    /* store capacity: everything referenced by the active list cannot be evicted */
    fresh(); sc = cfw_cache_peek(&g_ctx);
    { static uint8_t blob[70000]; uint32_t bl = mk_big_image(blob, 255, 255, 1); uint32_t o[4], w[4];
      for (uint32_t i = 0; i < 4; i++) {
          uint8_t r[12]; rec_asset(r, CFW_SHAPE_IMAGE, 15, 0, 0, 500 + i, 0);
          m_begin(10); m_put_asset(500 + i, 1, CFW_ASSET_IMAGE, bl, 0, blob, bl); m_put_object(50 + i, 1, r, 12);
          CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
          o[i] = 50 + i; w[i] = 1;
      }
      m_show(11, 1, 0); m_refs(o, w, 4); CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
      uint32_t used = store_used();
      m_begin(12); m_put_asset(600, 1, CFW_ASSET_IMAGE, bl, 0, blob, bl);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_CAPACITY && asset(600) == 0 && store_used() == used && sc->active_count == 4);
      CHECK(pixel_at(sc->fb, 0, 0) == 1); }
}

/* A failed message leaves the previous scene valid and reclaims what it staged. */
static void test_transactions(void) {
    fresh();
    cfw_cache *sc = cfw_cache_peek(&g_ctx);
    put_circle(1, 1, 100, 100, 15);
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(1, 1, 0); m_refs(o, w, 1); CHECK(send(38) == 0); }
    uint32_t rev = sc->revision, live = live_objects(), la = live_assets(), used = store_used(), heap = (uint32_t)g_alloc_live;
    uint8_t img[64]; uint32_t il = mk_image(img, 8, 8, 5);
    uint8_t r[64]; uint8_t *e = rec_inline(r, 0x1f, 0, 0, 0, 0, "abc");
    uint8_t ri[12]; rec_asset(ri, CFW_SHAPE_IMAGE, 15, 0, 0, 100, 0);
    /* valid entries followed by a bad one */
    m_begin(2); m_put_asset(100, 1, CFW_ASSET_IMAGE, il, 0, img, il); m_put_object(2, 1, r, (uint32_t)(e - r)); m_put_object(3, 1, ri, 12); m_put_object(3, 1, ri, 12);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && reply_extra()[0] == CFW_CACHE_REFUSE_DUPLICATE);
    CHECK(sc->revision == rev && live_objects() == live && live_assets() == la && store_used() == used && (uint32_t)g_alloc_live == heap);
    CHECK(sc->active_count == 1 && pixel_at(sc->fb, 100, 100) == 15);
    /* a SHOW whose embedded PUT is fine but whose reference is missing: nothing stays */
    m_show(3, 1, 0); m_put_asset(100, 1, CFW_ASSET_IMAGE, il, 0, img, il); m_put_object(3, 1, ri, 12); m_put_count(2);
    { uint32_t o[] = { 3, 77 }, w[] = { 1, 1 }; m_refs(o, w, 2); }
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_MISSING && rd32(reply_extra() + 2) == 77);
    CHECK(live_objects() == live && live_assets() == la && store_used() == used && sc->active_count == 1 && pixel_at(sc->fb, 100, 100) == 15);
    /* the same SHOW with a valid list: one message defines and shows */
    m_show(4, 1, 0); m_put_asset(100, 1, CFW_ASSET_IMAGE, il, 0, img, il); m_put_object(3, 1, ri, 12); m_put_count(2);
    { uint32_t o[] = { 3, 1 }, w[] = { 1, 1 }; m_refs(o, w, 2); }
    CHECK(send(38) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && sc->active_count == 2 && pixel_at(sc->fb, 3, 3) == 5 && sc->revision == rev + 1);
    /* a replaced active object whose staging fails keeps the old content */
    rev = sc->revision;
    m_begin(5); { uint8_t rr[20]; int16_t v[] = { 300, 300, 30 }; rec(rr, CFW_SHAPE_CIRCLE_FILL, 9, 0, v, 3); m_put_object(1, 2, rr, 20); } m_put_object(3, 1, ri, 12); m_put_object(8, 0, ri, 12);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_REFUSED && obj(1)->version == 1 && obj(1)->p[0] == 100 && (obj(1)->state & CFW_CACHE_ST_ACTIVE) && sc->revision == rev);
    /* no vector workspace (its heap-13 allocation failed before the gate): a path is CAPACITY, never an allocation under the gate */
    { uint8_t cmds[] = { 0, 0, 0, 0, 0, 1, 0x40, 0x06, 0, 0, 1, 0x40, 0x06, 0x40, 0x06, 4 }; uint8_t rp[64]; uint8_t *pe = rec_path(rp, 15, 0, 10, 10, 256, cmds, sizeof cmds);
      cfw_heap13_free(sc->vector_work); sc->vector_work = 0;
      g_alloc_fail_after = 0;
      m_begin(6); m_put_object(9, 1, rp, (uint32_t)(pe - rp));
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_CAPACITY && obj(9) == 0 && live_assets() == la + 1 && sc->vector_work == 0);
      g_alloc_fail_after = -1;
      m_begin(7); m_put_object(9, 1, rp, (uint32_t)(pe - rp));
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && obj(9) && sc->vector_work);
      uint32_t o[] = { 9 }, w[] = { 1 }; m_show(8, 1, 0); m_refs(o, w, 1);
      CHECK(send(38) == 0 && pixel_at(sc->fb, 50, 50) == 15 && pixel_at(sc->fb, 120, 50) == 0); }
    /* no store memory: CAPACITY, not a crash (the heap gives nothing, then only 64 KiB) */
    cfw_scene_release(&g_ctx); bzero((uint8_t *)&g_ctx, sizeof g_ctx); g_ctx.magic = CFW_CTX_MAGIC; g_epoch = 0; g_store_fail = 1;
    m_begin(1); g_msg[g_len++] = 0; CHECK(send(41) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && rd16(reply_extra() + 1) == 0); g_epoch = reply_epoch();
    sc = cfw_cache_peek(&g_ctx);
    m_begin(9); m_put_asset(100, 1, CFW_ASSET_IMAGE, il, 0, img, il);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_CAPACITY && live_assets() == 0);
    g_store_fail = 2;                                               /* only the smallest request succeeds */
    m_begin(9); m_put_asset(100, 1, CFW_ASSET_IMAGE, il, 0, img, il);
    CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && sc->store_bytes == CFW_CACHE_STORE_MIN && g_ctx.texture_cache_size == CFW_CACHE_STORE_MIN);
    { static uint8_t big[70000]; bzero(big, 70000); put32(big, CFW_FONT_TABLE_BYTES); mk_image(big + CFW_FONT_TABLE_BYTES, 4, 4, 2);
      m_begin(9); m_put_asset(101, 1, CFW_ASSET_FONT, 70000, 0, big, 30000);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_CAPACITY);          /* bigger than the whole store */
      uint32_t sl = mk_big_image(big, 200, 200, 1);
      m_begin(9); m_put_asset(101, 1, CFW_ASSET_IMAGE, sl, 0, big, sl);
      CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && store_used() <= CFW_CACHE_STORE_MIN); }
    g_store_fail = 0;
    /* lease inactive: refused at the transport level */
    g_lease = 0; m_begin(10); CHECK(send(37) == -1); g_lease = 1;
}

/* Hidden animations freeze where they are; the timer stops; the frame is not re-presented. */
static void test_hidden_animation(void) {
    fresh();
    cfw_cache *sc = cfw_cache_peek(&g_ctx);
    put_circle(1, 1, 100, 100, 15);
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(1, 1, 0); m_refs(o, w, 1); m_glide(1, 200, 0, 10, LINEAR); CHECK(send(38) == 0); }
    for (int i = 0; i < 4; i++) tick();
    int16_t mid = obj(1)->p[0];
    CHECK(mid > 100 && mid < 300 && obj(1)->frames == 10);
    int stops = g_timer_stopped;
    m_begin(2); g_msg[g_len++] = 0; g_msg[g_len++] = 0; CHECK(send(39) == 0);
    CHECK(obj(1)->frames == 0 && obj(1)->p[0] == mid && g_timer_stopped == stops + 1 && sc->anim_active == 0);
    int presented = g_presented;
    for (int i = 0; i < 4; i++) tick();
    CHECK(g_presented == presented && obj(1)->p[0] == mid);
    /* reopen: frozen geometry until the phone snaps it */
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(3, 1, 0); m_refs(o, w, 1); CHECK(send(38) == 0 && obj(1)->p[0] == mid && sc->anim_active == 0); }
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(4, 1, 0); m_refs(o, w, 1); int16_t v[] = { 300, 100 }; m_tween(1, 0x3, 0, LINEAR, v); CHECK(send(38) == 0 && obj(1)->p[0] == 300 && obj(1)->frames == 0); }
    /* a raster takeover mid-flight is an implicit hide: frozen, off the list, settle reported */
    g_settled = 0;
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(5, 1 | 4, 0); m_refs(o, w, 1); m_glide(1, -200, 0, 10, LINEAR); CHECK(send(38) == 0 && sc->anim_active); }
    tick();
    cfw_scene_takeover(&g_ctx);
    CHECK(sc->active_count == 0 && obj(1) && obj(1)->frames == 0 && g_settled == 1 && g_settled_tag == 5 && !sc->anim_active);
    CHECK(cfw_scene_resync_shadow(&g_ctx, g_container_shadow) == 0);
    /* lease lapse from the tick stops the timer without touching memory */
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(6, 1, 0); m_refs(o, w, 1); m_glide(1, 50, 0, 10, LINEAR); CHECK(send(38) == 0); }
    g_lease = 0; stops = g_timer_stopped;
    scene_tick(&g_ctx);
    CHECK(sc->anim_active == 0 && g_timer_stopped == stops + 1);
    g_lease = 1;
    /* animation with no shadow: values snap to the end */
    g_shadow_missing = 1;
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(7, 1, 0); m_refs(o, w, 1); m_glide(1, 50, 0, 10, LINEAR); CHECK(send(38) == -1); }
    g_shadow_missing = 0;
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(8, 0, 0); m_refs(o, w, 1); m_glide(1, 50, 0, 10, LINEAR); g_fail_timer = 1; g_ctx.scene_timer = 0;
      CHECK(send(38) == 0 && obj(1)->frames == 0 && !sc->anim_active); g_fail_timer = 0; }
    /* inside a bundle (present = 0): rendered into the shadow, nothing presented */
    presented = g_presented; int shadows = g_presented_shadow;
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(9, 1, 0); m_refs(o, w, 1); cfw_rectlist rl = { 0 }; g_ctx.cache_reply_cap = 245;
      CHECK(cfw_scene_dispatch(&g_ctx, 38, g_msg, g_len, 0, &rl) == 0 && g_presented == presented && g_presented_shadow == shadows && g_ctx.shadow_stale == 0);
      CHECK(count_color(g_container_shadow, 15) > 0); }
}

/* STATE: unchanged, journal deltas, paginated snapshots, reset detection. */
static void test_state(void) {
    fresh();
    cfw_cache *sc = cfw_cache_peek(&g_ctx);
    for (uint32_t i = 1; i <= 40; i++) put_circle(i, i * 10, 1, 1, 15);
    CHECK(sc->revision == 40);
    m_begin(0); put32(g_msg + 2, 40); put16(g_msg + 6, 0); g_len = 8;
    CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_UNCHANGED && reply_revision() == 40);
    /* delta of the last three mutations */
    m_begin(0); put32(g_msg + 2, 37); put16(g_msg + 6, 0); g_len = 8;
    CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_DELTA);
    { const uint8_t *x = reply_extra(); CHECK(x[0] == 3 && x[1] == 0 && rd32(x + 2) == 38 && rd32(x + 6) == 380 && rd32(x + 20) == 40 && rd32(x + 24) == 400); }
    /* too old for the journal (64 entries): snapshot pages */
    for (uint32_t i = 41; i <= 120; i++) put_circle(i, 1, 1, 1, 15);
    m_begin(0); put32(g_msg + 2, 10); put16(g_msg + 6, 0); g_len = 8;
    CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_SNAPSHOT);
    uint32_t total = rd16(reply_extra()), per = reply_extra()[4], seen = 0;
    CHECK(total == 120 && per == (245u - 10u - 5u) / 13u);
    for (uint32_t page = 0; seen < total; page++) {
        m_begin(0); put32(g_msg + 2, 10); put16(g_msg + 6, (int)page); g_len = 8;
        CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_SNAPSHOT && rd16(reply_extra() + 2) == page && reply_request() == page);
        uint32_t n = reply_extra()[4];
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = reply_extra() + 5 + 13 * i;
            uint32_t id = rd32(e + 1), ver = rd32(e + 5);
            CHECK(e[0] == 0 && id == seen + 1 && ver == (id <= 40 ? id * 10 : 1) && rd32(e + 9) == obj(id)->last_use);
            seen++;
        }
        CHECK(page < 20);
    }
    CHECK(seen == 120);
    /* a phone ahead of the firmware (rebooted cache) gets a snapshot, not a delta */
    m_begin(0); put32(g_msg + 2, 999); put16(g_msg + 6, 0); g_len = 8;
    CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_SNAPSHOT);
    /* a wrong epoch is stale; epoch 0 always answers with the current epoch */
    put16(g_msg, g_epoch + 1); CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_STALE && reply_epoch() == g_epoch);
    put16(g_msg, 0); CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_SNAPSHOT && reply_epoch() == g_epoch);
    /* assets appear in snapshots with kind 1 */
    { uint8_t img[64]; uint32_t il = mk_image(img, 4, 4, 1); m_begin(1); m_put_asset(500, 3, CFW_ASSET_IMAGE, il, 0, img, il); CHECK(send(37) == 0); }
    m_begin(0); put32(g_msg + 2, sc->revision - 1); put16(g_msg + 6, 0); g_len = 8;
    CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_DELTA && reply_extra()[0] == 1 && reply_extra()[1] == 1 && rd32(reply_extra() + 2) == 500 && rd32(reply_extra() + 6) == 3);
    /* a tiny link: no room for a reply header means no reply at all, the message still applies */
    g_ctx.cache_reply_cap = 4;
    { cfw_rectlist rl = { 0 }; m_begin(2); put_circle(200, 1, 1, 1, 15); g_ctx.cache_reply_cap = 4; g_ctx.cache_reply_len = 0;
      m_begin(2); { uint8_t r[20]; int16_t v[] = { 1, 1, 1 }; rec(r, CFW_SHAPE_CIRCLE_FILL, 15, 0, v, 3); m_put_object(201, 1, r, 20); }
      CHECK(cfw_scene_dispatch(&g_ctx, 37, g_msg, g_len, 1, &rl) == 0 && g_ctx.cache_reply_len == 0 && obj(201)); }
    /* no cache at all: RESET status with the epoch to use */
    cfw_scene_release(&g_ctx);
    m_begin(0); put32(g_msg + 2, 0); put16(g_msg + 6, 0); g_len = 8; put16(g_msg, 0);
    CHECK(send(40) == 0 && reply_status() == CFW_CACHE_REPLY_RESET && reply_epoch() == g_ctx.cache_epoch && g_ctx.scene == 0);
    g_epoch = reply_epoch();
    put_circle(1, 1, 1, 1, 15);
    CHECK(reply_epoch() == g_epoch && obj(1));                /* creation keeps the advertised epoch */
}

/* Lease loss marks the cache lost from any thread; the memory goes under the gate. */
static void test_lifecycle(void) {
    fresh();
    put_circle(1, 1, 100, 100, 15);
    { uint32_t o[] = { 1 }, w[] = { 1 }; m_show(1, 1, 0); m_refs(o, w, 1); m_glide(1, 100, 0, 10, LINEAR); CHECK(send(38) == 0); }
    uint16_t epoch = g_epoch;
    int stops = g_timer_stopped;
    cfw_cache_lose(&g_ctx);
    CHECK(g_ctx.cache_lost && g_ctx.cache_epoch != epoch && g_timer_stopped == stops + 1 && g_ctx.scene != 0);
    tick();                                                   /* the timer thread does nothing with a lost cache */
    CHECK(g_ctx.scene != 0);
    cfw_cache_reap(&g_ctx);                                   /* display task, gate held */
    CHECK(g_ctx.scene == 0 && g_ctx.texture_cache == 0 && !g_ctx.cache_lost && g_alloc_live == 0);
    m_begin(2); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_STALE && reply_epoch() == g_ctx.cache_epoch);
    /* the next handler reaps by itself */
    fresh(); put_circle(1, 1, 1, 1, 15);
    cfw_cache_lose(&g_ctx);
    g_epoch = g_ctx.cache_epoch;
    m_begin(3); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED && g_ctx.scene != 0 && obj(1) == 0);
    /* losing twice before a reap bumps once per loss; a reap with nothing lost is a no-op */
    cfw_cache_lose(&g_ctx); cfw_cache_lose(&g_ctx); cfw_cache_reap(&g_ctx); cfw_cache_reap(&g_ctx);
    CHECK(g_ctx.scene == 0);
    /* mode 11 cleanup (cfw_scene_release) frees everything and changes the epoch */
    fresh(); put_circle(1, 1, 1, 1, 15); epoch = g_epoch;
    cfw_scene_release(&g_ctx);
    CHECK(g_ctx.scene == 0 && g_ctx.texture_cache == 0 && g_ctx.cache_epoch != epoch && g_alloc_live == 0);
    cfw_scene_release(&g_ctx);                                /* idempotent */
    /* inline text object round trip through a lifecycle: bytes live in the store */
    fresh();
    { uint8_t r[64]; uint8_t *e = rec_inline(r, 0x1f, 10, 12, 25, 40, "hello");
      m_begin(4); m_put_object(3, 1, r, (uint32_t)(e - r)); CHECK(send(37) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED);
      cfw_cache *sc = cfw_cache_peek(&g_ctx);
      CHECK(obj(3)->type == CFW_SHAPE_TEXT_INLINE && obj(3)->p[4] == 5 && g_ctx.texture_cache[sc->assets[obj(3)->asset - 1].offset] == 'h');
      uint32_t o[] = { 3 }, w[] = { 1 }; m_show(5, 1, 0); m_refs(o, w, 1); m_glide(3, 100, 0, 4, LINEAR);
      CHECK(send(38) == 0 && pixel_at(sc->fb, 12, 20) == 15 && pixel_at(sc->fb, 35, 20) == 0);
      for (int f = 0; f < 4; f++) tick();
      CHECK(obj(3)->p[0] == 110 && pixel_at(sc->fb, 112, 20) == 15 && pixel_at(sc->fb, 12, 20) == 0);
      /* a SET with a bad length is rejected before anything is applied */
      r[12] = 200; m_begin(6); m_put_object(3, 2, r, (uint32_t)(e - r)); CHECK(send(37) == -1 && obj(3)->version == 1);
      cfw_scene_release(&g_ctx); CHECK(g_alloc_live == 0); }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    test_immediate(dir);
    test_easing();
    test_cache_basic(dir);
    test_identity();
    test_lru();
    test_assets();
    test_offsets();
    test_transactions();
    test_hidden_animation();
    test_state();
    test_lifecycle();
    cfw_scene_release(&g_ctx);
    CHECK(g_alloc_live == 0);
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}

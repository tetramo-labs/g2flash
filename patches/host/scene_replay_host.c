/*
 * Replay a demos/shapes-suite.ts payload dump through scene.c on the host.
 *
 * The suite writes every mode-37/38 message it would send, the wait between
 * messages and a label per case (`bun shapes-suite.ts --dump suite.bin`). This
 * drives the retained-scene code exactly as the firmware would — dispatch each
 * message, then run the animation timer through the wait — and reports every
 * message the firmware would reject, every tween's start/end/frames and how
 * many ticks it actually got before the next message, and any slot still
 * animating when the next case begins. No rasterizer output is checked; the
 * point is the wire format and the pacing.
 *
 *   cc -std=c11 -O1 -Wall -I.. -o /tmp/scene_replay scene_replay_host.c && /tmp/scene_replay suite.bin
 *
 * Exit status 1 when a message is rejected or an animation never advanced.
 */
#include <stdint.h>
#include "utils.c"
#include <stdio.h>
#include <stdlib.h>

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
static int g_presented;
static uint8_t g_container_shadow[IMAGE_BYTES];

static customCfwContext *peekCustomCfwContext(void) { return &g_ctx; }
int cfw_fb_lease_active(void) { return 1; }
static void rl_add(cfw_rectlist *rl, uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    if (rl && rl->n < CFW_RECT_MAX) {
        rl->r[rl->n].l = (uint16_t)l; rl->r[rl->n].t = (uint16_t)t;
        rl->r[rl->n].w = (uint16_t)w; rl->r[rl->n].h = (uint16_t)h; rl->n++;
    }
}
static void *cfw_heap13_malloc(uint32_t size) { return malloc(size); }
static void cfw_heap13_free(void *p) { free(p); }
static uint8_t *cfw_shadow_buffer(void) { return g_container_shadow; }
static void present_shadow(uint32_t w, uint32_t h, cfw_rectlist *rl) {
    (void)w; (void)h; if (rl) rl->direct_submitted = 1;
}
static int  stub_timer_start(uint32_t h, uint32_t ms) { (void)h; (void)ms; return 0; }
static int  stub_timer_stop(uint32_t h) { (void)h; return 0; }
static uint32_t stub_timer_new(void *cb, uint32_t t, void *a, void *attr) { (void)cb; (void)t; (void)a; (void)attr; return 0x1234; }
static void stub_gate_wait(void) {}
static void stub_gate_signal(void) {}
#define FW_TIMER_START stub_timer_start
#define FW_TIMER_STOP stub_timer_stop
#define FW_TIMER_NEW stub_timer_new
#define FW_DISPLAY_WAIT stub_gate_wait
#define FW_DISPLAY_SIGNAL stub_gate_signal

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
static void cfw_scene_notify_settled(customCfwContext *ctx, uint16_t tag) { (void)ctx; (void)tag; }

#include "shapes.c"
#include "scene.c"

/* ---- replay ------------------------------------------------------------------- */

static const char *type_name(uint8_t t) {
    static const char *names[] = { "none", "line", "rect", "rect_fill", "circle", "circle_fill", "tri", "tri_fill",
                                   "quad", "quad_fill", "bezier2", "bezier3", "arc", "pie", "image", "text", "text_cached", "text_inline" };
    return t <= CFW_SHAPE_TYPE_MAX ? names[t] : "?";
}

/* Per slot: the animation in flight, as seen when its message was applied. */
typedef struct {
    int active;
    uint8_t frames;
    int16_t from[CFW_SHAPE_PARAMS], to[CFW_SHAPE_PARAMS];
    uint8_t color_from, color_to, width_from, width_to;
    uint32_t ticks;        /* ticks that advanced it */
    int msg;               /* message index that started it */
} anim_state;

static anim_state g_anim[CFW_SCENE_SLOTS];
static int g_msgs, g_rejected, g_stalled, g_lines;
static char g_label[256] = "(start)";

static void describe(FILE *f, const anim_state *a, uint8_t type) {
    int first = 1;
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
        if (a->from[k] == a->to[k]) continue;
        fprintf(f, "%sp%u %d>%d", first ? "" : " ", k, a->from[k], a->to[k]);
        first = 0;
    }
    if (a->color_from != a->color_to) { fprintf(f, "%scolor %u>%u", first ? "" : " ", a->color_from, a->color_to); first = 0; }
    if (a->width_from != a->width_to) { fprintf(f, "%swidth %u>%u", first ? "" : " ", a->width_from, a->width_to); first = 0; }
    (void)type;
}

static void finish(uint32_t slot, uint8_t type, const char *how) {
    anim_state *a = &g_anim[slot];
    if (!a->active) return;
    if (a->ticks == 0) {
        printf("  STALLED %s slot %u %s: ", g_label, slot, type_name(type));
        describe(stdout, a, type);
        printf(" over %u frames got no tick before it was replaced\n", a->frames);
        g_stalled++;
    } else if (g_lines < 10) {
        printf("  slot %3u %-11s ", slot, type_name(type));
        describe(stdout, a, type);
        printf("  %u frames, %u ticks%s\n", a->frames, a->ticks, how);
        g_lines++;
    } else if (g_lines == 10) {
        printf("  ...\n");
        g_lines++;
    }
    a->active = 0;
}

static void after_message(cfw_scene *sc) {
    for (uint32_t i = 0; sc && i < sc->slot_hi && i < CFW_SCENE_SLOTS; i++) {
        cfw_slot *sl = &sc->slots[i];
        anim_state *a = &g_anim[i];
        int animating = sl->type != CFW_SHAPE_NONE && sl->frames != 0;
        /* a fresh animation: frame counter reset to 0 with frames set */
        if (animating && sl->frame == 0) {
            if (a->active) finish(i, sl->type, ", cut by a new animation");
            a->active = 1;
            a->frames = sl->frames;
            a->ticks = 0;
            a->msg = g_msgs;
            for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) { a->from[k] = sl->from[k]; a->to[k] = sl->to[k]; }
            a->color_from = sl->color_from; a->color_to = sl->color_to;
            a->width_from = sl->width_from; a->width_to = sl->width_to;
        } else if (!animating && a->active) {
            finish(i, sl->type, ", ended by a SET/DELETE");
        }
    }
}

static void run_wait(cfw_scene *sc, uint32_t ms) {
    if (sc == 0) return;
    uint32_t ticks = ms / sc->period_ms;
    for (uint32_t t = 0; t < ticks; t++) {
        if (!sc->anim_active) break;
        uint8_t before[CFW_SCENE_SLOTS];
        for (uint32_t i = 0; i < CFW_SCENE_SLOTS; i++) before[i] = sc->slots[i].frames;
        g_ctx.direct_pending = 0;
        g_now += sc->period_ms;
        scene_tick(&g_ctx);
        if (sc->fb) cfw_scene_render_if_due(&g_ctx, sc->fb);
        for (uint32_t i = 0; i < CFW_SCENE_SLOTS; i++) {
            if (before[i] == 0 || !g_anim[i].active) continue;
            g_anim[i].ticks++;
            if (sc->slots[i].frames == 0) finish(i, sc->slots[i].type, "");
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s suite.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (fread(data, 1, (size_t)size, f) != (size_t)size) { perror("read"); return 2; }
    fclose(f);

    bzero((uint8_t *)&g_ctx, sizeof g_ctx);
    g_ctx.magic = CFW_CTX_MAGIC;

    long pos = 0;
    while (pos < size) {
        uint8_t kind = data[pos++];
        if (kind == 3) {
            uint8_t n = data[pos++];
            cfw_scene *sc = cfw_scene_peek(&g_ctx);
            for (uint32_t i = 0; sc && i < CFW_SCENE_SLOTS; i++)
                if (g_anim[i].active && sc->slots[i].frames)
                    printf("  note: slot %u still animating (%u of %u frames) as the next case starts\n",
                           i, sc->slots[i].frame, sc->slots[i].frames);
            snprintf(g_label, sizeof g_label, "%.*s", n, (const char *)data + pos);
            pos += n;
            g_lines = 0;
            printf("== %s\n", g_label);
        } else if (kind == 2) {
            uint32_t ms = (uint32_t)data[pos] | (uint32_t)data[pos + 1] << 8 | (uint32_t)data[pos + 2] << 16 | (uint32_t)data[pos + 3] << 24;
            pos += 4;
            run_wait(cfw_scene_peek(&g_ctx), ms);
        } else if (kind == 1) {
            uint32_t len = (uint32_t)data[pos] | (uint32_t)data[pos + 1] << 8;
            pos += 2;
            const uint8_t *msg = data + pos;
            pos += len;
            g_msgs++;
            cfw_rectlist rl;
            rl.n = 0;
            rl.direct_submitted = 0;
            int r = cfw_scene_dispatch(&g_ctx, g_container_shadow, msg[0], msg + 1, len - 1, 1, &rl);
            if (r != 0) {
                g_rejected++;
                printf("  REJECTED %s message %d (mode %u, %u bytes):", g_label, g_msgs, msg[0], len);
                for (uint32_t i = 0; i < len && i < 24; i++) printf(" %02x", msg[i]);
                printf("%s\n", len > 24 ? " ..." : "");
                continue;
            }
            after_message(cfw_scene_peek(&g_ctx));
        } else {
            fprintf(stderr, "bad record kind %u at %ld\n", kind, pos - 1);
            return 2;
        }
    }
    printf("%d messages, %d rejected, %d animations stalled, %d frames presented\n", g_msgs, g_rejected, g_stalled, g_presented);
    return (g_rejected || g_stalled) ? 1 : 0;
}

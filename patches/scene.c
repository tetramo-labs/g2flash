#include <stdint.h>
#include "cfw_context.h"
#include "debug.h"
#include "malloc.h"
#include "shapes.h"
#include "scene.h"
#include "vector.c"

/* ---- Retained scene ----------------------------------------------------------
 *
 * Wire format (mode 37):  [37][flags:u8][bg:u8][record]...
 *   flags bit 0 COMMIT  render every visible slot into the scene frame and present it
 *         bit 1 CLEAR   empty all slots first
 *         bit 2 FREEZE  stop every running animation at its current value first
 *   bg    4-bit gray the frame is cleared to before slots are painted
 *   records, applied in order after the whole message validates:
 *     [0][slot][shape record]                        SET     define or replace a slot
 *                                                    (20 bytes, or 13 + len for TEXT_INLINE)
 *     [1][slot]                                      DELETE
 *     [2][slot][visible:u8]                          SHOW/HIDE
 *     [3][slot][dx:i16][dy:i16]                      MOVE    translate immediately
 *     [4][slot][dx:i16][dy:i16][frames:u8][curve x4] GLIDE   eased translate over frames
 *     [5][slot][mask:u16][frames:u8][curve x4][value:i16 per set mask bit]
 *                                                    TWEEN   mask bits 0-7 = p0..p7,
 *                                                            bit 8 = color, bit 9 = width
 *     [6][slot]                                      FREEZE  stop, keep the current value
 *     [7][slot]                                      FINISH  stop, jump to the end value
 *     [8][slot][visible][color][fillrule][x:i16][y:i16][scaleQ8:u16][len:u16][path...]
 *                                                    SET_PATH (revision 21)
 *     [9][slot][angleQ8:i32][pivotX:i16][pivotY:i16][durationMs:u16][curve x4]
 *                                                    ROTATE (revision 21)
 *   Path grammar, limits, atomic replacement and rotation semantics are in
 *   VECTOR_PROTOCOL.md. Ops 8/9 require a concrete slot. A path is filled,
 *   with p0/p1 = translation and p2 = uniform Q8 scale for GLIDE/TWEEN.
 *   slot 255 addresses every slot for ops 1, 2, 3, 4, 6 and 7.
 *   A glide on a slot that is already gliding composes: the remaining motion
 *   plus the new delta becomes one fresh eased move. A SET clears its slot's
 *   animation. Curves are CSS cubic-bezier(x1,y1,x2,y2) control points in
 *   1/255 units; frames <= 1 applies the change immediately.
 *
 * Mode 38:  [38][sub]...
 *   0            freeze all animations
 *   1 [ms:u8]    frame period 10..250 ms (default 33)
 *   2            release the scene (slots + frame buffer)
 *   3            finish all animations and present the final frame
 *
 * Threads: the EvenHub worker owns the display gate while it patches or renders
 * the scene. Animation frames are paced by a CFW osTimer; the callback takes
 * the same gate, advances every animating slot, queues the scene frame for the
 * display task and lets display_copy_hook re-render it right before the copy,
 * so the timer thread itself never rasterizes. The scene never touches EvenHub
 * container memory, so a torn-down layout cannot be corrupted by a late frame.
 * If heap 13 cannot spare the 150 KiB frame, a COMMIT still draws the scene
 * into the container shadow on the EvenHub task (as safe as mode 36) and only
 * animation is refused, so shapes keep working under memory pressure. */

#define CFW_SCENE_FLAG_COMMIT 0x01u
#define CFW_SCENE_FLAG_CLEAR  0x02u
#define CFW_SCENE_FLAG_FREEZE 0x04u

#define CFW_SCENE_OP_SET     0u
#define CFW_SCENE_OP_DELETE  1u
#define CFW_SCENE_OP_SHOW    2u
#define CFW_SCENE_OP_MOVE    3u
#define CFW_SCENE_OP_GLIDE   4u
#define CFW_SCENE_OP_TWEEN   5u
#define CFW_SCENE_OP_FREEZE  6u
#define CFW_SCENE_OP_FINISH  7u
#define CFW_SCENE_OP_PATH    8u
#define CFW_SCENE_OP_ROTATE  9u
#define CFW_SCENE_ALL_SLOTS  255u

#define CFW_TWEEN_COLOR_BIT  0x100u
#define CFW_TWEEN_WIDTH_BIT  0x200u
#define CFW_TWEEN_MASK_VALID 0x3ffu

/* --- easing (Q15, no division by 64-bit values) ------------------------------ */

static uint32_t cfw_ease_byte_q15(uint8_t b) {
    return ((uint32_t)b * 32768u + 127u) / 255u;
}

/* One axis of a cubic bezier with P0 = 0 and P3 = 1: 3(1-t)^2 t p1 + 3(1-t) t^2 p2 + t^3. */
static uint32_t cfw_ease_axis(uint32_t t, uint32_t p1, uint32_t p2) {
    uint32_t mt = 32768u - t;
    uint64_t a = ((uint64_t)3u * mt * mt) >> 15;
    a = (a * t) >> 15;
    uint64_t b = ((uint64_t)3u * mt * t) >> 15;
    b = (b * t) >> 15;
    uint64_t c = ((uint64_t)t * t) >> 15;
    c = (c * t) >> 15;
    return (uint32_t)((a * p1 + b * p2 + (c << 15)) >> 15);
}

/* Progress (Q15) at normalized time u (Q15) along the curve: solve x(t) = u by
 * bisection, then evaluate y(t). */
static uint32_t cfw_ease_progress(uint32_t u, const uint8_t *curve) {
    if (u == 0) return 0;
    if (u >= 32768u) return 32768u;
    uint32_t x1 = cfw_ease_byte_q15(curve[0]), y1 = cfw_ease_byte_q15(curve[1]);
    uint32_t x2 = cfw_ease_byte_q15(curve[2]), y2 = cfw_ease_byte_q15(curve[3]);
    uint32_t lo = 0, hi = 32768u;
    for (uint32_t i = 0; i < 14; i++) {
        uint32_t mid = (lo + hi) >> 1;
        if (cfw_ease_axis(mid, x1, x2) < u) lo = mid; else hi = mid;
    }
    uint32_t y = cfw_ease_axis((lo + hi) >> 1, y1, y2);
    return y > 32768u ? 32768u : y;
}

static int32_t cfw_lerp_q15(int32_t from, int32_t to, uint32_t prog) {
    int32_t d = to - from;
    return from + (int32_t)(((int64_t)d * (int32_t)prog + 16384) >> 15);
}

static int16_t cfw_sat16(int32_t v) {
    return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

static uint8_t cfw_sat8(int32_t v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* --- storage ------------------------------------------------------------------ */

static cfw_scene *cfw_scene_peek(customCfwContext *ctx) {
    cfw_scene *sc = ctx ? ctx->scene : 0;
    return (sc && sc->magic == CFW_SCENE_MAGIC) ? sc : 0;
}

static cfw_scene *cfw_scene_get(customCfwContext *ctx) {
    cfw_scene *sc = cfw_scene_peek(ctx);
    if (sc) return sc;
    sc = (cfw_scene *)cfw_heap13_malloc(sizeof(cfw_scene));
    if (sc == 0) return 0;
    bzero((uint8_t *)sc, sizeof(cfw_scene));
    sc->magic = CFW_SCENE_MAGIC;
    sc->period_ms = CFW_SCENE_DEFAULT_PERIOD;
    ctx->scene = sc;
    return sc;
}

static uint8_t *cfw_scene_frame(cfw_scene *sc) {
    if (sc->fb == 0) {
        sc->fb = (uint8_t *)cfw_heap13_malloc(IMAGE_BYTES);
        if (sc->fb) bzero(sc->fb, IMAGE_BYTES);
    }
    return sc->fb;
}

static void cfw_slot_clear(cfw_slot *sl) {
    if (sl->path) cfw_heap13_free(sl->path);
    bzero((uint8_t *)sl, sizeof(*sl));
}

/* Stop pacing frames. Safe from any thread, including the timer callback. */
static void cfw_scene_stop(customCfwContext *ctx) {
    cfw_scene *sc = cfw_scene_peek(ctx);
    if (sc) sc->anim_active = 0;
    if (ctx && ctx->scene_timer) FW_TIMER_STOP(ctx->scene_timer);
}

/* Free everything. Only from contexts that own the display gate (mode 11
 * cleanup, mode 38), so no queued frame can still point at the buffer. */
static void cfw_scene_release(customCfwContext *ctx) {
    cfw_scene_stop(ctx);
    cfw_scene *sc = cfw_scene_peek(ctx);
    if (sc == 0) { if (ctx) ctx->scene = 0; return; }
    ctx->scene = 0;
    sc->magic = 0;
    if (sc->fb) cfw_heap13_free(sc->fb);
    for (uint32_t i=0;i<sc->slot_hi;i++) cfw_slot_clear(&sc->slots[i]);
    if (sc->vector_work) cfw_heap13_free(sc->vector_work);
    cfw_heap13_free(sc);
}

/* --- rendering ---------------------------------------------------------------- */

static void cfw_scene_render(cfw_scene *sc, uint8_t *fb, cfw_rectlist *rl) {
    cfw_raster r = { fb, IMAGE_STRIDE, (int32_t)IMAGE_W, (int32_t)IMAGE_H };
    cfw_raster_clear(&r, sc->bg);
    for (uint32_t i = 0; i < sc->slot_hi && i < CFW_SCENE_SLOTS; i++) {
        const cfw_slot *sl = &sc->slots[i];
        if (sl->type == CFW_SHAPE_NONE || !(sl->flags & CFW_SHAPE_FLAG_VISIBLE)) continue;
        cfw_shape s;
        s.type = sl->type;
        s.flags = sl->flags;
        s.color = sl->color;
        s.width = sl->width;
        for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) s.p[k] = sl->p[k];
        s.text = sc->text[i];
        if (sl->path && sc->vector_work)
            cv_path_draw(&r, sc->vector_work, sl->path, sl->p, &sl->rotation, sl->color);
        else if (sc->vector_work)
            cv_shape_draw(&r, sc->vector_work, &s, &sl->rotation, rl);
        else cfw_shape_draw(&r, &s, rl);
    }
    rl_add(rl, 0, 0, IMAGE_W, IMAGE_H);
}

/* Called by display_copy_hook (display task) while the gate is held for the
 * queued scene frame. */
static void cfw_scene_render_if_due(customCfwContext *ctx, uint8_t *fb) {
    cfw_scene *sc = cfw_scene_peek(ctx);
    if (sc == 0 || !sc->render_due || fb == 0 || fb != sc->fb) return;
    sc->render_due = 0;
    cfw_rectlist rl;
    rl.n = 0;
    rl.direct_submitted = 0;
    cfw_scene_render(sc, fb, &rl);
}

/* --- animation ----------------------------------------------------------------- */

static void cfw_slot_snap_end(cfw_slot *sl) {
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) sl->p[k] = sl->to[k];
    sl->color = sl->color_to;
    sl->width = sl->width_to;
    sl->frames = 0;
    sl->frame = 0;
}

static void cfw_rotation_advance(cfw_rotation *t, uint32_t now) {
    if (!t->duration) return;
    uint32_t elapsed=now-t->started;
    if (elapsed>=t->duration) {t->angle=t->to;t->duration=0;return;}
    uint32_t progress=cfw_ease_progress(elapsed*32768u/t->duration,t->curve);
    t->angle=cfw_lerp_q15(t->from,t->to,progress);
}

static void cfw_slot_finish(cfw_slot *sl) {
    if(sl->frames)cfw_slot_snap_end(sl);
    if(sl->rotation.duration){sl->rotation.angle=sl->rotation.to;sl->rotation.duration=0;}
}

/* Advance every animating slot by one frame. Returns 1 while any is still moving. */
static int cfw_scene_advance(cfw_scene *sc) {
    int moving = 0;
    uint32_t now=FW_MS_TICK;
    for (uint32_t i = 0; i < sc->slot_hi && i < CFW_SCENE_SLOTS; i++) {
        cfw_slot *sl = &sc->slots[i];
        cfw_rotation_advance(&sl->rotation, now);
        if (sl->rotation.duration) moving=1;
        if (sl->type == CFW_SHAPE_NONE || sl->frames == 0) continue;
        uint32_t frame = (uint32_t)sl->frame + 1u;
        if (frame >= sl->frames) {
            cfw_slot_snap_end(sl);
            continue;
        }
        uint32_t prog = cfw_ease_progress(frame * 32768u / sl->frames, sl->curve);
        for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++)
            sl->p[k] = cfw_sat16(cfw_lerp_q15(sl->from[k], sl->to[k], prog));
        sl->color = cfw_sat8(cfw_lerp_q15(sl->color_from, sl->color_to, prog));
        sl->width = cfw_sat8(cfw_lerp_q15(sl->width_from, sl->width_to, prog));
        sl->frame = (uint8_t)frame;
        moving = 1;
    }
    sc->anim_active = (uint8_t)moving;
    return moving;
}

/* Begin an eased move of a slot from its current values to `to` (geometry),
 * `color_to` and `width_to` over `frames`. */
static void cfw_slot_animate(cfw_slot *sl, const int16_t *to, uint8_t color_to,
                             uint8_t width_to, uint8_t frames, const uint8_t *curve) {
    if (frames <= 1u) {
        for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) sl->p[k] = to[k];
        sl->color = color_to;
        sl->width = width_to;
        sl->frames = 0;
        sl->frame = 0;
        return;
    }
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
        sl->from[k] = sl->p[k];
        sl->to[k] = to[k];
    }
    sl->color_from = sl->color;
    sl->color_to = color_to;
    sl->width_from = sl->width;
    sl->width_to = width_to;
    sl->frame = 0;
    sl->frames = frames;
    for (uint32_t k = 0; k < 4; k++) sl->curve[k] = curve[k];
}

static void cfw_slot_glide(cfw_slot *sl, int32_t dx, int32_t dy, uint8_t frames, const uint8_t *curve) {
    uint8_t xm, ym;
    cfw_shape_masks(sl->type, &xm, &ym);
    if (sl->type==CFW_SHAPE_PATH) {xm=1;ym=2;}
    int16_t to[CFW_SHAPE_PARAMS];
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
        /* compose with an in-flight glide: continue from the current value toward
         * the old target plus the new delta */
        int32_t base = sl->frames ? sl->to[k] : sl->p[k];
        if (xm & (1u << k)) base += dx;
        if (ym & (1u << k)) base += dy;
        to[k] = cfw_sat16(base);
    }
    uint8_t c = sl->frames ? sl->color_to : sl->color;
    uint8_t w = sl->frames ? sl->width_to : sl->width;
    cfw_slot_animate(sl, to, c, w, frames, curve);
}

static void cfw_slot_freeze(cfw_slot *sl) {
    sl->frames = 0;
    sl->frame = 0;
    cfw_rotation_advance(&sl->rotation, FW_MS_TICK);
    sl->rotation.duration=0;
}

/* --- mode 37 ---------------------------------------------------------------------- */

static uint32_t cfw_popcount10(uint32_t v) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 10; i++) n += (v >> i) & 1u;
    return n;
}

/* Byte length of the record at `p`, or 0 if it is malformed / truncated. */
static uint32_t cfw_scene_record_len(const uint8_t *p, uint32_t avail) {
    if (avail < 2u) return 0;
    uint32_t op = p[0], slot = p[1], need;
    switch (op) {
    case CFW_SCENE_OP_PATH:
        if(avail<13 || p[4]>1 || p[3]>15 || (p[2]&~1u) || rd16(p+9)>2048) return 0;
        need=13u+rd16(p+11);
        if(need==13 || need>13+CFW_PATH_MAX_BYTES || slot==255) return 0;
        break;
    case CFW_SCENE_OP_ROTATE: {
        if(avail<16 || slot==255) return 0;
        int32_t angle=(int32_t)rd32(p+2);
        /* Bound differences so lerp and all trig arithmetic stay defined. */
        if(angle < -360*256*100 || angle > 360*256*100) return 0;
        need=16;break;
    }
    case CFW_SCENE_OP_SET: {
        uint32_t rec = cfw_shape_record_len(p + 2, avail - 2u);
        if (rec == 0) return 0;
        need = 2u + rec;
        break;
    }
    case CFW_SCENE_OP_DELETE:
    case CFW_SCENE_OP_FREEZE:
    case CFW_SCENE_OP_FINISH: need = 2u; break;
    case CFW_SCENE_OP_SHOW:   need = 3u; break;
    case CFW_SCENE_OP_MOVE:   need = 6u; break;
    case CFW_SCENE_OP_GLIDE:  need = 11u; break;
    case CFW_SCENE_OP_TWEEN: {
        if (avail < 9u) return 0;
        uint32_t mask = rd16(p + 2);
        if (mask == 0 || (mask & ~CFW_TWEEN_MASK_VALID)) return 0;
        need = 9u + 2u * cfw_popcount10(mask);
        break;
    }
    default: return 0;
    }
    if (need > avail) return 0;
    if (slot >= CFW_SCENE_SLOTS && slot != CFW_SCENE_ALL_SLOTS) return 0;
    if (slot == CFW_SCENE_ALL_SLOTS && (op == CFW_SCENE_OP_SET || op == CFW_SCENE_OP_TWEEN)) return 0;
    if (op == CFW_SCENE_OP_SET) {
        cfw_shape s;
        cfw_shape_decode(p + 2, &s);
        if (!cfw_shape_valid(&s)) return 0;
    }
    return need;
}

static void cfw_scene_apply_one(cfw_scene *sc, cfw_slot *sl, const uint8_t *p) {
    uint32_t op = p[0];
    switch (op) {
    case CFW_SCENE_OP_DELETE:
        cfw_slot_clear(sl);
        break;
    case CFW_SCENE_OP_SHOW:
        if (p[2]) sl->flags |= CFW_SHAPE_FLAG_VISIBLE;
        else      sl->flags &= (uint8_t)~CFW_SHAPE_FLAG_VISIBLE;
        break;
    case CFW_SCENE_OP_MOVE:
        cfw_slot_glide(sl, (int16_t)rd16(p + 2), (int16_t)rd16(p + 4), 0, 0);
        break;
    case CFW_SCENE_OP_GLIDE:
        cfw_slot_glide(sl, (int16_t)rd16(p + 2), (int16_t)rd16(p + 4), p[6], p + 7);
        break;
    case CFW_SCENE_OP_TWEEN: {
        uint32_t mask = rd16(p + 2);
        uint8_t frames = p[4];
        const uint8_t *curve = p + 5;
        const uint8_t *v = p + 9;
        int16_t to[CFW_SHAPE_PARAMS];
        for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
            to[k] = sl->p[k];
            if (mask & (1u << k)) { to[k] = (int16_t)rd16(v); v += 2; }
        }
        uint8_t c = sl->color, w = sl->width;
        if (mask & CFW_TWEEN_COLOR_BIT) { c = (uint8_t)rd16(v); v += 2; }
        if (mask & CFW_TWEEN_WIDTH_BIT) { w = (uint8_t)rd16(v); v += 2; }
        cfw_slot_animate(sl, to, c, w, frames, curve);
        break;
    }
    case CFW_SCENE_OP_FREEZE:
        cfw_slot_freeze(sl);
        break;
    case CFW_SCENE_OP_FINISH:
        cfw_slot_finish(sl);
        break;
    case CFW_SCENE_OP_ROTATE: {
        cfw_rotation *t=&sl->rotation;
        cfw_rotation_advance(t,FW_MS_TICK);
        t->from=t->angle;t->to=(int32_t)rd32(p+2);
        t->px=(int16_t)rd16(p+6);t->py=(int16_t)rd16(p+8);
        t->duration=rd16(p+10);t->started=FW_MS_TICK;
        for(uint32_t k=0;k<4;k++)t->curve[k]=p[12+k];
        if(!t->duration)t->angle=t->to;
        break;
    }
    default:
        break;
    }
    (void)sc;
}

static void cfw_scene_apply(cfw_scene *sc, const uint8_t *p) {
    uint32_t op = p[0], slot = p[1];
    if (op == CFW_SCENE_OP_SET) {
        cfw_slot *sl = &sc->slots[slot];
        cfw_shape s;
        cfw_shape_decode(p + 2, &s);
        cfw_slot_clear(sl);
        sl->type = s.type;
        sl->flags = s.flags;
        sl->color = s.color;
        sl->width = s.width;
        for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) sl->p[k] = s.p[k];
        if (s.type == CFW_SHAPE_TEXT_INLINE)
            for (uint32_t k = 0; k < (uint16_t)s.p[4]; k++) sc->text[slot][k] = s.text[k];
        if (slot + 1u > sc->slot_hi) sc->slot_hi = slot + 1u;
        return;
    }
    if (slot == CFW_SCENE_ALL_SLOTS) {
        for (uint32_t i = 0; i < sc->slot_hi && i < CFW_SCENE_SLOTS; i++)
            if (sc->slots[i].type != CFW_SHAPE_NONE) cfw_scene_apply_one(sc, &sc->slots[i], p);
        return;
    }
    if (sc->slots[slot].type != CFW_SHAPE_NONE) cfw_scene_apply_one(sc, &sc->slots[slot], p);
}

static int cfw_scene_any_animating(const cfw_scene *sc) {
    for (uint32_t i = 0; i < sc->slot_hi && i < CFW_SCENE_SLOTS; i++)
        if (sc->slots[i].type != CFW_SHAPE_NONE && (sc->slots[i].frames || sc->slots[i].rotation.duration)) return 1;
    return 0;
}

/* Make sure the frame timer exists and is armed. EvenHub task only (creates
 * the osTimer lazily; it is deleted by mode 11 cleanup). Animation needs the
 * CFW-owned frame: without it the slots keep their end values instead. */
/* Keep the PC-relative scene_tick address near its caller: this clang's
 * Thumb MOVW/MOVT assembler rejects large negative local-symbol addends. */
static __attribute__((always_inline)) inline void cfw_scene_arm(customCfwContext *ctx, cfw_scene *sc) {
    if (!cfw_scene_any_animating(sc)) { sc->anim_active = 0; return; }
    if (cfw_scene_frame(sc) == 0) {
        for (uint32_t i = 0; i < sc->slot_hi && i < CFW_SCENE_SLOTS; i++)
            cfw_slot_finish(&sc->slots[i]);
        sc->anim_active = 0;
        return;
    }
    if (ctx->scene_timer == 0)
        ctx->scene_timer = FW_TIMER_NEW((void *)&scene_tick, 0, ctx, 0);
    if (ctx->scene_timer == 0) {
        for(uint32_t i=0;i<sc->slot_hi;i++)cfw_slot_finish(&sc->slots[i]);
        sc->anim_active = 0; return;
    }
    sc->anim_active = 1;
    FW_TIMER_START(ctx->scene_timer, sc->period_ms);
}

static int cfw_scene_present(customCfwContext *ctx, uint8_t *state, cfw_scene *sc,
                             cfw_rectlist *rl) {
    sc->render_due = 0;
    uint8_t *fb = cfw_scene_frame(sc);
    if (fb) {
        cfw_scene_render(sc, fb, rl);
        present_buffer(ctx, fb, rl);
        return 0;
    }
    /* no CFW frame: draw into the live container shadow instead (static only) */
    uint8_t *shadow = cfw_shadow_buffer(state);
    if (shadow == 0) return -1;
    cfw_scene_render(sc, shadow, rl);
    present_shadow(state, IMAGE_W, IMAGE_H, rl);
    return 0;
}

static int cfw_scene_patch(customCfwContext *ctx, uint8_t *state,
                           const uint8_t *src, uint32_t srclen, cfw_rectlist *rl) {
    if (srclen < 2u || !cfw_fb_lease_active()) return -1;
    uint8_t flags = src[0];
    uint8_t bg = src[1] & 0x0fu;
    /* validate every record before touching the scene */
    uint32_t pos = 2;
    while (pos < srclen) {
        uint32_t n = cfw_scene_record_len(src + pos, srclen - pos);
        if (n == 0) return -1;
        pos += n;
    }
    cfw_scene *sc = cfw_scene_get(ctx);
    if (sc == 0) return -1;
    /* Stage all new paths, simulating slot types/counts to validate dependencies
     * and the final resource budget. Failure never mutates the active scene.
     * One SET_PATH per slot per message keeps staging bounded to 128 entries. */
    cfw_path *staged[CFW_SCENE_SLOTS];
    uint16_t counts[CFW_SCENE_SLOTS];
    uint8_t types[CFW_SCENE_SLOTS];
    for(uint32_t i=0;i<CFW_SCENE_SLOTS;i++) {
        staged[i]=0;
        counts[i]=(flags&CFW_SCENE_FLAG_CLEAR)?0:(sc->slots[i].path?sc->slots[i].path->count:0);
        types[i]=(flags&CFW_SCENE_FLAG_CLEAR)?0:sc->slots[i].type;
    }
    uint32_t staged_edges=0;
    pos=2;
    while(pos<srclen) {
        const uint8_t *p=src+pos;uint32_t op=p[0],slot=p[1];
        if(op==CFW_SCENE_OP_PATH || op==CFW_SCENE_OP_ROTATE) {
            if(!sc->vector_work)sc->vector_work=(cfw_vector_work *)cfw_heap13_malloc(sizeof(cfw_vector_work));
            if(!sc->vector_work)goto fail;
        }
        if(op==CFW_SCENE_OP_PATH) {
            if(staged[slot])goto fail;
            int n=cv_compile(sc->vector_work,p+13,rd16(p+11));
            if(n<0 || staged_edges+(uint32_t)n>CFW_PATH_SCENE_EDGES)goto fail;
            cfw_path *path=(cfw_path *)cfw_heap13_malloc(sizeof(cfw_path)+(uint32_t)n*sizeof(cfw_edge));
            if(!path)goto fail;
            path->count=(uint16_t)n;path->rule=p[4];path->pad=0;
            for(int i=0;i<n;i++)path->edges[i]=sc->vector_work->edges[i];
            staged[slot]=path;counts[slot]=(uint16_t)n;types[slot]=CFW_SHAPE_PATH;
            staged_edges+=(uint32_t)n;
        } else if(op==CFW_SCENE_OP_SET) {types[slot]=p[2];counts[slot]=0;}
        else if(op==CFW_SCENE_OP_DELETE) {
            if(slot==255)for(uint32_t i=0;i<CFW_SCENE_SLOTS;i++){types[i]=0;counts[i]=0;}
            else {types[slot]=0;counts[slot]=0;}
        } else if(op==CFW_SCENE_OP_ROTATE) {
            if(!types[slot] || (types[slot]>CFW_SHAPE_PIE && types[slot]!=CFW_SHAPE_PATH))goto fail;
        } else if(op==CFW_SCENE_OP_TWEEN && types[slot]==CFW_SHAPE_PATH) {
            uint32_t mask=rd16(p+2);
            if(mask&~0x107u)goto fail;
            const uint8_t *v=p+9;
            for(uint32_t k=0;k<10;k++)if(mask&(1u<<k)) {
                int32_t value=(int16_t)rd16(v);v+=2;
                if((k==2 && (value<0 || value>2048)) || (k==8 && (value<0 || value>15)))goto fail;
            }
        }
        pos+=cfw_scene_record_len(p,srclen-pos);
    }
    {uint32_t total=0;for(uint32_t i=0;i<CFW_SCENE_SLOTS;i++)total+=counts[i];
     if(total>CFW_PATH_SCENE_EDGES)goto fail;}
    sc->bg = bg;
    if (flags & CFW_SCENE_FLAG_CLEAR) {
        for (uint32_t i = 0; i < sc->slot_hi && i < CFW_SCENE_SLOTS; i++)
            cfw_slot_clear(&sc->slots[i]);
        sc->slot_hi = 0;
    }
    if (flags & CFW_SCENE_FLAG_FREEZE)
        for (uint32_t i = 0; i < sc->slot_hi && i < CFW_SCENE_SLOTS; i++) cfw_slot_freeze(&sc->slots[i]);
    pos = 2;
    while (pos < srclen) {
        uint32_t n = cfw_scene_record_len(src + pos, srclen - pos);
        if(src[pos]==CFW_SCENE_OP_PATH) {
            uint32_t slot=src[pos+1];cfw_slot *sl=&sc->slots[slot];
            cfw_slot_clear(sl);sl->path=staged[slot];staged[slot]=0;
            sl->type=CFW_SHAPE_PATH;sl->flags=src[pos+2];sl->color=src[pos+3];
            sl->p[0]=(int16_t)rd16(src+pos+5);sl->p[1]=(int16_t)rd16(src+pos+7);sl->p[2]=(int16_t)rd16(src+pos+9);
            if(slot+1>sc->slot_hi)sc->slot_hi=slot+1;
        } else cfw_scene_apply(sc, src + pos);
        pos += n;
    }
    cfw_scene_arm(ctx, sc);
    if (flags & CFW_SCENE_FLAG_COMMIT) return cfw_scene_present(ctx, state, sc, rl);
    return 0;
fail:
    for(uint32_t i=0;i<CFW_SCENE_SLOTS;i++)if(staged[i])cfw_heap13_free(staged[i]);
    return -1;
}

static int cfw_scene_control(customCfwContext *ctx, uint8_t *state,
                             const uint8_t *src, uint32_t srclen, cfw_rectlist *rl) {
    if (srclen < 1u) return -1;
    cfw_scene *sc = cfw_scene_peek(ctx);
    switch (src[0]) {
    case 0:
        cfw_scene_stop(ctx);
        if (sc) for (uint32_t i = 0; i < sc->slot_hi && i < CFW_SCENE_SLOTS; i++) cfw_slot_freeze(&sc->slots[i]);
        return 0;
    case 1: {
        if (srclen < 2u) return -1;
        uint32_t ms = src[1];
        if (ms < CFW_SCENE_MIN_PERIOD) ms = CFW_SCENE_MIN_PERIOD;
        if (ms > CFW_SCENE_MAX_PERIOD) ms = CFW_SCENE_MAX_PERIOD;
        if (sc == 0) sc = cfw_scene_get(ctx);
        if (sc == 0) return -1;
        sc->period_ms = (uint8_t)ms;
        return 0;
    }
    case 2:
        cfw_scene_release(ctx);
        return 0;
    case 3:
        if (sc == 0 || !cfw_fb_lease_active()) return -1;
        cfw_scene_stop(ctx);
        for (uint32_t i = 0; i < sc->slot_hi && i < CFW_SCENE_SLOTS; i++)
            cfw_slot_finish(&sc->slots[i]);
        return cfw_scene_present(ctx, state, sc, rl);
    default:
        return -1;
    }
}

static int cfw_scene_dispatch(customCfwContext *ctx, uint8_t *state, uint8_t mode,
                              const uint8_t *src, uint32_t srclen,
                              int present, cfw_rectlist *rl) {
    if (!present || ctx == 0) return -1;       /* not composable inside mode 8 */
    if (mode == 37) return cfw_scene_patch(ctx, state, src, srclen, rl);
    if (mode == 38) return cfw_scene_control(ctx, state, src, srclen, rl);
    return -1;
}

/* --- frame timer ------------------------------------------------------------------ */

/* osTimer callback on the RTOS timer thread. Mirrors image_worker's gate
 * discipline: take the display gate, mutate, queue one refresh, and leave the
 * gate held until the display task consumes the job. Frames are dropped
 * rather than queued up when the display task is still busy, and any lease
 * lapse, timeout or missing scene ends the animation quietly. Non-static so
 * -O2 keeps it: osTimerNew only ever sees it as a function-pointer value. */
void scene_tick(void *arg) {
    customCfwContext *ctx = (customCfwContext *)arg;
    if (ctx == 0 || ctx->magic != CFW_CTX_MAGIC) return;
    if (!cfw_fb_lease_active()) { cfw_scene_stop(ctx); return; }
    cfw_scene *sc = cfw_scene_peek(ctx);
    if (sc == 0 || !sc->anim_active || ctx->scene_timer == 0) return;
    if (ctx->direct_pending) {                     /* display task still busy: skip a frame */
        FW_TIMER_START(ctx->scene_timer, sc->period_ms);
        return;
    }
    FW_DISPLAY_WAIT();
    if (ctx->direct_pending) {                     /* timed out; we do not own the gate */
        sc->anim_active = 0;
        return;
    }
    sc = cfw_scene_peek(ctx);                      /* re-read under the gate */
    if (sc == 0 || !sc->anim_active || sc->fb == 0) {
        if (sc) sc->anim_active = 0;
        FW_DISPLAY_SIGNAL();
        return;
    }
    int moving = cfw_scene_advance(sc);
    cfw_rectlist rl;
    rl.n = 0;
    rl.direct_submitted = 0;
    sc->render_due = 1;                            /* display_copy_hook rasterizes */
    present_buffer(ctx, sc->fb, &rl);
    if (!rl.direct_submitted) {
        sc->render_due = 0;
        FW_DISPLAY_SIGNAL();
    }
    if (moving) FW_TIMER_START(ctx->scene_timer, sc->period_ms);
    else sc->anim_active = 0;
}

#pragma once
#include <stdint.h>
#include "cfw_context.h"
#include "debug.h"
#include "vector.h"

/* ---- Retained shape scene + glide/tween animation (modes 37 and 18) --------
 *
 * The scene is a flat, phone-owned list of shape slots (paint order = slot
 * order) that the firmware can re-render on its own timer, so a phone can
 * start a glide or a tween with one small message instead of streaming frames.
 * It renders into a CFW-owned full-panel 4bpp buffer (never into the EvenHub
 * container's memory), which is then presented through the same direct-
 * framebuffer job as every other custom mode. See the mode 37/38 paragraphs at
 * the top of zlib_glue.c for the wire format. */

#define CFW_SCENE_SLOTS          128u
#define CFW_SCENE_MAGIC          0x53434e45u   /* 'SCNE' */
#define CFW_SCENE_DEFAULT_PERIOD 33u           /* ms between animation frames */
#define CFW_SCENE_MIN_PERIOD     10u
#define CFW_SCENE_MAX_PERIOD     250u
#define CFW_SCENE_TEXT_BYTES     CFW_SHAPE_INLINE_MAX   /* inline string store per slot */

typedef struct {
    uint8_t  type;          /* CFW_SHAPE_*, 0 = empty slot */
    uint8_t  flags;         /* CFW_SHAPE_FLAG_VISIBLE */
    uint8_t  color;         /* current color / options byte */
    uint8_t  width;         /* current stroke width */
    int16_t  p[8];          /* current geometry */
    int16_t  from[8];       /* tween start geometry */
    int16_t  to[8];         /* tween end geometry */
    uint8_t  frame;         /* frames already advanced */
    uint8_t  frames;        /* total frames, 0 = idle */
    uint8_t  curve[4];      /* cubic-bezier easing x1 y1 x2 y2, each /255 */
    uint8_t  color_from, color_to;
    uint8_t  width_from, width_to;
    uint8_t  pad[2];
    cfw_rotation rotation;
    cfw_path *path;         /* immutable compiled contour, owned by this slot */
} cfw_slot;

typedef struct cfw_scene_s {
    uint32_t magic;
    uint8_t *fb;            /* the owned panel shadow while the scene renders (image_buffers.c) */
    uint32_t slot_hi;       /* one past the highest slot ever written */
    uint8_t  bg;            /* background gray the scene clears to */
    uint8_t  anim_active;   /* at least one slot has frames left */
    volatile uint8_t render_due; /* display_copy_hook must re-render before copying */
    uint8_t  period_ms;     /* animation frame period */
    uint8_t  settle_pending; /* a tagged commit has not reported settled yet */
    uint16_t tag;           /* phone tag echoed in the settled report (op 10) */
    cfw_vector_work *vector_work; /* lazy, serialized by the display gate */
    cfw_slot slots[CFW_SCENE_SLOTS];
    uint8_t  text[CFW_SCENE_SLOTS][CFW_SCENE_TEXT_BYTES];  /* TEXT_INLINE bytes per slot */
} cfw_scene;

static int  cfw_scene_dispatch(customCfwContext *ctx, uint8_t mode,
                               const uint8_t *src, uint32_t srclen,
                               int present, cfw_rectlist *rl);
static void cfw_scene_stop(customCfwContext *ctx);
static void cfw_scene_takeover(customCfwContext *ctx);
static int  cfw_scene_resync_shadow(customCfwContext *ctx, uint8_t *shadow);
static void cfw_scene_notify_settled(customCfwContext *ctx, uint16_t tag);
static void cfw_scene_release(customCfwContext *ctx);
static void cfw_scene_render_if_due(customCfwContext *ctx, uint8_t *fb);
void scene_tick(void *arg);

#pragma once
#include <stdint.h>
#include "cfw_context.h"
#include "debug.h"
#include "texture_cache.h"
#include "shapes.h"
#include "vector.h"

/* ---- Retained object cache (revision 38, modes 37-41) -----------------------
 *
 * Every drawable the phone defines is a versioned OBJECT with a stable 32-bit
 * id; images, cached fonts and shared strings are versioned ASSETS. Objects
 * stay resident after the view that showed them is hidden, so reopening a
 * dashboard is one SHOW of (id, version) references instead of a resend, and
 * the least recently used unpinned objects are evicted when room is needed.
 * The displayed scene is an ordered list of active objects (paint order =
 * list order, independent of storage order); its dependency closure is pinned.
 *
 * Storage: this descriptor block lives on heap 13 (allocated lazily, released
 * by mode 11); variable data (asset bytes, inline strings, compiled path
 * edges) lives in the 256 KiB store on the EvenHub heap behind a 16-byte
 * granule bitmap allocator. Descriptors are reusable; a recycled index can
 * never satisfy a reference to its former occupant because references carry
 * (id, version) and are validated against the descriptor before use.
 *
 * Wire formats, replies and the transaction/eviction rules are documented at
 * the top of scene.c. */

#define CFW_CACHE_MAGIC          0x43414348u   /* 'CACH' */
#define CFW_CACHE_OBJECTS        256u
#define CFW_CACHE_ASSETS         256u
#define CFW_CACHE_STORE_BYTES    CFW_TEXTURE_CACHE_SIZE   /* the store's maximum; the heap decides (scene.c) */
#define CFW_CACHE_STORE_MIN      (64u * 1024u)
#define CFW_CACHE_GRANULE        16u
#define CFW_CACHE_GRANULES       (CFW_CACHE_STORE_BYTES / CFW_CACHE_GRANULE)
#define CFW_CACHE_JOURNAL        64u
#define CFW_CACHE_TXN_CHUNKS     32u           /* asset chunks applied per message */
#define CFW_CACHE_ALL_OBJECTS    0xffffffffu   /* op target: every active object */
#define CFW_SCENE_DEFAULT_PERIOD 33u           /* ms between animation frames */
#define CFW_SCENE_MIN_PERIOD     10u
#define CFW_SCENE_MAX_PERIOD     250u

/* asset kinds */
#define CFW_ASSET_IMAGE   0u
#define CFW_ASSET_FONT    1u
#define CFW_ASSET_STRING  2u
#define CFW_ASSET_EDGES   3u   /* compiled path; private to one object */

/* descriptor state bits */
#define CFW_CACHE_ST_COMPLETE 0x01u  /* asset: every byte arrived and validated */
#define CFW_CACHE_ST_STAGED   0x02u  /* created by the transaction in progress */
#define CFW_CACHE_ST_PRIVATE  0x04u  /* asset: owned by exactly one object, no id */
#define CFW_CACHE_ST_ACTIVE   0x08u  /* object: on the active list */
#define CFW_CACHE_ST_PINNED   0x10u  /* object: referenced by the transaction in progress */

typedef struct {
    uint32_t id;            /* 0 = free (private assets are also 0: reached via their object) */
    uint32_t version;
    uint32_t offset;        /* into the store, granule aligned */
    uint32_t length;        /* total bytes */
    uint32_t filled;        /* upload progress; == length once complete */
    uint32_t last_use;
    uint16_t refs;          /* objects holding this asset */
    uint16_t replaces;      /* staged: index + 1 of the descriptor it supersedes */
    uint8_t  kind;
    uint8_t  state;
    uint8_t  pad[2];
} cfw_asset;

typedef struct {
    uint32_t id;            /* 0 = free */
    uint32_t version;
    uint32_t last_use;
    uint16_t asset;         /* index + 1 of the image/string/edge asset, 0 = none */
    uint16_t font;          /* index + 1 of the font asset, 0 = none */
    uint16_t replaces;      /* staged: index + 1 of the descriptor it supersedes */
    uint8_t  type;          /* CFW_SHAPE_* */
    uint8_t  flags;         /* CFW_SHAPE_FLAG_VISIBLE */
    uint8_t  color;         /* current color / options byte */
    uint8_t  width;         /* current stroke width (fill rule for PATH) */
    uint8_t  state;
    uint8_t  frame;         /* frames already advanced */
    uint8_t  frames;        /* total frames, 0 = idle */
    uint8_t  curve[4];      /* cubic-bezier easing x1 y1 x2 y2, each /255 */
    uint8_t  color_from, color_to;
    uint8_t  width_from, width_to;
    uint8_t  pad[3];
    int16_t  p[8];          /* current geometry */
    int16_t  from[8];       /* tween start geometry */
    int16_t  to[8];         /* tween end geometry */
    cfw_rotation rotation;
} cfw_object;

typedef struct {
    uint32_t id;
    uint32_t version;       /* 0 = evicted or dropped */
    uint32_t revision;      /* cache revision that recorded it */
} cfw_journal_entry;

/* One message's staging state. Lives in the cache block, not on the stack:
 * the bridge task that runs the right lens's handlers has less stack than
 * the BLE task, and this is about 1.3 KiB. */
typedef struct {
    struct cfw_cache_s *sc;
    uint8_t *store;
    uint32_t evicted;
    uint8_t status;              /* CFW_CACHE_REPLY_* once failed; APPLIED while fine */
    uint8_t reason;
    uint8_t missing_count, missing_more;
    uint32_t missing[16];
    uint16_t staged_assets[CFW_CACHE_ASSETS];   /* in entry order, index + 1 */
    uint16_t staged_objects[CFW_CACHE_OBJECTS];
    uint16_t staged_asset_count, staged_object_count;
    uint32_t chunk_count;
    struct { uint16_t asset; uint16_t len; } chunks[CFW_CACHE_TXN_CHUNKS];
} cfw_txn;

typedef struct cfw_cache_s {
    uint32_t magic;
    uint8_t *fb;            /* the owned panel shadow while the scene renders (image_buffers.c) */
    uint8_t  bg;            /* background gray the scene clears to */
    uint8_t  anim_active;   /* at least one active object has frames left */
    volatile uint8_t render_due; /* display_copy_hook must re-render before copying */
    uint8_t  period_ms;     /* animation frame period */
    uint8_t  settle_pending; /* a tagged SHOW has not reported settled yet */
    uint8_t  last_show_valid; /* last_show_request holds the last applied SHOW/HIDE */
    uint16_t tag;           /* request echoed in the settled report (field 129) */
    uint16_t last_show_request;
    uint16_t active_count;
    uint32_t store_bytes;   /* capacity of the store this cache addresses (granules * 16) */
    uint32_t revision;      /* mutation counter, journaled */
    uint32_t use_clock;     /* recency counter; advances per accepted logical use */
    uint32_t journal_lost_rev; /* highest revision overwritten out of the journal ring */
    uint8_t  journal_head, journal_count, pad0[2];
    cfw_vector_work *vector_work; /* lazy, serialized by the display gate */
    cfw_txn txn;            /* the message being applied (handlers run one at a time) */
    uint16_t next[CFW_CACHE_OBJECTS]; /* SHOW: the incoming active list */
    uint32_t bitmap[CFW_CACHE_GRANULES / 32u];
    uint16_t active[CFW_CACHE_OBJECTS];
    cfw_journal_entry journal[CFW_CACHE_JOURNAL];
    cfw_asset assets[CFW_CACHE_ASSETS];
    cfw_object objects[CFW_CACHE_OBJECTS];
} cfw_cache;

static int  cfw_scene_dispatch(customCfwContext *ctx, uint8_t mode,
                               const uint8_t *src, uint32_t srclen,
                               int present, cfw_rectlist *rl);
static int  cfw_cache_immediate(customCfwContext *ctx, uint8_t mode, uint8_t *shadow,
                                uint32_t stride, uint32_t panel_w, uint32_t panel_h,
                                const uint8_t *src, uint32_t srclen, cfw_rectlist *rl);
static void cfw_scene_stop(customCfwContext *ctx);
static void cfw_scene_takeover(customCfwContext *ctx);
static int  cfw_scene_resync_shadow(customCfwContext *ctx, uint8_t *shadow);
static void cfw_scene_notify_settled(customCfwContext *ctx, uint16_t tag);
static void cfw_scene_release(customCfwContext *ctx);
static void cfw_scene_render_if_due(customCfwContext *ctx, uint8_t *fb);
static void cfw_cache_lose(customCfwContext *ctx);
static void cfw_cache_prepare(customCfwContext *ctx, uint8_t mode);
/* message_transport.c: raise the reply capacity of the ingress streams. */
static void cfw_message_reply_capacity_hint(uint8_t capacity);
static void cfw_cache_reap(customCfwContext *ctx);
void scene_tick(void *arg);

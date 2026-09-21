#include <stdint.h>
#include "cfw_context.h"
#include "debug.h"
#include "malloc.h"
#include "memory.h"
#include "shapes.h"
#include "scene.h"
#include "vector.c"

/* ---- Retained object cache -----------------------------------------------------
 *
 * Every cache message starts [mode][epoch:u16][request:u16]. The epoch names
 * the cache incarnation the phone believes it is talking to; it changes when
 * the cache is reset, lost with the framebuffer lease, or released, so a
 * rebooted or flushed cache can never be mistaken for the old one. A message
 * with a stale epoch changes nothing and is answered STALE with the current
 * epoch. Every reply carries the current epoch and mutation revision.
 *
 * Mode 37 PUT:   [37][epoch][request][entry]...
 *   [0][asset id:u32][version:u32][kind:u8][total:u32][offset:u32][len:u16][data]
 *       PUT_ASSET. kind 0 image ([w][h][RLE]), 1 font (96 relative uint32 glyph
 *       offsets + glyph images), 2 string (1..128 bytes of UTF-8). Chunks of one
 *       asset arrive in order (offset = bytes already stored); an asset is not
 *       resident until its last chunk has been validated. A new version of an
 *       id replaces the old one in place for every object that references it.
 *   [1][object id:u32][version:u32][shape record]
 *       PUT_OBJECT. Record layouts are in shapes.h; IMAGE/TEXT/TEXT_CACHED
 *       reference assets by id (they must already be resident, or be uploaded
 *       earlier in the same message). Inline text and path commands become
 *       private assets owned by the object. Same id + same version is a no-op;
 *       a new version replaces the object, resets its animation and keeps its
 *       place on the active list.
 *   Ids and versions are nonzero. The whole message is one transaction: any
 *   failure leaves the cache exactly as it was (staged descriptors and store
 *   bytes are reclaimed).
 *
 * Mode 38 SHOW:  [38][epoch][request][flags:u8][bg:u8][puts:u8][put entry x puts]
 *                [count:u16][(id:u32, version:u32) x count][op]...
 *   Validates every reference against the resident (id, version) and, on
 *   success, replaces the active list with the references in paint order,
 *   freezes the objects that left it, applies the ops, renders and presents.
 *   flags bit 0 PRESENT  render and present (inside a mode-8 bundle: render into
 *                        the shadow the bundle presents)
 *         bit 1 KEEP     count must be 0; the active list is unchanged (ops only)
 *         bit 2 TAG      report settings field 129 = [request] once every
 *                        animation this SHOW started has ended
 *         bit 3 FREEZE   stop every running animation at its current value first
 *   ops address objects by id (0xffffffff = every active object for 2/3/4/6/7):
 *     [2][id][visible:u8]                                   VISIBLE
 *     [3][id][dx:i16][dy:i16]                               MOVE
 *     [4][id][dx:i16][dy:i16][frames:u8][curve x4]          GLIDE
 *     [5][id][mask:u16][frames:u8][curve x4][value:i16 per set bit]  TWEEN
 *     [6][id]  FREEZE      [7][id]  FINISH
 *     [9][id][angleQ8:i32][pivotX:i16][pivotY:i16][durationMs:u16][curve x4]  ROTATE
 *   A reference whose object is absent or at another version makes the SHOW
 *   fail as a whole: reply MISSING lists (some of) the ids and the displayed
 *   scene is unchanged. A SHOW or HIDE repeating the last applied request id
 *   is acknowledged APPLIED without applying anything again, so a retried
 *   SHOW never runs its motion twice.
 *
 * Mode 39 HIDE:  [39][epoch][request][flags:u8][bg:u8]
 *   Freezes and empties the active list; the objects stay cached and become
 *   evictable. flags bit 0 PRESENT blanks the panel to bg; bit 2 TAG reports
 *   settled immediately.
 *
 * Mode 40 STATE: [40][epoch][revision:u32][page:u16]
 *   epoch 0 asks for the current epoch. Replies UNCHANGED when the phone is at
 *   the current revision, DELTA with the journal entries after its revision
 *   when they fit one reply, otherwise SNAPSHOT page `page` of every resident
 *   object and asset. Queries do not touch recency.
 *
 * Mode 41 CONTROL: [41][epoch][request][sub]
 *   sub 0 [cap:u8]?  RESET: drop everything and adopt `epoch` as the new epoch
 *                    (0 = let the firmware pick one). Accepted at any epoch, so
 *                    a phone can give both lenses one session epoch at once.
 *                    The optional byte tells the transport how many bytes one
 *                    notification may carry on this link (a reply capacity the
 *                    firmware otherwise only learns from request packet sizes).
 *   sub 1 [n][ids]   DROP inactive objects (an active id refuses the message)
 *   sub 2 [ms]       animation frame period 10..250 ms (default 33)
 *
 * Replies ride the SID-0xf0 transport as reply kind 5, before the ACK:
 *   [5][stream][ordinal:u16][lens][mode][request:u16][status][epoch:u16][revision:u32][extra]
 *   status 0 APPLIED   extra [evicted:u8]  objects evicted to make room
 *          1 STALE     epoch mismatch, nothing applied
 *          2 MISSING   extra [n][more][id x n]
 *          3 CAPACITY  extra [evicted:u8]  could not fit even after eviction
 *          4 REFUSED   extra [reason:u8]   CFW_CACHE_REFUSE_*
 *          5 UNCHANGED
 *          6 DELTA     extra [n][(kind, id, version) x n]   version 0 = gone
 *          7 SNAPSHOT  extra [total:u16][page:u16][n][(kind, id, version, last_use) x n]
 *          8 RESET     no cache exists yet; the header carries the epoch to use
 *   A malformed message is NACKed by the transport and gets no reply.
 *
 * Eviction. The active list and its assets are pinned, as is everything a
 * transaction in progress references. When a descriptor or store space is
 * needed, unreferenced assets go first, then the least recently used unpinned
 * object (lowest index on a tie); the objects' now-unreferenced assets follow.
 * Recency advances only on accepted PUTs and SHOWs, in message order, never
 * on animation ticks, queries or a repeated request. The displayed scene is
 * never evicted to make a failed replacement fit: that is CAPACITY.
 *
 * Immediate modes: 19 [asset id:u32][x:i16][y:i16][options] draws an image
 * asset into the shadow; 20 [font id:u32][x][y][options][len][string] draws a
 * string with a font asset; mode-36 IMAGE/TEXT records resolve the same way.
 *
 * Threads: the receiving task owns the display gate while it mutates the cache
 * or renders. Animation frames are paced by a CFW osTimer; the callback takes
 * the same gate, advances every active animation, queues the frame and lets
 * display_copy_hook re-render right before the copy. Lease loss only flags the
 * cache as lost (cfw_cache_lose, any thread); the memory is released under the
 * gate by the next handler or display refresh (cfw_cache_reap). */

#define CFW_SHOW_FLAG_PRESENT 0x01u
#define CFW_SHOW_FLAG_KEEP    0x02u
#define CFW_SHOW_FLAG_TAG     0x04u
#define CFW_SHOW_FLAG_FREEZE  0x08u

#define CFW_PUT_ASSET   0u
#define CFW_PUT_OBJECT  1u
#define CFW_PUT_ASSET_HDR  20u
#define CFW_PUT_OBJECT_HDR 9u

#define CFW_OP_VISIBLE 2u
#define CFW_OP_MOVE    3u
#define CFW_OP_GLIDE   4u
#define CFW_OP_TWEEN   5u
#define CFW_OP_FREEZE  6u
#define CFW_OP_FINISH  7u
#define CFW_OP_ROTATE  9u

#define CFW_TWEEN_COLOR_BIT  0x100u
#define CFW_TWEEN_WIDTH_BIT  0x200u
#define CFW_TWEEN_MASK_VALID 0x3ffu

#define CFW_CACHE_REPLY_APPLIED   0u
#define CFW_CACHE_REPLY_STALE     1u
#define CFW_CACHE_REPLY_MISSING   2u
#define CFW_CACHE_REPLY_CAPACITY  3u
#define CFW_CACHE_REPLY_REFUSED   4u
#define CFW_CACHE_REPLY_UNCHANGED 5u
#define CFW_CACHE_REPLY_DELTA     6u
#define CFW_CACHE_REPLY_SNAPSHOT  7u
#define CFW_CACHE_REPLY_RESET     8u
#define CFW_CACHE_REPLY_HDR       10u

#define CFW_CACHE_REFUSE_DUPLICATE 1u   /* the same id twice in one message */
#define CFW_CACHE_REFUSE_ASSET     2u   /* asset bytes failed validation */
#define CFW_CACHE_REFUSE_TARGET    3u   /* op target not active / wrong type */
#define CFW_CACHE_REFUSE_MASK      4u   /* tween mask or value outside the type's range */
#define CFW_CACHE_REFUSE_ACTIVE    5u   /* DROP of an active object */
#define CFW_CACHE_REFUSE_EDGES     6u   /* active paths exceed the edge budget */
#define CFW_CACHE_REFUSE_KEEP      7u   /* KEEP with references */
#define CFW_CACHE_REFUSE_UPLOAD    8u   /* chunk out of order or past the end */
#define CFW_CACHE_REFUSE_KIND      9u   /* asset kind not allowed for the reference */
#define CFW_CACHE_REFUSE_ID        10u  /* id or version 0, or a reserved id */

#define CFW_JOURNAL_OBJECT 0u
#define CFW_JOURNAL_ASSET  1u
#define CFW_CACHE_ST_NEXT  0x20u        /* object: member of the incoming active list */
#define CFW_CACHE_MISSING_MAX 16u

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

static cfw_cache *cfw_cache_peek(customCfwContext *ctx) {
    cfw_cache *sc = ctx ? ctx->scene : 0;
    return (sc && sc->magic == CFW_CACHE_MAGIC) ? sc : 0;
}

/* The epoch lives in the context, so it survives the cache's memory. It is
 * never 0 (0 in a STATE query means "tell me"). */
static uint16_t cfw_cache_epoch(customCfwContext *ctx) {
    if (ctx->cache_epoch == 0) {
        uint32_t t = FW_MS_TICK;
        ctx->cache_epoch = (uint16_t)((t ^ (t >> 16)) & 0xffffu);
        if (ctx->cache_epoch == 0) ctx->cache_epoch = 1;
    }
    return ctx->cache_epoch;
}

static void cfw_cache_next_epoch(customCfwContext *ctx) {
    uint16_t e = (uint16_t)(cfw_cache_epoch(ctx) + 1u);
    ctx->cache_epoch = e ? e : 1u;
}

static cfw_cache *cfw_cache_get(customCfwContext *ctx) {
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (sc) return sc;
    sc = (cfw_cache *)cfw_malloc(sizeof(cfw_cache));      /* EvenHub heap, next to the store */
    if (sc == 0) return 0;
    bzero((uint8_t *)sc, sizeof(cfw_cache));
    sc->magic = CFW_CACHE_MAGIC;
    sc->period_ms = CFW_SCENE_DEFAULT_PERIOD;
    sc->store_bytes = ctx->texture_cache ? ctx->texture_cache_size : 0;
    ctx->scene = sc;
    (void)cfw_cache_epoch(ctx);
    return sc;
}

/* The asset store on the EvenHub heap: 256 KiB when the heap can spare it, else
 * the largest of 192/128/96/64 KiB it can. Only cfw_cache_prepare calls this
 * (before the display gate); handlers see whatever was obtained. */
static uint8_t *cfw_cache_store(customCfwContext *ctx) {
    static const uint32_t sizes[] = { 256u * 1024u, 192u * 1024u, 128u * 1024u, 96u * 1024u, CFW_CACHE_STORE_MIN };
    if (ctx->texture_cache == 0) {
        for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            uint8_t *store = (uint8_t *)cfw_malloc(sizes[i]);
            if (store == 0) continue;
            ctx->texture_cache = store;
            ctx->texture_cache_size = sizes[i];
            break;
        }
        if (ctx->texture_cache == 0) return 0;
    }
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (sc) sc->store_bytes = ctx->texture_cache_size;
    return ctx->texture_cache;
}

static void cfw_cache_store_release(customCfwContext *ctx) {
    if (ctx && ctx->texture_cache) {
        uint8_t *store = ctx->texture_cache;
        ctx->texture_cache = 0;
        ctx->texture_cache_size = 0;
        FW_FREE(store);
    }
}

/* Revision 34: the scene renders into the owned panel shadow (image_buffers.c)
 * instead of a second 150 KiB frame; `fb` aliases the shadow while the scene
 * owns the panel. */
static uint8_t *cfw_scene_frame(cfw_cache *sc) {
    sc->fb = cfw_shadow_buffer();
    return sc->fb;
}

/* Stop pacing frames. Safe from any thread, including the timer callback. */
static void cfw_scene_stop(customCfwContext *ctx) {
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (sc) sc->anim_active = 0;
    if (ctx && ctx->scene_timer) FW_TIMER_STOP(ctx->scene_timer);
}

static void cfw_scene_settled(customCfwContext *ctx, cfw_cache *sc) {
    if (sc == 0 || !sc->settle_pending) return;
    sc->settle_pending = 0;
    cfw_scene_notify_settled(ctx, sc->tag);
}

/* Free the descriptors and the store. Only from contexts that own the display
 * gate (mode 11 cleanup, reap), so no queued frame can still point at them. */
static void cfw_cache_free_storage(customCfwContext *ctx) {
    cfw_scene_stop(ctx);
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (sc == 0) { if (ctx) ctx->scene = 0; cfw_cache_store_release(ctx); return; }
    ctx->scene = 0;
    sc->magic = 0;
    sc->fb = 0;                                  /* aliases the shadow; not ours to free */
    if (sc->vector_work) cfw_heap13_free(sc->vector_work);
    FW_FREE(sc);
    cfw_cache_store_release(ctx);
}

/* Free everything and start a new epoch (mode 11 cleanup). */
static void cfw_scene_release(customCfwContext *ctx) {
    if (ctx == 0) return;
    cfw_cache_next_epoch(ctx);
    cfw_cache_free_storage(ctx);
}

/* Lease expiry or release, from any thread: stop animating and mark the cache
 * lost. Its memory is reclaimed under the display gate by cfw_cache_reap. */
static void cfw_cache_lose(customCfwContext *ctx) {
    if (ctx == 0) return;
    cfw_scene_stop(ctx);
    if (ctx->scene || ctx->texture_cache) {
        ctx->cache_lost = 1;
        cfw_cache_next_epoch(ctx);
    }
}

static void cfw_cache_reap(customCfwContext *ctx) {
    if (ctx == 0 || !ctx->cache_lost) return;
    ctx->cache_lost = 0;
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (sc) cfw_scene_settled(ctx, sc);
    cfw_cache_free_storage(ctx);      /* cfw_cache_lose already moved the epoch */
}

/* Allocate the descriptor block and the store before a cache message takes
 * the display gate (image_worker, receiving task, image mutex held). Both come
 * from the EvenHub heap, whose allocator has a mutex of its own; taking it
 * while holding the display gate risks a lock-order deadlock with the display
 * task. Nothing is allocated without the lease (the lease loss path frees). */
static void cfw_cache_prepare(customCfwContext *ctx, uint8_t mode) {
    if (ctx == 0 || mode < 37 || mode > 41 || mode == 40) return;
    cfw_cache_reap(ctx);
    if (!cfw_fb_lease_active()) return;
    cfw_cache *sc = cfw_cache_get(ctx);
    if (sc == 0) return;
    (void)cfw_cache_store(ctx);                 /* RESET reports the size the heap gave */
    if (mode == 37 || mode == 38) {
        /* paths and rotation need the vector workspace (heap 13); take it now too */
        if (!sc->vector_work) sc->vector_work = (cfw_vector_work *)cfw_heap13_malloc(sizeof(cfw_vector_work));
    }
}

/* --- store allocator (16-byte granules, first fit) ----------------------------- */

static uint32_t cfw_store_granules(uint32_t len) {
    return (len + CFW_CACHE_GRANULE - 1u) / CFW_CACHE_GRANULE;
}

static int cfw_store_bit(const cfw_cache *sc, uint32_t g) {
    return (sc->bitmap[g >> 5] >> (g & 31u)) & 1u;
}

static void cfw_store_mark(cfw_cache *sc, uint32_t g0, uint32_t n, int set) {
    for (uint32_t g = g0; g < g0 + n; g++) {
        if (set) sc->bitmap[g >> 5] |= 1u << (g & 31u);
        else     sc->bitmap[g >> 5] &= ~(1u << (g & 31u));
    }
}

static uint32_t cfw_store_capacity(const cfw_cache *sc) {
    uint32_t g = sc->store_bytes / CFW_CACHE_GRANULE;
    return g > CFW_CACHE_GRANULES ? CFW_CACHE_GRANULES : g;
}

static int cfw_store_alloc(cfw_cache *sc, uint32_t len, uint32_t *offset) {
    uint32_t need = cfw_store_granules(len), limit = cfw_store_capacity(sc);
    if (need == 0 || need > limit) return 0;
    uint32_t run = 0;
    for (uint32_t g = 0; g < limit; g++) {
        if (sc->bitmap[g >> 5] == 0xffffffffu) { g |= 31u; run = 0; continue; }
        if (cfw_store_bit(sc, g)) { run = 0; continue; }
        if (++run == need) {
            uint32_t g0 = g + 1u - need;
            cfw_store_mark(sc, g0, need, 1);
            *offset = g0 * CFW_CACHE_GRANULE;
            return 1;
        }
    }
    return 0;
}

static void cfw_store_free(cfw_cache *sc, uint32_t offset, uint32_t len) {
    uint32_t g0 = offset / CFW_CACHE_GRANULE, n = cfw_store_granules(len);
    if (g0 >= CFW_CACHE_GRANULES) return;
    if (n > CFW_CACHE_GRANULES - g0) n = CFW_CACHE_GRANULES - g0;
    cfw_store_mark(sc, g0, n, 0);
}

/* --- journal, lookup, release ----------------------------------------------------- */

static void cfw_journal_note(cfw_cache *sc, uint8_t kind, uint32_t id, uint32_t version) {
    uint32_t slot = (sc->journal_head + sc->journal_count) % CFW_CACHE_JOURNAL;
    if (sc->journal_count == CFW_CACHE_JOURNAL) {
        cfw_journal_entry *old = &sc->journal[sc->journal_head];
        if (old->revision > sc->journal_lost_rev) sc->journal_lost_rev = old->revision;
        sc->journal_head = (uint8_t)((sc->journal_head + 1u) % CFW_CACHE_JOURNAL);
        slot = (sc->journal_head + sc->journal_count - 1u) % CFW_CACHE_JOURNAL;
    } else {
        sc->journal_count++;
    }
    cfw_journal_entry *e = &sc->journal[slot];
    e->id = id;
    e->version = version | ((uint32_t)kind << 31);   /* bit 31 = asset; versions are < 2^31 */
    e->revision = sc->revision + 1u;                  /* recorded under the revision being built */
}

static int cfw_asset_find(const cfw_cache *sc, uint32_t id, int staged) {
    if (id == 0) return -1;
    for (uint32_t i = 0; i < CFW_CACHE_ASSETS; i++) {
        const cfw_asset *a = &sc->assets[i];
        if (a->id != id || (a->state & CFW_CACHE_ST_PRIVATE)) continue;
        if (((a->state & CFW_CACHE_ST_STAGED) != 0) == (staged != 0)) return (int)i;
    }
    return -1;
}

static int cfw_object_find(const cfw_cache *sc, uint32_t id, int staged) {
    if (id == 0) return -1;
    for (uint32_t i = 0; i < CFW_CACHE_OBJECTS; i++) {
        const cfw_object *o = &sc->objects[i];
        if (o->id != id) continue;
        if (((o->state & CFW_CACHE_ST_STAGED) != 0) == (staged != 0)) return (int)i;
    }
    return -1;
}

static void cfw_asset_free(cfw_cache *sc, uint32_t idx) {
    cfw_asset *a = &sc->assets[idx];
    if (a->id == 0 && !(a->state & CFW_CACHE_ST_PRIVATE) && a->length == 0) return;
    if (a->id != 0 && !(a->state & CFW_CACHE_ST_STAGED) && (a->state & CFW_CACHE_ST_COMPLETE))
        cfw_journal_note(sc, CFW_JOURNAL_ASSET, a->id, 0);
    cfw_store_free(sc, a->offset, a->length);
    bzero((uint8_t *)a, sizeof(*a));
}

static void cfw_asset_unref(cfw_cache *sc, uint16_t ref) {
    if (ref == 0) return;
    cfw_asset *a = &sc->assets[ref - 1u];
    if (a->refs) a->refs--;
    if ((a->state & CFW_CACHE_ST_PRIVATE) && a->refs == 0) cfw_asset_free(sc, ref - 1u);
}

static void cfw_active_remove(cfw_cache *sc, uint16_t idx) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < sc->active_count; i++)
        if (sc->active[i] != idx) sc->active[n++] = sc->active[i];
    sc->active_count = (uint16_t)n;
}

static void cfw_object_free(cfw_cache *sc, uint32_t idx) {
    cfw_object *o = &sc->objects[idx];
    if (o->id == 0) return;
    if (!(o->state & CFW_CACHE_ST_STAGED)) cfw_journal_note(sc, CFW_JOURNAL_OBJECT, o->id, 0);
    if (o->state & CFW_CACHE_ST_ACTIVE) cfw_active_remove(sc, (uint16_t)idx);
    cfw_asset_unref(sc, o->asset);
    cfw_asset_unref(sc, o->font);
    bzero((uint8_t *)o, sizeof(*o));
}

/* --- eviction ------------------------------------------------------------------- */

static int cfw_evict_asset(cfw_cache *sc) {
    int best = -1;
    for (uint32_t i = 0; i < CFW_CACHE_ASSETS; i++) {
        const cfw_asset *a = &sc->assets[i];
        if ((a->id == 0 && a->length == 0) || a->refs || (a->state & (CFW_CACHE_ST_STAGED | CFW_CACHE_ST_PRIVATE))) continue;
        if (best < 0 || a->last_use < sc->assets[best].last_use) best = (int)i;
    }
    if (best < 0) return 0;
    cfw_asset_free(sc, (uint32_t)best);
    return 1;
}

static int cfw_evict_object(cfw_cache *sc, uint32_t *evicted) {
    int best = -1;
    for (uint32_t i = 0; i < CFW_CACHE_OBJECTS; i++) {
        const cfw_object *o = &sc->objects[i];
        if (o->id == 0 || (o->state & (CFW_CACHE_ST_ACTIVE | CFW_CACHE_ST_STAGED | CFW_CACHE_ST_PINNED))) continue;
        if (best < 0 || o->last_use < sc->objects[best].last_use) best = (int)i;
    }
    if (best < 0) return 0;
    cfw_object_free(sc, (uint32_t)best);
    if (evicted) (*evicted)++;
    return 1;
}

/* A free descriptor, evicting objects LRU-first when none is free. */
static int cfw_object_alloc(cfw_cache *sc, uint32_t *evicted) {
    for (;;) {
        for (uint32_t i = 0; i < CFW_CACHE_OBJECTS; i++)
            if (sc->objects[i].id == 0) return (int)i;
        if (!cfw_evict_object(sc, evicted)) return -1;
    }
}

static int cfw_asset_alloc(cfw_cache *sc, uint32_t *evicted) {
    for (;;) {
        for (uint32_t i = 0; i < CFW_CACHE_ASSETS; i++)
            if (sc->assets[i].id == 0 && sc->assets[i].length == 0) return (int)i;
        if (!cfw_evict_asset(sc) && !cfw_evict_object(sc, evicted)) return -1;
    }
}

/* Store space, evicting unreferenced assets then LRU objects until it fits. */
static int cfw_store_reserve(cfw_cache *sc, uint32_t len, uint32_t *offset, uint32_t *evicted) {
    for (;;) {
        if (cfw_store_alloc(sc, len, offset)) return 1;
        if (!cfw_evict_asset(sc) && !cfw_evict_object(sc, evicted)) return 0;
    }
}

/* --- rendering ---------------------------------------------------------------- */

static void cfw_object_shape(const cfw_cache *sc, const uint8_t *store, const cfw_object *o, cfw_shape *s) {
    s->type = o->type;
    s->flags = o->flags;
    s->color = o->color;
    s->width = o->width;
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) s->p[k] = o->p[k];
    s->text = 0;
    s->asset_id = s->font_id = 0;
    s->asset = s->font = 0;
    s->asset_len = s->font_len = 0;
    if (o->asset && store) {
        const cfw_asset *a = &sc->assets[o->asset - 1u];
        s->asset = store + a->offset;
        s->asset_len = a->length;
        if (o->type == CFW_SHAPE_TEXT_INLINE) s->text = s->asset;
    }
    if (o->font && store) {
        const cfw_asset *a = &sc->assets[o->font - 1u];
        s->font = store + a->offset;
        s->font_len = a->length;
    }
}

static void cfw_scene_render(customCfwContext *ctx, cfw_cache *sc, uint8_t *fb, cfw_rectlist *rl) {
    cfw_raster r = { fb, IMAGE_STRIDE, (int32_t)IMAGE_W, (int32_t)IMAGE_H };
    const uint8_t *store = ctx ? ctx->texture_cache : 0;
    cfw_raster_clear(&r, sc->bg);
    for (uint32_t i = 0; i < sc->active_count; i++) {
        const cfw_object *o = &sc->objects[sc->active[i]];
        if (o->id == 0 || !(o->flags & CFW_SHAPE_FLAG_VISIBLE)) continue;
        cfw_shape s;
        cfw_object_shape(sc, store, o, &s);
        if (o->type == CFW_SHAPE_PATH) {
            if (s.asset && sc->vector_work)
                cv_path_draw(&r, sc->vector_work, (const cfw_path *)(const void *)s.asset, o->p, &o->rotation, o->color);
        } else if (sc->vector_work)
            cv_shape_draw(&r, sc->vector_work, &s, &o->rotation, rl);
        else cfw_shape_draw(&r, &s, rl);
    }
    rl_add(rl, 0, 0, IMAGE_W, IMAGE_H);
}

/* Called by display_copy_hook (display task) while the gate is held for the
 * queued scene frame. */
static void cfw_scene_render_if_due(customCfwContext *ctx, uint8_t *fb) {
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (sc == 0 || !sc->render_due || fb == 0 || fb != sc->fb) return;
    sc->render_due = 0;
    cfw_rectlist rl;
    rl.n = 0;
    rl.direct_submitted = 0;
    cfw_scene_render(ctx, sc, fb, &rl);
}

/* 0 always, now that the frame aliases the shadow; kept for the dispatcher. */
static int cfw_scene_resync_shadow(customCfwContext *ctx, uint8_t *shadow) {
    (void)ctx; (void)shadow;
    return 0;
}

/* --- animation ----------------------------------------------------------------- */

static void cfw_object_snap_end(cfw_object *o) {
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) o->p[k] = o->to[k];
    o->color = o->color_to;
    o->width = o->width_to;
    o->frames = 0;
    o->frame = 0;
}

static void cfw_rotation_advance(cfw_rotation *t, uint32_t now) {
    if (!t->duration) return;
    uint32_t elapsed = now - t->started;
    if (elapsed >= t->duration) { t->angle = t->to; t->duration = 0; return; }
    uint32_t progress = cfw_ease_progress(elapsed * 32768u / t->duration, t->curve);
    t->angle = cfw_lerp_q15(t->from, t->to, progress);
}

static void cfw_object_finish(cfw_object *o) {
    if (o->frames) cfw_object_snap_end(o);
    if (o->rotation.duration) { o->rotation.angle = o->rotation.to; o->rotation.duration = 0; }
}

static void cfw_object_freeze(cfw_object *o) {
    o->frames = 0;
    o->frame = 0;
    cfw_rotation_advance(&o->rotation, FW_MS_TICK);
    o->rotation.duration = 0;
}

/* Advance every animating active object by one frame. Returns 1 while any is still moving. */
static int cfw_scene_advance(cfw_cache *sc) {
    int moving = 0;
    uint32_t now = FW_MS_TICK;
    for (uint32_t i = 0; i < sc->active_count; i++) {
        cfw_object *o = &sc->objects[sc->active[i]];
        if (o->id == 0) continue;
        cfw_rotation_advance(&o->rotation, now);
        if (o->rotation.duration) moving = 1;
        if (o->frames == 0) continue;
        uint32_t frame = (uint32_t)o->frame + 1u;
        if (frame >= o->frames) {
            cfw_object_snap_end(o);
            continue;
        }
        uint32_t prog = cfw_ease_progress(frame * 32768u / o->frames, o->curve);
        for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++)
            o->p[k] = cfw_sat16(cfw_lerp_q15(o->from[k], o->to[k], prog));
        o->color = cfw_sat8(cfw_lerp_q15(o->color_from, o->color_to, prog));
        o->width = cfw_sat8(cfw_lerp_q15(o->width_from, o->width_to, prog));
        o->frame = (uint8_t)frame;
        moving = 1;
    }
    sc->anim_active = (uint8_t)moving;
    return moving;
}

/* Begin an eased move from the current values to `to` (geometry), `color_to`
 * and `width_to` over `frames`. */
static void cfw_object_animate(cfw_object *o, const int16_t *to, uint8_t color_to,
                               uint8_t width_to, uint8_t frames, const uint8_t *curve) {
    if (frames <= 1u) {
        for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) o->p[k] = to[k];
        o->color = color_to;
        o->width = width_to;
        o->frames = 0;
        o->frame = 0;
        return;
    }
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
        o->from[k] = o->p[k];
        o->to[k] = to[k];
    }
    o->color_from = o->color;
    o->color_to = color_to;
    o->width_from = o->width;
    o->width_to = width_to;
    o->frame = 0;
    o->frames = frames;
    for (uint32_t k = 0; k < 4; k++) o->curve[k] = curve[k];
}

static void cfw_object_glide(cfw_object *o, int32_t dx, int32_t dy, uint8_t frames, const uint8_t *curve) {
    uint8_t xm, ym;
    cfw_shape_masks(o->type, &xm, &ym);
    int16_t to[CFW_SHAPE_PARAMS];
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
        /* compose with an in-flight glide: continue from the current value toward
         * the old target plus the new delta */
        int32_t base = o->frames ? o->to[k] : o->p[k];
        if (xm & (1u << k)) base += dx;
        if (ym & (1u << k)) base += dy;
        to[k] = cfw_sat16(base);
    }
    uint8_t c = o->frames ? o->color_to : o->color;
    uint8_t w = o->frames ? o->width_to : o->width;
    cfw_object_animate(o, to, c, w, frames, curve);
}

static int cfw_scene_any_animating(const cfw_cache *sc) {
    for (uint32_t i = 0; i < sc->active_count; i++) {
        const cfw_object *o = &sc->objects[sc->active[i]];
        if (o->id != 0 && (o->frames || o->rotation.duration)) return 1;
    }
    return 0;
}

/* Make sure the frame timer exists and is armed. Receiving task only (creates
 * the osTimer lazily; it is deleted by mode 11 cleanup). scene_tick's address
 * goes through CFW_FN_ADDR: this clang's Thumb MOVW/MOVT assembler rejects
 * PC-relative addends beyond 64 KB, and the blob is larger than that. */
static __attribute__((always_inline)) inline void cfw_scene_arm(customCfwContext *ctx, cfw_cache *sc) {
    if (!cfw_scene_any_animating(sc)) { sc->anim_active = 0; return; }
    if (cfw_scene_frame(sc) == 0) {
        for (uint32_t i = 0; i < sc->active_count; i++) cfw_object_finish(&sc->objects[sc->active[i]]);
        sc->anim_active = 0;
        return;
    }
    if (ctx->scene_timer == 0)
        ctx->scene_timer = FW_TIMER_NEW(CFW_FN_ADDR(scene_tick), 0, ctx, 0);
    if (ctx->scene_timer == 0) {
        for (uint32_t i = 0; i < sc->active_count; i++) cfw_object_finish(&sc->objects[sc->active[i]]);
        sc->anim_active = 0;
        return;
    }
    sc->anim_active = 1;
    FW_TIMER_START(ctx->scene_timer, sc->period_ms);
}

/* `present` = 0 inside a mode-8 bundle: render into the owned shadow that
 * the bundle presents at its end. Reports settled when nothing is animating. */
static int cfw_scene_present(customCfwContext *ctx, cfw_cache *sc, int present, cfw_rectlist *rl) {
    sc->render_due = 0;
    uint8_t *fb = cfw_scene_frame(sc);
    if (fb == 0) return -1;
    cfw_scene_render(ctx, sc, fb, rl);
    if (present) {
        present_buffer(ctx, fb, rl);
        ctx->shadow_stale = 1;
    } else {
        ctx->shadow_stale = 0;
    }
    if (!sc->anim_active) cfw_scene_settled(ctx, sc);
    return 0;
}

/* A raster mode is about to own the panel: this is an implicit HIDE without a
 * blank. Everything freezes, leaves the active list (and so becomes
 * evictable) and a pending settle is reported. */
static void cfw_scene_takeover(customCfwContext *ctx) {
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (sc == 0) return;
    cfw_scene_stop(ctx);
    for (uint32_t i = 0; i < sc->active_count; i++) {
        cfw_object *o = &sc->objects[sc->active[i]];
        cfw_object_freeze(o);
        o->state &= (uint8_t)~CFW_CACHE_ST_ACTIVE;
    }
    sc->active_count = 0;
    sc->last_show_valid = 0;
    cfw_scene_settled(ctx, sc);
}

/* --- replies -------------------------------------------------------------------- */

/* Begin a reply in the context buffer; returns the extra area or 0 when the
 * link cannot carry even the header (then no reply is sent). */
static uint8_t *cfw_reply_begin(customCfwContext *ctx, uint8_t mode, uint16_t request,
                                uint8_t status, cfw_cache *sc, uint32_t *extra_cap) {
    ctx->cache_reply_len = 0;
    if (ctx->cache_reply_cap < CFW_CACHE_REPLY_HDR) { *extra_cap = 0; return 0; }
    uint8_t *p = ctx->cache_reply;
    uint32_t revision = sc ? sc->revision : 0;
    uint16_t epoch = cfw_cache_epoch(ctx);
    p[0] = mode;
    p[1] = (uint8_t)request; p[2] = (uint8_t)(request >> 8);
    p[3] = status;
    p[4] = (uint8_t)epoch; p[5] = (uint8_t)(epoch >> 8);
    p[6] = (uint8_t)revision; p[7] = (uint8_t)(revision >> 8);
    p[8] = (uint8_t)(revision >> 16); p[9] = (uint8_t)(revision >> 24);
    ctx->cache_reply_len = CFW_CACHE_REPLY_HDR;
    *extra_cap = (uint32_t)ctx->cache_reply_cap - CFW_CACHE_REPLY_HDR;
    return p + CFW_CACHE_REPLY_HDR;
}

static void cfw_reply_simple(customCfwContext *ctx, uint8_t mode, uint16_t request,
                             uint8_t status, cfw_cache *sc, int byte) {
    uint32_t cap;
    uint8_t *x = cfw_reply_begin(ctx, mode, request, status, sc, &cap);
    if (x && byte >= 0 && cap >= 1u) { x[0] = (uint8_t)byte; ctx->cache_reply_len++; }
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* --- transactions ------------------------------------------------------------------- */

static void cfw_txn_init(cfw_txn *t, cfw_cache *sc, uint8_t *store) {
    t->sc = sc;
    t->store = store;
    t->evicted = 0;
    t->status = CFW_CACHE_REPLY_APPLIED;
    t->reason = 0;
    t->missing_count = t->missing_more = 0;
    t->staged_asset_count = t->staged_object_count = 0;
    t->chunk_count = 0;
}

static int cfw_txn_fail(cfw_txn *t, uint8_t status, uint8_t reason) {
    if (t->status == CFW_CACHE_REPLY_APPLIED) { t->status = status; t->reason = reason; }
    return 0;
}

static void cfw_txn_missing(cfw_txn *t, uint32_t id) {
    t->status = CFW_CACHE_REPLY_MISSING;
    for (uint32_t i = 0; i < t->missing_count; i++) if (t->missing[i] == id) return;
    if (t->missing_count < CFW_CACHE_MISSING_MAX) t->missing[t->missing_count++] = id;
    else t->missing_more = 1;
}

/* Bytes of one asset already stored by this transaction's chunks. */
static uint32_t cfw_txn_staged_fill(const cfw_txn *t, uint16_t asset) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < t->chunk_count; i++) if (t->chunks[i].asset == asset) n += t->chunks[i].len;
    return n;
}

static void cfw_txn_abort(cfw_txn *t) {
    cfw_cache *sc = t->sc;
    for (uint32_t i = 0; i < t->staged_object_count; i++)
        cfw_object_free(sc, t->staged_objects[i] - 1u);
    for (uint32_t i = 0; i < t->staged_asset_count; i++)
        cfw_asset_free(sc, t->staged_assets[i] - 1u);
    for (uint32_t i = 0; i < CFW_CACHE_OBJECTS; i++)
        sc->objects[i].state &= (uint8_t)~(CFW_CACHE_ST_PINNED | CFW_CACHE_ST_NEXT);
    t->staged_object_count = t->staged_asset_count = 0;
}

/* Validate the completed bytes of an asset by kind. */
static int cfw_asset_content_valid(const uint8_t *data, uint32_t len, uint8_t kind) {
    if (kind == CFW_ASSET_IMAGE) {
        cfw_cached_image img;
        return cfw_texture_image_in(data, len, 0, &img) && img.rle_len + 2u == len;
    }
    if (kind == CFW_ASSET_FONT) return cfw_texture_font_valid(data, len);
    if (kind == CFW_ASSET_STRING) return cfw_texture_string_valid(data, len);
    return 0;
}

/* Allocate a fresh asset descriptor plus store space of `length` bytes. */
static int cfw_txn_new_asset(cfw_txn *t, uint32_t id, uint32_t version, uint8_t kind,
                             uint32_t length, uint8_t extra_state, uint16_t replaces) {
    cfw_cache *sc = t->sc;
    if (length == 0 || length > sc->store_bytes) { cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0); return -1; }
    if (t->staged_asset_count >= CFW_CACHE_ASSETS) { cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0); return -1; }
    int idx = cfw_asset_alloc(sc, &t->evicted);
    if (idx < 0) { cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0); return -1; }
    uint32_t offset;
    if (!cfw_store_reserve(sc, length, &offset, &t->evicted)) { cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0); return -1; }
    cfw_asset *a = &sc->assets[idx];
    bzero((uint8_t *)a, sizeof(*a));
    a->id = id;
    a->version = version;
    a->kind = kind;
    a->offset = offset;
    a->length = length;
    a->replaces = replaces;
    a->state = (uint8_t)(CFW_CACHE_ST_STAGED | extra_state);
    t->staged_assets[t->staged_asset_count++] = (uint16_t)(idx + 1);
    return idx;
}

/* Stage one PUT_ASSET entry (header already length-checked). */
static int cfw_txn_put_asset(cfw_txn *t, const uint8_t *p) {
    cfw_cache *sc = t->sc;
    uint32_t id = rd32(p + 1), version = rd32(p + 5);
    uint8_t kind = p[9];
    uint32_t total = rd32(p + 10), offset = rd32(p + 14), len = rd16(p + 18);
    const uint8_t *data = p + CFW_PUT_ASSET_HDR;
    if (id == 0 || version == 0 || version >= 0x80000000u) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_ID);
    if (kind > CFW_ASSET_STRING) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_KIND);
    if (total == 0 || total > CFW_CACHE_STORE_BYTES || offset > total || len > total - offset || len == 0)
        return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_UPLOAD);
    if (total > sc->store_bytes) return cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0);
    if (kind == CFW_ASSET_STRING && total > CFW_SHAPE_INLINE_MAX) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_ASSET);
    int staged = cfw_asset_find(sc, id, 1);
    int resident = cfw_asset_find(sc, id, 0);
    int idx;
    if (offset == 0) {
        if (staged >= 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_DUPLICATE);
        if (resident >= 0) {
            cfw_asset *r = &sc->assets[resident];
            if (r->version == version && r->length == total && (r->state & CFW_CACHE_ST_COMPLETE))
                return 1;                                   /* already resident: no-op */
            if (r->version == version && !(r->state & CFW_CACHE_ST_COMPLETE) && r->kind == kind && r->length == total) {
                /* restarting an interrupted upload: refill in place */
                r->filled = 0;
                idx = resident;
                goto copy;
            }
        }
        idx = cfw_txn_new_asset(t, id, version, kind, total, 0, resident >= 0 ? (uint16_t)(resident + 1) : 0);
        if (idx < 0) return 0;
    } else {
        idx = staged >= 0 ? staged : resident;
        if (idx < 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_UPLOAD);
        cfw_asset *a = &sc->assets[idx];
        if (a->version != version || a->kind != kind || a->length != total || (a->state & CFW_CACHE_ST_COMPLETE))
            return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_UPLOAD);
        if (a->filled + cfw_txn_staged_fill(t, (uint16_t)(idx + 1)) != offset)
            return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_UPLOAD);
    }
copy:
    if (t->chunk_count >= CFW_CACHE_TXN_CHUNKS) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_UPLOAD);
    if (t->store == 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0);
    /* Bytes land now (the space is reserved); `filled` only advances at commit,
     * so an aborted message leaves a resident partial upload where it was. */
    memcpy(t->store + sc->assets[idx].offset + offset, data, len);
    t->chunks[t->chunk_count].asset = (uint16_t)(idx + 1);
    t->chunks[t->chunk_count].len = (uint16_t)len;
    t->chunk_count++;
    if (offset + len == total) {
        if (!cfw_asset_content_valid(t->store + sc->assets[idx].offset, total, kind))
            return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_ASSET);
    }
    return 1;
}

/* Resolve an asset reference for a new object: the version staged in this
 * message wins, else the resident complete one. Returns index + 1, 0 when
 * missing (recorded), -1 when the kind is wrong. */
static int cfw_txn_resolve(cfw_txn *t, uint32_t id, uint8_t kind) {
    cfw_cache *sc = t->sc;
    int idx = cfw_asset_find(sc, id, 1);
    if (idx >= 0) {
        const cfw_asset *a = &sc->assets[idx];
        if (a->filled + cfw_txn_staged_fill(t, (uint16_t)(idx + 1)) != a->length) { cfw_txn_missing(t, id); return 0; }
    } else {
        idx = cfw_asset_find(sc, id, 0);
        if (idx < 0) { cfw_txn_missing(t, id); return 0; }
        const cfw_asset *a = &sc->assets[idx];
        if (!(a->state & CFW_CACHE_ST_COMPLETE) &&
            a->filled + cfw_txn_staged_fill(t, (uint16_t)(idx + 1)) != a->length) { cfw_txn_missing(t, id); return 0; }
    }
    if (sc->assets[idx].kind != kind) return -1;
    return idx + 1;
}

/* The workspace is allocated by cfw_cache_prepare, never here (display gate). */
static cfw_vector_work *cfw_cache_vector_work(cfw_cache *sc) {
    return sc->vector_work;
}

/* Stage one PUT_OBJECT entry (record length already checked). */
static int cfw_txn_put_object(cfw_txn *t, const uint8_t *p) {
    cfw_cache *sc = t->sc;
    uint32_t id = rd32(p + 1), version = rd32(p + 5);
    if (id == 0 || id == CFW_CACHE_ALL_OBJECTS || version == 0 || version >= 0x80000000u)
        return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_ID);
    cfw_shape s;
    cfw_shape_decode(p + CFW_PUT_OBJECT_HDR, &s);
    if (!cfw_shape_valid(&s, 1)) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_ASSET);
    if (cfw_object_find(sc, id, 1) >= 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_DUPLICATE);
    int resident = cfw_object_find(sc, id, 0);
    if (resident >= 0 && sc->objects[resident].version == version) return 1;   /* no-op */
    if (resident >= 0) sc->objects[resident].state |= CFW_CACHE_ST_PINNED;
    if (t->staged_object_count >= CFW_CACHE_OBJECTS) return cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0);

    /* dependencies first, so a missing asset costs no descriptor */
    int asset = 0, font = 0;
    if (s.type == CFW_SHAPE_IMAGE) asset = cfw_txn_resolve(t, s.asset_id, CFW_ASSET_IMAGE);
    else if (s.type == CFW_SHAPE_TEXT || s.type == CFW_SHAPE_TEXT_CACHED) asset = cfw_txn_resolve(t, s.asset_id, CFW_ASSET_STRING);
    if (asset < 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_KIND);
    if (s.type == CFW_SHAPE_TEXT_CACHED) {
        font = cfw_txn_resolve(t, s.font_id, CFW_ASSET_FONT);
        if (font < 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_KIND);
    }
    if (t->status == CFW_CACHE_REPLY_MISSING) return 0;

    int idx = cfw_object_alloc(sc, &t->evicted);
    if (idx < 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0);
    cfw_object *o = &sc->objects[idx];
    bzero((uint8_t *)o, sizeof(*o));
    o->id = id;
    o->version = version;
    o->type = s.type;
    o->flags = s.flags;
    o->color = s.color;
    o->width = s.width;
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) o->p[k] = s.p[k];
    o->state = CFW_CACHE_ST_STAGED;
    o->replaces = resident >= 0 ? (uint16_t)(resident + 1) : 0;
    t->staged_objects[t->staged_object_count++] = (uint16_t)(idx + 1);

    if (s.type == CFW_SHAPE_TEXT_INLINE) {
        uint32_t len = (uint16_t)s.p[4];
        int pa = cfw_txn_new_asset(t, 0, 1, CFW_ASSET_STRING, len, CFW_CACHE_ST_PRIVATE | CFW_CACHE_ST_COMPLETE, 0);
        if (pa < 0) return 0;
        if (t->store == 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0);
        memcpy(t->store + sc->assets[pa].offset, s.text, len);
        sc->assets[pa].filled = len;
        sc->assets[pa].refs = 1;
        o->asset = (uint16_t)(pa + 1);
    } else if (s.type == CFW_SHAPE_PATH) {
        cfw_vector_work *w = cfw_cache_vector_work(sc);
        if (w == 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0);
        int n = cv_compile(w, s.text, (uint16_t)s.p[3]);
        if (n < 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_ASSET);
        uint32_t bytes = (uint32_t)sizeof(cfw_path) + (uint32_t)n * (uint32_t)sizeof(cfw_edge);
        int pa = cfw_txn_new_asset(t, 0, 1, CFW_ASSET_EDGES, bytes, CFW_CACHE_ST_PRIVATE | CFW_CACHE_ST_COMPLETE, 0);
        if (pa < 0) return 0;
        if (t->store == 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0);
        cfw_path *path = (cfw_path *)(void *)(t->store + sc->assets[pa].offset);
        path->count = (uint16_t)n;
        path->rule = s.width;
        path->pad = 0;
        for (int i = 0; i < n; i++) path->edges[i] = w->edges[i];
        sc->assets[pa].filled = bytes;
        sc->assets[pa].refs = 1;
        o->asset = (uint16_t)(pa + 1);
        o->p[3] = 0;
    } else {
        if (asset) { o->asset = (uint16_t)asset; sc->assets[asset - 1].refs++; }
        if (font)  { o->font = (uint16_t)font;   sc->assets[font - 1].refs++; }
    }
    return 1;
}

/* Byte length of the PUT entry at p, 0 when malformed. Pure syntax. */
static uint32_t cfw_put_entry_len(const uint8_t *p, uint32_t avail) {
    if (avail < 1u) return 0;
    if (p[0] == CFW_PUT_ASSET) {
        if (avail < CFW_PUT_ASSET_HDR) return 0;
        uint32_t len = rd16(p + 18);
        return CFW_PUT_ASSET_HDR + len <= avail ? CFW_PUT_ASSET_HDR + len : 0;
    }
    if (p[0] == CFW_PUT_OBJECT) {
        if (avail < CFW_PUT_OBJECT_HDR + 1u) return 0;
        uint32_t rec = cfw_shape_record_len(p + CFW_PUT_OBJECT_HDR, avail - CFW_PUT_OBJECT_HDR);
        return rec ? CFW_PUT_OBJECT_HDR + rec : 0;
    }
    return 0;
}

static int cfw_txn_put_entry(cfw_txn *t, const uint8_t *p) {
    return p[0] == CFW_PUT_ASSET ? cfw_txn_put_asset(t, p) : cfw_txn_put_object(t, p);
}

/* Make every staged descriptor resident. Cannot fail. */
static void cfw_txn_commit(cfw_txn *t) {
    cfw_cache *sc = t->sc;
    int mutated = t->evicted != 0;
    for (uint32_t i = 0; i < t->chunk_count; i++)
        sc->assets[t->chunks[i].asset - 1u].filled += t->chunks[i].len;
    for (uint32_t i = 0; i < t->staged_asset_count; i++) {
        uint32_t idx = t->staged_assets[i] - 1u;
        cfw_asset *a = &sc->assets[idx];
        if (a->replaces) {
            uint32_t old = a->replaces - 1u;
            for (uint32_t k = 0; k < CFW_CACHE_OBJECTS; k++) {
                cfw_object *o = &sc->objects[k];
                if (o->asset == old + 1u) { o->asset = (uint16_t)(idx + 1); a->refs++; sc->assets[old].refs--; }
                if (o->font == old + 1u)  { o->font = (uint16_t)(idx + 1);  a->refs++; sc->assets[old].refs--; }
            }
            cfw_asset_free(sc, old);
            a->replaces = 0;
        }
        a->state &= (uint8_t)~CFW_CACHE_ST_STAGED;
        if (a->filled == a->length) a->state |= CFW_CACHE_ST_COMPLETE;
        a->last_use = sc->use_clock;
        if (a->id && (a->state & CFW_CACHE_ST_COMPLETE)) cfw_journal_note(sc, CFW_JOURNAL_ASSET, a->id, a->version);
        mutated = 1;
    }
    for (uint32_t i = 0; i < t->chunk_count; i++) {
        cfw_asset *a = &sc->assets[t->chunks[i].asset - 1u];
        if (a->filled == a->length && !(a->state & CFW_CACHE_ST_COMPLETE)) {
            a->state |= CFW_CACHE_ST_COMPLETE;
            if (a->id) cfw_journal_note(sc, CFW_JOURNAL_ASSET, a->id, a->version);
        }
        mutated = 1;
    }
    for (uint32_t i = 0; i < t->staged_object_count; i++) {
        uint32_t idx = t->staged_objects[i] - 1u;
        cfw_object *o = &sc->objects[idx];
        if (o->replaces) {
            uint32_t old = o->replaces - 1u;
            cfw_object *prev = &sc->objects[old];
            if (prev->state & CFW_CACHE_ST_ACTIVE) {
                for (uint32_t k = 0; k < sc->active_count; k++) if (sc->active[k] == old) sc->active[k] = (uint16_t)idx;
                o->state |= CFW_CACHE_ST_ACTIVE;
                prev->state &= (uint8_t)~CFW_CACHE_ST_ACTIVE;
            }
            prev->state &= (uint8_t)~CFW_CACHE_ST_PINNED;
            o->state |= (uint8_t)(prev->state & CFW_CACHE_ST_NEXT);
            cfw_object_free(sc, old);
            o->replaces = 0;
        }
        o->state &= (uint8_t)~CFW_CACHE_ST_STAGED;
        o->last_use = ++sc->use_clock;
        if (o->asset) sc->assets[o->asset - 1u].last_use = o->last_use;
        if (o->font)  sc->assets[o->font - 1u].last_use = o->last_use;
        cfw_journal_note(sc, CFW_JOURNAL_OBJECT, o->id, o->version);
        mutated = 1;
    }
    if (mutated) sc->revision++;
    t->staged_object_count = t->staged_asset_count = 0;
    t->chunk_count = 0;
}

/* Reply for a transaction that did not commit. */
static void cfw_txn_reply_failure(customCfwContext *ctx, cfw_txn *t, uint8_t mode, uint16_t request) {
    uint32_t cap;
    if (t->status == CFW_CACHE_REPLY_MISSING) {
        uint8_t *x = cfw_reply_begin(ctx, mode, request, t->status, t->sc, &cap);
        if (x == 0 || cap < 2u) return;
        uint32_t n = t->missing_count;
        uint32_t fit = (cap - 2u) / 4u;
        uint8_t more = t->missing_more;
        if (n > fit) { n = fit; more = 1; }
        x[0] = (uint8_t)n; x[1] = more;
        for (uint32_t i = 0; i < n; i++) wr32(x + 2 + 4 * i, t->missing[i]);
        ctx->cache_reply_len = (uint8_t)(CFW_CACHE_REPLY_HDR + 2u + 4u * n);
        return;
    }
    if (t->status == CFW_CACHE_REPLY_CAPACITY) { cfw_reply_simple(ctx, mode, request, t->status, t->sc, (int)(t->evicted > 255u ? 255u : t->evicted)); return; }
    cfw_reply_simple(ctx, mode, request, t->status, t->sc, t->reason);
}

/* --- ops on active objects ------------------------------------------------------------ */

static uint32_t cfw_popcount10(uint32_t v) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 10; i++) n += (v >> i) & 1u;
    return n;
}

/* Byte length of the op at p, or 0 when malformed. Pure syntax. */
static uint32_t cfw_op_len(const uint8_t *p, uint32_t avail) {
    if (avail < 5u) return 0;
    uint32_t need;
    switch (p[0]) {
    case CFW_OP_VISIBLE: need = 6u; break;
    case CFW_OP_MOVE:    need = 9u; break;
    case CFW_OP_GLIDE:   need = 14u; break;
    case CFW_OP_FREEZE:
    case CFW_OP_FINISH:  need = 5u; break;
    case CFW_OP_ROTATE: {
        if (avail < 19u) return 0;
        int32_t angle = (int32_t)rd32(p + 5);
        if (angle < -360 * 256 * 100 || angle > 360 * 256 * 100) return 0;
        need = 19u; break;
    }
    case CFW_OP_TWEEN: {
        if (avail < 12u) return 0;
        uint32_t mask = rd16(p + 5);
        if (mask == 0 || (mask & ~CFW_TWEEN_MASK_VALID)) return 0;
        need = 12u + 2u * cfw_popcount10(mask);
        break;
    }
    default: return 0;
    }
    if (need > avail) return 0;
    if (rd32(p + 1) == CFW_CACHE_ALL_OBJECTS && (p[0] == CFW_OP_TWEEN || p[0] == CFW_OP_ROTATE)) return 0;
    return need;
}

/* Semantic check of one op against the incoming active set (NEXT-marked). */
static int cfw_op_check(cfw_txn *t, const uint8_t *p) {
    cfw_cache *sc = t->sc;
    uint32_t id = rd32(p + 1);
    if (id == CFW_CACHE_ALL_OBJECTS) return 1;
    int idx = cfw_object_find(sc, id, 1);
    if (idx < 0) idx = cfw_object_find(sc, id, 0);
    if (idx < 0 || !(sc->objects[idx].state & CFW_CACHE_ST_NEXT)) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_TARGET);
    const cfw_object *o = &sc->objects[idx];
    if (p[0] == CFW_OP_ROTATE) {
        if (o->type > CFW_SHAPE_PIE && o->type != CFW_SHAPE_PATH) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_TARGET);
        if (cfw_cache_vector_work(sc) == 0) return cfw_txn_fail(t, CFW_CACHE_REPLY_CAPACITY, 0);
    } else if (p[0] == CFW_OP_TWEEN) {
        uint32_t mask = rd16(p + 5);
        if (mask & ~cfw_shape_tween_mask(o->type)) return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_MASK);
        if (o->type == CFW_SHAPE_PATH) {
            const uint8_t *v = p + 12;
            for (uint32_t k = 0; k < 10; k++) if (mask & (1u << k)) {
                int32_t value = (int16_t)rd16(v); v += 2;
                if ((k == 2 && (value < 0 || value > 2048)) || (k == 8 && (value < 0 || value > 15)))
                    return cfw_txn_fail(t, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_MASK);
            }
        }
    }
    return 1;
}

static void cfw_op_apply_one(cfw_object *o, const uint8_t *p) {
    switch (p[0]) {
    case CFW_OP_VISIBLE:
        if (p[5]) o->flags |= CFW_SHAPE_FLAG_VISIBLE;
        else      o->flags &= (uint8_t)~CFW_SHAPE_FLAG_VISIBLE;
        break;
    case CFW_OP_MOVE:
        cfw_object_glide(o, (int16_t)rd16(p + 5), (int16_t)rd16(p + 7), 0, 0);
        break;
    case CFW_OP_GLIDE:
        cfw_object_glide(o, (int16_t)rd16(p + 5), (int16_t)rd16(p + 7), p[9], p + 10);
        break;
    case CFW_OP_TWEEN: {
        uint32_t mask = rd16(p + 5);
        uint8_t frames = p[7];
        const uint8_t *curve = p + 8;
        const uint8_t *v = p + 12;
        int16_t to[CFW_SHAPE_PARAMS];
        for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
            to[k] = o->p[k];
            if (mask & (1u << k)) { to[k] = (int16_t)rd16(v); v += 2; }
        }
        uint8_t c = o->color, w = o->width;
        if (mask & CFW_TWEEN_COLOR_BIT) { c = (uint8_t)rd16(v); v += 2; }
        if (mask & CFW_TWEEN_WIDTH_BIT) { w = (uint8_t)rd16(v); v += 2; }
        cfw_object_animate(o, to, c, w, frames, curve);
        break;
    }
    case CFW_OP_FREEZE:
        cfw_object_freeze(o);
        break;
    case CFW_OP_FINISH:
        cfw_object_finish(o);
        break;
    case CFW_OP_ROTATE: {
        cfw_rotation *t = &o->rotation;
        cfw_rotation_advance(t, FW_MS_TICK);
        t->from = t->angle; t->to = (int32_t)rd32(p + 5);
        t->px = (int16_t)rd16(p + 9); t->py = (int16_t)rd16(p + 11);
        t->duration = (uint16_t)rd16(p + 13); t->started = FW_MS_TICK;
        for (uint32_t k = 0; k < 4; k++) t->curve[k] = p[15 + k];
        if (!t->duration) t->angle = t->to;
        break;
    }
    default:
        break;
    }
}

static void cfw_op_apply(cfw_cache *sc, const uint8_t *p) {
    uint32_t id = rd32(p + 1);
    if (id == CFW_CACHE_ALL_OBJECTS) {
        for (uint32_t i = 0; i < sc->active_count; i++) cfw_op_apply_one(&sc->objects[sc->active[i]], p);
        return;
    }
    int idx = cfw_object_find(sc, id, 0);
    if (idx >= 0) cfw_op_apply_one(&sc->objects[idx], p);
}

/* --- handlers ------------------------------------------------------------------------- */

/* Common prologue: reap a lost cache, check the lease, parse the header. */
static int cfw_cache_prologue(customCfwContext *ctx, const uint8_t *src, uint32_t srclen,
                              uint16_t *epoch, uint16_t *request) {
    if (srclen < 4u) return -1;
    cfw_cache_reap(ctx);
    if (!cfw_fb_lease_active()) return -1;
    *epoch = (uint16_t)rd16(src);
    *request = (uint16_t)rd16(src + 2);
    return 0;
}

static int cfw_cache_put(customCfwContext *ctx, const uint8_t *src, uint32_t srclen) {
    uint16_t epoch, request;
    if (cfw_cache_prologue(ctx, src, srclen, &epoch, &request)) return -1;
    uint32_t pos = 4;
    while (pos < srclen) {
        uint32_t n = cfw_put_entry_len(src + pos, srclen - pos);
        if (n == 0) return -1;
        pos += n;
    }
    if (epoch != cfw_cache_epoch(ctx)) { cfw_reply_simple(ctx, 37, request, CFW_CACHE_REPLY_STALE, cfw_cache_peek(ctx), -1); return 0; }
    cfw_cache *sc = cfw_cache_peek(ctx);          /* allocated by cfw_cache_prepare, before the gate */
    uint8_t *store = ctx->texture_cache;
    if (sc == 0 || store == 0) { cfw_reply_simple(ctx, 37, request, CFW_CACHE_REPLY_CAPACITY, sc, 0); return 0; }
    cfw_txn *t = &sc->txn;
    cfw_txn_init(t, sc, store);
    pos = 4;
    while (pos < srclen) {
        uint32_t n = cfw_put_entry_len(src + pos, srclen - pos);
        if (!cfw_txn_put_entry(t, src + pos) && t->status != CFW_CACHE_REPLY_MISSING) break;
        pos += n;
    }
    if (t->status != CFW_CACHE_REPLY_APPLIED) {
        cfw_txn_abort(t);
        if (t->evicted) sc->revision++;
        cfw_txn_reply_failure(ctx, t, 37, request);
        return 0;
    }
    cfw_txn_commit(t);
    for (uint32_t i = 0; i < CFW_CACHE_OBJECTS; i++) sc->objects[i].state &= (uint8_t)~CFW_CACHE_ST_PINNED;
    cfw_reply_simple(ctx, 37, request, CFW_CACHE_REPLY_APPLIED, sc, (int)(t->evicted > 255u ? 255u : t->evicted));
    return 0;
}

static int cfw_cache_show(customCfwContext *ctx, const uint8_t *src, uint32_t srclen,
                          int present, cfw_rectlist *rl) {
    uint16_t epoch, request;
    if (cfw_cache_prologue(ctx, src, srclen, &epoch, &request)) return -1;
    if (srclen < 7u) return -1;
    uint8_t flags = src[4], bg = src[5] & 0x0fu, puts = src[6];
    /* syntax pass */
    uint32_t pos = 7;
    for (uint32_t i = 0; i < puts; i++) {
        uint32_t n = cfw_put_entry_len(src + pos, srclen - pos);
        if (n == 0) return -1;
        pos += n;
    }
    if (srclen - pos < 2u) return -1;
    uint32_t count = rd16(src + pos);
    uint32_t refs = pos + 2;
    pos = refs + count * 8u;
    if (count > CFW_CACHE_OBJECTS || pos > srclen) return -1;
    uint32_t ops = pos;
    while (pos < srclen) {
        uint32_t n = cfw_op_len(src + pos, srclen - pos);
        if (n == 0) return -1;
        pos += n;
    }
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (epoch != cfw_cache_epoch(ctx)) { cfw_reply_simple(ctx, 38, request, CFW_CACHE_REPLY_STALE, sc, -1); return 0; }
    if ((flags & CFW_SHOW_FLAG_KEEP) && count) { cfw_reply_simple(ctx, 38, request, CFW_CACHE_REPLY_REFUSED, sc, CFW_CACHE_REFUSE_KEEP); return 0; }
    if (sc == 0) { cfw_reply_simple(ctx, 38, request, CFW_CACHE_REPLY_CAPACITY, 0, 0); return 0; }
    if (sc->last_show_valid && sc->last_show_request == request) {
        /* retransmission of an applied request: acknowledge, apply nothing */
        cfw_reply_simple(ctx, 38, request, CFW_CACHE_REPLY_APPLIED, sc, 0);
        return 0;
    }
    uint8_t *store = ctx->texture_cache;
    if (puts && store == 0) { cfw_reply_simple(ctx, 38, request, CFW_CACHE_REPLY_CAPACITY, sc, 0); return 0; }
    cfw_txn *tp = &sc->txn;
    cfw_txn_init(tp, sc, store);
    uint16_t *next = sc->next;

    /* pin every referenced resident object before staging can evict it */
    for (uint32_t i = 0; i < count; i++) {
        int idx = cfw_object_find(sc, rd32(src + refs + 8u * i), 0);
        if (idx >= 0) sc->objects[idx].state |= CFW_CACHE_ST_PINNED;
    }
    if (flags & CFW_SHOW_FLAG_KEEP)
        for (uint32_t i = 0; i < sc->active_count; i++) sc->objects[sc->active[i]].state |= CFW_CACHE_ST_PINNED;
    pos = 7;
    for (uint32_t i = 0; i < puts; i++) {
        uint32_t n = cfw_put_entry_len(src + pos, srclen - pos);
        if (!cfw_txn_put_entry(tp, src + pos) && tp->status != CFW_CACHE_REPLY_MISSING) break;
        pos += n;
    }
    /* resolve the references: staged versions first, then resident */
        uint32_t edges = 0;
    if (tp->status == CFW_CACHE_REPLY_APPLIED || tp->status == CFW_CACHE_REPLY_MISSING) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t id = rd32(src + refs + 8u * i), version = rd32(src + refs + 8u * i + 4u);
            int idx = cfw_object_find(sc, id, 1);
            if (idx < 0) idx = cfw_object_find(sc, id, 0);
            if (idx < 0 || sc->objects[idx].version != version) { cfw_txn_missing(tp, id); next[i] = 0; continue; }
            next[i] = (uint16_t)idx;
            sc->objects[idx].state |= CFW_CACHE_ST_NEXT;
            if (sc->objects[idx].type == CFW_SHAPE_PATH && sc->objects[idx].asset)
                edges += ((const cfw_path *)(const void *)(store + sc->assets[sc->objects[idx].asset - 1u].offset))->count;
        }
        if (flags & CFW_SHOW_FLAG_KEEP)
            for (uint32_t i = 0; i < sc->active_count; i++) {
                cfw_object *o = &sc->objects[sc->active[i]];
                o->state |= CFW_CACHE_ST_NEXT;
                if (o->type == CFW_SHAPE_PATH && o->asset)
                    edges += ((const cfw_path *)(const void *)(store + sc->assets[o->asset - 1u].offset))->count;
            }
    }
    if (tp->status == CFW_CACHE_REPLY_APPLIED && edges > CFW_PATH_SCENE_EDGES) cfw_txn_fail(tp, CFW_CACHE_REPLY_REFUSED, CFW_CACHE_REFUSE_EDGES);
    if (tp->status == CFW_CACHE_REPLY_APPLIED)
        for (pos = ops; pos < srclen; pos += cfw_op_len(src + pos, srclen - pos))
            if (!cfw_op_check(tp, src + pos)) break;
    if (tp->status != CFW_CACHE_REPLY_APPLIED) {
        cfw_txn_abort(tp);
        if (tp->evicted) sc->revision++;
        cfw_txn_reply_failure(ctx, tp, 38, request);
        return 0;
    }
    cfw_txn_commit(tp);

    /* the active list: departing objects freeze, arrivals become pinned */
    if (!(flags & CFW_SHOW_FLAG_KEEP)) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t id = rd32(src + refs + 8u * i);
            int idx = cfw_object_find(sc, id, 0);
            next[i] = (uint16_t)(idx < 0 ? 0 : idx);
        }
        for (uint32_t i = 0; i < CFW_CACHE_OBJECTS; i++) {
            cfw_object *o = &sc->objects[i];
            if (o->id == 0) continue;
            if ((o->state & CFW_CACHE_ST_ACTIVE) && !(o->state & CFW_CACHE_ST_NEXT)) cfw_object_freeze(o);
            if (o->state & CFW_CACHE_ST_NEXT) o->state |= CFW_CACHE_ST_ACTIVE;
            else o->state &= (uint8_t)~CFW_CACHE_ST_ACTIVE;
        }
        for (uint32_t i = 0; i < count; i++) sc->active[i] = next[i];
        sc->active_count = (uint16_t)count;
        /* recency: every reference of a shown scene, in paint order */
        for (uint32_t i = 0; i < count; i++) {
            cfw_object *o = &sc->objects[next[i]];
            o->last_use = ++sc->use_clock;
            if (o->asset) sc->assets[o->asset - 1u].last_use = o->last_use;
            if (o->font)  sc->assets[o->font - 1u].last_use = o->last_use;
        }
    }
    for (uint32_t i = 0; i < CFW_CACHE_OBJECTS; i++)
        sc->objects[i].state &= (uint8_t)~(CFW_CACHE_ST_PINNED | CFW_CACHE_ST_NEXT);
    if (flags & CFW_SHOW_FLAG_FREEZE)
        for (uint32_t i = 0; i < sc->active_count; i++) cfw_object_freeze(&sc->objects[sc->active[i]]);
    for (pos = ops; pos < srclen; pos += cfw_op_len(src + pos, srclen - pos)) cfw_op_apply(sc, src + pos);
    sc->bg = bg;
    sc->last_show_request = request;
    sc->last_show_valid = 1;
    if (flags & CFW_SHOW_FLAG_TAG) {
        cfw_scene_settled(ctx, sc);      /* a superseded tag reports now */
        sc->tag = request;
        sc->settle_pending = 1;
    }
    cfw_scene_arm(ctx, sc);
    int r = 0;
    if (flags & CFW_SHOW_FLAG_PRESENT) r = cfw_scene_present(ctx, sc, present, rl);
    else if (!sc->anim_active) cfw_scene_settled(ctx, sc);
    if (r != 0) { cfw_reply_simple(ctx, 38, request, CFW_CACHE_REPLY_CAPACITY, sc, 0); return -1; }
    cfw_reply_simple(ctx, 38, request, CFW_CACHE_REPLY_APPLIED, sc, (int)(tp->evicted > 255u ? 255u : tp->evicted));
    return 0;
}

static int cfw_cache_hide(customCfwContext *ctx, const uint8_t *src, uint32_t srclen,
                          int present, cfw_rectlist *rl) {
    uint16_t epoch, request;
    if (cfw_cache_prologue(ctx, src, srclen, &epoch, &request)) return -1;
    if (srclen < 6u) return -1;
    uint8_t flags = src[4], bg = src[5] & 0x0fu;
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (epoch != cfw_cache_epoch(ctx)) { cfw_reply_simple(ctx, 39, request, CFW_CACHE_REPLY_STALE, sc, -1); return 0; }
    if (sc && sc->last_show_valid && sc->last_show_request == request) { cfw_reply_simple(ctx, 39, request, CFW_CACHE_REPLY_APPLIED, sc, 0); return 0; }
    if (sc) {
        cfw_scene_stop(ctx);
        for (uint32_t i = 0; i < sc->active_count; i++) {
            cfw_object *o = &sc->objects[sc->active[i]];
            cfw_object_freeze(o);
            o->state &= (uint8_t)~CFW_CACHE_ST_ACTIVE;
        }
        sc->active_count = 0;
        cfw_scene_settled(ctx, sc);
        sc->last_show_request = request;
        sc->last_show_valid = 1;
        sc->bg = bg;
    }
    if (flags & CFW_SHOW_FLAG_TAG) cfw_scene_notify_settled(ctx, request);
    if (flags & CFW_SHOW_FLAG_PRESENT) {
        uint8_t *fb = cfw_shadow_buffer();
        if (fb == 0) return -1;
        cfw_raster r = { fb, IMAGE_STRIDE, (int32_t)IMAGE_W, (int32_t)IMAGE_H };
        cfw_raster_clear(&r, bg);
        rl_add(rl, 0, 0, IMAGE_W, IMAGE_H);
        if (present) { present_buffer(ctx, fb, rl); ctx->shadow_stale = 1; }
        else ctx->shadow_stale = 0;
    }
    cfw_reply_simple(ctx, 39, request, CFW_CACHE_REPLY_APPLIED, sc, 0);
    return 0;
}

static int cfw_cache_state(customCfwContext *ctx, const uint8_t *src, uint32_t srclen) {
    if (srclen < 8u) return -1;
    cfw_cache_reap(ctx);
    uint16_t epoch = (uint16_t)rd16(src);
    uint32_t revision = rd32(src + 2);
    uint16_t page = (uint16_t)rd16(src + 6);
    cfw_cache *sc = cfw_cache_peek(ctx);
    uint32_t cap;
    if (epoch != 0 && epoch != cfw_cache_epoch(ctx)) { cfw_reply_simple(ctx, 40, page, CFW_CACHE_REPLY_STALE, sc, -1); return 0; }
    if (sc == 0) { cfw_reply_simple(ctx, 40, page, CFW_CACHE_REPLY_RESET, 0, -1); return 0; }
    if (page == 0 && epoch != 0) {
        if (revision == sc->revision) { cfw_reply_simple(ctx, 40, page, CFW_CACHE_REPLY_UNCHANGED, sc, -1); return 0; }
        if (revision < sc->revision && sc->journal_lost_rev <= revision) {
            uint32_t n = 0;
            for (uint32_t i = 0; i < sc->journal_count; i++)
                if (sc->journal[(sc->journal_head + i) % CFW_CACHE_JOURNAL].revision > revision) n++;
            uint8_t *x = cfw_reply_begin(ctx, 40, page, CFW_CACHE_REPLY_DELTA, sc, &cap);
            if (x && cap >= 1u + 9u * n && n <= 255u) {
                x[0] = (uint8_t)n;
                uint8_t *q = x + 1;
                for (uint32_t i = 0; i < sc->journal_count; i++) {
                    const cfw_journal_entry *e = &sc->journal[(sc->journal_head + i) % CFW_CACHE_JOURNAL];
                    if (e->revision <= revision) continue;
                    q[0] = (uint8_t)(e->version >> 31);
                    wr32(q + 1, e->id);
                    wr32(q + 5, e->version & 0x7fffffffu);
                    q += 9;
                }
                ctx->cache_reply_len = (uint8_t)(CFW_CACHE_REPLY_HDR + 1u + 9u * n);
                return 0;
            }
            ctx->cache_reply_len = 0;
        }
    }
    /* snapshot page: objects by index, then public complete assets */
    uint8_t *x = cfw_reply_begin(ctx, 40, page, CFW_CACHE_REPLY_SNAPSHOT, sc, &cap);
    if (x == 0 || cap < 5u + 13u) { ctx->cache_reply_len = 0; return 0; }   /* a link too small for one entry gets no page */
    uint32_t per = (cap - 5u) / 13u;
    if (per > 255u) per = 255u;
    uint32_t total = 0, skip = (uint32_t)page * per, n = 0;
    uint8_t *q = x + 5;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t limit = pass ? CFW_CACHE_ASSETS : CFW_CACHE_OBJECTS;
        for (uint32_t i = 0; i < limit; i++) {
            uint32_t id, version, last;
            if (pass == 0) {
                const cfw_object *o = &sc->objects[i];
                if (o->id == 0 || (o->state & CFW_CACHE_ST_STAGED)) continue;
                id = o->id; version = o->version; last = o->last_use;
            } else {
                const cfw_asset *a = &sc->assets[i];
                if (a->id == 0 || (a->state & (CFW_CACHE_ST_STAGED | CFW_CACHE_ST_PRIVATE)) || !(a->state & CFW_CACHE_ST_COMPLETE)) continue;
                id = a->id; version = a->version; last = a->last_use;
            }
            if (total >= skip && n < per) {
                q[0] = (uint8_t)pass;
                wr32(q + 1, id); wr32(q + 5, version); wr32(q + 9, last);
                q += 13; n++;
            }
            total++;
        }
    }
    x[0] = (uint8_t)total; x[1] = (uint8_t)(total >> 8);
    x[2] = (uint8_t)page; x[3] = (uint8_t)(page >> 8);
    x[4] = (uint8_t)n;
    ctx->cache_reply_len = (uint8_t)(CFW_CACHE_REPLY_HDR + 5u + 13u * n);
    return 0;
}

static void cfw_cache_reset(customCfwContext *ctx, cfw_cache *sc, uint16_t epoch) {
    cfw_scene_stop(ctx);
    cfw_scene_settled(ctx, sc);
    uint8_t period = sc->period_ms;
    cfw_vector_work *work = sc->vector_work;
    uint32_t store_bytes = sc->store_bytes;
    bzero((uint8_t *)sc, sizeof(*sc));
    sc->magic = CFW_CACHE_MAGIC;
    sc->period_ms = period;
    sc->vector_work = work;
    sc->store_bytes = store_bytes;
    if (epoch) ctx->cache_epoch = epoch;
    else cfw_cache_next_epoch(ctx);
}

static int cfw_cache_control(customCfwContext *ctx, const uint8_t *src, uint32_t srclen) {
    uint16_t epoch, request;
    if (cfw_cache_prologue(ctx, src, srclen, &epoch, &request)) return -1;
    if (srclen < 5u) return -1;
    uint8_t sub = src[4];
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (sub == 0) {
        if (srclen >= 6u && src[5] > ctx->cache_reply_cap + CFW_REPLY_HDR_SIZE) {
            cfw_message_reply_capacity_hint(src[5]);
            ctx->cache_reply_cap = (uint8_t)(src[5] - CFW_REPLY_HDR_SIZE);
        }
        if (sc == 0) { cfw_reply_simple(ctx, 41, request, CFW_CACHE_REPLY_CAPACITY, 0, 0); return 0; }
        cfw_cache_reset(ctx, sc, epoch);
        /* APPLIED extra: [evicted][store KiB:u16] so the phone can size its mirror */
        uint32_t cap;
        uint8_t *x = cfw_reply_begin(ctx, 41, request, CFW_CACHE_REPLY_APPLIED, sc, &cap);
        if (x && cap >= 3u) {
            uint32_t kib = ctx->texture_cache_size / 1024u;
            x[0] = 0; x[1] = (uint8_t)kib; x[2] = (uint8_t)(kib >> 8);
            ctx->cache_reply_len = CFW_CACHE_REPLY_HDR + 3u;
        }
        return 0;
    }
    if (epoch != cfw_cache_epoch(ctx)) { cfw_reply_simple(ctx, 41, request, CFW_CACHE_REPLY_STALE, sc, -1); return 0; }
    if (sub == 1) {
        if (srclen < 6u) return -1;
        uint32_t n = src[5];
        if (srclen != 6u + 4u * n) return -1;
        if (sc) {
            for (uint32_t i = 0; i < n; i++) {
                int idx = cfw_object_find(sc, rd32(src + 6 + 4 * i), 0);
                if (idx >= 0 && (sc->objects[idx].state & CFW_CACHE_ST_ACTIVE)) {
                    cfw_reply_simple(ctx, 41, request, CFW_CACHE_REPLY_REFUSED, sc, CFW_CACHE_REFUSE_ACTIVE);
                    return 0;
                }
            }
            int dropped = 0;
            for (uint32_t i = 0; i < n; i++) {
                int idx = cfw_object_find(sc, rd32(src + 6 + 4 * i), 0);
                if (idx >= 0) { cfw_object_free(sc, (uint32_t)idx); dropped = 1; }
            }
            if (dropped) sc->revision++;
        }
        cfw_reply_simple(ctx, 41, request, CFW_CACHE_REPLY_APPLIED, sc, 0);
        return 0;
    }
    if (sub == 2) {
        if (srclen < 6u) return -1;
        uint32_t ms = src[5];
        if (ms < CFW_SCENE_MIN_PERIOD) ms = CFW_SCENE_MIN_PERIOD;
        if (ms > CFW_SCENE_MAX_PERIOD) ms = CFW_SCENE_MAX_PERIOD;
        if (sc == 0) { cfw_reply_simple(ctx, 41, request, CFW_CACHE_REPLY_CAPACITY, 0, 0); return 0; }
        sc->period_ms = (uint8_t)ms;
        cfw_reply_simple(ctx, 41, request, CFW_CACHE_REPLY_APPLIED, sc, 0);
        return 0;
    }
    return -1;
}

static int cfw_scene_dispatch(customCfwContext *ctx, uint8_t mode,
                              const uint8_t *src, uint32_t srclen,
                              int present, cfw_rectlist *rl) {
    if (ctx == 0) return -1;
    ctx->cache_reply_len = 0;
    switch (mode) {
    case 37: return cfw_cache_put(ctx, src, srclen);
    case 38: return cfw_cache_show(ctx, src, srclen, present, rl);
    case 39: return cfw_cache_hide(ctx, src, srclen, present, rl);
    case 40: return cfw_cache_state(ctx, src, srclen);
    case 41: return cfw_cache_control(ctx, src, srclen);
    default: return -1;
    }
}

/* --- immediate draws (modes 19/20/36) ------------------------------------------------- */

/* Resolve a public, complete asset of `kind` to its store region. */
static const uint8_t *cfw_cache_asset_region(customCfwContext *ctx, uint32_t id, uint8_t kind, uint32_t *len) {
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (sc == 0 || ctx->texture_cache == 0) return 0;
    int idx = cfw_asset_find(sc, id, 0);
    if (idx < 0) return 0;
    const cfw_asset *a = &sc->assets[idx];
    if (a->kind != kind || !(a->state & CFW_CACHE_ST_COMPLETE)) return 0;
    *len = a->length;
    return ctx->texture_cache + a->offset;
}

static int cfw_shape_resolve(customCfwContext *ctx, cfw_shape *s) {
    if (ctx == 0) return 0;
    if (s->type == CFW_SHAPE_IMAGE) {
        s->asset = cfw_cache_asset_region(ctx, s->asset_id, CFW_ASSET_IMAGE, &s->asset_len);
        return s->asset != 0;
    }
    if (s->type == CFW_SHAPE_TEXT || s->type == CFW_SHAPE_TEXT_CACHED) {
        s->asset = cfw_cache_asset_region(ctx, s->asset_id, CFW_ASSET_STRING, &s->asset_len);
        if (s->asset == 0) return 0;
        if (s->type == CFW_SHAPE_TEXT) return 1;
        s->font = cfw_cache_asset_region(ctx, s->font_id, CFW_ASSET_FONT, &s->font_len);
        return s->font != 0;
    }
    return 1;
}

static int cfw_cache_immediate(customCfwContext *ctx, uint8_t mode, uint8_t *shadow,
                               uint32_t stride, uint32_t panel_w, uint32_t panel_h,
                               const uint8_t *src, uint32_t srclen, cfw_rectlist *rl) {
    if (ctx == 0 || src == 0) return -1;
    cfw_cache_reap(ctx);
    uint32_t len;
    if (mode == 19) {
        if (srclen != 9u) return -1;
        const uint8_t *data = cfw_cache_asset_region(ctx, rd32(src), CFW_ASSET_IMAGE, &len);
        if (data == 0) return -1;
        return cfw_texture_draw_image_region(shadow, stride, panel_w, panel_h, data, len,
                                             (int32_t)(int16_t)rd16(src + 4),
                                             (int32_t)(int16_t)rd16(src + 6), src[8], rl);
    }
    if (mode == 20) {
        if (srclen < 10u || srclen != 10u + src[9]) return -1;
        const uint8_t *font = cfw_cache_asset_region(ctx, rd32(src), CFW_ASSET_FONT, &len);
        if (font == 0) return -1;
        return cfw_texture_draw_string_region(shadow, stride, panel_w, panel_h, font, len,
                                              (int32_t)(int16_t)rd16(src + 4),
                                              (int32_t)(int16_t)rd16(src + 6), src[8],
                                              src + 10, src[9], rl);
    }
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
    cfw_cache *sc = cfw_cache_peek(ctx);
    if (sc == 0 || !sc->anim_active || ctx->scene_timer == 0 || ctx->cache_lost) return;
    if (ctx->direct_pending) {                     /* display task still busy: skip a frame */
        FW_TIMER_START(ctx->scene_timer, sc->period_ms);
        return;
    }
    FW_DISPLAY_WAIT();
    if (ctx->direct_pending) {                     /* timed out; we do not own the gate */
        sc->anim_active = 0;
        return;
    }
    sc = cfw_cache_peek(ctx);                      /* re-read under the gate */
    if (sc == 0 || !sc->anim_active || sc->fb == 0 || ctx->cache_lost) {
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
    ctx->shadow_stale = 1;
    if (!rl.direct_submitted) {
        sc->render_due = 0;
        FW_DISPLAY_SIGNAL();
    }
    if (moving) FW_TIMER_START(ctx->scene_timer, sc->period_ms);
    else {
        sc->anim_active = 0;
        cfw_scene_settled(ctx, sc);
    }
}

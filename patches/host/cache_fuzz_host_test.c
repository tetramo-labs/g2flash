/*
 * Random object cache traffic, with the invariants of shapes_host_test.c
 * checked after every message, frame tick and immediate draw.
 *
 * Each seed builds its own assets and object records and then sends a mix of
 * chunked, interrupted, restarted and invalid uploads, object PUTs and
 * same-version re-asserts, SHOWs with staged entries, ops and missing
 * references, HIDEs, STATE queries, RESETs, DROPs and immediate draws, with
 * lease loss and renewal, a busy display gate, and phases in which the
 * EvenHub heap can give only a smaller store, a few granules of rollback
 * workspace, or nothing. Beyond the invariants it checks that a complete
 * asset holds the bytes uploaded for its id and version, that an inline
 * string holds its record's text, and that a rejected message which leaves the
 * revision where it was leaves the cache as it was.
 *
 *   cc -std=c11 -O1 -g -Wall -I.. -fsanitize=address,undefined -o /tmp/cache_fuzz cache_fuzz_host_test.c
 *   /tmp/cache_fuzz                  the default seeds (run_vector_tests.py)
 *   /tmp/cache_fuzz 1 500 [steps]    500 seeds from seed 1
 *
 * The default seeds must reach CAPACITY and a rejection past a full rollback
 * workspace, the paths revision 55 changed; other seed sets only report.
 */
#define main shapes_main
#include "shapes_host_test.c"
#undef main

#define FZ_DEFAULT_FIRST 1u
#define FZ_DEFAULT_SEEDS 8u
#define FZ_DEFAULT_STEPS 4000u
#define FZ_ASSET_IDS  24u      /* (id - 1) % 3: image, font, string */
#define FZ_OBJECT_IDS 48u
#define FZ_VERSIONS   3u       /* version 3 of an asset may be invalid */

static uint32_t g_fz_state = 1;
static uint32_t fz_next(void) { g_fz_state ^= g_fz_state << 13; g_fz_state ^= g_fz_state >> 17; g_fz_state ^= g_fz_state << 5; return g_fz_state; }
static uint32_t rn(uint32_t n) { return n ? fz_next() % n : 0; }

static struct { uint8_t data[66000]; uint32_t len; uint8_t kind, bad; } g_fz_assets[FZ_ASSET_IDS][FZ_VERSIONS];
static struct { uint8_t rec[64]; uint32_t len; uint8_t text[24]; uint32_t text_len; } g_fz_records[FZ_OBJECT_IDS][FZ_VERSIONS];
static const char g_fz_chars[] = "abcdefgh";
static uint16_t g_fz_request = 1000;

static struct {
    uint32_t messages, applied, refused, capacity, missing, stale, nack, evicting, draws;
    uint32_t unchanged, kept_with_workspace, kept_without_workspace;
} g_fz;

static uint32_t fz_image_id(void) { return 1u + 3u * rn(FZ_ASSET_IDS / 3u); }
static uint32_t fz_font_id(void) { return 2u + 3u * rn(FZ_ASSET_IDS / 3u); }
static uint32_t fz_string_id(void) { return 3u + 3u * rn(FZ_ASSET_IDS / 3u); }

static void fz_build_assets(void) {
    for (uint32_t i = 0; i < FZ_ASSET_IDS; i++)
        for (uint32_t v = 0; v < FZ_VERSIONS; v++) {
            uint8_t kind = (uint8_t)(i % 3u), *d = g_fz_assets[i][v].data;
            uint32_t len;
            if (kind == CFW_ASSET_IMAGE) {
                uint32_t w = 140u + rn(116), h = 140u + rn(116);
                len = rn(4) ? mk_big_image(d, w, h, (uint8_t)(1u + rn(15))) : mk_image(d, 1u + rn(60), 1u + rn(60), (uint8_t)(1u + rn(15)));
            } else if (kind == CFW_ASSET_FONT) {
                char chars[9];
                uint32_t n = 0;
                for (uint32_t c = 0; c < 8; c++) if (rn(3)) chars[n++] = g_fz_chars[c];
                if (n == 0) chars[n++] = 'a';
                chars[n] = 0;
                len = mk_font(d, chars, (uint8_t)(1u + rn(15)));
                if (i < 8u) { uint32_t pad = rn(9000); bzero(d + len, pad); len += pad; }   /* several messages to upload */
            } else {
                len = 1u + rn(40);
                for (uint32_t c = 0; c < len; c++) d[c] = rn(6) == 0 ? (uint8_t)(1u + rn(31)) : (uint8_t)g_fz_chars[rn(8)];
            }
            g_fz_assets[i][v].bad = 0;
            if (v == FZ_VERSIONS - 1u && rn(2)) {
                g_fz_assets[i][v].bad = 1;
                if (kind == CFW_ASSET_IMAGE) len--;
                else if (kind == CFW_ASSET_FONT) put32(d + 4u * (uint32_t)('a' - 32), len + 100u);
                else d[0] = 0;
            }
            g_fz_assets[i][v].len = len;
            g_fz_assets[i][v].kind = kind;
        }
}

/* One record per object id and version, as a phone defines them: a new
 * version for new content. Re-asserts vary only colour and position. */
static void fz_build_records(void) {
    for (uint32_t i = 0; i < FZ_OBJECT_IDS; i++)
        for (uint32_t v = 0; v < FZ_VERSIONS; v++) {
            uint8_t *r = g_fz_records[i][v].rec, *e;
            int x = (int)rn(700) - 30, y = (int)rn(540) - 30;
            g_fz_records[i][v].text_len = 0;
            switch (rn(6)) {
            case 0: { int16_t p[] = { (int16_t)rn(640), (int16_t)rn(480), (int16_t)(1 + rn(40)) }; e = rec(r, CFW_SHAPE_CIRCLE_FILL, 15, 0, p, 3); break; }
            case 1: e = rec_asset(r, CFW_SHAPE_IMAGE, (uint8_t)rn(64), x, y, fz_image_id(), 0); break;
            case 2: e = rec_asset(r, CFW_SHAPE_TEXT_CACHED, (uint8_t)rn(64), x, y, fz_string_id(), fz_font_id()); break;
            case 3: {
                char s[21];
                uint32_t n = 1u + rn(20);
                for (uint32_t c = 0; c < n; c++) s[c] = g_fz_chars[rn(8)];
                s[n] = 0;
                e = rec_inline(r, 15, (int)rn(640), (int)rn(480), 0, 0, s);
                for (uint32_t c = 0; c < n; c++) g_fz_records[i][v].text[c] = (uint8_t)s[c];
                g_fz_records[i][v].text_len = n;
                break;
            }
            case 4: {
                uint8_t cmds[40];
                uint32_t n = 0, points = 3u + rn(4);
                for (uint32_t k = 0; k < points; k++) {
                    cmds[n++] = k ? 1u : 0u;
                    put16(cmds + n, (int)(rn(200) * 16u)); put16(cmds + n + 2, (int)(rn(200) * 16u)); n += 4;
                }
                cmds[n++] = 4;
                e = rec_path(r, 15, (uint8_t)rn(2), (int)rn(600), (int)rn(400), 256, cmds, n);
                break;
            }
            default: { int16_t p[] = { (int16_t)rn(640), (int16_t)rn(480), (int16_t)(1 + rn(100)), (int16_t)(1 + rn(100)) }; e = rec(r, CFW_SHAPE_RECT_FILL, 9, 0, p, 4); break; }
            }
            g_fz_records[i][v].len = (uint32_t)(e - r);
        }
}

static void fz_put_object(uint32_t id, uint32_t version) {
    uint8_t r[64];
    const uint8_t *canon = g_fz_records[id - 1u][version - 1u].rec;
    uint32_t len = g_fz_records[id - 1u][version - 1u].len;
    for (uint32_t i = 0; i < len; i++) r[i] = canon[i];
    if (rn(6) == 0) { r[2] = (uint8_t)rn(64); put16(r + 4, (int)rn(700) - 30); put16(r + 6, (int)rn(540) - 30); }
    m_put_object(id, version, r, len);
}

static void fz_chunk(uint32_t id, uint32_t version, uint8_t kind, uint32_t offset, uint32_t n) {
    const uint8_t *data = g_fz_assets[id - 1u][version - 1u].data;
    uint32_t len = g_fz_assets[id - 1u][version - 1u].len;
    if (offset >= len) return;
    if (n > len - offset) n = len - offset;
    m_put_asset(id, version, kind, len, offset, data + offset, n);
}

/* A complete public asset holds what was uploaded for its id and version, and
 * an inline string what its object's record says. */
static void fz_check_contents(int mode) {
    const cfw_cache *sc = cfw_cache_peek(&g_ctx);
    const uint8_t *store = g_ctx.texture_cache;
    if (sc == 0 || store == 0) return;
    for (uint32_t i = 0; i < CFW_CACHE_ASSETS; i++) {
        const cfw_asset *a = &sc->assets[i];
        if (a->id == 0 || a->id > FZ_ASSET_IDS || a->version == 0 || a->version > FZ_VERSIONS || !(a->state & CFW_CACHE_ST_COMPLETE)) continue;
        const uint8_t *data = g_fz_assets[a->id - 1u][a->version - 1u].data;
        uint32_t len = g_fz_assets[a->id - 1u][a->version - 1u].len;
        INVARIANT(!g_fz_assets[a->id - 1u][a->version - 1u].bad || a->kind != g_fz_assets[a->id - 1u][a->version - 1u].kind,
                  "asset id %u v%u completed from invalid bytes", a->id, a->version);
        INVARIANT(a->length == len && bytes_equal(store + a->offset, data, len), "asset id %u v%u differs from its upload", a->id, a->version);
    }
    for (uint32_t k = 0; k < CFW_CACHE_OBJECTS; k++) {
        const cfw_object *o = &sc->objects[k];
        if (o->type != CFW_SHAPE_TEXT_INLINE || o->id == 0 || o->id > FZ_OBJECT_IDS || o->version > FZ_VERSIONS || o->asset == 0) continue;
        const cfw_asset *a = &sc->assets[o->asset - 1u];
        INVARIANT(a->length == g_fz_records[o->id - 1u][o->version - 1u].text_len &&
                  bytes_equal(store + a->offset, g_fz_records[o->id - 1u][o->version - 1u].text, a->length),
                  "object id %u v%u holds another string", o->id, o->version);
    }
}

/* A rejected message that leaves the revision where it was restored or never
 * touched every resident object and complete asset. An incomplete upload's
 * progress is the phone's to repeat, so it is not compared. */
static cfw_cache g_fz_before;
static void fz_check_unchanged(int mode) {
    const cfw_cache *sc = cfw_cache_peek(&g_ctx), *b = &g_fz_before;
    INVARIANT(sc->use_clock == b->use_clock && sc->active_count == b->active_count && bytes_equal(sc->active, b->active, sizeof(sc->active)),
              "a rejection changed recency or the active list");
    INVARIANT(sc->journal_head == b->journal_head && sc->journal_count == b->journal_count && sc->journal_lost_rev == b->journal_lost_rev &&
              bytes_equal(sc->journal, b->journal, sizeof(sc->journal)), "a rejection changed the journal");
    INVARIANT(bytes_equal(sc->bitmap, b->bitmap, sizeof(sc->bitmap)), "a rejection changed the store bitmap");
    for (uint32_t k = 0; k < CFW_CACHE_OBJECTS; k++) {
        const cfw_object *o = &sc->objects[k], *p = &b->objects[k];
        INVARIANT(o->id == p->id && o->version == p->version && o->type == p->type && o->asset == p->asset && o->font == p->font && o->state == p->state,
                  "a rejection changed object %u (id %u v%u, was id %u v%u)", k, o->id, o->version, p->id, p->version);
    }
    for (uint32_t i = 0; i < CFW_CACHE_ASSETS; i++) {
        const cfw_asset *a = &sc->assets[i], *p = &b->assets[i];
        INVARIANT(a->id == p->id && a->version == p->version && a->kind == p->kind && a->offset == p->offset && a->length == p->length &&
                  a->state == p->state && a->refs == p->refs && ((p->state & CFW_CACHE_ST_COMPLETE) == 0 || a->filled == p->filled),
                  "a rejection changed asset %u (id %u v%u, was id %u v%u)", i, a->id, a->version, p->id, p->version);
    }
}

static void fz_tick(void) {
    for (uint32_t k = rn(3); k; k--) tick();
}

static void fz_reset(void) {
    m_begin(g_fz_request++); put16(g_msg, (int)(1u + rn(0xfffe))); g_msg[g_len++] = 0;
    if (send(41) == 0 && reply_status() == CFW_CACHE_REPLY_APPLIED) g_epoch = reply_epoch();
}

static void fz_send(uint8_t mode) {
    const cfw_cache *sc = cfw_cache_peek(&g_ctx);
    uint16_t epoch = g_ctx.cache_epoch;
    if (sc) memcpy(&g_fz_before, sc, sizeof g_fz_before);
    int r = send(mode);
    int status = r == 0 ? reply_status() : -1;
    g_fz.messages++;
    if (r != 0) g_fz.nack++;
    else if (status == CFW_CACHE_REPLY_APPLIED) { g_fz.applied++; if (mode == 37 || mode == 38) g_fz.evicting += reply_extra()[0] != 0; }
    else if (status == CFW_CACHE_REPLY_REFUSED) g_fz.refused++;
    else if (status == CFW_CACHE_REPLY_CAPACITY) g_fz.capacity++;
    else if (status == CFW_CACHE_REPLY_MISSING) g_fz.missing++;
    else if (status == CFW_CACHE_REPLY_STALE) g_fz.stale++;
    int rejected = r != 0 || mode == 40 || status == CFW_CACHE_REPLY_REFUSED || status == CFW_CACHE_REPLY_CAPACITY ||
                   status == CFW_CACHE_REPLY_MISSING || status == CFW_CACHE_REPLY_STALE;
    if (sc && rejected && cfw_cache_peek(&g_ctx) == sc && g_ctx.cache_epoch == epoch) {
        if (sc->revision == g_fz_before.revision) { fz_check_unchanged(mode); g_fz.unchanged++; }
        else if (sc->undo) g_fz.kept_with_workspace++;
        else g_fz.kept_without_workspace++;
    }
    fz_check_contents(mode);
    if (!g_lease && rn(2)) { g_lease = 1; g_lease_lapse = 0; cfw_cache_lose(&g_ctx); }   /* FB_ACQUIRE after the lease was lost */
    if (r == 0 && status == CFW_CACHE_REPLY_STALE) fz_reset();
    fz_tick();
}

/* The largest block the EvenHub heap can give changes as its other users come
 * and go. A new store or workspace is sized by what it can give then. */
static void fz_heap_phase(void) {
    uint32_t granule = (uint32_t)sizeof(((cfw_undo *)0)->granules[0]);
    g_evenhub_fail = 0;
    switch (rn(8)) {
    case 0: case 1: case 2: g_evenhub_max = 0; break;
    case 3: case 4: g_evenhub_max = CFW_CACHE_STORE_MIN + rn(3) * 32u * 1024u; break;
    case 5: case 6: g_evenhub_max = (uint32_t)sizeof(cfw_undo) + (1u + rn(64)) * granule; break;
    default: g_evenhub_fail = 1; break;
    }
}

static void fz_upload(void) {
    uint32_t id = rn(2) ? fz_image_id() : 1u + rn(FZ_ASSET_IDS), version = 1u + rn(FZ_VERSIONS);
    uint8_t kind = rn(30) == 0 ? (uint8_t)rn(3) : g_fz_assets[id - 1u][version - 1u].kind;
    uint32_t len = g_fz_assets[id - 1u][version - 1u].len;
    uint32_t chunk = rn(3) == 0 ? len : (rn(2) ? 8160u : 1u + rn(4000));
    uint32_t offset = rn(6) == 0 ? chunk * rn(4) : 0;
    while (offset < len) {
        m_begin(g_fz_request++);
        for (uint32_t k = 1u + rn(3); k && offset < len && g_len + chunk + 64u < sizeof g_msg; k--) { fz_chunk(id, version, kind, offset, chunk); offset += chunk; }
        if (rn(4) == 0) fz_put_object(1u + rn(FZ_OBJECT_IDS), 1u + rn(FZ_VERSIONS));
        fz_send(37);
        if (rn(10) == 0) break;
        if (rn(12) == 0 && offset >= chunk) offset -= chunk;
    }
}

static void fz_show(void) {
    uint8_t flags = CFW_SHOW_FLAG_PRESENT;
    if (rn(5) == 0) flags |= CFW_SHOW_FLAG_FREEZE;
    if (rn(4) == 0) flags |= CFW_SHOW_FLAG_TAG;
    int keep = rn(10) == 0;
    if (keep) flags |= CFW_SHOW_FLAG_KEEP;
    m_show(g_fz_request++, flags, (uint8_t)rn(4));
    uint32_t puts = keep ? 0 : rn(3);
    for (uint32_t k = 0; k < puts; k++) {
        if (rn(2)) fz_put_object(1u + rn(FZ_OBJECT_IDS), 1u + rn(FZ_VERSIONS));
        else { uint32_t id = 1u + rn(FZ_ASSET_IDS), v = 1u + rn(FZ_VERSIONS); fz_chunk(id, v, g_fz_assets[id - 1u][v - 1u].kind, 0, 6000); }
    }
    m_put_count((uint8_t)puts);
    uint32_t ids[64], vers[64], n = 0;
    const cfw_cache *sc = cfw_cache_peek(&g_ctx);
    if (!keep && sc && rn(3) == 0) {
        for (uint32_t i = 0; i < CFW_CACHE_OBJECTS && n < 40u; i++) {
            const cfw_object *o = &sc->objects[i];
            if (o->id && (o->asset || o->font) && rn(4)) { ids[n] = o->id; vers[n] = o->version; n++; }
        }
    } else if (!keep) {
        for (uint32_t want = rn(10); n < want; ) {
            uint32_t id = 1u + rn(FZ_OBJECT_IDS), dup = 0;
            for (uint32_t j = 0; j < n; j++) dup |= ids[j] == id;
            if (dup) { want--; continue; }
            cfw_object *o = obj(id);
            ids[n] = id; vers[n] = o && rn(4) ? o->version : 1u + rn(FZ_VERSIONS); n++;
        }
    }
    m_refs(ids, vers, n);
    uint32_t targets = keep && sc ? sc->active_count : n;
    for (uint32_t k = rn(3); k && targets; k--) {
        uint32_t target = rn(8) == 0 ? CFW_CACHE_ALL_OBJECTS : keep ? sc->objects[sc->active[rn(targets)]].id : ids[rn(n)];
        uint8_t *p = g_msg + g_len;
        switch (rn(6)) {
        case 0: m_glide(target, (int)rn(200) - 100, (int)rn(200) - 100, (uint8_t)(2u + rn(20)), EASE); break;
        case 1: m_visible(target, (uint8_t)rn(2)); break;
        case 2: p[0] = CFW_OP_MOVE; put32(p + 1, target); put16(p + 5, (int)rn(60) - 30); put16(p + 7, (int)rn(60) - 30); g_len += 9; break;
        case 3: m_rotate(target, (int32_t)rn(720 * 256) - 360 * 256, (int)rn(640), (int)rn(480), (uint16_t)rn(600)); break;
        case 4: m_op1(CFW_OP_FREEZE, target); break;
        default: m_op1(CFW_OP_FINISH, target); break;
        }
    }
    fz_send(38);
}

static void fz_draw(void) {
    cfw_rectlist rl = { 0 };
    uint8_t m[40];
    put16(m + 4, (int)rn(700) - 30); put16(m + 6, (int)rn(540) - 30); m[8] = (uint8_t)rn(64);
    int mode = 19, r;
    if (rn(2)) {
        put32(m, fz_image_id());
        r = cfw_cache_immediate(&g_ctx, 19, g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, m, 9, &rl);
    } else {
        uint32_t n = 1u + rn(20);
        put32(m, fz_font_id()); m[9] = (uint8_t)n;
        for (uint32_t k = 0; k < n; k++) m[10 + k] = rn(8) == 0 ? (uint8_t)fz_next() : (uint8_t)g_fz_chars[rn(8)];
        mode = 20;
        r = cfw_cache_immediate(&g_ctx, 20, g_shadow, IMAGE_STRIDE, IMAGE_W, IMAGE_H, m, 10 + n, &rl);
    }
    if (r == 0) g_fz.draws++;
    check_cache_invariants(mode);
}

static void fz_step(void) {
    if (rn(300) == 0) fz_heap_phase();
    if (rn(40) == 0) g_lease_lapse = 1;
    g_gate_busy = rn(8) == 0;
    uint32_t a = rn(100);
    if (a < 35) fz_upload();
    else if (a < 55) {
        m_begin(g_fz_request++);
        for (uint32_t k = 1u + rn(4); k; k--) fz_put_object(1u + rn(FZ_OBJECT_IDS), 1u + rn(FZ_VERSIONS));
        fz_send(37);
    } else if (a < 78) fz_show();
    else if (a < 82) { m_begin(g_fz_request++); g_msg[g_len++] = CFW_SHOW_FLAG_PRESENT | (rn(4) ? 0 : CFW_SHOW_FLAG_TAG); g_msg[g_len++] = (uint8_t)rn(4); fz_send(39); }
    else if (a < 85) { m_begin(0); put16(g_msg, rn(2) ? g_epoch : 0); put32(g_msg + 2, fz_next()); put16(g_msg + 6, (int)rn(3)); g_len = 8; fz_send(40); }
    else if (a < 88) { cfw_cache_lose(&g_ctx); if (rn(2)) fz_reset(); }
    else if (a < 90) fz_reset();
    else if (a < 93) {
        m_begin(g_fz_request++); g_msg[g_len++] = 1;
        uint32_t n = 1u + rn(4);
        g_msg[g_len++] = (uint8_t)n;
        for (uint32_t k = 0; k < n; k++) { put32(g_msg + g_len, 1u + rn(FZ_OBJECT_IDS)); g_len += 4; }
        fz_send(41);
    } else fz_draw();
}

static void fz_run(uint32_t seed, uint32_t steps) {
    g_fz_state = seed ? seed : 1u;
    g_evenhub_max = 0; g_evenhub_fail = 0;
    fz_build_assets();
    fz_build_records();
    fresh();
    for (uint32_t s = 0; s < steps; s++) fz_step();
    g_evenhub_max = 0; g_evenhub_fail = 0; g_gate_busy = 0;
    printf("seed %u: %u messages, applied %u, refused %u, capacity %u, missing %u, stale %u, nack %u, evicting %u, "
           "rejections checked unchanged %u, kept past a full workspace %u, without one %u, draws %u\n",
           seed, g_fz.messages, g_fz.applied, g_fz.refused, g_fz.capacity, g_fz.missing, g_fz.stale, g_fz.nack,
           g_fz.evicting, g_fz.unchanged, g_fz.kept_with_workspace, g_fz.kept_without_workspace, g_fz.draws);
}

int main(int argc, char **argv) {
    uint32_t first = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 0) : FZ_DEFAULT_FIRST;
    uint32_t seeds = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : FZ_DEFAULT_SEEDS;
    uint32_t steps = argc > 3 ? (uint32_t)strtoul(argv[3], 0, 0) : FZ_DEFAULT_STEPS;
    uint32_t capacity = 0, kept = 0;
    for (uint32_t s = first; s < first + seeds; s++) {
        bzero((uint8_t *)&g_fz, sizeof g_fz);
        fz_run(s, steps);
        capacity += g_fz.capacity;
        kept += g_fz.kept_with_workspace;
    }
    if (argc == 1) CHECK(capacity > 0 && kept > 0);
    cfw_scene_release(&g_ctx);
    CHECK(g_alloc_live == 0);
    printf("cache fuzz: %s (%d failures), %u seeds, %u invariant checks\n", g_fail ? "FAILED" : "OK", g_fail, seeds, g_invariant_checks);
    return g_fail ? 1 : 0;
}

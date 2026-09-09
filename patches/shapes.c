#include <stdint.h>
#include "cfw_context.h"
#include "debug.h"
#include "texture_cache.h"
#include "shapes.h"

/* ---- Software vector rasterizer for the packed-4bpp shadow ------------------
 *
 * Everything here is integer math (no floats, no 64-bit division) and writes
 * through two clipped primitives: a single pixel and a horizontal span. Solid
 * spans fill whole bytes (two pixels) at a time. Circles, rings and rounded
 * corners are scan-converted exactly per row from an integer square root; every
 * polygonal shape (triangles, quads, wide lines, pie sectors) goes through one
 * even-odd scanline filler working in 1/16 px units (Q4), sampled at pixel
 * centers, so shared edges never gap or double-cover. Curves and arcs are
 * flattened to short line segments. Hairlines (width <= 1) use Bresenham.
 *
 * Coordinates are clamped to +-1024 px before any Q4 math so intermediate
 * products stay within 32 bits; anything that far outside the 640x480 panel is
 * invisible anyway. */

#define CFW_Q4          16
#define CFW_COORD_MAX   1023
#define CFW_COORD_MIN   (-1024)
#define CFW_DIM_MAX     2047
#define CFW_POLY_MAX    8u
#define CFW_BEZIER_SEGS 16u
#define CFW_ARC_MAX_SEGS 72u

static uint32_t cfw_isqrt(uint32_t v) {
    uint32_t res = 0;
    uint32_t bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

/* Floor division for den > 0 (C's / truncates toward zero). */
static int32_t cfw_div_floor(int32_t num, int32_t den) {
    int32_t q = num / den;
    if ((num % den) != 0 && num < 0) q--;
    return q;
}

static int32_t cfw_clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- pixel + span ---------------------------------------------------------- */

static void rs_pixel(const cfw_raster *r, int32_t x, int32_t y, uint8_t c) {
    if (x < 0 || y < 0 || x >= r->w || y >= r->h) return;
    uint8_t *p = r->buf + (uint32_t)y * r->stride + ((uint32_t)x >> 1);
    if (x & 1) *p = (uint8_t)((*p & 0xf0u) | c);
    else       *p = (uint8_t)((*p & 0x0fu) | (uint8_t)(c << 4));
}

/* Inclusive [x0, x1] on row y; empty when x0 > x1. */
static void rs_hspan(const cfw_raster *r, int32_t x0, int32_t x1, int32_t y, uint8_t c) {
    if (y < 0 || y >= r->h || x0 > x1 || x1 < 0 || x0 >= r->w) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= r->w) x1 = r->w - 1;
    uint8_t *row = r->buf + (uint32_t)y * r->stride;
    if (x0 & 1) {
        row[x0 >> 1] = (uint8_t)((row[x0 >> 1] & 0xf0u) | c);
        x0++;
    }
    if (x0 > x1) return;
    if (!(x1 & 1)) {
        row[x1 >> 1] = (uint8_t)((row[x1 >> 1] & 0x0fu) | (uint8_t)(c << 4));
        x1--;
    }
    if (x0 > x1) return;
    uint8_t v = (uint8_t)(c * 0x11u);
    for (int32_t i = x0 >> 1; i <= (x1 >> 1); i++) row[i] = v;
}

static void cfw_raster_clear(const cfw_raster *r, uint8_t color) {
    uint32_t words = (r->stride * (uint32_t)r->h) >> 2;
    uint32_t v = (uint32_t)(color & 0x0fu) * 0x11111111u;
    uint32_t *dst = (uint32_t *)(void *)r->buf;
    for (uint32_t i = 0; i < words; i++) dst[i] = v;
}

/* ---- rectangles and circles ------------------------------------------------ */

static void rs_fill_rect(const cfw_raster *r, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t c) {
    if (w <= 0 || h <= 0) return;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t y1 = y + h;
    if (y1 > r->h) y1 = r->h;
    for (int32_t row = y0; row < y1; row++) rs_hspan(r, x, x + w - 1, row, c);
}

/* Half-width (Q4) of a circle of radius rad16 at vertical offset dy16 from its
 * center, or -1 when the row misses the circle. */
static int32_t rs_chord_half(int32_t rad16, int32_t dy16) {
    if (dy16 < 0) dy16 = -dy16;
    if (dy16 >= rad16) return -1;
    uint32_t rr = (uint32_t)rad16 * (uint32_t)rad16;
    uint32_t dd = (uint32_t)dy16 * (uint32_t)dy16;
    return (int32_t)cfw_isqrt(rr - dd);
}

/* First/last pixel covered by the continuous span [a16, b16). */
static int rs_span_pixels(int32_t a16, int32_t b16, int32_t *first, int32_t *last) {
    *first = cfw_div_floor(a16 + 7, CFW_Q4);
    *last = cfw_div_floor(b16 + 7, CFW_Q4) - 1;
    return *first <= *last;
}

/* Pixel extents of the rounded rectangle (x,y,w,h,rad) on `row`. Corners are
 * quarter circles of radius rad (clamped to half the shorter side). */
static int rs_rrect_row(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rad,
                        int32_t row, int32_t *l, int32_t *rt) {
    if (w <= 0 || h <= 0 || row < y || row >= y + h) return 0;
    int32_t maxr = (w < h ? w : h) / 2;
    if (rad > maxr) rad = maxr;
    if (rad < 0) rad = 0;
    *l = x;
    *rt = x + w - 1;
    if (rad == 0) return 1;
    int32_t dy16 = -1;
    if (row < y + rad)              dy16 = (y + rad - row) * CFW_Q4 - 8;
    else if (row >= y + h - rad)    dy16 = (row - (y + h - rad)) * CFW_Q4 + 8;
    if (dy16 < 0) return 1;
    int32_t half = rs_chord_half(rad * CFW_Q4, dy16);
    if (half < 0) return 0;
    int32_t first, last;
    int32_t a16 = x * CFW_Q4 + rad * CFW_Q4 - half;
    int32_t b16 = (x + w) * CFW_Q4 - rad * CFW_Q4 + half;
    if (!rs_span_pixels(a16, b16, &first, &last)) return 0;
    *l = first;
    *rt = last;
    return 1;
}

/* Rounded rectangle: filled when wd <= 0, else a border of width wd. */
static void rs_rrect(const cfw_raster *r, int32_t x, int32_t y, int32_t w, int32_t h,
                     int32_t rad, int32_t wd, uint8_t c) {
    if (w <= 0 || h <= 0) return;
    int inner = wd > 0 && w > 2 * wd && h > 2 * wd;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t y1 = y + h;
    if (y1 > r->h) y1 = r->h;
    for (int32_t row = y0; row < y1; row++) {
        int32_t ol, orr, il, ir;
        if (!rs_rrect_row(x, y, w, h, rad, row, &ol, &orr)) continue;
        if (inner && rs_rrect_row(x + wd, y + wd, w - 2 * wd, h - 2 * wd,
                                  rad - wd, row, &il, &ir)) {
            rs_hspan(r, ol, il - 1, row, c);
            rs_hspan(r, ir + 1, orr, row, c);
        } else {
            rs_hspan(r, ol, orr, row, c);
        }
    }
}

/* Circle centered on pixel (cx,cy): filled when wd <= 0, else a ring of width wd. */
static void rs_circle(const cfw_raster *r, int32_t cx, int32_t cy, int32_t rad, int32_t wd, uint8_t c) {
    if (rad < 0) return;
    /* radius r + 1/2 around the pixel center: an odd 2r+1 px diameter, so a
     * radius-1 dot is a 3x3 block and a ring of width wd is exactly wd px thick */
    int32_t rad16 = rad * CFW_Q4 + 8;
    int32_t in16 = wd > 0 && rad > wd ? (rad - wd) * CFW_Q4 + 8 : -1;
    int32_t cx16 = cx * CFW_Q4 + 8;
    for (int32_t row = cy - rad; row <= cy + rad; row++) {
        if (row < 0 || row >= r->h) continue;
        int32_t dy16 = (cy - row) * CFW_Q4;
        int32_t ho = rs_chord_half(rad16, dy16);
        if (ho < 0) continue;
        int32_t of, ol;
        if (!rs_span_pixels(cx16 - ho, cx16 + ho, &of, &ol)) continue;
        int32_t hi = in16 > 0 ? rs_chord_half(in16, dy16) : -1;
        int32_t inf, inl;
        if (hi > 0 && rs_span_pixels(cx16 - hi, cx16 + hi, &inf, &inl)) {
            rs_hspan(r, of, inf - 1, row, c);
            rs_hspan(r, inl + 1, ol, row, c);
        } else {
            rs_hspan(r, of, ol, row, c);
        }
    }
}

/* ---- polygons (Q4 vertices, even-odd, center sampled) ---------------------- */

static void rs_fill_poly_q4(const cfw_raster *r, const int32_t *px, const int32_t *py,
                            uint32_t n, uint8_t c) {
    if (n < 3 || n > CFW_POLY_MAX) return;
    int32_t ymin = py[0], ymax = py[0];
    for (uint32_t i = 1; i < n; i++) {
        if (py[i] < ymin) ymin = py[i];
        if (py[i] > ymax) ymax = py[i];
    }
    int32_t row0 = cfw_div_floor(ymin, CFW_Q4);
    int32_t row1 = cfw_div_floor(ymax, CFW_Q4);
    if (row0 < 0) row0 = 0;
    if (row1 >= r->h) row1 = r->h - 1;
    for (int32_t row = row0; row <= row1; row++) {
        int32_t yc = row * CFW_Q4 + 8;
        int32_t xs[CFW_POLY_MAX];
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t j = (i + 1 == n) ? 0 : i + 1;
            int32_t ax = px[i], ay = py[i], bx = px[j], by = py[j];
            if (ay == by) continue;
            if (ay > by) {
                int32_t t = ax; ax = bx; bx = t;
                t = ay; ay = by; by = t;
            }
            if (yc < ay || yc >= by) continue;
            xs[k++] = ax + cfw_div_floor((yc - ay) * (bx - ax), by - ay);
        }
        for (uint32_t i = 1; i < k; i++) {          /* insertion sort */
            int32_t v = xs[i];
            uint32_t j = i;
            while (j > 0 && xs[j - 1] > v) { xs[j] = xs[j - 1]; j--; }
            xs[j] = v;
        }
        for (uint32_t i = 0; i + 1 < k; i += 2) {
            int32_t first, last;
            if (rs_span_pixels(xs[i], xs[i + 1], &first, &last))
                rs_hspan(r, first, last, row, c);
        }
    }
}

/* ---- lines ----------------------------------------------------------------- */

static void rs_hairline(const cfw_raster *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t c) {
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;
    for (;;) {
        rs_pixel(r, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Line between Q4 points. Widths >= 2 become a filled quad with square caps
 * (half a width past each end), so consecutive segments of a flattened curve
 * overlap instead of leaving wedge gaps at the joints. */
static void rs_line_q4(const cfw_raster *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       int32_t wd, uint8_t c) {
    if (wd <= 1) {
        rs_hairline(r, cfw_div_floor(x0, CFW_Q4), cfw_div_floor(y0, CFW_Q4),
                    cfw_div_floor(x1, CFW_Q4), cfw_div_floor(y1, CFW_Q4), c);
        return;
    }
    int32_t dx = x1 - x0, dy = y1 - y0;
    uint32_t len = cfw_isqrt((uint32_t)(dx * dx) + (uint32_t)(dy * dy));
    int32_t hw = wd * 8;                                 /* half width in Q4 */
    if (len == 0) {
        int32_t px[4] = { x0 - hw, x0 + hw, x0 + hw, x0 - hw };
        int32_t py[4] = { y0 - hw, y0 - hw, y0 + hw, y0 + hw };
        rs_fill_poly_q4(r, px, py, 4, c);
        return;
    }
    int32_t ex = dx * hw / (int32_t)len, ey = dy * hw / (int32_t)len;
    int32_t nx = -ey, ny = ex;
    int32_t px[4] = { x0 - ex + nx, x1 + ex + nx, x1 + ex - nx, x0 - ex - nx };
    int32_t py[4] = { y0 - ey + ny, y1 + ey + ny, y1 + ey - ny, y0 - ey - ny };
    rs_fill_poly_q4(r, px, py, 4, c);
}

static int32_t cfw_pix_q4(int32_t v) { return v * CFW_Q4 + 8; }

static void rs_line(const cfw_raster *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    int32_t wd, uint8_t c) {
    rs_line_q4(r, cfw_pix_q4(x0), cfw_pix_q4(y0), cfw_pix_q4(x1), cfw_pix_q4(y1), wd, c);
}

/* Closed polygon outline through integer vertices. */
static void rs_stroke_poly(const cfw_raster *r, const int32_t *px, const int32_t *py,
                           uint32_t n, int32_t wd, uint8_t c) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = (i + 1 == n) ? 0 : i + 1;
        rs_line(r, px[i], py[i], px[j], py[j], wd, c);
    }
}

/* ---- curves ---------------------------------------------------------------- */

static void rs_bezier2(const cfw_raster *r, int32_t x0, int32_t y0, int32_t cx, int32_t cy,
                       int32_t x1, int32_t y1, int32_t wd, uint8_t c) {
    int32_t px = cfw_pix_q4(x0), py = cfw_pix_q4(y0);
    for (uint32_t i = 1; i <= CFW_BEZIER_SEGS; i++) {
        int32_t u = (int32_t)(CFW_BEZIER_SEGS - i), t = (int32_t)i;
        int32_t nx = (u * u * x0 + 2 * u * t * cx + t * t * x1) * CFW_Q4 / 256 + 8;
        int32_t ny = (u * u * y0 + 2 * u * t * cy + t * t * y1) * CFW_Q4 / 256 + 8;
        rs_line_q4(r, px, py, nx, ny, wd, c);
        px = nx; py = ny;
    }
}

static void rs_bezier3(const cfw_raster *r, int32_t x0, int32_t y0, int32_t ax, int32_t ay,
                       int32_t bx, int32_t by, int32_t x1, int32_t y1, int32_t wd, uint8_t c) {
    int32_t px = cfw_pix_q4(x0), py = cfw_pix_q4(y0);
    for (uint32_t i = 1; i <= CFW_BEZIER_SEGS; i++) {
        int32_t u = (int32_t)(CFW_BEZIER_SEGS - i), t = (int32_t)i;
        int32_t uu = u * u, tt = t * t;
        int32_t nx = (uu * u * x0 + 3 * uu * t * ax + 3 * u * tt * bx + tt * t * x1) * CFW_Q4 / 4096 + 8;
        int32_t ny = (uu * u * y0 + 3 * uu * t * ay + 3 * u * tt * by + tt * t * y1) * CFW_Q4 / 4096 + 8;
        rs_line_q4(r, px, py, nx, ny, wd, c);
        px = nx; py = ny;
    }
}

/* sin(deg) * 32768 for 0..90 degrees. */
static const uint16_t cfw_sin_q15[91] = {
    0, 572, 1144, 1715, 2286, 2856, 3425, 3993, 4560, 5126, 5690, 6252, 6813,
    7371, 7927, 8481, 9032, 9580, 10126, 10668, 11207, 11743, 12275, 12803, 13328, 13848,
    14365, 14876, 15384, 15886, 16384, 16877, 17364, 17847, 18324, 18795, 19261, 19720, 20174,
    20622, 21063, 21498, 21926, 22348, 22763, 23170, 23571, 23965, 24351, 24730, 25102, 25466,
    25822, 26170, 26510, 26842, 27166, 27482, 27789, 28088, 28378, 28660, 28932, 29197, 29452,
    29698, 29935, 30163, 30382, 30592, 30792, 30983, 31164, 31336, 31499, 31651, 31795, 31928,
    32052, 32166, 32270, 32365, 32449, 32524, 32588, 32643, 32688, 32723, 32748, 32763, 32767,
};

static int32_t cfw_sin_deg(int32_t a) {
    a %= 360;
    if (a < 0) a += 360;
    int32_t sign = 1;
    if (a >= 180) { a -= 180; sign = -1; }
    if (a > 90) a = 180 - a;
    return sign * (int32_t)cfw_sin_q15[a];
}

static int32_t cfw_cos_deg(int32_t a) { return cfw_sin_deg(a + 90); }

/* Q4 point on the circle (cx,cy,rad) at integer degrees. 0 = +x, angles grow
 * toward +y (clockwise on screen). */
static void rs_arc_point(int32_t cx, int32_t cy, int32_t rad, int32_t deg, int32_t *x, int32_t *y) {
    int32_t r16 = rad * CFW_Q4;
    *x = cfw_pix_q4(cx) + ((r16 * cfw_cos_deg(deg) + 16384) >> 15);
    *y = cfw_pix_q4(cy) + ((r16 * cfw_sin_deg(deg) + 16384) >> 15);
}

static uint32_t rs_arc_segments(int32_t span) {
    uint32_t n = (uint32_t)(span < 0 ? -span : span) / 6u + 1u;
    return n > CFW_ARC_MAX_SEGS ? CFW_ARC_MAX_SEGS : n;
}

static void rs_arc(const cfw_raster *r, int32_t cx, int32_t cy, int32_t rad, int32_t a0, int32_t a1,
                   int32_t wd, uint8_t c) {
    if (rad <= 0) return;
    int32_t span = cfw_clamp(a1 - a0, -360, 360);
    uint32_t n = rs_arc_segments(span);
    int32_t px, py;
    rs_arc_point(cx, cy, rad, a0, &px, &py);
    for (uint32_t i = 1; i <= n; i++) {
        int32_t nx, ny;
        rs_arc_point(cx, cy, rad, a0 + span * (int32_t)i / (int32_t)n, &nx, &ny);
        rs_line_q4(r, px, py, nx, ny, wd, c);
        px = nx; py = ny;
    }
}

static void rs_pie(const cfw_raster *r, int32_t cx, int32_t cy, int32_t rad, int32_t a0, int32_t a1, uint8_t c) {
    if (rad <= 0) return;
    int32_t span = cfw_clamp(a1 - a0, -360, 360);
    if (span == 0) return;
    uint32_t n = rs_arc_segments(span);
    int32_t px[3], py[3];
    px[0] = cfw_pix_q4(cx);
    py[0] = cfw_pix_q4(cy);
    rs_arc_point(cx, cy, rad, a0, &px[1], &py[1]);
    for (uint32_t i = 1; i <= n; i++) {
        rs_arc_point(cx, cy, rad, a0 + span * (int32_t)i / (int32_t)n, &px[2], &py[2]);
        rs_fill_poly_q4(r, px, py, 3, c);
        px[1] = px[2];
        py[1] = py[2];
    }
}

/* ---- records --------------------------------------------------------------- */

/* Which parameters are x / y coordinates, per type (used for clamping and for
 * glides, which translate exactly these). */
static const uint8_t cfw_shape_xmask_tab[CFW_SHAPE_TYPE_MAX + 1] = {
    0x00, 0x05, 0x01, 0x01, 0x01, 0x01, 0x15, 0x15, 0x55, 0x55,
    0x15, 0x55, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
};
static const uint8_t cfw_shape_ymask_tab[CFW_SHAPE_TYPE_MAX + 1] = {
    0x00, 0x0a, 0x02, 0x02, 0x02, 0x02, 0x2a, 0x2a, 0xaa, 0xaa,
    0x2a, 0xaa, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
};

/* Byte length of the record at `rec`, or 0 when it is truncated or malformed. */
static uint32_t cfw_shape_record_len(const uint8_t *rec, uint32_t avail) {
    if (avail < 1u) return 0;
    if (rec[0] != CFW_SHAPE_TEXT_INLINE) return avail >= CFW_SHAPE_RECORD_BYTES ? CFW_SHAPE_RECORD_BYTES : 0;
    if (avail < CFW_SHAPE_INLINE_HDR) return 0;
    uint32_t len = rec[CFW_SHAPE_INLINE_HDR - 1u];
    if (len == 0 || len > CFW_SHAPE_INLINE_MAX || CFW_SHAPE_INLINE_HDR + len > avail) return 0;
    return CFW_SHAPE_INLINE_HDR + len;
}

static void cfw_shape_masks(uint8_t type, uint8_t *xmask, uint8_t *ymask) {
    if (type > CFW_SHAPE_TYPE_MAX) type = 0;
    *xmask = cfw_shape_xmask_tab[type];
    *ymask = cfw_shape_ymask_tab[type];
}

static void cfw_shape_decode(const uint8_t *rec, cfw_shape *out) {
    out->type = rec[0];
    out->flags = rec[1];
    out->color = rec[2];
    out->width = rec[3];
    out->text = 0;
    if (rec[0] == CFW_SHAPE_TEXT_INLINE) {
        for (uint32_t i = 0; i < 4u; i++) out->p[i] = (int16_t)rd16(rec + 4 + 2 * i);
        out->p[4] = (int16_t)rec[CFW_SHAPE_INLINE_HDR - 1u];
        for (uint32_t i = 5; i < CFW_SHAPE_PARAMS; i++) out->p[i] = 0;
        out->text = rec + CFW_SHAPE_INLINE_HDR;
        return;
    }
    for (uint32_t i = 0; i < CFW_SHAPE_PARAMS; i++)
        out->p[i] = (int16_t)rd16(rec + 4 + 2 * i);
}

static int cfw_shape_valid(const cfw_shape *s) {
    if (s->type == CFW_SHAPE_NONE || s->type > CFW_SHAPE_TYPE_MAX) return 0;
    if (s->type == CFW_SHAPE_TEXT_INLINE)
        return s->text != 0 && s->p[4] > 0 && (uint16_t)s->p[4] <= CFW_SHAPE_INLINE_MAX;
    if (s->type == CFW_SHAPE_TEXT || s->type == CFW_SHAPE_TEXT_CACHED) {
        uint32_t off = (uint16_t)s->p[2], len = (uint16_t)s->p[3];
        if (len > CFW_SHAPE_TEXT_MAX || off + len > CFW_TEXTURE_CACHE_SIZE) return 0;
    }
    return 1;
}

/* IMAGE/TEXT slots reuse the mode 13/14/15 renderers by re-encoding their
 * payloads on the stack; the text bytes live in the phone-owned texture cache. */
static void cfw_shape_draw_texture(const cfw_raster *r, const cfw_shape *s, cfw_rectlist *rl) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (ctx == 0 || ctx->texture_cache == 0) return;
    uint8_t buf[8 + CFW_SHAPE_TEXT_MAX];
    uint32_t x = (uint16_t)s->p[0], y = (uint16_t)s->p[1];
    if (s->type == CFW_SHAPE_IMAGE) {
        uint32_t off = (uint16_t)s->p[2];
        buf[0] = (uint8_t)off; buf[1] = (uint8_t)(off >> 8);
        buf[2] = (uint8_t)x;   buf[3] = (uint8_t)(x >> 8);
        buf[4] = (uint8_t)y;   buf[5] = (uint8_t)(y >> 8);
        buf[6] = s->color;
        cfw_texture_draw_image(r->buf, r->stride, (uint32_t)r->w, (uint32_t)r->h, buf, 7, rl);
        return;
    }
    uint32_t off = (uint16_t)s->p[2], len = (uint16_t)s->p[3];
    if (len > CFW_SHAPE_TEXT_MAX || off + len > CFW_TEXTURE_CACHE_SIZE) return;
    uint32_t hdr;
    if (s->type == CFW_SHAPE_TEXT) {
        buf[0] = (uint8_t)x; buf[1] = (uint8_t)(x >> 8);
        buf[2] = (uint8_t)y; buf[3] = (uint8_t)(y >> 8);
        buf[4] = s->color;
        buf[5] = (uint8_t)len;
        hdr = 6;
    } else {
        uint32_t font = (uint16_t)s->p[4];
        buf[0] = (uint8_t)font; buf[1] = (uint8_t)(font >> 8);
        buf[2] = (uint8_t)x;    buf[3] = (uint8_t)(x >> 8);
        buf[4] = (uint8_t)y;    buf[5] = (uint8_t)(y >> 8);
        buf[6] = s->color;
        buf[7] = (uint8_t)len;
        hdr = 8;
    }
    for (uint32_t i = 0; i < len; i++) buf[hdr + i] = ctx->texture_cache[off + i];
    if (s->type == CFW_SHAPE_TEXT) {
        uint32_t tokens[CFW_SHAPE_TEXT_MAX];
        cfw_builtin_draw_string_buf(r->buf, r->stride, (uint32_t)r->w, (uint32_t)r->h,
                                    buf, hdr + len, rl, tokens, CFW_SHAPE_TEXT_MAX);
    } else {
        cfw_texture_draw_string(r->buf, r->stride, (uint32_t)r->w, (uint32_t)r->h,
                                buf, hdr + len, rl);
    }
}

/* Built-in font string from the record itself, clipped to its box when w/h > 0. */
static void cfw_shape_draw_inline_text(const cfw_raster *r, const cfw_shape *s, cfw_rectlist *rl) {
    uint32_t len = (uint16_t)s->p[4];
    if (s->text == 0 || len == 0 || len > CFW_SHAPE_INLINE_MAX) return;
    int32_t x = s->p[0], y = s->p[1], w = s->p[2], h = s->p[3];
    int32_t clip_w = r->w, clip_h = r->h;
    if (w > 0 && x + w < clip_w) clip_w = x + w;
    if (h > 0 && y + h < clip_h) clip_h = y + h;
    if (clip_w <= 0 || clip_h <= 0) return;
    uint8_t buf[6 + CFW_SHAPE_INLINE_MAX];
    buf[0] = (uint8_t)x; buf[1] = (uint8_t)((uint16_t)x >> 8);
    buf[2] = (uint8_t)y; buf[3] = (uint8_t)((uint16_t)y >> 8);
    buf[4] = s->color;
    buf[5] = (uint8_t)len;
    for (uint32_t i = 0; i < len; i++) buf[6 + i] = s->text[i];
    uint32_t tokens[CFW_SHAPE_INLINE_MAX];
    cfw_builtin_draw_string_buf(r->buf, r->stride, (uint32_t)clip_w, (uint32_t)clip_h,
                                buf, 6 + len, rl, tokens, CFW_SHAPE_INLINE_MAX);
}

static void cfw_shape_draw(const cfw_raster *r, const cfw_shape *s, cfw_rectlist *rl) {
    if (s->type == CFW_SHAPE_TEXT_INLINE) {
        cfw_shape_draw_inline_text(r, s, rl);
        return;
    }
    if (s->type == CFW_SHAPE_IMAGE || s->type == CFW_SHAPE_TEXT ||
        s->type == CFW_SHAPE_TEXT_CACHED) {
        cfw_shape_draw_texture(r, s, rl);
        return;
    }
    uint8_t xm, ym;
    cfw_shape_masks(s->type, &xm, &ym);
    int32_t p[CFW_SHAPE_PARAMS];
    for (uint32_t i = 0; i < CFW_SHAPE_PARAMS; i++) {
        int32_t v = s->p[i];
        if ((xm | ym) & (1u << i)) v = cfw_clamp(v, CFW_COORD_MIN, CFW_COORD_MAX);
        p[i] = v;
    }
    uint8_t c = s->color & 0x0fu;
    int32_t wd = s->width;
    switch (s->type) {
    case CFW_SHAPE_LINE:
        rs_line(r, p[0], p[1], p[2], p[3], wd, c);
        break;
    case CFW_SHAPE_RECT:
    case CFW_SHAPE_RECT_FILL:
        rs_rrect(r, p[0], p[1], cfw_clamp(p[2], 0, CFW_DIM_MAX), cfw_clamp(p[3], 0, CFW_DIM_MAX),
                 cfw_clamp(p[4], 0, CFW_DIM_MAX),
                 s->type == CFW_SHAPE_RECT ? (wd < 1 ? 1 : wd) : 0, c);
        break;
    case CFW_SHAPE_CIRCLE:
    case CFW_SHAPE_CIRCLE_FILL:
        rs_circle(r, p[0], p[1], cfw_clamp(p[2], 0, CFW_COORD_MAX),
                  s->type == CFW_SHAPE_CIRCLE ? (wd < 1 ? 1 : wd) : 0, c);
        break;
    case CFW_SHAPE_TRI:
    case CFW_SHAPE_QUAD: {
        uint32_t n = s->type == CFW_SHAPE_TRI ? 3u : 4u;
        int32_t px[4] = { p[0], p[2], p[4], p[6] };
        int32_t py[4] = { p[1], p[3], p[5], p[7] };
        rs_stroke_poly(r, px, py, n, wd, c);
        break;
    }
    case CFW_SHAPE_TRI_FILL:
    case CFW_SHAPE_QUAD_FILL: {
        uint32_t n = s->type == CFW_SHAPE_TRI_FILL ? 3u : 4u;
        int32_t px[4], py[4];
        for (uint32_t i = 0; i < n; i++) {
            px[i] = cfw_pix_q4(p[2 * i]);
            py[i] = cfw_pix_q4(p[2 * i + 1]);
        }
        rs_fill_poly_q4(r, px, py, n, c);
        break;
    }
    case CFW_SHAPE_BEZIER2:
        rs_bezier2(r, p[0], p[1], p[2], p[3], p[4], p[5], wd, c);
        break;
    case CFW_SHAPE_BEZIER3:
        rs_bezier3(r, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], wd, c);
        break;
    case CFW_SHAPE_ARC:
        rs_arc(r, p[0], p[1], cfw_clamp(p[2], 0, CFW_COORD_MAX), p[3], p[4], wd, c);
        break;
    case CFW_SHAPE_PIE:
        rs_pie(r, p[0], p[1], cfw_clamp(p[2], 0, CFW_COORD_MAX), p[3], p[4], c);
        break;
    default:
        break;
    }
}

/* Mode 36 payload: [count:u8][record x count]. Draws straight into the shadow,
 * so it composes with the other shadow modes inside a mode-8 batch. The whole
 * list is validated before any pixel is written. */
static int cfw_shapes_immediate(uint8_t *shadow, uint32_t stride,
                                uint32_t panel_w, uint32_t panel_h,
                                const uint8_t *src, uint32_t len,
                                cfw_rectlist *rl) {
    if (shadow == 0 || src == 0 || len < 1u) return -1;
    uint32_t count = src[0];
    uint32_t pos = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t n = cfw_shape_record_len(src + pos, len - pos);
        if (n == 0) return -1;
        cfw_shape s;
        cfw_shape_decode(src + pos, &s);
        if (!cfw_shape_valid(&s)) return -1;
        pos += n;
    }
    if (pos != len) return -1;
    cfw_raster r = { shadow, stride, (int32_t)panel_w, (int32_t)panel_h };
    pos = 1;
    for (uint32_t i = 0; i < count; i++) {
        cfw_shape s;
        cfw_shape_decode(src + pos, &s);
        cfw_shape_draw(&r, &s, rl);
        pos += cfw_shape_record_len(src + pos, len - pos);
    }
    rl_add(rl, 0, 0, panel_w, panel_h);
    return 0;
}

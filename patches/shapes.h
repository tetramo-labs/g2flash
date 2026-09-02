#pragma once
#include <stdint.h>
#include "debug.h"

/* ---- Shape records (modes 16 and 17) ---------------------------------------
 *
 * One record is 20 bytes: [type:u8][flags:u8][color:u8][width:u8][p0..p7:int16 LE].
 * Geometry parameters are signed pixel coordinates in the 640x480 panel space
 * (negative and oversized values are clipped). Types and their parameters:
 *
 *   1  LINE          x1 y1 x2 y2                      width = stroke (1 = hairline)
 *   2  RECT          x y w h r                        stroked rounded rect, r = corner radius
 *   3  RECT_FILL     x y w h r
 *   4  CIRCLE        cx cy r                          stroked ring
 *   5  CIRCLE_FILL   cx cy r
 *   6  TRI           x1 y1 x2 y2 x3 y3                stroked
 *   7  TRI_FILL      x1 y1 x2 y2 x3 y3
 *   8  QUAD          x1 y1 x2 y2 x3 y3 x4 y4          stroked
 *   9  QUAD_FILL     x1 y1 x2 y2 x3 y3 x4 y4
 *  10  BEZIER2       x0 y0 cx cy x1 y1                quadratic, stroked
 *  11  BEZIER3       x0 y0 c0x c0y c1x c1y x1 y1      cubic, stroked
 *  12  ARC           cx cy r a0 a1                    stroked arc, degrees, 0 = +x, clockwise
 *  13  PIE           cx cy r a0 a1                    filled sector
 *  14  IMAGE         x y texoff                       cached RLE image (mode 13 semantics)
 *  15  TEXT          x y texoff len                   UTF-8 bytes at texoff, built-in 20 px font
 *  16  TEXT_CACHED   x y texoff len fontoff           bytes at texoff drawn with a mode-14 font
 *
 * `color` is the 4-bit gray for geometry. For IMAGE/TEXT types it is the
 * mode-13/14/15 options byte (low nibble top color, bit 4 transparent source
 * color 0, bit 5 inverse ramp). `flags` bit 0 = visible (retained scenes only).
 * Circles are centered on pixel (cx,cy) and cover an odd 2r+1 px diameter (r = 1
 * is a 3x3 dot); polygons, wide lines and curves are sampled at pixel centers. */

#define CFW_SHAPE_RECORD_BYTES 20u
#define CFW_SHAPE_PARAMS       8u

#define CFW_SHAPE_NONE          0u
#define CFW_SHAPE_LINE          1u
#define CFW_SHAPE_RECT          2u
#define CFW_SHAPE_RECT_FILL     3u
#define CFW_SHAPE_CIRCLE        4u
#define CFW_SHAPE_CIRCLE_FILL   5u
#define CFW_SHAPE_TRI           6u
#define CFW_SHAPE_TRI_FILL      7u
#define CFW_SHAPE_QUAD          8u
#define CFW_SHAPE_QUAD_FILL     9u
#define CFW_SHAPE_BEZIER2      10u
#define CFW_SHAPE_BEZIER3      11u
#define CFW_SHAPE_ARC          12u
#define CFW_SHAPE_PIE          13u
#define CFW_SHAPE_IMAGE        14u
#define CFW_SHAPE_TEXT         15u
#define CFW_SHAPE_TEXT_CACHED  16u
#define CFW_SHAPE_TYPE_MAX     16u

#define CFW_SHAPE_FLAG_VISIBLE 0x01u
#define CFW_SHAPE_TEXT_MAX     64u   /* bytes per TEXT/TEXT_CACHED slot */

/* Packed-4bpp render target; every primitive clips to w x h. */
typedef struct {
    uint8_t *buf;
    uint32_t stride;
    int32_t  w;
    int32_t  h;
} cfw_raster;

/* A decoded record. */
typedef struct {
    uint8_t type;
    uint8_t flags;
    uint8_t color;
    uint8_t width;
    int16_t p[CFW_SHAPE_PARAMS];
} cfw_shape;

static void cfw_shape_decode(const uint8_t *rec, cfw_shape *out);
static int  cfw_shape_valid(const cfw_shape *s);
static void cfw_shape_draw(const cfw_raster *r, const cfw_shape *s, cfw_rectlist *rl);
static void cfw_shape_masks(uint8_t type, uint8_t *xmask, uint8_t *ymask);
static void cfw_raster_clear(const cfw_raster *r, uint8_t color);
static int  cfw_shapes_immediate(uint8_t *shadow, uint32_t stride,
                                 uint32_t panel_w, uint32_t panel_h,
                                 const uint8_t *src, uint32_t len,
                                 cfw_rectlist *rl);

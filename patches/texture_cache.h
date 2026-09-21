#pragma once
#include <stdint.h>
#include "debug.h"

/* Capacity of the asset store (scene.c). Kept under its historical name: the
 * 256 KiB block on the EvenHub heap is the same allocation, but revision 38
 * owns it through the asset allocator instead of exposing raw offsets. */
#define CFW_TEXTURE_CACHE_SIZE (256u * 1024u)

/* Cached image format:  [width:u8][height:u8][4bpp RLE tokens...]
 * Font asset format:    [96 x u32 glyph offsets, relative to the asset][glyph images]
 *
 * Every helper takes an explicit (base, size) region: an asset resolved by the
 * object cache, never the whole store. Offsets are uint32 and checked as
 * `offset <= size && length <= size - offset` before any access. */
typedef struct {
    const uint8_t *rle;
    uint32_t rle_len;
    uint32_t width;
    uint32_t height;
} cfw_cached_image;

#define CFW_FONT_TABLE_CHARS 96u
#define CFW_FONT_TABLE_BYTES (CFW_FONT_TABLE_CHARS * 4u)

static int cfw_texture_image_in(const uint8_t *base, uint32_t size, uint32_t offset,
                                cfw_cached_image *out);
static int cfw_texture_font_valid(const uint8_t *font, uint32_t size);
static int cfw_texture_draw_image_region(uint8_t *shadow, uint32_t stride,
                                         uint32_t panel_w, uint32_t panel_h,
                                         const uint8_t *data, uint32_t size,
                                         int32_t x, int32_t y, uint8_t options,
                                         cfw_rectlist *rl);
static int cfw_texture_draw_string_region(uint8_t *shadow, uint32_t stride,
                                          uint32_t panel_w, uint32_t panel_h,
                                          const uint8_t *font, uint32_t font_size,
                                          int32_t x, int32_t y, uint8_t options,
                                          const uint8_t *string, uint32_t string_len,
                                          cfw_rectlist *rl);
static int cfw_texture_string_valid(const uint8_t *s, uint32_t len);
static int cfw_builtin_draw_string(uint8_t *shadow, uint32_t stride,
                                   uint32_t panel_w, uint32_t panel_h,
                                   const uint8_t *src, uint32_t len,
                                   cfw_rectlist *rl);
static int cfw_builtin_draw_string_buf(uint8_t *shadow, uint32_t stride,
                                       uint32_t panel_w, uint32_t panel_h,
                                       const uint8_t *src, uint32_t len,
                                       cfw_rectlist *rl,
                                       uint32_t *tokens, uint32_t max_tokens);

#pragma once
#include <stdint.h>

/* 64 KiB on heap 13, the footprint revision 29 ran with. Upstream's 256 KiB
 * lives on the EvenHub heap, but that heap also carries the stock page objects
 * and a lease that lapses without a caller never frees the block; scene
 * records address the cache with uint16 offsets, so 64 KiB is all a client
 * can use. The wire stays uint32 (modes 18/19/20). */
#define CFW_TEXTURE_CACHE_SIZE (64u * 1024u)

static void cfw_texture_cache_release(customCfwContext *ctx);
static int cfw_texture_cache_update(const uint8_t *src, uint32_t len);
/* Modes 19/20 (uint32 cache offsets, uint32 glyph table). The *_at helpers take
 * already-parsed arguments so shape records (shapes.c) can draw cached images
 * and cached-font strings without re-encoding a mode payload. */
static int cfw_texture_draw_image_at(uint8_t *shadow, uint32_t stride,
                                     uint32_t panel_w, uint32_t panel_h,
                                     uint32_t offset, int32_t x, int32_t y,
                                     uint8_t options, cfw_rectlist *rl);
static int cfw_texture_draw_image(uint8_t *shadow, uint32_t stride,
                                  uint32_t panel_w, uint32_t panel_h,
                                  const uint8_t *src, uint32_t len,
                                  cfw_rectlist *rl);
static int cfw_texture_draw_string_at(uint8_t *shadow, uint32_t stride,
                                      uint32_t panel_w, uint32_t panel_h,
                                      uint32_t font_offset, int32_t x, int32_t y,
                                      uint8_t options, const uint8_t *string,
                                      uint32_t string_len, cfw_rectlist *rl);
static int cfw_texture_draw_string(uint8_t *shadow, uint32_t stride,
                                   uint32_t panel_w, uint32_t panel_h,
                                   const uint8_t *src, uint32_t len,
                                   cfw_rectlist *rl);
static int cfw_builtin_draw_string(uint8_t *shadow, uint32_t stride,
                                   uint32_t panel_w, uint32_t panel_h,
                                   const uint8_t *src, uint32_t len,
                                   cfw_rectlist *rl);
static int cfw_builtin_draw_string_buf(uint8_t *shadow, uint32_t stride,
                                       uint32_t panel_w, uint32_t panel_h,
                                       const uint8_t *src, uint32_t len,
                                       cfw_rectlist *rl,
                                       uint32_t *tokens, uint32_t max_tokens);

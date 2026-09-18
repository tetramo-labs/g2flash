#pragma once
#include <stdint.h>

#define CFW_TEXTURE_CACHE_SIZE (256u * 1024u)

static void cfw_texture_cache_release(customCfwContext *ctx);
static int cfw_texture_cache_update(const uint8_t *src, uint32_t len, int wide);
/* Modes 13/14 carry uint16 cache offsets (wide = 0); modes 19/20 carry uint32
 * offsets and a uint32 glyph table (wide = 1). Everything else is shared. */
static int cfw_texture_draw_image(uint8_t *shadow, uint32_t stride,
                                  uint32_t panel_w, uint32_t panel_h,
                                  const uint8_t *src, uint32_t len,
                                  cfw_rectlist *rl);
static int cfw_texture_draw_image_wide(uint8_t *shadow, uint32_t stride,
                                       uint32_t panel_w, uint32_t panel_h,
                                       const uint8_t *src, uint32_t len,
                                       cfw_rectlist *rl);
static int cfw_texture_draw_string(uint8_t *shadow, uint32_t stride,
                                   uint32_t panel_w, uint32_t panel_h,
                                   const uint8_t *src, uint32_t len,
                                   cfw_rectlist *rl);
static int cfw_texture_draw_string_wide(uint8_t *shadow, uint32_t stride,
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

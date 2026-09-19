#include "cfw_context.h"

#define CFW_FRAMEBUFFER_BYTES (640u * 480u / 2u)
/* Revision 33: the shadow lives on the stock EvenHub heap (0x202020a8, 450 KiB),
 * where the 576x288 image container's buffers (2 x 166 KiB) lived until revision
 * 30 and where nothing of ours lives now. Heap 13 is LVGL's heap; this fork's
 * retained scene frame already takes 150 KiB there and revision 31's shadow +
 * transport buffers on top of it starved LVGL on hardware (pages rendered but
 * never completed, shadow modes failed while control modes acked). */
#ifndef CFW_IMAGE_ALLOC
#define CFW_IMAGE_ALLOC cfw_malloc
#define CFW_IMAGE_FREE FW_FREE
#endif

/* The caller owns the display gate until its refresh is consumed. */
static uint8_t *cfw_shadow_buffer(void) {
    customCfwContext *ctx = getCustomCfwContext();
    if (!ctx) return 0;
    if (!ctx->framebuffer_shadow) {
        uint8_t *buffer = CFW_IMAGE_ALLOC(CFW_FRAMEBUFFER_BYTES);
        if (!buffer) return 0;
        for (uint32_t i = 0; i < CFW_FRAMEBUFFER_BYTES; ++i) buffer[i] = 0;
        ctx->framebuffer_shadow = buffer;
    }
    return ctx->framebuffer_shadow;
}

static void cfw_shadow_release(customCfwContext *ctx) {
    if (ctx->framebuffer_shadow) CFW_IMAGE_FREE(ctx->framebuffer_shadow);
    ctx->framebuffer_shadow = 0;
}

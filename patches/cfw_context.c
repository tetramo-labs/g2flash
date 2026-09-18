#include "cfw_context.h"

#ifndef CFW_CONTEXT_POINTER_VALID
#define CFW_CONTEXT_POINTER_VALID(ctx) \
    (((uintptr_t)(ctx) & 3) == 0 && (uintptr_t)(ctx) - 0x20000000u < 0x00800000u)
#endif

/* Return the singleton only if it already exists and passes the slot/magic checks.
 * Ordinary stock refreshes pass through display_copy_hook, so that hook must never
 * allocate CFW state. */
static customCfwContext *peekCustomCfwContext(void) {
    customCfwContext *ctx = __atomic_load_n((customCfwContext **)CFW_CTX_SLOT, __ATOMIC_ACQUIRE);
    if (CFW_CONTEXT_POINTER_VALID(ctx) && ctx->magic == CFW_CTX_MAGIC &&
        ctx->ctx_size == (uint32_t)sizeof(customCfwContext))   /* 2.2.10.38: same layout only */
        return ctx;
    return 0;
}

/* Fetch (or lazily create) the CFW singleton context. Its pointer lives in the
 * explicitly reserved primary-TLSF tail (CFW_CTX_SLOT); we only ever touch that
 * word through this helper (image traffic or the private settings lease). The
 * slot ptr is range-checked to SRAM and the struct's magic verified before
 * trusting it, so warm-reset garbage can't be mistaken for a live context.
 * Returns 0 if the one-time struct malloc fails. */
static customCfwContext *getCustomCfwContext(void) {
    for (;;) {
        /* BLE, the bridge and the EvenHub deferred task can create the context
         * concurrently. Publish only fully initialized state; free a losing
         * allocation instead of replacing another task's context (which may
         * already own a shadow or a transport stream buffer). */
        customCfwContext *expected = __atomic_load_n((customCfwContext **)CFW_CTX_SLOT, __ATOMIC_ACQUIRE);
        customCfwContext *ctx = peekCustomCfwContext();
        if (ctx) return ctx;
        ctx = (customCfwContext *)cfw_malloc(sizeof(customCfwContext));
        if (!ctx) return 0;
        bzero((uint8_t *)ctx, sizeof(customCfwContext));
        ctx->magic = CFW_CTX_MAGIC;
        ctx->ctx_size = (uint32_t)sizeof(customCfwContext);   /* 2.2.10.38 */
        ctx->diag_hide = 1;    /* overlay off by default; mode 7 sub 2 turns it on */
        for (uint32_t i = 0; i < CFW_FID_RING; i++) ctx->recent_fids[i] = 0xffff;  /* sentinel */
        if (__atomic_compare_exchange_n((customCfwContext **)CFW_CTX_SLOT, &expected,
                                         ctx, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return ctx;
        FW_FREE(ctx);
    }
}

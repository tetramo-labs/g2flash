#pragma once
#include <stdint.h>

/* Upstream sensor controls share mode numbers with our drawing protocol.
 * Complete shape records need at least 15 bytes; scenes need at least three.
 * Mode 19 is an unambiguous ALS alias for new clients. [16,0] was an empty
 * shape list and now also queries ALS; it still changes no shadow pixels. */
static int cfw_is_als_control(const uint8_t *src, uint32_t len) {
    if (!src || len < 2) return 0;
    uint8_t mode = src[0] & 0x7fu;
    return mode == 19 ||
           (mode == 16 && len <= 9 && src[1] <= 2);
}

static int cfw_is_ring_battery_control(const uint8_t *src, uint32_t len) {
    return src && len == 2 && src[0] == 17 && src[1] == 0;
}

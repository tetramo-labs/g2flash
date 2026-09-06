#pragma once
#include <stdint.h>

/* Revision 21: bounded, compiled SVG paths; see VECTOR_PROTOCOL.md. */
#define CFW_SHAPE_PATH 18u  /* retained only, constructed by SET_PATH */
#define CFW_PATH_MAX_EDGES 512u
#define CFW_PATH_MAX_BYTES 8192u
#define CFW_PATH_SCENE_EDGES 4096u
#define CFW_PATH_MAX_COMMANDS 1024u
typedef struct { int32_t x0, y0, x1, y1; } cfw_edge;
typedef struct {
    uint16_t count;
    uint8_t rule; /* 0 nonzero, 1 even-odd */
    uint8_t pad;
    cfw_edge edges[];
} cfw_path;
typedef struct {
    int32_t angle, from, to; /* unwrapped degrees * 256 */
    int16_t px, py;         /* panel-space pivot, pixels */
    uint32_t started;
    uint16_t duration;      /* milliseconds; 0 idle */
    uint8_t curve[4];
} cfw_rotation;

/* Heap workspace, never a large RTOS stack allocation. Shared under display gate. */
typedef struct {
    cfw_edge edges[CFW_PATH_MAX_EDGES];
    int32_t xs[CFW_PATH_MAX_EDGES];
    int8_t winds[CFW_PATH_MAX_EDGES];
} cfw_vector_work;

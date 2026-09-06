/* Offline asset preparation uses the actual firmware compiler to check limits. */
#define main unused_shapes_test_main
#include "shapes_host_test.c"
#undef main

int main(void) {
    uint8_t data[CFW_PATH_MAX_BYTES + 1];
    size_t size = fread(data, 1, sizeof(data), stdin);
    if (ferror(stdin)) return 2;
    cfw_vector_work *work = calloc(1, sizeof(*work));
    if (!work) return 2;
    int edges = cv_compile(work, data, (uint32_t)size);
    free(work);
    printf("%d\n", edges);
    return edges < 0 ? 1 : 0;
}

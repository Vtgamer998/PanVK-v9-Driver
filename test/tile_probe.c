#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"

static void dump_tiles(struct v9_cmd_buffer *cmd, uint32_t W, uint32_t H) {
    for (uint32_t ty = 0; ty < H / 16; ty++) {
        for (uint32_t tx = 0; tx < W / 16; tx++) {
            int g = 0;
            for (uint32_t y = ty * 16; y < (ty + 1) * 16 && y < H; y++)
                for (uint32_t x = tx * 16; x < (tx + 1) * 16 && x < W; x++)
                    if (v9_cmd_buffer_read_pixel(cmd, x, y) == 0xFF00FF00) g++;
            printf("[%u,%u]=%4d ", tx, ty, g);
        }
        printf("\n");
    }
}

int main(int argc, char **argv) {
    uint32_t W = argc > 1 ? atoi(argv[1]) : 32;
    uint32_t H = argc > 2 ? atoi(argv[2]) : 32;
    float x0 = argc > 3 ? atof(argv[3]) : -0.5f * W;
    float y0 = argc > 4 ? atof(argv[4]) : -0.5f * H;

    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) { fprintf(stderr, "no dev\n"); return 1; }

    struct v9_render_target_config config = { .width = W, .height = H, .clear_color = 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) { fprintf(stderr, "no cmd\n"); return 1; }

    struct pan_kmod_bo *posbo = pan_kmod_bo_alloc(
        dev, 48, PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
    if (!posbo) return 1;
    float *pos = posbo->cpu;
    pos[0] = x0;            pos[1] = y0;            pos[2] = 0.5f; pos[3] = 1.0f;
    pos[4] = x0 + 2.0f * W; pos[5] = y0;            pos[6] = 0.5f; pos[7] = 1.0f;
    pos[8] = x0;            pos[9] = y0 + 2.0f * H; pos[10] = 0.5f; pos[11] = 1.0f;

    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed(cmd, 0, 0, 0, posbo->gpu, 3);
    v9_cmd_buffer_end(cmd);
    int ret = v9_cmd_buffer_submit(cmd);
    printf("submit rc=%d  fb=%ux%u tri=(%.1f,%.1f)-(%.1f,%.1f)-(%.1f,%.1f)\n",
           ret, W, H, pos[0], pos[1], pos[4], pos[5], pos[8], pos[9]);
    dump_tiles(cmd, W, H);

    int total = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            if (v9_cmd_buffer_read_pixel(cmd, x, y) == 0xFF00FF00) total++;
    printf("total green: %d / %d\n", total, W * H);

    /* Dump polygon list entries written by the tiler */
    uint8_t *bcpu = v9_cmd_buffer_get_mem_cpu(cmd);
    uint64_t bbase = v9_cmd_buffer_get_mem_gpu(cmd);
    uint64_t pl = v9_cmd_buffer_get_polylist_gpu(cmd);
    uint32_t *plw = (uint32_t *)(bcpu + (pl - bbase));
    unsigned tiles = ((W + 15) / 16) * ((H + 15) / 16);
    printf("polylist entries (%u tiles):\n", tiles);
    for (unsigned i = 0; i < tiles; i++) {
        uint32_t w0 = plw[i * 2], w1 = plw[i * 2 + 1];
        printf("  tile[%u]: 0x%08x 0x%08x%s\n", i, w0, w1,
               (w0 || w1) ? "  <-- WRITTEN" : "");
    }

    pan_kmod_bo_free(posbo);

    pan_kmod_bo_free(posbo);
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

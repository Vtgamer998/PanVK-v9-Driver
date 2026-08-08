#include <stdio.h>
#include <stdlib.h>
#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"

int main(int argc, char **argv) {
    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) return 1;

    unsigned sizes[][2] = {
        {16,16}, {32,32}, {64,64}, {96,96}, {128,128},
        {160,160}, {240,240}, {320,320}, {400,400}, {480,480},
        {640,480}, {800,600}, {1280,720}
    };
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        unsigned w = sizes[i][0], h = sizes[i][1];
        struct v9_render_target_config config = { w, h, 0xFF0000FF };
        struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
        if (!cmd) { printf("%ux%u: create FAILED\n", w, h); continue; }
        v9_cmd_buffer_begin(cmd);
        v9_cmd_draw_indexed_triangle(cmd);
        v9_cmd_buffer_end(cmd);
        int ret = v9_cmd_buffer_submit(cmd);

        unsigned sx = w > 80 ? w / 40 : 1;
        unsigned sy = h > 80 ? h / 30 : 1;
        int green = 0, other = 0;
        for (unsigned y = 0; y < h; y += sy)
            for (unsigned x = 0; x < w; x += sx) {
                uint32_t p = v9_cmd_buffer_read_pixel(cmd, x, y);
                if (p == 0xFF00FF00) green++; else other++;
            }
        printf("%4ux%-4u ret=%d green=%d/%d\n", w, h, ret, green, green + other);
        v9_cmd_buffer_destroy(cmd);
    }
    pan_kmod_dev_destroy(dev);
    return 0;
}

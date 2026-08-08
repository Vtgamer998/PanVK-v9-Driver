/*
 * Test harness for Step 2: Valhall v9 GenXML Descriptor Pack & Command Buffer Engine
 */

#include <stdio.h>
#include <stdlib.h>

#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("=== Testing Step 2: Valhall v9 Command Buffer & GenXML Pack Engine ===\n");

    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) {
        fprintf(stderr, "FAIL: pan_kmod_dev_create returned NULL\n");
        return 1;
    }

    struct v9_render_target_config config = {
        .width = 800,
        .height = 600,
        .clear_color = 0xFF0000FF,
    };

    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) {
        fprintf(stderr, "FAIL: v9_cmd_buffer_create returned NULL\n");
        pan_kmod_dev_destroy(dev);
        return 1;
    }
    printf("SUCCESS: v9_cmd_buffer created for %dx%d render target\n", config.width, config.height);

    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed_triangle(cmd);
    v9_cmd_buffer_end(cmd);
    printf("SUCCESS: Command buffer recorded (TILER_JOB + Fragment JC)\n");

    int ret = v9_cmd_buffer_submit(cmd);
    if (ret != 0) {
        fprintf(stderr, "FAIL: v9_cmd_buffer_submit failed (ret=%d)\n", ret);
        v9_cmd_buffer_destroy(cmd);
        pan_kmod_dev_destroy(dev);
        return 1;
    }
    printf("SUCCESS: v9_cmd_buffer_submit completed all 4 atoms cleanly!\n");

    uint32_t green_count = 0;
    for (uint32_t y = 0; y < config.height; y++) {
        for (uint32_t x = 0; x < config.width; x++) {
            if (v9_cmd_buffer_read_pixel(cmd, x, y) == 0xFF00FF00) green_count++;
        }
    }
    printf("Rendered Output: %u / %u pixels rendered solid green (0xFF00FF00)!\n",
           green_count, config.width * config.height);

    if (green_count > 0) {
        printf("SUCCESS: %u pixels rendered solid green (0xFF00FF00)!\n", green_count);
    } else {
        fprintf(stderr, "FAIL: Expected >0 green pixels, got 0\n");
        v9_cmd_buffer_destroy(cmd);
        pan_kmod_dev_destroy(dev);
        return 1;
    }

    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);

    printf("=== Step 2: Valhall v9 GenXML Pack & Command Engine Test PASSED CLEANLY! ===\n");
    return 0;
}

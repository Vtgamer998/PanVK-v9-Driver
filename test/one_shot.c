#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"

int main(int argc, char **argv) {
    unsigned w = 800, h = 600;
    if (argc >= 3) { w = atoi(argv[1]); h = atoi(argv[2]); }

    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) return 1;

    struct v9_render_target_config config = { w, h, 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) return 1;

    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed_triangle(cmd);
    v9_cmd_buffer_end(cmd);

    int ret = v9_cmd_buffer_submit(cmd);
    uint32_t p = v9_cmd_buffer_read_pixel(cmd, 0, 0);
    printf("SIZE %ux%u: ret=%d pixel(0,0)=0x%08x green=%d\n", w, h, ret, p, p == 0xFF00FF00);

    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return ret == 0 && p == 0xFF00FF00 ? 0 : 1;
}

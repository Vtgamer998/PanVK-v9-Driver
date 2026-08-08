#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"

int main(void) {
    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) return 1;

    struct v9_render_target_config config = { 800, 600, 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) return 1;

    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed_triangle(cmd);
    v9_cmd_buffer_end(cmd);

    int fails = 0, pass = 0, green = 0;
    for (int i = 0; i < 10; i++) {
        int ret = v9_cmd_buffer_submit(cmd);
        uint32_t p = v9_cmd_buffer_read_pixel(cmd, 0, 0);
        if (ret == 0 && p == 0xFF00FF00) { pass++; green++; }
        else if (ret == 0) pass++;
        else fails++;
        usleep(50000);
    }
    printf("RESULT: pass=%d fails=%d green=%d\n", pass, fails, green);

    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

#include <stdio.h>
#include <string.h>
#include <poll.h>
#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"

#define KBASE_QUEUE_REQ_FRAGMENT (0x041u)

static int drain(struct pan_kmod_dev *dev, const char *tag) {
    int n = 0;
    for (int e = 0; e < 8; e++) {
        struct pollfd pfd;
        pfd.fd = pan_kmod_dev_fd(dev);
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 30);
        if (pr <= 0) break;
        uint32_t a = 0, c = 0;
        pan_kmod_wait_event(dev, &a, &c);
        printf("  %s drain %d: atom=%u code=0x%x\n", tag, e, a, c);
        n++;
    }
    return n;
}

int main(void) {
    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) return 1;
    struct v9_render_target_config config = { 800, 600, 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) return 1;
    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed_triangle(cmd);
    v9_cmd_buffer_end(cmd);

    for (int i = 0; i < 4; i++) {
        uint32_t code = 0;
        pan_kmod_submit_atom(v9_cmd_buffer_get_dev(cmd),
                             v9_cmd_buffer_get_frag_jc_gpu(cmd),
                             KBASE_QUEUE_REQ_FRAGMENT, 2, &code);
        printf("iter %d: submit code=0x%x pixel=0x%08x\n", i, code,
               v9_cmd_buffer_read_pixel(cmd, 0, 0));
        drain(v9_cmd_buffer_get_dev(cmd), "frag");
    }

    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

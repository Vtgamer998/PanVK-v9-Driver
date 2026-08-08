#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"

#define KBASE_QUEUE_REQ_FRAGMENT (0x041u)

int main(void) {
    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) return 1;
    struct v9_render_target_config config = { 800, 600, 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) return 1;
    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed_triangle(cmd);
    v9_cmd_buffer_end(cmd);

    uint32_t code = 0;
    int ret = pan_kmod_submit_atom(v9_cmd_buffer_get_dev(cmd),
                                   v9_cmd_buffer_get_frag_jc_gpu(cmd),
                                   KBASE_QUEUE_REQ_FRAGMENT, 2, &code);
    printf("submit: ret=%d code=0x%x\n", ret, code);

    for (int e = 0; e < 6; e++) {
        struct pollfd pfd;
        pfd.fd = pan_kmod_dev_fd(v9_cmd_buffer_get_dev(cmd));
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 50);
        if (pr <= 0) { printf("drain %d: poll=%d (no more events)\n", e, pr); break; }
        uint32_t a = 0, c = 0;
        int r = pan_kmod_wait_event(v9_cmd_buffer_get_dev(cmd), &a, &c);
        printf("drain %d: ret=%d atom=%u code=0x%x\n", e, r, a, c);
    }

    printf("pixel(0,0)=0x%08x\n", v9_cmd_buffer_read_pixel(cmd, 0, 0));
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"

#define KBASE_QUEUE_REQ_FRAGMENT (0x041u)

struct raw_event { uint32_t event_code; uint8_t atom_number; uint8_t prio; uint8_t jobslot; uint8_t unused; uint64_t timer; uint64_t udata; } __attribute__((packed));

int main(void) {
    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) return 1;
    int fd = pan_kmod_dev_fd(dev);
    struct v9_render_target_config config = { 800, 600, 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) return 1;
    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed_triangle(cmd);
    v9_cmd_buffer_end(cmd);
    int codes[16] = {0}; int n = 0;
    for (int i = 0; i < 16; i++) {
        pan_kmod_submit_atom(v9_cmd_buffer_get_dev(cmd), v9_cmd_buffer_get_frag_jc_gpu(cmd),
                             KBASE_QUEUE_REQ_FRAGMENT, 2, NULL);
        struct raw_event ev; memset(&ev, 0, sizeof(ev));
        ssize_t r = read(fd, &ev, sizeof(ev));
        if (r <= 0) { printf("iter %d READ r=%zd\n", i, r); break; }
        printf("iter %d: code=0x%x atom=%u pixel=0x%08x\n", i, ev.event_code, ev.atom_number,
               v9_cmd_buffer_read_pixel(cmd, 0, 0));
        int found = 0;
        for (int j = 0; j < n; j++) if (codes[j] == ev.event_code) { found = 1; break; }
        if (!found && n < 16) codes[n++] = ev.event_code;
    }
    printf("unique codes:");
    for (int j = 0; j < n; j++) printf(" 0x%x", codes[j]);
    printf("\n");
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

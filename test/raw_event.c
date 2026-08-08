#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"
#include "kbase_winsys.h"

#define KBASE_QUEUE_REQ_FRAGMENT (0x041u)
#define KBASE_IOCTL_JOB_SUBMIT _IOC(_IOC_WRITE, 0x80, 2, 16)

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

    for (int i = 0; i < 16; i++) {
        struct kbase_atom_mtk atom;
        memset(&atom, 0, sizeof(atom));
        atom.jc = v9_cmd_buffer_get_frag_jc_gpu(cmd);
        atom.atom_number = 2;
        atom.core_req = KBASE_QUEUE_REQ_FRAGMENT;
        struct { uint64_t addr; uint32_t nr_atoms; uint32_t stride; } submit = {
            (uint64_t)&atom, 1, sizeof(atom) };
        if (ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0) {
            perror("submit"); break;
        }
        struct raw_event ev; memset(&ev, 0, sizeof(ev));
        ssize_t r = read(fd, &ev, sizeof(ev));
        if (r <= 0) { printf("iter %d READ r=%zd\n", i, r); break; }
        printf("iter %d: code=0x%x atom=%u prio=%u jobslot=%u timer=%llu udata=%llu pixel=0x%08x\n",
               i, ev.event_code, ev.atom_number, ev.prio, ev.jobslot,
               (unsigned long long)ev.timer, (unsigned long long)ev.udata,
               v9_cmd_buffer_read_pixel(cmd, 0, 0));
    }

    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

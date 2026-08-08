#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"

/*
 * Multi-frame probe for the MTK r49 "one-frame-per-process" limitation.
 *
 * Hypothesis under test: the fragment atom leaves the kbase context's job
 * slots in a state that makes the NEXT tiler submission hang.  We test two
 * variants via env:
 *   TWO_FRAME_REUSE=1  -> resubmit the SAME cmd buffer (same BOs) each frame.
 *   (default)          -> fresh cmd buffer + BOs per frame (current behaviour).
 *
 * Run ONE at a time, e.g.:
 *   ./run_single_test.sh two_frame
 *   TWO_FRAME_REUSE=1 ./run_single_test.sh two_frame
 *   V9_FRAG_SINGLE_JOB=1 ./run_single_test.sh two_frame
 */

int main(void) {
    int reuse = getenv("TWO_FRAME_REUSE") && atoi(getenv("TWO_FRAME_REUSE")) == 1;
    int fresh_dev = getenv("TWO_FRAME_FRESH_DEV") && atoi(getenv("TWO_FRAME_FRESH_DEV")) == 1;
    int nframes = 3;
    const char *v = getenv("TWO_FRAME_N");
    if (v) nframes = atoi(v);
    if (nframes < 1) nframes = 1;

    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) { fprintf(stderr, "no dev\n"); return 1; }

    struct v9_cmd_buffer *reused = NULL;
    for (int f = 0; f < nframes; f++) {
        struct v9_cmd_buffer *cmd;
        if (reuse && reused) {
            cmd = reused;
        } else {
            struct v9_render_target_config config = { 16, 16, 0xFF0000FF };
            cmd = v9_cmd_buffer_create(dev, &config);
            if (!cmd) { printf("frame %d: create FAILED\n", f); break; }
            if (reuse) reused = cmd;
        }
        v9_cmd_buffer_begin(cmd);
        v9_cmd_draw_indexed_triangle(cmd);
        v9_cmd_buffer_end(cmd);
        int ret = v9_cmd_buffer_submit(cmd);
        int green = 0, blue = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint32_t p = v9_cmd_buffer_read_pixel(cmd, x, y);
                if (p == 0xFF00FF00) green++;
                else if (p == 0xFF0000FF) blue++;
            }
        printf("frame %d: ret=%d green=%d/256 blue=%d/256\n", f, ret, green, blue);
        if (!reuse) v9_cmd_buffer_destroy(cmd);
        if (fresh_dev) {
            /* Close + reopen /dev/mali0 so the kbase context is fresh and the
             * MTK r49 kernel doesn't carry the wedged job-slot state of the
             * previous frame's TERMINATED fragment. */
            pan_kmod_dev_destroy(dev);
            dev = pan_kmod_dev_create(NULL);
            if (!dev) { printf("frame %d: dev re-create FAILED\n", f); break; }
            if (reuse) reused = NULL;
        }
    }
    if (reused) v9_cmd_buffer_destroy(reused);
    pan_kmod_dev_destroy(dev);
    /* Give the kernel kbase driver time to finish MMU page table cleanup and
     * L2 cache flush before the next execution.  Without this delay, running
     * ./two_frame twice quickly can cause a memory collision on the GPU MMU
     * (remapping an area the hardware still considers "in use"). */
    usleep(2000000); /* 2 seconds */
    return 0;
}

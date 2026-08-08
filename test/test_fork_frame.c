/*
 * test_fork_frame.c — MTK r49 workaround: render each frame in a child process
 *
 * The MTK r49 kernel can wedge the GPU job-slot and silently drop fragment
 * completion events.  The wedge is per-process: when the process exits, the
 * kernel reclaims every slot, BO, and pending event automatically.  By forking
 * a fresh child per frame, every frame starts on a brand-new clean context
 * with no stale events and no wedged slot.
 *
 *   parent: forks N children, each writes its frame pixels into a shared
 *           mmap-anonymous buffer (parent reads after waitpid)
 *   child : opens /dev/mali0, submits tiler + flush + fragment (auto-reopen
 *           on 0x42 + drain on timeout, so a single child failure does not
 *           poison the parent), writes 256x256 RGBA pixels into the shared
 *           slot, and exits.
 *
 * If a child hangs past PANVK_CHILD_TIMEOUT_MS the parent SIGKILLs it; the
 * kernel still cleans up the GPU state, so subsequent frames are safe.
 *
 *   PANVK_DRY_RUN=1 ./test_fork_frame               # no GPU
 *   TEST_FRAMES=10 TEST_W=128 TEST_H=128 ./test_fork_frame
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "v9_cmd_stream.h"
#include "kbase_winsys.h"

#define MAX_W 256
#define MAX_H 256

static uint32_t *g_shared_rgba;     /* parent reads, child writes */
static int       g_w, g_h;

static int render_frame_in_child(int frame_idx, uint32_t *out_rgba) {
    /* (Re)initialise the GPU driver for this fresh process.  pan_kmod_dev_create
     * calls kbase_dev_open which performs VERSION_CHECK + SET_FLAGS + GET_GPUPROPS,
     * and kbase_drain_events discards any buffered stale events inherited from
     * the parent via fork(). */
    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) {
        fprintf(stderr, "[child %d] pan_kmod_dev_create failed\n", frame_idx);
        return -ENODEV;
    }

    struct v9_render_target_config cfg = {
        .width  = (uint32_t)g_w,
        .height = (uint32_t)g_h,
        .clear_color = 0xFF000000, /* black; green triangle should paint over it */
    };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &cfg);
    if (!cmd) {
        fprintf(stderr, "[child %d] v9_cmd_buffer_create failed\n", frame_idx);
        pan_kmod_dev_destroy(dev);
        return -ENOMEM;
    }

    int rc = v9_cmd_buffer_begin(cmd);
    fprintf(stderr, "[child %d] begin rc=%d\n", frame_idx, rc);
    if (rc == 0) {
        v9_cmd_buffer_set_attributes(cmd, NULL, 0);
        v9_cmd_draw_indexed_triangle(cmd);
        rc = v9_cmd_buffer_end(cmd);
        fprintf(stderr, "[child %d] end rc=%d\n", frame_idx, rc);
    }
    if (rc == 0) {
        rc = v9_cmd_buffer_submit(cmd);
        fprintf(stderr, "[child %d] submit rc=%d\n", frame_idx, rc);
    }

    /* Read pixels via the public API (color_bo is opaque).  The green triangle
     * is 0xFF00FF00 (little-endian ABGR: R=0, G=0xFF, B=0). */
    int green = 0;
    if (rc == 0) {
        uint32_t cy = (uint32_t)g_h / 2, cx = (uint32_t)g_w / 2;
        uint32_t p = v9_cmd_buffer_read_pixel(cmd, cx, cy);
        green = ((p & 0x0000FF00u) != 0); /* green byte non-zero */
        for (int y = 0; y < g_h; y++)
            for (int x = 0; x < g_w; x++)
                out_rgba[y * g_w + x] = v9_cmd_buffer_read_pixel(cmd, x, y);
        fprintf(stderr, "[child %d] center px=0x%08x green=%d\n", frame_idx, p, green);
    } else {
        fprintf(stderr, "[child %d] submit rc=%d\n", frame_idx, rc);
    }

    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    if (rc != 0)        return 1;
    return green ? 0 : 2;
}

int main(void) {
    int n_frames = 10;
    int timeout_ms = 2000;
    const char *env;

    if ((env = getenv("TEST_FRAMES")) && atoi(env) > 0) n_frames = atoi(env);
    if ((env = getenv("TEST_W"))      && atoi(env) > 0) g_w = atoi(env);
    else                                                g_w = 256;
    if ((env = getenv("TEST_H"))      && atoi(env) > 0) g_h = atoi(env);
    else                                                g_h = 256;
    if ((env = getenv("PANVK_CHILD_TIMEOUT_MS")) && atoi(env) > 0) timeout_ms = atoi(env);
    if (g_w > MAX_W || g_h > MAX_H) {
        fprintf(stderr, "TEST_W/TEST_H capped at %d\n", MAX_W);
        g_w = g_w > MAX_W ? MAX_W : g_w;
        g_h = g_h > MAX_H ? MAX_H : g_h;
    }

    /* Shared RGBA buffer — anonymous mmap is visible to the child after fork,
     * zero-copy, no IPC, no pipe roundtrip. */
    size_t bytes = (size_t)g_w * g_h * 4 * sizeof(uint32_t);
    g_shared_rgba = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_shared_rgba == MAP_FAILED) {
        perror("mmap shared");
        return 1;
    }
    memset(g_shared_rgba, 0, bytes);

    printf("test_fork_frame: %d frames at %dx%d, child timeout %dms\n",
           n_frames, g_w, g_h, timeout_ms);
    fflush(stdout);

    int green_count = 0, render_err = 0, killed_count = 0;
    for (int i = 0; i < n_frames; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            break;
        }
        if (pid == 0) {
            /* CHILD: render and exit.  exit() (vs return from main) flushes
             * stdio buffers and runs atexit handlers — neither matters here,
             * but it makes the GPU cleanup path easier to reason about. */
            int r = render_frame_in_child(i, g_shared_rgba);
            _exit(r == 0 ? 0 : 2);
        }

        /* PARENT: wait for child with timeout. */
        int status = 0;
        for (int waited = 0; waited < timeout_ms; waited += 50) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) { pid = -1; break; }
            if (w < 0)    { perror("waitpid"); break; }
            usleep(50 * 1000);
        }
        if (pid != -1) {
            /* Child hung — the GPU is genuinely wedged.  SIGKILL still
             * guarantees the kernel reclaims the slot when the process dies. */
            fprintf(stderr, "[frame %d] child hung past %dms — SIGKILL\n", i, timeout_ms);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            killed_count++;
            continue;
        }

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0) {
                /* Verify pixels in the shared buffer (parent view). */
                uint32_t p = g_shared_rgba[(g_h / 2) * g_w + (g_w / 2)];
                int green = ((p & 0x0000FF00u) != 0); /* green byte non-zero */
                if (green) green_count++;
                else       render_err++;
                printf("frame %d: green=%s pixel(0x%08x)\n",
                       i, green ? "yes" : "no ", p);
            } else {
                fprintf(stderr, "frame %d: child exit %d\n", i, code);
                render_err++;
            }
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "frame %d: child killed by signal %d\n",
                    i, WTERMSIG(status));
            killed_count++;
        }
        fflush(stdout);
    }

    printf("=== RESULT: %d/%d frames green, %d render-err, %d killed ===\n",
           green_count, n_frames, render_err, killed_count);

    munmap(g_shared_rgba, bytes);
    return (green_count == n_frames) ? 0 : 1;
}

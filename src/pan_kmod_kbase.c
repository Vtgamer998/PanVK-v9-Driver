/*
 * Mali-G68 MC4 (Valhall v9) pan_kmod kbase backend implementation
 * Connects Mesa pan_kmod interfaces directly to hardware /dev/mali0
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#include "pan_kmod_kbase.h"
#include "kbase_winsys.h"
#include "kbase_slot_unwedge.h"

/* Maximum consecutive fragment failures before permanent poison.
 * Each failure → dev_reopen() → new /dev/mali0 fd.  Too many rapid
 * reopens exhaust kernel kbase resources → OOM → reboot. */
#define PAN_KMOD_MAX_CONSECUTIVE_FAILURES 3

/* Persistent wedge marker — survives pan_kmod_dev destroy/create cycles.
 * Written when the GPU is permanently wedged (consecutive_failures limit hit).
 * Checked at create time so a new instance refuses to open the GPU and
 * does not start another reopen loop.  Cleared on explicit call or reboot. */
#define PAN_KMOD_WEDGE_FILE "/data/local/tmp/.panvk_gpu_wedged"

static int  pan_kmod_wedge_is_set(void) {
    struct stat st; return stat(PAN_KMOD_WEDGE_FILE, &st) == 0;
}
static void pan_kmod_wedge_set(void) {
    FILE *f = fopen(PAN_KMOD_WEDGE_FILE, "w");
    if (f) { fprintf(f, "GPU wedged\n"); fclose(f); }
}
static void pan_kmod_wedge_clear(void) { unlink(PAN_KMOD_WEDGE_FILE); }

struct pan_kmod_dev {
    struct kbase_dev *kdev;
    struct pan_kmod_dev_props props;
    /* Serialises every access to dev->kdev (including pan_kmod_dev_reopen
     * closing/freeing it) so a concurrent multi-threaded submit can never
     * use the old kdev/fd after reopen freed it (use-after-free / double
     * close that would otherwise crash or wedge the phone). */
    pthread_mutex_t dev_lock;
    /* Counts consecutive fragment timeouts/faults with no successful frame
     * in between.  When this reaches PAN_KMOD_MAX_CONSECUTIVE_FAILURES the
     * device is permanently poisoned: no more reopens are attempted and all
     * submits return -ENODEV immediately.
     *
     * Without this limit a permanently wedged GPU (e.g. after a crash loop)
     * causes infinite rapid reopen() calls that exhaust kernel kbase context
     * resources → kernel OOM → reboot. */
    int consecutive_failures;
};

struct pan_kmod_dev *pan_kmod_dev_create(const char *dev_node) {
    /* Check persistent wedge marker — set when GPU was permanently wedged
     * in a previous instance.  Refuse to open and start another reopen loop
     * that would exhaust kbase context slots → OOM → reboot.
     * The user must reboot to clear this state (reboot removes /data/local/tmp). */
    if (pan_kmod_wedge_is_set()) {
        fprintf(stderr,
            "pan_kmod: GPU permanently wedged (marker file exists: %s).\n"
            "Reboot the device to recover. Refusing to open /dev/mali0.\n",
            PAN_KMOD_WEDGE_FILE);
        return NULL;
    }

    struct kbase_dev *kdev = kbase_dev_open(dev_node);
    if (!kdev) return NULL;

    struct pan_kmod_dev *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        kbase_dev_close(kdev);
        return NULL;
    }

    dev->kdev = kdev;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&dev->dev_lock, &attr);
    pthread_mutexattr_destroy(&attr);

    /* Drain any stale events buffered from a previous context on this fd.
     * On MTK r49 the kernel re-queues unreceived events when a new context
     * opens /dev/mali0; consuming them now prevents them from being mistaken
     * for completions of the first atoms submitted by this context. */
    kbase_drain_events(kdev);

    /* Read real GPU ID from hardware.  kbase_dev_open already called
     * KBASE_IOCTL_GET_GPUPROPS; we just retrieve the cached value.
     * The 0x90001000 fallback corresponds to the G77 reference used by
     * the Mesa compiler; on this device the probed id is 0x92041010
     * (Mali-G68 MC4), still Valhall arch 9. */
    uint32_t detected_id = kbase_dev_gpu_id(kdev);
    dev->props.gpu_id = detected_id ? detected_id : 0x90001000;
    dev->props.gpu_revision = 0x0000;
    dev->props.core_count = 4; /* Mali-G68 MC4; could also be read from props blob */
    uint32_t arch = dev->props.gpu_id >> 28;
    printf("pan_kmod: gpu_id=0x%08x arch=%u (%s)\n",
           dev->props.gpu_id, arch,
           arch == 9  ? "Valhall v9"  :
           arch == 10 ? "Valhall v10" :
           arch == 7  ? "Bifrost v7"  :
           arch == 6  ? "Midgard"     : "unknown");
    snprintf(dev->props.ddk_version, sizeof(dev->props.ddk_version), "r49p1-03bet0");

    return dev;
}

void pan_kmod_dev_destroy(struct pan_kmod_dev *dev) {
    if (!dev) return;
    pthread_mutex_lock(&dev->dev_lock);
    if (dev->kdev) kbase_dev_close(dev->kdev);
    pthread_mutex_unlock(&dev->dev_lock);
    pthread_mutex_destroy(&dev->dev_lock);
    free(dev);
}

/* Close the underlying kbase context and reopen a fresh one, keeping the
 * pan_kmod_dev handle and its props.  This is the MTK r49 multi-frame
 * workaround: after a TERMINATED (0x4) fragment the kernel wedges the job
 * slot, so any further submit in the SAME kbase context read-faults (0x42).
 * A fresh /dev/mali0 open gives a clean scheduler context, and since the
 * mmap'd SAME_VA BOs persist across the close (verified experimentally on
 * MT6893), persistent swapchain / shader BOs keep working. */
int pan_kmod_dev_reopen(struct pan_kmod_dev *dev) {
    if (!dev) return -EINVAL;
    /* Hold the device lock for the whole swap so a concurrent submit cannot
     * be using the old kdev/fd while we free it (use-after-free).  Recursive
     * so internal callers (submit_fragment_timeout) can re-enter. */
    pthread_mutex_lock(&dev->dev_lock);
    uint32_t saved_gpu_id = kbase_dev_gpu_id(dev->kdev);
    if (dev->kdev) kbase_dev_close(dev->kdev);
    dev->kdev = kbase_dev_open(NULL);
    if (!dev->kdev) {
        pthread_mutex_unlock(&dev->dev_lock);
        return -ENODEV;
    }
    /* MTK r49 returns gpu_id=0 from GET_GPUPROPS on the second open within
     * the same process.  Preserve the value from the first open. */
    if (saved_gpu_id && !kbase_dev_gpu_id(dev->kdev)) {
        dev->props.gpu_id = saved_gpu_id;
    }
    /* MTK r49 delivers some stale events with up to ~50ms delay after the
     * new fd is opened.  Wait briefly before draining so these late-arriving
     * events are already in the fd's read buffer when we consume them.
     * Without this wait the drain misses them and they arrive during the next
     * submit, being consumed as fake completions for the new atoms. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 60 * 1000000L }; /* 60ms */
    nanosleep(&ts, NULL);

    /* Two-pass drain: the first pass consumes events already buffered; a short
     * pause then catches any stragglers that arrived between the open and the
     * first drain (MTK r49 event delivery is asynchronous and can span 50+ ms). */
    int total = kbase_drain_events(dev->kdev);
    ts.tv_nsec = 15 * 1000000L; /* 15ms */
    nanosleep(&ts, NULL);
    total += kbase_drain_events(dev->kdev);
    if (total > 0)
        fprintf(stderr, "pan_kmod_dev_reopen: drained %d stale events (incl. delayed)\n", total);

    /* Set atom_counter to 200 so the first atoms of the new context use ids
     * 201-254 — a safe zone that never collides with stale events from the old
     * context (which used ids 1-~10 per frame).  Without this, frame N+1's
     * atom=1 consumes the delayed stale event for frame N's atom=1, making the
     * tiler appear to complete instantly while still on the slot → next submit
     * hits JOB_READ_FAULT and the scheduler wedges. */
    kbase_dev_set_atom_counter(dev->kdev, 200);
    pthread_mutex_unlock(&dev->dev_lock);
    return 0;
}

int pan_kmod_dev_query_props(struct pan_kmod_dev *dev, struct pan_kmod_dev_props *props) {
    if (!dev || !props) return -EINVAL;
    *props = dev->props;
    return 0;
}

uint32_t pan_kmod_dev_query_props_gpu_id(struct pan_kmod_dev *dev) {
    return dev ? dev->props.gpu_id : 0;
}

void pan_kmod_dev_set_gpu_id(struct pan_kmod_dev *dev, uint32_t gpu_id) {
    if (dev) dev->props.gpu_id = gpu_id;
}

struct pan_kmod_bo_impl {
    struct pan_kmod_bo base;
    struct kbase_bo *kbo;
};

struct pan_kmod_bo *pan_kmod_bo_alloc(struct pan_kmod_dev *dev, size_t size, uint32_t flags) {
    if (!dev || !dev->kdev || size == 0) return NULL;

    uint32_t kflags = 0;
    if (flags & PAN_KMOD_BO_FLAG_READ)     kflags |= KBASE_BO_PROT_READ;
    if (flags & PAN_KMOD_BO_FLAG_WRITE)    kflags |= KBASE_BO_PROT_WRITE;
    if (flags & PAN_KMOD_BO_FLAG_EXEC)     kflags |= KBASE_BO_PROT_EXEC;
    if (flags & PAN_KMOD_BO_FLAG_COHERENT) kflags |= KBASE_BO_COHERENT;

    struct kbase_bo *kbo = kbase_bo_alloc(dev->kdev, size, kflags);
    if (!kbo) return NULL;

    struct pan_kmod_bo_impl *impl = calloc(1, sizeof(*impl));
    if (!impl) {
        kbase_bo_free(kbo);
        return NULL;
    }

    impl->kbo = kbo;
    impl->base.dev = dev;
    impl->base.cpu = kbo->cpu;
    impl->base.gpu = kbo->gpu;
    impl->base.size = kbo->size;
    impl->base.handle = kbo->handle;
    impl->base.flags = flags;

    return &impl->base;
}

void pan_kmod_bo_free(struct pan_kmod_bo *bo) {
    if (!bo) return;
    struct pan_kmod_bo_impl *impl = (struct pan_kmod_bo_impl *)bo;
    if (impl->kbo) {
        kbase_bo_free(impl->kbo);
        impl->kbo = NULL;
    }
    free(impl);
}

int pan_kmod_submit_atom(struct pan_kmod_dev *dev, uint64_t jc_gpu, uint32_t core_req,
                         uint32_t atom_id, uint32_t *event_code) {
    /* Never wait forever on a GPU atom: a hung job must fail fast so the
     * test harness can exit instead of tripping the Mali watchdog (which
     * reboots the phone).  Default 1500ms, override via PANVK_SUBMIT_TIMEOUT_MS. */
    return pan_kmod_submit_atom_timeout(dev, jc_gpu, core_req, atom_id, event_code,
                                        kbase_submit_timeout_ms(1500));
}

int pan_kmod_submit_atom_timeout(struct pan_kmod_dev *dev, uint64_t jc_gpu, uint32_t core_req,
                                 uint32_t atom_id, uint32_t *event_code, int timeout_ms) {
    if (!dev || !dev->kdev) return -EINVAL;

    int ret;
    uint32_t rx_atom = 0, rx_code = 0;
    /* Hold the device lock for the whole submit+wait: serialises against a
     * concurrent pan_kmod_dev_reopen() (which closes/frees dev->kdev) so we
     * never ioctl() or read() on a freed fd. */
    pthread_mutex_lock(&dev->dev_lock);

    uint8_t assigned_atom = 0;
    ret = kbase_submit_job(dev->kdev, jc_gpu, core_req, atom_id, 0, 0, &assigned_atom);
    if (ret >= 0) {
        /* Always go through the poll+read timeout path - never the blocking read.
         * A hung GPU would otherwise trip the MTK watchdog -> phone reboot.
         * Pass the rotating atom number assigned by submit so the wait skips
         * stale events from other atoms (MTK r49 delivers events out of order
         * / re-reads lost completions — consuming the wrong atom's event here
         * is what makes the caller advance while its job is still on the slot,
         * triggering JOB_READ_FAULT and wedging the scheduler). */
        if (timeout_ms <= 0) timeout_ms = kbase_submit_timeout_ms(1500);
        ret = kbase_wait_event_timeout(dev->kdev, &rx_atom, &rx_code, timeout_ms, assigned_atom);
    }

    if (event_code) *event_code = rx_code;
    if (ret == -EAGAIN) {
        fprintf(stderr, "pan_kmod: atom %u TIMED OUT (timeout=%dms) - GPU may be hung\n",
                atom_id, timeout_ms);
        /* Non-fragment atoms always signal completion, so a timeout here is a
         * real hang: poison the device (further submits fail fast, no block)
         * and persist the reboot-aware wedge marker. */
        kbase_dev_set_poisoned(dev->kdev, 1);
        kbase_wedge_mark();
        pthread_mutex_unlock(&dev->dev_lock);
        return -ETIMEDOUT; /* Never treat a hung GPU as success */
    }
    pthread_mutex_unlock(&dev->dev_lock);
    return (ret == 0 && rx_code == 0x1) ? 0 : -EIO;
}

int pan_kmod_submit_flush_timeout(struct pan_kmod_dev *dev, uint64_t jc_gpu,
                                  uint32_t atom_id, uint32_t *event_code, int timeout_ms) {
    if (!dev || !dev->kdev) return -EINVAL;
    /* The render already completed by the time the caller reaches here, so a
     * flush stall (e.g. 0x58 DATA_INVALID or kernel read-fault after a
     * TERMINATED fragment) must NOT poison the device or persist the reboot
     * marker -- the frame is already visible and the game loop must go on. */
    pthread_mutex_lock(&dev->dev_lock);
    uint8_t assigned_atom = 0;
    int ret = kbase_submit_job(dev->kdev, jc_gpu, KBASE_QUEUE_REQ_FLUSH, atom_id, 0, 0, &assigned_atom);
    if (ret >= 0) {
        uint32_t rx_atom = 0, rx_code = 0;
        ret = kbase_wait_event_timeout(dev->kdev, &rx_atom, &rx_code, timeout_ms, assigned_atom);
        if (event_code) *event_code = rx_code;
    }
    pthread_mutex_unlock(&dev->dev_lock);
    /* Treat everything (DONE, TERMINATED, timeout, lost) as success-from-here;
     * the caller decides based on pixels. */
    return 0;
}

int pan_kmod_submit_fragment_timeout(struct pan_kmod_dev *dev, uint64_t jc_gpu, uint32_t core_req,
                                     uint32_t atom_id, uint32_t *event_code, int timeout_ms) {
    if (!dev || !dev->kdev) return -EINVAL;

    /* Permanently wedged GPU: stop immediately without opening more fds.
     * Each reopen() consumes a kernel kbase context slot; exhausting them
     * causes kernel OOM → reboot.  The caller must destroy and recreate
     * the device (which implies a process restart) to recover. */
    if (dev->consecutive_failures >= PAN_KMOD_MAX_CONSECUTIVE_FAILURES) {
        fprintf(stderr,
            "pan_kmod: GPU permanently wedged (%d consecutive failures) — "
            "refusing submit to prevent reboot. Restart the process.\n",
            dev->consecutive_failures);
        return -ENODEV;
    }
    /* Fragment polygon-list jobs on MTK r49 can take >200ms on first run
     * (MMU page-table faults + shader warmup).  Default 1500ms like every
     * other atom; override via PANVK_SUBMIT_TIMEOUT_MS. */
    if (timeout_ms <= 0 || timeout_ms > 5000)
        timeout_ms = kbase_submit_timeout_ms(400);

    /* The unwedge/reopen paths below swap dev->kdev, so hold the recursive
     * device lock for the whole submit+wait+unwedge sequence to stop another
     * thread from using the old kdev while it is freed. */
    pthread_mutex_lock(&dev->dev_lock);

    uint8_t assigned_atom = 0;
    int ret = kbase_submit_job(dev->kdev, jc_gpu, core_req, atom_id, 0, 0, &assigned_atom);
    if (ret < 0) {
        pthread_mutex_unlock(&dev->dev_lock);
        return ret;
    }

    uint32_t rx_atom = 0, rx_code = 0;
    /* Always go through the poll+read timeout path - never the blocking read.
     * A hung GPU would otherwise trip the MTK watchdog -> phone reboot.
     * Pass the rotating atom number assigned by submit so the wait skips
     * stale events from other atoms — consuming the wrong atom's event is
     * what lets the caller advance while its job is still on the slot. */
    if (timeout_ms <= 0) timeout_ms = kbase_submit_timeout_ms(1500);
    ret = kbase_wait_event_timeout(dev->kdev, &rx_atom, &rx_code, timeout_ms, assigned_atom);

    if (event_code) *event_code = rx_code;
    if (ret == -EAGAIN) {
        fprintf(stderr, "pan_kmod: fragment atom %u TIMED OUT (timeout=%dms) - GPU may be hung\n",
                atom_id, timeout_ms);
        dev->consecutive_failures++;
        if (dev->consecutive_failures < PAN_KMOD_MAX_CONSECUTIVE_FAILURES) {
            pan_kmod_dev_reopen(dev);
        } else {
            fprintf(stderr,
                "pan_kmod: %d consecutive failures — GPU permanently wedged, "
                "writing wedge marker and refusing further submits.\n"
                "Reboot the device to recover.\n",
                dev->consecutive_failures);
            pan_kmod_wedge_set();
        }
        pthread_mutex_unlock(&dev->dev_lock);
        return -ETIMEDOUT;
    }
    /* MTK r49 completion semantics for polygon-list fragment jobs:
     * 0x1 = DONE, 0x4 = TERMINATED (kernel soft/hard-stop after render),
     * 0x4002 = CANCELLED (watchdog after render).  On this kernel the
     * fragment chain always renders before being stopped, so treat all
     * three as render-complete. */
    if (rx_code == 0x42) {
        /* JOB_READ_FAULT: slot already wedged before this submit.
         * Try the cheap unwedge first (one null renderpass-end flush atom);
         * only fall back to reopen() if that fails.  The render may still
         * have completed — return 0, caller verifies pixels.  The unwedge
         * atom must differ from the REAL fragment atom number (assigned_atom),
         * not the caller's ignored atom_id hint. */
        uint8_t uw_atom = (uint8_t)((assigned_atom % 254) + 1);
        if (uw_atom == assigned_atom)
            uw_atom = (uint8_t)((uw_atom % 254) + 1);
        if (kbase_slot_unwedge(dev->kdev, uw_atom, 200) != 0) {
            fprintf(stderr, "pan_kmod: fragment 0x42 JOB_READ_FAULT - auto-reopening kbase context\n");
            pan_kmod_dev_reopen(dev);
        }
        pthread_mutex_unlock(&dev->dev_lock);
        return 0;
    }

    int success = (ret == 0 && (rx_code == 0x1 || rx_code == 0x4 || rx_code == 0x4002));

    if (success && (rx_code == 0x4 || rx_code == 0x4002)) {
        /* TERMINATED / CANCELLED: fragment rendered but the MTK r49 kernel
         * leaves the fragment slot internally marked "in use".  Without an
         * unwedge, the next fragment submit would hit JOB_READ_FAULT (0x42)
         * or watchdog (0x4002) → Android ANR dialog.
         *
         * Workaround: submit a null renderpass-end flush atom (core_req=0x002,
         * jc=0, renderpass_id=0xFF).  This triggers the kernel's internal
         * kbase_job_slot_softstop() on the fragment slot, releasing the
         * "in use" mark in-place.  The null flush completes with 0x1 DONE
         * in <2ms.  After it returns the slot is clean and the next frame
         * can submit immediately without pan_kmod_dev_reopen().  The unwedge
         * atom must differ from the REAL fragment atom number (assigned_atom),
         * not the caller's ignored atom_id hint. */
        uint8_t unwedge_atom = (uint8_t)((assigned_atom % 254) + 1);
        if (unwedge_atom == assigned_atom)
            unwedge_atom = (uint8_t)((unwedge_atom % 254) + 1);

        int uw = kbase_slot_unwedge(dev->kdev, unwedge_atom, 200);
        if (uw != 0) {
            fprintf(stderr,
                "pan_kmod: slot unwedge failed (%d), falling back to dev_reopen\n", uw);
            pan_kmod_dev_reopen(dev);
            /* After dev_reopen the GPU hardware slot may still be wedged
             * (the MTK r49 GPU shares slots across kbase contexts).  Do NOT
             * poison here — the render may have actually completed
             * (TERMINATED means the kernel stopped the job AFTER execution).
             * The caller (v9_cmd_buffer_submit) verifies pixels and only
             * sets the permanent wedge marker when the render truly failed
             * (rendered=false).  Poisoning here would block all subsequent
             * submits even when the frame rendered successfully. */
        }
    }

    pthread_mutex_unlock(&dev->dev_lock);

    if (success) {
        dev->consecutive_failures = 0;
        pan_kmod_wedge_clear(); /* GPU recovered — remove stale marker if any */
        return 0;
    } else {
        dev->consecutive_failures++;
        if (dev->consecutive_failures >= PAN_KMOD_MAX_CONSECUTIVE_FAILURES) {
            fprintf(stderr,
                "pan_kmod: fragment failure %d/%d — GPU permanently wedged, "
                "writing marker. Reboot to recover.\n",
                dev->consecutive_failures, PAN_KMOD_MAX_CONSECUTIVE_FAILURES);
            pan_kmod_wedge_set();
        } else {
            fprintf(stderr,
                "pan_kmod: fragment failure %d/%d — will retry next frame\n",
                dev->consecutive_failures, PAN_KMOD_MAX_CONSECUTIVE_FAILURES);
        }
        return -EIO;
    }
}

int pan_kmod_submit_batch(struct pan_kmod_dev *dev, void *atoms, uint32_t nr_atoms) {
    if (!dev || !dev->kdev) return -EINVAL;
    pthread_mutex_lock(&dev->dev_lock);
    int ret = kbase_submit_batch(dev->kdev, atoms, nr_atoms);
    pthread_mutex_unlock(&dev->dev_lock);
    return ret;
}

int pan_kmod_wait_event(struct pan_kmod_dev *dev, uint32_t *atom_nr, uint32_t *event_code) {
    if (!dev || !dev->kdev) return -EINVAL;
    /* kbase_wait_event now internally uses poll+read with a 5s cap, so this is
     * safe (never blocks forever -> never trips the MTK watchdog). */
    pthread_mutex_lock(&dev->dev_lock);
    int ret = kbase_wait_event(dev->kdev, atom_nr, event_code);
    pthread_mutex_unlock(&dev->dev_lock);
    return ret;
}

int pan_kmod_wait_event_timeout(struct pan_kmod_dev *dev, uint32_t *atom_nr,
                                uint32_t *event_code, int timeout_ms, uint8_t expect_atom) {
    if (!dev || !dev->kdev) return -EINVAL;
    pthread_mutex_lock(&dev->dev_lock);
    int ret = kbase_wait_event_timeout(dev->kdev, atom_nr, event_code, timeout_ms, expect_atom);
    pthread_mutex_unlock(&dev->dev_lock);
    return ret;
}

int pan_kmod_dev_fd(struct pan_kmod_dev *dev) {
    if (!dev || !dev->kdev) return -EINVAL;
    return kbase_dev_fd(dev->kdev);
}

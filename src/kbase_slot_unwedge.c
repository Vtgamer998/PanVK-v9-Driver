/*
 * kbase_slot_unwedge.c — MTK r49 fragment slot unwedge workaround
 *
 * See kbase_slot_unwedge.h for full design rationale.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "kbase_slot_unwedge.h"
#include "kbase_winsys.h"

/*
 * The null flush atom layout.
 *
 * Key fields that trigger the MTK r49 renderpass-end cleanup path:
 *
 *   core_req      = 0x002 (KBASE_QUEUE_REQ_FLUSH)
 *     Routes to the CS slot (not the fragment slot), so it never
 *     competes with the wedged fragment slot.
 *
 *   jc            = 0x0
 *     Null job chain pointer.  The CS slot sees an empty chain and
 *     completes immediately with DONE (0x1).
 *
 *   renderpass_id = 0xFF
 *     The MTK r49 kbase treats 0xFF as "end of renderpass sentinel".
 *     This triggers kbasep_js_atom_done() → kbase_job_slot_softstop()
 *     on the fragment slot, releasing its "in use" flag without
 *     touching any BO or resubmitting the fragment.
 *
 *   pre_dep[0]    = { atom_nr - 1, ORDER }
 *     Ordering dependency on the fragment atom ensures the kernel
 *     does not issue the null flush until the fragment atom has
 *     been fully retired (even if its event was silently dropped).
 *
 * Everything else is zero / default.
 */

/* Internal: build and submit the null flush atom directly via ioctl.
 * We do NOT go through kbase_submit_job() because we need to set
 * renderpass_id = 0xFF, which kbase_submit_job() does not expose. */
static int submit_null_flush(struct kbase_dev *dev,
                             uint8_t atom_nr,
                             uint8_t dep_atom_nr) {
    struct kbase_atom_mtk atom;
    memset(&atom, 0, sizeof(atom));

    atom.jc           = 0x0;          /* null job chain          */
    atom.atom_number  = atom_nr;
    atom.core_req     = 0x002u;       /* KBASE_QUEUE_REQ_FLUSH   */
    atom.renderpass_id = 0xFF;        /* end-of-renderpass marker */
    atom.prio         = 0;
    atom.jobslot      = 0;

    /* Order dependency on the fragment atom */
    if (dep_atom_nr) {
        atom.pre_dep[0].atom_id  = dep_atom_nr;
        atom.pre_dep[0].dep_type = 0x0; /* BASE_JD_DEP_TYPE_ORDER */
    }

    struct kbase_ioctl_job_submit submit = {
        .addr     = (uint64_t)(uintptr_t)&atom,
        .nr_atoms = 1,
        .stride   = sizeof(atom),
    };

    int fd = kbase_dev_fd(dev);
    if (fd < 0) return -EBADF;

    if (ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0) {
        perror("kbase_slot_unwedge: KBASE_IOCTL_JOB_SUBMIT");
        return -errno;
    }

    return 0;
}

int kbase_slot_unwedge(struct kbase_dev *dev, uint8_t atom_nr, int timeout_ms) {
    if (!dev) return -EINVAL;
    if (timeout_ms <= 0) timeout_ms = 200; /* null flush completes in <2ms normally */

    /* Hold the submit lock so the rotating atom number assigned below cannot
     * collide with a concurrent submit's rotating number (MTK r49 mis-routes
     * events / read-faults 0x42 when two live atoms share a number).  The
     * caller-supplied atom_nr is only a hint; the unique rotating number is
     * what the kernel actually sees. */
    kbase_submit_lock(dev);
    uint8_t nr = kbase_dev_next_atom_nr(dev);

    /* dep_atom_nr = nr - 1, wrapping correctly within 1-255 */
    uint8_t dep = (nr > 1) ? (nr - 1) : 255;

    fprintf(stderr,
        "kbase_slot_unwedge: submitting null renderpass-end flush "
        "(atom=%u dep=%u timeout=%dms)\n",
        nr, dep, timeout_ms);

    int ret = submit_null_flush(dev, nr, dep);
    kbase_submit_unlock(dev);
    if (ret != 0) {
        fprintf(stderr, "kbase_slot_unwedge: submit failed (%d)\n", ret);
        return ret;
    }

    /* Wait for the null flush to complete.  Match on the atom number we
     * actually submitted (nr) so a stale event from another atom is not
     * mistaken for the null flush completion. */
    uint32_t rx_atom = 0, rx_code = 0;
    ret = kbase_wait_event_timeout(dev, &rx_atom, &rx_code, timeout_ms, nr);

    if (ret == -EAGAIN) {
        fprintf(stderr,
            "kbase_slot_unwedge: null flush TIMED OUT — "
            "slot still wedged, fall back to dev_reopen()\n");
        return -ETIMEDOUT;
    }

    if (ret != 0) {
        fprintf(stderr, "kbase_slot_unwedge: wait error %d\n", ret);
        return ret;
    }

    /* 0x1 DONE = clean.  0x4 TERMINATED = also acceptable on some kernels.
     * 0x40 SOFT_STOPPED = the kernel soft-stopped the null flush itself;
     * this is actually the EXPECTED outcome since the null flush's purpose
     * is to trigger kbase_job_slot_softstop() on the fragment slot.  Accept
     * it as success — the slot is released after soft-stop. */
    if (rx_code == 0x1 || rx_code == 0x4 || rx_code == 0x40) {
        fprintf(stderr,
            "kbase_slot_unwedge: OK — fragment slot released "
            "(null flush code=0x%x atom=%u)\n",
            rx_code, rx_atom);
        return 0;
    }

    fprintf(stderr,
        "kbase_slot_unwedge: unexpected event code=0x%x atom=%u — "
        "slot may still be wedged\n",
        rx_code, rx_atom);
    return -EIO;
}

/*
 * kbase_slot_unwedge.h — MTK r49 fragment slot unwedge workaround
 *
 * WHY THIS EXISTS
 * ---------------
 * After a fragment atom completes with 0x4 (TERMINATED), the MTK r49 kernel
 * leaves the fragment slot internally marked as "in use".  Any subsequent
 * fragment submit hits JOB_READ_FAULT (0x42) or a watchdog timeout (0x4002),
 * causing the Android ANR dialog.
 *
 * THE WORKAROUND — "Null Renderpass Flush"
 * -----------------------------------------
 * The MTK r49 kbase has a special path for atoms with:
 *   core_req   = KBASE_QUEUE_REQ_FLUSH (0x002)   — CS slot, not frag slot
 *   jc         = 0x0                              — null job chain
 *   renderpass_id = 0xFF                          — "end of renderpass" sentinel
 *
 * When this atom is submitted after a TERMINATED fragment, the kernel's
 * renderpass tracking logic runs its end-of-pass cleanup, which internally
 * calls kbase_job_slot_softstop() on the fragment slot.  This releases the
 * "in use" mark WITHOUT closing the context or resubmitting the fragment.
 *
 * The null flush atom always completes with 0x1 (DONE) within 1-2ms.
 * After it completes, the fragment slot is clean and accepts new submits.
 *
 * This is the same mechanism the Arm blob uses internally between renderpasses
 * in a frame — we are just triggering it manually after every fragment.
 *
 * SEQUENCE PER FRAME
 * ------------------
 *   1. Submit Tiler atom        (core_req=0x04E)  → wait 0x1
 *   2. Submit Pre-Flush atom    (core_req=0x002)  → wait 0x1
 *   3. Submit Fragment atom     (core_req=0x041)  → wait 0x4 (TERMINATED)
 *   4. Submit Null-Flush atom   (core_req=0x002, jc=0, renderpass_id=0xFF)
 *                                                 → wait 0x1 (slot released)
 *   ── next frame starts with a clean slot ──
 *
 * USAGE
 * -----
 *   #include "kbase_slot_unwedge.h"
 *
 *   // After every fragment submit, call:
 *   int ret = kbase_slot_unwedge(dev, next_atom_id);
 *   // ret == 0: slot is clean, safe to submit next frame
 *   // ret != 0: slot still wedged — fall back to pan_kmod_dev_reopen()
 */

#ifndef KBASE_SLOT_UNWEDGE_H
#define KBASE_SLOT_UNWEDGE_H

#include <stdint.h>
#include "kbase_winsys.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Submit a null renderpass-end flush atom to release the fragment slot.
 *
 * @dev        open kbase device
 * @atom_nr    atom number to use (must not collide with live atoms)
 * @timeout_ms max wait in ms (0 = default 500ms — this atom is always fast)
 *
 * Returns 0 on success (slot clean).
 * Returns -ETIMEDOUT if the null flush itself timed out (very unlikely).
 * Returns -EIO on unexpected event code.
 */
int kbase_slot_unwedge(struct kbase_dev *dev, uint8_t atom_nr, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* KBASE_SLOT_UNWEDGE_H */

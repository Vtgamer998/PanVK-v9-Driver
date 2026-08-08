/*
 * Mali-G68 MC4 (Valhall v9) kbase Winsys Driver
 * Direct userspace winsys implementation over /dev/mali0
 */

#ifndef KBASE_WINSYS_H
#define KBASE_WINSYS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory Protection Flags */
#define KBASE_BO_PROT_READ     (1u << 0)
#define KBASE_BO_PROT_WRITE    (1u << 1)
#define KBASE_BO_PROT_EXEC     (1u << 2)
#define KBASE_BO_COHERENT      (1u << 3)

/* Atom Queue Core Requirements */
/* MTK r49 core_req bits (empirically mapped): FS=0x01 (frag slot),
 * PROTECTED_MODE_SWITCH=0x02, T=0x04 (tiler slot), CS=0x08 (compute/vertex
 * slot), COHERENT_GROUP=0x40.  Using 0x001 (FS bit) on a compute job routed
 * it to the fragment slot -> 0x58 DATA_INVALID (wrong slot). */
#define KBASE_QUEUE_REQ_FRAGMENT (0x041u) /* BASE_JD_REQ_FS | BASE_JD_REQ_COHERENT_GROUP */
#define KBASE_QUEUE_REQ_TILER    (0x04Eu) /* PROTECTED | TILER | CS | COHERENT */
#define KBASE_QUEUE_REQ_FLUSH    (0x002u) /* CS compute slot for cache flush atoms */
/* 0x048/0x04A (CS|C / P|CS|C) reached the CS slot but the atom was
 * TERMINATED (0x4) kernel-side before HW execution.  Chrome's real captured
 * compute atom on this same MTK r49 kernel uses core_req = 0x4E
 * (P|T|CS|C) -- identical to our working TILER combo. */
#define KBASE_QUEUE_REQ_COMPUTE  (0x04Eu) /* PROTECTED | TILER | CS | COHERENT */

/* Atom pre-dependency types (BASE_JD_DEP_TYPE_*) */
#define KBASE_JD_DEP_TYPE_ORDER 0x0
#define KBASE_JD_DEP_TYPE_DATA  0x1

struct kbase_dev;

struct kbase_bo {
    struct kbase_dev *dev;
    void *cpu;
    void *gpu_map;
    uint64_t gpu;
    size_t size;
    size_t gpu_map_size;
    uint32_t handle;
    uint32_t flags;
};

#define BASE_MEM_IMPORT_TYPE_UMM 2
#define BASE_MEM_NEED_MMAP       (1ull << 14)

/* Atom pre-dependency: id of the predecessor atom in the same batch. */
struct kbase_atom_dep {
    uint8_t atom_id;
    uint8_t dep_type;
} __attribute__((packed));

/* kbase atom as expected by the MTK r49 kernel (base_atom layout). */
struct kbase_atom_mtk {
    uint64_t seq_nr;
    uint64_t jc;
    uint64_t udata[2];
    uint64_t extres_list;
    uint16_t nr_extres;
    uint8_t  jit_id[2];
    struct kbase_atom_dep pre_dep[2];
    uint8_t  atom_number;
    uint8_t  prio;
    uint8_t  device_nr;
    uint8_t  jobslot;
    uint32_t core_req;
    uint8_t  renderpass_id;
    uint8_t  padding[7];
    uint32_t frame_nr;
    uint32_t pad2;
} __attribute__((packed));

/* kbase JOB_SUBMIT ioctl payload (mirrors kbase_ioctl_job_submit in the
 * kernel).  Exposed here so external helpers (e.g. kbase_slot_unwedge) can
 * submit atoms directly via ioctl -- the kbase_submit_job() wrapper hides
 * renderpass_id, which is required for the null renderpass-end flush. */
struct kbase_ioctl_job_submit {
    uint64_t addr;
    uint32_t nr_atoms;
    uint32_t stride;
};

#define KBASE_IOCTL_JOB_SUBMIT     _IOC(_IOC_WRITE, 0x80, 2, 16)

union kbase_ioctl_mem_import {
    struct {
        uint64_t flags;
        uint64_t phandle;
        uint32_t type;
        uint32_t padding;
    } in;
    struct {
        uint64_t flags;
        uint64_t gpu_va;
        uint64_t va_pages;
    } out;
};
#define KBASE_IOCTL_MEM_IMPORT _IOC(_IOC_READ|_IOC_WRITE, 0x80, 22, sizeof(union kbase_ioctl_mem_import))

struct kbase_dev *kbase_dev_open(const char *dev_node);
void kbase_dev_close(struct kbase_dev *dev);
int kbase_dev_fd(struct kbase_dev *dev);
/* Returns the GPU product ID read from hardware via KBASE_IOCTL_GET_GPUPROPS.
 * bits 28-31 = arch (9=Valhall v9, 10=Valhall v10, 7=Bifrost, 6=Midgard).
 * Returns 0 if the ioctl failed (caller must fall back to a hardcoded value). */
uint32_t kbase_dev_gpu_id(struct kbase_dev *dev);
void     kbase_dev_set_atom_counter(struct kbase_dev *dev, uint8_t value);
/* Returns the next safe rotating atom number (1-255).  Callers that submit
 * one atom at a time must use this instead of a fixed 0/1/2 so the kernel
 * never sees two live atoms with the same number (MTK r49 mis-routes events
 * / read-faults when atom ids collide across frames). */
uint8_t kbase_dev_next_atom_nr(struct kbase_dev *dev);

/* Serialise a raw JOB_SUBMIT ioctl against kbase_submit_job()/kbase_submit_batch()
 * (both of which assign rotating atom numbers under the same lock).  Direct-
 * ioctl helpers (e.g. kbase_slot_unwedge) MUST hold these around the ioctl so
 * their atom number cannot collide with a concurrent submit's rotating number. */
void kbase_submit_lock(struct kbase_dev *dev);
void kbase_submit_unlock(struct kbase_dev *dev);

/* GPU-wedge safety (MTK r49): a fragment atom that never delivers DONE leaves
 * the kernel scheduler wedged (JOB_READ_FAULT); any LATER /dev/mali0 submit
 * can then block forever and freeze the phone.  The driver reacts to a
 * timeout by poisoning its own device (further submits fail fast instead of
 * blocking) and writing a reboot-aware marker so a fresh process refuses to
 * touch the GPU until the phone is rebooted (which is the only recovery).
 * Returns 1 if the GPU is currently considered wedged. */
void kbase_wedge_mark(void);
int kbase_wedge_check(void);
void kbase_dev_set_poisoned(struct kbase_dev *dev, int poisoned);
int kbase_dev_poisoned(struct kbase_dev *dev);

/* SAFE-TEST MODE helpers (see kbase_winsys.c). */
int kbase_dry_run(void);
int kbase_submit_timeout_ms(int fallback);

struct kbase_bo *kbase_bo_alloc(struct kbase_dev *dev, size_t size, uint32_t flags);
struct kbase_bo *kbase_bo_import_dma_buf(struct kbase_dev *dev, int dma_buf_fd, size_t size);
void kbase_bo_free(struct kbase_bo *bo);

int kbase_submit_job(struct kbase_dev *dev, uint64_t jc, uint32_t core_req, uint32_t atom_nr,
                      uint8_t jobslot, uint32_t frame_nr, uint8_t *nr_out);
int kbase_submit_batch(struct kbase_dev *dev, void *atoms, uint32_t nr_atoms);
int kbase_wait_event(struct kbase_dev *dev, uint32_t *atom_nr, uint32_t *event_code);
int kbase_wait_event_timeout(struct kbase_dev *dev, uint32_t *atom_nr,
                             uint32_t *event_code, int timeout_ms, uint8_t expect_atom);
/* Drain all buffered stale events from the fd without blocking.
 * Must be called after kbase_dev_open() / pan_kmod_dev_reopen() before any
 * new submit to prevent stale events from the previous context being consumed
 * as completions for new atoms (MTK r49 event re-queue quirk). */
int kbase_drain_events(struct kbase_dev *dev);

#ifdef __cplusplus
}
#endif

#endif /* KBASE_WINSYS_H */

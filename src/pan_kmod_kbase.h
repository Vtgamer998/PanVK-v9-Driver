/*
 * Mali-G68 MC4 (Valhall v9) pan_kmod kbase backend
 * Mesa-compatible kmod abstraction over /dev/mali0
 */

#ifndef PAN_KMOD_KBASE_H
#define PAN_KMOD_KBASE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory Flags */
#define PAN_KMOD_BO_FLAG_READ         (1u << 0)
#define PAN_KMOD_BO_FLAG_WRITE        (1u << 1)
#define PAN_KMOD_BO_FLAG_EXEC         (1u << 2)
#define PAN_KMOD_BO_FLAG_COHERENT     (1u << 3)

struct pan_kmod_dev;

struct pan_kmod_bo {
    struct pan_kmod_dev *dev;
    void *cpu;
    uint64_t gpu;
    size_t size;
    uint32_t handle;
    uint32_t flags;
};

struct pan_kmod_dev_props {
    uint32_t gpu_id;
    uint32_t gpu_revision;
    uint32_t core_count;
    char ddk_version[64];
};

/* Panfrost kmod API for kbase */
struct pan_kmod_dev *pan_kmod_dev_create(const char *dev_node);
void pan_kmod_dev_destroy(struct pan_kmod_dev *dev);

/* Reopen the /dev/mali0 fd / kbase context in place (MTK r49 multi-frame
 * workaround - clears the wedged job slot after a TERMINATED fragment,
 * preserving the pan_kmod_dev handle, props, and all SAME_VA BOs). */
int pan_kmod_dev_reopen(struct pan_kmod_dev *dev);

int pan_kmod_dev_query_props(struct pan_kmod_dev *dev, struct pan_kmod_dev_props *props);
uint32_t pan_kmod_dev_query_props_gpu_id(struct pan_kmod_dev *dev);
void pan_kmod_dev_set_gpu_id(struct pan_kmod_dev *dev, uint32_t gpu_id);

struct pan_kmod_bo *pan_kmod_bo_alloc(struct pan_kmod_dev *dev, size_t size, uint32_t flags);
void pan_kmod_bo_free(struct pan_kmod_bo *bo);

int pan_kmod_submit_atom(struct pan_kmod_dev *dev, uint64_t jc_gpu, uint32_t core_req,
                         uint32_t atom_id, uint32_t *event_code);

/* Best-effort flush submit: never poisons/wedges the device on timeout (a
 * real hang after an already-rendered frame would otherwise make the whole
 * device unusable).  Used for the post-fragment L2 drain. */
int pan_kmod_submit_flush_timeout(struct pan_kmod_dev *dev, uint64_t jc_gpu,
                                  uint32_t atom_id, uint32_t *event_code, int timeout_ms);

int pan_kmod_submit_atom_timeout(struct pan_kmod_dev *dev, uint64_t jc_gpu, uint32_t core_req,
                                 uint32_t atom_id, uint32_t *event_code, int timeout_ms);
int pan_kmod_submit_fragment_timeout(struct pan_kmod_dev *dev, uint64_t jc_gpu, uint32_t core_req,
                                     uint32_t atom_id, uint32_t *event_code, int timeout_ms);
int pan_kmod_submit_batch(struct pan_kmod_dev *dev, void *atoms, uint32_t nr_atoms);
int pan_kmod_wait_event(struct pan_kmod_dev *dev, uint32_t *atom_nr, uint32_t *event_code);
int pan_kmod_wait_event_timeout(struct pan_kmod_dev *dev, uint32_t *atom_nr,
                                uint32_t *event_code, int timeout_ms, uint8_t expect_atom);
int pan_kmod_dev_fd(struct pan_kmod_dev *dev);

#ifdef __cplusplus
}
#endif

#endif /* PAN_KMOD_KBASE_H */

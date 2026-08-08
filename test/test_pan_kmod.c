/*
 * Test harness for Step 1: pan_kmod kbase backend for Mali-G68
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pan_kmod_kbase.h"
#include "kbase_winsys.h"

int main(int argc, char **argv) {
    printf("=== Testing Step 1: Mesa pan_kmod kbase Backend ===\n");

    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) {
        fprintf(stderr, "FAIL: pan_kmod_dev_create returned NULL\n");
        return 1;
    }
    printf("SUCCESS: pan_kmod_dev created\n");

    struct pan_kmod_dev_props props;
    if (pan_kmod_dev_query_props(dev, &props) == 0) {
        printf("SUCCESS: Device Props: GPU ID=0x%08x Rev=0x%04x Cores=%u DDK=%s\n",
               props.gpu_id, props.gpu_revision, props.core_count, props.ddk_version);
    } else {
        fprintf(stderr, "FAIL: pan_kmod_dev_query_props failed\n");
        pan_kmod_dev_destroy(dev);
        return 1;
    }

    struct pan_kmod_bo *bo = pan_kmod_bo_alloc(dev, 65536, PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
    if (!bo) {
        fprintf(stderr, "FAIL: pan_kmod_bo_alloc failed\n");
        pan_kmod_dev_destroy(dev);
        return 1;
    }
    printf("SUCCESS: pan_kmod_bo_alloc allocated 64KB BO (CPU %p, GPU 0x%llx)\n",
           bo->cpu, (unsigned long long)bo->gpu);

    /* Submit test cache flush atom */
    uint32_t *fl = (uint32_t *)bo->cpu;
    memset(fl, 0, 64);
    fl[4] = (3u << 1); /* Type = Cache Flush (3) */
    fl[8] = 0xFFFFFFFFu; fl[9] = 0xFFFFFFFFu;

    uint32_t event_code = 0;
    int ret = pan_kmod_submit_atom(dev, bo->gpu, KBASE_QUEUE_REQ_FLUSH, 1, &event_code);
    if (ret == 0 && event_code == 0x1) {
        printf("SUCCESS: pan_kmod_submit_atom cache flush succeeded with event_code=0x1\n");
    } else {
        fprintf(stderr, "FAIL: pan_kmod_submit_atom failed (ret=%d, event_code=0x%x)\n", ret, event_code);
        pan_kmod_bo_free(bo);
        pan_kmod_dev_destroy(dev);
        return 1;
    }

    pan_kmod_bo_free(bo);
    pan_kmod_dev_destroy(dev);

    printf("=== Step 1: pan_kmod kbase Backend Test PASSED CLEANLY! ===\n");
    return 0;
}

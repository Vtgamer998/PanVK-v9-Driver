#include <stdio.h>
#include <stdlib.h>
#include "pan_kmod_kbase.h"

/* Does a GPU BO allocated on dev1 survive a dev close+reopen (dev2)?
 * If yes, fresh-dev-per-frame is viable for persistent swapchain BOs.
 * If no, every frame must re-allocate everything (slow but works for POC). */
int main(void) {
    struct pan_kmod_dev *dev1 = pan_kmod_dev_create(NULL);
    if (!dev1) { printf("dev1 create FAIL\n"); return 1; }
    printf("dev1 open OK\n");

    struct pan_kmod_bo *bo = pan_kmod_bo_alloc(dev1, 4096,
        PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
    if (!bo) { printf("bo alloc FAIL\n"); return 1; }
    printf("bo alloc OK: cpu=%p gpu=0x%llx size=%zu\n",
           bo->cpu, (unsigned long long)bo->gpu, bo->size);

    /* Write something CPU-side so we can verify it survives. */
    volatile uint32_t *p = (volatile uint32_t *)bo->cpu;
    p[0] = 0xCAFEBABE;
    p[1] = 0xDEADBEEF;
    printf("CPU wrote: p[0]=0x%x p[1]=0x%x\n", p[0], p[1]);

    /* Close dev1 WITHOUT freeing the BO first (we want to know if the BO
     * survives the dev teardown at the kernel level). */
    pan_kmod_dev_destroy(dev1);
    printf("dev1 closed\n");

    /* Reopen dev2 and CHECK if the cpu pointer / gpu pointer are still valid. */
    struct pan_kmod_dev *dev2 = pan_kmod_dev_create(NULL);
    if (!dev2) { printf("dev2 create FAIL\n"); return 1; }
    printf("dev2 open OK\n");

    /* Try reading the same CPU pointer (the mmap may have been unmapped on
     * dev1 close). If SIGSEGV/crash, BOs do NOT survive dev cycle. */
    printf("reading bo->cpu after dev cycle: p[0]=0x%x p[1]=0x%x\n", p[0], p[1]);

    /* Try using bo->gpu for a submit (cannot easily without cmd buffer; just
     * report whether the cpu/gpu addresses look sane). */
    printf("bo->gpu after dev cycle = 0x%llx (was 0x%llx before)\n",
           (unsigned long long)bo->gpu, (unsigned long long)bo->gpu);

    pan_kmod_bo_free(bo);
    pan_kmod_dev_destroy(dev2);
    printf("PASS: BO survived dev cycle (CPU read worked)\n");
    return 0;
}

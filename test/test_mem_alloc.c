#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>

#include "kbase_winsys.h"

int main(void) {
    struct kbase_dev *dev = kbase_dev_open(NULL);
    if (!dev) { fprintf(stderr, "FAIL: dev_open\n"); return 1; }
    printf("dev opened\n");

    size_t page = sysconf(_SC_PAGESIZE);
    size_t size = page * 2; /* 2 pages */

    struct kbase_bo *bo = kbase_bo_alloc(dev, size, KBASE_BO_PROT_READ | KBASE_BO_PROT_WRITE);
    if (!bo) { fprintf(stderr, "FAIL: bo_alloc\n"); return 1; }

    printf("size=%zu aligned=%zu\n", size, bo->size);
    printf("cpu=0x%llx gpu=0x%llx  (cpu==gpu: %s)\n",
           (unsigned long long)(uintptr_t)bo->cpu,
           (unsigned long long)bo->gpu,
           bo->cpu == (void *)(uintptr_t)bo->gpu ? "YES" : "NO");

    memset(bo->cpu, 0xAB, bo->size);
    printf("wrote 0xAB to cpu; read back: 0x%02x\n", ((uint8_t *)bo->cpu)[0]);

    kbase_bo_free(bo);
    kbase_dev_close(dev);
    printf("PASS\n");
    return 0;
}

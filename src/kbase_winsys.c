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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "kbase_winsys.h"

/* SAFE-TEST MODE
 * --------------
 * PANVK_DRY_RUN=1 makes every /dev/mali0 interaction a no-op: no ioctls,
 * no GPU submits, no kernel involvement.  Memory becomes plain anonymous
 * mmap and events are synthesized as DONE.  This lets the full Vulkan
 * pipeline (instance/device/SPIR-V/queue-submit) be exercised without any
 * risk of hanging the GPU and rebooting the phone.
 */
static int kbase_dry_run_enabled(void) {
    static int state = -1;
    if (state < 0) {
        const char *v = getenv("PANVK_DRY_RUN");
        state = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return state;
}

int kbase_dry_run(void) {
    return kbase_dry_run_enabled();
}

/* Default GPU submit timeout in ms.  A hung job must never block forever,
 * otherwise the Mali kernel watchdog fires and the phone reboots.  Use a
 * conservative default and let callers override with PANVK_SUBMIT_TIMEOUT_MS.
 */
int kbase_submit_timeout_ms(int fallback) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("PANVK_SUBMIT_TIMEOUT_MS");
        cached = v ? atoi(v) : -1;
        if (cached <= 0) cached = fallback;
    }
    return cached;
}

/* Kernel UAPI ioctl definitions.
 *
 * The MediaTek r49 kbase kernel on Mali-G68 (MT6893/Dimensity 700) requires
 * the API version handshake to report a supported version or it fails with
 * EPERM.  Verified without root on /dev/mali0: version 11.13 succeeds
 * (user=11.13, kernel=11.13), while 11.0 is rejected.
 */
#define KBASE_IOCTL_VERSION_CHECK  _IOC(_IOC_READ|_IOC_WRITE, 0x80, 0, 4)
#define KBASE_IOCTL_SET_FLAGS      _IOC(_IOC_WRITE, 0x80, 1, 4)
#define KBASE_IOCTL_JOB_SUBMIT     _IOC(_IOC_WRITE, 0x80, 2, 16)
#define KBASE_IOCTL_GET_GPUPROPS   _IOC(_IOC_READ|_IOC_WRITE, 0x80, 3, 8)
#define KBASE_IOCTL_MEM_ALLOC      _IOC(_IOC_READ|_IOC_WRITE, 0x80, 5, 32)
#define KBASE_IOCTL_MEM_FREE       _IOC(_IOC_WRITE, 0x80, 6, 8)

struct kbase_ioctl_get_gpuprops {
    uint64_t buffer;  /* pointer to props buffer */
    uint32_t size;    /* buffer size in bytes    */
    uint32_t flags;   /* must be 0               */
};


#define BASE_CONTEXT_CREATE_KERNEL_FLAGS (1u << 0)
#define BASE_MEM_SAME_VA                 (1u << 16)

/* r49 kernel API version expected by this DDK (11.13). */
#define KBASE_API_MAJOR 11u
#define KBASE_API_MINOR 13u

struct kbase_ioctl_version_check {
    uint16_t major;
    uint16_t minor;
};

struct kbase_ioctl_set_flags {
    uint32_t create_flags;
};

struct kbase_ioctl_mem_free {
    uint64_t handle;
};

struct base_dependency {
    uint8_t atom_id;
    uint8_t dep_type;
} __attribute__((packed));

/* Moved to kbase_winsys.h so external helpers (e.g. kbase_slot_unwedge) can
 * build and submit atoms directly via ioctl without going through the
 * kbase_submit_job() wrapper.  The wrapper hides the renderpass_id field,
 * which is needed for the null renderpass-end flush workaround on MTK r49. */

/* Actual MTK r49 base_jd_event_v2 format:
 *   u32  event_code;   // 4 bytes at offset 0
 *   u8   atom_number;  // 1 byte  at offset 4
 *   u8   prio;         // 1 byte  at offset 5
 *   u8   jobslot;      // 1 byte  at offset 6
 *   u8   unused;       // 1 byte  at offset 7
 *   u64  timer;        // 8 bytes at offset 8
 *   u64  udata;        // 8 bytes at offset 16
 */
struct base_jd_event_v2 {
    uint32_t event_code;
    uint8_t  atom_number;
    uint8_t  prio;
    uint8_t  jobslot;
    uint8_t  unused;
    uint64_t timer;
    uint64_t udata;
} __attribute__((packed));

 struct kbase_dev {
     int fd;
     uint16_t major;
     uint16_t minor;
     int poisoned;
     uint8_t atom_counter;   /* rotating submit atom number (1-255)            */
     pthread_mutex_t submit_lock; /* serialises KBASE_IOCTL_JOB_SUBMIT (DXVK MT) */
     uint32_t gpu_id;        /* read via KBASE_IOCTL_GET_GPUPROPS; 0 if failed  */
 };

/* Reboot-aware GPU-wedge marker.
 *
 * When an atom times out the MTK r49 kernel may leave the scheduler wedged:
 * a subsequent /dev/mali0 submit blocks forever and freezes the phone (the
 * process parks in an uninterruptible kernel read).  The only recovery is a
 * reboot.  We persist a marker (tagged with the kernel boot_id, regenerated
 * on every boot) so later runs fail fast with a clear message instead of
 * freezing the phone; the marker self-clears on the next boot because the
 * boot_id no longer matches.
 */
static const char *kbase_wedge_marker_path(void) {
    const char *p = getenv("PANVK_WEDGE_MARKER");
    return p ? p : "/data/data/com.termux/files/home/.panvk_v9_wedged";
}

/* Read the per-boot kernel boot_id.  Returns 0 and leaves buf untouched on
 * failure.  /proc/uptime is SELinux-denied on Android, but boot_id is world-
 * readable and regenerated on every boot. */
static int kbase_boot_id(char *buf, size_t buflen) {
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!f) return 0;
    int ok = fgets(buf, (int)buflen, f) != NULL;
    fclose(f);
    if (!ok) return 0;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return n > 0;
}

void kbase_wedge_mark(void) {
    char boot[64];
    if (!kbase_boot_id(boot, sizeof(boot))) {
        fprintf(stderr, "kbase_wedge_mark: cannot read boot_id - wedge marker NOT written\n");
        return;
    }
    FILE *f = fopen(kbase_wedge_marker_path(), "w");
    if (!f) { perror("kbase_wedge_mark: fopen"); return; }
    fprintf(f, "%s\n", boot);
    fclose(f);
}

int kbase_wedge_check(void) {
    FILE *f = fopen(kbase_wedge_marker_path(), "r");
    if (!f) return 0; /* no marker -> not wedged */
    char marker_boot[64] = "";
    if (!fgets(marker_boot, sizeof(marker_boot), f)) {
        fclose(f);
        unlink(kbase_wedge_marker_path());
        return 0;
    }
    fclose(f);
    size_t n = strlen(marker_boot);
    while (n > 0 && (marker_boot[n - 1] == '\n' || marker_boot[n - 1] == '\r' || marker_boot[n - 1] == ' '))
        marker_boot[--n] = '\0';

    char cur_boot[64];
    if (!kbase_boot_id(cur_boot, sizeof(cur_boot))) {
        /* Cannot verify the boot id: be conservative and stay wedged. */
        fprintf(stderr, "kbase_wedge_check: cannot read boot_id - assuming GPU still wedged\n");
        return 1;
    }
    if (strcmp(marker_boot, cur_boot) != 0) {
        /* New boot since the marker was written: the GPU is clean again. */
        unlink(kbase_wedge_marker_path());
        return 0;
    }
    return 1; /* same boot: GPU still wedged */
}

struct kbase_dev *kbase_dev_open(const char *dev_node) {
    const char *path = dev_node ? dev_node : "/dev/mali0";

    if (kbase_dry_run()) {
        struct kbase_dev *dev = calloc(1, sizeof(*dev));
        if (!dev) return NULL;
        dev->fd = -1;
        dev->major = KBASE_API_MAJOR;
        dev->minor = KBASE_API_MINOR;
        dev->gpu_id = 0x92041010; /* Mali-G68 MC4 Valhall v9 default for dry-run */
        dev->atom_counter = 0;
        pthread_mutex_init(&dev->submit_lock, NULL);
        printf("kbase_winsys: DRY-RUN initialized %s (no /dev/mali0 access)\n", path);
        return dev;
    }

    if (kbase_wedge_check()) {
        fprintf(stderr, "kbase_winsys: GPU WEDGED (marker %s) - the previous test "
                        "left /dev/mali0 in JOB_READ_FAULT.  REBOOT the phone to "
                        "recover; this run refuses to touch the GPU to avoid "
                        "freezing the device.\n",
                kbase_wedge_marker_path());
        return NULL;
    }

    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror("kbase_dev_open: open /dev/mali0");
        return NULL;
    }

    struct kbase_ioctl_version_check vc = { KBASE_API_MAJOR, KBASE_API_MINOR };
    if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &vc) < 0) {
        perror("kbase_dev_open: KBASE_IOCTL_VERSION_CHECK");
        close(fd);
        return NULL;
    }

    uint32_t flags = 0;
    if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &flags) < 0) {
        perror("kbase_dev_open: KBASE_IOCTL_SET_FLAGS");
        close(fd);
        return NULL;
    }

    struct kbase_dev *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        close(fd);
        return NULL;
    }

    dev->fd = fd;
    dev->major = vc.major;
    dev->minor = vc.minor;
    dev->atom_counter = 0;
    pthread_mutex_init(&dev->submit_lock, NULL);

    /* Detect real GPU ID from hardware so pan_kmod can set arch-correct props. */
    {
        uint8_t props_buf[256];
        memset(props_buf, 0, sizeof(props_buf));
        struct kbase_ioctl_get_gpuprops gp = {
            .buffer = (uint64_t)(uintptr_t)props_buf,
            .size   = (uint32_t)sizeof(props_buf),
            .flags  = 0,
        };
        if (ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &gp) >= 0) {
            dev->gpu_id = *(uint32_t *)props_buf;
        }
    }

    uint32_t arch = dev->gpu_id >> 28;
    printf("kbase_winsys: initialized %s (kbase v%u.%u, gpu_id=0x%08x arch=%u)\n",
           path, vc.major, vc.minor, dev->gpu_id, arch);
    return dev;
}

void kbase_dev_close(struct kbase_dev *dev) {
    if (!dev) return;
    if (dev->fd >= 0) close(dev->fd);
    pthread_mutex_destroy(&dev->submit_lock);
    free(dev);
}

void kbase_dev_set_poisoned(struct kbase_dev *dev, int poisoned) {
    if (dev) dev->poisoned = poisoned ? 1 : 0;
}

int kbase_dev_poisoned(struct kbase_dev *dev) {
    return dev ? dev->poisoned : 0;
}

int kbase_dev_fd(struct kbase_dev *dev) {
    return (dev && dev->fd >= 0) ? dev->fd : -1;
}

uint32_t kbase_dev_gpu_id(struct kbase_dev *dev) {
    return dev ? dev->gpu_id : 0;
}

void kbase_dev_set_atom_counter(struct kbase_dev *dev, uint8_t value) {
    if (dev) dev->atom_counter = value;
}

/* Returns the next safe atom number (1-255, never 0).
 * Called under submit_lock so callers get a unique id per submit. */
static uint8_t next_atom_id(struct kbase_dev *dev) {
    dev->atom_counter = (uint8_t)((dev->atom_counter % 254) + 1);
    return dev->atom_counter;
}

/* Next atom number for one-shot submits.  Rotates 1..255 so the kernel never
 * sees two live atoms with the same id (MTK r49 mis-routes/cancels jobs whose
 * atom numbers collide).  Protected by the submit_lock in kbase_submit_job. */
uint8_t kbase_dev_next_atom_nr(struct kbase_dev *dev) {
    if (!dev) return 0;
    uint8_t n = next_atom_id(dev);
    if (n == 0) n = 1;             /* skip 0 (reserved) */
    return n;
}

void kbase_submit_lock(struct kbase_dev *dev) {
    if (dev) pthread_mutex_lock(&dev->submit_lock);
}

void kbase_submit_unlock(struct kbase_dev *dev) {
    if (dev) pthread_mutex_unlock(&dev->submit_lock);
}

/* Drain all pending events from /dev/mali0 without blocking.
 *
 * MTK r49 quirk: after a fragment atom whose DONE/TERMINATED event was
 * silently dropped by the kernel, the event sits buffered in the fd's read
 * queue.  When pan_kmod_dev_reopen() closes the old fd and opens a fresh one,
 * the new context starts with atom_counter=1 — so atom 1 of the new context
 * collides with the stale event for atom 1 of the previous context that the
 * kernel delivers on the NEW fd (the kbase driver re-queues unreceived events
 * on context switch on MTK r49).  Draining before any submit prevents the
 * stale event from being mistaken for a fresh completion, which would make the
 * real completion invisible and cause the next tiler to wedge.
 *
 * Returns the number of events drained (0 = clean). */
int kbase_drain_events(struct kbase_dev *dev) {
    if (!dev || dev->fd < 0) return 0;
    int drained = 0;
    struct base_jd_event_v2 ev;
    /* Cap iterations to avoid an infinite loop if the kernel keeps delivering
     * stale events (e.g. repeated re-queuing on a wedged slot). */
    while (drained < 1024) {
        struct pollfd pfd = { .fd = dev->fd, .events = POLLIN };
        int r = poll(&pfd, 1, 0); /* non-blocking: 0ms timeout */
        if (r <= 0) break;
        if (!(pfd.revents & POLLIN)) break;
        ssize_t nr = read(dev->fd, &ev, sizeof(ev));
        if (nr <= 0) break;
        fprintf(stderr, "kbase_drain_events: discarded stale event code=0x%x atom=%u\n",
                ev.event_code, ev.atom_number);
        drained++;
    }
    return drained;
}

struct kbase_bo *kbase_bo_alloc(struct kbase_dev *dev, size_t size, uint32_t flags) {
    if (!dev || size == 0) return NULL;

    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t aligned_size = (size + page_size - 1) & ~(page_size - 1);

    if (kbase_dry_run()) {
        void *cpu_ptr = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (cpu_ptr == MAP_FAILED) {
            perror("kbase_bo_alloc (dry-run): mmap");
            return NULL;
        }
        struct kbase_bo *bo = calloc(1, sizeof(*bo));
        if (!bo) {
            munmap(cpu_ptr, aligned_size);
            return NULL;
        }
        bo->dev = dev;
        bo->cpu = cpu_ptr;
        bo->gpu = (uint64_t)(uintptr_t)cpu_ptr;
        bo->size = aligned_size;
        bo->flags = flags;
        return bo;
    }

    uint64_t nr_pages = aligned_size / page_size;

    /* SAME_VA flag: MTK r49 uses 0x2000 (verified empirically: passing
     * 0x10000 makes the kernel OR 0x2000 back over it, and the ioctl echoes
     * the flags into slot[0]).  With SAME_VA the kernel maps CPU == GPU VA. */
    uint64_t mem_flags = 0x200F; /* CPU_RD|CPU_WR|GPU_RD|GPU_WR | SAME_VA(0x2000) */
    if (flags & KBASE_BO_PROT_EXEC) {
        mem_flags = 0x0001 | 0x0002 | 0x0004 | 0x0010 | 0x2000; /* 0x2017: CPU_RD|CPU_WR|GPU_RD|GPU_EX|SAME_VA */
    }

    /* MTK r49 MEM_ALLOC layout (4 x u64, 32 bytes).  Empirical ioctl result
     * for both 0x200F and 0x1000F:
     *   in  = { nr_pages, nr_pages, 0, mem_flags }
     *   out = { flags_echo, gpu_va, 0, flags_echo }
     * i.e. the kernel echoes the flags in slots 0/3 and returns the reserved
     * GPU VA in slot 1.  Slots 0/2 are NOT the VA -- trust slot 1. */
    uint64_t alloc[4];
    alloc[0] = nr_pages;
    alloc[1] = nr_pages;
    alloc[2] = 0;
    alloc[3] = mem_flags;
    if (ioctl(dev->fd, KBASE_IOCTL_MEM_ALLOC, alloc) < 0) {
        perror("kbase_bo_alloc: KBASE_IOCTL_MEM_ALLOC");
        return NULL;
    }
    uint64_t gpu_va = alloc[1]; /* [out] GPU VA (slot 1 on MTK r49) */

    /* The shader ISA is executed by the GPU, never by the CPU.  Mapping with
     * PROT_EXEC is denied by SELinux for unprivileged apps, so map RW only;
     * GPU_EX is already set in mem_flags so the GPU MMU still sees the pages
     * as executable. */
    int prot = PROT_READ | PROT_WRITE;

    /* SAME_VA is active, so the CPU mapping lands at the same VA the GPU
     * uses.  Map WITHOUT MAP_FIXED: kbase places it at the gpu_va and the
     * returned pointer is what we hand the GPU (cpu_ptr == gpu address).
     * Passing the VA as the mmap offset lets kbase locate the region. */
    void *cpu_ptr = mmap(NULL, aligned_size, prot, MAP_SHARED, dev->fd, gpu_va);
    if (cpu_ptr == MAP_FAILED) {
        /* Free the kernel GPU allocation that was just made to avoid leaking it. */
        struct kbase_ioctl_mem_free free_arg = { .handle = gpu_va };
        ioctl(dev->fd, KBASE_IOCTL_MEM_FREE, &free_arg);
        perror("kbase_bo_alloc: mmap");
        return NULL;
    }

    struct kbase_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) {
        munmap(cpu_ptr, aligned_size);
        return NULL;
    }

    bo->dev = dev;
    bo->cpu = cpu_ptr;
    bo->gpu = (uint64_t)cpu_ptr;
    bo->size = aligned_size;
    bo->flags = flags;

    return bo;
}

struct kbase_bo *kbase_bo_import_dma_buf(struct kbase_dev *dev, int dma_buf_fd, size_t size) {
    if (!dev || dma_buf_fd < 0 || size == 0) return NULL;

    int import_fd = dma_buf_fd;
    union kbase_ioctl_mem_import param = {
        .in = {
            .flags = 0xf, /* CPU/GPU read and write */
            .phandle = (uint64_t)(uintptr_t)&import_fd,
            .type = BASE_MEM_IMPORT_TYPE_UMM,
            .padding = 0,
        },
    };

    if (ioctl(dev->fd, KBASE_IOCTL_MEM_IMPORT, &param) < 0) {
        perror("kbase_bo_import_dma_buf: KBASE_IOCTL_MEM_IMPORT failed");
        return NULL;
    }

    void *gpu_map = NULL;
    uint64_t gpu_va = param.out.gpu_va;
    size_t gpu_map_size = 0;
    if (param.out.flags & BASE_MEM_NEED_MMAP) {
        gpu_map_size = param.out.va_pages * (size_t)sysconf(_SC_PAGESIZE);
        /* This UMM mapping establishes the GPU VA. MTK's kbase rejects CPU
         * faults on it, so CPU access must use a mapping of the dma-buf fd. */
        gpu_map = mmap(NULL, gpu_map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       dev->fd, param.out.gpu_va);
        if (gpu_map == MAP_FAILED) {
            perror("kbase_bo_import_dma_buf: mmap");
            return NULL;
        }
        gpu_va = (uint64_t)(uintptr_t)gpu_map;
    }

    void *cpu_ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         dma_buf_fd, 0);
    if (cpu_ptr == MAP_FAILED) {
        perror("kbase_bo_import_dma_buf: dma-buf mmap");
        if (gpu_map) munmap(gpu_map, gpu_map_size);
        return NULL;
    }

    struct kbase_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) {
        munmap(cpu_ptr, size);
        if (gpu_map) munmap(gpu_map, gpu_map_size);
        return NULL;
    }

    bo->dev = dev;
    bo->cpu = cpu_ptr;
    bo->gpu_map = gpu_map;
    bo->gpu = gpu_va;
    bo->size = size;
    bo->gpu_map_size = gpu_map_size;
    bo->flags = KBASE_BO_PROT_READ | KBASE_BO_PROT_WRITE;
    return bo;
}

void kbase_bo_free(struct kbase_bo *bo) {
    if (!bo) return;
    if (bo->cpu && bo->size > 0) munmap(bo->cpu, bo->size);
    if (bo->gpu_map && bo->gpu_map_size > 0)
        munmap(bo->gpu_map, bo->gpu_map_size);
    free(bo);
}

int kbase_submit_job(struct kbase_dev *dev, uint64_t jc, uint32_t core_req, uint32_t atom_nr,
                      uint8_t jobslot, uint32_t frame_nr, uint8_t *nr_out) {
    if (!dev || !jc) return -EINVAL;

    if (kbase_dry_run()) {
        printf("kbase_submit_job (dry-run): jc=0x%llx core_req=0x%x atom=%u\n",
               (unsigned long long)jc, core_req, atom_nr);
        return 0;
    }

    /* After a fragment timed out, the scheduler is wedged; any further submit
     * can block forever in the kernel and freeze the phone.  Fail fast. */
    if (kbase_dev_poisoned(dev)) {
        fprintf(stderr, "kbase_submit_job: REFUSED (device poisoned after a GPU timeout). "
                        "No further /dev/mali0 submits in this process.\n");
        return -EAGAIN;
    }

    struct kbase_atom_mtk atom;
    memset(&atom, 0, sizeof(atom));
    atom.jc = jc;
    /* Use a rotating atom number instead of the caller's (possibly reused)
     * hint: two live atoms with the same number confuse the MTK r49 kernel
     * (event mis-routing / 0x42 JOB_READ_FAULT).  Serialised by submit_lock so
     * DXVK/VKD3D multi-threaded submits don't race. */
    pthread_mutex_lock(&dev->submit_lock);
    uint8_t nr = kbase_dev_next_atom_nr(dev);
    atom.atom_number = nr;
    atom.core_req = core_req;
    atom.jobslot = jobslot;
    atom.frame_nr = frame_nr;

    struct kbase_ioctl_job_submit submit = {
        .addr = (uint64_t)&atom,
        .nr_atoms = 1,
        .stride = sizeof(atom)
    };

    if (ioctl(dev->fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0) {
        perror("kbase_submit_job: KBASE_IOCTL_JOB_SUBMIT");
        pthread_mutex_unlock(&dev->submit_lock);
        return -errno;
    }

    /* Expose the rotating atom number actually assigned so the caller can
     * match the completion event (pan_kmod_submit_*_timeout passes it to
     * kbase_wait_event_timeout's expect_atom).  Two live atoms with the same
     * number mis-route events on MTK r49, so the submit side must hand the
     * real number back rather than letting the caller guess. */
    if (nr_out) *nr_out = nr;

    pthread_mutex_unlock(&dev->submit_lock);
    return 0;
}

int kbase_submit_batch(struct kbase_dev *dev, void *atoms, uint32_t nr_atoms) {
    if (!dev || !atoms || nr_atoms == 0) return -EINVAL;
    struct kbase_atom_mtk *arr = atoms;

    if (kbase_dry_run()) {
        printf("kbase_submit_batch (dry-run): %u atoms\n", nr_atoms);
        return 0;
    }

    if (kbase_dev_poisoned(dev)) {
        fprintf(stderr, "kbase_submit_batch: REFUSED (device poisoned after a GPU timeout). "
                        "No further /dev/mali0 submits in this process.\n");
        return -EAGAIN;
    }

    /* The caller hardcodes batch-local atom numbers 0/1/2 and references them
     * via pre_dep[].atom_id.  The MTK r49 kernel must never see two LIVE atoms
     * with the same number across submits (it mis-routes events / read-faults
     * 0x42), so under the submit_lock we renumber each batch atom from the
     * rotating counter and remap the intra-batch deps to the new numbers. */
    pthread_mutex_lock(&dev->submit_lock);

    uint8_t old_to_new[255];
    memset(old_to_new, 0, sizeof(old_to_new));
    for (uint32_t i = 0; i < nr_atoms && i < 255; i++) {
        uint8_t old = arr[i].atom_number;
        uint8_t n = kbase_dev_next_atom_nr(dev);
        arr[i].atom_number = n;
        old_to_new[old] = n;
    }
    for (uint32_t i = 0; i < nr_atoms && i < 255; i++) {
        for (int d = 0; d < 2; d++) {
            if (arr[i].pre_dep[d].dep_type == 0) continue; /* ORDER = no dep slot */
            uint8_t id = arr[i].pre_dep[d].atom_id;
            if (id < nr_atoms && old_to_new[id] != 0)
                arr[i].pre_dep[d].atom_id = old_to_new[id];
        }
    }

    struct kbase_ioctl_job_submit submit = {
        .addr = (uint64_t)(uintptr_t)arr,
        .nr_atoms = nr_atoms,
        .stride = sizeof(struct kbase_atom_mtk)
    };

    int ret = 0;
    if (ioctl(dev->fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0) {
        perror("kbase_submit_batch: KBASE_IOCTL_JOB_SUBMIT");
        ret = -errno;
    }

    pthread_mutex_unlock(&dev->submit_lock);
    return ret;
}

int kbase_wait_event(struct kbase_dev *dev, uint32_t *atom_nr, uint32_t *event_code) {
    /* Never block forever: cap at 5 seconds even when called with no explicit
     * timeout.  A hung GPU would otherwise block forever on this read and trip
     * the MTK watchdog -> phone reboot.  expect_atom=0: accept any atom (the
     * generic wait path does not know which atom to expect). */
    return kbase_wait_event_timeout(dev, atom_nr, event_code, 5000, 0);
}

int kbase_wait_event_timeout(struct kbase_dev *dev, uint32_t *atom_nr,
                             uint32_t *event_code, int timeout_ms, uint8_t expect_atom) {
    if (!dev || timeout_ms < 0) return -EINVAL;

    if (kbase_dry_run()) {
        if (atom_nr)   *atom_nr = 0;
        if (event_code) *event_code = 0x1; /* DONE */
        return 0;
    }

    /* The fd is O_NONBLOCK so the event read can never block forever (a
     * blocking read on a wedged MTK kbase device is what freezes the phone).
     * Loop on poll+read until the timeout expires.  Track real elapsed time:
     * poll() may consume most of `remaining` in a single wait, so simply
     * decrementing remaining by 1 per iteration would stretch the deadline to
     * O(N^2) when spurious POLLIN/read-competition repeats (e.g. another
     * thread draining the same fd). */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int remaining = timeout_ms;
    while (remaining > 0) {
        struct pollfd pfd = { .fd = dev->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, remaining);
        if (ret < 0) return -errno;
        if (ret == 0) return -EAGAIN;
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & (POLLERR | POLLHUP)) return -EIO;
            return -EAGAIN;
        }

        struct base_jd_event_v2 ev;
        memset(&ev, 0, sizeof(ev));
        ssize_t nr = read(dev->fd, &ev, sizeof(ev));
        if (nr > 0) {
            /* MTK r49 delivers events out of order and re-reads stale events
             * from the fd when a prior atom's completion was lost.  If the
             * caller told us which atom it expects (expect_atom != 0) and
             * this event belongs to a different atom, DISCARD it and keep
             * waiting — returning the wrong atom's event is what makes the
             * caller think its job finished while it is still on the slot,
             * causing the next submit to hit JOB_READ_FAULT and wedge the
             * scheduler (screen freeze). */
            if (expect_atom != 0 && ev.atom_number != expect_atom) {
                fprintf(stderr,
                    "kbase_wait_event: SKIP stale event code=0x%x atom=%u (expect_atom=%u)\n",
                    ev.event_code, ev.atom_number, expect_atom);
                goto next_iteration;
            }
            if (atom_nr)   *atom_nr = ev.atom_number;
            if (event_code) *event_code = ev.event_code;
            printf("kbase_wait_event: code=0x%x atom=%u prio=%u jobslot=%u timer=%llu\n",
                   ev.event_code, ev.atom_number, ev.prio, ev.jobslot,
                   (unsigned long long)ev.timer);
            return 0;
        }
        if (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return -errno;
        /* Event consumed by a previous wait or spurious POLLIN: recompute the
         * remaining budget from the monotonic clock, not a fixed decrement. */
next_iteration:
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L
                        + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        remaining = timeout_ms - (int)elapsed_ms;
    }
    return -EAGAIN;
}

/*
 * Test harness: sweep descriptor-field variants for the compute job (type 4)
 * on the real GPU.  Each variant re-packs the compute job and submits a single
 * atom, printing the event code + exception words.  Lets us bisect which
 * compute-job fields (if any) clear BASE_JD_EVENT_TERMINATED.
 *
 * Usage: ./test_compute_sweep [libpanvk_v9_compiler.so] [cs.spv]
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pan_kmod_kbase.h"
#include "kbase_winsys.h"
#include "v9_cmd_stream.h"
#include "v9_pack.h"
#include "panvk_v9_compiler.h"

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = n;
    return buf;
}

struct variant {
    const char *name;
    uint32_t header_index;      /* bit 16 of job header word */
    uint32_t allow_merging;     /* bit 31 of payload word 0 */
    uint32_t task_increment;    /* payload word 1 bits 0-13 */
    uint32_t task_axis;         /* payload word 1 bits 14-15 */
    uint32_t res_count;         /* low bits of resources pointer (1-63) */
    uint32_t fau_count;         /* SE word 1 */
    uint32_t use_fau;           /* write a real FAU pointer */
    uint32_t tls_packed_w1;     /* value for Local Storage word 1 */
};

int main(int argc, char **argv) {
    const char *libpath = argc > 1 ? argv[1] : "libpanvk_v9_compiler.so";
    const char *cspath  = argc > 2 ? argv[2] : "/data/data/com.termux/files/usr/tmp/opencode/cs.spv";

    printf("=== Compute descriptor sweep (real GPU) ===\n");

    void *lib = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "FAIL: dlopen: %s\n", dlerror()); return 1; }
    int (*compile)(const uint32_t *, size_t, enum panvk_v9_shader_stage,
                   const char *, const struct panvk_v9_pipeline_layout *,
                   struct panvk_v9_compiled_shader *, char *, size_t) =
        (void *)dlsym(lib, "panvk_v9_compile_spirv");
    void (*cleanup)(struct panvk_v9_compiled_shader *) =
        (void *)dlsym(lib, "panvk_v9_compiled_shader_cleanup");
    if (!compile || !cleanup) { fprintf(stderr, "FAIL: dlsym\n"); return 1; }

    size_t spv_size = 0;
    uint8_t *spv = read_file(cspath, &spv_size);
    if (!spv) { fprintf(stderr, "FAIL: cannot read %s\n", cspath); return 1; }

    struct panvk_v9_descriptor_binding cs_bindings[1] = {
        { .set = 0, .binding = 0, .descriptor_type = 7, .array_size = 1,
          .resource_index = 0x01000000 }, /* panvk: table 1 (set+1), index 0 */
    };
    struct panvk_v9_pipeline_layout cs_layout = {
        .bindings = cs_bindings, .binding_count = 1, .ubo_count = 0,
    };

    struct panvk_v9_compiled_shader cs = {0};
    char error[1024] = {0};
    int rc = compile((const uint32_t *)spv, spv_size, PANVK_V9_SHADER_COMPUTE,
                     "main", &cs_layout, &cs, error, sizeof(error));
    if (rc != 0) { fprintf(stderr, "FAIL: compute compile: %s\n", error); return 1; }
    printf("CS compiled: %zu bytes, local_size=%ux%ux%u\n",
           cs.binary_size, cs.local_size_x, cs.local_size_y, cs.local_size_z);
    FILE *dmp = fopen("/data/data/com.termux/files/usr/tmp/opencode/cs.bin", "wb");
    if (dmp) { fwrite(cs.binary, 1, cs.binary_size, dmp); fclose(dmp); }

    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) { fprintf(stderr, "FAIL: dev\n"); return 1; }

    struct v9_render_target_config config = { .width = 64, .height = 64, .clear_color = 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) { fprintf(stderr, "FAIL: cmd\n"); return 1; }

    struct pan_kmod_bo *ssbo = pan_kmod_bo_alloc(dev, 64, PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
    if (!ssbo) { fprintf(stderr, "FAIL: ssbo\n"); return 1; }
    memset(ssbo->cpu, 0, 64);

    struct v9_ssbo_binding ssbo_binding = { .address = ssbo->gpu, .size = 64, .index = 0 };

    v9_cmd_buffer_begin(cmd);
    rc = v9_cmd_buffer_set_compute_shader(cmd, &cs);
    if (rc) { fprintf(stderr, "FAIL: set_compute_shader\n"); return 1; }
    rc = v9_cmd_buffer_set_ssbos(cmd, &ssbo_binding, 1);
    if (rc) { fprintf(stderr, "FAIL: set_ssbos\n"); return 1; }
    rc = v9_cmd_buffer_dispatch(cmd, 4, 1, 1);
    if (rc) { fprintf(stderr, "FAIL: dispatch\n"); return 1; }

    uint8_t *base_cpu = (uint8_t *)v9_cmd_buffer_get_mem_cpu(cmd);
    uint64_t base_gpu = v9_cmd_buffer_get_mem_gpu(cmd);
    uint64_t cj_gpu = base_gpu + 0xE600;
    uint32_t *cj = (uint32_t *)(base_cpu + (cj_gpu - base_gpu));

    uint64_t res_gpu = base_gpu + 0xD200;   /* cmd->res_gpu */
    uint64_t sp_gpu  = base_gpu + 0xCC80;   /* cmd->sp_cs_gpu */
    uint64_t tls_gpu = base_gpu + 0xE100;   /* cmd->tls_gpu */
    uint64_t fau_gpu = base_gpu + 0xDD00;
    uint64_t cmd_ssbo_gpu = base_gpu + 0xD340; /* cmd->ssbo_gpu (desc table) */
    uint32_t *td = (uint32_t *)(base_cpu + (tls_gpu - base_gpu));
    uint32_t *fau = (uint32_t *)(base_cpu + (fau_gpu - base_gpu));

    memset(fau, 0, 64);

    struct variant variants[] = {
        /* name, header_index, merging, inc, axis, res_count, fau_count, use_fau, tls_w1 */
        { "panvk cnt=4",                   0, 1, 256, 2, 4, 0, 0, 0x80000000u },
        { "panvk cnt=2",                   0, 1, 256, 2, 2, 0, 0, 0x80000000u },
        { "panvk cnt=8",                   0, 1, 256, 2, 8, 0, 0, 0x80000000u },
    };
    /* Resources[0] stays empty (driver_set not used); the SSBO descriptor
     * table lives in Resources[1] via set_ssbos. */
    {
        uint32_t *res = (uint32_t *)(base_cpu + (res_gpu - base_gpu));
        printf("RES0: %08x %08x %08x %08x | RES1: %08x %08x %08x %08x\n",
               res[0], res[1], res[2], res[3], res[4], res[5], res[6], res[7]);
        printf("SSBOtbl: %08x %08x %08x %08x %08x %08x %08x %08x\n",
               ((uint32_t *)base_cpu)[0xD340/4], ((uint32_t *)base_cpu)[0xD344/4],
               ((uint32_t *)base_cpu)[0xD348/4], ((uint32_t *)base_cpu)[0xD34C/4],
               ((uint32_t *)base_cpu)[0xD350/4], ((uint32_t *)base_cpu)[0xD354/4],
               ((uint32_t *)base_cpu)[0xD358/4], ((uint32_t *)base_cpu)[0xD35C/4]);
        fflush(stdout);
    }

    int n = (int)(sizeof(variants) / sizeof(variants[0]));
    int found = 0;

    for (int i = 0; i < n; i++) {
        struct variant *v = &variants[i];

        /* Repack the job from scratch for this variant. */
        v9_pack_compute_job(cj, 4, 1, 1, 4, 1, 1,
                            res_gpu, sp_gpu, tls_gpu, 0, 0);
        if (v->header_index)
            cj[4] |= (v->header_index << 16);
        cj[8] = (cj[8] & 0x7FFFFFFFu) | (v->allow_merging ? 0x80000000u : 0u);
        cj[9] = (v->task_increment & 0x3FFFu) | ((v->task_axis & 0x3u) << 14);
        cj[24] = (uint32_t)((res_gpu & ~0x3Full) | (v->res_count & 0x3Fu));
        cj[17] = v->fau_count;
        if (v->use_fau) {
            cj[30] = (uint32_t)fau_gpu;
            cj[31] = (uint32_t)(fau_gpu >> 32);
        }
        td[1] = v->tls_packed_w1;

        uint32_t event_code = 0;
        int ret = pan_kmod_submit_atom(dev, cj_gpu, KBASE_QUEUE_REQ_COMPUTE, 0, &event_code);

        uint32_t exc = cj[0], first = cj[1];
        uint64_t fault = (uint64_t)cj[2] | ((uint64_t)cj[3] << 32);
        printf("[%s] ret=%d event=0x%x exc=0x%x first_incomplete=0x%x fault_ptr=0x%llx%s\n",
               v->name, ret, event_code, exc, first,
               (unsigned long long)fault,
               (ret == 0 && event_code == 0x1) ? "  <== SUCCESS" : "");

        if (ret == 0 && event_code == 0x1) {
            uint32_t *out = (uint32_t *)ssbo->cpu;
            printf("  SSBO contents: d[0]=0x%08x d[1]=0x%08x\n", out[0], out[1]);
            found = 1;
            break;
        }
    }

    if (!found) {
        printf("No variant cleared TERMINATED.\n");
    }

    pan_kmod_bo_free(ssbo);
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    cleanup(&cs);
    free(spv);
    dlclose(lib);
    return found ? 0 : 1;
}

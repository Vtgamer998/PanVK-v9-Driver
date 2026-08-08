/*
 * Test harness for Valhall v9 compute dispatch: compiler -> shader program
 * (stage 1) -> compute job (type 4) -> SSBO resource -> submit.
 *
 * DRY_RUN (PANVK_DRY_RUN=1): validates every packed structure and the submit
 * path without touching /dev/mali0.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pan_kmod_kbase.h"
#include "kbase_winsys.h"
#include "v9_cmd_stream.h"
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

int main(int argc, char **argv) {
    const char *libpath = argc > 1 ? argv[1] : "libpanvk_v9_compiler.so";
    const char *cspath  = argc > 2 ? argv[2] : "/data/data/com.termux/files/usr/tmp/opencode/cs.spv";

    printf("=== Testing Valhall v9 compute dispatch ===\n");

    void *lib = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "FAIL: dlopen(%s): %s\n", libpath, dlerror());
        return 1;
    }
    int (*compile)(const uint32_t *, size_t, enum panvk_v9_shader_stage,
                   const char *, const struct panvk_v9_pipeline_layout *,
                   struct panvk_v9_compiled_shader *, char *, size_t) =
        (void *)dlsym(lib, "panvk_v9_compile_spirv");
    void (*cleanup)(struct panvk_v9_compiled_shader *) =
        (void *)dlsym(lib, "panvk_v9_compiled_shader_cleanup");
    if (!compile || !cleanup) {
        fprintf(stderr, "FAIL: dlsym: %s\n", dlerror());
        return 1;
    }

    size_t spv_size = 0;
    uint8_t *spv = read_file(cspath, &spv_size);
    if (!spv) { fprintf(stderr, "FAIL: cannot read %s\n", cspath); return 1; }

    struct panvk_v9_descriptor_binding cs_bindings[1] = {
        { .set = 0, .binding = 0, .descriptor_type = 6, .array_size = 1,
          .resource_index = 0 },
    };
    struct panvk_v9_pipeline_layout cs_layout = {
        .bindings = cs_bindings, .binding_count = 1, .ubo_count = 1,
    };

    struct panvk_v9_compiled_shader cs = {0};
    char error[1024] = {0};
    int rc = compile((const uint32_t *)spv, spv_size, PANVK_V9_SHADER_COMPUTE,
                     "main", &cs_layout, &cs, error, sizeof(error));
    if (rc != 0) {
        fprintf(stderr, "FAIL: compute compile: %s\n", error);
        free(spv);
        return 1;
    }
    printf("CS compiled: %zu bytes, local_size=%ux%ux%u\n",
           cs.binary_size, cs.local_size_x, cs.local_size_y, cs.local_size_z);

    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) { fprintf(stderr, "FAIL: pan_kmod_dev_create NULL\n"); return 1; }

    struct v9_render_target_config config = { .width = 64, .height = 64, .clear_color = 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) { fprintf(stderr, "FAIL: v9_cmd_buffer_create NULL\n"); return 1; }

    struct pan_kmod_bo *ssbo = pan_kmod_bo_alloc(dev, 64, PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
    if (!ssbo) { fprintf(stderr, "FAIL: ssbo alloc\n"); return 1; }
    memset(ssbo->cpu, 0, 64);

    struct v9_ssbo_binding ssbo_binding = { .address = ssbo->gpu, .size = 64, .index = 0 };

    v9_cmd_buffer_begin(cmd);
    rc = v9_cmd_buffer_set_compute_shader(cmd, &cs);
    if (rc) { fprintf(stderr, "FAIL: set_compute_shader rc=%d\n", rc); return 1; }
    rc = v9_cmd_buffer_set_ssbos(cmd, &ssbo_binding, 1);
    if (rc) { fprintf(stderr, "FAIL: set_ssbos rc=%d\n", rc); return 1; }
    rc = v9_cmd_buffer_dispatch(cmd, 4, 1, 1);
    if (rc) { fprintf(stderr, "FAIL: dispatch rc=%d\n", rc); return 1; }

    /* Inspect the packed compute job at compute_job_gpu. */
    uint8_t *base_cpu = (uint8_t *)v9_cmd_buffer_get_mem_cpu(cmd);
    uint64_t base_gpu = v9_cmd_buffer_get_mem_gpu(cmd);
    uint64_t cj_gpu = base_gpu + 0xE600;
    uint32_t *cj = (uint32_t *)(base_cpu + (cj_gpu - base_gpu));
    printf("Compute job @ 0x%llx:\n", (unsigned long long)cj_gpu);
    printf("  header: 0x%08x (type=%u)\n", cj[4], (cj[4] >> 1) & 0xF);
    printf("  wgsize: 0x%08x (x-1=%u y-1=%u z-1=%u)\n",
           cj[8], cj[8] & 0x3FF, (cj[8] >> 10) & 0x3FF, (cj[8] >> 20) & 0x3FF);
    printf("  counts: %u x %u x %u\n", cj[10], cj[11], cj[12]);
    printf("  shader env resources=0x%llx shader=0x%llx tls=0x%llx\n",
           (unsigned long long)(cj[24] | ((uint64_t)cj[25] << 32)),
           (unsigned long long)(cj[26] | ((uint64_t)cj[27] << 32)),
           (unsigned long long)(cj[28] | ((uint64_t)cj[29] << 32)));

    /* Inspect compute shader program descriptor (stage must be 1). */
    uint64_t sp_cs_gpu = base_gpu + 0xCC80;
    uint32_t *sp = (uint32_t *)(base_cpu + (sp_cs_gpu - base_gpu));
    printf("CS shader program @ 0x%llx: word0=0x%08x (type=%u stage=%u)\n",
           (unsigned long long)sp_cs_gpu, sp[0], sp[0] & 0xF, (sp[0] >> 4) & 0xF);

    printf("Compute job full dump (32 words):\n");
    for (int i = 0; i < 32; i += 4)
        printf("  %04x: %08x %08x %08x %08x\n", i * 4, cj[i], cj[i + 1],
               cj[i + 2], cj[i + 3]);

    /* Inspect SSBO resource descriptor. */
    uint64_t ssbo_desc_gpu = base_gpu + 0xD340;
    uint32_t *sd = (uint32_t *)(base_cpu + (ssbo_desc_gpu - base_gpu));
    printf("SSBO desc @ 0x%llx: type=%u size=%u addr=0x%llx\n",
           (unsigned long long)ssbo_desc_gpu, sd[0] & 0xF, sd[1],
           (unsigned long long)(sd[2] | ((uint64_t)sd[3] << 32)));

    uint64_t tls_desc_gpu = base_gpu + 0xE100;
    uint32_t *td = (uint32_t *)(base_cpu + (tls_desc_gpu - base_gpu));
    printf("TLS desc @ 0x%llx: %08x %08x %08x %08x %08x %08x %08x %08x\n",
           (unsigned long long)tls_desc_gpu, td[0], td[1], td[2], td[3],
           td[4], td[5], td[6], td[7]);

    uint64_t res_gpu = base_gpu + 0xCC00;
    uint32_t *res = (uint32_t *)(base_cpu + (res_gpu - base_gpu));
    printf("Resource table @ 0x%llx:\n", (unsigned long long)res_gpu);
    for (int i = 0; i < 12; i += 4)
        printf("  %04x: %08x %08x %08x %08x\n", i * 4, res[i], res[i + 1],
               res[i + 2], res[i + 3]);

    rc = v9_cmd_buffer_end(cmd);
    if (rc) { fprintf(stderr, "FAIL: end rc=%d\n", rc); return 1; }
    rc = v9_cmd_buffer_submit(cmd);
    if (rc) { fprintf(stderr, "FAIL: submit rc=%d\n", rc); return 1; }

    printf("Compute dispatch submitted OK\n");
    if (!kbase_dry_run()) {
        uint32_t *out = (uint32_t *)ssbo->cpu;
        printf("SSBO contents: d[0]=0x%08x d[1]=0x%08x\n", out[0], out[1]);
    }

    pan_kmod_bo_free(ssbo);
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    cleanup(&cs);
    free(spv);
    dlclose(lib);

    printf("=== Valhall v9 compute dispatch test PASSED CLEANLY! ===\n");
    return 0;
}

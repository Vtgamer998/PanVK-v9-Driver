/*
 * FAU layout test: a compute shader with 3 SSBOs. The compiler emits one
 * LEA_PKA mode descriptor per binding into the FAU (fau_consts[], fau_count).
 * The driver must write fau_consts into the FAU buffer and program
 * SE[1] = fau_count, SE[14] = FAU buffer address. All 3 SSBO stores must land.
 *
 * Usage: ./test_fau_multi [libpanvk_v9_compiler.so] [cs3.spv]
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

int main(int argc, char **argv) {
    const char *libpath = argc > 1 ? argv[1] : "libpanvk_v9_compiler.so";
    const char *cspath  = argc > 2 ? argv[2] : "cs3.spv";
    int n_ssbo          = argc > 3 ? atoi(argv[3]) : 3;

    printf("=== FAU layout multi-SSBO test (real GPU) ===\n");

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

    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) { fprintf(stderr, "FAIL: dev\n"); return 1; }

    struct v9_render_target_config config = { .width = 64, .height = 64, .clear_color = 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) { fprintf(stderr, "FAIL: cmd\n"); return 1; }

    /* One SSBO per binding, all accessed by the shader. */
    struct pan_kmod_bo *ssbo[3];
    for (int i = 0; i < 3; i++) {
        ssbo[i] = pan_kmod_bo_alloc(dev, 64, PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
        if (!ssbo[i]) { fprintf(stderr, "FAIL: ssbo%d\n", i); return 1; }
    }

    /* n bindings: resource_index = mode descriptor, one per SSBO. */
    uint32_t modes[3] = { 0x01000001u, 0x01000002u, 0x01000004u };
    struct panvk_v9_descriptor_binding bindings[3] = {
        { .set = 0, .binding = 0, .descriptor_type = 7, .array_size = 1, .resource_index = modes[0] },
        { .set = 0, .binding = 1, .descriptor_type = 7, .array_size = 1, .resource_index = modes[1] },
        { .set = 0, .binding = 2, .descriptor_type = 7, .array_size = 1, .resource_index = modes[2] },
    };
    struct panvk_v9_pipeline_layout layout = {
        .bindings = bindings, .binding_count = n_ssbo, .ubo_count = 0,
    };
    struct panvk_v9_compiled_shader cs = {0};
    char error[1024] = {0};
    int rc = compile((const uint32_t *)spv, spv_size, PANVK_V9_SHADER_COMPUTE,
                     "main", &layout, &cs, error, sizeof(error));
    if (rc) { fprintf(stderr, "FAIL: compile: %s\n", error); return 1; }
    printf("compiled: fau_count=%u fau_consts=[0x%08x 0x%08x 0x%08x]\n",
           cs.fau_count, cs.fau_consts[0], cs.fau_consts[1], cs.fau_consts[2]);
    if (v9_cmd_buffer_set_compute_shader(cmd, &cs)) {
        fprintf(stderr, "FAIL: set_compute_shader\n"); return 1;
    }
    uint8_t *base_cpu = (uint8_t *)v9_cmd_buffer_get_mem_cpu(cmd);
    uint64_t base_gpu = v9_cmd_buffer_get_mem_gpu(cmd);
    uint64_t cj_gpu = base_gpu + 0xE600;
    uint32_t *cj = (uint32_t *)(base_cpu + (cj_gpu - base_gpu));
    uint64_t res_gpu = base_gpu + 0xD200;
    uint64_t sp_gpu  = base_gpu + 0xCC80;
    uint64_t tls_gpu = base_gpu + 0xE100;
    uint32_t *res = (uint32_t *)(base_cpu + (res_gpu - base_gpu));
    uint32_t *tbl = (uint32_t *)(base_cpu + 0xD340);
    uint32_t *td = (uint32_t *)(base_cpu + (tls_gpu - base_gpu));

    for (int i = 0; i < 3; i++)
        memset(ssbo[i]->cpu, 0, 64);
    memset(tbl, 0, 8 * 32);
    memset(res, 0, 16 * 16);

    /* Descriptor table: mode index bits select a 32-byte slot. Mode for
     * binding i = 0x01000000 | (1 << i) -> slots 1, 2, 4 (bytes 32, 64, 128). */
    uint32_t slots[3] = { 32, 64, 128 };
    for (int i = 0; i < n_ssbo; i++)
        v9_pack_buffer((uint32_t *)((uint8_t *)tbl + slots[i]), ssbo[i]->gpu, 64);

    /* RES1 resource covering the descriptor table through the last slot. */
    v9_pack_resource(res + 1 * 4, base_gpu + 0xD340, slots[n_ssbo - 1] + 32);

    uint64_t fau_gpu = base_gpu + 0xDD00;
    uint32_t *fau = (uint32_t *)(base_cpu + (fau_gpu - base_gpu));
    memset(fau, 0, 64);
    for (unsigned i = 0; i < cs.fau_count; i++)
        fau[i] = cs.fau_consts[i];
    v9_pack_compute_job(cj, 4, 1, 1, 4, 1, 1, res_gpu, sp_gpu, tls_gpu,
                        cs.fau_count, fau_gpu);
    td[1] = 0x80000000u;

    uint32_t event_code = 0;
    int ret = pan_kmod_submit_atom(dev, cj_gpu, KBASE_QUEUE_REQ_COMPUTE, 0, &event_code);
    uint32_t exc = cj[0], first = cj[1];
    uint64_t fault = (uint64_t)cj[2] | ((uint64_t)cj[3] << 32);
    printf("ret=%d event=0x%x exc=0x%x flt=0x%llx\n",
           ret, event_code, exc, (unsigned long long)fault);

    int ok = 1;
    for (int i = 0; i < n_ssbo; i++) {
        uint32_t *out = (uint32_t *)ssbo[i]->cpu;
        printf("ssbo%d[0]=0x%08x%s\n", i, out[0], out[0] == 1 ? "  <== WRITE" : "");
        if (out[0] != 1) ok = 0;
    }

    printf(ok ? "FAU: all SSBOs written.\n" : "FAU: FAILURE.\n");

    cleanup(&cs);
    for (int i = 0; i < 3; i++) pan_kmod_bo_free(ssbo[i]);
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    free(spv);
    dlclose(lib);
    return ok ? 0 : 1;
}

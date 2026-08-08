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
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f); *size = n; return buf;
}

int main(int argc, char **argv) {
    const char *libpath = argc > 1 ? argv[1] : "libpanvk_v9_compiler.so";
    const char *cspath  = argc > 2 ? argv[2] : "cs3.spv";
    int mode3 = argc > 3 ? atoi(argv[3]) : 0; /* 0 = 0,1,2 mode; 1 = 1,2,4 mode */

    void *lib = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    int (*compile)(const uint32_t *, size_t, enum panvk_v9_shader_stage, const char *, const struct panvk_v9_pipeline_layout *, struct panvk_v9_compiled_shader *, char *, size_t) = dlsym(lib, "panvk_v9_compile_spirv");
    void (*cleanup)(struct panvk_v9_compiled_shader *) = dlsym(lib, "panvk_v9_compiled_shader_cleanup");
    size_t spv_size = 0; uint8_t *spv = read_file(cspath, &spv_size);

    uint32_t modes[3];
    uint32_t slots[3];
    if (mode3) { modes[0]=0x01000001u; modes[1]=0x01000002u; modes[2]=0x01000004u; slots[0]=32; slots[1]=64; slots[2]=128; }
    else       { modes[0]=0;           modes[1]=1;           modes[2]=2;           slots[0]=0;  slots[1]=32; slots[2]=64; }

    struct panvk_v9_descriptor_binding bindings[3] = {
        { .set = 0, .binding = 0, .descriptor_type = 7, .array_size = 1, .resource_index = modes[0] },
        { .set = 0, .binding = 1, .descriptor_type = 7, .array_size = 1, .resource_index = modes[1] },
        { .set = 0, .binding = 2, .descriptor_type = 7, .array_size = 1, .resource_index = modes[2] },
    };
    struct panvk_v9_pipeline_layout layout = { .bindings = bindings, .binding_count = 3, .ubo_count = 0 };
    struct panvk_v9_compiled_shader cs = {0}; char error[1024] = {0};
    int rc = compile((const uint32_t *)spv, spv_size, PANVK_V9_SHADER_COMPUTE, "main", &layout, &cs, error, sizeof error);
    printf("compile rc=%d fau_count=%u fau=[0x%08x 0x%08x 0x%08x]\n", rc, cs.fau_count, cs.fau_consts[0], cs.fau_consts[1], cs.fau_consts[2]);

    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    struct v9_render_target_config config = { .width = 64, .height = 64, .clear_color = 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    struct pan_kmod_bo *ssbo[3];
    for (int i = 0; i < 3; i++) ssbo[i] = pan_kmod_bo_alloc(dev, 64, PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
    for (int i = 0; i < 3; i++) memset(ssbo[i]->cpu, 0, 64);

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

    memset(tbl, 0, 8 * 32);
    memset(res, 0, 16 * 16);
    for (int i = 0; i < 3; i++)
        v9_pack_buffer((uint32_t *)((uint8_t *)tbl + slots[i]), ssbo[i]->gpu, 64);
    v9_pack_resource(res + 4, base_gpu + 0xD340, slots[2] + 32);

    v9_cmd_buffer_set_compute_shader(cmd, &cs);
    v9_cmd_buffer_dispatch(cmd, 4, 1, 1);

    v9_pack_compute_job(cj, 4, 1, 1, 4, 1, 1, res_gpu, sp_gpu, tls_gpu, cs.fau_count, base_gpu + 0xDD00);
    uint32_t *fau = (uint32_t *)(base_cpu + 0xDD00);
    memset(fau, 0, 64);
    for (unsigned i = 0; i < cs.fau_count; i++) fau[i] = cs.fau_consts[i];
    td[1] = 0x80000000u;

    uint32_t event_code = 0;
    int ret = pan_kmod_submit_atom(dev, cj_gpu, KBASE_QUEUE_REQ_COMPUTE, 0, &event_code);
    printf("ret=%d event=0x%x\n", ret, event_code);
    for (int i = 0; i < 3; i++)
        printf("ssbo%d[0]=0x%08x%s\n", i, ((uint32_t *)ssbo[i]->cpu)[0], ((uint32_t *)ssbo[i]->cpu)[0] == 1 ? "  <== WRITE" : "");

    for (int i = 0; i < 3; i++) pan_kmod_bo_free(ssbo[i]);
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    cleanup(&cs);
    return 0;
}

/*
 * Probe: which mode-descriptor encoding makes LEA_PKA resolve the SSBO?
 *
 * For each probe we (re)compile the cs with a specific resource_index, pack
 * the resource table + descriptor table accordingly, submit, and read back the
 * SSBO to see if the store landed.
 *
 * Usage: ./test_pka_probe [libpanvk_v9_compiler.so] [cs.spv]
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

struct probe {
    const char *name;
    uint32_t resource_index;   /* LEA_PKA mode descriptor */
    uint32_t ssbo_index;       /* ssbo descriptor slot in the table */
    uint32_t res_slot;         /* which resource slot points at the table */
    uint32_t desc_offset;      /* descriptor byte offset within table */
    int contains_descriptors;  /* resource bit 24 */
    uint32_t res_count;        /* low bits of Resources pointer */
    int use_fau;               /* provide FAU buffer with mode at word 0 */
};

static uint32_t
desc_offset_from_mode(uint32_t mode)
{
    /* hypothesis: descriptor index = bits 0-23, * 32 bytes */
    return (mode & 0xFFFFFFu) * 32;
}

int main(int argc, char **argv) {
    const char *libpath = argc > 1 ? argv[1] : "libpanvk_v9_compiler.so";
    const char *cspath  = argc > 2 ? argv[2] : "/data/data/com.termux/files/usr/tmp/opencode/cs.spv";

    printf("=== PKA mode probe (real GPU) ===\n");

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

    struct pan_kmod_bo *ssbo = pan_kmod_bo_alloc(dev, 64, PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
    if (!ssbo) { fprintf(stderr, "FAIL: ssbo\n"); return 1; }

    uint8_t *base_cpu = (uint8_t *)v9_cmd_buffer_get_mem_cpu(cmd);
    uint64_t base_gpu = v9_cmd_buffer_get_mem_gpu(cmd);
    uint64_t cj_gpu = base_gpu + 0xE600;
    uint32_t *cj = (uint32_t *)(base_cpu + (cj_gpu - base_gpu));
    uint64_t res_gpu = base_gpu + 0xD200;
    uint64_t sp_gpu  = base_gpu + 0xCC80;
    uint64_t tls_gpu = base_gpu + 0xE100;
    uint32_t *res = (uint32_t *)(base_cpu + (res_gpu - base_gpu));
    uint32_t *ssbos = (uint32_t *)(base_cpu + 0xD340);
    uint32_t *td = (uint32_t *)(base_cpu + (tls_gpu - base_gpu));

    struct probe probes[] = {
        /* baseline: index 0, RES0 = table, desc at slot 0 */
        { "A 0x00000000 RES0 d0 cnt1", 0x00000000u, 0, 0, 0, 1, 1, 0 },
        /* panvk-style: table 1, RES1 desc@0, count 8 */
        { "H2 0x01000000 RES1 d0 cnt8", 0x01000000u, 0, 1, 0, 1, 8, 0 },
        /* index 1 via FAU uniform (u0.w0) */
        { "H3 0x01000001 RES1 d@32 +FAU", 0x01000001u, 0, 1, 32, 1, 8, 1 },
        /* index 1 via FAU, but no FAU buffer (control) */
        { "H3x 0x01000001 RES1 d@32 noFAU", 0x01000001u, 0, 1, 32, 1, 8, 0 },
        /* index 2 via FAU */
        { "H5 0x01000002 RES1 d@64 +FAU", 0x01000002u, 0, 1, 64, 1, 8, 1 },
        /* index 0x100 via FAU */
        { "H6 0x01000100 RES1 d@0x2000 +FAU", 0x01000100u, 0, 1,
          desc_offset_from_mode(0x01000100u), 1, 8, 1 },
    };

    int n = (int)(sizeof(probes) / sizeof(probes[0]));
    int ok = 0;
    for (int i = 0; i < n; i++) {
        struct probe *p = &probes[i];

        memset(ssbo->cpu, 0, 64);
        memset(ssbos, 0, 8 * 32);
        memset(res, 0, 16 * 16);

        struct panvk_v9_descriptor_binding bindings[1] = {
            { .set = 0, .binding = 0, .descriptor_type = 7, .array_size = 1,
              .resource_index = p->resource_index },
        };
        struct panvk_v9_pipeline_layout layout = {
            .bindings = bindings, .binding_count = 1, .ubo_count = 0,
        };
        struct panvk_v9_compiled_shader cs = {0};
        char error[1024] = {0};
        int rc = compile((const uint32_t *)spv, spv_size, PANVK_V9_SHADER_COMPUTE,
                         "main", &layout, &cs, error, sizeof(error));
        if (rc != 0) { fprintf(stderr, "  [%s] compile FAIL: %s\n", p->name, error); continue; }
        printf("  isa[0..3]=%08x %08x %08x %08x\n",
               ((uint32_t *)cs.binary)[0], ((uint32_t *)cs.binary)[1],
               ((uint32_t *)cs.binary)[2], ((uint32_t *)cs.binary)[3]);

        {
            char path[128];
            snprintf(path, sizeof(path),
                     "/data/data/com.termux/files/usr/tmp/opencode/probe_%08x.bin",
                     p->resource_index);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(cs.binary, 1, cs.binary_size, f); fclose(f); }
        }

        v9_cmd_buffer_set_compute_shader(cmd, &cs);

        /* Pack the SSBO descriptor directly into the table at the
         * descriptor byte offset implied by the mode. */
        uint32_t *ssbo_tbl = (uint32_t *)(base_cpu + 0xD340);
        uint32_t table_bytes = p->desc_offset + 32;
        memset(ssbo_tbl, 0, 8 * 32);
        v9_pack_buffer(ssbo_tbl + p->desc_offset / 4, ssbo->gpu, 64);

        /* Point resource slot p->res_slot at the ssbo descriptor table (or the
         * buffer itself if contains_descriptors=0), with enough room for the
         * descriptor slots used. */
        if (p->contains_descriptors) {
            v9_pack_resource(res + p->res_slot * 4, base_gpu + 0xD340,
                             table_bytes);
        } else {
            uint32_t *r = res + p->res_slot * 4;
            memset(r, 0, 16);
            r[0] = (uint32_t)ssbo->gpu;
            r[1] = (uint32_t)(ssbo->gpu >> 32); /* contains_descriptors = 0 */
            r[2] = 64;
        }

        v9_pack_compute_job(cj, 4, 1, 1, 4, 1, 1, res_gpu, sp_gpu, tls_gpu, 0, 0);
        cj[24] = (uint32_t)((res_gpu & ~0x3Full) | (p->res_count & 0x3Fu));
        td[1] = 0x80000000u;

        /* Provide the FAU uniform buffer: se[1] = FAU count, se[14] = address.
         * u0.w0 (uniform register 0 word 0) = FAU word 0. */
        if (p->use_fau) {
            uint64_t fau_gpu = base_gpu + 0xDD00;
            uint32_t *fau = (uint32_t *)(base_cpu + (fau_gpu - base_gpu));
            memset(fau, 0, 64);
            fau[0] = p->resource_index;
            cj[17] = 4; /* FAU count (words) */
            pack_u64(cj + 30, fau_gpu);
        }

        uint32_t event_code = 0;
        int ret = pan_kmod_submit_atom(dev, cj_gpu, KBASE_QUEUE_REQ_COMPUTE, 0, &event_code);
        uint32_t exc = cj[0], first = cj[1];
        uint64_t fault = (uint64_t)cj[2] | ((uint64_t)cj[3] << 32);
        uint32_t *out = (uint32_t *)ssbo->cpu;
        printf("[%s] ret=%d event=0x%x exc=0x%x flt=0x%llx d[0]=0x%08x%s\n",
               p->name, ret, event_code, exc, (unsigned long long)fault,
               out[0], (out[0] == 1) ? "  <== WRITE" : "");
        if (ret == 0 && event_code == 0x1 && out[0] == 1)
            ok = 1;

        cleanup(&cs);
    }

    printf(ok ? "PROBE: at least one config wrote the SSBO.\n"
              : "PROBE: no config wrote the SSBO.\n");
    pan_kmod_bo_free(ssbo);
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    free(spv);
    dlclose(lib);
    return ok ? 0 : 1;
}

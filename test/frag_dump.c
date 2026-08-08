#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"
#include "kbase_winsys.h"

struct v9_cmd_buffer {
    unsigned refcount;
    struct pan_kmod_dev *dev;
    struct v9_render_target_config config;
    struct pan_kmod_bo *mem_bo;
    struct pan_kmod_bo *exec_bo;
    struct pan_kmod_bo *exec_vs_bo;
    struct pan_kmod_bo *color_bo;
    uint64_t mfbd_gva, rt0_gpu, polylist_gpu, sampleloc_gpu, dcd_gpu, sp_gpu,
             sp_vertex_gpu, isa_gpu, isa_vertex_gpu;
    bool has_vertex_shader, has_varying_shader, has_draw_command, use_malloc_vertex;
    uint64_t res_gpu, ubo_gpu, attr_buf_gpu, attr_gpu, flush_jc_gpu,
             tiler_heap_desc_gpu, tiler_ctx_gpu, pos_gpu, blend_gpu, depth_gpu,
             tls_gpu, idx_gpu, tiler_job_gpu, frag_jc_gpu, frag_jc2_gpu,
             mfbd2_gpu, dcd2_gpu, tiler_heap_backing_gpu, color_gpu;
};

static void dump_fj(uint32_t *fj, const char *name) {
    printf("%s\n", name);
    for (int i = 0; i < 16; i++) {
        if (i % 4 == 0) printf("  0x%03x:", i * 4);
        printf(" 0x%08x", fj[i]);
        if (i % 4 == 3) printf("\n");
    }
}

int main(int argc, char **argv) {
    unsigned w = argc > 1 ? (unsigned)atoi(argv[1]) : 16;
    unsigned h = argc > 2 ? (unsigned)atoi(argv[2]) : 16;
    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) return 1;
    struct v9_render_target_config config = { w, h, 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) return 1;
    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed_triangle(cmd);
    v9_cmd_buffer_end(cmd);

    uint8_t *base_cpu = (uint8_t *)cmd->mem_bo->cpu;
    uint64_t base_gva = cmd->mem_bo->gpu;
    uint32_t *fj1 = (uint32_t *)(base_cpu + (cmd->frag_jc_gpu - base_gva));
    uint32_t *fj2 = (uint32_t *)(base_cpu + (cmd->frag_jc2_gpu - base_gva));

    dump_fj(fj1, "FJ1 after end");
    dump_fj(fj2, "FJ2 after end");

    uint32_t *mf = (uint32_t *)(base_cpu + (cmd->mfbd_gva - base_gva));
    printf("MFBD after end\n");
    for (int i = 0; i < 32; i++) {
        if (i % 4 == 0) printf("  0x%03x:", i * 4);
        printf(" 0x%08x", mf[i]);
        if (i % 4 == 3) printf("\n");
    }
    uint32_t *mf2 = (uint32_t *)(base_cpu + (cmd->mfbd2_gpu - base_gva));
    printf("MFBD2 after end\n");
    for (int i = 0; i < 32; i++) {
        if (i % 4 == 0) printf("  0x%03x:", i * 4);
        printf(" 0x%08x", mf2[i]);
        if (i % 4 == 3) printf("\n");
    }

    printf("=== submitting ===\n");
    int ret = v9_cmd_buffer_submit(cmd);
    printf("ret=%d\n", ret);
    dump_fj(fj1, "FJ1 after submit");
    dump_fj(fj2, "FJ2 after submit");

    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

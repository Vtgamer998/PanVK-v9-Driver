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

int main(void) {
    unsigned w = 64, h = 64;
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
    printf("base=0x%llx tiler_job_gpu=0x%llx tiler_ctx_gpu=0x%llx\n",
           (unsigned long long)base_gva,
           (unsigned long long)cmd->tiler_job_gpu,
           (unsigned long long)cmd->tiler_ctx_gpu);

    uint32_t *vt = (uint32_t *)(base_cpu + (cmd->tiler_job_gpu - base_gva));
    printf("TJ full (256 bytes)\n");
    for (int i = 0; i < 64; i++) {
        if (i % 4 == 0) printf("  0x%03x:", i * 4);
        printf(" 0x%08x", vt[i]);
        if (i % 4 == 3) printf("\n");
    }

    uint32_t *tc = (uint32_t *)(base_cpu + (cmd->tiler_ctx_gpu - base_gva));
    printf("tiler_ctx:\n");
    for (int i = 0; i < 48; i++) {
        if (i % 4 == 0) printf("  0x%03x:", i * 4);
        printf(" 0x%08x", tc[i]);
        if (i % 4 == 3) printf("\n");
    }

    uint32_t *th = (uint32_t *)(base_cpu + (cmd->tiler_heap_desc_gpu - base_gva));
    printf("tiler_heap_desc:\n");
    for (int i = 0; i < 8; i++) printf("  th[%d]=0x%08x\n", i, th[i]);

    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

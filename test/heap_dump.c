#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(int argc, char **argv) {
    unsigned w = argc > 1 ? atoi(argv[1]) : 800;
    unsigned h = argc > 2 ? atoi(argv[2]) : 600;
    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) return 1;
    struct v9_render_target_config config = { w, h, 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) return 1;
    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed_triangle(cmd);
    v9_cmd_buffer_end(cmd);
    int ret = v9_cmd_buffer_submit(cmd);
    printf("submit ret=%d\n", ret);

    uint8_t *base_cpu = (uint8_t *)cmd->mem_bo->cpu;
    uint64_t base_gva = cmd->mem_bo->gpu;
    uint64_t heap = cmd->tiler_heap_backing_gpu;
    uint8_t *hcpu = base_cpu + (heap - base_gva);
    printf("heap backing gpu=0x%llx\n", (unsigned long long)heap);
    for (int i = 0; i < 32; i++) {
        uint64_t q = *(uint64_t *)(hcpu + i * 8);
        if (q) printf("  heap+0x%04x: 0x%016llx\n", i * 8, (unsigned long long)q);
    }
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

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

    unsigned tiles_x = (w + 15) / 16, tiles_y = (h + 15) / 16;
    size_t hdr_bytes = tiles_x * tiles_y * 8;
    printf("tiles %ux%u hdr_bytes=%zu\n", tiles_x, tiles_y, hdr_bytes);

    uint64_t *pl = (uint64_t *)(base_cpu + (cmd->polylist_gpu - base_gva));
    int nonzero = 0;
    for (size_t i = 0; i < hdr_bytes / 8; i++)
        if (pl[i]) nonzero++;
    printf("polylist header: %d/%zu nonzero\n", nonzero, hdr_bytes / 8);

    printf("first 40 headers:\n");
    for (int i = 0; i < 40 && i < (int)(hdr_bytes / 8); i++)
        printf("  [%2d] 0x%016llx\n", i, (unsigned long long)pl[i]);

    printf("tiler heap desc:\n");
    uint32_t *th = (uint32_t *)(base_cpu + (cmd->tiler_heap_desc_gpu - base_gva));
    for (int i = 0; i < 8; i++) printf("  th[%d]=0x%08x\n", i, th[i]);

    printf("tiler ctx:\n");
    uint32_t *tc = (uint32_t *)(base_cpu + (cmd->tiler_ctx_gpu - base_gva));
    for (int i = 0; i < 8; i++) printf("  tc[%d]=0x%08x\n", i, tc[i]);

    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

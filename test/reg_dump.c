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

static void scan_region(uint8_t *cpu, uint64_t start, uint64_t end, const char *name) {
    int nz = 0;
    uint64_t first_nz = 0;
    for (uint64_t a = start; a < end; a += 8) {
        uint64_t q = *(uint64_t *)(cpu + a);
        if (q) { if (!first_nz) first_nz = a; nz++; }
    }
    printf("%s [0x%llx..0x%llx): %d nonzero, first at 0x%llx\n",
           name, (unsigned long long)start, (unsigned long long)end, nz,
           (unsigned long long)first_nz);
}

int main(int argc, char **argv) {
    unsigned w = argc > 1 ? atoi(argv[1]) : 32;
    unsigned h = argc > 2 ? atoi(argv[2]) : 32;
    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) return 1;
    struct v9_render_target_config config = { w, h, 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) return 1;
    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed_triangle(cmd);
    v9_cmd_buffer_end(cmd);
    int ret = v9_cmd_buffer_submit(cmd);
    printf("submit ret=%d size=%ux%u\n", ret, w, h);
    uint8_t *base_cpu = (uint8_t *)cmd->mem_bo->cpu;
    uint64_t base_gva = cmd->mem_bo->gpu;
    printf("mem_bo size=0x%zx gva=0x%llx\n", cmd->mem_bo->size,
           (unsigned long long)base_gva);
    printf("tc[0] = 0x%016llx\n",
           (unsigned long long)*(uint64_t *)(base_cpu + (cmd->tiler_ctx_gpu - base_gva)));
    scan_region(base_cpu, 0x7000, 0x8000, "polylist 0x7000");
    scan_region(base_cpu, 0x40000, 0x80000, "heap 0x40000");
    scan_region(base_cpu, 0xE200, 0xEA00, "tiler+frag jobs");
    printf("color(0,0)=0x%08x\n", v9_cmd_buffer_read_pixel(cmd, 0, 0));
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    unsigned w = 64, h = 64;
    if (argc >= 3) { w = atoi(argv[1]); h = atoi(argv[2]); }
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
    uint64_t polylist_off = cmd->polylist_gpu - base_gva;
    uint64_t heap_off = cmd->tiler_heap_backing_gpu - base_gva;
    uint64_t tctx_off = cmd->tiler_ctx_gpu - base_gva;

    /* Submit only the TILER_JOB atom to inspect binning output */
    uint32_t event_code = 0;
    int ret = pan_kmod_submit_atom(cmd->dev, cmd->tiler_job_gpu, KBASE_QUEUE_REQ_TILER, 0, &event_code);
    printf("TILER_JOB submit ret=%d event=0x%x (%dx%d)\n", ret, event_code, w, h);

    /* Dump tiler context after GPU writes */
    uint32_t *tc = (uint32_t *)(base_cpu + tctx_off);
    printf("tiler_ctx after:\n");
    for (int i = 0; i < 48; i++) {
        if (i % 4 == 0) printf("  0x%03x:", i * 4);
        printf(" 0x%08x", tc[i]);
        if (i % 4 == 3) printf("\n");
    }

    uint32_t *th = (uint32_t *)(base_cpu + (cmd->tiler_heap_desc_gpu - base_gva));
    printf("tiler_heap_desc after: th[0]=0x%08x th[1]=0x%08x\n", th[0], th[1]);
    printf("  th[2:3]=0x%08x%08x th[4:5]=0x%08x%08x th[6:7]=0x%08x%08x\n",
           th[3], th[2], th[5], th[4], th[7], th[6]);

    /* Dump polygon list header slots */
    uint64_t *hdr = (uint64_t *)(base_cpu + polylist_off);
    unsigned tiles_x = (w + 15) / 16, tiles_y = (h + 15) / 16;
    int found = 0;
    for (unsigned t = 0; t < tiles_x * tiles_y; t++) {
        if (hdr[t] != 0) {
            printf("poly slot %u (tile %u,%u): raw 0x%016llx (lo=0x%08x hi=0x%08x)\n",
                   t, t % tiles_x, t / tiles_x, (unsigned long long)hdr[t],
                   (uint32_t)hdr[t], (uint32_t)(hdr[t] >> 32));
            found++;
        }
    }
    printf("poly scan: %d active of %u\n", found, tiles_x * tiles_y);

    /* Dump heap non-zero words */
    uint64_t *heap = (uint64_t *)(base_cpu + heap_off);
    int hcount = 0;
    for (unsigned i = 0; i < 0x40000 / 8; i++) {
        if (heap[i] != 0) {
            if (hcount < 8)
                printf("heap+0x%05x: 0x%016llx\n", i * 8, (unsigned long long)heap[i]);
            hcount++;
        }
    }
    printf("heap scan: %d non-zero words\n", hcount);

    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    return 0;
}

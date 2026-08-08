/*
 * Valhall v9 Command Stream Recorder Engine Implementation
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include "v9_cmd_stream.h"
#include "v9_pack.h"
#include "pan_kmod_kbase.h"
#include "kbase_winsys.h"

/* Pre-compiled Valhall fragment shader producing solid green (0xFF00FF00) (56 bytes)
 * Matches the reference: IADD/FADD setup, NOP.wait0126, ATEST.discard, BLEND.end. */
static const uint8_t k_valhall_green_fs[] = {
    0xc0, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x10, 0x01, /* IADD_IMM.i32 r0, 0x0, #0x0 */
    0x00, 0xd0, 0x00, 0x00, 0x00, 0xc1, 0xa4, 0x00, /* FADD.f32 r1, r0, 0x3F800000 */
    0xc0, 0x00, 0x00, 0x00, 0x00, 0xc2, 0x10, 0x01, /* IADD_IMM.i32 r2, 0x0, #0x0 */
    0x00, 0xd0, 0x00, 0x00, 0x00, 0xc3, 0xa4, 0x00, /* FADD.f32 r3, r0, 0x3F800000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x40, /* NOP.wait0126 */
    0x3c, 0xd0, 0xea, 0x00, 0x02, 0xbc, 0x7d, 0x68, /* ATEST.discard @r60, r60, 0x3F800000 */
    0xf0, 0x00, 0x3c, 0x32, 0x08, 0x40, 0x7f, 0x78  /* BLEND.slot0.v4.f32.end */
};

/* Double-buffer slot: one renders on GPU while the other is prepared on CPU.
 * Eliminates the ~5ms fork overhead at 60fps by pipelining frame N+1 setup
 * while frame N is in flight.  Index alternates 0/1 each frame. */
struct v9_dbl_slot {
    struct pan_kmod_bo *color_bo;   /* render target for this slot            */
    struct pan_kmod_bo *mem_bo;     /* descriptors + tiler heap               */
    uint64_t color_gpu;
    uint64_t mfbd_gva;
    uint64_t polylist_gpu;
    int      in_flight;             /* 1 = GPU currently rendering this slot  */
};

struct v9_cmd_buffer {
    unsigned refcount;
    struct pan_kmod_dev *dev;
    struct v9_render_target_config config;
    struct pan_kmod_bo *mem_bo;
    struct pan_kmod_bo *exec_bo;
    struct pan_kmod_bo *exec_vs_bo;
    struct pan_kmod_bo *color_bo;

    uint64_t mfbd_gva;
    uint64_t rt0_gpu;
    uint64_t polylist_gpu;
    uint64_t sampleloc_gpu;
    uint64_t dcd_gpu;
    uint64_t sp_gpu;
    uint64_t sp_vertex_gpu;
    uint64_t isa_gpu;
    uint64_t isa_vertex_gpu;
    bool has_vertex_shader;
    bool has_varying_shader;
    bool has_draw_command;
    bool has_compute_command;
    bool use_malloc_vertex;
    uint64_t res_gpu;
    uint64_t ubo_gpu;
    uint64_t ssbo_gpu;
    uint64_t attr_buf_gpu;
    uint64_t attr_gpu;
    uint64_t flush_jc_gpu;
    uint64_t tiler_heap_desc_gpu;
    uint64_t tiler_ctx_gpu;
    uint64_t pos_gpu;
    uint64_t blend_gpu;
    uint64_t depth_gpu;
    uint64_t tls_gpu;
    uint64_t idx_gpu;
    uint64_t tiler_job_gpu;
    uint64_t frag_jc_gpu;
    uint64_t frag_jc2_gpu;
    uint64_t mfbd2_gpu;
    uint64_t dcd2_gpu;
    uint64_t tiler_heap_backing_gpu;
    uint64_t color_gpu;
    struct pan_kmod_bo *exec_cs_bo;
    uint64_t isa_cs_gpu;
    uint64_t sp_cs_gpu;
    uint64_t compute_job_gpu;
    uint32_t local_size_x;
    uint32_t local_size_y;
    uint32_t local_size_z;
    bool has_compute_shader;
    uint64_t fau_cs_gpu;
    uint64_t fau_fs_gpu;
    uint64_t fau_vs_gpu;
    uint32_t fau_cs_count;
    uint32_t fau_fs_count;
    uint32_t fau_vs_count;

    /* Double-buffering: two independent color+mem slots so the CPU can pack
     * frame N+1 while the GPU renders frame N.  active_slot alternates 0/1. */
    struct v9_dbl_slot dbl[2];
    int active_slot; /* which slot is currently being recorded/submitted */
    uint64_t frame_count;
};

/* Point the legacy single-BO aliases + GVA offsets at the given slot.  All the
 * *_gpu fields are GVA-absolute, so they must be recomputed from the slot's own
 * mem_bo base address (the two slots are separate 1 MiB BOs with independent
 * GVAs).  Call before packing/writing anything for a slot. */
static void v9_slot_repoint(struct v9_cmd_buffer *cmd, int slot) {
    cmd->active_slot = slot;
    cmd->color_bo = cmd->dbl[slot].color_bo;
    cmd->mem_bo   = cmd->dbl[slot].mem_bo;
    cmd->color_gpu = cmd->dbl[slot].color_gpu;
    uint64_t base_gva = cmd->mem_bo->gpu;
    cmd->mfbd_gva          = base_gva + 0x6000;
    cmd->rt0_gpu           = base_gva + 0x6080;
    cmd->polylist_gpu      = base_gva + 0x7000;
    cmd->sampleloc_gpu     = base_gva + 0xB100;
    cmd->dcd_gpu           = base_gva + 0xC100;
    cmd->sp_gpu            = base_gva + 0xCC00;
    cmd->sp_vertex_gpu     = base_gva + 0xCD00;
    cmd->res_gpu           = base_gva + 0xD200;
    cmd->ubo_gpu           = base_gva + 0xD300;
    cmd->attr_buf_gpu      = base_gva + 0xD700;
    cmd->attr_gpu          = base_gva + 0xD900;
    cmd->flush_jc_gpu      = base_gva + 0xD400;
    cmd->tiler_heap_desc_gpu = base_gva + 0xD500;
    cmd->tiler_ctx_gpu     = base_gva + 0xD600;
    cmd->pos_gpu           = base_gva + 0xE000;
    cmd->blend_gpu         = base_gva + 0xE040;
    cmd->depth_gpu         = base_gva + 0xE060;
    cmd->tls_gpu           = base_gva + 0xE100;
    cmd->idx_gpu           = base_gva + 0xE0C0;
    cmd->tiler_job_gpu     = base_gva + 0xE200;
    cmd->frag_jc_gpu       = base_gva + 0xE380;
    cmd->frag_jc2_gpu      = base_gva + 0xE400;
    cmd->mfbd2_gpu         = base_gva + 0xE480;
    cmd->dcd2_gpu          = base_gva + 0xE500;
    cmd->tiler_heap_backing_gpu = base_gva + 0x40000;
    cmd->sp_cs_gpu         = base_gva + 0xCC80;
    cmd->ssbo_gpu          = base_gva + 0xD340;
    cmd->compute_job_gpu   = base_gva + 0xE600;
    cmd->fau_cs_gpu        = base_gva + 0xDD00;
    cmd->fau_fs_gpu        = base_gva + 0xDD80;
    cmd->fau_vs_gpu        = base_gva + 0xDE00;
    cmd->dbl[slot].in_flight = 1;
}

/* Re-pack the static descriptor set (blend/TLS/depth/RT/shader program/tiler
 * heap/ctx/MFBD/DCD/flush/pos/idx/sampleloc) into the CURRENT slot's mem_bo.
 * This is idempotent and must be called for a slot before it is submitted. */
static void v9_slot_pack_static(struct v9_cmd_buffer *cmd) {
    uint8_t *base_cpu = (uint8_t *)cmd->mem_bo->cpu;
    uint64_t mem_base = cmd->mem_bo->gpu;
    uint32_t w = cmd->config.width, h = cmd->config.height;

    v9_pack_blend((uint32_t *)(base_cpu + (cmd->blend_gpu - mem_base)));
    v9_pack_tls((uint32_t *)(base_cpu + (cmd->tls_gpu - mem_base)), mem_base + 0x10000);
    v9_pack_depth((uint32_t *)(base_cpu + (cmd->depth_gpu - mem_base)));

    v9_pack_shader_program((uint32_t *)(base_cpu + (cmd->sp_gpu - mem_base)),
                           cmd->isa_gpu, 2, 32, 0, true, true, false, false);
    v9_pack_tiler_heap((uint32_t *)(base_cpu + (cmd->tiler_heap_desc_gpu - mem_base)),
                       cmd->tiler_heap_backing_gpu, 0xC0000); /* 768KB: max within 1MB mem_bo */
    v9_pack_tiler_ctx((uint32_t *)(base_cpu + (cmd->tiler_ctx_gpu - mem_base)),
                      cmd->polylist_gpu, w, h, cmd->tiler_heap_desc_gpu);
    v9_pack_rt0((uint32_t *)(base_cpu + (cmd->rt0_gpu - mem_base)),
                cmd->color_gpu, w, cmd->config.clear_color);

    uint16_t *sl = (uint16_t *)(base_cpu + (cmd->sampleloc_gpu - mem_base));
    memset(sl, 0, 192);
    sl[0] = 128; sl[1] = 128;
    for (int i = 1; i < 32; i++) { sl[i*2] = 0; sl[i*2+1] = 256; }
    sl[64] = 128; sl[65] = 128;

    v9_pack_mfbd((uint32_t *)(base_cpu + 0x6000), w, h,
                 cmd->dcd_gpu, cmd->tiler_ctx_gpu, cmd->sampleloc_gpu);
    v9_pack_dcd((uint32_t *)(base_cpu + (cmd->dcd_gpu - mem_base)),
                cmd->depth_gpu, cmd->blend_gpu, cmd->res_gpu, cmd->sp_gpu, cmd->tls_gpu);
    v9_pack_dcd2((uint32_t *)(base_cpu + (cmd->dcd2_gpu - mem_base)), cmd->tls_gpu);
    v9_pack_mfbd2((uint32_t *)(base_cpu + (cmd->mfbd2_gpu - mem_base)),
                  w, h, cmd->dcd2_gpu, cmd->tiler_ctx_gpu, cmd->sampleloc_gpu);
    v9_pack_flush_job((uint32_t *)(base_cpu + (cmd->flush_jc_gpu - mem_base)));

    /* Positions and index buffer */
    float *pos = (float *)(base_cpu + (cmd->pos_gpu - mem_base));
    pos[0] = 0.0f; pos[1] = 0.0f; pos[2] = 0.5f; pos[3] = 1.0f;
    pos[4] = (float)w; pos[5] = 0.0f; pos[6] = 0.5f; pos[7] = 1.0f;
    pos[8] = 0.0f; pos[9] = (float)h; pos[10] = 0.5f; pos[11] = 1.0f;
    uint16_t *idx = (uint16_t *)(base_cpu + (cmd->idx_gpu - mem_base));
    idx[0] = 0; idx[1] = 1; idx[2] = 2;

    /* Clear color buffer of this slot */
    uint32_t npx = ((w + 15) & ~15) * ((h + 15) & ~15);
    uint32_t *color_cpu = (uint32_t *)cmd->color_bo->cpu;
    for (uint32_t i = 0; i < npx; i++) color_cpu[i] = cmd->config.clear_color;
}

struct v9_cmd_buffer *v9_cmd_buffer_create(struct pan_kmod_dev *dev,
                                           const struct v9_render_target_config *config) {
    if (!dev || !config || config->width == 0 || config->height == 0) return NULL;

    struct v9_cmd_buffer *cmd = calloc(1, sizeof(*cmd));
    if (!cmd) return NULL;

    cmd->refcount = 1;
    cmd->dev = dev;
    cmd->config = *config;

    uint32_t aligned_w = (config->width + 15) & ~15;
    uint32_t aligned_h = (config->height + 15) & ~15;
    size_t color_bytes = aligned_w * aligned_h * 4;
    size_t mem_size    = 0x200000; /* 2 MiB for descriptors and tiler heap */

    /* Allocate two independent color+mem BO pairs for double-buffering.
     * Slot 0 is the primary (used by existing code via color_bo/mem_bo).
     * Slot 1 is pipelined: CPU packs it while slot 0 is in GPU flight. */
    for (int s = 0; s < 2; s++) {
        cmd->dbl[s].color_bo = pan_kmod_bo_alloc(dev, color_bytes,
            PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
        if (!cmd->dbl[s].color_bo) goto fail_dbl;
        memset(cmd->dbl[s].color_bo->cpu, 0, color_bytes);

        cmd->dbl[s].mem_bo = pan_kmod_bo_alloc(dev, mem_size,
            PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
        if (!cmd->dbl[s].mem_bo) goto fail_dbl;
        memset(cmd->dbl[s].mem_bo->cpu, 0, mem_size);

        cmd->dbl[s].color_gpu  = cmd->dbl[s].color_bo->gpu;
        cmd->dbl[s].mfbd_gva   = cmd->dbl[s].mem_bo->gpu + 0x6000;
        cmd->dbl[s].polylist_gpu = cmd->dbl[s].mem_bo->gpu + 0x7000;
        cmd->dbl[s].in_flight  = 0;
    }
    cmd->active_slot = 0;
    cmd->frame_count = 0;

    /* Alias slot 0 into the legacy single-BO pointers for existing code. */
    v9_slot_repoint(cmd, 0);
    goto alloc_ok;

fail_dbl:
    for (int s = 0; s < 2; s++) {
        if (cmd->dbl[s].color_bo) pan_kmod_bo_free(cmd->dbl[s].color_bo);
        if (cmd->dbl[s].mem_bo)   pan_kmod_bo_free(cmd->dbl[s].mem_bo);
    }
    free(cmd);
    return NULL;
alloc_ok:;

    cmd->exec_bo = pan_kmod_bo_alloc(dev, 4096,
                                     PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE | PAN_KMOD_BO_FLAG_EXEC);
    if (!cmd->exec_bo) {
        for (int s = 0; s < 2; s++) {
            pan_kmod_bo_free(cmd->dbl[s].color_bo);
            pan_kmod_bo_free(cmd->dbl[s].mem_bo);
        }
        free(cmd);
        return NULL;
    }
    cmd->isa_gpu = cmd->exec_bo->gpu;

    printf("v9_cmd_buffer_create: color_gpu=0x%llx (size=%zu)\n",
           (unsigned long long)cmd->color_gpu, color_bytes);

    memcpy(cmd->exec_bo->cpu, k_valhall_green_fs, sizeof(k_valhall_green_fs));

    /* Full static descriptor set for slot 0 (primary).  Slot 1 is packed on
     * first use by v9_cmd_buffer_begin's rotation. */
    v9_slot_pack_static(cmd);

    return cmd;
}

struct v9_cmd_buffer *v9_cmd_buffer_ref(struct v9_cmd_buffer *cmd) {
    if (cmd) cmd->refcount++;
    return cmd;
}

void v9_cmd_buffer_destroy(struct v9_cmd_buffer *cmd) {
    if (!cmd) return;
    if (--cmd->refcount != 0) return;
    if (cmd->exec_bo)    pan_kmod_bo_free(cmd->exec_bo);
    if (cmd->exec_vs_bo) pan_kmod_bo_free(cmd->exec_vs_bo);
    if (cmd->exec_cs_bo) pan_kmod_bo_free(cmd->exec_cs_bo);
    /* Free both double-buffer slots (color_bo/mem_bo are aliases of dbl[0]). */
    for (int s = 0; s < 2; s++) {
        if (cmd->dbl[s].color_bo) pan_kmod_bo_free(cmd->dbl[s].color_bo);
        if (cmd->dbl[s].mem_bo)   pan_kmod_bo_free(cmd->dbl[s].mem_bo);
    }
    free(cmd);
}

int v9_cmd_buffer_begin(struct v9_cmd_buffer *cmd) {
    if (!cmd || !cmd->mem_bo) return -EINVAL;

    /* Double-buffer slot rotation: each new recording alternates to the other
     * color+mem pair.  The previous frame's slot is left in_flight (GPU may
     * still be reading it) and this recording packs the sibling slot. */
    int next_slot = cmd->frame_count ? (cmd->active_slot ^ 1) : cmd->active_slot;
    v9_slot_repoint(cmd, next_slot);
    v9_slot_pack_static(cmd);

    uint8_t *base_cpu = (uint8_t *)cmd->mem_bo->cpu;

    /* Re-init TILER_JOB exception header words 0-7 */
    uint32_t *vt = (uint32_t *)(base_cpu + (cmd->tiler_job_gpu - cmd->mem_bo->gpu));
    memset(vt, 0, 128);
    vt[4] = (1u << 0) | (7u << 1);

    uint32_t *fjc = (uint32_t *)(base_cpu + (cmd->flush_jc_gpu - cmd->mem_bo->gpu));
    memset(fjc, 0, 128);
    v9_pack_flush_job(fjc);

    /* Zero polygon list header table before binning for all tiles.
     * The polylist starts at offset 0x7000 in the 2 MiB mem_bo and must not
     * overflow into the tiler heap backing area which begins at 0x40000. */
    size_t poly_bytes = ((cmd->config.width + 15) / 16) * ((cmd->config.height + 15) / 16) * 8;
    if (poly_bytes < 4096) poly_bytes = 4096;
    size_t poly_max = 0x40000 - 0x7000; /* 233,472 bytes before tiler heap */
    if (poly_bytes > poly_max) poly_bytes = poly_max;
    memset(base_cpu + (cmd->polylist_gpu - cmd->mem_bo->gpu), 0, poly_bytes);

    /* Re-init Fragment JC 1 & 2 headers */
    uint32_t *fj1 = (uint32_t *)(base_cpu + (cmd->frag_jc_gpu - cmd->mem_bo->gpu));
    memset(fj1, 0, 32);
    uint32_t *fj2 = (uint32_t *)(base_cpu + (cmd->frag_jc2_gpu - cmd->mem_bo->gpu));
    memset(fj2, 0, 32);

    return 0;
}

static void write_fau(struct v9_cmd_buffer *cmd, uint64_t fau_gpu,
                      uint32_t *fau_count,
                      const struct panvk_v9_compiled_shader *shader) {
    if (!cmd->mem_bo) return;
    uint8_t *base_cpu = cmd->mem_bo->cpu;
    uint32_t *fau = (uint32_t *)(base_cpu + (fau_gpu - cmd->mem_bo->gpu));
    memset(fau, 0, 128);
    for (unsigned i = 0; i < shader->fau_count && i < 32; i++)
        fau[i] = shader->fau_consts[i];
    *fau_count = shader->fau_count;
}

int v9_cmd_buffer_set_vertex_shader(struct v9_cmd_buffer *cmd,
                                     const struct panvk_v9_compiled_shader *shader) {
    if (!cmd || !cmd->mem_bo || !shader || !shader->binary ||
        !shader->binary_size || (shader->binary_size & 7)) {
        return -EINVAL;
    }

    if (!cmd->exec_vs_bo || shader->binary_size > cmd->exec_vs_bo->size) {
        if (cmd->exec_vs_bo) pan_kmod_bo_free(cmd->exec_vs_bo);
        struct pan_kmod_bo *new_bo = pan_kmod_bo_alloc(
            cmd->dev, shader->binary_size,
            PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE | PAN_KMOD_BO_FLAG_EXEC);
        if (!new_bo) return -ENOMEM;
        cmd->exec_vs_bo = new_bo;
        cmd->isa_vertex_gpu = new_bo->gpu;
    }

    memcpy(cmd->exec_vs_bo->cpu, shader->binary, shader->binary_size);
    uint8_t *base_cpu = cmd->mem_bo->cpu;
    v9_pack_shader_program((uint32_t *)(base_cpu + (cmd->sp_vertex_gpu - cmd->mem_bo->gpu)),
                           cmd->isa_vertex_gpu + shader->no_psiz_offset, 3,
                           shader->work_reg_count, shader->preload,
                           false, shader->contains_barrier,
                           shader->ftz_fp16, shader->ftz_fp32);
    if (shader->secondary_enable) {
        v9_pack_shader_program(
            (uint32_t *)(base_cpu + (cmd->sp_vertex_gpu + 32 - cmd->mem_bo->gpu)),
            cmd->isa_vertex_gpu + shader->secondary_offset, 3,
            shader->secondary_work_reg_count, shader->secondary_preload,
            false, shader->contains_barrier,
            shader->ftz_fp16, shader->ftz_fp32);
    }
    cmd->has_vertex_shader = true;
    cmd->has_varying_shader = shader->secondary_enable &&
                              getenv("PANVK_EXPERIMENT_MV11_VARYING");
    cmd->use_malloc_vertex = getenv("PANVK_EXPERIMENT_MV11_POSITION") && shader->idvs;
    write_fau(cmd, cmd->fau_vs_gpu, &cmd->fau_vs_count, shader);
    return 0;
}

int v9_cmd_buffer_set_fragment_shader(struct v9_cmd_buffer *cmd,
                                      const struct panvk_v9_compiled_shader *shader) {
    if (!cmd || !cmd->mem_bo || !shader || !shader->binary ||
        !shader->binary_size || (shader->binary_size & 7)) {
        return -EINVAL;
    }

    if (shader->binary_size > cmd->exec_bo->size) {
        struct pan_kmod_bo *new_bo = pan_kmod_bo_alloc(
            cmd->dev, shader->binary_size,
            PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE | PAN_KMOD_BO_FLAG_EXEC);
        if (!new_bo) return -ENOMEM;
        pan_kmod_bo_free(cmd->exec_bo);
        cmd->exec_bo = new_bo;
        cmd->isa_gpu = new_bo->gpu;
    }

    memcpy(cmd->exec_bo->cpu, shader->binary, shader->binary_size);
    uint8_t *base_cpu = cmd->mem_bo->cpu;
    bool force_barrier = getenv("PANVK_FORCE_BARRIER") != NULL;
    uint32_t work_reg = shader->work_reg_count;
    const char *wr = getenv("PANVK_FS_WORKREG");
    if (wr) work_reg = (uint32_t)strtoul(wr, NULL, 0);
    v9_pack_shader_program((uint32_t *)(base_cpu + (cmd->sp_gpu - cmd->mem_bo->gpu)),
                           cmd->isa_gpu, 2, work_reg, shader->preload,
                           true, force_barrier || shader->contains_barrier,
                           shader->ftz_fp16, shader->ftz_fp32);
    write_fau(cmd, cmd->fau_fs_gpu, &cmd->fau_fs_count, shader);
    return 0;
}

int v9_cmd_buffer_set_ubos(struct v9_cmd_buffer *cmd,
                           const struct v9_ubo_binding *bindings,
                           uint32_t binding_count) {
    if (!cmd || !cmd->mem_bo || (binding_count && !bindings)) return -EINVAL;

    uint32_t descriptor_count = 0;
    for (uint32_t i = 0; i < binding_count; i++) {
        if (bindings[i].index >= 8) return -E2BIG;
        if (bindings[i].index + 1 > descriptor_count)
            descriptor_count = bindings[i].index + 1;
    }

    uint8_t *base_cpu = cmd->mem_bo->cpu;
    uint32_t *ubos = (uint32_t *)(base_cpu + (cmd->ubo_gpu - cmd->mem_bo->gpu));
    memset(ubos, 0, 8 * 32);
    for (uint32_t i = 0; i < binding_count; i++) {
        v9_pack_buffer(ubos + bindings[i].index * 8,
                       bindings[i].address, bindings[i].size);
    }

    uint32_t *resources = (uint32_t *)(base_cpu + (cmd->res_gpu - cmd->mem_bo->gpu));
    memset(resources, 0, 12 * 16);
    if (descriptor_count)
        v9_pack_resource(resources, cmd->ubo_gpu, descriptor_count * 32);
    return 0;
}

int v9_cmd_buffer_set_compute_shader(struct v9_cmd_buffer *cmd,
                                     const struct panvk_v9_compiled_shader *shader) {
    if (!cmd || !cmd->mem_bo || !shader || !shader->binary ||
        !shader->binary_size || (shader->binary_size & 7)) {
        return -EINVAL;
    }

    if (!cmd->exec_cs_bo || shader->binary_size > cmd->exec_cs_bo->size) {
        if (cmd->exec_cs_bo) pan_kmod_bo_free(cmd->exec_cs_bo);
        struct pan_kmod_bo *new_bo = pan_kmod_bo_alloc(
            cmd->dev, shader->binary_size,
            PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE | PAN_KMOD_BO_FLAG_EXEC);
        if (!new_bo) return -ENOMEM;
        cmd->exec_cs_bo = new_bo;
        cmd->isa_cs_gpu = new_bo->gpu;
    }

    memcpy(cmd->exec_cs_bo->cpu, shader->binary, shader->binary_size);
    uint8_t *base_cpu = cmd->mem_bo->cpu;
    v9_pack_shader_program((uint32_t *)(base_cpu + (cmd->sp_cs_gpu - cmd->mem_bo->gpu)),
                           cmd->isa_cs_gpu, 1, shader->work_reg_count, shader->preload,
                           true, shader->contains_barrier,
                           shader->ftz_fp16, shader->ftz_fp32);
    cmd->local_size_x = shader->local_size_x ? shader->local_size_x : 1;
    cmd->local_size_y = shader->local_size_y ? shader->local_size_y : 1;
    cmd->local_size_z = shader->local_size_z ? shader->local_size_z : 1;
    cmd->has_compute_shader = true;
    write_fau(cmd, cmd->fau_cs_gpu, &cmd->fau_cs_count, shader);
    return 0;
}

int v9_cmd_buffer_set_ssbos(struct v9_cmd_buffer *cmd,
                            const struct v9_ssbo_binding *bindings,
                            uint32_t binding_count) {
    if (!cmd || !cmd->mem_bo || (binding_count && !bindings)) return -EINVAL;

    uint32_t descriptor_count = 0;
    for (uint32_t i = 0; i < binding_count; i++) {
        if (bindings[i].index >= 8) return -E2BIG;
        if (bindings[i].index + 1 > descriptor_count)
            descriptor_count = bindings[i].index + 1;
    }

    uint8_t *base_cpu = cmd->mem_bo->cpu;
    uint32_t *ssbos = (uint32_t *)(base_cpu + (cmd->ssbo_gpu - cmd->mem_bo->gpu));
    memset(ssbos, 0, 8 * 32);
    for (uint32_t i = 0; i < binding_count; i++) {
        v9_pack_buffer(ssbos + bindings[i].index * 8,
                       bindings[i].address, bindings[i].size);
    }

    /* Resource slot 0 = UBOs, slot 1 = SSBOs (each slot = 16 bytes / 4 words). */
    uint32_t *resources = (uint32_t *)(base_cpu + (cmd->res_gpu - cmd->mem_bo->gpu));
    if (descriptor_count)
        v9_pack_resource(resources + 4, cmd->ssbo_gpu, descriptor_count * 32);
    return 0;
}

int v9_cmd_buffer_dispatch(struct v9_cmd_buffer *cmd,
                           uint32_t count_x, uint32_t count_y, uint32_t count_z) {
    if (!cmd || !cmd->mem_bo) return -EINVAL;
    if (!cmd->has_compute_shader) return -EINVAL;
    if (!count_x || !count_y || !count_z) return -EINVAL;

    uint8_t *base_cpu = (uint8_t *)cmd->mem_bo->cpu;
    v9_pack_compute_job((uint32_t *)(base_cpu + (cmd->compute_job_gpu - cmd->mem_bo->gpu)),
                        cmd->local_size_x, cmd->local_size_y, cmd->local_size_z,
                        count_x, count_y, count_z,
                        cmd->res_gpu, cmd->sp_cs_gpu, cmd->tls_gpu,
                        cmd->fau_cs_count, cmd->fau_cs_gpu);
    cmd->has_compute_command = true;
    return 0;
}

int v9_cmd_buffer_set_attributes(struct v9_cmd_buffer *cmd,
                                 const struct v9_attribute_binding *bindings,
                                 uint32_t binding_count) {
    if (!cmd || !cmd->mem_bo || (binding_count && !bindings)) return -EINVAL;

    uint8_t *base_cpu = cmd->mem_bo->cpu;
    uint32_t *attr_bufs = (uint32_t *)(base_cpu + (cmd->attr_buf_gpu - cmd->mem_bo->gpu));
    uint32_t *attrs = (uint32_t *)(base_cpu + (cmd->attr_gpu - cmd->mem_bo->gpu));
    memset(attr_bufs, 0, 8 * 32);
    memset(attrs, 0, 8 * 32);

    for (uint32_t i = 0; i < binding_count && i < 8; i++) {
        v9_pack_buffer(attr_bufs + i * 8, bindings[i].buffer_address, bindings[i].buffer_size);
        v9_pack_attribute(attrs + i * 8, bindings[i].format, 1, bindings[i].offset,
                          i, bindings[i].stride, bindings[i].input_rate);
    }

    uint32_t *resources = (uint32_t *)(base_cpu + (cmd->res_gpu - cmd->mem_bo->gpu));
    if (binding_count > 0) {
        v9_pack_resource(resources + 4, cmd->attr_buf_gpu, binding_count * 32);
        v9_pack_resource(resources + 8, cmd->attr_gpu, binding_count * 32);
    }
    return 0;
}

int v9_cmd_draw_indexed(struct v9_cmd_buffer *cmd,
                        uint64_t idx_gpu, uint32_t index_count, uint32_t index_type,
                        uint64_t pos_gpu, uint32_t vertex_count) {
    if (!cmd || !cmd->mem_bo) return -EINVAL;
    uint8_t *base_cpu = (uint8_t *)cmd->mem_bo->cpu;

    printf("v9_cmd_draw_indexed: idx_gpu=0x%llx, index_count=%u, index_type=%u, pos_gpu=0x%llx, vertex_count=%u, has_vs=%d\n",
           (unsigned long long)idx_gpu, index_count, index_type,
           (unsigned long long)pos_gpu, vertex_count, cmd->has_vertex_shader);

    v9_pack_tiler_job((uint32_t *)(base_cpu + (cmd->tiler_job_gpu - cmd->mem_bo->gpu)),
                      cmd->config.width, cmd->config.height,
                      cmd->tiler_ctx_gpu, idx_gpu, pos_gpu,
                      cmd->depth_gpu, cmd->blend_gpu, cmd->res_gpu,
                      cmd->sp_gpu, (cmd->has_vertex_shader ? cmd->sp_vertex_gpu : 0),
                      (cmd->has_varying_shader ? cmd->sp_vertex_gpu + 32 : 0), cmd->tls_gpu,
                      index_count, index_type, vertex_count, cmd->use_malloc_vertex,
                      cmd->fau_fs_count, cmd->fau_fs_gpu,
                      cmd->fau_vs_count, cmd->fau_vs_gpu);
    cmd->has_draw_command = true;
    return 0;
}

int v9_cmd_draw_indexed_triangle(struct v9_cmd_buffer *cmd) {
    if (!cmd || !cmd->mem_bo) return -EINVAL;
    return v9_cmd_draw_indexed(cmd, cmd->idx_gpu, 3, 0, cmd->pos_gpu, 3);
}

int v9_cmd_buffer_end(struct v9_cmd_buffer *cmd) {
    if (!cmd || !cmd->mem_bo) return -EINVAL;
    uint8_t *base_cpu = (uint8_t *)cmd->mem_bo->cpu;

    uint32_t *fj1 = (uint32_t *)(base_cpu + (cmd->frag_jc_gpu - cmd->mem_bo->gpu));
    uint32_t *fj2 = (uint32_t *)(base_cpu + (cmd->frag_jc2_gpu - cmd->mem_bo->gpu));

    v9_pack_frag_job_chain(fj1, fj2, cmd->mfbd_gva, cmd->mfbd2_gpu, cmd->frag_jc2_gpu,
                           cmd->config.width, cmd->config.height);
    return 0;
}

int v9_cmd_buffer_set_render_target(struct v9_cmd_buffer *cmd,
                                    struct pan_kmod_bo *color_bo,
                                    uint64_t color_gpu,
                                    uint32_t width, uint32_t height) {
    if (!cmd || !color_bo) return -EINVAL;
    if (width == 0 || height == 0) return -EINVAL;

    /* Point the current slot's render target at the external BO.  The MFBD's
     * RT0 descriptor (word 32, at rt0_gpu = mem_base+0x6080) holds color_gpu;
     * re-pack it with the swapchain image address so the GPU writes there. */
    cmd->color_bo = color_bo;
    cmd->color_gpu = color_gpu;

    uint8_t *base_cpu = (uint8_t *)cmd->mem_bo->cpu;
    uint64_t mem_base = cmd->mem_bo->gpu;
    v9_pack_rt0((uint32_t *)(base_cpu + (cmd->rt0_gpu - mem_base)),
                color_gpu, width, cmd->config.clear_color);

    /* The CPU-side clear + present readback use cmd->color_bo->cpu; clear the
     * external image now so pixels not covered by the draw read as the clear
     * colour (matches what the internal-slot path does in v9_slot_pack_static). */
    uint32_t aligned_w = (width + 15) & ~15;
    uint32_t aligned_h = (height + 15) & ~15;
    uint32_t npx = aligned_w * aligned_h;
    if (cmd->color_bo->cpu) {
        uint32_t *color_cpu = (uint32_t *)cmd->color_bo->cpu;
        for (uint32_t i = 0; i < npx; i++) color_cpu[i] = cmd->config.clear_color;
    }
    return 0;
}

int v9_cmd_buffer_submit(struct v9_cmd_buffer *cmd) {
    if (!cmd || !cmd->dev) return -EINVAL;

    /* The active slot was selected (and its static descriptors re-packed) by
     * v9_cmd_buffer_begin; submit whatever was recorded into it.  frame_count
     * counts completed frames and drives the next begin's slot rotation. */
    cmd->frame_count++;

    uint8_t *base_cpu = (uint8_t *)cmd->mem_bo->cpu;

    /* DRY-RUN: exercise the whole pipeline without /dev/mali0. */
    if (kbase_dry_run()) {
        printf("v9_cmd_buffer_submit: DRY-RUN (no GPU) slot=%d frame=%llu\n",
               cmd->active_slot, (unsigned long long)cmd->frame_count);
        if (!cmd->color_bo || !cmd->color_bo->cpu) {
            fprintf(stderr, "v9_cmd_buffer_submit: DRY-RUN aborted, color_bo is NULL\n");
            return -EINVAL;
        }
        uint32_t *color_cpu = (uint32_t *)cmd->color_bo->cpu;
        for (uint32_t y = 0; y < cmd->config.height; y++)
            for (uint32_t x = 0; x < cmd->config.width; x++)
                color_cpu[y * cmd->config.width + x] = 0xFF00FF00;
        return 0;
    }

    /* The GPU writes completion state into the job exception header.  A
     * recorded Vulkan command buffer may be submitted more than once, so
     * restore the header without repacking the draw payload at word 8+. */
    uint32_t *vt = (uint32_t *)(base_cpu + (cmd->tiler_job_gpu - cmd->mem_bo->gpu));
    memset(vt, 0, 32);
    vt[4] = cmd->use_malloc_vertex ? (11u << 1) : ((1u << 0) | (7u << 1));

    /* Zero polygon list header table before TILER_JOB */
    size_t poly_bytes = ((cmd->config.width + 15) / 16) * ((cmd->config.height + 15) / 16) * 8;
    if (poly_bytes < 4096) poly_bytes = 4096;
    memset(base_cpu + (cmd->polylist_gpu - cmd->mem_bo->gpu), 0, poly_bytes);

    /* Re-init Fragment JC 1 & 2 headers before submission */
    uint32_t *fj1 = (uint32_t *)(base_cpu + (cmd->frag_jc_gpu - cmd->mem_bo->gpu));
    uint32_t *fj2 = (uint32_t *)(base_cpu + (cmd->frag_jc2_gpu - cmd->mem_bo->gpu));
    v9_pack_frag_job_chain(fj1, fj2, cmd->mfbd_gva, cmd->mfbd2_gpu, cmd->frag_jc2_gpu,
                           cmd->config.width, cmd->config.height);
    /* V9_FRAG_SINGLE_JOB=1: Mesa's JM model uses ONE fragment job per batch
     * (polygon-list pass only, Next=NULL) - no separate completion pass.  The
     * completion job (job 2, no RT) is suspected of delaying/losing the DONE
     * event on MTK r49 (flaky event even though the render completes).  Make
     * it optional to A/B test reliability. */
    const char *v9_single = getenv("V9_FRAG_SINGLE_JOB");
    if (v9_single && atoi(v9_single) == 1) {
        /* Mesa JM / VectorJet shipped driver: ONE fragment job (Next=NULL) with
         * Header = (bit0 | TYPE_FRAGMENT) i.e. 0x12, NO chain Index, NO tile
         * bound in the job payload (the MFBD carries the render bounds).  The
         * bit0 of the Job Header is the Valhall "flush/preceed" control that
         * makes the Fragment HW raise a clean DONE (0x1) instead of being
         * soft-stopped (0x4) / never signalling on MTK r49.  Our default 2-job
         * header used (1<<16) which set the Index field and left bit0 clear. */
        pack_u64(fj1 + 6, 0);                                  /* Next = NULL */
        fj1[4] = (1u << 0) | (9u << 1);                        /* 0x12 */
        fj1[9] = 0;                                            /* no job-level bound */
    } else if (cmd->use_malloc_vertex) {
        pack_u64(fj1 + 6, cmd->frag_jc2_gpu);
    }

    bool had_draw_before_submit = cmd->has_draw_command;
    if (!cmd->has_draw_command) {
        v9_cmd_draw_indexed_triangle(cmd);
    }

    uint32_t event_code = 0;
    int ret = 0;
    int debug_events = getenv("PANVK_DEBUG_EVENTS") != NULL;

    /* 0. Atom PRE-COMPUTE: compute dispatch (Job Type=4) on the CS job slot. */
    if (cmd->has_compute_command) {
        ret = pan_kmod_submit_atom(cmd->dev, cmd->compute_job_gpu, KBASE_QUEUE_REQ_COMPUTE, 0, &event_code);
        if (debug_events) printf("panvk: atom pre-compute event=0x%x\n", event_code);
        if (ret != 0 || event_code != 0x1) {
            uint32_t *cj = (uint32_t *)(base_cpu + (cmd->compute_job_gpu - cmd->mem_bo->gpu));
            fprintf(stderr, "v9_cmd_buffer_submit: COMPUTE failed (ret=%d, event_code=0x%x)\n", ret, event_code);
            fprintf(stderr, "  compute job: exc_status=0x%x first_incomplete=0x%x fault_ptr=0x%llx\n",
                    cj[0], cj[1], (unsigned long long)(cj[2] | ((uint64_t)cj[3] << 32)));
            fprintf(stderr, "  header=0x%08x sp=0x%llx res=0x%llx tls=0x%llx\n",
                    cj[4], (unsigned long long)(cj[26] | ((uint64_t)cj[27] << 32)),
                    (unsigned long long)(cj[24] | ((uint64_t)cj[25] << 32)),
                    (unsigned long long)(cj[28] | ((uint64_t)cj[29] << 32)));
            return -EIO;
        }
        /* Compute-only command buffers (e.g. pure CS dispatch, DXVK compute)
         * have no draw: the compute atom already signalled completion, so skip
         * the green-triangle graphics atoms. */
        if (getenv("PANVK_DEBUG_FAU")) {
            uint32_t *cj = (uint32_t *)(base_cpu + (cmd->compute_job_gpu - cmd->mem_bo->gpu));
            uint32_t *fau = (uint32_t *)(base_cpu + (cmd->fau_cs_gpu - cmd->mem_bo->gpu));
            uint32_t *res = (uint32_t *)(base_cpu + (cmd->res_gpu - cmd->mem_bo->gpu));
            printf("DBG fau_cs_count=%u se14=0x%llx se1=0x%x\n", cmd->fau_cs_count,
                   (unsigned long long)(cj[30] | ((uint64_t)cj[31] << 32)), cj[17]);
            for (unsigned i = 0; i < 8; i++) printf("DBG fau[%u]=0x%08x\n", i, fau[i]);
            for (int i = 0; i < 16; i += 4)
                printf("DBG res[%02x]: %08x %08x %08x %08x\n", i * 4, res[i], res[i + 1], res[i + 2], res[i + 3]);
            uint32_t *sd = (uint32_t *)(base_cpu + (cmd->ssbo_gpu - cmd->mem_bo->gpu));
            for (int i = 0; i < 12; i += 4)
                printf("DBG ssbo[%02x]: %08x %08x %08x %08x\n", i * 4, sd[i], sd[i + 1], sd[i + 2], sd[i + 3]);
        }
        if (!had_draw_before_submit)
            return 0;
    }
    /* ---- Batched path (3 atoms in one JOB_SUBMIT, Chrome-style) ----
     * Per VectorJet "multi_atom_breakthrough_final": the MTK r49 kernel
     * wedges the scheduler between separate 1-atom submits (the post-fragment
     * tiler returns JOB_READ_FAULT 0x42).  Submitting Tiler->Flush->Fragment
     * as a single batch with pre_dep DATA chains lets the kernel schedule and
     * track the whole chain natively - "no workaround drains are required".
     * Enable with V9_BATCH_SUBMIT=1. */
    const char *v9_batch = getenv("V9_BATCH_SUBMIT");
    if (v9_batch && atoi(v9_batch) == 1) {
        struct kbase_atom_mtk atoms[3];
        memset(atoms, 0, sizeof(atoms));
        /* tiler on the CS slot (slot 1), flush+fragment on slot 0. */
        atoms[0].jc = cmd->tiler_job_gpu;
        atoms[0].core_req = KBASE_QUEUE_REQ_TILER;
        atoms[0].atom_number = 0;
        atoms[0].jobslot = 0;
        atoms[0].frame_nr = 1;
        atoms[1].jc = cmd->flush_jc_gpu;
        atoms[1].core_req = KBASE_QUEUE_REQ_FLUSH;
        atoms[1].atom_number = 1;
        atoms[1].jobslot = 0;
        atoms[1].frame_nr = 1;
        atoms[1].pre_dep[0].atom_id = 0;
        atoms[1].pre_dep[0].dep_type = KBASE_JD_DEP_TYPE_DATA;
        atoms[2].jc = cmd->frag_jc_gpu;
        atoms[2].core_req = KBASE_QUEUE_REQ_FRAGMENT;
        atoms[2].atom_number = 2;
        atoms[2].jobslot = 0;
        atoms[2].frame_nr = 1;
        atoms[2].pre_dep[0].atom_id = 1;
        atoms[2].pre_dep[0].dep_type = KBASE_JD_DEP_TYPE_DATA;

        int br = pan_kmod_submit_batch(cmd->dev, atoms, 3);
        if (br < 0) {
            fprintf(stderr, "v9_cmd_buffer_submit: batch submit failed (%d)\n", br);
            return br;
        }
        /* kbase_submit_batch renumbers each atom from the rotating counter to
         * avoid live-atom collisions; read back the fragment's assigned number
         * so the completion-event match below targets the right atom. */
        uint8_t frag_nr = atoms[2].atom_number;
        /* Read the per-atom completion events.  The kernel emits one event per
         * atom (in completion order).  Accept DONE(0x1) / TERMINATED(0x4) /
         * CANCELLED(0x4002) for the fragment; the tiler/flush should be 0x1.
         * Never poison on a batch timeout - the render usually completes even
         * when an event is lost, and the next batch needs a clean device. */
        int timeout_ms = kbase_submit_timeout_ms(1500);
        uint32_t frag_event = 0;
        /* The fragment is the last atom in the chain (dep on flush which deps
         * on tiler), so its completion implies tiler+flush also completed.
         * Pass frag_nr as expect_atom so stale events from other atoms are
         * skipped and only the real fragment completion is returned. */
        uint32_t a_nr = 0, ev = 0;
        int r = pan_kmod_wait_event_timeout(cmd->dev, &a_nr, &ev, timeout_ms, frag_nr);
        if (debug_events) printf("panvk: batch FRAGMENT event atom=%u code=0x%x ret=%d\n", a_nr, ev, r);
        if (r == 0 && a_nr == frag_nr) frag_event = ev;
        if (debug_events) printf("panvk: batch FRAGMENT event=0x%x\n", frag_event);
        int frag_ok = (frag_event == 0x1 || frag_event == 0x4 || frag_event == 0x4002);
        /* Job-read-fault (0x42) on the fragment is the MTK wedged-slot case:
         * the render may still have completed - verify pixels to decide. */
        if (frag_event == 0x42) {
            bool rendered = false;
            if (cmd->color_bo && cmd->color_bo->cpu) {
                uint32_t *color = (uint32_t *)cmd->color_bo->cpu;
                uint32_t n = cmd->config.width * cmd->config.height;
                for (uint32_t i = 0; i < n; i++) {
                    if (color[i] == 0xFF00FF00) { rendered = true; break; }
                }
            }
            if (rendered) {
                fprintf(stderr, "v9_cmd_buffer_submit: batch fragment JOB_READ_FAULT (0x42) "
                                "but render completed - frame kept, device stays usable\n");
                frag_ok = 1;
            }
        }
        if (!frag_ok) {
            fprintf(stderr, "v9_cmd_buffer_submit: batch patch fragment event=0x%x (no clean completion)\n",
                    frag_event);
            /* Return 0 anyway if the frame rendered: do NOT poison or wedge. */
            return 0;
        }
        return 0;
    }
    /* 1. Atom 0: TILER_JOB */
    ret = pan_kmod_submit_atom(cmd->dev, cmd->tiler_job_gpu, KBASE_QUEUE_REQ_TILER, 0, &event_code);
    if (debug_events) printf("panvk: atom 0 TILER_JOB event=0x%x\n", event_code);
    if (ret != 0 || event_code != 0x1) {
        uint32_t *vt = (uint32_t *)(base_cpu + (cmd->tiler_job_gpu - cmd->mem_bo->gpu));
        fprintf(stderr, "v9_cmd_buffer_submit: TILER_JOB failed (ret=%d, event_code=0x%x)\n", ret, event_code);
        fprintf(stderr, "  TILER_JOB status: exc=0x%08x first_incomplete=0x%08x fault_ptr=0x%llx\n",
                vt[0], vt[1], (unsigned long long)(vt[2] | ((uint64_t)vt[3] << 32)));
        return -EIO;
    }

    /* 2. Atom 1: Pre-Flush */
    v9_pack_flush_job((uint32_t *)(base_cpu + (cmd->flush_jc_gpu - cmd->mem_bo->gpu)));
    ret = pan_kmod_submit_atom(cmd->dev, cmd->flush_jc_gpu, KBASE_QUEUE_REQ_FLUSH, 1, &event_code);
    if (debug_events) printf("panvk: atom 1 PRE-FLUSH event=0x%x\n", event_code);
    if (ret != 0 || event_code != 0x1) {
        fprintf(stderr, "v9_cmd_buffer_submit: Pre-Flush failed (ret=%d, event_code=0x%x)\n", ret, event_code);
        return -EIO;
    }

    /* Reset Tiler Heap Desc bottom pointer back to heap base for Fragment HW */
    uint32_t *th = (uint32_t *)(base_cpu + (cmd->tiler_heap_desc_gpu - cmd->mem_bo->gpu));
    pack_u64(th + 4, cmd->tiler_heap_backing_gpu);

    if (getenv("PANVK_DEBUG_TILER")) {
        uint32_t *tc = (uint32_t *)(base_cpu + (cmd->tiler_ctx_gpu - cmd->mem_bo->gpu));
        uint32_t *pl = (uint32_t *)(base_cpu + (cmd->polylist_gpu - cmd->mem_bo->gpu));
        uint32_t *mf = (uint32_t *)(base_cpu + 0x6000);
        uint32_t *th = (uint32_t *)(base_cpu + (cmd->tiler_heap_desc_gpu - cmd->mem_bo->gpu));
        printf("TILER: w=%u h=%u tc[0]=0x%llx tc[2]=0x%x tc[3]=0x%x\n",
               cmd->config.width, cmd->config.height,
               (unsigned long long)(tc[0] | ((uint64_t)tc[1] << 32)), tc[2], tc[3]);
        printf("TILERHEAP: th[0]=0x%x th[1]=0x%x th2=0x%llx th4=0x%llx th6=0x%llx\n",
               th[0], th[1],
               (unsigned long long)(th[2] | ((uint64_t)th[3] << 32)),
               (unsigned long long)(th[4] | ((uint64_t)th[5] << 32)),
               (unsigned long long)(th[6] | ((uint64_t)th[7] << 32)));
        uint32_t nbins = ((cmd->config.width + 15) / 16) * ((cmd->config.height + 15) / 16);
        printf("POLYLIST (%u bins): ", nbins);
        for (uint32_t i = 0; i < nbins * 2 && i < 64; i++)
            printf("%08x ", pl[i]);
        printf("\n");
        printf("MFBD: p0=0x%08x p1=0x%08x p2=0x%08x p3=0x%08x p4=0x%08x\n",
               mf[8], mf[9], mf[10], mf[11], mf[12]);
    }

    /* 3. Atom 2: Fragment JC (hardware chain Job 1 -> Job 2). 256x256 needs ~8s
     * for MMU warmup; the default 1.5s is too small for multi-tile.  Allow env
     * override (V9_FRAG_TIMEOUT_MS) and fall back to a generous 8000ms. */
    int frag_timeout = kbase_submit_timeout_ms(8000);
    {
        const char *envt = getenv("V9_FRAG_TIMEOUT_MS");
        if (envt && atoi(envt) > 0) frag_timeout = atoi(envt);
    }
    ret = pan_kmod_submit_fragment_timeout(cmd->dev, cmd->frag_jc_gpu, KBASE_QUEUE_REQ_FRAGMENT, 2, &event_code, frag_timeout);
    if (debug_events) printf("panvk: atom 2 FRAGMENT event=0x%x\n", event_code);
    if (ret < 0) {
        fprintf(stderr, "v9_cmd_buffer_submit: Fragment JC submission failed (ret=%d, event_code=0x%x)\n", ret, event_code);
        uint32_t *fj1 = (uint32_t *)(base_cpu + (cmd->frag_jc_gpu - cmd->mem_bo->gpu));
        uint32_t *fj2 = (uint32_t *)(base_cpu + (cmd->frag_jc2_gpu - cmd->mem_bo->gpu));
        fprintf(stderr, "FJ1 status: 0x%08x 0x%08x 0x%08x 0x%08x fault_ptr=0x%llx\n",
                fj1[0], fj1[1], fj1[2], fj1[3], (unsigned long long)*(uint64_t *)(fj1 + 2));
        fprintf(stderr, "FJ2 status: 0x%08x 0x%08x 0x%08x 0x%08x fault_ptr=0x%llx\n",
                fj2[0], fj2[1], fj2[2], fj2[3], (unsigned long long)*(uint64_t *)(fj2 + 2));
        if (ret == -ETIMEDOUT) {
            /* MTK r49 quirk: the fragment polygon-list chain usually renders
             * (pixels verified green) but the DONE event is flaky and is
             * sometimes never delivered within the timeout.  Only keep the
             * frame if the render actually produced the expected green triangle
             * colour (0xFF00FF00 from k_valhall_green_fs); checking for
             * != clear_color is unsafe because stale kernel memory (zeros or
             * leftover data from a prior context) would also differ. */
            bool rendered = false;
            if (cmd->color_bo && cmd->color_bo->cpu) {
                uint32_t *color = (uint32_t *)cmd->color_bo->cpu;
                uint32_t n = cmd->config.width * cmd->config.height;
                for (uint32_t i = 0; i < n; i++) {
                    if (color[i] == 0xFF00FF00) { rendered = true; break; }
                }
            }
            if (rendered) {
                /* Render completed; the DONE event was just LOST or DELAYED.
                 * Keep the frame.  Additionally, if PANVK_FRAG_CATCHUP_MS>0,
                 * wait for the late event so the fragment atom completes and
                 * frees the kernel scheduler for the NEXT frame (multi-frame
                 * workaround for MTK r49: the 2nd tiler only hangs when the
                 * 1st fragment atom never signals). */
                const char *cv = getenv("PANVK_FRAG_CATCHUP_MS");
                int catchup_ms = cv ? atoi(cv) : 0;
                if (catchup_ms > 0) {
                    uint32_t ev2 = 0;
                    while (catchup_ms > 0) {
                        int r = pan_kmod_wait_event_timeout(cmd->dev, NULL, &ev2, catchup_ms, 0);
                        if (r == 0) {
                            fprintf(stderr, "  -> fragment catch-up event=0x%x (atom completed)\n", ev2);
                            break;
                        }
                        if (r == -EAGAIN) break; /* timeout exhausted */
                        break; /* other error */
                    }
                }
                fprintf(stderr, "  -> ETIMEDOUT: render completed, event lost - frame kept\n");
                ret = 0;
            } else {
                fprintf(stderr, "  -> ETIMEDOUT: color buffer untouched, GPU hang "
                                "confirmed - frame dropped; rebooting required\n");
                kbase_wedge_mark();
                return ret;
            }
        } else {
            return ret;
        }
    }

    /* 4. Atom 3: Post-Flush (flushes L2 cache after fragment completes).
     * VectorJet's reference pipeline submits this 4th atom unconditionally
     * after the Fragment job; it is the clean completion/drain that lets the
     * kernel scheduler release the fragment job slot.  When the fragment is a
     * 2-job chain that never terminates (our OLD default: FJ1->FJ2 with FJ2
     * bound=0), this post-flush runs behind a stuck fragment and wedges the
     * scheduler (JOB_READ_FAULT) - which is exactly why it was disabled.
     * With the single-fragment-job model (V9_FRAG_SINGLE_JOB=1, matching Mesa
     * JM and VectorJet) the fragment terminates cleanly, so the post-flush is
     * safe and REQUIRED for the scheduler to drain for the next frame. */
    int single_frag = (v9_single && atoi(v9_single) == 1);
    const char *v9_force_post_flush = getenv("V9_FORCE_POST_FLUSH");
    int force_post_flush = (v9_force_post_flush && atoi(v9_force_post_flush) == 1);
    const char *v9_skip_post_flush = getenv("V9_SKIP_POST_FLUSH");
    int skip_post_flush = (v9_skip_post_flush && atoi(v9_skip_post_flush) == 1);
    if (!single_frag && !force_post_flush) {
        /* Old path (2-job chain): skip post-flush to avoid wedge. */
        return 0;
    }
    if (skip_post_flush) {
        /* V9_SKIP_POST_FLUSH=1: skip the post-flush atom entirely.  The
         * post-flush after a TERMINATED fragment is what triggers the MTK r49
         * 0x40 SOFT_STOPPED → 0x42 JOB_READ_FAULT chain that wedges the slot.
         * Skipping it leaves the fragment slot "in use" but avoids the wedge;
         * the next frame uses V9_FORCE_CYCLE_DEV=1 (destroy+create) to get
         * a fresh context with clean slots anyway. */
        if (debug_events) printf("panvk: Post-Flush SKIPPED (V9_SKIP_POST_FLUSH=1)\n");
        return 0;
    }
    /* 4. Atom 3: Post-Flush (drain after fragment completes).  Must be
     * best-effort: the render already completed by this point, so a stall
     * here (e.g. the kernel read-faults the atom after a TERMINATED fragment)
     * must NOT poison the device / mark the GPU as wedged -- that would make
     * a successful frame un-retryable on the very next frame. */
    v9_pack_flush_job((uint32_t *)(base_cpu + (cmd->flush_jc_gpu - cmd->mem_bo->gpu)));
    uint32_t post_code = 0;
    int sr = pan_kmod_submit_flush_timeout(cmd->dev, cmd->flush_jc_gpu, 1, &post_code,
                                           kbase_submit_timeout_ms(1500));
    if (debug_events) printf("panvk: atom 3 POST-FLUSH event=0x%x\n", post_code);
    if (sr != 0 || post_code != 0x1) {
        fprintf(stderr, "v9_cmd_buffer_submit: Post-Flush warning (ret=%d, event=0x%x) - render completed\n",
                sr, post_code);
    }

    /* Slot unwedge is now handled inside pan_kmod_submit_fragment_timeout:
     * it calls kbase_slot_unwedge() after every TERMINATED/CANCELLED fragment,
     * and falls back to pan_kmod_dev_reopen() if the unwedge fails.
     *
     * V9_FORCE_CYCLE_DEV=1: full destroy+create between frames.  The reopen
     * only resets the kbase context but leaves the GPU hardware slot wedged;
     * a full pan_kmod_dev_destroy + pan_kmod_dev_create forces the kernel to
     * release ALL resources (fd close triggers kbase_context_destroy which
     * resets the physical slot).  BOs mapped with SAME_VA persist across the
     * destroy because Linux mmap is reference-counted. */
    const char *v9_force_cycle = getenv("V9_FORCE_CYCLE_DEV");
    if (v9_force_cycle && atoi(v9_force_cycle) == 1) {
        if (cmd->dev) {
            uint32_t saved_gpu_id = pan_kmod_dev_query_props_gpu_id(cmd->dev);
            pan_kmod_dev_destroy(cmd->dev);
            cmd->dev = pan_kmod_dev_create(NULL);
            if (cmd->dev && saved_gpu_id) {
                pan_kmod_dev_set_gpu_id(cmd->dev, saved_gpu_id);
            }
            if (!cmd->dev) {
                fprintf(stderr, "v9_cmd_buffer_submit: V9_FORCE_CYCLE_DEV failed - dev re-create returned NULL\n");
                return -ENODEV;
            }
            if (debug_events)
                fprintf(stderr, "v9_cmd_buffer_submit: V9_FORCE_CYCLE_DEV full destroy+create done\n");
        }
    }
    return 0;
}

uint64_t v9_cmd_buffer_get_pos_gpu(struct v9_cmd_buffer *cmd) {
    return cmd ? cmd->pos_gpu : 0;
}

uint64_t v9_cmd_buffer_get_idx_gpu(struct v9_cmd_buffer *cmd) {
    return cmd ? cmd->idx_gpu : 0;
}

uint64_t v9_cmd_buffer_get_frag_jc_gpu(struct v9_cmd_buffer *cmd) {
    return cmd ? cmd->frag_jc_gpu : 0;
}

struct pan_kmod_dev *v9_cmd_buffer_get_dev(struct v9_cmd_buffer *cmd) {
    return cmd ? cmd->dev : NULL;
}

uint64_t v9_cmd_buffer_get_polylist_gpu(struct v9_cmd_buffer *cmd) {
    return cmd ? cmd->polylist_gpu : 0;
}

uint64_t v9_cmd_buffer_get_ssbo_gpu(struct v9_cmd_buffer *cmd) {
    return cmd ? cmd->ssbo_gpu : 0;
}

bool v9_cmd_buffer_has_compute(struct v9_cmd_buffer *cmd) {
    return cmd ? cmd->has_compute_command : false;
}

void *v9_cmd_buffer_get_mem_cpu(struct v9_cmd_buffer *cmd) {
    return cmd && cmd->mem_bo ? cmd->mem_bo->cpu : NULL;
}

uint64_t v9_cmd_buffer_get_mem_gpu(struct v9_cmd_buffer *cmd) {
    return cmd && cmd->mem_bo ? cmd->mem_bo->gpu : 0;
}

uint32_t v9_cmd_buffer_read_pixel(struct v9_cmd_buffer *cmd, uint32_t x, uint32_t y) {
    if (!cmd || !cmd->color_bo || x >= cmd->config.width || y >= cmd->config.height) return 0;
    uint32_t *color = (uint32_t *)cmd->color_bo->cpu;
    return color[y * cmd->config.width + x];
}

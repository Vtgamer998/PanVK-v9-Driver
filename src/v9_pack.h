/*
 * Valhall v9 GenXML Descriptor Pack Helpers for Mali-G68 MC4
 * Encapsulates hardware descriptor layouts (MFBD, DCD, Tiler Ctx, TJ, FJ)
 */

#ifndef V9_PACK_H
#define V9_PACK_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void pack_u64(uint32_t *ptr, uint64_t val) {
    ptr[0] = (uint32_t)(val & 0xFFFFFFFFu);
    ptr[1] = (uint32_t)(val >> 32);
}

static inline void v9_pack_blend(uint32_t *bl) {
    memset(bl, 0, 32);
    bl[0] = (1u << 9);                                  /* Enable=1 */
    bl[1] = (2u << 0) | (2u << 4) | (1u << 8) |         /* RGB: A=Src, B=Src, C=Zero */
            ((2u << 0) | (2u << 4) | (1u << 8)) << 12 | /* Alpha: same */
            (0xFu << 28);                               /* Color Mask = RGBA */
    bl[2] = (2u << 0) | (3u << 3) | (0u << 16);         /* Mode=2, num_comps-1=3, RT=0 */
    bl[3] = (237u << 12) | 0u;                          /* Conversion: RGBA8_TB | RGBA */
}

static inline void v9_pack_tls(uint32_t *ls, uint64_t tls_base) {
    memset(ls, 0, 32);
    ls[0] = 0;
    ls[1] = 0x80000000u;
    ls[2] = (uint32_t)(tls_base & 0xFFFFFFFFu);
    ls[3] = (uint32_t)((tls_base >> 32) & 0xFFFFu);
}

static inline void v9_pack_depth(uint32_t *zs) {
    memset(zs, 0, 32);
    zs[0] = (7u << 0) | (7u << 4) | (7u << 16);
    zs[4] = (1u << 22) | (7u << 29);
}

static inline void v9_pack_shader_program(uint32_t *sp, uint64_t isa_gpu,
                                          uint32_t stage, uint32_t work_reg_count, uint64_t preload,
                                          bool primary_shader, bool contains_barrier,
                                          bool ftz_fp16, bool ftz_fp32) {
    memset(sp, 0, 16);
    uint32_t register_allocation = work_reg_count <= 32 ? 2u : 0u;
    uint32_t ftz_mode = ftz_fp32 ? (ftz_fp16 ? 2u : 1u) : 0u;
    sp[0] = (8u << 0) | ((stage & 0xFu) << 4) |
            ((uint32_t)primary_shader << 8) | (ftz_mode << 17) |
            ((uint32_t)contains_barrier << 28) | (register_allocation << 30);
    sp[1] = (uint32_t)(preload >> 48);
    pack_u64(sp + 2, isa_gpu);
}

static inline void v9_pack_buffer(uint32_t *buffer, uint64_t address, uint32_t size) {
    memset(buffer, 0, 32);
    buffer[0] = (9u << 0) | (1u << 4);
    buffer[1] = size;
    pack_u64(buffer + 2, address);
}

#define MALI_ATTRIBUTE_DIVISOR_RATE_VERTEX   0u
#define MALI_ATTRIBUTE_DIVISOR_RATE_INSTANCE 1u

static inline void v9_pack_attribute(uint32_t *attr, uint32_t format, uint32_t table,
                                     uint32_t offset, uint32_t buffer_index,
                                     uint32_t stride, uint32_t input_rate) {
    memset(attr, 0, 32);
    attr[0] = (5u << 0) | (0u << 4) | (1u << 8) | ((format & 0x3FFFFFu) << 10); /* Type=5 (Attribute), Offset enable=1 */
    attr[1] = (table & 0x3Fu) |
              ((input_rate ? MALI_ATTRIBUTE_DIVISOR_RATE_INSTANCE :
                             MALI_ATTRIBUTE_DIVISOR_RATE_VERTEX) << 6);
    attr[2] = offset;
    attr[3] = buffer_index;
    attr[4] = stride;
}

static inline void v9_pack_resource(uint32_t *resource, uint64_t address,
                                    uint32_t descriptor_bytes) {
    memset(resource, 0, 16);
    resource[0] = (uint32_t)address;
    resource[1] = (uint32_t)(address >> 32) | (1u << 24);
    resource[2] = descriptor_bytes;
}

static inline void v9_pack_tiler_heap(uint32_t *th, uint64_t backing_gpu, uint32_t size) {
    memset(th, 0, 32);
    th[0] = (9u << 0) | (2u << 4) | (0u << 8);
    th[1] = size;
    pack_u64(th + 2, backing_gpu);
    pack_u64(th + 4, backing_gpu);
    pack_u64(th + 6, backing_gpu + size);
}

static inline void v9_pack_tiler_ctx(uint32_t *tc, uint64_t polylist_gpu, uint32_t width,
                                     uint32_t height, uint64_t heap_desc_gpu) {
    memset(tc, 0, 192);
    pack_u64(tc + 0, polylist_gpu | (1ULL << 48));
    tc[2] = 0x1; /* Hierarchy mask = 1 (Level 0 16x16 bin) */
    tc[3] = (width - 1) | ((height - 1) << 16);
    pack_u64(tc + 6, heap_desc_gpu);
}

static inline void v9_pack_rt0(uint32_t *rt0, uint64_t color_gpu, uint32_t width, uint32_t clear_color) {
    memset(rt0, 0, 64);
    rt0[0] = (1 << 26);
    uint32_t swizzle_rgba = (0 << 0) | (1 << 3) | (2 << 6) | (3 << 9);
    rt0[1] = (1 << 0) | (19 << 3) | (2 << 8) | (1 << 15) | (swizzle_rgba << 16) | (1u << 31);
    pack_u64(rt0 + 8, color_gpu);
    rt0[10] = width * 4;
    rt0[12] = clear_color;
    rt0[13] = clear_color;
    rt0[14] = clear_color;
    rt0[15] = clear_color;
}

static inline void v9_pack_mfbd(uint32_t *mfbd, uint32_t width, uint32_t height,
                                uint64_t dcd_gpu, uint64_t tiler_ctx_gpu, uint64_t sampleloc_gpu) {
    memset(mfbd, 0, 128);
    mfbd[0] = 1;
    pack_u64(mfbd + 4, sampleloc_gpu);
    pack_u64(mfbd + 6, dcd_gpu);
    uint32_t *params = mfbd + 8;
    /* Framebuffer Parameters (v9 genxml): word8 Width/Height minus(1),
     * word9 Bound Min X/Y, word10 Bound Max X/Y in PIXELS (frame_bounding_box),
     * word11: Sample Count log2, Tie-Break, Effective Tile Size log2,
     * Render Target Count minus(1), Color Buffer Allocation shr(10).
     * Mesa pan_emit_fbd: bound_min=0,0 ; bound_max=(w-1),(h-1) ;
     * effective_tile_size=log2(16)=4 ; render_target_count=0 (1 RT) ;
     * color_buffer_allocation=1024>>10=1 for a 16x16 RGBA8 tile buffer. */
    params[0] = (width - 1)  | ((height - 1) << 16);
    params[1] = 0;                                   /* Bound Min 0,0 px */
    params[2] = (width - 1)  | ((height - 1) << 16); /* Bound Max px */
    params[3] = (2 << 6) | (4 << 9) | (0 << 19) | (1 << 24);
    params[4] = (1 << 16);
    pack_u64(params + 6, tiler_ctx_gpu);
}

static inline void v9_pack_dcd(uint32_t *dcd, uint64_t depth_gpu, uint64_t blend_gpu,
                               uint64_t res_gpu, uint64_t sp_gpu, uint64_t tls_gpu) {
    memset(dcd, 0, 3 * 128);
    dcd[0] = 0x00000228; /* pixel_kill=WEAK_EARLY, zs_update=STRONG_EARLY */
    dcd[1] = 0x0000FFFF; /* Sample mask 0xFFFF */
    dcd[7] = 0x3F800000;
    pack_u64(dcd + 10, depth_gpu);
    pack_u64(dcd + 12, 1ULL | blend_gpu);
    pack_u64(dcd + 24, 8ULL | res_gpu);
    pack_u64(dcd + 26, sp_gpu);
    pack_u64(dcd + 28, tls_gpu);
    pack_u64(dcd + 30, 0);
}

static inline void v9_pack_tiler_job(uint32_t *vt, uint32_t width, uint32_t height,
                                      uint64_t tiler_ctx_gpu, uint64_t idx_gpu, uint64_t pos_gpu,
                                      uint64_t depth_gpu, uint64_t blend_gpu, uint64_t res_gpu,
                                      uint64_t sp_gpu, uint64_t sp_vertex_gpu,
                                      uint64_t sp_varying_gpu, uint64_t tls_gpu,
                                      uint32_t index_count, uint32_t index_type,
                                      uint32_t vertex_count, bool malloc_vertex,
                                      uint32_t fau_fs_count, uint64_t fau_fs_gpu,
                                      uint32_t fau_vs_count, uint64_t fau_vs_gpu) {
    /* MALLOC_VERTEX_JOB (Job Type 11) needs a compiled position shader to feed
     * the integrated tiler: when sp_vertex_gpu==0 (no vertex shader compiled,
     * e.g. the fallback stub SPIR-V path) the position shader env is absent and
     * the MALLOC job faults with TILER DATA_INVALID (0x58).  Fall back to the
     * non-IDVS "Tiler Job" (Type 7) which draws the raw vertex buffer directly.
     * Use malloc_vertex only when the caller produced a position shader. */
    malloc_vertex = malloc_vertex && (sp_vertex_gpu != 0);
    memset(vt, 0, 384);
    /* Job Header word 4: bit0 = Is_64b (OBRIGATORIO, default true no genxml
     * v7 "Job Header"; sem ele o GPU parseia o descritor como 32-bit e falha
     * com TILER DATA_INVALID 0x58 apontando para o proprio job).  Type=11
     * (Malloc Vertex/IDVS) ou Type=7 (Tiler non-IDVS). */
    vt[4] = (1u << 0) | (malloc_vertex ? (11u << 1) : (7u << 1));
    pack_u64(vt + 6, 0);                          /* Next = 0 */

    /* PRIMITIVE section (offset 32, words 8..11).
     * Draw mode 8 = Triangles.  Allow_rotating_primitives (bit15),
     * Low/High depth cull (bits16/17) set to clip safely.  Index type
     * encodes the index element: 0 => none, 2 => uint16, 3 => uint32. */
    if (malloc_vertex) {
        uint32_t hw_index_type = (idx_gpu && index_type) ? (index_type == 1 ? 3u : 2u) : 0u;
        vt[8] = 8u | (hw_index_type << 8) | (1u << 15) | (1u << 16) | (1u << 17);
    } else {
        vt[8] = (index_type == 1 ? 0x3C008 : 0x38008);
    }
    vt[9] = 0;                                    /* Base vertex offset */
    vt[10] = 0;                                   /* Instance offset */
    vt[11] = index_count > 0 ? index_count : 3;   /* Index count */

    /* INSTANCE_COUNT (offset 48) = pack count word. */
    vt[12] = 1;                                   /* instance_count = 1 */

    /* ALLOCATION (offset 52): Vertex packet stride 16, attribute stride 0
     * for "no varyings"; with a secondary varying shader: packet stride
     * generic_size+16, attribute stride generic_size.  For Type 7 this word
     * holds the vertex count for the non-indexed (IDVS-less) draw. */
    if (malloc_vertex) {
        vt[13] = sp_varying_gpu ? ((0 + 16) | ((0u) << 16))
                                : (16u | (0u << 16));
    } else {
        vt[13] = (vertex_count > 0 ? vertex_count : 3);
    }
    if (malloc_vertex && sp_varying_gpu)
        vt[8] |= 1u << 18;                        /* Secondary IDVS varying shader */

    /* TILER section at offset 56 (words 14..15): tiler context descriptor. */
    pack_u64(vt + 14, tiler_ctx_gpu);

    /* Type 7 non-IDVS path needs the extra tiler context words at 17/24. */
    if (!malloc_vertex) {
        vt[17] = 4;
        pack_u64(vt + 24, tiler_ctx_gpu);
    }

    /* SCISSOR at offset 104 (26..27): full framebuffer. */
    vt[26] = 0;                                   /* Min X | (Min Y<<16) = 0,0 */
    vt[27] = (width - 1) | ((height - 1) << 16);  /* Max X | (Max Y<<16) */

    /* PRIMITIVE_SIZE at offset 112 (28..29): fixed line width = 0 (no PSIZ). */
    pack_u64(vt + 28, 0x3f800000ULL);

    /* INDICES at offset 120 (30..31): index buffer address (0 if none). */
    pack_u64(vt + 30, idx_gpu);

    /* DRAW section (DCD) at offset 128 (words 32+). */
    uint32_t *dw = vt + 32;
    dw[0] = (1u << 0) | (1u << 1) | (1u << 6);    /* allow fwd kill/be killed + reorder */
    dw[1] = 0xFFFF | (0x1u << 16);                /* Sample mask 0xFFFF, RT mask 1 */
    if (malloc_vertex) {
        /* Vertex array: Packet mode (bit0=1) - HW fills the pointer/strides
         * back from the IDVS position buffer.  Pointer bits left zero. */
        dw[2] = 1u;                               /* Packet = true */
        dw[3] = 0;
        dw[4] = 0;
    } else {
        /* Type 7: direct vertex array = pos_gpu shr 6. */
        uint64_t V = pos_gpu >> 6;
        dw[2] = (uint32_t)((V & 0x03FFFFFFu) << 6);
        dw[3] = (uint32_t)((V >> 26) & 0xFFFFFFFFu);
        dw[4] = (16u << 16);
    }
    dw[7] = 0x3F800000;                           /* Maximum Z = 1.0 */
    pack_u64(dw + 10, depth_gpu);                 /* Depth/stencil address */
    /* Blend: count=1 in low 4 bits, address shr(4) in remaining bits. */
    pack_u64(dw + 12, 1ULL | blend_gpu);
    /* Shader Environment (fcopy in) at DRAW+16 (words 48..63). */
    uint32_t *se = dw + 16;
    se[0] = 0;                                    /* Attribute offset */
    se[1] = fau_fs_count;                         /* FAU count */
    pack_u64(se + 8, 8ULL | res_gpu);             /* Resources */
    pack_u64(se + 10, sp_gpu);                    /* Shader program */
    pack_u64(se + 12, tls_gpu);                   /* Thread storage */
    pack_u64(se + 14, fau_fs_count ? fau_fs_gpu : 0);  /* FAU */

    /* POSITION shader env at MALLOC_VERTEX_JOB offset 256 (words 64..79). */
    if (sp_vertex_gpu) {
        uint32_t *pos_se = vt + 64;
        pos_se[0] = 0;
        pos_se[1] = fau_vs_count;
        pack_u64(pos_se + 8, 8ULL | res_gpu);
        pack_u64(pos_se + 10, sp_vertex_gpu);
        pack_u64(pos_se + 12, tls_gpu);
        pack_u64(pos_se + 14, fau_vs_count ? fau_vs_gpu : 0);
    }
    /* VARYING shader env at offset 320 (words 80..95). */
    if (sp_varying_gpu) {
        uint32_t *vary_se = vt + 80;
        vary_se[0] = 0;
        vary_se[1] = fau_vs_count;
        pack_u64(vary_se + 8, 8ULL | res_gpu);
        pack_u64(vary_se + 10, sp_varying_gpu);
        pack_u64(vary_se + 12, tls_gpu);
        pack_u64(vary_se + 14, fau_vs_count ? fau_vs_gpu : 0);
    }
}

static inline void v9_pack_frag_job_chain(uint32_t *fj1, uint32_t *fj2,
                                           uint64_t mfbd1_gpu, uint64_t mfbd2_gpu,
                                           uint64_t fj2_gpu, uint32_t width, uint32_t height) {
    /* Fragment Job Payload word 1 is the tile bound (Bound Max X/Y) in 16x16
     * tile units, inclusive (Mesa pan_fb_bbox_from_xywh: max = size-1).  A 0,0
     * bound restricts rendering to tile (0,0) AND leaves the polygon-list
     * hierarchy un-walked, which is what made the fragment soft-stop/never
     * signal DONE (0x1) on the MTK r49 kernel. */
    uint32_t bound = (((width - 1) >> 4) | (((height - 1) >> 4) << 16));

    /* Job 2: End-of-frame / completion pass.
     * Per VectorJet "fragment_job_termination_resolved" (2026-07-30): FJ2 is
     * the completion pass that lets the 2-job fragment chain emit a clean DONE
     * (0x1) instead of being watchdog-soft-stopped (0x4) / lost.  The tile
     * bound here is the empirically-discovered completion magic 0x00030003,
     * NOT the framebuffer's computed bound (a 0 bound makes FJ2 a no-op on
     * 16x16 and the chain never terminates). */
    memset(fj2, 0, 128);
    fj2[4] = (2u << 16) | (9u << 1);  /* 0x00020012: index 2 in chain, Type 9 */
    fj2[5] = 1;                        /* Bit 0 = 1 */
    pack_u64(fj2 + 6, 0);              /* Next = NULL */
    fj2[8] = 0;
    fj2[9] = bound;                    /* completion-pass tile bound: covers all tiles */
    pack_u64(fj2 + 10, mfbd2_gpu | 0x03u);

    /* Job 1: Main polygon-list rendering pass */
    memset(fj1, 0, 128);
    fj1[4] = (1u << 16) | (9u << 1);   /* 0x00010012: index 1 in chain, Type 9 */
    fj1[5] = 0;
    pack_u64(fj1 + 6, fj2_gpu);        /* Next = Job 2 (completion pass) */
    fj1[8] = 0;
    fj1[9] = 0; /* bound=0: let the MFBD carry the real tile bounds (Chrome does this;
                 * setting a non-zero job-level bound on MTK r49 wedge the fragment
                 * HW in frames larger than 1 tile -> renders black + never DONE). */
    pack_u64(fj1 + 10, mfbd1_gpu | 0x01u); /* Polygon List Mode */}

static inline void v9_pack_mfbd2(uint32_t *mfbd2, uint32_t width, uint32_t height,
                                  uint64_t dcd2_gpu, uint64_t tiler_ctx_gpu, uint64_t sampleloc_gpu) {
    memset(mfbd2, 0, 128);
    mfbd2[0] = 0;
    mfbd2[2] = 0x00010000;
    pack_u64(mfbd2 + 4, sampleloc_gpu);
    pack_u64(mfbd2 + 6, dcd2_gpu);

    uint32_t tiles_x = (width + 15) / 16;
    uint32_t tiles_y = (height + 15) / 16;
    uint32_t *params = mfbd2 + 8;
    params[0] = (width - 1) | ((height - 1) << 16);
    params[1] = (tiles_x - 1) | ((tiles_y - 1) << 16);
    params[2] = (width - 1) | ((height - 1) << 16);
    params[3] = 0x01039000; /* No color RT */
    params[4] = 0x00200000;
    pack_u64(params + 6, tiler_ctx_gpu);
}

static inline void v9_pack_dcd2(uint32_t *dcd2, uint64_t tls_gpu) {
    memset(dcd2, 0, 128);
    pack_u64(dcd2 + 28, tls_gpu);
}

static inline void v9_pack_flush_job(uint32_t *fl) {
    memset(fl, 0, 64);
    fl[4] = (3u << 1);            /* Type = 3 (Cache Flush) */
    fl[8] = 0xFFFFFFFFu;          /* Invalidate/Clean all core caches */
    fl[9] = 0xFFFFFFFFu;          /* Invalidate/Clean all L2 caches */
}

/* Valhall v9 compute job (Job Type=4). Layout from genxml/v9.xml:
 *   [0:8)   Job Header          (job_type=4 at bits [1:4])
 *   [8:32)  Compute Payload     (workgroup size/count, offsets)
 *   [16:32) Shader Environment  (embedded at Compute Payload word 8)
 */
static inline void v9_pack_compute_job(uint32_t *cj, uint32_t local_size_x,
                                       uint32_t local_size_y, uint32_t local_size_z,
                                       uint32_t workgroup_count_x,
                                       uint32_t workgroup_count_y,
                                       uint32_t workgroup_count_z,
                                       uint64_t res_gpu, uint64_t sp_gpu,
                                       uint64_t tls_gpu, uint32_t fau_count,
                                       uint64_t fau_gpu) {
    memset(cj, 0, 128);
    /* Job Header: type = 4 (Compute). */
    cj[4] = (4u << 1);
    uint32_t *pl = cj + 8; /* Compute Payload starts at word 8 */

    /* Word 0: workgroup size-1 (10 bits each), allow_merging=0. */
    uint32_t wx = (local_size_x - 1) & 0x3FFu;
    uint32_t wy = (local_size_y - 1) & 0x3FFu;
    uint32_t wz = (local_size_z - 1) & 0x3FFu;
    pl[0] = wx | (wy << 10) | (wz << 20);
    /* Word 1: task increment = 1, task axis = X. */
    pl[1] = (1u << 0) | (0u << 14);
    /* Words 2-4: workgroup counts. */
    pl[2] = workgroup_count_x;
    pl[3] = workgroup_count_y;
    pl[4] = workgroup_count_z;
    /* Words 5-7: offsets = 0. */
    pl[5] = 0;
    pl[6] = 0;
    pl[7] = 0;

    /* Words 8-23: Shader Environment. */
    uint32_t *se = pl + 8;
    se[0] = 0; /* Attribute offset */
    se[1] = fau_count; /* FAU count */
    pack_u64(se + 8, 8ULL | res_gpu);
    pack_u64(se + 10, sp_gpu);
    pack_u64(se + 12, tls_gpu);
    pack_u64(se + 14, fau_gpu);
}

static inline uint32_t v9_pack_bits(uint32_t val, unsigned lo, unsigned hi) {
    unsigned w = hi - lo + 1;
    uint32_t mask = w >= 32 ? 0xFFFFFFFFu : ((1u << w) - 1u);
    return (val & mask) << lo;
}

static inline uint32_t v9_vk_format_to_mali(uint32_t vkFormat) {
    switch (vkFormat) {
        case 37: /* R8G8B8A8_UNORM */ return (187u << 12) | 0u; /* RGBA order 0, sRGB 0 */
        case 43: /* R8G8B8A8_SRGB  */ return (187u << 12) | (1u << 20);
        case 44: /* B8G8R8A8_UNORM */ return (187u << 12) | 1u; /* BGRA order 1? */
        case 50: /* B8G8R8A8_SRGB  */ return (187u << 12) | 1u | (1u << 20);
        case 109: /* R32G32B32A32_SFLOAT */ return (0u << 12) | 0u; /* placeholder for float */
        default: return (187u << 12);
    }
}

static inline uint32_t v9_mali_tex_swizzle_default(void) { return 0x688u; /* RGBA */ }

static inline uint32_t v9_clump_for_bpp(uint32_t bpp) {
    switch (bpp) {
        case 1: return 0; /* RAW8 */
        case 2: return 1; /* RAW16 */
        case 4: return 2; /* RAW32 */
        case 8: return 3; /* RAW64 */
        case 16: return 4; /* RAW128 */
        default: return 2;
    }
}

static inline void v9_pack_sampler(uint32_t *s, uint32_t mag_filter, uint32_t min_filter,
                                   uint32_t mipmap_mode, uint32_t wrap_s, uint32_t wrap_t, uint32_t wrap_r,
                                   bool unnormalized) {
    memset(s, 0, 32);
    /* Type Sampler =1 at 0:3 */
    s[0] = v9_pack_bits(1, 0, 3) |
           v9_pack_bits(0, 4, 5) | /* reduction average */
           v9_pack_bits(wrap_r, 8, 11) |
           v9_pack_bits(wrap_t, 12, 15) |
           v9_pack_bits(wrap_s, 16, 19) |
           v9_pack_bits(1, 21, 21) | /* round to nearest even */
           v9_pack_bits(0, 22, 22) | /* srgb override */
           v9_pack_bits(1, 23, 23) | /* seamless */
           v9_pack_bits(0, 24, 24) |
           v9_pack_bits(unnormalized ? 0 : 1, 25, 25) | /* normalized */
           v9_pack_bits(1, 26, 26) |
           v9_pack_bits(min_filter == 0 ? 1 : 0, 27, 27) | /* minify nearest if VK_FILTER_NEAREST(0) */
           v9_pack_bits(mag_filter == 0 ? 1 : 0, 28, 28) |
           v9_pack_bits(0, 29, 29) |
           v9_pack_bits(mipmap_mode, 30, 31);
    /* min/max LOD: 0..15 as ulod 8 frac */
    s[1] = v9_pack_bits(0, 0, 12) | /* min lod 0 */
           v9_pack_bits(0, 13, 15) | /* compare func never */
           v9_pack_bits((uint32_t)(15 * 256), 16, 28); /* max lod 15 */
    s[2] = v9_pack_bits(0, 0, 15) | /* lod bias 0 */
           v9_pack_bits(0, 16, 20) | /* max anisotropy-1 */
           v9_pack_bits(0, 24, 25); /* lod algorithm isotropic */
    s[3] = 0;
    s[4] = 0; s[5]=0; s[6]=0; s[7]=0;
}

static inline void v9_pack_texture(uint32_t *t, uint32_t dimension, uint32_t format,
                                   uint32_t width, uint32_t height, uint32_t swizzle,
                                   uint64_t payload_gpu) {
    memset(t, 0, 32);
    t[0] = v9_pack_bits(2, 0, 3) | /* Texture */
           v9_pack_bits(dimension, 4, 6) |
           v9_pack_bits(0, 8, 8) | /* sample corner */
           v9_pack_bits(0, 9, 9) | /* normalize */
           v9_pack_bits(format, 10, 31);
    t[1] = v9_pack_bits(width - 1, 0, 15) | v9_pack_bits(height - 1, 16, 31);
    t[2] = v9_pack_bits(swizzle & 0xFFF, 0, 11) |
           v9_pack_bits(0, 12, 12) | /* texel interleave */
           v9_pack_bits(0, 16, 20) | /* levels-1 (1 level =>0) */
           v9_pack_bits(0, 24, 28); /* min level */
    t[3] = v9_pack_bits(0, 0, 12) | /* min lod 0 */
           v9_pack_bits(0, 13, 15) | /* log2 sample count 0 */
           v9_pack_bits(0, 16, 28); /* max lod 0 */
    pack_u64(t + 4, payload_gpu);
    t[6] = v9_pack_bits(0, 0, 15); /* array_size-1 */
    t[7] = v9_pack_bits(0, 0, 15); /* depth-1 */
}

static inline void v9_pack_generic_plane(uint32_t *p, uint64_t gpu_addr, uint32_t row_stride,
                                         uint64_t slice_stride, uint32_t size, uint32_t width, uint32_t height) {
    memset(p, 0, 32);
    /* Type Plane =10 (0xA) */
    p[0] = v9_pack_bits(10, 0, 3) |
           v9_pack_bits(1, 4, 7) | /* Generic plane type 1 */
           v9_pack_bits(0, 8, 11) | /* clump ordering linear */
           v9_pack_bits(2, 24, 31); /* clump format RAW32 for RGBA8 (value 2) */
    p[1] = size;
    pack_u64(p + 2, gpu_addr);
    p[4] = row_stride;
    p[5] = 0;
    p[6] = (uint32_t)slice_stride;
    p[7] = v9_pack_bits(width - 1, 0, 15) | v9_pack_bits(height - 1, 16, 31);
}

#ifdef __cplusplus
}
#endif

#endif /* V9_PACK_H */

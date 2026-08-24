/* SPDX-License-Identifier: MIT
 *
 * Mesa NIR + Panfrost Valhall compiler adapter. This file is built as a
 * separate compiler library and deliberately has no kbase or Vulkan-loader
 * dependencies.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panvk_v9_compiler.h"

/* GLIBC compatibility globals for Bionic */
char *program_invocation_name = (char *)"vkmark";
char *program_invocation_short_name = (char *)"vkmark";

/* Mesa's util logging references the Android log API on Bionic builds, but the
 * library must also link and load on non-Android hosts (e.g. Termux). */
extern int __android_log_write(int prio, const char *tag, const char *msg);
int __android_log_write(int prio, const char *tag, const char *msg) {
    (void)prio;
    (void)tag;
    if (msg) fprintf(stderr, "%s\n", msg);
    return 0;
}

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "bifrost/bifrost_compile.h"
#include "pan_compiler.h"
#include "util/ralloc.h"
#include "util/u_dynarray.h"

/* GPU ID passed to the Mesa Valhall v9 compiler. The actual device on this
 * phone is a Mali-G68 MC4 (0x92041010 - Valhall arch 9, same ISA), but G68 is
 * not listed in Mesa's pan_model table, so pan_get_model would return NULL.
 * G77 (0x90001000) is the closest listed Valhall v9 model and produces
 * identical ISA; the real probed id is only used for the runtime device
 * query in pan_kmod_kbase.c / vkGetPhysicalDeviceProperties. */
#define PANVK_V9_COMPILER_GPU_ID 0x90001000u

static pthread_once_t glsl_types_once = PTHREAD_ONCE_INIT;

static void initialize_glsl_types(void) {
    glsl_type_singleton_init_or_ref();
}

struct compile_diagnostic {
    char *buffer;
    size_t size;
};

static void compiler_error(struct compile_diagnostic *diagnostic,
                           const char *message) {
    if (diagnostic->buffer && diagnostic->size)
        snprintf(diagnostic->buffer, diagnostic->size, "%s", message);
}

static void spirv_debug(void *private_data, enum nir_spirv_debug_level level,
                        size_t spirv_offset, const char *message) {
    struct compile_diagnostic *diagnostic = private_data;
    if (level >= NIR_SPIRV_DEBUG_LEVEL_ERROR && diagnostic->buffer && diagnostic->size) {
        snprintf(diagnostic->buffer, diagnostic->size,
                 "SPIR-V word %zu: %s", spirv_offset, message);
    }
}

static bool spirv_has_function_entry_point(const uint32_t *spirv, size_t word_count,
                                           enum panvk_v9_shader_stage stage,
                                           const char *entry_point) {
    uint32_t entry_id = 0;
    for (size_t offset = 5; offset < word_count;) {
        uint16_t count = spirv[offset] >> 16;
        uint16_t opcode = spirv[offset] & 0xffff;
        if (!count || offset + count > word_count) return false;
        if (opcode == 15 && count >= 4 && spirv[offset + 1] == (uint32_t)stage) {
            const char *name = (const char *)&spirv[offset + 3];
            size_t name_bytes = (count - 3) * sizeof(uint32_t);
            const char *end = memchr(name, '\0', name_bytes);
            if (end && strlen(entry_point) == (size_t)(end - name) &&
                !memcmp(name, entry_point, (size_t)(end - name))) {
                entry_id = spirv[offset + 2];
                break;
            }
        }
        offset += count;
    }
    if (!entry_id) return false;

    for (size_t offset = 5; offset < word_count;) {
        uint16_t count = spirv[offset] >> 16;
        uint16_t opcode = spirv[offset] & 0xffff;
        if (!count || offset + count > word_count) return false;
        if (opcode == 54 && count >= 5 && spirv[offset + 2] == entry_id) return true;
        offset += count;
    }
    return false;
}

static inline unsigned panvk_res_handle(unsigned table, unsigned index) {
    return (table << 24) | (index & 0xFFFFFFu);
}
static inline unsigned panvk_res_handle_get_table(unsigned handle) { return handle >> 24; }
static inline unsigned panvk_res_handle_get_index(unsigned handle) { return handle & 0xFFFFFFu; }

struct lower_descriptors_ctx {
    const struct panvk_v9_pipeline_layout *layout;
    bool unsupported;
};

static const struct panvk_v9_descriptor_binding *
find_descriptor_binding(const struct panvk_v9_pipeline_layout *layout,
                        uint32_t set, uint32_t binding) {
    if (!layout) return NULL;
    for (uint32_t i = 0; i < layout->binding_count; i++) {
        if (layout->bindings[i].set == set && layout->bindings[i].binding == binding)
            return &layout->bindings[i];
    }
    return NULL;
}

static void get_tex_deref_binding(nir_deref_instr *deref, uint32_t *set,
                                  uint32_t *binding, uint32_t *index_imm,
                                  nir_def **index_ssa, uint32_t *max_idx) {
    *index_imm = 0;
    *max_idx = 0;
    *index_ssa = NULL;
    if (deref->deref_type == nir_deref_type_array) {
        if (nir_src_is_const(deref->arr.index))
            *index_imm = nir_src_as_uint(deref->arr.index);
        else
            *index_ssa = deref->arr.index.ssa;
        /* max_idx = array_size -1 for bounds; 0 means variable */
        *max_idx = (uint32_t)glsl_get_aoa_size(nir_deref_instr_parent(deref)->type) - 1;
        if (*max_idx == UINT32_MAX) *max_idx = 0;
        deref = nir_deref_instr_parent(deref);
    }
    assert(deref->deref_type == nir_deref_type_var);
    nir_variable *var = deref->var;
    *set = var->data.descriptor_set;
    *binding = var->data.binding;
}

static bool lower_tex_instr(nir_builder *b, nir_tex_instr *tex,
                            const struct panvk_v9_pipeline_layout *layout) {
    bool progress = false;
    b->cursor = nir_before_instr(&tex->instr);
    int sampler_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
    if (sampler_src_idx >= 0) {
        nir_deref_instr *deref = nir_src_as_deref(tex->src[sampler_src_idx].src);
        uint32_t set, binding, index_imm, max_idx;
        nir_def *index_ssa;
        get_tex_deref_binding(deref, &set, &binding, &index_imm, &index_ssa, &max_idx);
        const struct panvk_v9_descriptor_binding *bind = find_descriptor_binding(layout, set, binding);
        if (!bind) return false;
        nir_tex_instr_remove_src(tex, sampler_src_idx);
        uint32_t base = bind->resource_index;
        if (bind->descriptor_type == 1 /* VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER */) {
            base = (base & 0xFFFFFFu) | 0x05000000u; /* sampler table 5, same idx */
        }
        /* For array, stride 1 (each desc 1 slot). */
        uint32_t static_idx = base + index_imm;
        if (index_ssa) {
            nir_def *offset = nir_iadd(b, nir_imm_int(b, base), nir_imul_imm(b, index_ssa, 1));
            nir_tex_instr_add_src(tex, nir_tex_src_sampler_offset, offset);
            /* sampler_index immediate stays 0, offset carries full handle */
        } else {
            tex->sampler_index = static_idx;
        }
        progress = true;
    } else {
        /* No sampler deref: use dummy sampler handle if texop needs sampler but none provided.
         * For Valhall, dummy = table 0 index 16 (as Mesa). */
        bool need_sampler = !(tex->op == nir_texop_txf || tex->op == nir_texop_txf_ms ||
                              tex->op == nir_texop_txs || tex->op == nir_texop_query_levels ||
                              tex->op == nir_texop_texture_samples ||
                              tex->op == nir_texop_samples_identical);
        if (need_sampler && nir_tex_instr_src_index(tex, nir_tex_src_sampler_handle) < 0 &&
            nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset) < 0) {
            tex->sampler_index = panvk_res_handle(0, 16);
        }
    }
    int tex_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
    if (tex_src_idx >= 0) {
        nir_deref_instr *deref = nir_src_as_deref(tex->src[tex_src_idx].src);
        uint32_t set, binding, index_imm, max_idx;
        nir_def *index_ssa;
        get_tex_deref_binding(deref, &set, &binding, &index_imm, &index_ssa, &max_idx);
        const struct panvk_v9_descriptor_binding *bind = find_descriptor_binding(layout, set, binding);
        if (!bind) return false;
        nir_tex_instr_remove_src(tex, tex_src_idx);
        uint32_t base = bind->resource_index;
        uint32_t static_idx = base + index_imm;
        if (index_ssa) {
            nir_def *offset = nir_iadd(b, nir_imm_int(b, base), nir_imul_imm(b, index_ssa, 1));
            nir_tex_instr_add_src(tex, nir_tex_src_texture_offset, offset);
        } else {
            tex->texture_index = static_idx;
        }
        progress = true;
    }
    return progress;
}

static bool lower_tex_pass(nir_builder *b, nir_instr *instr, void *data) {
    if (instr->type != nir_instr_type_tex) return false;
    return lower_tex_instr(b, nir_instr_as_tex(instr), (const struct panvk_v9_pipeline_layout *)data);
}

static nir_def *get_image_handle(nir_builder *b, nir_deref_instr *deref,
                                 const struct panvk_v9_pipeline_layout *layout) {
    uint32_t set, binding, index_imm, max_idx;
    nir_def *index_ssa;
    get_tex_deref_binding(deref, &set, &binding, &index_imm, &index_ssa, &max_idx);
    const struct panvk_v9_descriptor_binding *bind = find_descriptor_binding(layout, set, binding);
    if (!bind) return NULL;
    uint32_t base = bind->resource_index;
    if (index_ssa) {
        return nir_iadd(b, nir_imm_int(b, base + index_imm), nir_imul_imm(b, index_ssa, 1));
    } else {
        return nir_imm_int(b, base + index_imm);
    }
}

static bool lower_image_intrinsic(nir_builder *b, nir_intrinsic_instr *intr,
                                  const struct panvk_v9_pipeline_layout *layout) {
    b->cursor = nir_before_instr(&intr->instr);
    nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
    nir_def *handle = get_image_handle(b, deref, layout);
    if (!handle) return false;
    /* Rewrite image_deref_* to plain image_* with handle index. */
    nir_rewrite_image_intrinsic(intr, handle, nir_image_intrinsic_type_default);
    return true;
}

static bool lower_image_pass(nir_builder *b, nir_instr *instr, void *data) {
    if (instr->type != nir_instr_type_intrinsic) return false;
    nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
    const struct panvk_v9_pipeline_layout *layout = data;
    switch (intr->intrinsic) {
    case nir_intrinsic_image_deref_load:
    case nir_intrinsic_image_deref_store:
    case nir_intrinsic_image_deref_atomic:
    case nir_intrinsic_image_deref_atomic_swap:
    case nir_intrinsic_image_deref_size:
    case nir_intrinsic_image_deref_samples:
        return lower_image_intrinsic(b, intr, layout);
    default:
        return false;
    }
}

static bool lower_descriptor_intrinsic(nir_builder *builder,
                                       nir_instr *instruction, void *data) {
    if (instruction->type != nir_instr_type_intrinsic) return false;
    nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instruction);
    struct lower_descriptors_ctx *ctx = data;
    builder->cursor = nir_before_instr(instruction);
    nir_def *replacement = NULL;

    switch (intrinsic->intrinsic) {
    case nir_intrinsic_vulkan_resource_index: {
        const struct panvk_v9_descriptor_binding *binding = find_descriptor_binding(
            ctx->layout, nir_intrinsic_desc_set(intrinsic),
            nir_intrinsic_binding(intrinsic));
        if (!binding || (binding->descriptor_type != 6 /* VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER */ &&
                         binding->descriptor_type != 7 /* VK_DESCRIPTOR_TYPE_STORAGE_BUFFER */ &&
                         binding->descriptor_type != 8 /* VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC */ &&
                         binding->descriptor_type != 9 /* VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC */)) {
            ctx->unsupported = true;
            return false;
        }
        /* Valhall vec2_index_32bit_offset address: (first_desc_index,
         * array_index, offset=0). The dynamic array index rides in src[0]. */
        replacement = nir_vec3(builder,
            nir_iadd(builder, nir_imm_int(builder, binding->resource_index),
                     intrinsic->src[0].ssa),
            nir_imm_int(builder, 0),
            nir_imm_int(builder, 0));
        break;
    }
    case nir_intrinsic_vulkan_resource_reindex:
        replacement = nir_vec2(builder,
            nir_iadd(builder, nir_channel(builder, intrinsic->src[0].ssa, 0),
                     intrinsic->src[1].ssa),
            nir_channel(builder, intrinsic->src[0].ssa, 1));
        break;
    case nir_intrinsic_load_vulkan_descriptor:
        if (nir_intrinsic_desc_type(intrinsic) != nir_descriptor_type_uniform_buffer &&
            nir_intrinsic_desc_type(intrinsic) != nir_descriptor_type_storage_buffer) {
            ctx->unsupported = true;
            return false;
        }
        replacement = intrinsic->src[0].ssa;
        break;
    default:
        return false;
    }

    nir_def_rewrite_uses(&intrinsic->def, replacement);
    nir_instr_remove(instruction);
    return true;
}

static unsigned panvk_v9_glsl_type_size(const struct glsl_type *type,
                                        bool bindless) {
    return glsl_count_attribute_slots(type, false);
}

/* nir_lower_viewport_transform emits load_viewport_scale/offset intrinsics
 * which the real panvk inlines from the pipeline viewport state. This driver
 * has a single fixed viewport, so inline the identity transform. */
/* pan_nir_lower_noperspective_vs inserts a nir_load_noperspective_varyings_pan
 * intrinsic that upstream panvk resolves from a VS sysval (the bitmask of
 * varyings marked noperspective in the linked FS).  That plumbing does not
 * exist here, so fold the load to a constant 0: every varying is treated as
 * perspective-correct, which is the safe default. */
static bool fold_noperspective_load(nir_builder *builder,
                                    nir_instr *instruction, void *data) {
    (void)data;
    if (instruction->type != nir_instr_type_intrinsic) return false;
    nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instruction);
    if (intrinsic->intrinsic != nir_intrinsic_load_noperspective_varyings_pan)
        return false;
    builder->cursor = nir_before_instr(instruction);
    nir_def *zero = nir_imm_intN_t(builder, 0, 32);
    nir_def_rewrite_uses(&intrinsic->def, zero);
    nir_instr_remove(instruction);
    return true;
}

static bool lower_viewport_sysvals(nir_builder *builder,
                                   nir_instr *instruction, void *data) {
    if (instruction->type != nir_instr_type_intrinsic) return false;
    nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instruction);
    (void)data;
    nir_def *replacement = NULL;
    switch (intrinsic->intrinsic) {
    case nir_intrinsic_load_viewport_scale:
        builder->cursor = nir_before_instr(instruction);
        replacement = nir_vec3(builder, nir_imm_float(builder, 1.0),
                               nir_imm_float(builder, 1.0),
                               nir_imm_float(builder, 1.0));
        break;
    case nir_intrinsic_load_viewport_offset:
        builder->cursor = nir_before_instr(instruction);
        replacement = nir_vec3(builder, nir_imm_float(builder, 0.0),
                               nir_imm_float(builder, 0.0),
                               nir_imm_float(builder, 0.0));
        break;
    default:
        return false;
    }
    nir_def_rewrite_uses(&intrinsic->def, replacement);
    nir_instr_remove(instruction);
    return true;
}

static bool prepare_nir(nir_shader *nir,
                        const struct panvk_v9_pipeline_layout *layout) {
    /* Match the Vulkan runtime's canonical SPIR-V cleanup before applying
     * Panfrost-specific lowering. In particular, returns and helper functions
     * must not reach the Valhall backend. */
    NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
    NIR_PASS(_, nir, nir_lower_returns);
    NIR_PASS(_, nir, nir_inline_functions);
    NIR_PASS(_, nir, nir_opt_copy_prop);
    NIR_PASS(_, nir, nir_opt_deref);
    nir_remove_non_entrypoints(nir);
    NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);
    NIR_PASS(_, nir, nir_split_var_copies);
    NIR_PASS(_, nir, nir_split_per_member_structs);
    NIR_PASS(_, nir, nir_lower_clip_cull_distance_to_vec4s);
    NIR_PASS(_, nir, nir_propagate_invariant, false);

    nir_function_impl *entrypoint = nir_shader_get_entrypoint(nir);
    if (!entrypoint) return false;
    return true;
}

/* Valhall expects a scalar 32-bit buffer index; fold the vec3
 * (desc_index, array_index, offset) address into a single packed index. */
static bool pack_buf_index(nir_builder *builder,
                           nir_instr *instruction, void *data) {
    if (instruction->type != nir_instr_type_intrinsic) return false;
    nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instruction);
    unsigned index_src;
    switch (intrinsic->intrinsic) {
    case nir_intrinsic_load_ubo:
    case nir_intrinsic_load_ssbo:
        index_src = 0;
        break;
    case nir_intrinsic_store_ssbo:
        index_src = 1;
        break;
    default:
        return false;
    }
    nir_def *index = intrinsic->src[index_src].ssa;
    if (index->num_components == 1) return false;
    builder->cursor = nir_before_instr(instruction);
    nir_def *packed = nir_iadd(builder, nir_channel(builder, index, 0),
                               nir_channel(builder, index, 1));
    nir_src_rewrite(&intrinsic->src[index_src], packed);
    return true;
}

/* The vec2_index lowering drops the descriptor alignment from the deref
 * chain, leaving align_mul=0 which the Valhall mem-access size lowering
 * rejects. Descriptors are at least 16-byte aligned, restore that. */
static bool fixup_mem_align(nir_builder *builder,
                            nir_instr *instruction, void *data) {
    if (instruction->type != nir_instr_type_intrinsic) return false;
    nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instruction);
    switch (intrinsic->intrinsic) {
    case nir_intrinsic_load_ubo:
    case nir_intrinsic_load_ssbo:
    case nir_intrinsic_store_ssbo:
        break;
    default:
        return false;
    }
    if (!nir_intrinsic_has_align_mul(intrinsic)) return false;
    uint32_t align_mul = nir_intrinsic_align_mul(intrinsic);
    uint32_t align_offset = nir_intrinsic_align_offset(intrinsic);
    if (align_mul == 0) {
        align_mul = 16;
        align_offset = 0;
        nir_intrinsic_set_align_mul(intrinsic, align_mul);
        nir_intrinsic_set_align_offset(intrinsic, align_offset);
    }
    return true;
}

/* The Valhall tex lowering (pan_nir_lower_tex) infinite-loops on tex
 * instructions that still carry texture_deref/sampler_deref sources (the
 * panvk descriptor lowering that converts them to indices is not implemented
 * for textures here).  Reject such shaders early so the caller falls back to
 * the compiled stub shader instead of hanging in va_lower_tex. */
static bool check_tex_deref(nir_builder *b, nir_instr *instruction, void *data) {
    bool *unsupported = data;
    (void)b;
    if (instruction->type != nir_instr_type_tex) return false;
    nir_tex_instr *tex = nir_instr_as_tex(instruction);
    for (unsigned i = 0; i < tex->num_srcs; i++) {
        if (tex->src[i].src_type == nir_tex_src_texture_deref ||
            tex->src[i].src_type == nir_tex_src_sampler_deref) {
            *unsupported = true;
            return false;
        }
    }
    return false;
}

static bool check_image_deref(nir_builder *b, nir_instr *instruction, void *data) {
    bool *unsupported = data;
    (void)b;
    if (instruction->type != nir_instr_type_intrinsic) return false;
    nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instruction);
    switch (intr->intrinsic) {
    case nir_intrinsic_image_deref_load:
    case nir_intrinsic_image_deref_store:
    case nir_intrinsic_image_deref_atomic:
    case nir_intrinsic_image_deref_atomic_swap:
    case nir_intrinsic_image_deref_size:
    case nir_intrinsic_image_deref_samples:
        *unsupported = true;
        return false;
    default:
        return false;
    }
}

static bool lower_mem_io(nir_shader *nir,
                         const struct panvk_v9_pipeline_layout *layout) {
    struct lower_descriptors_ctx ctx = { .layout = layout };
    NIR_PASS(_, nir, nir_shader_instructions_pass, lower_descriptor_intrinsic,
             nir_metadata_block_index | nir_metadata_dominance, &ctx);
    if (ctx.unsupported) return false;

    /* Lower texture/sampler derefs to handle indices (Valhall texture_index / sampler_index). */
    NIR_PASS(_, nir, nir_shader_instructions_pass, lower_tex_pass,
             nir_metadata_block_index | nir_metadata_dominance, (void *)layout);
    /* Lower storage image derefs to handle indices. */
    NIR_PASS(_, nir, nir_shader_instructions_pass, lower_image_pass,
             nir_metadata_block_index | nir_metadata_dominance, (void *)layout);
    bool tex_still_unsupported = false;
    NIR_PASS(_, nir, nir_shader_instructions_pass, check_tex_deref,
             nir_metadata_none, &tex_still_unsupported);
    if (tex_still_unsupported) return false;
    /* Check leftover image_deref intrinsics (storage images). */
    bool img_still_unsupported = false;
    NIR_PASS(_, nir, nir_shader_instructions_pass,
             check_image_deref, nir_metadata_none, &img_still_unsupported);
    if (img_still_unsupported) return false;

    NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo,
             nir_address_format_vec2_index_32bit_offset);
    NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo,
             nir_address_format_vec2_index_32bit_offset);
    NIR_PASS(_, nir, nir_shader_instructions_pass, pack_buf_index,
             nir_metadata_control_flow, NULL);
    NIR_PASS(_, nir, nir_shader_instructions_pass, fixup_mem_align,
             nir_metadata_none, NULL);
    NIR_PASS(_, nir, nir_opt_copy_prop_vars);
    NIR_PASS(_, nir, nir_opt_combine_stores, nir_var_all);
    NIR_PASS(_, nir, nir_lower_system_values);
    NIR_PASS(_, nir, nir_split_var_copies);
    NIR_PASS(_, nir, nir_lower_var_copies);
    NIR_PASS(_, nir, nir_lower_global_vars_to_local);
    nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
    return true;
}

/* Unsized (driver-reserved) FAU push-constant region: matches panvk. The
 * command stream writes the whole pack once per draw; forward decl below. */
static inline unsigned panvk_v9_fau_word(uint32_t off) { return off / 8; }

/* Replicates upstream panvk_vX_shader.c lower_load_push_consts() /
 * collect_push_constant() + move_push_constant() for the packed FAU region.
 * The Valhall backend (bi_emit_load_push_constant) requires load_push_constant
 * to have base=0, range=0 and .base (first arg) to be a constant word index
 * (measured in 32-bit words) inside fau->reserved. */
struct v9_push_const_ctx {
    BITSET_DECLARE(used, 32);
    uint32_t count;
};

static void v9_ctx_collect(struct v9_push_const_ctx *ctx, uint32_t byte_off,
                           uint32_t size) {
    uint32_t first = byte_off / 8, last = (byte_off + size - 1) / 8;
    for (uint32_t w = first; w <= last && w < 32; w++)
        if (!BITSET_TEST(ctx->used, w)) {
            BITSET_SET(ctx->used, w);
            ctx->count++;
        }
}

static bool collect_push_constants(nir_builder *b, nir_instr *instruction,
                                   void *data) {
    if (instruction->type != nir_instr_type_intrinsic) return false;
    nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instruction);
    if (intr->intrinsic != nir_intrinsic_load_push_constant) return false;

    struct v9_push_const_ctx *ctx = data;
    uint32_t base = nir_intrinsic_base(intr) + nir_intrinsic_range(intr);
    (void)base;
    /* SPIR-V push constant loads arrive with a constant first index (the
     * byte offset inside the push constant block). Mark every 8-byte FAU
     * chunk the access touches. Sysvals (base >= SYSVALS_PUSH_CONST_BASE)
     * do not exist in this driver, so every load is a user push constant. */
    uint32_t off = nir_intrinsic_base(intr);
    if (nir_src_is_const(intr->src[0]))
        off += nir_src_as_uint(intr->src[0]);
    uint32_t size = (intr->def.bit_size / 8) * intr->def.num_components;
    v9_ctx_collect(ctx, off, size);
    (void)b;
    return false;
}

/* Remap each user push constant access to its packed FAU word: sysvals get the
 * first words (none here), then the used 8-byte chunks in offset order each
 * own 2 consecutive 32-bit words. */
static bool move_push_constants(nir_builder *b, nir_instr *instruction,
                                void *data) {
    if (instruction->type != nir_instr_type_intrinsic) return false;
    nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instruction);
    if (intr->intrinsic != nir_intrinsic_load_push_constant) return false;
    if (!nir_src_is_const(intr->src[0])) return false;

    const struct v9_push_const_ctx *ctx = data;
    uint32_t off = nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[0]);
    uint32_t word = panvk_v9_fau_word(off);
    uint32_t packed = 0;
    for (uint32_t w = 0; w < word; w++)
        if (BITSET_TEST(ctx->used, w)) packed += 2; /* one 8-byte chunk = 2 words */
    packed = (packed + (off & 7) / 4) * 4;

    b->cursor = nir_before_instr(instruction);
    nir_src_rewrite(&intr->src[0], nir_imm_int(b, packed));
    nir_intrinsic_set_base(intr, 0);
    nir_intrinsic_set_range(intr, 0);
    (void)ctx;
    return true;
}

/* Build the FAU word layout for the given NIR shader: sysval words first
 * (none), then user push constants, then reserved for immediates. Returns
 * the number of reserved FAU words (sysvals+push consts) so the backend can
 * place promoted immediates after them. */
static uint32_t build_fau_push_layout(nir_shader *nir,
                                      struct pan_compile_inputs *inputs,
                                      struct v9_push_const_ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    NIR_PASS(_, nir, nir_shader_instructions_pass, collect_push_constants,
             nir_metadata_block_index | nir_metadata_dominance, ctx);
    /* Two 32-bit words per used 8-byte chunk. */
    inputs->fau.reserved = ctx->count * 2;
    NIR_PASS(_, nir, nir_shader_instructions_pass, move_push_constants,
             nir_metadata_block_index | nir_metadata_dominance, ctx);
    return ctx->count;
}

int panvk_v9_compile_spirv(const uint32_t *spirv, size_t spirv_size,
                           enum panvk_v9_shader_stage stage,
                           const char *entry_point,
                           const struct panvk_v9_pipeline_layout *layout,
                           struct panvk_v9_compiled_shader *result,
                           char *error, size_t error_size) {
    if (!result) return -EINVAL;
    memset(result, 0, sizeof(*result));
    if (error && error_size) error[0] = '\0';

    if (!spirv || spirv_size < 20 || (spirv_size & 3) || !entry_point ||
        (stage != PANVK_V9_SHADER_VERTEX && stage != PANVK_V9_SHADER_FRAGMENT &&
         stage != PANVK_V9_SHADER_COMPUTE)) {
        if (error && error_size) snprintf(error, error_size, "invalid compiler arguments");
        return -EINVAL;
    }
    if (!spirv_has_function_entry_point(spirv, spirv_size / sizeof(uint32_t),
                                        stage, entry_point)) {
        if (error && error_size)
            snprintf(error, error_size, "SPIR-V entry point has no function body");
        return -EINVAL;
    }

    struct compile_diagnostic diagnostic = { error, error_size };
    pthread_once(&glsl_types_once, initialize_glsl_types);
    struct spirv_to_nir_options spirv_options = {
        .environment = NIR_SPIRV_VULKAN,
        .ubo_addr_format = nir_address_format_vec2_index_32bit_offset,
        .ssbo_addr_format = nir_address_format_vec2_index_32bit_offset,
        .push_const_addr_format = nir_address_format_32bit_offset,
        .shared_addr_format = nir_address_format_32bit_offset,
        .temp_addr_format = nir_address_format_32bit_offset,
        .debug = {
            .func = spirv_debug,
            .private_data = &diagnostic,
        },
    };

    nir_shader *nir = spirv_to_nir(spirv, spirv_size / sizeof(uint32_t),
                                    NULL, (mesa_shader_stage)stage,
                                    entry_point, &spirv_options,
                                    &bifrost_nir_options_v9);
    if (!nir) {
        if (!error || !error[0]) compiler_error(&diagnostic, "SPIR-V to NIR conversion failed");
        return -EINVAL;
    }
    if (getenv("PANVK_V9_DEBUG_PRE_NIR")) {
        FILE *f = fopen("/data/data/com.termux/files/usr/tmp/opencode/nir_pre.txt", "a");
        if (f) {
            fprintf(f, "==== stage=%d ====\n", stage);
            nir_print_shader(nir, f);
            fclose(f);
        }
    }

    if (!prepare_nir(nir, layout)) {
        compiler_error(&diagnostic, "shader uses an unsupported descriptor binding");
        ralloc_free(nir);
        return -EINVAL;
    }

    struct pan_compile_inputs inputs = {0};
    inputs.gpu_id = PANVK_V9_COMPILER_GPU_ID;
    inputs.no_idvs = stage != PANVK_V9_SHADER_VERTEX;
    inputs.trust_varying_flat_highp_types = true;
    inputs.fau.promote_immediates = true;

    pan_preprocess_nir(nir, inputs.gpu_id);

    if (!lower_mem_io(nir, layout)) {
        compiler_error(&diagnostic, "shader uses an unsupported descriptor binding");
        ralloc_free(nir);
        return -EINVAL;
    }
    if (getenv("PANVK_V9_DEBUG_POST_NIR")) {
        FILE *f = fopen("/data/data/com.termux/files/usr/tmp/opencode/nir_post.txt", "a");
        if (f) {
            fprintf(f, "==== stage=%d ====\n", stage);
            nir_print_shader(nir, f);
            fclose(f);
        }
    }

    /* Lower var-based shader in/out IO to explicit load_input/store_output
     * intrinsics, matching the panvk flow. The varying layout must be
     * collected after nir_assign_io_var_locations but before the IO is fully
     * converted so the Valhall backend sees explicit varyings. */
    nir_assign_io_var_locations(nir, nir_var_shader_in);
    nir_assign_io_var_locations(nir, nir_var_shader_out);
    NIR_PASS(_, nir, nir_lower_var_copies);
    NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
             nir_var_shader_in | nir_var_shader_out, UINT32_MAX);
    NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
             panvk_v9_glsl_type_size, nir_lower_io_use_interpolated_input_intrinsics);
    NIR_PASS(_, nir, nir_opt_constant_folding);

    struct pan_varying_layout varying_layout;
    if (stage == PANVK_V9_SHADER_VERTEX) {
        pan_varying_collect_formats(&varying_layout, nir, inputs.gpu_id,
                                    inputs.trust_varying_flat_highp_types,
                                    true);
        pan_build_varying_layout_compact(&varying_layout, nir, inputs.gpu_id);
        inputs.varying_layout = &varying_layout;
    }

    struct pan_shader_info info = {0};
    struct util_dynarray binary;
    util_dynarray_init(&binary, NULL);

    pan_postprocess_nir(nir, &inputs, &info);
    if (getenv("PANVK_V9_DEBUG_PP_NIR")) {
        FILE *f = fopen("/data/data/com.termux/files/usr/tmp/opencode/nir_pp.txt", "a");
        if (f) {
            fprintf(f, "==== stage=%d ====\n", stage);
            nir_print_shader(nir, f);
            fclose(f);
        }
    }
    if (stage == PANVK_V9_SHADER_VERTEX)
        NIR_PASS(_, nir, nir_shader_instructions_pass, fold_noperspective_load,
                 nir_metadata_none, NULL);
    if (stage != PANVK_V9_SHADER_COMPUTE)
        NIR_PASS(_, nir, nir_shader_instructions_pass, lower_viewport_sysvals,
                 nir_metadata_none, NULL);
    pan_shader_compile(nir, &inputs, &binary, &info);
    if (stage == PANVK_V9_SHADER_COMPUTE) {
        /* WorkgroupSize is a compile-time property from the SPIR-V
         * decoration; Valhall packs it into the compute job INVOCATION word. */
        result->local_size_x = nir->info.workgroup_size[0];
        result->local_size_y = nir->info.workgroup_size[1];
        result->local_size_z = nir->info.workgroup_size[2];
    }
    ralloc_free(nir);

    if (!binary.size) {
        compiler_error(&diagnostic, "Valhall compiler produced an empty binary");
        util_dynarray_fini(&binary);
        return -EIO;
    }

    result->binary = malloc(binary.size);
    if (!result->binary) {
        util_dynarray_fini(&binary);
        return -ENOMEM;
    }
    memcpy(result->binary, binary.data, binary.size);
    result->binary_size = binary.size;
    result->work_reg_count = info.work_reg_count;
    result->tls_size = info.tls_size;
    result->wls_size = info.wls_size;
    result->preload = info.preload;
    result->contains_barrier = info.contains_barrier;
    result->ftz_fp16 = info.ftz_fp16;
    result->ftz_fp32 = info.ftz_fp32;
    result->outputs_written = info.outputs_written;
    result->fau_count = info.fau.count;
    for (unsigned i = 0; i < PAN_MAX_PUSH; i++) {
        if (i < info.fau.count && BITSET_TEST(info.fau.is_const, i))
            result->fau_consts[i] = info.fau.words[i].constant;
        else
            result->fau_consts[i] = 0;
    }
    if (stage == PANVK_V9_SHADER_VERTEX) {
        result->idvs = info.vs.idvs;
        result->no_psiz_offset = info.vs.no_psiz_offset;
        result->secondary_enable = info.vs.secondary_enable;
        result->secondary_offset = info.vs.secondary_offset;
        result->secondary_work_reg_count = info.vs.secondary_work_reg_count;
        result->secondary_preload = info.vs.secondary_preload;
    }
    if (stage == PANVK_V9_SHADER_FRAGMENT) {
        result->writes_depth = info.fs.writes_depth;
        result->writes_stencil = info.fs.writes_stencil;
        result->writes_coverage = info.fs.writes_coverage;
        result->can_discard = info.fs.can_discard;
    }

    util_dynarray_fini(&binary);
    return 0;
}

void panvk_v9_compiled_shader_cleanup(struct panvk_v9_compiled_shader *shader) {
    if (!shader) return;
    free(shader->binary);
    memset(shader, 0, sizeof(*shader));
}

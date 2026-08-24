/* SPDX-License-Identifier: MIT */
#ifndef PANVK_V9_COMPILER_H
#define PANVK_V9_COMPILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum panvk_v9_shader_stage {
    PANVK_V9_SHADER_VERTEX = 0,
    PANVK_V9_SHADER_FRAGMENT = 4,
    PANVK_V9_SHADER_COMPUTE = 5, /* MESA_SHADER_COMPUTE */
};

struct panvk_v9_compiled_shader {
    uint8_t *binary;
    size_t binary_size;
    uint32_t work_reg_count;
    uint32_t tls_size;
    uint32_t wls_size;
    uint64_t preload;
    bool contains_barrier;
    /* Compute-only: local workgroup size in invocations. */
    uint32_t local_size_x;
    uint32_t local_size_y;
    uint32_t local_size_z;
    bool writes_depth;
    bool writes_stencil;
    bool writes_coverage;
    bool can_discard;
    bool ftz_fp16;
    bool ftz_fp32;
    uint64_t outputs_written;
    bool idvs;
    uint32_t no_psiz_offset;
    bool secondary_enable;
    uint32_t secondary_offset;
    uint32_t secondary_work_reg_count;
    uint64_t secondary_preload;
    /* Valhall FAU (uniform register) layout: words the driver must write into
     * the FAU buffer (SE words 14-15 address, SE word 1 = fau_count). Words
     * that are not constants (e.g. UBO relocations) are zeroed here. */
    uint32_t fau_count;
    uint32_t fau_consts[128];
    /* Number of 32-bit FAU words reserved at the start of the FAU for
     * sysvals + user push constants (an 8-byte chunk occupies 2 words).
     * Immediates/promoted constants never overlap this range. */
    uint32_t fau_reserved;
    /* Used user push-constant 8-byte chunks, byte-offsets listed in packed
     * (prefix-sum) order: chunk j occupies FAU 32-bit words [2j, 2j+2). */
    uint32_t fau_push_count;
    uint32_t fau_push_chunks[32];
};

struct panvk_v9_descriptor_binding {
    uint32_t set;
    uint32_t binding;
    uint32_t descriptor_type;
    uint32_t array_size;
    uint32_t resource_index;
};

struct panvk_v9_pipeline_layout {
    const struct panvk_v9_descriptor_binding *bindings;
    uint32_t binding_count;
    uint32_t ubo_count;
};

/* Compile Vulkan SPIR-V to Mali Valhall v9 machine code. The returned binary
 * is owned by the result and released with panvk_v9_compiled_shader_cleanup(). */
int panvk_v9_compile_spirv(const uint32_t *spirv, size_t spirv_size,
                           enum panvk_v9_shader_stage stage,
                           const char *entry_point,
                           const struct panvk_v9_pipeline_layout *layout,
                           struct panvk_v9_compiled_shader *result,
                           char *error, size_t error_size);

void panvk_v9_compiled_shader_cleanup(struct panvk_v9_compiled_shader *shader);

#ifdef __cplusplus
}
#endif

#endif

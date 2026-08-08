/*
 * Valhall v9 Command Stream Recorder Engine
 * Higher-level Vulkan-like command buffer API for Mali-G68
 */

#ifndef V9_CMD_STREAM_H
#define V9_CMD_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "pan_kmod_kbase.h"
#include "panvk_v9_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

struct v9_cmd_buffer;

struct v9_render_target_config {
    uint32_t width;
    uint32_t height;
    uint32_t clear_color;
};

struct v9_ubo_binding {
    uint64_t address;
    uint32_t size;
    uint32_t index;
};

struct v9_ssbo_binding {
    uint64_t address;
    uint32_t size;
    uint32_t index;
};

struct v9_attribute_binding {
    uint32_t format;
    uint32_t offset;
    uint32_t stride;
    uint32_t input_rate;
    uint64_t buffer_address;
    uint32_t buffer_size;
};

struct v9_cmd_buffer *v9_cmd_buffer_create(struct pan_kmod_dev *dev,
                                           const struct v9_render_target_config *config);
struct v9_cmd_buffer *v9_cmd_buffer_ref(struct v9_cmd_buffer *cmd);
void v9_cmd_buffer_destroy(struct v9_cmd_buffer *cmd);

int v9_cmd_buffer_begin(struct v9_cmd_buffer *cmd);
int v9_cmd_buffer_set_vertex_shader(struct v9_cmd_buffer *cmd,
                                     const struct panvk_v9_compiled_shader *shader);
int v9_cmd_buffer_set_fragment_shader(struct v9_cmd_buffer *cmd,
                                      const struct panvk_v9_compiled_shader *shader);
int v9_cmd_buffer_set_compute_shader(struct v9_cmd_buffer *cmd,
                                     const struct panvk_v9_compiled_shader *shader);
int v9_cmd_buffer_set_ubos(struct v9_cmd_buffer *cmd,
                           const struct v9_ubo_binding *bindings,
                           uint32_t binding_count);
int v9_cmd_buffer_set_ssbos(struct v9_cmd_buffer *cmd,
                            const struct v9_ssbo_binding *bindings,
                            uint32_t binding_count);
int v9_cmd_buffer_dispatch(struct v9_cmd_buffer *cmd,
                           uint32_t count_x, uint32_t count_y, uint32_t count_z);
int v9_cmd_buffer_set_attributes(struct v9_cmd_buffer *cmd,
                                 const struct v9_attribute_binding *bindings,
                                 uint32_t binding_count);
int v9_cmd_draw_indexed_triangle(struct v9_cmd_buffer *cmd);
int v9_cmd_draw_indexed(struct v9_cmd_buffer *cmd,
                        uint64_t idx_gpu, uint32_t index_count, uint32_t index_type,
                        uint64_t pos_gpu, uint32_t vertex_count);
int v9_cmd_buffer_end(struct v9_cmd_buffer *cmd);
int v9_cmd_buffer_submit(struct v9_cmd_buffer *cmd);

/* Redirect this command buffer's colour render target to an external BO (e.g.
 * a swapchain image).  Call after v9_cmd_buffer_begin() and before submit so
 * the GPU writes into the swapchain image instead of the internal slot BO. */
int v9_cmd_buffer_set_render_target(struct v9_cmd_buffer *cmd,
                                    struct pan_kmod_bo *color_bo,
                                    uint64_t color_gpu,
                                    uint32_t width, uint32_t height);

uint64_t v9_cmd_buffer_get_pos_gpu(struct v9_cmd_buffer *cmd);
uint64_t v9_cmd_buffer_get_idx_gpu(struct v9_cmd_buffer *cmd);
uint64_t v9_cmd_buffer_get_frag_jc_gpu(struct v9_cmd_buffer *cmd);
uint64_t v9_cmd_buffer_get_polylist_gpu(struct v9_cmd_buffer *cmd);
uint64_t v9_cmd_buffer_get_ssbo_gpu(struct v9_cmd_buffer *cmd);
bool v9_cmd_buffer_has_compute(struct v9_cmd_buffer *cmd);
void v9_cmd_buffer_update_config(struct v9_cmd_buffer *cmd, uint32_t width, uint32_t height, uint32_t clear_color);
void *v9_cmd_buffer_get_mem_cpu(struct v9_cmd_buffer *cmd);
uint64_t v9_cmd_buffer_get_mem_gpu(struct v9_cmd_buffer *cmd);
struct pan_kmod_dev *v9_cmd_buffer_get_dev(struct v9_cmd_buffer *cmd);
uint32_t v9_cmd_buffer_read_pixel(struct v9_cmd_buffer *cmd, uint32_t x, uint32_t y);

#ifdef __cplusplus
}
#endif

#endif /* V9_CMD_STREAM_H */

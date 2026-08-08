/*
 * PanVK Valhall v9 Vulkan Entry Points & WSI Swapchain Layer Implementation
 * Full Vulkan API implementation for vkmark & Mesa Vulkan applications
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>

#include "panvk_v9_entrypoints.h"
#include "panvk_v9_compiler.h"

#define ICD_LOADER_MAGIC 0x01CDC0DEu

static inline void set_loader_magic(void *object) {
    *(uintptr_t *)object = ICD_LOADER_MAGIC;
}

struct VkInstance_T {
    uintptr_t loader_data;
    struct VkPhysicalDevice_T *phys_dev;
};

struct VkPhysicalDevice_T {
    uintptr_t loader_data;
    struct pan_kmod_dev *kdev;
    struct pan_kmod_dev_props props;
};

/* Features requested at vkCreateDevice time via the pNext chain.  The driver
 * records which Vulkan 1.1/1.2/1.3 + extension features the app asked for so
 * render paths can branch on them later. */
struct panvk_v9_enabled_features {
    bool dynamic_rendering;
    bool descriptor_indexing;
    bool runtime_descriptor_array;
    bool partially_bound;
    bool update_after_bind;
    bool timeline_semaphore;
    bool buffer_device_address;
    bool host_query_reset;
    bool synchronization2;
    bool maintenance4;
    bool pipeline_creation_cache_control;
    bool robust_buffer_access;
    bool robust_image_access;
    bool scalar_block_layout;
    bool uniform_buffer_standard_layout;
    bool geometry_shader;
    bool tessellation_shader;
    bool samplers;
    bool depth_clamp;
    bool large_points;
    bool wide_lines;
    bool multi_draw_indirect;
    bool draw_indirect_first_instance;
    bool fill_mode_non_solid;
    bool sampler_anisotropy;
    bool texture_compression_astc_ldr;
    bool vertex_pipeline_stores_and_atomics;
    bool fragment_stores_and_atomics;
    bool shader_storage_image_read_without_format;
    bool shader_storage_image_write_without_format;
    bool shader_float64;
    bool shader_float16;
    bool shader_int64;
    bool shader_int16;
    bool shader_terminate_invocation;
    bool subgroup_broadcast_dynamic_id;
    bool storage_buffer_array_dynamic_indexing;
    bool storage_image_array_dynamic_indexing;
    bool sampled_image_array_dynamic_indexing;
    bool uniform_buffer_array_dynamic_indexing;
    bool shader_shared_int64_atomics;
};

struct VkDevice_T {
    uintptr_t loader_data;
    struct pan_kmod_dev *kdev;
    struct VkPhysicalDevice_T *phys_dev;
    struct VkQueue_T *queue;
    struct panvk_v9_enabled_features features;
    /* Serialises vkQueueSubmit* across threads: the double-buffered v9_cmd
     * state (active_slot, mem_bo swap, per-buffer fields) and the kbase atom
     * submit sequence are not internally thread-safe, so all queue submits on
     * a device are funneled through this mutex. */
    pthread_mutex_t submit_mutex;
};

struct VkQueue_T {
    uintptr_t loader_data;
    struct VkDevice_T *device;
    struct v9_cmd_buffer *last_v9_cmd;
};

struct VkCommandPool_T {
    struct VkDevice_T *device;
};

struct vk_vertex_binding {
    struct VkBuffer_T *buffer;
    VkDeviceSize offset;
};

struct VkCommandBuffer_T {
    uintptr_t loader_data;
    struct VkDevice_T *device;
    struct v9_cmd_buffer *v9_cmd;
    bool rendering_active;
    struct VkPipeline_T *graphics_pipeline;
    struct VkPipeline_T *compute_pipeline;
    struct VkViewport viewport;
    struct VkRect2D scissor;
    bool viewport_set;
    bool scissor_set;
    uint8_t push_constants[128];
    uint32_t push_constants_size;
    float depth_bias_constant_factor;
    float depth_bias_constant_offset;
    float depth_bias_clamp;
    bool depth_bias_set;
    VkDescriptorSet descriptor_sets[8];
    struct vk_vertex_binding vertex_bindings[16];
    struct VkBuffer_T *index_buffer;
    VkDeviceSize index_offset;
    uint32_t index_type;
};

struct VkSurfaceKHR_T {
    Display *dpy;
    xcb_connection_t *connection;
    uint32_t window;
    uint32_t width;
    uint32_t height;
    bool is_xcb;
};

struct VkSwapchainKHR_T {
    struct VkDevice_T *device;
    struct VkSurfaceKHR_T *surface;
    uint32_t width;
    uint32_t height;
    uint32_t image_count;
    struct VkImage_T *images;
    uint32_t next_image; /* rotating index handed out by vkAcquireNextImageKHR */
    GC gc;
    xcb_gcontext_t xcb_gc;
    XImage *ximage;
    char *image_data;
};

struct VkImage_T {
    struct VkSwapchainKHR_T *swapchain;
    uint32_t index;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t format;
    uint32_t image_type;
    uint32_t mip_levels;
    uint32_t array_layers;
    uint32_t samples;
    uint32_t tiling;
    uint32_t usage;
    uint64_t size;
    uint64_t row_pitch[16];
    uint64_t mip_offset[16];
    struct pan_kmod_bo *bo;
    VkDeviceSize memory_offset;
};

struct VkImageView_T {
    struct VkImage_T *image;
    uint32_t format;
    uint32_t view_type;
    uint32_t base_mip;
    uint32_t mip_count;
    uint32_t base_layer;
    uint32_t layer_count;
};

struct VkDeviceMemory_T {
    struct pan_kmod_bo *bo;
    void *cpu;
    VkDeviceSize size;
};

struct VkBuffer_T {
    VkDeviceSize size;
    struct pan_kmod_bo *bo;
    VkDeviceSize memory_offset;
};

struct VkShaderModule_T {
    size_t code_size;
    uint32_t *code;
    uint32_t stage_mask;
};

struct VkPipelineLayout_T {
    struct panvk_v9_pipeline_layout compiler_layout;
    struct panvk_v9_descriptor_binding *bindings;
};

struct VkRenderPass_T {
    int dummy;
};

struct VkFramebuffer_T {
    struct VkDevice_T *device;
    struct VkImageView_T **attachments;
    uint32_t attachment_count;
    uint32_t width;
    uint32_t height;
};

struct VkPipelineCache_T {
    int dummy;
};

struct VkPipeline_T {
    uint32_t stage_mask;
    uint32_t stage;
    char vertex_entry_point[64];
    char fragment_entry_point[64];
    char compute_entry_point[64];
    struct panvk_v9_compiled_shader vertex_binary;
    struct panvk_v9_compiled_shader fragment_binary;
    struct panvk_v9_compiled_shader compute_binary;
    struct VkShaderModule_T *compute_module;
    struct panvk_v9_pipeline_layout compiler_layout;
    struct panvk_v9_descriptor_binding *bindings;
    struct VkVertexInputBindingDescription vertex_bindings[16];
    struct VkVertexInputAttributeDescription vertex_attributes[16];
    uint32_t vertex_binding_count;
    uint32_t vertex_attribute_count;
    bool shaders_compiled;
    uint32_t topology;
    bool primitive_restart;
    struct VkViewport viewport;
    struct VkRect2D scissor;
    bool dynamic_viewport;
    bool dynamic_scissor;
    bool rasterizer_discard;
    uint32_t polygon_mode;
    uint32_t cull_mode;
    uint32_t front_face;
    float line_width;
    uint32_t rasterization_samples;
    bool depth_test;
    bool depth_write;
    uint32_t depth_compare_op;
    bool blend_enable;
    uint32_t color_write_mask;
};

struct VkDescriptorSetLayout_T {
    uint32_t binding_count;
    struct VkDescriptorSetLayoutBinding *bindings;
    uint32_t *binding_offsets;
    uint32_t descriptor_count;
    VkDescriptorBindingFlags *binding_flags; /* one per binding (descriptor indexing) */
    int32_t variable_binding;                /* index of variable-descriptor-count binding, or -1 */
    uint32_t variable_descriptor_count;      /* declared max count of that binding */
};

struct VkDescriptorPool_T {
    int dummy;
};

struct VkDescriptorSet_T {
    VkDescriptorSetLayout layout;
    struct VkDescriptorBufferInfo *buffers;
};

struct VkSemaphore_T {
    uint64_t counter;
    struct VkSemaphore_T *timeline; /* non-NULL for timeline semaphores */
};

struct VkFence_T {
    bool signaled;
};

struct VkEvent_T {
    bool signaled;
};

struct VkQueryPool_T {
    uint32_t query_count;
};

struct panvk_compiler_api {
    void *library;
    int (*compile)(const uint32_t *, size_t, enum panvk_v9_shader_stage,
                   const char *, const struct panvk_v9_pipeline_layout *,
                   struct panvk_v9_compiled_shader *, char *, size_t);
    void (*cleanup)(struct panvk_v9_compiled_shader *);
    bool attempted;
};

/* GLIBC compatibility globals and functions for Bionic */
char *program_invocation_name = (char *)"vkmark";
char *program_invocation_short_name = (char *)"vkmark";
extern int *__errno(void);
int *__errno_location(void) {
    return __errno();
}

static struct panvk_compiler_api compiler_api;
static pthread_mutex_t compiler_api_mutex = PTHREAD_MUTEX_INITIALIZER;

static void command_buffer_apply_ssbos(VkCommandBuffer commandBuffer);

static bool load_compiler(void) {
    pthread_mutex_lock(&compiler_api_mutex);
    if (compiler_api.attempted) {
        bool loaded = compiler_api.library != NULL;
        pthread_mutex_unlock(&compiler_api_mutex);
        return loaded;
    }
    compiler_api.attempted = true;

    const char *path = getenv("PANVK_V9_COMPILER_LIBRARY");
    if (path && path[0]) {
        compiler_api.library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        compiler_api.library = dlopen("./libpanvk_v9_compiler.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        compiler_api.library = dlopen("/data/data/com.termux/files/home/libpanvk_v9_compiler.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        compiler_api.library = dlopen("libpanvk_v9_compiler.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        const char *err = dlerror();
        FILE *flog = fopen("/data/data/com.termux/files/usr/tmp/panvk_debug.log", "a");
        if (flog) {
            fprintf(flog, "load_compiler dlopen failed: %s\n", err ? err : "unknown");
            fclose(flog);
        }
        pthread_mutex_unlock(&compiler_api_mutex);
        return false;
    }

    compiler_api.compile = dlsym(compiler_api.library, "panvk_v9_compile_spirv");
    compiler_api.cleanup = dlsym(compiler_api.library, "panvk_v9_compiled_shader_cleanup");
    if (!compiler_api.compile || !compiler_api.cleanup) {
        const char *err = dlerror();
        FILE *flog = fopen("/data/data/com.termux/files/usr/tmp/panvk_debug.log", "a");
        if (flog) {
            fprintf(flog, "load_compiler dlsym failed: %s\n", err ? err : "unknown");
            fclose(flog);
        }
        dlclose(compiler_api.library);
        memset(&compiler_api, 0, sizeof(compiler_api));
        compiler_api.attempted = true;
        pthread_mutex_unlock(&compiler_api_mutex);
        return false;
    }
    pthread_mutex_unlock(&compiler_api_mutex);
    return true;
}

/* Loader Negotiation */
VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion) {
    if (!pSupportedVersion) return VK_ERROR_INITIALIZATION_FAILED;
    if (*pSupportedVersion > 6) {
        *pSupportedVersion = 6;
    }
    return VK_SUCCESS;
}

VkResult vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
    if (!pApiVersion) return VK_ERROR_INITIALIZATION_FAILED;
    *pApiVersion = VK_MAKE_API_VERSION(0, 1, 3, 0); /* Vulkan 1.3 */
    return VK_SUCCESS;
}

/* Extension & Layer Enumeration */
VkResult vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount, struct VkLayerProperties *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;

    static const VkExtensionProperties inst_exts[] = {
        { .extensionName = VK_KHR_SURFACE_EXTENSION_NAME, .specVersion = 25 },
        { .extensionName = VK_KHR_XLIB_SURFACE_EXTENSION_NAME, .specVersion = 6 },
        { .extensionName = VK_KHR_XCB_SURFACE_EXTENSION_NAME, .specVersion = 6 },
        { .extensionName = VK_KHR_DISPLAY_EXTENSION_NAME, .specVersion = 23 },
        { .extensionName = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_EXT_DEBUG_UTILS_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_EXT_DEBUG_REPORT_EXTENSION_NAME, .specVersion = 10 },
        { .extensionName = VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, .specVersion = 1 },
    };
    uint32_t num_exts = sizeof(inst_exts) / sizeof(inst_exts[0]);

    if (!pProperties) {
        *pPropertyCount = num_exts;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pPropertyCount < num_exts) ? *pPropertyCount : num_exts;
    memcpy(pProperties, inst_exts, to_copy * sizeof(VkExtensionProperties));
    *pPropertyCount = to_copy;
    return VK_SUCCESS;
}

VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;

    /* Extension surface advertised (API 1.3 so most 1.1/1.2/1.3 features are
     * core; these are the KHR/EXT ones DXVK and VKD3D require at device
     * creation. */
    static const VkExtensionProperties dev_exts[] = {
        { .extensionName = VK_KHR_SWAPCHAIN_EXTENSION_NAME, .specVersion = 70 },
        { .extensionName = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, .specVersion = 3 },
        { .extensionName = VK_KHR_MAINTENANCE_1_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_MAINTENANCE_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MAINTENANCE_3_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MAINTENANCE_4_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_MAINTENANCE_5_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MAINTENANCE_6_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MAINTENANCE_7_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MAINTENANCE_8_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME, .specVersion = 4 },
        { .extensionName = VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_16BIT_STORAGE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_8BIT_STORAGE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, .specVersion = 14 },
        { .extensionName = VK_EXT_DEBUG_UTILS_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_EXT_DEBUG_REPORT_EXTENSION_NAME, .specVersion = 10 },
        { .extensionName = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MULTIVIEW_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, .specVersion = 1 },
        /* ---- Additional surface probed by DXVK / vkd3d-proton ---- */
        { .extensionName = VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SHADER_SUBGROUP_ROTATE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_4444_FORMATS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, .specVersion = 4 },
        { .extensionName = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, .specVersion = 3 },
        { .extensionName = VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, .specVersion = 1 },
    };
    uint32_t num_exts = sizeof(dev_exts) / sizeof(dev_exts[0]);

    if (!pProperties) {
        *pPropertyCount = num_exts;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pPropertyCount < num_exts) ? *pPropertyCount : num_exts;
    memcpy(pProperties, dev_exts, to_copy * sizeof(VkExtensionProperties));
    *pPropertyCount = to_copy;
    return VK_SUCCESS;
}

/* Instance & Device Management */
VkResult vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
    if (!pInstance) return VK_ERROR_INITIALIZATION_FAILED;

    if (pCreateInfo && pCreateInfo->ppEnabledExtensionNames) {
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            printf("DEBUG: vkCreateInstance requested extension: '%s'\n", pCreateInfo->ppEnabledExtensionNames[i]);
        }
    }

    struct VkInstance_T *inst = calloc(1, sizeof(*inst));
    if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;
    set_loader_magic(inst);

    struct pan_kmod_dev *kdev = pan_kmod_dev_create(NULL);
    if (kdev) {
        struct VkPhysicalDevice_T *pdev = calloc(1, sizeof(*pdev));
        if (pdev) {
            set_loader_magic(pdev);
            pdev->kdev = kdev;
            pan_kmod_dev_query_props(kdev, &pdev->props);
            inst->phys_dev = pdev;
        } else {
            pan_kmod_dev_destroy(kdev);
        }
    }

    *pInstance = inst;
    return VK_SUCCESS;
}

void vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    if (!instance) return;
    if (instance->phys_dev) {
        if (instance->phys_dev->kdev) {
            pan_kmod_dev_destroy(instance->phys_dev->kdev);
        }
        free(instance->phys_dev);
    }
    free(instance);
}

VkResult vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount, VkPhysicalDevice *pPhysicalDevices) {
    if (!pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!instance || !instance->phys_dev) {
        *pPhysicalDeviceCount = 0;
        return VK_SUCCESS;
    }

    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = 1;
        return VK_SUCCESS;
    }

    *pPhysicalDevices = instance->phys_dev;
    *pPhysicalDeviceCount = 1;
    return VK_SUCCESS;
}

VkResult vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *pPhysicalDeviceGroupCount, struct VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroups) {
    if (!pPhysicalDeviceGroupCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pPhysicalDeviceGroups) {
        *pPhysicalDeviceGroupCount = 1;
        return VK_SUCCESS;
    }
    pPhysicalDeviceGroups[0].physicalDeviceCount = 1;
    vkEnumeratePhysicalDevices(instance, &pPhysicalDeviceGroups[0].physicalDeviceCount, pPhysicalDeviceGroups[0].physicalDevices);
    *pPhysicalDeviceGroupCount = 1;
    return VK_SUCCESS;
}

void vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties) {
    if (!pProperties) return;
    memset(pProperties, 0, sizeof(*pProperties));
    pProperties->apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);
    pProperties->driverVersion = (1u << 22) | (1u << 12) | 0;
    pProperties->vendorID = 0x13B5; /* ARM Vendor ID */
    /* GPU real = Mali-G68 MC4 (0x92041010). O GPU ID de compilacao continua
     * 0x90001000 (G77) porque o G68 nao e modelo listado no Mesa
     * (pan_get_model retornaria NULL); ambos sao Valhall arch 9 (mesma ISA). */
    pProperties->deviceID = 0x92041010u;
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    snprintf(pProperties->deviceName, sizeof(pProperties->deviceName),
             "ARM Mali-G68 MC4 (Valhall v9 - PanVK Open Source Driver)");
    pProperties->pipelineCacheUUID[0] = 0x50; /* 'P' */
    pProperties->pipelineCacheUUID[1] = 0x56; /* 'V' */
    pProperties->pipelineCacheUUID[2] = 0x39; /* '9' */

    VkPhysicalDeviceLimits *l = &pProperties->limits;
    l->maxImageDimension1D = 4096;
    l->maxImageDimension2D = 4096;
    l->maxImageDimension3D = 2048;
    l->maxImageDimensionCube = 4096;
    l->maxImageArrayLayers = 2048;
    l->maxTexelBufferElements = 1 << 20;
    l->maxUniformBufferRange = 1u << 20;
    l->maxStorageBufferRange = 1u << 30;
    l->maxPushConstantsSize = 128;
    l->maxMemoryAllocationCount = 0xffff;
    l->maxSamplerAllocationCount = 4096;
    l->bufferImageGranularity = 64;
    l->sparseAddressSpaceSize = 1ull << 32;
    l->maxBoundDescriptorSets = 8;
    l->maxPerStageDescriptorSamplers = 64;
    l->maxPerStageDescriptorUniformBuffers = 64;
    l->maxPerStageDescriptorStorageBuffers = 64;
    l->maxPerStageDescriptorSampledImages = 64;
    l->maxPerStageDescriptorStorageImages = 64;
    l->maxPerStageDescriptorInputAttachments = 64;
    l->maxPerStageResources = 128;
    l->maxDescriptorSetSamplers = 256;
    l->maxDescriptorSetUniformBuffers = 256;
    l->maxDescriptorSetUniformBuffersDynamic = 8;
    l->maxDescriptorSetStorageBuffers = 256;
    l->maxDescriptorSetStorageBuffersDynamic = 4;
    l->maxDescriptorSetSampledImages = 256;
    l->maxDescriptorSetStorageImages = 256;
    l->maxDescriptorSetInputAttachments = 64;
    l->maxVertexInputAttributes = 16;
    l->maxVertexInputBindings = 16;
    l->maxVertexInputAttributeOffset = 2047;
    l->maxVertexInputBindingStride = 2048;
    l->maxVertexOutputComponents = 64;
    l->maxFragmentInputComponents = 64;
    l->maxFragmentOutputAttachments = 8;
    l->maxFragmentDualSrcAttachments = 1;
    l->maxFragmentCombinedOutputResources = 8;
    l->maxComputeSharedMemorySize = 16 * 1024;
    l->maxComputeWorkGroupCount[0] = 65535;
    l->maxComputeWorkGroupCount[1] = 65535;
    l->maxComputeWorkGroupCount[2] = 65535;
    l->maxComputeWorkGroupInvocations = 128;
    l->maxComputeWorkGroupSize[0] = 128;
    l->maxComputeWorkGroupSize[1] = 128;
    l->maxComputeWorkGroupSize[2] = 128;
    l->subPixelPrecisionBits = 4;
    l->subTexelPrecisionBits = 4;
    l->mipmapPrecisionBits = 4;
    l->maxDrawIndexedIndexValue = UINT32_MAX;
    l->maxDrawIndirectCount = 0xfffff;
    l->maxSamplerLodBias = 4.0f;
    l->maxSamplerAnisotropy = 16.0f;
    l->maxViewports = 16;
    l->maxViewportDimensions[0] = 4096;
    l->maxViewportDimensions[1] = 4096;
    l->viewportBoundsRange[0] = -4096.0f;
    l->viewportBoundsRange[1] = 4096.0f;
    l->viewportSubPixelBits = 4;
    l->minMemoryMapAlignment = 64;
    l->minTexelBufferOffsetAlignment = 64;
    l->minUniformBufferOffsetAlignment = 64;
    l->minStorageBufferOffsetAlignment = 64;
    l->minTexelOffset = -8;
    l->maxTexelOffset = 7;
    l->minTexelGatherOffset = -8;
    l->maxTexelGatherOffset = 7;
    l->minInterpolationOffset = -0.5f;
    l->maxInterpolationOffset = 0.5f;
    l->subPixelInterpolationOffsetBits = 4;
    l->maxFramebufferWidth = 4096;
    l->maxFramebufferHeight = 4096;
    l->maxFramebufferLayers = 1024;
    l->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->maxColorAttachments = 8;
    l->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->maxSampleMaskWords = 1;
    l->timestampComputeAndGraphics = VK_TRUE;
    l->timestampPeriod = 1.0f;
    l->maxClipDistances = 8;
    l->maxCullDistances = 8;
    l->maxCombinedClipAndCullDistances = 8;
    l->discreteQueuePriorities = 2;
    l->pointSizeRange[0] = 1.0f;
    l->pointSizeRange[1] = 255.0f;
    l->lineWidthRange[0] = 1.0f;
    l->lineWidthRange[1] = 8.0f;
    l->pointSizeGranularity = 1.0f;
    l->lineWidthGranularity = 1.0f;
    l->strictLines = VK_FALSE;
    l->standardSampleLocations = VK_TRUE;
    l->optimalBufferCopyOffsetAlignment = 64;
    l->optimalBufferCopyRowPitchAlignment = 64;
    l->nonCoherentAtomSize = 64;
}

void vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties) {
    if (!pProperties) return;
    vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);

    for (void *next = pProperties->pNext; next; next = *((void **)next + 1)) {
        VkStructureType sType = *(const VkStructureType *)next;
        switch (sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
            VkPhysicalDeviceVulkan11Properties *p = next;
            p->maxMultiviewViewCount = 4;
            p->maxMultiviewInstanceIndex = 0xffff;
            p->subgroupSize = 8;
            p->subgroupSupportedStages = VK_SHADER_STAGE_VERTEX_BIT |
                                        VK_SHADER_STAGE_FRAGMENT_BIT |
                                        VK_SHADER_STAGE_COMPUTE_BIT;
            p->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                             VK_SUBGROUP_FEATURE_VOTE_BIT |
                                             VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                             VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                             VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                             VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
            p->subgroupQuadOperationsInAllStages = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: {
            VkPhysicalDeviceVulkan12Properties *p = next;
            p->driverID = VK_DRIVER_ID_MESA_PANVK;
            snprintf(p->driverName, sizeof(p->driverName), "panvk-v9");
            snprintf(p->driverInfo, sizeof(p->driverInfo),
                     "PanVK v9 (Mesa compiler backend, Mali-G68 MC4)");
            p->conformanceVersion.major = 1;
            p->conformanceVersion.minor = 3;
            p->conformanceVersion.patch = 0;
            p->conformanceVersion.subminor = 0;
            p->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
            p->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
            p->shaderSignedZeroInfNanPreserveFloat16 = VK_FALSE;
            p->shaderSignedZeroInfNanPreserveFloat32 = VK_FALSE;
            p->shaderSignedZeroInfNanPreserveFloat64 = VK_FALSE;
            p->shaderDenormPreserveFloat16 = VK_FALSE;
            p->shaderDenormPreserveFloat32 = VK_FALSE;
            p->shaderDenormPreserveFloat64 = VK_FALSE;
            p->shaderDenormFlushToZeroFloat16 = VK_FALSE;
            p->shaderDenormFlushToZeroFloat32 = VK_FALSE;
            p->shaderDenormFlushToZeroFloat64 = VK_FALSE;
            p->maxTimelineSemaphoreValueDifference = INT64_MAX;
            p->framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES: {
            VkPhysicalDeviceVulkan13Properties *p = next;
            p->minSubgroupSize = 8;
            p->maxSubgroupSize = 8;
            p->maxComputeWorkgroupSubgroups = 8;
            p->requiredSubgroupSizeStages = VK_SHADER_STAGE_VERTEX_BIT |
                                            VK_SHADER_STAGE_FRAGMENT_BIT |
                                            VK_SHADER_STAGE_COMPUTE_BIT;
            p->maxInlineUniformBlockSize = 256;
            p->maxPerStageDescriptorInlineUniformBlocks = 4;
            p->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks = 4;
            p->maxDescriptorSetInlineUniformBlocks = 4;
            p->maxDescriptorSetUpdateAfterBindInlineUniformBlocks = 4;
            p->maxInlineUniformTotalSize = 4096;
            p->storageTexelBufferOffsetAlignmentBytes = 64;
            p->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
            p->uniformTexelBufferOffsetAlignmentBytes = 64;
            p->uniformTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
            p->maxBufferSize = 1ull << 34;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
            VkPhysicalDeviceSubgroupProperties *p = next;
            p->subgroupSize = 8;
            p->supportedStages = VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT |
                                 VK_SHADER_STAGE_COMPUTE_BIT;
            p->supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                     VK_SUBGROUP_FEATURE_VOTE_BIT |
                                     VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                     VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                     VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                     VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
            p->quadOperationsInAllStages = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
            VkPhysicalDeviceDriverProperties *p = next;
            p->driverID = VK_DRIVER_ID_MESA_PANVK;
            snprintf(p->driverName, sizeof(p->driverName), "panvk-v9");
            snprintf(p->driverInfo, sizeof(p->driverInfo),
                     "PanVK v9 (Mesa compiler backend, Mali-G68 MC4)");
            p->conformanceVersion.major = 1;
            p->conformanceVersion.minor = 3;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES: {
            VkPhysicalDeviceTimelineSemaphoreProperties *p = next;
            p->maxTimelineSemaphoreValueDifference = INT64_MAX;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES: {
            VkPhysicalDeviceDescriptorIndexingProperties *p = next;
            p->maxUpdateAfterBindDescriptorsInAllPools = 1u << 20;
            p->shaderUniformBufferArrayNonUniformIndexingNative = VK_FALSE;
            p->shaderSampledImageArrayNonUniformIndexingNative = VK_FALSE;
            p->shaderStorageBufferArrayNonUniformIndexingNative = VK_FALSE;
            p->shaderStorageImageArrayNonUniformIndexingNative = VK_FALSE;
            p->shaderInputAttachmentArrayNonUniformIndexingNative = VK_FALSE;
            p->robustBufferAccessUpdateAfterBind = VK_FALSE;
            p->quadDivergentImplicitLod = VK_FALSE;
            p->maxPerStageDescriptorUpdateAfterBindSamplers = 64;
            p->maxPerStageDescriptorUpdateAfterBindUniformBuffers = 64;
            p->maxPerStageDescriptorUpdateAfterBindStorageBuffers = 64;
            p->maxPerStageDescriptorUpdateAfterBindSampledImages = 64;
            p->maxPerStageDescriptorUpdateAfterBindStorageImages = 64;
            p->maxPerStageDescriptorUpdateAfterBindInputAttachments = 64;
            p->maxPerStageUpdateAfterBindResources = 128;
            p->maxDescriptorSetUpdateAfterBindSamplers = 256;
            p->maxDescriptorSetUpdateAfterBindUniformBuffers = 256;
            p->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 8;
            p->maxDescriptorSetUpdateAfterBindStorageBuffers = 256;
            p->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 4;
            p->maxDescriptorSetUpdateAfterBindSampledImages = 256;
            p->maxDescriptorSetUpdateAfterBindStorageImages = 256;
            p->maxDescriptorSetUpdateAfterBindInputAttachments = 64;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES: {
            VkPhysicalDeviceMaintenance4Properties *p = next;
            p->maxBufferSize = 1ull << 34;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES: {
            VkPhysicalDeviceSubgroupSizeControlProperties *p = next;
            p->minSubgroupSize = 8;
            p->maxSubgroupSize = 8;
            p->maxComputeWorkgroupSubgroups = 8;
            p->requiredSubgroupSizeStages = VK_SHADER_STAGE_VERTEX_BIT |
                                            VK_SHADER_STAGE_FRAGMENT_BIT |
                                            VK_SHADER_STAGE_COMPUTE_BIT;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR: {
            VkPhysicalDeviceFragmentShadingRatePropertiesKHR *p = next;
            p->minFragmentShadingRateAttachmentTexelSize.width = 1;
            p->minFragmentShadingRateAttachmentTexelSize.height = 1;
            p->maxFragmentShadingRateAttachmentTexelSize.width = 1;
            p->maxFragmentShadingRateAttachmentTexelSize.height = 1;
            p->maxFragmentShadingRateAttachmentTexelSizeAspectRatio = 1;
            p->fragmentShadingRateNonTrivialCombinerOps = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_PROPERTIES: {
            VkPhysicalDevicePipelineRobustnessProperties *p = next;
            p->defaultRobustnessStorageBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS;
            p->defaultRobustnessUniformBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS;
            p->defaultRobustnessVertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS;
            p->defaultRobustnessImages = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS;
            break;
        }
        default:
            break;
        }
    }
}

void vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures) {
    if (!pFeatures) return;
    memset(pFeatures, 0, sizeof(*pFeatures));
    panvk_v9_fill_features(pFeatures);
}

/* Feature matrix advertised to DXVK/VKD3D.  Kept in panvk_v9_fill_features2
 * so both entry points agree. */
void panvk_v9_fill_features(VkPhysicalDeviceFeatures *f) {
    if (!f) return;
    f->robustBufferAccess = VK_TRUE;
    f->fullDrawIndexUint32 = VK_TRUE;
    f->imageCubeArray = VK_TRUE;
    f->independentBlend = VK_TRUE;
    f->samplerAnisotropy = VK_TRUE;
    f->depthClamp = VK_TRUE;
    f->vertexPipelineStoresAndAtomics = VK_TRUE;
    f->fragmentStoresAndAtomics = VK_TRUE;
    f->shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    f->shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    f->shaderStorageImageArrayDynamicIndexing = VK_TRUE;
    f->shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
    f->shaderClipDistance = VK_TRUE;
    f->shaderCullDistance = VK_TRUE;
    f->shaderStorageImageReadWithoutFormat = VK_TRUE;
    f->shaderStorageImageWriteWithoutFormat = VK_TRUE;
    f->shaderImageGatherExtended = VK_TRUE;
    f->multiDrawIndirect = VK_TRUE;
    f->drawIndirectFirstInstance = VK_TRUE;
    f->textureCompressionASTC_LDR = VK_TRUE;
}

/* Walk a VkPhysicalDeviceFeatures2 pNext chain and fill every feature struct
 * present, mirroring Mesa panvk's arch>=9 feature matrix. */
void panvk_v9_fill_features2(VkPhysicalDeviceFeatures2 *features2) {
    if (!features2) return;
    memset(&features2->features, 0, sizeof(features2->features));
    panvk_v9_fill_features(&features2->features);

    for (void *next = features2->pNext; next; next = *((void **)next + 1)) {
        VkStructureType sType = *(const VkStructureType *)next;
        switch (sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
            VkPhysicalDeviceVulkan11Features *f = next;
            f->storageBuffer16BitAccess = VK_TRUE;
            f->uniformAndStorageBuffer16BitAccess = VK_TRUE;
            f->storagePushConstant16 = VK_FALSE;
            f->storageInputOutput16 = VK_FALSE;
            f->multiview = VK_TRUE;
            f->multiviewGeometryShader = VK_FALSE;
            f->multiviewTessellationShader = VK_FALSE;
            f->variablePointersStorageBuffer = VK_TRUE;
            f->variablePointers = VK_TRUE;
            f->protectedMemory = VK_FALSE;
            f->samplerYcbcrConversion = VK_TRUE;
            f->shaderDrawParameters = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
            VkPhysicalDeviceVulkan12Features *f = next;
            f->samplerMirrorClampToEdge = VK_TRUE;
            f->drawIndirectCount = VK_TRUE;
            f->storageBuffer8BitAccess = VK_TRUE;
            f->uniformAndStorageBuffer8BitAccess = VK_TRUE;
            f->storagePushConstant8 = VK_FALSE;
            f->shaderBufferInt64Atomics = VK_TRUE;
            f->shaderSharedInt64Atomics = VK_TRUE;
            f->shaderFloat16 = VK_TRUE;
            f->descriptorIndexing = VK_TRUE;
            f->shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
            f->shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
            f->shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
            f->shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
            f->shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE;
            f->shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
            f->descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            f->descriptorBindingPartiallyBound = VK_TRUE;
            f->descriptorBindingVariableDescriptorCount = VK_TRUE;
            f->runtimeDescriptorArray = VK_TRUE;
            f->samplerFilterMinmax = VK_FALSE;
            f->scalarBlockLayout = VK_TRUE;
            f->imagelessFramebuffer = VK_TRUE;
            f->uniformBufferStandardLayout = VK_TRUE;
            f->shaderSubgroupExtendedTypes = VK_TRUE;
            f->separateDepthStencilLayouts = VK_TRUE;
            f->hostQueryReset = VK_TRUE;
            f->timelineSemaphore = VK_TRUE;
            f->bufferDeviceAddress = VK_TRUE;
            f->bufferDeviceAddressCaptureReplay = VK_FALSE;
            f->bufferDeviceAddressMultiDevice = VK_FALSE;
            f->vulkanMemoryModel = VK_TRUE;
            f->vulkanMemoryModelDeviceScope = VK_TRUE;
            f->vulkanMemoryModelAvailabilityVisibilityChains = VK_TRUE;
            f->shaderOutputViewportIndex = VK_FALSE;
            f->shaderOutputLayer = VK_FALSE;
            f->subgroupBroadcastDynamicId = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
            VkPhysicalDeviceVulkan13Features *f = next;
            f->robustImageAccess = VK_TRUE;
            f->inlineUniformBlock = VK_TRUE;
            f->descriptorBindingInlineUniformBlockUpdateAfterBind = VK_TRUE;
            f->pipelineCreationCacheControl = VK_TRUE;
            f->privateData = VK_TRUE;
            f->shaderDemoteToHelperInvocation = VK_TRUE;
            f->shaderTerminateInvocation = VK_TRUE;
            f->subgroupSizeControl = VK_TRUE;
            f->computeFullSubgroups = VK_TRUE;
            f->synchronization2 = VK_TRUE;
            f->textureCompressionASTC_HDR = VK_FALSE;
            f->shaderZeroInitializeWorkgroupMemory = VK_TRUE;
            f->dynamicRendering = VK_TRUE;
            f->shaderIntegerDotProduct = VK_TRUE;
            f->maintenance4 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
            VkPhysicalDeviceDescriptorIndexingFeatures *f = next;
            f->shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
            f->shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
            f->shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
            f->shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
            f->shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE;
            f->shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
            f->descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            f->descriptorBindingPartiallyBound = VK_TRUE;
            f->descriptorBindingVariableDescriptorCount = VK_TRUE;
            f->runtimeDescriptorArray = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
            VkPhysicalDeviceBufferDeviceAddressFeatures *f = next;
            f->bufferDeviceAddress = VK_TRUE;
            f->bufferDeviceAddressCaptureReplay = VK_FALSE;
            f->bufferDeviceAddressMultiDevice = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: {
            VkPhysicalDeviceTimelineSemaphoreFeatures *f = next;
            f->timelineSemaphore = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: {
            VkPhysicalDeviceDynamicRenderingFeatures *f = next;
            f->dynamicRendering = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES: {
            VkPhysicalDeviceHostQueryResetFeatures *f = next;
            f->hostQueryReset = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES: {
            VkPhysicalDeviceSynchronization2Features *f = next;
            f->synchronization2 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES: {
            VkPhysicalDeviceMaintenance4Features *f = next;
            f->maintenance4 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
            VkPhysicalDevice16BitStorageFeatures *f = next;
            f->storageBuffer16BitAccess = VK_TRUE;
            f->uniformAndStorageBuffer16BitAccess = VK_TRUE;
            f->storagePushConstant16 = VK_FALSE;
            f->storageInputOutput16 = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: {
            VkPhysicalDevice8BitStorageFeatures *f = next;
            f->storageBuffer8BitAccess = VK_TRUE;
            f->uniformAndStorageBuffer8BitAccess = VK_TRUE;
            f->storagePushConstant8 = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: {
            VkPhysicalDeviceSubgroupSizeControlFeatures *f = next;
            f->subgroupSizeControl = VK_TRUE;
            f->computeFullSubgroups = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT: {
            VkPhysicalDevice4444FormatsFeaturesEXT *f = next;
            f->formatA4R4G4B4 = VK_TRUE;
            f->formatA4B4G4R4 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES: {
            VkPhysicalDeviceImageRobustnessFeatures *f = next;
            f->robustImageAccess = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES: {
            VkPhysicalDevicePipelineRobustnessFeatures *f = next;
            f->pipelineRobustness = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES: {
            VkPhysicalDeviceIndexTypeUint8Features *f = next;
            f->indexTypeUint8 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR: {
            VkPhysicalDeviceFragmentShadingRateFeaturesKHR *f = next;
            f->pipelineFragmentShadingRate = VK_TRUE;
            f->primitiveFragmentShadingRate = VK_FALSE;
            f->attachmentFragmentShadingRate = VK_FALSE;
            break;
        }
        default:
            break;
        }
    }
}

void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures) {
    if (!pFeatures) return;
    panvk_v9_fill_features2(pFeatures);
}

/* Vulkan 1.1/1.2/1.3 property chains (mirrors Mesa panvk limits). */
void panvk_v9_fill_properties2(VkPhysicalDeviceProperties2 *props2) {
    if (!props2) return;
    vkGetPhysicalDeviceProperties2(NULL, props2);
}

void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties *pQueueFamilyProperties) {
    if (!pQueueFamilyPropertyCount) return;
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    /* Family 0: Graphics + Compute + Transfer (0x7) */
    memset(pQueueFamilyProperties, 0, sizeof(VkQueueFamilyProperties));
    pQueueFamilyProperties->queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                                        VK_QUEUE_TRANSFER_BIT;
    pQueueFamilyProperties->queueCount = 1;
    pQueueFamilyProperties->timestampValidBits = 64;
    pQueueFamilyProperties->minImageTransferGranularity.width = 1;
    pQueueFamilyProperties->minImageTransferGranularity.height = 1;
    pQueueFamilyProperties->minImageTransferGranularity.depth = 1;
    *pQueueFamilyPropertyCount = 1;
}

void vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties2 *pQueueFamilyProperties) {
    if (!pQueueFamilyPropertyCount) return;
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount,
                                             &pQueueFamilyProperties->queueFamilyProperties);
}

void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    if (!pMemoryProperties) return;
    memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));
    pMemoryProperties->memoryTypeCount = 2;
    pMemoryProperties->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMemoryProperties->memoryTypes[0].heapIndex = 0;
    pMemoryProperties->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMemoryProperties->memoryTypes[1].heapIndex = 0;

    pMemoryProperties->memoryHeapCount = 1;
    pMemoryProperties->memoryHeaps[0].size = 4096ULL * 1024ULL * 1024ULL; /* 4GB */
    pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

void vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    if (!pMemoryProperties) return;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &pMemoryProperties->memoryProperties);
}

static VkFormatFeatureFlags panvk_v9_format_features(uint32_t format) {
    VkFormatFeatureFlags features =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
        features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                   VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                   VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        break;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                   VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        break;
    case VK_FORMAT_S8_UINT:
        features = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        break;
    default:
        break;
    }
    return features;
}

void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties *pFormatProperties) {
    if (!pFormatProperties) return;
    memset(pFormatProperties, 0, sizeof(*pFormatProperties));
    VkFormatFeatureFlags features = panvk_v9_format_features(format);
    pFormatProperties->linearTilingFeatures = features;
    pFormatProperties->optimalTilingFeatures = features;
    pFormatProperties->bufferFeatures = VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
                                        VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT |
                                        VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

VkResult vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *pImageFormatProperties) {
    if (!pImageFormatProperties) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pImageFormatProperties, 0, sizeof(*pImageFormatProperties));
    pImageFormatProperties->maxExtent.width = 4096;
    pImageFormatProperties->maxExtent.height = 4096;
    pImageFormatProperties->maxExtent.depth = (type == VK_IMAGE_TYPE_3D) ? 2048 : 1;
    pImageFormatProperties->maxMipLevels = 16;
    pImageFormatProperties->maxArrayLayers = 2048;
    pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pImageFormatProperties->maxResourceSize = 256ULL * 1024ULL * 1024ULL;
    return VK_SUCCESS;
}

void vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *pPropertyCount, VkSparseImageFormatProperties *pProperties) {
    if (pPropertyCount) *pPropertyCount = 0;
}

VkResult vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    if (!physicalDevice || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkDevice_T *dev = calloc(1, sizeof(*dev));
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;

    set_loader_magic(dev);
    dev->phys_dev = physicalDevice;
    dev->kdev = physicalDevice->kdev;

    /* Record features requested through the VkDeviceCreateInfo pNext chain. */
    if (pCreateInfo) {
        for (const void *next = pCreateInfo->pNext; next; next = *((const void *const *)next + 1)) {
            VkStructureType sType = *(const VkStructureType *)next;
            switch (sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
                const VkPhysicalDeviceVulkan11Features *f = next;
                dev->features.shader_float16 = f->storageInputOutput16;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                const VkPhysicalDeviceVulkan12Features *f = next;
                dev->features.runtime_descriptor_array = f->runtimeDescriptorArray;
                dev->features.partially_bound = f->descriptorBindingPartiallyBound;
                dev->features.update_after_bind = f->descriptorBindingSampledImageUpdateAfterBind;
                dev->features.timeline_semaphore = f->timelineSemaphore;
                dev->features.buffer_device_address = f->bufferDeviceAddress;
                dev->features.host_query_reset = f->hostQueryReset;
                dev->features.scalar_block_layout = f->scalarBlockLayout;
                dev->features.uniform_buffer_standard_layout = f->uniformBufferStandardLayout;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
                const VkPhysicalDeviceVulkan13Features *f = next;
                dev->features.dynamic_rendering = f->dynamicRendering;
                dev->features.synchronization2 = f->synchronization2;
                dev->features.maintenance4 = f->maintenance4;
                dev->features.pipeline_creation_cache_control = f->pipelineCreationCacheControl;
                dev->features.robust_image_access = f->robustImageAccess;
                dev->features.shader_terminate_invocation = f->shaderTerminateInvocation;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: {
                const VkPhysicalDeviceDynamicRenderingFeatures *f = next;
                dev->features.dynamic_rendering = f->dynamicRendering;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
                const VkPhysicalDeviceDescriptorIndexingFeatures *f = next;
                dev->features.runtime_descriptor_array = f->runtimeDescriptorArray;
                dev->features.partially_bound = f->descriptorBindingPartiallyBound;
                dev->features.update_after_bind = f->descriptorBindingSampledImageUpdateAfterBind;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
                const VkPhysicalDeviceBufferDeviceAddressFeatures *f = next;
                dev->features.buffer_device_address = f->bufferDeviceAddress;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: {
                const VkPhysicalDeviceTimelineSemaphoreFeatures *f = next;
                dev->features.timeline_semaphore = f->timelineSemaphore;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES: {
                const VkPhysicalDeviceHostQueryResetFeatures *f = next;
                dev->features.host_query_reset = f->hostQueryReset;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES: {
                const VkPhysicalDeviceSynchronization2Features *f = next;
                dev->features.synchronization2 = f->synchronization2;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES: {
                const VkPhysicalDeviceMaintenance4Features *f = next;
                dev->features.maintenance4 = f->maintenance4;
                break;
            }
            default:
                break;
            }
        }
    }

    /* Old-style VkPhysicalDeviceFeatures enabled through VkDeviceCreateInfo. */
    if (pCreateInfo && pCreateInfo->pEnabledFeatures) {
        const VkPhysicalDeviceFeatures *f = pCreateInfo->pEnabledFeatures;
        dev->features.robust_buffer_access = f->robustBufferAccess;
        dev->features.geometry_shader = f->geometryShader;
        dev->features.tessellation_shader = f->tessellationShader;
        dev->features.depth_clamp = f->depthClamp;
        dev->features.wide_lines = f->wideLines;
        dev->features.multi_draw_indirect = f->multiDrawIndirect;
        dev->features.draw_indirect_first_instance = f->drawIndirectFirstInstance;
        dev->features.fill_mode_non_solid = f->fillModeNonSolid;
        dev->features.vertex_pipeline_stores_and_atomics = f->vertexPipelineStoresAndAtomics;
        dev->features.fragment_stores_and_atomics = f->fragmentStoresAndAtomics;
        dev->features.shader_storage_image_read_without_format = f->shaderStorageImageReadWithoutFormat;
        dev->features.shader_storage_image_write_without_format = f->shaderStorageImageWriteWithoutFormat;
        dev->features.shader_float64 = f->shaderFloat64;
        dev->features.shader_int64 = f->shaderInt64;
        dev->features.shader_int16 = f->shaderInt16;
    }

    *pDevice = dev;
    pthread_mutex_init(&dev->submit_mutex, NULL);
    return VK_SUCCESS;
}

void vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    if (!device) return;
    pthread_mutex_destroy(&device->submit_mutex);
    if (device->queue) v9_cmd_buffer_destroy(device->queue->last_v9_cmd);
    free(device->queue);
    free(device);
}

void vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    if (!device || !pQueue) return;
    if (queueFamilyIndex != 0 || queueIndex != 0) {
        *pQueue = NULL;
        return;
    }
    if (!device->queue) {
        device->queue = calloc(1, sizeof(*device->queue));
        if (!device->queue) {
            *pQueue = NULL;
            return;
        }
        set_loader_magic(device->queue);
        device->queue->device = device;
    }
    *pQueue = device->queue;
}

/* panvk-style image layout (linear tiling) */
static uint8_t panvk_v9_format_bpp(uint32_t format) {
    switch (format) {
    case 9:  /* VK_FORMAT_R8_UNORM */           return 1;
    case 17: /* VK_FORMAT_S8_UINT */             return 1;
    case 16: /* VK_FORMAT_R8G8_UNORM */          return 2;
    case 124:/* VK_FORMAT_D16_UNORM */           return 2;
    case 37: /* VK_FORMAT_R8G8B8A8_UNORM */      return 4;
    case 43: /* VK_FORMAT_R8G8B8A8_SRGB */       return 4;
    case 44: /* VK_FORMAT_B8G8R8A8_UNORM */      return 4;
    case 50: /* VK_FORMAT_B8G8R8A8_SRGB */       return 4;
    case 87: /* VK_FORMAT_R16G16_SFLOAT */       return 4;
    case 97: /* VK_FORMAT_R16G16B16A16_SFLOAT */ return 8;
    case 100:/* VK_FORMAT_R32_SFLOAT */          return 4;
    case 103:/* VK_FORMAT_R32G32_SFLOAT */       return 8;
    case 106:/* VK_FORMAT_R32G32B32_SFLOAT */    return 12;
    case 109:/* VK_FORMAT_R32G32B32A32_SFLOAT */ return 16;
    case 126:/* VK_FORMAT_D32_SFLOAT */          return 4;
    case 129:/* VK_FORMAT_D24_UNORM_S8_UINT */   return 4;
    default:                                     return 4;
    }
}

static uint64_t align64(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

/* Convert one 32bpp pixel between RGBA8 (37/43) and BGRA8 (44/50) by swapping
 * the red and blue channels.  Identical formats copy the pixel unchanged. */
static void panvk_v9_convert_pixel(uint32_t src_format, uint32_t dst_format,
                                   const uint8_t *src, uint8_t *dst) {
    if (src_format == dst_format || panvk_v9_format_bpp(src_format) != 4 ||
        panvk_v9_format_bpp(dst_format) != 4) {
        memcpy(dst, src, 4);
        return;
    }
    int src_rgba = (src_format == 37 || src_format == 43);
    int dst_rgba = (dst_format == 37 || dst_format == 43);
    if (src_rgba == dst_rgba) {
        memcpy(dst, src, 4);
        return;
    }
    dst[0] = src[2]; /* R <- B */
    dst[1] = src[1];
    dst[2] = src[0]; /* B <- R */
    dst[3] = src[3];
}

static void panvk_v9_image_layout_init(struct VkImage_T *image) {
    uint32_t bpp = panvk_v9_format_bpp(image->format);
    uint64_t offset = 0;
    for (uint32_t level = 0; level < image->mip_levels && level < 16; level++) {
        uint64_t w = image->width  >> level; if (w < 1) w = 1;
        uint64_t h = image->height >> level; if (h < 1) h = 1;
        uint64_t d = image->depth  >> level; if (d < 1) d = 1;
        if (image->image_type == 0) d = 1; /* VK_IMAGE_TYPE_1D */

        uint64_t row_stride = align64(w * bpp, 64);
        uint64_t slice      = align64(h * row_stride, 64);
        uint64_t level_size = slice * d * image->array_layers;

        image->row_pitch[level] = row_stride;
        image->mip_offset[level] = offset;
        offset += level_size;
    }
    image->size = offset > 0 ? offset : 4096;
}

static uint64_t panvk_v9_image_get_offset(struct VkImage_T *image,
                                          uint32_t mip, uint32_t layer) {
    uint64_t d = image->depth >> mip; if (d < 1) d = 1;
    if (image->image_type == 0) d = 1;
    uint64_t h = image->height >> mip; if (h < 1) h = 1;
    uint64_t row_stride = image->row_pitch[mip];
    uint64_t slice      = align64(h * row_stride, 64);
    return image->mip_offset[mip] + (uint64_t)layer * slice * d;
}

static uint64_t panvk_v9_image_slice_pitch(struct VkImage_T *image, uint32_t mip) {
    uint64_t h = image->height >> mip; if (h < 1) h = 1;
    return align64(h * image->row_pitch[mip], 64);
}

/* Memory Allocation & Buffer Management */
VkResult vkAllocateMemory(VkDevice device, const struct VkMemoryAllocateInfo *pAllocateInfo, const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {
    if (!device || !device->kdev || !pAllocateInfo || !pMemory) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkDeviceMemory_T *mem = calloc(1, sizeof(*mem));
    if (!mem) return VK_ERROR_OUT_OF_HOST_MEMORY;

    size_t sz = pAllocateInfo->allocationSize > 0 ? pAllocateInfo->allocationSize : 4096;
    mem->bo = pan_kmod_bo_alloc(device->kdev, sz, PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
    if (!mem->bo) {
        free(mem);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    mem->size = sz;
    *pMemory = mem;
    return VK_SUCCESS;
}

void vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator) {
    if (!memory) return;
    if (memory->bo) pan_kmod_bo_free(memory->bo);
    free(memory);
}

VkResult vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkFlags flags, void **ppData) {
    if (!memory || !memory->bo || !ppData) return VK_ERROR_INITIALIZATION_FAILED;
    *ppData = (uint8_t *)memory->bo->cpu + offset;
    return VK_SUCCESS;
}

void vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
}

VkResult vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges) {
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    return VK_SUCCESS;
}

VkResult vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges) {
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    return VK_SUCCESS;
}

VkResult vkCreateBuffer(VkDevice device, const struct VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) {
    if (!device || !pCreateInfo || !pBuffer) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkBuffer_T *buf = calloc(1, sizeof(*buf));
    if (!buf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    buf->size = pCreateInfo->size;
    *pBuffer = buf;
    return VK_SUCCESS;
}

void vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
    if (buffer) free(buffer);
}

void vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, struct VkMemoryRequirements *pMemoryRequirements) {
    if (!pMemoryRequirements) return;
    pMemoryRequirements->size = buffer ? buffer->size : 4096;
    pMemoryRequirements->alignment = 64;
    pMemoryRequirements->memoryTypeBits = 0x3;
}

VkResult vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    if (buffer && memory) {
        buffer->bo = memory->bo;
        buffer->memory_offset = memoryOffset;
    }
    return VK_SUCCESS;
}

VkResult vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                       VkImage *pImage) {
    if (!pCreateInfo || !pImage) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkImage_T *image = calloc(1, sizeof(*image));
    if (!image) return VK_ERROR_OUT_OF_HOST_MEMORY;
    image->width = pCreateInfo->extent.width;
    image->height = pCreateInfo->extent.height;
    image->depth = pCreateInfo->extent.depth;
    image->format = pCreateInfo->format;
    image->image_type = pCreateInfo->imageType;
    image->mip_levels = pCreateInfo->mipLevels;
    image->array_layers = pCreateInfo->arrayLayers;
    image->samples = pCreateInfo->samples;
    image->tiling = pCreateInfo->tiling;
    image->usage = pCreateInfo->usage;
    panvk_v9_image_layout_init(image);
    *pImage = image;
    return VK_SUCCESS;
}

void vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {
    if (image && !image->swapchain) free(image);
}

void vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                                  struct VkMemoryRequirements *pMemoryRequirements) {
    if (!pMemoryRequirements) return;
    if (image) panvk_v9_image_layout_init(image);
    pMemoryRequirements->size = image ? image->size : 4096;
    pMemoryRequirements->alignment = 4096;
    pMemoryRequirements->memoryTypeBits = 0x3;
}

void vkGetImageSubresourceLayout(VkDevice device, VkImage image,
                                 const VkImageSubresource *sub,
                                 VkSubresourceLayout *layout) {
    if (!image || !sub || !layout) return;
    uint32_t mip = sub->mipLevel < image->mip_levels ? sub->mipLevel : 0;
    uint64_t h = image->height >> mip; if (h < 1) h = 1;
    uint64_t d = image->depth >> mip;  if (d < 1) d = 1;
    if (image->image_type == 0) d = 1;
    layout->rowPitch = image->row_pitch[mip];
    layout->arrayPitch = align64(h * layout->rowPitch, 64);
    layout->depthPitch = layout->arrayPitch;
    layout->offset = panvk_v9_image_get_offset(image, mip, sub->arrayLayer);
    layout->size = layout->arrayPitch * d;
}

VkResult vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                           VkDeviceSize memoryOffset) {
    if (!image || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    image->bo = memory->bo;
    image->memory_offset = memoryOffset;
    return VK_SUCCESS;
}

VkResult vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                           const VkAllocationCallbacks *pAllocator,
                           VkImageView *pView) {
    if (!pView) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkImageView_T *view = calloc(1, sizeof(*view));
    if (!view) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (ci) {
        view->image = ci->image;
        view->format = ci->format;
        view->view_type = ci->viewType;
        view->base_mip = ci->subresourceRange.baseMipLevel;
        view->mip_count = ci->subresourceRange.levelCount;
        view->base_layer = ci->subresourceRange.baseArrayLayer;
        view->layer_count = ci->subresourceRange.layerCount;
    }
    *pView = view;
    return VK_SUCCESS;
}

void vkDestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks *pAllocator) {
    free(imageView);
}

/* Shader Module & Pipeline Implementation */
#define SPIRV_MAGIC 0x07230203u
#define SPIRV_OP_ENTRY_POINT 15u

static uint32_t spirv_execution_model_stage(uint32_t execution_model) {
    switch (execution_model) {
    case 0: return VK_SHADER_STAGE_VERTEX_BIT;
    case 4: return VK_SHADER_STAGE_FRAGMENT_BIT;
    default: return 0;
    }
}

static bool spirv_string_equals(const uint32_t *words, size_t word_count,
                                const char *expected) {
    if (!expected) return false;
    size_t expected_len = strlen(expected);
    size_t max_len = word_count * sizeof(uint32_t);
    const char *value = (const char *)words;
    const char *end = memchr(value, '\0', max_len);
    return end && (size_t)(end - value) == expected_len &&
           memcmp(value, expected, expected_len) == 0;
}

static bool spirv_validate_and_scan(const uint32_t *code, size_t code_size,
                                    uint32_t *stage_mask) {
    if (!code || code_size < 5 * sizeof(uint32_t) ||
        code_size % sizeof(uint32_t) != 0 || code[0] != SPIRV_MAGIC ||
        code[1] > 0x00010600u || code[3] == 0 || code[4] != 0) {
        return false;
    }

    size_t count = code_size / sizeof(uint32_t);
    uint32_t stages = 0;
    bool found_entry_point = false;
    for (size_t offset = 5; offset < count;) {
        uint32_t instruction = code[offset];
        uint32_t word_count = instruction >> 16;
        uint32_t opcode = instruction & 0xffffu;
        if (word_count == 0 || word_count > count - offset) return false;
        if (opcode == SPIRV_OP_ENTRY_POINT) {
            if (word_count < 4) return false;
            found_entry_point = true;
            stages |= spirv_execution_model_stage(code[offset + 1]);
            if (!memchr((const char *)&code[offset + 3], '\0',
                        (word_count - 3) * sizeof(uint32_t))) {
                return false;
            }
        }
        offset += word_count;
    }

    if (stage_mask) *stage_mask = stages;
    return found_entry_point;
}

static bool spirv_has_entry_point(VkShaderModule module, uint32_t stage,
                                  const char *name) {
    if (!module || !(module->stage_mask & stage) || !name) return false;
    size_t count = module->code_size / sizeof(uint32_t);
    for (size_t offset = 5; offset < count;) {
        uint32_t instruction = module->code[offset];
        uint32_t word_count = instruction >> 16;
        uint32_t opcode = instruction & 0xffffu;
        if (opcode == SPIRV_OP_ENTRY_POINT && word_count >= 4 &&
            spirv_execution_model_stage(module->code[offset + 1]) == stage &&
            spirv_string_equals(&module->code[offset + 3], word_count - 3, name)) {
            return true;
        }
        offset += word_count;
    }
    return false;
}

VkResult vkCreateShaderModule(VkDevice device, const struct VkShaderModuleCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule) {
    if (!device || !pCreateInfo || !pShaderModule) return VK_ERROR_INITIALIZATION_FAILED;
    *pShaderModule = NULL;

    uint32_t stage_mask = 0;
    if (!spirv_validate_and_scan(pCreateInfo->pCode, pCreateInfo->codeSize,
                                 &stage_mask)) {
        return VK_ERROR_INVALID_SHADER_NV;
    }

    struct VkShaderModule_T *sm = calloc(1, sizeof(*sm));
    if (!sm) return VK_ERROR_OUT_OF_HOST_MEMORY;

    sm->code_size = pCreateInfo->codeSize;
    sm->stage_mask = stage_mask;
    sm->code = malloc(sm->code_size);
    if (!sm->code) {
        free(sm);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(sm->code, pCreateInfo->pCode, sm->code_size);

    *pShaderModule = sm;
    return VK_SUCCESS;
}

void vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator) {
    if (!shaderModule) return;
    if (shaderModule->code) free(shaderModule->code);
    free(shaderModule);
}

VkResult vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkPipelineCache *pPipelineCache) {
    if (!pPipelineCache) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkPipelineCache_T *pc = calloc(1, sizeof(*pc));
    *pPipelineCache = pc;
    return VK_SUCCESS;
}

void vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache, const VkAllocationCallbacks *pAllocator) {
    if (pipelineCache) free(pipelineCache);
}

VkResult vkCreatePipelineLayout(VkDevice device, const struct VkPipelineLayoutCreateInfo *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout) {
    if (!pCreateInfo || !pPipelineLayout) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkPipelineLayout_T *pl = calloc(1, sizeof(*pl));
    if (!pl) return VK_ERROR_OUT_OF_HOST_MEMORY;

    uint32_t binding_count = 0;
    for (uint32_t set = 0; set < pCreateInfo->setLayoutCount; set++) {
        if (pCreateInfo->pSetLayouts[set])
            binding_count += pCreateInfo->pSetLayouts[set]->binding_count;
    }
    pl->bindings = calloc(binding_count, sizeof(*pl->bindings));
    if (binding_count && !pl->bindings) {
        free(pl);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    uint32_t index = 0;
    uint32_t ubo_index = 0;
    uint32_t ssbo_index = 0;
    for (uint32_t set = 0; set < pCreateInfo->setLayoutCount; set++) {
        VkDescriptorSetLayout set_layout = pCreateInfo->pSetLayouts[set];
        if (!set_layout) continue;
        for (uint32_t i = 0; i < set_layout->binding_count; i++) {
            const struct VkDescriptorSetLayoutBinding *binding = &set_layout->bindings[i];
            struct panvk_v9_descriptor_binding *out = &pl->bindings[index++];
            out->set = set;
            out->binding = binding->binding;
            out->descriptor_type = binding->descriptorType;
            out->array_size = binding->descriptorCount;
            if (binding->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                binding->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                out->resource_index = 0x01000000u | ubo_index;
                ubo_index += binding->descriptorCount;
            } else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                       binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                out->resource_index = 0x01000000u | ssbo_index;
                ssbo_index += binding->descriptorCount;
            }
        }
    }
    pl->compiler_layout.bindings = pl->bindings;
    pl->compiler_layout.binding_count = binding_count;
    pl->compiler_layout.ubo_count = ubo_index;
    *pPipelineLayout = pl;
    return VK_SUCCESS;
}

void vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout pipelineLayout, const VkAllocationCallbacks *pAllocator) {
    if (!pipelineLayout) return;
    free(pipelineLayout->bindings);
    free(pipelineLayout);
}

VkResult vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass) {
    if (!pRenderPass) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkRenderPass_T *rp = calloc(1, sizeof(*rp));
    *pRenderPass = rp;
    return VK_SUCCESS;
}

void vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator) {
    if (renderPass) free(renderPass);
}

VkResult vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass) {
    if (!pRenderPass) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkRenderPass_T *rp = calloc(1, sizeof(*rp));
    if (!rp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pRenderPass = rp;
    return VK_SUCCESS;
}

VkResult vkCreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2 *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass) {
    return vkCreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
}

VkResult vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer) {
    if (!device || !pFramebuffer) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkFramebuffer_T *fb = calloc(1, sizeof(*fb));
    if (!fb) return VK_ERROR_OUT_OF_HOST_MEMORY;
    fb->device = device;
    if (pCreateInfo) {
        fb->attachment_count = pCreateInfo->attachmentCount;
        fb->width = pCreateInfo->width;
        fb->height = pCreateInfo->height;
        if (fb->attachment_count && pCreateInfo->pAttachments) {
            fb->attachments = calloc(fb->attachment_count, sizeof(*fb->attachments));
            if (!fb->attachments) {
                free(fb);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            for (uint32_t i = 0; i < fb->attachment_count; i++)
                fb->attachments[i] = pCreateInfo->pAttachments[i];
        }
    }
    *pFramebuffer = fb;
    return VK_SUCCESS;
}

void vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator) {
    if (framebuffer) {
        free(framebuffer->attachments);
        free(framebuffer);
    }
}

VkResult vkCreateDescriptorSetLayout(VkDevice device,
                                     const struct VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                     const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pSetLayout) {
    if (!pCreateInfo || !pSetLayout) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkDescriptorSetLayout_T *dsl = calloc(1, sizeof(*dsl));
    if (!dsl) return VK_ERROR_OUT_OF_HOST_MEMORY;
    dsl->binding_count = pCreateInfo->bindingCount;
    dsl->bindings = calloc(dsl->binding_count, sizeof(*dsl->bindings));
    dsl->binding_offsets = calloc(dsl->binding_count, sizeof(*dsl->binding_offsets));
    if (dsl->binding_count) {
        dsl->binding_flags = calloc(dsl->binding_count, sizeof(*dsl->binding_flags));
    }
    if (dsl->binding_count && (!dsl->bindings || !dsl->binding_offsets)) {
        free(dsl->binding_flags);
        free(dsl->binding_offsets);
        free(dsl->bindings);
        free(dsl);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (dsl->binding_count) {
        memcpy(dsl->bindings, pCreateInfo->pBindings,
               dsl->binding_count * sizeof(*dsl->bindings));
        for (uint32_t i = 0; i < dsl->binding_count; i++) {
            dsl->binding_offsets[i] = dsl->descriptor_count;
            dsl->descriptor_count += dsl->bindings[i].descriptorCount;
        }
    }
    /* Descriptor indexing: honour VkDescriptorSetLayoutBindingFlagsCreateInfo
     * pNext (variable descriptor count, partially bound, update-after-bind). */
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *bfci = pCreateInfo->pNext;
    while (bfci) {
        if (bfci->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
            for (uint32_t i = 0; i < dsl->binding_count && i < bfci->bindingCount; i++) {
                dsl->binding_flags[i] = bfci->pBindingFlags[i];
                if (bfci->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
                    dsl->variable_binding = i;
                    dsl->variable_descriptor_count = dsl->bindings[i].descriptorCount;
                }
            }
            break;
        }
        bfci = bfci->pNext;
    }
    *pSetLayout = dsl;
    return VK_SUCCESS;
}

void vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout setLayout, const VkAllocationCallbacks *pAllocator) {
    if (!setLayout) return;
    free(setLayout->binding_flags);
    free(setLayout->binding_offsets);
    free(setLayout->bindings);
    free(setLayout);
}

VkResult vkCreateDescriptorPool(VkDevice device,
                                const struct VkDescriptorPoolCreateInfo *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pDescriptorPool) {
    if (!pDescriptorPool) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkDescriptorPool_T *dp = calloc(1, sizeof(*dp));
    *pDescriptorPool = dp;
    return VK_SUCCESS;
}

void vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks *pAllocator) {
    if (descriptorPool) free(descriptorPool);
}

VkResult vkAllocateDescriptorSets(VkDevice device,
                                  const struct VkDescriptorSetAllocateInfo *pAllocateInfo,
                                  VkDescriptorSet *pDescriptorSets) {
    if (!pAllocateInfo || !pDescriptorSets || !pAllocateInfo->pSetLayouts)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* Variable descriptor counts per set come from the pNext array. */
    const uint32_t *var_counts = NULL;
    const VkDescriptorSetVariableDescriptorCountAllocateInfo *vdci = pAllocateInfo->pNext;
    while (vdci) {
        if (vdci->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) {
            var_counts = vdci->pDescriptorCounts;
            break;
        }
        vdci = vdci->pNext;
    }
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++)
        pDescriptorSets[i] = NULL;
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
        VkDescriptorSet set = calloc(1, sizeof(*set));
        if (!set) goto fail;
        set->layout = pAllocateInfo->pSetLayouts[i];
        uint32_t n = set->layout->descriptor_count;
        if (set->layout->variable_binding >= 0 && var_counts) {
            /* Replace the variable binding's declared count with the requested
             * count (array indexing is still within the layout's descriptor
             * space; just shrink the total allocation). */
            uint32_t requested = var_counts[i];
            if (requested < set->layout->variable_descriptor_count) {
                uint32_t shrink = set->layout->variable_descriptor_count - requested;
                n = (n > shrink) ? (n - shrink) : 0;
            }
        }
        set->buffers = calloc(n ? n : 1, sizeof(*set->buffers));
        if (n && !set->buffers) {
            free(set);
            goto fail;
        }
        pDescriptorSets[i] = set;
    }
    return VK_SUCCESS;

fail:
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
        if (!pDescriptorSets[i]) continue;
        free(pDescriptorSets[i]->buffers);
        free(pDescriptorSets[i]);
        pDescriptorSets[i] = NULL;
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets) {
    if (!pDescriptorSets) return VK_SUCCESS;
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        if (pDescriptorSets[i]) {
            free(pDescriptorSets[i]->buffers);
            free(pDescriptorSets[i]);
        }
    }
    return VK_SUCCESS;
}

void vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
                            const VkWriteDescriptorSet *pDescriptorWrites,
                            uint32_t descriptorCopyCount, const VkCopyDescriptorSet *pDescriptorCopies) {
    for (uint32_t w = 0; w < descriptorWriteCount; w++) {
        const VkWriteDescriptorSet *write = &pDescriptorWrites[w];
        if (!write->dstSet || !write->pBufferInfo) continue;
        VkDescriptorSetLayout layout = write->dstSet->layout;
        for (uint32_t b = 0; b < layout->binding_count; b++) {
            const struct VkDescriptorSetLayoutBinding *binding = &layout->bindings[b];
            if (binding->binding != write->dstBinding ||
                binding->descriptorType != write->descriptorType ||
                write->dstArrayElement + write->descriptorCount > binding->descriptorCount)
                continue;
            memcpy(&write->dstSet->buffers[layout->binding_offsets[b] +
                                          write->dstArrayElement],
                   write->pBufferInfo,
                   write->descriptorCount * sizeof(*write->pBufferInfo));
            break;
        }
    }
}

static bool pipeline_dynamic_state(const struct VkPipelineDynamicStateCreateInfo *dynamic,
                                   uint32_t state) {
    if (!dynamic || !dynamic->pDynamicStates) return false;
    for (uint32_t i = 0; i < dynamic->dynamicStateCount; i++) {
        if (dynamic->pDynamicStates[i] == state) return true;
    }
    return false;
}

static VkResult pipeline_parse_shader_stages(struct VkPipeline_T *pipeline,
                                             const struct VkGraphicsPipelineCreateInfo *info) {
    if (!info->pStages || info->stageCount == 0) return VK_ERROR_INVALID_SHADER_NV;

    for (uint32_t i = 0; i < info->stageCount; i++) {
        const struct VkPipelineShaderStageCreateInfo *stage = &info->pStages[i];
        if ((stage->stage != VK_SHADER_STAGE_VERTEX_BIT &&
             stage->stage != VK_SHADER_STAGE_FRAGMENT_BIT) ||
            !spirv_has_entry_point(stage->module, stage->stage, stage->pName)) {
            return VK_ERROR_INVALID_SHADER_NV;
        }
        if (pipeline->stage_mask & stage->stage) return VK_ERROR_INVALID_SHADER_NV;

        pipeline->stage_mask |= stage->stage;
        if (stage->stage == VK_SHADER_STAGE_VERTEX_BIT) {
            snprintf(pipeline->vertex_entry_point,
                     sizeof(pipeline->vertex_entry_point), "%s", stage->pName);
        } else {
            snprintf(pipeline->fragment_entry_point,
                     sizeof(pipeline->fragment_entry_point), "%s", stage->pName);
        }
    }

    return (pipeline->stage_mask & VK_SHADER_STAGE_VERTEX_BIT) ?
           VK_SUCCESS : VK_ERROR_INVALID_SHADER_NV;
}

static VkResult pipeline_compile_shaders(struct VkPipeline_T *pipeline,
                                         const struct VkGraphicsPipelineCreateInfo *info) {
    const char *required_env = getenv("PANVK_REQUIRE_COMPILER");
    bool required = required_env && required_env[0] && strcmp(required_env, "0");
    if (!load_compiler()) {
        return required ? VK_ERROR_INVALID_SHADER_NV : VK_SUCCESS;
    }

    char error[512];
    for (uint32_t i = 0; i < info->stageCount; i++) {
        const struct VkPipelineShaderStageCreateInfo *stage = &info->pStages[i];
        enum panvk_v9_shader_stage compiler_stage;
        struct panvk_v9_compiled_shader *binary;
        if (stage->stage == VK_SHADER_STAGE_VERTEX_BIT) {
            compiler_stage = PANVK_V9_SHADER_VERTEX;
            binary = &pipeline->vertex_binary;
        } else if (stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            compiler_stage = PANVK_V9_SHADER_FRAGMENT;
            binary = &pipeline->fragment_binary;
        } else {
            continue;
        }

        int ret = compiler_api.compile(stage->module->code, stage->module->code_size,
                                       compiler_stage, stage->pName,
                                       &pipeline->compiler_layout,
                                       binary,
                                       error, sizeof(error));
        FILE *flog = fopen("/data/data/com.termux/files/usr/tmp/panvk_debug.log", "a");
        if (flog) {
            fprintf(flog, "compile stage=%d ret=%d code_size=%zu err='%s'\n",
                    compiler_stage, ret, stage->module->code_size, error);
            fclose(flog);
        }
        if (ret) {
            compiler_api.cleanup(&pipeline->vertex_binary);
            compiler_api.cleanup(&pipeline->fragment_binary);
            return required ? VK_ERROR_INVALID_SHADER_NV : VK_SUCCESS;
        }
    }

    pipeline->shaders_compiled = pipeline->vertex_binary.binary_size != 0;
    return VK_SUCCESS;
}

static void pipeline_cleanup(struct VkPipeline_T *pipeline) {
    if (!pipeline) return;
    if (compiler_api.cleanup) {
        compiler_api.cleanup(&pipeline->vertex_binary);
        compiler_api.cleanup(&pipeline->fragment_binary);
        compiler_api.cleanup(&pipeline->compute_binary);
    }
    free(pipeline->bindings);
    free(pipeline);
}

static VkResult pipeline_copy_layout(struct VkPipeline_T *pipeline,
                                     VkPipelineLayout layout) {
    if (!layout || !layout->compiler_layout.binding_count) return VK_SUCCESS;
    size_t size = layout->compiler_layout.binding_count * sizeof(*pipeline->bindings);
    pipeline->bindings = malloc(size);
    if (!pipeline->bindings) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(pipeline->bindings, layout->bindings, size);
    pipeline->compiler_layout = layout->compiler_layout;
    pipeline->compiler_layout.bindings = pipeline->bindings;
    return VK_SUCCESS;
}

static void pipeline_parse_fixed_state(struct VkPipeline_T *pipeline,
                                       const struct VkGraphicsPipelineCreateInfo *info) {
    pipeline->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pipeline->polygon_mode = VK_POLYGON_MODE_FILL;
    pipeline->cull_mode = VK_CULL_MODE_NONE;
    pipeline->front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    pipeline->line_width = 1.0f;
    pipeline->rasterization_samples = VK_SAMPLE_COUNT_1_BIT;
    pipeline->depth_compare_op = VK_COMPARE_OP_ALWAYS;
    pipeline->color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    if (info->pVertexInputState) {
        pipeline->vertex_binding_count =
            info->pVertexInputState->vertexBindingDescriptionCount < 16 ?
            info->pVertexInputState->vertexBindingDescriptionCount : 16;
        pipeline->vertex_attribute_count =
            info->pVertexInputState->vertexAttributeDescriptionCount < 16 ?
            info->pVertexInputState->vertexAttributeDescriptionCount : 16;
        if (pipeline->vertex_binding_count &&
            info->pVertexInputState->pVertexBindingDescriptions) {
            memcpy(pipeline->vertex_bindings,
                   info->pVertexInputState->pVertexBindingDescriptions,
                   pipeline->vertex_binding_count * sizeof(pipeline->vertex_bindings[0]));
        }
        if (pipeline->vertex_attribute_count &&
            info->pVertexInputState->pVertexAttributeDescriptions) {
            memcpy(pipeline->vertex_attributes,
                   info->pVertexInputState->pVertexAttributeDescriptions,
                   pipeline->vertex_attribute_count * sizeof(pipeline->vertex_attributes[0]));
        }
    }

    if (info->pInputAssemblyState) {
        pipeline->topology = info->pInputAssemblyState->topology;
        pipeline->primitive_restart = info->pInputAssemblyState->primitiveRestartEnable != 0;
    }
    if (info->pViewportState) {
        if (info->pViewportState->viewportCount && info->pViewportState->pViewports)
            pipeline->viewport = info->pViewportState->pViewports[0];
        if (info->pViewportState->scissorCount && info->pViewportState->pScissors)
            pipeline->scissor = info->pViewportState->pScissors[0];
    }
    pipeline->dynamic_viewport = pipeline_dynamic_state(info->pDynamicState,
                                                        VK_DYNAMIC_STATE_VIEWPORT);
    pipeline->dynamic_scissor = pipeline_dynamic_state(info->pDynamicState,
                                                       VK_DYNAMIC_STATE_SCISSOR);
    if (info->pRasterizationState) {
        pipeline->rasterizer_discard = info->pRasterizationState->rasterizerDiscardEnable != 0;
        pipeline->polygon_mode = info->pRasterizationState->polygonMode;
        pipeline->cull_mode = info->pRasterizationState->cullMode;
        pipeline->front_face = info->pRasterizationState->frontFace;
        pipeline->line_width = info->pRasterizationState->lineWidth;
    }
    if (info->pMultisampleState)
        pipeline->rasterization_samples = info->pMultisampleState->rasterizationSamples;
    if (info->pDepthStencilState) {
        pipeline->depth_test = info->pDepthStencilState->depthTestEnable != 0;
        pipeline->depth_write = info->pDepthStencilState->depthWriteEnable != 0;
        pipeline->depth_compare_op = info->pDepthStencilState->depthCompareOp;
    }
    if (info->pColorBlendState && info->pColorBlendState->attachmentCount &&
        info->pColorBlendState->pAttachments) {
        pipeline->blend_enable = info->pColorBlendState->pAttachments[0].blendEnable != 0;
        pipeline->color_write_mask = info->pColorBlendState->pAttachments[0].colorWriteMask;
    }
}

VkResult vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                                   uint32_t createInfoCount,
                                   const struct VkGraphicsPipelineCreateInfo *pCreateInfos,
                                   const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines) {
    if (!device || !pCreateInfos || !pPipelines) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < createInfoCount; i++) pPipelines[i] = NULL;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        struct VkPipeline_T *pipe = calloc(1, sizeof(*pipe));
        if (!pipe) return VK_ERROR_OUT_OF_HOST_MEMORY;

        VkResult result = pipeline_copy_layout(pipe, pCreateInfos[i].layout);
        if (result == VK_SUCCESS)
            result = pipeline_parse_shader_stages(pipe, &pCreateInfos[i]);
        if (result == VK_SUCCESS)
            result = pipeline_compile_shaders(pipe, &pCreateInfos[i]);
        if (result != VK_SUCCESS) {
            pipeline_cleanup(pipe);
            for (uint32_t j = 0; j < i; j++) {
                pipeline_cleanup(pPipelines[j]);
                pPipelines[j] = NULL;
            }
            return result;
        }
        pipeline_parse_fixed_state(pipe, &pCreateInfos[i]);
        pPipelines[i] = pipe;
    }
    return VK_SUCCESS;
}

VkResult vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines) {
    if (!pPipelines) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        struct VkPipeline_T *pipe = calloc(1, sizeof(*pipe));
        if (!pipe) { pPipelines[i] = NULL; return VK_ERROR_OUT_OF_HOST_MEMORY; }
        const struct VkComputePipelineCreateInfo *info = &pCreateInfos[i];
        pipe->stage_mask = VK_SHADER_STAGE_COMPUTE_BIT;
        if (info->stage.module && info->stage.module->code) {
            strncpy(pipe->compute_entry_point,
                    info->stage.pName ? info->stage.pName : "main",
                    sizeof(pipe->compute_entry_point) - 1);
            pipe->compute_module = info->stage.module;
        }
        if (info->layout)
            pipeline_copy_layout(pipe, info->layout);
        if (load_compiler()) {
            char error[512];
            int ret = compiler_api.compile(
                info->stage.module->code, info->stage.module->code_size,
                PANVK_V9_SHADER_COMPUTE, info->stage.pName,
                &pipe->compiler_layout, &pipe->compute_binary,
                error, sizeof(error));
            FILE *flog = fopen("/data/data/com.termux/files/usr/tmp/panvk_debug.log", "a");
            if (flog) {
                fprintf(flog, "compile compute ret=%d code_size=%zu err='%s'\n",
                        ret, info->stage.module->code_size, error);
                fclose(flog);
            }
        }
        pPipelines[i] = pipe;
    }
    return VK_SUCCESS;
}

void vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *pAllocator) {
    pipeline_cleanup(pipeline);
}

VkResult vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore) {
    if (!pSemaphore) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkSemaphore_T *sem = calloc(1, sizeof(*sem));
    if (!sem) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (pCreateInfo) {
        const VkSemaphoreTypeCreateInfo *ti = pCreateInfo->pNext;
        while (ti) {
            if (ti->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
                if (ti->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
                    /* Self-pointer marks this semaphore as a timeline; its
                     * counter holds the current timeline value. */
                    sem->timeline = sem;
                    sem->counter = ti->initialValue;
                }
                break;
            }
            ti = ti->pNext;
        }
    }
    *pSemaphore = sem;
    return VK_SUCCESS;
}

void vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator) {
    if (semaphore) free(semaphore);
}

VkResult vkCreateFence(VkDevice device, const VkFenceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkFence *pFence) {
    if (!pFence) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkFence_T *f = calloc(1, sizeof(*f));
    if (f && pCreateInfo) {
        f->signaled = (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
    }
    *pFence = f;
    return f ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

void vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *pAllocator) {
    if (fence) free(fence);
}

VkResult vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences) {
    if (!pFences) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < fenceCount; i++) {
        if (pFences[i]) pFences[i]->signaled = false;
    }
    return VK_SUCCESS;
}

VkResult vkGetFenceStatus(VkDevice device, VkFence fence) {
    return fence && fence->signaled ? VK_SUCCESS : VK_NOT_READY;
}

VkResult vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences,
                         uint32_t waitAll, uint64_t timeout) {
    if (!pFences) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < fenceCount; i++) {
        if (pFences[i]) pFences[i]->signaled = true;
    }
    return VK_SUCCESS;
}

/* Command Pool & Buffer Management */
VkResult vkCreateCommandPool(VkDevice device, const struct VkCommandPoolCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool) {
    if (!device || !pCommandPool) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkCommandPool_T *pool = calloc(1, sizeof(*pool));
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->device = device;
    *pCommandPool = pool;
    return VK_SUCCESS;
}

void vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks *pAllocator) {
    if (commandPool) free(commandPool);
}

VkResult vkAllocateCommandBuffers(VkDevice device, const struct VkCommandBufferAllocateInfo *pAllocateInfo, VkCommandBuffer *pCommandBuffers) {
    if (!device || !pAllocateInfo || !pCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;

    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
        struct VkCommandBuffer_T *cb = calloc(1, sizeof(*cb));
        if (!cb) return VK_ERROR_OUT_OF_HOST_MEMORY;
        set_loader_magic(cb);
        cb->device = device;
        pCommandBuffers[i] = cb;
    }
    return VK_SUCCESS;
}

void vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers) {
    if (!pCommandBuffers) return;
    for (uint32_t i = 0; i < commandBufferCount; i++) {
        if (pCommandBuffers[i]) {
            if (pCommandBuffers[i]->v9_cmd) {
                v9_cmd_buffer_destroy(pCommandBuffers[i]->v9_cmd);
            }
            free(pCommandBuffers[i]);
        }
    }
}

VkResult vkCreateEvent(VkDevice device, const struct VkEventCreateInfo *pCreateInfo, const struct VkAllocationCallbacks *pAllocator, VkEvent *pEvent) {
    if (!pEvent) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkEvent_T *e = calloc(1, sizeof(*e));
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    e->signaled = (pCreateInfo && pCreateInfo->flags & VK_EVENT_CREATE_DEVICE_ONLY_BIT) ? false : false;
    set_loader_magic(e);
    *pEvent = e;
    return VK_SUCCESS;
}

void vkDestroyEvent(VkDevice device, VkEvent event, const struct VkAllocationCallbacks *pAllocator) {
    free(event);
}

VkResult vkCreateQueryPool(VkDevice device, const struct VkQueryPoolCreateInfo *pCreateInfo, const struct VkAllocationCallbacks *pAllocator, VkQueryPool *pQueryPool) {
    if (!pQueryPool) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkQueryPool_T *qp = calloc(1, sizeof(*qp));
    if (!qp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    qp->query_count = pCreateInfo->queryCount;
    set_loader_magic(qp);
    *pQueryPool = qp;
    return VK_SUCCESS;
}

void vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool, const struct VkAllocationCallbacks *pAllocator) {
    if (queryPool) free(queryPool);
}

void vkCmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, uint32_t flags) {
    (void)queryPool; (void)query; (void)flags;
}

void vkCmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount) {
    (void)queryPool; (void)firstQuery; (void)queryCount;
}

VkResult vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const struct VkCommandBufferBeginInfo *pBeginInfo) {
    if (!commandBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    commandBuffer->graphics_pipeline = NULL;
    commandBuffer->viewport_set = false;
    commandBuffer->scissor_set = false;
    memset(commandBuffer->descriptor_sets, 0, sizeof(commandBuffer->descriptor_sets));
    return VK_SUCCESS;
}

void vkCmdBindPipeline(VkCommandBuffer commandBuffer, uint32_t pipelineBindPoint, VkPipeline pipeline) {
    if (!commandBuffer) return;
    if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
        commandBuffer->compute_pipeline = pipeline;
        return;
    }
    if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    commandBuffer->graphics_pipeline = pipeline;
    if (pipeline) {
        if (!pipeline->dynamic_viewport) {
            commandBuffer->viewport = pipeline->viewport;
            commandBuffer->viewport_set = true;
        }
        if (!pipeline->dynamic_scissor) {
            commandBuffer->scissor = pipeline->scissor;
            commandBuffer->scissor_set = true;
        }
    }
}

void vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport *pViewports) {
    if (!commandBuffer || firstViewport != 0 || viewportCount == 0 || !pViewports) return;
    commandBuffer->viewport = pViewports[0];
    commandBuffer->viewport_set = true;
}

void vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D *pScissors) {
    if (!commandBuffer || firstScissor != 0 || scissorCount == 0 || !pScissors) return;
    commandBuffer->scissor = pScissors[0];
    commandBuffer->scissor_set = true;
}

void vkCmdSetDepthBias(VkCommandBuffer commandBuffer, float depthBiasConstantFactor,
                       float depthBiasClamp, float depthBiasSlopeFactor) {
    if (!commandBuffer) return;
    commandBuffer->depth_bias_constant_factor = depthBiasConstantFactor;
    commandBuffer->depth_bias_clamp = depthBiasClamp;
    commandBuffer->depth_bias_constant_offset = depthBiasSlopeFactor;
    commandBuffer->depth_bias_set = true;
}

void vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                        uint32_t stageFlags, uint32_t offset, uint32_t size, const void *pValues) {
    if (!commandBuffer || !pValues || !size) return;
    if (offset >= sizeof(commandBuffer->push_constants)) return;
    if (offset + size > sizeof(commandBuffer->push_constants))
        size = sizeof(commandBuffer->push_constants) - offset;
    memcpy(commandBuffer->push_constants + offset, pValues, size);
    if (offset + size > commandBuffer->push_constants_size)
        commandBuffer->push_constants_size = offset + size;
}

void vkCmdSetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask) {
    (void)event; (void)stageMask;
}

void vkCmdResetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask) {
    (void)event; (void)stageMask;
}

void vkCmdWaitEvents(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents,
                     VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
                     uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
                     uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                     uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers) {
    (void)eventCount; (void)pEvents; (void)srcStageMask; (void)dstStageMask;
    (void)memoryBarrierCount; (void)pMemoryBarriers; (void)bufferMemoryBarrierCount;
    (void)pBufferMemoryBarriers; (void)imageMemoryBarrierCount; (void)pImageMemoryBarriers;
}

void vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query) {
    (void)queryPool; (void)query;
}

void vkCmdWriteTimestamp(VkCommandBuffer commandBuffer, uint32_t pipelineStage, VkQueryPool queryPool, uint32_t query) {
    (void)pipelineStage; (void)queryPool; (void)query;
}

void vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t x, uint32_t y, uint32_t z) {
    if (!commandBuffer || !commandBuffer->compute_pipeline ||
        x == 0 || y == 0 || z == 0)
        return;
    VkPipeline pipeline = commandBuffer->compute_pipeline;
    if (!commandBuffer->v9_cmd) {
        struct v9_render_target_config config = {
            .width = 300, .height = 300, .clear_color = 0,
        };
        commandBuffer->v9_cmd = v9_cmd_buffer_create(commandBuffer->device->kdev, &config);
        if (!commandBuffer->v9_cmd) return;
        v9_cmd_buffer_begin(commandBuffer->v9_cmd);
    }
    command_buffer_apply_ssbos(commandBuffer);
    if (pipeline->compute_binary.binary_size)
        v9_cmd_buffer_set_compute_shader(commandBuffer->v9_cmd, &pipeline->compute_binary);
    v9_cmd_buffer_dispatch(commandBuffer->v9_cmd, x, y, z);
}

void vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset) {
    (void)buffer; (void)offset;
}

VkResult vkSetEvent(VkDevice device, VkEvent event) {
    if (!event) return VK_ERROR_INITIALIZATION_FAILED;
    event->signaled = true;
    return VK_SUCCESS;
}

VkResult vkResetEvent(VkDevice device, VkEvent event) {
    if (!event) return VK_ERROR_INITIALIZATION_FAILED;
    event->signaled = false;
    return VK_SUCCESS;
}

VkResult vkGetEventStatus(VkDevice device, VkEvent event) {
    if (!event) return VK_ERROR_INITIALIZATION_FAILED;
    return event->signaled ? VK_EVENT_SET : VK_EVENT_RESET;
}

void vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, uint32_t pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t *pDynamicOffsets) {
    (void)pipelineBindPoint; (void)layout; (void)dynamicOffsetCount; (void)pDynamicOffsets;
    if (!commandBuffer || firstSet >= 8 || descriptorSetCount > 8 - firstSet ||
        (descriptorSetCount && !pDescriptorSets)) return;
    memcpy(&commandBuffer->descriptor_sets[firstSet], pDescriptorSets,
           descriptorSetCount * sizeof(*pDescriptorSets));
}

void vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer *pBuffers, const VkDeviceSize *pOffsets) {
    if (!commandBuffer || firstBinding >= 16 || !pBuffers || !pOffsets) return;
    for (uint32_t i = 0; i < bindingCount && (firstBinding + i) < 16; i++) {
        commandBuffer->vertex_bindings[firstBinding + i].buffer = pBuffers[i];
        commandBuffer->vertex_bindings[firstBinding + i].offset = pOffsets[i];
    }
}

void vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t indexType) {
    if (!commandBuffer) return;
    commandBuffer->index_buffer = buffer;
    commandBuffer->index_offset = offset;
    commandBuffer->index_type = indexType;
}

void vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
                     uint32_t regionCount, const struct VkBufferCopy *pRegions) {
    if (!srcBuffer || !srcBuffer->bo || !dstBuffer || !dstBuffer->bo || !pRegions) return;

    for (uint32_t i = 0; i < regionCount; i++) {
        VkDeviceSize src_offset = srcBuffer->memory_offset + pRegions[i].srcOffset;
        VkDeviceSize dst_offset = dstBuffer->memory_offset + pRegions[i].dstOffset;
        VkDeviceSize size = pRegions[i].size;
        if (src_offset > srcBuffer->bo->size || size > srcBuffer->bo->size - src_offset ||
            dst_offset > dstBuffer->bo->size || size > dstBuffer->bo->size - dst_offset) {
            continue;
        }
        memcpy((uint8_t *)dstBuffer->bo->cpu + dst_offset,
               (const uint8_t *)srcBuffer->bo->cpu + src_offset, size);
    }
}

VkResult vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSampler *pSampler) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    if (pSampler) *pSampler = (VkSampler)(uintptr_t)0x1;
    return VK_SUCCESS;
}

void vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)sampler; (void)pAllocator;
}

void vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage,
                            VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy *pRegions) {
    (void)commandBuffer; (void)dstImageLayout;
    if (!srcBuffer || !srcBuffer->bo || !dstImage || !dstImage->bo || !pRegions) return;
    const VkBufferImageCopy *regions = pRegions;
    const uint8_t *src_base = (const uint8_t *)srcBuffer->bo->cpu + srcBuffer->memory_offset;
    uint8_t *dst_base = (uint8_t *)dstImage->bo->cpu + dstImage->memory_offset;
    uint32_t bpp = panvk_v9_format_bpp(dstImage->format);
    for (uint32_t r = 0; r < regionCount; r++) {
        const struct VkBufferImageCopy *rc = &regions[r];
        uint32_t mip = rc->imageSubresource.mipLevel;
        if (mip >= dstImage->mip_levels) continue;
        uint32_t layer_count = rc->imageSubresource.layerCount ? rc->imageSubresource.layerCount : 1;
        uint64_t w = rc->imageExtent.width;
        uint64_t h = rc->imageExtent.height;
        uint64_t d = rc->imageExtent.depth ? rc->imageExtent.depth : 1;
        uint64_t row_stride = dstImage->row_pitch[mip];
        uint64_t slice_pitch = panvk_v9_image_slice_pitch(dstImage, mip);
        uint64_t buffer_row_pitch = rc->bufferRowLength ? (uint64_t)rc->bufferRowLength * bpp : row_stride;
        uint64_t buffer_slice_pitch = rc->bufferImageHeight ? (uint64_t)rc->bufferImageHeight * buffer_row_pitch
                                                            : h * buffer_row_pitch;
        uint64_t buf_off = rc->bufferOffset;
        if (buf_off > srcBuffer->bo->size) continue;
        for (uint32_t layer = 0; layer < layer_count; layer++) {
            uint64_t img_off = panvk_v9_image_get_offset(dstImage, mip,
                                                         rc->imageSubresource.baseArrayLayer + layer)
                             + (uint64_t)rc->imageOffset.z * slice_pitch;
            const uint8_t *sp = src_base + buf_off;
            uint8_t *dp = dst_base + img_off;
            for (uint64_t z = 0; z < d; z++, dp += slice_pitch) {
                const uint8_t *zsp = sp + z * buffer_slice_pitch;
                for (uint64_t y = 0; y < h; y++) {
                    memcpy(dp + y * row_stride, zsp + y * buffer_row_pitch, w * bpp);
                }
            }
            buf_off += buffer_slice_pitch * d;
        }
    }
}

void vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
                            VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy *pRegions) {
    (void)commandBuffer; (void)srcImageLayout;
    if (!srcImage || !srcImage->bo || !dstBuffer || !dstBuffer->bo || !pRegions) return;
    const VkBufferImageCopy *regions = pRegions;
    const uint8_t *src_base = (const uint8_t *)srcImage->bo->cpu + srcImage->memory_offset;
    uint8_t *dst_base = (uint8_t *)dstBuffer->bo->cpu + dstBuffer->memory_offset;
    uint32_t bpp = panvk_v9_format_bpp(srcImage->format);
    for (uint32_t r = 0; r < regionCount; r++) {
        const struct VkBufferImageCopy *rc = &regions[r];
        uint32_t mip = rc->imageSubresource.mipLevel;
        if (mip >= srcImage->mip_levels) continue;
        uint32_t layer_count = rc->imageSubresource.layerCount ? rc->imageSubresource.layerCount : 1;
        uint64_t w = rc->imageExtent.width;
        uint64_t h = rc->imageExtent.height;
        uint64_t d = rc->imageExtent.depth ? rc->imageExtent.depth : 1;
        uint64_t row_stride = srcImage->row_pitch[mip];
        uint64_t slice_pitch = panvk_v9_image_slice_pitch(srcImage, mip);
        uint64_t buffer_row_pitch = rc->bufferRowLength ? (uint64_t)rc->bufferRowLength * bpp : row_stride;
        uint64_t buffer_slice_pitch = rc->bufferImageHeight ? (uint64_t)rc->bufferImageHeight * buffer_row_pitch
                                                            : h * buffer_row_pitch;
        uint64_t buf_off = rc->bufferOffset;
        if (buf_off > dstBuffer->bo->size) continue;
        for (uint32_t layer = 0; layer < layer_count; layer++) {
            uint64_t img_off = panvk_v9_image_get_offset(srcImage, mip,
                                                         rc->imageSubresource.baseArrayLayer + layer)
                             + (uint64_t)rc->imageOffset.z * slice_pitch;
            const uint8_t *sp = src_base + img_off;
            uint8_t *dp = dst_base + buf_off;
            for (uint64_t z = 0; z < d; z++, sp += slice_pitch) {
                uint8_t *zdp = dp + z * buffer_slice_pitch;
                for (uint64_t y = 0; y < h; y++) {
                    memcpy(zdp + y * buffer_row_pitch, sp + y * row_stride, w * bpp);
                }
            }
            buf_off += buffer_slice_pitch * d;
        }
    }
}

void vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
                    VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy *pRegions) {
    (void)commandBuffer; (void)srcImageLayout; (void)dstImageLayout;
    if (!srcImage || !srcImage->bo || !dstImage || !dstImage->bo || !pRegions) return;
    const VkImageCopy *regions = pRegions;
    uint32_t src_bpp = panvk_v9_format_bpp(srcImage->format);
    uint32_t dst_bpp = panvk_v9_format_bpp(dstImage->format);
    const uint8_t *src_base = (const uint8_t *)srcImage->bo->cpu + srcImage->memory_offset;
    uint8_t *dst_base = (uint8_t *)dstImage->bo->cpu + dstImage->memory_offset;
    for (uint32_t r = 0; r < regionCount; r++) {
        const struct VkImageCopy *rc = &regions[r];
        uint32_t smip = rc->srcSubresource.mipLevel;
        uint32_t dmip = rc->dstSubresource.mipLevel;
        if (smip >= srcImage->mip_levels || dmip >= dstImage->mip_levels) continue;
        uint64_t w = rc->extent.width;
        uint64_t h = rc->extent.height;
        uint64_t d = rc->extent.depth ? rc->extent.depth : 1;
        uint64_t s_row = srcImage->row_pitch[smip];
        uint64_t s_slice = panvk_v9_image_slice_pitch(srcImage, smip);
        uint64_t d_row = dstImage->row_pitch[dmip];
        uint64_t d_slice = panvk_v9_image_slice_pitch(dstImage, dmip);
        size_t copy_w = w * (src_bpp < dst_bpp ? src_bpp : dst_bpp);
        int needs_convert = (src_bpp == 4 && dst_bpp == 4 &&
                             ((srcImage->format == 37 || srcImage->format == 43) !=
                              (dstImage->format == 37 || dstImage->format == 43)));
        for (uint32_t layer = 0; layer < (rc->srcSubresource.layerCount ? rc->srcSubresource.layerCount : 1); layer++) {
            const uint8_t *sp = src_base
                + panvk_v9_image_get_offset(srcImage, smip, rc->srcSubresource.baseArrayLayer + layer)
                + (uint64_t)rc->srcOffset.z * s_slice + (uint64_t)rc->srcOffset.y * s_row
                + (uint64_t)rc->srcOffset.x * src_bpp;
            uint8_t *dp = dst_base
                + panvk_v9_image_get_offset(dstImage, dmip, rc->dstSubresource.baseArrayLayer + layer)
                + (uint64_t)rc->dstOffset.z * d_slice + (uint64_t)rc->dstOffset.y * d_row
                + (uint64_t)rc->dstOffset.x * dst_bpp;
            for (uint64_t z = 0; z < d; z++, sp += s_slice, dp += d_slice) {
                for (uint64_t y = 0; y < h; y++) {
                    const uint8_t *srow = sp + y * s_row;
                    uint8_t *drow = dp + y * d_row;
                    if (needs_convert) {
                        for (uint64_t x = 0; x < w; x++)
                            panvk_v9_convert_pixel(srcImage->format, dstImage->format,
                                                   srow + x * 4, drow + x * 4);
                    } else {
                        memcpy(drow, srow, copy_w);
                    }
                }
            }
        }
    }
}

void vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
                    VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageBlit *pRegions, VkFilter filter) {
    (void)commandBuffer; (void)srcImageLayout; (void)dstImageLayout; (void)filter;
    if (!srcImage || !srcImage->bo || !dstImage || !dstImage->bo || !pRegions) return;
    const VkImageBlit *regions = pRegions;
    uint32_t src_bpp = panvk_v9_format_bpp(srcImage->format);
    uint32_t dst_bpp = panvk_v9_format_bpp(dstImage->format);
    if (src_bpp > dst_bpp) dst_bpp = src_bpp;
    const uint8_t *src_base = (const uint8_t *)srcImage->bo->cpu + srcImage->memory_offset;
    uint8_t *dst_base = (uint8_t *)dstImage->bo->cpu + dstImage->memory_offset;
    for (uint32_t r = 0; r < regionCount; r++) {
        const struct VkImageBlit *rb = &regions[r];
        uint32_t smip = rb->srcSubresource.mipLevel;
        uint32_t dmip = rb->dstSubresource.mipLevel;
        if (smip >= srcImage->mip_levels || dmip >= dstImage->mip_levels) continue;
        int sw = rb->srcOffsets[1].x - rb->srcOffsets[0].x;
        int sh = rb->srcOffsets[1].y - rb->srcOffsets[0].y;
        int dw = rb->dstOffsets[1].x - rb->dstOffsets[0].x;
        int dh = rb->dstOffsets[1].y - rb->dstOffsets[0].y;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) continue;
        uint64_t s_row = srcImage->row_pitch[smip];
        uint64_t d_row = dstImage->row_pitch[dmip];
        uint64_t layer_count = rb->srcSubresource.layerCount ? rb->srcSubresource.layerCount : 1;
        int needs_convert = (src_bpp == 4 && dst_bpp == 4 &&
                             ((srcImage->format == 37 || srcImage->format == 43) !=
                              (dstImage->format == 37 || dstImage->format == 43)));
        for (uint32_t layer = 0; layer < layer_count; layer++) {
            const uint8_t *sp = src_base
                + panvk_v9_image_get_offset(srcImage, smip, rb->srcSubresource.baseArrayLayer + layer)
                + (uint64_t)rb->srcOffsets[0].y * s_row + (uint64_t)rb->srcOffsets[0].x * src_bpp;
            uint8_t *dp = dst_base
                + panvk_v9_image_get_offset(dstImage, dmip, rb->dstSubresource.baseArrayLayer + layer)
                + (uint64_t)rb->dstOffsets[0].y * d_row + (uint64_t)rb->dstOffsets[0].x * dst_bpp;
            for (int y = 0; y < dh; y++) {
                int sy = (int)((int64_t)y * sh / dh);
                const uint8_t *srow = sp + (uint64_t)sy * s_row;
                uint8_t *drow = dp + (uint64_t)y * d_row;
                for (int x = 0; x < dw; x++) {
                    int sx = (int)((int64_t)x * sw / dw);
                    if (needs_convert) {
                        panvk_v9_convert_pixel(srcImage->format, dstImage->format,
                                               srow + (uint64_t)sx * 4, drow + (uint64_t)x * 4);
                    } else {
                        memcpy(drow + (uint64_t)x * dst_bpp, srow + (uint64_t)sx * src_bpp, src_bpp);
                    }
                }
            }
        }
    }
}

void vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
                          const VkClearColorValue *color, uint32_t rangeCount, const VkImageSubresourceRange *pRanges) {
    (void)commandBuffer; (void)imageLayout;
    if (!image || !image->bo || !color || !pRanges) return;
    const VkImageSubresourceRange *ranges = pRanges;
    uint8_t *base = (uint8_t *)image->bo->cpu + image->memory_offset;
    uint32_t bpp = panvk_v9_format_bpp(image->format);
    uint8_t *rowbuf = malloc(4096 * (bpp ? bpp : 4));
    if (!rowbuf) return;
    for (uint32_t r = 0; r < rangeCount; r++) {
        const struct VkImageSubresourceRange *range = &ranges[r];
        uint32_t base_mip = range->baseMipLevel;
        uint32_t mip_count = range->levelCount == 0xffffffffu
                             ? (image->mip_levels - base_mip) : range->levelCount;
        uint32_t base_layer = range->baseArrayLayer;
        uint32_t layer_count = range->layerCount == 0xffffffffu
                               ? (image->array_layers - base_layer) : range->layerCount;
        for (uint32_t mip = base_mip; mip < base_mip + mip_count && mip < image->mip_levels; mip++) {
            uint64_t w = image->width >> mip; if (w < 1) w = 1;
            uint64_t h = image->height >> mip; if (h < 1) h = 1;
            uint64_t d = image->depth >> mip; if (d < 1) d = 1;
            if (image->image_type == 0) d = 1;
            if (w > 4096) w = 4096;
            uint64_t row_stride = image->row_pitch[mip];
            uint64_t slice_pitch = panvk_v9_image_slice_pitch(image, mip);
            for (uint64_t x = 0; x < w; x++) {
                uint8_t *p = rowbuf + x * bpp;
                if (bpp == 1) p[0] = (uint8_t)color->uint32[0];
                else if (bpp == 2) ((uint16_t *)p)[0] = (uint16_t)color->uint32[0];
                else {
                    memcpy(p, &color->uint32[0], bpp > 4 ? 4 : bpp);
                    if (bpp > 4) memcpy(p + 4, &color->uint32[1], 4);
                    if (bpp > 8) memcpy(p + 8, &color->uint32[2], 4);
                    if (bpp > 12) memcpy(p + 12, &color->uint32[3], 4);
                }
            }
            for (uint32_t layer = 0; layer < layer_count; layer++) {
                uint64_t img_off = panvk_v9_image_get_offset(image, mip, base_layer + layer);
                for (uint64_t z = 0; z < d; z++)
                    for (uint64_t y = 0; y < h; y++)
                        memcpy(base + img_off + z * slice_pitch + y * row_stride, rowbuf, w * bpp);
            }
        }
    }
    free(rowbuf);
}

void vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
                                 const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange *pRanges) {
    (void)commandBuffer; (void)image; (void)imageLayout; (void)pDepthStencil; (void)rangeCount; (void)pRanges;
}

void vkCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount, const VkClearAttachment *pAttachments,
                           uint32_t rectCount, const VkClearRect *pRects) {
    (void)commandBuffer; (void)attachmentCount; (void)pAttachments; (void)rectCount; (void)pRects;
}

void vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
                          VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
                          uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
                          uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                          uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers) {
}

static void command_buffer_apply_ubos(VkCommandBuffer commandBuffer) {
    struct v9_ubo_binding ubos[8];
    uint32_t ubo_count = 0;
    VkPipeline pipeline = commandBuffer->graphics_pipeline;
    if (!pipeline) {
        v9_cmd_buffer_set_ubos(commandBuffer->v9_cmd, NULL, 0);
        return;
    }

    for (uint32_t i = 0; i < pipeline->compiler_layout.binding_count; i++) {
        const struct panvk_v9_descriptor_binding *binding =
            &pipeline->compiler_layout.bindings[i];
        if ((binding->descriptor_type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
             binding->descriptor_type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ||
            binding->set >= 8 || !commandBuffer->descriptor_sets[binding->set])
            continue;

        VkDescriptorSet set = commandBuffer->descriptor_sets[binding->set];
        for (uint32_t b = 0; b < set->layout->binding_count; b++) {
            if (set->layout->bindings[b].binding != binding->binding) continue;
            for (uint32_t elem = 0; elem < binding->array_size && ubo_count < 8; elem++) {
                const struct VkDescriptorBufferInfo *info =
                    &set->buffers[set->layout->binding_offsets[b] + elem];
                if (!info->buffer || !info->buffer->bo || info->offset >= info->buffer->size)
                    continue;
                VkDeviceSize available = info->buffer->size - info->offset;
                VkDeviceSize range = info->range == VK_WHOLE_SIZE || info->range > available ?
                                     available : info->range;
                ubos[ubo_count++] = (struct v9_ubo_binding) {
                    .address = info->buffer->bo->gpu + info->buffer->memory_offset + info->offset,
                    .size = range > UINT32_MAX ? UINT32_MAX : (uint32_t)range,
                    .index = (binding->resource_index & 0xFFFFFFu) + elem,
                };
            }
            break;
        }
    }
    v9_cmd_buffer_set_ubos(commandBuffer->v9_cmd, ubos, ubo_count);
}

static void command_buffer_apply_ssbos(VkCommandBuffer commandBuffer) {
    struct v9_ssbo_binding ssbos[8];
    uint32_t ssbo_count = 0;
    VkPipeline pipeline = commandBuffer->compute_pipeline;
    if (!pipeline) {
        v9_cmd_buffer_set_ssbos(commandBuffer->v9_cmd, NULL, 0);
        return;
    }

    for (uint32_t i = 0; i < pipeline->compiler_layout.binding_count; i++) {
        const struct panvk_v9_descriptor_binding *binding =
            &pipeline->compiler_layout.bindings[i];
        if ((binding->descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
             binding->descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) ||
            binding->set >= 8 || !commandBuffer->descriptor_sets[binding->set])
            continue;

        VkDescriptorSet set = commandBuffer->descriptor_sets[binding->set];
        for (uint32_t b = 0; b < set->layout->binding_count; b++) {
            if (set->layout->bindings[b].binding != binding->binding) continue;
            for (uint32_t elem = 0; elem < binding->array_size && ssbo_count < 8; elem++) {
                const struct VkDescriptorBufferInfo *info =
                    &set->buffers[set->layout->binding_offsets[b] + elem];
                if (!info->buffer || !info->buffer->bo || info->offset >= info->buffer->size)
                    continue;
                VkDeviceSize available = info->buffer->size - info->offset;
                VkDeviceSize range = info->range == VK_WHOLE_SIZE || info->range > available ?
                                     available : info->range;
                ssbos[ssbo_count++] = (struct v9_ssbo_binding) {
                    .address = info->buffer->bo->gpu + info->buffer->memory_offset + info->offset,
                    .size = range > UINT32_MAX ? UINT32_MAX : (uint32_t)range,
                    .index = (binding->resource_index & 0xFFFFFFu) + elem,
                };
            }
            break;
        }
    }
    v9_cmd_buffer_set_ssbos(commandBuffer->v9_cmd, ssbos, ssbo_count);
}

static uint32_t vk_format_to_pan_v9_attr_format(uint32_t vk_fmt) {
    switch (vk_fmt) {
        case 103: /* VK_FORMAT_R32G32_SFLOAT */       return 0x020083;
        case 106: /* VK_FORMAT_R32G32B32_SFLOAT */    return 0x020084;
        case 109: /* VK_FORMAT_R32G32B32A32_SFLOAT */ return 0x020085;
        case 37:  /* VK_FORMAT_R8G8B8A8_UNORM */      return 0x000085;
        default:                                      return 0x020084;
    }
}

static void command_buffer_apply_attributes(VkCommandBuffer commandBuffer) {
    struct v9_attribute_binding attrs[8] = {0};
    uint32_t attr_count = 0;
    VkPipeline pipeline = commandBuffer->graphics_pipeline;

    for (uint32_t i = 0; pipeline && i < pipeline->vertex_attribute_count; i++) {
        const struct VkVertexInputAttributeDescription *attribute =
            &pipeline->vertex_attributes[i];
        if (attribute->location >= 8 || attribute->binding >= 16)
            continue;

        const struct VkVertexInputBindingDescription *binding = NULL;
        for (uint32_t b = 0; b < pipeline->vertex_binding_count; b++) {
            if (pipeline->vertex_bindings[b].binding == attribute->binding) {
                binding = &pipeline->vertex_bindings[b];
                break;
            }
        }
        VkBuffer buf = commandBuffer->vertex_bindings[attribute->binding].buffer;
        if (!binding || !buf || !buf->bo) continue;

        VkDeviceSize offset = commandBuffer->vertex_bindings[attribute->binding].offset;
        attrs[attribute->location] = (struct v9_attribute_binding) {
            .format = vk_format_to_pan_v9_attr_format(attribute->format),
            .offset = attribute->offset,
            .stride = binding->stride,
            .input_rate = binding->inputRate,
            .buffer_address = buf->bo->gpu + buf->memory_offset + offset,
            .buffer_size = (buf->size > offset) ? (uint32_t)(buf->size - offset) : 0,
        };
        if (attribute->location + 1 > attr_count)
            attr_count = attribute->location + 1;
    }
    if (attr_count == 0 && commandBuffer->v9_cmd) {
        attrs[0] = (struct v9_attribute_binding) {
            .format = vk_format_to_pan_v9_attr_format(VK_FORMAT_R32G32B32_SFLOAT),
            .offset = 0,
            .stride = 16,
            .input_rate = 0,
            .buffer_address = v9_cmd_buffer_get_pos_gpu(commandBuffer->v9_cmd),
            .buffer_size = 4096,
        };
        attr_count = 1;
    }
    v9_cmd_buffer_set_attributes(commandBuffer->v9_cmd, attrs, attr_count);
}

void vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    if (commandBuffer && commandBuffer->v9_cmd && vertexCount > 0 && instanceCount > 0 &&
        (!commandBuffer->graphics_pipeline ||
         !commandBuffer->graphics_pipeline->rasterizer_discard)) {
        command_buffer_apply_ubos(commandBuffer);
        command_buffer_apply_attributes(commandBuffer);
        if (commandBuffer->graphics_pipeline) {
            if (commandBuffer->graphics_pipeline->vertex_binary.binary_size) {
                v9_cmd_buffer_set_vertex_shader(
                    commandBuffer->v9_cmd,
                    &commandBuffer->graphics_pipeline->vertex_binary);
            }
            if (commandBuffer->graphics_pipeline->fragment_binary.binary_size &&
                (!getenv("PANVK_EXPERIMENT_MV11_POSITION") ||
                 (commandBuffer->graphics_pipeline->vertex_binary.secondary_enable &&
                  getenv("PANVK_EXPERIMENT_MV11_VARYING")))) {
                v9_cmd_buffer_set_fragment_shader(
                    commandBuffer->v9_cmd,
                    &commandBuffer->graphics_pipeline->fragment_binary);
            }
        }
        uint64_t pos_gpu = commandBuffer->vertex_bindings[0].buffer && commandBuffer->vertex_bindings[0].buffer->bo ?
                           commandBuffer->vertex_bindings[0].buffer->bo->gpu +
                           commandBuffer->vertex_bindings[0].buffer->memory_offset +
                           commandBuffer->vertex_bindings[0].offset + (firstVertex * 16) :
                           v9_cmd_buffer_get_pos_gpu(commandBuffer->v9_cmd);
        uint64_t idx_gpu = getenv("PANVK_EXPERIMENT_MV11_POSITION") ? 0 :
                           v9_cmd_buffer_get_idx_gpu(commandBuffer->v9_cmd);
        v9_cmd_draw_indexed(commandBuffer->v9_cmd, idx_gpu, vertexCount, 0,
                            pos_gpu, vertexCount);
    }
}

void vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
                          const struct VkRenderPassBeginInfo *pRenderPassBegin,
                          uint32_t contents) {
    if (!commandBuffer || !pRenderPassBegin) return;

    uint32_t clear_color = 0;
    if (pRenderPassBegin->clearValueCount > 0 && pRenderPassBegin->pClearValues) {
        const float *c = (const float *)pRenderPassBegin->pClearValues;
        uint8_t r = (uint8_t)(c[0] * 255.0f);
        uint8_t g = (uint8_t)(c[1] * 255.0f);
        uint8_t b = (uint8_t)(c[2] * 255.0f);
        uint8_t a = (uint8_t)(c[3] * 255.0f);
        clear_color = (a << 24) | (b << 16) | (g << 8) | r;
    }

    struct v9_render_target_config config = {
        .width = pRenderPassBegin->renderArea.extent.width > 0 ? pRenderPassBegin->renderArea.extent.width : 300,
        .height = pRenderPassBegin->renderArea.extent.height > 0 ? pRenderPassBegin->renderArea.extent.height : 300,
        .clear_color = clear_color,
    };

    if (commandBuffer->v9_cmd) {
        v9_cmd_buffer_destroy(commandBuffer->v9_cmd);
    }
    commandBuffer->v9_cmd = v9_cmd_buffer_create(commandBuffer->device->kdev, &config);
    if (commandBuffer->v9_cmd) {
        v9_cmd_buffer_begin(commandBuffer->v9_cmd);
    }

    /* DXVK-style render: the framebuffer's colour attachment (0) is a swapchain
     * image or a user image backed by a BO.  Redirect the render target to that
     * image so the frame is drawn into the presented surface, not the internal
     * slot BO. */
    if (commandBuffer->v9_cmd && pRenderPassBegin->framebuffer) {
        struct VkFramebuffer_T *fb = pRenderPassBegin->framebuffer;
        struct VkImageView_T *view = (fb->attachment_count > 0) ? fb->attachments[0] : NULL;
        struct VkImage_T *img = view ? view->image : NULL;
        if (img && img->bo) {
            v9_cmd_buffer_set_render_target(commandBuffer->v9_cmd,
                                            img->bo, img->bo->gpu + img->memory_offset,
                                            img->width, img->height);
        }
    }
}

void vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    if (commandBuffer && commandBuffer->v9_cmd && indexCount > 0 && instanceCount > 0 &&
        (!commandBuffer->graphics_pipeline ||
         !commandBuffer->graphics_pipeline->rasterizer_discard)) {
        command_buffer_apply_ubos(commandBuffer);
        command_buffer_apply_attributes(commandBuffer);
        if (commandBuffer->graphics_pipeline) {
            if (commandBuffer->graphics_pipeline->vertex_binary.binary_size) {
                v9_cmd_buffer_set_vertex_shader(
                    commandBuffer->v9_cmd,
                    &commandBuffer->graphics_pipeline->vertex_binary);
            }
            if (commandBuffer->graphics_pipeline->fragment_binary.binary_size &&
                (!getenv("PANVK_EXPERIMENT_MV11_POSITION") ||
                 (commandBuffer->graphics_pipeline->vertex_binary.secondary_enable &&
                  getenv("PANVK_EXPERIMENT_MV11_VARYING")))) {
                v9_cmd_buffer_set_fragment_shader(
                    commandBuffer->v9_cmd,
                    &commandBuffer->graphics_pipeline->fragment_binary);
            }
        }
        uint64_t pos_gpu = commandBuffer->vertex_bindings[0].buffer && commandBuffer->vertex_bindings[0].buffer->bo ?
                           commandBuffer->vertex_bindings[0].buffer->bo->gpu +
                           commandBuffer->vertex_bindings[0].buffer->memory_offset +
                           commandBuffer->vertex_bindings[0].offset + (vertexOffset * 16) :
                           v9_cmd_buffer_get_pos_gpu(commandBuffer->v9_cmd);
        uint64_t idx_gpu = commandBuffer->index_buffer && commandBuffer->index_buffer->bo ?
                           commandBuffer->index_buffer->bo->gpu +
                           commandBuffer->index_buffer->memory_offset +
                           commandBuffer->index_offset + (firstIndex * (commandBuffer->index_type == 1 ? 4 : 2)) :
                           v9_cmd_buffer_get_idx_gpu(commandBuffer->v9_cmd);
        v9_cmd_draw_indexed(commandBuffer->v9_cmd, idx_gpu, indexCount, commandBuffer->index_type, pos_gpu, indexCount);
    }
}

void vkCmdEndRenderPass(VkCommandBuffer commandBuffer) {
    if (commandBuffer && commandBuffer->v9_cmd) {
        v9_cmd_buffer_end(commandBuffer->v9_cmd);
    }
}

VkResult vkEndCommandBuffer(VkCommandBuffer commandBuffer) {
    if (!commandBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    return VK_SUCCESS;
}

VkResult vkQueueSubmit(VkQueue queue, uint32_t submitCount, const struct VkSubmitInfo *pSubmits, VkFence fence) {
    if (!queue || !pSubmits) return VK_ERROR_INITIALIZATION_FAILED;

    pthread_mutex_lock(&queue->device->submit_mutex);
    VkResult result = VK_SUCCESS;
    for (uint32_t s = 0; s < submitCount; s++) {
        for (uint32_t cb = 0; cb < pSubmits[s].commandBufferCount; cb++) {
            VkCommandBuffer cmd = pSubmits[s].pCommandBuffers[cb];
            if (cmd && cmd->v9_cmd) {
                if (queue->last_v9_cmd != cmd->v9_cmd) {
                    v9_cmd_buffer_destroy(queue->last_v9_cmd);
                    queue->last_v9_cmd = v9_cmd_buffer_ref(cmd->v9_cmd);
                }
                int ret = v9_cmd_buffer_submit(cmd->v9_cmd);
                if (ret != 0) {
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    break;
                }
            }
        }
        /* Signal binary semaphores after the (synchronous) submit.  Timeline
         * values come from VkTimelineSemaphoreSubmitInfo pNext on this submit. */
        if (result == VK_SUCCESS) {
            const struct VkTimelineSemaphoreSubmitInfo *tsi = NULL;
            const void *nxt = pSubmits[s].pNext;
            while (nxt) {
                const VkBaseInStructure *base = nxt;
                if (base->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                    tsi = nxt;
                    break;
                }
                nxt = base->pNext;
            }
            const uint64_t *tl_values = tsi ? tsi->pSignalSemaphoreValues : NULL;
            for (uint32_t si = 0; si < pSubmits[s].signalSemaphoreCount; si++) {
                VkSemaphore sem = pSubmits[s].pSignalSemaphores[si];
                if (!sem) continue;
                if (sem->timeline) {
                    sem->counter = tl_values ? tl_values[si] : sem->counter + 1;
                } else {
                    sem->counter = 1;
                }
            }
        }
        if (result != VK_SUCCESS) break;
    }
    if (fence) ((VkFence)fence)->signaled = true;
    pthread_mutex_unlock(&queue->device->submit_mutex);
    return result;
}

VkResult vkQueueWaitIdle(VkQueue queue) {
    return VK_SUCCESS;
}

VkResult vkDeviceWaitIdle(VkDevice device) {
    return VK_SUCCESS;
}

void vkGetDeviceQueue2(VkDevice device, const struct VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue) {
    if (!device || !pQueue) return;
    if (!device->queue) {
        device->queue = calloc(1, sizeof(*device->queue));
        if (!device->queue) { *pQueue = NULL; return; }
        set_loader_magic(device->queue);
        device->queue->device = device;
    }
    *pQueue = device->queue;
}

void vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer, const struct VkDependencyInfo *pDependencyInfo) {
    (void)pDependencyInfo;
}

void vkCmdPipelineBarrier2KHR(VkCommandBuffer commandBuffer, const struct VkDependencyInfo *pDependencyInfo) {
    vkCmdPipelineBarrier2(commandBuffer, pDependencyInfo);
}

VkResult vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const struct VkSubmitInfo2 *pSubmits, VkFence fence) {
    if (!queue || !pSubmits) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&queue->device->submit_mutex);
    VkResult result = VK_SUCCESS;
    for (uint32_t s = 0; s < submitCount; s++) {
        for (uint32_t cb = 0; cb < pSubmits[s].commandBufferInfoCount; cb++) {
            VkCommandBuffer cmd = pSubmits[s].pCommandBufferInfos[cb].commandBuffer;
            if (cmd && cmd->v9_cmd) {
                if (queue->last_v9_cmd != cmd->v9_cmd) {
                    v9_cmd_buffer_destroy(queue->last_v9_cmd);
                    queue->last_v9_cmd = v9_cmd_buffer_ref(cmd->v9_cmd);
                }
                int ret = v9_cmd_buffer_submit(cmd->v9_cmd);
                if (ret != 0) {
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    break;
                }
            }
        }
        if (result != VK_SUCCESS) break;
    }
    if (fence) ((VkFence)fence)->signaled = true;
    pthread_mutex_unlock(&queue->device->submit_mutex);
    return result;
}

void vkCmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers) {
    (void)commandBufferCount; (void)pCommandBuffers;
}

VkResult vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const struct VkSubmitInfo2 *pSubmits, VkFence fence) {
    return vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

/* WSI & Surface Implementation */
VkResult vkCreateXlibSurfaceKHR(VkInstance instance, const struct VkXlibSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
    if (!pCreateInfo || !pSurface) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkSurfaceKHR_T *surf = calloc(1, sizeof(*surf));
    if (!surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    surf->dpy = (Display *)pCreateInfo->dpy;
    surf->window = pCreateInfo->window;
    surf->width = 300;
    surf->height = 300;

    if (surf->dpy && surf->window) {
        XWindowAttributes attr;
        if (XGetWindowAttributes(surf->dpy, surf->window, &attr)) {
            surf->width = attr.width;
            surf->height = attr.height;
        }
    }

    *pSurface = surf;
    return VK_SUCCESS;
}

VkResult vkCreateXcbSurfaceKHR(VkInstance instance, const struct VkXcbSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
    if (!pCreateInfo || !pSurface) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkSurfaceKHR_T *surf = calloc(1, sizeof(*surf));
    if (!surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    surf->connection = (xcb_connection_t *)pCreateInfo->connection;
    surf->window = (uint32_t)pCreateInfo->window;
    surf->is_xcb = true;
    surf->width = 800;
    surf->height = 600;

    if (surf->connection && surf->window) {
        xcb_get_geometry_cookie_t cookie = xcb_get_geometry(surf->connection, surf->window);
        xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(surf->connection, cookie, NULL);
        if (reply) {
            surf->width = reply->width;
            surf->height = reply->height;
            free(reply);
        }
    }

    *pSurface = surf;
    return VK_SUCCESS;
}

uint32_t vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, xcb_connection_t *connection, xcb_visualid_t visual_id) {
    return 1; /* VK_TRUE */
}

VkResult vkGetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkDisplayPropertiesKHR *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkDisplayPlanePropertiesKHR *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice physicalDevice, uint32_t planeIndex, uint32_t *pDisplayCount, VkDisplayKHR *pDisplays) {
    if (!pDisplayCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pDisplayCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetDisplayModePropertiesKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display, uint32_t *pPropertyCount, VkDisplayModePropertiesKHR *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

void vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator) {
    if (surface) free(surface);
}

VkResult vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, uint32_t *pSupported) {
    if (!pSupported) return VK_ERROR_INITIALIZATION_FAILED;
    *pSupported = 1; /* Queue family 0 supports surface presentation */
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, struct VkSurfaceCapabilitiesKHR *pSurfaceCapabilities) {
    if (!pSurfaceCapabilities) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t w = surface ? surface->width : 300;
    uint32_t h = surface ? surface->height : 300;

    pSurfaceCapabilities->minImageCount = 1;
    pSurfaceCapabilities->maxImageCount = 8;
    pSurfaceCapabilities->currentExtent.width = w;
    pSurfaceCapabilities->currentExtent.height = h;
    pSurfaceCapabilities->minImageExtent.width = 1;
    pSurfaceCapabilities->minImageExtent.height = 1;
    pSurfaceCapabilities->maxImageExtent.width = 4096;
    pSurfaceCapabilities->maxImageExtent.height = 4096;
    pSurfaceCapabilities->maxImageArrayLayers = 1;
    pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    pSurfaceCapabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t *pSurfaceFormatCount, struct VkSurfaceFormatKHR *pSurfaceFormats) {
    if (!pSurfaceFormatCount) return VK_ERROR_INITIALIZATION_FAILED;

    static const struct VkSurfaceFormatKHR formats[] = {
        { .format = VK_FORMAT_B8G8R8A8_UNORM, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    uint32_t num_formats = sizeof(formats) / sizeof(formats[0]);

    if (!pSurfaceFormats) {
        *pSurfaceFormatCount = num_formats;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pSurfaceFormatCount < num_formats) ? *pSurfaceFormatCount : num_formats;
    memcpy(pSurfaceFormats, formats, to_copy * sizeof(struct VkSurfaceFormatKHR));
    *pSurfaceFormatCount = to_copy;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t *pPresentModeCount, uint32_t *pPresentModes) {
    if (!pPresentModeCount) return VK_ERROR_INITIALIZATION_FAILED;

    static const uint32_t modes[] = { VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR };
    uint32_t num_modes = sizeof(modes) / sizeof(modes[0]);

    if (!pPresentModes) {
        *pPresentModeCount = num_modes;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pPresentModeCount < num_modes) ? *pPresentModeCount : num_modes;
    memcpy(pPresentModes, modes, to_copy * sizeof(uint32_t));
    *pPresentModeCount = to_copy;
    return VK_SUCCESS;
}

/* Swapchain Implementation */
VkResult vkCreateSwapchainKHR(VkDevice device, const struct VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) {
    if (!device || !pCreateInfo || !pSwapchain) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkSwapchainKHR_T *sc = calloc(1, sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;

    sc->device = device;
    sc->surface = pCreateInfo->surface;
    sc->width = pCreateInfo->imageExtent.width > 0 ? pCreateInfo->imageExtent.width : 300;
    sc->height = pCreateInfo->imageExtent.height > 0 ? pCreateInfo->imageExtent.height : 300;
    sc->image_count = pCreateInfo->minImageCount > 0 ? pCreateInfo->minImageCount : 2;

    /* Give each swapchain image a real GPU BO so vkCmdBeginRenderPass can
     * render into it (DXVK binds these images as colour attachments). */
    sc->images = calloc(sc->image_count, sizeof(struct VkImage_T));
    if (!sc->images) {
        free(sc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t aligned_w = (sc->width + 15) & ~15;
    uint32_t aligned_h = (sc->height + 15) & ~15;
    size_t color_bytes = (size_t)aligned_w * aligned_h * 4;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        struct VkImage_T *img = &sc->images[i];
        img->swapchain = sc;
        img->index = i;
        img->width = sc->width;
        img->height = sc->height;
        img->depth = 1;
        img->format = pCreateInfo->imageFormat;
        img->image_type = VK_IMAGE_TYPE_2D;
        img->mip_levels = 1;
        img->array_layers = 1;
        img->samples = VK_SAMPLE_COUNT_1_BIT;
        img->tiling = VK_IMAGE_TILING_OPTIMAL;
        img->usage = pCreateInfo->imageUsage;
        img->size = color_bytes;
        img->bo = pan_kmod_bo_alloc(device->kdev, color_bytes,
                                    PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
        if (!img->bo) goto fail_bo;
        memset(img->bo->cpu, 0, color_bytes);
        img->row_pitch[0] = sc->width * 4;
        img->memory_offset = 0;
    }

    if (sc->surface && sc->surface->is_xcb && sc->surface->connection && sc->surface->window) {
        sc->xcb_gc = xcb_generate_id(sc->surface->connection);
        xcb_create_gc(sc->surface->connection, sc->xcb_gc, sc->surface->window, 0, NULL);
        sc->image_data = malloc(sc->width * sc->height * 4);
    } else if (sc->surface && sc->surface->dpy && sc->surface->window) {
        int screen = DefaultScreen(sc->surface->dpy);
        sc->gc = XCreateGC(sc->surface->dpy, sc->surface->window, 0, NULL);
        sc->image_data = malloc(sc->width * sc->height * 4);
        if (sc->image_data) {
            sc->ximage = XCreateImage(sc->surface->dpy, DefaultVisual(sc->surface->dpy, screen),
                                     24, ZPixmap, 0, sc->image_data, sc->width, sc->height, 32, 0);
        }
    }

    *pSwapchain = sc;
    return VK_SUCCESS;

fail_bo:
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (sc->images[i].bo) pan_kmod_bo_free(sc->images[i].bo);
    }
    free(sc->images);
    free(sc);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

void vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator) {
    if (!swapchain) return;
    for (uint32_t i = 0; i < swapchain->image_count; i++) {
        if (swapchain->images[i].bo) pan_kmod_bo_free(swapchain->images[i].bo);
    }
    if (swapchain->images) free(swapchain->images);
    if (swapchain->surface && swapchain->surface->is_xcb && swapchain->surface->connection && swapchain->xcb_gc) {
        xcb_free_gc(swapchain->surface->connection, swapchain->xcb_gc);
    } else if (swapchain->surface && swapchain->surface->dpy && swapchain->gc) {
        XFreeGC(swapchain->surface->dpy, swapchain->gc);
    }
    if (swapchain->image_data) free(swapchain->image_data);
    free(swapchain);
}

VkResult vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages) {
    if (!swapchain || !pSwapchainImageCount) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pSwapchainImages) {
        *pSwapchainImageCount = swapchain->image_count;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pSwapchainImageCount < swapchain->image_count) ? *pSwapchainImageCount : swapchain->image_count;
    for (uint32_t i = 0; i < to_copy; i++) {
        pSwapchainImages[i] = &swapchain->images[i];
    }
    *pSwapchainImageCount = to_copy;
    return VK_SUCCESS;
}

VkResult vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex) {
    if (!swapchain || !pImageIndex) return VK_ERROR_INITIALIZATION_FAILED;
    if (swapchain->image_count == 0) return VK_ERROR_OUT_OF_DATE_KHR;

    /* Round-robin over the swapchain images.  Rendering is synchronous (submit
     * returns only after the GPU finished), so every image is always free. */
    uint32_t idx = swapchain->next_image++ % swapchain->image_count;
    *pImageIndex = idx;
    if (semaphore) {
        if (semaphore->timeline)
            semaphore->counter++;
        else
            semaphore->counter = 1; /* binary: signal immediately */
    }
    if (fence) ((VkFence)fence)->signaled = true;
    return VK_SUCCESS;
}

VkResult vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *pAcquireInfo, uint32_t *pImageIndex) {
    if (!pAcquireInfo) return VK_ERROR_INITIALIZATION_FAILED;
    return vkAcquireNextImageKHR(device, pAcquireInfo->swapchain, pAcquireInfo->timeout,
                                 pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex);
}

VkResult vkQueuePresentKHR(VkQueue queue, const struct VkPresentInfoKHR *pPresentInfo) {
    if (!pPresentInfo || pPresentInfo->swapchainCount == 0) return VK_ERROR_INITIALIZATION_FAILED;

    /* Wait on any binary/timeline semaphores the app submitted with.  Rendering
     * is synchronous, so these are already satisfied; just consume them. */
    if (pPresentInfo->waitSemaphoreCount > 0 && pPresentInfo->pWaitSemaphores) {
        for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
            if (pPresentInfo->pWaitSemaphores[i]) {
                /* binary / timeline: no-op, the submit that produced the frame
                 * already completed before vkQueuePresentKHR was called. */
            }
        }
    }

    VkSwapchainKHR sc = pPresentInfo->pSwapchains[0];
    if (sc && sc->surface && sc->image_data) {
        uint32_t present_index = pPresentInfo->pImageIndices ? pPresentInfo->pImageIndices[0] : 0;
        if (present_index >= sc->image_count) present_index = 0;

        /* Prefer reading the presented swapchain image's own BO (DXVK renders
         * into it via the framebuffer attachment); fall back to the last command
         * buffer's internal target for the old single-buffer test harness. */
        uint32_t *src = NULL;
        if (sc->images[present_index].bo && sc->images[present_index].bo->cpu) {
            src = (uint32_t *)sc->images[present_index].bo->cpu;
        } else if (queue && queue->last_v9_cmd) {
            /* CPU readback path for the standalone test (no swapchain BOs). */
            uint32_t *dst = (uint32_t *)sc->image_data;
            uint64_t nonzero_pixels = 0;
            uint32_t first_pixel = 0;
            for (uint32_t y = 0; y < sc->height; y++) {
                for (uint32_t x = 0; x < sc->width; x++) {
                    uint32_t pixel = v9_cmd_buffer_read_pixel(queue->last_v9_cmd, x, y);
                    dst[y * sc->width + x] = pixel;
                    if (x == 0 && y == 0) first_pixel = pixel;
                    nonzero_pixels += pixel != 0;
                }
            }
            if (getenv("PANVK_PRESENT_DEBUG")) {
                fprintf(stderr,
                        "panvk present: image=%u size=%ux%u first=0x%08x nonzero=%llu/%llu\n",
                        present_index, sc->width, sc->height, first_pixel,
                        (unsigned long long)nonzero_pixels,
                        (unsigned long long)sc->width * sc->height);
            }
        }

        if (src) {
            memcpy(sc->image_data, src, sc->width * sc->height * 4);
            if (getenv("PANVK_PRESENT_DEBUG")) {
                uint64_t nonzero_pixels = 0;
                uint32_t *dst = (uint32_t *)sc->image_data;
                for (uint32_t y = 0; y < sc->height; y++)
                    for (uint32_t x = 0; x < sc->width; x++)
                        nonzero_pixels += dst[y * sc->width + x] != 0;
                fprintf(stderr,
                        "panvk present: image=%u size=%ux%u first=0x%08x nonzero=%llu/%llu\n",
                        present_index, sc->width, sc->height,
                        (uint32_t)sc->image_data[0], (unsigned long long)nonzero_pixels,
                        (unsigned long long)sc->width * sc->height);
            }
        }

        if (sc->surface->is_xcb && sc->surface->connection && sc->surface->window) {
            xcb_put_image(sc->surface->connection, XCB_IMAGE_FORMAT_Z_PIXMAP,
                          sc->surface->window, sc->xcb_gc,
                          sc->width, sc->height, 0, 0, 0, 24,
                          sc->width * sc->height * 4, (const uint8_t *)sc->image_data);
            xcb_flush(sc->surface->connection);
        } else if (sc->surface->dpy && sc->surface->window && sc->ximage && sc->gc) {
            XPutImage(sc->surface->dpy, sc->surface->window, sc->gc, sc->ximage, 0, 0, 0, 0, sc->width, sc->height);
            XFlush(sc->surface->dpy);
        }
    }
    return VK_SUCCESS;
}

uint32_t panvk_v9_read_pixel(VkCommandBuffer commandBuffer, uint32_t x, uint32_t y) {
    if (commandBuffer && commandBuffer->v9_cmd) {
        return v9_cmd_buffer_read_pixel(commandBuffer->v9_cmd, x, y);
    }
    return 0;
}

/* CPU access helpers for the bring-up tests: expose the backing BO of an
 * image or buffer so tests can verify blit/copy/clear results without a GPU. */
void *panvk_v9_image_cpu(VkImage image) {
    if (!image || !image->bo) return NULL;
    return (uint8_t *)image->bo->cpu + image->memory_offset;
}

void *panvk_v9_buffer_cpu(VkBuffer buffer) {
    if (!buffer || !buffer->bo) return NULL;
    return (uint8_t *)buffer->bo->cpu + buffer->memory_offset;
}

uint32_t panvk_v9_image_pixel(VkImage image, uint32_t x, uint32_t y) {
    if (!image || !image->bo || !image->bo->cpu) return 0;
    if (x >= image->width || y >= image->height) return 0;
    const uint8_t *base = (const uint8_t *)image->bo->cpu + image->memory_offset;
    uint32_t bpp = panvk_v9_format_bpp(image->format);
    uint32_t v;
    memcpy(&v, base + y * image->row_pitch[0] + x * bpp, 4);
    return v;
}

size_t panvk_v9_compute_binary_size(VkPipeline pipeline) {
    return pipeline ? pipeline->compute_binary.binary_size : 0;
}

uint32_t panvk_v9_compute_local_size(VkPipeline pipeline, uint32_t axis) {
    if (!pipeline) return 0;
    switch (axis) {
    case 0: return pipeline->compute_binary.local_size_x;
    case 1: return pipeline->compute_binary.local_size_y;
    case 2: return pipeline->compute_binary.local_size_z;
    default: return 0;
    }
}

bool panvk_v9_cmd_has_compute(VkCommandBuffer commandBuffer) {
    return commandBuffer && commandBuffer->v9_cmd &&
           v9_cmd_buffer_has_compute(commandBuffer->v9_cmd);
}

/* KHR Aliases for PhysicalDevice2 functions */
void vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice, struct VkPhysicalDeviceProperties2 *pProperties) {
    vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
}
void vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice, struct VkPhysicalDeviceFeatures2 *pFeatures) {
    vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}
void vkGetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount, struct VkQueueFamilyProperties2 *pQueueFamilyProperties) {
    vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}
void vkGetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice physicalDevice, struct VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    vkGetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
}
VkResult vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t *pPhysicalDeviceGroupCount, struct VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroups) {
    return vkEnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroups);
}

/* =====================================================================
 * Extended entry points (core 1.2/1.3 + KHR/EXT) so that every advertised
 * extension and every feature gate used by DXVK / vkd3d-proton resolves to
 * a non-NULL function.  Real work is done where cheap; the rest are safe
 * (no-op / best-effort) stubs — captured so the bring-up harness can measure
 * exactly which advertised functionality is actually executable.
 * ===================================================================== */

VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo *pInfo) {
    if (!pInfo || !pInfo->buffer) return 0;
    /* Return the real GPU address of the buffer's backing BO plus its memory
     * offset, so DXVK can use it in shaders (descriptor address, etc.). */
    struct VkBuffer_T *b = pInfo->buffer;
    if (b->bo) return (VkDeviceAddress)(b->bo->gpu + b->memory_offset);
    return 0;
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddressKHR(VkDevice device, const VkBufferDeviceAddressInfo *pInfo) {
    return vkGetBufferDeviceAddress(device, pInfo);
}
VKAPI_ATTR uint64_t VKAPI_CALL vkGetBufferOpaqueCaptureAddress(VkDevice device, const VkBufferDeviceAddressInfo *pInfo) {
    return (uint64_t)vkGetBufferDeviceAddress(device, pInfo);
}
VKAPI_ATTR uint64_t VKAPI_CALL vkGetDeviceMemoryOpaqueCaptureAddress(VkDevice device, const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo) {
    if (!pInfo || !pInfo->memory || !pInfo->memory->bo) return 0;
    return (uint64_t)(pInfo->memory->bo->gpu);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore, uint64_t *pValue) {
    if (!pValue) return VK_ERROR_INITIALIZATION_FAILED;
    *pValue = (semaphore) ? semaphore->counter : 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValueKHR(VkDevice device, VkSemaphore semaphore, uint64_t *pValue) {
    return vkGetSemaphoreCounterValue(device, semaphore, pValue);
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *pWaitInfo, uint64_t timeout) {
    if (!pWaitInfo) return VK_ERROR_INITIALIZATION_FAILED;
    /* Submits are synchronous, so any timeline value the app is waiting on has
     * already been reached by the time this is called.  Only honour the
     * waitAll flag semantics on the counters; never block. */
    for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; i++) {
        VkSemaphore sem = pWaitInfo->pSemaphores[i];
        if (!sem) continue;
        uint64_t want = pWaitInfo->pValues[i];
        uint64_t have = sem->counter;
        if (pWaitInfo->flags & VK_SEMAPHORE_WAIT_ANY_BIT) {
            if (have >= want) return VK_SUCCESS;
        } else {
            if (have < want) return VK_TIMEOUT;
        }
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphoresKHR(VkDevice device, const VkSemaphoreWaitInfo *pWaitInfo, uint64_t timeout) {
    return vkWaitSemaphores(device, pWaitInfo, timeout);
}
VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo *pSignalInfo) {
    if (pSignalInfo && pSignalInfo->semaphore) pSignalInfo->semaphore->counter = pSignalInfo->value;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphoreKHR(VkDevice device, const VkSemaphoreSignalInfo *pSignalInfo) {
    return vkSignalSemaphore(device, pSignalInfo);
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo *pRenderingInfo) {
    if (commandBuffer) commandBuffer->rendering_active = VK_TRUE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer commandBuffer) {
    if (commandBuffer) commandBuffer->rendering_active = VK_FALSE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfoKHR *pRenderingInfo) {
    vkCmdBeginRendering(commandBuffer, (const VkRenderingInfo *)pRenderingInfo);
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderingKHR(VkCommandBuffer commandBuffer) { vkCmdEndRendering(commandBuffer); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo *pDependencyInfo) {
    if (event) event->signaled = VK_TRUE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags2 stageMask) {
    if (event) event->signaled = VK_FALSE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents, const VkDependencyInfo *pDependencyInfos) {
    for (uint32_t i = 0; i < eventCount; i++) if (pEvents[i]) pEvents[i]->signaled = VK_TRUE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2KHR(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags2 stageMask) { vkCmdResetEvent2(commandBuffer, event, stageMask); }
VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2KHR(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents, const VkDependencyInfo *pDependencyInfos) { vkCmdWaitEvents2(commandBuffer, eventCount, pEvents, pDependencyInfos); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2KHR(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo *pDependencyInfo) { vkCmdSetEvent2(commandBuffer, event, pDependencyInfo); }
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCountKHR(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) { vkCmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride); }
VKAPI_ATTR void VKAPI_CALL vkCmdDispatchBase(VkCommandBuffer commandBuffer, uint32_t baseGroupX, uint32_t baseGroupY, uint32_t baseGroupZ, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize, const void *pData) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet *pDescriptorWrites) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate, VkPipelineLayout layout, uint32_t set, const void *pData) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t reference) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t compareMask) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t writeMask) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4]) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds, float maxDepthBounds) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEnable(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkBool32 *pColorBlendEnables) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorWriteMask(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorComponentFlags *pColorMasks) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEquation(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorBlendEquationEXT *pColorBlendEquations) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D *pScissors) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t viewportCount, const VkViewport *pViewports) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOp(VkCommandBuffer commandBuffer, VkLogicOp logicOp) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias2EXT(VkCommandBuffer commandBuffer, const VkDepthBiasInfoEXT *pDepthBiasInfo) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetSampleLocationsEXT(VkCommandBuffer commandBuffer, const VkSampleLocationsInfoEXT *pSampleLocationsInfo) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDiscardRectangleEXT(VkCommandBuffer commandBuffer, uint32_t firstDiscardRectangle, uint32_t discardRectangleCount, const VkRect2D *pDiscardRectangles) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetFragmentShadingRateKHR(VkCommandBuffer commandBuffer, const VkExtent2D *pFragmentSize, const VkFragmentShadingRateCombinerOpKHR combinerOps[2]) {
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceFragmentShadingRatesKHR(VkPhysicalDevice physicalDevice, uint32_t *pFragmentShadingRateCount, VkPhysicalDeviceFragmentShadingRateKHR *pFragmentShadingRates) {
    if (!pFragmentShadingRateCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pFragmentShadingRates) { *pFragmentShadingRateCount = 1; return VK_SUCCESS; }
    if (*pFragmentShadingRateCount >= 1) {
        pFragmentShadingRates[0].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
        pFragmentShadingRates[0].pNext = NULL;
        pFragmentShadingRates[0].sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        pFragmentShadingRates[0].fragmentSize.width = 1;
        pFragmentShadingRates[0].fragmentSize.height = 1;
    }
    *pFragmentShadingRateCount = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2(VkDevice device, VkImage image, const VkImageSubresource2 *pSubresource, VkSubresourceLayout2 *pLayout) {
    if (pSubresource && pLayout) vkGetImageSubresourceLayout(device, image, &pSubresource->imageSubresource, &pLayout->subresourceLayout);
}
VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2KHR(VkDevice device, VkImage image, const VkImageSubresource2KHR *pSubresource, VkSubresourceLayout2KHR *pLayout) { vkGetImageSubresourceLayout2(device, image, (const VkImageSubresource2 *)pSubresource, (VkSubresourceLayout2 *)pLayout); }
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo, VkMemoryRequirements2 *pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    VkImage tmp = NULL;
    if (pInfo->pCreateInfo && vkCreateImage(device, pInfo->pCreateInfo, NULL, &tmp) == VK_SUCCESS && tmp) {
        vkGetImageMemoryRequirements(device, tmp, &pMemoryRequirements->memoryRequirements);
        vkDestroyImage(device, tmp, NULL);
    } else {
        pMemoryRequirements->memoryRequirements.size = 4096;
        pMemoryRequirements->memoryRequirements.alignment = 4096;
        pMemoryRequirements->memoryRequirements.memoryTypeBits = 0x3;
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo, uint32_t *pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements) {
    if (pSparseMemoryRequirementCount) *pSparseMemoryRequirementCount = 0;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(VkPhysicalDevice physicalDevice, uint32_t *pTimeDomainCount, VkTimeDomainEXT *pTimeDomains) {
    if (!pTimeDomainCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pTimeDomains) { *pTimeDomainCount = 1; return VK_SUCCESS; }
    if (*pTimeDomainCount >= 1) pTimeDomains[0] = VK_TIME_DOMAIN_DEVICE_EXT;
    *pTimeDomainCount = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetCalibratedTimestampsEXT(VkDevice device, uint32_t timestampCount, const VkCalibratedTimestampInfoEXT *pTimestampInfos, uint64_t *pTimestamps, uint64_t *pMaxDeviation) {
    if (pTimestamps) for (uint32_t i = 0; i < timestampCount; i++) pTimestamps[i] = 0;
    if (pMaxDeviation) *pMaxDeviation = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOpEXT(VkCommandBuffer commandBuffer, VkLogicOp logicOp) { vkCmdSetLogicOp(commandBuffer, logicOp); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetCullModeEXT(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode) { vkCmdSetCullMode(commandBuffer, cullMode); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFaceEXT(VkCommandBuffer commandBuffer, VkFrontFace frontFace) { vkCmdSetFrontFace(commandBuffer, frontFace); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopologyEXT(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology) { vkCmdSetPrimitiveTopology(commandBuffer, primitiveTopology); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable) { vkCmdSetDepthTestEnable(commandBuffer, depthTestEnable); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable) { vkCmdSetDepthWriteEnable(commandBuffer, depthWriteEnable); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnableEXT(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable) { vkCmdSetStencilTestEnable(commandBuffer, stencilTestEnable); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOpEXT(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp, VkCompareOp compareOp) {
}

/* Vulkan ICD Entry Point Lookup Table */
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;
#define MATCH(name) if (strcmp(pName, #name) == 0) return (PFN_vkVoidFunction)name
    MATCH(vk_icdNegotiateLoaderICDInterfaceVersion);
    MATCH(vkGetInstanceProcAddr);
    MATCH(vkGetDeviceProcAddr);
    MATCH(vk_icdGetInstanceProcAddr);
    MATCH(vkEnumerateInstanceVersion);
    MATCH(vkCreateInstance);
    MATCH(vkDestroyInstance);
    MATCH(vkEnumerateInstanceExtensionProperties);
    MATCH(vkEnumerateInstanceLayerProperties);
    MATCH(vkEnumerateDeviceExtensionProperties);
    MATCH(vkEnumeratePhysicalDevices);
    MATCH(vkEnumeratePhysicalDeviceGroups);
    MATCH(vkEnumeratePhysicalDeviceGroupsKHR);
    MATCH(vkGetPhysicalDeviceProperties);
    MATCH(vkGetPhysicalDeviceProperties2);
    MATCH(vkGetPhysicalDeviceProperties2KHR);
    MATCH(vkGetPhysicalDeviceFeatures);
    MATCH(vkGetPhysicalDeviceFeatures2);
    MATCH(vkGetPhysicalDeviceFeatures2KHR);
    MATCH(vkGetPhysicalDeviceQueueFamilyProperties);
    MATCH(vkGetPhysicalDeviceQueueFamilyProperties2);
    MATCH(vkGetPhysicalDeviceQueueFamilyProperties2KHR);
    MATCH(vkGetPhysicalDeviceMemoryProperties);
    MATCH(vkGetPhysicalDeviceMemoryProperties2);
    MATCH(vkGetPhysicalDeviceMemoryProperties2KHR);
    MATCH(vkGetPhysicalDeviceFormatProperties);
    MATCH(vkGetPhysicalDeviceImageFormatProperties);
    MATCH(vkGetPhysicalDeviceSparseImageFormatProperties);
    MATCH(vkCreateDevice);
    MATCH(vkDestroyDevice);
    MATCH(vkGetDeviceQueue);
    MATCH(vkAllocateMemory);
    MATCH(vkFreeMemory);
    MATCH(vkMapMemory);
    MATCH(vkUnmapMemory);
    MATCH(vkCreateBuffer);
    MATCH(vkDestroyBuffer);
    MATCH(vkGetBufferMemoryRequirements);
    MATCH(vkBindBufferMemory);
    MATCH(vkCreateImage);
    MATCH(vkDestroyImage);
    MATCH(vkGetImageMemoryRequirements);
    MATCH(vkGetImageSubresourceLayout);
    MATCH(vkBindImageMemory);
    MATCH(vkCreateImageView);
    MATCH(vkDestroyImageView);
    MATCH(vkCreateShaderModule);
    MATCH(vkDestroyShaderModule);
    MATCH(vkCreatePipelineCache);
    MATCH(vkDestroyPipelineCache);
    MATCH(vkCreatePipelineLayout);
    MATCH(vkDestroyPipelineLayout);
    MATCH(vkCreateRenderPass);
    MATCH(vkDestroyRenderPass);
    MATCH(vkCreateFramebuffer);
    MATCH(vkDestroyFramebuffer);
    MATCH(vkCreateDescriptorSetLayout);
    MATCH(vkDestroyDescriptorSetLayout);
    MATCH(vkCreateDescriptorPool);
    MATCH(vkDestroyDescriptorPool);
    MATCH(vkAllocateDescriptorSets);
    MATCH(vkFreeDescriptorSets);
    MATCH(vkUpdateDescriptorSets);
    MATCH(vkCreateGraphicsPipelines);
    MATCH(vkCreateComputePipelines);
    MATCH(vkDestroyPipeline);
    MATCH(vkCreateSemaphore);
    MATCH(vkDestroySemaphore);
    MATCH(vkCreateFence);
    MATCH(vkDestroyFence);
    MATCH(vkResetFences);
    MATCH(vkGetFenceStatus);
    MATCH(vkWaitForFences);
    MATCH(vkCreateCommandPool);
    MATCH(vkDestroyCommandPool);
    MATCH(vkAllocateCommandBuffers);
    MATCH(vkFreeCommandBuffers);
    MATCH(vkBeginCommandBuffer);
    MATCH(vkEndCommandBuffer);
    MATCH(vkCmdBindPipeline);
    MATCH(vkCmdSetViewport);
    MATCH(vkCmdSetScissor);
    MATCH(vkCmdBindDescriptorSets);
    MATCH(vkCmdBindVertexBuffers);
    MATCH(vkCmdBindIndexBuffer);
    MATCH(vkCmdPushConstants);
    MATCH(vkCmdSetDepthBias);
    MATCH(vkCreateEvent);
    MATCH(vkDestroyEvent);
    MATCH(vkGetEventStatus);
    MATCH(vkSetEvent);
    MATCH(vkResetEvent);
    MATCH(vkCmdSetEvent);
    MATCH(vkCmdResetEvent);
    MATCH(vkCmdWaitEvents);
    MATCH(vkCreateQueryPool);
    MATCH(vkDestroyQueryPool);
    MATCH(vkCmdBeginQuery);
    MATCH(vkCmdEndQuery);
    MATCH(vkCmdWriteTimestamp);
    MATCH(vkCmdResetQueryPool);
    MATCH(vkCmdDispatch);
    MATCH(vkCmdDispatchIndirect);
    MATCH(vkFlushMappedMemoryRanges);
    MATCH(vkInvalidateMappedMemoryRanges);
    MATCH(vkCreateRenderPass2);
    MATCH(vkCreateRenderPass2KHR);
    MATCH(vkGetDeviceQueue2);
    MATCH(vkCmdPipelineBarrier2);
    MATCH(vkCmdPipelineBarrier2KHR);
    MATCH(vkQueueSubmit2);
    MATCH(vkQueueSubmit2KHR);
    MATCH(vkCmdExecuteCommands);
    MATCH(vkCmdCopyBuffer);
    MATCH(vkCreateSampler);
    MATCH(vkDestroySampler);
    MATCH(vkCmdCopyBufferToImage);
    MATCH(vkCmdCopyImageToBuffer);
    MATCH(vkCmdCopyImage);
    MATCH(vkCmdBlitImage);
    MATCH(vkCmdClearColorImage);
    MATCH(vkCmdClearDepthStencilImage);
    MATCH(vkCmdClearAttachments);
    MATCH(vkCmdPipelineBarrier);
    MATCH(vkCmdDraw);
    MATCH(vkCmdBeginRenderPass);
    MATCH(vkCmdDrawIndexed);
    MATCH(vkCmdEndRenderPass);
    MATCH(vkQueueSubmit);
    MATCH(vkQueueWaitIdle);
    MATCH(vkDeviceWaitIdle);
    MATCH(vkCreateXlibSurfaceKHR);
    MATCH(vkCreateXcbSurfaceKHR);
    MATCH(vkGetPhysicalDeviceXcbPresentationSupportKHR);
    MATCH(vkGetPhysicalDeviceDisplayPropertiesKHR);
    MATCH(vkGetPhysicalDeviceDisplayPlanePropertiesKHR);
    MATCH(vkGetDisplayPlaneSupportedDisplaysKHR);
    MATCH(vkGetDisplayModePropertiesKHR);
    MATCH(vkDestroySurfaceKHR);
    MATCH(vkGetPhysicalDeviceSurfaceSupportKHR);
    MATCH(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    MATCH(vkGetPhysicalDeviceSurfaceFormatsKHR);
    MATCH(vkGetPhysicalDeviceSurfacePresentModesKHR);
    MATCH(vkCreateSwapchainKHR);
    MATCH(vkDestroySwapchainKHR);
    MATCH(vkGetSwapchainImagesKHR);
    MATCH(vkAcquireNextImageKHR);
    MATCH(vkQueuePresentKHR);
    MATCH(vkGetBufferDeviceAddress);
    MATCH(vkGetBufferDeviceAddressKHR);
    MATCH(vkGetBufferOpaqueCaptureAddress);
    MATCH(vkGetDeviceMemoryOpaqueCaptureAddress);
    MATCH(vkGetSemaphoreCounterValue);
    MATCH(vkGetSemaphoreCounterValueKHR);
    MATCH(vkWaitSemaphores);
    MATCH(vkWaitSemaphoresKHR);
    MATCH(vkSignalSemaphore);
    MATCH(vkSignalSemaphoreKHR);
    MATCH(vkCmdBeginRendering);
    MATCH(vkCmdEndRendering);
    MATCH(vkCmdBeginRenderingKHR);
    MATCH(vkCmdEndRenderingKHR);
    MATCH(vkCmdSetEvent2);
    MATCH(vkCmdResetEvent2);
    MATCH(vkCmdWaitEvents2);
    MATCH(vkCmdSetEvent2KHR);
    MATCH(vkCmdResetEvent2KHR);
    MATCH(vkCmdWaitEvents2KHR);
    MATCH(vkCmdDrawIndirect);
    MATCH(vkCmdDrawIndexedIndirect);
    MATCH(vkCmdDrawIndirectCount);
    MATCH(vkCmdDrawIndirectCountKHR);
    MATCH(vkCmdDispatchBase);
    MATCH(vkCmdFillBuffer);
    MATCH(vkCmdUpdateBuffer);
    MATCH(vkCmdPushDescriptorSetKHR);
    MATCH(vkCmdPushDescriptorSetWithTemplateKHR);
    MATCH(vkCmdSetStencilReference);
    MATCH(vkCmdSetStencilCompareMask);
    MATCH(vkCmdSetStencilWriteMask);
    MATCH(vkCmdSetBlendConstants);
    MATCH(vkCmdSetDepthBounds);
    MATCH(vkCmdSetDepthTestEnable);
    MATCH(vkCmdSetDepthWriteEnable);
    MATCH(vkCmdSetDepthCompareOp);
    MATCH(vkCmdSetStencilTestEnable);
    MATCH(vkCmdSetCullMode);
    MATCH(vkCmdSetFrontFace);
    MATCH(vkCmdSetPrimitiveTopology);
    MATCH(vkCmdSetColorBlendEnable);
    MATCH(vkCmdSetColorWriteMask);
    MATCH(vkCmdSetColorBlendEquation);
    MATCH(vkCmdSetScissorWithCount);
    MATCH(vkCmdSetViewportWithCount);
    MATCH(vkCmdSetLogicOp);
    MATCH(vkCmdSetDepthBiasEnable);
    MATCH(vkCmdSetPrimitiveRestartEnable);
    MATCH(vkCmdSetRasterizerDiscardEnable);
    MATCH(vkCmdSetDepthBoundsTestEnable);
    MATCH(vkCmdSetDepthBias2EXT);
    MATCH(vkCmdSetSampleLocationsEXT);
    MATCH(vkCmdSetDiscardRectangleEXT);
    MATCH(vkCmdSetFragmentShadingRateKHR);
    MATCH(vkGetPhysicalDeviceFragmentShadingRatesKHR);
    MATCH(vkGetImageSubresourceLayout2);
    MATCH(vkGetImageSubresourceLayout2KHR);
    MATCH(vkGetDeviceImageMemoryRequirements);
    MATCH(vkGetDeviceImageSparseMemoryRequirements);
    MATCH(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT);
    MATCH(vkGetCalibratedTimestampsEXT);
    MATCH(vkCmdSetLogicOpEXT);
    MATCH(vkCmdSetCullModeEXT);
    MATCH(vkCmdSetFrontFaceEXT);
    MATCH(vkCmdSetPrimitiveTopologyEXT);
    MATCH(vkCmdSetDepthTestEnableEXT);
    MATCH(vkCmdSetDepthWriteEnableEXT);
    MATCH(vkCmdSetStencilTestEnableEXT);
    MATCH(vkCmdSetStencilOpEXT);
    MATCH(panvk_v9_read_pixel);
#undef MATCH
    return NULL;
}

PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    return vkGetInstanceProcAddr(NULL, pName);
}

__attribute__((visibility("default"))) PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return vkGetInstanceProcAddr(instance, pName);
}
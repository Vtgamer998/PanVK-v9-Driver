/*
 * Test harness for Step 4: Vulkan Loader ICD Dynamic Shared Library Integration
 * Dynamically loads libvulkan_panvk_v9.so via dlopen(), resolves entry points via vk_icdGetInstanceProcAddr, and executes full Vulkan pipeline rendering
 */

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

#include "panvk_v9_entrypoints.h"

typedef PFN_vkVoidFunction (*PFN_vk_icdGetInstanceProcAddr)(VkInstance instance, const char *pName);

static uint32_t *read_spirv(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) {
        fclose(file);
        return NULL;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || (file_size & 3) || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    uint32_t *code = malloc((size_t)file_size);
    if (!code || fread(code, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(code);
        code = NULL;
    } else {
        *size = (size_t)file_size;
    }
    fclose(file);
    return code;
}

int main(int argc, char **argv) {
    printf("=== Testing Step 4: Vulkan Loader ICD Shared Library Integration ===\n");

    const char *so_path = "./libvulkan_panvk_v9.so";
    void *handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "FAIL: dlopen('%s') failed: %s\n", so_path, dlerror());
        return 1;
    }
    printf("SUCCESS: Dynamically loaded '%s' via dlopen()\n", so_path);

    PFN_vk_icdGetInstanceProcAddr gpa = (PFN_vk_icdGetInstanceProcAddr)dlsym(handle, "vk_icdGetInstanceProcAddr");
    if (!gpa) {
        fprintf(stderr, "FAIL: dlsym('vk_icdGetInstanceProcAddr') failed: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }
    printf("SUCCESS: Resolved 'vk_icdGetInstanceProcAddr' entry point from ICD\n");

#define LOOKUP(type, name) type pfn_##name = (type)gpa(NULL, #name); \
    if (!pfn_##name) { fprintf(stderr, "FAIL: ICD missing proc address for '%s'\n", #name); dlclose(handle); return 1; }

    typedef VkResult (*PFN_vkCreateInstance)(const struct VkInstanceCreateInfo *, void *, VkInstance *);
    typedef void (*PFN_vkDestroyInstance)(VkInstance, void *);
    typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t *, VkPhysicalDevice *);
    typedef void (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, struct VkPhysicalDeviceProperties *);
    typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const struct VkDeviceCreateInfo *, void *, VkDevice *);
    typedef void (*PFN_vkDestroyDevice)(VkDevice, void *);
    typedef void (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue *);
    typedef VkResult (*PFN_vkCreateCommandPool)(VkDevice, const struct VkCommandPoolCreateInfo *, void *, VkCommandPool *);
    typedef void (*PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, void *);
    typedef VkResult (*PFN_vkAllocateCommandBuffers)(VkDevice, const struct VkCommandBufferAllocateInfo *, VkCommandBuffer *);
    typedef void (*PFN_vkFreeCommandBuffers)(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer *);
    typedef VkResult (*PFN_vkBeginCommandBuffer)(VkCommandBuffer, const struct VkCommandBufferBeginInfo *);
    typedef VkResult (*PFN_vkEndCommandBuffer)(VkCommandBuffer);
    typedef VkResult (*PFN_vkCreateShaderModule)(VkDevice, const struct VkShaderModuleCreateInfo *, void *, VkShaderModule *);
    typedef void (*PFN_vkDestroyShaderModule)(VkDevice, VkShaderModule, void *);
    typedef VkResult (*PFN_vkCreateGraphicsPipelines)(VkDevice, VkPipelineCache, uint32_t, const struct VkGraphicsPipelineCreateInfo *, void *, VkPipeline *);
    typedef void (*PFN_vkDestroyPipeline)(VkDevice, VkPipeline, void *);
    typedef void (*PFN_vkCmdBindPipeline)(VkCommandBuffer, uint32_t, VkPipeline);
    typedef void (*PFN_vkCmdBeginRenderPass)(VkCommandBuffer, const struct VkRenderPassBeginInfo *, uint32_t);
    typedef void (*PFN_vkCmdDrawIndexed)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    typedef void (*PFN_vkCmdEndRenderPass)(VkCommandBuffer);
    typedef VkResult (*PFN_vkQueueSubmit)(VkQueue, uint32_t, const struct VkSubmitInfo *, void *);
    typedef uint32_t (*PFN_panvk_v9_read_pixel)(VkCommandBuffer, uint32_t, uint32_t);

    LOOKUP(PFN_vkCreateInstance, vkCreateInstance);
    LOOKUP(PFN_vkDestroyInstance, vkDestroyInstance);
    LOOKUP(PFN_vkEnumeratePhysicalDevices, vkEnumeratePhysicalDevices);
    LOOKUP(PFN_vkGetPhysicalDeviceProperties, vkGetPhysicalDeviceProperties);
    LOOKUP(PFN_vkCreateDevice, vkCreateDevice);
    LOOKUP(PFN_vkDestroyDevice, vkDestroyDevice);
    LOOKUP(PFN_vkGetDeviceQueue, vkGetDeviceQueue);
    LOOKUP(PFN_vkCreateCommandPool, vkCreateCommandPool);
    LOOKUP(PFN_vkDestroyCommandPool, vkDestroyCommandPool);
    LOOKUP(PFN_vkAllocateCommandBuffers, vkAllocateCommandBuffers);
    LOOKUP(PFN_vkFreeCommandBuffers, vkFreeCommandBuffers);
    LOOKUP(PFN_vkBeginCommandBuffer, vkBeginCommandBuffer);
    LOOKUP(PFN_vkEndCommandBuffer, vkEndCommandBuffer);
    LOOKUP(PFN_vkCreateShaderModule, vkCreateShaderModule);
    LOOKUP(PFN_vkDestroyShaderModule, vkDestroyShaderModule);
    LOOKUP(PFN_vkCreateGraphicsPipelines, vkCreateGraphicsPipelines);
    LOOKUP(PFN_vkDestroyPipeline, vkDestroyPipeline);
    LOOKUP(PFN_vkCmdBindPipeline, vkCmdBindPipeline);
    LOOKUP(PFN_vkCmdBeginRenderPass, vkCmdBeginRenderPass);
    LOOKUP(PFN_vkCmdDrawIndexed, vkCmdDrawIndexed);
    LOOKUP(PFN_vkCmdEndRenderPass, vkCmdEndRenderPass);
    LOOKUP(PFN_vkQueueSubmit, vkQueueSubmit);
    LOOKUP(PFN_panvk_v9_read_pixel, panvk_v9_read_pixel);
#undef LOOKUP

    printf("SUCCESS: Resolved Vulkan shader, pipeline, command, and device entry points\n");

    /* Create Instance */
    VkInstance instance = NULL;
    struct VkInstanceCreateInfo instInfo = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    pfn_vkCreateInstance(&instInfo, NULL, &instance);

    /* Enumerate Physical Devices */
    uint32_t count = 0;
    pfn_vkEnumeratePhysicalDevices(instance, &count, NULL);
    VkPhysicalDevice physDev = NULL;
    pfn_vkEnumeratePhysicalDevices(instance, &count, &physDev);

    struct VkPhysicalDeviceProperties props;
    pfn_vkGetPhysicalDeviceProperties(physDev, &props);
    printf("SUCCESS: Dynamically queried device: '%s'\n", props.deviceName);

    /* Create Device & Queue */
    VkDevice device = NULL;
    struct VkDeviceCreateInfo devInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    pfn_vkCreateDevice(physDev, &devInfo, NULL, &device);
    VkQueue queue = NULL;
    VkQueue same_queue = NULL;
    pfn_vkGetDeviceQueue(device, 0, 0, &queue);
    pfn_vkGetDeviceQueue(device, 0, 0, &same_queue);
    if (!queue || queue != same_queue) {
        fprintf(stderr, "FAIL: repeated vkGetDeviceQueue returned different handles\n");
        return 1;
    }

    /* Exercise SPIR-V entry-point discovery and graphics pipeline state parsing.
     * With no arguments these structural modules exercise the fixed fallback;
     * pass vertex and fragment SPIR-V paths to exercise native compilation. */
    static const uint32_t vertex_spirv[] = {
        0x07230203, 0x00010000, 0, 2, 0,
        0x0005000f, 0, 1, 0x6e69616d, 0,
    };
    static const uint32_t fragment_spirv[] = {
        0x07230203, 0x00010000, 0, 2, 0,
        0x0005000f, 4, 1, 0x6e69616d, 0,
    };
    size_t vertex_code_size = sizeof(vertex_spirv);
    size_t fragment_code_size = sizeof(fragment_spirv);
    uint32_t *vertex_code = NULL;
    uint32_t *fragment_code = NULL;
    if (argc == 3) {
        vertex_code = read_spirv(argv[1], &vertex_code_size);
        fragment_code = read_spirv(argv[2], &fragment_code_size);
        if (!vertex_code || !fragment_code) {
            fprintf(stderr, "FAIL: could not read SPIR-V test files\n");
            free(vertex_code);
            free(fragment_code);
            return 1;
        }
    }
    struct VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vertex_code_size,
        .pCode = vertex_code ? vertex_code : vertex_spirv,
    };
    VkShaderModule vertex_shader = NULL;
    VkShaderModule fragment_shader = NULL;
    if (pfn_vkCreateShaderModule(device, &shader_info, NULL, &vertex_shader) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vertex SPIR-V module rejected\n");
        return 1;
    }
    shader_info.codeSize = fragment_code_size;
    shader_info.pCode = fragment_code ? fragment_code : fragment_spirv;
    if (pfn_vkCreateShaderModule(device, &shader_info, NULL, &fragment_shader) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: fragment SPIR-V module rejected\n");
        return 1;
    }
    free(vertex_code);
    free(fragment_code);

    struct VkPipelineShaderStageCreateInfo stages[] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_shader, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_shader, .pName = "main" },
    };
    struct VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    struct VkViewport viewport = { 0, 0, 16, 16, 0, 1 };
    struct VkRect2D scissor = { .extent = { 16, 16 } };
    struct VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor,
    };
    struct VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1,
    };
    struct VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    struct VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    struct VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment,
    };
    struct VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
    };
    VkPipeline pipeline = NULL;
    if (pfn_vkCreateGraphicsPipelines(device, NULL, 1, &pipeline_info, NULL, &pipeline) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: graphics pipeline state creation failed\n");
        return 1;
    }
    pfn_vkDestroyShaderModule(device, vertex_shader, NULL);
    pfn_vkDestroyShaderModule(device, fragment_shader, NULL);
    printf("SUCCESS: Parsed SPIR-V stages and graphics pipeline state\n");

    /* Allocate Command Buffer */
    VkCommandPool pool = NULL;
    struct VkCommandPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pfn_vkCreateCommandPool(device, &poolInfo, NULL, &pool);

    VkCommandBuffer cmd = NULL;
    struct VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .commandBufferCount = 1,
    };
    pfn_vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    /* Record & Submit Command Buffer */
    struct VkCommandBufferBeginInfo beginInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    pfn_vkBeginCommandBuffer(cmd, &beginInfo);
    struct VkRenderPassBeginInfo rpInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderArea.extent = { .width = 16, .height = 16 },
    };
    pfn_vkCmdBeginRenderPass(cmd, &rpInfo, 0);
    pfn_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    pfn_vkCmdDrawIndexed(cmd, 3, 1, 0, 0, 0);
    pfn_vkCmdEndRenderPass(cmd);
    pfn_vkEndCommandBuffer(cmd);

    struct VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    pfn_vkQueueSubmit(queue, 1, &submitInfo, NULL);
    printf("SUCCESS: Dispatched vkQueueSubmit via dynamic ICD function pointers\n");

    /* Verify Output */
    uint32_t p0 = pfn_panvk_v9_read_pixel(cmd, 0, 0);
    printf("Rendered Output: pixel(0,0)=0x%08x\n", p0);
    if (p0 == 0xFF00FF00) {
        printf("SUCCESS: Dynamically loaded PanVK ICD rendered solid green (0xFF00FF00)!\n");
    } else {
        fprintf(stderr, "FAIL: Expected 0xFF00FF00, got 0x%08x\n", p0);
        dlclose(handle);
        return 1;
    }

    pfn_vkFreeCommandBuffers(device, pool, 1, &cmd);
    pfn_vkDestroyPipeline(device, pipeline, NULL);
    pfn_vkDestroyCommandPool(device, pool, NULL);
    pfn_vkDestroyDevice(device, NULL);
    pfn_vkDestroyInstance(instance, NULL);
    dlclose(handle);

    printf("=== Step 4: Vulkan Loader ICD Shared Library Integration PASSED CLEANLY! ===\n");
    return 0;
}

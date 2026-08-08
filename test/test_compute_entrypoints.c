/*
 * Test harness: full entrypoints compute path.
 * vkCreateInstance -> device -> compute pipeline (CS compiled) -> descriptor
 * set (SSBO) -> vkCmdBindDescriptorSets -> vkCmdDispatch -> vkQueueSubmit.
 * PANVK_DRY_RUN=1 exercises the whole path without /dev/mali0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panvk_v9_entrypoints.h"
#include "kbase_winsys.h"

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = n;
    return buf;
}

int main(int argc, char **argv) {
    const char *spv_path = argc > 1 ? argv[1] : "/data/data/com.termux/files/usr/tmp/opencode/cs.spv";
    printf("=== Testing entrypoints compute dispatch path ===\n");

    VkInstance instance = NULL;
    struct VkInstanceCreateInfo instInfo = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    if (vkCreateInstance(&instInfo, NULL, &instance) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkCreateInstance\n");
        return 1;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    VkPhysicalDevice physDev = NULL;
    vkEnumeratePhysicalDevices(instance, &count, &physDev);
    struct VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDev, &props);
    printf("Physical Device: '%s' (0x%04x/0x%08x)\n", props.deviceName, props.vendorID, props.deviceID);

    VkDevice device = NULL;
    struct VkDeviceCreateInfo devInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    if (vkCreateDevice(physDev, &devInfo, NULL, &device) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkCreateDevice\n");
        return 1;
    }
    VkQueue queue = NULL;
    vkGetDeviceQueue(device, 0, 0, &queue);

    /* SSBO storage buffer */
    VkBuffer ssbo = NULL;
    struct VkBufferCreateInfo bufInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 64,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    vkCreateBuffer(device, &bufInfo, NULL, &ssbo);
    struct VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, ssbo, &memReq);
    struct VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReq.size,
        .memoryTypeIndex = 0,
    };
    VkDeviceMemory ssboMem = NULL;
    vkAllocateMemory(device, &allocInfo, NULL, &ssboMem);
    vkBindBufferMemory(device, ssbo, ssboMem, 0);

    /* Descriptor set layout: binding 0 = storage buffer */
    struct VkDescriptorSetLayoutBinding dslb = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    struct VkDescriptorSetLayoutCreateInfo dslinfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &dslb,
    };
    VkDescriptorSetLayout dsl = NULL;
    vkCreateDescriptorSetLayout(device, &dslinfo, NULL, &dsl);

    VkDescriptorPool pool = NULL;
    struct VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &(struct VkDescriptorPoolSize) {
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 },
    };
    vkCreateDescriptorPool(device, &poolInfo, NULL, &pool);
    VkDescriptorSet descSet = NULL;
    struct VkDescriptorSetAllocateInfo setAlloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dsl,
    };
    vkAllocateDescriptorSets(device, &setAlloc, &descSet);

    struct VkDescriptorBufferInfo dbInfo = { .buffer = ssbo, .offset = 0, .range = 64 };
    struct VkWriteDescriptorSet writeDesc = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descSet,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &dbInfo,
    };
    vkUpdateDescriptorSets(device, 1, &writeDesc, 0, NULL);

    /* Pipeline layout with binding 0 = set 0 storage buffer */
    struct VkPipelineLayoutCreateInfo plInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsl,
    };
    VkPipelineLayout pl = NULL;
    vkCreatePipelineLayout(device, &plInfo, NULL, &pl);

    /* Shader module from SPIR-V */
    size_t spv_size = 0;
    uint8_t *spv = read_file(spv_path, &spv_size);
    if (!spv) { fprintf(stderr, "FAIL: cannot read %s\n", spv_path); return 1; }
    struct VkShaderModuleCreateInfo smInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spv_size,
        .pCode = (const uint32_t *)spv,
    };
    VkShaderModule module = NULL;
    vkCreateShaderModule(device, &smInfo, NULL, &module);

    /* Compute pipeline */
    struct VkPipelineShaderStageCreateInfo stageInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = module,
        .pName = "main",
    };
    struct VkComputePipelineCreateInfo cpInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stageInfo,
        .layout = pl,
    };
    VkPipeline cpipe = NULL;
    if (vkCreateComputePipelines(device, NULL, 1, &cpInfo, NULL, &cpipe) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkCreateComputePipelines\n");
        return 1;
    }
    if (!panvk_v9_compute_binary_size(cpipe)) {
        fprintf(stderr, "FAIL: compute shader not compiled (binary_size=0)\n");
        return 1;
    }
    printf("Compute pipeline compiled: %zu bytes, local=%ux%ux%u\n",
           panvk_v9_compute_binary_size(cpipe),
           panvk_v9_compute_local_size(cpipe, 0),
           panvk_v9_compute_local_size(cpipe, 1),
           panvk_v9_compute_local_size(cpipe, 2));

    /* Command buffer: dispatch 4x1x1 with SSBO bound */
    VkCommandPool pool2 = NULL;
    struct VkCommandPoolCreateInfo cpoolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    vkCreateCommandPool(device, &cpoolInfo, NULL, &pool2);
    VkCommandBuffer cmd = NULL;
    struct VkCommandBufferAllocateInfo cbAlloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool2,
        .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(device, &cbAlloc, &cmd);

    struct VkCommandBufferBeginInfo beginInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &beginInfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &descSet, 0, NULL);
    vkCmdDispatch(cmd, 4, 1, 1);
    vkEndCommandBuffer(cmd);

    struct VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    if (vkQueueSubmit(queue, 1, &submitInfo, NULL) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkQueueSubmit\n");
        return 1;
    }
    if (!panvk_v9_cmd_has_compute(cmd)) {
        fprintf(stderr, "FAIL: command buffer has no compute job after dispatch\n");
        return 1;
    }
    printf("Compute dispatch submitted OK (cmd buffer has compute job)\n");

    if (!kbase_dry_run()) {
        void *data = NULL;
        vkMapMemory(device, ssboMem, 0, 64, 0, &data);
        uint32_t *d = (uint32_t *)data;
        printf("SSBO after compute: d[0]=0x%08x d[1]=0x%08x\n", d[0], d[1]);
        vkUnmapMemory(device, ssboMem);
    }

    vkFreeCommandBuffers(device, pool2, 1, &cmd);
    vkDestroyCommandPool(device, pool2, NULL);
    vkDestroyPipeline(device, cpipe, NULL);
    vkDestroyPipelineLayout(device, pl, NULL);
    vkDestroyShaderModule(device, module, NULL);
    vkFreeDescriptorSets(device, pool, 1, &descSet);
    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyDescriptorSetLayout(device, dsl, NULL);
    vkDestroyBuffer(device, ssbo, NULL);
    vkFreeMemory(device, ssboMem, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    free(spv);

    printf("=== Entrypoints compute dispatch test PASSED CLEANLY! ===\n");
    return 0;
}

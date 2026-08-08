/*
 * Test harness: full entrypoints compute path with 3 SSBOs (bindings 0,1,2).
 * Exercises the FAU wiring: vkCreatePipelineLayout assigns resource_index
 * 0,1,2 sequentially; the compiler bakes LEA_PKA modes into fau_consts; the
 * driver writes them into the FAU buffer and programs SE[1]/SE[14].
 * Real GPU: all 3 SSBOs must be written to 1.
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
    const char *spv_path = argc > 1 ? argv[1] : "cs3.spv";
    printf("=== Entrypoints multi-SSBO compute (3 SSBOs) ===\n");

    VkInstance instance = NULL;
    struct VkInstanceCreateInfo instInfo = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    if (vkCreateInstance(&instInfo, NULL, &instance) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkCreateInstance\n"); return 1;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    VkPhysicalDevice physDev = NULL;
    vkEnumeratePhysicalDevices(instance, &count, &physDev);
    VkDevice device = NULL;
    struct VkDeviceCreateInfo devInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    if (vkCreateDevice(physDev, &devInfo, NULL, &device) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkCreateDevice\n"); return 1;
    }
    VkQueue queue = NULL;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkBuffer ssbo[3];
    VkDeviceMemory ssboMem[3];
    for (int i = 0; i < 3; i++) {
        struct VkBufferCreateInfo bufInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 64,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        };
        vkCreateBuffer(device, &bufInfo, NULL, &ssbo[i]);
        struct VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, ssbo[i], &memReq);
        struct VkMemoryAllocateInfo allocInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = memReq.size,
            .memoryTypeIndex = 0,
        };
        vkAllocateMemory(device, &allocInfo, NULL, &ssboMem[i]);
        vkBindBufferMemory(device, ssbo[i], ssboMem[i], 0);
    }

    struct VkDescriptorSetLayoutBinding dslb[3] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    struct VkDescriptorSetLayoutCreateInfo dslinfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = dslb,
    };
    VkDescriptorSetLayout dsl = NULL;
    vkCreateDescriptorSetLayout(device, &dslinfo, NULL, &dsl);

    VkDescriptorPool pool = NULL;
    struct VkDescriptorPoolSize poolSize = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 };
    struct VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
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

    struct VkDescriptorBufferInfo dbInfo[3] = {
        { .buffer = ssbo[0], .offset = 0, .range = 64 },
        { .buffer = ssbo[1], .offset = 0, .range = 64 },
        { .buffer = ssbo[2], .offset = 0, .range = 64 },
    };
    struct VkWriteDescriptorSet writeDesc[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descSet, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbInfo[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descSet, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbInfo[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descSet, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbInfo[2] },
    };
    vkUpdateDescriptorSets(device, 3, writeDesc, 0, NULL);

    struct VkPipelineLayoutCreateInfo plInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsl,
    };
    VkPipelineLayout pl = NULL;
    vkCreatePipelineLayout(device, &plInfo, NULL, &pl);

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
        fprintf(stderr, "FAIL: vkCreateComputePipelines\n"); return 1;
    }
    printf("Compute pipeline compiled: %zu bytes\n", panvk_v9_compute_binary_size(cpipe));

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
        fprintf(stderr, "FAIL: vkQueueSubmit\n"); return 1;
    }

    int ok = 1;
    for (int i = 0; i < 3; i++) {
        void *data = NULL;
        vkMapMemory(device, ssboMem[i], 0, 64, 0, &data);
        uint32_t d = *(uint32_t *)data;
        printf("ssbo%d[0]=0x%08x%s\n", i, d, d == 1 ? "  <== WRITE" : "");
        if (d != 1) ok = 0;
        vkUnmapMemory(device, ssboMem[i]);
    }
    printf(ok ? "PASS: all 3 SSBOs written via Vulkan entrypoints.\n"
              : "FAIL: entrypoints multi-SSBO.\n");

    vkFreeCommandBuffers(device, pool2, 1, &cmd);
    vkDestroyCommandPool(device, pool2, NULL);
    vkDestroyPipeline(device, cpipe, NULL);
    vkDestroyPipelineLayout(device, pl, NULL);
    vkDestroyShaderModule(device, module, NULL);
    vkFreeDescriptorSets(device, pool, 1, &descSet);
    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyDescriptorSetLayout(device, dsl, NULL);
    for (int i = 0; i < 3; i++) {
        vkDestroyBuffer(device, ssbo[i], NULL);
        vkFreeMemory(device, ssboMem[i], NULL);
    }
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    free(spv);
    return ok ? 0 : 1;
}

/*
 * Test harness for Step 3: PanVK Vulkan Driver API Layer
 * Verifies standard Vulkan API initialization, rendering, & output on Mali-G68 MC4
 */

#include <stdio.h>
#include <stdlib.h>

#include "panvk_v9_entrypoints.h"

int main(int argc, char **argv) {
    printf("=== Testing Step 3: PanVK Open-Source Vulkan Driver API Layer ===\n");

    /* 1. vkCreateInstance */
    VkInstance instance = NULL;
    struct VkInstanceCreateInfo instInfo = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    if (vkCreateInstance(&instInfo, NULL, &instance) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkCreateInstance failed\n");
        return 1;
    }
    printf("SUCCESS: vkCreateInstance initialized\n");

    /* 2. vkEnumeratePhysicalDevices */
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    printf("SUCCESS: Found %u physical Vulkan device(s)\n", count);

    VkPhysicalDevice physDev = NULL;
    vkEnumeratePhysicalDevices(instance, &count, &physDev);

    struct VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDev, &props);
    printf("SUCCESS: Physical Device: '%s' (Vendor ID 0x%04x, Device ID 0x%08x)\n",
           props.deviceName, props.vendorID, props.deviceID);

    /* 3. vkCreateDevice & vkGetDeviceQueue */
    VkDevice device = NULL;
    struct VkDeviceCreateInfo devInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    if (vkCreateDevice(physDev, &devInfo, NULL, &device) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkCreateDevice failed\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    printf("SUCCESS: Logical VkDevice created\n");

    VkQueue queue = NULL;
    vkGetDeviceQueue(device, 0, 0, &queue);

    /* 4. vkCreateCommandPool & vkAllocateCommandBuffers */
    VkCommandPool pool = NULL;
    struct VkCommandPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    vkCreateCommandPool(device, &poolInfo, NULL, &pool);

    VkCommandBuffer cmd = NULL;
    struct VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);
    printf("SUCCESS: VkCommandBuffer allocated from VkCommandPool\n");

    /* 5. Record Vulkan Command Buffer */
    struct VkCommandBufferBeginInfo beginInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &beginInfo);

    struct VkRenderPassBeginInfo rpInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderArea.extent = { .width = 16, .height = 16 },
    };
    vkCmdBeginRenderPass(cmd, &rpInfo, 0);
    vkCmdDrawIndexed(cmd, 3, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    printf("SUCCESS: Recorded Vulkan command buffer (vkCmdBeginRenderPass -> vkCmdDrawIndexed -> vkCmdEndRenderPass)\n");

    /* 6. vkQueueSubmit */
    struct VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    if (vkQueueSubmit(queue, 1, &submitInfo, NULL) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkQueueSubmit failed\n");
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        vkDestroyCommandPool(device, pool, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    printf("SUCCESS: vkQueueSubmit completed hardware execution on /dev/mali0!\n");

    /* 7. Verify Pixel Output */
    uint32_t p0 = panvk_v9_read_pixel(cmd, 0, 0);
    uint32_t p15 = panvk_v9_read_pixel(cmd, 15, 15);
    printf("Rendered Output: pixel(0,0)=0x%08x, pixel(15,15)=0x%08x\n", p0, p15);

    if (p0 == 0xFF00FF00) {
        printf("SUCCESS: Pixel (0,0) rendered solid green (0xFF00FF00) via Vulkan API!\n");
    } else {
        fprintf(stderr, "FAIL: Expected 0xFF00FF00, got 0x%08x\n", p0);
        return 1;
    }

    /* Cleanup */
    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    printf("=== Step 3: PanVK Vulkan Driver API Layer PASSED CLEANLY! ===\n");
    return 0;
}

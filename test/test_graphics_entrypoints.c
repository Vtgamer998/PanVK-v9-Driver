/*
 * Full graphics path through the Vulkan entrypoints, headless:
 * vkCreateInstance -> device -> cmd buffer -> render pass (clear) ->
 * vkCmdDrawIndexed (default green triangle) -> vkEndCommandBuffer ->
 * vkQueueSubmit -> read pixels from the rendered RT.
 * This is the "give image" path that vkQueuePresentKHR feeds from.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panvk_v9_entrypoints.h"
#include "kbase_winsys.h"

int main(void) {
    printf("=== Entrypoints graphics render (green triangle) ===\n");

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

    VkCommandPool pool = NULL;
    struct VkCommandPoolCreateInfo cpoolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    vkCreateCommandPool(device, &cpoolInfo, NULL, &pool);
    VkCommandBuffer cmd = NULL;
    struct VkCommandBufferAllocateInfo cbAlloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(device, &cbAlloc, &cmd);

    struct VkCommandBufferBeginInfo beginInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkClearValue cv;
    memset(&cv, 0, sizeof(cv));
    cv.color.float32[0] = 1.0f; cv.color.float32[1] = 0.0f;
    cv.color.float32[2] = 0.0f; cv.color.float32[3] = 1.0f;
    struct VkRenderPassBeginInfo rpInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderArea = { .offset = { 0, 0 }, .extent = { 64, 64 } },
        .clearValueCount = 1,
        .pClearValues = &cv,
    };
    vkCmdBeginRenderPass(cmd, &rpInfo, 0);
    vkCmdDrawIndexed(cmd, 3, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    struct VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    VkResult r = vkQueueSubmit(queue, 1, &submitInfo, NULL);
    printf("vkQueueSubmit rc=%d\n", r);
    if (r != VK_SUCCESS) { fprintf(stderr, "FAIL: submit\n"); return 1; }

    /* Read the rendered pixels. */
    uint32_t green = 0, red = 0, other = 0;
    uint32_t first = panvk_v9_read_pixel(cmd, 0, 0);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint32_t p = panvk_v9_read_pixel(cmd, x, y);
            if (p == 0xFF00FF00) green++;
            else if (p == 0xFF0000FF) red++;
            else other++;
        }
    printf("first=0x%08x green=%u red=%u other=%u (of %u)\n",
           first, green, red, other, 64u * 64u);
    int ok = green > 0;
    printf(ok ? "PASS: green triangle rendered via entrypoints.\n"
              : "FAIL: no rendered pixels.\n");

    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return ok ? 0 : 1;
}

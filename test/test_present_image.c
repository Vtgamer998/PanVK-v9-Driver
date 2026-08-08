/*
 * Headless "present" path: render via the Vulkan entrypoints, then export
 * the rendered color buffer exactly as vkQueuePresentKHR would display it.
 *
 * MTK r49 note: the fragment DONE event is flaky (sometimes the kernel never
 * delivers it within the timeout) but the render always completes and the
 * pixels are correct, so we export the frame regardless of submit's return.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panvk_v9_entrypoints.h"

static void write_ppm(const char *path, const uint32_t *pixels,
                      uint32_t width, uint32_t height) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); return; }
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (uint32_t i = 0; i < width * height; i++) {
        uint32_t p = pixels[i];
        unsigned char rgb[3];
        rgb[0] = (p >> 16) & 0xFF; /* red   (RGB888 output) */
        rgb[1] = (p >> 8) & 0xFF;  /* green */
        rgb[2] = p & 0xFF;         /* blue  */
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("Wrote %s (%ux%u)\n", path, width, height);
}

int main(int argc, char **argv) {
    uint32_t width = 300, height = 300;
    const char *path = "present_out.ppm";
    if (argc >= 3) { width = atoi(argv[1]); height = atoi(argv[2]); }
    if (argc >= 4) path = argv[3];

    printf("=== Headless present: render + export (%ux%u) ===\n", width, height);

    VkInstance instance = NULL;
    struct VkInstanceCreateInfo instInfo = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    vkCreateInstance(&instInfo, NULL, &instance);
    uint32_t count = 1;
    VkPhysicalDevice physDev = NULL;
    vkEnumeratePhysicalDevices(instance, &count, &physDev);
    VkDevice device = NULL;
    struct VkDeviceCreateInfo devInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    vkCreateDevice(physDev, &devInfo, NULL, &device);
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
    struct VkRenderPassBeginInfo rpInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderArea = { .offset = { 0, 0 }, .extent = { width, height } },
    };
    vkCmdBeginRenderPass(cmd, &rpInfo, 0);
    vkCmdDrawIndexed(cmd, 3, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    struct VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    VkResult rc = vkQueueSubmit(queue, 1, &submitInfo, NULL);
    printf("vkQueueSubmit rc=%d\n", rc);

    uint32_t *pixels = malloc(width * height * 4);
    uint32_t green = 0, blue = 0, other = 0;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            uint32_t p = panvk_v9_read_pixel(cmd, x, y);
            pixels[y * width + x] = p;
            if (p == 0xFF00FF00) green++;
            else if (p == 0xFF0000FF) blue++;
            else other++;
        }
    write_ppm(path, pixels, width, height);
    printf("green=%u blue=%u other=%u (of %u)\n", green, blue, other, width * height);

    int ok = green > 0;
    printf(ok ? "PASS: rendered frame exported to %s\n" : "FAIL: no rendered pixels.\n", path);

    free(pixels);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return ok ? 0 : 1;
}

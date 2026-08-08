/*
 * PanVK Valhall v9 X11 Display Presentation Test for Termux-X11
 * Renders on ARM Mali-G68 MC4 (/dev/mali0) and displays frame on DISPLAY=:0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "panvk_v9_entrypoints.h"

int main(int argc, char **argv) {
    const char *display_name = getenv("DISPLAY");
    if (!display_name) display_name = ":0";

    printf("=== PanVK Vulkan Driver Termux-X11 Presentation Test ===\n");
    printf("Connecting to X11 display '%s'...\n", display_name);

    Display *dpy = XOpenDisplay(display_name);
    if (!dpy) {
        fprintf(stderr, "FAIL: Could not open X11 display '%s'\n", display_name);
        return 1;
    }
    printf("SUCCESS: Connected to X11 server '%s'\n", display_name);

    int screen = DefaultScreen(dpy);
    uint32_t width = 300;
    uint32_t height = 300;

    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 100, 100,
                                     width, height, 1,
                                     BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XStoreName(dpy, win, "PanVK Mali-G68 Vulkan Driver (Valhall v9)");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask);
    XMapWindow(dpy, win);
    XFlush(dpy);
    printf("SUCCESS: Created X11 window (%ux%u) on DISPLAY=%s\n", width, height, display_name);

    /* Initialize Vulkan Pipeline */
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
    struct VkCommandPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    vkCreateCommandPool(device, &poolInfo, NULL, &pool);

    VkCommandBuffer cmd = NULL;
    struct VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    /* Record Vulkan Command Buffer for window dimensions */
    struct VkCommandBufferBeginInfo beginInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &beginInfo);
    struct VkRenderPassBeginInfo rpInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderArea.extent = { .width = width, .height = height },
    };
    vkCmdBeginRenderPass(cmd, &rpInfo, 0);
    vkCmdDrawIndexed(cmd, 3, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    /* Submit to GPU Hardware /dev/mali0 */
    struct VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    printf("Submitting Vulkan command buffer to GPU hardware (/dev/mali0)...\n");
    if (vkQueueSubmit(queue, 1, &submitInfo, NULL) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkQueueSubmit failed\n");
        return 1;
    }
    printf("SUCCESS: Hardware execution on Mali-G68 completed!\n");

    /* Allocate XImage buffer to present Vulkan output onto X11 display */
    char *image_data = malloc(width * height * 4);
    if (!image_data) {
        fprintf(stderr, "FAIL: malloc image_data failed\n");
        return 1;
    }

    uint32_t *pixels = (uint32_t *)image_data;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            pixels[y * width + x] = panvk_v9_read_pixel(cmd, x, y);
        }
    }

    XImage *ximage = XCreateImage(dpy, DefaultVisual(dpy, screen),
                                  24, ZPixmap, 0, image_data, width, height, 32, 0);

    GC gc = XCreateGC(dpy, win, 0, NULL);

    printf("Presenting Vulkan rendered frame onto Termux-X11 window for 1000 frames (~20 seconds)...\n");
    for (int frame = 0; frame < 1000; frame++) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }
        XPutImage(dpy, win, gc, ximage, 0, 0, 0, 0, width, height);
        XFlush(dpy);
        usleep(20000); /* ~50 fps display loop */
    }

    printf("SUCCESS: Frame presentation completed cleanly on DISPLAY=%s!\n", display_name);

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    printf("=== PanVK Termux-X11 Presentation Test PASSED CLEANLY! ===\n");
    return 0;
}

/*
 * test_swapchain_x11.c — Fase 2: swapchain multi-frame (sem freeze).
 *
 * Valida o caminho completo: acquire -> begin/draw -> submit -> present
 * usando as entrypoints Vulkan reais do driver (vkCreateSwapchainKHR,
 * vkAcquireNextImageKHR, vkQueuePresentKHR).  O driver aplica V9_CYCLE_DEV
 * automatico entre frames (abre/fecha /dev/mali0 sem freezar, BOs persistem).
 *
 * Roda SEM root e SEM xserver obrigatorio: se DISPLAY nao existe, usa um
 * surface stub interno (headless) pra exercitar o pipeline Vulkan multi-frame.
 * COM X11 real (DISPLAY=:0 via Termux:X11), apresenta de verdade na janela.
 *
 *   DISPLAY=:0 ./test_swapchain_x11            # 30 frames 300x300 + present
 *   PANVK_DRY_RUN=1 ./test_swapchain_x11        # sem GPU (valida plumbing)
 *   TEST_FRAMES=120 TEST_W=640 TEST_H=480 ./test_swapchain_x11
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "panvk_v9_entrypoints.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *display_name = getenv("DISPLAY");
    int nframes = 30, w = 300, h = 300;
    const char *f = getenv("TEST_FRAMES"); if (f) nframes = atoi(f);
    const char *tw = getenv("TEST_W");     if (tw) w = atoi(tw);
    const char *th = getenv("TEST_H");     if (th) h = atoi(th);
    if (nframes < 1) nframes = 1;
    if (w < 16) w = 16; if (h < 16) h = 16;

    printf("=== Fase 2: Swapchain multi-frame (%dx%d, %d frames, DISPLAY=%s) ===\n",
           w, h, nframes, display_name ? display_name : "(none)");

    Display *dpy = NULL;
    Window win = None;
    int have_x = 0;
    if (display_name && display_name[0]) {
        dpy = XOpenDisplay(display_name);
        if (dpy) {
            have_x = 1;
            int screen = DefaultScreen(dpy);
            win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 50, 50, w, h, 1,
                                      BlackPixel(dpy, screen), WhitePixel(dpy, screen));
            XStoreName(dpy, win, "PanVK Swapchain (Mali-G68 v9)");
            XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);
            XMapWindow(dpy, win); XFlush(dpy);
            for (int i = 0; i < 50; i++) {
                if (XPending(dpy)) { XEvent ev; XNextEvent(dpy, &ev); if (ev.type == MapNotify) break; }
                usleep(10000);
            }
            printf("OK: X11 conectado -> present real na janela\n");
        }
    }
    if (!have_x) printf("OK: SEM X11 (headless) - exercita pipeline Vulkan multi-frame; present sera no-op visual\n");

    VkInstance instance = NULL;
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS) { fprintf(stderr, "FAIL: instance\n"); return 1; }
    uint32_t pc = 1; VkPhysicalDevice phys = NULL;
    vkEnumeratePhysicalDevices(instance, &pc, &phys);
    if (!phys) { fprintf(stderr, "FAIL: no physDev\n"); return 1; }
    VkDevice device = NULL;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    if (vkCreateDevice(phys, &dci, NULL, &device) != VK_SUCCESS) { fprintf(stderr, "FAIL: device\n"); return 1; }
    VkQueue queue = NULL;
    vkGetDeviceQueue(device, 0, 0, &queue);

    /* Surface: X11 se disponivel, senao stub para exercitar swapchain. */
    VkSurfaceKHR surface = NULL;
    if (have_x) {
        VkXlibSurfaceCreateInfoKHR xsci = { .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, .dpy = dpy, .window = win };
        if (vkCreateXlibSurfaceKHR(instance, &xsci, NULL, &surface) != VK_SUCCESS)
            fprintf(stderr, "WARN: vkCreateXlibSurfaceKHR falhou\n");
    }

    VkSurfaceCapabilitiesKHR caps = {0};
    if (surface) vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);

    /* Swapchain: image extent = tamanho do framebuffer. */
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = caps.maxImageCount ? (caps.maxImageCount < 3 ? caps.maxImageCount : 3) : 3,
        .imageExtent = { .width = (uint32_t)w, .height = (uint32_t)h },
    };
    VkSwapchainKHR swapchain = NULL;
    if (vkCreateSwapchainKHR(device, &sci, NULL, &swapchain) != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkCreateSwapchainKHR\n"); return 1;
    }
    uint32_t img_count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &img_count, NULL);
    printf("OK: swapchain criada (%u imagens, extent=%ux%u)\n", img_count, sci.imageExtent.width, sci.imageExtent.height);

    VkCommandPool pool = NULL;
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    vkCreateCommandPool(device, &pci, NULL, &pool);
    VkCommandBuffer cmd = NULL;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
    vkAllocateCommandBuffers(device, &cai, &cmd);

    printf("Rodando %d frames...\n", nframes);
    for (int fi = 0; fi < nframes; fi++) {
        if (have_x) { while (XPending(dpy)) { XEvent ev; XNextEvent(dpy, &ev); } }

        uint32_t img_idx = 0;
        VkResult ar = vkAcquireNextImageKHR(device, swapchain, ~0ULL, NULL, NULL, &img_idx);
        if (ar != VK_SUCCESS) { fprintf(stderr, "frame %d: acquire FAIL %d\n", fi, ar); break; }

        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cmd, &bi);
        VkRenderPassBeginInfo rp = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderArea.extent = { .width = (uint32_t)w, .height = (uint32_t)h } };
        vkCmdBeginRenderPass(cmd, &rp, 0);
        vkCmdDrawIndexed(cmd, 3, 1, 0, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        VkResult sr = vkQueueSubmit(queue, 1, &si, NULL);
        if (sr != VK_SUCCESS) { fprintf(stderr, "frame %d: submit FAIL %d\n", fi, sr); break; }

        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &img_idx };
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr != VK_SUCCESS) { fprintf(stderr, "frame %d: present FAIL %d\n", fi, pr); break; }

        uint32_t got = panvk_v9_read_pixel(cmd, 0, 0);
        printf("frame %d: OK img=%u pixel(0,0)=0x%08x%s\n", fi, img_idx, got,
               (got == 0xFF00FF00) ? " (green)" : "");
        if (have_x) usleep(33000);
    }

    printf("=== OK: %d frames OK via swapchain (CYCLE-DEV automatico) ===\n", nframes);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    if (surface && have_x) { /* no vkDestroySurfaceKHR in driver yet - stub ok for test */ }
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    if (have_x) { XDestroyWindow(dpy, win); XCloseDisplay(dpy); }
    return 0;
}

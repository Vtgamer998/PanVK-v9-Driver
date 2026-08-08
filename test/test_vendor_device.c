/*
 * test_vendor_device.c — cria um VkDevice REAL no driver vendor
 * (libGLES_mali.so via loader do sistema) fora do contexto HAL.
 * Se passar, o driver vendor é 100% utilizável por processo normal
 * (que é o que o DXVK no Winlator faz).
 */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;

#define GIPA(i, n) ((PFN_##n)gipa(i, #n))

int main(void) {
    void *driver = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!driver) { printf("loader do sistema: %s\n", dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(driver, "vkGetInstanceProcAddr");

    VkInstance inst;
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    if (GIPA(NULL, vkCreateInstance)(&ici, NULL, &inst) != VK_SUCCESS) { printf("instance falhou\n"); return 1; }

    VkPhysicalDevice pd;
    uint32_t n = 1;
    if (GIPA(inst, vkEnumeratePhysicalDevices)(inst, &n, &pd) != VK_SUCCESS || n == 0) { printf("sem device\n"); return 1; }

    VkPhysicalDeviceProperties p;
    GIPA(inst, vkGetPhysicalDeviceProperties)(pd, &p);
    printf("device: %s, api %u.%u.%u\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));

    VkQueueFamilyProperties qf;
    uint32_t nq = 1;
    GIPA(inst, vkGetPhysicalDeviceQueueFamilyProperties)(pd, &nq, &qf);
    printf("queue family 0: count=%u flags=0x%x\n", qf.queueCount, qf.queueFlags);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev;
    VkResult r = GIPA(inst, vkCreateDevice)(pd, &dci, NULL, &dev);
    if (r != VK_SUCCESS) { printf("vkCreateDevice falhou: %d\n", r); return 1; }

    VkQueue q;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    PFN_vkGetDeviceQueue gdq = (PFN_vkGetDeviceQueue)gdpa(dev, "vkGetDeviceQueue");
    PFN_vkDestroyDevice gdd = (PFN_vkDestroyDevice)gdpa(dev, "vkDestroyDevice");
    gdq(dev, 0, 0, &q);
    if (!q) { printf("vkGetDeviceQueue falhou\n"); return 1; }

    printf("DEVICE CRIADO OK (fora do contexto HAL) - DXVK vai funcionar\n");

    gdd(dev, NULL);
    GIPA(inst, vkDestroyInstance)(inst, NULL);
    return 0;
}

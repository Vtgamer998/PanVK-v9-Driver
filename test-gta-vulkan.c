// test-gta-vulkan.c — Simula o que GTA V precisa do Vulkan
// Compilar: clang -o test-gta test-gta-vulkan.c -lvulkan -lm
// Rodar: VK_ICD_FILENAMES=/path/to/panvk_v9_icd.aarch64.json ./test-gta

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define LOG(fmt, ...) fprintf(stderr, "[GTA-TEST] " fmt "\n", ##__VA_ARGS__)
#define PASS(name) fprintf(stderr, "  [PASS] %s\n", name)
#define FAIL(name, reason) fprintf(stderr, "  [FAIL] %s — %s\n", name, reason)

// Extensões que GTA V / DXVK precisa
static const char* REQUIRED_INSTANCE_EXTS[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
};
static const int NUM_REQUIRED_INSTANCE_EXTS = 4;

static const char* REQUIRED_DEVICE_EXTS[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME,
    VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    VK_KHR_MAINTENANCE2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE3_EXTENSION_NAME,
    "VK_KHR_maintenance4",
    VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
    VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME,
    VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME,
    VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
    VK_EXT_SEPARATE_STENCIL_USAGE_EXTENSION_NAME,
    VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
    "VK_KHR_shader_float_controls",
    VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
};
static const int NUM_REQUIRED_DEVICE_EXTS = 24;

// Propriedades físicas que GTA V espera
static VkPhysicalDeviceProperties2 expected_props;

static int check_instance_extensions(VkInstance inst) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
    VkExtensionProperties* exts = malloc(count * sizeof(VkExtensionProperties));
    vkEnumerateInstanceExtensionProperties(NULL, &count, exts);

    LOG("=== Extensões Instance (%d disponíveis) ===", count);
    int missing = 0;
    for (int i = 0; i < NUM_REQUIRED_INSTANCE_EXTS; i++) {
        int found = 0;
        for (uint32_t j = 0; j < count; j++) {
            if (strcmp(exts[j].extensionName, REQUIRED_INSTANCE_EXTS[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (found) {
            PASS(REQUIRED_INSTANCE_EXTS[i]);
        } else {
            FAIL(REQUIRED_INSTANCE_EXTS[i], "AUSENTE");
            missing++;
        }
    }
    free(exts);
    return missing;
}

static int check_device_extensions(VkPhysicalDevice phys) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL);
    VkExtensionProperties* exts = malloc(count * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, exts);

    LOG("=== Extensões Device (%d disponíveis) ===", count);
    int missing = 0;
    for (int i = 0; i < NUM_REQUIRED_DEVICE_EXTS; i++) {
        int found = 0;
        for (uint32_t j = 0; j < count; j++) {
            if (strcmp(exts[j].extensionName, REQUIRED_DEVICE_EXTS[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (found) {
            PASS(REQUIRED_DEVICE_EXTS[i]);
        } else {
            FAIL(REQUIRED_DEVICE_EXTS[i], "AUSENTE");
            missing++;
        }
    }
    free(exts);
    return missing;
}

static void check_physical_device(VkPhysicalDevice phys) {
    VkPhysicalDeviceProperties2 props2 = {0};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    vkGetPhysicalDeviceProperties2(phys, &props2);

    LOG("=== Propriedades do Dispositivo ===");
    LOG("  deviceName: %s", props2.properties.deviceName);
    LOG("  deviceType: %d (1=CPU, 2=DISCRETE_GPU, 3=VIRTUAL_GPU, 4=INTEGRATED_GPU)", props2.properties.deviceType);
    LOG("  vendorID: 0x%04x", props2.properties.vendorID);
    LOG("  deviceID: 0x%04x", props2.properties.deviceID);
    LOG("  apiVersion: %d.%d.%d",
        VK_VERSION_MAJOR(props2.properties.apiVersion),
        VK_VERSION_MINOR(props2.properties.apiVersion),
        VK_VERSION_PATCH(props2.properties.apiVersion));
    LOG("  driverVersion: %d.%d.%d",
        VK_VERSION_MAJOR(props2.properties.driverVersion),
        VK_VERSION_MINOR(props2.properties.driverVersion),
        VK_VERSION_PATCH(props2.properties.driverVersion));

    // Limites que GTA V precisa
    LOG("=== Limites do Dispositivo ===");
    VkPhysicalDeviceLimits lim = props2.properties.limits;
    LOG("  maxImageDimension2D: %u", lim.maxImageDimension2D);
    LOG("  maxImageDimension3D: %u", lim.maxImageDimension3D);
    LOG("  maxUniformBufferRange: %u", lim.maxUniformBufferRange);
    LOG("  maxStorageBufferRange: %u", lim.maxStorageBufferRange);
    LOG("  maxPushConstantsSize: %u", lim.maxPushConstantsSize);
    LOG("  maxComputeWorkGroupCount[0]: %u", lim.maxComputeWorkGroupCount[0]);
    LOG("  maxComputeWorkGroupSize[0]: %u", lim.maxComputeWorkGroupSize[0]);
    LOG("  maxColorAttachments: %u", lim.maxColorAttachments);
    LOG("  maxViewportDimensions[0]: %u", lim.maxViewportDimensions[0]);
    LOG("  maxFramebufferWidth: %u", lim.maxFramebufferWidth);
    LOG("  maxFramebufferHeight: %u", lim.maxFramebufferHeight);
    LOG("  maxTextureSize: %u", lim.maxImageDimension2D);
    LOG("  maxSamplerAnisotropy: %f", lim.maxSamplerAnisotropy);
    LOG("  timestampPeriod: %llu", (unsigned long long)lim.timestampPeriod);

    // Memory
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    LOG("=== Memória ===");
    LOG("  memoryHeapCount: %u", mem.memoryHeapCount);
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++) {
        LOG("  heap[%llu]: size=%llu MB flags=%s",
            (unsigned long long)i,
            (unsigned long long)(mem.memoryHeaps[i].size / (1024*1024)),
            (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL" : "HOST_VISIBLE");
    }
    LOG("  memoryTypeCount: %u", mem.memoryTypeCount);

    // Queue families
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(phys, &qf_count, NULL);
    VkQueueFamilyProperties2* qf = malloc(qf_count * sizeof(VkQueueFamilyProperties2));
    for (uint32_t i = 0; i < qf_count; i++) {
        qf[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(phys, &qf_count, qf);
    LOG("=== Queue Families (%d) ===", qf_count);
    for (uint32_t i = 0; i < qf_count; i++) {
        LOG("  queue[%d]: count=%d flags=0x%x",
            i, qf[i].queueFamilyProperties.queueCount, qf[i].queueFamilyProperties.queueFlags);
    }
    free(qf);
}

static int test_create_device(VkInstance inst, VkPhysicalDevice phys) {
    LOG("=== Teste: vkCreateDevice ===");

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, NULL);
    VkQueueFamilyProperties* qf = malloc(qf_count * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, qf);

    int graphics_queue = -1;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_queue = i;
            break;
        }
    }
    free(qf);

    if (graphics_queue < 0) {
        FAIL("vkCreateDevice", "nenhum queue family com GRAPHICS_BIT");
        return 1;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {0};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = graphics_queue;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    // Habilitar extensões que conseguimos
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &ext_count, NULL);
    VkExtensionProperties* avail = malloc(ext_count * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &ext_count, avail);

    const char* enable_exts[64];
    int enable_count = 0;

    for (int i = 0; i < NUM_REQUIRED_DEVICE_EXTS && enable_count < 64; i++) {
        for (uint32_t j = 0; j < ext_count; j++) {
            if (strcmp(avail[j].extensionName, REQUIRED_DEVICE_EXTS[i]) == 0) {
                enable_exts[enable_count++] = REQUIRED_DEVICE_EXTS[i];
                break;
            }
        }
    }
    free(avail);

    LOG("  Habilitando %d extensões do device", enable_count);

    VkDeviceCreateInfo device_info = {0};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = enable_count;
    device_info.ppEnabledExtensionNames = enable_exts;

    VkDevice device = VK_NULL_HANDLE;
    VkResult result = vkCreateDevice(phys, &device_info, NULL, &device);

    if (result == VK_SUCCESS) {
        PASS("vkCreateDevice");
        vkDestroyDevice(device, NULL);
        return 0;
    } else {
        FAIL("vkCreateDevice", "falhou");
        return 1;
    }
}

static int test_swapchain(VkInstance inst, VkPhysicalDevice phys) {
    LOG("=== Teste: vkCreateSwapchainKHR ===");

    // Precisa de superfície para testar swapchain
    // No Termux sem display, só testamos se a extensão existe
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &ext_count, NULL);
    VkExtensionProperties* exts = malloc(ext_count * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &ext_count, exts);

    int has_swapchain = 0;
    for (uint32_t i = 0; i < ext_count; i++) {
        if (strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            has_swapchain = 1;
            break;
        }
    }
    free(exts);

    if (has_swapchain) {
        PASS("VK_KHR_swapchain (extensão disponível)");
    } else {
        FAIL("VK_KHR_swapchain", "extensão AUSENTE");
    }
    return has_swapchain ? 0 : 1;
}

int main(int argc, char** argv) {
    LOG("============================================");
    LOG(" GTA V Vulkan Requirement Test (PanVK-v9)");
    LOG("============================================");
    LOG("");

    // Criar VkInstance
    VkApplicationInfo app_info = {0};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "GTA V Vulkan Test";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "RAGE Engine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_MAKE_VERSION(1, 3, 0);

    VkInstanceCreateInfo inst_info = {0};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pApplicationInfo = &app_info;

    VkInstance inst = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&inst_info, NULL, &inst);
    if (result != VK_SUCCESS) {
        LOG("ERRO: vkCreateInstance falhou (%d)", result);
        return 1;
    }
    PASS("vkCreateInstance");

    // Verificar extensões instance
    int missing_inst = check_instance_extensions(inst);

    // Enumerar dispositivos
    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(inst, &dev_count, NULL);
    VkPhysicalDevice* phys_devs = malloc(dev_count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(inst, &dev_count, phys_devs);
    LOG("");
    LOG("=== %d dispositivo(s) Vulkan encontrado(s) ===", dev_count);

    int total_missing = missing_inst;
    for (uint32_t i = 0; i < dev_count; i++) {
        LOG("");
        LOG("--- Dispositivo %d ---", i);
        check_physical_device(phys_devs[i]);
        total_missing += check_device_extensions(phys_devs[i]);
        total_missing += test_create_device(inst, phys_devs[i]);
        total_missing += test_swapchain(inst, phys_devs[i]);
    }

    free(phys_devs);
    vkDestroyInstance(inst, NULL);

    LOG("");
    LOG("============================================");
    LOG(" RESULTADO: %d extensões/funções AUSENTES", total_missing);
    if (total_missing == 0) {
        LOG(" O driver PanVK atende TODOS os requisitos do GTA V!");
    } else {
        LOG(" O driver PanVK NÃO atende todos os requisitos.");
        LOG(" GTA V precisa das extensões listadas como [FAIL] acima.");
    }
    LOG("============================================");

    return total_missing;
}

/*
 * probe_vendor_vulkan.c — sonda o driver Vulkan VENDOR (libGLES_mali.so)
 * via dlopen + vkGetInstanceProcAddr (como o loader faz), SEM tocar na GPU.
 * Saida: apiVersion, deviceName, features, memoria, queue families,
 * extensoes -> decide qual DXVK/vkd3d usar no Winlator Mali.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

static void *driver = NULL;
static PFN_vkGetInstanceProcAddr gipa = NULL;

#define GIPA(i, n) ((PFN_##n)gipa(i, #n))

/* --- Android dynamic-linker namespace API (bionic linker.h, layout estavel) --- */
struct android_namespace_t;
#define ANDROID_NAMESPACE_TYPE_SHARED (1ull << 1)
#define ANDROID_DLEXT_USE_NAMESPACE   0x200

typedef struct {
    uint64_t flags;
    void *reserved_addr;
    size_t reserved_size;
    int fd;
    off_t offset;
    const char *path;
    void *library_fd;
    uint64_t library_fd_offset;
    struct android_namespace_t *library_namespace;
} android_dlextinfo_kbase;

typedef struct android_namespace_t *(*android_create_namespace_fn)(
    const char *name, const char *ld_library_path, const char *default_library_path,
    uint64_t type, const char *permitted_when_isolated);
typedef void *(*android_dlopen_ext_fn)(const char *filename, int flags,
                                       const android_dlextinfo_kbase *extinfo);

static void *load_vendor(const char *path) {
    void *libdl = dlopen("libdl.so", RTLD_NOW | RTLD_LOCAL);
    if (!libdl) { printf("libdl: %s\n", dlerror()); return NULL; }

    android_create_namespace_fn create_ns =
        (android_create_namespace_fn)dlsym(libdl, "android_create_namespace");
    android_dlopen_ext_fn dlopen_ext =
        (android_dlopen_ext_fn)dlsym(libdl, "android_dlopen_ext");
    if (!create_ns || !dlopen_ext) {
        printf("namespace API indisponivel\n");
        return NULL;
    }

    struct android_namespace_t *ns = create_ns(
        "vendor-kbase",
        "/vendor/lib64:/vendor/lib64/hw:/vendor/lib64/egl:/vendor/lib:/vendor/lib/hw",
        "/system/lib64:/system/lib", ANDROID_NAMESPACE_TYPE_SHARED, NULL);
    if (!ns) { printf("android_create_namespace falhou\n"); return NULL; }

    android_dlextinfo_kbase ext = { .flags = ANDROID_DLEXT_USE_NAMESPACE,
                                    .library_namespace = ns };
    return dlopen_ext(path, RTLD_NOW | RTLD_LOCAL, &ext);
}

int main(void) {
    const char *path = getenv("V9_VENDOR_DRIVER");
    if (!path) path = "/vendor/lib64/egl/libGLES_mali.so";

    driver = load_vendor(path);
    if (!driver) {
        driver = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!driver) { printf("dlopen falhou: %s\n", dlerror()); return 1; }
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(driver, "vkGetInstanceProcAddr");
    if (!gipa) { printf("vkGetInstanceProcAddr nao exportado (nao e driver vulkan?)\n"); return 1; }

    printf("== %s ==\n", path);

    PFN_vkEnumerateInstanceVersion eiVersion = GIPA(NULL, vkEnumerateInstanceVersion);
    uint32_t apiVer = 0;
    if (eiVersion) { eiVersion(&apiVer); printf("instance apiVersion = %u.%u.%u\n", VK_API_VERSION_MAJOR(apiVer), VK_API_VERSION_MINOR(apiVer), VK_API_VERSION_PATCH(apiVer)); }
    else printf("instance apiVersion = n/d\n");

    PFN_vkEnumerateInstanceExtensionProperties eiExt = GIPA(NULL, vkEnumerateInstanceExtensionProperties);
    uint32_t n = 0; eiExt(NULL, &n, NULL);
    VkExtensionProperties *ext = malloc(n * sizeof(*ext));
    eiExt(NULL, &n, ext);
    printf("instance extensions (%u):\n", n);
    for (uint32_t i = 0; i < n; i++) printf("  %s\n", ext[i].extensionName);
    free(ext);

    VkInstance inst;
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    PFN_vkCreateInstance ci = GIPA(NULL, vkCreateInstance);
    VkResult r = ci(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        printf("vkCreateInstance falhou: %d (tentando sem appinfo)\n", r);
        ici.pApplicationInfo = NULL;
        r = ci(&ici, NULL, &inst);
        if (r != VK_SUCCESS) { printf("vkCreateInstance ainda falhou: %d\n", r); return 1; }
    }

    PFN_vkEnumeratePhysicalDevices epd = GIPA(inst, vkEnumeratePhysicalDevices);
    uint32_t nd = 0; epd(inst, &nd, NULL);
    printf("\nphysical devices: %u\n", nd);
    if (nd == 0) return 0;

    VkPhysicalDevice pd;
    epd(inst, &nd, &pd);

    PFN_vkGetPhysicalDeviceProperties gpd = GIPA(inst, vkGetPhysicalDeviceProperties);
    VkPhysicalDeviceProperties p;
    gpd(pd, &p);
    printf("\ndeviceName        = %s\n", p.deviceName);
    printf("deviceType        = %d\n", p.deviceType);
    printf("driverVersion     = 0x%x\n", p.driverVersion);
    printf("apiVersion        = %u.%u.%u\n", VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    printf("vendorID          = 0x%x\n", p.vendorID);
    printf("deviceID          = 0x%x\n", p.deviceID);

    PFN_vkGetPhysicalDeviceFeatures gdf = GIPA(inst, vkGetPhysicalDeviceFeatures);
    VkPhysicalDeviceFeatures f;
    gdf(pd, &f);
    printf("\nfeatures:\n  tessellation=%d geometry=%d shaderFloat64=%d shaderInt64=%d\n  multiViewport=%d samplerAnisotropy=%d\n",
        f.tessellationShader, f.geometryShader, f.shaderFloat64, f.shaderInt64, f.multiViewport, f.samplerAnisotropy);
    printf("  robustBufferAccess=%d largePoints=%d wideLines=%d\n", f.robustBufferAccess, f.largePoints, f.wideLines);

    PFN_vkGetPhysicalDeviceMemoryProperties gmp = GIPA(inst, vkGetPhysicalDeviceMemoryProperties);
    VkPhysicalDeviceMemoryProperties m;
    gmp(pd, &m);
    printf("\nmemory types=%u heaps=%u:\n", m.memoryTypeCount, m.memoryHeapCount);
    for (uint32_t i = 0; i < m.memoryTypeCount; i++) {
        printf("  type%u heap=%u flags=0x%x", i, m.memoryTypes[i].heapIndex, m.memoryTypes[i].propertyFlags);
        if (m.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) printf(" DEVICE_LOCAL");
        if (m.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) printf(" HOST_VISIBLE");
        if (m.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) printf(" HOST_COHERENT");
        printf("\n");
    }
    for (uint32_t i = 0; i < m.memoryHeapCount; i++) printf("  heap%u size=%llu MB flags=0x%x\n", i, (unsigned long long)m.memoryHeaps[i].size >> 20, m.memoryHeaps[i].flags);

    PFN_vkGetPhysicalDeviceQueueFamilyProperties gqf = GIPA(inst, vkGetPhysicalDeviceQueueFamilyProperties);
    uint32_t nq = 0; gqf(pd, &nq, NULL);
    VkQueueFamilyProperties *qf = malloc(nq * sizeof(*qf));
    gqf(pd, &nq, qf);
    printf("\nqueue families=%u:\n", nq);
    for (uint32_t i = 0; i < nq; i++) {
        printf("  qf%u count=%u flags=0x%x", i, qf[i].queueCount, qf[i].queueFlags);
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) printf(" GRAPHICS");
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) printf(" COMPUTE");
        if (qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) printf(" TRANSFER");
        printf(" minImageTransferGranularity=(%u,%u,%u)\n", qf[i].minImageTransferGranularity.width, qf[i].minImageTransferGranularity.height, qf[i].minImageTransferGranularity.depth);
    }
    free(qf);

    PFN_vkEnumerateDeviceExtensionProperties ede = GIPA(inst, vkEnumerateDeviceExtensionProperties);
    uint32_t ne = 0; ede(pd, NULL, &ne, NULL);
    VkExtensionProperties *de = malloc(ne * sizeof(*de));
    ede(pd, NULL, &ne, de);
    printf("\ndevice extensions (%u):\n", ne);
    for (uint32_t i = 0; i < ne; i++) printf("  %s\n", de[i].extensionName);
    free(de);

    PFN_vkDestroyInstance di = GIPA(inst, vkDestroyInstance);
    di(inst, NULL);
    dlclose(driver);
    printf("\nprobe ok\n");
    return 0;
}

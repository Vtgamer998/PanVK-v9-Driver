/*
 * Loader + panvk-style image/memory test:
 * dlopen() the ICD, resolve via vk_icdGetInstanceProcAddr (like the real Vulkan
 * loader), then exercise the full image pipeline:
 *   allocate/bind buffer+image -> buffer->image copy -> image clear ->
 *   image->buffer readback -> image blit (nearest) -> image->image copy.
 * Copy/clear are executed at record time, so data is verified before submit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "panvk_v9_entrypoints.h"

typedef PFN_vkVoidFunction (*PFN_vk_icdGetInstanceProcAddr)(VkInstance instance, const char *pName);

/* VK_IMAGE_ASPECT_COLOR_BIT */
#define ASPECT_COLOR 0x1

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    printf("=== Testing Loader + panvk-style Image/Memory Support ===\n");

    void *handle = dlopen("./libvulkan_panvk_v9.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) { fprintf(stderr, "FAIL: dlopen: %s\n", dlerror()); return 1; }
    PFN_vk_icdGetInstanceProcAddr gpa = (PFN_vk_icdGetInstanceProcAddr)dlsym(handle, "vk_icdGetInstanceProcAddr");
    if (!gpa) { fprintf(stderr, "FAIL: dlsym vk_icdGetInstanceProcAddr: %s\n", dlerror()); return 1; }
    printf("SUCCESS: dlopen + resolved vk_icdGetInstanceProcAddr\n");

#define LOOKUP(type, name) type pfn_##name = (type)gpa(NULL, #name); \
    if (!pfn_##name) { fprintf(stderr, "FAIL: ICD missing proc '%s'\n", #name); dlclose(handle); return 1; }

    typedef VkResult (*PFN_vkCreateInstance)(const struct VkInstanceCreateInfo *, void *, VkInstance *);
    typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t *, VkPhysicalDevice *);
    typedef void (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, struct VkPhysicalDeviceProperties *);
    typedef void (*PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, void *);
    typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const struct VkDeviceCreateInfo *, void *, VkDevice *);
    typedef void (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue *);
    typedef VkResult (*PFN_vkAllocateMemory)(VkDevice, const struct VkMemoryAllocateInfo *, void *, VkDeviceMemory *);
    typedef void (*PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, void *);
    typedef VkResult (*PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, uint32_t, void **);
    typedef void (*PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);
    typedef VkResult (*PFN_vkCreateBuffer)(VkDevice, const struct VkBufferCreateInfo *, void *, VkBuffer *);
    typedef void (*PFN_vkDestroyBuffer)(VkDevice, VkBuffer, void *);
    typedef void (*PFN_vkGetBufferMemoryRequirements)(VkDevice, VkBuffer, struct VkMemoryRequirements *);
    typedef VkResult (*PFN_vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
    typedef VkResult (*PFN_vkCreateImage)(VkDevice, const struct VkImageCreateInfo *, void *, VkImage *);
    typedef void (*PFN_vkDestroyImage)(VkDevice, VkImage, void *);
    typedef void (*PFN_vkGetImageMemoryRequirements)(VkDevice, VkImage, struct VkMemoryRequirements *);
    typedef void (*PFN_vkGetImageSubresourceLayout)(VkDevice, VkImage, const void *, void *);
    typedef VkResult (*PFN_vkBindImageMemory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
    typedef VkResult (*PFN_vkCreateImageView)(VkDevice, const void *, void *, VkImageView *);
    typedef void (*PFN_vkDestroyImageView)(VkDevice, VkImageView, void *);
    typedef VkResult (*PFN_vkCreateCommandPool)(VkDevice, const struct VkCommandPoolCreateInfo *, void *, VkCommandPool *);
    typedef VkResult (*PFN_vkAllocateCommandBuffers)(VkDevice, const struct VkCommandBufferAllocateInfo *, VkCommandBuffer *);
    typedef VkResult (*PFN_vkBeginCommandBuffer)(VkCommandBuffer, const struct VkCommandBufferBeginInfo *);
    typedef VkResult (*PFN_vkEndCommandBuffer)(VkCommandBuffer);
    typedef void (*PFN_vkCmdPipelineBarrier)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t, const void *, uint32_t, const void *, uint32_t, const void *);
    typedef void (*PFN_vkCmdCopyBufferToImage)(VkCommandBuffer, VkBuffer, VkImage, uint32_t, uint32_t, const void *);
    typedef void (*PFN_vkCmdCopyImageToBuffer)(VkCommandBuffer, VkImage, uint32_t, VkBuffer, uint32_t, const void *);
    typedef void (*PFN_vkCmdCopyImage)(VkCommandBuffer, VkImage, uint32_t, VkImage, uint32_t, uint32_t, const void *);
    typedef void (*PFN_vkCmdBlitImage)(VkCommandBuffer, VkImage, uint32_t, VkImage, uint32_t, uint32_t, const void *, uint32_t);
    typedef void (*PFN_vkCmdClearColorImage)(VkCommandBuffer, VkImage, uint32_t, const void *, uint32_t, const void *);
    typedef VkResult (*PFN_vkQueueSubmit)(VkQueue, uint32_t, const struct VkSubmitInfo *, void *);

    LOOKUP(PFN_vkCreateInstance, vkCreateInstance);
    LOOKUP(PFN_vkEnumeratePhysicalDevices, vkEnumeratePhysicalDevices);
    LOOKUP(PFN_vkGetPhysicalDeviceProperties, vkGetPhysicalDeviceProperties);
    LOOKUP(PFN_vkGetPhysicalDeviceMemoryProperties, vkGetPhysicalDeviceMemoryProperties);
    LOOKUP(PFN_vkCreateDevice, vkCreateDevice);
    LOOKUP(PFN_vkGetDeviceQueue, vkGetDeviceQueue);
    LOOKUP(PFN_vkAllocateMemory, vkAllocateMemory);
    LOOKUP(PFN_vkFreeMemory, vkFreeMemory);
    LOOKUP(PFN_vkMapMemory, vkMapMemory);
    LOOKUP(PFN_vkUnmapMemory, vkUnmapMemory);
    LOOKUP(PFN_vkCreateBuffer, vkCreateBuffer);
    LOOKUP(PFN_vkDestroyBuffer, vkDestroyBuffer);
    LOOKUP(PFN_vkGetBufferMemoryRequirements, vkGetBufferMemoryRequirements);
    LOOKUP(PFN_vkBindBufferMemory, vkBindBufferMemory);
    LOOKUP(PFN_vkCreateImage, vkCreateImage);
    LOOKUP(PFN_vkDestroyImage, vkDestroyImage);
    LOOKUP(PFN_vkGetImageMemoryRequirements, vkGetImageMemoryRequirements);
    LOOKUP(PFN_vkGetImageSubresourceLayout, vkGetImageSubresourceLayout);
    LOOKUP(PFN_vkBindImageMemory, vkBindImageMemory);
    LOOKUP(PFN_vkCreateImageView, vkCreateImageView);
    LOOKUP(PFN_vkDestroyImageView, vkDestroyImageView);
    LOOKUP(PFN_vkCreateCommandPool, vkCreateCommandPool);
    LOOKUP(PFN_vkAllocateCommandBuffers, vkAllocateCommandBuffers);
    LOOKUP(PFN_vkBeginCommandBuffer, vkBeginCommandBuffer);
    LOOKUP(PFN_vkEndCommandBuffer, vkEndCommandBuffer);
    LOOKUP(PFN_vkCmdPipelineBarrier, vkCmdPipelineBarrier);
    LOOKUP(PFN_vkCmdCopyBufferToImage, vkCmdCopyBufferToImage);
    LOOKUP(PFN_vkCmdCopyImageToBuffer, vkCmdCopyImageToBuffer);
    LOOKUP(PFN_vkCmdCopyImage, vkCmdCopyImage);
    LOOKUP(PFN_vkCmdBlitImage, vkCmdBlitImage);
    LOOKUP(PFN_vkCmdClearColorImage, vkCmdClearColorImage);
    LOOKUP(PFN_vkQueueSubmit, vkQueueSubmit);
#undef LOOKUP
    printf("SUCCESS: Resolved image/memory/command proc addresses from ICD\n");

    /* Instance + Physical Device */
    VkInstance instance = NULL;
    struct VkInstanceCreateInfo instInfo = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    CHECK(pfn_vkCreateInstance(&instInfo, NULL, &instance) == VK_SUCCESS, "vkCreateInstance");
    uint32_t count = 0;
    pfn_vkEnumeratePhysicalDevices(instance, &count, NULL);
    VkPhysicalDevice physDev = NULL;
    pfn_vkEnumeratePhysicalDevices(instance, &count, &physDev);
    struct VkPhysicalDeviceProperties props;
    pfn_vkGetPhysicalDeviceProperties(physDev, &props);
    printf("Device: '%s'\n", props.deviceName);

    /* Validate VkPhysicalDeviceMemoryProperties layout:
     * u32 count | 32 x {u32 flags,u32 heap} | u32 heapCount | 16 x {u64 size,u32 flags} */
    uint8_t memprops[520];
    pfn_vkGetPhysicalDeviceMemoryProperties(physDev, memprops);
    uint32_t type_count = *(uint32_t *)(memprops + 0);
    uint32_t t0_flags = *(uint32_t *)(memprops + 4);
    uint32_t t0_heap  = *(uint32_t *)(memprops + 8);
    uint32_t t1_flags = *(uint32_t *)(memprops + 12);
    uint32_t heap_count = *(uint32_t *)(memprops + 260);
    uint64_t heap0_size = *(uint64_t *)(memprops + 264);
    uint32_t heap0_flags = *(uint32_t *)(memprops + 272);
    CHECK(type_count == 2, "memoryTypeCount == 2 (got %u)", type_count);
    CHECK((t0_flags & 0x7) == 0x7, "type0 DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT (got 0x%x)", t0_flags);
    CHECK(t0_heap == 0, "type0 heapIndex == 0");
    CHECK((t1_flags & 0x1) == 0x1, "type1 DEVICE_LOCAL (got 0x%x)", t1_flags);
    CHECK(heap_count == 1, "memoryHeapCount == 1 (got %u)", heap_count);
    CHECK(heap0_size == 4096ULL * 1024 * 1024, "heap0.size == 4GB (got %llu)", (unsigned long long)heap0_size);
    CHECK(heap0_flags == 0x1, "heap0 DEVICE_LOCAL");
    printf("SUCCESS: VkPhysicalDeviceMemoryProperties layout correct (2 types, 1 heap)\n");

    /* Device + Queue */
    VkDevice device = NULL;
    struct VkDeviceCreateInfo devInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    CHECK(pfn_vkCreateDevice(physDev, &devInfo, NULL, &device) == VK_SUCCESS, "vkCreateDevice");
    VkQueue queue = NULL;
    pfn_vkGetDeviceQueue(device, 0, 0, &queue);
    CHECK(queue != NULL, "vkGetDeviceQueue");

    /* ---- Buffer A: source gradient data ---- */
    const uint32_t W = 64, H = 32;
    struct VkBufferCreateInfo bufInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = W * H * 4,
        .usage = 0x8 /* TRANSFER_SRC */,
    };
    VkBuffer bufA = NULL;
    CHECK(pfn_vkCreateBuffer(device, &bufInfo, NULL, &bufA) == VK_SUCCESS, "vkCreateBuffer(A)");
    struct VkMemoryRequirements breq;
    pfn_vkGetBufferMemoryRequirements(device, bufA, &breq);
    CHECK(breq.size >= W * H * 4, "buffer req.size ok (got %llu)", (unsigned long long)breq.size);
    CHECK((breq.memoryTypeBits & 0x3) != 0, "buffer memoryTypeBits has types 0/1");
    struct VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = breq.size,
        .memoryTypeIndex = 0,
    };
    VkDeviceMemory memA = NULL;
    CHECK(pfn_vkAllocateMemory(device, &allocInfo, NULL, &memA) == VK_SUCCESS, "vkAllocateMemory(A)");
    CHECK(pfn_vkBindBufferMemory(device, bufA, memA, 0) == VK_SUCCESS, "vkBindBufferMemory(A)");
    void *mapA = NULL;
    CHECK(pfn_vkMapMemory(device, memA, 0, breq.size, 0, &mapA) == VK_SUCCESS, "vkMapMemory(A)");
    uint8_t *gradient = mapA;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t px = 0xFF000000u | (x << 16) | (y << 8) | ((x + y) & 0xFF);
            memcpy(gradient + (y * W + x) * 4, &px, 4);
        }
    pfn_vkUnmapMemory(device, memA);
    printf("SUCCESS: buffer allocated/bound/mapped, gradient written\n");

    /* ---- Image I1: 64x32 R8G8B8A8, 2 mips, 2 layers ---- */
    struct VkImageCreateInfo imgInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = 1, /* 2D */
        .format = 37,   /* VK_FORMAT_R8G8B8A8_UNORM */
        .extent = { .width = W, .height = H, .depth = 1 },
        .mipLevels = 2,
        .arrayLayers = 2,
        .samples = 1,
        .tiling = 0,    /* LINEAR */
        .usage = 0x1 | 0x2 | 0x4, /* TRANSFER_SRC | TRANSFER_DST | SAMPLED */
    };
    VkImage img1 = NULL;
    CHECK(pfn_vkCreateImage(device, &imgInfo, NULL, &img1) == VK_SUCCESS, "vkCreateImage(I1)");
    struct VkMemoryRequirements ireq;
    pfn_vkGetImageMemoryRequirements(device, img1, &ireq);
    /* mip0: rowPitch=256, slice=8192; level0=2*8192=16384.
     * mip1: rowPitch=128, slice=2048; level1=2*2048=4096. total=20480 */
    CHECK(ireq.size == 20480, "image req.size == 20480 (got %llu)", (unsigned long long)ireq.size);
    CHECK(ireq.alignment == 4096, "image req.alignment == 4096 (got %llu)", (unsigned long long)ireq.alignment);
    CHECK((ireq.memoryTypeBits & 0x3) != 0, "image memoryTypeBits has types 0/1");
    allocInfo.allocationSize = ireq.size;
    allocInfo.memoryTypeIndex = 1; /* DEVICE_LOCAL-only type must also work */
    VkDeviceMemory memI = NULL;
    CHECK(pfn_vkAllocateMemory(device, &allocInfo, NULL, &memI) == VK_SUCCESS, "vkAllocateMemory(I1)");
    CHECK(pfn_vkBindImageMemory(device, img1, memI, 0) == VK_SUCCESS, "vkBindImageMemory(I1)");
    printf("SUCCESS: image created, requirements=20480@4096, bound to DEVICE_LOCAL type\n");

    /* Subresource layout checks (linear tiling) */
    struct VkImageSubresource sub = { .aspectMask = ASPECT_COLOR, .mipLevel = 0, .arrayLayer = 0 };
    struct VkSubresourceLayout sublay;
    pfn_vkGetImageSubresourceLayout(device, img1, &sub, &sublay);
    CHECK(sublay.rowPitch == 256, "mip0 rowPitch == 256 (got %llu)", (unsigned long long)sublay.rowPitch);
    CHECK(sublay.offset == 0, "mip0 offset == 0 (got %llu)", (unsigned long long)sublay.offset);
    sub.arrayLayer = 1;
    pfn_vkGetImageSubresourceLayout(device, img1, &sub, &sublay);
    CHECK(sublay.offset == 8192, "mip0 layer1 offset == 8192 (got %llu)", (unsigned long long)sublay.offset);
    sub.mipLevel = 1; sub.arrayLayer = 0;
    pfn_vkGetImageSubresourceLayout(device, img1, &sub, &sublay);
    CHECK(sublay.rowPitch == 128, "mip1 rowPitch == 128 (got %llu)", (unsigned long long)sublay.rowPitch);
    CHECK(sublay.offset == 16384, "mip1 offset == 16384 (got %llu)", (unsigned long long)sublay.offset);
    printf("SUCCESS: vkGetImageSubresourceLayout computes panvk-style linear layout\n");

    /* Image view */
    struct VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img1,
        .viewType = 1, /* 2D */
        .format = 37,
        .subresourceRange = { .aspectMask = ASPECT_COLOR, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
    };
    VkImageView view = NULL;
    CHECK(pfn_vkCreateImageView(device, &viewInfo, NULL, &view) == VK_SUCCESS, "vkCreateImageView");
    pfn_vkDestroyImageView(device, view, NULL);
    printf("SUCCESS: image view created/destroyed\n");

    /* ---- Buffer B/C/D: readback buffers ---- */
    struct VkBufferCreateInfo rbInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = W * H * 4,
        .usage = 0x1, /* TRANSFER_DST */
    };
    VkBuffer bufB = NULL, bufC = NULL, bufD = NULL;
    CHECK(pfn_vkCreateBuffer(device, &rbInfo, NULL, &bufB) == VK_SUCCESS, "vkCreateBuffer(B)");
    CHECK(pfn_vkCreateBuffer(device, &rbInfo, NULL, &bufC) == VK_SUCCESS, "vkCreateBuffer(C)");
    rbInfo.size = 8 * 4 * 4;
    CHECK(pfn_vkCreateBuffer(device, &rbInfo, NULL, &bufD) == VK_SUCCESS, "vkCreateBuffer(D)");
    pfn_vkGetBufferMemoryRequirements(device, bufB, &breq);
    allocInfo.allocationSize = breq.size; allocInfo.memoryTypeIndex = 0;
    VkDeviceMemory memB = NULL, memC = NULL, memD = NULL;
    CHECK(pfn_vkAllocateMemory(device, &allocInfo, NULL, &memB) == VK_SUCCESS, "vkAllocateMemory(B)");
    CHECK(pfn_vkBindBufferMemory(device, bufB, memB, 0) == VK_SUCCESS, "vkBindBufferMemory(B)");
    CHECK(pfn_vkAllocateMemory(device, &allocInfo, NULL, &memC) == VK_SUCCESS, "vkAllocateMemory(C)");
    CHECK(pfn_vkBindBufferMemory(device, bufC, memC, 0) == VK_SUCCESS, "vkBindBufferMemory(C)");
    allocInfo.allocationSize = breq.size;
    CHECK(pfn_vkAllocateMemory(device, &allocInfo, NULL, &memD) == VK_SUCCESS, "vkAllocateMemory(D)");
    CHECK(pfn_vkBindBufferMemory(device, bufD, memD, 0) == VK_SUCCESS, "vkBindBufferMemory(D)");

    /* ---- Command buffer recording ---- */
    VkCommandPool pool = NULL;
    struct VkCommandPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pfn_vkCreateCommandPool(device, &poolInfo, NULL, &pool);
    VkCommandBuffer cmd = NULL;
    struct VkCommandBufferAllocateInfo allocCB = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .commandBufferCount = 1,
    };
    pfn_vkAllocateCommandBuffers(device, &allocCB, &cmd);
    struct VkCommandBufferBeginInfo beginInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    pfn_vkBeginCommandBuffer(cmd, &beginInfo);

    /* barrier (no-op internally, must be accepted) */
    pfn_vkCmdPipelineBarrier(cmd, 0x100 | 0x200, 0x100 | 0x200, 0, 0, NULL, 0, NULL, 0, NULL);

    /* copy buffer A -> image mip0 layer0 */
    struct VkBufferImageCopy bic = {
        .bufferOffset = 0,
        .bufferRowLength = W,
        .bufferImageHeight = 0,
        .imageSubresource = { .aspectMask = ASPECT_COLOR, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { .width = W, .height = H, .depth = 1 },
    };
    pfn_vkCmdCopyBufferToImage(cmd, bufA, img1, 1 /* GENERAL */, 1, &bic);

    /* clear image mip0 layer1 to color (1.0f,0,0,1.0f) -> 0x3f800000 raw */
    VkClearColorValue red = { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } };
    struct VkImageSubresourceRange clearRange = {
        .aspectMask = ASPECT_COLOR, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 1, .layerCount = 1,
    };
    pfn_vkCmdClearColorImage(cmd, img1, 1 /* GENERAL */, &red, 1, &clearRange);

    /* copy image mip0 layer0 -> buffer B (gradient readback) */
    pfn_vkCmdCopyImageToBuffer(cmd, img1, 1, bufB, 1, &bic);
    /* copy image mip0 layer1 -> buffer C (cleared readback) */
    bic.imageSubresource.baseArrayLayer = 1;
    pfn_vkCmdCopyImageToBuffer(cmd, img1, 1, bufC, 1, &bic);

    /* blit image mip0 -> img2 (64x32 -> 8x4 nearest) */
    VkImage img2 = NULL;
    struct VkImageCreateInfo img2Info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = 1, .format = 37,
        .extent = { .width = 8, .height = 4, .depth = 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = 1, .tiling = 0,
        .usage = 0x1 | 0x2,
    };
    pfn_vkCreateImage(device, &img2Info, NULL, &img2);
    pfn_vkGetImageMemoryRequirements(device, img2, &ireq);
    allocInfo.allocationSize = ireq.size; allocInfo.memoryTypeIndex = 0;
    VkDeviceMemory memI2 = NULL;
    pfn_vkAllocateMemory(device, &allocInfo, NULL, &memI2);
    pfn_vkBindImageMemory(device, img2, memI2, 0);
    struct VkImageBlit blit = {
        .srcSubresource = { .aspectMask = ASPECT_COLOR, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)W, (int32_t)H, 1 } },
        .dstSubresource = { .aspectMask = ASPECT_COLOR, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .dstOffsets = { { 0, 0, 0 }, { 8, 4, 1 } },
    };
    pfn_vkCmdBlitImage(cmd, img1, 1, img2, 1, 1, &blit, 0 /* NEAREST */);

    /* copy img2 -> img1 mip0 layer0 area? No: image->image copy img2 -> img2 clone.
     * Verify vkCmdCopyImage between two images. */
    VkImage img3 = NULL;
    struct VkImageCreateInfo img3Info = img2Info;
    pfn_vkCreateImage(device, &img3Info, NULL, &img3);
    pfn_vkGetImageMemoryRequirements(device, img3, &ireq);
    allocInfo.allocationSize = ireq.size;
    VkDeviceMemory memI3 = NULL;
    pfn_vkAllocateMemory(device, &allocInfo, NULL, &memI3);
    pfn_vkBindImageMemory(device, img3, memI3, 0);
    struct VkImageCopy icopy = {
        .srcSubresource = { .aspectMask = ASPECT_COLOR, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { .aspectMask = ASPECT_COLOR, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .dstOffset = { 0, 0, 0 },
        .extent = { .width = 8, .height = 4, .depth = 1 },
    };
    pfn_vkCmdCopyImage(cmd, img2, 1, img3, 1, 1, &icopy);

    /* readback img3 -> buffer D */
    struct VkBufferImageCopy bicD = {
        .bufferOffset = 0, .bufferRowLength = 8, .bufferImageHeight = 0,
        .imageSubresource = { .aspectMask = ASPECT_COLOR, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { .width = 8, .height = 4, .depth = 1 },
    };
    pfn_vkCmdCopyImageToBuffer(cmd, img3, 1, bufD, 1, &bicD);

    pfn_vkEndCommandBuffer(cmd);

    /* ---- Verify data (executed at record time) ---- */
    void *mapB = NULL, *mapC = NULL, *mapD = NULL;
    pfn_vkMapMemory(device, memB, 0, W * H * 4, 0, &mapB);
    pfn_vkMapMemory(device, memC, 0, W * H * 4, 0, &mapC);
    pfn_vkMapMemory(device, memD, 0, 8 * 4 * 4, 0, &mapD);

    uint32_t ok_copy = 1;
    for (uint32_t y = 0; y < H && ok_copy; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t expect = 0xFF000000u | (x << 16) | (y << 8) | ((x + y) & 0xFF);
            uint32_t got; memcpy(&got, (uint8_t *)mapB + (y * W + x) * 4, 4);
            if (got != expect) { ok_copy = 0; printf("  copy mismatch at (%u,%u): got 0x%08x want 0x%08x\n", x, y, got, expect); break; }
        }
    CHECK(ok_copy, "buffer->image->buffer roundtrip preserved gradient");

    uint32_t cleared = 1;
    for (uint32_t i = 0; i < W * H && cleared; i++) {
        uint32_t got; memcpy(&got, (uint8_t *)mapC + i * 4, 4);
        if (got != 0x3f800000u) { cleared = 0; printf("  clear mismatch at %u: got 0x%08x\n", i, got); break; }
    }
    CHECK(cleared, "vkCmdClearColorImage filled layer1 with 0x3f800000");

    uint32_t d0, d7x3;
    memcpy(&d0, (uint8_t *)mapD + 0, 4);
    memcpy(&d7x3, (uint8_t *)mapD + (3 * 8 + 7) * 4, 4);
    /* nearest blit from (0,0) and (56,24) of the gradient:
     * gradient(56,24) = 0xFF000000 | (56<<16) | (24<<8) | (80) = 0xFF381850 */
    CHECK(d0 == 0xFF000000u, "blit/copy pixel(0,0) == gradient(0,0) (got 0x%08x)", d0);
    CHECK(d7x3 == 0xFF381850u, "blit/copy pixel(7,3) == gradient(56,24) (got 0x%08x)", d7x3);
    printf("SUCCESS: buffer<->image copy, clear, blit and image->image copy verified\n");

    pfn_vkUnmapMemory(device, memB);
    pfn_vkUnmapMemory(device, memC);
    pfn_vkUnmapMemory(device, memD);

    /* ---- Submit through the loader path ---- */
    struct VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VkResult submit = pfn_vkQueueSubmit(queue, 1, &submitInfo, NULL);
    CHECK(submit == VK_SUCCESS, "vkQueueSubmit success (got %d)", (int)submit);

    /* ---- Cleanup ---- */
    pfn_vkFreeMemory(device, memA, NULL);
    pfn_vkFreeMemory(device, memI, NULL);
    pfn_vkFreeMemory(device, memB, NULL);
    pfn_vkFreeMemory(device, memC, NULL);
    pfn_vkFreeMemory(device, memD, NULL);
    pfn_vkFreeMemory(device, memI2, NULL);
    pfn_vkFreeMemory(device, memI3, NULL);
    pfn_vkDestroyBuffer(device, bufA, NULL);
    pfn_vkDestroyBuffer(device, bufB, NULL);
    pfn_vkDestroyBuffer(device, bufC, NULL);
    pfn_vkDestroyBuffer(device, bufD, NULL);
    pfn_vkDestroyImage(device, img1, NULL);
    pfn_vkDestroyImage(device, img2, NULL);
    pfn_vkDestroyImage(device, img3, NULL);

    if (fails == 0)
        printf("=== Loader + Image/Memory Support PASSED CLEANLY! ===\n");
    else
        printf("=== Loader + Image/Memory Support FAILED (%d checks) ===\n", fails);

    dlclose(handle);
    return fails == 0 ? 0 : 1;
}

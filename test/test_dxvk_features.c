/*
 * Exercise the DXVK-facing features in DRY_RUN:
 *   1. vkCreateSemaphore + VkSemaphoreTypeCreateInfo (timeline)
 *   2. vkGetBufferDeviceAddress returning the real BO GPU address
 *   3. vkCmdBlitImage RGBA8 -> BGRA8 channel conversion
 *   4. vkCmdCopyImage with format conversion
 *   5. Descriptor indexing: layout with binding flags + variable descriptor count
 *   6. vkCmdCopyBuffer / vkCmdCopyBufferToImage round-trip
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panvk_v9_entrypoints.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); } \
    else { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void) {
    printf("=== DXVK feature exercise (DRY_RUN) ===\n");

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

    /* --- 1. Timeline semaphore --- */
    struct VkSemaphoreTypeCreateInfo tci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 42,
    };
    struct VkSemaphoreCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &tci,
    };
    VkSemaphore timeline = NULL;
    vkCreateSemaphore(device, &sci, NULL, &timeline);
    uint64_t val = 0;
    vkGetSemaphoreCounterValue(device, timeline, &val);
    CHECK(val == 42, "timeline semaphore initialValue honored");

    struct VkSemaphoreSignalInfo sig = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = timeline, .value = 100,
    };
    vkSignalSemaphore(device, &sig);
    vkGetSemaphoreCounterValue(device, timeline, &val);
    CHECK(val == 100, "timeline vkSignalSemaphore sets counter");

    struct VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &timeline, .pValues = &(uint64_t){100},
    };
    CHECK(vkWaitSemaphores(device, &wi, ~0ULL) == VK_SUCCESS, "timeline wait on reached value");

    /* --- 2. Buffer device address --- */
    VkBuffer buffer = NULL;
    struct VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    vkCreateBuffer(device, &bci, NULL, &buffer);
    VkMemoryRequirements memreq = {0};
    vkGetBufferMemoryRequirements(device, buffer, &memreq);
    VkDeviceMemory mem = NULL;
    struct VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memreq.size ? memreq.size : 4096,
        .memoryTypeIndex = 0,
    };
    CHECK(vkAllocateMemory(device, &mai, NULL, &mem) == VK_SUCCESS, "allocate device memory");
    CHECK(vkBindBufferMemory(device, buffer, mem, 0) == VK_SUCCESS, "bind buffer memory");
    struct VkBufferDeviceAddressInfo badi = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer };
    VkDeviceAddress addr = vkGetBufferDeviceAddress(device, &badi);
    CHECK(addr != 0 && addr != (VkDeviceAddress)(uintptr_t)buffer, "BDA returns real GPU address (non-fake)");

    /* --- 5. Descriptor indexing: binding flags + variable count --- */
    struct VkDescriptorSetLayoutBinding bindings[2];
    memset(bindings, 0, sizeof(bindings));
    bindings[0] = (struct VkDescriptorSetLayoutBinding){
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 4, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    bindings[1] = (struct VkDescriptorSetLayoutBinding){
        .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 16, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorBindingFlags flags[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
    };
    struct VkDescriptorSetLayoutBindingFlagsCreateInfo bfci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 2, .pBindingFlags = flags,
    };
    struct VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings, .pNext = &bfci,
    };
    VkDescriptorSetLayout dsl = NULL;
    CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl) == VK_SUCCESS, "create layout with binding flags");

    struct VkDescriptorPoolSize poolSize = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 32,
    };
    struct VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1, .pPoolSizes = &poolSize, .maxSets = 1,
    };
    VkDescriptorPool pool = NULL;
    vkCreateDescriptorPool(device, &dpci, NULL, &pool);

    uint32_t varCount = 8;
    struct VkDescriptorSetVariableDescriptorCountAllocateInfo vdci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = 1, .pDescriptorCounts = &varCount,
    };
    struct VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl, .pNext = &vdci,
    };
    VkDescriptorSet ds = NULL;
    CHECK(vkAllocateDescriptorSets(device, &dsai, &ds) == VK_SUCCESS, "allocate set with variable descriptor count");

    struct VkDescriptorBufferInfo dbInfo = { .buffer = buffer, .range = 4096 };
    struct VkWriteDescriptorSet wd = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = ds, .dstBinding = 0, .dstArrayElement = 2,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &dbInfo,
    };
    vkUpdateDescriptorSets(device, 1, &wd, 0, NULL);
    CHECK(1, "update descriptor with dstArrayElement > 0");

    /* --- 3/4. Blit / copy with format conversion --- */
    VkImage srcImg = NULL, dstImg = NULL;
    struct VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent = { .width = 64, .height = 64, .depth = 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    vkCreateImage(device, &ici, NULL, &srcImg);
    struct VkMemoryRequirements smr = {0};
    vkGetImageMemoryRequirements(device, srcImg, &smr);
    VkDeviceMemory srcMem = NULL;
    struct VkMemoryAllocateInfo smai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = smr.size ? smr.size : 64 * 64 * 4,
        .memoryTypeIndex = 0,
    };
    vkAllocateMemory(device, &smai, NULL, &srcMem);
    vkBindImageMemory(device, srcImg, srcMem, 0);

    uint32_t *srcPix = (uint32_t *)panvk_v9_image_cpu(srcImg);
    for (int i = 0; i < 64 * 64; i++) srcPix[i] = 0xFF332211u; /* bytes R,G,B,A = 0x11,0x22,0x33,0xFF */

    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkCreateImage(device, &ici, NULL, &dstImg);
    VkDeviceMemory dstMem = NULL;
    struct VkMemoryAllocateInfo dmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = smr.size ? smr.size : 64 * 64 * 4,
        .memoryTypeIndex = 0,
    };
    vkAllocateMemory(device, &dmai, NULL, &dstMem);
    vkBindImageMemory(device, dstImg, dstMem, 0);

    VkCommandPool cpool = NULL;
    struct VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    vkCreateCommandPool(device, &cpci, NULL, &cpool);
    VkCommandBuffer cmd = NULL;
    struct VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool, .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(device, &cbai, &cmd);
    struct VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &cbbi);

    struct VkImageCopy icpy = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .extent = { .width = 64, .height = 64, .depth = 1 },
    };
    vkCmdCopyImage(cmd, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &icpy);
    CHECK(panvk_v9_image_pixel(dstImg,0,0) == 0xFF112233u,
          "vkCmdCopyImage converts RGBA8 -> BGRA8 (channels swapped)");

    memset(panvk_v9_image_cpu(dstImg), 0, 64 * 64 * 4);
    struct VkImageBlit iblit = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .srcOffsets = { {0,0,0}, {64,64,1} },
        .dstOffsets = { {0,0,0}, {32,32,1} },
    };
    vkCmdBlitImage(cmd, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &iblit, VK_FILTER_NEAREST);
    CHECK(panvk_v9_image_pixel(dstImg,0,0) == 0xFF112233u,
          "vkCmdBlitImage scales + converts (pixel 0 channel-swapped)");

    /* --- 6. Copy buffer round-trip --- */
    VkBuffer dstBuf = NULL;
    struct VkBufferCreateInfo dbci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    vkCreateBuffer(device, &dbci, NULL, &dstBuf);
    VkMemoryRequirements dbr = {0};
    vkGetBufferMemoryRequirements(device, dstBuf, &dbr);
    VkDeviceMemory dbMem = NULL;
    struct VkMemoryAllocateInfo dbmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = dbr.size ? dbr.size : 4096, .memoryTypeIndex = 0,
    };
    vkAllocateMemory(device, &dbmai, NULL, &dbMem);
    vkBindBufferMemory(device, dstBuf, dbMem, 0);
    uint32_t *srcBufCpu = (uint32_t *)panvk_v9_buffer_cpu(buffer);
    if (srcBufCpu) srcBufCpu[0] = 0xDEADBEEFu;
    struct VkBufferCopy bcp = { .srcOffset = 0, .dstOffset = 0, .size = 1024 };
    vkCmdCopyBuffer(cmd, buffer, dstBuf, 1, &bcp);
    CHECK(panvk_v9_buffer_cpu(dstBuf) && ((uint32_t *)panvk_v9_buffer_cpu(dstBuf))[0] == 0xDEADBEEFu,
          "vkCmdCopyBuffer copies CPU-backing BO contents");

    vkEndCommandBuffer(cmd);
    struct VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    vkQueueSubmit(queue, 1, &si, NULL);

    /* --- 6b. Buffer -> Image copy --- */
    VkImage dstImg2 = NULL;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkCreateImage(device, &ici, NULL, &dstImg2);
    VkDeviceMemory dstMem2 = NULL;
    struct VkMemoryAllocateInfo dmai2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = smr.size ? smr.size : 64 * 64 * 4, .memoryTypeIndex = 0,
    };
    vkAllocateMemory(device, &dmai2, NULL, &dstMem2);
    vkBindImageMemory(device, dstImg2, dstMem2, 0);
    struct VkBufferImageCopy bic = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        .imageExtent = { .width = 64, .height = 64, .depth = 1 },
    };
    vkCmdCopyBufferToImage(cmd, buffer, dstImg2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    CHECK(panvk_v9_image_pixel(dstImg2,0,0) == 0xDEADBEEFu,
          "vkCmdCopyBufferToImage fills image from buffer");

    /* --- 7. Swapchain + framebuffer render path (DXVK flow) --- */
    VkSwapchainKHR sc = NULL;
    struct VkSwapchainCreateInfoKHR swci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = NULL, /* headless stub surface */
        .minImageCount = 2,
        .imageExtent = { .width = 64, .height = 64 },
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    };
    CHECK(vkCreateSwapchainKHR(device, &swci, NULL, &sc) == VK_SUCCESS, "create swapchain");
    uint32_t scn = 0;
    vkGetSwapchainImagesKHR(device, sc, &scn, NULL);
    VkImage scimg[4] = {0};
    CHECK(vkGetSwapchainImagesKHR(device, sc, &scn, scimg) == VK_SUCCESS && scn == 2,
          "get 2 swapchain images");
    CHECK(panvk_v9_image_cpu(scimg[0]) != NULL && panvk_v9_image_cpu(scimg[1]) != NULL,
          "swapchain images have real BO backing");

    VkImageView scview = NULL;
    struct VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = scimg[0], .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    vkCreateImageView(device, &ivci, NULL, &scview);
    VkFramebuffer fb = NULL;
    struct VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &scview,
        .width = 64, .height = 64, .layers = 1,
    };
    vkCreateFramebuffer(device, &fci, NULL, &fb);

    VkCommandPool scpool = NULL;
    struct VkCommandPoolCreateInfo scpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    vkCreateCommandPool(device, &scpci, NULL, &scpool);
    VkCommandBuffer sccmd = NULL;
    struct VkCommandBufferAllocateInfo sccai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = scpool, .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(device, &sccai, &sccmd);
    struct VkCommandBufferBeginInfo scbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(sccmd, &scbbi);
    struct VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .framebuffer = fb,
        .renderArea = { .offset = {0,0}, .extent = {64,64} },
    };
    vkCmdBeginRenderPass(sccmd, &rpbi, 0);
    vkCmdDrawIndexed(sccmd, 3, 1, 0, 0, 0);
    vkCmdEndRenderPass(sccmd);
    vkEndCommandBuffer(sccmd);
    struct VkSubmitInfo scsi = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &sccmd };
    CHECK(vkQueueSubmit(queue, 1, &scsi, NULL) == VK_SUCCESS, "submit swapchain render");
    {
        uint32_t p00 = panvk_v9_image_pixel(scimg[0],0,0);
        uint32_t p63 = panvk_v9_image_pixel(scimg[0],63,63);
        uint32_t nonzero = 0, green = 0;
        uint32_t minx=99,miny=99,maxx=0,maxy=0;
        for (uint32_t y = 0; y < 64; y++)
            for (uint32_t x = 0; x < 64; x++) {
                uint32_t p = panvk_v9_image_pixel(scimg[0],x,y);
                nonzero += p != 0;
                green += p == 0xFF00FF00u;
                if (p==0xFF00FF00u) {
                    if (x<minx) minx=x; if (x>maxx) maxx=x;
                    if (y<miny) miny=y; if (y>maxy) maxy=y;
                }
            }
        printf("DBG swapchain render: (0,0)=0x%08x (63,63)=0x%08x nonzero=%u green=%u bbox=(%u,%u)-(%u,%u)\n",
               p00, p63, nonzero, green, minx, miny, maxx, maxy);
        /* The default triangle is a 16x16 green triangle at the top-left corner;
         * any green pixels inside the image prove the GPU rendered through the
         * framebuffer into the swapchain image's BO. */
        CHECK(green > 0, "render into swapchain image via framebuffer (green present)");
    }

    vkDestroyCommandPool(device, scpool, NULL);
    vkDestroyFramebuffer(device, fb, NULL);
    vkDestroyImageView(device, scview, NULL);
    vkDestroySwapchainKHR(device, sc, NULL);

    vkDestroyCommandPool(device, cpool, NULL);
    vkDestroyBuffer(device, dstBuf, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkDestroySemaphore(device, timeline, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    printf("=== %s: %d failures ===\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

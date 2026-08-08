#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdint.h>
typedef void* (*PFN)(void*, const char*);
int main(void) {
    void *h = dlopen("./libvulkan_panvk_v9.so", RTLD_NOW);
    if (!h) { printf("dlopen FAIL: %s\n", dlerror()); return 2; }
    PFN ipa = (PFN)dlsym(h, "vk_icdGetInstanceProcAddr");
    const char *names[] = {
        "vkCreateRenderPass2", "vkCreateRenderPass2KHR",
        "vkCmdPushConstants", "vkCmdSetDepthBias",
        "vkCreateEvent", "vkDestroyEvent", "vkGetEventStatus", "vkSetEvent", "vkResetEvent",
        "vkCmdSetEvent", "vkCmdResetEvent", "vkCmdWaitEvents",
        "vkCreateQueryPool", "vkDestroyQueryPool", "vkCmdBeginQuery", "vkCmdEndQuery",
        "vkCmdWriteTimestamp", "vkCmdResetQueryPool",
        "vkCmdDispatch", "vkCmdDispatchIndirect",
        "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges",
        "vkGetDeviceQueue2", "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR",
        "vkQueueSubmit2", "vkQueueSubmit2KHR", "vkCmdExecuteCommands",
        NULL
    };
    int ok = 0, missing = 0;
    for (int i = 0; names[i]; i++) {
        void *p = ipa(NULL, names[i]);
        if (p) ok++; else { missing++; printf("MISSING: %s\n", names[i]); }
    }
    printf("resolved %d/%d new entry points (%d missing)\n", ok, ok+missing, missing);
    dlclose(h);
    return missing ? 1 : 0;
}

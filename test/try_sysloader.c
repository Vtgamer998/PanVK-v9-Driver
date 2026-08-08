#include <stdio.h>
#include <dlfcn.h>
int main(void){
    void* h=dlopen("/system/lib64/libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    if(!h){printf("dlopen sys loader: %s\n",dlerror());return 1;}
    void* p=dlsym(h,"vkGetInstanceProcAddr");
    printf("sys loader ok, vkGetInstanceProcAddr=%p\n",p);
    return 0;
}

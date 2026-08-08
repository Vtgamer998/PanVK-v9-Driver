#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

int main() {
    void *h1 = dlopen("/system/lib64/libEGL.so", RTLD_LAZY | RTLD_LOCAL);
    if (h1) {
        printf("OK: libEGL.so loaded via Termux linker\n");
        void *eglGetProc = dlsym(h1, "eglGetProcAddress");
        if (eglGetProc) printf("OK: eglGetProcAddress found\n");
        dlclose(h1);
    } else {
        printf("FAIL: libEGL.so - %s\n", dlerror());
    }

    void *h2 = dlopen("/system/lib64/libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
    if (h2) {
        printf("OK: libGLESv2.so loaded via Termux linker\n");
        void *glClear = dlsym(h2, "glClearColor");
        if (glClear) printf("OK: glClearColor found\n");
        dlclose(h2);
    } else {
        printf("FAIL: libGLESv2.so - %s\n", dlerror());
    }

    void *h3 = dlopen("/vendor/lib64/egl/libGLES_mali.so", RTLD_LAZY | RTLD_LOCAL);
    if (h3) {
        printf("OK: libGLES_mali.so loaded via Termux linker!\n");
        void *eglGet = dlsym(h3, "eglGetProcAddress");
        if (eglGet) printf("OK: eglGetProcAddress found in Mali blob\n");
        dlclose(h3);
    } else {
        printf("FAIL: libGLES_mali.so - %s\n", dlerror());
    }
    return 0;
}
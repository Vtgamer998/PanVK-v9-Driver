#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#include "pan_kmod_kbase.h"
#include "v9_cmd_stream.h"
#include "panvk_v9_compiler.h"

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = n;
    return buf;
}

int main(int argc, char **argv) {
    const char *libpath = argc > 1 ? argv[1] : "/data/data/com.termux/files/home/libpanvk_v9_compiler.so";
    const char *fspath = argc > 2 ? argv[2] : "/data/data/com.termux/files/usr/tmp/opencode/fs.spv";

    printf("=== FS-on-GPU isolation test ===\n");

    void *lib = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    int (*compile)(const uint32_t *, size_t, enum panvk_v9_shader_stage,
                   const char *, const struct panvk_v9_pipeline_layout *,
                   struct panvk_v9_compiled_shader *, char *, size_t) =
        (void *)dlsym(lib, "panvk_v9_compile_spirv");
    void (*cleanup)(struct panvk_v9_compiled_shader *) =
        (void *)dlsym(lib, "panvk_v9_compiled_shader_cleanup");
    if (!compile || !cleanup) { fprintf(stderr, "dlsym failed\n"); return 1; }

    size_t spv_size = 0;
    uint8_t *spv = read_file(fspath, &spv_size);
    if (!spv) { fprintf(stderr, "cannot read %s\n", fspath); return 1; }
    char error[1024] = {0};
    struct panvk_v9_compiled_shader fs = {0};
    int rc = compile((const uint32_t *)spv, spv_size, PANVK_V9_SHADER_FRAGMENT,
                     "main", NULL, &fs, error, sizeof(error));
    free(spv);
    if (rc != 0) { fprintf(stderr, "compile failed: %s\n", error); return 1; }
    printf("FS compiled: %zu bytes work_reg=%u preload=0x%llx barrier=%d\n",
           fs.binary_size, fs.work_reg_count, (unsigned long long)fs.preload,
           fs.contains_barrier);

    const char *tr = getenv("PANVK_FS_TRUNCATE");
    if (tr) {
        size_t want = strtoul(tr, NULL, 0);
        if (want < fs.binary_size) fs.binary_size = want;
        printf("TRUNCATED to %zu bytes\n", fs.binary_size);
    }

    struct pan_kmod_dev *dev = pan_kmod_dev_create(NULL);
    if (!dev) { fprintf(stderr, "no dev\n"); return 1; }

    struct v9_render_target_config config = { .width = 32, .height = 32, .clear_color = 0xFF0000FF };
    struct v9_cmd_buffer *cmd = v9_cmd_buffer_create(dev, &config);
    if (!cmd) { fprintf(stderr, "no cmd\n"); return 1; }

    int sret = v9_cmd_buffer_set_fragment_shader(cmd, &fs);
    printf("set_fragment_shader rc=%d\n", sret);

    v9_cmd_buffer_begin(cmd);
    v9_cmd_draw_indexed_triangle(cmd);
    v9_cmd_buffer_end(cmd);

    int ret = v9_cmd_buffer_submit(cmd);
    printf("submit rc=%d\n", ret);

    uint32_t px = v9_cmd_buffer_read_pixel(cmd, 16, 16);
    printf("pixel(16,16)=0x%08x\n", px);
    int green = 0;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            if (v9_cmd_buffer_read_pixel(cmd, x, y) == 0xFF00FF00) green++;
    printf("green pixels: %d\n", green);

    cleanup(&fs);
    v9_cmd_buffer_destroy(cmd);
    pan_kmod_dev_destroy(dev);
    dlclose(lib);
    printf(ret == 0 && green > 0 ? "PASS\n" : "FAIL\n");
    return (ret == 0 && green > 0) ? 0 : 1;
}

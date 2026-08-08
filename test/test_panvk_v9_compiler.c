#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

static int compile_one(void *lib, int (*compile)(const uint32_t *, size_t,
                     enum panvk_v9_shader_stage, const char *,
                     const struct panvk_v9_pipeline_layout *,
                     struct panvk_v9_compiled_shader *, char *, size_t),
                     void (*cleanup)(struct panvk_v9_compiled_shader *),
                     const char *path, enum panvk_v9_shader_stage stage) {
    size_t spv_size = 0;
    uint8_t *spv = read_file(path, &spv_size);
    if (!spv) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    const char *name = stage == PANVK_V9_SHADER_VERTEX ? "VS" :
                       stage == PANVK_V9_SHADER_FRAGMENT ? "FS" : "CS";
    printf("%s: %zu bytes\n", name, spv_size);

    struct panvk_v9_descriptor_binding cs_bindings[1] = {
        { .set = 0, .binding = 0, .descriptor_type = 6, .array_size = 1,
          .resource_index = 0 },
    };
    struct panvk_v9_pipeline_layout cs_layout = {
        .bindings = cs_bindings, .binding_count = 1, .ubo_count = 1,
    };

    char error[1024] = {0};
    struct panvk_v9_compiled_shader shader = {0};
    int rc = compile((const uint32_t *)spv, spv_size, stage, "main",
                     stage == PANVK_V9_SHADER_COMPUTE ? &cs_layout : NULL,
                     &shader, error, sizeof(error));
    printf("  compile rc=%d binary_size=%zu work_reg=%u tls=%u preload=0x%llx\n",
           rc, shader.binary_size, shader.work_reg_count, shader.tls_size,
           (unsigned long long)shader.preload);
    if (rc != 0) {
        printf("  error: %s\n", error);
        free(spv);
        return 1;
    }
    if (!shader.binary || shader.binary_size < 4) {
        printf("  produced empty binary\n");
        free(spv);
        return 1;
    }
    if (stage == PANVK_V9_SHADER_VERTEX) {
        printf("  idvs=%d no_psiz_offset=%u secondary_enable=%d\n",
               shader.idvs, shader.no_psiz_offset, shader.secondary_enable);
    } else if (stage == PANVK_V9_SHADER_FRAGMENT) {
        printf("  outputs_written=0x%llx writes_depth=%d can_discard=%d\n",
               (unsigned long long)shader.outputs_written, shader.writes_depth,
               shader.can_discard);
    } else {
        printf("  local_size=%ux%ux%u wls=%u tls=%u\n",
               shader.local_size_x, shader.local_size_y, shader.local_size_z,
               shader.wls_size, shader.tls_size);
    }
    FILE *dmp = fopen(stage == PANVK_V9_SHADER_VERTEX ? "/data/data/com.termux/files/usr/tmp/opencode/vs.bin" : stage == PANVK_V9_SHADER_FRAGMENT ? "/data/data/com.termux/files/usr/tmp/opencode/fs.bin" : "/data/data/com.termux/files/usr/tmp/opencode/cs.bin", "wb"); if (dmp) { fwrite(shader.binary, 1, shader.binary_size, dmp); fclose(dmp); }
    const uint32_t *words = (const uint32_t *)shader.binary;
    size_t nwords = shader.binary_size / 4;
    printf("  first %zu dwords:\n", nwords < 8 ? nwords : 8);
    for (size_t i = 0; i < nwords && i < 8; i++)
        printf("    0x%08x\n", words[i]);
    cleanup(&shader);
    free(spv);
    return 0;
}

int main(int argc, char **argv) {
    const char *libpath = argc > 1 ? argv[1] : "libpanvk_v9_compiler.so";
    const char *vspath = argc > 2 ? argv[2] : "vs.spv";
    const char *fspath = argc > 3 ? argv[3] : "fs.spv";
    const char *cspath = argc > 4 ? argv[4] : "cs.spv";

    void *lib = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", libpath, dlerror());
        return 1;
    }
    int (*compile)(const uint32_t *, size_t, enum panvk_v9_shader_stage,
                   const char *, const struct panvk_v9_pipeline_layout *,
                   struct panvk_v9_compiled_shader *, char *, size_t) =
        (int (*)(const uint32_t *, size_t, enum panvk_v9_shader_stage,
                 const char *, const struct panvk_v9_pipeline_layout *,
                 struct panvk_v9_compiled_shader *, char *, size_t))(void *)dlsym(lib, "panvk_v9_compile_spirv");
    void (*cleanup)(struct panvk_v9_compiled_shader *) =
        (void (*)(struct panvk_v9_compiled_shader *))(void *)dlsym(lib, "panvk_v9_compiled_shader_cleanup");
    if (!compile || !cleanup) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 1;
    }

    int failures = 0;
    failures += compile_one(lib, compile, cleanup, vspath, PANVK_V9_SHADER_VERTEX);
    failures += compile_one(lib, compile, cleanup, fspath, PANVK_V9_SHADER_FRAGMENT);
    failures += compile_one(lib, compile, cleanup, cspath, PANVK_V9_SHADER_COMPUTE);

    dlclose(lib);
    if (failures) {
        printf("\nFAILED with %d failure(s)\n", failures);
        return 1;
    }
    printf("\nPASSED CLEANLY!\n");
    return 0;
}

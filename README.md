# PanVK-v9 Driver — Vulkan ICD for Mali-G68 MC4

Experimental Vulkan driver for **ARM Mali-G68 MC4 (MediaTek MT6893, Valhall v9)**.

---

## IMPORTANT NOTICE

### Disclaimer

**THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.**

The author **IS NOT RESPONSIBLE** for any damages arising from the use of this software, including hardware damage, data loss, system instability, or warranty void.

**YOU ASSUME ALL RESPONSIBILITY FOR USING THIS SOFTWARE.**

---

## About

An experimental Vulkan ICD (Installable Client Driver) for ARM Mali GPUs, similar to Turnip for Qualcomm Adreno, but for Valhall v9 architecture. Designed for use inside Winlator containers on Android.

### Capabilities

- Vulkan 1.1 entry points (~649 exported functions)
- SPIR-V to Valhall v9 compiler (Mesa 26.2 wrapper)
- Texture/sampler support with descriptor set binding
- Command buffer wiring for draw/dispatch
- Swapchain via XCB (X11)

### Limitations

- **NOT a complete driver** — many extensions return VK_ERROR_FEATURE_NOT_PRESENT
- **Fragility** — may cause GPU JOB_READ_FAULT
- **Requires Winlator** — tested on Winlator 11.1

---

## IMPORTANT: glibc Build Required

**The ICD MUST be built for GNU/Linux (glibc), NOT Android (bionic).**

The Winlator container runs Ubuntu Linux with glibc. A bionic-linked `.so` will NEVER load in a glibc process — the dynamic linker cannot resolve `libc.so` (bionic) vs `libc.so.6` (glibc).

### Toolchain

Uses `zig cc` as cross-compiler:

```bash
pkg install zig
```

Target: `aarch64-linux-gnu.2.35` (Ubuntu 22.04 compatible)

---

## Project Structure

```
PanVK-v9-Driver/
├── src/
│   ├── panvk_v9_entrypoints.c    # Main Vulkan entrypoints (7000+ lines)
│   ├── panvk_v9_entrypoints.h    # Entrypoint declarations
│   ├── v9_cmd_stream.c           # Command buffer / job chain
│   ├── v9_cmd_stream.h           # Command buffer structs
│   ├── v9_pack.h                 # TSD/MSD packing for Mali-G68
│   ├── pan_kmod_kbase.c          # Kernel driver interface
│   ├── kbase_winsys.c            # /dev/mali0 ioctls
│   ├── kbase_slot_unwedge.c      # GPU wedge safety
│   ├── panvk_v9_x11.c            # XCB/X11 window system
│   ├── panvk_v9_compiler_mesa.c  # Mesa SPIR-V compiler wrapper
│   ├── vk_missing_stubs.c        # Stubs for ~286 unimplemented extensions
│   ├── vk_stubs_decl.h           # Stub declarations
│   └── errno_stub.c              # glibc errno compatibility
├── build/
│   ├── build.sh                  # Main build script (calls build_glibc.sh)
│   ├── build_glibc.sh            # glibc cross-compilation via zig
│   ├── repack_apk.sh             # Full rebuild + APK repack pipeline
│   └── libvulkan_panvk_v9.so     # Pre-built glibc ICD (910KB)
├── json/
│   └── panvk_v9_icd.aarch64.json # ICD manifest
├── test/                         # Vulkan API tests
├── scripts/                      # Helper scripts
├── docs/                         # Documentation
└── winlator_package/             # Winlator integration files
```

---

## Build Instructions

### Prerequisites

```bash
pkg install zig apktool android-tools
```

### Quick Build

```bash
cd build/
bash build_glibc.sh libvulkan_panvk_v9.so
```

### Full APK Repack

```bash
cd build/
bash repack_apk.sh fixed53
```

This will:
1. Compile ICD with `zig cc -target aarch64-linux-gnu.2.35`
2. Package into `panvk-1.0.tzst` with X11 runtime libs
3. Repack Winlator APK with `apktool`
4. Sign with `apksigner`

---

## ICD JSON Configuration

```json
{
    "file_format_version": "1.0.1",
    "ICD": {
        "api_version": "1.3.239",
        "library_arch": "64",
        "library_path": "/data/data/com.winlator/files/rootfs/usr/lib/libvulkan_panvk_v9.so"
    }
}
```

---

## Runtime Dependencies

The ICD requires these glibc libraries (bundled in tzst):

| Library | Source | Purpose |
|---|---|---|
| `libc.so.6` | Ubuntu rootfs | Standard C library |
| `libX11.so.6` | Debian arm64 | X11 window system |
| `libxcb.so.1` | Debian arm64 | XCB protocol |
| `libXau.so.6` | Debian arm64 | X authorization |
| `libXdmcp.so.6` | Debian arm64 | Display manager |

---

## Technical Details

### Vulkan Loader Interface

- ICD Interface Version: 4
- Exports: `vk_icdGetInstanceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion`
- 649 `T` symbols exported (all core Vulkan + extensions)

### Kernel Interface

- Uses `/dev/mali0` via kbase ioctls
- GPU: ARM Mali-G68 MC4 (Valhall v9)
- Memory: coherenct GT-table, 4KB pages

### Compiler

- Mesa 26.2 SPIR-V to Valhall v9
- Loaded via `dlopen` at runtime
- Exported: `panvk_v9_compile_spirv`, `panvk_v9_compiled_shader_cleanup`

---

## Environment Variables

| Variable | Description |
|---|---|
| `PANVK_DRY_RUN=1` | Safe mode (CPU only) |
| `PANVK_V9_LOG=/path/to/log` | Custom log file path |
| `PANVK_V9_COMPILER_LIBRARY=/path/to.so` | Custom compiler library path |

---

## Credits

Based on reverse engineering work by **[VectorJet/Mali-G77-MC9](https://github.com/VectorJet/Mali-G77-MC9)**.

### Technologies

- Mesa 26.2 (Panfrost compiler)
- Vulkan 1.1 API
- Linux kbase kernel interface
- Zig cross-compiler (glibc target)

---

## License

Experimental study project. Use at your own risk.

---

**REMEMBER: YOU ASSUME ALL RESPONSIBILITY FOR USING THIS SOFTWARE.**

# PanVK-v9 — Vulkan Driver for Mali-G68 MC4 (Kimchi/AdrenoTools format)

Package in the same format as Turnip-Kimchi drivers (`lib*.so` + `meta.json`),
but for **ARM Mali-G68 MC4 GPU (MediaTek MT6893, Valhall v9)**.

## ⚠️ READ FIRST — Why it DOESN'T work on AdrenoTools/Winlator-Turnip

| Item | Reality |
|---|---|
| Turnip / Kimchi / AdrenoTools | **Exclusive to Qualcomm Adreno GPUs**. |
| Your GPU | **ARM Mali-G68 MC4** — not Adreno. |
| AdrenoTools | Injects driver via Qualcomm kernel **KGSL** interface. Mali **has no KGSL** → driver **won't load**. |
| Winlator on Mali | Uses system Mali Vulkan driver + DXVK/VKD3D, or **VirGL** (software). Doesn't use Turnip. |
| This driver | Proof of concept: renders solid triangle via complete Vulkan pipeline on real GPU, using **Mesa 26.2 SPIR-V→Valhall compiler**. **Not** a complete Vulkan implementation — **doesn't run games** (DXVK/VKD3D). |

**Summary**: to run Winlator with Turnip, you need a **Snapdragon/Adreno** device. On this Mali device, the viable path is the system Mali Vulkan driver; this project is a study/experimental ICD.

## Package Contents

```
libvulkan_panvk_v9.so   → Vulkan ICD (Mali-G68 MC4, Valhall v9)
libpanvk_v9_compiler.so → SPIR-V→Valhall v9 compiler (Mesa 26.2 backend)
vulkan.panvk_v9.json    → ICD manifest (Vulkan Loader)
meta.json               → AdrenoTools format metadata
version.txt             → version
INSTALL_WINLATOR.sh     → manual Winlator installation guide
vulkan.panvk_v9.container.json → container rootfs injection manifest
dense_map               → direct render (ret=0, 256/256 green)
test_vulkan_loader_icd  → complete Vulkan pipeline via dlopen
test_loader_images      → memory/image/copy/clear/blit via loader (+ .c)
```

## How to install MANUALLY (for apps using Vulkan Loader)

Android's Vulkan Loader looks for ICDs in `/vendor/etc/vulkan/` and
`/data/adb/vulkan/`. **Without root**, use `VK_ICD_FILENAMES`:

```bash
export VK_ICD_FILENAMES=/path/to/vulkan.panvk_v9.json
export LD_LIBRARY_PATH=/path/to:$LD_LIBRARY_PATH
```

In Termux (where loader finds .json in same folder):

```bash
cd PanVK-v9-Kimchi
VK_ICD_FILENAMES=./vulkan.panvk_v9.json LD_LIBRARY_PATH=. ./test_vulkan
```

## Quick verification (Termux, on this device)

```bash
# 1) Direct render via kbase job chain
../vulkan-driver/dense_map 16 16          # ret=0, 256/256 green

# 2) Complete Vulkan pipeline via ICD (dlopen)
../vulkan-driver/test_vulkan_loader_icd   # PASSED CLEANLY, pixel 0xff00ff00

# 3) Memory/image (panvk style): buffer↔image, clear, blit, copy
../vulkan-driver/test_loader_images       # PASSED CLEANLY
```

## SPIR-V→Valhall Compiler (Mesa 26.2)

The driver loads `libpanvk_v9_compiler.so` (**Mesa 26.2 / Panfrost
Valhall v9** backend, GPU ID `0x90001000`) which compiles arbitrary SPIR-V to Valhall
ISA on the real GPU. `test_vulkan_loader_icd` receives `vs.spv` and `fs.spv`,
compiles both in Mesa and renders **solid green (0xFF00FF00)**.

```bash
../vulkan-driver/test_vulkan_loader_icd vs.spv fs.spv   # PASSED CLEANLY!
../vulkan-driver/test_fs_bin                            # FS compiled in Mesa, on GPU
```

## Image support (panvk style)

Real linear layout following `panvk_image.c` pattern (row pitch 64B, mips/layers/depth),
`vkGetImageSubresourceLayout`, and **executable copy/clear path**:
`vkCmdCopyBufferToImage`, `vkCmdCopyImageToBuffer`, `vkCmdCopyImage`,
`vkCmdBlitImage` (NEAREST) and `vkCmdClearColorImage` — via memcpy on BO.
`VkPhysicalDeviceMemoryProperties` fixed (2 types, 4GB heap).
Image data is moved by CPU; arbitrary VS/FS shaders compile and
run on Mesa (see "SPIR-V→Valhall Compiler" section).

## Known technical limitations

1. **Multi-tile**: fragment only processes first 16x16 tile (vendor reference
   `replay_egl_triangle` has same behavior).
2. **Fragment termination**: MTK r49 kernel never emits `0x1 DONE` for
   polygon-list; always `0x4`/`0x4002` (accepted as success). This may leave
   slot in JOB_READ_FAULT between frames in same process.
3. **SELinux**: shader page mapped RW + `GPU_EX` (CPU PROT_EXEC denied).
4. **Incomplete**: no full swapchain, no Vulkan 1.1+ extensions/features
   required by DXVK/VKD3D → **doesn't run Windows games on Winlator**.
5. **GPU in residual JOB_READ_FAULT**: after 1st real frame (`PANVK_DRY_RUN=0`)
   GPU freezes until phone is rebooted. Use `PANVK_DRY_RUN=1` by default.

## Safe tests (2026-08-02)

The driver now exposes **~150 Vulkan entry points** (added in this session:
`vkCreateRenderPass2`, events, query pools, `vkCmdPushConstants`, `vkCmdSetDepthBias`,
`vkCmdDispatch`, memory flush/invalidate, sync2/vkQueueSubmit2, etc.). Validation
is done **without risk of rebooting the phone**:

```bash
./test_ep_probe                      # resolves 28/28 new entry points (0 missing)
PANVK_DRY_RUN=1 ./test_vulkan_loader_icd   # complete pipeline, no /dev/mali0, green pixel
PANVK_DRY_RUN=1 ./test_loader_images       # memory/image/copy/clear/blit
./test_panvk_v9_compiler ./libpanvk_v9_compiler.so vs.spv fs.spv   # CPU-only
```

Real GPU test (only after **phone reboot**, 1 frame):
```bash
PANVK_DRY_RUN=0 PANVK_SUBMIT_TIMEOUT_MS=1500 V9_SKIP_POST_FLUSH=1 timeout 25 ./dense_map 16 16
```

**Note on "DX game on Winlator" goal:** DXVK (DX9/11) and vkd3d-proton (DX12)
translate complete DirectX→Vulkan. The driver needs almost total API coverage and
real compute on Valhall. Mesa's PanVK rejects Mali-G68/G77 (arch 9) and only talks
to DRM pan/thor — being **unusable** on this terminal; so the path is completing
the custom ICD (which talks /dev/mali* kbase), not porting Mesa.

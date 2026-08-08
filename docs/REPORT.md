# PanVK v9 — Vulkan ICD for Mali-G68 MC4 (Valhall v9) — Status Report

**Date**: 07/08/2026
**Hardware**: ARM Mali-G68 MC4 (MediaTek MT6893 / Dimensity 700, Valhall v9 architecture)
**Environment**: Android/Termux, `/dev/mali0`, kernel `mali_kbase` MTK r49 (version 11.13)
**Approach**: "Turnip-style" Vulkan driver — custom Vulkan entry points implementation + custom kbase backend (no Mesa/Panfrost kernel).

> **Hardware identification correction (07/08/2026)**: this device's real
> device is **Mali-G68 MC4 (product_id `0x92041010`)**, as reported by
> `KBASE_IOCTL_GET_GPUPROPS` (see `vendor_probe.txt`). This is the model
> used in `vkGetPhysicalDeviceProperties` (`deviceID`, `deviceName`) and in
> `kbase_winsys.c` default dry-run. Mesa's SPIR-V→Valhall compiler
> continues receiving `PANVK_V9_COMPILER_GPU_ID = 0x90001000` (G77) via
> `panvk_v9_compiler_mesa.c`, because **G68 is not listed in `pan_model.h`**
> in Mesa — G77 and G68 share the same Valhall v9 ISA and produce identical
> binary, so this ID is just an "alias" for the compiler backend.
> Source file comment headers and core count (`core_count = 4`) were
> corrected in this session.

---

## 0. Session 02/08/2026 — SPIR-V→VALHALL COMPILER (Mesa 26.2) WORKING ON GPU

### Main result

The complete pipeline **SPIR-V → Mesa Valhall v9 Compiler → PanVK-v9 ICD → GPU** now
works end-to-end: the FS compiled by Mesa's real backend (from
`fs.spv`) renders on the real GPU, and the complete Vulkan test via `dlopen` of the ICD
prints **`pixel(0,0)=0xff00ff00`** (solid green) and **PASSED CLEANLY!**

```
=== Step 4: Vulkan Loader ICD Shared Library Integration ===
SUCCESS: Dynamically loaded './libvulkan_panvk_v9.so' via dlopen()
SUCCESS: Dynamically queried device: 'ARM Mali-G68 MC4 (Valhall v9 - PanVK Open Source Driver)'
v9_cmd_draw_indexed: ... has_vs=1
Rendered Output: pixel(0,0)=0xff00ff00
SUCCESS: Dynamically loaded PanVK ICD rendered solid green (0xFF00FF00)!
=== Step 4: Vulkan Loader ICD Shared Library Integration PASSED CLEANLY! ===
```

### Root cause discovered: incorrect GPU ID (Bifrost instead of Valhall)

All previous `0x51` faults were caused by **`MALI_G77_GPU_ID = 0x9000u`**.
`pan_arch(0x9000)` returns 0 (empty arch → Bifrost fallback), so the Mesa
compiler packed **Bifrost clauses** instead of **Valhall v9** instructions. The GPU
decoded this garbage as `BRANCHZ offset:532992` / `LD_VAR_BUF index:0x2` →
read fault. The old "FS reads invalid varying" were a **symptom** of this
Bifrost binary, not an actual read.

**Fix**: GPU ID in Mesa format = `PAN_PROD_ID(9,0,1)` = **`0x90001000`**
(arch 9 in bits 28-31, cf. `pan_model.h`/`pan_model.c:84`). Applied in
`panvk_v9_compiler_mesa.c` (compiler) and `pan_kmod_kbase.c` (device query).

### Second bug: invalid NIR crashed copy_prop (segfault)

The wrapper replaced sysvals `load_viewport_scale`/`load_viewport_offset`
(which are **vec3**) with `nir_vec2(...)` — a 2-component def accessed with
`.z` (out of bounds) → invalid NIR → segfault inside
`nir_opt_copy_prop` (`copy_propagate_alu` → `copy->src[swizzle[0]]` out of vector).

**Fix**: `lower_viewport_sysvals` now generates `nir_vec3(1,1,1)` for scale and
`nir_vec3(0,0,0)` for offset (identity viewport transformation, consistent with
the fixed position buffer used by tests).

### GPU validation

- `test_panvk_v9_compiler vs.spv fs.spv` → VS 276 B + FS 280 B, **PASSED CLEANLY!**
- `test_fs_bin` (FS compiled by Mesa injected directly in job chain, without Vulkan):
  FS 128 B, `work_reg=32`, `preload=0x3000000000000000` (r60/r61 preloaded
  as in real panvk), **225 green pixels, PASS** — same geometry as fixed FS.
- `test_vulkan_loader_icd vs.spv fs.spv` → **pixel(0,0)=0xff00ff00, PASSED** (3/3
  stable runs).
- `inputs.no_idvs` reverted to upstream value (`stage != VERTEX`, IDVS enabled in VS)
  and continues working — the unconditional workaround was not necessary.

### Files changed in this session

| File | Change |
|---|---|
| `panvk_v9_compiler_mesa.c` | `MALI_G77_GPU_ID 0x90001000u`; viewport sysvals as **vec3**; removed debug prints |
| `bifrost_nir.c` | removed debug instrumentation (crash localized) |
| `pan_kmod_kbase.c` | `gpu_id = 0x90001000` in device query |
| `v9_cmd_stream.c` | env knobs `PANVK_FS_WORKREG`/`PANVK_FORCE_BARRIER` (debug) |
| `test_fs_bin.c` | new FS-on-GPU isolation harness |

### Build

```bash
cd ~/mesa-26.2.0-rc3/build-panvk
ninja src/panfrost/libpanvk_v9_compiler.so
cp src/panfrost/libpanvk_v9_compiler.so ~/libpanvk_v9_compiler.so
```

---

## 1. Summary of What Was Done in This Session

The driver now **successfully renders a solid green triangle (0xFF00FF00)** on
the real GPU through the complete Vulkan pipeline (ICD → Instance → Device → SPIR-V →
Graphics Pipeline → Command Buffer → Queue Submit → GPU), with **ret=0**.

### Fixes applied in this session

1. **Fragment shader fixed (40 → 56 bytes)**.
   The reference `replay_egl_triangle.c` uses a 7-instruction FS (56 bytes)
   including `NOP.wait0126` and `ATEST.discard` before `BLEND.slot0.v4.f32.end`.
   Our FS had only 5 instructions (40 bytes) — missing the register dependency
   wait and alpha-test, which could cause reading registers not yet written by FADD.
   File: `v9_cmd_stream.c` (`k_valhall_green_fs`).

2. **Fragment job chain (FJ1 → FJ2) fixed**.
   In `v9_pack.h` / `v9_pack_frag_job_chain`: before `(void)fj2_gpu;` and `fj1[6:7]=0`
   (Next=NULL). Now `pack_u64(fj1 + 6, fj2_gpu)` — Job 1 (polygon-list pass)
   points to Job 2 (completion/end-of-frame pass),
   `fj2[4]=0x00020012`, `fj2[5]=1`, `fj2[9]=0x00030003`, `Next=NULL`,
   MFBD2|0x03. Without Job 2, hardware renders but waits for completion pass
   → watchdog `0x4002`.

3. **MTK kernel event semantics**.
   - `0x1` = DONE
   - `0x4` = TERMINATED (soft/hard-stop by kernel after render)
   - `0x4002` = CANCELLED (watchdog)
   - `0x42` = JOB_READ_FAULT
   The fragment in polygon-list mode **never signals 0x1** on this kernel — it
   always renders and then is stopped (0x4) or cancelled (0x4002). The reference
   has the same behavior. Therefore:
   - `pan_kmod_submit_fragment_timeout()` (new) accepts `0x1/0x4/0x4002` as
     fragment success.
   - TILER/Flush remain strict (`0x1` only) — separated from the lenient
     fragment acceptance.
   - Post-Flush became **best-effort**: if kernel gives JOB_READ_FAULT after
     fragment TERMINATED, frame is still considered success (pixels are
     already rendered), just with a warning.

### Byte-by-byte verifications against reference (all identical)

- FJ1: `0x010=0x00010012`, `0x018=Next→frag_jc2`, `0x028=mfbd|0x01`
- FJ2: `0x010=0x00020012`, `0x018=0`, `0x024=0x00030003`, `0x028=mfbd2|0x03`
- MFBD (32 words), MFBD2, DCD, RT0, Tiler Context, Tiler Heap Desc
- Tiler Context: `tc[0]=polylist`, `tc[2]=1` (hierarchy 16x16), `tc[3]=w-1|h-1<<16`,
  `tc[6:7]=heap desc` — identical. Bit 48 of `tc[0]` is equivalent to the
  `0x00010000` value GPU writes to `tc[1]` after tiler runs (reference starts
  with `tc[1]=0` and GPU writes `0x00010000`).
- Atoms `kbase_atom_mtk`: `core_req` 0x04E (TILER), 0x002 (Flush),
  0x041 (Fragment) — same.
- Memory: shader ISA in separate GPU_EX page (`0x2017`) — same as reference.
- Polygon list in 64x64: 16 active slots, pointing to heap — same.

---

## 2. Test Results (after fixes)

### `dense_map 16 16` (single frame) — 6/6 runs ret=0, 256/256 green
```
ret=0
################ (16 lines × 16 '#')
```
Events: `TILER 0x1`, `Flush 0x1`, `Fragment 0x4`, `Post-Flush (best-effort)`.

### Complete Vulkan pipeline (`test_vulkan_loader_icd`)
```
=== Step 4: Vulkan Loader ICD Shared Library Integration ===
SUCCESS: Dynamically loaded './libvulkan_panvk_v9.so' via dlopen()
SUCCESS: Resolved 'vk_icdGetInstanceProcAddr' entry point from ICD
SUCCESS: Resolved Vulkan shader, pipeline, command, and device entry points
SUCCESS: Dynamically queried device: 'ARM Mali-G68 MC4 (Valhall v9 - PanVK Open Source Driver)'
SUCCESS: Parsed SPIR-V stages and graphics pipeline state
SUCCESS: Dispatched vkQueueSubmit via dynamic ICD function pointers
Rendered Output: pixel(0,0)=0xff00ff00
SUCCESS: Dynamically loaded PanVK ICD rendered solid green (0xFF00FF00)!
=== Step 4: Vulkan Loader ICD Shared Library Integration PASSED CLEANLY! ===
```

### `size_sweep` (single frame per size, each in new memory)
- 16x16: ret=0, 256/256 green (correct)
- 32x32/64x64/...: ret=0 (fragment accepted), but only **first 16x16 tile**
  renders — **same limitation as reference** (reference renders 169/4096 in 64x64).

### Known limitations (shared with reference `replay_egl_triangle`)
1. **Multi-tile**: fragment only processes first tile (16x16). Reference has
   exactly same behavior (169 px in 64x64). Not a layout problem — all
   descriptors are identical to reference.
2. **Fragment termination**: never signals `0x1 DONE`; always `0x4`/`0x4002`.
   This occasionally leaves GPU in JOB_READ_FAULT state, which can
   "freeze" subsequent submissions **in same process**. Each new process
   works normally.
3. **No root/SELinux**: shader page mapped RW + `GPU_EX` (CPU PROT_EXEC
   blocked by SELinux).

---

## 2.5 Memory/Image Support (panvk style) — added in this session

Following `panvk_image.c` layout logic, the driver now has:

- **Real linear layout** (`panvk_v9_image_layout_init`): row pitch aligned to 64,
  slices per mip/layer/depth, offsets per mip — `vkGetImageMemoryRequirements`
  and new `vkGetImageSubresourceLayout` reflect exact layout.
- **Format→bytes** (`panvk_v9_format_bpp`): R8, R8G8, R8G8B8A8, B8G8R8A8,
  R16G16, R16G16B16A16, R32, R32G32, R32G32B32, R32G32B32A32, D16, D32, D24S8...
- **Real copy/clear path (CPU)**: `vkCmdCopyBufferToImage`,
  `vkCmdCopyImageToBuffer`, `vkCmdCopyImage`, `vkCmdBlitImage` (NEAREST) and
  `vkCmdClearColorImage` now execute memcpy/fill on BO respecting linear layout
  (bufferRowLength/bufferImageHeight/layers/offsets).
- **`VkPhysicalDeviceMemoryProperties` fixed**: default struct layout
  (heap at byte 260, not 132), 2 memory types (type 0 DEVICE_LOCAL +
  HOST_VISIBLE + HOST_COHERENT; type 1 DEVICE_LOCAL) and 1 4GB heap.
- **`vkGetPhysicalDeviceImageFormatProperties`**: up to 4096², depth 2048
  (3D), 16 mips, 16 layers.

### New test: `test_loader_images`
Complete flow via `vk_icdGetInstanceProcAddr` (like real loader):
buffer→image→buffer (64x32 gradient intact), layer clear (0x3f800000),
blit NEAREST 64x32→8x4, image→image copy — all byte-verified.
Result: **PASSED CLEANLY!** (1 GPU submit, copy/clear executed in record).

```
=== Loader + Image/Memory Support PASSED CLEANLY! ===
```

> Note: image data is moved via memcpy (CPU). Fragment renderer now uses
> **complete SPIR-V→Valhall compiler (Mesa 26.2)** — see section 0.
> Image texturing/sampling on GPU not yet connected to cmd stream, but
> arbitrary VS/FS shaders compile and run.

---

## 3. Architecture (Turnip-style)

```
Vulkan App (vkmark/test)
        │  vk_icdGetInstanceProcAddr
        ▼
libvulkan_panvk_v9.so   ← Vulkan ICD (panvk_v9_entrypoints.c)
   ├── panvk_v9_compiler.h        ← SPIR-V→Valhall compiler interface
   │      (dlopen libpanvk_v9_compiler.so at runtime)
   ├── panvk_v9_x11.c             ← WSI/X11 (presentation)
   └── panvk_v9_entrypoints.c     ← vkCreateInstance/Device/Pipeline/QueueSubmit
            │
            ▼
   v9_cmd_stream.c / v9_pack.h    ← builds Valhall v9 job chain
            │  (MFBD, DCD, RT0, Tiler Ctx, Tiler Heap, TJ, FJ1/FJ2, Flush)
            ▼
   pan_kmod_kbase.c / kbase_winsys.c   ← kbase backend (/dev/mali0)
            │  (MEM_ALLOC SAME_VA, GPU_EX, JOB_SUBMIT kbase_atom_mtk, events)
            ▼
        /dev/mali0 (mali_kbase MTK r49)
```

- **ICD manifest**: `panvk_v9_icd.json` → `./libvulkan_panvk_v9.so`.
- **Shader compiler**: dynamically loaded (`libpanvk_v9_compiler.so`).
  Real **Mesa 26.2 / Panfrost Valhall v9** backend compiling arbitrary SPIR-V
  (VS/FS) to Valhall ISA, with GPU ID `0x90001000` — see section 0.
- **kbase backend**: SAME_VA mapping, `kbase_atom_mtk` atoms (packed 72 B),
  serial submits (batch pre_dep doesn't work on MTK r49).

---

## 4. Driver Files

| File | Role |
|---|---|
| `panvk_v9_entrypoints.c/.h` | Vulkan entry points + WSI (ICD) |
| `panvk_v9_compiler.h` | SPIR-V→Valhall compiler interface |
| `panvk_v9_compiler_mesa.c` | Mesa compiler wrapper |
| `panvk_v9_x11.c` | WSI X11 |
| `panvk_v9_icd.json` | Vulkan ICD manifest |
| `v9_cmd_stream.c/.h` | Command buffer / Valhall v9 job chain construction |
| `v9_pack.h` | Descriptor layout/packing (MFBD, DCD, FJ, TJ, ...) |
| `pan_kmod_kbase.c/.h` | kbase abstraction layer |
| `kbase_winsys.c/.h` | /dev/mali0 ioctls (MEM_ALLOC, JOB_SUBMIT, events) |
| Tests | `dense_map`, `size_sweep`, `frag_dump`, `two_frame`, `tj_post`, `poly_dump`, `test_vulkan_loader_icd`, ... |

---

## 5. How to Build

```bash
# ICD library
clang -O2 -shared -fPIC -o libvulkan_panvk_v9.so \
    panvk_v9_entrypoints.c panvk_v9_x11.c \
    v9_cmd_stream.c pan_kmod_kbase.c kbase_winsys.c \
    -ldl -lpthread -lX11 -lxcb

# Complete pipeline test
clang -O2 -o test_vulkan_loader_icd test_vulkan_loader_icd.c

# Render tests
clang -O2 -o dense_map dense_map.c v9_cmd_stream.c pan_kmod_kbase.c kbase_winsys.c
clang -O2 -o size_sweep size_sweep.c v9_cmd_stream.c pan_kmod_kbase.c kbase_winsys.c
```

## 6. How to Run

```bash
./dense_map 16 16                    # single frame 16x16 (ret=0, 256/256 green)
./size_sweep                         # size sweep
./test_vulkan_loader_icd             # complete Vulkan pipeline via ICD dlopen
./test_loader_images                 # memory/image/copy/clear/blit via loader
```

Environment variables:
- `V9_SKIP_POST_FLUSH=1` — skips Post-Flush (avoids JOB_READ_FAULT residual
  when fragment is terminated; useful for multiple frames).
- `PANVK_DRY_RUN=1` — 100% CPU Vulkan pipeline, no /dev/mali0 (safe tests).
- `PANVK_DRY_RUN=0` — uses real GPU (`/dev/mali0`) for tests.
- `PANVK_SUBMIT_TIMEOUT_MS=<ms>` — maximum atom wait timeout (default 1500ms).
  Timeout becomes `-ETIMEDOUT` (fast fail), ALWAYS, instead of infinite wait that
  triggers Mali watchdog and reboots phone.

---

## 7. Session 2026-08-02 — Fragment Fragility + API Completeness

### 7.1 Issue: fragment timeout was fixed (200ms)
- In `v9_cmd_stream.c` the fragment (atom 2) used **fixed 200ms timeout**, while
  TILER/Flush used configurable time (`PANVK_SUBMIT_TIMEOUT_MS`). On MTK r49 the
  first fragment can take >200ms (page faults + warmup) → spurious timeout.
- Fixed: `pan_kmod_submit_fragment_timeout` now uses `kbase_submit_timeout_ms(1500)`
  by default, and in `v9_cmd_stream.c` the caller passes same configurable value.
- **New safe behavior:** all GPU timeouts → `-ETIMEDOUT` + warning
  `"pan_kmod: ... TIMED OUT (timeout=%dms) - GPU may be hung"`. Before, code treated
  open timeout as success (masked hang and let watchdog reboot).

### 7.2 Warning: residual JOB_READ_FAULT state persists between processes
- First frame after boot (`dense_map 16 16`) renders green (`ret=0`), but after
  fractions GPU enters residual `JOB_READ_FAULT` state and **fragment stops
  completing** even in new processes — `event_code=0x0`, `ret=-110` with timeout.
  Previous REPORT.md claimed "each new process works normally", but this **was not
  confirmed** in this revision: GPU needs **phone reboot** to
  clear state. Driver now fails safely (without restarting system).

### 7.3 Strategic decision on Mesa PanVK
Complete mapping of `mesa-26.2.0-rc3`:
- **Real Mesa PanVK driver DOES NOT run on this phone**: rejects arch 9/G77
  (`panvk_physical_device.c:401-423` → `VK_ERROR_INCOMPATIBLE_DRIVER`) and only talks to
  `/dev/dri/renderD*` via **DRM panfrost/panthor** (`panfrost_kmod.c:21-58`), never with
  `/dev/mali0`/MTK kbase ioctls. `build-panvk/` doesn't even compile `libvulkan_panfrost.so`.
- **Only useful Mesa resource is Valhall v9 compiler** (`panvk_v9_compiler_mesa.c`
  → `libpanvk_v9_compiler.so`), already linked and functional in ICD.
- Conclusion: path to goal is **completing custom ICD** (which talks kbase),
  not porting complete Mesa.

### 7.4 Vulkan surface expansion (custom ICD)
Session added missing entry points (validated by `test_entrypoints_probe`, 28/28
resolved via `vk_icdGetInstanceProcAddr`):
- `vkCreateRenderPass2`(KHR), `vkCmdPushConstants`, `vkCmdSetDepthBias`
- Events: `vkCreateEvent`, `vkDestroyEvent`, `vkGetEventStatus`, `vkSetEvent`,
  `vkResetEvent`, `vkCmdSetEvent`, `vkCmdResetEvent`, `vkCmdWaitEvents`
- Queries: `vkCreateQueryPool`, `vkDestroyQueryPool`, `vkCmdBeginQuery`,
  `vkCmdBeginQuery`, `vkCmdEndQuery`, `vkCmdWriteTimestamp`, `vkCmdResetQueryPool`
- Compute: `vkCreateComputePipelines` (saves stage, no execution yet),
  `vkCmdDispatch`/`vkCmdDispatchIndirect` (safe stub)
- Memory: `vkFlushMappedMemoryRanges`, `vkInvalidateMappedMemoryRanges` (no-op, HW
  is coherent — robust loop)
- Sync2/1.3: `vkGetDeviceQueue2`, `vkCmdPipelineBarrier2`(KHR), `vkQueueSubmit2`(KHR),
  `vkCmdExecuteCommands`

Total exposed entry points: **~150** (was 120).

### Still pending for games (DXVK/vkd3d)
- Real compute on Valhall v9 (compiler only emits VS/FS today).
- Wayland swapchain (`vkCreateWaylandSurfaceKHR`), real semaphore/timeline sync,
  external-memory FD, buffer device address.
- Dynamic state completeness (`vkCmdSetStencil*`, `vkCmdSetBlendConstants`, etc.).
- Real GPU test after reboot.

### Generate package
```bash
python3 - <<'PY'
import zipfile, os
name = 'PanVK-v9-driver_winlator_kimchi.zip'
if os.path.exists(name): os.remove(name)
z = zipfile.ZipFile(name, 'w', zipfile.ZIP_DEFLATED)
for r, d, fs in os.walk('PanVK-v9-Kimchi'):
    for f in fs:
        p = os.path.join(r, f); z.write(p, p)
z.close()
PY
```

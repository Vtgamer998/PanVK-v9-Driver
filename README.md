# PanVK-v9 Driver — Vulkan ICD for Mali-G68 MC4

Experimental Vulkan driver for **ARM Mali-G68 MC4 (MediaTek MT6893, Valhall v9)**.

---

## ⚠️ IMPORTANT NOTICE — READ BEFORE USE

### Disclaimer

**THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.**

The author **IS NOT RESPONSIBLE** for any direct, indirect, incidental, special, consequential, or punitive damages arising from the use or inability to use this software, including but not limited to:

- **HARDWARE DAMAGE**: Phone crash, forced reboot, permanent GPU or component damage
- **DATA LOSS**: Corruption of data, configurations, or applications
- **SYSTEM INSTABILITY**: Freezing, operating system crashes
- **WARRANTY VOID**: Using this software may void the device warranty
- **OPERATING SYSTEM DAMAGE**: Possible corruption of Android or kernel

**YOU ASSUME ALL RESPONSIBILITY FOR USING THIS SOFTWARE.**

### Known Risks

| Risk | Severity | Description |
|---|---|---|
| **Phone reboot** | HIGH | GPU may enter JOB_READ_FAULT state, causing forced reboot by watchdog |
| **System freeze** | HIGH | GPU timeout may freeze the device |
| **GPU damage** | MEDIUM | Improper use may cause overheating or premature wear |
| **Data loss** | MEDIUM | Forced reboot may cause loss of unsaved data |
| **Warranty void** | HIGH | System modifications may void manufacturer warranty |

### Safety Recommendations

1. **BACKUP** all important data before use
2. **DO NOT USE** on primary device or with critical data
3. **KEEP** phone charging and well ventilated
4. **USE** `PANVK_DRY_RUN=1` by default (safe mode, no real GPU)
5. **REBOOT** phone immediately if abnormal behavior is noticed
6. **DO NOT MODIFY** kernel or operating system

---

## Credits and Acknowledgments

### Base Project

This driver was developed based on the reverse engineering project:

**[VectorJet/Mali-G77-MC9](https://github.com/VectorJet/Mali-G77-MC9)**

> Reverse-engineering notes and tools for ARM Mali-G77 MC9 GPU driver behavior.

Special thanks to **VectorJet** for the reverse engineering work that made this project possible.

### Technologies Used

- **Mesa 26.2** — SPIR-V→Valhall v9 compiler (Panfrost backend)
- **Vulkan API** — Low-level graphics interface
- **Linux Kernel** — kbase interface for Mali GPU
- **ARM Mali-G68 MC4** — Target GPU (MediaTek Dimensity 700)

---

## About the Project

### What it is

An experimental Vulkan ICD (Installable Client Driver) for ARM Mali GPUs, similar to Turnip for Qualcomm Adreno, but for Valhall v9 architecture.

### Capabilities

- Triangle rendering via complete Vulkan pipeline
- Functional SPIR-V→Valhall compiler (Mesa 26.2)
- ~150 Vulkan entry points supported
- Memory and images (linear layout, copy, blit, clear)

### Limitations

- **DOES NOT run games** (DXVK/VKD3D require complete API implementation)
- **Not multi-tile** — processes only first 16x16 tile
- **Fragility** — may cause JOB_READ_FAULT and reboot
- **SELinux** — shader page mapped RW + PROT_EXEC blocked

---

## Project Structure

```
PanVK-v9-Driver/
├── src/                    # Driver source code
│   ├── panvk_v9_*.c/.h    # Vulkan entry points and ICD
│   ├── v9_cmd_stream.*    # Command buffer / job chain
│   ├── v9_pack.h          # Descriptor layout
│   ├── pan_kmod_kbase.*   # kbase backend
│   └── kbase_winsys.*     # /dev/mali0 ioctls
├── test/                   # Tests and examples
│   ├── test_*.c           # Vulkan API tests
│   ├── dense_map.c        # Direct render
│   └── ...                # Other tests
├── build/                  # Build and execution scripts
│   ├── build.sh           # Compilation script
│   └── run_*.sh           # Execution scripts
├── docs/                   # Documentation
│   ├── README.md          # This file
│   └── REPORT.md          # Technical report
└── README.md               # Main documentation
```

---

## Prerequisites

- **Hardware**: Android device with ARM Mali-G68 MC4 GPU (or similar Valhall v9)
- **Software**: Termux on Android
- **Access**: Root may be required for some tests
- **Kernel**: `mali_kbase` (MediaTek r49 or compatible)

---

## Installation

### Compilation

```bash
cd src/

# ICD library
clang -O2 -shared -fPIC -o libvulkan_panvk_v9.so \
    panvk_v9_entrypoints.c panvk_v9_x11.c \
    v9_cmd_stream.c pan_kmod_kbase.c kbase_winsys.c \
    -ldl -lpthread -lX11 -lxcb

# Compiler (optional, requires Mesa 26.2)
# See docs/REPORT.md for instructions
```

### Configuration

```bash
export VK_ICD_FILENAMES=/path/to/vulkan.panvk_v9.json
export LD_LIBRARY_PATH=/path/to:$LD_LIBRARY_PATH
```

---

## Usage

### Safe Tests (no real GPU)

```bash
PANVK_DRY_RUN=1 ./test_vulkan_loader_icd   # Complete pipeline, CPU only
PANVK_DRY_RUN=1 ./test_loader_images       # Memory/image
```

### Real Test (with GPU) — CAUTION!

```bash
# AFTER phone reboot, single frame
PANVK_DRY_RUN=0 PANVK_SUBMIT_TIMEOUT_MS=1500 V9_SKIP_POST_FLUSH=1 timeout 25 ./dense_map 16 16
```

**⚠️ WARNING**: Tests with real GPU may cause phone reboot!

---

## Environment Variables

| Variable | Description |
|---|---|
| `PANVK_DRY_RUN=1` | Safe mode (CPU only, no /dev/mali0) |
| `PANVK_DRY_RUN=0` | Uses real GPU (DANGEROUS!) |
| `PANVK_SUBMIT_TIMEOUT_MS` | Submit timeout (default 1500ms) |
| `V9_SKIP_POST_FLUSH=1` | Skips Post-Flush (avoids JOB_READ_FAULT) |
| `PANVK_FS_WORKREG` | Debug: FS work registers |
| `PANVK_FORCE_BARRIER` | Debug: force barrier |

---

## Technical Limitations

1. **Multi-tile**: Processes only first 16x16 tile
2. **Fragment termination**: Never signals 0x1 DONE (behavior identical to reference)
3. **SELinux**: Shader page mapped RW + PROT_EXEC blocked
4. **Incomplete**: No full swapchain, no complete Vulkan 1.1+ extensions
5. **GPU fragility**: May enter JOB_READ_FAULT and freeze

---

## DXVK/DirectX Game Support (Winlator)

**Current Status: NOT SUPPORTED**

For running Windows games via DXVK/vkd3d-proton, the following would be needed:
- Complete swapchain implementation
- Sync primitives (semaphores, fences)
- Compute shaders
- Dynamic state
- Buffer device address
- External memory FD

Estimated effort: 4-6 months full-time development.

**Recommendation**: Use the official Mali driver from the device manufacturer for game support.

---

## License

This project is a study/experimental ICD. Use at your own risk.

---

## Documentation

- [docs/README.md](docs/README.md) — Detailed driver documentation
- [docs/REPORT.md](docs/REPORT.md) — Complete technical report

---

## Contact

For issues or suggestions, open an issue in the repository.

---

**REMEMBER: YOU ASSUME ALL RESPONSIBILITY FOR USING THIS SOFTWARE. THE AUTHOR IS NOT RESPONSIBLE FOR ANY DAMAGES.**

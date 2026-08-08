#!/data/data/com.termux/files/usr/bin/bash
# Build PanVK-v9 driver + test harness (Termux, clang)
set -e
cd "$(dirname "$0")"

CC=${CC:-clang}
CFLAGS="-O2 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-format"

echo "== ICD libvulkan_panvk_v9.so =="
$CC $CFLAGS -shared -fPIC -o libvulkan_panvk_v9.so \
    panvk_v9_entrypoints.c panvk_v9_x11.c \
    v9_cmd_stream.c pan_kmod_kbase.c kbase_winsys.c kbase_slot_unwedge.c \
    -ldl -lpthread -lX11 -lxcb

echo "== compiler lib libpanvk_v9_compiler.so (wrapper Mesa, if sources present) =="
if [ -f panvk_v9_compiler_mesa.c ] && [ -f "$HOME/mesa-26.2.0-rc3/src/compiler/glsl_types.h" ]; then
    MESA="$HOME/mesa-26.2.0-rc3"
    INC="-I$MESA/src/panfrost -I$MESA/src/panfrost/shared -I$MESA/src/panfrost/compiler -I$MESA/src/panfrost/lib -I$MESA/src/panfrost/libpan -I$MESA/src/panfrost/model -I$MESA/src/compiler/nir -I$MESA/src/compiler -I$MESA/src/compiler/spirv -I$MESA/src/util -I$MESA/src/util/format -I$MESA/include -I$MESA/src"
    $CC $CFLAGS -shared -fPIC -o libpanvk_v9_compiler.so \
        panvk_v9_compiler_mesa.c $INC -ldl -lpthread || echo "  (skip compiler, requires Mesa headers)"
fi

echo "== render tests (link cmd stream + kbase) =="
for t in dense_map two_frame tile_probe; do
    $CC $CFLAGS -o $t $t.c v9_cmd_stream.c pan_kmod_kbase.c kbase_winsys.c kbase_slot_unwedge.c
done

echo "== GPU-wedge safety tests =="
$CC $CFLAGS -o test_wedge test_wedge.c kbase_winsys.c

echo "== loader/ICD tests =="
$CC $CFLAGS -o test_vulkan_loader_icd test_vulkan_loader_icd.c
$CC $CFLAGS -o test_loader_images test_loader_images.c

echo "== feature negotiation test (optional) =="
if [ -f test_features.c ]; then
    $CC $CFLAGS -o test_features test_features.c -ldl
fi

echo "build ok"

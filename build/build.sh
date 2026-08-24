#!/data/data/com.termux/files/usr/bin/bash
# Build PanVK-v9 driver + test harness (Termux, clang)
set -e
cd "$(dirname "$0")"

CC=${CC:-clang}
CFLAGS="-O2 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-format"
SRCDIR="../src"

echo "== ICD libvulkan_panvk_v9.so (GNU/Linux glibc via zig) =="
./build_glibc.sh libvulkan_panvk_v9.so

echo "== compiler lib libpanvk_v9_compiler.so (wrapper Mesa, if sources present) =="
if [ -f $SRCDIR/panvk_v9_compiler_mesa.c ] && [ -f "$HOME/mesa-26.2.0-rc3/src/compiler/glsl_types.h" ]; then
    MESA="$HOME/mesa-26.2.0-rc3"
    INC="-I$MESA/src/panfrost -I$MESA/src/panfrost/shared -I$MESA/src/panfrost/compiler -I$MESA/src/panfrost/lib -I$MESA/src/panfrost/libpan -I$MESA/src/panfrost/model -I$MESA/src/compiler/nir -I$MESA/src/compiler -I$MESA/src/compiler/spirv -I$MESA/src/util -I$MESA/src/util/format -I$MESA/include -I$MESA/src"
    $CC $CFLAGS -shared -fPIC -o libpanvk_v9_compiler.so \
        $SRCDIR/panvk_v9_compiler_mesa.c $INC -ldl -lpthread || echo "  (skip compiler, requires Mesa headers)"
fi

echo "== render tests (link cmd stream + kbase) =="
TESTDIR="../test"
CFLAGS="$CFLAGS -I$SRCDIR"
for t in dense_map two_frame tile_probe; do
    $CC $CFLAGS -o $t $TESTDIR/$t.c $SRCDIR/v9_cmd_stream.c $SRCDIR/pan_kmod_kbase.c $SRCDIR/kbase_winsys.c $SRCDIR/kbase_slot_unwedge.c
done

echo "== GPU-wedge safety tests =="
$CC $CFLAGS -o test_wedge $TESTDIR/test_wedge.c $SRCDIR/kbase_winsys.c

echo "== loader/ICD tests =="
$CC $CFLAGS -o test_vulkan_loader_icd $TESTDIR/test_vulkan_loader_icd.c
$CC $CFLAGS -o test_loader_images $TESTDIR/test_loader_images.c

echo "== feature negotiation test (optional) =="
if [ -f $TESTDIR/test_features.c ]; then
    $CC $CFLAGS -o test_features $TESTDIR/test_features.c -ldl
fi

echo "build ok"

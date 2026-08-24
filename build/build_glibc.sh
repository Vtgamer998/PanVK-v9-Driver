#!/data/data/com.termux/files/usr/bin/bash
# Build ICD libvulkan_panvk_v9.so para GNU/Linux aarch64 (glibc) — Winlator rootfs
# Toolchain: zig cc (bundled glibc 2.35 headers/libs/CRT)
set -e
cd "$(dirname "$0")"

SRCDIR="../src"
PREFIX=/data/data/com.termux/files/usr
SYSROOT=/data/data/com.termux/files/usr/tmp/glibc_sysroot

OUT=${1:-libvulkan_panvk_v9.so}

echo "== ICD glibc (zig cc -> aarch64-linux-gnu.2.35) =="
zig cc -target aarch64-linux-gnu.2.35 -shared -fPIC -O2 \
    -Wall -Wno-unused-parameter -Wno-unused-function -Wno-format \
    -Wno-date-time -Wno-error \
    -D_GNU_SOURCE \
    -I"$SYSROOT/include" -I"$SRCDIR" \
    -o "$OUT" \
    "$SRCDIR/panvk_v9_entrypoints.c" \
    "$SRCDIR/v9_cmd_stream.c" \
    "$SRCDIR/pan_kmod_kbase.c" \
    "$SRCDIR/kbase_winsys.c" \
    "$SRCDIR/kbase_slot_unwedge.c" \
    "$SRCDIR/vk_missing_stubs.c" \
    "$SYSROOT/lib/libX11.so.6" \
    "$SYSROOT/lib/libxcb.so.1" \
    "$SYSROOT/lib/libXau.so.6" \
    "$SYSROOT/lib/libXdmcp.so.6"

echo "== verificacao rapida =="
file "$OUT" | head -1
readelf -d "$OUT" | grep NEEDED
echo "U vk*: $(nm -u "$OUT" | grep -c '^ *U vk') || true"
echo "build_glibc ok: $OUT"

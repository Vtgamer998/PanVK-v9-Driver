#!/data/data/com.termux/files/usr/bin/bash
# Script definitivo de build glibc + repack do APK Winlator
set -e
cd "$(dirname "$0")"

VERSION=${1:-"fixed52"}
OUTPUT_APK="/storage/emulated/0/Download/Winlator_11.1_mali_panvk_${VERSION}.apk"

echo "=== 1. Compilando ICD com glibc (zig cc aarch64-linux-gnu.2.35) ==="
bash ./build_glibc.sh libvulkan_panvk_v9.so

echo "=== 2. Montando tzst com runtime glibc + X11 ==="
EX="/data/data/com.termux/files/usr/tmp/apkwork/panvk_extract"
SYSROOT="/data/data/com.termux/files/usr/tmp/glibc_sysroot"
mkdir -p "$EX/lib" "$EX/usr/lib"

cp libvulkan_panvk_v9.so "$EX/lib/"
cp "$SYSROOT/lib/libX11.so.6" \
   "$SYSROOT/lib/libxcb.so.1" \
   "$SYSROOT/lib/libXau.so.6" \
   "$SYSROOT/lib/libXdmcp.so.6" "$EX/usr/lib/"

cd "$EX"
tar -I "zstd -19" -cf /data/data/com.termux/files/usr/tmp/new_panvk-1.0.tzst .

echo "=== 3. Atualizando e reconstruindo APK ==="
cd /data/data/com.termux/files/usr/tmp
cp new_panvk-1.0.tzst apk_decode_orig/assets/graphics_driver/panvk-1.0.tzst
rm -f apk_orig_reb.apk apk_orig_reb_aligned.apk
apktool b apk_decode_orig -o apk_orig_reb.apk
zipalign -p 4 apk_orig_reb.apk apk_orig_reb_aligned.apk

echo "=== 4. Assinando APK ==="
apksigner sign --ks debug.jks --ks-pass pass:android --key-pass pass:android \
  --out "$OUTPUT_APK" apk_orig_reb_aligned.apk

echo "=== Concluído com sucesso! ==="
ls -lh "$OUTPUT_APK"

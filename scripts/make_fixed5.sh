#!/data/data/com.termux/files/usr/bin/bash
# ============================================================================
# make_fixed5.sh — Gera Winlator_11.1_mali_panvk_fixed5.apk
# Versão definitiva com todas as correções:
# 1. Driver Bionic (NDK) correto em /usr/lib/libvulkan_panvk_v9.so
# 2. Compilador SPIR-V em /usr/lib/libpanvk_v9_compiler.so
# 3. ICD JSON absoluto (/data/data/com.winlator/files/rootfs/usr/lib/libvulkan_panvk_v9.so)
# 4. config.box64rc injetado com env vars essenciais
# 5. Permissões 777 corrigidas na rootfs
# ============================================================================
set -uo pipefail

DL=~/storage/downloads
WORK=~/panvk-work
TMP=/data/data/com.termux/files/usr/tmp/opencode/fixed5build
SRC="$WORK/Winlator_original.apk"
OUT="$DL/Winlator_11.1_mali_panvk_fixed5.apk"
DRIVER="$WORK/glibc-build/libvulkan_panvk_v9.so"
COMPILER="$WORK/glibc-build/libpanvk_v9_compiler.so"
KS="$WORK/panvk.keystore"

echo "== [1/6] Verificando pré-requisitos =="
[ -f "$SRC" ] || { echo "ERRO: $SRC não encontrado"; exit 1; }
[ -f "$DRIVER" ] || { echo "ERRO: $DRIVER não encontrado"; exit 1; }
[ -f "$COMPILER" ] || { echo "AVISO: $COMPILER não encontrado (DXVK sem pipelines)"; }

echo "== [2/6] Extraindo APK base =="
rm -rf "$TMP"; mkdir -p "$TMP/apk" "$TMP/rootfs"
unzip -qo "$SRC" -d "$TMP/apk" || { echo "ERRO: unzip falhou"; exit 1; }

echo "== [3/6] Descomprimindo rootfs.tzst =="
TZST=$(ls "$TMP/apk/assets/rootfs.tzst" 2>/dev/null)
[ -f "$TZST" ] || { echo "ERRO: rootfs.tzst não encontrado"; exit 1; }
zstd -df "$TZST" -o "$TMP/rootfs.tar" || { echo "ERRO: zstd falhou"; exit 1; }
tar -xf "$TMP/rootfs.tar" -C "$TMP/rootfs" || { echo "ERRO: tar extract"; exit 1; }

echo "== [4/6] Injetando Driver, Compilador, ICD JSON e Box64 RC =="
mkdir -p "$TMP/rootfs/usr/lib" "$TMP/rootfs/usr/share/vulkan/icd.d" "$TMP/rootfs/etc"

# Driver
install -m 755 "$DRIVER" "$TMP/rootfs/usr/lib/libvulkan_panvk_v9.so"
echo "  injetado: usr/lib/libvulkan_panvk_v9.so ($(file "$TMP/rootfs/usr/lib/libvulkan_panvk_v9.so" | grep -o 'for Android[^,]*'))"

# Compilador
if [ -f "$COMPILER" ]; then
  install -m 755 "$COMPILER" "$TMP/rootfs/usr/lib/libpanvk_v9_compiler.so"
  mkdir -p "$TMP/rootfs/usr/lib/aarch64-linux-gnu"
  install -m 755 "$COMPILER" "$TMP/rootfs/usr/lib/aarch64-linux-gnu/libpanvk_v9_compiler.so"
  echo "  injetado: libpanvk_v9_compiler.so"
fi

# ICD JSON com caminho absoluto do host
HOST_PATH="/data/data/com.winlator/files/rootfs"
cat > "$TMP/rootfs/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json" << ICD
{
    "file_format_version": "1.0.1",
    "ICD": {
        "api_version": "1.2.0",
        "library_arch": "64",
        "library_path": "$HOST_PATH/usr/lib/libvulkan_panvk_v9.so"
    }
}
ICD
chmod 644 "$TMP/rootfs/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json"
mkdir -p "$TMP/rootfs/etc/vulkan/icd.d"
cp -f "$TMP/rootfs/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json" "$TMP/rootfs/etc/vulkan/icd.d/"

# Box64 RC
cat > "$TMP/rootfs/etc/config.box64rc" << RC
[*]
BOX64_ENV=VK_ICD_FILENAMES=$HOST_PATH/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json
BOX64_ENV1=PANVK_V9_COMPILER_LIBRARY=$HOST_PATH/usr/lib/libpanvk_v9_compiler.so
RC
chmod 644 "$TMP/rootfs/etc/config.box64rc"

# Permissões 777
find "$TMP/rootfs" -type d -exec chmod 777 {} + 2>/dev/null
find "$TMP/rootfs" -type f -exec chmod 777 {} + 2>/dev/null

echo "== [5/6] Reempacotando rootfs (zstd -19) =="
cd "$TMP/rootfs"
tar -cf "$TMP/rootfs-new.tar" . || { echo "ERRO: tar create"; exit 1; }
zstd -19 -T0 -f "$TMP/rootfs-new.tar" -o "$TMP/rootfs-new.tar.zst" || \
  zstd -19f "$TMP/rootfs-new.tar" -o "$TMP/rootfs-new.tar.zst"
install -m 644 "$TMP/rootfs-new.tar.zst" "$TMP/apk/assets/rootfs.tzst"
echo "  rootfs.tzst gerado ($(stat -c%s "$TMP/apk/assets/rootfs.tzst") bytes)"

echo "== [6/6] Rezipando, Alinhando e Assinando APK =="
rm -rf "$TMP/apk/META-INF"
cd "$TMP/apk"
rm -f "$TMP/unsigned.apk" "$TMP/aligned.apk"
zip -q -r -X "$TMP/unsigned.apk" . || { echo "ERRO: zip"; exit 1; }

zipalign -f 4 "$TMP/unsigned.apk" "$TMP/aligned.apk" || cp "$TMP/unsigned.apk" "$TMP/aligned.apk"

apksigner sign --ks "$KS" --ks-pass pass:panvk123 --key-pass pass:panvk123 \
  --out "$OUT" "$TMP/aligned.apk" || { echo "ERRO: apksigner"; exit 1; }

echo ""
echo "============================================================"
echo " SUCESSO! APK GERADO:"
echo " $OUT"
echo "============================================================"
apksigner verify --print-certs "$OUT" 2>&1 | head -4

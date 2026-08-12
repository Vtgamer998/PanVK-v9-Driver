#!/data/data/com.termux/files/usr/bin/bash
# ============================================================================
# diag-apk.sh — Diagnóstico do APK Winlator modificado (debug_6_signed)
# ----------------------------------------------------------------------------
# Compara o APK modificado com a base Winlator_11.1.apk, localiza a rootfs,
# verifica se o driver injetado é bionic ou glibc e testa a integridade da
# rootfs. É o primeiro passo para descobrir POR QUE o container não abre.
#
# USO:
#   bash ~/panvk-work/diag-apk.sh [APK_MODIFICADO]
#   (padrão: ~/storage/downloads/Winlator_11.1_mali_panvk_debug_6_signed.apk)
# ============================================================================
set -u

DL=~/storage/downloads
WORK=~/panvk-work/diag
MOD="${1:-$DL/Winlator_11.1_mali_panvk_debug_6_signed.apk}"
BASE="$DL/Winlator_11.1.apk"

mkdir -p "$WORK/mod"
rm -rf "$WORK/mod"/*

echo "============================================================"
echo " DIAGNÓSTICO DO APK WINLATOR (container não abre)"
echo "============================================================"

echo ""
echo "== [1/10] Ferramentas disponíveis =="
for t in unzip zip tar xz file java keytool zipalign apksigner readelf nm; do
  if command -v $t >/dev/null 2>&1; then echo "  OK    $t"; else echo "  FALTA $t"; fi
done
echo "  (dica: pkg install zipalign apksigner openjdk-17 xz-utils binutils)"

echo ""
echo "== [2/10] APK modificado existe? =="
if [ ! -f "$MOD" ]; then echo "  ERRO: $MOD não encontrado"; exit 1; fi
ls -la "$MOD"
echo ""
echo "== [3/10] Lista do APK modificado (estrutura geral) =="
unzip -l "$MOD" | awk '{print $4}' | grep -v '^$' | head -50

echo ""
echo "== [4/10] Diferença de conteúdo vs base =="
if [ -f "$BASE" ]; then
  unzip -l "$BASE" | awk '{print $4}' | sort > "$WORK/base.list"
  unzip -l "$MOD"  | awk '{print $4}' | sort > "$WORK/mod.list"
  echo "  --- Arquivos que EXISTEM no modificado e NÃO na base: ---"
  comm -13 "$WORK/base.list" "$WORK/mod.list" | head -30
  echo "  --- Arquivos que EXISTEM na base e NÃO no modificado: ---"
  comm -23 "$WORK/base.list" "$WORK/mod.list" | head -30
else
  echo "  (base $BASE não encontrada — pulando comparação)"
fi

echo ""
echo "== [5/10] Extraindo APK modificado =="
unzip -qo "$MOD" -d "$WORK/mod" && echo "  extraído em $WORK/mod"

echo ""
echo "== [6/10] Layout da rootfs (assets) =="
echo "  --- assets/ ---"
ls -la "$WORK/mod/assets/" 2>/dev/null | head -30
if ls "$WORK/mod/assets/package/"*.xz >/dev/null 2>&1; then
  echo "  FORMATO: rootfs = pacotes .xz partidos em assets/package/"
  ls -la "$WORK/mod/assets/package/" | head -15
elif ls "$WORK/mod/assets/"*.img >/dev/null 2>&1; then
  echo "  FORMATO: rootfs = imagem ($(ls "$WORK/mod/assets/"*.img))"
  file "$WORK/mod/assets/"*.img
else
  echo "  FORMATO: outro (procurando...)"; ls -la "$WORK/mod/assets/"
fi

echo ""
echo "== [7/10] Arquivos PanVK/vulkan injetados =="
find "$WORK/mod" \( -iname '*panvk*' -o -iname '*vulkan*' -o -iname '*icd*' \) 2>/dev/null | head -40

echo ""
echo "== [8/10] libs nativas do APK (lib/arm64-v8a) =="
ls -la "$WORK/mod/lib/arm64-v8a/" 2>/dev/null | head -30

echo ""
echo "== [9/10] Tipo do driver .so (bionic vs glibc) =="
find "$WORK/mod" -name 'libvulkan_panvk_v9.so' -exec file {} \; 2>/dev/null
echo "  --- NEEDED do .so injetado (procure libX11/libxcb/libc.so.6) ---"
for f in $(find "$WORK/mod" -name 'libvulkan_panvk_v9.so' 2>/dev/null); do
  readelf -d "$f" 2>/dev/null | grep -E 'NEEDED' || echo "  (readelf indisponível)"
done
echo "  --- __errno shim? ---"
find "$WORK/mod" -name 'liberrno_shim.so' -exec echo "  FOUND: {}" \; 2>/dev/null || echo "  (não encontrado no APK)"
echo "  --- __errno no libc.so.6 da rootfs? (se vazio → precisa do shim) ---"
for f in $(find "$WORK/mod" -name 'libc.so.6' -path '*/aarch64*' 2>/dev/null); do
  nm -D "$f" 2>/dev/null | grep '__errno' || echo "  $f: __errno NÃO exportado → use LD_PRELOAD liberrno_shim.so"
done

echo ""
echo "== [10/10] Assinatura do APK =="
if command -v apksigner >/dev/null 2>&1; then
  apksigner verify --print-certs "$MOD" 2>&1 | head -10
else
  echo "  apksigner não instalado (pkg install apksigner)"
fi

echo ""
echo "============================================================"
echo " RESULTADOS IMPORTANTES PARA VER:"
echo "  1. Formato da rootfs (pacotes .xz OU imagem)"
echo "  2. Se o .so é 'ELF 64-bit ... GNU/Linux' (glibc) ou 'Android' (bionic)"
echo "  3. Se o .so pede libX11.so.6 / libxcb.so.1 / libc.so (incompatível)"
echo "  4. Se faltam arquivos da base no APK modificado"
echo "  5. Se liberrno_shim.so está presente (para __errno)"
echo "  6. Se libc.so.6 exporta __errno (se não, precisa do shim)"
echo "============================================================"

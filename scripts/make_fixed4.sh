#!/data/data/com.termux/files/usr/bin/bash
# make_fixed4.sh — Gera Winlator_11.1_mali_panvk_fixed4.apk
# A partir do fixed3.apk (que já tem ICD json, box64rc, compiler.so corretos),
# TROCA apenas o binário do driver por um build BIONIC (NDK) que o loader
# nativo do Android consegue carregar. Remove o shim glibc (desnecessário no bionic).
set -uo pipefail

DL=~/storage/downloads
WORK=~/panvk-work
TMP=/data/data/com.termux/files/usr/tmp/opencode/fixed4build
SRC="$DL/Winlator_11.1_mali_panvk_fixed3.apk"
OUT="$DL/Winlator_11.1_mali_panvk_fixed4.apk"
DRIVER="$WORK/glibc-build/libvulkan_panvk_v9.so"   # BIONIC (NDK r29)
KS="$WORK/panvk.keystore"

echo "== Pré-voo: driver é bionic? =="
if file "$DRIVER" | grep -qi 'Android'; then
  echo "  OK: driver bionic ($DRIVER)"
else
  echo "  ERRO: $DRIVER NÃO é bionic (esperado 'for Android'). Abortando."
  exit 1
fi
if ! readelf -d "$DRIVER" 2>/dev/null | grep -qE 'NEEDED.*libc\.so[^.]'; then
  echo "  ERRO: driver não pede libc.so (bionic). NEEDED:"
  readelf -d "$DRIVER" 2>/dev/null | grep NEEDED
  exit 1
fi
echo "  NEEDED:" $(readelf -d "$DRIVER" 2>/dev/null | grep NEEDED | sed 's/.*\[//;s/\]//' | tr '\n' ' ')

rm -rf "$TMP"; mkdir -p "$TMP/apk" "$TMP/rootfs"
echo "== Extraindo $SRC =="
unzip -qo "$SRC" -d "$TMP/apk" || { echo "ERRO: unzip falhou"; exit 1; }

echo "== Descomprimindo rootfs.tzst (zstd) =="
TZST=$(ls "$TMP/apk/assets/rootfs.tzst" 2>/dev/null)
[ -f "$TZST" ] || { echo "ERRO: rootfs.tzst não encontrado"; exit 1; }
zstd -df "$TZST" -o "$TMP/rootfs.tar" || { echo "ERRO: zstd falhou"; exit 1; }
echo "== Testando integridade do tar =="
if ! tar -tf "$TMP/rootfs.tar" >/dev/null 2>&1; then
  echo "ERRO: rootfs.tar corrompido"; exit 1
fi
echo "  tar OK"
tar -xf "$TMP/rootfs.tar" -C "$TMP/rootfs" || { echo "ERRO: tar extract"; exit 1; }

echo "== Trocando o driver por BIONIC =="
install -m 755 "$DRIVER" "$TMP/rootfs/usr/lib/libvulkan_panvk_v9.so" || { echo "ERRO: cp driver"; exit 1; }
# shim glibc não é necessário no bionic (libc do Android já fornece __errno)
rm -f "$TMP/rootfs/usr/lib/liberrno_shim.so"
echo "  removido liberrno_shim.so (glibc, não usado no bionic)"
echo "  driver agora:" $(file "$TMP/rootfs/usr/lib/libvulkan_panvk_v9.so" | grep -o 'for Android[^,]*')

echo "== Reempacotando rootfs (zstd -19) =="
cd "$TMP/rootfs"
tar -cf "$TMP/rootfs-new.tar" . || { echo "ERRO: tar create"; exit 1; }
zstd -19 -T0 -f "$TMP/rootfs-new.tar" -o "$TMP/rootfs-new.tar.zst" || \
  zstd -19f "$TMP/rootfs-new.tar" -o "$TMP/rootfs-new.tar.zst"
install -m 644 "$TMP/rootfs-new.tar.zst" "$TMP/apk/assets/rootfs.tzst"
echo "  rootfs.tzst:" $(ls -la "$TMP/apk/assets/rootfs.tzst" | awk '{print $5}')

echo "== Rezipando APK (sem META-INF) =="
rm -rf "$TMP/apk/META-INF"
cd "$TMP/apk"
rm -f "$TMP/unsigned.apk"
zip -q -r -X "$TMP/unsigned.apk" . || { echo "ERRO: zip"; exit 1; }

echo "== zipalign =="
zipalign -f 4 "$TMP/unsigned.apk" "$TMP/aligned.apk" || cp "$TMP/unsigned.apk" "$TMP/aligned.apk"

echo "== assinando =="
apksigner sign --ks "$KS" --ks-pass pass:panvk123 --key-pass pass:panvk123 \
  --out "$OUT" "$TMP/aligned.apk" || { echo "ERRO: apksigner"; exit 1; }

echo "== verificando assinatura =="
apksigner verify --print-certs "$OUT" 2>&1 | head -4

echo ""
echo "============================================================"
echo " PRONTO: $OUT"
echo "============================================================"
echo "Driver injetado: BIONIC (NDK) — carregável pelo loader nativo do Android."
echo "Próximo passo: desinstalar o Winlator antigo e instalar este; rodar"
echo "vulkaninfo no container e procurar 'Mali-G68 MC4'."

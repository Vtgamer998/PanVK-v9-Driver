#!/data/data/com.termux/files/usr/bin/bash
# ============================================================================
# instala-panvk.sh — PASSO ÚNICO, SÓ TERMUX (sem terminal no Winlator).
#
# Garante que o APK final do Winlator sai COMPLETO (ICD json + driver +
# compiler), verifica o resultado e imprime os próximos passos no aparelho.
#
# USO:
#   bash ~/panvk-work/instala-panvk.sh
#
# Depois de rodar, o que sobra é no aparelho (sem digitar nada no Winlator):
#   desinstalar o Winlator -> instalar o APK novo -> criar container novo ->
#   colar a env var -> testar. O script imprime tudo no final.
# ============================================================================
set -uo pipefail

DL=~/storage/downloads
WORK=~/panvk-work
MOD="${1:-$DL/Winlator_11.1_mali_panvk_debug_6_signed.apk}"
BASE="$DL/Winlator_11.1.apk"
OUTAPK="$DL/Winlator_11.1_mali_panvk_fixed.apk"
ENVVAR="VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json"
ENVVAR_ERRNO="LD_PRELOAD=/lib/liberrno_shim.so"
ENVVAR_NOSYM="BOX64_NOSYMBOLIC=1"

echo "============================================================"
echo " INSTALA PANVK — só Termux"
echo "============================================================"

echo ""
echo "== [1/6] Ferramentas =="
FALTANDO=""
for t in zip unzip xz zstd zipalign apksigner keytool file readelf; do
  if command -v $t >/dev/null 2>&1; then echo "  OK    $t"; else echo "  FALTA $t"; FALTANDO="$FALTANDO $t"; fi
done
if [ -n "$FALTANDO" ]; then
  echo "  Instale: pkg install zip unzip xz-utils zstd zipalign apksigner openjdk-17 binutils file"
  exit 1
fi

echo ""
echo "== [2/6] Driver glibc (libvulkan_panvk_v9_glibc.so) =="
DRIVER="$WORK/glibc-build/libvulkan_panvk_v9_glibc.so"
if [ ! -f "$DRIVER" ]; then
  DRIVER="$WORK/glibc-build/libvulkan_panvk_v9.so"
  if [ -f "$DRIVER" ]; then
    echo " AVISO: _glibc.so não existe, usando libvulkan_panvk_v9.so (verifique o ABI)"
  else
    echo " ERRO: nenhum driver encontrado em $WORK/glibc-build/ — rode cross-build-glibc.sh primeiro"
    exit 1
  fi
fi
echo " $(file "$DRIVER")"
if file "$DRIVER" | grep -qi 'Android'; then
  echo " ERRO: o driver é BIONIC — precisa ser glibc. Rode: bash ~/panvk-work/cross-build-glibc.sh"
  exit 1
fi
echo " NEEDED:"
readelf -d "$DRIVER" 2>/dev/null | grep NEEDED | sed 's/^/ /' | head -10

echo ""
echo "== [3/6] Compiler (libpanvk_v9_compiler.so) =="
COMP="$WORK/glibc-build/libpanvk_v9_compiler.so"
if [ -f "$COMP" ]; then
  echo "  arquivo: $COMP ($(stat -c%s "$COMP") bytes)"
  echo "  $(file "$COMP")"
  NEEDED=$(readelf -d "$COMP" 2>/dev/null | grep -E 'NEEDED.*lib(c|m)\.so' | head -5)
  echo "  $NEEDED" | sed 's/^/    /'
  case "$NEEDED" in *libc.so.6*|*libm.so.6*) echo "  -> compiler GLIBC ok, será injetado" ;; *) echo "  -> AVISO: não confirmado glibc, mas será injetado mesmo assim (integra aceita ELF)" ;; esac
else
  echo "  AVISO: $COMP não existe!"
  echo "  O APK vai sair SEM o compiler -> DXVK não cria pipelines (container abre, jogo não renderiza)."
  echo "  Se tiver os objetos em ~/mesa-mesa-26.2.0-rc3/build-compiler, me chama que eu te passo o re-link."
fi

echo ""
echo "  --- errno shim ---"
if [ -f "$WORK/liberrno_shim.so" ]; then
  echo "  liberrno_shim.so: $WORK/liberrno_shim.so ($(stat -c%s "$WORK/liberrno_shim.so") bytes)"
  echo "  exporta __errno: $(nm -D "$WORK/liberrno_shim.so" | grep '__errno' | wc -l) símbolos"
else
  echo "  (liberrno_shim.so será compilado pelo integra.sh)"
fi

echo ""
echo "== [4/6] Rodando integra.sh (injeta + recompacta + assina) =="
if [ ! -f "$MOD" ]; then
  echo "  (base $MOD não existe — usando o APK fixed atual como base)"
  MOD="$OUTAPK"
fi
[ -f "$BASE" ] || echo "  (atenção: APK base $BASE não existe — ok se a rootfs do MOD estiver íntegra)"
bash "$WORK/integra.sh" "$MOD" "$BASE" || { echo "  ERRO no integra.sh — cole a saída acima."; exit 1; }

echo ""
echo "== [5/6] Verificando o APK final =="
[ -f "$OUTAPK" ] || { echo "  ERRO: $OUTAPK não foi gerado"; exit 1; }
echo "  APK: $OUTAPK ($(stat -c%s "$OUTAPK") bytes)"
CHECK="$WORK/checkfinal"; rm -rf "$CHECK"; mkdir -p "$CHECK"
unzip -qo "$OUTAPK" 'assets/package/*' -d "$CHECK" 2>/dev/null \
  || unzip -qo "$OUTAPK" 'assets/*.tzst' -d "$CHECK" 2>/dev/null
TAR=""
if ls "$CHECK/assets/package/"*.xz >/dev/null 2>&1; then
  for f in $(ls "$CHECK/assets/package/"*.xz | sort -V); do cat "$f"; done > "$WORK/check-rootfs.tar.xz"
  xz -dkf "$WORK/check-rootfs.tar.xz" -c > "$WORK/check-rootfs.tar" 2>/dev/null
  TAR="$WORK/check-rootfs.tar"
elif ls "$CHECK/assets/"*.tzst >/dev/null 2>&1; then
  TZ=$(ls "$CHECK/assets/"*.tzst 2>/dev/null | grep -v 'rootfs_patches\|container_pattern\|pulseaudio\|box64\|dxwrapper\|graphics_driver\|soundfont\|wallpapers\|wincomponents' | grep 'rootfs.tzst' | head -1)
  [ -n "$TZ" ] && zstd -dk "$TZ" -o "$WORK/check-rootfs.tar" --force 2>/dev/null
  TAR="$WORK/check-rootfs.tar"
fi
if [ -n "$TAR" ] && tar -tf "$TAR" >/dev/null 2>&1; then
  echo "  --- arquivos PanVK dentro da rootfs do APK final ---"
  HIT=$(tar -tvf "$TAR" 2>/dev/null | grep -E 'panvk_v9_icd|libvulkan_panvk|libpanvk_v9_compiler|liberrno_shim')
  if [ -n "$HIT" ]; then
    echo "$HIT"
  else
    echo "  !!! NENHUM arquivo panvk na rootfs do APK — algo falhou na injeção"
  fi
else
  echo "  !!! não consegui ler a rootfs do APK (formato diferente?) — verifique com diag-apk.sh"
fi

echo ""
echo "== [6/6] PRÓXIMOS PASSOS NO APARELHO (sem abrir terminal no Winlator) =="
echo "  1. Desinstale o Winlator COMPLETO:"
echo "     Ajustes > Apps > Winlator > Desinstalar"
echo "     (⚠️ apaga containers/rootfs — é OBRIGATÓRIO: sem root a rootfs velha fica no aparelho)"
echo "  2. Instale: $OUTAPK"
echo "  3. Abra o Winlator e espere a extração da rootfs terminar"
echo "  4. Crie um container NOVO"
echo "  5. No container, em 'Environment Variables', adicione EXATAMENTE (uma linha só):"
echo "       $ENVVAR"
echo "       $ENVVAR_ERRNO"
echo "       $ENVVAR_NOSYM"
echo "  6. Abra o container e rode vulkaninfo (ou um jogo) — procure 'Mali-G68'"
echo ""
echo "  Se o passo [5/6] listou os arquivos panvk + liberrno_shim acima, o APK está pronto."
echo "  Se listou só 2 (sem compiler), o DXVK não vai renderizar — me avisa."

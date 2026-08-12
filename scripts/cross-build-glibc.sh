#!/data/data/com.termux/files/usr/bin/bash
# ============================================================================
# cross-build-glibc.sh — Compila o driver PanVK-v9 para GLIBC (rootfs Winlator)
# ----------------------------------------------------------------------------
# MOTIVO: o driver atual foi compilado no Termux (bionic/Android). Dentro da
# rootfs Debian (glibc) do Winlator, um .so bionic NÃO carrega. Este script
# recompila o driver para glibc (aarch64-linux-gnu) SEM depender de X11/xcb
# (o Winlator usa o backend WINE de apresentação, não X11).
#
# REQUISITOS (rodar no Termux):
#   pkg update && pkg install aarch64-linux-gnu-gcc binutils xz
#   pkg install x11-repo && pkg install xorg-dev libxcb  (headers X11/xcb)
#   pkg install vulkan-headers                              (headers Vulkan)
#
# USO:
#   bash ~/panvk-work/cross-build-glibc.sh
#
# SAÍDA: ~/panvk-work/glibc-build/libvulkan_panvk_v9.so  (glibc, sem X11)
# ============================================================================
set -euo pipefail

SRC=~/storage/downloads/g5/src
OUT=~/panvk-work/glibc-build
XHDR="$OUT/xhdr"
mkdir -p "$OUT" "$XHDR"

echo "== [1/5] Checando ferramentas =="
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  echo "AVISO: 'aarch64-linux-gnu-gcc' não está disponível no Termux"
  echo "       (o pacote foi removido do repositório main)."
  echo ""
  echo "USANDO MÉTODO ALTERNATIVO (recomendado): Debian via proot-distro"
  echo "  bash ~/panvk-work/cross-build-proot.sh"
  echo ""
  echo "(opcional) verifique se existe outro pacote de cross-compiler:"
  echo "  pkg search gcc"
  exit 2
fi
if ! command -v nm >/dev/null 2>&1; then
  echo "ERRO: 'nm' não encontrado. Instale: pkg install binutils"
  exit 1
fi
echo "OK: ferramentas presentes"

echo "== [2/5] Copiando headers X11/xcb/vulkan (arch-independent) =="
for d in X11 xcb vulkan; do
  if [ -d "$PREFIX/include/$d" ]; then
    cp -r "$PREFIX/include/$d" "$XHDR/"
    echo "  headers $d copiados"
  else
    echo "  AVISO: $PREFIX/include/$d não existe (xorg-dev / libxcb / vulkan-headers?)"
  fi
done

echo "== [3/5] Gerando stubs AArch64 para símbolos X11/xcb =="
# Extrai do .so bionic existente os símbolos X11/xcb que o driver referencia
# e gera stubs em assembly (mov x0,#0; ret) — assim o .so glibc fica
# autocontido: não pede libX11/libxcb no container.
REF_SO="$SRC/libvulkan_panvk_v9.so"
STUB_S="$OUT/x11stub.S"
: > "$STUB_S"
if [ -f "$REF_SO" ]; then
  SYMS=$(nm -D --undefined-only "$REF_SO" 2>/dev/null | awk '{print $2}' | \
         grep -E '^(X[A-Z]|XCreate|XOpen|XClose|XDefault|XRoot|XMap|XFlush|XPut|XGet|XFree|XDestroy|XPending|XNext|XStore|XSelect|XChange|xcb_)' || true)
  if [ -n "$SYMS" ]; then
    for s in $SYMS; do
      cat >> "$STUB_S" <<EOF
    .text
    .global $s
    .type $s, %function
$s:
    mov x0, #0
    ret
    .size $s, .-$s
EOF
    done
    echo "  gerados $(echo "$SYMS" | wc -l) stubs:"
    echo "$SYMS" | sed 's/^/    /'
  else
    echo "  nenhum símbolo X11/xcb detectado no .so de referência"
  fi
else
  echo "  AVISO: .so de referência não encontrado ($REF_SO) — gerando stubs padrão"
  for s in XOpenDisplay XCloseDisplay XCreateWindow XCreateSimpleWindow \
           XStoreName XSelectInput XMapWindow XFlush XDefaultScreen XRootWindow \
           XBlackPixel XWhitePixel XCreateImage XCreateGC XPutImage XFreeGC \
           XDestroyWindow XPending XNextEvent xcb_connect xcb_disconnect; do
    cat >> "$STUB_S" <<EOF
    .text
    .global $s
    .type $s, %function
$s:
    mov x0, #0
    ret
    .size $s, .-$s
EOF
  done
fi

echo "== [4/5] Compilando libvulkan_panvk_v9.so (glibc, sem X11) =="
aarch64-linux-gnu-gcc -O2 -shared -fPIC -fvisibility=hidden \
  -isystem "$XHDR" \
  -o "$OUT/libvulkan_panvk_v9.so" \
  "$SRC/panvk_v9_entrypoints.c" \
  "$SRC/v9_cmd_stream.c" \
  "$SRC/pan_kmod_kbase.c" \
  "$SRC/kbase_winsys.c" \
  "$SRC/kbase_slot_unwedge.c" \
  "$STUB_S" \
  -ldl -lpthread -lm
echo "OK: $OUT/libvulkan_panvk_v9.so"

echo "== [5/5] Verificação =="
file "$OUT/libvulkan_panvk_v9.so"
echo "--- Dependências dinâmicas (NEEDED) ---"
aarch64-linux-gnu-readelf -d "$OUT/libvulkan_panvk_v9.so" | grep -i needed || \
  readelf -d "$OUT/libvulkan_panvk_v9.so" | grep -i needed
echo "--- Símbolos X11/xcb restantes (devem ser só os stubados) ---"
aarch64-linux-gnu-nm -D --undefined-only "$OUT/libvulkan_panvk_v9.so" 2>/dev/null | \
  grep -E 'X[A-Z]|xcb_' | head -20 || echo "  nenhum (autocontido!)"

echo ""
echo "PRÓXIMO PASSO:"
echo "  bash ~/panvk-work/diag-apk.sh      (diagnosticar o APK atual)"
echo "  bash ~/panvk-work/integra.sh       (injetar na rootfs + assinar)"
echo ""
echo "NOTA: se este script falhar porque aarch64-linux-gnu-gcc não existe,"
echo "      rode o cross-build-proot.sh (Debian via proot-distro)."

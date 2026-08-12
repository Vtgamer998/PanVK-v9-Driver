#!/data/data/com.termux/files/usr/bin/bash
# ============================================================================
# cross-build-proot.sh — Compila o driver PanVK-v9 para GLIBC usando um
#                        Debian nativo via proot-distro (aarch64 = mesmo arch)
# ----------------------------------------------------------------------------
# POR QUE: o Termux não tem mais cross-compiler no repositório main
# (aarch64-linux-gnu-gcc foi removido). Compilando DENTRO de um Debian
# proot-distro, o gcc gera binário glibc aarch64 nativo — exatamente o que a
# rootfs do Winlator precisa. O driver é compilado SEM X11 (stubs em asm),
# então o container não depende de libX11/libxcb.
#
# REQUISITOS: Termux com internet. proot-distro será instalado sozinho.
#
# USO:
#   bash ~/panvk-work/cross-build-proot.sh
#
# SAÍDA: ~/panvk-work/glibc-build/libvulkan_panvk_v9.so  (glibc, sem X11)
# ============================================================================
set -uo pipefail

OUT=~/panvk-work/glibc-build
mkdir -p "$OUT"

echo "== [1/4] proot-distro =="
if ! command -v proot-distro >/dev/null 2>&1; then
  echo "Instalando proot-distro..."
  pkg install -y proot-distro || { echo "ERRO: instale com: pkg install proot-distro"; exit 1; }
fi

echo "== [2/4] Debian instalado? =="
if [ ! -d "$PREFIX/var/lib/proot-distro/containers/debian" ]; then
  echo "Instalando Debian (baixa ~300MB, pode demorar)..."
  proot-distro install debian || { echo "ERRO: falha ao instalar Debian"; exit 1; }
fi

echo "== [3/4] Gravando script de build (roda DENTRO do Debian) =="
cat > "$OUT/build-inside.sh" <<'BUILDEOF'
#!/bin/bash
set -uo pipefail
export DEBIAN_FRONTEND=noninteractive

echo "== apt: gcc + headers X11/Vulkan =="
apt-get update -qq
apt-get install -y -qq gcc binutils libvulkan-dev libx11-dev libxcb1-dev >/dev/null 2>&1

SRC=/data/data/com.termux/files/home/storage/downloads/g5/src
OUT=/data/data/com.termux/files/home/panvk-work/glibc-build
mkdir -p "$OUT"

STUB="$OUT/x11stub.S"
: > "$STUB"
add_stub() {
    printf '    .text\n    .global %s\n    .type %s, %%function\n%s:\n    mov x0, #0\n    ret\n    .size %s, .-%s\n' \
           "$1" "$1" "$1" "$1" "$1" >> "$STUB"
}

# Símbolos X11/xcb conhecidos do driver
for s in XOpenDisplay XCloseDisplay XDefaultScreen XRootWindow XBlackPixel \
         XWhitePixel XCreateSimpleWindow XCreateWindow XStoreName XSelectInput \
         XMapWindow XMapRaised XFlush XCreateImage XCreateGC XPutImage XFreeGC \
         XDestroyWindow XPending XNextEvent XInternAtom XSetWMProtocols \
         XChangeProperty XDrawString XSetWindowBackground \
         xcb_connect xcb_disconnect xcb_create_window; do
    add_stub "$s"
done

echo "== compilando (loop de stubs p/ símbolos X11/xcb faltando) =="
# Roda até não sobrar símbolo X11/xcb indefinido. Símbolos glibc (__errno etc.)
# ficam undefined de propósito — são resolvidos em runtime pela libc.so.6.
for i in 1 2 3 4 5 6 7 8 9 10; do
    gcc -O2 -g -rdynamic -fno-omit-frame-pointer -shared -fPIC -fvisibility=hidden -o "$OUT/libvulkan_panvk_v9.so" \
       "$SRC/panvk_v9_entrypoints.c" \
       "$SRC/v9_cmd_stream.c" \
       "$SRC/pan_kmod_kbase.c" \
       "$SRC/kbase_winsys.c" \
       "$SRC/kbase_slot_unwedge.c" \
       "$STUB" -ldl -lpthread -lm 2>"$OUT/err.txt" || {
        echo "ERRO no link:"; cat "$OUT/err.txt"; exit 1; }
    MISSING=$(nm -D --undefined-only "$OUT/libvulkan_panvk_v9.so" 2>/dev/null \
              | awk '{print $2}' | grep -E '^X[A-Z]|^xcb_' | sort -u)
    if [ -z "$MISSING" ]; then
        echo "BUILD OK"
        break
    fi
    while IFS= read -r s; do add_stub "$s"; done <<< "$MISSING"
    echo "  tentativa $i: adicionados stubs: $(echo "$MISSING" | tr '\n' ' ')"
done

echo ""
echo "== resultado =="
file "$OUT/libvulkan_panvk_v9.so"
echo "-- NEEDED (dependências) --"
readelf -d "$OUT/libvulkan_panvk_v9.so" | grep NEEDED || true
echo "-- símbolos X11/xcb ainda indefinidos (deve estar vazio) --"
nm -D --undefined-only "$OUT/libvulkan_panvk_v9.so" | grep -E 'X[A-Z]|xcb_' || echo "  nenhum — autocontido!"
BUILDEOF

echo "== [4/4] Rodando build dentro do Debian =="
proot-distro login debian -- bash /data/data/com.termux/files/home/panvk-work/glibc-build/build-inside.sh

echo ""
echo "PRÓXIMO PASSO:"
echo "  bash ~/panvk-work/diag-apk.sh   (ver layout do APK e se o driver atual é bionic)"
echo "  bash ~/panvk-work/integra.sh    (injetar + assinar)"

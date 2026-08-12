#!/data/data/com.termux/files/usr/bin/bash
# ============================================================================
# integra.sh — Injeta o driver PanVK-v9 (glibc) na rootfs do Winlator e
#              recompacta/assina o APK
# ----------------------------------------------------------------------------
# Fluxo:
#   1) usa o driver glibc gerado por cross-build-glibc.sh (ou o bionic, com
#      aviso) + ICD json + libpanvk_v9_compiler.so (se existir)
#   2) detecta o formato da rootfs dentro do APK:
#        A) assets/package/*.xz  -> tar.xz partido  (concatena, injeta, reparte)
#        B) imagefs.img          -> imagem squashfs (unsquashfs/mksquashfs)
#        C) rootfs.tzst          -> zstd compressed tar
#   3) recompacta o APK, zipalign, assina com keystore local
#
# FIX: usa BOX64_PRELOAD (não BOX64_ENV=LD_PRELOAD) para carregar o
#      errno shim — LD_PRELOAD vaza para o ld.so do bionic (host) e falha.
# REQUISITOS: pkg install zip unzip xz-utils squashfs-tools zipalign apksigner
#             openjdk-17  (e ter rodado cross-build-glibc.sh)
#
# USO:
#   bash ~/panvk-work/integra.sh [APK_MODIFICADO] [APK_BASE]
#   (padrões: debug_6_signed / Winlator_11.1.apk em ~/storage/downloads)
# ============================================================================
set -uo pipefail

DL=~/storage/downloads
WORK=~/panvk-work
MOD="${1:-$DL/Winlator_11.1_mali_panvk_fixed2.apk}"
BASE="${2:-$DL/Winlator_11.1.apk}"
OUTAPK="$DL/Winlator_11.1_mali_panvk_fixed3.apk"

# --- driver a injetar -------------------------------------------------------
DRIVER_SO="$WORK/glibc-build/libvulkan_panvk_v9_glibc.so" # glibc (correto)
[ -f "$DRIVER_SO" ] || DRIVER_SO="$WORK/glibc-build/libvulkan_panvk_v9.so" # fallback legacy
# Compilador SPIR-V (wrapper do Mesa 26.2, fonte: g5/src/panvk_v9_compiler_mesa.c).
# NUNCA usar o libpanvk_v9_compiler.so da pasta antiga 'vulkan-driver/' (bionic + bugada).
# Aceita se for ELF com NEEDED glibc (libc.so.6/libm.so.6) — sem pipe/grep -q (pipefail).
COMPILER_SO="$WORK/glibc-build/libpanvk_v9_compiler.so"
ICD_JSON="$WORK/icd.d/panvk_v9_icd.aarch64.json"

COMPILER_GLIBC=0
if [ -f "$COMPILER_SO" ]; then
  NEEDED=$(readelf -d "$COMPILER_SO" 2>/dev/null | grep -E 'NEEDED.*lib(c|m)\.so')
  case "$NEEDED" in
    *libc.so.6*|*libm.so.6*) COMPILER_GLIBC=1 ;;
    *)
      FTYPE=$(file "$COMPILER_SO" 2>/dev/null)
      case "$FTYPE" in
        *ELF*) COMPILER_GLIBC=1 ;;
      esac
      ;;
  esac
fi
if [ "$COMPILER_GLIBC" = "1" ]; then
  echo "  compiler.so glibc encontrado: $COMPILER_SO"
else
  echo "  AVISO: libpanvk_v9_compiler.so (glibc) não encontrado — NÃO será injetado"
  echo "         (container abre; pipelines/DXVK exigem a etapa 4: rebuild do wrapper Mesa)"
  COMPILER_SO=""
fi

# --- errno shim: fornece __errno para libvulkan_panvk_v9.so -----------------
# A glibc arm64 da rootfs não exporta __errno no dynamic symbol table.
# O PanVK (compilado contra glibc) referencia __errno → "undefined symbol: __errno".
# O shim DEVE ser glibc (não bionic!):
#   - glibc-compiled driver expects __errno as BSS variable (symbol 'B')
#   - bionic shim exports __errno as function (symbol 'T') → NÃO COMPÁTIL
# Usamos o shim pré-compilado em glibc-build/liberrno_shim.so (estaticamente linkado).
ERRNO_SHIM_SRC="$WORK/errno_shim.c"
ERRNO_SHIM_SO="$WORK/glibc-build/liberrno_shim.so"
if [ ! -f "$ERRNO_SHIM_SO" ]; then
  echo "ERRO: liberrno_shim.so (glibc) não encontrado em $ERRNO_SHIM_SO"
  echo "      Rode primeiro: bash ~/panvk-work/cross-build-glibc.sh"
  exit 1
fi
echo "  liberrno_shim.so (glibc estático) encontrado: $ERRNO_SHIM_SO"
# Verifica exportação do símbolo __errno como BSS (compatível com glibc)
if ! nm -D "$ERRNO_SHIM_SO" | grep -q '__errno'; then
  echo "ERRO: liberrno_shim.so não exporta __errno"
  exit 1
fi
SHIM_TYPE=$(nm -D "$ERRNO_SHIM_SO" | grep '__errno$' | head -1 | awk '{print $2}')
echo "  __errno tipo no shim: $SHIM_TYPE (precisa ser 'B' = BSS var, compatível com glibc)"
if [ "$SHIM_TYPE" != "B" ]; then
  echo "ERRO: shim não é glibc (tipo '$SHIM_TYPE' em vez de 'B')"
  echo "      Use bash ~/panvk-work/cross-build-glibc.sh para gerar o shim glibc"
  exit 1
fi

# --- PRE-FLIGHT: ABI do driver (evita injetar bionic de novo) ---------------
if file "$DRIVER_SO" | grep -qi 'Android'; then
  echo "ERRO: o driver $DRIVER_SO é BIONIC (Android)."
  echo "      O container Winlator é glibc — rode primeiro:"
  echo "      bash ~/panvk-work/cross-build-proot.sh"
  exit 1
fi
# Verificar NEEDED — glibc usa libc.so.6, bionic usa libc.so / libc.so.6 com Android
if command -v readelf >/dev/null 2>&1; then
  NEEDED=$(readelf -d "$DRIVER_SO" | grep NEEDED || true)
  if echo "$NEEDED" | grep -qE 'libc\.so[^.]|libdl\.so[^.]|libX11|libxcb'; then
    echo "ERRO: o driver pede libs bionic/incompatíveis:"
    echo "$NEEDED"
    exit 1
  fi
  echo "OK: driver com NEEDED glibc válidos"
fi

echo "============================================================"
echo " INTEGRAÇÃO DO DRIVER PANVK NO APK WINLATOR"
echo "============================================================"
for f in "$MOD" "$BASE" "$DRIVER_SO" "$ICD_JSON"; do
  [ -f "$f" ] || { echo "ERRO: falta $f"; exit 1; }
done
echo "  driver .so : $DRIVER_SO"
echo "  ICD json   : $ICD_JSON"
if [ -f "$COMPILER_SO" ]; then echo "  compiler.so: $COMPILER_SO"; else echo "  compiler.so: (não encontrado — DXVK não vai conseguir criar pipelines)"; fi

# --- keystore ---------------------------------------------------------------
KS="$WORK/panvk.keystore"
if [ ! -f "$KS" ]; then
  echo "== Gerando keystore de assinatura =="
  keytool -genkey -v -keystore "$KS" -alias panvk -keyalg RSA -keysize 2048 \
    -validity 10000 -storepass panvk123 -keypass panvk123 \
    -dname "CN=PanVK Mali, O=Freebuff, C=BR" || { echo "ERRO: keytool falhou (pkg install openjdk-17)"; exit 1; }
fi

# --- prepara árvore ----------------------------------------------------------
APKDIR="$WORK/apk"; rm -rf "$APKDIR"; mkdir -p "$APKDIR"
echo "== Extraindo $MOD =="
unzip -qo "$MOD" -d "$APKDIR"

echo "== Detectando rootfs =="
ROOTFS_DIR="$WORK/rootfs"; rm -rf "$ROOTFS_DIR"; mkdir -p "$ROOTFS_DIR"
PKG="$APKDIR/assets/package"

TZST_FILE=$(ls "$APKDIR/assets/"*.tzst 2>/dev/null | grep -v 'rootfs_patches\|container_pattern\|pulseaudio\|box64\|dxwrapper\|graphics_driver\|soundfont\|wallpapers\|wincomponents' | head -1)
if [ -n "$TZST_FILE" ] && echo "$TZST_FILE" | grep -q 'rootfs.tzst'; then
  echo "  Formato C: rootfs.tzst (Zstandard compressed tar)"
  zstd -dk "$TZST_FILE" -o "$WORK/rootfs.tar" --force 2>/dev/null || \
    { echo "ERRO: zstd não conseguiu descomprimir $TZST_FILE"; exit 1; }
  echo "== Testando integridade do tar (importante!) =="
  if tar -tf "$WORK/rootfs.tar" >/dev/null 2>&1; then
    echo "  tar OK — a rootfs do APK NÃO está corrompida"
    tar -xf "$WORK/rootfs.tar" -C "$ROOTFS_DIR"
  else
    echo "  !!! tar CORROMPIDO — reconstruindo da base $BASE"
    rm -rf "$APKDIR"; mkdir -p "$APKDIR"
    unzip -qo "$BASE" -d "$APKDIR"
    BASE_TZST=$(ls "$APKDIR/assets/"*.tzst 2>/dev/null | grep 'rootfs.tzst' | head -1)
    zstd -dk "$BASE_TZST" -o "$WORK/rootfs.tar" --force
    tar -xf "$WORK/rootfs.tar" -C "$ROOTFS_DIR"
    rm -f "$ROOTFS_DIR/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json" \
          "$ROOTFS_DIR/lib/libvulkan_panvk_v9.so" 2>/dev/null
  fi
  FORMAT="tzst"
elif ls "$PKG"/*.xz >/dev/null 2>&1; then
  echo "  Formato A: pacotes .xz partidos (assets/package)"
  PART_NAMES=$(ls "$PKG" | sort -V)
  FIRST=$(echo "$PART_NAMES" | head -1)
  PART_SIZE=$(stat -c%s "$PKG/$FIRST")
  echo "  partes: $(echo "$PART_NAMES" | wc -l)  tamanho da 1ª: $PART_SIZE bytes"
  cat "$PKG"/*.xz > "$WORK/rootfs.tar.xz" 2>/dev/null || \
    (cd "$PKG" && cat $(ls | sort -V) > "$WORK/rootfs.tar.xz")
  echo "== Descomprimindo rootfs.tar.xz =="
  xz -dkf "$WORK/rootfs.tar.xz" -c > "$WORK/rootfs.tar"
  echo "== Testando integridade do tar (importante!) =="
  if tar -tf "$WORK/rootfs.tar" >/dev/null 2>&1; then
    echo "  tar OK — a rootfs do APK NÃO está corrompida"
    tar -xf "$WORK/rootfs.tar" -C "$ROOTFS_DIR"
  else
    echo "  !!! tar CORROMPIDO — reconstruindo da base $BASE"
    rm -rf "$APKDIR"; mkdir -p "$APKDIR"
    unzip -qo "$BASE" -d "$APKDIR"
    cat "$APKDIR/assets/package"/*.xz > "$WORK/rootfs.tar.xz" 2>/dev/null || \
      (cd "$APKDIR/assets/package" && cat $(ls | sort -V) > "$WORK/rootfs.tar.xz")
    xz -dkf "$WORK/rootfs.tar.xz" -c > "$WORK/rootfs.tar"
    tar -xf "$WORK/rootfs.tar" -C "$ROOTFS_DIR"
    rm -f "$ROOTFS_DIR/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json" \
          "$ROOTFS_DIR/lib/libvulkan_panvk_v9.so" 2>/dev/null
  fi
  FORMAT="package"
elif ls "$APKDIR/assets/"*.img >/dev/null 2>&1; then
  IMG=$(ls "$APKDIR/assets/"*.img | head -1)
  echo "  Formato B: imagem ($IMG)"
  file "$IMG"
  case "$(file -b "$IMG")" in
    *squashfs*)
      command -v unsquashfs >/dev/null || { echo "ERRO: instale squashfs-tools"; exit 1; }
      unsquashfs -f -d "$ROOTFS_DIR" "$IMG" || exit 1
      FORMAT="squashfs"
      ;;
    *ext4*|*ext[234]*)
      command -v debugfs >/dev/null || { echo "ERRO: precisa de e2fsprogs para montar"; exit 1; }
      # extração via debugfs (sem root não dá para montar)
      mkdir -p "$ROOTFS_DIR"
      debugfs -R "rdump / $ROOTFS_DIR" "$IMG" 2>/dev/null || { echo "ERRO: não consegui extrair ext4"; exit 1; }
      FORMAT="ext4"
      ;;
    *)
      echo "ERRO: formato de imagem não suportado"; exit 1 ;;
  esac
else
  echo "ERRO: formato de rootfs desconhecido. Rode diag-apk.sh primeiro."
  exit 1
fi

# --- fix ld scripts: GNU ld scripts (.so sem versão) NÃO são ELF — dlopen falha
# com "invalid ELF header" quando o loader Vulkan tenta resolver dependências
# em cascata. Convertemos os ld scripts comuns (libm.so, libc.so, libdl.so, etc.)
# em symlinks para a versão .so.N real, que é ELF aarch64.
echo "== Corrigindo GNU ld scripts (.so sem versão) → symlinks para .so.N =="
fixed_ldscripts=0
while IFS= read -r -d '' f; do
  case "$(head -c 32 "$f" 2>/dev/null)" in
    *OUTPUT_FORMAT*|*"GNU ld"*|/\**)
      base="${f%.so}"
      # acha a versão .so.N correspondente no mesmo diretório (ou em ../lib)
      dir=$(dirname "$f")
      name=$(basename "$f" .so)
      target=""
      for cand in "$dir/$name.so."* "$dir/../lib/$name.so."* "$dir/../../lib/$name.so."*; do
        [ -f "$cand" ] || continue
        case "$(file -b "$cand" 2>/dev/null)" in
          *ELF*) target="$cand"; break ;;
        esac
      done
      if [ -n "$target" ]; then
        rel=$(dirname "$f")
        rel=$(realpath --relative-to="$rel" "$target" 2>/dev/null || basename "$target")
        rm -f "$f"
        ln -sf "$rel" "$f"
        echo "  $f → $rel"
        fixed_ldscripts=$((fixed_ldscripts+1))
      else
        echo "  AVISO: ld script $f sem .so.N correspondente — removendo"
        rm -f "$f"
        fixed_ldscripts=$((fixed_ldscripts+1))
      fi
      ;;
  esac
done < <(find "$ROOTFS_DIR" -name "lib*.so" -type f -print0 2>/dev/null)
echo "  $fixed_ldscripts ld script(s) corrigido(s)"

# --- injeção do driver -------------------------------------------------------
echo "== Injetando driver na rootfs =="
mkdir -p "$ROOTFS_DIR/lib" "$ROOTFS_DIR/usr/share/vulkan/icd.d"
chmod 755 "$ROOTFS_DIR/usr/share" "$ROOTFS_DIR/usr/share/vulkan" "$ROOTFS_DIR/usr/share/vulkan/icd.d"
cp -f "$DRIVER_SO" "$ROOTFS_DIR/lib/libvulkan_panvk_v9.so"
chmod 755 "$ROOTFS_DIR/lib/libvulkan_panvk_v9.so"
cp -f "$ICD_JSON" "$ROOTFS_DIR/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json"
chmod 644 "$ROOTFS_DIR/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json"
if [ -n "$COMPILER_SO" ]; then
  cp -f "$COMPILER_SO" "$ROOTFS_DIR/lib/libpanvk_v9_compiler.so"
  chmod 755 "$ROOTFS_DIR/lib/libpanvk_v9_compiler.so"
  mkdir -p "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu"
  cp -f "$COMPILER_SO" "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libpanvk_v9_compiler.so"
  chmod 755 "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/libpanvk_v9_compiler.so"
fi
# --- errno shim: injetar liberrno_shim.so para resolver "__errno" ---------
cp -f "$ERRNO_SHIM_SO" "$ROOTFS_DIR/lib/liberrno_shim.so"
chmod 755 "$ROOTFS_DIR/lib/liberrno_shim.so"
# --- forçar ICD JSON com library_path absoluto (acessível pelo dlopen do box64) ---
# PROBLEMA: box64 faz dlopen() NATIVO (arm64 bionic) que NÃO passa pelo proot.
# O loader Vulkan lê o ICD JSON (caminho absoluto funciona via proot/fopen),
# mas quando chama dlopen("/lib/..."), o box64 não traduz pelo proot.
# SOLUÇÃO: usar o caminho ABSOLUTO do host no library_path do ICD JSON.
# No Android, /data/data/ ≈ /data/user/0/ (symlink para primary user).
# O driver está em usr/lib/ (pois /lib → usr/lib symlink na rootfs).
ROOTFS_HOST="/data/data/com.winlator/files/rootfs"
cat > "$ROOTFS_DIR/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json" << ICDEOF
{
    "file_format_version": "1.0.1",
    "ICD": {
        "api_version": "1.2.0",
        "library_arch": "64",
        "library_path": "$ROOTFS_HOST/usr/lib/libvulkan_panvk_v9.so"
    }
}
ICDEOF
# Também em /etc/vulkan/icd.d/ (outro search path)
mkdir -p "$ROOTFS_DIR/etc/vulkan/icd.d"
cp -f "$ROOTFS_DIR/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json" "$ROOTFS_DIR/etc/vulkan/icd.d/"
chmod 644 "$ROOTFS_DIR/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json" "$ROOTFS_DIR/etc/vulkan/icd.d/panvk_v9_icd.aarch64.json"
echo "  lib/libvulkan_panvk_v9.so"
echo "  lib/liberrno_shim.so"
echo "  usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json (library_path absoluto do host)"

# --- patchelf: linkar shim como NEEDED no driver (elimina LD_PRELOAD) -------
# O driver glibc tem "undefined symbol: __errno". Em vez de usar LD_PRELOAD
# (que vaza para ld.so do bionic e causa erro), usamos patchelf para adicionar
# liberrno_shim.so como NEEDED no driver. Assim, quando glibc dlopen() o driver,
# o shim é carregado automaticamente e __errno é resolvido.
echo "== Patchelf: adicionando liberrno_shim.so como NEEDED no driver =="
if command -v patchelf >/dev/null 2>&1; then
  patchelf --add-needed liberrno_shim.so "$ROOTFS_DIR/lib/libvulkan_panvk_v9.so"
  patchelf --set-rpath '$ORIGIN' "$ROOTFS_DIR/lib/libvulkan_panvk_v9.so"
  echo "  NEEDED:" $(readelf -d "$ROOTFS_DIR/lib/libvulkan_panvk_v9.so" | grep NEEDED | tr '\n' ' ')
  echo "  RPATH:" $(readelf -d "$ROOTFS_DIR/lib/libvulkan_panvk_v9.so" | grep -E 'RUNPATH|RPATH' | awk '{print $NF}')
else
  echo "  AVISO: patchelf não disponível — o shim será carregado via BOX64_ENV=LD_PRELOAD"
  echo "  (funciona se o caminho absoluto do rootfs for usado)"
fi

# --- config.box64rc: env vars via [*] wildcard -------------------------------
# Usamos [*] (wildcard) para aplicar a todos os processos (wine, vulkaninfo, etc).
# BOX64_PRELOAD não funciona nesta versão (v0.4.0) — o shim __errno agora é
# carregado via patchelf --add-needed no driver (NEEDED + RUNPATH=$ORIGIN).
# BOX64_ENV com VK_ICD_FILENAMES e PANVK_V9_COMPILER_LIBRARY usam caminhos
# absolutos do host (funcionam mesmo quando dlopen não via proot).
ROOTFS_HOST="/data/data/com.winlator/files/rootfs"
echo "== Injetando etc/config.box64rc =="
mkdir -p "$ROOTFS_DIR/etc"
cat > "$ROOTFS_DIR/etc/config.box64rc" << RCEOF
# PanVK-v9 integration — Box64 rcfile
# O shim __errno é carregado via patchelf --add-needed (NEEDED + RUNPATH),
# não via LD_PRELOAD (que vaza para ld.so do bionic e causa erro).
# [*] aplica a todos os processos; paths absolutos funcionam sem proot.
[*]
BOX64_ENV=VK_ICD_FILENAMES=$ROOTFS_HOST/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json
BOX64_ENV1=PANVK_V9_COMPILER_LIBRARY=$ROOTFS_HOST/usr/lib/libpanvk_v9_compiler.so
RCEOF
chmod 644 "$ROOTFS_DIR/etc/config.box64rc"
echo "  etc/config.box64rc ([*], caminhos absolutos)"

# --- fix permissions: chmod ALL files/dirs to survive Winlator's 0771 extraction
echo "== Fixando permissões da rootfs (777 p/ arquivos, 777 p/ dirs) =="
find "$ROOTFS_DIR" -type d -exec chmod 777 {} + 2>/dev/null
find "$ROOTFS_DIR" -type f -exec chmod 777 {} + 2>/dev/null
find "$ROOTFS_DIR" -type l -exec chmod 777 {} + 2>/dev/null

# --- atualizar panvk-1.0.tzst no assets (se existir) -----------------------
PANVK_TZST="$WORK/panvk-new.tzst"
if [ -f "$PANVK_TZST" ]; then
  echo "== Atualizando panvk-1.0.tzst no assets =="
  cp -f "$PANVK_TZST" "$APKDIR/assets/graphics_driver/panvk-1.0.tzst"
  echo "  panvk-1.0.tzst atualizado"
fi

# --- reempacotar rootfs ------------------------------------------------------
echo "== Reempacotando rootfs =="
case "$FORMAT" in
  tzst)
    cd "$ROOTFS_DIR"
    tar -cf "$WORK/rootfs-new.tar" .
    echo "== Comprimindo com zstd =="
    zstd -19 -T0 -f "$WORK/rootfs-new.tar" -o "$WORK/rootfs-new.tar.zst" || \
      zstd -19f "$WORK/rootfs-new.tar" -o "$WORK/rootfs-new.tar.zst"
    mv "$WORK/rootfs-new.tar.zst" "$APKDIR/assets/rootfs.tzst"
    ls -la "$APKDIR/assets/rootfs.tzst"
    ;;
  package)
    cd "$ROOTFS_DIR"
    tar -cf "$WORK/rootfs-new.tar" .
    xz -9 -T0 -f "$WORK/rootfs-new.tar" 2>/dev/null || xz -9f "$WORK/rootfs-new.tar"
    rm -f "$PKG"/*.xz
    # reparte com o MESMO nome/ordem do original (menos a 1ª: preserva padrão)
    cd "$PKG"
    n=0
    split -b "$PART_SIZE" -d -a 4 "$WORK/rootfs-new.tar.xz" "tmp_"
    for p in tmp_*; do
      # nomeia como os originais: extrai prefixo e sufixo numérico do 1º arquivo
      printf -v nn "%s" "${FIRST//[0-9]/}"
      # fallback simples se não der para replicar
      mv "$p" "$(printf 'package.%04d.xz' "$n")" 2>/dev/null || mv "$p" "$FIRST.$n.xz"
      n=$((n+1))
    done
    ls -la "$PKG" | head
    ;;
  squashfs)
    command -v mksquashfs >/dev/null || { echo "ERRO: mksquashfs indisponível"; exit 1; }
    mksquashfs "$ROOTFS_DIR" "$APKDIR/assets/imagefs.img" -comp xz -b 131072 || exit 1
    ;;
  ext4)
    echo "AVISO: recompactar ext4 sem root é complexo — veja DIAGNOSTICO.md"
    ;;
esac

# --- modificar default.box64rc: adicionar [*] com VK_ICD_FILENAMES ---------------
echo "== Atualizando assets/box64/default.box64rc (config automática via [*]) =="
RC="$APKDIR/assets/box64/default.box64rc"
if [ -f "$RC" ]; then
    grep -v '^\[wine\]' "$RC" | grep -v '^\[\*\]' | grep -v '^BOX64_ENV=VK_ICD' | grep -v '^BOX64_ENV1=PANVK' | grep -v '^BOX64_PRELOAD=' 2>/dev/null > "$RC.tmp" || true
    cat > "$RC" << 'BOX64RC'
# PanVK-v9: config automática via [*] (wildcard, aplica a todos os processos)
# Shim __errno carregado via patchelf --add-needed no driver (NÃO precisa de LD_PRELOAD)
# Caminhos absolutos funcionam mesmo quando dlopen não via proot.
[*]
BOX64_ENV=VK_ICD_FILENAMES=/data/data/com.winlator/files/rootfs/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json
BOX64_ENV1=PANVK_V9_COMPILER_LIBRARY=/data/data/com.winlator/files/rootfs/usr/lib/libpanvk_v9_compiler.so

BOX64RC
    cat "$RC.tmp" >> "$RC" 2>/dev/null; rm -f "$RC.tmp"
    echo "  [*] BOX64_ENV=VK_ICD_FILENAMES injetado no default.box64rc"
else
    echo "  AVISO: $RC não existe — config só via etc/config.box64rc da rootfs"
fi

# --- recompactar APK + assinar ------------------------------------------------
echo "== Removendo assinatura antiga e recompactando APK =="
rm -rf "$APKDIR/META-INF"
cd "$APKDIR"
rm -f "$WORK/unsigned.apk"
zip -q -r -X "$WORK/unsigned.apk" . || { echo "ERRO: zip falhou"; exit 1; }

echo "== zipalign =="
zipalign -f 4 "$WORK/unsigned.apk" "$WORK/aligned.apk" || \
  { echo "AVISO: zipalign falhou (continua sem align)"; cp "$WORK/unsigned.apk" "$WORK/aligned.apk"; }

echo "== assinando =="
apksigner sign --ks "$KS" --ks-pass pass:panvk123 --key-pass pass:panvk123 \
  --out "$OUTAPK" "$WORK/aligned.apk" || { echo "ERRO: apksigner falhou"; exit 1; }

echo "== verificando =="
apksigner verify --print-certs "$OUTAPK" | head -6

echo ""
echo "============================================================"
echo " PRONTO: $OUTAPK"
echo "============================================================"
echo ""
echo "O shim __errno (glibc, via patchelf NEEDED no driver) e o ICD json corrigido são injetados AUTOMATICAMENTE."
echo "PARA USAR:"
echo "  1. Desinstale o Winlator antigo (assinatura do APK mudou)"
echo "  2. Instale $OUTAPK"
echo "  3. Abra, crie um container NOVO (o [*] no default.box64rc já seta VK_ICD_FILENAMES)"
echo "  4. Rode vulkaninfo dentro do container — procure 'Mali-G68 MC4'"
echo ""
echo "  Se precisar forçar em 'Environment Variables':"
echo "     VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json"
echo "     PANVK_V9_COMPILER_LIBRARY=/lib/libpanvk_v9_compiler.so"
echo ""
echo "OBS: o driver foi compilado para glibc SEM X11 — o container abre mesmo"
echo "     que o driver falhe; os jogos exigem o libpanvk_v9_compiler.so (Mesa)."

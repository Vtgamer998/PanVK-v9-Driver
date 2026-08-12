#!/bin/bash
# ============================================================================
# teste-container.sh — injeta o PanVK-v9 no container Winlator ATUAL e roda
# vulkaninfo com VK_ICD_FILENAMES já configurado.
#
# COMO USAR (2 passos):
#
#   1) No TERMUX (host), deixe os arquivos no storage compartilhado:
#        cp ~/panvk-work/icd.d/panvk_v9_icd.aarch64.json     ~/storage/downloads/
#        cp ~/panvk-work/glibc-build/libvulkan_panvk_v9.so   ~/storage/downloads/
#        cp ~/panvk-work/glibc-build/libpanvk_v9_compiler.so ~/storage/downloads/
#        cp ~/panvk-work/teste-container.sh                  ~/storage/downloads/
#
#   2) No TERMINAL DO WINLATOR (dentro do container), rode:
#        sh /sdcard/Download/teste-container.sh
#      (use o caminho certo do storage se o /sdcard não existir:
#       o script procura sozinho)
# ============================================================================
set -u

# --- localiza o storage compartilhado (tenta os montes comuns) --------------
SRC=""
for d in /sdcard/Download /storage/emulated/0/Download /mnt/shared/Download /mnt/sdcard/Download; do
  if [ -f "$d/panvk_v9_icd.aarch64.json" ]; then SRC="$d"; break; fi
done

if [ -z "$SRC" ]; then
  echo "ERRO: não achei os arquivos no storage compartilhado."
  echo "      Copie do Termux para ~/storage/downloads/ e rode de novo."
  exit 1
fi
echo "== storage compartilhado encontrado: $SRC =="

VK_DIR=/usr/share/vulkan/icd.d

echo ""
echo "== [1/5] Copiando arquivos para o container =="
mkdir -p "$VK_DIR"
cp -f "$SRC/panvk_v9_icd.aarch64.json" "$VK_DIR/panvk_v9_icd.aarch64.json"
cp -f "$SRC/libvulkan_panvk_v9.so" /lib/libvulkan_panvk_v9.so
if [ -f "$SRC/libpanvk_v9_compiler.so" ]; then
  cp -f "$SRC/libpanvk_v9_compiler.so" /lib/libpanvk_v9_compiler.so
  echo "  compiler.so: copiado"
else
  echo "  compiler.so: não está no storage (container abre, mas DXVK não cria pipelines)"
fi
chmod 644 "$VK_DIR/panvk_v9_icd.aarch64.json"
chmod 755 /lib/libvulkan_panvk_v9.so
chmod 755 /lib/libpanvk_v9_compiler.so 2>/dev/null

echo ""
echo "== [2/5] Conferindo o que foi instalado =="
ls -la "$VK_DIR/" /lib/libvulkan_panvk_v9.so /lib/libpanvk_v9_compiler.so 2>/dev/null
echo "--- conteúdo do ICD json (library_path tem que ser /lib/...) ---"
cat "$VK_DIR/panvk_v9_icd.aarch64.json"
echo "--- tipo do driver (tem que ser GNU/Linux, NUNCA Android) ---"
file /lib/libvulkan_panvk_v9.so 2>/dev/null || readelf -h /lib/libvulkan_panvk_v9.so 2>/dev/null | head -5

echo ""
echo "== [3/5] Exportando VK_ICD_FILENAMES =="
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json
export VK_LOADER_DEBUG=error,debug
echo "  VK_ICD_FILENAMES=$VK_ICD_FILENAMES"

echo ""
echo "== [4/5] Rodando vulkaninfo =="
if command -v vulkaninfo >/dev/null 2>&1; then
  vulkaninfo > /tmp/vkinfo.log 2>&1
  echo "--- resumo (procure Mali-G68 / panvk / ERROR) ---"
  grep -iE 'deviceName|driverName|Mali|panvk|ERROR' /tmp/vkinfo.log | head -25
  echo "--- últimas linhas do log ---"
  tail -5 /tmp/vkinfo.log
else
  echo "  vulkaninfo não instalado no container."
  echo "  Opções: apt install vulkan-tools  |  ou rode um jogo de teste e cole o log"
fi

echo ""
echo "== [5/5] Fim. =="
echo "  Se apareceu 'Mali-G68' no resumo, o driver PanVK carregou!"
echo "  Se apareceu ERROR de loader, cole aqui a saída do passo [4/5]."

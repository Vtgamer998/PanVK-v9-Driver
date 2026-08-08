# PanVK-v9 — Driver Vulkan para Mali-G68 MC4 (formato Kimchi/AdrenoTools)

Pacote no mesmo formato dos drivers Turnip-Kimchi (`lib*.so` + `meta.json`),
mas para **GPU ARM Mali-G68 MC4 (MediaTek MT6893, Valhall v9)**.

## ⚠️ LEIA ANTES — Por que NÃO funciona no AdrenoTools/Winlator-Turnip

| Item | Realidade |
|---|---|
| Turnip / Kimchi / AdrenoTools | **Exclusivo para GPUs Qualcomm Adreno**. |
| Seu GPU | **ARM Mali-G68 MC4** — não é Adreno. |
| AdrenoTools | Injeta o driver via a interface **KGSL** do kernel Qualcomm. Em Mali **não existe KGSL** → o driver **não carrega**. |
| Winlator em Mali | Usa o driver Vulkan Mali de sistema + DXVK/VKD3D, ou **VirGL** (software). Não usa Turnip. |
| Este driver | Prova de conceito: renderiza um triângulo sólido via pipeline Vulkan completo na GPU real, usando o **compilador SPIR-V→Valhall do Mesa 26.2**. **Não** é uma implementação Vulkan completa — **não** roda jogos (DXVK/VKD3D). |

**Resumo**: para rodar no Winlator com Turnip, você precisa de um aparelho
**Snapdragon/Adreno**. Neste aparelho Mali, o caminho viável é o driver Vulkan
Mali do sistema; este projeto é um estudo/ICD experimental.

## Conteúdo do pacote

```
libvulkan_panvk_v9.so   → ICD Vulkan (Mali-G68 MC4, Valhall v9)
libpanvk_v9_compiler.so → compilador SPIR-V→Valhall v9 (backend Mesa 26.2)
vulkan.panvk_v9.json    → manifesto ICD (Vulkan Loader)
meta.json               → metadados no formato AdrenoTools
version.txt             → versão
INSTALL_WINLATOR.sh     → roteiro de instalação manual no Winlator
vulkan.panvk_v9.container.json → manifesto p/ injeção no rootfs do container
dense_map               → render direto (ret=0, 256/256 verdes)
test_vulkan_loader_icd  → pipeline Vulkan completo via dlopen
test_loader_images      → memória/imagem/copy/clear/blit via loader (+ .c)
```

## Como instalar MANUALMENTE (para apps que usam Vulkan Loader)

O Vulkan Loader do Android procura ICDs em `/vendor/etc/vulkan/` e
`/data/adb/vulkan/`. **Sem root**, use `VK_ICD_FILENAMES`:

```bash
export VK_ICD_FILENAMES=/caminho/para/vulkan.panvk_v9.json
export LD_LIBRARY_PATH=/caminho/para:$LD_LIBRARY_PATH
```

No Termux (onde o loader encontra o .json na mesma pasta):

```bash
cd PanVK-v9-Kimchi
VK_ICD_FILENAMES=./vulkan.panvk_v9.json LD_LIBRARY_PATH=. ./teste_vulkan
```

## Verificação rápida (Termux, neste aparelho)

```bash
# 1) Render direto via job chain kbase
../vulkan-driver/dense_map 16 16          # ret=0, 256/256 verdes

# 2) Pipeline Vulkan completo via ICD (dlopen)
../vulkan-driver/test_vulkan_loader_icd   # PASSED CLEANLY, pixel 0xff00ff00

# 3) Memória/imagem (estilo panvk): buffer↔imagem, clear, blit, cópia
../vulkan-driver/test_loader_images       # PASSED CLEANLY
```

## Compilador SPIR-V→Valhall (Mesa 26.2)

O driver carrega `libpanvk_v9_compiler.so` (backend **Mesa 26.2 / Panfrost
Valhall v9**, GPU ID `0x90001000`) que compila SPIR-V arbitrário para ISA
Valhall na GPU real. O `test_vulkan_loader_icd` recebe `vs.spv` e `fs.spv`,
compila ambos no Mesa e renderiza **verde sólido (0xFF00FF00)**.

```bash
../vulkan-driver/test_vulkan_loader_icd vs.spv fs.spv   # PASSED CLEANLY!
../vulkan-driver/test_fs_bin                            # FS compilado no Mesa, na GPU
```

## Suporte a imagens (estilo panvk)

Layout linear real no padrão `panvk_image.c` (row pitch 64B, mips/layers/depth),
`vkGetImageSubresourceLayout`, e **caminho de cópia/clear executável**:
`vkCmdCopyBufferToImage`, `vkCmdCopyImageToBuffer`, `vkCmdCopyImage`,
`vkCmdBlitImage` (NEAREST) e `vkCmdClearColorImage` — via memcpy no BO.
`VkPhysicalDeviceMemoryProperties` corrigido (2 tipos, heap 4GB).
Os dados de imagem são movidos por CPU; shaders VS/FS arbitrários compilam e
rodam no Mesa (ver seção "Compilador SPIR-V→Valhall").

## Limitações técnicas conhecidas

1. **Multi-tile**: o fragmento só processa a primeira tile 16x16 (a referência
   vendor `replay_egl_triangle` tem o mesmo comportamento).
2. **Terminação do fragmento**: o kernel MTK r49 nunca emite `0x1 DONE` para
   polygon-list; sempre `0x4`/`0x4002` (aceitos como sucesso). Isso pode deixar
   o slot em JOB_READ_FAULT entre frames no mesmo processo.
3. **SELinux**: página de shader mapeada RW + `GPU_EX` (PROT_EXEC de CPU negado).
4. **Não é completo**: sem swapchain plena, sem extensões/features Vulkan 1.1+
   exigidas por DXVK/VKD3D → **não roda jogos Windows no Winlator**.
5. **GPU em JOB_READ_FAULT residual**: após o 1º frame real (`PANVK_DRY_RUN=0`)
   a GPU trava até o celular ser reiniciado. Use `PANVK_DRY_RUN=1` por padrão.

## Testes seguros (2026-08-02)

O driver agora expõe **~150 entry points Vulkan** (adicionados nesta sessão:
`vkCreateRenderPass2`, events, query pools, `vkCmdPushConstants`, `vkCmdSetDepthBias`,
`vkCmdDispatch`, memória flush/invalidate, sync2/vkQueueSubmit2, etc.). A validação
é feita **sem risco de reiniciar o celular**:

```bash
./test_ep_probe                      # resolve 28/28 entry points novos (0 missing)
PANVK_DRY_RUN=1 ./test_vulkan_loader_icd   # pipeline completo, sem /dev/mali0, pixel verde
PANVK_DRY_RUN=1 ./test_loader_images       # memória/imagem/copy/clear/blit
./test_panvk_v9_compiler ./libpanvk_v9_compiler.so vs.spv fs.spv   # CPU-only
```

Teste real GPU (somente após **reboot** do celular, 1 frame):
```bash
PANVK_DRY_RUN=0 PANVK_SUBMIT_TIMEOUT_MS=1500 V9_SKIP_POST_FLUSH=1 timeout 25 ./dense_map 16 16
```

**Nota sobre o objetivo "jogo DX no Winlator":** DXVK (DX9/11) e vkd3d-proton (DX12)
traduzem DirectX→Vulkan completo. O driver precisa de quase-totalidade da API e
compute de verdade no Valhall. O PanVK do Mesa rejeita Mali-G68/G77 (arch 9) e só fala
com DRM pan/thor — sendo **inutilizável** neste terminal; logo o caminho é completar
o ICD custom (que fala /dev/mali* kbase), não portar o Mesa.

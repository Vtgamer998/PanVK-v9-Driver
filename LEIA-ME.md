# PanVK-v9 → Winlator — Kit de Integração

Kit completo para integrar o driver **PanVK-v9 (Mali-G68 MC4)** no APK do
Winlator 11.1 (base brunodev85) e corrigir o problema do
`Winlator_11.1_mali_panvk_debug_6_signed.apk` (driver carrega, container não abre).

## Arquivos

| Arquivo | Função |
|---|---|
| `diag-apk.sh` | Diagnostica o APK modificado vs base (rootfs, driver bionic/glibc, assinatura, `__errno`) |
| `cross-build-glibc.sh` | Recompila o driver para **glibc** (se o cross-compiler existir) |
| `cross-build-proot.sh` | **Recomendado** — compila para glibc num Debian via proot-distro (sem X11, stubs automáticos) |
| `integra.sh` | Injeta o driver + `liberrno_shim.so` na rootfs do APK + recompacta + zipalign + assina |
| `errno_shim.c` | Fonte do shim que exporta `__errno` para o PanVK |
| `icd.d/panvk_v9_icd.aarch64.json` | Manifesto ICD (caminho do container: `/lib/...`) |
| `DIAGNOSTICO.md` | Explicação de por que o container não abre + checklist |
| `panvk-v9-fonte/` | (opcional) cópia dos fontes do driver g5 |

## Como usar (no Termux)

```bash
# 0) instalar dependências
pkg update && pkg install zip unzip xz-utils binutils squashfs-tools \
  zipalign apksigner openjdk-17 proot-distro

# 1) diagnosticar o APK atual (debug_6_signed) — SEMPRE primeiro
bash ~/panvk-work/diag-apk.sh

# 2) compilar o driver para glibc (Debian via proot-distro)
bash ~/panvk-work/cross-build-proot.sh
#    (alternativa: cross-build-glibc.sh, se houver cross-compiler)

# 3) injetar + assinar (gera Winlator_11.1_mali_panvk_fixed.apk)
#    O integra.sh agora COMPILA automaticamente liberrno_shim.so
bash ~/panvk-work/integra.sh

# 4) instalar o APK final (desinstalar o Winlator antigo antes — chave mudou)
#    no container, em Environment Variables, adicione:
#    LD_PRELOAD=/lib/liberrno_shim.so
#    VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json
#    BOX64_NOSYMBOLIC=1
```

## Resumo técnico do problema do container

1. **`__errno` não exportado pela glibc arm64 da rootfs** — a glibc arm64 da
   rootfs não tem `__errno` no símbolos dinâmicos. O PanVK o referencia →
   `symbol lookup error: undefined symbol: __errno`. Fix: `liberrno_shim.so`
   (compilado e injetado automaticamente pelo `integra.sh`) +
   `LD_PRELOAD=/lib/liberrno_shim.so` no container.

2. **Driver bionic** (compilado no Termux) não carrega dentro da rootfs glibc → recompilar (`cross-build-proot.sh`).

3. **X11 acoplado** ao `.so` (o build inclui o programa de teste `panvk_v9_x11.c` e linka `-lX11 -lxcb`) → build sem X11 (stubs AArch64).

4. **ICD json com caminho do host** (`/data/data/com.winlator/...`) em vez do caminho do container (`/lib/...`).

5. **Rootfs corrompida** em injeções manuais → `diag-apk.sh` detecta; `integra.sh` reconstrói da base.

6. **Faltando `libpanvk_v9_compiler.so`** (compilador Mesa) → container abre, mas pipelines/DXVK falham.

## Atenção

- Driver experimental: risco de travamento/reboot do aparelho (ver `g5/README.md`).
- Não roda jogos ainda (prova de conceito) — o objetivo aqui é o **container abrir com o driver carregado**.
- **FONTE CORRETA do driver: `~/storage/downloads/g5/` (mais novo).** A pasta
  `~/storage/downloads/vulkan-driver/` é a **versão antiga** (reboot/travamento
  ao falar com a GPU) — NÃO usar nada de lá. Todos os scripts compilam de `g5/src`.
- Se o telefone já travou com o driver antigo, ele pode ter deixado o marcador
  de GPU wedged em `/data/local/tmp/.panvk_gpu_wedged` → **reinicie o telefone**
  antes de testar o container (o driver novo recusa abrir a GPU até o reboot).

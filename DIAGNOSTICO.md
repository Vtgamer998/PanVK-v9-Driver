# DIAGNÓSTICO — "o driver carrega mas o container não abre"

## ✅ CAUSA CONFIRMADA (diagnóstico real do APK)

> O driver `libvulkan_panvk_v9.so` foi compilado para **bionic (Android)**
> (NEEDED: `libdl.so`, `libc.so`), mas o container Winlator é **glibc**
> (rootfs tem `libdl.so.2`, `libc.so.6`, loader `libvulkan.so.1.3.301`).
> O loader não carrega o driver → container trava.

## Outras causas possíveis (se o problema persistir depois do fix)

### 0. `__errno` não resolvido na glibc arm64 da rootfs (CAUSA REAL DO CRASH)
O `libvulkan_panvk_v9.so` (glibc arm64) referencia `__errno` como símbolo
externo. A glibc arm64 na rootfs do Winlator **não exporta** `__errno` no
dynamic symbol table (verificado: `nm -D rootfs/lib/aarch64-linux-gnu/libc.so.6`
→ nenhum `__errno`).

**Prova** (logs.txt linha 300):
```
TestD3D.exe: symbol lookup error: .../libvulkan_panvk_v9.so: undefined symbol: __errno
```

**Correção**: `liberrno_shim.so` é injetado na rootfs em `/lib/liberrno_shim.so`
e carregado via `LD_PRELOAD=/lib/liberrno_shim.so` nas Environment Variables
do container. O shim exporta `__errno` e `__errno_location` como thread-local,
compatível com a ABI glibc arm64.

Alternativa: `BOX64_NOSYMBOLIC=1` também ajuda em alguns casos de resolução
de símbolos dentro do stack Box64.

---

### 1. Driver compilado para BIONIC (Android), não GLIBC
O `build.sh` do driver usa o `clang` do Termux → o `.so` é linkado contra a
libc **bionic** do Android. Dentro do Winlator, a rootfs é **Debian/glibc**.
Quando o loader Vulkan do container tenta `dlopen` desse `.so`, ele falha
(procura `libc.so` no lugar de `libc.so.6`) ou crasha o processo do Wine.

**Prova rápida** (Termux):
```bash
file ~/storage/downloads/g5/build/libvulkan_panvk_v9.so
# se disser "ELF ... for Android (API ...)" → é bionic
# se disser "ELF ... GNU/Linux" → é glibc
```

**Correção**: `bash ~/panvk-work/cross-build-glibc.sh`
(recompila para glibc com `aarch64-linux-gnu-gcc`, sem X11).

### 2. O driver depende de X11/xcb, mas foi feito para o backend WINE
O `build.sh` compila `panvk_v9_x11.c` DENTRO do `.so` e linka `-lX11 -lxcb`.
Isso dá ao driver dependências `libX11.so.6`/`libxcb.so.1` no container.
O Winlator usa o backend de apresentação **WINE** (sem X11), então o correto
é compilar **sem X11** (o script gera stubs em assembly e não linka X11).

### 3. Rootfs do APK corrompida na injeção
Se o APK foi modificado mexendo em `assets/package/*.xz` (o tar.xz partido da
rootfs) e o tar ficou inválido, o Winlator **não extrai a rootfs** e o
container não abre. Isso acontece quando se substitui os pacotes à mão.

**Prova**: `bash ~/panvk-work/diag-apk.sh` → se `tar CORROMPIDO`, é isto.
O `integra.sh` reconstrói a rootfs a partir do APK **base** limpo nesse caso.

### 4. ICD json com caminho que não existe no container
Os scripts antigos usavam caminhos do **host** Android:
`/data/data/com.winlator/files/rootfs/lib/...` — dentro do container (proot)
esse caminho NÃO existe. O caminho correto DENTRO do container é:
- json em `/usr/share/vulkan/icd.d/panvk_v9_icd.aarch64.json`
- `"library_path": "/lib/libvulkan_panvk_v9.so"`

### 5. libpanvk_v9_compiler.so ausente
O driver `dlopen` a `libpanvk_v9_compiler.so` (wrapper do compilador Mesa
26.2 SPIR-V→Valhall). Sem ela, `vkCreateGraphicsPipelines` falha e o DXVK
não renderiza. O container ABRE mesmo assim (Wine não precisa de pipeline
para bootar), mas é preciso ter esse arquivo junto para renderizar.
Caminho de busca do driver: `PANVK_V9_COMPILER_LIBRARY`,
`./libpanvk_v9_compiler.so`, `libpanvk_v9_compiler.so` (LD_LIBRARY_PATH).

### 6. Assinatura
Se o APK foi re-assinado com chave errada/incompleta, o Android pode aceitar
a instalação mas o runtime falhar. O `integra.sh` gera um keystore próprio e
assina com `apksigner` (v1+v2).

---

## Checklist de verificação (nessa ordem)

1. `bash ~/panvk-work/diag-apk.sh` → leia os 10 passos
2. `file` do `.so` injetado → glibc? bionic?
3. `readelf -d` do `.so` → pede `libc.so.6`? `libX11.so.6`? `libxcb.so.1`? `libc.so`?
   (pode pedir `libdl.so.2` — é glibc, OK; se pede `libdl.so` sem `.2` — é bionic)
4. **Verifique `__errno`** em logs.txt: procure `undefined symbol: __errno`
   Se encontrado → o `liberrno_shim.so` está faltando ou `LD_PRELOAD` não foi
   configurado. Rode `integra.sh` de novo (ele injeta o shim automaticamente).
5. A rootfs descomprime (`tar -tf` OK)?
6. O json do ICD está em `usr/share/vulkan/icd.d/` com caminho `/lib/...`?
7. `liberrno_shim.so` está em `/lib/` dentro da rootfs do APK?
8. Container tem `LD_PRELOAD=/lib/liberrno_shim.so` em Environment Variables?
9. Container: `vulkaninfo` dentro do container mostra "Mali-G68 MC4"?

---

## Expectativa realista

Pelo `g5/docs/REPORT.md`, este driver é um **prova de conceito**:
- ✅ Renderiza triângulo verde via pipeline Vulkan completo na GPU real
- ✅ ~150 entry points, fences/semáforos, imagens/cópia/clear
- ❌ Sem swapchain completa / compute real / extensões DXVK completas
- ❌ GPU pode entrar em JOB_READ_FAULT residual (exige reboot do telefone)

Ou seja: dá para fazer o **container abrir com o driver carregado**, mas
rodar jogos exige completar o driver (ou usar o driver do sistema).

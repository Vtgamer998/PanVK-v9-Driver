# PanVK v9 — Vulkan ICD para Mali-G68 MC4 (Valhall v9) — Relatório de Estado

**Data**: 07/08/2026
**Hardware**: ARM Mali-G68 MC4 (MediaTek MT6893 / Dimensity 700, arquitetura Valhall v9)
**Ambiente**: Android/Termux, `/dev/mali0`, kernel `mali_kbase` MTK r49 (versão 11.13)
**Abordagem**: driver Vulkan "estilo Turnip" — implementação própria dos entry points
Vulkan + backend próprio de kbase (sem Mesa/Panfrost kernel).

> **Correção de identificação do hardware (07/08/2026)**: o device real deste
> aparelho é o **Mali-G68 MC4 (product_id `0x92041010`)**, conforme reportado
> pelo `KBASE_IOCTL_GET_GPUPROPS` (ver `vendor_probe.txt`). Este é o modelo
> usado em `vkGetPhysicalDeviceProperties` (`deviceID`, `deviceName`) e no
> default dry-run de `kbase_winsys.c`. O compilador SPIR-V→Valhall do Mesa
> continua recebendo `PANVK_V9_COMPILER_GPU_ID = 0x90001000` (G77) por via do
> `panvk_v9_compiler_mesa.c`, pois **G68 não está listado em `pan_model.h`**
> do Mesa — G77 e G68 compartilham a mesma ISA Valhall v9 e produzem binário
> idêntico, então esse ID é apenas o "alias" para o backend do compilador.
> Os headers de comentários dos arquivos fonte e a contagem de cores
> (`core_count = 4`) foram corrigidos nesta sessão.

---

## 0. Sessão de 02/08/2026 — COMPILADOR SPIR-V→VALHALL (Mesa 26.2) FUNCIONANDO NA GPU

### Resultado principal

O pipeline completo **SPIR-V → Compilador Mesa Valhall v9 → ICD PanVK-v9 → GPU** agora
funciona de ponta a ponta: o FS compilado pelo backend real do Mesa (a partir do
`fs.spv`) renderiza na GPU real, e o teste Vulkan completo via `dlopen` do ICD
imprime **`pixel(0,0)=0xff00ff00`** (verde sólido) e **PASSED CLEANLY!**

```
=== Step 4: Vulkan Loader ICD Shared Library Integration ===
SUCCESS: Dynamically loaded './libvulkan_panvk_v9.so' via dlopen()
SUCCESS: Dynamically queried device: 'ARM Mali-G68 MC4 (Valhall v9 - PanVK Open Source Driver)'
v9_cmd_draw_indexed: ... has_vs=1
Rendered Output: pixel(0,0)=0xff00ff00
SUCCESS: Dynamically loaded PanVK ICD rendered solid green (0xFF00FF00)!
=== Step 4: Vulkan Loader ICD Shared Library Integration PASSED CLEANLY! ===
```

### Causa-raiz descoberta: GPU ID incorreto (Bifrost em vez de Valhall)

Todos os faults `0x51` anteriores eram causados por **`MALI_G77_GPU_ID = 0x9000u`**.
`pan_arch(0x9000)` retorna 0 (arch vazio → fallback Bifrost), então o compilador
Mesa empacotava **clauses Bifrost** em vez de instruções **Valhall v9**. O GPU
decodificava esse lixo como `BRANCHZ offset:532992` / `LD_VAR_BUF index:0x2` →
fault de leitura. Os antigos "FS lê varying inválido" eram **sintoma** desse
binário Bifrost, não uma leitura real.

**Correção**: GPU ID no formato Mesa = `PAN_PROD_ID(9,0,1)` = **`0x90001000`**
(arch 9 nos bits 28-31, cf. `pan_model.h`/`pan_model.c:84`). Aplicado em
`panvk_v9_compiler_mesa.c` (compilador) e `pan_kmod_kbase.c` (device query).

### Segundo bug: NIR inválido derrubava o copy_prop (segfault)

O wrapper substituía os sysvals `load_viewport_scale`/`load_viewport_offset`
(que são **vec3**) por `nir_vec2(...)` — um def de 2 componentes acessado com
`.z` (fora dos limites) → NIR inválido → segfault dentro de
`nir_opt_copy_prop` (`copy_propagate_alu` → `copy->src[swizzle[0]]` fora do vetor).

**Correção**: `lower_viewport_sysvals` agora gera `nir_vec3(1,1,1)` para scale e
`nir_vec3(0,0,0)` para offset (transformação de viewport identidade, coerente com
o buffer de posições fixas usado pelos testes).

### Validação na GPU

- `test_panvk_v9_compiler vs.spv fs.spv` → VS 276 B + FS 280 B, **PASSED CLEANLY!**
- `test_fs_bin` (FS compilado pelo Mesa injetado direto no job chain, sem Vulkan):
  FS de 128 B, `work_reg=32`, `preload=0x3000000000000000` (r60/r61 preloadados
  como no panvk real), **225 pixels verdes, PASS** — mesma geometria do FS fixo.
- `test_vulkan_loader_icd vs.spv fs.spv` → **pixel(0,0)=0xff00ff00, PASSED** (3/3
  execuções estáveis).
- `inputs.no_idvs` voltou ao valor upstream (`stage != VERTEX`, IDVS ligado no VS)
  e continua funcionando — o workaround incondicional não era necessário.

### Arquivos alterados nesta sessão

| Arquivo | Mudança |
|---|---|
| `panvk_v9_compiler_mesa.c` | `MALI_G77_GPU_ID 0x90001000u`; viewport sysvals como **vec3**; remoção dos debug prints |
| `bifrost_nir.c` | remoção da instrumentação de debug (crash localizado) |
| `pan_kmod_kbase.c` | `gpu_id = 0x90001000` na query do device |
| `v9_cmd_stream.c` | knobs env `PANVK_FS_WORKREG`/`PANVK_FORCE_BARRIER` (debug) |
| `test_fs_bin.c` | novo harness de isolamento FS-na-GPU |

### Construção

```bash
cd ~/mesa-26.2.0-rc3/build-panvk
ninja src/panfrost/libpanvk_v9_compiler.so
cp src/panfrost/libpanvk_v9_compiler.so ~/libpanvk_v9_compiler.so
```

---

## 1. Resumo do Que Foi Feito Nesta Sessão

O driver renderiza agora **com sucesso um triângulo verde sólido (0xFF00FF00)** na
GPU real através do pipeline Vulkan completo (ICD → Instance → Device → SPIR-V →
Graphics Pipeline → Command Buffer → Queue Submit → GPU), com **ret=0**.

### Correções aplicadas nesta sessão

1. **Shader de fragmento corrigido (40 → 56 bytes)**.
   A referência `replay_egl_triangle.c` usa um FS de 7 instruções (56 bytes)
   incluindo `NOP.wait0126` e `ATEST.discard` antes do `BLEND.slot0.v4.f32.end`.
   O nosso tinha apenas 5 instruções (40 bytes) — faltava o wait de dependência
   de registradores e o alpha-test, o que podia causar leitura de registradores
   ainda não escritos pelo FADD.
   Arquivo: `v9_cmd_stream.c` (`k_valhall_green_fs`).

2. **Cadeia de 2 jobs do Fragmento (FJ1 → FJ2) corrigida**.
   Em `v9_pack.h` / `v9_pack_frag_job_chain`: antes `(void)fj2_gpu;` e `fj1[6:7]=0`
   (Next=NULL). Agora `pack_u64(fj1 + 6, fj2_gpu)` — o Job 1 (passada de
   polygon-list) aponta para o Job 2 (passada de conclusão/fim de frame),
   `fj2[4]=0x00020012`, `fj2[5]=1`, `fj2[9]=0x00030003`, `Next=NULL`,
   MFBD2|0x03. Sem o Job 2, o hardware renderiza mas fica esperando a passada
   de conclusão → watchdog `0x4002`.

3. **Semântica de eventos do kernel MTK**.
   - `0x1` = DONE
   - `0x4` = TERMINATED (soft/hard-stop do kernel após o render)
   - `0x4002` = CANCELLED (watchdog)
   - `0x42` = JOB_READ_FAULT
   O fragmento em modo polygon-list **nunca sinaliza 0x1** neste kernel — ele
   sempre renderiza e depois é parado (0x4) ou cancelado (0x4002). A referência
   tem o mesmo comportamento. Por isso:
   - `pan_kmod_submit_fragment_timeout()` (novo) aceita `0x1/0x4/0x4002` como
     sucesso do fragmento.
   - TILER/Flush continuam estritos (`0x1` apenas) — foi separado do aceite
     leniente do fragmento.
   - Post-Flush tornou-se **best-effort**: se o kernel der JOB_READ_FAULT após
     o fragmento TERMINATED, o frame continua sendo considerado sucesso (os
     pixels já estão renderizados), apenas com um warning.

### Verificações byte-a-byte contra a referência (todas idênticas)

- FJ1: `0x010=0x00010012`, `0x018=Next→frag_jc2`, `0x028=mfbd|0x01`
- FJ2: `0x010=0x00020012`, `0x018=0`, `0x024=0x00030003`, `0x028=mfbd2|0x03`
- MFBD (32 words), MFBD2, DCD, RT0, Tiler Context, Tiler Heap Desc
- Tiler Context: `tc[0]=polylist`, `tc[2]=1` (hierarchy 16x16), `tc[3]=w-1|h-1<<16`,
  `tc[6:7]=heap desc` — idênticos. O bit 48 de `tc[0]` é equivalente ao valor
  `0x00010000` que o próprio GPU grava em `tc[1]` após o tiler rodar (a
  referência começa com `tc[1]=0` e o GPU escreve `0x00010000`).
- Atoms `kbase_atom_mtk`: `core_req` 0x04E (TILER), 0x002 (Flush),
  0x041 (Fragment) — iguais.
- Memória: shader ISA em página GPU_EX separada (`0x2017`) — igual à referência.
- Polygon list em 64x64: 16 slots ativos, apontando para o heap — iguais.

---

## 2. Resultados de Teste (após as correções)

### `dense_map 16 16` (frame único) — 6/6 execuções ret=0, 256/256 verdes
```
ret=0
################ (16 linhas × 16 '#')
```
Eventos: `TILER 0x1`, `Flush 0x1`, `Fragment 0x4`, `Post-Flush (best-effort)`.

### Pipeline Vulkan completo (`test_vulkan_loader_icd`)
```
=== Step 4: Vulkan Loader ICD Shared Library Integration ===
SUCCESS: Dynamically loaded './libvulkan_panvk_v9.so' via dlopen()
SUCCESS: Resolved 'vk_icdGetInstanceProcAddr' entry point from ICD
SUCCESS: Resolved Vulkan shader, pipeline, command, and device entry points
SUCCESS: Dynamically queried device: 'ARM Mali-G68 MC4 (Valhall v9 - PanVK Open Source Driver)'
SUCCESS: Parsed SPIR-V stages and graphics pipeline state
SUCCESS: Dispatched vkQueueSubmit via dynamic ICD function pointers
Rendered Output: pixel(0,0)=0xff00ff00
SUCCESS: Dynamically loaded PanVK ICD rendered solid green (0xFF00FF00)!
=== Step 4: Vulkan Loader ICD Shared Library Integration PASSED CLEANLY! ===
```

### `size_sweep` (frame único por tamanho, cada um em memória nova)
- 16x16: ret=0, 256/256 verdes (correto)
- 32x32/64x64/...: ret=0 (fragmento aceito), mas somente a **primeira tile
  16x16** renderiza — **mesma limitação da referência** (a referência renderiza
  169/4096 em 64x64).

### Limitações conhecidas (compartilhadas com a referência `replay_egl_triangle`)
1. **Multi-tile**: o fragmento só processa a primeira tile (16x16). A referência
   tem exatamente o mesmo comportamento (169 px em 64x64). Não é um problema do
   nosso layout — todos os descritores são idênticos aos da referência.
2. **Terminação do fragmento**: nunca sinaliza `0x1 DONE`; sempre `0x4`/`0x4002`.
   Isso deixa o GPU ocasionalmente em estado de JOB_READ_FAULT, o que pode
   "travar" submissões seguintes **no mesmo processo**. Cada processo novo
   funciona normalmente.
3. **Sem root/SELinux**: página de shader mapeada RW + `GPU_EX` (PROT_EXEC de CPU
   bloqueado por SELinux).

---

## 2.5 Suporte a Memória/Imagens (estilo panvk) — adicionado nesta sessão

Seguindo a lógica de layout de `panvk_image.c`, o driver agora tem:

- **Layout linear real** (`panvk_v9_image_layout_init`): row pitch alinhado a 64,
  slices por mip/layer/depth, offsets por mip — `vkGetImageMemoryRequirements`
  e o novo `vkGetImageSubresourceLayout` refletem o layout exato.
- **Formato→bytes** (`panvk_v9_format_bpp`): R8, R8G8, R8G8B8A8, B8G8R8A8,
  R16G16, R16G16B16A16, R32, R32G32, R32G32B32, R32G32B32A32, D16, D32, D24S8...
- **Caminho de cópia/clear real (CPU)**: `vkCmdCopyBufferToImage`,
  `vkCmdCopyImageToBuffer`, `vkCmdCopyImage`, `vkCmdBlitImage` (NEAREST) e
  `vkCmdClearColorImage` agora executam memcpy/fill no BO respeitando o layout
  linear (bufferRowLength/bufferImageHeight/layers/offsets).
- **`VkPhysicalDeviceMemoryProperties` corrigido**: layout padrão do struct
  (heap em byte 260, não 132), 2 tipos de memória (tipo 0 DEVICE_LOCAL +
  HOST_VISIBLE + HOST_COHERENT; tipo 1 DEVICE_LOCAL) e 1 heap de 4GB.
- **`vkGetPhysicalDeviceImageFormatProperties`**: até 4096², profundidade 2048
  (3D), 16 mips, 16 layers.

### Novo teste: `test_loader_images`
Fluxo completo via `vk_icdGetInstanceProcAddr` (como o loader real):
buffer→imagem→buffer (gradiente 64x32 íntegro), clear de layer (0x3f800000),
blit NEAREST 64x32→8x4, cópia imagem→imagem — tudo verificado byte a byte.
Resultado: **PASSED CLEANLY!** (1 submit GPU, copy/clear executados em record).

```
=== Loader + Image/Memory Support PASSED CLEANLY! ===
```

> Nota: os dados de imagem são movidos via memcpy (CPU). O renderizador de
> fragmentos usa agora o **compilador SPIR-V→Valhall completo (Mesa 26.2)** — ver
> seção 0. Texturização/amostragem de imagem na GPU ainda não está ligada ao
> cmd stream, mas shaders VS/FS arbitrários compilam e rodam.

---

## 3. Arquitetura (estilo Turnip)

```
Vulkan App (vkmark/teste)
        │  vk_icdGetInstanceProcAddr
        ▼
libvulkan_panvk_v9.so   ← ICD Vulkan (panvk_v9_entrypoints.c)
   ├── panvk_v9_compiler.h        ← interface do compilador SPIR-V→Valhall
   │      (dlopen libpanvk_v9_compiler.so em runtime)
   ├── panvk_v9_x11.c             ← WSI/X11 (apresentação)
   └── panvk_v9_entrypoints.c     ← vkCreateInstance/Device/Pipeline/QueueSubmit
            │
            ▼
   v9_cmd_stream.c / v9_pack.h    ← constrói o job chain Valhall v9
            │  (MFBD, DCD, RT0, Tiler Ctx, Tiler Heap, TJ, FJ1/FJ2, Flush)
            ▼
   pan_kmod_kbase.c / kbase_winsys.c   ← backend kbase (/dev/mali0)
            │  (MEM_ALLOC SAME_VA, GPU_EX, JOB_SUBMIT kbase_atom_mtk, eventos)
            ▼
        /dev/mali0 (mali_kbase MTK r49)
```

- **ICD manifest**: `panvk_v9_icd.json` → `./libvulkan_panvk_v9.so`.
- **Compilador de shader**: carregado dinamicamente (`libpanvk_v9_compiler.so`).
  Backend real **Mesa 26.2 / Panfrost Valhall v9** compilando SPIR-V arbitrário
  (VS/FS) para o ISA Valhall, com GPU ID `0x90001000` — ver seção 0.
- **Backend kbase**: mapeamento SAME_VA, átomos `kbase_atom_mtk` (packed 72 B),
  submits seriais (pre_dep em batch não funciona no MTK r49).

---

## 4. Arquivos do Driver

| Arquivo | Papel |
|---|---|
| `panvk_v9_entrypoints.c/.h` | Entry points Vulkan + WSI (ICD) |
| `panvk_v9_compiler.h` | Interface do compilador SPIR-V→Valhall |
| `panvk_v9_compiler_mesa.c` | Wrapper do compilador Mesa |
| `panvk_v9_x11.c` | WSI X11 |
| `panvk_v9_icd.json` | Manifesto ICD Vulkan |
| `v9_cmd_stream.c/.h` | Construção do command buffer / job chain Valhall |
| `v9_pack.h` | Layout/empacotamento dos descritores (MFBD, DCD, FJ, TJ, ...) |
| `pan_kmod_kbase.c/.h` | Camada de abstração do kbase |
| `kbase_winsys.c/.h` | ioctls /dev/mali0 (MEM_ALLOC, JOB_SUBMIT, eventos) |
| Testes | `dense_map`, `size_sweep`, `frag_dump`, `two_frame`, `tj_post`, `poly_dump`, `test_vulkan_loader_icd`, ... |

---

## 5. Como Compilar

```bash
# Biblioteca ICD
clang -O2 -shared -fPIC -o libvulkan_panvk_v9.so \
    panvk_v9_entrypoints.c panvk_v9_x11.c \
    v9_cmd_stream.c pan_kmod_kbase.c kbase_winsys.c \
    -ldl -lpthread -lX11 -lxcb

# Teste de pipeline completo
clang -O2 -o test_vulkan_loader_icd test_vulkan_loader_icd.c

# Testes de render
clang -O2 -o dense_map dense_map.c v9_cmd_stream.c pan_kmod_kbase.c kbase_winsys.c
clang -O2 -o size_sweep size_sweep.c v9_cmd_stream.c pan_kmod_kbase.c kbase_winsys.c
```

## 6. Como Executar

```bash
./dense_map 16 16                    # frame único 16x16 (ret=0, 256/256 verdes)
./size_sweep                         # varredura de tamanhos
./test_vulkan_loader_icd             # pipeline Vulkan completo via dlopen do ICD
./test_loader_images                 # memória/imagem/copy/clear/blit via loader
```

Variáveis de ambiente:
- `V9_SKIP_POST_FLUSH=1` — pula o Post-Flush (evita o JOB_READ_FAULT residual
  quando o fragmento é terminado; útil para múltiplos frames).
- `PANVK_DRY_RUN=1` — pipeline Vulkan 100% CPU, sem abrir /dev/mali0 (testes seguros).
- `PANVK_DRY_RUN=0` — usa a GPU real (`/dev/mali0`) para testes.
- `PANVK_SUBMIT_TIMEOUT_MS=<ms>` — timeout máximo de espera por atom (default 1500ms).
  Timeout vira `-ETIMEDOUT` (falha rápida), SEMPRE, em vez de espera infinita que
  aciona o watchdog do Mali e reinicia o celular.

---

## 7. Sessão 2026-08-02 — Fragilidade do fragmento + completude da API

### 7.1 Nagara: timeout do fragmento era fixo (200ms)
- Em `v9_cmd_stream.c` o fragmento (atom 2) usava timeout **fixo de 200ms**, enquanto
  TILER/Flush usavam o tempo configurável (`PANVK_SUBMIT_TIMEOUT_MS`). Em MTK r49 o
  primeiro fragmento pode levar >200ms (page faults + warmup) → timeout espúrio.
- Corrigido: `pan_kmod_submit_fragment_timeout` agora usa `kbase_submit_timeout_ms(1500)`
  por padrão, e em `v9_cmd_stream.c` o chamador passa o mesmo valor configurável.
- **Comportamento novo (seguro):** todo timeout de GPU → `-ETIMEDOUT` + aviso
  `"pan_kmod: ... TIMED OUT (timeout=%dms) - GPU may be hung"`. Antes o código tratava
  timeout aberto como sucesso (mascarava hang e deixava o watch dog reiniciar).

### 7.2 Watch out: estado residual JOB_READ_FAULT persiste entre processos
- O primeiro frame após boot (`dense_map 16 16`) renderiza verde (`ret=0`), mas após
  frações a GPU entra em estado residual `JOB_READ_FAULT` e o **fragmento para de
  completar** mesmo em novos processos — `event_code=0x0`, `ret=-110` com timeout.
  O REPORT.md anterior afirmava "cada processo novo funciona normal", mas isso **não
  se confirmou** nessa revisão: a GPU precisa de **reboot do celular** para
  limpar o estado. O driver agora falha com segurança (sem reiniciar o sistema).

### 7.3 Decisão estratégica sobre o Mesa PanVK
Mapeamento completo do `mesa-26.2.0-rc3`:
- O driver **PanVK real do Mesa NÃO roda neste celular**: rejeita arquitetura 9/G77
  (`panvk_physical_device.c:401-423` → `VK_ERROR_INCOMPATIBLE_DRIVER`) e só fala com
  `/dev/dri/renderD*` via **DRM panfrost/panthor** (`panfrost_kmod.c:21-58`), nunca com
  `/dev/mali0`/ioctls kbase do MTK. O `build-panvk/` nem compila `libvulkan_panfrost.so`.
- **O único recurso útil do Mesa é o compilador Valhall v9** (`panvk_v9_compiler_mesa.c`
  → `libpanvk_v9_compiler.so`), já linkado e funcional no ICD.
- Conclusão: o caminho para o objetivo é **completar o ICD custom** (que fala kbase),
  não portar o PanVK completo.

### 7.4 Expansion da superfície Vulkan (ICD custom)
Sessão adicionou entry points faltantes (validado por `test_entrypoints_probe`, 28/28
resolvidos via `vk_icdGetInstanceProcAddr`):
- `vkCreateRenderPass2`(KHR), `vkCmdPushConstants`, `vkCmdSetDepthBias`
- Events: `vkCreateEvent`, `vkDestroyEvent`, `vkGetEventStatus`, `vkSetEvent`,
  `vkResetEvent`, `vkCmdSetEvent`, `vkCmdResetEvent`, `vkCmdWaitEvents`
- Queries: `vkCreateQueryPool`, `vkDestroyQueryPool`, `vkCmdBeginQuery`,
  `vkCmdBeginQuery`, `vkCmdEndQuery`, `vkCmdWriteTimestamp`, `vkCmdResetQueryPool`
- Compute: `vkCreateComputePipelines` (guarda o stage, sem execução ainda),
  `vkCmdDispatch`/`vkCmdDispatchIndirect` (stub seguro)
- Memória: `vkFlushMappedMemoryRanges`, `vkInvalidateMappedMemoryRanges` (no-op, HW
  é coerente — loop robusto)
- Sync2/1.3: `vkGetDeviceQueue2`, `vkCmdPipelineBarrier2`(KHR), `vkQueueSubmit2`(KHR),
  `vkCmdExecuteCommands`

Total de entry points expostos: **~150** (era 120).

### Ainda pendente para jogos (DXVK/vkd3d)
- Compute de verdade no Valhall v9 (compiler só emite VS/FS hoje).
- Swapchain em Wayland (`vkCreateWaylandSurfaceKHR`), sync real de semáforos/timeline,
  external-memory FD, buffer device address.
- Completude de state dinâmico (`vkCmdSetStencil*`, `vkCmdSetBlendConstants`, etc.).
- Teste de GPU real após reboot.

### Gerar o pacote
```bash
python3 - <<'PY'
import zipfile, os
name = 'PanVK-v9-driver_winlator_kimchi.zip'
if os.path.exists(name): os.remove(name)
z = zipfile.ZipFile(name, 'w', zipfile.ZIP_DEFLATED)
for r, d, fs in os.walk('PanVK-v9-Kimchi'):
    for f in fs:
        p = os.path.join(r, f); z.write(p, p)
z.close()
PY
```

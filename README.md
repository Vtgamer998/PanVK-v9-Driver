# PanVK-v9 Driver — Vulkan ICD para Mali-G68 MC4

Driver Vulkan experimental para **ARM Mali-G68 MC4 (MediaTek MT6893, Valhall v9)**.

---

## ⚠️ AVISO IMPORTANTE — LEIA ANTES DE USAR

### Isenção de Responsabilidade

**ESTE SOFTWARE É FORNECIDO "COMO ESTÁ", SEM GARANTIAS DE QUALQUER TIPO.**

O autor **NÃO SE RESPONSABILIZA** por quaisquer danos diretos, indiretos, incidentais, especiais, consequenciais ou punitivos decorrentes do uso ou impossibilidade de uso deste software, incluindo mas não se limitando a:

- **DANOS AO HARDWARE**: Travamento do celular, reinicialização forçada, dano permanente à GPU ou outros componentes
- **PERDA DE DADOS**: Corrupção de dados, configurações ou aplicações
- **INSTABILIDADE DO SISTEMA**: Congelamento, crashes do sistema operacional
- **VIOLAÇÃO DE GARANTIA**: Uso deste software pode anular a garantia do dispositivo
- **DANOS AO SISTEMA OPERACIONAL**: Possível corrupção do Android ou kernel

**VOCÊ ASSUME TODA A RESPONSABILIDADE PELO USO DESTE SOFTWARE.**

### Riscos Conhecidos

| Risco | Severidade | Descrição |
|---|---|---|
| **Reinicialização do celular** | ALTA | A GPU pode entrar em estado de JOB_READ_FAULT, causando reinício forçado pelo watchdog |
| **Travamento do sistema** | ALTA | Timeout de GPU pode congelar o dispositivo |
| **Dano à GPU** | MÉDIA | Uso indevido pode causar superaquecimento ou desgaste prematuro |
| **Perda de dados** | MÉDIA | Reinicialização forçada pode causar perda de dados não salvos |
| **Anulação de garantia** | ALTA | Modificações no sistema podem anular a garantia do fabricante |

### Recomendações de Segurança

1. **FAÇA BACKUP** de todos os dados importantes antes de usar
2. **NÃO USE** em dispositivo principal ou com dados críticos
3. **MANTENHA** o celular carregando e com boa ventilação
4. **USE** `PANVK_DRY_RUN=1` por padrão (modo seguro, sem GPU real)
5. **REINICIE** o celular imediatamente se notar comportamento estranho
6. **NÃO MODIFIQUE** o kernel ou sistema operacional

---

## Créditos e Agradecimentos

### Projeto Base

Este driver foi desenvolvido com base no projeto de engenharia reversa:

**[VectorJet/Mali-G77-MC9](https://github.com/VectorJet/Mali-G77-MC9)**

> Reverse-engineering notes and tools for ARM Mali-G77 MC9 GPU driver behavior.

Agradecimento especial ao **VectorJet** pelo trabalho de engenharia reversa que tornou este projeto possível.

### Tecnologias Utilizadas

- **Mesa 26.2** — Compilador SPIR-V→Valhall v9 (backend Panfrost)
- **Vulkan API** — Interface gráfica de baixo nível
- **Linux Kernel** — Interface kbase para GPU Mali
- **ARM Mali-G68 MC4** — GPU alvo (MediaTek Dimensity 700)

---

## Sobre o Projeto

### O que é

Um driver Vulkan ICD (Installable Client Driver) experimental para GPUs ARM Mali, similar ao Turnip para Qualcomm Adreno, mas para arquitetura Valhall v9.

### Capacidades

- Renderização de triângulos via pipeline Vulkan completo
- Compilador SPIR-V→Valhall funcional (Mesa 26.2)
- Suporte a ~150 entry points Vulkan
- Memória e imagens (layout linear, cópia, blit, clear)

### Limitações

- **NÃO roda jogos** (DXVK/VKD3D requerem implementação completa da API)
- **Não é multi-tile** — processa apenas primeira tile 16x16
- **Fragilidade** — pode causar JOB_READ_FAULT e reinicialização
- **SELinux** — página de shader mapeada RW + PROT_EXEC bloqueado

---

## Estrutura do Projeto

```
PanVK-v9-Driver/
├── src/                    # Código fonte do driver
│   ├── panvk_v9_*.c/.h    # Entry points e ICD Vulkan
│   ├── v9_cmd_stream.*    # Command buffer / job chain
│   ├── v9_pack.h          # Layout dos descritores
│   ├── pan_kmod_kbase.*   # Backend kbase
│   └── kbase_winsys.*     # ioctls /dev/mali0
├── test/                   # Testes e exemplos
│   ├── test_*.c           # Testes da API Vulkan
│   ├── dense_map.c        # Render direto
│   └── ...                # Outros testes
├── build/                  # Scripts de build e execução
│   ├── build.sh           # Script de compilação
│   └── run_*.sh           # Scripts de execução
├── docs/                   # Documentação
│   ├── README.md          # Este arquivo
│   └── REPORT.md          # Relatório técnico
└── README.md               # Documentação principal
```

---

## Pré-requisitos

- **Hardware**: Dispositivo Android com GPU ARM Mali-G68 MC4 (ou similar Valhall v9)
- **Software**: Termux no Android
- **Acesso**: Root pode ser necessário para alguns testes
- **Kernel**: `mali_kbase` (MediaTek r49 ou compatível)

---

## Instalação

### Compilação

```bash
cd src/

# Biblioteca ICD
clang -O2 -shared -fPIC -o libvulkan_panvk_v9.so \
    panvk_v9_entrypoints.c panvk_v9_x11.c \
    v9_cmd_stream.c pan_kmod_kbase.c kbase_winsys.c \
    -ldl -lpthread -lX11 -lxcb

# Compilador (opcional, requer Mesa 26.2)
# Ver docs/REPORT.md para instruções
```

### Configuração

```bash
export VK_ICD_FILENAMES=/caminho/para/vulkan.panvk_v9.json
export LD_LIBRARY_PATH=/caminho/para:$LD_LIBRARY_PATH
```

---

## Uso

### Testes Seguros (sem GPU real)

```bash
PANVK_DRY_RUN=1 ./test_vulkan_loader_icd   # Pipeline completo, CPU only
PANVK_DRY_RUN=1 ./test_loader_images       # Memória/imagem
```

### Teste Real (com GPU) — CUIDADO!

```bash
# APÓS REBOOT do celular, 1 frame único
PANVK_DRY_RUN=0 PANVK_SUBMIT_TIMEOUT_MS=1500 V9_SKIP_POST_FLUSH=1 timeout 25 ./dense_map 16 16
```

**⚠️ ATENÇÃO**: Testes com GPU real podem causar reinicialização do celular!

---

## Variáveis de Ambiente

| Variável | Descrição |
|---|---|
| `PANVK_DRY_RUN=1` | Modo seguro (CPU only, sem /dev/mali0) |
| `PANVK_DRY_RUN=0` | Usa GPU real (PERIGOSO!) |
| `PANVK_SUBMIT_TIMEOUT_MS` | Timeout de submit (default 1500ms) |
| `V9_SKIP_POST_FLUSH=1` | Pula Post-Flush (evita JOB_READ_FAULT) |
| `PANVK_FS_WORKREG` | Debug: work registers do FS |
| `PANVK_FORCE_BARRIER` | Debug: força barrier |

---

## Limitações Técnicas

1. **Multi-tile**: Processa apenas primeira tile 16x16
2. **Terminação do fragmento**: Nunca sinaliza 0x1 DONE (comportamento idêntico à referência)
3. **SELinux**: Página de shader mapeada RW + PROT_EXEC bloqueado
4. **Não é completo**: Sem swapchain plena, sem extensões Vulkan 1.1+ completas
5. **GPU fragilidade**: Pode entrar em JOB_READ_FAULT e travar

---

## Licença

Este projeto é um estudo/ICD experimental. Uso por sua conta e risco.

---

## Documentação

- [docs/README.md](docs/README.md) — Documentação detalhada do driver
- [docs/REPORT.md](docs/REPORT.md) — Relatório técnico completo

---

## Contato

Para issues ou sugestões, abra uma issue no repositório.

---

**LEMBRE-SE: VOCÊ ASSUME TODA A RESPONSABILIDADE PELO USO DESTE SOFTWARE. O AUTOR NÃO SE RESPONSABILIZA POR QUAISQUER DANOS.**

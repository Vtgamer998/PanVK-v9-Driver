#!/bin/sh
# diag_image_hang.sh
# Roda 4 testes em sequência para isolar onde a imagem trava.
# Copie para o mesmo diretório dos binários e execute:
#   chmod +x diag_image_hang.sh && ./diag_image_hang.sh
#
# IMPORTANTE: reinicie o celular antes de rodar cada bloco se travar.

BIN=./two_frame   # binário de teste multi-frame
TIMEOUT=30        # segundos máximos por teste

echo "========================================"
echo "TESTE 1 — Baseline: 1 frame, modo seco (sem GPU real)"
echo "Esperado: frame 0: ret=0 green=256/256"
echo "----------------------------------------"
PANVK_DRY_RUN=1 timeout $TIMEOUT $BIN
echo "RESULTADO TESTE 1: $?"
echo ""

echo "========================================"
echo "TESTE 2 — 1 frame real na GPU, sem post-flush"
echo "Esperado: ret=0, algum green > 0"
echo "Se travar aqui: problema está no FRAGMENTO em si"
echo "----------------------------------------"
PANVK_DRY_RUN=0 \
TWO_FRAME_N=1 \
V9_FRAG_SINGLE_JOB=0 \
V9_FORCE_POST_FLUSH=0 \
V9_NO_CYCLE_DEV=1 \
PANVK_SUBMIT_TIMEOUT_MS=2000 \
timeout $TIMEOUT $BIN
echo "RESULTADO TESTE 2: $?"
echo ""

echo "========================================"
echo "TESTE 3 — 2 frames, sem post-flush, sem cycle dev"
echo "Esperado: frame 0 ok, frame 1 trava ou ret!=0"
echo "Se trava no frame 1: é o slot wedged entre frames (esperado)"
echo "Se não trava: problema estava no post-flush"
echo "----------------------------------------"
PANVK_DRY_RUN=0 \
TWO_FRAME_N=2 \
V9_FRAG_SINGLE_JOB=0 \
V9_FORCE_POST_FLUSH=0 \
V9_NO_CYCLE_DEV=1 \
PANVK_SUBMIT_TIMEOUT_MS=2000 \
timeout $TIMEOUT $BIN
echo "RESULTADO TESTE 3: $?"
echo ""

echo "========================================"
echo "TESTE 4 — 2 frames, com auto-reopen entre frames"
echo "Esperado: frame 0 e frame 1 ambos ok"
echo "Se ambos ok: as correções do chat funcionaram"
echo "Se frame 1 ainda trava: bug no pan_kmod_dev_reopen"
echo "----------------------------------------"
PANVK_DRY_RUN=0 \
TWO_FRAME_N=2 \
TWO_FRAME_FRESH_DEV=1 \
V9_FRAG_SINGLE_JOB=0 \
V9_FORCE_POST_FLUSH=0 \
PANVK_SUBMIT_TIMEOUT_MS=2000 \
timeout $TIMEOUT $BIN
echo "RESULTADO TESTE 4: $?"
echo ""

echo "========================================"
echo "TESTE 5 — 3 frames com single-job fragment + post-flush"
echo "Esperado: todos os 3 frames ok"
echo "Se ok: este modo é o mais estável para produção"
echo "----------------------------------------"
PANVK_DRY_RUN=0 \
TWO_FRAME_N=3 \
V9_FRAG_SINGLE_JOB=1 \
V9_FORCE_POST_FLUSH=1 \
PANVK_SUBMIT_TIMEOUT_MS=2000 \
PANVK_FRAG_CATCHUP_MS=500 \
timeout $TIMEOUT $BIN
echo "RESULTADO TESTE 5: $?"
echo ""

echo "Diagnóstico completo."
echo "Cole aqui os resultados e o padrão de travamento fica claro."

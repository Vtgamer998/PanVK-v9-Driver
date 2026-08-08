#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# run_safe_test.sh — roda os testes do PanVK-v9 SEM risco de
# travar a GPU e reiniciar o celular.
#
# POR QUE ANTES O CELULAR REINICIAVA:
#   O kernel Mali (MTK r49) tem um watchdog: se a GPU fica
#   pendurada (job chain inválido) por tempo demais, o sistema
#   reinicia. Os testes antigos faziam submits com espera
#   INFINITA (timeout -1) e/ou vários frames no MESMO processo,
#   deixando a GPU em JOB_READ_FAULT.
#
# MEDIDAS DE SEGURANÇA (todas aplicadas aqui):
#   1. PANVK_DRY_RUN=1        -> testa o pipeline Vulkan inteiro
#      sem abrir /dev/mali0 (0% risco de reiniciar).
#   2. PANVK_SUBMIT_TIMEOUT_MS -> espera LIMITADA em cada atom
#      da GPU; se pendurar, o teste falha rápido (não trava).
#   3. V9_SKIP_POST_FLUSH=1   -> evita o JOB_READ_FAULT residual
#      do Post-Flush após o fragmento ser terminado.
#   4. timeout 20             -> mata o processo se ele pendurar.
#   5. UM frame por processo  -> cada processo novo a GPU volta
#      a funcionar normalmente (limitação conhecida do kernel).
#   6. POISON PILL (novo):    -> se um atom der TIMEOUT, o driver
#      grava um marcador (com o boot_id do kernel) e envenena o
#      device: submits seguintes NO MESMO processo e em processos
#      NOVOS são RECUSADOS (fail-fast) em vez de bloquear na GPU
#      e congelar o celular. O marcador expira sozinho após um
#      REBOOT (boot_id muda). Veja o modo "wedge" abaixo.
# ============================================================
set -u
cd "$(dirname "$0")"

DRY="${PANVK_DRY_RUN:-1}"          # default: 1 = seco (seguro)
TO="${PANVK_SUBMIT_TIMEOUT_MS:-1500}"

warn() { printf '\033[33m[WARN]\033[0m %s\n' "$*"; }
ok()   { printf '\033[32m[ OK ]\033[0m %s\n' "$*"; }
bad()  { printf '\033[31m[FAIL]\033[0m %s\n' "$*"; }

usage() {
    cat <<'EOF'
Uso: run_safe_test.sh [modo]

Modos:
  compiler    Compila SPIR-V -> Valhall (100% CPU, sem GPU).  [SEGURO]
  loader      Pipeline Vulkan completo via dlopen do ICD.     [SEGURO - usa dry-run por padrao]
  images      Memoria/imagem/copy/clear/blit via loader.      [SEGURO - usa dry-run por padrao]
  gpu         UM frame real 16x16 na GPU (dense_map 16 16).   [RISCO BAIXO]
              Este e o UNICO modo que toca /dev/mali0. Use
              apenas quando necessario: PANVK_DRY_RUN=0 ./run_safe_test.sh gpu
  wedge       Mostra o estado do marcador de GPU travada.     [SEGURO]
              Se a GPU travou, diz que REBOOT e necessario.
  all         compiler + loader + images (todos secos).       [SEGURO]

Variaveis:
  PANVK_DRY_RUN=0          -> permite acesso real a GPU (padrao 1=seco)
  PANVK_SUBMIT_TIMEOUT_MS  -> ms de espera por atom (padrao 1500)
EOF
}

[ $# -lt 1 ] && { usage; exit 1; }
MODE="$1"

SPV=""
if [ -f /data/data/com.termux/files/usr/tmp/opencode/vs.spv ]; then
    SPV=" /data/data/com.termux/files/usr/tmp/opencode/vs.spv /data/data/com.termux/files/usr/tmp/opencode/fs.spv"
    [ -f /data/data/com.termux/files/usr/tmp/opencode/cs.spv ] && SPV="$SPV /data/data/com.termux/files/usr/tmp/opencode/cs.spv"
fi

export PANVK_SUBMIT_TIMEOUT_MS="$TO"
export V9_SKIP_POST_FLUSH="${V9_SKIP_POST_FLUSH:-1}"

run() {
    local name="$1"; shift
    if [ "$DRY" = "1" ]; then
        env PANVK_DRY_RUN=1 timeout 20 "$@" 2>&1
    else
        warn "Acesso REAL a GPU para: $name"
        timeout 20 "$@" 2>&1
    fi
    local rc=$?
    if [ $rc -eq 124 ]; then
        bad "$name: TIMEOUT (20s) — abortado; GPU provavelmente pendurada."
        return 1
    fi
    if [ $rc -ne 0 ]; then
        bad "$name: falhou (rc=$rc)"
        return 1
    fi
    ok "$name: PASS"
    return 0
}

fail=0

case "$MODE" in
    compiler)
        run "compiler SPIR-V->Valhall" ./test_panvk_v9_compiler ./libpanvk_v9_compiler.so $SPV
        fail=$?
        ;;
    loader)
        run "Vulkan pipeline via ICD (dlopen)" ./test_vulkan_loader_icd $SPV
        fail=$?
        ;;
    images)
        run "memoria/imagem/copy/clear/blit" ./test_loader_images
        fail=$?
        ;;
    gpu)
        if [ "$DRY" = "1" ]; then
            warn "Modo seco: para tocar a GPU real use PANVK_DRY_RUN=0 ./run_safe_test.sh gpu"
            warn "Rodando em dry-run (nao toca /dev/mali0)."
        fi
        run "GPU single-frame 16x16 (dense_map)" ./dense_map 16 16
        fail=$?
        ;;
    wedge)
        if ! [ -x ./test_wedge ]; then
            bad "test_wedge nao compilado - rode bash build.sh"
            exit 1
        fi
        env PANVK_DRY_RUN=1 ./test_wedge
        if [ -f "$HOME/.panvk_v9_wedged" ]; then
            bad "GPU marcada como TRAVADA (marker presente)."
            bad "REBOOT no celular para limpar - o marker expira sozinho (boot_id muda)."
            fail=1
        else
            ok "GPU sem marcador de travamento - pronta para teste real."
        fi
        ;;
    all)
        run "compiler SPIR-V->Valhall" ./test_panvk_v9_compiler ./libpanvk_v9_compiler.so $SPV
        fail=$((fail || $?))
        run "Vulkan pipeline via ICD (dlopen)" ./test_vulkan_loader_icd $SPV
        fail=$((fail || $?))
        run "memoria/imagem/copy/clear/blit" ./test_loader_images
        fail=$((fail || $?))
        ;;
    *)
        usage
        exit 1
        ;;
esac

echo
if [ $fail -eq 0 ]; then
    ok "TODOS OS TESTES PASSARAM (seguros)"
else
    bad "Houveram falhas (veja acima)"
fi
exit $fail

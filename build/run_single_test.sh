#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# run_single_test.sh — Roda UM ÚNICO teste na GPU real, de forma
# segura, SEM congelar o celular.
#
# REGRAS DE SEGURANÇA (criticas para o kernel MTK r49):
#   1. EXATAMENTE um teste por invocação (1 submit por processo).
#   2. Gate do marker: se a GPU estiver marcada como travada,
#      ABORTA antes de abrir /dev/mali0 (pede reboot).
#   3. timeout por atom (1500ms) + timeout externo (30s).
#   4. post-flush DESLIGADO (causa o hang).
#   5. Depois de rodar, mostra o estado do marker.
#
# COMO USAR (sempre um comando por vez, nunca em loop):
#   ./run_single_test.sh dense_map 16 16
#   ./run_single_test.sh test_present_image 32 32
#   ./run_single_test.sh test_compute_entrypoints3 cs3.spv
# ============================================================
set -u
cd "$(dirname "$0")"

ok()   { printf '\033[32m[ OK ]\033[0m %s\n' "$*"; }
bad()  { printf '\033[31m[FAIL]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[WARN]\033[0m %s\n' "$*"; }

if [ $# -lt 1 ]; then
    echo "Uso: ./run_single_test.sh <bin> [args...]"
    exit 2
fi
BIN="$1"; shift

export PANVK_SUBMIT_TIMEOUT_MS="${PANVK_SUBMIT_TIMEOUT_MS:-1500}"
export V9_SKIP_POST_FLUSH=1

if ! [ -x "$BIN" ]; then
    bad "binario '$BIN' nao encontrado/executavel"
    exit 1
fi

# Gate do marker (evita abrir /dev/mali0 com GPU travada)
if [ -f "$HOME/.panvk_v9_wedged" ]; then
    bad "GPU marcada como TRAVADA. REBOOT no celular para limpar."
    bad "Abortado antes de tocar /dev/mali0 - celular NAO vai congelar."
    exit 3
fi

warn "Teste REAL na GPU: $BIN $*"
warn "Se travar/pendurar, o driver grava o marker e falha rapido (nao congela)."
echo

timeout 30 "./$BIN" "$@" 2>&1
rc=$?

echo
if [ $rc -eq 124 ]; then
    bad "TIMEOUT (30s) - processo morto pelo timeout externo."
fi
if [ -f "$HOME/.panvk_v9_wedged" ]; then
    bad "ATENCAO: marker de GPU travada foi gravado."
    bad "A GPU ficou em estado invalido. REBOOT no celular antes do proximo teste."
    exit 4
fi
if [ $rc -eq 0 ]; then
    ok "$BIN passou (GPU limpa, sem marker)."
else
    bad "$BIN falhou (rc=$rc). Veja saida acima."
fi
exit $rc

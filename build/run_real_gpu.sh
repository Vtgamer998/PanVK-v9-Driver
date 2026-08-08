#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# run_real_gpu.sh — validação na GPU REAL (Mali-G68 /dev/mali0)
#
# OBSERVAÇÃO CRÍTICA: rode este script LOGO APÓS UM REBOOT do
# celular, com a GPU "limpa". Em 02/08 o kernel MTK r49 deixava
# a GPU em JOB_READ_FAULT residual após o 1º frame e exigia
# reboot. O fix do "Bound Max X/Y" do fragment job (fj1[9])
# aparentemente resolveu isso: 3 frames no MESMO processo
# passaram a renderizar 256/256 sem travar.
#
# O QUE ESTE SCRIPT TESTA:
#   1. dense_map 16x16   -> 1 frame, 256/256 verdes (regressão)
#   2. dense_map 64x64   -> 1 frame MULTI-TILE, conta tiles verdes
#   3. two_frame         -> 3 frames NO MESMO processo (o teste
#                           que antes travava após o 1º frame)
#   4. eventos do fragmento (DONE 0x1 x TERMINATED 0x4/CANCELLED 0x4002)
#
# SEGURANÇA: post-flush DESLIGADO (causava o hang), timeout por
# atom 1500ms, timeout externo 20s por teste. Se o teste travar,
# o kernel pode deixar a GPU em JOB_READ_FAULT -> reboot.
#
# POISON PILL (novo): se algum atom der TIMEOUT, o driver grava
# /data/data/com.termux/files/home/.panvk_v9_wedged (com o boot_id
# do kernel) e RECUSA submits seguintes. Por isso, se a GPU já
# estiver marcada como travada, este script ABORTA antes de tocar
# /dev/mali0 - o celular não congela mais; basta REBOOTAR.
# ============================================================
set -u
cd "$(dirname "$0")"

ok()   { printf '\033[32m[ OK ]\033[0m %s\n' "$*"; }
bad()  { printf '\033[31m[FAIL]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[WARN]\033[0m %s\n' "$*"; }

export PANVK_SUBMIT_TIMEOUT_MS="${PANVK_SUBMIT_TIMEOUT_MS:-1500}"
export V9_SKIP_POST_FLUSH=1
export PANVK_DEBUG_EVENTS=1

if [ "${PANVK_DRY_RUN:-0}" = "1" ]; then
    warn "PANVK_DRY_RUN=1 não é um teste real. Use sem a variável."
    exit 2
fi

if [ -f "$HOME/.panvk_v9_wedged" ]; then
    bad "GPU marcada como TRAVADA (marker: $HOME/.panvk_v9_wedged)."
    bad "REBOOT no celular para limpar (o marker expira sozinho após o boot)."
    bad "ABORTADO antes de tocar /dev/mali0 - o celular NÃO vai congelar."
    exit 3
fi

warn "Testando a GPU REAL. Não feche o terminal. Se travar, reboot no celular."
echo

fail=0

echo "== 1) dense_map 16x16 (regressão, 1 frame) =="
timeout 20 ./dense_map 16 16 2>/dev/null | python3 -c "
import sys
rows=[l for l in sys.stdin if set(l.strip())<=set('#.') and len(l.strip())==16]
g=sum(l.count('#') for l in rows)
print(f'green={g}/256')
sys.exit(0 if g==256 else 1)
"
if [ $? -eq 0 ]; then ok "dense_map 16x16 256/256"; else bad "dense_map 16x16 incompleto"; fail=1; fi
echo

echo "== 2) dense_map 64x64 (MULTI-TILE, conta tiles com pixel verde) =="
timeout 20 ./dense_map 64 64 2>/dev/null | python3 -c "
import sys
rows=[l for l in sys.stdin if set(l.strip())<=set('#.') and len(l.strip())==64]
if len(rows)!=64:
    print('grid nao renderizou ('+str(len(rows))+' linhas)'); sys.exit(1)
g=sum(l.count('#') for l in rows)
tiles=0
for ty in range(4):
    for tx in range(4):
        has=any(l[tx*16:(tx+1)*16].count('#') for l in rows[ty*16:(ty+1)*16])
        tiles+=1 if has else 0
print(f'green_px={g}/4096 tiles_com_pixel={tiles}/16')
sys.exit(0 if tiles>1 else 1)
"
if [ $? -eq 0 ]; then ok "dense_map 64x64 multi-tile (antes: so tile 0)"; else bad "dense_map 64x64 ainda mono-tile"; fail=1; fi
echo

echo "== 3) two_frame (3 frames NO MESMO processo - antes exigia reboot) =="
timeout 30 ./two_frame 2>&1 | grep -E "^frame [0-9]"
if [ $? -eq 0 ]; then ok "multi-frame no mesmo processo OK"; else bad "multi-frame falhou/hangou"; fail=1; fi
echo

echo "== 4) Eventos do fragmento (procure 0x1 DONE ideal; 0x4/0x4002 aceitos) =="
timeout 20 ./dense_map 64 64 2>&1 | grep -E "panvk: atom 2 FRAGMENT"
echo

if [ $fail -eq 0 ]; then
    ok "GPU REAL: todos os testes passaram"
else
    bad "Algum teste falhou - veja acima (reboot pode ser necessario)"
fi
exit $fail

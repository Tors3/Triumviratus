#!/usr/bin/env bash
# FASE 2 — rete 7.0 "legio-septima": rifinitura su dati Leela, seminata dalla rete di fase 1.
#
# La struttura a due fasi e' l'unica cosa che SF afferma esplicitamente:
#   "it is usually best to FIRST train a network with datasets generated with Stockfish
#    (depth 9, nodes 5000), and THEN retrain an already good network using various
#    Lc0-derived datasets"
# e allenare SOLO su Lc0 e' peggio (buchi di copertura posizionale). Per questo la fase 1
# non contiene dati Leela e la fase 2 non riparte da zero.
#
# USO:  ./train_phase2.sh <MODELLO_FASE1.pt> [DATA_DIR] [NUM_WORKERS]
set -euo pipefail

MODEL="${1:?serve il .pt finale della fase 1, oppure il suo over_last}"
DATA="${2:-/data/phase2}"
WORKERS="${3:-}"
FEATURES="Full_Threats+HalfKAv2_hm^+PP_3Wide+PassedPawns"
# Il trainer NON e' per forza accanto a questo script: setup_vm.sh lo installa in
# <DEST>/nnue-pytorch, mentre lo script puo' stare altrove (es. copiato in /root).
# Ordine: $TRAINER esplicito -> accanto allo script -> percorsi noti di setup_vm.sh.
TRAINER="${TRAINER:-}"
if [ -z "$TRAINER" ]; then
  for c in "$(dirname "$0")/nnue-pytorch"            "$HOME/legio-septima/nnue-pytorch"            /root/legio-septima/nnue-pytorch; do
    [ -f "$c/train.py" ] && { TRAINER="$c"; break; }
  done
fi
[ -n "$TRAINER" ] && [ -f "$TRAINER/train.py" ] || {
  echo "ERRORE: trainer non trovato. Passalo esplicito:  TRAINER=/percorso/nnue-pytorch $0 ..." >&2
  exit 1
}
echo "[trainer] $TRAINER"

# VM 48 vCPU -> 24 worker. Non (nproc-4): oltre un certo punto i worker si contendono
# memoria e cache e il throughput scende invece di salire. 24 lascia meta' macchina al
# resto (il loader e' gia' multithread al suo interno). Da rivedere solo se la prima
# epoca mostra utilizzo GPU basso: in quel caso il collo e' il loader e si puo' salire.
WORKERS="${WORKERS:-24}"


# --- RIPRESA AUTOMATICA (istanze spot) ------------------------------------------------
# last.ckpt e' scritto alla FINE DI OGNI EPOCA (save_last=True), quindi una preemption
# costa al massimo un'epoca. Ma la dir dei checkpoint e' lightning_logs/version_N con N
# che si INCREMENTA a ogni riavvio: senza --default-root-dir fisso + ricerca esplicita,
# al riavvio il run ripartirebbe da zero senza dirlo. Qui lo rendiamo esplicito.
ROOT="${ROOT:-$DATA/../run_phase2}"
mkdir -p "$ROOT"
RESUME=""
LAST=$(find "$ROOT" -name last.ckpt -type f -newermt '@0' -exec ls -1t {} + 2>/dev/null | head -1)
if [ -n "$LAST" ]; then
  RESUME="--resume-from-checkpoint $LAST"
  echo "[RIPRESA] trovato $LAST"
  echo "[RIPRESA] $(stat -c '%y' "$LAST" 2>/dev/null)"
else
  echo "[NUOVO] nessun checkpoint in $ROOT: parto da zero"
fi

cd "$TRAINER"

# Corpus fase 2: Leela rietichettato BT4 + T91 2026. Passa i binpack che hai scaricato;
# il trainer li interleava. Il glob tiene fuori le varianti .q finche' non sappiamo cosa sono.
shopt -s nullglob
DATASETS=( "$DATA"/*.relabel-BT4-tf13tune.binpack "$DATA"/T91-2026-*.binpack )
mapfile -t DATASETS < <(printf '%s\n' "${DATASETS[@]}" | grep -v '\.q\.binpack$')
[ "${#DATASETS[@]}" -gt 0 ] || { echo "nessun binpack in $DATA"; exit 1; }
echo "dataset fase 2: ${#DATASETS[@]} file"
printf '  %s\n' "${DATASETS[@]}"

python train.py "${DATASETS[@]}" \
  --features "$FEATURES" \
  --resume-from-model "$MODEL" \
  --max-epochs 800 \
  --epoch-size 100000000 \
  --batch-size 16384 \
  --lr 4.375e-4 \
  --gamma 0.995 \
  --start-lambda 1.0 \
  --end-lambda 0.75 \
  --random-fen-skipping 3 \
  --validation-size 1000000 \
  --check-val-every-n-epoch 5 \
  --network-save-period 20 \
  --save-last-network True \
  --save-top-k 5 \
  --num-workers "$WORKERS" \
  --accelerator cuda \
  --default-root-dir "$ROOT" \
  --seed 42 \
  $RESUME \
  "${@:4}"

# ------------------------------------------------------------------------------------
# PERCHE' DIVERSI DALLA FASE 1
#
# --resume-from-model  semina dalla rete di fase 1 con uno SCHEDULE FRESCO (non riprende
#                      lo stato dell'ottimizzatore). E' il "retrain an already good network".
# --lr 4.375e-4        META' della fase 1: si rifinisce, non si riparte.
# --gamma 0.995        piu' lento, coerente col run piu' lungo.
#                      Verifica sulle epoche reali: 0.995^800 = 0.018 => lr finale all'1,8%.
# --max-epochs 800     piu' lungo proprio perche' l'lr e' piu' basso.
# lambda               INVARIATO: anneal 1.0 -> 0.75 anche qui (best practice SF in
#                      ENTRAMBE le fasi). Il nostro 0.725 sperimentale non e' la ricetta SF.
#
# ⚠️ A/B DA FARE QUI, non altrove: due fase-2 identiche dalla STESSA rete di fase 1, stesso
# schedule e stesse posizioni viste, una CON T91-2026 nell'interleave e una SENZA. E' l'unico
# modo di trasformare la scommessa su T91 in un numero. Gate net-isolated a TC >= 20+0.2;
# pavimento di risoluzione +-Elo ~ 344/sqrt(N) => 5 Elo richiedono ~4.700 partite.
#
# CHIUSURA: over_last dei 3 checkpoint finali.
# ------------------------------------------------------------------------------------

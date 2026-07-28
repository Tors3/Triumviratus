#!/usr/bin/env bash
# FASE 1 — rete 7.0 "legio-septima": training di base da zero su self-play Stockfish + DFRC
# rietichettati BT4. Non e' un graft: impara TUTTA la rete.
#
# USO:  ./train_phase1.sh [DATA_DIR] [NUM_WORKERS]
#       ./train_phase1.sh /data/phase1 40
set -euo pipefail

DATA="${1:-/data/phase1}"
WORKERS="${2:-}"
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

# --num-workers di default nel trainer e' 1: su una GPU seria il data loader diventa IL
# collo e la scheda resta ferma.
# VM 48 vCPU -> 24 worker. Non (nproc-4): oltre un certo punto i worker si contendono
# memoria e cache e il throughput scende invece di salire. 24 lascia meta' macchina al
# resto (il loader e' gia' multithread al suo interno). Da rivedere solo se la prima
# epoca mostra utilizzo GPU basso: in quel caso il collo e' il loader e si puo' salire.
WORKERS="${WORKERS:-24}"

# GPU da usare, lista separata da virgole ("0" singola, "0,1" per DDP su due schede).
# Con piu' GPU train.py DIVIDE --batch-size fra i device (per_gpu = globale / n_device),
# quindi il batch globale resta 16384 e la ricetta non cambia.
GPUS="${GPUS:-0}"


# --- RIPRESA AUTOMATICA (istanze spot) ------------------------------------------------
# last.ckpt e' scritto alla FINE DI OGNI EPOCA (save_last=True), quindi una preemption
# costa al massimo un'epoca. Ma la dir dei checkpoint e' lightning_logs/version_N con N
# che si INCREMENTA a ogni riavvio: senza --default-root-dir fisso + ricerca esplicita,
# al riavvio il run ripartirebbe da zero senza dirlo. Qui lo rendiamo esplicito.
ROOT="${ROOT:-$DATA/../run_phase1}"
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

# I dataset sono POSIZIONALI e il trainer li interleava da solo a livello di chunk:
# non serve pre-fondere con interleave_binpacks.py.
# 🔴 --gpus DA SOLO NON FA DDP: train.py legge world_size dall'ambiente distribuito e senza
# torchrun resta 1, quindi gira su UNA scheda sola (verificato il 28/07: GPU 0 al 96%, le
# altre tre a 0%). Il multi-GPU va lanciato con torchrun + ddp_launcher.py, che si occupa
# anche di affinity e thread. In DDP --gpus non serve: ogni rank usa la sua LOCAL_RANK.
NGPU=$(echo "$GPUS" | tr ',' '\n' | grep -c .)
if [ "$NGPU" -gt 1 ]; then
  echo "[DDP] $NGPU GPU via torchrun"
  LAUNCH="torchrun --standalone --nproc_per_node=$NGPU ddp_launcher.py train.py"
  GPUARG=""
else
  LAUNCH="python train.py"
  GPUARG="--gpus $GPUS"
fi

$LAUNCH \
  "$DATA"/nodes5000pv2_UHO.relabel-BT4-tf13tune.binpack \
  "$DATA"/dfrc_n5000.relabel-BT4-tf13tune.binpack \
  "$DATA"/multinet_pv-2_diff-100_nodes-5000.relabel-BT4-tf13tune.binpack \
  "$DATA"/wrongIsRight_nodes5000pv2.relabel-BT4-tf13tune.binpack \
  "$DATA"/fishpack32.relabel-BT4-tf13tune.binpack \
  --features "$FEATURES" \
  --max-epochs 500 \
  --epoch-size 100000000 \
  --batch-size 131072 \
  --lr 2.47e-3 \
  --gamma 0.990 \
  --start-lambda 1.0 \
  --end-lambda 0.75 \
  --random-fen-skipping 3 \
  --validation-size 1000000 \
  --check-val-every-n-epoch 5 \
  --network-save-period 20 \
  --save-last-network True \
  --save-top-k 5 \
  --num-workers "$WORKERS" \
  --threads "${TORCH_THREADS:-1}" \
  --accelerator cuda \
  $GPUARG \
  --default-root-dir "$ROOT" \
  --seed 42 \
  $RESUME \
  "${@:3}"

# ------------------------------------------------------------------------------------
# PERCHE' QUESTI VALORI
#
# --max-epochs 500     ricetta SF per lo stage di base (l'800 e' la fase 2, ed e' anche il
#                      default del trainer: NON lasciarlo com'e').
#                      epoch-size 100M => 600 ep = 60 MILIARDI di posizioni viste, cioe'
#                      ~1-2 passaggi sul corpus da 32-52 G. Dimensionato bene: niente
#                      ripasso eccessivo, niente dati sprecati.
# --lr 1.24e-3         default upstream, coincide con la ricetta SF.
# --gamma 0.990        default upstream. Verificato sulle epoche REALI (lezione pagata:
#                      gamma 0.997 su 800 ep lasciava l'lr al 25% a meta' run):
#                      0.992^600 = 0.008 => l'lr finisce allo 0,8% dell'iniziale. Corretto.
# --end-lambda 0.75    🔴 IL DEFAULT E' 1.0 = puro eval, NESSUN anneal. E' il fix di config
#                      piu' importante che avevamo (nostro storico: 0.75 FISSO). L'anneal
#                      1.0 -> 0.75 parte fidandosi della eval e vira verso il risultato.
# --random-fen-skipping 3   default 0, best practice SF: decorrela le posizioni dentro il
#                      batch (senza, sono mosse consecutive della stessa partita). Costa
#                      ~4x lavoro ai reader, quindi conviene solo se la CPU ha margine.
#                      Misurato su 4x A4000 il 28/07: GPU al 100%, CPU al 22% => margine
#                      abbondante, si tiene. Sull'H100 (CPU e GPU entrambe al ~25%, regime
#                      latency-bound) era invece il primo candidato da togliere.
# --validation-size 1000000  🔴 IL DEFAULT E' 0 = validazione DISATTIVATA. Ma la val_loss e'
#                      il nostro giudice di convergenza: sotto i 10 Elo i gate mentono, la
#                      val_loss no. Senza questo si guida alla cieca.
# --num-workers        default 1 => GPU affamata. Vedi sopra.
# --optimizer-name     non passato: il default e' gia' `rangerlite`, l'ottimizzatore fuso
#                      nuovo (+14,7% di throughput).
# --network-save-period 20   snapshot .nnue + .ckpt periodico ogni 20 ep: e' la STORIA
#                      (serve a over_last e a tornare indietro). La RIPRESA invece usa
#                      last.ckpt, che il trainer scrive a OGNI epoca.
# --save-top-k 5       tiene solo gli ultimi 5 periodici: over_last ne usa 3, tenerne 30
#                      sarebbe ~35 GB di disco per niente (~1,1 GB l'uno con lo stato
#                      dell'ottimizzatore). Con 5: ~7 GB.
#
# QUANDO FERMARSI: non a 600 per forza. Nel run v2 il plateau arrivo' a ~ep80 su 473 fatte, e
# fermarsi risparmio' 2,3 giorni e $130. Guardare la val_loss, non il calendario.
# CHIUSURA: media degli ultimi 3 checkpoint (over_last) — riduzione di varianza gratis,
# ricetta gia' validata su v1 e v2.
#
# PRIMA COSA SULLA GPU: cronometrare UNA epoca e guardare l'utilizzo GPU. Se sta sotto il
# 60% il collo e' il loader (alza --num-workers), non la scheda.
# ------------------------------------------------------------------------------------

#!/usr/bin/env bash
# FASE 2 — rete 7.0 "legio-septima": corpus Leela.
#
# Fa due cose, in quest'ordine:
#   1. libera il disco cancellando i binpack della FASE 1 (~130 GB)
#   2. riempie il disco fino a un budget (default 380 GB su un disco da 500) scaricando,
#      in ordine di priorita', i dati Leela rietichettati BT4 + T91 puro.
#
# USO:
#   ./download_phase2.sh <MODELLO_FASE1> [DEST] [BUDGET_GB] [PARALLELI]
#   ./download_phase2.sh /data/nets/phase1_final.pt /data/phase2 380 3
#
# 🔴 Il primo argomento NON e' decorativo: e' la prova che la fase 1 e' finita. Lo script
#    RIFIUTA di cancellare 130 GB di dati di training se la rete che dovevano produrre non
#    esiste. Cancellare prima e scoprire poi che il run era morto e' un pomeriggio di
#    download buttato.
set -uo pipefail

MODEL="${1:?serve il percorso del modello di fase 1: e' la prova che la fase 1 e' finita}"
DEST="${2:-/data/phase2}"
BUDGET_GB="${3:-380}"
JOBS="${4:-3}"
PHASE1_DIR="${PHASE1_DIR:-/data/phase1}"
LOG="$DEST/download.log"

die() { echo "ERRORE: $*" >&2; exit 1; }

# 'huggingface-cli' e' stato DISMESSO nelle versioni recenti di huggingface_hub: esiste
# ancora come eseguibile ma esce con errore invece di scaricare. La CLI nuova e' 'hf'.
HF_CLI=""
for c in hf huggingface-cli; do
  command -v "$c" >/dev/null 2>&1 && { HF_CLI="$c"; break; }
done
[ -n "$HF_CLI" ] || \
  die "ne' 'hf' ne' 'huggingface-cli' presenti:  pip install -U 'huggingface_hub[hf_transfer]'"
command -v zstd >/dev/null 2>&1 || echo "ATTENZIONE: zstd assente -> i T91 (.zst) non verranno decompressi"

# --- 1. la fase 1 e' davvero finita? -------------------------------------------------
[ -f "$MODEL" ] || die "modello di fase 1 non trovato: $MODEL — la fase 1 non e' finita, non cancello niente."
msize=$(stat -c%s "$MODEL" 2>/dev/null || echo 0)
[ "$msize" -gt 10000000 ] || die "il modello $MODEL e' solo $msize byte: sembra troncato. Non cancello niente."
echo "[+] modello di fase 1 verificato: $MODEL ($(numfmt --to=iec "$msize" 2>/dev/null || echo "$msize B"))"

# --- 2. libera lo spazio della fase 1 ------------------------------------------------
if [ -d "$PHASE1_DIR" ]; then
  freeable=$(du -sBG "$PHASE1_DIR" 2>/dev/null | cut -f1)
  echo "[+] cancello i binpack di fase 1 in $PHASE1_DIR (libero ~${freeable})"
  find "$PHASE1_DIR" -name '*.binpack' -type f -print -delete
  # la cache di hf_hub tiene una COPIA di ogni blob: senza questo il disco non si libera
  rm -rf "$PHASE1_DIR/.cache/huggingface" 2>/dev/null
  echo "[+] fatto."
else
  echo "[i] $PHASE1_DIR non esiste, salto la cancellazione (usa PHASE1_DIR=... per indicarla)"
fi

mkdir -p "$DEST" || die "non posso creare $DEST"

# --- 3. lista per PRIORITA' ----------------------------------------------------------
# formato: repo|file|GB_su_disco   (per gli .zst la stima e' il DECOMPRESSO, ~2x)
# Priorita' 1: TUTTI i T80 rietichettati con la rete Leela nuova (BT4). Sono la spina
#              dorsale del corpus SF e la richiesta esplicita.
# Priorita' 2: T91 2026 puro — la scommessa, generata dai net BT4 piu' recenti (§13).
# Priorita' 3: T60T70Farseer e T60, per copertura storica, fin dove entra nel budget.

echo "[+] interrogo l'API per l'elenco reale dei file e le dimensioni..."
PLAN=$(python - "$BUDGET_GB" <<'PY'
import json, re, sys, urllib.request
budget = float(sys.argv[1]) * 1e9
REPOS = ["vondele/linrock_relabel_1", "vondele/from_kaggle_2_relabel", "vondele/from_kaggle_1_relabel"]
files = []
for repo in REPOS:
    try:
        with urllib.request.urlopen(f"https://huggingface.co/api/datasets/{repo}/tree/main", timeout=40) as r:
            tree = json.load(r)
    except Exception as e:
        print(f"# API fallita per {repo}: {e}", file=sys.stderr); continue
    for e in tree:
        p = e["path"]
        if not p.endswith(".binpack") or ".q." in p:
            continue
        sz = e.get("size") or (e.get("lfs") or {}).get("size") or 0
        files.append((repo, p, sz))

# T91: .zst, il disco occupato e' il DECOMPRESSO (~2x il file scaricato)
try:
    with urllib.request.urlopen("https://huggingface.co/api/datasets/jshriver/t91-binpacks/tree/main", timeout=40) as r:
        for e in json.load(r):
            p = e["path"]
            if p.endswith(".binpack.zst") and "2026" in p:
                sz = e.get("size") or (e.get("lfs") or {}).get("size") or 0
                files.append(("jshriver/t91-binpacks", p, int(sz * 2)))
except Exception as e:
    print(f"# API fallita per t91: {e}", file=sys.stderr)

def prio(repo, path):
    if "test80" in path:                      return 0   # tutti i T80 relabeled
    if repo.startswith("jshriver"):           return 1   # T91 2026 puro
    if "T60T70wIsRightFarseer" in path:       return 2
    if "test60" in path:                      return 3
    return 4                                              # leela96 e resto

# 🔴 DEDUP: alcuni mesi esistono SIA come file intero SIA come .part_0/.part_1 dello
# stesso identico dato (verificato: ottobre 2022 = 69.1 GB intero, 34.6+34.6 in parti).
# Senza questo filtro si scaricava il mese due volte: 69 GB buttati e le stesse posizioni
# duplicate nel mix, che DILUISCE il gradiente invece di rafforzarlo.
def base_name(path):
    return re.sub(r"\.part_\d+(?=\.binpack)", "", path)

whole = {base_name(p) for _, p, _ in files if ".part_" not in p}
before = len(files)
files = [f for f in files if ".part_" not in f[1] or base_name(f[1]) not in whole]
if before != len(files):
    print(f"# dedup: scartati {before - len(files)} file .part_N gia' presenti come file intero",
          file=sys.stderr)

files.sort(key=lambda f: (prio(f[0], f[1]), -f[2]))
used = 0
for repo, path, sz in files:
    if used + sz > budget:
        continue
    used += sz
    print(f"{repo}\t{path}\t{sz}")
print(f"# TOTALE PIANIFICATO: {used/1e9:.1f} GB su {budget/1e9:.0f} GB di budget", file=sys.stderr)
PY
)
echo "$PLAN" | awk -F'\t' 'NF==3{printf "    %-28s %-62s %6.1f GB\n", $1, $2, $3/1e9}'
n=$(echo "$PLAN" | awk -F'\t' 'NF==3' | wc -l)
[ "$n" -gt 0 ] || die "nessun file pianificato (API irraggiungibile?)"

# --- 4. spazio disco -----------------------------------------------------------------
need=$(echo "$PLAN" | awk -F'\t' 'NF==3{s+=$3} END{printf "%d", s/1073741824}')
avail=$(df -PBG "$DEST" | awk 'NR==2{gsub("G","",$4); print $4}')
echo "[+] pianificati ${need} GB, disponibili ${avail} GB"
[ "${avail:-0}" -ge "$need" ] || die "spazio insufficiente: servono ${need} GB, liberi ${avail} GB."

# --- 5. download parallelo ------------------------------------------------------------
export HF_HUB_ENABLE_HF_TRANSFER=1
python -c "import hf_transfer" 2>/dev/null || { echo "ATTENZIONE: hf_transfer assente -> lento"; export HF_HUB_ENABLE_HF_TRANSFER=0; }

dl_one() {
  local repo="$1" f="$2" dest="$3" log="$4"
  echo "[$(date +%H:%M:%S)] START  $f" | tee -a "$log"
  if "${HF_CLI:-hf}" download "$repo" --repo-type dataset --include "$f" \
       --local-dir "$dest" >>"$log" 2>&1; then
    # i T91 arrivano compressi: --rm perche' altrimenti serve il doppio dello spazio
    if [[ "$f" == *.zst ]] && command -v zstd >/dev/null 2>&1; then
      echo "[$(date +%H:%M:%S)] UNZSTD $f" | tee -a "$log"
      zstd -d --rm -q "$dest/$f" >>"$log" 2>&1 || { echo "  decompressione FALLITA: $f" | tee -a "$log"; return 1; }
    fi
    echo "[$(date +%H:%M:%S)] OK     $f" | tee -a "$log"
  else
    echo "[$(date +%H:%M:%S)] FALLITO $f  <-- rilancia, riprende da qui" | tee -a "$log"
    return 1
  fi
}
export -f dl_one
export HF_CLI   # serve dentro il bash -c di xargs, che non eredita le variabili non esportate

echo "$PLAN" | awk -F'\t' 'NF==3{print $1"\t"$2}' \
  | xargs -P "$JOBS" -d'\n' -I{} bash -c 'IFS=$'"'"'\t'"'"' read -r r f <<< "{}"; dl_one "$r" "$f" "$1" "$2"' _ "$DEST" "$LOG"
rc=$?

echo
echo "=== contenuto finale di $DEST"
du -sh "$DEST" 2>/dev/null
ls -1 "$DEST"/*.binpack 2>/dev/null | wc -l | xargs echo "binpack presenti:"
[ $rc -eq 0 ] && echo "=== FASE 2 PRONTA. Lancia:  ./train_phase2.sh $MODEL $DEST" \
              || { echo "=== INCOMPLETO: rilancia lo stesso comando (riprende dai parziali)"; exit 1; }

#!/usr/bin/env bash
# Scarica il corpus di FASE 1 della rete 7.0 "legio-septima".
#
# Sono i binpack di self-play Stockfish + DFRC GIA' RIETICHETTATI con Leela BT4
# (`vondele/master-binpacks_relabel`, vondele = core dev SF). Perche' i relabeled e non i
# classici: stessa scala di label su tutto lo stage, che e' un vantaggio reale (mescolare
# label BT4 e label search-SF significa due distribuzioni di target nello stesso training).
#
# USO:
#   ./download_phase1.sh [DEST] [PARALLELI]
#   ./download_phase1.sh /data/phase1 3
#
# Default: DEST=./phase1, PARALLELI=3.
set -uo pipefail

DEST="${1:-./phase1}"
JOBS="${2:-3}"
REPO="vondele/master-binpacks_relabel"
NEED_GB=130          # varianti non-.q
LOG="$DEST/download.log"

# I cinque file della fase 1. Le varianti ".q" NON sono incluse: sono altri ~140 GB e non
# sappiamo ancora cosa siano (l'ipotesi "qsearch pre-applicata" e' stata SMENTITA dal
# formato binpack: rompere le catene costerebbe ~10x, non il +7% osservato). Decidere
# dopo il test su fishpack32 — vedi PROSPETTIVE_RETE_7.0.md §17.2.
FILES=(
  "nodes5000pv2_UHO.relabel-BT4-tf13tune.binpack"                    # 44.2 GB — self-play SF 5000 nodi, aperture UHO
  "dfrc_n5000.relabel-BT4-tf13tune.binpack"                          # 41.0 GB — DFRC / Fischer random
  "multinet_pv-2_diff-100_nodes-5000.relabel-BT4-tf13tune.binpack"   # 30.5 GB — multi-rete etichettatrice
  "wrongIsRight_nodes5000pv2.relabel-BT4-tf13tune.binpack"           #  7.8 GB — posizioni dove la eval sbaglia
  "fishpack32.relabel-BT4-tf13tune.binpack"                          #  5.9 GB — misc
)

die() { echo "ERRORE: $*" >&2; exit 1; }

# 'huggingface-cli' e' stato DISMESSO nelle versioni recenti di huggingface_hub: esiste
# ancora come eseguibile ma esce con errore invece di scaricare. La CLI nuova e' 'hf'.
HF_CLI=""
for c in hf huggingface-cli; do
  command -v "$c" >/dev/null 2>&1 && { HF_CLI="$c"; break; }
done
[ -n "$HF_CLI" ] || \
  die "ne' 'hf' ne' 'huggingface-cli' presenti. Installa:  pip install -U 'huggingface_hub[hf_transfer]'"

mkdir -p "$DEST" || die "non posso creare $DEST"

# --- spazio disco: fallire QUI e' molto meglio che a 120 GB scaricati -------------------
avail_gb=$(df -PBG "$DEST" | awk 'NR==2{gsub("G","",$4); print $4}')
echo "spazio disponibile in $DEST: ${avail_gb} GB (servono ~${NEED_GB} GB)"
if [ "${avail_gb:-0}" -lt "$NEED_GB" ]; then
  die "spazio insufficiente: ${avail_gb} GB liberi, ne servono ~${NEED_GB}."
fi

# hf_transfer = download multi-connessione, indispensabile su file da 40 GB
export HF_HUB_ENABLE_HF_TRANSFER=1
python -c "import hf_transfer" 2>/dev/null || {
  echo "ATTENZIONE: hf_transfer non installato -> download molto piu' lento."
  echo "            pip install -U 'huggingface_hub[hf_transfer]'"
  export HF_HUB_ENABLE_HF_TRANSFER=0
}

echo "=== fase 1 legio-septima: ${#FILES[@]} file, ~${NEED_GB} GB, $JOBS in parallelo"
echo "=== destinazione: $(readlink -f "$DEST")"
date | tee -a "$LOG"

# --- download parallelo ----------------------------------------------------------------
# Un huggingface-cli per file, JOBS alla volta. Il resume e' gratuito: hf_hub tiene i
# blob in cache e riprende i parziali, quindi rilanciare lo script dopo un'interruzione
# NON riscarica quello che c'e' gia'.
dl_one() {
  local f="$1" dest="$2" repo="$3" log="$4"
  echo "[$(date +%H:%M:%S)] START  $f" | tee -a "$log"
  if "${HF_CLI:-hf}" download "$repo" --repo-type dataset \
       --include "$f" --local-dir "$dest" >>"$log" 2>&1; then
    echo "[$(date +%H:%M:%S)] OK     $f  ($(du -h "$dest/$f" 2>/dev/null | cut -f1))" | tee -a "$log"
  else
    echo "[$(date +%H:%M:%S)] FALLITO $f  <-- rilancia lo script, riprende da qui" | tee -a "$log"
    return 1
  fi
}
export -f dl_one
export HF_CLI   # serve dentro il bash -c di xargs, che non eredita le variabili non esportate

printf '%s\n' "${FILES[@]}" \
  | xargs -P "$JOBS" -I{} bash -c 'dl_one "$@"' _ {} "$DEST" "$REPO" "$LOG"
rc=$?

# --- verifica: dimensione locale vs dimensione dichiarata dall'API ---------------------
echo; echo "=== verifica dimensioni contro l'API HuggingFace"
python - "$DEST" "$REPO" "${FILES[@]}" <<'PY'
import json, os, sys, urllib.request
dest, repo, files = sys.argv[1], sys.argv[2], sys.argv[3:]
try:
    with urllib.request.urlopen(f"https://huggingface.co/api/datasets/{repo}/tree/main", timeout=30) as r:
        remote = {e["path"]: (e.get("size") or (e.get("lfs") or {}).get("size") or 0)
                  for e in json.load(r)}
except Exception as e:
    print("  impossibile interrogare l'API (%s): salto la verifica" % e); sys.exit(0)

bad = 0
tot = 0
for f in files:
    p = os.path.join(dest, f)
    have = os.path.getsize(p) if os.path.exists(p) else 0
    want = remote.get(f, 0)
    tot += have
    if not want:
        print("  ?  %-62s (dimensione remota ignota)" % f)
    elif have == want:
        print("  OK %-62s %6.1f GB" % (f, have / 1e9))
    else:
        print("  !! %-62s locale %.1f GB, atteso %.1f GB" % (f, have / 1e9, want / 1e9))
        bad += 1
print("\ntotale scaricato: %.1f GB" % (tot / 1e9))
sys.exit(1 if bad else 0)
PY
vrc=$?

echo
if [ $rc -eq 0 ] && [ $vrc -eq 0 ]; then
  echo "=== FASE 1 COMPLETA E VERIFICATA"
  echo "Prossimo passo: interleave dei binpack, poi il training."
  echo "  python interleave_binpacks.py $DEST/*.binpack own_7500.binpack phase1_mix.binpack"
else
  echo "=== INCOMPLETO (download rc=$rc, verifica rc=$vrc)"
  echo "Rilancia lo stesso comando: riprende dai parziali, non riscarica il completato."
  exit 1
fi

#!/usr/bin/env bash
# Prepara la VM per il training della rete 7.0 "legio-septima".
#
# Ricostruisce il trainer da ZERO in modo riproducibile: clona upstream al pin esatto e ci
# applica la nostra patch (3 file). Niente copie di cartelle: cosi' e' verificabile che
# sulla VM giri esattamente lo stesso codice che abbiamo validato in locale.
#
# USO:  ./setup_vm.sh [DIR_DESTINAZIONE]
set -euo pipefail

DEST="${1:-$HOME/legio-septima}"
PIN="9f72946529c4187d3679014036cd22c3be419716"   # upstream 2026-07-26
HERE="$(cd "$(dirname "$0")" && pwd)"
PATCH="$HERE/triumviratus_passedpawns.patch"

[ -f "$PATCH" ] || { echo "patch non trovata: $PATCH" >&2; exit 1; }

echo "=== 1/5 prerequisiti"
py=""
for c in python3.13 python3.12 python3; do
  if command -v "$c" >/dev/null 2>&1; then
    v=$("$c" -c 'import sys; print(sys.version_info[:2] >= (3,12))' 2>/dev/null || echo False)
    [ "$v" = "True" ] && { py="$c"; break; }
  fi
done
[ -n "$py" ] || { echo "ERRORE: serve Python >= 3.12 (upstream usa la sintassi PEP 695 'type X = ...')" >&2; exit 1; }
echo "  python: $py ($($py --version 2>&1))"
for t in git cmake ninja g++ zstd; do
  command -v "$t" >/dev/null 2>&1 && echo "  $t: ok" || echo "  ⚠ $t MANCANTE (apt install $t)"
done
nproc_n=$(nproc); echo "  core: $nproc_n"
command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi --query-gpu=name,memory.total,compute_cap --format=csv,noheader | sed 's/^/  GPU: /'

echo "=== 2/5 clone di upstream al pin"
if [ -d "$DEST/nnue-pytorch/.git" ]; then
  echo "  gia' presente, salto il clone"
else
  mkdir -p "$DEST"
  git clone -q https://github.com/official-stockfish/nnue-pytorch.git "$DEST/nnue-pytorch"
fi
cd "$DEST/nnue-pytorch"
git checkout -q "$PIN"
echo "  HEAD: $(git log -1 --format='%h %ad %s' --date=short)"

echo "=== 3/5 patch Triumviratus (PassedPawns + ordine dei blocchi)"
if git diff --quiet -- model/modules/features/__init__.py 2>/dev/null && \
   [ ! -f model/modules/features/passed_pawns.py ]; then
  git apply --verbose "$PATCH"
  echo "  applicata."
else
  echo "  gia' applicata, salto."
fi

echo "=== 4/5 build del data loader"
# Build Release semplice: lo script upstream fa un giro PGO che richiede un binpack di
# campione e llvm-profdata, e a noi non serve per la correttezza.
cmake -S data_loader/cpp -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLIB_COPY_DIR="$PWD" >/dev/null
cmake --build build --target training_data_loader -j"$nproc_n" >/dev/null
ls -la ./*training_data_loader*.so build/*training_data_loader*.so 2>/dev/null | head -2
# il loader cerca la libreria in ./build/**: assicuriamocene
[ -f build/libtraining_data_loader.so ] || [ -f build/training_data_loader.so ] || \
  cp ./*training_data_loader*.so build/ 2>/dev/null || true

echo "=== 5/5 dipendenze python + verifica"
$py -m pip install -q -U "huggingface_hub[hf_transfer]" >/dev/null
$py -m pip install -q -r requirements.txt >/dev/null
# 🔴 DOPO requirements.txt, non prima: la risoluzione delle dipendenze di torch tira su
# numpy 2.5, ma numba (usato da model/utils/serialize.py) vuole <= 2.4 e fallisce
# all'import. Il pin va messo qui, altrimenti viene sovrascritto.
$py -m pip install -q "numpy<2.5" >/dev/null
# 🔴 CuPy NON e' in requirements.txt, ma senza di lui RangerLite ricade sul percorso di
# aggiornamento in Python puro: misurato in locale il 27% del tempo per step a batch 16384,
# e l'unico segnale e' una UserWarning persa in un log da 600 epoche. Triton (per
# --compile-backend inductor) arriva gia' con le wheel torch su Linux.
# 🔴 PIN <14 OBBLIGATORIO. cupy-cuda12x 14.x e' incompatibile con i pacchetti NVIDIA che
# torch si porta dietro e fallisce all'import:
#     ImportError: cannot import name find_nvidia_binary_utility
# Da quell'import dipendono i kernel FUSI del feature transformer (che sono il 78% del
# tempo GPU) e l'optimizer fuso RangerLite. Senza, si ricade su aten::_embedding_bag e si
# perde il 36% di throughput, con l'unico segnale una UserWarning in mezzo al log.
# Misurato il 28/07: 130k -> 177k pos/s solo riparando questo. 13.6.0 verificata OK.
$py -m pip install -q "cupy-cuda12x<14" >/dev/null || \
  echo "  ⚠️ CuPy non installato: FT e optimizer sul percorso lento (-36% circa)"
# 🔴 La wheel di torch deve combaciare col DRIVER, non solo con la GPU: una wheel cu130 su
# un driver CUDA 12.8 importa senza errori e fallisce solo al primo .to("cuda"), cioe' dopo
# aver scaricato i dati. Qui si verifica in dieci secondi, prima di tutto il resto.
$py - <<'PYCHK'
import subprocess, sys, torch
print(f"  torch: {torch.__version__} | cuda disponibile: {torch.cuda.is_available()}")
drv = subprocess.run(["nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader"],
                     capture_output=True, text=True).stdout.strip().splitlines()
print(f"  driver: {drv[0] if drv else 'sconosciuto'}")
try:
    torch.zeros(8, device="cuda")            # e' QUESTO che scopre il mismatch
    print(f"  ✅ GPU utilizzabili: {torch.cuda.device_count()}")
except RuntimeError as e:
    print(f"  ❌ torch non riesce a usare la GPU: {e}", file=sys.stderr)
    print("     Wheel e driver non combaciano. Installa la build giusta, es. per CUDA 12.8:",
          file=sys.stderr)
    print("     uv pip install --reinstall torch --index-url https://download.pytorch.org/whl/cu128",
          file=sys.stderr)
    sys.exit(1)
PYCHK
$py - <<'PY'
from model.modules.features import DEFAULT_FEATURES, get_feature_cls, _FEATURE_COMPONENTS

# Il kernel fuso dell'optimizer e i kernel Triton degradano in SILENZIO se mancano:
# qui li rendiamo rumorosi, perche' su una GPU a noleggio si paga la differenza a ore.
try:
    import cupy  # noqa: F401
    print("  ✅ CuPy presente: kernel fuso dell'optimizer attivo")
except ImportError:
    print("  ⚠️ CuPy ASSENTE: RangerLite usera' il percorso Python (piu' lento)")
try:
    import triton  # noqa: F401
    print("  ✅ Triton presente: --compile-backend inductor utilizzabile")
except ImportError:
    print("  ⚠️ Triton ASSENTE: usare --compile-backend cudagraphs")
assert "PassedPawns" in _FEATURE_COMPONENTS, "patch NON applicata: PassedPawns non registrato"
assert DEFAULT_FEATURES == "Full_Threats+HalfKAv2_hm^+PP_3Wide+PassedPawns", \
    f"ordine dei blocchi sbagliato: {DEFAULT_FEATURES}"
tot = sum(c.NUM_REAL_FEATURES for c in get_feature_cls(DEFAULT_FEATURES))
print(f"  feature set: {DEFAULT_FEATURES}")
print(f"  input reali totali: {tot}   (atteso 86992)")
assert tot == 86992, "conteggio input diverso dall'atteso: motore e trainer divergerebbero"
print("  ✅ trainer pronto e coerente col motore")
PY

cat <<EOF

=== SETUP COMPLETO in $DEST/nnue-pytorch

Prossimi passi:
  1) $HERE/download_phase1.sh /data/phase1 3
  2) $HERE/train_phase1.sh    /data/phase1 24
     -> cronometra la PRIMA epoca e guarda 'nvidia-smi': se l'utilizzo GPU e' < 60%
        il collo e' il data loader, alza --num-workers.
  3) a fase 1 finita:
     $HERE/download_phase2.sh <modello_fase1.pt> /data/phase2 380 3
     $HERE/train_phase2.sh    <modello_fase1.pt> /data/phase2 24
EOF

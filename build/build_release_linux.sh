#!/usr/bin/env bash
# =============================================================================
#  build_release_linux.sh — la matrice Linux di Triumviratus 7.0, con PGO.
#
#  E' il gemello Linux di build_release_all.ps1: stessa pipeline (PGO clang +
#  ThinLTO), stessi DUE canary, ma solo le varianti che servono qui. Sostituisce
#  build_release_native.sh, che costruiva e basta: nessun controllo sul binario
#  prodotto, e puntava ancora a Triumviratus_6.
#
#  USO
#    ./build_release_linux.sh                     # avx512 (il rig), build di TEST
#    ./build_release_linux.sh --release           # avx512, build di SPEDIZIONE
#    ./build_release_linux.sh avx2 avx512         # piu' varianti
#    ./build_release_linux.sh --release all       # la matrice Linux intera
#    CANARY=259746 ./build_release_linux.sh       # firma attesa esplicita
#
#  TEST vs SPEDIZIONE — la differenza conta, e non e' cosmetica:
#    test (default)  opzioni UCI di tuning VISIBILI  -> serve al rig: senza,
#                    fastchess manda `setoption` che il motore SCARTA IN SILENZIO
#                    e si misura BASE contro BASE (gia' costato una notte il 16/08)
#    --release       -DTRIUMV_RELEASE: restano solo Hash/Threads/Move Overhead/
#                    EvalFile/SyzygyPath. Byte-identico nella ricerca (il canary
#                    lo verifica), cambia solo cosa si dichiara in `uci`.
#
#  VARIANTI (i gradini sono quelli del Makefile, vedi i suoi commenti)
#    avx512       F/BW/DQ/VL, NIENTE VNNI   Skylake-SP/X, Xeon W-21xx  <- il rig
#    vnni512      + VNNI                    Cascade Lake, Ice Lake SP, Zen4
#    avx512icl    + VBMI/VBMI2/BITALG       Ice Lake client+, Sapphire Rapids, Zen5
#    avx2         AVX2 + BMI2/PEXT          AMD Zen3+           (persp ON)
#    avx2-intel   come avx2, persp OFF      Intel Haswell..Rocket Lake
#    avx2-nopext  AVX2 senza PEXT           AMD Zen1/Zen2 (pext microcodato)
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$PWD"
SRC="$ROOT/Triumviratus_7"
OUT="$ROOT/_release/linux_7.0"
BOOK="${BOOK:-$ROOT/OpeningBooks/UHO_4060_v4/UHO_4060_v4.epd}"
CXX="${RELCXX:-clang++}"
PGO_POS="${PGO_POS:-200}"
PGO_WORKERS="${PGO_WORKERS:-$(nproc)}"
OBJDUMP="${OBJDUMP:-llvm-objdump}"
# 🔴 Canary ARMATO di default dal 09/09/2026, come il gemello Windows: prima era
# disarmato se non si passava CANARY=..., e un canary che va ricordato a mano non e'
# un canary. 240503 = bake di ContHistMulti spento (10/09); era 242956, vettore blend eval. Disarmarlo: CANARY= ./build...
CANARY="${CANARY-240503}"

RELEASE=0
VARIANTS=()
for a in "$@"; do
  case "$a" in
    --release) RELEASE=1 ;;
    all)       VARIANTS+=(avx512 vnni512 avx512icl avx2 avx2-intel avx2-nopext) ;;
    -*)        echo "flag sconosciuta: $a" >&2; exit 1 ;;
    *)         VARIANTS+=("$a") ;;
  esac
done
[ ${#VARIANTS[@]} -eq 0 ] && VARIANTS=(avx512)

# --- come si costruisce ogni variante (target del Makefile + variabili) ------
make_args_for() {
  case "$1" in
    avx512)      echo "pgo-avx512 PGOGOAL=avx512" ;;
    vnni512)     echo "pgo-avx512 PGOGOAL=vnni512" ;;
    avx512icl)   echo "pgo-avx512 PGOGOAL=avx512icl" ;;
    avx2)        echo "pgo-avx2" ;;
    avx2-intel)  echo "pgo-avx2 INTEL=1" ;;
    avx2-nopext) echo "pgo-avx2 NOPEXT=1" ;;
    *) echo "" ;;
  esac
}

# --- CANARY ISA: mnemonici che NON devono comparire, per variante -----------
# 🔴 Perche' STATICO e non a runtime (lezione del 10/08/2026). Il 6/08 fu spedito
# un `avx512` con 44 `vpdpbusd` (VNNI). Su Skylake-X — che il VNNI non ha, arriva
# con Cascade Lake — il motore risponde a `uci`, dice `readyok` e muore di SIGILL
# alla prima valutazione. Il canary bench non lo vide per una ragione strutturale:
# e' un controllo a runtime fatto sulla macchina di BUILD, che esegue tutto cio'
# che compila. 🔑 Una macchina che POSSIEDE la feature non puo' falsificare
# l'assunzione che ci sia. Quindi si guarda dentro il binario, non se parte.
# ⚠️ E questo rig e' esattamente il caso: dual Xeon Gold 6138 = Skylake-SP, AVX-512
# SENZA VNNI. Un `avx512` mal costruito muore qui.
# `zmm` prende qualunque uso di AVX-512 (e' nel nome dei registri); `\bpext\b` col
# confine di parola non collide con pextrb/w/d/q, che sono SSE4 e legittime.
forbidden_for() {
  case "$1" in
    avx512)      echo 'vpdpbusd|vpdpwssd|vpermb|vperm[it]2b|vpcompress[bw]|vpexpand[bw]|vpshufbitqmb|vpopcnt[bw]|vpsh[lr]dv' ;;
    vnni512)     echo 'vpermb|vperm[it]2b|vpcompress[bw]|vpexpand[bw]|vpshufbitqmb|vpopcnt[bw]|vpsh[lr]dv' ;;
    avx512icl)   echo '' ;;                       # gradino piu' alto: niente da vietare
    avx2|avx2-intel) echo 'zmm|vpdpbusd|vpdpwssd' ;;
    avx2-nopext) echo 'zmm|vpdpbusd|vpdpwssd|\bpext\b|\bpdep\b' ;;
    *) echo '' ;;
  esac
}

echo "===== BUILD LINUX 7.0 ====="
echo "  varianti : ${VARIANTS[*]}"
echo "  modo     : $([ $RELEASE -eq 1 ] && echo 'SPEDIZIONE (-DTRIUMV_RELEASE)' || echo 'TEST (opzioni di tuning visibili)')"
echo "  compiler : $CXX    workers PGO: $PGO_WORKERS    posizioni: $PGO_POS"
echo "  libro    : $BOOK"
[ -f "$BOOK" ] || { echo "[!] libro non trovato: $BOOK" >&2; exit 1; }
command -v "$OBJDUMP" >/dev/null || { echo "[!] $OBJDUMP non trovato: il canary ISA non potrebbe girare. Installa llvm." >&2; exit 1; }
mkdir -p "$OUT"
cd "$SRC"
[ -f nn-legio-septima.nnue ] || echo "[!] nn-legio-septima.nnue assente alla build-root: la rete NON sara' embeddata"

# --- FLAG PROVATE E SCARTATE ------------------------------------------------
# -mbranches-within-32B-boundaries (provata il 09/09/2026, NON adottata).
# Su Skylake (questo rig e' modello 85) l'erratum JCC ha una mitigazione a
# microcodice che ESCLUDE dalla cache di micro-operazioni i salti a cavallo di un
# confine di 32 byte. Si vedeva: la copertura DSB era 22,3%, bassissima. La flag
# fa esattamente il suo mestiere e la porta a 31,4%, con 600 micro-operazioni per
# nodo in meno a carico del decoder.
# 🔑 E non cambia NULLA: 5.673,3 +- 27,0 cicli/nodo contro 5.666,6 +- 28,6, su 16
# coppie alternate. Il meccanismo era reale e il collo di bottiglia era altrove -
# il top-down dice backend 37,4% contro frontend 18,6%, cioe' la macchina aspetta
# la memoria, e consegnarle le istruzioni piu' in fretta non la fa aspettare meno.
# Vale come misura NEGATIVA da non ripetere: chi vedra' quel 22% di copertura
# pensera' che sia un difetto da correggere, e non lo e'.

# --- LA CPU DI QUESTA MACCHINA REGGE LA VARIANTE? ---------------------------
# 🔴 Lezione del 09/09/2026. `--release all` e' morto sulla vnni512 con un traceback
# Python di pgo_train.py ("motore terminato inaspettatamente"), cinque minuti dopo
# l'avvio e senza una parola sulla causa vera: il PGO deve ESEGUIRE il binario che
# sta addestrando, e su questo rig (Xeon Gold 6138 = Skylake-SP, AVX-512 SENZA VNNI)
# il binario vnni512 muore di istruzione illegale al primo `vpdpbusd`. Il canary ISA
# non poteva vederlo: gira DOPO la build, e la build non ci arriva mai.
# 🔑 Un target che la macchina di build non sa eseguire non e' costruibile con PGO,
# punto. vnni512 e avx512icl vanno costruite su una macchina che ha quelle istruzioni
# (il portatile: vedi bootstrap_release_laptop.ps1).
needed_isa_for() {
  case "$1" in
    vnni512)     echo 'avx512_vnni' ;;
    avx512icl)   echo 'avx512_vnni avx512_vbmi avx512_bitalg avx512_vpopcntdq' ;;
    avx512)      echo 'avx512f avx512bw avx512dq avx512vl' ;;
    avx2|avx2-intel) echo 'avx2 bmi2' ;;
    avx2-nopext) echo 'avx2' ;;
    *) echo '' ;;
  esac
}
CPUFLAGS=" $(grep -m1 '^flags' /proc/cpuinfo | cut -d: -f2-) "
for v in "${VARIANTS[@]}"; do
  for f in $(needed_isa_for "$v"); do
    case "$CPUFLAGS" in
      *" $f "*) ;;
      *) echo "[!] variante '$v': questa CPU non ha '$f'." >&2
         echo "    Il PGO esegue il binario che addestra, quindi qui morirebbe di" >&2
         echo "    istruzione illegale a meta' addestramento. Costruiscila su una" >&2
         echo "    macchina che ha quell'istruzione (vedi bootstrap_release_laptop.ps1)." >&2
         exit 1 ;;
    esac
  done
done

RELGOAL=""; SUFFIX=""
if [ $RELEASE -eq 1 ]; then RELGOAL="RELGOAL=release"; SUFFIX="_release"; fi

declare -A SIG
for v in "${VARIANTS[@]}"; do
  ARGS="$(make_args_for "$v")"
  [ -n "$ARGS" ] || { echo "[!] variante sconosciuta: $v" >&2; exit 1; }
  echo
  echo "===== $v ====="
  # `make clean` fra una variante e l'altra: gli .o non si ricompilano al cambio
  # di flag, e quelli della fase PGO-gen sono bitcode ThinLTO che il link normale
  # non riconosce nemmeno.
  make clean >/dev/null
  # shellcheck disable=SC2086
  # EXTRACXXFLAGS: define extra per una misura A/B (es. -DTRIUMV_FROZEN da solo).
  # Passato sulla riga di comando di make, quindi make lo propaga da se' ai
  # sub-make delle fasi PGO via MAKEFLAGS: non serve un export come in Stockfish.
  make $ARGS $RELGOAL CXX="$CXX" BOOK="$BOOK" PGO_POS="$PGO_POS" PGO_WORKERS="$PGO_WORKERS" \
    EXTRACXXFLAGS="${EXTRACXXFLAGS:-}" \
    > "$OUT/build_$v.log" 2>&1 || { echo "[!] build fallita, vedi $OUT/build_$v.log"; tail -20 "$OUT/build_$v.log"; exit 1; }
  BIN="$OUT/triumviratus_linux_${v}${SUFFIX}"
  mv -f triumviratus "$BIN"
  SIG[$v]=$(printf 'bench\nquit\n' | "$BIN" 2>/dev/null | grep -oP 'Nodes searched\s+:\s+\K[0-9]+' || echo ERRORE)
  echo "  -> $(basename "$BIN")   bench ${SIG[$v]}   $(du -h "$BIN" | cut -f1)"
done

# --- CANARY 1: la firma bench, uguale su tutta la matrice -------------------
# Le varianti differiscono solo per ISA: stessi attacchi, stesso albero. Una
# firma diversa fra due varianti significa che una calcola qualcosa di diverso.
echo
echo "===== CANARY BENCH ====="
UNIQ=$(printf '%s\n' "${SIG[@]}" | sort -u)
if [ "$(echo "$UNIQ" | wc -l)" -ne 1 ]; then
  echo "[!] FIRME DIVERSE fra le varianti:"; for v in "${VARIANTS[@]}"; do printf '    %-14s %s\n' "$v" "${SIG[$v]}"; done
  exit 1
fi
echo "  tutta la matrice a $UNIQ"
if [ -n "${CANARY:-}" ] && [ "$UNIQ" != "$CANARY" ]; then
  echo "[!] firma $UNIQ diversa dall'attesa $CANARY: il comportamento e' cambiato. NON rilasciare senza saperne il motivo."
  exit 1
fi

# --- CANARY 2: ISA, statico -------------------------------------------------
echo
echo "===== CANARY ISA (statico) ====="
FAIL=0
for v in "${VARIANTS[@]}"; do
  PAT="$(forbidden_for "$v")"
  BIN="$OUT/triumviratus_linux_${v}${SUFFIX}"
  if [ -z "$PAT" ]; then printf '  %-14s ok (nessun vincolo)\n' "$v"; continue; fi
  HITS=$("$OBJDUMP" -d "$BIN" 2>/dev/null | grep -cE "$PAT" || true)
  if [ "$HITS" -gt 0 ]; then
    MN=$("$OBJDUMP" -d "$BIN" 2>/dev/null | grep -oE "$PAT" | sort -u | tr '\n' ' ')
    printf '  %-14s ⛔ %s istruzioni FUORI TARGET: %s\n' "$v" "$HITS" "$MN"
    FAIL=1
  else
    printf '  %-14s ok, zero istruzioni fuori target\n' "$v"
  fi
done
[ $FAIL -eq 0 ] || { echo; echo "[!] CANARY ISA FALLITO. Su una CPU senza quella feature il motore muore di SIGILL: NON rilasciare."; exit 1; }

echo
echo "[+] fatto. Binari in $OUT/"
ls -la "$OUT"/triumviratus_linux_* 2>/dev/null | sed 's/^/    /'
echo
echo "    Per il rig:  cp $OUT/triumviratus_linux_avx512 $ROOT/_rig_locale/bin/engine"
echo "    e allineare la firma attesa in _rig_locale/run.sh e match.sh."

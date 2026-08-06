# =============================================================================
#  Makefile — build LINUX dell'engine Triumviratus (g++/clang++).
#  La build Windows resta su MSVC (.sln/.vcxproj); questo e' SOLO per Linux
#  (es. generazione self-play su GCP). Le differenze OS sono #ifdef-guardate.
#
#  USO:
#    make              -> build AVX2 + BMI2/PEXT (Intel, AMD Zen3+)
#    make clean nopext -> build AVX2 SENZA pext  (AMD Zen1/Zen2: OBBLIGATORIA, vedi sotto)
#    make clean avx512 -> build AVX512 + VNNI (Intel n2/c3, AMD Zen4 c3d) = piu' veloce
#    make CXX=clang++  -> usa clang invece di g++
#    make clean
#
#  ⚠️ PEXT (`make` liscia) e' 3 cicli su Intel e Zen3+, ma **microcodato ~18 cicli su
#     AMD Zen1/Zen2**: li' va usato `nopext` (o NOPEXT=1), che passa al fallback
#     fancy-magics + dual-hyperbola-quintessence. Misurato +21,4% NPS sulla VM Zen2
#     (EPYC 7B12). Le due build sono node-identical: stessi attacchi, stesso bench.
#  ⚠️ Cambiando arch (avx2<->avx512) fai prima `make clean` (gli .o non si
#     ricompilano da soli al cambio di flag).
#  ⚠️ Le reti .nnue NON sono embedded (-DNNUE_EMBEDDING_OFF): vanno messe
#     accanto al binario (come su Windows). resolve_net_path le trova via
#     /proc/self/exe.
# =============================================================================
CXX ?= g++
CC  ?= gcc
EXE ?= triumviratus

# TRANN1 NNUE (nnue/), SFNNv13-derived + PawnPair/PassedPawns. We compile ONLY the subset
# the NNUE eval needs (mirror of the Windows vcxproj): position_min.cpp provides
# Position::set_pieces + update_piece_threats; the heavy master position.cpp /
# nnue_misc.cpp / evaluate.cpp / m1_evalmatch.cpp / tune/score/ucioption are NOT
# compiled (they pull movegen/uci closures not needed for eval). Explicit list (no
# wildcard) so those excluded files never sneak in.
V13 := nnue
CPP_SRCS := $(filter-out nnt.cpp,$(wildcard *.cpp)) \
            $(V13)/position_min.cpp $(V13)/bitboard.cpp $(V13)/attacks.cpp \
            $(V13)/misc.cpp $(V13)/memory.cpp \
            $(V13)/nnue/network.cpp $(V13)/nnue/nnue_accumulator.cpp \
            $(V13)/nnue/features/feat_perm.cpp \
            $(V13)/nnue/features/full_threats.cpp $(V13)/nnue/features/half_ka_v2_hm.cpp \
            $(V13)/nnue/features/pawn_pair.cpp $(V13)/nnue/features/passed_pawns.cpp
C_SRCS   := fathom/tbprobe.c

OBJS := $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

# Define replicati dalla build MSVC Release (USE_POPCNT su GCC = __builtin_popcountll
# = hardware, risultato identico). TRIUMV5_THREATS_INCR = accumulatore-threats
# incrementale (default, come la build Windows).
# EMBEDDING (2026-07-13): se il net di default (EvalFileDefaultName, vedi
# nnue/evaluate.h) e' presente alla build-root, viene EMBEDDATA nel binario via
# incbin (un file omonimo accanto all'exe la sovrascrive; EvalFile carica
# qualsiasi path). Senza file -> NNUE_EMBEDDING_OFF come prima (es. bundle
# sorgenti-only per le VM). NB: il nome deve restare allineato a evaluate.h.
NET_NAME := nn-legio-septima.nnue
ifneq (,$(wildcard $(NET_NAME)))
  EMBED_DEF :=
  $(info [i] $(NET_NAME) trovata: rete EMBEDDATA nel binario)
else
  EMBED_DEF := -DNNUE_EMBEDDING_OFF
  $(info [i] nessuna $(NET_NAME) alla build-root: embedding OFF, serve il file accanto al binario)
endif
DEFS := -DNDEBUG $(EMBED_DEF) -DTRIUMV5_THREATS_INCR -DUSE_PEXT -DUSE_POPCNT \
        -DUSE_SSE -DUSE_SSE2 -DUSE_SSSE3 -DUSE_SSE41 -DUSE_AVX2

ARCH := -mavx2 -mbmi2 -mfma -mpopcnt -msse4.1 -mssse3 -msse2

# `make avx512` aggiunge AVX512 + VNNI + ICL (VBMI/VBMI2/BITALG) = identico alla
# build Windows shipping (TRANN1 forward + dirty-threats vettoriali). OK su c3
# (Sapphire Rapids) e c3d (Zen4): entrambi hanno VBMI/VBMI2.
ifeq (avx512,$(filter avx512,$(MAKECMDGOALS)))
  DEFS += -DUSE_AVX512 -DUSE_VNNI -DUSE_AVX512ICL
  ARCH += -mavx512f -mavx512bw -mavx512dq -mavx512vl -mavx512vnni \
          -mavx512vbmi -mavx512vbmi2 -mavx512bitalg
endif

# `make nopext` (equivalente: NOPEXT=1, che propaga anche ai target PGO ricorsivi):
# toglie USE_PEXT/-mbmi2 -> magic.cpp usa il fallback fancy-magics e nnue/attacks.h
# il percorso DUAL_HYPERBOLA_QUINT (AVX2). Stessi attacchi = node-identical: la firma
# bench NON deve cambiare. Obbligatoria su AMD Zen1/Zen2 (pext microcodato).
ifneq (,$(filter nopext,$(MAKECMDGOALS))$(NOPEXT))
  DEFS := $(filter-out -DUSE_PEXT,$(DEFS))
  ARCH := $(filter-out -mbmi2,$(ARCH))
  $(info [i] build senza PEXT (fancy-magics + dual hyperbola quintessence))
endif

# `make intel` (equivalente: INTEL=1, che propaga ai target PGO ricorsivi): asse VENDOR,
# ortogonale all'ISA — si combina con avx512, nopext o niente. Spegne `persp`
# (append_changed_indices_both, porting SF 7b550409), che aggiorna le due prospettive in
# una sola passata sulla dirty list. Misurato a ordine alternato con nulli di apertura e
# chiusura: +0,63% su Zen2, +1,87% su Zen4, ma **-1,2% su Intel Rocket Lake** (15/300
# posizioni). Cambia solo COME si aggiorna l'accumulatore, non cosa contiene:
# node-identical, la firma bench NON deve cambiare.
# ⚠️ A compile-time e non con dispatch cpuid: sotto PGO il ramo non preso durante il
# training resta freddo, quindi un dispatch a runtime svantaggerebbe per costruzione uno
# dei due percorsi.
ifneq (,$(filter intel,$(MAKECMDGOALS))$(INTEL))
  DEFS += -DTRIUMV_NO_PERSP_BOTH
  $(info [i] build Intel: persp (append_changed_indices_both) SPENTA)
endif

# 🔴 6/08/2026 — `persp` si spegne anche per ISA, non solo per vendor. Misurato su Zen4
# 8845HS, build PGO fresche e node-identical (bench 261287 su tutte le varianti):
#     AVX2    : +2,3% con persp ACCESA (mediana stabile su 5 letture)
#     AVX-512 : ~-1,3%, lettura instabile (+0,97 / -2,60 / -1,35 / -1,82 / -1,28)
# Su AVX-512 i tile sono piu' larghi e la pressione sui registri cambia: stesso profilo
# della pawn cache (+1,37% AVX2, -0,11% AVX-512, spenta li'). Negativa su AMD, e su Intel
# era gia' -1,17% ⇒ nessun vendor la vuole accesa su AVX-512: niente split, una build sola.
# ⚠️ Il -1,3% e' meno solido del +2,3% (60 posizioni, mediana instabile, senza nullo di
#    sessione). Basta a NON accendere una patch; non basterebbe ad accenderla.
ifeq (avx512,$(filter avx512,$(MAKECMDGOALS)))
  ifeq (,$(findstring TRIUMV_NO_PERSP_BOTH,$(DEFS)))
    DEFS += -DTRIUMV_NO_PERSP_BOTH
    $(info [i] build AVX-512: persp SPENTA per ISA (misura 6/08/2026))
  endif
endif

# `make profile` abilita i cycle-counter per-fase (eval/movegen/make/tt/score) nel
# bench (vedi profile.h). Default = no-op, byte-identico.
ifeq (profile,$(filter profile,$(MAKECMDGOALS)))
  DEFS += -DTRIUMV_PROFILE
endif

# `make release` nasconde dai UCI tutti i parametri di tuning/diagnostica, lasciando
# solo le opzioni ufficiali (Hash/Threads/Move Overhead/EvalFile/SyzygyPath).
ifeq (release,$(filter release,$(MAKECMDGOALS)))
  DEFS += -DTRIUMV_RELEASE
endif

# ---- ANDROID (arm64) --------------------------------------------------------
#   make clean && make android NDK=/path/android-ndk-r27          # arm64-v8a (ogni 64bit)
#   make clean && make android-dotprod NDK=/path/android-ndk-r27  # armv8.2+dotprod (2018+, NNUE piu' veloce)
# Niente USE_PEXT -> magic.cpp usa il fallback fancy-magics BBC (gia' nel codice,
# costanti precalcolate in init.cpp) e nnue/ i suoi magics SF; i path NEON del
# vendored (USE_NEON / USE_NEON_DOTPROD, convenzione SF) sono gia' presenti.
# Su Windows lancialo da Git Bash/MSYS2 (i compiler NDK hanno wrapper .cmd).
# Uso: binario + .nnue accanto, in Termux o GUI Android (DroidFish etc.).
LIBS := -lpthread -lm
ifneq (,$(filter android android-dotprod,$(MAKECMDGOALS)))
  ifeq ($(NDK),)
    $(error make android richiede NDK=<path all'Android NDK, es. /c/android-ndk-r27>)
  endif
  UNAME_S := $(shell uname -s)
  ifneq (,$(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S)))
    HOST_TAG := windows-x86_64
  else ifeq ($(UNAME_S),Darwin)
    HOST_TAG := darwin-x86_64
  else
    HOST_TAG := linux-x86_64
  endif
  # clang++ "nudo" + --target (i wrapper aarch64-*-clang++ sono .cmd su Windows e
  # non girano sotto make/MSYS2; il target esplicito e' equivalente, NDK r19+).
  TC   := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin
  CXX  := $(TC)/clang++
  CC   := $(TC)/clang
  DEFS := -DNDEBUG $(EMBED_DEF) -DTRIUMV5_THREATS_INCR -DUSE_POPCNT -DUSE_NEON=8
  ARCH := --target=aarch64-linux-android24
  # bionic: pthread dentro libc (niente -lpthread); 16KB page-size per Android 15+.
  LIBS := -lm -static-libstdc++ -Wl,-z,max-page-size=16384
  EXE  := triumviratus_android
endif
ifneq (,$(filter android-dotprod,$(MAKECMDGOALS)))
  DEFS += -DUSE_NEON_DOTPROD
  ARCH += -march=armv8.2-a+dotprod
endif

OPT      := -O3 -funroll-loops

# ---- LTO / PGO (Linux, clang++ only) ----------------------------------------
#   make avx512 CXX=clang++ LTO=1            -> ThinLTO sui target normali
#   make pgo-avx512 CXX=clang++ [BOOK=...]   -> pipeline PGO completa (2 fasi):
#     fase A strumentata -> pgo_train.py (workload UCI reale) -> llvm-profdata
#     merge -> fase B -fprofile-use. Stessa ricetta della build Windows
#     (build_pgo_clang_6_trann1.ps1) che vale ~+9%. Il motore ha bisogno della
#     rete a runtime durante il training (embedded se il .nnue e' alla root,
#     altrimenti accanto all'exe). g++ NON supportato per questi target.
COMPILER_VER := $(shell $(CXX) --version 2>/dev/null)
IS_CLANG     := $(findstring clang,$(COMPILER_VER))
ifeq ($(LTO),1)
  ifeq (,$(IS_CLANG))
    $(error LTO=1 richiede CXX=clang++ (-flto=thin non esiste su g++))
  endif
  OPT += -flto=thin
endif
PROFDIR       ?= pgo_profiles
BOOK          ?= $(HOME)/engine/book_8mvs.epd
TRAINER       ?= ../pgo_train.py
PGO_POS       ?= 200
PGO_WORKERS   ?= $(shell nproc 2>/dev/null || echo 8)
LLVM_PROFDATA ?= llvm-profdata
ifeq (gen,$(PGO_PHASE))
  OPT  += -flto=thin -fprofile-generate=$(PROFDIR) -mtune=native
  DEFS += -DCLANG_PGO_GEN            # quit-handler flusha il profilo (uci_mt.cpp)
endif
ifeq (use,$(PGO_PHASE))
  OPT  += -flto=thin -fprofile-use=$(PROFDIR)/merged.profdata -mtune=native \
          -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled
endif

# EXTRACXXFLAGS: flag/define extra dalla riga di comando, es.
#   make EXTRACXXFLAGS=-UUSE_PEXT        (equivalente a `make nopext`)
#   make EXTRACXXFLAGS=-DTRIUMV_NO_PAWN_CACHE   (baseline A/B di una misura)
# Va in FONDO alle flag: un -U/-D qui vince su quelli di DEFS.
# 🔴 Prima del 5/08/2026 questa variabile NON esisteva e setup_spsa_vm.sh la passava
#    comunque: il -UUSE_PEXT delle VM Zen2 veniva ignorato IN SILENZIO.
CXXFLAGS ?= -std=c++17 $(OPT) -w $(DEFS) $(ARCH) $(EXTRACXXFLAGS) -pthread -I. -Ifathom
CFLAGS   ?= -O3 -w $(ARCH) -pthread -I. -Ifathom

all: $(EXE)
avx512: $(EXE)
nopext: $(EXE)
intel: $(EXE)
profile: $(EXE)
release: $(EXE)
android: $(EXE)
android-dotprod: $(EXE)

$(EXE): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LIBS)
	@echo "[+] Fatto: ./$(EXE)  (metti le .nnue accanto al binario)"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(EXE)

# ---- PGO pipeline (ricorsivo: clean fra le fasi = niente .o con flag misti) --
pgo-avx512:
ifeq (,$(IS_CLANG))
	$(error pgo-avx512 richiede CXX=clang++ (formato profdata LLVM))
endif
	@mkdir -p $(PROFDIR) && rm -f $(PROFDIR)/*.profraw
	@test -f $(BOOK) || echo "[!] libro $(BOOK) assente: pgo_train.py usera' i FEN builtin"
	$(MAKE) clean
	$(MAKE) avx512 $(RELGOAL) CXX=$(CXX) PGO_PHASE=gen
	LLVM_PROFILE_FILE="$(PROFDIR)/prof_%p.profraw" \
	  python3 $(TRAINER) ./$(EXE) 0 $(BOOK) $(PGO_POS) --workers $(PGO_WORKERS)
	$(LLVM_PROFDATA) merge -output=$(PROFDIR)/merged.profdata $(PROFDIR)/*.profraw
	$(MAKE) clean
	$(MAKE) avx512 $(RELGOAL) CXX=$(CXX) PGO_PHASE=use
	@echo "[+] PGO fatto: ./$(EXE)  (verifica: bench 13 deve dare la stessa firma della build liscia)"

pgo-avx2:
ifeq (,$(IS_CLANG))
	$(error pgo-avx2 richiede CXX=clang++ (formato profdata LLVM))
endif
	@mkdir -p $(PROFDIR) && rm -f $(PROFDIR)/*.profraw
	$(MAKE) clean
	$(MAKE) all $(RELGOAL) CXX=$(CXX) PGO_PHASE=gen
	LLVM_PROFILE_FILE="$(PROFDIR)/prof_%p.profraw" \
	  python3 $(TRAINER) ./$(EXE) 0 $(BOOK) $(PGO_POS) --workers $(PGO_WORKERS)
	$(LLVM_PROFDATA) merge -output=$(PROFDIR)/merged.profdata $(PROFDIR)/*.profraw
	$(MAKE) clean
	$(MAKE) all $(RELGOAL) CXX=$(CXX) PGO_PHASE=use
	@echo "[+] PGO fatto: ./$(EXE)"

.PHONY: all avx512 nopext intel profile release android android-dotprod clean pgo-avx512 pgo-avx2

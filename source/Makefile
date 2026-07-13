# =============================================================================
#  Makefile — build LINUX dell'engine Triumviratus (g++/clang++).
#  La build Windows resta su MSVC (.sln/.vcxproj); questo e' SOLO per Linux
#  (es. generazione self-play su GCP). Le differenze OS sono #ifdef-guardate.
#
#  USO:
#    make              -> build AVX2 + BMI2 (sicura su quasi ogni x86-64 moderna)
#    make clean avx512 -> build AVX512 + VNNI (Intel n2/c3, AMD Zen4 c3d) = piu' veloce
#    make CXX=clang++  -> usa clang invece di g++
#    make clean
#
#  ⚠️ L'engine usa _pext_u64 SEMPRE (magic.cpp) -> serve BMI2. Su AMD Zen2
#     (istanze n2d/c2d) PEXT e' lento: preferisci Intel (n2/c3) o AMD Zen4 (c3d).
#  ⚠️ Cambiando arch (avx2<->avx512) fai prima `make clean` (gli .o non si
#     ricompilano da soli al cambio di flag).
#  ⚠️ Le reti .nnue NON sono embedded (-DNNUE_EMBEDDING_OFF): vanno messe
#     accanto al binario (come su Windows). resolve_net_path le trova via
#     /proc/self/exe.
# =============================================================================
CXX ?= g++
CC  ?= gcc
EXE ?= triumviratus

# Vendored Stockfish-master SFNNv13 NNUE (nnue/). We compile ONLY the subset
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
            $(V13)/nnue/features/full_threats.cpp $(V13)/nnue/features/half_ka_v2_hm.cpp \
            $(V13)/nnue/features/pawn_pair.cpp
C_SRCS   := fathom/tbprobe.c

OBJS := $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

# Define replicati dalla build MSVC Release (USE_POPCNT su GCC = __builtin_popcountll
# = hardware, risultato identico). TRIUMV5_THREATS_INCR = accumulatore-threats
# incrementale (default, come la build Windows).
# EMBEDDING (2026-07-13): se nn-rubicon-alea-v2.nnue e' presente alla build-root,
# viene EMBEDDATA nel binario via incbin (un file omonimo accanto all'exe la
# sovrascrive; EvalFile carica qualsiasi path). Senza file -> NNUE_EMBEDDING_OFF
# come prima (es. bundle sorgenti-only per le VM).
ifneq (,$(wildcard nn-rubicon-alea-v2.nnue))
  EMBED_DEF :=
  $(info [i] nn-rubicon-alea-v2.nnue trovata: rete EMBEDDATA nel binario)
else
  EMBED_DEF := -DNNUE_EMBEDDING_OFF
  $(info [i] nessuna nn-rubicon-alea-v2.nnue alla build-root: embedding OFF, serve il file accanto al binario)
endif
DEFS := -DNDEBUG $(EMBED_DEF) -DTRIUMV5_THREATS_INCR -DUSE_PEXT -DUSE_POPCNT \
        -DUSE_SSE -DUSE_SSE2 -DUSE_SSSE3 -DUSE_SSE41 -DUSE_AVX2

ARCH := -mavx2 -mbmi2 -mfma -mpopcnt -msse4.1 -mssse3 -msse2

# `make avx512` aggiunge AVX512 + VNNI + ICL (VBMI/VBMI2/BITALG) = identico alla
# build Windows shipping (SFNNv13 forward + dirty-threats vettoriali). OK su c3
# (Sapphire Rapids) e c3d (Zen4): entrambi hanno VBMI/VBMI2.
ifeq (avx512,$(filter avx512,$(MAKECMDGOALS)))
  DEFS += -DUSE_AVX512 -DUSE_VNNI -DUSE_AVX512ICL
  ARCH += -mavx512f -mavx512bw -mavx512dq -mavx512vl -mavx512vnni \
          -mavx512vbmi -mavx512vbmi2 -mavx512bitalg
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
CXXFLAGS ?= -std=c++17 $(OPT) -w $(DEFS) $(ARCH) -pthread -I. -Ifathom
CFLAGS   ?= -O3 -w $(ARCH) -pthread -I. -Ifathom

all: $(EXE)
avx512: $(EXE)
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

.PHONY: all avx512 profile release android android-dotprod clean

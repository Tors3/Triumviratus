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

# Vendored Stockfish-master SFNNv13 NNUE (sfnnue_v13/). We compile ONLY the subset
# the NNUE eval needs (mirror of the Windows vcxproj): position_min.cpp provides
# Position::set_pieces + update_piece_threats; the heavy master position.cpp /
# nnue_misc.cpp / evaluate.cpp / m1_evalmatch.cpp / tune/score/ucioption are NOT
# compiled (they pull movegen/uci closures not needed for eval). Explicit list (no
# wildcard) so those excluded files never sneak in.
V13 := sfnnue_v13
CPP_SRCS := $(filter-out nnt.cpp,$(wildcard *.cpp)) \
            $(V13)/position_min.cpp $(V13)/bitboard.cpp $(V13)/attacks.cpp \
            $(V13)/misc.cpp $(V13)/memory.cpp \
            $(V13)/nnue/network.cpp $(V13)/nnue/nnue_accumulator.cpp \
            $(V13)/nnue/features/full_threats.cpp $(V13)/nnue/features/half_ka_v2_hm.cpp
C_SRCS   := fathom/tbprobe.c

OBJS := $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

# Define replicati dalla build MSVC Release (+ NNUE_EMBEDDING_OFF e USE_POPCNT,
# che su GCC danno il path __builtin_popcountll = hardware, risultato identico).
# TRIUMV5_THREATS_INCR = accumulatore-threats incrementale (default, come la build
# Windows): serve sia al FeatureTransformer sia al bridge (eager mirror).
DEFS := -DNDEBUG -DNNUE_EMBEDDING_OFF -DTRIUMV5_THREATS_INCR -DUSE_PEXT -DUSE_POPCNT \
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

OPT      := -O3 -funroll-loops
CXXFLAGS ?= -std=c++17 $(OPT) -w $(DEFS) $(ARCH) -pthread -I. -Ifathom
CFLAGS   ?= -O3 -w $(ARCH) -pthread -I. -Ifathom

all: $(EXE)
avx512: $(EXE)

$(EXE): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ -lpthread -lm
	@echo "[+] Fatto: ./$(EXE)  (metti le .nnue accanto al binario)"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(EXE)

.PHONY: all avx512 clean

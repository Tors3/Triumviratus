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

CPP_SRCS := $(wildcard *.cpp) \
            $(wildcard sfnnue/*.cpp) \
            $(wildcard sfnnue/nnue/*.cpp) \
            $(wildcard sfnnue/nnue/features/*.cpp)
C_SRCS   := fathom/tbprobe.c

OBJS := $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

# Define replicati dalla build MSVC Release (+ NNUE_EMBEDDING_OFF e USE_POPCNT,
# che su GCC danno il path __builtin_popcountll = hardware, risultato identico).
DEFS := -DNDEBUG -DNNUE_EMBEDDING_OFF -DUSE_PEXT -DUSE_POPCNT \
        -DUSE_SSE -DUSE_SSE2 -DUSE_SSSE3 -DUSE_SSE41 -DUSE_AVX2

ARCH := -mavx2 -mbmi2 -mfma -mpopcnt -msse4.1 -mssse3 -msse2

# `make avx512` aggiunge AVX512 + VNNI
ifeq (avx512,$(filter avx512,$(MAKECMDGOALS)))
  DEFS += -DUSE_AVX512 -DUSE_VNNI
  ARCH += -mavx512f -mavx512bw -mavx512dq -mavx512vl -mavx512vnni
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

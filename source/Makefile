# =============================================================================
#  Makefile — build LINUX dell'engine Triumviratus (g++/clang++).
#  La build Windows resta su MSVC (.sln/.vcxproj); questo e' SOLO per Linux
#  (es. generazione self-play su GCP). Le differenze OS sono #ifdef-guardate.
#
#  USO:
#    make              -> build AVX2 + BMI2/PEXT (Intel, AMD Zen3+)
#    make clean nopext -> build AVX2 SENZA pext  (AMD Zen1/Zen2: OBBLIGATORIA, vedi sotto)
#    make clean avx512 -> build AVX512 F/BW/DQ/VL, SENZA VNNI (Skylake-X, Xeon W-21xx)
#    make clean vnni512-> build AVX512 + VNNI (Cascade Lake, Ice Lake SP, Zen4) = piu' veloce
#    make clean avxvnni-> build AVX2 + AVX-VNNI (Alder Lake, Raptor Lake, Arrow Lake, Meteor Lake: Intel senza AVX-512)
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
# Compiler (03/11/2026): the newest clang++ of version 19 or later when one is installed (the engine needs 19:
# static_assert), g++ otherwise; CXX=... / CC=... on the command line or in the environment choose another. The Windows
# releases are built with clang 23 (source/universal/ubuild_lib.ps1); GCC 13 and 16 with PGO gave this engine 14-22%
# more cycles per node than clang with PGO (Windows, 02/11/2026).
CLANGXX := $(shell for c in clang++-25 clang++-24 clang++-23 clang++-22 clang++-21 clang++-20 clang++-19 clang++; do \
             command -v $$c >/dev/null 2>&1 && [ "$$($$c -dumpversion 2>/dev/null | cut -d. -f1)" -ge 19 ] 2>/dev/null \
             && { echo $$c; break; }; done 2>/dev/null)
ifeq ($(origin CXX),default)
  CXX := $(if $(CLANGXX),$(CLANGXX),g++)
endif
ifeq ($(origin CC),default)
  CC := $(if $(findstring clang++,$(CXX)),$(subst clang++,clang,$(CXX)),gcc)
endif
EXE ?= triumviratus

# Sources: every .cpp of the root (engine, sliders.cpp, large_pages.cpp, consilium_bridge.cpp) plus the network module
# consilium/cn.cpp. The network reads the engine's board, and the make writes the per-move changes
# (search/06_nndirty.inc). Block 6 of 8.1 (18/10/2026) replaced the previous inference, block 7 the last files of
# nnue/ (sliding attacks, large pages and their helpers): that folder no longer exists.
CPP_SRCS := $(filter-out nnt.cpp,$(wildcard *.cpp)) \
            consilium/cn.cpp
C_SRCS   := fathom/tbprobe.c

OBJS := $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

# Define replicati dalla build MSVC Release (USE_POPCNT su GCC = __builtin_popcountll
# = hardware, risultato identico). TRIUMV5_THREATS_INCR = accumulatore-threats
# incrementale (default, come la build Windows).
# EMBEDDING (2026-07-13): se il net di default (CN_DEFAULT_NET_FILE, in
# consilium_bridge.cpp) e' presente alla build-root, viene EMBEDDATA nel binario via
# incbin/ (un file omonimo accanto all'exe la sovrascrive; EvalFile carica
# qualsiasi path). Senza file -> NNUE_EMBEDDING_OFF come prima (es. bundle
# sorgenti-only per le VM). NB: il nome deve restare allineato a CN_DEFAULT_NET_FILE.
# 17/10/2026 (review by Bastiii): the 8.0 network, aligned with CN_DEFAULT_NET_FILE (it still named the 7.0 network,
# so Linux builds missed the file at the build root and turned embedding off).
# Consilium Plebis is the only network of 8.1 (26/10/2026: the Consilium engine was removed from this source; the last
# source with both is _backup/Triumviratus_8.1_pre_rimozione_c1_2026-10-26). The default network is nn-consilium-plebis.nnue (nn-consilium2.nnue until 28/10/2026),
# as CN_DEFAULT_NET_FILE. -DTRIUMV_C2 is still passed for scripts that test for it; the code no longer reads it.
# 9.0 (09/11/2026): the inference reads TRANN5 files only (Lex Hortensia); the default network is nn-lex-hortensia.nnue.
NET_NAME ?= nn-lex-hortensia.nnue
C2_DEF := -DTRIUMV_C2
ifneq (,$(wildcard $(NET_NAME)))
  EMBED_DEF :=
  $(info [i] $(NET_NAME) trovata: rete EMBEDDATA nel binario)
else
  EMBED_DEF := -DNNUE_EMBEDDING_OFF
  $(info [i] nessuna $(NET_NAME) alla build-root: embedding OFF, serve il file accanto al binario)
endif
DEFS := -DNDEBUG $(EMBED_DEF) $(C2_DEF) -DTRIUMV5_THREATS_INCR -DUSE_PEXT -DUSE_POPCNT \
        -DUSE_SSE -DUSE_SSE2 -DUSE_SSSE3 -DUSE_SSE41 -DUSE_AVX2

ARCH := -mavx2 -mbmi2 -mfma -mpopcnt -msse4.1 -mssse3 -msse2

# 🔴 6/08/2026 — `avx512` NON definisce piu' USE_AVX512ICL. Prima lo faceva, con
# -mavx512vbmi/vbmi2/bitalg, perche' era stato tarato sulle VM di GCP (c3 Sapphire
# Rapids, c3d Zen4: entrambe hanno VBMI2). Ma quel binario e' quello che si SPEDISCE
# come "avx512", e su **Skylake-X e Cascade Lake** — AVX512F/BW/DQ/VL/VNNI, senza
# VBMI/VBMI2/BITALG — muore di istruzione illegale all'avvio. Sono macchine vere e
# diffuse: verificato oggi su uno Xeon Gold 6242 (Cascade Lake), dove il target
# vecchio non sarebbe partito.
# Lo script Windows era gia' stato separato il 3/08; il Makefile era rimasto indietro,
# quindi il canale di build Linux produceva ancora il binario rotto.
# 🔴 10/08/2026 — VNNI FUORI da `avx512`. Il difetto del 6/08 e' sopravvissuto UN
# LIVELLO PIU' IN BASSO: si tolse VBMI2 e si diede per scontato che Skylake-X avesse
# VNNI. Non ce l'ha — VNNI arriva con Cascade Lake. Il commento qui sopra diceva
# "Skylake-X e Cascade Lake — AVX512F/BW/DQ/VL/VNNI" ed era sbagliato sul primo dei due.
# 🔑 PERCHE' NON EMERSE: la verifica del 6/08 fu fatta su uno Xeon Gold 6242, che e'
# Cascade Lake e il VNNI ce l'ha. Il caso che rompe non poteva uscire da li'. Una
# macchina di verifica che possiede la feature non puo' falsificare l'assunzione che
# quella feature ci sia.
# VERIFICATO il 10/08/2026 su i9-7940X (Skylake-X, 14 core): il binario `avx512`
# conteneva 44 `vpdpbusd` e moriva con SIGILL (exit 132) alla prima valutazione,
# subito dopo `readyok` — cioe' il tester vede il motore rispondere a `uci` e poi
# sparire alla prima mossa.
# ✅ Tre gradini (x86-64-avx512 / vnni512 / avx512icl):
#   make avx512      F/BW/DQ/VL              -> Skylake-X, Xeon W-21xx
#   make vnni512     + VNNI                  -> Cascade Lake, Ice Lake SP, Zen4
#   make avx512icl   + VBMI/VBMI2/BITALG     -> Ice Lake client+, Sapphire Rapids, Zen5
# ⚠️ L'ICL non serve: misurato +0,04% con 75/150 posizioni esatte, z~0, su Zen4.
#    Si costruisce ma non c'e' motivo di pubblicarlo.
ifneq (,$(filter avx512 vnni512 avx512icl,$(MAKECMDGOALS)))
  DEFS += -DUSE_AVX512
  # CD (block 7 of 8.1): vplzcntq of the slider kernel (sliders.h); every AVX-512 processor has it.
  ARCH += -mavx512f -mavx512cd -mavx512bw -mavx512dq -mavx512vl
endif
ifneq (,$(filter vnni512 avx512icl,$(MAKECMDGOALS)))
  DEFS += -DUSE_VNNI
  ARCH += -mavx512vnni
endif
# V1 (15/10/2026): AVX-VNNI, la forma VEX a 256 bit di vpdpbusd (Intel dal 12a generazione senza AVX-512). Il prodotto
# scalare u8 x i8 della prima rete sparsa e degli strati densi diventa una istruzione invece di tre (maddubs, madd,
# add); identico perche' gli ingressi valgono al piu' 127. Misurato su i9-14900KF, g++, nodi identici (bench 137419):
# +4,55% NPS (tre giri nps_pair: +4,71, +4,55, +4,40; nullo +0,02). CPUID: foglia 7, sottofoglia 1, EAX bit 4.
ifeq (avxvnni,$(filter avxvnni,$(MAKECMDGOALS)))
  DEFS += -DUSE_VNNI -DUSE_AVXVNNI
  ARCH += -mavxvnni
endif
ifeq (avx512icl,$(filter avx512icl,$(MAKECMDGOALS)))
  DEFS += -DUSE_AVX512ICL
  ARCH += -mavx512vbmi -mavx512vbmi2 -mavx512bitalg
endif

# `make nopext` (equivalente: NOPEXT=1, che propaga anche ai target PGO ricorsivi):
# toglie USE_PEXT/-mbmi2 -> le tabelle di magic.cpp usano i fancy magics; il ramo AVX2 (sliders.h) non usa
# tabelle e non cambia. Stessi attacchi = node-identical: la firma
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
# ✅ 25/09/2026 — decisione dell'utente: persp ACCESA anche qui, per universalita'. Il -1,2%
# su Rocket Lake era una misura a tempo che non abbiamo piu' motivo di considerare
# affidabile (su Skylake-SP, a coppie simultanee, persp vale +0,80%). Il target `intel`
# resta come nome, ma oggi produce lo stesso codice del target senza `intel`.
ifneq (,$(filter intel,$(MAKECMDGOALS))$(INTEL))
  $(info [i] build Intel: persp ACCESA (dal 25/09/2026 come su tutti i target))
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
# ⚠️ 10/08/2026: il filtro prendeva SOLO il goal letterale `avx512`, quindi `avx512icl`
# usciva con persp ACCESA nonostante la misura dica il contrario per entrambi i vendor.
# Lo script Windows usa `-match '^avx512'` e le prendeva tutte: e' la stessa deriva
# Windows/Linux gia' vista sopra. Ora il filtro elenca tutti e tre i target AVX-512.
# ✅ 25/09/2026 — persp, ibrido e pawn cache ACCESI su TUTTI i target AVX-512 (decisione
# dell'utente: le misure Intel valgono per tutti i target). Misura a coppie simultanee su
# Xeon Gold 6138 (tools/nps_pair.py, test nullo -0,03%): persp +0,80% IC95 [+0,50, +1,10].
# Le misure a tempo di agosto su Zen4 che li avevano spenti non avevano la risoluzione
# necessaria. Per spegnerli: -DTRIUMV_NO_PERSP_BOTH, -DTRIUMV_NO_HYBRID_AVX512,
# -DTRIUMV_NO_PAWN_CACHE_AVX512.

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
# costanti precalcolate in init.cpp); i percorsi NEON del modulo della rete (USE_NEON / USE_NEON_DOTPROD) sono
# gia' presenti.
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
  DEFS := -DNDEBUG $(EMBED_DEF) $(C2_DEF) -DTRIUMV5_THREATS_INCR -DUSE_POPCNT -DUSE_NEON=8
  ARCH := --target=aarch64-linux-android24
  # bionic: pthread dentro libc (niente -lpthread); 16KB page-size per Android 15+.
  LIBS := -lm -static-libstdc++ -Wl,-z,max-page-size=16384
  EXE  := triumviratus_android
endif
ifneq (,$(filter android-dotprod,$(MAKECMDGOALS)))
  DEFS += -DUSE_NEON_DOTPROD
  ARCH += -march=armv8.2-a+dotprod
endif

# ---- sse41 (15/10/2026): x86-64 senza AVX2 (CPU prima di Haswell, 2013): SSE4.1 + POPCNT, niente PEXT ------
ifneq (,$(filter sse41,$(MAKECMDGOALS)))
  DEFS := $(filter-out -DUSE_AVX2 -DUSE_PEXT,$(DEFS))
  ARCH := -msse4.1 -mssse3 -msse2 -mpopcnt
  EXE  := triumviratus_sse41
endif

# ---- macOS (15/10/2026, issue #4) ------------------------------------------
#   make macos-arm CXX=clang++   Apple Silicon (M1 e successivi): NEON + dot product,
#                                                           gli stessi percorsi della build android-dotprod
#   make macos-x86 CXX=clang++   Mac Intel con AVX2 (Haswell, 2013, e successivi)
# Compilati e verificati col bench dal workflow .github/workflows/macos.yml (macchine macOS di GitHub).
ifneq (,$(filter macos-arm,$(MAKECMDGOALS)))
  DEFS := -DNDEBUG $(EMBED_DEF) $(C2_DEF) -DTRIUMV5_THREATS_INCR -DUSE_POPCNT -DUSE_NEON=8 -DUSE_NEON_DOTPROD
  ARCH := -mcpu=apple-m1
  EXE  := triumviratus_macos_arm
endif
ifneq (,$(filter macos-x86,$(MAKECMDGOALS)))
  EXE  := triumviratus_macos_x86
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
# CODEGEN=1 (17/10/2026, review by Bastiii): the two code-generation flags of the Windows build
# (Triumviratus_8.0.vcxproj): no cmov-to-branch conversion (-0.15% / -0.29% cycles) and block placement from the PGO
# profile (ext-tsp, -0.6% middlegame cycles). With ThinLTO the code is generated at link time, so they go to the linker
# through lld. Opt-in until tried on a Linux machine; same moves either way.
ifeq ($(CODEGEN),1)
  OPT += -fuse-ld=lld -Wl,-mllvm,-x86-cmov-converter=false -Wl,-mllvm,-enable-ext-tsp-block-placement=true
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
vnni512: $(EXE)
avxvnni: $(EXE)
avx512icl: $(EXE)
nopext: $(EXE)
intel: $(EXE)
profile: $(EXE)
release: $(EXE)
android: $(EXE)
android-dotprod: $(EXE)
macos-arm: $(EXE)
macos-x86: $(EXE)
sse41: $(EXE)

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
# PGOGOAL: quale target ISA costruire dentro la pipeline PGO. Serve da quando i
# gradini AVX-512 sono TRE (avx512 / vnni512 / avx512icl): prima ce n'erano due e
# `avx512` bastava. Su una macchina con VNNI, lasciare il default significherebbe
# tarare su un binario piu' lento di quello che quella CPU sa eseguire.
#   make pgo-avx512 PGOGOAL=vnni512 CXX=clang++
PGOGOAL ?= avx512
pgo-avx512:
ifeq (,$(IS_CLANG))
	$(error pgo-avx512 richiede CXX=clang++ (formato profdata LLVM))
endif
	@mkdir -p $(PROFDIR) && rm -f $(PROFDIR)/*.profraw
	@test -f $(BOOK) || echo "[!] libro $(BOOK) assente: pgo_train.py usera' i FEN builtin"
	$(MAKE) clean
	$(MAKE) $(PGOGOAL) $(RELGOAL) CXX=$(CXX) PGO_PHASE=gen
	LLVM_PROFILE_FILE="$(PROFDIR)/prof_%p.profraw" \
	  python3 $(TRAINER) ./$(EXE) 0 $(BOOK) $(PGO_POS) --workers $(PGO_WORKERS)
	$(LLVM_PROFDATA) merge -output=$(PROFDIR)/merged.profdata $(PROFDIR)/*.profraw
	$(MAKE) clean
	$(MAKE) $(PGOGOAL) $(RELGOAL) CXX=$(CXX) PGO_PHASE=use
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

.PHONY: all avx512 vnni512 avxvnni avx512icl nopext intel profile release android android-dotprod clean pgo-avx512 pgo-avx2

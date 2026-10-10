// Binario universale (08/10/2026): UNA unita' di compilazione per variante ISA. Tutto il motore (i .cpp del progetto
// tranne fathom, che e' C e indipendente dall'ISA) sta nel namespace della variante, scelto da -DTRIUMV_VID=<n>:
//   1 avx2-nopext   2 avx2   3 avx512   4 vnni512   5 avx512icl
// Il main del motore diventa Triumv_v<n>::main, chiamato da entry.cpp dopo cpuid.
// I costruttori globali della variante vanno nella sezione .tv<n>$m e non in .CRT$XCU: il CRT non li esegue, li
// esegue entry.cpp solo per la variante scelta. Compilati con le istruzioni della variante (AVX-512 ...), su una CPU
// che non le ha non devono partire.
#include "prelude.h"

#if TRIUMV_VID == 1
#pragma init_seg(".tv1$m")
#define TRIUMV_VNS Triumv_v1
#elif TRIUMV_VID == 2
#pragma init_seg(".tv2$m")
#define TRIUMV_VNS Triumv_v2
#elif TRIUMV_VID == 3
#pragma init_seg(".tv3$m")
#define TRIUMV_VNS Triumv_v3
#elif TRIUMV_VID == 4
#pragma init_seg(".tv4$m")
#define TRIUMV_VNS Triumv_v4
#elif TRIUMV_VID == 5
#pragma init_seg(".tv5$m")
#define TRIUMV_VNS Triumv_v5
#else
#error "TRIUMV_VID mancante (1..5)"
#endif

namespace TRIUMV_VNS {
#include "../attacks.cpp"
#include "../init.cpp"
#include "../io.cpp"
#include "../magic.cpp"
#include "../main.cpp"
#include "../misc.cpp"
#include "../movegen.cpp"
#include "../perft.cpp"
#include "../presentation.cpp"
#include "../random.cpp"
#include "../see.cpp"
#include "../threads.cpp"
#include "../uci_mt.cpp"
#include "../zobrist.cpp"
#include "../chess960.cpp"
#include "../nnue_bridge.cpp"
#include "../nnue/bitboard.cpp"
#include "../nnue/attacks.cpp"
#include "../nnue/misc.cpp"
#include "../nnue/memory.cpp"
#include "../nnue/nnue/network.cpp"
#include "../nnue/nnue/nnue_accumulator.cpp"
#include "../nnue/nnue/features/feat_perm.cpp"
#include "../nnue/nnue/features/full_threats.cpp"
#include "../nnue/nnue/features/pawn_pair.cpp"
#include "../nnue/nnue/features/passed_pawns.cpp"
#include "../nnue/nnue/features/half_ka_v2_hm.cpp"
#include "../syzygy.cpp"
}  // namespace TRIUMV_VNS

#ifdef TRIUMV_STANDALONE
// Build della sola variante (training PGO e prove): il main vero chiama quello della variante. Qui i costruttori
// globali stanno in .tv<n>$m e il CRT non li esegue: li esegue questo main, come fa entry.cpp.
typedef void(__cdecl *TvInit)(void);
#if TRIUMV_VID == 1
#pragma section(".tv1$a", read)
#pragma section(".tv1$z", read)
__declspec(allocate(".tv1$a")) const TvInit tv_a = nullptr;
__declspec(allocate(".tv1$z")) const TvInit tv_z = nullptr;
#elif TRIUMV_VID == 2
#pragma section(".tv2$a", read)
#pragma section(".tv2$z", read)
__declspec(allocate(".tv2$a")) const TvInit tv_a = nullptr;
__declspec(allocate(".tv2$z")) const TvInit tv_z = nullptr;
#elif TRIUMV_VID == 3
#pragma section(".tv3$a", read)
#pragma section(".tv3$z", read)
__declspec(allocate(".tv3$a")) const TvInit tv_a = nullptr;
__declspec(allocate(".tv3$z")) const TvInit tv_z = nullptr;
#elif TRIUMV_VID == 4
#pragma section(".tv4$a", read)
#pragma section(".tv4$z", read)
__declspec(allocate(".tv4$a")) const TvInit tv_a = nullptr;
__declspec(allocate(".tv4$z")) const TvInit tv_z = nullptr;
#else
#pragma section(".tv5$a", read)
#pragma section(".tv5$z", read)
__declspec(allocate(".tv5$a")) const TvInit tv_a = nullptr;
__declspec(allocate(".tv5$z")) const TvInit tv_z = nullptr;
#endif
extern "C" const char *g_triumv_isa = "standalone";
int main() {
  for (const TvInit *p = &tv_a + 1; p < &tv_z; ++p)
    if (*p)
      (*p)();
  return TRIUMV_VNS::main();
}
#endif

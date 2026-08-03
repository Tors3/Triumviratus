/*
  Permutazione per LOCALITA' delle righe di `threatWeights`.

  IL FATTO (misurato 3/08/2026, istogramma per riga sul bench):
    righe totali 64.464, TOCCATE solo 18.272 (28,3%)
      top  1% delle righe (   644) = 60,0% degli accessi
      top  5% delle righe ( 3.223) = 89,5%
    il 90% degli accessi sta in 3.358 righe = 3,3 MB SE CONTIGUE
    oggi sono sparse su 63 MB = 16.116 pagine da 4 KB

  3,3 MB stanno in L3. Impacchettare le righe calde trasforma il 90% degli accessi da
  miss DRAM + miss TLB a hit di cache. E' il motivo per cui un update incrementale costa
  1423 cicli contro i 337 del solo lavoro aritmetico: non e' l'aritmetica, e' che si va
  in DRAM per dati che starebbero in cache se fossero vicini.

  ⚠️ Le large pages sarebbero il rimedio hardware, ma richiedono un privilegio Windows che
  nessun tester abilita. La permutazione e' il sostituto software, ed e' dentro il binario:
  ce l'hanno tutti.

  NON CAMBIA LA VALUTAZIONE. Si permutano le righe dei pesi a caricamento rete e si
  rimappano gli indici allo stesso modo: il bench DEVE restare 207259.

  🔴 LA TRAPPOLA. Per una feature ESCLUSA `FullThreats::make_index` non ritorna una
  costante: ritorna `DeadBase + offsets[att][from] + lut2[att][from][to]`, cioe' un valore
  >= FeatRows ma NON uguale a FeatRows. Se la tabella coprisse solo FeatRows voci, quella
  lettura andrebbe FUORI dall'array. Per questo `FeatPermSize` e' generosa e tutta la coda
  vale FeatRows: cosi' il filtro resta UNA lettura senza branch e le feature morte cadono
  da sole in `push_back_if_lt(row, FeatRows)`.
*/

#ifndef NNUE_FEATURES_FEAT_PERM_INCLUDED
#define NNUE_FEATURES_FEAT_PERM_INCLUDED

#include "../nnue_common.h"

namespace Triumviratus::Eval::NNUE::Features {

// Righe della tabella threatWeights: FullThreats 59808 + PawnPair 4560 + PassedPawns 96.
// Verificato con static_assert contro le tre feature set in feat_perm.cpp.
inline constexpr IndexType FeatRows = 64464;

// Base usata da init_index_luts per le feature escluse (vedi la trappola qui sopra) e
// dimensione della tabella, che deve coprire DeadBase + max(offsets + lut2).
// max(offsets) e' il cumulativePieceOffset della donna (~1400), max(lut2) <= 27.
inline constexpr IndexType FeatDeadBase = FeatRows;
inline constexpr IndexType FeatPermSize = 66560;

#ifndef TRIUMV_NO_FEAT_PERM
extern const unsigned short FeatPerm[FeatPermSize];

// Una lettura, nessun branch. La tabella e' 130 KB e sta in L2.
inline IndexType feat_row(IndexType raw) { return FeatPerm[raw]; }
#else
// Baseline dell'A/B: identita', con le feature morte che collassano sul sentinella.
inline IndexType feat_row(IndexType raw) { return raw < FeatRows ? raw : FeatRows; }
#endif

}  // namespace Triumviratus::Eval::NNUE::Features

#endif  // NNUE_FEATURES_FEAT_PERM_INCLUDED

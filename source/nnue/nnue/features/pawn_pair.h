/*
  Pawn-pair input features (Triumviratus 6.0, rubicon-alea-v2 graft).

  Unordered pairs of pawns on the same or adjacent files. 96 pawn slots
  (48 own + 48 enemy; oriented per perspective and file-mirrored with the king
  exactly like FullThreats/HalfKA via OrientTBL) -> index hi*(hi-1)/2 + lo,
  96*95/2 = 4560 features. Spec identical to Stormphrax (the 7->8 jump),
  Viridithas and Pawnocchio; trained by nnue-pytorch block "PawnPair".
  Captures phalanx/chains/doubled/isolated pawns as LEARNED patterns —
  pawn-pawn geometry that threat features cannot see (side-by-side pawns
  attack nothing).

  GPLv3, derived from Stockfish NNUE plumbing (see COPYING).
*/

#ifndef NNUE_FEATURES_PAWN_PAIR_INCLUDED
#define NNUE_FEATURES_PAWN_PAIR_INCLUDED

#include "../../misc.h"
#include "../../types.h"
#include "../nnue_common.h"
#include "full_threats.h"  // OrientTBL (stessa orientazione per tutti i blocchi)

namespace Stockfish {
class Position;
}

namespace Stockfish::Eval::NNUE::Features {

class PawnPair {
   public:
    // Hash value embedded in the evaluation file — MUST match the trainer
    // (model/modules/features/pawn_pair.py: 0x50414952 = "PAIR")
    static constexpr u32 HashValue = 0x50414952u;

    // Number of feature dimensions
    static constexpr IndexType Dimensions = 4560;

    // TRANN1 folded design: gli indici emessi da append_* sono GIA' offsettati
    // di FullThreats::Dimensions, perche' i pesi pawn-pair vivono in coda
    // all'array threatWeights -> il SIMD dell'accumulatore resta invariato.
    static constexpr IndexType FoldOffset = FullThreats::Dimensions;

    // Maximum number of simultaneously active features: C(16,2) = all pawns
    // inside one file band (theoretical worst case).
    static constexpr IndexType MaxActiveDimensions = 120;
    // Folded design: gli indici pawn viaggiano nelle LISTE threat (capacita' 256,
    // dimensionata per threats+pawn insieme) -> stesso tipo di lista.
    using IndexList = FullThreats::IndexList;
    using DiffType  = DirtyPawns;

    // 0..47 own pawns, 48..95 enemy pawns (oriented squares 8..55 -> -8)
    static inline IndexType pawn_id(Color perspective, i8 orientation, Color pc, Square sq) {
        return (pc != perspective ? 48 : 0) + (u8(sq) ^ orientation) - 8;
    }

    static inline IndexType
    make_index(Color perspective, Square ksq, Square s1, Color c1, Square s2, Color c2) {
        const i8        orientation = FullThreats::OrientTBL[ksq] ^ (56 * perspective);
        const IndexType a           = pawn_id(perspective, orientation, c1, s1);
        const IndexType b           = pawn_id(perspective, orientation, c2, s2);
        const IndexType hi          = a > b ? a : b;
        const IndexType lo          = a > b ? b : a;
        return hi * (hi - 1) / 2 + lo;
    }

    // Get a list of indices for active features (full refresh)
    static void append_active_indices(Color perspective, const Position& pos, IndexList& active);

    // Get a list of indices for recently changed features (incremental)
    static void append_changed_indices(Color           perspective,
                                       Square          ksq,
                                       const DiffType& diff,
                                       IndexList&      removed,
                                       IndexList&      added);
};

}  // namespace Stockfish::Eval::NNUE::Features

#endif  // #ifndef NNUE_FEATURES_PAWN_PAIR_INCLUDED

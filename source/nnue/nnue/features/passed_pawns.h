/*
  Passed-pawn input features (Triumviratus, rubicon-alea-v3 graft).

  One feature per passed pawn: 96 slots (48 own + 48 enemy; squares oriented
  per perspective and file-mirrored with the king exactly like PawnPair/
  FullThreats/HalfKA via OrientTBL). "Passed" = no enemy pawn on the same or
  adjacent files ahead of the pawn AND no own pawn directly ahead on the same
  file (a rear doubled pawn is not a passer).

  The passed property is the relational signal HalfKA cannot represent
  directly (it requires combining the enemy-pawn configuration over three
  files); interactions with blockers / kings / own chains are learnable
  downstream through the SFNNv13 pairwise-multiplied L1 against the HalfKA
  and PawnPair blocks, so no explicit flags are encoded (v1 deliberately
  square-only). Trained by the nnue-pytorch block "PassedPawns".

  GPLv3, derived from Stockfish NNUE plumbing (see COPYING).
*/

#ifndef NNUE_FEATURES_PASSED_PAWNS_INCLUDED
#define NNUE_FEATURES_PASSED_PAWNS_INCLUDED

#include "../../misc.h"
#include "../../types.h"
#include "../nnue_common.h"
#include "full_threats.h"  // OrientTBL (stessa orientazione per tutti i blocchi)
#include "pawn_pair.h"     // catena FoldOffset: i pesi passed vivono DOPO il segmento pawn-pair

namespace Triumviratus {
class Position;
}

namespace Triumviratus::Eval::NNUE::Features {

class PassedPawns {
   public:
    // Hash value embedded in the evaluation file — MUST match the trainer
    // (model/modules/features/passed_pawns.py: 0x50535344 = "PSSD")
    static constexpr u32 HashValue = 0x50535344u;

    // Number of feature dimensions: 48 oriented squares x {own, enemy}
    static constexpr IndexType Dimensions = 96;

    // TRANN1 folded design: gli indici emessi sono GIA' offsettati, i pesi
    // vivono in coda all'array threatWeights dopo il segmento PawnPair ->
    // il SIMD dell'accumulatore resta invariato.
    static constexpr IndexType FoldOffset = FullThreats::Dimensions + PawnPair::Dimensions;

    // Maximum number of simultaneously active features: all 16 pawns passed
    // (theoretical bound).
    static constexpr IndexType MaxActiveDimensions = 16;
    using IndexList = FullThreats::IndexList;
    using DiffType  = DirtyPawns;  // stessi trigger del PawnPair: SOLO eventi-pedone
                                   // (lo status passed dipende solo dai pedoni)

    // 0..47 own passer, 48..95 enemy passer (oriented squares 8..55 -> -8),
    // identico al pawn_id del PawnPair.
    static inline IndexType make_index(Color perspective, Square ksq, Color pc, Square sq) {
        const i8 orientation = FullThreats::OrientTBL[ksq] ^ (56 * perspective);
        return (pc != perspective ? 48 : 0) + (u8(sq) ^ orientation) - 8;
    }

    // Bitboard of passed pawns of color c given both RAW pawn sets (the
    // orientation enters only at index time, like PawnPair's band check).
    static Bitboard passers(Color c, Bitboard ownPawns, Bitboard oppPawns);

    // Get a list of indices for active features (full refresh)
    static void append_active_indices(Color perspective, const Position& pos, IndexList& active);

    // Get a list of indices for recently changed features (incremental)
    static void append_changed_indices(Color           perspective,
                                       Square          ksq,
                                       const DiffType& diff,
                                       IndexList&      removed,
                                       IndexList&      added);
};

}  // namespace Triumviratus::Eval::NNUE::Features

#endif  // #ifndef NNUE_FEATURES_PASSED_PAWNS_INCLUDED

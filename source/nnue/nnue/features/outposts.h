/*
  Outpost input features (Triumviratus, rubicon-alea-v4 graft).

  One feature per outpost SQUARE: 96 slots (48 own + 48 enemy; squares
  oriented per perspective and file-mirrored with the king exactly like
  PawnPair/PassedPawns/FullThreats/HalfKA via OrientTBL). Square sq is an
  outpost for color C iff:
    1. sq is DEFENDED by a pawn of C (pawn of C on an adjacent file, one
       rank behind sq in C's marching direction), AND
    2. NO enemy pawn can ever attack it: no pawn of ~C on an adjacent file
       with rank strictly ahead of sq from C's viewpoint.

  Deliberately square-only (no occupying-piece / square-color flags), like
  PassedPawns: interactions with the piece sitting on the outpost, kings and
  chains are learnable downstream through the SFNNv13 pairwise-multiplied L1
  against the HalfKA and PawnPair blocks. Trained by the nnue-pytorch block
  "Outposts".

  NOTE vs PassedPawns index math: pawns live on ranks 2-7 (oriented squares
  8..55, single "-8"), but OUTPOST squares live on oriented ranks 3-8 for the
  own side (o 16..63) and ranks 1-6 for the enemy side (o 0..47) — the
  defending pawn is one rank behind. Hence own = o-16, enemy = 48+o (both
  bands are still exactly 48 squares).

  GPLv3, derived from Stockfish NNUE plumbing (see COPYING).
*/

#ifndef NNUE_FEATURES_OUTPOSTS_INCLUDED
#define NNUE_FEATURES_OUTPOSTS_INCLUDED

#include "../../misc.h"
#include "../../types.h"
#include "../nnue_common.h"
#include "full_threats.h"   // OrientTBL (stessa orientazione per tutti i blocchi)
#include "passed_pawns.h"   // catena FoldOffset: i pesi outpost vivono DOPO il segmento passed

namespace Triumviratus {
class Position;
}

namespace Triumviratus::Eval::NNUE::Features {

class Outposts {
   public:
    // Hash value embedded in the evaluation file — MUST match the trainer
    // (model/modules/features/outposts.py: 0x4F545053 = "OTPS")
    static constexpr u32 HashValue = 0x4F545053u;

    // Number of feature dimensions: 48 oriented squares x {own, enemy}
    static constexpr IndexType Dimensions = 96;

    // TRANN1 folded design: gli indici emessi sono GIA' offsettati, i pesi
    // vivono in coda all'array threatWeights dopo il segmento PassedPawns ->
    // il SIMD dell'accumulatore resta invariato.
    static constexpr IndexType FoldOffset =
      FullThreats::Dimensions + PawnPair::Dimensions + PassedPawns::Dimensions;

    // Maximum number of simultaneously active features: each side's 8 pawns
    // defend at most 16 distinct squares -> 16 per color (theoretical bound).
    static constexpr IndexType MaxActiveDimensions = 32;
    using IndexList = FullThreats::IndexList;
    using DiffType  = DirtyPawns;  // stessi trigger del PawnPair/PassedPawns: SOLO
                                   // eventi-pedone (lo status outpost dipende solo dai pedoni)

    // 0..47 own outpost (oriented squares 16..63 -> -16), 48..95 enemy
    // outpost (oriented squares 0..47 -> +48). Bande diverse dal pawn_id di
    // PawnPair/PassedPawns: vedi nota in testa al file.
    static inline IndexType make_index(Color perspective, Square ksq, Color pc, Square sq) {
        const i8 orientation = FullThreats::OrientTBL[ksq] ^ (56 * perspective);
        const u8 o           = u8(sq) ^ orientation;
        return pc != perspective ? IndexType(48 + o) : IndexType(o - 16);
    }

    // Bitboard of outpost squares for color c given both RAW pawn sets (the
    // orientation enters only at index time, like PassedPawns::passers).
    static Bitboard outposts(Color c, Bitboard ownPawns, Bitboard oppPawns);

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

#endif  // #ifndef NNUE_FEATURES_OUTPOSTS_INCLUDED

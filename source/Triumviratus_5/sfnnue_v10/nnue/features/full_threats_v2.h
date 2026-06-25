/*
  Full_Threats V2 (Stockfish master nnue-pytorch commit 89d5725, giu 2026).
  60.720 feature (vs 79.856 di SF18 stable). Logica di make_index lifted dal
  data_loader del master (training_data_loader.cpp righe ~85-220).

  Differenze chiave rispetto a SF18 FullThreats:
   - Numvalidtargets per-piece (P=6,N=10,B=8,R=8,Q=10,K=0). Il KING e' rimosso
     come attacker e i tipi attacked sono ridotti (vedi map[][]).
   - PAWN-PUSH-FORWARD (pedone che spinge contro pezzo bloccato) e' ora una
     feature: nelle attacks del pawn si aggiunge il push square.
   - Esclusione same-type+from<to (deduplica le coppie speculari).

  Quest'API replica esattamente quella di FullThreats (append_active_indices,
  append_changed_indices, requires_refresh, make_index) cosi' il
  FeatureTransformer e' parallelizzabile con un solo #ifdef.
*/

#ifndef NNUE_FEATURES_FULL_THREATS_V2_INCLUDED
#define NNUE_FEATURES_FULL_THREATS_V2_INCLUDED

#include <cstdint>
#include "../../misc.h"
#include "../../types.h"
#include "../nnue_common.h"

namespace Stockfish {
class Position;
}

namespace Stockfish::Eval::NNUE::Features {

// Interleaved piece indexing the master uses: 0=wP 1=bP 2=wN 3=bN ...
// PIECE_NB=12 (NO_PIECE/NO_PIECE_TYPE excluded).
static constexpr int v2_numvalidtargets[12] = {6, 6, 10, 10, 8, 8, 8, 8, 10, 10, 0, 0};

class FullThreatsV2 {
   public:
    static constexpr const char* Name = "Full_Threats_V2(master)";

    // Hash value used in the embedded eval file. Master keeps the same name-hash
    // 0x8f234cb8 (it identifies the feature NAME, not the dimensions); the dim
    // is encoded in (HalfDimensions * 2) in get_hash_value.
    static constexpr std::uint32_t HashValue = 0x8f234cb8u;

    // Number of feature dimensions (master 89d5725).
    static constexpr IndexType Dimensions = 60720;

    // Orient a square according to perspective (mirror by file for half board).
    // Identical to V1 SF18 layout.
    static constexpr std::int8_t OrientTBL[SQUARE_NB] = {
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
    };

    // Master map[][]: row = attacker type-1 (P,N,B,R,Q,K), col = attacked type-1.
    // KING row + KING col differ from SF18 (-1 = excluded). Total of 6 valid
    // per piece (with color split = numvalidtargets[piece]/2).
    static constexpr int map[6][6] = {
        {0,  1, -1,  2, -1, -1},   // Pawn attacker
        {0,  1,  2,  3,  4, -1},   // Knight (no Knight->King)
        {0,  1,  2,  3, -1, -1},   // Bishop (no B->Q, B->K)
        {0,  1,  2,  3, -1, -1},   // Rook   (no R->Q, R->K)
        {0,  1,  2,  3,  4, -1},   // Queen  (no Q->K)
        {-1,-1, -1, -1, -1, -1},   // King: NOT an attacker (numvalidtargets=0)
    };

    struct FusedUpdateData {
        Bitboard dp2removedOriginBoard = 0;
        Bitboard dp2removedTargetBoard = 0;
        Square   dp2removed;
    };

    static constexpr IndexType MaxActiveDimensions = 128;
    using IndexList                                = ValueList<IndexType, MaxActiveDimensions>;
    using DiffType                                 = DirtyThreats;

    static IndexType
    make_index(Color perspective, Piece attkr, Square from, Square to, Piece attkd, Square ksq);

    static void append_active_indices(Color perspective, const Position& pos, IndexList& active);

    static void append_changed_indices(Color            perspective,
                                       Square           ksq,
                                       const DiffType&  diff,
                                       IndexList&       removed,
                                       IndexList&       added,
                                       FusedUpdateData* fd    = nullptr,
                                       bool             first = false);

    static bool requires_refresh(const DiffType& diff, Color perspective);
};

}  // namespace Stockfish::Eval::NNUE::Features

#endif  // NNUE_FEATURES_FULL_THREATS_V2_INCLUDED

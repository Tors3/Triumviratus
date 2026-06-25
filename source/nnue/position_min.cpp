// Triumviratus bridge glue for the vendored SFNNv13 NNUE:
//   (1) set_pieces() — minimal Position setup from a piece list (full refresh path).
//   (2) update_piece_threats<> — ported VERBATIM (scalar path) from the master
//       position.cpp so the engine can drive the Position INCREMENTALLY (M3): the
//       inline put_piece/remove_piece/move_piece/swap_piece call this with a
//       DirtyThreats* to record the FullThreats feature deltas per board change.
// We deliberately do NOT compile the master position.cpp (its movegen/tt/syzygy/uci
// closure is not needed); this file provides exactly the slice the NNUE needs.

#include <algorithm>
#include <cstring>

#include "position.h"
#include "types.h"

namespace Stockfish {

using namespace Attacks;  // ray_pass_bb / PseudoAttacks / PawnPushOrAttacks / attacks_bb

void Position::set_pieces(const Piece* pcs, const Square* sqs, int n, Color stm, StateInfo* si) {
    board.fill(NO_PIECE);
    byTypeBB.fill(0);
    byColorBB.fill(0);
    std::fill(std::begin(pieceCount), std::end(pieceCount), 0);

    std::memset(static_cast<void*>(si), 0, sizeof(StateInfo));
    si->epSquare = SQ_NONE;
    si->previous = nullptr;
    st           = si;

    sideToMove = stm;
    gamePly    = 0;
    chess960   = false;

    for (int i = 0; i < n; ++i)
        put_piece(pcs[i], sqs[i]);  // dts==nullptr => no threat dirties (full refresh)
}

namespace {
// Record one changed threat tuple (master position.cpp helper, verbatim).
inline void add_dirty_threat(DirtyThreats* const dts,
                             bool                putPiece,
                             Piece               pc,
                             Piece               threatened,
                             Square              s,
                             Square              threatenedSq) {
    dts->list.push_back({pc, threatened, s, threatenedSq, putPiece});
}
}  // namespace

// Ported VERBATIM from the master Position::update_piece_threats (non-ICL scalar
// path; we never define USE_AVX512ICL). Emits the FullThreats tuples that change
// when piece `pc` is placed on (putPiece=true) / removed from (putPiece=false)
// square `s`. ComputeRay handles discovered slider threats along a moved-piece ray;
// noRaysContaining skips a discovered ray fully contained in the move's from|to.
template<bool ComputeRay>
void Position::update_piece_threats(Piece               pc,
                                    bool                putPiece,
                                    Square              s,
                                    DirtyThreats* const dts,
                                    Bitboard            noRaysContaining) const {
    const Bitboard occupied     = pieces();
    const Bitboard rookQueens   = pieces(ROOK, QUEEN);
    const Bitboard bishopQueens = pieces(BISHOP, QUEEN);
    const Bitboard rAttacks     = attacks_bb<ROOK>(s, occupied);
    const Bitboard bAttacks     = attacks_bb<BISHOP>(s, occupied);
    const Bitboard kings        = pieces(KING);
    Bitboard       occupiedNoK  = occupied ^ kings;

    Bitboard sliders         = (rookQueens & rAttacks) | (bishopQueens & bAttacks);
    auto     process_sliders = [&](bool addDirectAttacks) {
        while (sliders)
        {
            Square sliderSq = pop_lsb(sliders);
            Piece  slider   = piece_on(sliderSq);

            const Bitboard ray        = ray_pass_bb(sliderSq, s);
            const Bitboard discovered = ray & (rAttacks | bAttacks) & occupiedNoK;

            if (discovered && (ray & noRaysContaining) != noRaysContaining)
            {
                const Square threatenedSq = lsb(discovered);
                const Piece  threatenedPc = piece_on(threatenedSq);
                add_dirty_threat(dts, !putPiece, slider, threatenedPc, sliderSq, threatenedSq);
            }

            if (addDirectAttacks)
                add_dirty_threat(dts, putPiece, slider, pc, sliderSq, s);
        }
    };

    if (type_of(pc) == KING)
    {
        if constexpr (ComputeRay)
            process_sliders(false);
        return;
    }

    const Bitboard knights    = pieces(KNIGHT);
    const Bitboard whitePawns = pieces(WHITE, PAWN);
    const Bitboard blackPawns = pieces(BLACK, PAWN);

    Bitboard threatened = attacks_bb(pc, s, occupied) & occupiedNoK;
    Bitboard incoming_threats =
      (PseudoAttacks[KNIGHT][s] & knights) | (PseudoAttacks[KING][s] & kings);

    // Compute both incoming and outgoing pawn threats. Incoming pawn pushers are only
    // added if 'pc' is a pawn.
    if (type_of(pc) == PAWN)
    {
        Bitboard whiteAttacks = PawnPushOrAttacks[WHITE][s];
        Bitboard blackAttacks = PawnPushOrAttacks[BLACK][s];

        threatened |= (color_of(pc) == WHITE ? whiteAttacks : blackAttacks) & pieces(PAWN);

        incoming_threats |= whiteAttacks & blackPawns;
        incoming_threats |= blackAttacks & whitePawns;
    }
    else
    {
        incoming_threats |=
          (attacks_bb<PAWN>(s, WHITE) & blackPawns) | (attacks_bb<PAWN>(s, BLACK) & whitePawns);
    }

    while (threatened)
    {
        Square threatenedSq = pop_lsb(threatened);
        Piece  threatenedPc = piece_on(threatenedSq);
        add_dirty_threat(dts, putPiece, pc, threatenedPc, s, threatenedSq);
    }

    if constexpr (ComputeRay)
        process_sliders(true);
    else
        incoming_threats |= sliders;

    while (incoming_threats)
    {
        Square srcSq = pop_lsb(incoming_threats);
        Piece  srcPc = piece_on(srcSq);
        add_dirty_threat(dts, putPiece, srcPc, pc, srcSq, s);
    }
}

template void Position::update_piece_threats<true>(Piece, bool, Square, DirtyThreats* const, Bitboard) const;
template void Position::update_piece_threats<false>(Piece, bool, Square, DirtyThreats* const, Bitboard) const;

}  // namespace Stockfish

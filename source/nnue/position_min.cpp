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

// SF 83514e49: a slider threatening a queen is a valid feature only if the
// slider is itself a queen (B/R->Q pairs are excluded from the feature map).
constexpr bool can_slider_threat(Piece pc, Piece slider) {
    return type_of(pc) != QUEEN || type_of(slider) == QUEEN;
}

#ifdef USE_AVX512ICL
// F-009 (audit 2026-07-02): port VERBATIM del path VETTORIZZATO del master
// (position.cpp:1160, gia' vendored in nnue/position.cpp ma mai compilato qui:
// il primo port prese solo il ramo scalare). Emette TUTTI i tuple di minaccia
// di una bitboard in una manciata di op AVX512-VBMI invece del loop pop_lsb
// con load piece_on per-casella. Attivo solo su build avx512 (USE_AVX512ICL);
// le build avx2 restano sul fallback scalare sotto.
template<int SqShift, int PcShift>
void write_multiple_dirties(const Position& p,
                            Bitboard        mask,
                            DirtyThreat     dt_template,
                            DirtyThreats*   dts) {
    static_assert(sizeof(DirtyThreat) == 4);

    const __m512i board    = _mm512_loadu_si512(p.piece_array().data());
    const int     dt_count = popcount(mask);
    assert(dt_count <= 16);

    const __m512i template_v = _mm512_set1_epi32(dt_template.raw());
    auto*         write      = dts->list.make_space(dt_count);

    // Extract the list of squares and upconvert to 32 bits. There are never more than 16
    // incoming threats so this is sufficient.
    __m512i threat_squares = _mm512_maskz_compress_epi8(mask, AllSquares);
    threat_squares         = _mm512_cvtepi8_epi32(_mm512_castsi512_si128(threat_squares));

    __m512i threat_pieces =
      _mm512_maskz_permutexvar_epi8(0x1111111111111111ULL, threat_squares, board);

    // Shift the piece and square into place
    threat_squares = _mm512_slli_epi32(threat_squares, SqShift);
    threat_pieces  = _mm512_slli_epi32(threat_pieces, PcShift);

    const __m512i dirties =
      _mm512_ternarylogic_epi32(template_v, threat_squares, threat_pieces, 254 /* A | B | C */);
    _mm512_storeu_si512(write, dirties);
}
#endif
}  // namespace

// Ported VERBATIM from the master Position::update_piece_threats (entrambi i
// path: scalare + ICL vettorizzato — F-009 2026-07-02). Emits the FullThreats tuples that change
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
    const Bitboard occupiedNoK  = occupied ^ pieces(KING);

    // SF 83514e49: filter invalid threat pairs early (pure speedup, the excluded
    // pairs map to index == Dimensions anyway and were dropped downstream).
    Bitboard sliders       = (rookQueens & rAttacks) | (bishopQueens & bAttacks);
    Bitboard directSliders = type_of(pc) == QUEEN ? sliders & pieces(QUEEN) : sliders;

    auto process_sliders = [&](bool addDirectAttacks) {
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
                if (can_slider_threat(threatenedPc, slider))
                    add_dirty_threat(dts, !putPiece, slider, threatenedPc, sliderSq, threatenedSq);
            }

            if (addDirectAttacks && can_slider_threat(pc, slider))
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

    Bitboard threatened       = attacks_bb(pc, s, occupied) & occupiedNoK;
    Bitboard incoming_threats = PseudoAttacks[KNIGHT][s] & knights;

    // Compute both incoming and outgoing pawn threats. Incoming pawn pushers are only
    // added if 'pc' is a pawn.
    Bitboard pawnThreats = 0;
    if (type_of(pc) == PAWN)
    {
        Bitboard whiteAttacks = PawnPushOrAttacks[WHITE][s];
        Bitboard blackAttacks = PawnPushOrAttacks[BLACK][s];

        threatened |= (color_of(pc) == WHITE ? whiteAttacks : blackAttacks) & pieces(PAWN);

        pawnThreats = whiteAttacks & blackPawns;
        pawnThreats |= blackAttacks & whitePawns;
    }
    else
    {
        pawnThreats =
          (attacks_bb<PAWN>(s, WHITE) & blackPawns) | (attacks_bb<PAWN>(s, BLACK) & whitePawns);
    }

    if (type_of(pc) == PAWN || type_of(pc) == KNIGHT || type_of(pc) == ROOK)
        incoming_threats |= pawnThreats;

    switch (type_of(pc))
    {
    case PAWN :
        threatened &= pieces(PAWN, KNIGHT, ROOK);
        break;
    case BISHOP :
    case ROOK :
        threatened &= pieces(PAWN, KNIGHT, BISHOP, ROOK);
        break;
    default :
        threatened &= occupiedNoK;
        break;
    }

#ifdef USE_AVX512ICL
    // F-009: emissione vettoriale. `sliders` non e' ancora consumato qui (la
    // lambda lo svuota dopo) -> i loro attacchi DIRETTI entrano in all_attackers
    // e process_sliders viene chiamata con addDirectAttacks=false (solo discovered).
    DirtyThreat dt_template{pc, NO_PIECE, s, Square(0), putPiece};
    write_multiple_dirties<DirtyThreat::ThreatenedSqOffset, DirtyThreat::ThreatenedPcOffset>(
      *this, threatened, dt_template, dts);

    Bitboard all_attackers = directSliders | incoming_threats;

    dt_template = {NO_PIECE, pc, Square(0), s, putPiece};
    write_multiple_dirties<DirtyThreat::PcSqOffset, DirtyThreat::PcOffset>(*this, all_attackers,
                                                                           dt_template, dts);
#else
    while (threatened)
    {
        Square threatenedSq = pop_lsb(threatened);
        Piece  threatenedPc = piece_on(threatenedSq);
        add_dirty_threat(dts, putPiece, pc, threatenedPc, s, threatenedSq);
    }
#endif

    if constexpr (ComputeRay)
    {
#ifndef USE_AVX512ICL
        process_sliders(true);
#else  // for ICL, direct threats were processed earlier (all_attackers)
        process_sliders(false);
#endif
    }
    else
    {
        incoming_threats |= directSliders;
    }

#ifndef USE_AVX512ICL
    while (incoming_threats)
    {
        Square srcSq = pop_lsb(incoming_threats);
        Piece  srcPc = piece_on(srcSq);
        add_dirty_threat(dts, putPiece, srcPc, pc, srcSq, s);
    }
#endif
}

template void Position::update_piece_threats<true>(Piece, bool, Square, DirtyThreats* const, Bitboard) const;
template void Position::update_piece_threats<false>(Piece, bool, Square, DirtyThreats* const, Bitboard) const;

}  // namespace Stockfish

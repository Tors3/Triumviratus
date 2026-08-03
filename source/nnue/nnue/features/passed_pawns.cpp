/*
  Passed-pawn input features (Triumviratus) — implementation.
  GPLv3, derived from Stockfish NNUE plumbing (see COPYING).
*/

#include "passed_pawns.h"

#include "feat_perm.h"

#include <array>

#include "../../bitboard.h"
#include "../../position.h"

namespace Triumviratus::Eval::NNUE::Features {

namespace {

// PassedSpan[c][s]: same+adjacent files, ranks strictly ahead of s from color
// c's viewpoint. ForwardFile[c][s]: same file only, strictly ahead. Costruite
// su square RAW: l'orientazione entra solo nell'indice (come la band PawnPair).
struct SpanTables {
    Bitboard passedSpan[COLOR_NB][SQUARE_NB];
    Bitboard forwardFile[COLOR_NB][SQUARE_NB];
};

constexpr SpanTables Spans = [] {
    SpanTables t{};
    for (int s = 0; s < SQUARE_NB; s++)
    {
        const int f = s & 7, r = s >> 3;
        for (int c = 0; c < COLOR_NB; c++)
        {
            Bitboard span = 0, fwd = 0;
            for (int rr = 0; rr < 8; rr++)
            {
                const bool ahead = (c == int(WHITE)) ? rr > r : rr < r;
                if (!ahead)
                    continue;
                fwd |= Bitboard(1) << (rr * 8 + f);
                for (int ff = f - 1; ff <= f + 1; ff++)
                    if (ff >= 0 && ff < 8)
                        span |= Bitboard(1) << (rr * 8 + ff);
            }
            t.passedSpan[c][s]  = span;
            t.forwardFile[c][s] = fwd;
        }
    }
    return t;
}();

}  // namespace

Bitboard PassedPawns::passers(Color c, Bitboard ownPawns, Bitboard oppPawns) {
    Bitboard out = 0, b = ownPawns;
    while (b)
    {
        const Square s = pop_lsb(b);
        if (!(oppPawns & Spans.passedSpan[c][s]) && !(ownPawns & Spans.forwardFile[c][s]))
            out |= square_bb(s);
    }
    return out;
}

// Full refresh: one feature per passed pawn of either color.
void PassedPawns::append_active_indices(Color perspective, const Position& pos, IndexList& active) {
    const Square   ksq = pos.square<KING>(perspective);
    const Bitboard wp  = pos.pieces(WHITE, PAWN);
    const Bitboard bp  = pos.pieces(BLACK, PAWN);

    Bitboard pw = passers(WHITE, wp, bp);
    while (pw)
        active.push_back(feat_row(FoldOffset + make_index(perspective, ksq, WHITE, pop_lsb(pw))));

    Bitboard pb = passers(BLACK, bp, wp);
    while (pb)
        active.push_back(feat_row(FoldOffset + make_index(perspective, ksq, BLACK, pop_lsb(pb))));
}

// Incremental: un evento-pedone puo' cambiare lo status passed anche di ALTRI
// pedoni (quelli avversari nella span delle case toccate). Invece di inseguire
// i candidati, ricomputiamo il SET completo dei passati prima/dopo dallo
// snapshot DirtyPawns (<=32 test su maschere, solo su eventi-pedone) e
// XOR-iamo: emissione esatta per costruzione, tutti i casi (cattura,
// promozione, en passant) gestiti uniformemente.
void PassedPawns::append_changed_indices(Color           perspective,
                                         Square          ksq,
                                         const DiffType& diff,
                                         IndexList&      removed,
                                         IndexList&      added) {
    if (!diff.any)
        return;

    Bitboard before[COLOR_NB] = {diff.pawnsBefore[WHITE], diff.pawnsBefore[BLACK]};
    Bitboard after[COLOR_NB]  = {before[WHITE], before[BLACK]};
    for (int i = 0; i < diff.nRemoved; i++)
        after[diff.removedC[i]] &= ~square_bb(diff.removedSq[i]);
    if (diff.addedSq != SQ_NONE)
        after[diff.addedC] |= square_bb(diff.addedSq);

    for (Color c : {WHITE, BLACK})
    {
        const Bitboard pb  = passers(c, before[c], before[~c]);
        const Bitboard pa  = passers(c, after[c], after[~c]);
        Bitboard       rem = pb & ~pa;
        Bitboard       add = pa & ~pb;
        while (rem)
            removed.push_back(feat_row(FoldOffset + make_index(perspective, ksq, c, pop_lsb(rem))));
        while (add)
            added.push_back(feat_row(FoldOffset + make_index(perspective, ksq, c, pop_lsb(add))));
    }
}

}  // namespace Triumviratus::Eval::NNUE::Features

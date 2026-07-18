/*
  Outpost input features (Triumviratus) — implementation.
  GPLv3, derived from Stockfish NNUE plumbing (see COPYING).
*/

#include "outposts.h"

#include <array>

#include "../../bitboard.h"
#include "../../position.h"

namespace Triumviratus::Eval::NNUE::Features {

namespace {

// AttackSpan[c][s]: ADJACENT files only (no same file), ranks strictly ahead
// of s from color c's viewpoint = squares from which an enemy pawn could ever
// come to attack s. Costruita su square RAW: l'orientazione entra solo
// nell'indice (come PassedPawns).
struct SpanTables {
    Bitboard attackSpan[COLOR_NB][SQUARE_NB];
};

constexpr SpanTables Spans = [] {
    SpanTables t{};
    for (int s = 0; s < SQUARE_NB; s++)
    {
        const int f = s & 7, r = s >> 3;
        for (int c = 0; c < COLOR_NB; c++)
        {
            Bitboard span = 0;
            for (int rr = 0; rr < 8; rr++)
            {
                const bool ahead = (c == int(WHITE)) ? rr > r : rr < r;
                if (!ahead)
                    continue;
                for (int ff = f - 1; ff <= f + 1; ff += 2)
                    if (ff >= 0 && ff < 8)
                        span |= Bitboard(1) << (rr * 8 + ff);
            }
            t.attackSpan[c][s] = span;
        }
    }
    return t;
}();

}  // namespace

Bitboard Outposts::outposts(Color c, Bitboard ownPawns, Bitboard oppPawns) {
    // Candidates = squares currently defended by an own pawn.
    Bitboard out = 0;
    Bitboard b   = c == WHITE ? pawn_attacks_bb<WHITE>(ownPawns) : pawn_attacks_bb<BLACK>(ownPawns);
    while (b)
    {
        const Square s = pop_lsb(b);
        if (!(oppPawns & Spans.attackSpan[c][s]))
            out |= square_bb(s);
    }
    return out;
}

// Full refresh: one feature per outpost square of either color.
void Outposts::append_active_indices(Color perspective, const Position& pos, IndexList& active) {
    const Square   ksq = pos.square<KING>(perspective);
    const Bitboard wp  = pos.pieces(WHITE, PAWN);
    const Bitboard bp  = pos.pieces(BLACK, PAWN);

    Bitboard ow = outposts(WHITE, wp, bp);
    while (ow)
        active.push_back(FoldOffset + make_index(perspective, ksq, WHITE, pop_lsb(ow)));

    Bitboard ob = outposts(BLACK, bp, wp);
    while (ob)
        active.push_back(FoldOffset + make_index(perspective, ksq, BLACK, pop_lsb(ob)));
}

// Incremental: identico a PassedPawns — un evento-pedone puo' cambiare lo
// status outpost di piu' case; ricomputiamo il SET completo prima/dopo dallo
// snapshot DirtyPawns e XOR-iamo (emissione esatta per costruzione).
void Outposts::append_changed_indices(Color           perspective,
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
        const Bitboard ob  = outposts(c, before[c], before[~c]);
        const Bitboard oa  = outposts(c, after[c], after[~c]);
        Bitboard       rem = ob & ~oa;
        Bitboard       add = oa & ~ob;
        while (rem)
            removed.push_back(FoldOffset + make_index(perspective, ksq, c, pop_lsb(rem)));
        while (add)
            added.push_back(FoldOffset + make_index(perspective, ksq, c, pop_lsb(add)));
    }
}

}  // namespace Triumviratus::Eval::NNUE::Features

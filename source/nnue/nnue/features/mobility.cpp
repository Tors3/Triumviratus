/*
  Mobility input features (Triumviratus 8.0 study) — implementation.
  GPLv3, derived from Stockfish NNUE plumbing (see COPYING).
*/

#include "mobility.h"

#include <algorithm>

#include "../../attacks.h"
#include "../../bitboard.h"
#include "../../position.h"

namespace Triumviratus::Eval::NNUE::Features {

bool g_mobility_on = false;

void Mobility::snapshot(const Position& pos, MobSnapshot& s) {
    s.color[WHITE] = pos.pieces(WHITE);
    s.color[BLACK] = pos.pieces(BLACK);
    for (PieceType pt = PAWN; pt <= QUEEN; ++pt)
        s.type[pt] = pos.pieces(pt);
}

int Mobility::collect(Color perspective, i8 orientation, const MobSnapshot& s, IndexType* out) {
    const Bitboard occ = s.color[WHITE] | s.color[BLACK];
    int            n   = 0;
    for (Color c : {WHITE, BLACK})
    {
        // Area di mobilita' classica: non le case dei propri pezzi, non quelle
        // battute dai pedoni avversari (una casa dove il pezzo verrebbe cacciato
        // non e' mobilita').
        const Bitboard ePawns = s.type[PAWN] & s.color[~c];
        const Bitboard eAtt   = c == WHITE ? pawn_attacks_bb<BLACK>(ePawns) : pawn_attacks_bb<WHITE>(ePawns);
        const Bitboard area   = ~(s.color[c] | eAtt);
        for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
        {
            Bitboard b = s.type[pt] & s.color[c];
            while (b)
            {
                if (n >= int(MaxActiveDimensions))
                    return n;
                const Square sq  = pop_lsb(b);
                const int    mob = popcount(Attacks::attacks_bb(pt, sq, occ) & area);
                out[n++] = make_index(perspective, orientation, c, pt, sq, bucket(pt, mob));
            }
        }
    }
    return n;
}

void Mobility::append_active_indices(Color perspective, const Position& pos, IndexList& active) {
    MobSnapshot s;
    snapshot(pos, s);
    const Square ksq         = pos.square<KING>(perspective);
    const i8     orientation = FullThreats::OrientTBL[ksq] ^ (56 * perspective);
    IndexType    buf[MaxActiveDimensions];
    const int    n = collect(perspective, orientation, s, buf);
    for (int i = 0; i < n; ++i)
        active.push_back(buf[i]);
}

// Differenza fra i due snapshot come MULTISET: due cavalli con la stessa casa
// orientata non esistono, ma due pezzi dello stesso tipo possono avere lo stesso
// indice solo se stanno sulla stessa casa (impossibile) -> in pratica e' un set,
// ma il merge ordinato tratta correttamente anche i duplicati.
void Mobility::append_changed_indices(Color           perspective,
                                      Square          ksq,
                                      const DiffType& diff,
                                      IndexList&      removed,
                                      IndexList&      added) {
    const i8  orientation = FullThreats::OrientTBL[ksq] ^ (56 * perspective);
    IndexType a[MaxActiveDimensions], b[MaxActiveDimensions];
    const int na = collect(perspective, orientation, diff.before, a);
    const int nb = collect(perspective, orientation, diff.after, b);
    std::sort(a, a + na);
    std::sort(b, b + nb);
    int i = 0, j = 0;
    while (i < na && j < nb)
    {
        if (a[i] == b[j])
            ++i, ++j;
        else if (a[i] < b[j])
            removed.push_back(a[i++]);
        else
            added.push_back(b[j++]);
    }
    while (i < na)
        removed.push_back(a[i++]);
    while (j < nb)
        added.push_back(b[j++]);
}

}  // namespace Triumviratus::Eval::NNUE::Features

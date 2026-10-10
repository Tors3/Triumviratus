/*
  Blocchi da innesto opzionali (09/10/2026) - implementazione. Vedi pawn_grafts.h e, per le definizioni, il riferimento
  del trainer Training_NNUE/graft_blocks/verify_blocks.py.
  GPLv3, derived from Stockfish NNUE plumbing (see COPYING).
*/

#include "pawn_grafts.h"

#include "passed_rel.h"

#include <algorithm>
#ifdef TRIUMV_VERIFY_GRAFT
    #include <cstdio>
    #include <cstdlib>
#endif

#include "../../bitboard.h"
#include "../../nn_board.h"

namespace Triumviratus::Eval::NNUE::Features {


int PawnGrafts::entries_of(unsigned mask, const unsigned long long* bb12, unsigned long long occ, std::uint16_t* out) {
    int n = 0;
    if (mask & PASSED_REL)
    {
        // PassedRel::entries_of: colore << 9 | stato << 6 | casa, per colore e casa crescenti; qui riordinato in
        // colore << 9 | casa << 3 | stato, che resta crescente.
        std::uint16_t e[16];
        const int     k = PassedRel::entries_of(bb12, occ, e);
        for (int i = 0; i < k; i++)
            out[n++] = std::uint16_t((e[i] >> 9) << 9 | (e[i] & 63) << 3 | ((e[i] >> 6) & 7));
    }
    // KingFiles (1), Space (2), LockedPawns (3), Space24 (4), KingFilesQ (5): tolti il 10/10/2026 (codice in
    // _backup/graft_engine_removed_2026-10-10/).
    if (mask & PASSED_STATE)  // PassedState (bit 6, 10/10/2026): esclude PassedRel, le voci sono le sole della lista
        n += PassedState::entries_of(bb12, occ, out + n);
    return n;
}

// make_index: in linea in pawn_grafts.h (_wip graftfix, 10/10/2026).

// Riferimento: il blocco acceso (PassedRel), dalla posizione. Lo usa vg_check_acc.
void PawnGrafts::append_active_indices(Color perspective, const NnBoard& pos, IndexList& active) {
    const int     ksq = pos.king(perspective);
    std::uint16_t e[NN_GRAFT_REF_MAX];
    const int     n = entries_of(nn_graft_mask, pos.bbs(), pos.occ(), e);
    for (int i = 0; i < n; i++)
        active.push_back(FoldOffset + make_index(perspective, ksq, e[i]));
}

// Refresh: le voci dello stato gia' calcolate dal recupero (NnStack::graftList), se ci sono; altrimenti da capo.
void PawnGrafts::append_active_indices(Color perspective, const NnBoard& pos, const NnState& st, IndexList& active) {
    const NnStack& S   = nn_stack_of(st);
    const int      src = S.graftSrc[st.idx];
    const int      ksq = pos.king(perspective);
    if (src == 255)
    {
        std::uint16_t e[NN_GRAFT_MAX];
        const int     n = entries_of(nn_graft_mask & LISTS, pos.bbs(), pos.occ(), e);
        for (int i = 0; i < n; i++)
            active.push_back(FoldOffset + make_index(perspective, ksq, e[i]));
        return;
    }
    const int n = S.graftN[src];
    if (nn_graft_mask == PASSED_REL)
    {
        // _wip graft_passedrel2: come append_changed_indices, senza lo switch sul blocco (PassedRel::make_index).
        const int o = FullThreats::OrientTBL[ksq] ^ (56 * (1 - int(perspective)));
        for (int i = 0; i < n; i++)
        {
            const std::uint16_t v   = S.graftList[src][i];
            const bool          rel = (v >> 9) != int(perspective);
            const int           g   = (rel ? 48 : 0) + (((v >> 3) & 63) ^ o) - 8;  // indice della feature v1
            // Formato a base (GRAFT_PASSEDREL_COSTO2 §8): la riga dello stato base e' zero e non si somma.
            if (PrelBased && int(v & 7) == PrelBase[g])
                continue;
            PREL_HIST(g, v & 7);
            active.push_back(FoldOffset + (v & 7) * 48 + g + (rel ? 336 : 0));
        }
        return;
    }
    for (int i = 0; i < n; i++)
        active.push_back(FoldOffset + make_index(perspective, ksq, S.graftList[src][i]));
}

// O1 (_wip pst_opt): voci di PassedState da capo per il refresh di uno stato senza lista del recupero (radice di uno
// stato di appoggio: eval, nnueverify). Fuori linea perche' qui NnBoard e' completo.
int PawnGrafts::pst_entries_scratch(const NnBoard& pos, std::uint16_t* buf) {
    return PassedState::entries_of(pos.bbs(), pos.occ(), buf);
}

#ifdef TRIUMV_VERIFY_GRAFT
// -DTRIUMV_VERIFY_GRAFT: confronto come insiemi e uscita al primo disaccordo (verify_v1_fused, R1).
namespace {
bool pg_same(const IndexType* a, int na, const IndexType* b, int nb) {
    IndexType s[NN_GRAFT_REF_MAX * 2];
    if (na != nb || na > NN_GRAFT_REF_MAX * 2)
        return false;
    std::copy(a, a + na, s);
    std::sort(s, s + na);
    return std::equal(s, s + na, b);
}

[[noreturn]] void pg_fail(const char* what, Color perspective, int ksq, Bitboard wp, Bitboard bp) {
    std::fprintf(stderr, "[GRAFT] %s diverse dal riferimento: prospettiva %d re %d pedoni %016llx %016llx maschera %u\n",
                 what, int(perspective), ksq, (unsigned long long) wp, (unsigned long long) bp, nn_graft_mask);
    std::fflush(stderr);
    std::abort();
}
}  // namespace

// R1 (_wip graft_passedrel3, 10/10/2026): righe v1 emesse dalla diff di PassedRel (pawn_grafts.h, prel_changed_fused)
// contro il percorso di sempre (PassedPawns::append_changed_indices sulla snapshot dei pedoni). Nelle liste date ci
// sono anche le righe di PassedRel (>= FeatRows) e, con TRIUMV_PREL_DELTA, le righe delta: si confrontano solo le
// righe < FeatRows, cioe' le v1 (feat_row le porta dentro la tabella permutata).
void PawnGrafts::verify_v1_fused(Color             perspective,
                                 int               ksq,
                                 const DirtyPawns& d,
                                 const IndexType*  rem,
                                 int               nr,
                                 const IndexType*  add,
                                 int               na) {
    IndexList refRem, refAdd;
    PassedPawns::append_changed_indices(perspective, ksq, d, refRem, refAdd);
    IndexType gotRem[NN_GRAFT_REF_MAX], gotAdd[NN_GRAFT_REF_MAX], sRem[NN_GRAFT_REF_MAX], sAdd[NN_GRAFT_REF_MAX];
    int       r = 0, a = 0;
    for (int i = 0; i < nr && r < NN_GRAFT_REF_MAX; i++)
        if (rem[i] < FeatRows)
            gotRem[r++] = rem[i];
    for (int i = 0; i < na && a < NN_GRAFT_REF_MAX; i++)
        if (add[i] < FeatRows)
            gotAdd[a++] = add[i];
    const int rr = std::min(refRem.ssize(), int(NN_GRAFT_REF_MAX)), ra = std::min(refAdd.ssize(), int(NN_GRAFT_REF_MAX));
    std::copy(refRem.begin(), refRem.begin() + rr, sRem);
    std::copy(refAdd.begin(), refAdd.begin() + ra, sAdd);
    std::sort(sRem, sRem + rr);
    std::sort(sAdd, sAdd + ra);
    if (!pg_same(gotRem, r, sRem, rr))
        pg_fail("righe v1 tolte dalla diff di PassedRel (R1)", perspective, ksq, d.before[WHITE], d.before[BLACK]);
    if (!pg_same(gotAdd, a, sAdd, ra))
        pg_fail("righe v1 aggiunte dalla diff di PassedRel (R1)", perspective, ksq, d.before[WHITE], d.before[BLACK]);
}
#endif

// append_changed_indices e append_changed_indices_both: in linea in pawn_grafts.h (_wip graftfix, P2), con la diff
// letta da NnState (conteggi) e NnStack::graft (voci).

}  // namespace Triumviratus::Eval::NNUE::Features

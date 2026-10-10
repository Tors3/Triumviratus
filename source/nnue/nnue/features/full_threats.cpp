/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

//Definition of input features FullThreats of NNUE evaluation function

#include "full_threats.h"

#include "feat_perm.h"

#include "../../../profile.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "../../attacks.h"
#include "../../bitboard.h"
#include "../../misc.h"
#include "../../nn_board.h"
#include "../../types.h"
#include "../nnue_common.h"
#include "../../../nn_attacks.h"   // attacchi del motore (stessa numerazione delle case)

namespace Triumviratus::Eval::NNUE::Features {

// Le tabelle qui sotto fino a index_lut2 sono costruite nella numerazione della RETE (a1 = 0, codici W_PAWN..B_KING),
// come il trainer: sono la definizione degli indici. A runtime si usano le loro copie nella numerazione del motore
// (EngineThreatTables, piu' in basso), costruite a compilazione dalle stesse.

struct HelperOffsets {
    int cumulativePieceOffset, cumulativeOffset;
};

constexpr std::array<Piece, 12> AllPieces = {
  W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
  B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
};

template<PieceType PT>
constexpr auto make_piece_indices_type() {
    static_assert(PT != PieceType::PAWN);

    std::array<std::array<u8, SQUARE_NB>, SQUARE_NB> out{};

    for (Square from = SQ_A1; from <= SQ_H8; ++from)
    {
        Bitboard attacks = Attacks::PseudoAttacks[PT][from];

        for (Square to = SQ_A1; to <= SQ_H8; ++to)
        {
            out[from][to] = constexpr_popcount(((1ULL << to) - 1) & attacks);
        }
    }

    return out;
}

template<Piece P>
constexpr auto make_piece_indices_piece() {
    static_assert(type_of(P) == PieceType::PAWN);

    std::array<std::array<u8, SQUARE_NB>, SQUARE_NB> out{};

    constexpr Color C = color_of(P);

    for (Square from = SQ_A1; from <= SQ_H8; ++from)
    {
        Bitboard attacks = Attacks::PseudoAttacks[C][from];

        for (Square to = SQ_A1; to <= SQ_H8; ++to)
        {
            out[from][to] = constexpr_popcount(((1ULL << to) - 1) & attacks);
        }
    }

    return out;
}

constexpr auto index_lut2_array() {
    constexpr auto KNIGHT_ATTACKS = make_piece_indices_type<PieceType::KNIGHT>();
    constexpr auto BISHOP_ATTACKS = make_piece_indices_type<PieceType::BISHOP>();
    constexpr auto ROOK_ATTACKS   = make_piece_indices_type<PieceType::ROOK>();
    constexpr auto QUEEN_ATTACKS  = make_piece_indices_type<PieceType::QUEEN>();
    constexpr auto KING_ATTACKS   = make_piece_indices_type<PieceType::KING>();

    std::array<std::array<std::array<u8, SQUARE_NB>, SQUARE_NB>, PIECE_NB> indices{};

    indices[W_PAWN] = make_piece_indices_piece<W_PAWN>();
    indices[B_PAWN] = make_piece_indices_piece<B_PAWN>();

    indices[W_KNIGHT] = KNIGHT_ATTACKS;
    indices[B_KNIGHT] = KNIGHT_ATTACKS;

    indices[W_BISHOP] = BISHOP_ATTACKS;
    indices[B_BISHOP] = BISHOP_ATTACKS;

    indices[W_ROOK] = ROOK_ATTACKS;
    indices[B_ROOK] = ROOK_ATTACKS;

    indices[W_QUEEN] = QUEEN_ATTACKS;
    indices[B_QUEEN] = QUEEN_ATTACKS;

    indices[W_KING] = KING_ATTACKS;
    indices[B_KING] = KING_ATTACKS;

    return indices;
}

constexpr auto init_threat_offsets() {
    std::array<HelperOffsets, PIECE_NB>                    indices{};
    std::array<std::array<IndexType, SQUARE_NB>, PIECE_NB> offsets{};

    int cumulativeOffset = 0;
    for (Piece piece : AllPieces)
    {
        int pieceIdx              = piece;
        int cumulativePieceOffset = 0;

        for (Square from = SQ_A1; from <= SQ_H8; ++from)
        {
            offsets[pieceIdx][from] = cumulativePieceOffset;

            if (type_of(piece) != PAWN)
            {
                Bitboard attacks = Attacks::PseudoAttacks[type_of(piece)][from];
                cumulativePieceOffset += constexpr_popcount(attacks);
            }

            else if (from >= SQ_A2 && from <= SQ_H7)
            {
                Bitboard attacks = (pieceIdx < 8) ? Attacks::PseudoAttacks[WHITE][from]
                                                  : Attacks::PseudoAttacks[BLACK][from];
                cumulativePieceOffset += constexpr_popcount(attacks);
            }
        }

        indices[pieceIdx] = {cumulativePieceOffset, cumulativeOffset};

        cumulativeOffset += numValidTargets[pieceIdx] * cumulativePieceOffset;
    }

    return std::pair{indices, offsets};
}

// Totale feature calcolato dalla tabella: DEVE combaciare con FullThreats::Dimensions.
// Senza questo assert la costante e la tabella potevano divergere in SILENZIO -> tutti gli
// indici dei blocchi folded (PawnPair, PassedPawns) sarebbero scivolati e il training avrebbe
// imparato su una mappatura sbagliata senza che niente lo segnalasse. (27/07/2026)
constexpr int threat_total_features() {
    int total = 0;
    for (Piece piece : AllPieces)
    {
        int pieceOffset = 0;
        for (Square from = SQ_A1; from <= SQ_H8; ++from)
        {
            if (type_of(piece) != PAWN)
                pieceOffset += constexpr_popcount(Attacks::PseudoAttacks[type_of(piece)][from]);
            else if (from >= SQ_A2 && from <= SQ_H7)
                pieceOffset += constexpr_popcount(
                  (int(piece) < 8) ? Attacks::PseudoAttacks[WHITE][from]
                                   : Attacks::PseudoAttacks[BLACK][from]);
        }
        total += numValidTargets[int(piece)] * pieceOffset;
    }
    return total;
}

static_assert(threat_total_features() == FullThreats::Dimensions,
              "FullThreats::Dimensions non combacia con la tabella di offset calcolata da "
              "numValidTargets/PseudoAttacks. Aggiornare la costante nell'header.");

constexpr auto helper_offsets = init_threat_offsets().first;
// Lookup array for indexing threats
constexpr auto offsets = init_threat_offsets().second;

constexpr auto init_index_luts() {
    std::array<std::array<std::array<u32, 2>, PIECE_NB>, PIECE_NB> indices{};

    for (Piece attacker : AllPieces)
    {
        for (Piece attacked : AllPieces)
        {
            bool      enemy        = (attacker ^ attacked) == 8;
            PieceType attackerType = type_of(attacker);
            PieceType attackedType = type_of(attacked);

            int  map           = FullThreats::map[attackerType - 1][attackedType - 1];
            bool semi_excluded = attackerType == attackedType && (enemy || attackerType != PAWN);
            IndexType feature  = helper_offsets[attacker].cumulativeOffset
                              + (color_of(attacked) * (numValidTargets[attacker] / 2) + map)
                                  * helper_offsets[attacker].cumulativePieceOffset;

            // 🔴 Il marcatore delle feature ESCLUSE e' `FeatDeadBase` (= FeatRows), non
            // piu' `FullThreats::Dimensions`. Motivo: l'indice finale e' base + offsets +
            // lut2, quindi per un'esclusa vale FeatDeadBase + qualcosa. Ancorandolo a
            // FeatRows tutti i valori morti cadono nella coda sentinella di FeatPerm e il
            // filtro resta una lettura senza branch. Con Dimensions (59808) sarebbero
            // finiti dentro il segmento PawnPair, che e' fatto di righe VALIDE.
            bool excluded                  = map < 0;
            indices[attacker][attacked][0] = excluded ? FeatDeadBase : feature;
            indices[attacker][attacked][1] = excluded || semi_excluded ? FeatDeadBase : feature;
        }
    }

    return indices;
}

// The final index is calculated from summing data found in these two LUTs, as well
// as offsets[attacker][from]

// [attacker][attacked][from < to]
constexpr auto index_lut1 = init_index_luts();
// [attacker][from][to]
// ⛔ 05/10/2026 — PROVATO E RIGETTATO: lut2 (64 KB) e offsets (4 KB) sostituite da una voce da 16 byte per
// [attacker][from] (maschera degli attacchi + offset) e popcount(bzhi(maschera, to)); in append_changed_indices_both
// i contatori delle quattro liste in registro (scrittura in entrambe le liste, avanza solo quella giusta). Nodi
// identici; xperf 6 giri quieti, build PGO: istruzioni +1,58%, cicli +0,81% (30 posizioni), +0,82% (30 finali).
// Queste tabelle restano gia' in L1 fra un update e l'altro: popcount, bzhi e le scritture doppie costano di piu'.
constexpr auto index_lut2 = index_lut2_array();

// ---------------------------------------------------------------------------------------------------------------------
// Le stesse tabelle nella numerazione del MOTORE (07/10/2026, scacchiera unica v2). Nessuna conversione a runtime.
//
// Case: la rete numera a1 = 0, il motore a8 = 0, cioe' casa_rete = casa ^ 56. La rete orienta una casa con
// (casa_rete ^ OrientTBL[re] ^ 56 * p); siccome OrientTBL dipende solo dalla colonna (vedi lo static_assert),
// (casa ^ 56 ^ OrientTBL[re] ^ 56 * p) = (casa ^ OrientTBL[re] ^ 56 * (1 - p)): con la riflessione del colore
// complementare la casa ORIENTATA e' la stessa, e lut2/offsets si leggono nelle stesse righe.
// Pezzi: la rete orienta un pezzo scambiandone il colore per il nero (codice ^ 8). Qui l'orientamento e' nelle tabelle,
// indicizzate per [prospettiva][codice del motore]: lut1 e offsets per pezzo, lut2 per riga (le righe di lut2 dipendono
// solo dal tipo, e per il pedone dal colore orientato: sette righe, 28 KB invece di 64).
// ---------------------------------------------------------------------------------------------------------------------
constexpr bool threat_orient_by_file_only() {
    for (int s = 0; s < SQUARE_NB; ++s)
        if (FullThreats::OrientTBL[s] != FullThreats::OrientTBL[s ^ 56])
            return false;
    return true;
}
static_assert(threat_orient_by_file_only(), "FullThreats::OrientTBL deve dipendere solo dalla colonna");

// codice del motore (0..11) -> codice della rete (W_PAWN..B_KING)
constexpr int net_piece(int e) { return e + 1 + 2 * (e >= NN_BLACK); }

struct EngineThreatTables {
    u32 lut1[COLOR_NB][12][12][2];       // [prospettiva][attaccante][attaccato][from < to orientati]
    u32 offsets[COLOR_NB][12][SQUARE_NB];  // [prospettiva][attaccante][from orientato]
    u8  lut2Row[COLOR_NB][12];           // [prospettiva][attaccante] -> riga di lut2
    u8  lut2[7][SQUARE_NB][SQUARE_NB];   // righe: pedone bianco, pedone nero (orientati), cavallo .. re
};

constexpr EngineThreatTables make_engine_threat_tables() {
    EngineThreatTables t{};
    for (int p = 0; p < COLOR_NB; ++p)
        for (int a = 0; a < 12; ++a)
        {
            const int na = net_piece(a) ^ (8 * p);  // pezzo orientato, codice della rete
            for (int d = 0; d < 12; ++d)
                for (int lt = 0; lt < 2; ++lt)
                    t.lut1[p][a][d][lt] = index_lut1[na][net_piece(d) ^ (8 * p)][lt];
            for (int s = 0; s < SQUARE_NB; ++s)
                t.offsets[p][a][s] = offsets[na][s];
            const int type = (na & 7) - 1;  // 0 = pedone .. 5 = re
            t.lut2Row[p][a] = u8(type == 0 ? (na >> 3) : type + 1);
        }
    for (int row = 0; row < 7; ++row)
    {
        const int na = row == 0 ? W_PAWN : row == 1 ? B_PAWN : W_PAWN + row - 1;
        for (int f = 0; f < SQUARE_NB; ++f)
            for (int s = 0; s < SQUARE_NB; ++s)
                t.lut2[row][f][s] = index_lut2[na][f][s];
    }
    return t;
}

// La riga di lut2 di un pezzo deve essere uguale per i due colori (tranne il pedone): e' cio' che permette sette righe.
constexpr bool lut2_rows_by_type() {
    for (int pt = KNIGHT; pt <= KING; ++pt)
        for (int f = 0; f < SQUARE_NB; ++f)
            for (int s = 0; s < SQUARE_NB; ++s)
                if (index_lut2[pt][f][s] != index_lut2[pt + 8][f][s])
                    return false;
    return true;
}
static_assert(lut2_rows_by_type(), "index_lut2: le righe dei pezzi (non pedoni) devono valere per i due colori");

constexpr EngineThreatTables ThreatTbl = make_engine_threat_tables();

// Index of a feature for a given king position and another piece on some square (numerazione del motore)
inline sf_always_inline IndexType
FullThreats::make_index(Color perspective, int attacker, int from, int to, int attacked, int ksq) {
    const int      orientation   = OrientTBL[ksq] ^ (56 * (1 - int(perspective)));
    const unsigned from_oriented = unsigned(from ^ orientation);
    const unsigned to_oriented   = unsigned(to ^ orientation);

    return ThreatTbl.lut1[perspective][attacker][attacked][from_oriented < to_oriented]
         + ThreatTbl.offsets[perspective][attacker][from_oriented]
         + ThreatTbl.lut2[ThreatTbl.lut2Row[perspective][attacker]][from_oriented][to_oriented];
}

// Attacchi di un pezzo non pedone, dalle tabelle del motore (nn_attacks.h).
static inline Bitboard engine_attacks(int pt, int s, Bitboard occupied) {
    switch (pt)
    {
    case NN_KNIGHT :
        return knight_attacks[s];
    case NN_BISHOP :
        return get_bishop_attacks(s, occupied);
    case NN_ROOK :
        return get_rook_attacks(s, occupied);
    default :  // NN_QUEEN: un solo calcolo DualMagic invece di due (AA1, 10/10/2026, stessi attacchi)
        return get_queen_attacks(s, occupied);
    }
}

// Get a list of indices for active features (refresh), sulla nostra scacchiera.

void FullThreats::append_active_indices(Color perspective, const NnBoard& pos, IndexList& active) {
    const int      ksq      = pos.king(perspective);
    const Bitboard occupied = pos.occ();

    // SF 83514e49 (2026-07-03): filter invalid threat pairs early — pairs outside
    // these masks map to excluded features anyway (index == Dimensions), skipping
    // them here is a pure speedup. No functional change.
    const Bitboard pawnTargets        = pos.type(NN_KNIGHT) | pos.type(NN_ROOK);
    const Bitboard minorSliderTargets = pawnTargets | pos.type(NN_PAWN) | pos.type(NN_BISHOP);
    const Bitboard queenTargets       = minorSliderTargets | pos.type(NN_QUEEN);

#ifdef TRIUMV_X4_VLREG
    // X4 (08/10/2026, VLREG): contatore della lista in un registro (vedi ValueList::data in misc.h).
    IndexType* const out = active.data();
    usize            cnt = active.size();
    #define X4_PUT_ACTIVE(row) \
        do \
        { \
            const IndexType r_ = (row); \
            out[cnt]           = r_; \
            cnt += r_ < FeatRows; \
        } while (0)
#else
    #define X4_PUT_ACTIVE(row) active.push_back_if_lt((row), FeatRows)
#endif

    for (Color color : {WHITE, BLACK})
    {
        const Color c = Color(perspective ^ color);

        {
            const int      attacker = NN_PAWN + NN_BLACK * c;
            const Bitboard cPawns   = pos.bb(attacker);

            // `fromDelta`: dalla casa attaccata alla casa del pedone. Nella nostra numerazione il bianco avanza verso
            // gli indici piu' bassi: un pedone bianco su s attacca s - 7 (colonna + 1) e s - 9 (colonna - 1).
            auto process_pawn_attacks = [&](Bitboard attacks, int fromDelta) {
                while (attacks)
                {
                    const int to       = pop_lsb(attacks);
                    const int from     = to + fromDelta;
                    const int attacked = pos.piece_on(to);
                    IndexType index    = make_index(perspective, attacker, from, to, attacked, ksq);
                    X4_PUT_ACTIVE(feat_row(index));
                }
            };

            if (c == WHITE)
            {
                process_pawn_attacks(((cPawns & ~FileHBB) >> 7) & pawnTargets, 7);
                process_pawn_attacks(((cPawns & ~FileABB) >> 9) & pawnTargets, 9);
            }
            else
            {
                process_pawn_attacks(((cPawns & ~FileABB) << 7) & pawnTargets, -7);
                process_pawn_attacks(((cPawns & ~FileHBB) << 9) & pawnTargets, -9);
            }
        }

        for (int pt = NN_KNIGHT; pt < NN_KING; ++pt)
        {
            const int attacker = pt + NN_BLACK * c;
            Bitboard  bb       = pos.bb(attacker);
            while (bb)
            {
                const int from    = pop_lsb(bb);
                Bitboard  targets = pt == NN_KNIGHT || pt == NN_QUEEN ? queenTargets : minorSliderTargets;
                Bitboard  attacks = engine_attacks(pt, from, occupied) & targets;
                while (attacks)
                {
                    const int to       = pop_lsb(attacks);
                    const int attacked = pos.piece_on(to);
                    IndexType index    = make_index(perspective, attacker, from, to, attacked, ksq);
                    X4_PUT_ACTIVE(feat_row(index));
                }
            }
        }
    }
#ifdef TRIUMV_X4_VLREG
    active.set_size(cnt);
#endif
#undef X4_PUT_ACTIVE
}

// Get a list of indices for recently changed features

void FullThreats::append_changed_indices(Color                   perspective,
                                         int                     ksq,
                                         const DiffType&         diff,
                                         IndexList&              removed,
                                         IndexList&              added,
                                         const ThreatWeightType* prefetchBase,
                                         IndexType               prefetchStride) {

#ifdef TRIUMV_X4_VLREG
    // X4 (08/10/2026, VLREG): stesse voci nello stesso ordine, con i due contatori e il numero di tuple in registri
    // (vedi ValueList::data in misc.h). La voce si scrive sempre nello slot libero della lista scelta e il contatore
    // avanza solo se la riga e' viva, come push_back_if_lt.
    IndexType* const rem  = removed.data();
    IndexType* const addv = added.data();
    usize            nRem = removed.size(), nAdd = added.size();
    const u32        n    = diff.n;
    for (u32 i = 0; i < n; ++i)
    {
        const DirtyThreat dirty(diff.list[i]);
        const bool        add   = dirty.add();
        const IndexType   index = feat_row(
          make_index(perspective, dirty.pc(), dirty.pc_sq(), dirty.threatened_sq(), dirty.threatened_pc(), ksq));
        if (prefetchBase)
            prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(reinterpret_cast<const void*>(
              reinterpret_cast<uintptr_t>(prefetchBase) + index * prefetchStride));
        const usize live = index < FeatRows;
        *(add ? addv + nAdd : rem + nRem) = index;
        nAdd += add ? live : 0;
        nRem += add ? 0 : live;
    }
    removed.set_size(nRem);
    added.set_size(nAdd);
#else
    for (u32 i = 0; i < diff.n; ++i)
    {
        const DirtyThreat dirty(diff.list[i]);
        auto attacker = dirty.pc();
        auto attacked = dirty.threatened_pc();
        auto from     = dirty.pc_sq();
        auto to       = dirty.threatened_sq();
        auto add      = dirty.add();

        auto&           insert = add ? added : removed;
        // `feat_row` rimappa alla riga permutata; per le feature morte ritorna la
        // sentinella FeatRows, che `push_back_if_lt` scarta esattamente come prima.
        const IndexType index = feat_row(make_index(perspective, attacker, from, to, attacked, ksq));

// 🔴 Sotto PROFILE_LIGHT questi contatori spariscono: stanno DENTRO il ciclo degli
// indici, il punto piu' caldo dell'update, e con loro attivi la misura della fase
// includeva l'instrumentazione. Stessa trappola degli istogrammi (nnue_accumulator.cpp).
#if defined(TRIUMV_PROFILE) && !defined(TRIUMV_PROFILE_LIGHT)
        // Quante tuple vengono generate e poi BUTTATE (map < 0 => riga == FeatRows).
        // Il prefetch qui sotto parte comunque: in regime memory-bound e' traffico sprecato.
        prof_n_thr_seen++;
        if (index >= FeatRows)
        {
            prof_n_thr_dead++;
            // Chi sono le tuple ancora scartate? [tipo attaccante][tipo attaccato]
            prof_dead_pair[attacker % 6 + 1][attacked % 6 + 1]++;
        }
#endif
        // ⛔ PROVATO E RIGETTATO il 3/08/2026: prefetchare i 4 tile SIMD della riga
        // invece della sola prima linea (la riga e' 1024 byte = 16 linee, e
        // `apply_combined` la consuma in 4 tile da 256 byte) misura **-5,92% NPS**
        // — 23/150 posizioni vinte, nodi identici (121.575.142), PGO avx2
        // interlacciato. Il ragionamento "copriamo 16/16 invece di 1/16" e' sbagliato:
        // le 15 linee restanti sono SEQUENZIALI dentro la riga e lo streamer L2 le
        // prendeva gia' da solo. I prefetch in piu' non aggiungono copertura, tolgono
        // slot di load e voci di fill buffer al lavoro vero. UNA linea per riga e'
        // l'ottimo, non un compromesso.
        if (prefetchBase)
            prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(reinterpret_cast<const void*>(
              reinterpret_cast<uintptr_t>(prefetchBase) + index * prefetchStride));
        insert.push_back_if_lt(index, FeatRows);
    }
#endif
}

// Porting completo di SF 7b550409 — vedi il commento in full_threats.h per la
// differenza voluta rispetto alla loro forma (niente alternanza delle scritture).
void FullThreats::append_changed_indices_both(int                     ksqW,
                                              int                     ksqB,
                                              const DiffType&         diff,
                                              IndexList&              removedW,
                                              IndexList&              addedW,
                                              IndexList&              removedB,
                                              IndexList&              addedB,
                                              const ThreatWeightType* prefetchBase,
                                              IndexType               prefetchStride) {

#ifdef TRIUMV_X4_VLREG
    // X4 (08/10/2026, VLREG): come append_changed_indices, quattro contatori in registri.
    IndexType* const remW = removedW.data();
    IndexType* const addW = addedW.data();
    IndexType* const remB = removedB.data();
    IndexType* const addB = addedB.data();
    usize nRemW = removedW.size(), nAddW = addedW.size(), nRemB = removedB.size(), nAddB = addedB.size();
    const u32 n = diff.n;
    for (u32 i = 0; i < n; ++i)
    {
        const DirtyThreat dirty(diff.list[i]);
        const auto        attacker = dirty.pc();
        const auto        attacked = dirty.threatened_pc();
        const auto        from     = dirty.pc_sq();
        const auto        to       = dirty.threatened_sq();
        const bool        add      = dirty.add();

        const IndexType iW = feat_row(make_index(WHITE, attacker, from, to, attacked, ksqW));
        const IndexType iB = feat_row(make_index(BLACK, attacker, from, to, attacked, ksqB));
        if (prefetchBase)
        {
            prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(reinterpret_cast<const void*>(
              reinterpret_cast<uintptr_t>(prefetchBase) + iW * prefetchStride));
            prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(reinterpret_cast<const void*>(
              reinterpret_cast<uintptr_t>(prefetchBase) + iB * prefetchStride));
        }
        const usize liveW = iW < FeatRows, liveB = iB < FeatRows;
        *(add ? addW + nAddW : remW + nRemW) = iW;
        *(add ? addB + nAddB : remB + nRemB) = iB;
        nAddW += add ? liveW : 0;
        nRemW += add ? 0 : liveW;
        nAddB += add ? liveB : 0;
        nRemB += add ? 0 : liveB;
    }
    removedW.set_size(nRemW);
    addedW.set_size(nAddW);
    removedB.set_size(nRemB);
    addedB.set_size(nAddB);
#else
    for (u32 i = 0; i < diff.n; ++i)
    {
        const DirtyThreat dirty(diff.list[i]);
        // Decodifica UNA volta sola: e' l'unica cosa condivisibile fra le due
        // prospettive, piu' il fatto che `dirty` si legge una volta invece di due
        // a distanza di un intero aggiornamento di accumulatore.
        const auto attacker = dirty.pc();
        const auto attacked = dirty.threatened_pc();
        const auto from     = dirty.pc_sq();
        const auto to       = dirty.threatened_sq();
        const bool add      = dirty.add();

        const IndexType iW = feat_row(make_index(WHITE, attacker, from, to, attacked, ksqW));
        const IndexType iB = feat_row(make_index(BLACK, attacker, from, to, attacked, ksqB));

#ifdef TRIUMV_PROFILE
        // Due tuple viste (una per prospettiva), come nel percorso a prospettiva
        // singola chiamato due volte: i contatori restano confrontabili.
        prof_n_thr_seen += 2;
        if (iW >= FeatRows)
        {
            prof_n_thr_dead++;
            prof_dead_pair[attacker % 6 + 1][attacked % 6 + 1]++;
        }
        if (iB >= FeatRows)
        {
            prof_n_thr_dead++;
            prof_dead_pair[attacker % 6 + 1][attacked % 6 + 1]++;
        }
#endif
        // UNA linea per riga, come nel percorso singolo: le altre 15 sono
        // sequenziali dentro la riga e le prende lo streamer L2 (il prefetch dei 4
        // tile aveva misurato −5,92%).
        if (prefetchBase)
        {
                prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(reinterpret_cast<const void*>(
                  reinterpret_cast<uintptr_t>(prefetchBase) + iW * prefetchStride));
                prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(reinterpret_cast<const void*>(
                  reinterpret_cast<uintptr_t>(prefetchBase) + iB * prefetchStride));
        }

        (add ? addedW : removedW).push_back_if_lt(iW, FeatRows);
        (add ? addedB : removedB).push_back_if_lt(iB, FeatRows);
    }
#endif
}

}  // namespace Triumviratus::Eval::NNUE::Features

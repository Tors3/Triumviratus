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
#include "../../position.h"
#include "../../types.h"
#include "../nnue_common.h"

namespace Triumviratus::Eval::NNUE::Features {

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
constexpr auto index_lut2 = index_lut2_array();

// Index of a feature for a given king position and another piece on some square
inline sf_always_inline IndexType FullThreats::make_index(
  Color perspective, Piece attacker, Square from, Square to, Piece attacked, Square ksq) {
    const i8 orientation   = OrientTBL[ksq] ^ (56 * perspective);
    unsigned from_oriented = u8(from) ^ orientation;
    unsigned to_oriented   = u8(to) ^ orientation;

    i8       swap              = 8 * perspective;
    unsigned attacker_oriented = attacker ^ swap;
    unsigned attacked_oriented = attacked ^ swap;

    return index_lut1[attacker_oriented][attacked_oriented][from_oriented < to_oriented]
         + offsets[attacker_oriented][from_oriented]
         + index_lut2[attacker_oriented][from_oriented][to_oriented];
}

// Get a list of indices for active features in ascending order

void FullThreats::append_active_indices(Color perspective, const Position& pos, IndexList& active) {
    const Square   ksq      = pos.square<KING>(perspective);
    const Bitboard occupied = pos.pieces();

    // SF 83514e49 (2026-07-03): filter invalid threat pairs early — pairs outside
    // these masks map to excluded features anyway (index == Dimensions), skipping
    // them here is a pure speedup. No functional change.
    const Bitboard pawnTargets        = pos.pieces(KNIGHT, ROOK);
    const Bitboard minorSliderTargets = pos.pieces(PAWN, KNIGHT, BISHOP, ROOK);
    const Bitboard queenTargets       = pos.pieces(PAWN, KNIGHT, BISHOP, ROOK, QUEEN);

    for (Color color : {WHITE, BLACK})
    {
        const Color c = Color(perspective ^ color);

        {
            const Piece    attacker = make_piece(c, PAWN);
            const Bitboard cPawns   = pos.pieces(c, PAWN);

            auto process_pawn_attacks = [&](Bitboard attacks, Direction attkDir) {
                while (attacks)
                {
                    Square to       = pop_lsb(attacks);
                    Square from     = to - attkDir;
                    Piece  attacked = pos.piece_on(to);
                    IndexType index = make_index(perspective, attacker, from, to, attacked, ksq);
                    active.push_back_if_lt(feat_row(index), FeatRows);
                }
            };

            if (c == WHITE)
            {
                process_pawn_attacks(shift<NORTH_EAST>(cPawns) & pawnTargets, NORTH_EAST);
                process_pawn_attacks(shift<NORTH_WEST>(cPawns) & pawnTargets, NORTH_WEST);
            }
            else
            {
                process_pawn_attacks(shift<SOUTH_WEST>(cPawns) & pawnTargets, SOUTH_WEST);
                process_pawn_attacks(shift<SOUTH_EAST>(cPawns) & pawnTargets, SOUTH_EAST);
            }
        }

        for (PieceType pt = KNIGHT; pt < KING; ++pt)
        {
            Piece    attacker = make_piece(c, pt);
            Bitboard bb       = pos.pieces(c, pt);
            while (bb)
            {
                Square   from    = pop_lsb(bb);
                Bitboard targets = pt == KNIGHT || pt == QUEEN ? queenTargets : minorSliderTargets;
                Bitboard attacks = Attacks::attacks_bb(pt, from, occupied) & targets;
                while (attacks)
                {
                    Square    to       = pop_lsb(attacks);
                    Piece     attacked = pos.piece_on(to);
                    IndexType index    = make_index(perspective, attacker, from, to, attacked, ksq);
                    active.push_back_if_lt(feat_row(index), FeatRows);
                }
            }
        }
    }
}

// Get a list of indices for recently changed features

void FullThreats::append_changed_indices(Color                   perspective,
                                         Square                  ksq,
                                         const DiffType&         diff,
                                         IndexList&              removed,
                                         IndexList&              added,
                                         const ThreatWeightType* prefetchBase,
                                         IndexType               prefetchStride) {

    for (const auto& dirty : diff.list)
    {
        auto attacker = dirty.pc();
        auto attacked = dirty.threatened_pc();
        auto from     = dirty.pc_sq();
        auto to       = dirty.threatened_sq();
        auto add      = dirty.add();

        auto&           insert = add ? added : removed;
        // `feat_row` rimappa alla riga permutata; per le feature morte ritorna la
        // sentinella FeatRows, che `push_back_if_lt` scarta esattamente come prima.
        const IndexType index = feat_row(make_index(perspective, attacker, from, to, attacked, ksq));

#ifdef TRIUMV_PROFILE
        // Quante tuple vengono generate e poi BUTTATE (map < 0 => riga == FeatRows).
        // Il prefetch qui sotto parte comunque: in regime memory-bound e' traffico sprecato.
        prof_n_thr_seen++;
        if (index >= FeatRows)
        {
            prof_n_thr_dead++;
            // Chi sono le tuple ancora scartate? [tipo attaccante][tipo attaccato]
            prof_dead_pair[type_of(attacker)][type_of(attacked)]++;
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
#ifdef TRIUMV_PF_SMALL
        // C7: il filtro push_back_if_lt sotto scarta le tuple morte, ma il prefetch
        // partiva comunque su una riga OLTRE la tabella viva: fill buffer sprecato.
        if (prefetchBase && index < FeatRows)
#else
        if (prefetchBase)
#endif
            prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(reinterpret_cast<const void*>(
              reinterpret_cast<uintptr_t>(prefetchBase) + index * prefetchStride));
        insert.push_back_if_lt(index, FeatRows);
    }
}

// Porting completo di SF 7b550409 — vedi il commento in full_threats.h per la
// differenza voluta rispetto alla loro forma (niente alternanza delle scritture).
void FullThreats::append_changed_indices_both(Square                  ksqW,
                                              Square                  ksqB,
                                              const DiffType&         diff,
                                              IndexList&              removedW,
                                              IndexList&              addedW,
                                              IndexList&              removedB,
                                              IndexList&              addedB,
                                              const ThreatWeightType* prefetchBase,
                                              IndexType               prefetchStride) {

    for (const auto& dirty : diff.list)
    {
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
            prof_dead_pair[type_of(attacker)][type_of(attacked)]++;
        }
        if (iB >= FeatRows)
        {
            prof_n_thr_dead++;
            prof_dead_pair[type_of(attacker)][type_of(attacked)]++;
        }
#endif
        // UNA linea per riga, come nel percorso singolo: le altre 15 sono
        // sequenziali dentro la riga e le prende lo streamer L2 (il prefetch dei 4
        // tile aveva misurato −5,92%).
        if (prefetchBase)
        {
#ifdef TRIUMV_PF_SMALL
            if (iW < FeatRows)
#endif
                prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(reinterpret_cast<const void*>(
                  reinterpret_cast<uintptr_t>(prefetchBase) + iW * prefetchStride));
#ifdef TRIUMV_PF_SMALL
            if (iB < FeatRows)
#endif
                prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(reinterpret_cast<const void*>(
                  reinterpret_cast<uintptr_t>(prefetchBase) + iB * prefetchStride));
        }

        (add ? addedW : removedW).push_back_if_lt(iW, FeatRows);
        (add ? addedB : removedB).push_back_if_lt(iB, FeatRows);
    }
}

}  // namespace Triumviratus::Eval::NNUE::Features

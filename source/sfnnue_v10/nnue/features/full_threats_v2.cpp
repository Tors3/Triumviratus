/*
  Full_Threats V2 implementation.

  Logica replicata BIT-PER-BIT da `data_loader/cpp/training_data_loader.cpp`
  del master nnue-pytorch commit 89d572575441c927947fdc8b98c41719c530d120.
  Sezioni:
   - threat_index: lines ~183-218
   - fill_features_sparse (=append_active_indices logic): lines ~220-302
   - threatfeaturecalc (=offsets init): lines ~96-135

  Convenzioni harness:
   - Piece = SF (W_PAWN=1..W_KING=6, B_PAWN=9..B_KING=14, NO_PIECE=0)
   - Master usa una numerazione INTERLEAVED (0=wP,1=bP,2=wN,...,11=bK) per le
     LUT (numvalidtargets, threatoffsets). Conversione: idx = 2*(type-1) + color.
   - Square = SF a1=0..h8=63
*/

#include "full_threats_v2.h"

#include <array>
#include <cstdint>

#include "../../bitboard.h"
#include "../../position.h"
#include "../../types.h"
#include "../nnue_common.h"

namespace Stockfish::Eval::NNUE::Features {

// Convert SF Piece -> master interleaved index (0=wP,1=bP,2=wN,...,11=bK).
// Pre: pc != NO_PIECE.
static inline int piece_to_v2idx(Piece pc) {
    return 2 * (int(type_of(pc)) - 1) + int(color_of(pc));
}

// ----------------------------------------------------------------------------
// Offsets table init (= master::threatfeaturecalc).
//   threatoffsets[p][0..63] = cumulative target-count up to that 'from' square
//   threatoffsets[p][64]    = total reachable squares (= "squareoffset" final)
//   threatoffsets[p][65]    = cumulative piece-offset across all preceding p
// Same logic as master, indexing pieces in master's interleaved order.
// ----------------------------------------------------------------------------
struct ThreatOffsetTable {
    int t[12][66];
};

// Plain attacks_bb wrapper that matches master's pseudo_attacks for non-pawns
// + adds the pawn push square (the V2 key feature).
static inline Bitboard v2_attacks_for_index(int pcIdx, Square from) {
    // pcIdx: 0=wP,1=bP,2=wN,...
    int color = pcIdx & 1;
    int type  = pcIdx >> 1;   // 0=P, 1=N, 2=B, 3=R, 4=Q, 5=K
    if (type == 0) {           // PAWN: diag attacks + push forward
        Bitboard b = square_bb(from);
        Bitboard attacks =
          (color == int(WHITE)) ? pawn_attacks_bb<WHITE>(b) : pawn_attacks_bb<BLACK>(b);
        int push = (color == int(WHITE)) ? 8 : -8;
        int dest = int(from) + push;
        if (dest >= 0 && dest < 64)
            attacks |= square_bb(Square(dest));
        return attacks;
    }
    // map V2 type back to SF PieceType (1..6)
    PieceType pt = PieceType(type + 1);
    return PseudoAttacks[pt][from];
}

static ThreatOffsetTable build_offsets() {
    // ⚠️ ORDINE CRITICO: master itera `for (c=0..1) for (pt=0..5) piece=2*pt+c`,
    // accumulando pieceoffset in ordine wP,wN,wB,wR,wQ,wK,bP,bN,bB,bR,bQ,bK.
    // Iterare invece p=0..11 sequenziale (wP,bP,wN,bN,...) produce cumulative
    // OFFSET DIVERSI per le righe BLACK -> tutte le feature dei pezzi neri
    // finiscono in slot sbagliati = port BIT-WRONG (verificato sperimentalmente:
    // eval asimmetrico tra queen-up bianco e queen-down bianco).
    ThreatOffsetTable o{};
    int pieceoffset = 0;
    for (int c = 0; c < 2; ++c) {
        for (int pt = 0; pt < 6; ++pt) {
            int p = 2 * pt + c;
            o.t[p][65] = pieceoffset;
            int squareoffset = 0;
            for (int from = 0; from < 64; ++from) {
                o.t[p][from] = squareoffset;
                if (pt != 0) {                              // non-pawn (color-independent attacks)
                    Bitboard atk = v2_attacks_for_index(p, Square(from));
                    squareoffset += popcount(atk);
                } else if (from >= 8 && from <= 55) {       // pawn in ranks 2..7 only
                    Bitboard atk = v2_attacks_for_index(p, Square(from));
                    squareoffset += popcount(atk);
                }
            }
            o.t[p][64] = squareoffset;
            pieceoffset += v2_numvalidtargets[p] * squareoffset;
        }
    }
    return o;
}

static const ThreatOffsetTable& threatoffsets() {
    static const ThreatOffsetTable T = build_offsets();
    return T;
}

// ----------------------------------------------------------------------------
// threat_index (master::threat_index lines ~183-218).
// Returns -1 (= Dimensions) when the (attacker,attacked) combo is excluded.
// ----------------------------------------------------------------------------
IndexType FullThreatsV2::make_index(
  Color perspective, Piece attkr, Square from, Square to, Piece attkd, Square ksq) {
    const bool enemy = (color_of(attkr) != color_of(attkd));

    // Orient by king square + perspective. Master uses a 2D OrientTBL where the
    // BLACK row also rank-flips (a8 = 56 for files a..d, h8 = 63 for files e..h).
    // We replicate that via `^ (56 * perspective)` at use-time, same trick V1 SF18
    // uses, so the 1D OrientTBL above can stay just the file-flip pattern.
    int orient = int(OrientTBL[ksq]) ^ (56 * int(perspective));
    from = Square(int(from) ^ orient);
    to   = Square(int(to)   ^ orient);

    Piece attkr_o = attkr;
    Piece attkd_o = attkd;
    if (perspective == BLACK) {
        // Master flips piece COLOR bit (low-bit of interleaved id ^= 1). For SF
        // pieces that's piece ^ 8 (toggle color bit, same as ~pc).
        attkr_o = Piece(int(attkr) ^ 8);
        attkd_o = Piece(int(attkd) ^ 8);
    }
    const int  pAtkrIdx  = piece_to_v2idx(attkr_o);
    const int  pAtkdIdx  = piece_to_v2idx(attkd_o);
    const int  atkrType  = pAtkrIdx >> 1;        // 0=P..5=K
    const int  atkdType  = pAtkdIdx >> 1;
    const int  mapVal    = FullThreatsV2::map[atkrType][atkdType];
    if (mapVal < 0) return Dimensions;

    // Same-type filter: skip mirror-half when same type AND (enemy OR not pawn).
    if (atkrType == atkdType && (enemy || atkrType != 0) && from < to)
        return Dimensions;

    // attacks of attacker on 'from' (no occ; pseudoattacks +pawn push)
    Bitboard attacks = v2_attacks_for_index(pAtkrIdx, from);

    // ordinal index of 'to' among reachable squares (= popcount of mask BELOW to)
    const Bitboard belowTo = (to == 0) ? Bitboard(0) : ((Bitboard(1) << int(to)) - 1);
    const int      ordTo   = popcount(belowTo & attacks);

    const auto& o = threatoffsets();
    return IndexType(
        o.t[pAtkrIdx][65]
        + (int(color_of(attkd_o)) * (v2_numvalidtargets[pAtkrIdx] / 2) + mapVal)
            * o.t[pAtkrIdx][64]
        + o.t[pAtkrIdx][int(from)]
        + ordTo);
}

// ----------------------------------------------------------------------------
// append_active_indices (master::fill_features_sparse lines ~220-302).
// Visit by color (in master's "order[perspective]" sequence W-first / B-first),
// then by piece type. For PAWN: attacks_left, attacks_right, AND attacks_forward
// (push square that ENDS on a pawn — the V2 new feature).
// ----------------------------------------------------------------------------
void FullThreatsV2::append_active_indices(Color perspective, const Position& pos, IndexList& active) {
    const Square   ksq = pos.square<KING>(perspective);
    const Bitboard occupied = pos.pieces();

    // Master's iteration ORDER mirrors C++ master: i in {WHITE,BLACK}, then j in pieces.
    // `order[perspective][i]` gives perspective^i (W first for white perspective).
    for (int i = 0; i <= 1; ++i) {
        Color c = Color(int(perspective) ^ i);

        for (PieceType pt = PAWN; pt <= KING; ++pt) {
            Piece    attkr = make_piece(c, pt);
            Bitboard bb    = pos.pieces(c, pt);

            if (pt == PAWN) {
                // Diagonal captures + push onto a pawn.
                Bitboard atL = (c == WHITE ? shift<NORTH_EAST>(bb) : shift<SOUTH_WEST>(bb)) & occupied;
                Bitboard atR = (c == WHITE ? shift<NORTH_WEST>(bb) : shift<SOUTH_EAST>(bb)) & occupied;
                Bitboard pawnsBB = pos.pieces(WHITE, PAWN) | pos.pieces(BLACK, PAWN);
                Bitboard atF = (c == WHITE ? shift<NORTH>(bb) : shift<SOUTH>(bb)) & pawnsBB;

                while (atL) {
                    Square to    = pop_lsb(atL);
                    Square from  = Square(int(to) - (c == WHITE ? 9 : -9));
                    Piece  attkd = pos.piece_on(to);
                    IndexType idx = make_index(perspective, attkr, from, to, attkd, ksq);
                    if (idx < Dimensions) active.push_back(idx);
                }
                while (atR) {
                    Square to    = pop_lsb(atR);
                    Square from  = Square(int(to) - (c == WHITE ? 7 : -7));
                    Piece  attkd = pos.piece_on(to);
                    IndexType idx = make_index(perspective, attkr, from, to, attkd, ksq);
                    if (idx < Dimensions) active.push_back(idx);
                }
                while (atF) {
                    Square to    = pop_lsb(atF);
                    Square from  = Square(int(to) - (c == WHITE ? 8 : -8));
                    Piece  attkd = pos.piece_on(to);
                    IndexType idx = make_index(perspective, attkr, from, to, attkd, ksq);
                    if (idx < Dimensions) active.push_back(idx);
                }
            } else {
                Bitboard b = bb;
                while (b) {
                    Square   from    = pop_lsb(b);
                    Bitboard attacks = attacks_bb(pt, from, occupied) & occupied;
                    while (attacks) {
                        Square    to    = pop_lsb(attacks);
                        Piece     attkd = pos.piece_on(to);
                        IndexType idx   = make_index(perspective, attkr, from, to, attkd, ksq);
                        if (idx < Dimensions) active.push_back(idx);
                    }
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// append_changed_indices: same shape as V1, just routes through V2's make_index.
// Master's fused-update logic identical (the FusedUpdateData struct).
// ----------------------------------------------------------------------------
void FullThreatsV2::append_changed_indices(Color            perspective,
                                           Square           ksq,
                                           const DiffType&  diff,
                                           IndexList&       removed,
                                           IndexList&       added,
                                           FusedUpdateData* fusedData,
                                           bool             first) {
    for (const auto& dirty : diff.list)
    {
        auto attacker = dirty.pc();
        auto attacked = dirty.threatened_pc();
        auto from     = dirty.pc_sq();
        auto to       = dirty.threatened_sq();
        auto add      = dirty.add();

        if (fusedData)
        {
            if (from == fusedData->dp2removed)
            {
                if (add) {
                    if (first) { fusedData->dp2removedOriginBoard |= to; continue; }
                } else if (fusedData->dp2removedOriginBoard & to) continue;
            }
            if (to != SQ_NONE && to == fusedData->dp2removed)
            {
                if (add) {
                    if (first) { fusedData->dp2removedTargetBoard |= from; continue; }
                } else if (fusedData->dp2removedTargetBoard & from) continue;
            }
        }

        auto&           insert = add ? added : removed;
        const IndexType index  = make_index(perspective, attacker, from, to, attacked, ksq);
        if (index < Dimensions) insert.push_back(index);
    }
}

bool FullThreatsV2::requires_refresh(const DiffType& diff, Color perspective) {
    // Same rule as V1: mirror-cross of the king of `perspective` triggers a
    // refresh of THAT perspective.
    return perspective == diff.us && (int8_t(diff.ksq) & 0b100) != (int8_t(diff.prevKsq) & 0b100);
}

}  // namespace Stockfish::Eval::NNUE::Features

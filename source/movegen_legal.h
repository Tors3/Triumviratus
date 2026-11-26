#pragma once
// movegen_legal.h -- fully legal move generator, used by the search (wired in search/08_movegen.inc).
//
// Every move it writes is legal, so the search needs neither legal_before nor the king test of the make for these
// moves. Per position, legal_prepare finds the checkers and the pinned pieces from the king outward (enemy sliders on
// the king's empty-board lines: nothing in between is a check, exactly one of our pieces in between is a pin) and
// derives two target masks: where a non-king capture may land and where a non-king quiet move may land (everything
// when not in check, the checker and the blocking squares in single check, nothing in double check). Pinned pieces
// move only along the line through their king. King steps and castling squares are tested one by one against the
// enemy attacks with our king lifted off the board, and en passant gets the rank test of the discovered rook check.
//
// The moves come out in the order of td_generate_moves_side (legal_emit_tv): the search breaks score ties by
// generation order, so this generator searches exactly the same tree as the staged pseudo-legal one.
//
// Moves are encode_move words (movegen.h). Included from search/08_movegen.inc inside threads.cpp, after the engine
// tables it maps below (between_tbl and the empty-board attacks of 07_makemove.inc). Standard chess only: Chess960
// keeps the staged generator.
//
// Origin: ported by A Tin Man from his engine Zoomies (https://github.com/CodeAdopter/Zoomies, MIT License,
// Copyright (c) 2026 Angelo Wolff), whose generator descends from Surge (https://github.com/nkarve/surge, MIT License,
// Copyright (c) 2020 DiehardTheTryhard). Both are distributed under the MIT License:
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
//   to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or substantial portions
//   of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
#include "attacks.h"
#include "defs.h"
#include "magic.h"
// Vector emission of the targets (ListSink::emit_vec) on AVX-512 builds; -DTRIUMV_NO_MOVEGEN_VEC keeps the loops.
#if defined(__AVX512F__) && defined(__AVX512BW__) && !defined(TRIUMV_NO_MOVEGEN_VEC)
#include <immintrin.h>
#define LEGALGEN_VEC 1
#endif

namespace legalgen {

// ---- Engine tables and helpers ------------------------------------------------------------------------------------

#if defined(_MSC_VER) && !defined(__clang__)
inline int lsb(U64 b) {
    unsigned long i;
    _BitScanForward64(&i, b);
    return (int)i;
}
#else
inline int lsb(U64 b) { return __builtin_ctzll(b); }
#endif
inline U64 bit(int sq) { return 1ULL << sq; }

constexpr U64 NOT_FILE_A = 0xfefefefefefefefeULL;
constexpr U64 NOT_FILE_H = 0x7f7f7f7f7f7f7f7fULL;

template <int C>
inline U64 pawn_set_attacks(U64 pawns) {
    if constexpr (C == white)
        return ((pawns & NOT_FILE_A) >> 9) | ((pawns & NOT_FILE_H) >> 7);
    else
        return ((pawns & NOT_FILE_A) << 7) | ((pawns & NOT_FILE_H) << 9);
}

// With USE_AVX2 every slider lookup computes bishop and rook together (magic.cpp): queens and pairs take one call.
inline U64 rook_attacks(int sq, U64 occ) { return get_rook_attacks(sq, occ); }
inline U64 bishop_attacks(int sq, U64 occ) { return get_bishop_attacks(sq, occ); }
inline U64 queen_attacks(int sq, U64 occ) { return get_queen_attacks(sq, occ); }
struct SliderAttacks {
    U64 b, r;
};
inline SliderAttacks slider_pair(int sq, U64 occ) {
    const SliderPair p = get_slider_pair(sq, occ);
    return {pair_bishop(p), pair_rook(p)};
}

// Unqualified on purpose: in the universal build the engine's tables live in the variant's namespace.
static U64 (&between_bb)[64][64] = between_tbl;
static U64 (&rook_empty)[64]     = empty_rook_att;
static U64 (&bishop_empty)[64]   = empty_bishop_att;

// The whole line through two aligned squares (0 if not aligned), for pinned pieces. Filled at static initialization.
alignas(64) static U64 line_bb[64][64];
static const bool line_bb_ready = [] {
    for (int a = 0; a < 64; a++)
        for (int b = 0; b < 64; b++) {
            line_bb[a][b] = 0;
            const int ra = a / 8, fa = a % 8, rb = b / 8, fb = b % 8;
            if (a == b || !(ra == rb || fa == fb || ra - rb == fa - fb || ra - rb == fb - fa))
                continue;
            const int dr = (rb > ra) - (rb < ra), df = (fb > fa) - (fb < fa);
            U64 line = 1ULL << a;
            for (int r = ra + dr, f = fa + df; r >= 0 && r < 8 && f >= 0 && f < 8; r += dr, f += df)
                line |= 1ULL << (r * 8 + f);
            for (int r = ra - dr, f = fa - df; r >= 0 && r < 8 && f >= 0 && f < 8; r -= dr, f -= df)
                line |= 1ULL << (r * 8 + f);
            line_bb[a][b] = line;
        }
    return true;
}();

// ---- Move words and sinks -----------------------------------------------------------------------------------------

// Move word fields, identical to encode_move in movegen.h.
constexpr int MV_TO      = 6;
constexpr int MV_PIECE   = 12;
constexpr int MV_PROMO   = 16;
constexpr int MV_CAPTURE = 1 << 20;
constexpr int MV_DOUBLE  = 1 << 21;
constexpr int MV_EP      = 1 << 22;
constexpr int MV_CASTLE  = 1 << 23;

constexpr int encode(int from, int to, int piece, int promoted = 0, int flags = 0) {
    return from | (to << MV_TO) | (piece << MV_PIECE) | (promoted << MV_PROMO) | flags;
}

// What a sink is handed.
enum Kind { K_QUIET, K_DOUBLE, K_CASTLE, K_CAPTURE, K_EP, K_PROMO, K_PROMO_CAPTURE };

// ALL: every legal move. TACTICS / QUIETS: the stages of fill_tactics and fill_quiets. Tactics are captures, en
// passant, capture-promotions and the queen push-promotion; quiets are moves to empty squares, double pushes,
// castling and all four push-promotions. QUIETS_NQ (26/11/2026) is QUIETS without the queen push-promotion, which the
// tactics stage already holds: the list fill_quiets used to compact after the generation.
enum Stage { ALL, TACTICS, QUIETS, QUIETS_NQ };

// Writes move words into a list, keeping the kinds of its stage. Kinds it drops cost nothing: the call is removed
// at compile time.
// (26/11/2026, speed, same lists) `skip` is a move the list must not hold: the hash move, which the queue hands out
// before the stages. It is dropped where it would be written (its target bit leaves the targets of its piece, or its
// single slot counts 0), so the fills no longer read every list again to compact it. -1 (no such move) matches no
// move word: every word is below 2^24. Same lists, same order as the old compaction.
template <Stage S>
struct ListSink {
    int* out;
    int  n = 0;
    int  skip;
    explicit ListSink(int* o, int s = -1) : out(o), skip(s) {}

    template <Kind K>
    static constexpr bool keeps() {
        if constexpr (S == TACTICS)
            return K == K_CAPTURE || K == K_EP || K == K_PROMO_CAPTURE || K == K_PROMO;
        else if constexpr (S == QUIETS || S == QUIETS_NQ)
            return K == K_QUIET || K == K_DOUBLE || K == K_CASTLE || K == K_PROMO;
        else
            return true;
    }
    // One move per bit of `to`; base holds from, piece and flags. Captures are sparse (a piece has none 68% of the
    // time, one 25%, as counted for AA4): the first is written unconditionally and counted only if it exists (bit 63
    // stands in when there is none), so the loop exit is predictable; the rest go through the loop. The quiet targets
    // of a piece (dense: 4-15 per piece) go through the vector emission where there is one.
    template <Kind K>
    void targets(int base, U64 to) {
        if constexpr (keeps<K>()) {
            to = unskip(base, to, 0);
#if defined(LEGALGEN_VEC)
            if constexpr (K == K_QUIET) {
                emit_vec(base, to, 0);
                return;
            }
#endif
            if constexpr (K == K_CAPTURE && S == TACTICS) {
                out[n] = base | (lsb(to | (1ULL << 63)) << MV_TO);
                n += to != 0;
                to &= to - 1;
            }
            for (; to; to &= to - 1)
                out[n++] = base | (lsb(to) << MV_TO);
        }
    }
    // Four promotions, queen first; `queen` is the promoting side's queen (Q or q), the others follow it down.
    template <Kind K>
    void target_promos(int base, U64 to, int queen) {
        if constexpr (keeps<K>())
            for (; to; to &= to - 1)
                promos<K>(base | (lsb(to) << MV_TO), queen);
    }
    template <Kind K>
    void one(int move) {
        if constexpr (keeps<K>()) {
            out[n] = move;
            n += move != skip;
        }
    }
    // Targets in square order, quiet moves and captures interleaved, the capture flag taken from `enemies` (the order
    // of mg_emit_targets). A stage with only one of the two kinds takes its own subset.
    void mixed(int base, U64 to, U64 enemies) {
        constexpr bool kq = keeps<K_QUIET>(), kc = keeps<K_CAPTURE>();
        if constexpr (kq && kc) {
            to = unskip(base, to, enemies);
#if defined(LEGALGEN_VEC)
            emit_vec(base, to, enemies);
#else
            for (; to; to &= to - 1) {
                const int t = lsb(to);
                out[n++]    = base | (t << MV_TO) | (int((enemies >> t) & 1) << 20);
            }
#endif
        } else if constexpr (kc)
            targets<K_CAPTURE>(base | MV_CAPTURE, to & enemies);
        else if constexpr (kq)
            targets<K_QUIET>(base, to & ~enemies);
    }
    // One move written unconditionally and counted if `keep` (0 or 1): no branch on the data.
    template <Kind K>
    void slot(int move, U64 keep) {
        if constexpr (keeps<K>()) {
            out[n] = move;
            n += (int)keep & (move != skip);
        }
    }

private:
    // `to` without the target of the skipped move when the skipped move is one of base's: same origin, piece and
    // flags, with the capture flag of its target taken from `enemies` (0: the flag is in base). No branch.
    U64 unskip(int base, U64 to, U64 enemies) const {
        const int t   = (skip >> MV_TO) & 63;
        const int cap = int((enemies >> t) & 1) << 20;
        const U64 hit = (U64)((skip & ~(63 << MV_TO)) == (base | cap));
        return to & ~(hit << t);
    }
#if defined(LEGALGEN_VEC)
    // (26/11/2026, speed, same moves in the same order) Vector emission of a piece's targets, in square order, the
    // capture flag of each target from `cap`. The scalar loop wrote one move per bit and its exit, with a count that
    // changes at every piece, was a mispredicted branch: in the profiles of 26/11 (Zen 4 and Xeon) the quiet loop was
    // the heaviest function of the generation (Zen 4: 28 cycles per node, 1.1% of the mispredicted branches). The
    // whole-vector stores beyond the count stay inside the lists (256 slots, at most 218 legal moves, 32 lanes).
    void emit_vec(int base, U64 to, U64 cap) {
        const __m512i bv = _mm512_set1_epi32(base);
#if defined(__AVX512VBMI2__)
        // Ice Lake / Zen 4: the 64 squares as bytes (+64 where a capture lands), packed in one vpcompressb and widened
        // to 32-bit words 16 at a time: target = byte & 63 in bits 6-11, capture = bit 6 of the byte in bit 20.
        alignas(64) static const unsigned char kSq[64] = {
            0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
            22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43,
            44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63};
        const __m512i sq = _mm512_load_si512(kSq);
        const __m512i b  = _mm512_mask_add_epi8(sq, (__mmask64)cap, sq, _mm512_set1_epi8(64));
        const __m512i c  = _mm512_maskz_compress_epi8((__mmask64)to, b);
        const __m512i m63 = _mm512_set1_epi32(63), m64 = _mm512_set1_epi32(64);
        auto words = [&](__m128i x) {
            const __m512i w = _mm512_cvtepu8_epi32(x);
            return _mm512_or_si512(bv, _mm512_or_si512(_mm512_slli_epi32(_mm512_and_si512(w, m63), MV_TO),
                                                       _mm512_slli_epi32(_mm512_and_si512(w, m64), 14)));
        };
        const int cnt = (int)_mm_popcnt_u64(to);
        _mm512_storeu_si512(out + n, words(_mm512_castsi512_si128(c)));
        if (cnt > 16)   // a queen or a rook in an open position
            _mm512_storeu_si512(out + n + 16, words(_mm512_extracti32x4_epi32(c, 1)));
        n += cnt;
#else
        // AVX-512F (Skylake-SP): four fixed groups of 16 squares, each built in register (target per lane, capture
        // bit from `cap`) and packed with vpcompressd on its 16 bits of `to` (the emission of 05/10/2026,
        // mg_emit_vec in 08_movegen.inc, which the legal generator of 15/10 did not inherit).
        alignas(64) static const int kLane[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        const __m512i lane = _mm512_load_si512(kLane);
        const __m512i capv = _mm512_set1_epi32(MV_CAPTURE);
        for (int c = 0; c < 4; c++) {
            const __mmask16 tm  = (__mmask16)(to >> (16 * c));
            const __mmask16 em  = (__mmask16)(cap >> (16 * c));
            const __m512i   tgt = _mm512_slli_epi32(_mm512_add_epi32(lane, _mm512_set1_epi32(16 * c)), MV_TO);
            __m512i         mv  = _mm512_or_si512(bv, tgt);
            mv                  = _mm512_mask_or_epi32(mv, em, mv, capv);
            _mm512_storeu_si512(out + n, _mm512_maskz_compress_epi32(tm, mv));
            n += (int)_mm_popcnt_u32(tm);
        }
#endif
    }
#endif
    // The tactics stage takes only the queen of a push-promotion, QUIETS_NQ only the other three, QUIETS all four.
    template <Kind K>
    void promos(int m, int queen) {
        if constexpr (!(S == QUIETS_NQ && K == K_PROMO)) {
            const int q = m | (queen << MV_PROMO);
            out[n]      = q;
            n += q != skip;
        }
        if constexpr (S == TACTICS && K == K_PROMO)
            return;
        for (int d = 1; d <= 3; d++) {
            const int u = m | ((queen - d) << MV_PROMO);
            out[n]      = u;
            n += u != skip;
        }
    }
};

// ---- Preparation --------------------------------------------------------------------------------------------------

// Everything legal_emit_tv needs, computed once per position.
struct LegalInfo {
    U64 checkers;       // enemy pieces giving check
    U64 pinned;         // our pieces pinned to our king
    U64 capture_mask;   // squares a non-king capture may land on
    U64 quiet_mask;     // squares a non-king quiet move may land on
    int king;
    // The enemy's attacks per kind (P..K) with our king lifted, and their union (enemy_map). Built once per node by
    // its first user: castling and the quiet king steps of the generator, or the quiet ordering (score_quiets).
    bool emap;
    U64  em[6];
    U64  em_all;
};

// Per-side constants on the a8 = 0 board. Deltas are origin minus target.
template <int Us>
struct Side {
    static constexpr int Them  = Us ^ 1;
    static constexpr int UP    = Us == white ? 0 : 6;   // offset of our pieces
    static constexpr int TP    = 6 - UP;                // offset of theirs
    static constexpr U64 RANK7 = Us == white ? 0x000000000000ff00ULL : 0x00ff000000000000ULL;   // promotes next
    static constexpr U64 RANK3 = Us == white ? 0x0000ff0000000000ULL : 0x0000000000ff0000ULL;   // after one step
    static constexpr int D_UP  = Us == white ? 8 : -8;
    static constexpr int D_UL  = Us == white ? 9 : -7;   // capture toward the a-file
    static constexpr int D_UR  = Us == white ? 7 : -9;   // capture toward the h-file
    static U64 up(U64 b) { return Us == white ? b >> 8 : b << 8; }
    // Castling: squares that must be empty between king and rook.
    static constexpr U64 OO_EMPTY  = Us == white ? 0x6000000000000000ULL : 0x0000000000000060ULL;
    static constexpr U64 OOO_EMPTY = Us == white ? 0x0e00000000000000ULL : 0x000000000000000eULL;
    static constexpr int OO_RIGHT  = Us == white ? wk : bk;
    static constexpr int OOO_RIGHT = Us == white ? wq : bq;
    static constexpr int KING_FROM = Us == white ? e1 : e8;
    static constexpr int OO_TO     = Us == white ? g1 : g8;
    static constexpr int OOO_TO    = Us == white ? c1 : c8;
};

template <int Us, class Pos>
inline void legal_prepare(const Pos& pos, LegalInfo& info) {
    using S = Side<Us>;
    const U64 us_bb = pos.occupancies[Us], them_bb = pos.occupancies[S::Them], all = us_bb | them_bb;
    const int ksq          = lsb(pos.bitboards[K + S::UP]);
    const U64 their_pawns  = pos.bitboards[P + S::TP];
    const U64 their_knight = pos.bitboards[N + S::TP];
    const U64 their_diag   = pos.bitboards[B + S::TP] | pos.bitboards[Q + S::TP];
    const U64 their_orth   = pos.bitboards[R + S::TP] | pos.bitboards[Q + S::TP];

    U64 checkers = (knight_attacks[ksq] & their_knight) | (pawn_attacks[Us][ksq] & their_pawns);
    U64 pinned   = 0;
    // Snipers: enemy sliders on our king's lines of an empty board (a table read, no slider lookup), as safety_init
    // does. Nothing in between is a check; exactly one piece in between, ours, is a pin.
    for (U64 snipers = (rook_empty[ksq] & their_orth) | (bishop_empty[ksq] & their_diag); snipers;
         snipers &= snipers - 1) {
        const int s       = lsb(snipers);
        const U64 between = between_bb[ksq][s] & all;
        if (!between)
            checkers |= bit(s);
        else if (!(between & (between - 1)) && (between & us_bb))
            pinned |= between;
    }

    info.checkers = checkers;
    info.pinned   = pinned;
    info.king     = ksq;
    info.emap     = false;
    if (!checkers) {
        info.capture_mask = them_bb;
        info.quiet_mask   = ~all;
    } else if (checkers & (checkers - 1)) {
        info.capture_mask = info.quiet_mask = 0;
    } else {
        info.capture_mask = checkers;
        info.quiet_mask   = (checkers & (their_diag | their_orth)) ? between_bb[ksq][lsb(checkers)] : 0;
    }
}

// Is `sq` attacked by side Them, with occupancy `occ`? For king steps and castling squares.
template <int Them, class Pos>
inline bool attacked_by(const Pos& pos, int sq, U64 occ) {
    constexpr int TP = Them == white ? 0 : 6;
    if ((pawn_attacks[Them ^ 1][sq] & pos.bitboards[P + TP]) || (knight_attacks[sq] & pos.bitboards[N + TP])
        || (king_attacks[sq] & pos.bitboards[K + TP]))
        return true;
    const SliderAttacks a = slider_pair(sq, occ);
    return (a.b & (pos.bitboards[B + TP] | pos.bitboards[Q + TP])) || (a.r & (pos.bitboards[R + TP] | pos.bitboards[Q + TP]));
}

// The enemy attack map of a node (side Us to move; info.king set): every enemy piece's attacks with our king lifted,
// per kind, one slider pair per enemy slider in square order (the lookups score_quiets made before). Without a check
// no enemy slider sees our king, so lifting it changes nothing; in check it gives the squares the king may not step to.
// (29/10/2026 night, speed, same moves and scores) Before, every king step and castling square was tested on its own,
// and score_quiets computed the same slider attacks again: one map per node instead, -0.4% cycles per node (Xeon,
// avx512 and avx2, deterministic PGO, xperf).
template <int Us, class Pos>
inline void enemy_map(const Pos& pos, LegalInfo& info) {
    using S = Side<Us>;
    const U64 occ = (pos.occupancies[Us] | pos.occupancies[S::Them]) ^ bit(info.king);
    const U64 eB = pos.bitboards[B + S::TP], eR = pos.bitboards[R + S::TP], eQ = pos.bitboards[Q + S::TP];
    U64 kn = 0;
    for (U64 t = pos.bitboards[N + S::TP]; t; t &= t - 1)
        kn |= knight_attacks[lsb(t)];
    U64 bb = 0, rr = 0, qq = 0;
    for (U64 t = eB | eR | eQ; t; t &= t - 1) {
        const int s = lsb(t);
        const SliderAttacks a = slider_pair(s, occ);
        const U64 mB = 0 - ((eB >> s) & 1), mR = 0 - ((eR >> s) & 1), mQ = 0 - ((eQ >> s) & 1);
        bb |= a.b & mB;
        rr |= a.r & mR;
        qq |= (a.b | a.r) & mQ;
    }
    info.em[0]  = pawn_set_attacks<S::Them>(pos.bitboards[P + S::TP]);
    info.em[1]  = kn;
    info.em[2]  = bb;
    info.em[3]  = rr;
    info.em[4]  = qq;
    info.em[5]  = king_attacks[lsb(pos.bitboards[K + S::TP])];
    info.em_all = info.em[0] | kn | bb | rr | qq | info.em[5];
    info.emap   = true;
}

// ---- Emission -----------------------------------------------------------------------------------------------------

// The order of td_generate_moves_side (08_movegen.inc): pawns (white: the promoting pawns first, then the others;
// black: the others first), knights, bishops, rooks, queens, castling, king steps; pieces in square order, targets in
// square order with quiet moves and captures interleaved; each non-promoting pawn writes push, double push, capture
// toward the a-file, capture toward the h-file, en passant.
template <int Us, bool HasPins, class Pos, class Sink>
inline void legal_emit_tv_pins(const Pos& pos, LegalInfo& info, Sink& sink) {
    using S = Side<Us>;
    const U64 us_bb = pos.occupancies[Us], them_bb = pos.occupancies[S::Them], all = us_bb | them_bb;
    const int ksq      = info.king;
    const U64 pinned   = info.pinned;
    const U64 checkers = info.checkers;
    const U64 cm = info.capture_mask, qm = info.quiet_mask, reach = cm | qm;
    constexpr int PAWN = P + S::UP, KNIGHT = N + S::UP, BISHOP = B + S::UP, ROOK = R + S::UP, QUEEN = Q + S::UP,
                  KING = K + S::UP;
    // The squares a piece on s may reach: its pin line if pinned, the whole board otherwise (no branch). Without pins
    // the line table is not read.
    auto allowed = [&](int s) -> U64 {
        if constexpr (HasPins)
            return line_bb[ksq][s] | (((pinned >> s) & 1) - 1ULL);
        else
            return ~0ULL;
    };

    if (!(checkers & (checkers - 1))) {   // in double check only the king moves
        const int ep = pos.enpassant;
        // Our pawns that may take en passant. Not in check: those that do not open a rank to our king, and the pinned
        // one on the line through the en passant square. In check: the unpinned ones when the checker is the pawn that
        // has just made its double step (no other check can be answered en passant).
        U64 ep_ok = 0;
        if (Sink::template keeps<K_EP>() && ep != no_sq) {
            const U64 takers = pawn_attacks[S::Them][ep] & pos.bitboards[PAWN];
            const int cap    = ep + S::D_UP;
            if (!checkers) {
                for (U64 b = takers & ~pinned; b; b &= b - 1) {
                    const int s = lsb(b);
                    if ((ksq >> 3) != (cap >> 3)
                        || !(rook_attacks(ksq, all ^ bit(s) ^ bit(cap)) & 0xffULL << (ksq & 56)
                             & (pos.bitboards[R + S::TP] | pos.bitboards[Q + S::TP])))
                        ep_ok |= bit(s);
                }
                if constexpr (HasPins)
                    ep_ok |= takers & pinned & line_bb[ep][ksq];
            } else if (checkers == bit(cap) && (checkers & pos.bitboards[P + S::TP]))
                ep_ok = takers & ~pinned;
        }

        auto promoting_pawns = [&] {
            for (U64 b = pos.bitboards[PAWN] & S::RANK7; b; b &= b - 1) {
                const int s  = lsb(b);
                const U64 al = allowed(s);
                sink.template target_promos<K_PROMO>(encode(s, 0, PAWN), S::up(bit(s)) & qm & al, QUEEN);
                sink.template target_promos<K_PROMO_CAPTURE>(encode(s, 0, PAWN, 0, MV_CAPTURE),
                                                             pawn_attacks[Us][s] & cm & al, QUEEN);
            }
        };
        auto other_pawns = [&] {
            constexpr bool kq = Sink::template keeps<K_QUIET>(), kd = Sink::template keeps<K_DOUBLE>(),
                           kc = Sink::template keeps<K_CAPTURE>();
            const U64 rest      = pos.bitboards[PAWN] & ~S::RANK7;
            const U64 push1     = (kq || kd) ? S::up(rest) & ~all : 0;
            const U64 single_to = kq ? push1 & qm : 0;
            const U64 double_to = kd ? S::up(push1 & S::RANK3) & ~all & qm : 0;
            const U64 cap_mask  = kc ? cm : 0;
            // Pawns with at least one move: back-shifts of the targets (any superset gives the same moves).
            const U64 down1 = Us == white ? single_to << 8 : single_to >> 8;
            const U64 down2 = Us == white ? double_to << 16 : double_to >> 16;
            for (U64 b = rest & (down1 | down2 | pawn_set_attacks<S::Them>(cap_mask) | ep_ok); b; b &= b - 1) {
                const int s    = lsb(b);
                const U64 al   = allowed(s);
                const U64 caps = pawn_attacks[Us][s] & cap_mask & al;
                sink.template slot<K_QUIET>(encode(s, s - S::D_UP, PAWN), ((single_to & al) >> (s - S::D_UP)) & 1);
                sink.template slot<K_DOUBLE>(encode(s, s - 2 * S::D_UP, PAWN, 0, MV_DOUBLE),
                                             ((double_to & al) >> (s - 2 * S::D_UP)) & 1);
                sink.template slot<K_CAPTURE>(encode(s, s - S::D_UL, PAWN, 0, MV_CAPTURE), (caps >> (s - S::D_UL)) & 1);
                sink.template slot<K_CAPTURE>(encode(s, s - S::D_UR, PAWN, 0, MV_CAPTURE), (caps >> (s - S::D_UR)) & 1);
                sink.template slot<K_EP>(encode(s, ep, PAWN, 0, MV_CAPTURE | MV_EP), (ep_ok >> s) & 1);
            }
        };
        if constexpr (Us == white) {
            promoting_pawns();
            other_pawns();
        } else {
            other_pawns();
            promoting_pawns();
        }

        for (U64 b = HasPins ? pos.bitboards[KNIGHT] & ~pinned : pos.bitboards[KNIGHT]; b; b &= b - 1) {
            const int s = lsb(b);
            sink.mixed(encode(s, 0, KNIGHT), knight_attacks[s] & reach, them_bb);
        }
        for (U64 b = pos.bitboards[BISHOP]; b; b &= b - 1) {
            const int s = lsb(b);
            sink.mixed(encode(s, 0, BISHOP), bishop_attacks(s, all) & reach & allowed(s), them_bb);
        }
        for (U64 b = pos.bitboards[ROOK]; b; b &= b - 1) {
            const int s = lsb(b);
            sink.mixed(encode(s, 0, ROOK), rook_attacks(s, all) & reach & allowed(s), them_bb);
        }
        for (U64 b = pos.bitboards[QUEEN]; b; b &= b - 1) {
            const int s = lsb(b);
            sink.mixed(encode(s, 0, QUEEN), queen_attacks(s, all) & reach & allowed(s), them_bb);
        }
    }

    // Castling (emitted before the king steps): not in check, the squares between king and rook empty, and the two
    // squares the king crosses not attacked, read from the node's enemy map (built here when castling is possible; the
    // king steps below and the quiet ordering read it too). Not in check, lifting our king changes no attack.
    if constexpr (Sink::template keeps<K_CASTLE>())
        if (!checkers) {
            constexpr int OO_PASS = Us == white ? f1 : f8, OOO_PASS = Us == white ? d1 : d8;
            auto hit = [&](int sq) {
                if (!info.emap)
                    enemy_map<Us>(pos, info);
                return ((info.em_all >> sq) & 1) != 0;
            };
            if ((pos.castle & S::OO_RIGHT) && !(all & S::OO_EMPTY) && !hit(OO_PASS) && !hit(S::OO_TO))
                sink.template one<K_CASTLE>(encode(S::KING_FROM, S::OO_TO, KING, 0, MV_CASTLE));
            if ((pos.castle & S::OOO_RIGHT) && !(all & S::OOO_EMPTY) && !hit(OOO_PASS) && !hit(S::OOO_TO))
                sink.template one<K_CASTLE>(encode(S::KING_FROM, S::OOO_TO, KING, 0, MV_CASTLE));
        }
    // King steps, with our king lifted off the board. The stages with quiet steps (quiet, all) read the node's enemy map
    // for every target; the tactics stage, with few captures by the king and often no quiet stage after it, tests each
    // target on its own (the cheap attackers answer first) unless the map is already there.
    {
        constexpr bool kq = Sink::template keeps<K_QUIET>(), kc = Sink::template keeps<K_CAPTURE>();
        const U64 occ  = all ^ bit(ksq);
        U64       kt   = king_attacks[ksq] & ~us_bb & (kq ? ~0ULL : them_bb) & (kc ? ~0ULL : ~them_bb);
        U64       safe = 0;
        if (info.emap || (kq && kt)) {
            if (!info.emap)
                enemy_map<Us>(pos, info);
            safe = kt & ~info.em_all;
        } else
            for (; kt; kt &= kt - 1)
                if (!attacked_by<S::Them>(pos, lsb(kt), occ))
                    safe |= bit(lsb(kt));
        sink.mixed(encode(ksq, 0, KING), safe, them_bb);
    }
}

template <int Us, class Pos, class Sink>
inline void legal_emit_tv(const Pos& pos, LegalInfo& info, Sink& sink) {
    if (info.pinned)
        legal_emit_tv_pins<Us, true>(pos, info, sink);
    else
        legal_emit_tv_pins<Us, false>(pos, info, sink);
}

// ---- Entry points -------------------------------------------------------------------------------------------------

template <int Us, Stage St = ALL, class Pos>
inline int generate_legal_tv_for(const Pos& pos, int* out) {
    LegalInfo info;
    legal_prepare<Us>(pos, info);
    ListSink<St> sink(out);
    legal_emit_tv<Us>(pos, info, sink);
    return sink.n;
}

// Side to move read at run time.
template <Stage St = ALL, class Pos>
inline int generate_legal_tv(const Pos& pos, int* out) {
    return pos.side == white ? generate_legal_tv_for<white, St>(pos, out) : generate_legal_tv_for<black, St>(pos, out);
}

}  // namespace legalgen

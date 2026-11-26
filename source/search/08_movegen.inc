// search/08_movegen.inc -- Per-thread move generation.
// Part of threads.cpp: it is NOT a translation unit of its own. It must be included only by threads.cpp, in the order
// fixed there (split of 27/09/2026, identical generated code: see the header of threads.cpp).
// ============================================================================
// MOVE GENERATION
// ============================================================================

// between_tbl (squares strictly between two aligned squares) is defined in 07_makemove.inc, which uses it for
// discovered checks; filled by init_cycle_tables (10_order.inc), used here too by the evasions (P2.3).

// Bitboard of the attackers of `sq` of colour `by` (P2.3 EvasionGen).
static inline U64 td_attackers_to(ThreadData &td, int sq, int by) {
  const int off = (by == white) ? 0 : 6;
  const SliderPair sp = get_slider_pair(sq, td.occupancies[both]);   // AA1 (10/10/2026): one computation for both
  return (pawn_attacks[by ^ 1][sq] & td.bitboards[P + off]) |
         (knight_attacks[sq] & td.bitboards[N + off]) |
         (king_attacks[sq] & td.bitboards[K + off]) |
         (pair_bishop(sp) &
          (td.bitboards[B + off] | td.bitboards[Q + off])) |
         (pair_rook(sp) &
          (td.bitboards[R + off] | td.bitboards[Q + off]));
}

// 🔴 quiets_only (2A, 29/07): the move picker called this function with captures_only=false
// in the QUIET stage, i.e. generating ALL pseudo-legal moves - including the captures it had
// already generated in the TACTICAL stage - only to mark them MP_CONSUMED and never return them
// (move picker, search/10_picker.inc). Every node that got past the good tacticals without a cutoff paid the
// empty generation of ~7 captures: pop_lsb, encode, store in the list, and a get_move_capture
// downstream. With quiets_only only the moves landing on EMPTY squares are generated.
//
// NODE-IDENTICAL, and it can be proved: the captures were already CONSUMED (never returned), the relative order
// of the quiet moves does not change, and the selection sort at :5322 breaks ties by the FIRST index - so
// removing them from the list changes neither the set nor the order of the moves returned.
// => the bench MUST stay identical: if it changes by one node the patch is wrong.
//
// ⚠️ Three things do NOT go through allowed_squares and must be excluded by hand:
//   - pawn captures (they use `& enemies` directly)
//   - EN PASSANT: it is a capture landing on an EMPTY square, so it would pass the filter
//   - (castling and quiet promotions are fine on their own: they land on empty squares)
//
// 🔴 known_in_check (2C, 29/07): -1 = unknown (computed here, as before), 0/1 = the caller
// already knows it. The evasion gate asked `td_is_square_attacked(ksq, ~side)` at EVERY call, and
// in the common case - not in check - that test has NO early exit: it must try pawns,
// knights, king, and two magic lookups (bishop and rook, two possible cache misses on big
// tables) just to answer "no". And it was paid THREE times per node: once in the search, once for
// the tactical stage of the move picker and once for the quiet stage.
// ⚠️ Passing a WRONG value changes the moves generated in check: the gate is the bench.
// promo_quiet: also generates the QUIET promotions when captures_only is true.
// 🔴 It serves ONLY the qsearch. The MovePicker must NOT pass it: its quiet stage
// (MPS_GEN_QUIET) already generates them, and enabling it on the tactical stage too would
// DUPLICATE them in the main search. Default false = all callers unchanged.
#define MG_ADD(m) (mg_out[mg_cnt++] = (m))

#if defined(__AVX512F__)
#include <immintrin.h>
// 05/10/2026 - VECTOR EMISSION of the moves of a piece (AVX-512F). The `while (attacks)` loop writes one move
// per bit and its exit, with a number of rounds that changes at every piece (0-27), is a mispredicted branch: ~9 pieces
// per generation and two generations per node when the quiet moves are reached. Here the 64 squares are four FIXED groups
// of 16: for each group the vector of the 16 moves is built (origin and piece in common, target square per
// lane, capture bit taken from the enemy mask) and compacted with vpcompressd on the 16 bits of `attacks`
// of the group. Same LSB order as the loop, same moves. The 16-slot writes beyond the count stay inside
// the list (256 slots for at most 218 pseudo-legal moves). -DTRIUMV_NO_MOVEGEN_VEC goes back to the loop.
// xperf 6 rounds, PGO avx512 build: cycles per node -1.51%, mispredicted branches -3.4%. On AVX2 (compaction with
// vpermd) +0.29%: the avx2 build stays on the loop.
static inline int mg_emit_vec(int *mg_out, int mg_cnt, U64 attacks, U64 enemies, int base) {
  alignas(64) static const int kLane[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const __m512i lane  = _mm512_load_si512(kLane);
  const __m512i basev = _mm512_set1_epi32(base);
  const __m512i cap   = _mm512_set1_epi32(1 << 20);   // capture bit of encode_move
  for (int c = 0; c < 4; c++) {   // four fixed groups: the compiler unrolls them
    const __mmask16 tm  = (__mmask16)(attacks >> (16 * c));
    const __mmask16 em  = (__mmask16)(enemies >> (16 * c));
    const __m512i   tgt = _mm512_slli_epi32(_mm512_add_epi32(lane, _mm512_set1_epi32(16 * c)), 6);
    __m512i mv = _mm512_or_si512(basev, tgt);
    mv         = _mm512_mask_or_epi32(mv, em, mv, cap);
    _mm512_storeu_si512(mg_out + mg_cnt, _mm512_maskz_compress_epi32(tm, mv));
    mg_cnt += count_bits(tm);
  }
  return mg_cnt;
}
#endif

// One move for every bit of `attacks`, in LSB order, with the capture bit as data (no branch on the data). The
// vector path is used when NOT generating captures only: with the full targets the loop does 4-15 rounds per
// piece, with captures only 0-1 and the fixed cost of the four groups does not pay off.
static inline int mg_emit_targets(int *mg_out, int mg_cnt, U64 attacks, U64 enemies, int source_square, int piece,
                                  bool captures_only) {
#if defined(__AVX512F__) && !defined(TRIUMV_NO_MOVEGEN_VEC)
  if (!captures_only)
    return mg_emit_vec(mg_out, mg_cnt, attacks, enemies, source_square | (piece << 12));
#else
  (void)captures_only;
#endif
  // AA4 (10/10/2026, speed, same moves in the same order): with captures only a piece has 0 (68%), 1 (25%),
  // 2 (6%) or more (2%) (counters, bench 15-16 on fens30/eg30, ~1.1 pieces per node): the exit of the loop depended on
  // that number, and a 2-bit predictor per site gets it wrong ~0.53-0.55 times per node. The first capture is always
  // written in the current slot and the counter advances by 0 or 1 (like the pawns, 04/10/2026); without captures the slot stays
  // outside the count (square 63 is only a legal value for the index). The loop stays for the second one and beyond.
  if (captures_only) {
    const int first = get_ls1b_index(attacks | (1ULL << 63));
    mg_out[mg_cnt] = encode_move(source_square, first, piece, 0, (int)((enemies >> first) & 1), 0, 0, 0);
    mg_cnt += attacks != 0;
    attacks &= attacks - 1;
  }
  while (attacks) {
    const int target_square = get_ls1b_index(attacks);
    mg_out[mg_cnt++] = encode_move(source_square, target_square, piece, 0,
                                   (int)((enemies >> target_square) & 1), 0, 0, 0);
    pop_lsb_bb(attacks);   // 2D: target_square IS the LSB of attacks
  }
  return mg_cnt;
}
// NPS 25/09/2026 - generator TEMPLATED on the colour. The xperf profile (samples on
// BranchMispredictions) gave td_generate_moves ~2.3 mispredicts/node against ~0.5 for the
// SF movegen, concentrated on the loop over piece types: with the colour at runtime every
// iteration redid the tests `piece == P`, `(td.side == white) ? piece == N : ...`.
// With MG_SIDE constant the loop unrolls and those tests disappear. Same generation
// order => identical nodes. The wrapper td_generate_moves() below keeps the signature.
template <int MG_SIDE>
static void td_generate_moves_side(ThreadData &td, moves *move_list, bool captures_only,
                                   bool quiets_only, int known_in_check, bool promo_quiet,
                                   bool promo_queen_only,
                              // Mode 7: besides the queen it also generates the KNIGHT. Rook and
                              // bishop stay out because they are strictly DOMINATED
                              // by the queen (it does everything they do); the only exception
                              // is avoiding stalemate, which is a matter for the full search in
                              // the endgame, not for the qsearch. Not the knight: it attacks squares the
                              // queen does not attack, so there are positions in which the
                              // knight promotion with check (king+queen fork) is
                              // the ONLY winning move, and today the qsearch does not see it.
                                   bool promo_knight) {
  PROF_GUARD(prof_mg);
#ifdef TRIUMV_PROFILE
  prof_n_mg++;   // SF: 0.52 calls/node, 156 cycles each (kiwipete)
#endif
  // 🔴 LOCAL COUNTER (09/09/2026). Before, every move went through
  // `add_move(move_list, m)` = `move_list->moves[move_list->count++] = m`, and
  // `count` lives IN THE SAME STRUCT as the array: the compiler cannot
  // prove that `moves[i]` does not alias `count` (it does not know that i < 256), so
  // it reloads the counter at every move. Three memory accesses instead of one.
  // Here the counter sits in a register and is stored ONCE, at the end.
  // Node-identical: same moves, same order.
  int  mg_cnt = 0;
  int *const mg_out = move_list->moves;
  int source_square, target_square;
  U64 bitboard, attacks;

  // --- BITWISE MAGIC: we precompute the masks ---
  U64 enemies =
      (MG_SIDE == white) ? td.occupancies[black] : td.occupancies[white];
  U64 friends =
      (MG_SIDE == white) ? td.occupancies[white] : td.occupancies[black];
  // If we want captures only, the only valid squares are the enemy ones.
  // Otherwise, all squares except ours.
  U64 allowed_squares = captures_only ? enemies
                      : quiets_only   ? ~td.occupancies[both]   // EMPTY squares only
                                      : ~friends;

  // P2.3 EvasionGen: in check the only pseudo-legal moves that CAN be
  // legal are: king moves, captures of the checker, blocks on the ray
  // (single check; double check: the king only). We restrict the masks HERE:
  // the order of the surviving moves stays IDENTICAL and the discarded ones would
  // have been rejected anyway by the make filter (king still in check) =>
  // node-identical. The blocking squares are EMPTY by definition (an enemy
  // piece on the ray = no check), so checker|between works both
  // for captures and for blocks. E.p. and castling: unchanged (e.p. can
  // resolve the check in special ways -> the make filters it; castling in
  // check is already self-excluded by the e1/e8-attacked test).
  U64 evasion_mask = ~0ULL;
  if (g_evasion_gen) {
    const int ksq =
        get_ls1b_index((MG_SIDE == white) ? td.bitboards[K] : td.bitboards[k]);
    // 2C: if the caller already knows it, all the attacker types are not scanned again.
    const bool in_chk = (known_in_check >= 0)
                            ? (known_in_check != 0)
                            : td_is_square_attacked(td, ksq, (MG_SIDE ^ 1));
    if (in_chk) {
      U64 checkers = td_attackers_to(td, ksq, (MG_SIDE ^ 1));
      if (checkers & (checkers - 1)) {
        evasion_mask = 0ULL; // double check: the king only
      } else {
        const int csq = get_ls1b_index(checkers);
        evasion_mask = checkers | between_tbl[csq][ksq];
      }
    }
  }
  // Mask for the NON-king pieces (the king keeps allowed_squares full).
  const U64 piece_allowed = allowed_squares & evasion_mask;

  // 🔴 ONLY the 6 piece types of the side to move (09/09/2026). The loop went over
  // all 12, including the six enemy ones: those iterations generate nothing
  // (every inner branch compares with the piece code of the side to move) but still pay
  // ~8 comparisons each. Inheritance of the BBC structure.
  // P..K = 0..5, p..k = 6..11, so the block of the side is contiguous.
  constexpr int mg_first = (MG_SIDE == white) ? P : p;
#if defined(__clang__)
#pragma clang loop unroll(full)
#elif defined(__GNUC__)
#pragma GCC unroll 6
#endif
  for (int piece = mg_first; piece <= mg_first + 5; piece++) {
    bitboard = td.bitboards[piece];

    if (MG_SIDE == white) {
      if (piece == P) {
        // Night of 04/10/2026 - PAWNS WITHOUT BRANCHES ON THE DATA, same order as before (identical tree). The per-pawn
        // loop made 4-5 data-dependent branches per pawn (free square, rank, block mask, double
        // push, 0-2 captures): the xperf profile gave the generation ~1.7 mispredicted branches per node. Now the
        // pushes are computed in bulk (bitboard shift, empty and block masks) and for every pawn the move is
        // always WRITTEN in the slot and the counter advanced only if it is valid (0 or 1): no branch. The order
        // stays that of the loop: per pawn (LSB), push, double push, capture to the left, to the right, e.p.
        // The pawns on the 7th (promotions) keep the old code: they are rare and come first in LSB order
        // (squares 8..15), so they are done before the others.
        U64 promo = bitboard & 0x000000000000FF00ULL;
        while (promo) {
          source_square = get_ls1b_index(promo);
          target_square = source_square - 8;

          // Quiet pawn moves: blocked if captures_only is true
          // (P2.3: every target must be in evasion_mask - in check
          //  only the blocks on the ray survive, out of check ~0ULL no-op)
          // 🔴 PromoQS EXCEPTION: the quiet promotion is TACTICAL for the whole
          // rest of the engine (score 750,000 in td_score_move, exempt from LMP,
          // futility, history prune and SEE) but it is born ONLY in here. With
          // captures_only the qsearch never saw it. The inner branch already separates
          // the 7th (promotions) from the rest (normal pushes): entering only with
          // the pawn on the 7th, the else is unreachable and no quiet moves are generated.
          if (!captures_only ||
              (promo_quiet && source_square >= a7 && source_square <= h7)) {
            if (!(target_square < a8) &&
                !get_bit(td.occupancies[both], target_square)) {
              if (source_square >= a7 && source_square <= h7) {
                if ((evasion_mask >> target_square) & 1) {
                  MG_ADD( encode_move(source_square, target_square,
                                                  piece, Q, 0, 0, 0, 0));
                  if (!promo_queen_only) {
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, R, 0, 0, 0, 0));
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, B, 0, 0, 0, 0));
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, N, 0, 0, 0, 0));
                  } else if (promo_knight) {
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, N, 0, 0, 0, 0));
                  }
                }
              } else {
                if ((evasion_mask >> target_square) & 1)
                  MG_ADD( encode_move(source_square, target_square,
                                                  piece, 0, 0, 0, 0, 0));
                if ((source_square >= a2 && source_square <= h2) &&
                    !get_bit(td.occupancies[both], target_square - 8) &&
                    ((evasion_mask >> (target_square - 8)) & 1))
                  MG_ADD(
                           encode_move(source_square, target_square - 8, piece,
                                       0, 0, 1, 0, 0));
              }
            }
          }

          // Pawn captures
          attacks = quiets_only ? 0ULL   // 2A: pawn captures do not go through allowed_squares
                  : (pawn_attacks[MG_SIDE][source_square] & enemies & evasion_mask);
          while (attacks) {
            target_square = get_ls1b_index(attacks);
            if (source_square >= a7 && source_square <= h7) {
              MG_ADD( encode_move(source_square, target_square,
                                              piece, Q, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, R, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, B, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, N, 1, 0, 0, 0));
            } else {
              MG_ADD( encode_move(source_square, target_square,
                                              piece, 0, 1, 0, 0, 0));
            }
            pop_lsb_bb(attacks);   // 2D: target_square IS the LSB of attacks
          }

          // En passant (always a capture, always generated)
          if (!quiets_only && td.enpassant != no_sq) {   // 2A: e.p. lands on an EMPTY square
            U64 enpassant_attacks =
                pawn_attacks[MG_SIDE][source_square] & (1ULL << td.enpassant);
            if (enpassant_attacks) {
              int target_enpassant = get_ls1b_index(enpassant_attacks);
              MG_ADD( encode_move(source_square, target_enpassant,
                                              piece, 0, 1, 0, 1, 0));
            }
          }
          pop_lsb_bb(promo);
        }
        {
          const U64 rest      = bitboard & ~0x000000000000FF00ULL;
          const U64 empty     = ~td.occupancies[both];
          const U64 push1     = captures_only ? 0ULL : (rest >> 8) & empty;           // square in front free
          const U64 single_to = push1 & evasion_mask;
          // Double push: from the 2nd rank (single pushes arriving on the 3rd = squares 40..47), with the 4th free and in the block mask.
          const U64 double_to = ((push1 & 0x0000FF0000000000ULL) >> 8) & empty & evasion_mask;
          const U64 cap_mask  = quiets_only ? 0ULL : enemies & evasion_mask;
          const U64 ep_bb     = (!quiets_only && td.enpassant != no_sq) ? (1ULL << td.enpassant) : 0ULL;
          const int ep_sq     = td.enpassant;   // if absent the written move is not counted
          // 06/10/2026 (speed, identical tree): only the pawns that have at least one move are scanned. A pawn
          // without push, double push and capture/e.p. wrote five slots without counting any: with
          // captures only (quiescence and tactical stage, ~1.4 pawns per node) almost all were like that. The pawns that
          // capture on `tgt` are the squares attacked by a BLACK pawn placed on `tgt` (same masks as
          // mask_pawn_attacks). Same set of counted moves, same order (LSB): identical nodes.
          const U64 tgt       = cap_mask | ep_bb;
          U64 bb = rest & ((single_to << 8) | (double_to << 16) | ((tgt << 7) & not_h_file) | ((tgt << 9) & not_a_file));
          while (bb) {
            const int s  = get_ls1b_index(bb);
            const U64 pa = pawn_attacks[white][s];
            const U64 caps = pa & cap_mask;
            mg_out[mg_cnt] = encode_move(s, s - 8, piece, 0, 0, 0, 0, 0);
            mg_cnt += (int)((single_to >> (s - 8)) & 1);
            mg_out[mg_cnt] = encode_move(s, s - 16, piece, 0, 0, 1, 0, 0);
            mg_cnt += (int)((double_to >> (s - 16)) & 1);
            mg_out[mg_cnt] = encode_move(s, s - 9, piece, 0, 1, 0, 0, 0);
            mg_cnt += (int)((caps >> (s - 9)) & 1);
            mg_out[mg_cnt] = encode_move(s, s - 7, piece, 0, 1, 0, 0, 0);
            mg_cnt += (int)((caps >> (s - 7)) & 1);
            mg_out[mg_cnt] = encode_move(s, ep_sq, piece, 0, 1, 0, 1, 0);
            mg_cnt += (int)((pa & ep_bb) != 0);
            pop_lsb_bb(bb);
          }
        }
      }
      if (piece == K) {
        // Arrocco: bloccato se captures_only   true
        if (!captures_only && g_chess960 && td.castle) {
          // Chess960 (chess960.h): SF's general rule. The normal branch below stays identical.
          auto att = [&td](int sq) { return td_is_square_attacked(td, sq, black) != 0; };
          for (int idx = 0; idx < 2; idx++)
            if (c960_can_castle(td.castle, idx, td.occupancies[both], att))
              MG_ADD( encode_move(castle_king_sq[white], castle_king_to(idx), piece, 0, 0, 0, 0, 1));
        } else if (!captures_only) {
          if (td.castle & wk) {
            if (!get_bit(td.occupancies[both], f1) &&
                !get_bit(td.occupancies[both], g1)) {
              if (!td_is_square_attacked(td, e1, black) &&
                  !td_is_square_attacked(td, f1, black))
                MG_ADD( encode_move(e1, g1, piece, 0, 0, 0, 0, 1));
            }
          }
          if (td.castle & wq) {
            if (!get_bit(td.occupancies[both], d1) &&
                !get_bit(td.occupancies[both], c1) &&
                !get_bit(td.occupancies[both], b1)) {
              if (!td_is_square_attacked(td, e1, black) &&
                  !td_is_square_attacked(td, d1, black))
                MG_ADD( encode_move(e1, c1, piece, 0, 0, 0, 0, 1));
            }
          }
        }
      }
    } else {
      if (piece == p) {
        // Mirror of white (see above). The black pawns that promote are on the 2nd rank (squares 48..55), the LAST
        // in LSB order: first the others without branches, then them with the old code.
        {
          const U64 rest      = bitboard & ~0x00FF000000000000ULL;
          const U64 empty     = ~td.occupancies[both];
          const U64 push1     = captures_only ? 0ULL : (rest << 8) & empty;
          const U64 single_to = push1 & evasion_mask;
          // Double push: from the 7th rank (single pushes arriving on the 6th = squares 16..23), with the 5th free and in the block mask.
          const U64 double_to = ((push1 & 0x0000000000FF0000ULL) << 8) & empty & evasion_mask;
          const U64 cap_mask  = quiets_only ? 0ULL : enemies & evasion_mask;
          const U64 ep_bb     = (!quiets_only && td.enpassant != no_sq) ? (1ULL << td.enpassant) : 0ULL;
          const int ep_sq     = td.enpassant;
          // Only the pawns that have at least one move (see white): they capture on `tgt` the squares attacked by a
          // WHITE pawn placed on `tgt`.
          const U64 tgt       = cap_mask | ep_bb;
          U64 bb = rest & ((single_to >> 8) | (double_to >> 16) | ((tgt >> 7) & not_a_file) | ((tgt >> 9) & not_h_file));
          while (bb) {
            const int s  = get_ls1b_index(bb);
            const U64 pa = pawn_attacks[black][s];
            const U64 caps = pa & cap_mask;
            mg_out[mg_cnt] = encode_move(s, s + 8, piece, 0, 0, 0, 0, 0);
            mg_cnt += (int)((single_to >> (s + 8)) & 1);
            mg_out[mg_cnt] = encode_move(s, s + 16, piece, 0, 0, 1, 0, 0);
            mg_cnt += (int)((double_to >> (s + 16)) & 1);
            mg_out[mg_cnt] = encode_move(s, s + 7, piece, 0, 1, 0, 0, 0);
            mg_cnt += (int)((caps >> (s + 7)) & 1);
            mg_out[mg_cnt] = encode_move(s, s + 9, piece, 0, 1, 0, 0, 0);
            mg_cnt += (int)((caps >> (s + 9)) & 1);
            mg_out[mg_cnt] = encode_move(s, ep_sq, piece, 0, 1, 0, 1, 0);
            mg_cnt += (int)((pa & ep_bb) != 0);
            pop_lsb_bb(bb);
          }
        }
        U64 promo = bitboard & 0x00FF000000000000ULL;
        while (promo) {
          source_square = get_ls1b_index(promo);
          target_square = source_square + 8;

          if (!captures_only ||
              (promo_quiet && source_square >= a2 && source_square <= h2)) {
            if (!(target_square > h1) &&
                !get_bit(td.occupancies[both], target_square)) {
              if (source_square >= a2 && source_square <= h2) {
                if ((evasion_mask >> target_square) & 1) {
                  MG_ADD( encode_move(source_square, target_square,
                                                  piece, q, 0, 0, 0, 0));
                  if (!promo_queen_only) {
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, r, 0, 0, 0, 0));
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, b, 0, 0, 0, 0));
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, n, 0, 0, 0, 0));
                  } else if (promo_knight) {
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, n, 0, 0, 0, 0));
                  }
                }
              } else {
                if ((evasion_mask >> target_square) & 1)
                  MG_ADD( encode_move(source_square, target_square,
                                                  piece, 0, 0, 0, 0, 0));
                if ((source_square >= a7 && source_square <= h7) &&
                    !get_bit(td.occupancies[both], target_square + 8) &&
                    ((evasion_mask >> (target_square + 8)) & 1))
                  MG_ADD(
                           encode_move(source_square, target_square + 8, piece,
                                       0, 0, 1, 0, 0));
              }
            }
          }

          attacks = quiets_only ? 0ULL   // 2A: pawn captures do not go through allowed_squares
                  : (pawn_attacks[MG_SIDE][source_square] & enemies & evasion_mask);
          while (attacks) {
            target_square = get_ls1b_index(attacks);
            if (source_square >= a2 && source_square <= h2) {
              MG_ADD( encode_move(source_square, target_square,
                                              piece, q, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, r, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, b, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, n, 1, 0, 0, 0));
            } else {
              MG_ADD( encode_move(source_square, target_square,
                                              piece, 0, 1, 0, 0, 0));
            }
            pop_lsb_bb(attacks);   // 2D: target_square IS the LSB of attacks
          }
          if (!quiets_only && td.enpassant != no_sq) {   // 2A: e.p. lands on an EMPTY square
            U64 enpassant_attacks =
                pawn_attacks[MG_SIDE][source_square] & (1ULL << td.enpassant);
            if (enpassant_attacks) {
              int target_enpassant = get_ls1b_index(enpassant_attacks);
              MG_ADD( encode_move(source_square, target_enpassant,
                                              piece, 0, 1, 0, 1, 0));
            }
          }
          pop_lsb_bb(promo);
        }
      }
      if (piece == k) {
        if (!captures_only && g_chess960 && td.castle) {
          auto att = [&td](int sq) { return td_is_square_attacked(td, sq, white) != 0; };
          for (int idx = 2; idx < 4; idx++)
            if (c960_can_castle(td.castle, idx, td.occupancies[both], att))
              MG_ADD( encode_move(castle_king_sq[black], castle_king_to(idx), piece, 0, 0, 0, 0, 1));
        } else if (!captures_only) {
          if (td.castle & bk) {
            if (!get_bit(td.occupancies[both], f8) &&
                !get_bit(td.occupancies[both], g8)) {
              if (!td_is_square_attacked(td, e8, white) &&
                  !td_is_square_attacked(td, f8, white))
                MG_ADD( encode_move(e8, g8, piece, 0, 0, 0, 0, 1));
            }
          }
          if (td.castle & bq) {
            if (!get_bit(td.occupancies[both], d8) &&
                !get_bit(td.occupancies[both], c8) &&
                !get_bit(td.occupancies[both], b8)) {
              if (!td_is_square_attacked(td, e8, white) &&
                  !td_is_square_attacked(td, d8, white))
                MG_ADD( encode_move(e8, c8, piece, 0, 0, 0, 0, 1));
            }
          }
        }
      }
    }

    // --- PIECES (here we apply the allowed_squares mask) ---
    if ((MG_SIDE == white) ? piece == N : piece == n) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = knight_attacks[source_square] & piece_allowed;
        mg_cnt = mg_emit_targets(mg_out, mg_cnt, attacks, enemies, source_square, piece, captures_only);
        pop_lsb_bb(bitboard);   // 2D: source_square IS the LSB of bitboard
      }
    }

    if ((MG_SIDE == white) ? piece == B : piece == b) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = get_bishop_attacks(source_square, td.occupancies[both]) &
                  piece_allowed;
        mg_cnt = mg_emit_targets(mg_out, mg_cnt, attacks, enemies, source_square, piece, captures_only);
        pop_lsb_bb(bitboard);   // 2D: source_square IS the LSB of bitboard
      }
    }

    if ((MG_SIDE == white) ? piece == R : piece == r) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = get_rook_attacks(source_square, td.occupancies[both]) &
                  piece_allowed;
        mg_cnt = mg_emit_targets(mg_out, mg_cnt, attacks, enemies, source_square, piece, captures_only);
        pop_lsb_bb(bitboard);   // 2D: source_square IS the LSB of bitboard
      }
    }

    if ((MG_SIDE == white) ? piece == Q : piece == q) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = get_queen_attacks(source_square, td.occupancies[both]) &
                  piece_allowed;
        mg_cnt = mg_emit_targets(mg_out, mg_cnt, attacks, enemies, source_square, piece, captures_only);
        pop_lsb_bb(bitboard);   // 2D: source_square IS the LSB of bitboard
      }
    }

    if ((MG_SIDE == white) ? piece == K : piece == k) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = king_attacks[source_square] & allowed_squares;
        mg_cnt = mg_emit_targets(mg_out, mg_cnt, attacks, enemies, source_square, piece, captures_only);
        pop_lsb_bb(bitboard);   // 2D: source_square IS the LSB of bitboard
      }
    }
  }
  move_list->count = mg_cnt;
}

// GL (15/10/2026): LEGAL generator (movegen_legal.h). It covers the three forms the search asks for: all moves
// (evasions, root, perft), the tacticals of fill_tactics and the quiet moves of fill_quiets. The lists are those of
// td_generate_moves_side minus the illegal moves, IN THE SAME ORDER (legal_emit_tv): identical tree, bench unchanged
// (137419). Chess960 and the forms the search does not ask for (knight promotion among the tacticals) stay on the
// usual pseudo-legal generator.
#include "../movegen_legal.h"

// true: the lists of td_generate_moves contain only legal moves (outside Chess960), so the search can
// skip legal_before and the king test in the make for the moves that are not the hash move.
static inline bool td_lists_legal() { return !g_chess960; }

// One stage of the queue (tacticals, quiets, evasions) with a single legal_prepare per node: pins, checks and masks
// are computed at the first stage and the queue keeps them for the next ones (MoveQueue::lgen).
struct LegalNode {
  legalgen::LegalInfo info;
  bool                ready;
};
// skip: a move the list must not hold (the hash move the queue has already handed out; 0 = none).
template <legalgen::Stage St>
static inline int legal_generate_stage(ThreadData &td, LegalNode &z, int *out, int skip = 0) {
  PROF_GUARD(prof_mg);
#ifdef TRIUMV_PROFILE
  prof_n_mg++;
#endif
  legalgen::ListSink<St> sink(out, skip ? skip : -1);
  if (td.side == white) {
    if (!z.ready) legalgen::legal_prepare<white>(td, z.info);
    legalgen::legal_emit_tv<white>(td, z.info, sink);
  } else {
    if (!z.ready) legalgen::legal_prepare<black>(td, z.info);
    legalgen::legal_emit_tv<black>(td, z.info, sink);
  }
  z.ready = true;
  return sink.n;
}

static void td_generate_moves(ThreadData &td, moves *move_list,
                              bool captures_only = false,
                              bool quiets_only   = false,
                              int  known_in_check = -1,
                              bool promo_quiet   = false,
                              bool promo_queen_only = false,
                              bool promo_knight = false) {
  if (td_lists_legal() && !promo_knight) {
    PROF_GUARD(prof_mg);
#ifdef TRIUMV_PROFILE
    prof_n_mg++;
#endif
    if (!captures_only && !quiets_only && !promo_queen_only) {
      move_list->count = legalgen::generate_legal_tv<legalgen::ALL>(td, move_list->moves);
      return;
    }
    if (captures_only && !quiets_only && promo_quiet && promo_queen_only) {
      move_list->count = legalgen::generate_legal_tv<legalgen::TACTICS>(td, move_list->moves);
      return;
    }
    if (quiets_only && !captures_only && !promo_queen_only) {
      move_list->count = legalgen::generate_legal_tv<legalgen::QUIETS>(td, move_list->moves);
      return;
    }
  }
  if (td.side == white)
    td_generate_moves_side<white>(td, move_list, captures_only, quiets_only, known_in_check,
                                  promo_quiet, promo_queen_only, promo_knight);
  else
    td_generate_moves_side<black>(td, move_list, captures_only, quiets_only, known_in_check,
                                  promo_quiet, promo_queen_only, promo_knight);
}

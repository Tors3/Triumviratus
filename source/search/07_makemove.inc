// search/07_makemove.inc -- Per-thread attacks, make_move and unmake_move.
// Part of threads.cpp: it is NOT a translation unit of its own. It must be included only by threads.cpp, in the order
// fixed there (split of 27/09/2026, identical generated code: see the header of threads.cpp).

// Additional keys of the corrections (09_history.inc): they serve the prefetch in td_make_move.
static inline U64 corr_phase_key(const ThreadData &td);
static inline U64 corr_fifty_key(const ThreadData &td);
static inline U64 corr_expert_key(const ThreadData &td);

// ============================================================================
// ATTACK DETECTION
// ============================================================================

static inline int td_is_square_attacked(ThreadData &td, int square,
                                        int attacker_side) {
  PROF_GUARD(prof_isatk);
#ifdef TRIUMV_PROFILE
  prof_n_isatk++;
#endif
  if (attacker_side == white) {
    if (pawn_attacks[black][square] & td.bitboards[P])
      return 1;
    if (knight_attacks[square] & td.bitboards[N])
      return 1;
    if (king_attacks[square] & td.bitboards[K])
      return 1;
    // AA1 (10/10/2026): bishop and rook from a single computation (magic.h).
    const SliderPair sp = get_slider_pair(square, td.occupancies[both]);
    if (pair_bishop(sp) & (td.bitboards[B] | td.bitboards[Q]))
      return 1;
    if (pair_rook(sp) & (td.bitboards[R] | td.bitboards[Q]))
      return 1;
  } else {
    if (pawn_attacks[white][square] & td.bitboards[p])
      return 1;
    if (knight_attacks[square] & td.bitboards[n])
      return 1;
    if (king_attacks[square] & td.bitboards[k])
      return 1;
    const SliderPair sp = get_slider_pair(square, td.occupancies[both]);
    if (pair_bishop(sp) & (td.bitboards[b] | td.bitboards[q]))
      return 1;
    if (pair_rook(sp) & (td.bitboards[r] | td.bitboards[q]))
      return 1;
  }
  return 0;
}

// Squares strictly between two aligned squares (0 for knight/non-aligned). Filled by init_cycle_tables
// (10_order.inc); used here (discovered checks), by the evasions (08_movegen.inc), by the legality test and
// by the upcoming-repetition test.
static U64 between_tbl[64][64];
// Rook and bishop attacks on an EMPTY board (06/10/2026, speed, identical tree): they serve to find the snipers
// aligned with a king (td_lmp_chk_init, safety_init, see_pins_update: ~3.3 reads per node). Before, they went through
// get_rook_attacks(sq, 0) / get_bishop_attacks(sq, 0), i.e. entry 0 of the square's row in the PEXT tables:
// rows of 32 KB (rook) and 4 KB (bishop): the 64 "empty" entries of each piece fall in the SAME L1 set
// (address bits 6-11 equal, 8 ways), together with entries 0-7 of every row. Here they sit in 1 contiguous KB. Same values.
// Filled by init_cycle_tables (10_order.inc) together with between_tbl.
alignas(64) static U64 empty_rook_att[64];
alignas(64) static U64 empty_bishop_att[64];

// ---------------------------------------------------------------------------
// LMPCheckGuard: "does this quiet move give check?" computed BEFORE the make.
// The engine's gives_check (:4631+) lives AFTER the make; the LMP prunes before,
// so the SF-style test with the check squares is needed: the squares from which a piece
// of type T would attack the enemy king, computed ONCE per node
// (lazily, only if the LMP really fires) and then O(1) per move.
struct LmpChkCtx {
  bool ready = false;
  U64 chk[6] = {0, 0, 0,
                0, 0, 0}; // indexed by type: P,N,B,R,Q,K (K unused)
  U64 disc = 0;           // our pieces that, by moving, uncover a check
};

static inline void td_lmp_chk_init(ThreadData &td, LmpChkCtx &c) {
  const int us = td.side;
  const int off = (us == white) ? 0 : 6;  // our offset in the bitboards
  const int eoff = (us == white) ? 6 : 0; // enemy offset
  const int eksq = get_ls1b_index(td.bitboards[K + eoff]);
  const U64 occ = td.occupancies[both];

  c.chk[N] = knight_attacks[eksq];
  const SliderPair sp = get_slider_pair(eksq, occ);   // AA1 (10/10/2026): one computation for both
  c.chk[B] = pair_bishop(sp);
  c.chk[R] = pair_rook(sp);
  c.chk[Q] = c.chk[B] | c.chk[R];
  c.chk[P] =
      pawn_attacks[us ^ 1][eksq]; // where OUR pawn attacks that king from
  c.chk[K] = 0;

  // Discovered checks: one of our pieces is the only screen between one of our
  // sliders and the enemy king. The enemy king is NOT in check (it is our
  // move), so any R/Q (resp. B/Q) of ours that appears when removing the
  // screen is necessarily behind it.
  // Evening of 04/10/2026 - from the SNIPERS, not from the candidates: start from our sliders aligned with the enemy king
  // (attacks on an empty board) and look at what lies in between: a single piece, ours, is the screen. Same
  // set as before (the enemy king is not in check, so every sniper has at least one piece in front), but one
  // read of the "in between" table per sniper instead of one PEXT read for every piece of ours seen from the king.
  const U64 ours = td.occupancies[us];
  const U64 orq = td.bitboards[R + off] | td.bitboards[Q + off];
  const U64 obq = td.bitboards[B + off] | td.bitboards[Q + off];
  c.disc = 0;
  U64 snipers = (empty_rook_att[eksq] & orq) | (empty_bishop_att[eksq] & obq);
  while (snipers) {
    const int sn = get_ls1b_index(snipers);
    const U64 between = between_tbl[eksq][sn] & occ;
    if (between && !(between & (between - 1)) && (between & ours))
      c.disc |= between;
    pop_lsb_bb(snipers);
  }
  c.ready = true;
}

// ponytail: overestimates on purpose. A "disc" piece that moves STAYING
// on the king's line uncovers nothing, but here it counts as a check anyway (an
// aligned() table is missing) - and the same for castling, where the rook can give
// check from its arrival square. Overestimating = pruning LESS = safe direction for
// a guard. If the patch pays off, it gets refined.
static inline bool td_lmp_gives_check(ThreadData &td, int move, LmpChkCtx &c) {
  PROF_GUARD(prof_gc);
  if (!c.ready)
    td_lmp_chk_init(td, c);
  const int tgt = get_move_target(move);
  const int pt = get_move_piece(move) % 6; // P..K independent of colour
  return (c.chk[pt] & (1ULL << tgt)) ||
         (c.disc & (1ULL << get_move_source(move)));
}

// Start/arrival square of the ROOK in castling, derived from the king's arrival square. Single source for
// all the sites (occupancies, keys, mailbox, make, rollback, unmake, NNUE). The start comes from
// castle_rook_sq (chess960.h): h1/a1/h8/a8 in normal chess, the square read from the FEN in 960.
static inline void td_castle_rook(int target, int &rook_pc, int &rf, int &rt) {
  const int idx = castle_index(target);
  rook_pc = idx < 2 ? R : r;
  rf = castle_rook_sq[idx];
  rt = castle_rook_to(idx);
}

// Incremental occupancy update. Flips only the bits that the move touches,
// instead of OR-recomputing all 12 bitboards (+2.89% NPS, node-identical). XOR
// is self-inverse, so calling it again with the same move reverses it (used on
// make-forward, illegal rollback, and unmake). 'us' must be the MOVING side at
// the call site. AA3 (10/10/2026): rollback and unmake restore the occupancies (and the keys of td_keys_update) from the
// copy in UndoInfo; only the make stays here.
// captured_piece == -1 means no capture. For castling the rook squares derive
// from the king target. Produces occupancies identical to the full recompute.
static inline void td_occ_update(ThreadData &td, int us, int source, int target,
                                 int captured_square, int captured_piece,
                                 int castling) {
  // XOR between the two squares (not OR): the same for every normal move (source != target), and in Chess960 the king that
  // castles without moving (source == target) changes nothing, instead of emptying its own square. With the
  // four squares of king and rook in XOR the 960 overlaps too (king where the rook started) come out right.
  td.occupancies[us] ^= (1ULL << source) ^ (1ULL << target);
  // capture (== target, or e.p. square). AA3 (10/10/2026): branchless, null mask without a victim.
  td.occupancies[us ^ 1] ^= captured_piece != -1 ? 1ULL << (captured_square & 63) : 0;
  if (castling) {
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    td.occupancies[us] ^= (1ULL << rf) ^ (1ULL << rt);
  }
  td.occupancies[both] = td.occupancies[white] | td.occupancies[black];
}

#ifdef TRIUMV_MAILBOX_VERIFY
// Exhaustive mailbox<->bitboard check, for validation. The engine's perft uses
// the GLOBAL make_move and never comes through here, so without this check there would
// be no way to prove that the castling/e.p./promotion branches are covered: a
// misalignment gives no crash and no different signature, only an ordering that is wrong.
//   build:  -DTRIUMV_MAILBOX_VERIFY   then   echo bench | engine
static inline void td_mailbox_verify(ThreadData &td, const char *where) {
  for (int sq = 0; sq < 64; sq++) {
    int expect = -1;
    for (int pc = P; pc <= k; pc++)
      if (get_bit(td.bitboards[pc], sq)) { expect = pc; break; }
#if defined(USE_AVX512ICL)
    if (td.pc8[sq] != expect) {
      fprintf(stderr, "[MAILBOX] %s: sq=%d pc8=%d bitboards=%d\n", where, sq, td.pc8[sq], expect);
      fflush(stderr);
      abort();
    }
#endif
    if (td.piece_on[sq] != expect) {
      fprintf(stderr, "[MAILBOX] %s: sq=%d mailbox=%d bitboards=%d\n", where, sq,
              td.piece_on[sq], expect);
      fflush(stderr);
      abort();
    }
  }
}
  #define MB_VERIFY(td, where) td_mailbox_verify((td), (where))
#else
  #define MB_VERIFY(td, where) ((void) 0)
#endif

// MAILBOX - the two halves are NOT self-inverse like td_occ_update (which is an XOR):
// here values are written, so distinct forward and backward passes are needed. They must be called
// at the SAME THREE SITES as td_occ_update, or the mailbox gets out of step with the bitboards
// and the ordering goes wrong silently (no crash, no different signature).
// avx512icl (05/11/2026, R26 §6): every write of piece_on is repeated on the byte mailbox td.pc8 (threads.h).
#if defined(USE_AVX512ICL)
  #define TD_PC8(sq, v) (td.pc8[(sq)] = (std::int8_t)(v))
#else
  #define TD_PC8(sq, v) ((void)0)
#endif
static inline void td_mailbox_apply(ThreadData &td, int piece, int source, int target,
                                    int promoted, int captured_piece,
                                    int captured_square, int castling) {
  // The victim BEFORE the arriving piece: in e.p. `captured_square` != target,
  // in a normal capture they coincide and the arriving piece must win.
  if (castling) {
    // First the two starts, then the two arrivals: in Chess960 the king can arrive where the rook started (or
    // not move), and writing "king arrived" before "rook left" would erase the king.
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    td.piece_on[source] = -1;
    td.piece_on[rf] = -1;
    td.piece_on[target] = piece;
    td.piece_on[rt] = rook_pc;
    TD_PC8(source, -1);
    TD_PC8(rf, -1);
    TD_PC8(target, piece);
    TD_PC8(rt, rook_pc);
    return;
  }
  // AA3 (10/10/2026): branchless on the capture; without a victim the first write falls on the start square. The index
  // is chosen with a mask: with `?:` clang emits a branch so as not to read captured_square.
  const int victim_sq = source ^ ((source ^ captured_square) & -int(captured_piece != -1));
  td.piece_on[victim_sq] = -1;
  td.piece_on[source] = -1;
  td.piece_on[target] = promoted ? promoted : piece;
  TD_PC8(victim_sq, -1);
  TD_PC8(source, -1);
  TD_PC8(target, promoted ? promoted : piece);
}

static inline void td_mailbox_revert(ThreadData &td, int piece, int source, int target,
                                     int promoted, int captured_piece,
                                     int captured_square, int castling) {
  (void) promoted;  // on the way back the piece returns to the starting one, promoted or not
  if (castling) {
    // Mirror of td_mailbox_apply: first the two arrivals are emptied, then king and rook are put back.
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    td.piece_on[target] = -1;
    td.piece_on[rt] = -1;
    td.piece_on[source] = piece;
    td.piece_on[rf] = rook_pc;
    TD_PC8(target, -1);
    TD_PC8(rt, -1);
    TD_PC8(source, piece);
    TD_PC8(rf, rook_pc);
    return;
  }
  // Mirrored order: target is emptied and THEN the victim is put back, so that when
  // captured_square == target (normal capture) the victim takes its place again.
  // AA3 (10/10/2026): branchless on the capture. Without a victim the third write puts -1 back on target.
  const int victim_sq = target ^ ((target ^ captured_square) & -int(captured_piece != -1));
  td.piece_on[target] = -1;
  td.piece_on[source] = piece;
  td.piece_on[victim_sq] = captured_piece;
  TD_PC8(target, -1);
  TD_PC8(source, piece);
  TD_PC8(victim_sq, captured_piece);
}

// ============================================================================
// MAKE MOVE (returns 1 if legal)
// ============================================================================

// Partial keys of the position (XOR self-inverse: the same call at the 3 sites of td_occ_update - make-forward,
// rollback of the illegal move, unmake):
//   pawn_key   (P2.1)          pawns only: off source, on target only if it does NOT promote; captured pawn off
//                              captured_square (e.p. too). Read by pawn_row and by the pawn correction.
//   np_key[c]  (CorrNonPawn)   non-pawns per colour, king included: the non-pawn mover, the PROMOTED piece appearing
//                              on target, the captured non-pawn (key of the captured colour), the castling rook.
//   mm_key[0/1] (NPS 25/09)    minors (N/B) and majors (R/Q), same events. Before, td_corr_index_minor/major
//                              rescanned 8 bitboards up to four times per node (~370 cycles/node on 25/09).
// Evening of 04/10/2026 - ONE BRANCHLESS function instead of the three (td_pawn_key_update, td_np_key_update,
// td_mm_key_update), which between them made about ten branches on the piece type, mispredicted (xperf: ~0.75 per
// node). The six keys are a contiguous array (threads.h: pawn, minors, majors, white np, black np, sink) and two
// tables say, per piece code, into which two keys its XOR goes; what goes into no key ends up
// in the sink. Index 15 is "no piece" (captured_piece = -1): piece_keys has 16 rows, the last four at zero.
// Same values as before by construction => identical nodes. Only one branch remains, the castling one (rare).
enum { KEY_PAWN = 0, KEY_MINOR = 1, KEY_MAJOR = 2, KEY_NP_W = 3, KEY_NP_B = 4, KEY_SINK = 5 };
static_assert(offsetof(ThreadData, mm_key) == offsetof(ThreadData, pawn_key) + 8 * KEY_MINOR &&
              offsetof(ThreadData, np_key) == offsetof(ThreadData, pawn_key) + 8 * KEY_NP_W &&
              offsetof(ThreadData, key_sink) == offsetof(ThreadData, pawn_key) + 8 * KEY_SINK,
              "td_keys_update: le chiavi parziali devono essere contigue in ThreadData");
//                                            P         N          B          R          Q          K
static constexpr unsigned char kKeyA[16] = {KEY_PAWN, KEY_MINOR, KEY_MINOR, KEY_MAJOR, KEY_MAJOR, KEY_SINK,
                                            KEY_PAWN, KEY_MINOR, KEY_MINOR, KEY_MAJOR, KEY_MAJOR, KEY_SINK,
                                            KEY_SINK, KEY_SINK, KEY_SINK, KEY_SINK};
static constexpr unsigned char kKeyB[16] = {KEY_SINK, KEY_NP_W, KEY_NP_W, KEY_NP_W, KEY_NP_W, KEY_NP_W,
                                            KEY_SINK, KEY_NP_B, KEY_NP_B, KEY_NP_B, KEY_NP_B, KEY_NP_B,
                                            KEY_SINK, KEY_SINK, KEY_SINK, KEY_SINK};
static inline void td_keys_update(ThreadData &td, int piece, int source, int target, int promoted,
                                  int captured_piece, int captured_square, int castling) {
  U64 *const keys = &td.pawn_key;
  // The mover: off source, on target (for the promoting pawn the target is taken by the promoted piece).
  const U64 kd = piece_keys[piece][source] ^ (piece_keys[piece][target] & (0 - (U64)(promoted == 0)));
  keys[kKeyA[piece]] ^= kd;
  keys[kKeyB[piece]] ^= kd;
  // The promoted piece appears on target (promoted = 0 -> key zeroed, it ends up in pawn/sink as zero).
  const U64 kp = piece_keys[promoted][target] & (0 - (U64)(promoted != 0));
  keys[kKeyA[promoted]] ^= kp;
  keys[kKeyB[promoted]] ^= kp;
  // The captured piece: -1 becomes 15 (zero row, keys in the sink).
  const int ci = captured_piece & 15;
  const U64 kc = piece_keys[ci][captured_square & 63];
  keys[kKeyA[ci]] ^= kc;
  keys[kKeyB[ci]] ^= kc;
  if (castling) {
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    const U64 kr = piece_keys[rook_pc][rf] ^ piece_keys[rook_pc][rt];
    keys[rook_pc == R ? KEY_NP_W : KEY_NP_B] ^= kr;
    keys[KEY_MAJOR] ^= kr;
  }
}

// ESTIMATED key of the child (SF key_after): piece from/to, victim from the mailbox (still pre-move), side, e.p.
// cleared. It ignores castling, promotion, new e.p. square and castling rights: in those (rare) cases the estimate is wrong and
// the make redoes the prefetches with the true key. Branchless: an empty square has -1 in the mailbox, i.e. row 15 of
// piece_keys, which is zero.
// ⛔ Night of 04/10/2026 - TRIED AND REMOVED: issuing these prefetches already in move_loop and quiesce, as soon as the move
// leaves the queue (before check, pruning and singular), to give the child's probe more lead time. xperf 6 rounds,
// identical nodes: cycles +0.03%, instructions +0.70%. The probe did not wait longer than the make already covers; the prefetches
// of the moves later pruned were only extra instructions.
static inline U64 td_child_key_guess(const ThreadData &td, int move) {
  const int source = get_move_source(move), target = get_move_target(move), piece = get_move_piece(move);
  U64 key = td.hash_key ^ piece_keys[piece][source] ^ piece_keys[piece][target] ^ side_key;
  key ^= piece_keys[td.piece_on[target] & 15][target];
  if (td.enpassant != no_sq)
    key ^= enpassant_keys[td.enpassant];
  return key;
}
// The line the child reads first: the hash bucket (the evaluation cache is gone, E1 in threads.h).
static inline void td_prefetch_child(const ThreadData &, U64 key) {
  TT_PREFETCH(&hash_table[tt_base_index(key)]);
}

// known_legal: the search has already decided legality (legal_before with the king not in check, see step_in) and the test
// "is our king attacked?" below would be a duplicate. Perft, root and validations leave it false.
static inline int td_make_move(ThreadData &td, int move, UndoInfo &undo, bool known_legal = false) {
  PROF_GUARD(prof_make);
  undo.old_castle = td.castle;
  undo.old_enpassant = td.enpassant;
  // AA3 (10/10/2026): keys, fifty-move counter, plies from the null move and occupancies before the move, for the unmake (see UndoInfo).
  std::memcpy(undo.keys, &td.hash_key, sizeof(undo.keys));
  std::memcpy(undo.occ, td.occupancies, sizeof(undo.occ));

  int source = get_move_source(move);
  int target = get_move_target(move);
  int piece = get_move_piece(move);
  int promoted = get_move_promoted(move);
  int capture = get_move_capture(move);
  int double_push = get_move_double(move);
  int enpass = get_move_enpassant(move);
  int castling = get_move_castling(move);

  // NPS 25/09/2026 - EARLY PREFETCH (SF key_after), see td_child_key_guess. The profile of 25/09 gave ~6% of the
  // time to the load of the TT entry in the child's probe: the prefetch started at the end of the make and had little lead time.
  // Identical nodes: the prefetches change nothing functional.
  const U64 pf_key = td_child_key_guess(td, move);
  td_prefetch_child(td, pf_key);

  // Captured piece and its square, from the still intact board (07/10/2026: computed here at the top, before any
  // change, because the network's dirty entries below need them too; same values as before).
  // AA3 (10/10/2026, speed, same values): value selects instead of branches. The capture is a property of the move
  // (24-28% of the makes in the counters, bench 15-16 on fens30/eg30), and the branch on it here, further down and in the unmake
  // was mispredicted: a 2-bit predictor per site gets it wrong ~0.23-0.28 times per node at each of the two sites.
  {
    const int start = (td.side == white) ? p : P;
    const int end = (td.side == white) ? k : K;
    // The range test replicates exactly the replaced loop, which searched only among the ENEMY pieces and left
    // -1 if it found nothing.
    const int mb_pc = td.piece_on[target];
    const int cp_normal = (mb_pc >= start && mb_pc <= end) ? mb_pc : -1;
    const int csq = enpass ? ((td.side == white) ? target + 8 : target - 8) : target;
    const int cp = enpass ? ((td.side == white) ? p : P) : cp_normal;
    undo.captured_square = capture ? csq : -1;
    undo.captured_piece = capture ? cp : -1;
  }

  // The network's dirty entries for this move, on the board BEFORE the move (06_nndirty.inc): a new state on the
  // handle's stack. If the move turns out illegal, the state is removed below together with the rest.
  {
    int rook_pc = -1, rf = 0, rt = 0;
    if (castling)
      td_castle_rook(target, rook_pc, rf, rt);
    nn_make_dirty(td, piece, source, target, promoted, undo.captured_piece, undo.captured_square, rook_pc, rf, rt);
  }

  pop_bit(td.bitboards[piece], source);
  set_bit(td.bitboards[piece], target);
  td.hash_key ^= piece_keys[piece][source];
  td.hash_key ^= piece_keys[piece][target];

  // AA3: fifty-move counter and victim branchless. Without a victim the mask is null (bitboard 0 unchanged) and row 15 of
  // piece_keys is zero (see td_keys_update).
  // With || and ?: clang emits three branches (capture, white pawn, black pawn): here bitwise and, no branch.
  const int fifty_runs = int(piece != P) & int(piece != p) & int(capture == 0);
  td.fifty = (td.fifty + 1) & -fifty_runs;
  {
    const int cp = undo.captured_piece;
    const U64 cbit = cp != -1 ? 1ULL << (undo.captured_square & 63) : 0;
    td.bitboards[cp != -1 ? cp : 0] &= ~cbit;
    td.hash_key ^= piece_keys[cp & 15][undo.captured_square & 63];
  }

  if (promoted) {
    pop_bit(td.bitboards[piece], target);
    td.hash_key ^= piece_keys[piece][target];
    set_bit(td.bitboards[promoted], target);
    td.hash_key ^= piece_keys[promoted][target];
  }

  if (td.enpassant != no_sq)
    td.hash_key ^= enpassant_keys[td.enpassant];
  td.enpassant = no_sq;

  if (double_push) {
    int ep_sq = (td.side == white) ? target + 8 : target - 8;
    // F-017 EPKeyFix (default ON): ep in the board/key ONLY if an enemy pawn
    // can capture it (pseudo-legal, as SF). No "phantom ep" -> the
    // transpositions a2a3+a3a4 vs a2a4 have the same key = more TT hits.
    // No effect on the movegen: without a capturer the ep square was unusable
    // anyway.
    if ((pawn_attacks[td.side][ep_sq] &
                          td.bitboards[(td.side == white) ? p : P])) {
      td.enpassant = ep_sq;
      td.hash_key ^= enpassant_keys[ep_sq];
    }
  }

  if (castling) {
    // King and rook in different bitboards: even with the 960 overlaps pop/set do not interfere.
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    pop_bit(td.bitboards[rook_pc], rf);
    set_bit(td.bitboards[rook_pc], rt);
    td.hash_key ^= piece_keys[rook_pc][rf];
    td.hash_key ^= piece_keys[rook_pc][rt];
  }

  td.hash_key ^= castle_keys[td.castle];
  td.castle &= castling_rights[source];
  td.castle &= castling_rights[target];
  td.hash_key ^= castle_keys[td.castle];

  td_occ_update(td, td.side, source, target, undo.captured_square,
                undo.captured_piece, castling);
  td_keys_update(td, piece, source, target, promoted, undo.captured_piece, undo.captured_square, castling);
  td_mailbox_apply(td, piece, source, target, promoted, undo.captured_piece,
                   undo.captured_square, castling);  // sito 1/3: make-forward
  MB_VERIFY(td, "make-forward");

  td.side ^= 1;
  td.hash_key ^= side_key;

  if (!known_legal &&
      td_is_square_attacked(td, get_ls1b_index((td.side == white) ? td.bitboards[k] : td.bitboards[K]), td.side)) {
    td.side ^= 1;

    if (promoted) {
      pop_bit(td.bitboards[promoted], target);
      set_bit(td.bitboards[piece], source);
    } else {
      pop_bit(td.bitboards[piece], target);
      set_bit(td.bitboards[piece], source);
    }

    if (undo.captured_piece != -1) {
      set_bit(td.bitboards[undo.captured_piece], undo.captured_square);
    }

    if (castling) {
      int rook_pc, rf, rt;
      td_castle_rook(target, rook_pc, rf, rt);
      pop_bit(td.bitboards[rook_pc], rt);
      set_bit(td.bitboards[rook_pc], rf);
    }

    td.castle = undo.old_castle;
    td.enpassant = undo.old_enpassant;
    // AA3: keys, fifty-move counter and occupancies from the copy (plies_from_null had not changed yet).
    std::memcpy(&td.hash_key, undo.keys, sizeof(undo.keys));
    std::memcpy(td.occupancies, undo.occ, sizeof(undo.occ));
    td_mailbox_revert(td, piece, source, target, promoted, undo.captured_piece,
                      undo.captured_square, castling);  // site 2/3: illegal move
    MB_VERIFY(td, "rollback-illegale");
    nn_unmake_dirty(td);  // the network state written at the top is no longer needed

    return 0;
  }

  // Prefetch of the corrections the child reads first: pawn structure and minor pieces (keys already
  // updated above). td.side is already the child's. Since 04/10/2026 the tables are shared between the
  // threads and big (1 MB per thread), no longer 64 KB in L2: the prefetch here has misses to hide again.
  // 06/10/2026 (report docs/audit_8.0/N_CORREZIONI.md): the rows are fetched ahead with the SAME keys that
  // corr_signal reads, phase salt (CorrPhase) and fifty-move band salt (CorrFiftyStep) included. Before, the keys
  // without salt were used: identical with the options off, with the options on two wasted prefetches and no row fetched ahead,
  // i.e. CorrPhase and CorrFifty paid speed in the SPRTs. With the options off the salts are 0: equivalent code.
  const U64 pf_phk = corr_phase_key(td), pf_fk = corr_fifty_key(td);
  TT_PREFETCH(&static_cast<CorrSlot (*)[2]>(td.st_corr)[(td.pawn_key ^ pf_phk ^ pf_fk) & td.st_corr_mask][td.side]);
  TT_PREFETCH(&static_cast<CorrSlot (*)[2]>(td.st_corr)[(td.mm_key[0] ^ pf_phk) & td.st_corr_mask][td.side]);
  // Evening of 04/10/2026: also the two non-pawn slots, which corr_signal reads right after (1.3 MB table per
  // thread: outside L2, every slot not fetched ahead is an L3 wait in the child).
  TT_PREFETCH(&static_cast<CorrSlot (*)[2]>(td.st_corr)[(td.np_key[white] ^ pf_fk) & td.st_corr_mask][td.side]);
  TT_PREFETCH(&static_cast<CorrSlot (*)[2]>(td.st_corr)[(td.np_key[black] ^ pf_fk) & td.st_corr_mask][td.side]);
  if (CorrExpertW)
    TT_PREFETCH(&static_cast<int16_t (*)[2]>(td.st_corr_x)[corr_expert_key(td) & td.st_corr_mask][td.side]);

  // The hash key can carry the fifty-move band (tt_key, HashFiftyStep): if it differs from the estimate the
  // right row is fetched. With the option off tt_key is td.hash_key and the test is the old one.
  const U64 tk = tt_key(td);
  if (tk != pf_key)   // wrong estimate (castling, promotion, e.p., fifty-move band)
    TT_PREFETCH(&hash_table[tt_base_index(tk)]);

  // ⛔ 04/10/2026 - TRIED AND REMOVED: prefetch here, at the move, of the HalfKA rows the child's update
  // will use (piece leaving/arriving/captured, two perspectives, band of the child). xperf 6 rounds, identical nodes:
  // cycles +0.58%, L3 misses per node unchanged (19.9 against 19.8). It fetches nothing useful ahead.
#ifdef TRIUMV_VERIFY_NNSYNC
  td.nnstack->st[td.nnstack->size - 1].key = nn_piece_key(td);  // synchronisation check (06_nndirty.inc)
#endif

  td.plies_from_null++; // P2.2: one more real move since the last null move

  return 1;
}

// ============================================================================
// UNMAKE MOVE
// ============================================================================

static inline void td_unmake_move(ThreadData &td, int move, UndoInfo &undo) {
  PROF_GUARD(prof_make);
  nn_unmake_dirty(td);   // the network state of this move leaves the stack
  // P2.2 (plies_from_null, mirror of the ++ of the successful make): restored with the AA3 copy below.
  td.side ^= 1;

  int source = get_move_source(move);
  int target = get_move_target(move);
  int piece = get_move_piece(move);
  int promoted = get_move_promoted(move);
  int castling = get_move_castling(move);

  if (promoted) {
    pop_bit(td.bitboards[promoted], target);
    set_bit(td.bitboards[piece], source);
  } else {
    pop_bit(td.bitboards[piece], target);
    set_bit(td.bitboards[piece], source);
  }

  // AA3 (10/10/2026): the victim comes back branchless (without a victim a null mask on bitboard 0).
  {
    const int cp = undo.captured_piece;
    const U64 cbit = cp != -1 ? 1ULL << (undo.captured_square & 63) : 0;
    td.bitboards[cp != -1 ? cp : 0] |= cbit;
  }

  if (castling) {
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    pop_bit(td.bitboards[rook_pc], rt);
    set_bit(td.bitboards[rook_pc], rf);
  }

  td.castle = undo.old_castle;
  td.enpassant = undo.old_enpassant;
  // AA3: key, partial keys, fifty-move counter, plies_from_null and occupancies from the make's copy, instead of
  // td_occ_update and td_keys_update (same values: they are those before the move).
  std::memcpy(&td.hash_key, undo.keys, sizeof(undo.keys));
  std::memcpy(td.occupancies, undo.occ, sizeof(undo.occ));
  td_mailbox_revert(td, piece, source, target, promoted, undo.captured_piece,
                    undo.captured_square, castling);  // sito 3/3: unmake
  MB_VERIFY(td, "unmake");
}

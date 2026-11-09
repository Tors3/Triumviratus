// search/12_quiesce.inc -- Node tools (hash table in the units of the search, legality, check, stepping into a child
// and back) and the quiescence search (block 5 of 8.1, in our form since 17/10/2026).
// Part of threads.cpp: NOT a translation unit of its own.

// ============================================================================
// HASH TABLE: reading and writing in the units and conventions of the search
// The depth is stored shifted by 3: -1 = quiescence, -2 = evaluation only, -3 = empty.
// ============================================================================
constexpr int HD_QUIESCE = -1;
constexpr int HD_EVAL    = -2;
constexpr int HD_SHIFT   = 3;
// The quiescence search writes and reads depth 0 in the hash (05/10/2026; +3.38 +- 2.58 over 20,162 games). That
// leaves one more level between "evaluation only" (-2) and quiescence: ProbCut entries at cut_depth + 1 = -1 stay
// below the quiescence (they do not cut there), and at depth 4 the small ProbCut accepts quiescence entries.
static inline int hd_qs() { return 0; }

enum { BOUND_UP = hash_flag_alpha, BOUND_LOW = hash_flag_beta, BOUND_EXACT = hash_flag_exact, BOUND_NIL = hash_flag_none };

struct HashView {
  bool hit;
  bool pv;
  int  move;
  int  value;       // VAL_NONE when absent
  int  depth;
  int  bound;
  int  eval_raw;    // stored evaluation (without the fifty-move damping), tt_eval_none when absent
};

// Does the entry's bound hold for the comparison asked? (lower = a lower bound is needed)
static inline bool bound_covers(int bound, bool lower) {
  return bound == BOUND_EXACT || bound == (lower ? BOUND_LOW : BOUND_UP);
}

// Decided values (mates, tablebase wins) are stored as distances from this node, not from the root.
static inline int to_hash_value(int v, int ply) {
  return val_known(v) && val_decided(v) ? v + (v > 0 ? ply : -ply) : v;
}

// Reading back turns the distance into one from the root again. A win that needs more plies than the fifty-move counter
// leaves (a mate measured to mate_value, a tablebase win to TB_WIN_TOP) is only "won somewhere": it comes back as the
// lowest decided value of that side. Rewritten 22/10/2026 in this symmetric form; identical results on all 1.09 billion
// combinations of value, ply and counter (scratch harness hashval_check.cpp).
static inline int from_hash_value(int v, int ply, int fifty) {
  if (!val_known(v)) return VAL_NONE;
  if (!val_decided(v)) return v;
  const int side = v > 0 ? 1 : -1, a = v * side;
  const int need = a >= MATE_FLOOR ? mate_value - a : TB_WIN_TOP - a;
  return need > 100 - fifty ? side * (TB_WIN_FLOOR - 1) : v - side * ply;
}

// Reading and writing go through tt_key (03_tables.inc): the board key plus, when on, the band of the fifty-move
// counter (HashFiftyStep).
static inline void hash_read(const ThreadData &td, HashView &h) {
  int move, value, depth, flag, ev;
  bool pv;
  const bool usable = probe_tt(tt_key(td), move, value, depth, flag, ev, pv);
  h.hit = usable || flag == BOUND_NIL;
  if (!h.hit) {
    h.pv = false; h.move = 0; h.value = VAL_NONE; h.depth = -HD_SHIFT; h.bound = BOUND_NIL; h.eval_raw = tt_eval_none;
    return;
  }
  h.pv = pv;
  h.move = move;
  h.depth = depth - HD_SHIFT;
  h.bound = flag;
  h.value = flag == BOUND_NIL ? VAL_NONE : from_hash_value(value, td.ply, td.fifty);
  h.eval_raw = ev;
}

static inline void hash_write(const ThreadData &td, int value, bool pv, int bound, int depth, int move, int eval_raw) {
  store_tt(tt_key(td), move, to_hash_value(value, td.ply), depth + HD_SHIFT, bound, 0, pv, eval_raw);
}

// Takes one ply off the entry: a bound that did not cut at sufficient depth is worth less.
static inline void hash_soften(U64 key) {
  tt_entry *e = tt_find(key);
  if (!e) return;
  const U64 d = e->data;
  const int dep = unpack_depth(d);
  if (dep == 0) return;
  const U64 nd = (d & ~(0xFFULL << 40)) | ((U64)(dep - 1) << 40);
  const U64 w = e->kw ^ d;
  e->data = nd;
  e->kw = w ^ nd;
}

// Static evaluation of the node: from the hash when present, otherwise from the network. raw = the form stored in the
// hash.
static inline int node_eval(ThreadData &td, const HashView &h, int &raw) {
  if (h.hit && h.eval_raw != tt_eval_none) {
    raw = h.eval_raw;
    return eval_refade(h.eval_raw, td.fifty);
  }
  const int v = static_evaluate(td);
  raw = eval_unfade(v, td.fifty);
  return v;
}

// Contempt (idea 3, 04/10/2026): a draw is worth -c for the side to move at the root and +c for the opponent. Aimed at
// rating lists (CCRL: ~90% draws), not self-play. 0 = off, the old draw values.
static inline int contempt(const ThreadData &td) {
  // DrawDefend (13/10/2026): when behind, a draw is worth +draw_defend for the root side (see 01_params.inc).
  // ContemptAhead (R19 §5, 03/11/2026, off at 0): the twin of DrawDefend, when ahead a draw is worth -contempt_ahead for
  // the root side (14_deepen.inc sets it from the score the previous move closed with).
  // ContemptOpp (09/11/2026, off at 0): the same against an opponent that has been erring (14_deepen.inc).
  const int c = Contempt - (DrawDefend ? td.draw_defend : 0) + (ContemptAhead ? td.contempt_ahead : 0) +
                (ContemptOpp ? td.contempt_opp : 0);
  return c ? (td.side == td.root_side ? -c : c) : 0;
}

// A draw with a value close to zero: +-1 depending on the node count, so that draws are not all identical.
static inline int draw_score(const ThreadData &td) { return -1 + (int)(td.nodes & 2) + contempt(td); }

// ============================================================================
// LEGALITY AND CHECK BEFORE MOVING
// ============================================================================

// a, b, c on the same line (rank, file or diagonal)?
static inline bool on_line(int a, int b, int c) {
  return (between_tbl[a][b] >> c & 1) || (between_tbl[a][c] >> b & 1) || (between_tbl[b][c] >> a & 1);
}

static inline U64 attackers_with(const ThreadData &td, int sq, int by, U64 occ) {
  const int off = by == white ? 0 : 6;
  const SliderPair sp = get_slider_pair(sq, occ);   // (10/10/2026) one computation for both (magic.h)
  return (pawn_attacks[by ^ 1][sq] & td.bitboards[P + off]) | (knight_attacks[sq] & td.bitboards[N + off]) |
         (king_attacks[sq] & td.bitboards[K + off]) |
         (pair_bishop(sp) & (td.bitboards[B + off] | td.bitboards[Q + off])) |
         (pair_rook(sp) & (td.bitboards[R + off] | td.bitboards[Q + off]));
}

// Context of the node for legality (pins on our king) and check (check squares on the enemy king), computed once at
// the first move.
struct MoveSafety {
  bool ready;
  int  our_king, their_king;
  U64  pinned;
  LmpChkCtx checks;
  bool pins_ready;   // the pins are computed only when legal_before is needed (hash move, Chess960)
};

// Pins on our king, for legal_before.
static void safety_pins(ThreadData &td, MoveSafety &s) {
  const int us = td.side, eoff = us == white ? 6 : 0;
  const U64 occ = td.occupancies[both];
  U64 snipers = (empty_rook_att[s.our_king] & (td.bitboards[R + eoff] | td.bitboards[Q + eoff])) |
                (empty_bishop_att[s.our_king] & (td.bitboards[B + eoff] | td.bitboards[Q + eoff]));
  s.pinned = 0;
  while (snipers) {
    const int sn = get_ls1b_index(snipers);
    const U64 between = between_tbl[s.our_king][sn] & occ;
    if (between && !(between & (between - 1)) && (between & td.occupancies[us]))
      s.pinned |= between;
    pop_lsb_bb(snipers);
  }
  s.pins_ready = true;
}

static void safety_init(ThreadData &td, MoveSafety &s) {
  const int us = td.side, them = us ^ 1;
  const int off = us == white ? 0 : 6, eoff = them == white ? 0 : 6;
  s.our_king = get_ls1b_index(td.bitboards[K + off]);
  s.their_king = get_ls1b_index(td.bitboards[K + eoff]);
  // With legal lists, legal_before serves only the hash move, which asks for the pins itself (legal_before).
  s.pins_ready = false;
  if (!td_lists_legal())
    safety_pins(td, s);
  if (!s.checks.ready)   // the quiet-move ordering may have computed them already
    td_lmp_chk_init(td, s.checks);
  s.ready = true;
}

// (06/10/2026, speed, same logic) legal_before runs ~2.2 times per node and was out of line with a full prologue even
// for the common case (not a king, no castling or e.p., usually not pinned: ~10 instructions of work). Now the common
// case is inline and the three rare cases (castling, en passant, king moves) are in legal_before_slow.
__attribute__((noinline)) static bool legal_before_slow(ThreadData &td, MoveSafety &s, int m) {
  const int from = move_from(m), to = move_to(m), us = td.side, them = us ^ 1;
  if (get_move_castling(m))
    return g_chess960 || !td_is_square_attacked(td, to, them);
  if (get_move_enpassant(m)) {
    const int capsq = us == white ? to + 8 : to - 8;
    const U64 occ = (td.occupancies[both] ^ (1ULL << from) ^ (1ULL << capsq)) | (1ULL << to);
    const int eoff = them == white ? 0 : 6;
    const SliderPair sp = get_slider_pair(s.our_king, occ);
    return !(pair_rook(sp) & (td.bitboards[R + eoff] | td.bitboards[Q + eoff])) &&
           !(pair_bishop(sp) & (td.bitboards[B + eoff] | td.bitboards[Q + eoff]));
  }
  if (from == s.our_king)
    return !attackers_with(td, to, them, td.occupancies[both] ^ (1ULL << from));
  return !(s.pinned >> from & 1) || on_line(from, to, s.our_king);
}

static inline bool legal_before(ThreadData &td, MoveSafety &s, int m) {
  PROF_GUARD(prof_gc);
  if (!s.ready) safety_init(td, s);
  if (!s.pins_ready) safety_pins(td, s);
  const int from = move_from(m);
  if ((m & 0xC00000) || from == s.our_king)   // castling (bit 23), en passant (bit 22), king
    return legal_before_slow(td, s, m);
  return !(s.pinned >> from & 1) || on_line(from, move_to(m), s.our_king);
}

static bool checks_before(ThreadData &td, MoveSafety &s, int m) {
  PROF_GUARD(prof_gc);
  if (!s.ready) safety_init(td, s);
  const int from = move_from(m), to = move_to(m), pc = move_piece(m), pt = pc - (td.side == white ? 0 : 6);
  const bool castling = get_move_castling(m) != 0;
  if (!castling && (s.checks.chk[pt] >> to & 1))
    return true;
  if ((s.checks.disc >> from & 1) && (castling || !on_line(from, to, s.their_king)))
    return true;
  const U64 occ = td.occupancies[both];
  const int off = td.side == white ? 0 : 6;
  if (get_move_promoted(m)) {
    const U64 o = occ ^ (1ULL << from);
    U64 att = 0;
    switch (get_move_promoted(m) % 6) {
    case N: att = knight_attacks[to]; break;
    case B: att = get_bishop_attacks(to, o); break;
    case R: att = get_rook_attacks(to, o); break;
    default: att = get_queen_attacks(to, o); break;
    }
    return att >> s.their_king & 1;
  }
  if (get_move_enpassant(m)) {
    const int capsq = td.side == white ? to + 8 : to - 8;
    const U64 o = (occ ^ (1ULL << from) ^ (1ULL << capsq)) | (1ULL << to);
    const SliderPair sp = get_slider_pair(s.their_king, o);
    return (pair_rook(sp) & (td.bitboards[R + off] | td.bitboards[Q + off])) ||
           (pair_bishop(sp) & (td.bitboards[B + off] | td.bitboards[Q + off]));
  }
  if (castling) {
    int rook_pc, rf, rt;
    td_castle_rook(to, rook_pc, rf, rt);
    return s.checks.chk[R] >> rt & 1;
  }
  return false;
}

// ============================================================================
// STEPS: into a child and back
// ============================================================================
// legal (15/10/2026): the move comes from a list of the legal generator (queue_move_legal), so the make skips the king
// test even in check.
static inline bool step_in(ThreadData &td, NodeFrame *f, int m, UndoInfo &u, bool legal = false) {
  td.repetition_table[++td.repetition_index] = td.hash_key;
  td.ply++;
  // (04/10/2026) Legality decided once is used only where it is complete: with the king NOT in check legal_before
  // decides alone (pins, king not walking into an attack, en passant, castling) and the make skips the test; in check
  // the test stays (legal_before is not complete there: a hash move or killer that did not answer the check was once
  // played, the king was captured a ply later and the network saw a position without a king). Chess960 castling always
  // goes through the make. With -DTRIUMV_VERIFY_LEGAL the make always repeats the test and prints every disagreement.
#ifdef TRIUMV_VERIFY_LEGAL
  if ((!f->in_check || legal) && !td_make_move(td, m, u)) {
    printf("info string LEGALITY WRONG: move %d taken as legal\n", m);
    fflush(stdout);
    td.ply--;
    td.repetition_index--;
    return false;
  }
  if (f->in_check && !legal && !td_make_move(td, m, u)) {
#else
  td.nn_hold = f->nn_hold;   // expert holds (C2HoldDepth, C2HoldQs): read by the make, cleared right after it
  const int made = td_make_move(td, m, u, (legal || !f->in_check) && !(g_chess960 && get_move_castling(m)));
  td.nn_hold = 0;
  if (!made) {
#endif
    td.ply--;
    td.repetition_index--;
    return false;
  }
  td.nodes++;
  f->move = m;
  f->hctx = 0;   // ThreatHist: the search's move loop rewrites it for its quiet moves
  f->captured = u.captured_piece;
  f->cont_hist = &follow_of(td)[f->in_check][is_tactical(m)][move_piece(m)][move_to(m)];
  f->cont_corr = &cont_corr_of(td)[move_piece(m)][move_to(m)];
  // (14/10/2026, speed at an identical tree) Prefetch of the continuation-correction cells the child reads first in
  // corr_signal (~-0.3% cycles). In the child f[-1] is this frame, so the rows are those of f[-1] and f[-3] here
  // (CorrContW, the child at -2 and -4) and of f[-2] (CorrContOppW, the child at -3); the cell is (arrived piece,
  // square). The structure corrections are already prefetched in the make. The frames before the root always exist
  // (sentinel row).
  {
    const int to = move_to(m), pc = td.piece_on[to];
    TT_PREFETCH(&(*f[-1].cont_corr)[pc][to]);
    TT_PREFETCH(&(*f[-3].cont_corr)[pc][to]);
    if (CorrContOppW)
      TT_PREFETCH(&(*f[-2].cont_corr)[pc][to]);
  }
  return true;
}

static inline void step_out(ThreadData &td, int m, UndoInfo &u) {
  td_unmake_move(td, m, u);
  td.ply--;
  td.repetition_index--;
}

struct NullUndo {
  int enpassant, fifty, plies_from_null;
  U64 hash;
};

static inline void step_in_null(ThreadData &td, NodeFrame *f, NullUndo &u) {
  td.repetition_table[++td.repetition_index] = td.hash_key;
  td.ply++;
  u.enpassant = td.enpassant;
  u.fifty = td.fifty;
  u.plies_from_null = td.plies_from_null;
  u.hash = td.hash_key;
  if (td.enpassant != no_sq)
    td.hash_key ^= enpassant_keys[td.enpassant];
  td.enpassant = no_sq;
  td.hash_key ^= side_key;
  td.side ^= 1;
  // The null move does not advance the fifty-move counter (05/10/2026).
  td.plies_from_null = 0;
  // The network records nothing: no piece moves, and the accumulators do not depend on the side to move.
  f->move = NULL_MOVE;
  f->hctx = 0;
  f->captured = -1;
  f->cont_hist = &follow_of(td)[0][0][NO_PC][0];
  f->cont_corr = &cont_corr_of(td)[NO_PC][0];
}

static inline void step_out_null(ThreadData &td, NullUndo &u) {
  td.side ^= 1;
  td.enpassant = u.enpassant;
  td.fifty = u.fifty;
  td.plies_from_null = u.plies_from_null;
  td.hash_key = u.hash;
  td.ply--;
  td.repetition_index--;
}

// Draw by the fifty-move rule (unless mate) or by repetition.
// (06/10/2026, speed, same logic) drawn_here runs at every node and was out of line with a 1.1 KB frame (the move list
// of the "fifty moves in check" case). In more than 80% of the calls the reversible window is under 4 plies and
// repeated_draw does not even loop: that case now returns inline.
__attribute__((noinline)) static bool drawn_here_slow(ThreadData &td, bool in_check) {
  PROF_GUARD(prof_rep);
  if (td.fifty > 99) {
    if (!in_check)
      return true;
    moves ml[1];
    td_generate_moves(td, ml, false, false, 1);
    if (td_lists_legal() && ml->count)   // legal list: one move is enough
      return true;
    for (int i = 0; i < ml->count; i++) {
      UndoInfo u;
      td.repetition_table[++td.repetition_index] = td.hash_key;
      td.ply++;
      const bool ok = td_make_move(td, ml->moves[i], u) != 0;
      if (ok)
        td_unmake_move(td, ml->moves[i], u);
      td.ply--;
      td.repetition_index--;
      if (ok)
        return true;
    }
  }
  return repeated_draw(td);
}

static inline bool drawn_here(ThreadData &td, bool in_check) {
  if (td.fifty <= 99) {   // same window as repeated_draw: under 4 plies no repetition is possible
    int end = td.fifty < td.plies_from_null ? td.fifty : td.plies_from_null;
    if (end > td.repetition_index)
      end = td.repetition_index;
    if (end < 4)
      return false;
  }
  return drawn_here_slow(td, in_check);
}

// InsufMaterial (10/10/2026; rewritten 15/10/2026): neither side can force mate. Only kings and minor pieces: with at
// most one minor it is a draw (KvK, KNvK, KBvK); with one minor each it is a draw if no king is in a corner (with a
// king in a corner mates exist, though not forced); with two minors on one side it is a draw only for two bishops on
// the same colour (KNNvK and KBBvK on different colours are not, KBNvK wins). The idea of closing the node this way
// is common to several engines; our code counts the minors per side. The network gives these
// positions 3-15 cp: the rule closes the node with an exact draw.
static inline bool insufficient_material(const ThreadData &td) {
  if (td.bitboards[P] | td.bitboards[p] | td.bitboards[R] | td.bitboards[r] | td.bitboards[Q] | td.bitboards[q])
    return false;
  const U64 white_minors = td.bitboards[N] | td.bitboards[B], black_minors = td.bitboards[n] | td.bitboards[b];
  const int nw = count_bits(white_minors), nb = count_bits(black_minors);
  if (nw + nb <= 1)
    return true;
  if (nw + nb > 2)
    return false;
  if (nw == 1) {   // one minor each
    constexpr U64 corners = (1ULL << 0) | (1ULL << 7) | (1ULL << 56) | (1ULL << 63);
    return !((td.bitboards[K] | td.bitboards[k]) & corners);
  }
  const U64 pair = nw ? white_minors : black_minors;   // the two minors of the same side
  if (pair & (td.bitboards[N] | td.bitboards[n]))
    return false;
  constexpr U64 light = 0xAA55AA55AA55AA55ULL;   // one colour of the board (a8 = 0); either colour works here
  return !(pair & light) || !(pair & ~light);
}

static bool has_legal_move(ThreadData &td) {
  moves ml[1];
  td_generate_moves(td, ml, false);
  if (td_lists_legal())   // the list is already legal
    return ml->count != 0;
  for (int i = 0; i < ml->count; i++) {
    UndoInfo u;
    td.repetition_table[++td.repetition_index] = td.hash_key;
    td.ply++;
    const bool ok = td_make_move(td, ml->moves[i], u) != 0;
    if (ok)
      td_unmake_move(td, ml->moves[i], u);
    td.ply--;
    td.repetition_index--;
    if (ok)
      return true;
  }
  return false;
}

static inline void pv_extend(ThreadData &td, int m) {
  const int ply = td.ply;
  td.pv_table[ply][ply] = m;
  const int child_len = td.pv_length[ply + 1];
  for (int i = ply + 1; i < child_len; i++)
    td.pv_table[ply][i] = td.pv_table[ply + 1][i];
  td.pv_length[ply] = child_len > ply + 1 ? child_len : ply + 1;
}

// ============================================================================
// QUIESCENCE (block 5 in our form, 17/10/2026)
// A node of the quiescence search has four parts:
//   1. early exits: draws (repetition ahead, fifty moves, insufficient material) and the hash;
//   2. the stand: out of check, the corrected evaluation is a value the side to move can claim without moving;
//   3. the walk: tactical moves (all evasions when in check); qs_worth decides for each one whether it is searched,
//      and may raise the node's value with what a skipped move was certainly worth;
//   4. the close: mate or stalemate when nothing was played, the value pulled toward beta, the hash entry.
// Levers (01_params.inc): QsFut0-3, QsSee0-3, QsGate, QsVictimW, QsSeeFutW, QsFhMix, QsStandMix. At their
// defaults the tree is the one of the 8.0 quiescence.
// ============================================================================

// What the walk knows about the node.
struct QsWalk {
  int ex;         // expert of the node (piece count band of the network)
  int fut_base;   // stand + futility margin of the expert; -infinity in check (nothing is pruned on value)
  int prev_to;    // target square of the previous move: a recapture there is never pruned on value
  C2Mat cm;       // C2SwitchQs: material and experts of the node (written only when the lever is on)
  int corr_abs = 0;   // QsSeeCorr (R19): |correction| of the node in evaluation units, 0 in check
};

// Material balance for the side to move, with the search's piece values scaled to evaluation units (QsMatStand).
static inline int qs_material(const ThreadData &td) {
  const int m = (count_bits(td.bitboards[P]) - count_bits(td.bitboards[p])) * SeeValP
              + (count_bits(td.bitboards[N]) - count_bits(td.bitboards[n])) * SeeValN
              + (count_bits(td.bitboards[B]) - count_bits(td.bitboards[b])) * SeeValB
              + (count_bits(td.bitboards[R]) - count_bits(td.bitboards[r])) * SeeValR
              + (count_bits(td.bitboards[Q]) - count_bits(td.bitboards[q])) * SeeValQ;
  return (td.side == white ? m : -m) * QsMatScale / 1024;   // in evaluation units
}

// The estimate QsMatStand compares with beta. QsMatMode 0: the material balance. QsMatMode 1: from the parent, the
// parent's corrected evaluation seen from this side, minus what the move that led here took (the captured piece, the
// gain of a promotion); it carries the positional part the material lacks. No estimate (no cut) when the parent was in
// check (its evaluation is a copy from two plies up) or came from a null move.
static inline int qs_stand_estimate(const ThreadData &td, const NodeFrame *f) {
  if (!QsMatMode)
    return qs_material(td);
  const NodeFrame &par = f[-1];
  if (par.in_check || !move_ok(par.move))
    return -infinity;
  int taken = 0;
  if (par.captured >= 0) {
    const int t = par.captured % 6;
    taken = t == P ? SeeValP : t == N ? SeeValN : t == B ? SeeValB : t == R ? SeeValR : t == Q ? SeeValQ : 0;
  }
  if (get_move_promoted(par.move))
    taken += SeeValQ - SeeValP;
  return -par.static_eval - taken * QsMatScale / 1024;
}

#ifdef TRIUMV_QSMAT_STATS
// Counters of QsMatStand (statistics build only): fresh evaluations of non-PV quiescence stands, shortcuts taken, and
// shortcuts where the corrected network evaluation would have been below beta (the node would not have cut).
#include <atomic>
static std::atomic<long long> g_qsm_evals{0}, g_qsm_cuts{0}, g_qsm_wrong{0};
static struct QsMatReport {
  ~QsMatReport() {
    if (g_qsm_evals)
      fprintf(stderr, "QsMatStand: fresh evals %lld, shortcuts %lld (%.2f%%), network below beta %lld (%.2f%% of the "
              "shortcuts)\n", g_qsm_evals.load(), g_qsm_cuts.load(), 100.0 * g_qsm_cuts / g_qsm_evals,
              g_qsm_wrong.load(), g_qsm_cuts ? 100.0 * g_qsm_wrong / g_qsm_cuts : 0.0);
  }
} g_qsm_report;
#endif

// Pull a value that lies above beta toward beta by a mix weight (/1024); 1024 keeps the value.
static inline int qs_pull(int value, int beta, int mix) { return (mix * value + (1024 - mix) * beta) / 1024; }

// Is move m worth a search? 'seen' counts the legal moves met so far, m included. A skipped plain capture can still
// raise 'best' to the value it was sure to reach. While the side to move is being mated, every move is searched.
static inline bool qs_worth(ThreadData &td, const QsWalk &w, int m, bool gives, int seen, int alpha, int &best) {
  if (val_lost(best))
    return true;
  // R19 B3 (QsSeeHist off at 0, QsSeeCorr on since 03/11/2026): both exchange tests below move by `ease`: the
  // capture's history (QsSeeHist: a capture
  // that has often worked may lose a little material) and the node's |correction|, by at most 256 (QsSeeCorr: where the
  // network misjudges the position, a capture that looks a little unsound is still tried). Elsewhere the gap, the
  // history and the correction make one threshold; here the gap has its own test, so the shift applies to both. Our
  // corrections are a few evaluation units against exchange steps of hundreds (the first step below -74 is a pawn,
  // about -208), so QsSeeCorr goes well above 1024 (03/11/2026: on the last test only, x16 left the bench unchanged).
  int ease = 0;
  if (QsSeeHist && is_tactical(m))
    ease += td.capt_hist[move_piece(m)][move_to(m)][victim_kind(td, m)] * QsSeeHist / 65536;
  if (QsSeeCorr)
    ease += std::min(w.corr_abs * QsSeeCorr / 1024, 256);
  // A plain capture: no check, no promotion, not a recapture, and a futility base that is a real value.
  const bool plain = !gives && move_to(m) != w.prev_to && !get_move_promoted(m) && !val_lost(w.fut_base);
  if (plain) {
    if (seen > QsGate)
      return false;
    // The victim at its per-expert value (see.cpp, 28/10/2026; e.p. reads code 15, value 0 as before).
    const int victim = piece_values_mat(td)[td.piece_on[move_to(m)] & 15] +
                       pawn_struct_adj(td, move_to(m), td.piece_on[move_to(m)]);   // pawn structure (see.h)
    const int fut = w.fut_base + victim * QsVictimW / 1024;
    // C2 idea 2 (21/10/2026, agent A; OFF by default, SPRT pending): a capture that gives another expert to either
    // perspective is judged by another learned regime after the move, so the stand plus the victim is a weaker bound:
    // the test needs C2SwitchQs more (the value the skipped move raises 'best' to stays the plain one).
    const int c2_wide = (C2SwitchQs && c2_switches(td, w.cm, m)) ? C2SwitchQs : 0;
    if (fut + c2_wide <= alpha) {
      best = std::max(best, fut);
      return false;
    }
    if (!see_holds(td, m, (alpha - w.fut_base) * QsSeeFutW / 1024 - ease)) {   // the exchange cannot close the gap
      best = std::max(best, std::min(alpha, w.fut_base));
      return false;
    }
  }
  if (!is_tactical(m))   // quiet evasions are searched only while being mated (above)
    return false;
  return see_holds(td, m, -by_exp4(w.ex, QsSee0, QsSee1, QsSee2, QsSee3) - ease);
}

// QCHK (AnQsChecks, analysis mode, 09/10/2026, our idea): at the first quiescence ply called by the main search, quiet
// moves that give check with SEE >= 0 are tried after the tactical ones; the child (in check) is a normal quiescence.
// QCHK = false is the plain quiescence.
template <bool QCHK = false>
static int quiesce(ThreadData &td, NodeFrame *f, int alpha, int beta, bool pv) {
  // ---- 1. Early exits -------------------------------------------------------
  td.pv_length[td.ply] = td.ply;
  // Expert holds: quiescence is below any depth limit (C2HoldDepth); C2HoldQs holds here only.
  f->nn_hold = (unsigned char)(C2HoldDepth || C2HoldQs);
  if (alpha < contempt(td) && td_upcoming_repetition(td)) {   // a repetition is in reach: the draw is a floor
    alpha = draw_score(td);
    if (alpha >= beta)
      return alpha;
  }
  if (pv && td.sel_depth < td.ply + 1)
    td.sel_depth = td.ply + 1;
  const bool in_check = f->in_check;
  if (td.ply >= max_ply)
    return in_check ? 0 : static_evaluate(td);
  if (drawn_here(td, in_check))
    return contempt(td);
  if (f[-1].captured >= 0 && insufficient_material(td))   // material changes only with a capture
    return contempt(td);

  HashView h;
  hash_read(td, h);
  f->tt_hit = h.hit;
  const bool hash_pv = h.hit && h.pv;
  if (!pv && h.depth >= hd_qs() && val_known(h.value) && bound_covers(h.bound, h.value >= beta))
    return h.value;

  // ---- 2. The stand ---------------------------------------------------------
  QsWalk w;
  w.ex = search_band(td);
  int best, eval_raw = tt_eval_none;
  if (in_check) {
    best = w.fut_base = -infinity;
  } else {
    const int signal = corr_signal(td, f);
    // QsMatStand (29/10/2026, OFF by default): stand on the material when the network would be called and the
    // material alone, minus the margin, already reaches beta (01_params.inc).
    const bool fresh = !(h.hit && h.eval_raw != tt_eval_none);
    const bool mat_cut = QsMatStand && !pv && fresh && qs_stand_estimate(td, f) - QsMatStand >= beta;
#ifdef TRIUMV_QSMAT_STATS
    if (!pv && fresh)
      g_qsm_evals++;
    if (mat_cut) {
      g_qsm_cuts++;
      int raw_unused;
      const int ev = corrected_eval(node_eval(td, h, raw_unused), signal + corr_signal_extra(td));
      if (ev < beta)
        g_qsm_wrong++;
    }
    // The statistics build only counts: the tree stays the one of QsMatStand 0, so the shares are not distorted by
    // the cuts themselves (a wrong cut changes the lines that follow).
#else
    if (mat_cut)
      return beta;
#endif
    if (C2PkStale)   // quiescence: a pawn-king miss reuses the last outputs (01_params.inc)
      nn_set_pk_stale(td.nnpos, 1);
    const int plain = node_eval(td, h, eval_raw);
    // CorrNetW (OFF): the correction by hidden state, when the network has just evaluated this very position.
    const int net_x = (CorrNetW && fresh) ? CorrNetW * corr_net_sum(td, nn_x1_signs(td.nnpos)) : 0;
    f->static_eval = best = corrected_eval(plain, signal + corr_signal_extra(td) + net_x);
    // A hash bound on the right side is a better estimate than the evaluation.
    if (h.hit && val_known(h.value) && !val_decided(h.value) && bound_covers(h.bound, h.value > best))
      best = h.value;
    if (best >= beta) {   // standing still is already enough
      if (!val_decided(best))
        best = qs_pull(best, beta, QsFhMix);
      if (!h.hit)
        hash_write(td, VAL_NONE, false, BOUND_LOW, HD_EVAL, 0, eval_raw);
      return best;
    }
    alpha = std::max(alpha, best);
    w.fut_base = f->static_eval + by_exp4(w.ex, QsFut0, QsFut1, QsFut2, QsFut3);
    if (QsSeeCorr)
      w.corr_abs = std::abs(signal + corr_signal_extra(td)) / CorrEvalDiv;
    if (C2SwitchQs)
      c2_mat_init(td, w.cm);
  }

  // ---- 3. The walk ----------------------------------------------------------
  w.prev_to = move_ok(f[-1].move) ? move_to(f[-1].move) : -1;
  MoveQueue q;
  queue_start(q, td, f, h.move, 0, in_check ? QK_EVADE : QK_QUIESCE);
  MoveSafety safety;
  safety.ready = false;
  int best_move = 0, seen = 0, m;
  while ((m = queue_next(q)) != 0) {
    const bool gen_legal = queue_move_legal(q, m);
    if (!gen_legal && !legal_before(td, safety, m))
      continue;
    const bool gives = checks_before(td, safety, m);
    if (!qs_worth(td, w, m, gives, ++seen, alpha, best))
      continue;

    UndoInfo u;
    if (!step_in(td, f, m, u, gen_legal)) {   // illegal after all: it does not count as seen
      seen--;
      continue;
    }
    f[1].in_check = gives;
    const int value = -quiesce<false>(td, f + 1, -beta, -alpha, pv);
    step_out(td, m, u);

    if (value <= best)
      continue;
    best = value;
    if (value <= alpha)
      continue;
    best_move = m;
    if (pv)
      pv_extend(td, m);
    if (value >= beta)
      break;   // (QsFhCapt, a capture-history reward for a quiescence fail high, closed negative, removed 04/11/2026)
    alpha = value;
  }
  int count = seen;

  if constexpr (QCHK) {
    if (!in_check && best < beta) {
      moves ml[1];
      td_generate_moves(td, ml, /*captures_only=*/false, /*quiets_only=*/true, /*known_in_check=*/0);
      const bool list_legal = td_lists_legal();
      for (int i = 0; i < ml->count && best < beta; i++) {
        const int qm = ml->moves[i];
        if (is_tactical(qm) || qm == q.hash_move || (!list_legal && !legal_before(td, safety, qm)) ||
            !checks_before(td, safety, qm) || !see_holds(td, qm, 0))
          continue;
        UndoInfo u;
        if (!step_in(td, f, qm, u, list_legal))
          continue;
        count++;
        f[1].in_check = true;
        const int value = -quiesce<false>(td, f + 1, -beta, -alpha, pv);
        step_out(td, qm, u);
        if (stop_threads.load(std::memory_order_relaxed))
          return 0;
        if (value > best) {
          best = value;
          if (value > alpha) {
            best_move = qm;
            if (pv)
              pv_extend(td, qm);
            if (value < beta)
              alpha = value;
          }
        }
      }
    }
  }

  // ---- 4. The close ---------------------------------------------------------
  if (!count) {
    if (in_check)
      return mated_at(td.ply);
    // Nothing was played and quiet moves are not generated here, so a stalemate can hide behind the stand. It is
    // looked for only when it is possible at all: besides the king, only blocked pawns, and the last move took a
    // piece (not a pawn).
    const int us = td.side, off = us == white ? 0 : 6;
    const U64 pawns = td.bitboards[P + off];
    const U64 pushes = us == white ? pawns >> 8 : pawns << 8;
    const U64 non_pawn = td.occupancies[us] & ~pawns & ~td.bitboards[K + off];
    const bool took_piece = f[-1].captured >= 0 && f[-1].captured % 6 >= N;
    if (!(pushes & ~td.occupancies[both]) && !non_pawn && took_piece && !has_legal_move(td))
      best = contempt(td);
  }
  if (!val_decided(best) && best > beta)
    best = qs_pull(best, beta, QsStandMix);
  hash_write(td, best, hash_pv, best >= beta ? BOUND_LOW : BOUND_UP, hd_qs(), best_move, eval_raw);
  return best;
}

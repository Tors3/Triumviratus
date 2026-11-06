// search/06_nndirty.inc -- The network's dirty entries, written by the make (07/10/2026, single board v2).
// Part of threads.cpp: it is NOT a translation unit of its own. It must be included only by threads.cpp, before
// 07_makemove.inc.
//
// The make writes on the stack of the network handle (NnStack, nn_dirty.h; td.nnstack) one state per move with only the
// moved piece (NnDirtyPiece), in the engine's numbering; the unmake removes it. Touched pawns (NnDirtyPawns) and the threat
// tuples that change (NnDirtyThreats) are computed only when an evaluation is reached (nn_dirty_catch_up),
// for the states that lack them: many nodes end before evaluating (TT cutoffs, check, pruning) and do not
// pay for them. The network reads the dirty entries as they are and updates the accumulators (consilium/cn.cpp); it has
// no board of its own.
//
// The catch-up derives the board before the first pending move from the thread's board, undoing the pending
// moves on local copies (bitboards, mailbox), then redoes the moves forward. Every move (nn_threat_steps) is
// broken down into the steps of a make (remove, put, move, replace a piece) on the local bitboards and for every step
// the tuples that change are emitted, on the state of that step. The pieces of the squares read are taken from the mailbox
// BEFORE the move: the reads always fall on squares occupied in the state of the step and not touched by the move (the
// sliders, the targets, the attackers; never the square of the step itself). The nnperft check guarantees it (whole
// accumulators compared with the refresh at every evaluation).
// xperf measurements (07/10/2026, DEVELOPMENT_8.0 §28): computing the threats in every make cost +0.9-1.2% cycles per
// node compared with the old lazy copy of the board; this catch-up, with the branchless finny-table
// comparison, gives -0.4% (middlegame) and -0.7% (endgames) compared with the copy.

#if defined(USE_AVX512) && !defined(TRIUMV_NO_THREATS_VECTOR_F)
  #define NN_THREATS_VECTOR
  #include <immintrin.h>
#endif

// Ray from s1 through s2 to the edge (s2 included, s1 excluded); 0 if the squares are not aligned or coincide.
// Stored as [s2][s1] (05/11/2026, R26 §6, same values): the seers of one square (s2 fixed, the seer s1 varying:
// nn_piece_threats) read one 512-byte row of the 32 KB table instead of a line in a different row each.
// NN_RAY(s1, s2) is the ray from s1 through s2.
alignas(64) static U64 nn_ray_pass[64][64];
#define NN_RAY(s1, s2) nn_ray_pass[(s2)][(s1)]

// (Tables of the graft blocks nn_pr_* and nn_ps_*, with nn_graft_tables_init: removed on 10/10/2026 with the blocks,
// code in _backup/Triumviratus_8.0_pre_rimozione_graft_2026-10-10 and in the training repo,
// 04_consilium/graft_engine_storico.)

static void nn_dirty_init() {
  for (int s1 = 0; s1 < 64; s1++)
    for (int s2 = 0; s2 < 64; s2++) {
      const U64 b1 = 1ULL << s1, b2 = 1ULL << s2;
      U64 r = 0;
      if (get_rook_attacks(s1, 0) & b2)
        r = get_rook_attacks(s1, 0) & (get_rook_attacks(s2, b1) | b2);
      else if (get_bishop_attacks(s1, 0) & b2)
        r = get_bishop_attacks(s1, 0) & (get_bishop_attacks(s2, b1) | b2);
      NN_RAY(s1, s2) = r;
    }
}


// Working board of the steps: bitboards per type (P..K, both colours), per colour and occupancy.
struct NnTB {
  U64 type[6];
  U64 color[2];
  U64 occ;
};

static inline void nn_tb_toggle(NnTB &tb, int pc, U64 b) {
  tb.type[pc >= 6 ? pc - 6 : pc] ^= b;
  tb.color[pc >= 6] ^= b;
  tb.occ ^= b;
}

// Reads the piece on a square (code 0..11, -1 = empty) from the local mailbox of the catch-up, which is always the one
// before the move being processed.
struct NnLookMailbox {
  const int *mb;
  int operator()(int s) const { return mb[s]; }
#ifdef NN_THREATS_VECTOR
  __m512i chunk(int c) const { return _mm512_loadu_si512(mb + 16 * c); }
#endif
};

// Byte board, avx512icl only (05/11/2026, R26 §6, same tuples; without VBMI2 the 16-byte chunks cost more
// instructions than they save, measured on the Xeon): the catch-up builds the board before the pending moves in one
// register from the byte mailbox td.pc8 (one aligned line, written by the make) and keeps it in a 64-byte aligned
// buffer written by one vector store per state. Before, 256 bytes of td.piece_on (int, five lines from offset 184 of
// ThreadData, every 64-byte load across two) were copied, patched with scalar stores and read back at once by 64-byte
// vector loads, which cannot take their data from the narrower stores and wait for them to reach L1. Every read of
// the buffer is served by its single store.
#if defined(USE_AVX512ICL) && defined(NN_THREATS_VECTOR)
  #define NN_MB8
struct NnLookBoard8 {
  const std::int8_t *mb;
  int operator()(int s) const { return mb[s]; }
  __m512i board() const { return _mm512_load_si512(mb); }   // read by the vpcompressb emission only
};
// Mask of one square, 0 for NN_SQ_NONE (64).
static inline __mmask64 nn_sq_mask(int sq) { return (__mmask64)((U64)(sq != NN_SQ_NONE) << (sq & 63)); }
// The byte board before (undo) and after (redo) a move: the writes of nn_mb_undo and nn_mb_redo, in the same order.
static inline __m512i nn_b8_undo(__m512i b, const NnDirtyPiece &dp) {
  b = _mm512_mask_mov_epi8(b, nn_sq_mask(dp.to) | nn_sq_mask(dp.add_sq), _mm512_set1_epi8(-1));
  b = _mm512_mask_mov_epi8(b, nn_sq_mask(dp.from), _mm512_set1_epi8((char)dp.pc));
  return _mm512_mask_mov_epi8(b, nn_sq_mask(dp.remove_sq), _mm512_set1_epi8((char)dp.remove_pc));
}
static inline __m512i nn_b8_redo(__m512i b, const NnDirtyPiece &dp) {
  b = _mm512_mask_mov_epi8(b, nn_sq_mask(dp.from) | nn_sq_mask(dp.remove_sq), _mm512_set1_epi8(-1));
  b = _mm512_mask_mov_epi8(b, nn_sq_mask(dp.to), _mm512_set1_epi8((char)dp.pc));
  return _mm512_mask_mov_epi8(b, nn_sq_mask(dp.add_sq), _mm512_set1_epi8((char)dp.add_pc));
}
#endif

static inline void nn_add_threat(NnDirtyThreats *dts, bool add, int pc, int tpc, int pcSq, int tSq) {
  dts->list[dts->n++] = nn_threat(pc, tpc, pcSq, tSq, add);
}

// A slider threatening a queen is a feature only if it is a queen itself (the bishop/rook ->
// queen pairs are excluded from the feature table).
static inline bool nn_can_slider_threat(int pc, int slider) {
  return pc % 6 != NN_QUEEN || slider % 6 == NN_QUEEN;
}

#ifdef NN_THREATS_VECTOR
// Vector emission (AVX-512 F/BW): every group of 16 squares is compacted with vpcompressd on its 16 bits of the
// mask, with the square and the piece already in their place in the tuple. The 16-entry writes beyond the count fall
// into the 16 spare entries of the list (nn_dirty.h). Four fixed groups: no branch depends on the mask. In
// our numbering group c is the squares 16c..16c+15, contiguous in the mailbox too.
template <int SqShift, int PcShift, typename Look>
static inline void nn_write_multi(const Look &look, U64 mask, std::uint32_t tmpl, NnDirtyThreats *dts) {
  if (!mask) return;
  std::uint32_t *write = dts->list + dts->n;
  dts->n += count_bits(mask);
  alignas(64) static constexpr std::uint32_t lane[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const __m512i lanev = _mm512_load_si512(lane);
  const __m512i template_v = _mm512_set1_epi32(int(tmpl));
  for (int c = 0; c < 4; c++) {
    const __mmask16 m = __mmask16(mask >> (16 * c));
    const __m512i pieces = _mm512_slli_epi32(look.chunk(c), PcShift);
    const __m512i squares = _mm512_slli_epi32(_mm512_add_epi32(lanev, _mm512_set1_epi32(16 * c)), SqShift);
    const __m512i dirties = _mm512_ternarylogic_epi32(template_v, squares, pieces, 254 /* A | B | C */);
    _mm512_storeu_si512(write, _mm512_maskz_compress_epi32(m, dirties));
    write += count_bits((U64)m);
  }
}
#if defined(NN_MB8)   // avx512icl: VBMI2 always there
// Byte board with VBMI2 (avx512icl): the squares (0..63) and the pieces are compacted with the same 64-bit mask by
// vpcompressb in registers, so the pieces come out in the order of the squares; then one widening and one store per
// 16 tuples. A mask has at most 18 squares (8 knights, 8 line pieces, 2 pawns attacking one square), so the second
// group is rare and the other two never run; their writes stay inside the spare entries of the list.
template <int SqShift, int PcShift>
static inline void nn_write_multi(const NnLookBoard8 &look, U64 mask, std::uint32_t tmpl, NnDirtyThreats *dts) {
  if (!mask) return;
  std::uint32_t *write = dts->list + dts->n;
  const int cnt = count_bits(mask);
  dts->n += cnt;
  alignas(64) static constexpr std::int8_t iota[64] = {
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
      22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43,
      44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63};
  const __m512i sq = _mm512_maskz_compress_epi8((__mmask64)mask, _mm512_load_si512(iota));
  const __m512i pc = _mm512_maskz_compress_epi8((__mmask64)mask, look.board());
  const __m512i template_v = _mm512_set1_epi32(int(tmpl));
  const auto put = [&](__m128i s16, __m128i p16, std::uint32_t *w) {
    const __m512i s = _mm512_slli_epi32(_mm512_cvtepu8_epi32(s16), SqShift);
    const __m512i p = _mm512_slli_epi32(_mm512_cvtepu8_epi32(p16), PcShift);
    _mm512_storeu_si512(w, _mm512_ternarylogic_epi32(template_v, s, p, 254 /* A | B | C */));
  };
  put(_mm512_castsi512_si128(sq), _mm512_castsi512_si128(pc), write);
  if (cnt > 16) {
    put(_mm512_extracti32x4_epi32(sq, 1), _mm512_extracti32x4_epi32(pc, 1), write + 16);
    if (cnt > 32) {
      put(_mm512_extracti32x4_epi32(sq, 2), _mm512_extracti32x4_epi32(pc, 2), write + 32);
      if (cnt > 48)
        put(_mm512_extracti32x4_epi32(sq, 3), _mm512_extracti32x4_epi32(pc, 3), write + 48);
    }
  }
}
#endif
#endif

// AA1 (10/10/2026, speed, same tuples): the slider attacks of the piece are those nn_piece_threats has already
// computed from the same square with the same occupancy (bAttacks, rAttacks): before, they were recomputed here, once for
// bishop and rook and twice for the queen, on top of the 2 in nn_piece_threats (counters, bench 15 on fens30: 1.19 calls and
// 3.05 DualMagic computations per node; now 1 per call).
static inline U64 nn_piece_attacks(int pc, int s, U64 bAttacks, U64 rAttacks) {
  switch (pc >= 6 ? pc - 6 : pc) {
  case NN_PAWN: return pawn_attacks[pc >= 6][s];
  case NN_KNIGHT: return knight_attacks[s];
  case NN_BISHOP: return bAttacks;
  case NN_ROOK: return rAttacks;
  default: return bAttacks | rAttacks;  // NN_QUEEN
  }
}

// The threat tuples that change when piece pc appears on s (put) or leaves it (!put), on the board `tb` of that
// moment. Which pairs are features at all is fixed by the input features of the network: kings are never threatened,
// a pawn threatens only knights and rooks and is counted as an attacker only of knights and rooks, a bishop or rook
// threatens no queen, a line piece threatens a queen only if it is a queen. Written 20/10/2026 in three groups (the
// order of the tuples does not matter, the accumulator adds them up; nnperft compares every accumulator with a
// refresh):
// - lines through s (Lines = true): a line piece that sees s also saw, or now sees, the first piece behind s on the
//   same line; that threat closes when pc arrives and opens when it leaves (kings excluded as targets). A line that
//   contains the whole move (from | to, `whole_move`) is skipped: there nothing opens or closes;
// - outgoing: the pieces pc attacks from s;
// - incoming: the pieces attacking s that make a feature with pc (knights, pawns for a knight or rook, line pieces).
// The king has only the first group. A capture leaves out two kinds of tuples that the next step would take back at
// once (nn_threat_steps): the lines of the seers in `skip_seers`, and the outgoing and incoming threats with the
// pieces on `skip_pairs`.
template <bool Lines, typename Look>
static inline void nn_piece_threats(const NnTB &tb, const Look &look, int pc, bool put, int s, NnDirtyThreats *dts,
                                    U64 whole_move = ~0ULL, U64 skip_seers = 0, U64 skip_pairs = 0,
                                    const SliderPair *pre = nullptr) {
  const int kind = pc >= 6 ? pc - 6 : pc;
  // AA1: both lines from one lookup (magic.h); `pre`: the caller already has them (nn_step_swap).
  const SliderPair sp = pre ? *pre : get_slider_pair(s, tb.occ);
  const U64 diag = pair_bishop(sp), orth = pair_rook(sp);
  const U64 targets_any = tb.occ & ~tb.type[NN_KING];
  const U64 seers = (diag & (tb.type[NN_BISHOP] | tb.type[NN_QUEEN])) | (orth & (tb.type[NN_ROOK] | tb.type[NN_QUEEN]));

  if constexpr (Lines) {
    for (U64 rest = seers & ~skip_seers; rest; rest &= rest - 1) {
      const int from = get_ls1b_index(rest);
      const U64 line = NN_RAY(from, s);
      const U64 behind = line & (diag | orth) & targets_any;
      if (!behind || (line & whole_move) == whole_move)
        continue;
      const int target = get_ls1b_index(behind);
      const int seer = look(from), victim = look(target);
      if (nn_can_slider_threat(victim, seer))
        nn_add_threat(dts, !put, seer, victim, from, target);
    }
  }
  if (kind == NN_KING)
    return;

  U64 outgoing = nn_piece_attacks(pc, s, diag, orth) & targets_any;
  if (kind == NN_PAWN)
    outgoing &= tb.type[NN_KNIGHT] | tb.type[NN_ROOK];
  else if (kind == NN_BISHOP || kind == NN_ROOK)
    outgoing &= tb.type[NN_PAWN] | tb.type[NN_KNIGHT] | tb.type[NN_BISHOP] | tb.type[NN_ROOK];

  U64 incoming = (knight_attacks[s] & tb.type[NN_KNIGHT]) | (kind == NN_QUEEN ? seers & tb.type[NN_QUEEN] : seers);
  if (kind == NN_KNIGHT || kind == NN_ROOK)
    incoming |= (pawn_attacks[white][s] & tb.type[NN_PAWN] & tb.color[black]) |
                (pawn_attacks[black][s] & tb.type[NN_PAWN] & tb.color[white]);
  outgoing &= ~skip_pairs;
  incoming &= ~skip_pairs;

#ifdef NN_THREATS_VECTOR
  nn_write_multi<NN_THR_TSQ, NN_THR_TPC>(look, outgoing, nn_threat(pc, 0, s, 0, put), dts);
  nn_write_multi<NN_THR_PCSQ, NN_THR_PC>(look, incoming, nn_threat(0, pc, 0, s, put), dts);
#else
  for (; outgoing; outgoing &= outgoing - 1) {
    const int t = get_ls1b_index(outgoing);
    nn_add_threat(dts, put, pc, look(t), s, t);
  }
  for (; incoming; incoming &= incoming - 1) {
    const int a = get_ls1b_index(incoming);
    nn_add_threat(dts, put, look(a), pc, a, s);
  }
#endif
}

// The steps of a make, each with its tuples. Remove: the threats are computed before, with the piece still there.
// Put: after. Move: before and after, skipping the uncovered rays that contain from|to. Replace (capture): the
// old piece leaves and the new one enters with the square always occupied, without uncovered rays; the lines from s do
// not contain s, so both pieces see the same slider attacks: one computation for the two (31/10/2026, speed, same
// tuples; counters on 30 positions x 400k nodes: slider lookups of the catch-up 1.20 -> 1.05 per node in the
// middlegame, 1.19 -> 1.05 in endgames).
template <typename Look>
static inline void nn_step_remove(NnTB &tb, const Look &look, int pc, int s, NnDirtyThreats *dts,
                                  U64 skip_seers = 0) {
  nn_piece_threats<true>(tb, look, pc, false, s, dts, ~0ULL, skip_seers);
  nn_tb_toggle(tb, pc, 1ULL << s);
}
template <typename Look>
static inline void nn_step_put(NnTB &tb, const Look &look, int pc, int s, NnDirtyThreats *dts) {
  nn_tb_toggle(tb, pc, 1ULL << s);
  nn_piece_threats<true>(tb, look, pc, true, s, dts);
}
template <typename Look>
static inline void nn_step_move(NnTB &tb, const Look &look, int pc, int from, int to, NnDirtyThreats *dts) {
  const U64 fromTo = (1ULL << from) | (1ULL << to);
  nn_piece_threats<true>(tb, look, pc, false, from, dts, fromTo);
  nn_tb_toggle(tb, pc, fromTo);
  nn_piece_threats<true>(tb, look, pc, true, to, dts, fromTo);
}
template <typename Look>
static inline void nn_step_swap(NnTB &tb, const Look &look, int oldPc, int newPc, int s, NnDirtyThreats *dts,
                                U64 skip_old = 0) {
  const SliderPair sp = get_slider_pair(s, tb.occ);
  nn_tb_toggle(tb, oldPc, 1ULL << s);
  nn_piece_threats<false>(tb, look, oldPc, false, s, dts, ~0ULL, 0, skip_old, &sp);
  nn_tb_toggle(tb, newPc, 1ULL << s);
  nn_piece_threats<false>(tb, look, newPc, true, s, dts, ~0ULL, 0, 0, &sp);
}

// All the tuples of a move, from the board before the move (tb, which leaves in the state after the move).
template <typename Look>
static inline void nn_threat_steps(NnTB &tb, const Look &look, const NnDirtyPiece &dp, NnDirtyThreats *dts) {
  dts->n = 0;
  const int pc = dp.pc, from = dp.from;
  if (dp.to != NN_SQ_NONE && dp.add_sq != NN_SQ_NONE) {
    // (29/10/2026, speed, same accumulators) Castling on four different squares (always in chess, often in Chess960):
    // a king move, then a rook move, each skipping the lines it closes and reopens itself. Removing both and putting
    // both back opened and closed again 1.4 tuples per castling move (bench), two rows per perspective each.
    if (count_bits((1ULL << from) | (1ULL << dp.to) | (1ULL << dp.remove_sq) | (1ULL << dp.add_sq)) == 4) {
      nn_step_move(tb, look, pc, from, dp.to, dts);
      nn_step_move(tb, look, dp.remove_pc, dp.remove_sq, dp.add_sq, dts);
      return;
    }
    // Otherwise (Chess960, the squares overlap): first king and rook are removed, then put back.
    nn_step_remove(tb, look, pc, from, dts);
    nn_step_remove(tb, look, dp.remove_pc, dp.remove_sq, dts);
    nn_step_put(tb, look, pc, dp.to, dts);
    nn_step_put(tb, look, dp.add_pc, dp.add_sq, dts);
    return;
  }
  const bool promo = dp.add_sq != NN_SQ_NONE;
  const int to = promo ? dp.add_sq : dp.to;
  const int arriving = promo ? dp.add_pc : pc;
  if (dp.remove_sq != NN_SQ_NONE) {
    if (dp.remove_sq != to) {  // en passant: first the captured pawn, then the moving pawn
      nn_step_remove(tb, look, dp.remove_pc, dp.remove_sq, dts);
      nn_step_move(tb, look, pc, from, to, dts);
    } else {
      // (29/10/2026, speed, same accumulators) A capture along a line: when the mover leaves `from`, the piece on
      // that line behind it (X, beyond `from` seen from `to`) gets a line onto the victim, and a line victim gets
      // one onto X; when the victim leaves `to`, both tuples are taken back (two rows per perspective each, for
      // nothing). So the mover leaving skips the seers on the line (`ray` and `to`), and the victim leaving skips its
      // threats with the piece on `ray` (only X can be there: the squares between are empty, `from` is empty by
      // then; knights and pawns are never on it). Nothing else changes: nnperft and nnueverify compare the
      // accumulators with a refresh. No line (a knight capture): `ray` is 0 and the piece on `to` cannot see `from`,
      // so nothing is skipped.
      const U64 ray = NN_RAY(to, from);
      nn_step_remove(tb, look, pc, from, dts, ray | (1ULL << to));
      nn_step_swap(tb, look, dp.remove_pc, arriving, to, dts, ray);
    }
  } else if (promo) {
    nn_step_remove(tb, look, pc, from, dts);
    nn_step_put(tb, look, arriving, to, dts);
  } else
    nn_step_move(tb, look, pc, from, to, dts);
}

// The pawns the move removes and adds, with the pawns before (PawnPair and PassedPawns blocks). Castling touches no
// pawns: the rook is in remove_pc, not a pawn.
static inline void nn_fill_pawns(NnDirtyPawns &dpw, const NnDirtyPiece &dp, U64 whitePawns, U64 blackPawns) {
  const bool moverIsPawn = dp.pc == P || dp.pc == p;
  const bool victimIsPawn = dp.remove_sq != NN_SQ_NONE && (dp.remove_pc == P || dp.remove_pc == p);
  dpw.nRemoved = 0;
  dpw.addedSq = NN_SQ_NONE;
  dpw.any = moverIsPawn || victimIsPawn;
  if (!dpw.any)
    return;
  dpw.before[white] = whitePawns;
  dpw.before[black] = blackPawns;
  if (moverIsPawn) {
    dpw.removedSq[dpw.nRemoved] = dp.from;
    dpw.removedC[dpw.nRemoved] = dp.pc >= 6;
    dpw.nRemoved++;
    if (dp.to != NN_SQ_NONE) {  // a promotion does not put a pawn back
      dpw.addedSq = dp.to;
      dpw.addedC = dp.pc >= 6;
    }
  }
  if (victimIsPawn) {
    dpw.removedSq[dpw.nRemoved] = dp.remove_sq;  // en passant: the pawn's square, not `to`
    dpw.removedC[dpw.nRemoved] = dp.remove_pc >= 6;
    dpw.nRemoved++;
  }
}

// Key of the piece placement only (pawns + non-pawns of both colours): it does not change with the null move, the
// side to move, en passant and castling rights. It serves the synchronisation check (-DTRIUMV_VERIFY_NNSYNC).
static inline U64 nn_piece_key(const ThreadData &td) { return td.pawn_key ^ td.np_key[white] ^ td.np_key[black]; }

// Make: a new state on the stack, written on the board BEFORE the move (the make has not touched anything yet).
// cap = captured piece (-1 none), capSq = its square; rookPc = -1 if it is not castling, otherwise the rook and its
// squares rf -> rt.
static inline void nn_make_dirty(ThreadData &td, int piece, int source, int target, int promoted, int cap, int capSq,
                                 int rookPc, int rf, int rt) {
  NnStack &S = *td.nnstack;
  NnState &st = S.st[S.size++];
  st.computed[white] = st.computed[black] = 0;
  // Position in the stack (nn_stack_of: passers of PassedPawns v1, R4), in the same line as `computed`.
  st.idx = (std::uint8_t)(S.size - 1);

  NnDirtyPiece &dp = st.dp;
  dp.pc = (std::uint8_t)piece;
  dp.from = (std::uint8_t)source;
  if (rookPc >= 0) {
    dp.to = (std::uint8_t)target;
    dp.remove_pc = dp.add_pc = (std::uint8_t)rookPc;
    dp.remove_sq = (std::uint8_t)rf;
    dp.add_sq = (std::uint8_t)rt;
  } else {
    dp.to = (std::uint8_t)(promoted ? NN_SQ_NONE : target);
    dp.remove_pc = (std::uint8_t)(cap & 15);
    dp.remove_sq = (std::uint8_t)(cap != -1 ? capSq : NN_SQ_NONE);
    dp.add_pc = (std::uint8_t)promoted;
    dp.add_sq = (std::uint8_t)(promoted ? target : NN_SQ_NONE);
  }
#if defined(CN_C2_ONE_EXPERT)
  dp.phase = 0;  // cost diagnosis only (21/10/2026): one expert, no refresh on material changes
  dp.phaseChanged = 0;
#else
  // Consilium Plebis: the expert of each perspective comes from the network's routing table over the non-pawn material
  // signatures (consilium/cn.h router_sig). Only a capture or a promotion changes material, so other moves copy the
  // parent's experts (the root's state has none: computed from the board).
#ifndef CN_NO_MAKE_EXPERT_COPY
  // C2 speed (23/10/2026): a pawn taken without a promotion leaves both non-pawn signatures as they were, so the
  // experts are the parent's (no popcounts, no table); above the first move the parent's experts are its state's byte.
  const bool c2_same = S.size >= 3 && !promoted && (cap == -1 || cap % 6 == P);
#else
  const bool c2_same = false;
#endif
  if (!c2_same && (cap != -1 || promoted || S.size < 3)) {
    const unsigned char *rt = nn_router_table();
    int n[2], bsh[2], r[2], q[2];
    for (int c = 0; c < 2; c++) {
      n[c] = count_bits(td.bitboards[N + 6 * c]);
      bsh[c] = count_bits(td.bitboards[B + 6 * c]);
      r[c] = count_bits(td.bitboards[R + 6 * c]);
      q[c] = count_bits(td.bitboards[Q + 6 * c]);
    }
#ifndef CN_NO_MAKE_EXPERT_COPY
    const int ph0 = S.size >= 3 ? S.st[S.size - 2].dp.phase : -1;
    const int e0w = ph0 >= 0 ? (ph0 & 15) : rt[cn_sig(n[0], bsh[0], r[0], q[0]) * 54 + cn_sig(n[1], bsh[1], r[1], q[1])];
    const int e0b = ph0 >= 0 ? (ph0 >> 4) : rt[cn_sig(n[1], bsh[1], r[1], q[1]) * 54 + cn_sig(n[0], bsh[0], r[0], q[0])];
#else
    const int e0w = rt[cn_sig(n[0], bsh[0], r[0], q[0]) * 54 + cn_sig(n[1], bsh[1], r[1], q[1])];
    const int e0b = rt[cn_sig(n[1], bsh[1], r[1], q[1]) * 54 + cn_sig(n[0], bsh[0], r[0], q[0])];
#endif
    auto dec = [&](int pc, int d) {
      const int t = pc % 6, c = pc / 6;
      if (t == N) n[c] += d;
      else if (t == B) bsh[c] += d;
      else if (t == R) r[c] += d;
      else if (t == Q) q[c] += d;
    };
    if (cap != -1) dec(cap, -1);
    if (promoted) dec(promoted, 1);
    int e1w = rt[cn_sig(n[0], bsh[0], r[0], q[0]) * 54 + cn_sig(n[1], bsh[1], r[1], q[1])];
    int e1b = rt[cn_sig(n[1], bsh[1], r[1], q[1]) * 54 + cn_sig(n[0], bsh[0], r[0], q[0])];
    // Expert holds (25/10/2026, 01_params.inc, off): keep the parent's experts (C2HoldDepth, C2HoldQs: the frame said
    // so through td.nn_hold) while the true one is close enough. C2HoldRoot and C2HoldPv removed on 27/10/2026.
    if (td.nn_hold) {
      if (std::abs(e1w - e0w) <= C2HoldDist) e1w = e0w;
      if (std::abs(e1b - e0b) <= C2HoldDist) e1b = e0b;
    }
    dp.phase = (std::uint8_t)(e1w | e1b << 4);
    dp.phaseChanged = (std::uint8_t)((e0w != e1w) | (e0b != e1b) << 1);
  } else {
    dp.phase = S.st[S.size - 2].dp.phase;
    dp.phaseChanged = 0;
  }
#endif
  // Threats and pawns: nn_dirty_catch_up, only if an evaluation is reached.
  // Prefetch of the network rows this move changes (19/10/2026): the memory latency runs under the hash probe and the
  // rest of the child node instead of at the start of its evaluation. Without a capture the third row repeats the
  // first (no branch). The rook of a castling move is left out (rare). Only where it pays (NN_MAKE_PREFETCH,
  // consilium_bridge.h: the AVX2 builds without AVX-512).
#ifdef NN_MAKE_PREFETCH
  const bool hasCap = cap != -1;
  nn_prefetch_move(td.bitboards, dp.phase, piece, source, promoted ? promoted : piece, target, hasCap ? cap : piece,
                   hasCap ? capSq : source);
#endif
}

// The move just recorded is not made (illegal) or is undone (unmake): the state leaves the stack.
static inline void nn_unmake_dirty(ThreadData &td) {
  NnStack &S = *td.nnstack;
  --S.size;
  // AA3 (10/10/2026, speed, same state): the write is always done, with the minimum (cmov) instead of the branch. The branch
  // depends on the child, evaluated or not: true in 51-59% of the unmakes (counters, bench 15-16 on fens30/eg30), and a
  // 2-bit predictor per site gets it wrong ~0.24-0.26 times per node.
  S.ready = std::min(S.ready, S.size);
}

// Undo and redo a move on the local copies of the catch-up. Here the branches stay: the pending moves are almost always one and the branches are well predicted. A branchless
// version was tried (square 64 = bitboard 0, XOR always, local mailbox of 65 squares with writes always executed): fewer instructions
// and fewer mispredicted branches, but +1.2 points of cycles per node in the middlegame (xperf 07/10): table reads and writes
// read back at once in 64-byte blocks by the vector code cost more than predictable branches.
static inline void nn_dp_toggle(NnTB &tb, const NnDirtyPiece &dp) {  // XOR: works both forward and backward
  nn_tb_toggle(tb, dp.pc, 1ULL << dp.from);
  if (dp.to != NN_SQ_NONE) nn_tb_toggle(tb, dp.pc, 1ULL << dp.to);
  if (dp.remove_sq != NN_SQ_NONE) nn_tb_toggle(tb, dp.remove_pc, 1ULL << dp.remove_sq);
  if (dp.add_sq != NN_SQ_NONE) nn_tb_toggle(tb, dp.add_pc, 1ULL << dp.add_sq);
}
// First the arrival squares are emptied, then the starting pieces are put back: in Chess960 castling the squares
// overlap.
static inline void nn_mb_undo(int *mb, const NnDirtyPiece &dp) {
  if (dp.to != NN_SQ_NONE) mb[dp.to] = -1;
  if (dp.add_sq != NN_SQ_NONE) mb[dp.add_sq] = -1;
  mb[dp.from] = dp.pc;
  if (dp.remove_sq != NN_SQ_NONE) mb[dp.remove_sq] = dp.remove_pc;
}
static inline void nn_mb_redo(int *mb, const NnDirtyPiece &dp) {
  mb[dp.from] = -1;
  if (dp.remove_sq != NN_SQ_NONE) mb[dp.remove_sq] = -1;
  if (dp.to != NN_SQ_NONE) mb[dp.to] = dp.pc;
  if (dp.add_sq != NN_SQ_NONE) mb[dp.add_sq] = dp.add_pc;
}

// Passers of colour c: same rule as the network's passers function (consilium/cn.cpp, which the network cannot
// export here: engine and network share no headers). A pawn is passed if it does not fall in the "shadow" (squares
// strictly behind, seen from c) of the enemy pawns widened to the adjacent files, nor in that of its own pawns on the
// same file. In our numbering white advances towards the lower indices: its shadow rises in index.
static inline U64 nn_passers(int c, U64 own, U64 opp) {
  const U64 fileA = 0x0101010101010101ULL, fileH = fileA << 7;
  const auto back = [c](U64 b, int n) { return c == white ? b << n : b >> n; };
  U64 oppShadow = back(opp | ((opp & ~fileH) << 1) | ((opp & ~fileA) >> 1), 8);
  oppShadow |= back(oppShadow, 8);
  oppShadow |= back(oppShadow, 16);
  oppShadow |= back(oppShadow, 32);
  U64 ownShadow = back(own, 8);
  ownShadow |= back(ownShadow, 8);
  ownShadow |= back(ownShadow, 16);
  ownShadow |= back(ownShadow, 32);
  return own & ~oppShadow & ~ownShadow;
}



// (Graft blocks PassedRel and PassedState: catch-up of their entries in the stack, nn_prel_* and nn_pst_*, and the check
// -DTRIUMV_VERIFY_GRAFT, removed on 10/10/2026 with the blocks; code in _backup/Triumviratus_8.0_pre_rimozione_graft_
// 2026-10-10 and in the training repo, 04_consilium/graft_engine_storico.)

// Threats and pawns of the states that lack them ([ready, size)), before every evaluation: the board before the
// first pending move is derived from the thread's board by undoing the pending moves, then the moves are redone
// forward. Usually the parent has already been evaluated and there is only one pending move.
static inline void nn_dirty_catch_up(ThreadData &td) {
  NnStack &S = *td.nnstack;
  const int first = S.ready, last = S.size;
  if (first >= last)
    return;
  NnTB tb;
  for (int t = 0; t < 6; t++)
    tb.type[t] = td.bitboards[t] | td.bitboards[t + 6];
  tb.color[white] = td.occupancies[white];
  tb.color[black] = td.occupancies[black];
  tb.occ = td.occupancies[both];
#if defined(NN_MB8)
  alignas(64) std::int8_t mb8[64];
  __m512i b8 = _mm512_load_si512(td.pc8);
  for (int i = last - 1; i >= first; i--) {
    nn_dp_toggle(tb, S.st[i].dp);
    b8 = nn_b8_undo(b8, S.st[i].dp);
  }
  _mm512_store_si512(mb8, b8);
#else
  alignas(64) int mb[64];
  std::memcpy(mb, td.piece_on, sizeof(mb));
  for (int i = last - 1; i >= first; i--) {
    nn_dp_toggle(tb, S.st[i].dp);
    nn_mb_undo(mb, S.st[i].dp);
  }
#endif
  // R4 (10/10/2026): passers for PassedPawns v1, one state at a time (NnStack::v1PassW/B). Those of the state
  // first - 1 are already in the stack, except at the root (~0): tb is now its board.
#if !defined(USE_AVX512ICL)
  U64 v1w = S.v1PassW[first - 1], v1b = S.v1PassB[first - 1];
  if (v1w == ~0ULL) {
    const U64 wp = tb.type[NN_PAWN] & tb.color[white], bp = tb.type[NN_PAWN] & tb.color[black];
    v1w = S.v1PassW[first - 1] = nn_passers(white, wp, bp);
    v1b = S.v1PassB[first - 1] = nn_passers(black, bp, wp);
  }
#endif
  for (int i = first; i < last; i++) {
    NnState &st = S.st[i];
    nn_fill_pawns(st.pawns, st.dp, tb.type[NN_PAWN] & tb.color[white], tb.type[NN_PAWN] & tb.color[black]);
#if defined(NN_MB8)
    nn_threat_steps(tb, NnLookBoard8{mb8}, st.dp, &st.threats);  // tb leaves in the state after the move
    b8 = nn_b8_redo(b8, st.dp);
    _mm512_store_si512(mb8, b8);
#elif defined(USE_AVX512)
    nn_threat_steps(tb, NnLookMailbox{mb}, st.dp, &st.threats);
    nn_mb_redo(mb, st.dp);
#else
    nn_threat_steps(tb, NnLookMailbox{mb}, st.dp, &st.threats);
    if (i + 1 < last)   // the local mailbox is not read after the last state (Houze Guo, 03/11/2026)
      nn_mb_redo(mb, st.dp);
#endif
#if defined(USE_AVX512ICL)
    // (05/11/2026, R26 §6, avx512icl, same features) The passers only on a pawn event, before (from the pawns before the
    // move, the board of the previous state) and after, in the state itself: no reads or writes of the two arrays at
    // the far end of NnStack (about 108 KB past the states) on every catch-up.
    if (st.pawns.any) {
      const NnDirtyPawns &pw = st.pawns;
      const U64 wp = tb.type[NN_PAWN] & tb.color[white], bp = tb.type[NN_PAWN] & tb.color[black];
      st.pass[0][white] = nn_passers(white, pw.before[white], pw.before[black]);
      st.pass[0][black] = nn_passers(black, pw.before[black], pw.before[white]);
      st.pass[1][white] = nn_passers(white, wp, bp);
      st.pass[1][black] = nn_passers(black, bp, wp);
    }
#else
    if (st.pawns.any) {  // R4: the passers change only with a pawn event
      const U64 wp = tb.type[NN_PAWN] & tb.color[white], bp = tb.type[NN_PAWN] & tb.color[black];
      v1w = nn_passers(white, wp, bp);
      v1b = nn_passers(black, bp, wp);
    }
    S.v1PassW[i] = v1w;
    S.v1PassB[i] = v1b;
#endif
  }
  S.ready = last;
}

// --- C2 signals (agent B) ---
// Consilium Plebis signals for the code outside the main search loop (21/10/2026): time management (14_deepen.inc) and
// corrections (09_history.inc). The expert of a perspective is the network's routing table over the non-pawn material
// signatures (own side, opponent), the same lookup as nn_make_dirty above. C2Count holds the non-pawn counts of both
// colours, so a line of captures and promotions can be followed without a board.
// The calling thread's routing table (SmpRouteShift can give the odd helpers a shifted copy), asked at each use.

struct C2Count {
  int n[2], b[2], r[2], q[2];
};
static inline C2Count c2_count(const U64 *bb) {
  C2Count k;
  for (int c = 0; c < 2; c++) {
    k.n[c] = count_bits(bb[N + 6 * c]);
    k.b[c] = count_bits(bb[B + 6 * c]);
    k.r[c] = count_bits(bb[R + 6 * c]);
    k.q[c] = count_bits(bb[Q + 6 * c]);
  }
  return k;
}
// Piece pc (engine code 0..11) leaves (d = -1) or arrives (d = +1). Pawns and kings do not enter the signature.
static inline void c2_count_add(C2Count &k, int pc, int d) {
  const int t = pc % 6, c = pc >= 6;
  if (t == N) k.n[c] += d;
  else if (t == B) k.b[c] += d;
  else if (t == R) k.r[c] += d;
  else if (t == Q) k.q[c] += d;
}
static inline int c2_sig(const C2Count &k, int c) { return cn_sig(k.n[c], k.b[c], k.r[c], k.q[c]); }
static inline int c2_expert(const C2Count &k, int c) { return nn_router_table()[c2_sig(k, c) * 54 + c2_sig(k, c ^ 1)]; }
// Both experts in the layout of NnDirtyPiece::phase: White | Black << 4.
static inline int c2_experts(const C2Count &k) { return c2_expert(k, white) | c2_expert(k, black) << 4; }
// Experts of the thread's current position. Every state above the root carries them (nn_make_dirty: computed on a
// capture, a promotion or the first move, copied otherwise; a null move adds no state and changes no material); the
// root's state has none, so there they come from the board.
static inline int c2_experts_td(const ThreadData &td) {
  const NnStack &S = *td.nnstack;
  return S.size >= 2 ? S.st[S.size - 1].dp.phase : c2_experts(c2_count(td.bitboards));
}

// Synchronisation check (-DTRIUMV_VERIFY_NNSYNC, like TRIUMV_VERIFY_SEEQ): the network's stack must describe the
// thread's board. Every state carries the piece key after its move (written by the successful make; at the root
// nn_root_sync). At every evaluation, for the WHOLE chain: the key of every state is that of the previous state with the
// move described by its dirty entries (piece leaving and arriving, captured, promoted, castling rook), and the key at the
// top is that of the board. One state too many, too few or with wrong dirty entries breaks the chain. Abort at the first
// disagreement.
#ifdef TRIUMV_VERIFY_NNSYNC
static inline U64 nn_dp_key_delta(const NnDirtyPiece &dp) {
  U64 d = piece_keys[dp.pc][dp.from];
  if (dp.to != NN_SQ_NONE)
    d ^= piece_keys[dp.pc][dp.to];
  if (dp.remove_sq != NN_SQ_NONE)
    d ^= piece_keys[dp.remove_pc][dp.remove_sq];
  if (dp.add_sq != NN_SQ_NONE)
    d ^= piece_keys[dp.add_pc][dp.add_sq];
  return d;
}
#endif
static inline void nn_verify_sync(const ThreadData &td) {
#ifdef TRIUMV_VERIFY_NNSYNC
  const NnStack &S = *td.nnstack;
  bool ok = S.size >= 1 && S.size <= NN_STACK_SIZE && S.ready <= S.size && S.st[S.size - 1].key == nn_piece_key(td);
  for (int i = 1; ok && i < S.size; i++)
    ok = S.st[i].key == (S.st[i - 1].key ^ nn_dp_key_delta(S.st[i].dp));
  if (!ok) {
    fprintf(stderr, "[NNSYNC] pila della rete fuori sincronia: stati=%d ply=%d\n", S.size, td.ply);
    fflush(stderr);
    abort();
  }
#else
  (void)td;
#endif
}

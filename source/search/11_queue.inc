// search/11_queue.inc -- Move ordering.
// Part of threads.cpp: NOT a translation unit of its own.

// SeePinned (05/10/2026): pinned pieces and pinners of both colours, once per position, for the SEE (see.cpp). For each
// king, the enemy sliders on its lines on an empty board are the snipers; a sniper with exactly one piece in between,
// and that piece ours, pins it.
static void see_pins_update(ThreadData &td) {
  for (int c = white; c <= black; c++) {
    const int off = c == white ? 0 : 6, eoff = 6 - off;
    const int ksq = get_ls1b_index(td.bitboards[K + off]);
    U64 snipers = (empty_rook_att[ksq] & (td.bitboards[R + eoff] | td.bitboards[Q + eoff])) |
                  (empty_bishop_att[ksq] & (td.bitboards[B + eoff] | td.bitboards[Q + eoff]));
    const U64 occ = td.occupancies[both] ^ snipers;
    U64 pinned = 0, pinners = 0;
    while (snipers) {
      const int sn = get_ls1b_index(snipers);
      const U64 b = between_tbl[ksq][sn] & occ;
      if (b && !(b & (b - 1)) && (b & td.occupancies[c])) {
        pinned |= b;
        pinners |= 1ULL << sn;
      }
      pop_lsb_bb(snipers);
    }
    td.see_pinned[c] = pinned;
    td.see_pinners[c ^ 1] = pinners;
  }
  td.see_pin_key = td.hash_key;
}

// SEE with the convention of the search: castling and en passant count 0. Promotions go to the SEE, which adds the
// promotion gain (17/10/2026, review by Bastiii; before, they counted 0 too).
static inline bool see_holds(ThreadData &td, int m, int threshold) {
  if (get_move_castling(m) || get_move_enpassant(m))
    return 0 >= threshold;
  PROF_GUARD(prof_see);
  if (SeePinned && td.see_pin_key != td.hash_key)
    see_pins_update(td);
  return td_see_ge(td, m, threshold) != 0;
}

// ============================================================================
// THE MOVE QUEUE (block 1 of 8.1, sub-block D)
// One queue per node hands out the moves in groups: hash move, captures that hold the exchange, quiet moves with good
// statistics, losing captures, the remaining quiet moves. In check all evasions are sorted together; in quiescence
// and ProbCut only captures are seen. Moves are pseudo-legal: td_make_move decides legality.
// Origin: staged move generation is a known idea, common to almost every engine, with the usual order of the groups.
// Ours: the three thresholds between the groups are per expert (StGood, StQuiet,
// StSort), chosen once per node when the queue fills (q.ex, q.good_capt, q.good_quiet). Defaults give the old
// constants, identical tree. Code map: docs/conceptual_maps/BLOCK_1_CODE_MAP.md.
// ============================================================================
enum QueueKind { QK_MAIN, QK_EVADE, QK_QUIESCE, QK_CUTS };

enum QueueStep {
  QS_HASH,
  QS_TACT_FILL, QS_TACT_WIN, QS_QUIET_FILL, QS_QUIET_HIGH, QS_TACT_LOSE, QS_QUIET_LOW,
  QS_EVADE_FILL,
  QS_ONLY_TACT_FILL,
  QS_DRAIN,
  QS_DONE
};

struct ScoredMove {
  int move;
  int score;
};

struct MoveQueue {
  ThreadData   *td;
  NodeFrame    *f;
  int           kind;
  int           step;
  int           hash_move;
  int           depth;
  int           cut_threshold;   // QK_CUTS: SEE threshold
  bool          quiets_off;      // move-count pruning: no more quiet moves
#ifdef TRIUMV_PRUNELOG
  bool          pl_count_off;    // data build: move-count pruning applied move by move in skip_move (prunelog.h)
#endif
  LmpChkCtx    *checks;          // check squares of the node, shared with legality (null = computed here)
  int           n_tact, n_all;   // tactical moves in [0, n_tact), quiet moves in [n_tact, n_all)
  int           cur;
  int           n_lose, cur_lose;
  ScoredMove    list[256];
  unsigned char losers[256];
  // The opponent's attacks in the node's position, computed by score_quiets (06/10/2026, SEE of quiet moves):
  // opp_att = every square it attacks, opp_slider = those seen by its sliders. Valid when opp_ready.
  bool          opp_ready;
  U64           opp_att, opp_slider;
  // ThreatHist: threatened squares per type of our piece (09_history.inc), written by score_quiets from the same maps
  // as the ordering; valid when menace_ready.
  bool          menace_ready;
  U64           menace[6];
  // Futility by move class (FutThr*, 01_params.inc): the squares where our piece of each type is attacked by a lesser
  // enemy piece, and whether one of our pieces stands on such a square. Written by score_quiets from the same maps as
  // the ordering, only with a FutThr lever on; valid when thr_ready.
  bool          thr_ready, thr_hang;
  U64           thr_lesser[6];
  // ProbCutMiss (01_params.inc): the captures the node's ProbCut searched and found below beta, set by move_loop
  // (pc_miss_n = 0 otherwise); queue_next hands them out with the losing captures.
  const int    *pc_miss;
  int           pc_miss_n;
  // (08/10/2026) Boundary between good and bad quiet moves, known after sorting (quiet_split >= 0), or -1 (the list
  // is walked comparing each score).
  int           quiet_split;
  LegalNode     lgen;            // legal generator: pins, checks and masks of the node, shared by the stages
  // LmrRank (block 2 of 8.1, 16/10/2026): rank_ok = the last move handed out is a quiet move of the sorted list, with
  // ordering score last_score; quiet_top = the highest score among the node's quiet moves (fill_quiets).
  bool          rank_ok;
  int           last_score, quiet_top;
  // Expert of the node and group thresholds: written by fill_tactics / fill_evasions (ex, good_capt) and fill_quiets
  // (good_quiet), read by queue_next.
  int           ex, good_capt, good_quiet;
  int           c2_conv;         // C2 idea 3: the side to move is ahead (+1) or behind (-1), set by move_loop; else 0
};

// -DTRIUMV_PCMISS_STATS (measurement build, one thread): how much ProbCutMiss touches. Printed at exit.
#ifdef TRIUMV_PCMISS_STATS
static struct PcMissStats {
  unsigned long long probes = 0, tried = 0, recorded = 0, demoted = 0;
  ~PcMissStats() {
    if (probes)
      fprintf(stderr, "pcmiss: ProbCut calls %llu, captures searched %llu, found below beta %llu, handed out with "
                      "the losers %llu\n", probes, tried, recorded, demoted);
  }
} g_pcmiss;
  #define PCMISS_STAT(x) (x)
#else
  #define PCMISS_STAT(x) ((void)0)
#endif

// ThreatHist: threat context of a quiet move, from the node's maps when they exist, otherwise from its two squares.
static inline int quiet_ctx(const MoveQueue &q, int m) {
  if (!ThreatHist || get_move_promoted(m))
    return 0;
  if (q.menace_ready) {
    const U64 mm = q.menace[kPieceKind[move_piece(m)]];
    return (int)((mm >> move_from(m)) & 1) | (int)((mm >> move_to(m)) & 1) << 1;
  }
  return quiet_ctx_slow(*q.td, m);
}

// SEE of a quiet move decided without an exchange (06/10/2026, speed, identical tree). For a plain quiet move (empty
// target, no promotion, castling or en passant) td_see_ge answers "no" for a threshold > 0 and, for a threshold <= 0,
// "yes" as soon as the opponent has no attacker on the target. It counts the attackers with the occupancy WITHOUT the
// origin square: removing that square opens lines only for the sliders that already see it. So if the target is not
// attacked and no enemy slider sees the origin, the result is known: 0 >= threshold. score_quiets prepares the maps
// for the queue's quiet moves; the hash move (handed out before the maps) and evasions go through the full SEE.
static inline bool see_holds_q(ThreadData &td, const MoveQueue &q, int m, int threshold) {
  constexpr int SPECIAL = 0x100000 | 0xf0000 | 0x400000 | 0x800000;   // capture, promotion, e.p., castling
  if (q.opp_ready && !(m & SPECIAL) && !((q.opp_att >> move_to(m)) & 1) && !((q.opp_slider >> move_from(m)) & 1)) {
#ifdef TRIUMV_VERIFY_SEEQ   // check: the full SEE must give the same answer
    if ((0 >= threshold) != see_holds(td, m, threshold)) {
      printf("info string SEEQ WRONG: move %d threshold %d\n", m, threshold);
      fflush(stdout);
      abort();
    }
#endif
    return 0 >= threshold;
  }
  return see_holds(td, m, threshold);
}

// Stable partial sort: moves with score >= limit go first in decreasing order, the others follow in generation order.
// (04/10/2026) The insertion sort here caused 11% of the engine's mispredicted branches; a swap variant changed
// nothing (the branches come from data-dependent comparisons) and cost 3.8% cycles, so the sort is branchless, with
// the same order (identical tree). Each move above the limit gets a unique key (score << 8 | 255 - position): on equal
// scores the first generated wins, as in the stable insertion. Its final place is the number of larger keys, counted
// in a loop the compiler vectorises; the moves below the limit follow in generation order, split with conditional
// increments. n <= 256, so the position fits in 8 bits. 32-bit keys (Wide = false) give 16 comparisons per AVX-512
// instruction; tactical and quiet scores stay well inside +-2^22 (the clamp is only a safety). Evasions use it too
// since 05/10/2026. The moves are read from the generator's list (mv) with their scores (sc) and written once, already
// in order, into the queue (out).
#if defined(__AVX512F__) || defined(__AVX2__)
#include <immintrin.h>
#endif
// (05/10/2026) Mask counting on AVX-512. The loop `r += key[j] > x` has a trip count nh that changes at every call:
// vectorised, it still left a scalar tail of nh % 16 iterations and a horizontal reduction per move. Here the keys
// beyond nh are INT_MIN (below every real key, which is at least -(1 << 30)), so a partial group is compared whole:
// 16 comparisons at a time -> mask -> popcount, no tail. The key is written in the first pass, in the current slot of
// the high list, and counts only if the move enters it. Same keys, same positions; -0.70% cycles per node.
// Returns how many moves went into the sorted part (score >= limit).
template <bool Wide>
static int rank_into(ScoredMove *out, const int *mv, const int *sc, int n, int limit) {
  PROF_GUARD(prof_mp);
  using Key = std::conditional_t<Wide, long long, int>;
  alignas(64) Key key[256 + 16];   // 16 extra slots for the INT_MIN padding of the vector path
  unsigned char hi[256], lo[256];
  int nh = 0, nl = 0;
  for (int i = 0; i < n; i++) {
    const int s  = sc[i];
    const int up = s >= limit;
    hi[nh] = (unsigned char)i;
    lo[nl] = (unsigned char)i;
    if constexpr (Wide)
      key[nh] = ((long long)s << 8) | (255 - i);
    else
      key[nh] = (int)((unsigned)std::clamp(s, -(1 << 22), (1 << 22) - 1) << 8) | (255 - i);
    nh += up;
    nl += 1 - up;
  }
#if defined(__AVX512F__)
  if constexpr (!Wide) {
    _mm512_storeu_si512(key + nh, _mm512_set1_epi32(INT_MIN));   // padding up to the next group of 16
    const int nchunk = (nh + 15) >> 4;
    for (int k = 0; k < nh; k++) {
      const __m512i x = _mm512_set1_epi32(key[k]);
      int r = 0;
      for (int c = 0; c < nchunk; c++)
        r += count_bits((U64)_mm512_cmpgt_epi32_mask(_mm512_load_si512(key + 16 * c), x));
      out[r] = {mv[hi[k]], sc[hi[k]]};
    }
  } else
#elif defined(__AVX2__)
  // (15/10/2026) The same full-group counting on AVX2 builds, 8 keys per comparison (vpcmpgtd -> movmskps -> popcnt):
  // the tail of the scalar loop left with a mispredicted branch almost at every sorted move (~2.6% of all
  // mispredictions). Here every move does the same number of groups. Same keys, same positions.
  if constexpr (!Wide) {
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(key + nh), _mm256_set1_epi32(INT_MIN));
    const int nchunk = (nh + 7) >> 3;
    for (int k = 0; k < nh; k++) {
      const __m256i x = _mm256_set1_epi32(key[k]);
      int r = 0;
      for (int c = 0; c < nchunk; c++)
        r += count_bits((U64)(unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(
                 _mm256_cmpgt_epi32(_mm256_load_si256(reinterpret_cast<const __m256i *>(key + 8 * c)), x))));
      out[r] = {mv[hi[k]], sc[hi[k]]};
    }
  } else
#endif
  for (int k = 0; k < nh; k++) {
    const Key x = key[k];
    int r = 0;
    for (int j = 0; j < nh; j++)
      r += key[j] > x;
    out[r] = {mv[hi[k]], sc[hi[k]]};
  }
  for (int k = 0; k < nl; k++)
    out[nh + k] = {mv[lo[k]], sc[lo[k]]};
  return nh;
}

// (28/10/2026, speed, same output as rank_into<false>) On AVX-512 the first pass and the copy of the unsorted part run
// 16 moves at a time. Per group: the mask of the moves at or above the limit, their keys (same clamp, same 255 -
// position) and their moves and scores packed in register (vpcompressd) into small arrays; the moves below the limit
// are packed, interleaved into ScoredMove pairs and stored straight after the sorted part, whose size is counted first.
// The ranks of the sorted part are counted as in rank_into. Stores are whole vectors: they stay inside the 256 slots
// (at most 218 moves plus 15 lanes) and never reach a part already written. Used from 8 moves up: on the short lists
// (the 2-4 captures of a quiescence node) the fixed cost of the vector code is larger than the scalar loop. Xeon
// (PGO, xperf, with the other patches of 28/10/2026 night), cycles per node middlegame / endgame: from 8 moves
// 4012 / 3803, on every list 4020 / 3819. -DTRIUMV_VERIFY_RANK compares every call with rank_into.
#if defined(__AVX2__) && !defined(__AVX512F__)
// AVX2 has no vpcompressd: for each 8-bit lane mask, the indices of its set lanes in order (the rest 0), for vpermd.
// Used by the AVX2 ranking below and by the compaction of fill_quiets.
struct Pack8 {
  unsigned char idx[256][8];
};
static constexpr Pack8 make_pack8() {
  Pack8 t{};
  for (int m = 0; m < 256; m++) {
    int k = 0;
    for (int b = 0; b < 8; b++)
      if ((m >> b) & 1) t.idx[m][k++] = (unsigned char)b;
  }
  return t;
}
alignas(64) static constexpr Pack8 kPack8 = make_pack8();
static inline __m256i pack8_idx(unsigned mask) {
  return _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(kPack8.idx[mask])));
}
// (29/10/2026 night, speed, same output as rank_into<false>) The vector ranking on AVX2 builds (avx2, avx2-nopext,
// avxvnni: Intel 12th gen and later, Core Ultra, AMD Zen 2/3), 8 moves at a time, packed with vpermd and kPack8. Same
// keys, same positions; the stores stay inside the 256 slots (at most 218 moves plus 7 lanes). The ranks are counted 8
// keys per comparison, as in rank_into. Xeon, avx2 variant (PGO, xperf), with the AVX2 compaction, hist_learn and TT
// below: -2.2% cycles per node in the middlegame and in endgames.
#define TRIUMV_HAVE_RANKV 1
static int rank_into_v(ScoredMove *out, const int *mv, const int *sc, int n, int limit) {
  PROF_GUARD(prof_mp);
  alignas(32) int key[256 + 8], hmv[256 + 8], hsc[256 + 8];
  unsigned char up[32];
  const int ng = (n + 7) >> 3;
  const __m256i lim = _mm256_set1_epi32(limit), iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
  int nh = 0;
  for (int g = 0; g < ng; g++) {
    const __m256i k = _mm256_cmpgt_epi32(_mm256_set1_epi32(n - 8 * g), iota);
    const __m256i s = _mm256_maskload_epi32(sc + 8 * g, k);
    up[g] = (unsigned char)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_andnot_si256(_mm256_cmpgt_epi32(lim, s), k)));
    nh += count_bits(up[g]);
  }
  int h = 0, l = nh;
  for (int g = 0; g < ng; g++) {
    const __m256i k = _mm256_cmpgt_epi32(_mm256_set1_epi32(n - 8 * g), iota);
    const __m256i s = _mm256_maskload_epi32(sc + 8 * g, k), m = _mm256_maskload_epi32(mv + 8 * g, k);
    const unsigned u = up[g], d = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(k)) & ~u;
    const __m256i cl = _mm256_min_epi32(_mm256_max_epi32(s, _mm256_set1_epi32(-(1 << 22))), _mm256_set1_epi32((1 << 22) - 1));
    const __m256i ky = _mm256_or_si256(_mm256_slli_epi32(cl, 8), _mm256_sub_epi32(_mm256_set1_epi32(255 - 8 * g), iota));
    const __m256i iu = pack8_idx(u);
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(key + h), _mm256_permutevar8x32_epi32(ky, iu));
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(hmv + h), _mm256_permutevar8x32_epi32(m, iu));
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(hsc + h), _mm256_permutevar8x32_epi32(s, iu));
    h += count_bits(u);
    const __m256i id = pack8_idx(d);
    const __m256i lm = _mm256_permutevar8x32_epi32(m, id), ls = _mm256_permutevar8x32_epi32(s, id);
    const __m256i a = _mm256_unpacklo_epi32(lm, ls), b = _mm256_unpackhi_epi32(lm, ls);   // pairs 0 1 4 5 / 2 3 6 7
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + l), _mm256_permute2x128_si256(a, b, 0x20));
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + l + 4), _mm256_permute2x128_si256(a, b, 0x31));
    l += count_bits(d);
  }
  _mm256_storeu_si256(reinterpret_cast<__m256i *>(key + nh), _mm256_set1_epi32(INT_MIN));   // padding of the last group
  const int nchunk = (nh + 7) >> 3;
  for (int j = 0; j < nh; j++) {
    const __m256i x = _mm256_set1_epi32(key[j]);
    int r = 0;
    for (int c = 0; c < nchunk; c++)
      r += count_bits((U64)(unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(
               _mm256_cmpgt_epi32(_mm256_load_si256(reinterpret_cast<const __m256i *>(key + 8 * c)), x))));
    out[r] = {hmv[j], hsc[j]};
  }
  return nh;
}
#endif
#if defined(__AVX512F__)
#define TRIUMV_HAVE_RANKV 1
static int rank_into_v(ScoredMove *out, const int *mv, const int *sc, int n, int limit) {
  PROF_GUARD(prof_mp);
  alignas(64) int key[256 + 16], hmv[256 + 16], hsc[256 + 16];
  __mmask16 up[16];
  const int ng = (n + 15) >> 4;
  const __m512i lim = _mm512_set1_epi32(limit);
  int nh = 0;
  for (int g = 0; g < ng; g++) {
    const __mmask16 k = n - 16 * g >= 16 ? (__mmask16)0xFFFF : (__mmask16)((1u << (n - 16 * g)) - 1);
    up[g] = _mm512_mask_cmpge_epi32_mask(k, _mm512_maskz_loadu_epi32(k, sc + 16 * g), lim);
    nh += count_bits(up[g]);
  }
  const __m512i iota = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  // pair interleave: lanes 0..7 of the moves with lanes 0..7 of the scores, then lanes 8..15
  const __m512i ilo = _mm512_setr_epi32(0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
  const __m512i ihi = _mm512_setr_epi32(8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31);
  int h = 0, l = nh;
  for (int g = 0; g < ng; g++) {
    const __mmask16 k = n - 16 * g >= 16 ? (__mmask16)0xFFFF : (__mmask16)((1u << (n - 16 * g)) - 1);
    const __m512i s = _mm512_maskz_loadu_epi32(k, sc + 16 * g), m = _mm512_maskz_loadu_epi32(k, mv + 16 * g);
    const __mmask16 u = up[g], d = k & (__mmask16)~u;
    const __m512i cl = _mm512_min_epi32(_mm512_max_epi32(s, _mm512_set1_epi32(-(1 << 22))), _mm512_set1_epi32((1 << 22) - 1));
    const __m512i ky = _mm512_or_si512(_mm512_slli_epi32(cl, 8),
                                       _mm512_sub_epi32(_mm512_set1_epi32(255 - 16 * g), iota));
    _mm512_storeu_si512(key + h, _mm512_maskz_compress_epi32(u, ky));
    _mm512_storeu_si512(hmv + h, _mm512_maskz_compress_epi32(u, m));
    _mm512_storeu_si512(hsc + h, _mm512_maskz_compress_epi32(u, s));
    h += count_bits(u);
    const __m512i lm = _mm512_maskz_compress_epi32(d, m), ls = _mm512_maskz_compress_epi32(d, s);
    _mm512_storeu_si512(reinterpret_cast<void *>(out + l), _mm512_permutex2var_epi32(lm, ilo, ls));
    _mm512_storeu_si512(reinterpret_cast<void *>(out + l + 8), _mm512_permutex2var_epi32(lm, ihi, ls));
    l += count_bits(d);
  }
  _mm512_storeu_si512(key + nh, _mm512_set1_epi32(INT_MIN));   // padding up to the next group of 16
  const int nchunk = (nh + 15) >> 4;
  for (int j = 0; j < nh; j++) {
    const __m512i x = _mm512_set1_epi32(key[j]);
    int r = 0;
    for (int c = 0; c < nchunk; c++)
      r += count_bits((U64)_mm512_cmpgt_epi32_mask(_mm512_load_si512(key + 16 * c), x));
    out[r] = {hmv[j], hsc[j]};
  }
  return nh;
}
#endif

// The ranking the fills call: the vector code on AVX-512 and AVX2 from 8 moves, rank_into otherwise (same output).
static inline int rank_list(ScoredMove *out, const int *mv, const int *sc, int n, int limit) {
#if defined(TRIUMV_HAVE_RANKV)
#if defined(TRIUMV_VERIFY_RANK)   // check: the same list and size as rank_into
  if (n >= 8) {
    ScoredMove chk[256];
    const int a = rank_into_v(out, mv, sc, n, limit), b = rank_into<false>(chk, mv, sc, n, limit);
    bool same = a == b;
    for (int i = 0; i < n && same; i++)
      same = out[i].move == chk[i].move && out[i].score == chk[i].score;
    if (!same) {
      printf("info string RANK WRONG: n %d limit %d\n", n, limit);
      fflush(stdout);
      abort();
    }
    return a;
  }
#else
  if (n >= 8)
    return rank_into_v(out, mv, sc, n, limit);
#endif
#endif
  return rank_into<false>(out, mv, sc, n, limit);
}

static void queue_start(MoveQueue &q, ThreadData &td, NodeFrame *f, int hash_move, int depth, int kind) {
  q.td = &td;
  q.f = f;
  q.kind = kind;
  q.depth = depth;
  q.quiets_off = false;
#ifdef TRIUMV_PRUNELOG
  q.pl_count_off = false;
#endif
  q.checks = nullptr;
  q.cut_threshold = 0;
  q.n_tact = q.n_all = q.cur = 0;
  q.n_lose = q.cur_lose = 0;
  q.opp_ready = false;
  q.menace_ready = false;
  q.thr_ready = false;
  q.pc_miss_n = 0;
  q.quiet_split = -1;
  q.lgen.ready = false;
  q.rank_ok = false;
  q.last_score = q.quiet_top = 0;
  q.ex = 3, q.good_capt = 1024, q.good_quiet = 0;
  q.c2_conv = 0;
  bool hash_ok = hash_move > 0 && td_is_pseudo_legal(td, hash_move);
  if (kind == QK_CUTS)
    hash_ok = hash_ok && is_tactical(hash_move);
  q.hash_move = hash_ok ? hash_move : 0;
  q.step = QS_HASH;
}

// Score of a capture (sub-block C): capture history and value of the victim, weighted per expert (HtCapt, HtVict,
// /1024; the defaults 1024 and 7168 give the old sum history + 7 x value). The score also sets the exchange threshold
// of a good capture in queue_next (-score x StGood / 19456).
// PromoOrder and BishopPairCapt (08/10/2026, off = tree unchanged): queen promotions and captures of a bishop of the
// enemy pair move up in the order. promo and bpair are PromoOrder and BishopPairCapt as the node uses them (scaled per
// C2 expert with C2OrdReg, fill_tactics).
static inline int score_tactic(const ThreadData &td, int m, int ex, int promo, int bpair) {
  const int vk = victim_kind(td, m);
  const int vto = move_to(m);   // per-expert value of the victim (see.cpp) and its pawn-structure correction (see.h)
  const int vval = piece_values_mat(td)[td.piece_on[vto] & 15] + pawn_struct_adj(td, vto, td.piece_on[vto]);
  int s = (by_exp4(ex, HtCapt0, HtCapt1, HtCapt2, HtCapt3) * td.capt_hist[move_piece(m)][move_to(m)][vk] +
           by_exp4(ex, HtVict0, HtVict1, HtVict2, HtVict3) * vval) / 1024;
  if (promo && promo_kind(m) == Q)
    s += promo;
  if (bpair && vk == B && count_bits(td.bitboards[td.side == white ? b : B]) >= 2)
    s += bpair;
  return s;
}

// (10/10/2026) Squares attacked by the set of knights in `b`, with the shifts of mask_knight_attacks (attacks.cpp)
// applied to the whole bitboard: the same union as the loop over knight_attacks[]. With a8 = 0 the a-file is bit 0 of
// each byte, so a right shift by 1 or 2 bits moves toward the a-file.
static inline U64 knight_fill(U64 b) {
  const U64 l1 = (b >> 1) & 0x7F7F7F7F7F7F7F7FULL, l2 = (b >> 2) & 0x3F3F3F3F3F3F3F3FULL;
  const U64 r1 = (b << 1) & 0xFEFEFEFEFEFEFEFEULL, r2 = (b << 2) & 0xFCFCFCFCFCFCFCFCULL;
  const U64 h1 = l1 | r1, h2 = l2 | r2;
  return (h1 << 16) | (h1 >> 16) | (h2 << 8) | (h2 >> 8);
}

// (28/10/2026 night, speed, same scores) VECTOR QUIET SCORE on AVX-512: the histories (main, pawn, the five
// continuations, /1024) and the threat term of 16 quiet moves at a time; the moves that give a direct check or touch a
// map of the chess-knowledge terms are flagged and finished by score_quiets in scalar, as are the first-ply histories.
// In the scalar loop the seven tables, their rows and the node's maps did not fit in the registers, so every read went
// through a pointer reloaded from the stack (~45 instructions per move for the histories alone). The history sum is
// exact in 32 bits (with the largest weights of 01_params.inc it stays below 27648 x 32767 < 2^31) and the division
// truncates toward zero as in the scalar code. Xeon (PGO, xperf, with the vector ranking above and the compaction of
// fill_quiets): -2.4% cycles per node in the middlegame, -2.1% in endgames, -6% instructions, bench unchanged.
// -DTRIUMV_VERIFY_VQ compares every score with the scalar loop.
#if defined(__AVX512F__)
// 16 int16 entries tab[idx[j]], sign-extended, each read as the aligned dword that holds it: an aligned dword never
// crosses a page, so no lane touches memory outside the page of its own entry.
static inline __m512i gather16(__mmask16 k, const int16_t *tab, __m512i idx) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(tab);
  const __m512i off = _mm512_add_epi32(_mm512_slli_epi32(idx, 1), _mm512_set1_epi32((int)(a & 3)));
  const __m512i dw = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), k, _mm512_srli_epi32(off, 2),
                                                 reinterpret_cast<const void *>(a & ~(uintptr_t)3), 4);
  // entry in the low half (off & 2 = 0): left 16, then arithmetic right 16; in the high half: arithmetic right 16 only
  const __m512i sh = _mm512_sub_epi32(_mm512_set1_epi32(16),
                                      _mm512_slli_epi32(_mm512_and_si512(off, _mm512_set1_epi32(2)), 3));
  return _mm512_srai_epi32(_mm512_sllv_epi32(dw, sh), 16);
}
// Tables and maps of the node for the vector score.
struct QuietVecIn {
  const int16_t *qh, *pw, *h1, *h2, *h3, *h4, *h6;   // main (side to move), pawn row, continuation rows 1-4 and 6
  int w_main, w_pawn, w_near, w_far;                  // weights of the history sum, /1024
  int us_off, thr_esc, thr_ent;
  const U64 *lesser, *chk, *kn_to;                    // per piece type: lower-value attackers, check squares, knowledge
  U64 shield;                                         // origins of the king-shield malus
  const int *mval;                                    // 16 piece codes
};
// sc[] = history sum + threat term; flags[g] = lanes of group g that score_quiets finishes in scalar.
static inline void quiet_vec_score(const QuietVecIn &in, const int *mv, int *sc, int n, unsigned short *flags) {
  const __m512i pk_tab = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3);   // kPromoKind
  const __m512i LES = _mm512_maskz_loadu_epi64(0x3F, in.lesser), CHK = _mm512_maskz_loadu_epi64(0x3F, in.chk),
                KN = _mm512_maskz_loadu_epi64(0x3F, in.kn_to), SH = _mm512_set1_epi64((long long)in.shield);
  const __m512i MV = _mm512_loadu_si512(in.mval);
  const __m512i one = _mm512_set1_epi64(1), m63 = _mm512_set1_epi32(63);
  for (int i = 0, g = 0; i < n; i += 16, g++) {
    const __mmask16 k = n - i >= 16 ? (__mmask16)0xFFFF : (__mmask16)((1u << (n - i)) - 1);
    const __m512i m = _mm512_maskz_loadu_epi32(k, mv + i);
    // histories: pc * 64 + to indexes the piece-to rows, the quiet key the main table
    const __m512i pidx = _mm512_and_si512(_mm512_srli_epi32(m, 6), _mm512_set1_epi32(0x3ff));
    const __m512i pkind =
        _mm512_permutexvar_epi32(_mm512_and_si512(_mm512_srli_epi32(m, 16), _mm512_set1_epi32(15)), pk_tab);
    const __m512i key = _mm512_or_si512(_mm512_and_si512(m, _mm512_set1_epi32(0xfff)), _mm512_slli_epi32(pkind, 12));
    __m512i hs = _mm512_mullo_epi32(gather16(k, in.qh, key), _mm512_set1_epi32(in.w_main));
    hs = _mm512_add_epi32(hs, _mm512_mullo_epi32(gather16(k, in.pw, pidx), _mm512_set1_epi32(in.w_pawn)));
    const __m512i nr = _mm512_add_epi32(gather16(k, in.h1, pidx), gather16(k, in.h2, pidx));
    hs = _mm512_add_epi32(hs, _mm512_mullo_epi32(nr, _mm512_set1_epi32(in.w_near)));
    const __m512i fr = _mm512_add_epi32(_mm512_add_epi32(gather16(k, in.h3, pidx), gather16(k, in.h4, pidx)),
                                        gather16(k, in.h6, pidx));
    hs = _mm512_add_epi32(hs, _mm512_mullo_epi32(fr, _mm512_set1_epi32(in.w_far)));
    // hs / 1024 truncated toward zero: 1023 is added to the negative sums before the arithmetic shift
    __m512i qv = _mm512_srai_epi32(
        _mm512_add_epi32(hs, _mm512_and_si512(_mm512_srai_epi32(hs, 31), _mm512_set1_epi32(1023))), 10);
    // squares and piece type of each lane; the bit tests on 64-bit maps run in two halves of 8 lanes
    const __m512i from = _mm512_and_si512(m, m63), to = _mm512_and_si512(_mm512_srli_epi32(m, 6), m63);
    const __m512i pc = _mm512_and_si512(_mm512_srli_epi32(m, 12), _mm512_set1_epi32(15));
    const __m512i pt = _mm512_sub_epi32(pc, _mm512_set1_epi32(in.us_off));
    unsigned fm = 0, tm = 0, cm = 0, km = 0;
    const auto half = [&](__m256i pt4, __m256i from4, __m256i to4, int sh) {
      const __m512i pt8 = _mm512_cvtepi32_epi64(pt4);
      const __m512i fb = _mm512_sllv_epi64(one, _mm512_cvtepi32_epi64(from4));
      const __m512i tb = _mm512_sllv_epi64(one, _mm512_cvtepi32_epi64(to4));
      const __m512i les = _mm512_permutexvar_epi64(pt8, LES);
      fm |= (unsigned)_mm512_test_epi64_mask(les, fb) << sh;
      tm |= (unsigned)_mm512_test_epi64_mask(les, tb) << sh;
      cm |= (unsigned)_mm512_test_epi64_mask(_mm512_permutexvar_epi64(pt8, CHK), tb) << sh;
      km |= (unsigned)(_mm512_test_epi64_mask(_mm512_permutexvar_epi64(pt8, KN), tb) |
                       _mm512_test_epi64_mask(SH, fb)) << sh;
    };
    half(_mm512_castsi512_si256(pt), _mm512_castsi512_si256(from), _mm512_castsi512_si256(to), 0);
    half(_mm512_extracti64x4_epi64(pt, 1), _mm512_extracti64x4_epi64(from, 1), _mm512_extracti64x4_epi64(to, 1), 8);
    // threat term: (mval[pc] * (thr_esc * from_t - thr_ent * to_t)) >> 6 (weights in 1/64, as the scalar path)
    const __m512i th = _mm512_sub_epi32(_mm512_maskz_mov_epi32((__mmask16)fm, _mm512_set1_epi32(in.thr_esc)),
                                        _mm512_maskz_mov_epi32((__mmask16)tm, _mm512_set1_epi32(in.thr_ent)));
    qv = _mm512_add_epi32(qv, _mm512_srai_epi32(_mm512_mullo_epi32(th, _mm512_permutexvar_epi32(pc, MV)), 6));
    _mm512_mask_storeu_epi32(sc + i, k, qv);
    flags[g] = (unsigned short)((cm | km) & k);
  }
}
#endif

// Scores of the quiet moves: weighted statistics, a bonus for direct checks that lose no material, and the move of a
// piece that leaves (or enters) a square attacked by an enemy piece of lower value.
static void score_quiets(MoveQueue &q, const int *mv, int *sc, int n) {
  PROF_GUARD(prof_score);
  ThreadData &td = *q.td;
  NodeFrame *f = q.f;
  const int us = td.side, them = us ^ 1;
  const int eoff = them == white ? 0 : 6, us_off = 6 - eoff;
  const U64 occ = td.occupancies[both];

  // (29/10/2026 night, speed, same scores) The enemy's attacks per kind are the node's enemy map of the legal generator
  // (movegen_legal.h, enemy_map), built by castling or the quiet king steps of the stage just before, or here: the
  // same lookups this function made before (one slider pair per enemy slider). The node is never in check here
  // (QK_MAIN), so lifting our king in that map changes no attack. -DTRIUMV_VERIFY_EMAP checks it against direct lookups.
  legalgen::LegalInfo local;
  legalgen::LegalInfo *li = &q.lgen.info;
  if (!q.lgen.ready) {   // the moves did not come from the legal generator (Chess960): a map for this node only
    local.king = get_ls1b_index(td.bitboards[K + us_off]);
    local.emap = false;
    li = &local;
  }
  if (!li->emap) {
    if (us == white)
      legalgen::enemy_map<white>(td, *li);
    else
      legalgen::enemy_map<black>(td, *li);
  }
  const U64 epawns = td.bitboards[P + eoff];
  const U64 eN = td.bitboards[N + eoff], eB = td.bitboards[B + eoff], eR = td.bitboards[R + eoff],
            eQ = td.bitboards[Q + eoff];
  const U64 by_pawn = li->em[P], by_knight = li->em[N], by_bishop = li->em[B], by_rook = li->em[R],
            by_queen = li->em[Q];
#ifdef TRIUMV_VERIFY_EMAP
  {
    U64 vb = 0, vr = 0, vq = 0;
    for (U64 t = eB | eR | eQ; t; pop_lsb_bb(t)) {
      const int sq = get_ls1b_index(t);
      const SliderPair sp = get_slider_pair(sq, occ);
      if ((eB >> sq) & 1) vb |= pair_bishop(sp);
      if ((eR >> sq) & 1) vr |= pair_rook(sp);
      if ((eQ >> sq) & 1) vq |= pair_bishop(sp) | pair_rook(sp);
    }
    const U64 vp = them == white ? ((epawns >> 7) & not_a_file) | ((epawns >> 9) & not_h_file)
                                 : ((epawns << 7) & not_h_file) | ((epawns << 9) & not_a_file);
    if (vb != by_bishop || vr != by_rook || vq != by_queen || knight_fill(eN) != by_knight || vp != by_pawn) {
      printf("info string EMAP WRONG: enemy map differs\n");
      fflush(stdout);
      abort();
    }
  }
#endif
  const U64 by_minor = by_knight | by_bishop;
  // Map for see_holds_q (the queen and the king are not in lesser[], only here).
  q.opp_slider = by_bishop | by_rook | by_queen;
  q.opp_att = li->em_all;
  q.opp_ready = true;
  U64 lesser[6];
  lesser[P] = 0;
  lesser[N] = lesser[B] = by_pawn;
  lesser[R] = by_minor | by_pawn;
  lesser[Q] = by_rook | lesser[R];
  lesser[K] = 0;
  // ThreatHist: threatened squares per piece type (09_history.inc): the lesser[] maps, plus pawn attacks for pawns.
  if (ThreatHist) {
    for (int k = P; k <= K; k++) q.menace[k] = lesser[k];
    q.menace[P] = by_pawn;
    q.menace_ready = true;
  }
  // Futility by move class (FutThr*): the same maps for the quiet gate, and whether one of our pieces is attacked by
  // a lesser one.
  if (FutThrSpare | FutThrPlain | FutThrHang) {
    for (int k = P; k <= K; k++) q.thr_lesser[k] = lesser[k];
    q.thr_hang = (((td.bitboards[N + us_off] | td.bitboards[B + us_off]) & lesser[N]) |
                  (td.bitboards[R + us_off] & lesser[R]) | (td.bitboards[Q + us_off] & lesser[Q])) != 0;
    q.thr_ready = true;
  }

  LmpChkCtx local_checks;
  LmpChkCtx &checks = q.checks ? *q.checks : local_checks;
  if (!checks.ready)
    td_lmp_chk_init(td, checks);

  // The chess-knowledge terms of this node (below). C2OrdReg (C2 idea 5, agent ORDER, 21/10/2026; OFF by default,
  // SPSA pending) scales each group by the C2 expert that names its regime (09_history.inc, c2_reg_scales). The tests
  // that decide whether a map is built stay on the parameters themselves, so a scale of 0 only removes the term.
  int ks_v = KingShield, op_v = OutpostOrder, opb_v = OutpostOrderB, pass_v = PassedPush;
  int atk_v = AttackOrder, atkq_v = AttackOrderQ;
  if (C2OrdReg) {   // --- C2 ordering (agent ORDER) ---
    const C2RegScales rs = c2_reg_scales(td, td.side);
    ks_v = c2_reg_apply(ks_v, rs.ks);
    atk_v = c2_reg_apply(atk_v, rs.atk), atkq_v = c2_reg_apply(atkq_v, rs.atk);
    pass_v = c2_reg_apply(pass_v, rs.pass);
    op_v = c2_reg_apply(op_v, rs.minor), opb_v = c2_reg_apply(opb_v, rs.minor);
  }

  // King shield: with the king on its home rank, our pawns next to the king move last. Only while the opponent has
  // attacking material (KingShieldNpm, non-pawn pieces).
  U64 shield = 0;
  if (KingShield) {
    const int ksq = get_ls1b_index(td.bitboards[us == white ? K : k]);
    const bool home = us == white ? ksq >= a1 : ksq <= h8;
    const int *pv = piece_values_mat(td);
    const int their_npm = pv[N + eoff] * count_bits(td.bitboards[N + eoff]) +
                          pv[B + eoff] * count_bits(td.bitboards[B + eoff]) +
                          pv[R + eoff] * count_bits(td.bitboards[R + eoff]) +
                          pv[Q + eoff] * count_bits(td.bitboards[Q + eoff]);
    if (home && their_npm >= KingShieldNpm)
      shield = king_attacks[ksq] & td.bitboards[us == white ? P : p];
  }

  // Outposts (08/10/2026, the user's idea): a knight going to a square in the enemy half (ranks 4-6 from our side),
  // defended by our pawn and never attackable by an enemy pawn (no enemy pawn ahead on the adjacent files), is tried
  // earlier. OutpostSafe: 1 = also, the opponent has no bishop of the square's colour; 2 = also, no knights (the square
  // cannot be contested by a minor piece). With a8 = 0, white advances toward lower indices.
  // Squares in front of the enemy pawns, in their direction of travel (front), and squares those pawns can attack as
  // they advance (span). Our pawn outside both is passed.
  U64 front = 0, span = 0;
  if (OutpostOrder || OutpostOrderB || PassedPush) {
    front = epawns;
    if (them == white) { front |= front >> 8; front |= front >> 16; front |= front >> 32; }
    else               { front |= front << 8; front |= front << 16; front |= front << 32; }
    span = them == white ? ((front >> 7) & not_a_file) | ((front >> 9) & not_h_file)
                         : ((front << 7) & not_h_file) | ((front << 9) & not_a_file);
  }
  U64 outpost = 0;
  if (OutpostOrder || OutpostOrderB) {
    const U64 ours_p = td.bitboards[P + us_off];
    const U64 guarded = us == white ? ((ours_p >> 7) & not_a_file) | ((ours_p >> 9) & not_h_file)
                                    : ((ours_p << 7) & not_h_file) | ((ours_p << 9) & not_a_file);
    const U64 rows = us == white ? 0x000000FFFFFF0000ULL : 0x0000FFFFFF000000ULL;   // ranks 4-6 / 5-3
    outpost = rows & guarded & ~span;
    if (OutpostSafe >= 1) {
      constexpr U64 light = 0xAA55AA55AA55AA55ULL;   // a8 (index 0) is a light square
      const U64 eb = td.bitboards[B + eoff];
      if (eb & light)  outpost &= ~light;
      if (eb & ~light) outpost &= light;
    }
    if (OutpostSafe >= 2 && td.bitboards[N + eoff])
      outpost = 0;
  }

  // Passed-pawn push (08/10/2026): target squares from relative rank PassedPushRank to the seventh where our pawn is
  // passed. The SEE (>= 0) is asked only of the pawn moves that reach them.
  U64 passed_to = 0;
  if (PassedPush) {
    U64 rows = 0;
    for (int rr = PassedPushRank; rr <= 7; rr++)
      rows |= 0xFFULL << (8 * (us == white ? 8 - rr : rr - 1));
    passed_to = rows & ~(front | span);
  }

  // Attacks from a safe square (08/10/2026; the rule is ours): for each piece type, the squares
  // not attacked by the opponent from which that piece would attack a target. Targets: enemy pieces (not pawns, not
  // the king) of a higher class (pawn < knight = bishop < rook < queen), or of the same or lower class but undefended.
  // The squares are found backwards, from the target's attacks (pawn: those of an enemy pawn).
  U64 attack_to[6] = {0, 0, 0, 0, 0, 0};
  if (AttackOrder || AttackOrderQ) {
    const U64 safe = ~q.opp_att;
    const U64 minors = eN | eB;
    // First pass, over the enemy sliders (the node's map has only the union per kind): the parts of attack_to that do
    // not depend on `safe` (rooks and the queen are always targets for the bishop, the queen for the rook) and the
    // lines of each slider for the second pass.
    U64 aa_b = 0, aa_r = 0, aa_q = 0;            // attack_to[B], [R], [Q]
    U64 sl_b[16], sl_r[16];                      // bishop and rook attacks of the sliders
    unsigned char sl_slot[64];                   // index into sl_b/sl_r per square (written only for sliders)
    int nsl = 0;
    for (U64 t = eB | eR | eQ; t; pop_lsb_bb(t)) {
      const int sq = get_ls1b_index(t);
      const SliderPair sp = get_slider_pair(sq, occ);
      const U64 b = pair_bishop(sp), r = pair_rook(sp);
      const U64 mR = 0 - ((eR >> sq) & 1), mQ = 0 - ((eQ >> sq) & 1);
      aa_b |= b & (mR | mQ);
      aa_r |= r & mQ;
      sl_b[nsl] = b;
      sl_r[nsl] = r;
      sl_slot[sq] = (unsigned char)nsl++;
    }
    // Second pass. Targets: of the bishop er|eq|(minors&safe), of the rook eq|((minors|er)&safe), of the queen
    // (minors|er)&safe, of the knight as the bishop, of the pawn minors|er|eq. The parts without `safe` are already in
    // aa_b and aa_r; safe bishops and rooks reuse their first-pass attacks; safe knights (0-2) are the only targets that
    // need a new computation. Knights and pawns as sets (knight_fill, pawn shifts).
    for (U64 t = (eB | eR) & safe; t; pop_lsb_bb(t)) {
      const int sq = get_ls1b_index(t), k = sl_slot[sq];
      aa_b |= sl_b[k] & (0 - ((eB >> sq) & 1));
      aa_r |= sl_r[k];
      aa_q |= sl_b[k] | sl_r[k];
    }
    for (U64 t = eN & safe; t; pop_lsb_bb(t)) {
      const SliderPair sp = get_slider_pair(get_ls1b_index(t), occ);
      aa_b |= pair_bishop(sp);
      aa_r |= pair_rook(sp);
      aa_q |= pair_bishop(sp) | pair_rook(sp);
    }
    if (AttackOrder) {
      const U64 tp = minors | eR | eQ;   // the attacks of an enemy pawn placed on each target
      attack_to[P] = them == white ? ((tp >> 7) & not_a_file) | ((tp >> 9) & not_h_file)
                                   : ((tp << 7) & not_h_file) | ((tp << 9) & not_a_file);
      attack_to[N] = knight_fill(eR | eQ | (minors & safe));
      attack_to[B] = aa_b;
      attack_to[R] = aa_r;
    }
    if (AttackOrderQ)
      attack_to[Q] = aa_q;
#ifdef TRIUMV_VERIFY_AA2   // check: the earlier loops (08/10/2026) must give the same maps
    {
      const U64 er = eR, eq = eQ;
      U64 old[6] = {0, 0, 0, 0, 0, 0}, ok = 0, ob = 0, orr = 0, oq = 0;
      for (U64 t = eN; t; pop_lsb_bb(t)) ok |= knight_attacks[get_ls1b_index(t)];
      for (U64 t = eB; t; pop_lsb_bb(t)) ob |= get_bishop_attacks(get_ls1b_index(t), occ);
      for (U64 t = eR; t; pop_lsb_bb(t)) orr |= get_rook_attacks(get_ls1b_index(t), occ);
      for (U64 t = eQ; t; pop_lsb_bb(t)) oq |= get_queen_attacks(get_ls1b_index(t), occ);
      if (AttackOrder) {
        for (U64 t = minors | er | eq; t; pop_lsb_bb(t)) old[P] |= pawn_attacks[them][get_ls1b_index(t)];
        for (U64 t = er | eq | (minors & safe); t; pop_lsb_bb(t)) old[N] |= knight_attacks[get_ls1b_index(t)];
        for (U64 t = er | eq | (minors & safe); t; pop_lsb_bb(t)) old[B] |= get_bishop_attacks(get_ls1b_index(t), occ);
        for (U64 t = eq | ((minors | er) & safe); t; pop_lsb_bb(t)) old[R] |= get_rook_attacks(get_ls1b_index(t), occ);
      }
      if (AttackOrderQ)
        for (U64 t = (minors | er) & safe; t; pop_lsb_bb(t)) old[Q] |= get_queen_attacks(get_ls1b_index(t), occ);
      bool same = ok == by_knight && ob == by_bishop && orr == by_rook && oq == by_queen;
      for (int k = P; k <= Q; k++) same = same && old[k] == attack_to[k];
      if (!same) {
        printf("info string AA2 WRONG: attack maps differ\n");
        fflush(stdout);
        abort();
      }
    }
#endif
    // AttackSafeSee 0: target square not attacked by any enemy piece. 1: not attacked by pieces of lower value
    // (lesser[]), and the exchange on the square is checked move by move with the SEE (below).
    for (int k = P; k <= Q; k++) attack_to[k] &= AttackSafeSee ? ~lesser[k] : safe;
  }
  const int attack_bonus[6] = {atk_v, atk_v, atk_v, atk_v, atkq_v, 0};

  // ---- Per-expert weights of the quiet-move score (block 1; docs/development/8.1/SEARCH_1_ORDERING.md) ----
  // tb_e = expert of the node (net_phase), shared by every weight below. (ThreatBal, one threat balance in place of
  // the threat term and the safe-square attack bonuses, lost -17 and was removed on 28/10/2026; code in
  // _backup/Triumviratus_8.1_pre_pulizia_2026-10-28.)
  const int tb_e = search_band(td);
  // A. Reading the histories (HistMix), /1024: main history, pawn history, near continuations (1, 2 plies back), far
  //    ones (3, 4, 6), first-ply history, and the agreement term (ours): when cont1 and cont2 agree in sign, HmAgree/1024
  //    of the smaller one is added. Defaults (2048, 2048, 1024, 1024, 1024, 0) give exactly the old sum
  //    2*main + 2*pawn + cont1..cont6 + first plies (checked by the bench, 16/10/2026; old formula removed).
  struct HmW { int main, pawn, near_, far_, low, agree; };
  const HmW hm = tb_e == 0 ? HmW{HmMain0, HmPawn0, HmNear0, HmFar0, HmLow0, HmAgree0}
               : tb_e == 1 ? HmW{HmMain1, HmPawn1, HmNear1, HmFar1, HmLow1, HmAgree1}
               : tb_e == 2 ? HmW{HmMain2, HmPawn2, HmNear2, HmFar2, HmLow2, HmAgree2}
                           : HmW{HmMain3, HmPawn3, HmNear3, HmFar3, HmLow3, HmAgree3};
  // B. Checks: bonus and exchange threshold of a direct check per expert (defaults = the old fixed 13323 and -75).
  //    ChkDisc (ours, off): bonus for a quiet move that discovers a check: the piece is in checks.disc (between one of
  //    our sliders and the enemy king) and lands off the line to the king (legalgen::line_bb). Skipped at 0.
  const int chk_bonus = by_exp4(tb_e, ChkBonus0, ChkBonus1, ChkBonus2, ChkBonus3);
  const int chk_see = by_exp4(tb_e, ChkSee0, ChkSee1, ChkSee2, ChkSee3);
  const int chk_disc = by_exp4(tb_e, ChkDisc0, ChkDisc1, ChkDisc2, ChkDisc3);
  const int eksq = get_ls1b_index(td.bitboards[K + eoff]);
  // B. Threats: leaving and entering a square attacked by a lower-value piece, separate weights per expert (one weight,
  //    ThreatOrderMul, served both; defaults give the old term). ThrSafe (ours, off): extra bonus when the threatened
  //    piece escapes to a square no enemy piece attacks (q.opp_att). Skipped at 0.
  const int thr_esc = by_exp4(tb_e, ThrEsc0, ThrEsc1, ThrEsc2, ThrEsc3);
  const int thr_ent = by_exp4(tb_e, ThrEnt0, ThrEnt1, ThrEnt2, ThrEnt3);
  const int thr_safe = by_exp4(tb_e, ThrSafe0, ThrSafe1, ThrSafe2, ThrSafe3);

  const PieceToRow &c1 = *f[-1].cont_hist, &c2 = *f[-2].cont_hist, &c3 = *f[-3].cont_hist,
                   &c4 = *f[-4].cont_hist, &c6 = *f[-6].cont_hist;
  const PieceToRow &prow = pawn_row(td);
  const int ply = f->ply;
  // A. First-ply history weight: 8 x entry x 1024 / (1024 + ply x LpDecay), per expert; LpDecay = 1024 is the old
  //    8 x entry / (1 + ply), same integer division. The divisor is constant in the node, so the division is a
  //    multiplication by the 40-bit reciprocal M = ceil(2^40 / D) (a per-move idiv cost ~1% of cycles, AA2 10/10/2026),
  //    read from a table per expert and ply since 20/10/2026 (build_lp_recip, 03_tables.inc; before, one 64-bit
  //    division per node). Exact while |numerator| < 2^40 / D: numerator <= 8 x HistLimQuiet x 1024 (5.9e7 at the
  //    default 7183), D <= 1024 + 4 x 3072 = 13312 -> limit 8.3e7, i.e. HistLimQuiet up to ~10,000 (beyond that, lower
  //    the maximum of LpDecay).
#ifdef TRIUMV_VERIFY_AA2
  const unsigned long long lp_den = 1024ULL + (unsigned long long)ply * by_exp4(tb_e, LpDecay0, LpDecay1, LpDecay2, LpDecay3);
#endif
  const unsigned long long lp_recip = ply < LOW_PLY_SIZE ? lp_recip_tbl[tb_e][ply] : 0;
  // C2 idea 5 (21/10/2026, agent A; OFF by default, SPRT pending): a quiet pawn or king move is scored by the change it
  // makes to the pawn-king skip of Consilium Plebis (mover's view, clamped to +-C2PkScale), C2PkOrder at the clamp. One
  // subnetwork probe per such move (the thread's pawn-king cache; the child's evaluation hits the entry if it is played).
  const int c2_pk_order = C2PkOrder, c2_pk_scale = C2PkScale;   // read once: the stores into sc[] could alias them
  const int c2_pk_node = c2_pk_order ? c2_pk_skip(td) : 0;
  // C2 idea 6 (21/10/2026, agent ORDER; OFF by default, SPRT pending): the structure history of the node's pawn-king
  // signature (09_history.inc), null at 0 or on a pawn-king cache miss.
  const int c2_sw = C2StructHistW;
  const int *mval = piece_values_mat(td);   // per-expert piece values of the node (see.cpp)
  const PieceToRow *srow = c2_sw ? c2_struct_row(td) : nullptr;
  // Pawn breaks (28/10/2026, ours, OFF): ordering bonus for a quiet pawn push that attacks an enemy pawn (pawn_break).
  const int brk_order = BreakOrder;
  const U64 their_pawns = td.bitboards[us == white ? p : P];

#if defined(TRIUMV_VERIFY_VQ)   // check: the vector scores go to vbuf, the scalar loop below fills sc, compared at the end
  int vbuf[256];
  bool vq_on = false;
#endif
#if defined(__AVX512F__)
  // Vector score (quiet_vec_score), when the scalar loop has no term outside it: no threat context, no agreement or
  // structure history, no discovered-check, safe-escape, pawn-break or pawn-king term (all off by default).
  if (!ThreatHist && hm.agree == 0 && !srow && !chk_disc && !thr_safe && !brk_order && !c2_pk_order) {
#if defined(TRIUMV_VERIFY_VQ)
    int *const vs = vbuf;
    vq_on = true;
#else
    int *const vs = sc;
#endif
    U64 kn_to[6];   // target squares where a chess-knowledge term can apply, per piece type
    for (int k = P; k <= K; k++) kn_to[k] = attack_to[k];
    kn_to[N] |= outpost;
    kn_to[B] |= outpost;
    kn_to[P] |= passed_to;
    const QuietVecIn in = {td.quiet_hist[us], &prow[0][0], &c1[0][0], &c2[0][0], &c3[0][0], &c4[0][0], &c6[0][0],
                           hm.main, hm.pawn, hm.near_, hm.far_, us_off, thr_esc, thr_ent, lesser, checks.chk, kn_to,
                           shield, mval};
    unsigned short flags[16];
    quiet_vec_score(in, mv, vs, n, flags);
    for (int g = 0; 16 * g < n; g++)   // the flagged moves: direct checks and chess-knowledge terms, as below
      for (unsigned fl = flags[g]; fl; fl &= fl - 1) {
        const int i = 16 * g + __builtin_ctz(fl), m = mv[i];
        const int pc = move_piece(m), to = move_to(m), from = move_from(m), pt = pc - us_off;
        int s = vs[i];
        if (((checks.chk[pt] >> to) & 1) && see_holds_q(td, q, m, chk_see))
          s += chk_bonus;
        if (shield >> from & 1)
          s -= ks_v;
        if (pt == N && (outpost >> to & 1))
          s += op_v;
        if (pt == B && (outpost >> to & 1))
          s += opb_v;
        if ((attack_to[pt] >> to & 1) && (!AttackSafeSee || see_holds_q(td, q, m, 0)))
          s += attack_bonus[pt];
        if (pt == P && (passed_to >> to & 1) && see_holds_q(td, q, m, 0))
          s += pass_v;
        vs[i] = s;
      }
    if (ply < LOW_PLY_SIZE)   // first plies: their history for every move, as below
      for (int i = 0; i < n; i++) {
        const int x = 8 * td.low_ply_hist[ply][quiet_key(mv[i])];
        const unsigned long long ax = (unsigned long long)(x < 0 ? -x : x) * 1024;
        const int lq = (int)((ax * lp_recip) >> 40);
        vs[i] += (x < 0 ? -lq : lq) * hm.low / 1024;
      }
#if !defined(TRIUMV_VERIFY_VQ)
    return;
#endif
  }
#endif

  for (int i = 0; i < n; i++) {
    const int m = mv[i];
    const int pc = move_piece(m), to = move_to(m), from = move_from(m), pt = pc - us_off;   // our move
    const int key = quiet_key(m);
    const int hctx = (ThreatHist && !get_move_promoted(m))
                         ? (int)((q.menace[pt] >> from) & 1) | (int)((q.menace[pt] >> to) & 1) << 1 : 0;
    const int v1 = c1[pc][to], v2 = c2[pc][to];
    long long hs = (long long)hm.main * qh_order(td, us, key, hctx) + (long long)hm.pawn * prow[pc][to] +
                   (long long)hm.near_ * (v1 + v2) + (long long)hm.far_ * (c3[pc][to] + c4[pc][to] + c6[pc][to]);
    if ((v1 ^ v2) >= 0)   // same sign (zero included): the agreement is the smaller one in absolute value
      hs += (long long)hm.agree * (v1 >= 0 ? std::min(v1, v2) : std::max(v1, v2));
    if (srow)   // --- C2 ordering (agent ORDER) ---
      hs += (long long)c2_sw * (*srow)[pc][to];
    int s = (int)(hs / 1024);
    if (((checks.chk[pt] >> to) & 1) && see_holds_q(td, q, m, chk_see))
      s += chk_bonus;
    if (chk_disc && ((checks.disc >> from) & 1) && !((legalgen::line_bb[from][eksq] >> to) & 1))
      s += chk_disc;
    {
      const int from_t = int((lesser[pt] >> from) & 1), to_t = int((lesser[pt] >> to) & 1);
      s += (mval[pc] * (thr_esc * from_t - thr_ent * to_t)) >> 6;   // weights in 1/64 (01_params.inc)
      if (thr_safe && from_t && !((q.opp_att >> to) & 1))
        s += mval[pc] * thr_safe;
    }
    if (ply < LOW_PLY_SIZE) {
      const int x = 8 * td.low_ply_hist[ply][key];
      const unsigned long long ax = (unsigned long long)(x < 0 ? -x : x) * 1024;
      const int lq = (int)((ax * lp_recip) >> 40);
#ifdef TRIUMV_VERIFY_AA2
      if ((x < 0 ? -lq : lq) != (long long)x * 1024 / (long long)lp_den) { printf("info string AA2 WRONG: low ply\n"); fflush(stdout); abort(); }
#endif
      s += (x < 0 ? -lq : lq) * hm.low / 1024;
    }
    if (shield >> from & 1)
      s -= ks_v;
    if (pt == N && (outpost >> to & 1))
      s += op_v;
    if (pt == B && (outpost >> to & 1))
      s += opb_v;
    if ((attack_to[pt] >> to & 1) && (!AttackSafeSee || see_holds_q(td, q, m, 0)))
      s += attack_bonus[pt];
    if (pt == P && (passed_to >> to & 1) && see_holds_q(td, q, m, 0))
      s += pass_v;
    if (brk_order && pt == P && !get_move_promoted(m) && (pawn_attacks[us][to] & their_pawns))
      s += brk_order;
    if (c2_pk_order && (pt == P || pt == K) && !get_move_promoted(m)) {
      const int d = std::clamp(c2_pk_skip_after(td, m) - c2_pk_node, -c2_pk_scale, c2_pk_scale);
      s += c2_pk_order * d / c2_pk_scale;
    }
    sc[i] = s;
  }
#if defined(TRIUMV_VERIFY_VQ)
  if (vq_on)
    for (int i = 0; i < n; i++)
      if (vbuf[i] != sc[i]) {
        printf("info string VQ WRONG: move %d vector %d scalar %d\n", mv[i], vbuf[i], sc[i]);
        fflush(stdout);
        abort();
      }
#endif
}

// (05/10/2026) The fill functions stay out of queue_next (noinline). Inlined, they brought their lists into
// queue_next: a 5,048-byte frame, over the 4 KB page, so on Windows every call of queue_next (one per move) went
// through the stack probe (__chkstk). The fills run once per stage of the node: their frame stays under 4 KB and the
// probe disappears. Instructions -2.76%, cycles -0.81%, identical nodes.
__attribute__((noinline)) static void fill_tactics(MoveQueue &q) {
  ThreadData &td = *q.td;
  moves ml[1];
  int sc[256];
  if (td_lists_legal())
    ml->count = legal_generate_stage<legalgen::TACTICS>(td, q.lgen, ml->moves);
  else
    td_generate_moves(td, ml, /*captures_only=*/true, /*quiets_only=*/false, /*known_in_check=*/0,
                      /*promo_quiet=*/true, /*promo_queen_only=*/true);
  // The hash move has already been handed out (QS_HASH): it is removed here by compacting in place, without
  // branches, so the loops of queue_next no longer compare it at every move (same order, same tree).
  int n = 0;
  for (int i = 0; i < ml->count; i++) {
    const int m = ml->moves[i];
    ml->moves[n] = m;
    n += m != q.hash_move;
  }
  q.ex = search_band(td);   // expert of the node: capture weights and group thresholds
  q.good_capt = by_exp4(q.ex, StGood0, StGood1, StGood2, StGood3);
  int promo = PromoOrder, bpair = BishopPairCapt;
  if (C2OrdReg) {   // --- C2 ordering (agent ORDER) --- idea 5: the two chess-knowledge terms of the captures by regime
    const C2RegScales rs = c2_reg_scales(td, td.side);
    promo = c2_reg_apply(promo, rs.pass);
    bpair = c2_reg_apply(bpair, rs.minor);
  }
  for (int i = 0; i < n; i++)
    sc[i] = score_tactic(td, ml->moves[i], q.ex, promo, bpair);
  // C2 ideas 2 and 3 (21/10/2026, agent A; OFF by default, SPRT pending), main search only. C2SwitchOrder: a capture
  // or promotion that gives another expert to either perspective moves up. C2ConvOrder (conversion push, the author's
  // idea): a trade of pieces of the same class moves up when the side to move is ahead and down when it is behind
  // (q.c2_conv, move_loop). The score also sets the exchange threshold of a good capture (queue_next): a bonus lets a
  // slightly losing exchange count as good, a malus sends an even trade among the losing captures.
  if (q.kind == QK_MAIN && (C2SwitchOrder || (C2ConvOrder && q.c2_conv))) {
    C2Mat cm;
    if (C2SwitchOrder)
      c2_mat_init(td, cm);
    for (int i = 0; i < n; i++) {
      const int m = ml->moves[i];
      if (C2SwitchOrder && c2_switches(td, cm, m))
        sc[i] += C2SwitchOrder;
      if (C2ConvOrder && q.c2_conv && c2_even_trade(td, m))
        sc[i] += q.c2_conv * C2ConvOrder;
    }
  }
  q.n_tact = q.n_all = n;
  rank_list(q.list, ml->moves, sc, n, INT_MIN);
  q.cur = 0;
}

__attribute__((noinline)) static void fill_quiets(MoveQueue &q) {
  ThreadData &td = *q.td;
  moves ml[1];
  int sc[256];
  if (td_lists_legal())
    ml->count = legal_generate_stage<legalgen::QUIETS>(td, q.lgen, ml->moves);
  else
    td_generate_moves(td, ml, /*captures_only=*/false, /*quiets_only=*/true, /*known_in_check=*/0);
  // The queen promotion is already among the tactical moves and the hash move has been handed out: both are removed
  // by compacting in place, without branches.
  int n = 0;
#if defined(__AVX512F__)
  // (28/10/2026 night, speed, same list) 16 moves at a time: queen promotion = promotion field 4 (Q) or 10 (q); the
  // kept moves are packed in register (vpcompressd) and stored whole. The store at n <= i never reaches the moves not
  // read yet, and the list has room for the 16 lanes (n + 16 <= 256).
  {
    const __m512i hmv = _mm512_set1_epi32(q.hash_move);
    for (int i = 0; i < ml->count; i += 16) {
      const __mmask16 k = ml->count - i >= 16 ? (__mmask16)0xFFFF : (__mmask16)((1u << (ml->count - i)) - 1);
      const __m512i v = _mm512_maskz_loadu_epi32(k, ml->moves + i);
      const __m512i pr = _mm512_and_si512(_mm512_srli_epi32(v, 16), _mm512_set1_epi32(15));
      const __mmask16 keep = _mm512_mask_cmpneq_epi32_mask(k, v, hmv) &
                             _mm512_cmpneq_epi32_mask(pr, _mm512_set1_epi32(Q)) &
                             _mm512_cmpneq_epi32_mask(pr, _mm512_set1_epi32(Q + 6));   // black queen (q)
      _mm512_storeu_si512(ml->moves + n, _mm512_maskz_compress_epi32(keep, v));
      n += count_bits(keep);
    }
  }
#elif defined(__AVX2__)
  // (29/10/2026 night, speed, same list) The same compaction on AVX2, 8 moves at a time: the kept lanes are packed with
  // vpermd and the index list of their 8-bit mask (kPack8). The store at n <= i never reaches the moves not read yet.
  {
    const __m256i hmv = _mm256_set1_epi32(q.hash_move), iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    for (int i = 0; i < ml->count; i += 8) {
      const __m256i k = _mm256_cmpgt_epi32(_mm256_set1_epi32(ml->count - i), iota);
      const __m256i v = _mm256_maskload_epi32(ml->moves + i, k);
      const __m256i pr = _mm256_and_si256(_mm256_srli_epi32(v, 16), _mm256_set1_epi32(15));
      const __m256i drop = _mm256_or_si256(_mm256_cmpeq_epi32(v, hmv),
                                           _mm256_or_si256(_mm256_cmpeq_epi32(pr, _mm256_set1_epi32(Q)),
                                                           _mm256_cmpeq_epi32(pr, _mm256_set1_epi32(Q + 6))));
      const unsigned keep = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_andnot_si256(drop, k)));
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(ml->moves + n), _mm256_permutevar8x32_epi32(v, pack8_idx(keep)));
      n += count_bits(keep);
    }
  }
#else
  for (int i = 0; i < ml->count; i++) {
    const int m = ml->moves[i];
    ml->moves[n] = m;
    n += (promo_kind(m) != Q) & (m != q.hash_move);
  }
#endif
  q.n_all = q.n_tact + n;
  score_quiets(q, ml->moves, sc, n);
  if (LmrRank) {   // LmrRank: the highest score among the quiet moves (including the unsorted ones below the limit)
    int top = n ? sc[0] : 0;
    for (int i = 1; i < n; i++)
      top = std::max(top, sc[i]);
    q.quiet_top = top;
  }
  // D. Expert already chosen by fill_tactics (quiet moves always come after captures, same position): StSort sets how
  //    many quiet moves are sorted, StQuiet the boundary between good and bad ones.
  const int limit = -by_exp4(q.ex, StSort0, StSort1, StSort2, StSort3) * q.depth;
  const int gq = q.good_quiet = by_exp4(q.ex, StQuiet0, StQuiet1, StQuiet2, StQuiet3);
  const int nh = rank_list(q.list + q.n_tact, ml->moves, sc, n, limit);
  q.cur = q.n_tact;
  // Good quiet moves (score > gq) leave in QS_QUIET_HIGH, the others in QS_QUIET_LOW, each in list order. When
  // limit <= gq + 1 (depth >= 5 at the starting values) the unsorted tail has scores < limit, so all <= gq, and the
  // sorted head is decreasing: the good moves are exactly a prefix of the list. The boundary is found once and the two
  // stages become two contiguous ranges, same output order (V2, 08/10/2026, speed at an identical tree). At low depth
  // the stages scan the list and compare each score.
  if (limit <= gq + 1) {
    int b = 0;
    while (b < nh && q.list[q.n_tact + b].score > gq)
      b++;
    q.quiet_split = q.n_tact + b;
#ifdef TRIUMV_VERIFY_QSPLIT   // check: only good moves before the boundary, only bad ones after it
    for (int i = q.n_tact; i < q.n_all; i++)
      if ((q.list[i].score > gq) != (i < q.quiet_split)) {
        printf("info string QSPLIT WRONG: index %d score %d boundary %d\n", i, q.list[i].score, q.quiet_split);
        fflush(stdout);
        abort();
      }
#endif
  }
}

__attribute__((noinline)) static void fill_evasions(MoveQueue &q) {
  ThreadData &td = *q.td;
  moves ml[1];
  int sc[256];
  if (td_lists_legal())
    ml->count = legal_generate_stage<legalgen::ALL>(td, q.lgen, ml->moves);
  else
    td_generate_moves(td, ml, false, false, /*known_in_check=*/1);
  int n = 0;   // without the hash move, already handed out (see fill_tactics)
  for (int i = 0; i < ml->count; i++) {
    const int m = ml->moves[i];
    ml->moves[n] = m;
    n += m != q.hash_move;
  }
  const PieceToRow &c1 = *q.f[-1].cont_hist;
  // Tactical moves ahead of every quiet one: 1 << 20 is enough (|quiet| <= LIM_QUIET + LIM_FOLLOW = 37,183) and fits
  // the 22 bits of the 32-bit keys (05/10/2026; same order, same tree).
  // C. Quiet evasions: main history and first continuation weighted per expert (HeMain, HeCont, /1024; defaults give
  //    the old sum). With weights up to 2048 a quiet move stays below 1 << 20 (|history| + |cont1| <= 37,183 x 2), so
  //    captures (+ 1 << 20) always come first.
  const int eex = q.ex = search_band(td);
  const int he_main = by_exp4(eex, HeMain0, HeMain1, HeMain2, HeMain3), he_cont = by_exp4(eex, HeCont0, HeCont1, HeCont2, HeCont3);
  for (int i = 0; i < n; i++) {
    const int m = ml->moves[i];
    sc[i] = is_tactical(m) ? piece_values_mat(td)[td.piece_on[move_to(m)] & 15] +
                                 pawn_struct_adj(td, move_to(m), td.piece_on[move_to(m)]) + (1 << 20)
                           : (he_main * qh_order(td, td.side, quiet_key(m), quiet_ctx(q, m)) +
                              he_cont * c1[move_piece(m)][move_to(m)]) / 1024;
  }
  q.n_all = q.n_tact = n;
  rank_list(q.list, ml->moves, sc, n, INT_MIN);
  q.cur = 0;
}

// (15/10/2026) Move m, handed out by the queue, comes from a list of the legal generator and not from the hash table:
// it is legal without legal_before or the king test in the make. Always false in Chess960.
static inline bool queue_move_legal(const MoveQueue &q, int m) { return td_lists_legal() && m != q.hash_move; }

// (15/10/2026, speed at an identical tree) When the queue hands out a move from a sorted list, the hash bucket of the
// NEXT move's child is prefetched (estimated key, as in the make). The child's probe waited for memory even with the
// make's prefetch, a few hundred cycles earlier; this one starts a whole subtree earlier, and most subtrees are small.
// Prefetch only: no effect on the tree. The peeked move may not be the one handed out next; then the prefetch is wasted.
// It pays on Intel (i9-14900KF, one engine per core: +1.1/+1.25%) but costs on AMD (identical nodes: Ryzen 7 2700X
// -1.4%, Ryzen 7 5700 -1.6% with one engine, -4.2% with seven). The avx2 build serves both, so the choice is made once
// at startup from the vendor string (CPUID leaf 0). Off outside x86 (not measured). A version that prefetched only the
// moves about to be played was tried and dropped (Intel -0.27%, AMD still negative).
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  #if defined(_MSC_VER)
    #include <intrin.h>
  #else
    #include <cpuid.h>
  #endif
#endif
static bool cpu_vendor_intel() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  unsigned b = 0, c = 0, d = 0;
  #if defined(_MSC_VER)
  int x[4];
  __cpuid(x, 0);
  b = (unsigned)x[1], c = (unsigned)x[2], d = (unsigned)x[3];
  #else
  unsigned a = 0;
  __get_cpuid(0, &a, &b, &c, &d);
  #endif
  return b == 0x756e6547u && d == 0x49656e69u && c == 0x6c65746eu;   // "Genu" "ineI" "ntel"
#else
  return false;
#endif
}
// (15/10/2026) Off in the AVX-512 builds (avx512, vnni512, avx512icl): on the Xeon, without this prefetch, -0.40%
// cycles per node in the middlegame and -0.72% in endgames. On where it was measured: Intel without AVX-512 (12th-14th
// generation) in the avx2, avx2-nopext and avxvnni builds.
#if defined(USE_AVX512)
static const bool g_next_prefetch = false;
#else
static const bool g_next_prefetch = cpu_vendor_intel();
#endif

static inline void queue_prefetch(const MoveQueue &q, int m) {
  if (g_next_prefetch)
    td_prefetch_child(*q.td, td_child_key_guess(*q.td, m));
}

// Next pseudo-legal move, 0 when the queue is empty. The hash move is handed out once.
static int queue_next(MoveQueue &q) {
  ThreadData &td = *q.td;
  q.rank_ok = false;   // LmrRank: set back to true only by the exits of the sorted quiet moves (below)
  for (;;) {
    switch (q.step) {
    case QS_HASH:
      q.step = q.kind == QK_EVADE ? QS_EVADE_FILL : q.kind == QK_MAIN ? QS_TACT_FILL : QS_ONLY_TACT_FILL;
      if (q.hash_move)
        return q.hash_move;
      break;

    case QS_TACT_FILL:
      fill_tactics(q);
      q.step = QS_TACT_WIN;
      break;

    case QS_TACT_WIN:
      while (q.cur < q.n_tact) {
        const ScoredMove sm = q.list[q.cur++];
        // ProbCutMiss: a capture the ProbCut of this node already searched and found below beta goes with the losers.
        bool missed = false;
        for (int k = 0; ProbCutMiss && k < q.pc_miss_n; k++) missed |= q.pc_miss[k] == sm.move;
        PCMISS_STAT(g_pcmiss.demoted += missed);
        // D. Good capture: SEE >= -score x StGood / (19 x 1024); StGood = 1024 is the old -score/19, same truncation.
        if (!missed && see_holds(td, sm.move, (int)(-(long long)sm.score * q.good_capt / 19456))) {
          if (q.cur < q.n_tact)
            queue_prefetch(q, q.list[q.cur].move);
          return sm.move;
        }
        q.losers[q.n_lose++] = (unsigned char)(q.cur - 1);
      }
      q.step = QS_QUIET_FILL;
      break;

    case QS_QUIET_FILL:
      if (!q.quiets_off)
        fill_quiets(q);
      q.step = QS_QUIET_HIGH;
      break;

    // Past the move count (quiets_off) the quiet stages stop (LmpKeepScore, which kept some quiet moves past the count,
    // closed negative and removed 04/11/2026).
    case QS_QUIET_HIGH:
      if (q.quiet_split >= 0) {   // the good moves are the prefix [n_tact, quiet_split)
        while (!q.quiets_off && q.cur < q.quiet_split) {
          const ScoredMove sm = q.list[q.cur++];
          if (q.cur < q.n_all)
            queue_prefetch(q, q.list[q.cur].move);
          q.rank_ok = true, q.last_score = sm.score;
          return sm.move;
        }
      } else
        while (!q.quiets_off && q.cur < q.n_all) {
          const ScoredMove sm = q.list[q.cur++];
          if (sm.score > q.good_quiet) {
            q.rank_ok = true, q.last_score = sm.score;
            return sm.move;
          }
        }
      q.step = QS_TACT_LOSE;
      break;

    case QS_TACT_LOSE:
      if (q.cur_lose < q.n_lose) {
        const int m = q.list[q.losers[q.cur_lose++]].move;
        if (q.cur_lose < q.n_lose)
          queue_prefetch(q, q.list[q.losers[q.cur_lose]].move);
        return m;
      }
      q.cur = q.quiet_split >= 0 ? q.quiet_split : q.n_tact;
      q.step = QS_QUIET_LOW;
      break;

    case QS_QUIET_LOW:
      if (q.quiet_split >= 0) {   // the bad moves are the rest [quiet_split, n_all), already in order
        while (!q.quiets_off && q.cur < q.n_all) {
          const ScoredMove sm = q.list[q.cur++];
          if (q.cur < q.n_all)
            queue_prefetch(q, q.list[q.cur].move);
          q.rank_ok = true, q.last_score = sm.score;
          return sm.move;
        }
      } else
        while (!q.quiets_off && q.cur < q.n_all) {
          const ScoredMove sm = q.list[q.cur++];
          if (sm.score <= q.good_quiet) {
            q.rank_ok = true, q.last_score = sm.score;
            return sm.move;
          }
        }
      q.step = QS_DONE;
      break;

    case QS_EVADE_FILL:
      fill_evasions(q);
      q.step = QS_DRAIN;
      break;

    case QS_ONLY_TACT_FILL:
      fill_tactics(q);
      q.step = QS_DRAIN;
      break;

    case QS_DRAIN:
      while (q.cur < q.n_tact) {
        const ScoredMove sm = q.list[q.cur++];
        if (q.kind == QK_CUTS && !see_holds(td, sm.move, q.cut_threshold))
          continue;
        if (q.cur < q.n_tact)
          queue_prefetch(q, q.list[q.cur].move);
        return sm.move;
      }
      q.step = QS_DONE;
      break;

    default:
      return 0;
    }
  }
}

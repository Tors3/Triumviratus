// search/13_search.inc -- Main search.
// Part of threads.cpp: it is NOT a translation unit of its own.
//
// A node goes through four stages, each a function: entry (draws, mates, hash, tablebases), evaluation and
// cuts before the moves, move loop, learning at the end of the node. The node state lives in NodeState.

// AN (09/10/2026): the search exists in two copies, game (AN = false) and analysis mode (AN = true), chosen once
// at the root (deepen). In games every analysis term is a constant: with the Deep* parameters frozen at 0
// in the release the compiler removes them, as before the analysis mode (xperf: the version with the terms read at
// every node cost ~1% of cycles in games). The two copies give the same values as before in their respective modes.
template <bool AN>
static int explore(ThreadData &td, NodeFrame *f, int alpha, int beta, int depth, bool cut, bool pv);

#ifdef TRIUMV_REDLOG
#include "redlog.h"
#endif
#ifdef TRIUMV_PRUNELOG
#include "prunelog.h"   // block 3 data: which pruning rule decided, and what a search says about it
#else
#define PL_RULE(x) ((void)0)
#define PL_SET(var, v) ((void)0)
#endif
#ifdef TRIUMV_ORDLOG
#include "ordlog.h"     // move-ordering data: where the refuting moves are in the queue order (oracle)
#endif

constexpr int TRACKED = 32;   // tried moves remembered for the maluses

struct NodeState {
  bool pv, root, cut, all;
  bool in_check, improving, opp_worse, tact_hash, prior_capture, hunt_mate;
  bool crystal;                   // AnCrystal: node of the previous principal variation near the root (crystal_node)
  bool no_early;                  // AnKingDanger / AnCycle: no pruning before the moves (analysis only)
  bool no_lmr;                    // AnKingDanger / AnCycle: no positive LMR reductions (analysis only)
  int  alpha, beta, depth;
  int  best, best_move, cap_value;
  int  moves;                     // legal moves searched (the frame's counter is reused by the singular search)
  int  eval, eval_raw, signal;
  int  signal_x;                  // experimental corrections in the evaluation only, not in the selectivity (CorrExpertW)
  int  prev_to;
  int  excluded;
  int  prior_red;
  HashView h;
  int  quiets[TRACKED], n_quiets;
  int  quiet_score[TRACKED];      // HistGrad: ordering score of each quiet move tried (INT_MIN: not from a sorted stage)
  int  best_score;                // HistGrad: ordering score of the best move (INT_MIN: none)
  unsigned char quiet_ctx[TRACKED];   // ThreatHist: threat context of each quiet move tried
  int  best_ctx;                  // ThreatHist: context of the best move (0 if tactical)
  int  tacts[TRACKED], n_tacts;
  U64  causal;                    // CausalRed: squares of the refutation of the last quiet move that failed low (0 = none)
  int  expert;                   // network expert for the node's material (net_phase), for the *Exp* margins
  int  disagree;                 // |psqt - positional| of the network on this node, -1 when the eval came from the hash
  // C2 search ideas (agent A): the Consilium Plebis experts of the node, read only when an idea needs them (g_c2_node).
  int  c2e;                      // expert indexing the C2 margin tables (C2ExpSide: ours or the opponent's perspective)
  int  c2_own;                   // expert of the opponent's perspective: it reads the material of the side to move
  int  cell;                     // C2Cells: expert x pawn-structure cell (0..31), 0 when off
  bool c2_asym;                  // the two perspectives use different experts
  int  c2_pk;                    // conversion push (C2ConvPkSrc 1): exact pawn-king effect of this node, INT_MIN = not read
  int  c2_imb;                   // C2Imb*: +1 ahead, -1 behind by material class and evaluation, 0 otherwise or off
  bool pk_crit;                  // PkCrit: the pawn-king subnetwork adds >= PkCritFrom here (fresh evaluations only)
};

#ifdef TRIUMV_REDLOG
// Re-search data (redlog.h): the current move, already made, searched with a null window at lo, lo+1 and lo+2.
template <bool AN>
static void redres_probes(ThreadData &td, NodeFrame *f, const NodeState &n, int type, int d, int nd, int lo,
                          int value, int move_no, int r, int flags) {
  RedresRec rec{};
  rec.type = (uint8_t)type;
  rec.d = (int8_t)d;
  rec.nd = (int8_t)nd;
  rec.depth = (int16_t)n.depth;
  rec.moves = (int16_t)n.moves;
  rec.move_no = (int16_t)move_no;
  rec.expert = (int16_t)n.expert;
  rec.ply = (int16_t)f->ply;
  rec.stat_score = f->stat_score;
  rec.r = r;
  rec.signal = n.signal;
  rec.hash_known = val_known(n.h.value);
  rec.hash_gap = rec.hash_known ? redres_clip(n.h.value - f->static_eval) : 0;
  rec.flags = (uint16_t)flags;
  rec.m_best = redres_clip(value - n.best);
  rec.m_alpha = redres_clip(value - n.alpha);
  // The probes share the hash table, so a later probe is cheaper than it would be alone: the order is drawn from six
  // permutations and stored in bits 3-5 of done, and the fit reads the cost of a depth only when it ran first.
  static const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  const int perm = int(redlog_counters[td.thread_id * 16 + 3]++ % 6);
  rec.done = uint8_t(perm << 3);
  for (int j = 0; j < 3; j++) {
    const int k = perms[perm][j], depth = lo + k;
    if (depth < 1)
      continue;
    const U64 before = td.nodes;
    const int v = -explore<AN>(td, f + 1, -(n.alpha + 1), -n.alpha, depth, !n.cut, false);
    if (stop_threads.load(std::memory_order_relaxed))
      return;
    rec.v[k] = redres_clip(v - n.alpha);
    rec.nodes[k] = (uint32_t)std::min<U64>(td.nodes - before, 0xffffffffu);
    rec.done |= uint8_t(1 << k);
  }
  redres_write(td.thread_id, rec);
}
#endif

// Per-expert margins (10/10/2026, ours): Consilium evaluates with four experts per material band (net_phase),
// and the trust in the static evaluation is not the same in the four. The levers RfpExp*, FutExp*, LmrExp* shift
// the reverse futility margin, the quiet-move futility margin and the LMR reduction according to the node's expert
// (extension of the per-expert corrections, CorrPhase +3.2). All at 0 = tree unchanged; in the release, with the
// parameters constant at 0, the compiler also removes the computation of the expert.
static inline int by_expert(int e, int v0, int v1, int v2, int v3) {
  return e == 0 ? v0 : e == 1 ? v1 : e == 2 ? v2 : v3;
}

// What the search learns (every statistic and correction update, as events): its own module since C5a, 27/10/2026.
#include "13_learn.inc"

// CausalRed (09/10/2026, June idea "causal pruning", rewritten on the current search). When one of our quiet moves
// fails low, the opponent's move that refutes it (the beta cutoff in the child) says where the problem is:
// the square it starts from, the one it arrives on and the squares the piece attacks from there. The sibling quiet moves tried
// afterwards that touch none of these squares (neither start nor arrival) usually change nothing against that refutation,
// and are reduced more. Never pruned: the reduced search that beats alpha is repeated at full depth as always.
// Checks, the hash move and passer pushes stay out. Known precedent: the "threat" of the null move
// used by older engines to prune unconnected moves (connected threats); here the threat is the refutation
// of the siblings and the move is reduced, not pruned.
// Squares attacked by the piece pc (code P..k) from the square sq with the occupancy occ.
static inline U64 piece_attacks(int pc, int sq, U64 occ) {
  switch (pc % 6) {
    case 0: return pawn_attacks[pc >= 6 ? black : white][sq];
    case 1: return knight_attacks[sq];
    case 2: return get_bishop_attacks(sq, occ);
    case 3: return get_rook_attacks(sq, occ);
    case 4: return get_queen_attacks(sq, occ);
    default: return king_attacks[sq];
  }
}
// King safety for the reduction levers (19/10/2026, ours, off). Squares attacked by colour c with occupancy occ.
static inline U64 attacked_by(const ThreadData &td, int c, U64 occ) {
  U64 att = 0;
  for (int pc = (c == white ? P : p), last = pc + 6; pc < last; pc++)
    for (U64 b = td.bitboards[pc]; b; b &= b - 1)
      att |= piece_attacks(pc, get_ls1b_index(b), occ);
  return att;
}
// Squares next to the king of colour c that the other side attacks (0-8).
static inline int king_ring_attacked(const ThreadData &td, int c) {
  const U64 kb = td.bitboards[c == white ? K : k];
  if (!kb)
    return 0;
  return count_bits(king_attacks[get_ls1b_index(kb)] & attacked_by(td, c ^ 1, td.occupancies[both]));
}
static inline U64 causal_mask(const ThreadData &td, int refute) {
  const int from = get_move_source(refute), to = get_move_target(refute), pc = get_move_piece(refute);
  return piece_attacks(pc, to, td.occupancies[both]) | (1ULL << from) | (1ULL << to);
}

static constexpr int NO_VALUE = 1 << 30;   // "no value": the stage did not close the node

// 40-bit reciprocals of 256*depth + LmrAllDivBase (divisor of the reduction at all nodes, see move_loop): M =
// ceil(2^40 / D). Evening of 12/10/2026: with LmrAllDivBase and HistDiv* as parameters, in the release the tables stay constants
// computed at compile time; in dev they are recomputed at every setoption and at startup (param_tables_refresh).
static constexpr auto make_all_node_recip = [](int base) {
  std::array<unsigned long long, max_ply + 8> r{};
  for (int d = 0; d < max_ply + 8; d++) {
    const unsigned long long D = 256ULL * d + (unsigned long long)base;
    r[d] = ((1ULL << 40) + D - 1) / D;
  }
  return r;
};
// History divisor per depth, a parabola of ours across depth (30/10/2026, adopted with the P2 bake of 01/11/2026):
// HistDivMid + HistDivCurve * (d - 8)^2 / 64 + HistDivSlope * (d - 8) / 8, times (1024 + HistDivScale + HistDivTilt *
// (d - 8)) / 1024, clamped to [256, 8192] (up to 8192 the 40-bit reciprocal stays exact for |hist| < 2^27). It replaced
// an inherited 16-entry table that zigzagged by +-8% around 3083 (the parabola fitted it with rms 227 against a spread
// of 253); the table was removed on 01/11/2026, source in _backup/Triumviratus_8.1_pre_rimozione_histdiv_2026-11-01.
static constexpr auto make_hist_div = [](int scale, int tilt, int mid, int curve, int slope) {
  std::array<int, 16> r{};
  for (int i = 0; i < 16; i++) {
    const long long d = i + 1, k = 1024LL + scale + (long long)tilt * (d - 8);
    const long long base = mid + curve * (d - 8) * (d - 8) / 64 + slope * (d - 8) / 8;
    r[i] = (int)std::clamp(base * k / 1024, 256LL, 8192LL);
  }
  return r;
};
static constexpr auto make_hist_recip = [](const std::array<int, 16> &div) {
  std::array<unsigned long long, 16> r{};
  for (int i = 0; i < 16; i++)
    r[i] = ((1ULL << 40) + (unsigned long long)div[i] - 1) / (unsigned long long)div[i];
  return r;
};
// ---------------------------------------------------------------------------
// C2 search ideas (21/10/2026, agent A; parameters and the expert scale in 01_params.inc, block "C2 search ideas").
// The margins per C2 expert sit in one table built from the parameters (dev: at every setoption, as the tables
// below; release: constants). g_c2_node: some idea reads the experts of the node, computed once in node_open;
// g_c2_red: some idea moves the reduction of a move. Both false at the defaults, so a node pays one test.
// ---------------------------------------------------------------------------
#define C2_TAB_LIST                                                                                                    \
  {{C2Rfp0, C2Rfp1, C2Rfp2, C2Rfp3, C2Rfp4, C2Rfp5, C2Rfp6, C2Rfp7},                                                    \
   {C2Fut0, C2Fut1, C2Fut2, C2Fut3, C2Fut4, C2Fut5, C2Fut6, C2Fut7},                                                    \
   {C2Lmr0, C2Lmr1, C2Lmr2, C2Lmr3, C2Lmr4, C2Lmr5, C2Lmr6, C2Lmr7},                                                    \
   {C2Nmp0, C2Nmp1, C2Nmp2, C2Nmp3, C2Nmp4, C2Nmp5, C2Nmp6, C2Nmp7}}
enum { C2T_RFP, C2T_FUT, C2T_LMR, C2T_NMP };
static constexpr bool c2_tab_any(const int (&t)[4][8]) {
  for (int i = 0; i < 4; i++)
    for (int e = 0; e < 8; e++)
      if (t[i][e]) return true;
  return false;
}
#ifdef TRIUMV_RELEASE
static constexpr int c2_tab[4][8] = C2_TAB_LIST;
static constexpr bool g_c2_node = c2_tab_any(c2_tab) || C2AsymRfp || C2AsymLmr;
static constexpr bool g_c2_red = C2SwitchRed || C2ConvRed || C2PkRed || C2PkVol || C2AsymLmr;
#endif
// The C2 evaluation terms (C2Conv*, C2EvalExp0..7) live in the bridge (consilium_bridge.cpp). Review 21/10/2026: they
// were pushed only at the start of a search, so "eval" and nnperft after a setoption ran with the old values (nnperft
// then checked nothing of the terms). Now also pushed at every setoption (dev) and by nnperft.
static void c2_eval_push() {
  const int conv[3] = {C2ConvW, C2ConvFrom, C2ConvCap};
  const int exp_scale[8] = {C2EvalExp0, C2EvalExp1, C2EvalExp2, C2EvalExp3,
                            C2EvalExp4, C2EvalExp5, C2EvalExp6, C2EvalExp7};
  nn_set_c2_eval(conv, exp_scale);
}
// Offsets per cell (C2 expert x pawn-structure class, 01_params.inc) for reverse futility, quiet futility, LMR, null
// move, razoring, ProbCut and move count, added on top of the per-band tables where those rules read them. They do not
// depend on C2Cells (author, 26/10: "split it while staying byte-identical"): the piece bands stay the base, all offsets
// 0 = the identical tree, and any offset not 0 turns the cell on (g_cell_on: the node computes its cell). C2Cells 1/2
// replaces the band by the cell's slot instead (experimental, lost). Dev: rebuilt at every setoption.
#define C2_CELL_LIST                                                                                                         \
  {{C2cRfp00, C2cRfp01, C2cRfp02, C2cRfp03, C2cRfp10, C2cRfp11, C2cRfp12, C2cRfp13,                                   \
    C2cRfp20, C2cRfp21, C2cRfp22, C2cRfp23, C2cRfp30, C2cRfp31, C2cRfp32, C2cRfp33,                                   \
    C2cRfp40, C2cRfp41, C2cRfp42, C2cRfp43, C2cRfp50, C2cRfp51, C2cRfp52, C2cRfp53,                                   \
    C2cRfp60, C2cRfp61, C2cRfp62, C2cRfp63, C2cRfp70, C2cRfp71, C2cRfp72, C2cRfp73},                                  \
   {C2cFut00, C2cFut01, C2cFut02, C2cFut03, C2cFut10, C2cFut11, C2cFut12, C2cFut13,                                   \
    C2cFut20, C2cFut21, C2cFut22, C2cFut23, C2cFut30, C2cFut31, C2cFut32, C2cFut33,                                   \
    C2cFut40, C2cFut41, C2cFut42, C2cFut43, C2cFut50, C2cFut51, C2cFut52, C2cFut53,                                   \
    C2cFut60, C2cFut61, C2cFut62, C2cFut63, C2cFut70, C2cFut71, C2cFut72, C2cFut73},                                  \
   {C2cLmr00, C2cLmr01, C2cLmr02, C2cLmr03, C2cLmr10, C2cLmr11, C2cLmr12, C2cLmr13,                                   \
    C2cLmr20, C2cLmr21, C2cLmr22, C2cLmr23, C2cLmr30, C2cLmr31, C2cLmr32, C2cLmr33,                                   \
    C2cLmr40, C2cLmr41, C2cLmr42, C2cLmr43, C2cLmr50, C2cLmr51, C2cLmr52, C2cLmr53,                                   \
    C2cLmr60, C2cLmr61, C2cLmr62, C2cLmr63, C2cLmr70, C2cLmr71, C2cLmr72, C2cLmr73},                                  \
   {C2cNmp00, C2cNmp01, C2cNmp02, C2cNmp03, C2cNmp10, C2cNmp11, C2cNmp12, C2cNmp13,                                   \
    C2cNmp20, C2cNmp21, C2cNmp22, C2cNmp23, C2cNmp30, C2cNmp31, C2cNmp32, C2cNmp33,                                   \
    C2cNmp40, C2cNmp41, C2cNmp42, C2cNmp43, C2cNmp50, C2cNmp51, C2cNmp52, C2cNmp53,                                   \
    C2cNmp60, C2cNmp61, C2cNmp62, C2cNmp63, C2cNmp70, C2cNmp71, C2cNmp72, C2cNmp73},                                  \
   {C2cRaz00, C2cRaz01, C2cRaz02, C2cRaz03, C2cRaz10, C2cRaz11, C2cRaz12, C2cRaz13,                                   \
    C2cRaz20, C2cRaz21, C2cRaz22, C2cRaz23, C2cRaz30, C2cRaz31, C2cRaz32, C2cRaz33,                                   \
    C2cRaz40, C2cRaz41, C2cRaz42, C2cRaz43, C2cRaz50, C2cRaz51, C2cRaz52, C2cRaz53,                                   \
    C2cRaz60, C2cRaz61, C2cRaz62, C2cRaz63, C2cRaz70, C2cRaz71, C2cRaz72, C2cRaz73},                                  \
   {C2cPcut00, C2cPcut01, C2cPcut02, C2cPcut03, C2cPcut10, C2cPcut11, C2cPcut12, C2cPcut13,                           \
    C2cPcut20, C2cPcut21, C2cPcut22, C2cPcut23, C2cPcut30, C2cPcut31, C2cPcut32, C2cPcut33,                           \
    C2cPcut40, C2cPcut41, C2cPcut42, C2cPcut43, C2cPcut50, C2cPcut51, C2cPcut52, C2cPcut53,                           \
    C2cPcut60, C2cPcut61, C2cPcut62, C2cPcut63, C2cPcut70, C2cPcut71, C2cPcut72, C2cPcut73},                          \
   {C2cLmp00, C2cLmp01, C2cLmp02, C2cLmp03, C2cLmp10, C2cLmp11, C2cLmp12, C2cLmp13,                                   \
    C2cLmp20, C2cLmp21, C2cLmp22, C2cLmp23, C2cLmp30, C2cLmp31, C2cLmp32, C2cLmp33,                                   \
    C2cLmp40, C2cLmp41, C2cLmp42, C2cLmp43, C2cLmp50, C2cLmp51, C2cLmp52, C2cLmp53,                                   \
    C2cLmp60, C2cLmp61, C2cLmp62, C2cLmp63, C2cLmp70, C2cLmp71, C2cLmp72, C2cLmp73}}
enum { C2C_RFP, C2C_FUT, C2C_LMR, C2C_NMP, C2C_RAZ, C2C_PCUT, C2C_LMP };
static constexpr bool cell_tab_any(const int (&t)[7][32]) {
  for (int i = 0; i < 7; i++)
    for (int c = 0; c < 32; c++)
      if (t[i][c]) return true;
  return false;
}
#ifdef TRIUMV_RELEASE
static constexpr int c2_cell_tab[7][32] = C2_CELL_LIST;
static constexpr bool g_cell_on = cell_tab_any(c2_cell_tab);
#else
static int c2_cell_tab[7][32];
static bool g_cell_on = false;
static void c2_cells_refresh() {
  const int t[7][32] = C2_CELL_LIST;
  std::memcpy(c2_cell_tab, t, sizeof(c2_cell_tab));
  g_cell_on = cell_tab_any(c2_cell_tab);
  const int b[32] = {C2CellBand00, C2CellBand01, C2CellBand02, C2CellBand03, C2CellBand10, C2CellBand11,
      C2CellBand12, C2CellBand13, C2CellBand20, C2CellBand21, C2CellBand22, C2CellBand23, C2CellBand30, C2CellBand31,
      C2CellBand32, C2CellBand33, C2CellBand40, C2CellBand41, C2CellBand42, C2CellBand43, C2CellBand50, C2CellBand51,
      C2CellBand52, C2CellBand53, C2CellBand60, C2CellBand61, C2CellBand62, C2CellBand63, C2CellBand70, C2CellBand71,
      C2CellBand72, C2CellBand73};
  for (int c = 0; c < 32; c++)
    g_cell_band[c] = (unsigned char)std::clamp(b[c], 0, 3);
}
#endif
#undef C2_CELL_LIST
// n.cell is computed only when an offset is set or C2Cells is on; otherwise it is 0 and every entry is 0 as well
static inline int cell_off(int fam, const NodeState &n) { return g_cell_on ? c2_cell_tab[fam][n.cell] : 0; }
#ifndef TRIUMV_RELEASE
static int  c2_tab[4][8];
static bool g_c2_node = false, g_c2_red = false;
static void c2_tables_refresh() {
  const int t[4][8] = C2_TAB_LIST;
  std::memcpy(c2_tab, t, sizeof(c2_tab));
  g_c2_node = c2_tab_any(c2_tab) || C2AsymRfp || C2AsymLmr;
  g_c2_red = C2SwitchRed || C2ConvRed || C2PkRed || C2PkVol || C2AsymLmr;
  c2_eval_push();
  c2_cells_refresh();
}
#endif
#undef C2_TAB_LIST

// ---------------------------------------------------------------------------
// C2 regime selectivity (21/10/2026, ours; docs/rete/C2_IDEE_FABLE_2026-10-21.md ideas 7, 8 and 11). OFF by default;
// SPRT pending. Parameters in 01_params.inc, block "C2 regime selectivity". Only cheap reads: the expert pair the make
// already stores in the network's state (c2_experts_td, one load), its switch flag (c2_last_switched) and the queue's
// own label of losing captures. No network work.
//
// Idea 7, same-regime comparisons (C2SameRegImp, bits). improving, opp_worse and the PriorRed* depth steps compare
// static evaluations of different plies; when a capture or a promotion between the two plies gave another expert to
// either perspective, the two values come from different learned regimes, and the flag follows the jump between
// experts rather than the position. Each frame keeps its pair (NodeFrame::c2pair, node_open); the comparison is made
// only inside one pair: improving looks back to f[-4] when f[-4] shares the pair of the node (else false, or true with
// bit 8), opp_worse is false after a switch, the PriorRed* steps are skipped after a switch. Closest closed idea:
// PhaseEdgeImproving (05/10, stopped at -14.1 +- 19.4 over 296 games, not a measurement): its trigger was a crossing of
// the piece-count bands of Consilium, which also fired on pawn captures (13% of captures). Here the trigger is the
// exact change of the C2 pair, per perspective, from non-pawn captures and promotions only.
//
// Idea 8, imbalance-directed selectivity (C2Imb*). The expert of a perspective follows the OPPONENT's non-pawn
// material, so the pair gives the material class of both sides: d = class(ours) - class(theirs) = e_them - e_us. With
// the corrected static evaluation it splits the nodes: ahead (d >= C2ImbGap, eval > C2ImbEval), behind (d <= -C2ImbGap,
// eval < -C2ImbEval), and the rest (balance, or compensation: material down and evaluation up), left alone. Behind:
// checks and losing captures (the queue's QS_TACT_LOSE stage) are reduced less, and the move-count threshold scaled
// (the defender must look wider). Ahead: the exchange test of a capture that takes the opponent's last piece of a type
// is relaxed (simplification), and the null-move floor is lowered. Depth and pruning only, no value change (unlike the
// Tal style, -40, which biased the root values toward sacrifices).
//
// Idea 11, regime-settled horizon (C2SettleHorizon). A capture or promotion that switches an expert of either
// perspective and would leave its child to the quiescence search gets one ply of real moves instead, so that one reply
// tests the new expert's view before the stand-pat closes the line; once per line (NodeFrame::settle_path, like the
// negext guard), not in all nodes with 1. Closest closed idea: TransitionExt (the capture that leaves only pawns, -18.2
// +- 16.4 over 402 on UHO, +0.16 +- 4.79 over 2,234 endgames): it extended at any depth, without a guard on the line,
// on the transition to a pawn ending only. Here the trigger is the C2 switch (0.108 per capture against 0.262 for the
// old bands), only at the horizon, at most once per line.
// ---------------------------------------------------------------------------
#ifdef TRIUMV_RELEASE
static constexpr bool g_c2_imb = C2ImbBehind || C2ImbBehindLmp || C2ImbAhead || C2ImbAheadNmp;
#else
static bool g_c2_imb = false;
static void c2_sel_refresh() { g_c2_imb = C2ImbBehind || C2ImbBehindLmp || C2ImbAhead || C2ImbAheadNmp; }
#endif
// Material direction of the node for the side to move: +1 ahead, -1 behind, 0 otherwise (idea 8).
static inline int c2_imbalance(const ThreadData &td, int static_eval) {
  const int e = c2_experts_td(td), us = td.side;
  const int d = ((e >> (4 * (us ^ 1))) & 15) - ((e >> (4 * us)) & 15);   // e_them (our material) - e_us (theirs)
  if (d >= C2ImbGap && static_eval > C2ImbEval)
    return 1;
  if (d <= -C2ImbGap && static_eval < -C2ImbEval)
    return -1;
  return 0;
}

#define HIST_DIV_ARGS HistDivScale, HistDivTilt, HistDivMid, HistDivCurve, HistDivSlope
#ifdef TRIUMV_RELEASE
static constexpr std::array<int, 16> hist_div = make_hist_div(HIST_DIV_ARGS);
static constexpr auto all_node_recip = make_all_node_recip(LmrAllDivBase);
static constexpr auto hist_recip = make_hist_recip(hist_div);
#else
static std::array<int, 16> hist_div;
static std::array<unsigned long long, max_ply + 8> all_node_recip;
static std::array<unsigned long long, 16> hist_recip;
static void param_tables_refresh() {
  c2_tables_refresh();
  c2_sel_refresh();
  hist_div = make_hist_div(HIST_DIV_ARGS);
  all_node_recip = make_all_node_recip(LmrAllDivBase);
  hist_recip = make_hist_recip(hist_div);
}
static const bool param_tables_ready = (param_tables_refresh(), true);
#endif
#undef HIST_DIV_ARGS
// W1 (08/10/2026): reciprocals of depth + 1 (mean of the cutoff value in node_close), same truncation.
static constexpr auto succ_recip = [] {
  std::array<unsigned long long, max_ply + 8> r{};
  for (int d = 0; d < max_ply + 8; d++)
    r[d] = ((1ULL << 40) + (unsigned long long)d) / (unsigned long long)(d + 1);
  return r;
}();

// ---------------------------------------------------------------------------
// "Long time" ramp (05/10/2026). Every lever has an offset Lt*: the effective value is base + Lt * weight, with the
// weight (0..1024) fixed per iteration by the root depth (lt_update, 14_deepen.inc). With the offset at 0
// the accessor returns the base lever: in the release (constants) the branch disappears and the code is the old one.
// ---------------------------------------------------------------------------
static inline int lt_value(int base, int offset, int w) { return base + offset * w / 1024; }

static inline void lt_update(ThreadData &td) {
  td.lt_w = std::clamp((td.root_depth - LtKnee) * 1024 / LtSpan, 0, 1024);
  const int w = td.lt_w;
  td.lt_lmr_not_improving = lt_value(LmrNotImproving, LmrNotImprovingLt, w);
  td.lt_lmr_offset        = lt_value(LmrOffset, LmrOffsetLt, w);
  td.lt_lmr_all_node      = lt_value(LmrAllNode, LmrAllNodeLt, w);
  td.lt_lmr_no_tt_full    = lt_value(LmrNoTTFull, LmrNoTTFullLt, w);
  td.lt_nmp_improving     = lt_value(NmpImproving, NmpImprovingLt, w);
  td.lt_quiet_fut_alpha   = lt_value(QuietFutAlpha, QuietFutAlphaLt, w);
  td.lt_rfp_max_depth     = lt_value(RfpMaxDepth, RfpMaxDepthLt, w);
  td.lt_nmp_verify_depth  = lt_value(NmpVerifyDepth, NmpVerifyDepthLt, w);
  if (LmrLogMulLt) {
    const int mul = lt_value(LmrLogMul, LmrLogMulLt, w);
    if (mul != td.lt_lmr_log_mul) {   // same formula as build_reduction_table, with the effective multiplier
      td.lt_lmr_log_mul = mul;
      td.lt_red_log[0] = 0;
      for (int i = 1; i < 256; i++)
        td.lt_red_log[i] = int(mul / 128.0 * std::log(double(i)));
    }
  }
}
static inline int e_lmr_not_improving(const ThreadData &td) { return LmrNotImprovingLt ? td.lt_lmr_not_improving : LmrNotImproving; }
static inline int e_lmr_offset(const ThreadData &td)        { return LmrOffsetLt ? td.lt_lmr_offset : LmrOffset; }
static inline int e_lmr_all_node(const ThreadData &td)      { return LmrAllNodeLt ? td.lt_lmr_all_node : LmrAllNode; }
static inline int e_lmr_no_tt_full(const ThreadData &td)    { return LmrNoTTFullLt ? td.lt_lmr_no_tt_full : LmrNoTTFull; }
static inline int e_nmp_improving(const ThreadData &td)     { return NmpImprovingLt ? td.lt_nmp_improving : NmpImproving; }
static inline int e_quiet_fut_alpha(const ThreadData &td)   { return QuietFutAlphaLt ? td.lt_quiet_fut_alpha : QuietFutAlpha; }
static inline int e_rfp_max_depth(const ThreadData &td)     { return RfpMaxDepthLt ? td.lt_rfp_max_depth : RfpMaxDepth; }
// Always the thread's value (09/10/2026): lt_update computes it per iteration and the analysis mode (AN_NMPV)
// overwrites it; read only after a successful null move, so at no cost. In games it equals NmpVerifyDepth.
static inline int e_nmp_verify_depth(const ThreadData &td)  { return td.lt_nmp_verify_depth; }
static inline const int *e_red_log(const ThreadData &td)    { return LmrLogMulLt ? td.lt_red_log : red_log; }

// ---------------------------------------------------------------------------
// Block 2 of 8.1 (20/10/2026): the reduction in force, the one of 8.0, in our form. Units of 1/1024 ply.
//   entry  the value the pruning rules read: a product of two logarithms (depth and move number), less the relative
//          window, more when the node is not improving, plus a constant; then the analysis style (AnGold, neutral in
//          games), a slope and a level per expert, and a credit for hash entries of the principal variation.
//   final  the value the search uses: the node (principal-variation hash and its value and depth, the offset, the move
//          number, the correction, cut node, hash capture), then the move (children cutoffs or the hash move, the
//          statistic score, the gap between alpha and the evaluation for quiet moves), a share more in all nodes that
//          fades with depth, less for passed pushes, more for quiet moves away from a sibling's refutation.
// `window_term` = window * LmrDeltaMul / root_delta, computed by move_loop only when the window changes (a division by
// root_delta at every move cost about 30 cycles). Numbers once written in the code are parameters at their old values.
// ---------------------------------------------------------------------------
static inline int deep_term(const ThreadData &td, int offset);   // below, with the ideas of 04/10/2026

static inline int red_entry_v80(const ThreadData &td, const NodeFrame *f, const NodeState &n, int move_no,
                                int window_term, int gd_style) {
  const int *rl = e_red_log(td);
  const int scale = rl[std::min(n.depth, 255)] * rl[std::min(move_no, 255)];
  int r = scale - window_term + (!n.improving ? scale * e_lmr_not_improving(td) / 512 : 0) + LmrBase;
  if (gd_style)   // AnGold: 97% in attack, 103% in manoeuvre
    r = gd_style == 1 ? r * 97 / 100 : gd_style == 2 ? r * 103 / 100 : r;
  if (LmrScaleExp0 | LmrScaleExp1 | LmrScaleExp2 | LmrScaleExp3)
    r = r * (1024 + by_expert(n.expert, LmrScaleExp0, LmrScaleExp1, LmrScaleExp2, LmrScaleExp3)) / 1024;
  if (f->tt_pv)
    r += LmrTtPvAdd;
  return r + by_expert(n.expert, LmrExp0, LmrExp1, LmrExp2, LmrExp3) + cell_off(C2C_LMR, n);
}

static inline int red_final_v80(const ThreadData &td, const NodeFrame *f, const NodeState &n, int m, int r, bool tact,
                                bool gives, bool passer, int node_red) {
  // The node.
  if (f->tt_pv)
    r -= LmrTtPvSub + deep_term(td, DeepTtPv) + n.pv * LmrTtPvPv + (n.h.value > n.alpha) * LmrTtPvAlpha +
         (n.h.depth >= n.depth) * (LmrTtPvDepth + n.cut * LmrTtPvCut);
  r += e_lmr_offset(td) - n.moves * LmrMoveCount - std::abs(n.signal) / LmrCorrDiv;
  if (n.cut)
    r += LmrCutNode + LmrCutNoTT * !n.h.move;
  if (n.tact_hash)
    r += LmrTTCapture;
  r += node_red;
  // The move.
  if (f[1].cutoff_cnt > 1)
    r += LmrCutoffBase + LmrCutoffMany * (f[1].cutoff_cnt > 2) + LmrCutoffAll * n.all;
  else if (m == n.h.move)
    r -= LmrTTMove;
  r -= f->stat_score * LmrStatDiv / 4096;
  if (!tact && !val_decided(n.alpha))
    r += LmrGapMul * std::clamp(n.alpha - n.eval, -LmrGapLo, LmrGapHi);
  if (n.all) {
    // r * LmrAllNode / (256 * depth + LmrAllDivBase), truncated toward zero, as a multiplication by the 40-bit
    // reciprocal of the table: exact while |r * LmrAllNode| < 2^24 (the divisor reaches 35,000), a real division
    // beyond (never seen).
    const long long x = (long long)r * e_lmr_all_node(td);
    const unsigned long long mag = (unsigned long long)(x < 0 ? -x : x);
    const int share = mag < (1ULL << 24) ? (int)((mag * all_node_recip[n.depth]) >> 40)
                                         : (int)(mag / (unsigned long long)(256 * n.depth + LmrAllDivBase));
    r += x < 0 ? -share : share;
  }
  if (passer) {
    // C2OrdReg (idea 5): the same per-expert scale as the passer ordering terms, C2PassExp[e_us] of the side that has
    // just moved (td.side ^ 1: the move is already made). C2OrdReg 0, or every scale 1024: the identical reduction.
    r -= C2OrdReg ? c2_reg_apply(PassedPushRed, c2_reg_scales(td, td.side ^ 1).pass) : PassedPushRed;
  }
  // CausalRed: a quiet move that touches none of the squares of a sibling's refutation is reduced more.
  if (CausalRed && n.causal && !tact && !gives && !passer && m != n.h.move &&
      !(((1ULL << move_from(m)) | (1ULL << move_to(m))) & n.causal))
    r += CausalRed;
  return r;
}

// ---------------------------------------------------------------------------
// Our ideas of 04/10/2026 (docs/audit_8.0/M_STUDIO_IDEE_RICERCA.md). All off by default: with the parameters
// at 0 the terms are 0 and in the release (constant parameters) the compiler removes them.
// ---------------------------------------------------------------------------

// Idea 2 (Deep*): "deep" offset weighted by td.deep_w (0..1024, grows with the root depth).
// Since 09/10/2026 the six levers the analysis mode uses (Rfp, Fut, Lmr, Nmp, Sing, Ext) arrive already weighted in the
// fields td.dz_* (deepen, per iteration): same value as before, but the analysis mode can turn them on also
// in the release, where the parameters are constants. deep_term stays for DeepHist and DeepTtPv.
static inline int deep_term(const ThreadData &td, int offset) { return offset * td.deep_w / 1024; }

// Crystal (AnCrystal, analysis mode): is the node on the principal variation of the previous iteration, near
// the root? In games crystal_ply = -1 and the test is always false.
static inline bool crystal_node(const ThreadData &td, const NodeFrame *f) {
  return f->ply <= td.crystal_ply && f->follow_pv;
}

// King in danger (AnKingDanger, Pos Solver, 14/10/2026; idea of Crystal, our code): at least 3 of the squares around
// the king of side `s` are attacked by the opponent. Up to 8 attack tests: computed only in analysis with the
// component on.
// AnNmpZug: at most 5 legal moves, as in Crystal. First version with the pseudo-legal ones (night of 14/10/2026): it
// almost never fired (1 position out of 383 with a different search). Called only out of check (null move), where
// legal_before holds.
// The same count serves the endgame rule NmpFewMoves (18/10/2026) with its own limit.
static bool few_moves(ThreadData &td, int limit = 5) {
  moves ml;
  td_generate_moves(td, &ml);
  MoveSafety s;
  s.ready = false;
  int legal = 0;
  for (int i = 0; i < ml.count; i++)
    if (legal_before(td, s, ml.moves[i]) && ++legal > limit)
      return false;
  return true;
}

static bool king_in_danger(ThreadData &td, int s) {
  const int ksq = get_ls1b_index(td.bitboards[s == white ? K : k]);
  U64 zone = king_attacks[ksq];
  int hit = 0;
  while (zone) {
    const int sq = get_ls1b_index(zone);
    zone &= zone - 1;
    if (td_is_square_attacked(td, sq, s ^ 1) && ++hit >= 3)
      return true;
  }
  return false;
}

// GoldDigger (AnGold, idea of ShashChess/amchess, our code): the null move is skipped when the tactical "heat"
// reaches 4. The root part (king in danger or possible sacrifice +10, clearly winning position +2,
// attack +2, advanced pawn +2, more than 30 legal moves +1) is in td.gd_heat; here are added the non-pawn
// material of the side to move (> 2000, +1) and the depth (> 8 and > 12, +1 each). In games gd_heat = -1000.
static inline bool gd_skip_null(const ThreadData &td, const NodeState &n) {
  if (td.gd_heat < -100)
    return false;
  const int off = td.side == white ? 0 : 6;
  const int *pv = piece_values_mat(td);
  int npm = 0;
  for (int t = N; t <= Q; t++)
    npm += pv[t + off] * count_bits(td.bitboards[t + off]);
  return td.gd_heat + (npm > 2000) + (n.depth > 8) + (n.depth > 12) >= 4;
}

// Time and limit check (thread 0 only, every 512 nodes).
static int g_poll_countdown = 0;
static void poll_limits(ThreadData &td) {
  if (td.thread_id != 0 || --g_poll_countdown > 0)
    return;
  g_poll_countdown = g_limits.nodes ? (int)std::min<U64>(512, g_limits.nodes / 1024 + 1) : 512;
  if (g_pondering.load(std::memory_order_relaxed))
    return;
  const int elapsed = get_time_ms() - g_limits.start_ms;
  extern int clock_hard_limit();
  U64 total = 0;
  if (g_limits.nodes)
    for (auto &t : thread_data) total += t.nodes;
  if ((g_limits.use_tm() && (elapsed > clock_hard_limit() || g_stop_at_ponderhit.load(std::memory_order_relaxed)) &&
       root_move_ready()) ||
      (g_limits.movetime && elapsed >= g_limits.movetime && root_move_ready()) ||
      (g_limits.nodes && total >= g_limits.nodes))
    stop_threads.store(true, std::memory_order_relaxed);
}

// --------------------------------------------------------------------------
// Stage 1: node entry. Returns a value if the node closes here.
// --------------------------------------------------------------------------
static int node_open(ThreadData &td, NodeFrame *f, NodeState &n) {
  // The threshold is the draw value for the side to move (0 without Contempt): so alpha never drops (05/10/2026,
  // porting audit: with threshold 0 Contempt lowered alpha on the root side).
  if (!n.root && n.alpha < contempt(td) && td_upcoming_repetition(td)) {
    n.alpha = draw_score(td);
    if (n.alpha >= n.beta)
      return n.alpha;
  }
  f->move_count = 0;
  f->follow_pv = n.root || (f[-1].follow_pv && td.ply - 1 < td.last_pv_len && f[-1].move == td.last_pv[td.ply - 1]);
  poll_limits(td);
  if (n.pv && td.sel_depth < td.ply + 1)
    td.sel_depth = td.ply + 1;

  if (!n.root) {
    if (stop_threads.load(std::memory_order_relaxed) || td.ply >= max_ply || drawn_here(td, n.in_check))
      return (td.ply >= max_ply && !n.in_check) ? static_evaluate(td) : draw_score(td);
    // BAKE INSUFMATERIAL (11/10/2026, agent AE): insufficient material after a capture (12_quiesce.inc).
    if (f[-1].captured >= 0 && insufficient_material(td))
      return draw_score(td);
  }

  // The hash entry is read before the window is narrowed to the reachable mate distances (order of ours, 28/10/2026):
  // the read depends on the position only, and a node that closes on the mate distance does not read its frame again.
  n.excluded = f->excluded;
  hash_read(td, n.h);
  f->tt_hit = n.h.hit;
  if (n.root)
    n.h.move = td.root[td.pv_idx].pv[0];
  if (!n.excluded)
    f->tt_pv = n.pv || (n.h.hit && n.h.pv);
  n.tact_hash = n.h.move && is_tactical(n.h.move);

  // Mate distance: no score above a mate at the next ply, none below being mated here.
  if (!n.root) {
    n.alpha = std::max(mated_at(td.ply), n.alpha);
    n.beta = std::min(mating_in(td.ply + 1), n.beta);
    if (n.alpha >= n.beta)
      return n.alpha;
  }

  n.prev_to = move_ok(f[-1].move) ? move_to(f[-1].move) : -1;
  n.prior_red = f[-1].reduction;
  f[-1].reduction = 0;
  f->stat_score = 0;
  f[2].cutoff_cnt = 0;
  f[1].null_cuts = 0;   // NmpSibCut: the children of this node count their null-move cuts afresh
  n.signal = corr_signal(td, f);
  n.signal_x = corr_signal_extra(td);
  // per-band tables: piece band of Consilium, or with C2Cells the slot of the node's cell (01_params.inc)
  n.cell = (C2Cells || g_cell_on) ? c2_cell(td) : 0;
  n.expert = C2Cells ? g_cell_band[n.cell] : search_band(td);
  n.c2e = n.c2_own = 0;
  n.c2_asym = false;
  n.c2_pk = INT_MIN;
  n.pk_crit = false;
  if (g_c2_node) {   // C2 ideas 1, 6, 7: the two experts of the node (from the make's state, one load)
    const int e_us = c2_expert(td, td.side), e_them = c2_expert(td, td.side ^ 1);
    n.c2e = C2ExpSide ? e_them : e_us;
    n.c2_own = e_them;
    n.c2_asym = e_us != e_them;
  }
  n.c2_imb = 0;
  if (C2SameRegImp)   // C2 idea 7 (regime selectivity): the expert pair of this node, for the comparisons of its children
    f->c2pair = (unsigned char)c2_experts_td(td);
  return NO_VALUE;
}

// --------------------------------------------------------------------------
// CUTOFF FROM THE HASH (block 5 in our form, 17/10/2026)
// An entry ends the node when it is deep enough and its bound settles the comparison with beta. In order:
//   1. depth: an upper bound (value <= beta) may lack HashLowSlack plies; any other entry must be deeper than the node;
//   2. node type: an entry cuts on the side the node expects (cut node and fail high, or the reverse), and on both
//      sides above depth HashAnyNode;
//   3. learning: a quiet hash move that fails high is rewarded, the previous move is penalized if it came early;
//   4. guards: no cutoff close to the fifty-move rule (HashFiftyGuard); from depth HashVerify the child of the hash
//      move, if it is in the table, must agree with the cutoff;
//   5. the returned value: a lower bound is pulled toward beta (HashDamp, ours after an idea seen in Coda);
//   6. an entry deep enough whose bound contradicts the window loses a ply above depth HashWeaken.
// Defaults = the 8.0 tree.
// --------------------------------------------------------------------------
static inline int hash_cut_value(const NodeState &n) {
  const int v = n.h.value;
  if (HashDamp && n.h.bound == BOUND_LOW && v >= n.beta && !val_decided(v))
    return (HashDamp * v + 10 * n.beta) / (HashDamp + 10);
  return v;
}

// Does the child of the hash move, when the table knows it, confirm the side of beta the entry claims?
static bool hash_child_agrees(ThreadData &td, const NodeState &n) {
  const HashView &h = n.h;
  MoveSafety s;
  s.ready = false;
  if (!legal_before(td, s, h.move))
    return true;
  UndoInfo u;
  td.repetition_table[++td.repetition_index] = td.hash_key;
  td.ply++;
  bool agrees = true;
  if (td_make_move(td, h.move, u)) {
    int cm, cv, cd, cf, ce;
    bool cp;
    const bool known = probe_tt(tt_key(td), cm, cv, cd, cf, ce, cp) && cf != BOUND_NIL;
    const int child = known ? from_hash_value(cv, td.ply, td.fifty) : VAL_NONE;
    td_unmake_move(td, h.move, u);
    agrees = !val_known(child) || (h.value >= n.beta) == (-child >= n.beta);
  }
  td.ply--;
  td.repetition_index--;
  return agrees;
}

static int hash_cutoff(ThreadData &td, NodeFrame *f, NodeState &n) {
  const HashView &h = n.h;
  const int need = n.depth - (h.value <= n.beta ? HashLowSlack : 0);
  const bool deep = h.depth > need;
  // HashNearMiss (an entry one ply short that cuts beyond a margin, seen in Coda): off. Removed on 05/10/2026 (-6.7 +-
  // 10.6 over 1,142 games at 20+0.2) and put back off on 12/10 for a second look; 0 = old tree.
  if (HashNearMiss && !deep && !n.pv && !n.cut && !n.excluded && h.depth == need && td.fifty < 90 &&
      h.bound == BOUND_UP && val_known(h.value) && !val_decided(h.value) && h.value + HashNearMiss <= n.alpha)
    return h.value + HashNearMiss;
  if (n.pv || n.excluded || !deep || !val_known(h.value))
    return NO_VALUE;
  const bool high = h.value >= n.beta;
  if (!bound_covers(h.bound, high) || (n.cut != high && n.depth <= HashAnyNode)) {
    if (h.bound == (high ? BOUND_UP : BOUND_LOW) && n.depth > HashWeaken)   // a deep bound against the window
      hash_soften(tt_key(td));
    return NO_VALUE;
  }
  if (h.move && high)
    on_hash_cut(td, f, n);
  if (td.fifty >= HashFiftyGuard)
    return NO_VALUE;
  if (n.depth >= HashVerify && h.move && td_is_pseudo_legal(td, h.move) && !val_decided(h.value) &&
      !hash_child_agrees(td, n))
    return NO_VALUE;
  return hash_cut_value(n);
}

// Once a move has raised alpha, the remaining moves of the node are searched shallower (AlphaRedLo < depth <
// AlphaRedHi). Our form (28/10/2026, plan point 3): the cut is AlphaRed plus a term per expert of the node
// (AlphaRedExp, plies) plus a term in how far the move cleared the old alpha, jump * AlphaRedJumpW / 4096 plies
// within [-1, 2] (positive weight: a move far above the old alpha leaves the others less to prove). The cut is never
// negative and the depth never drops below 1. AlphaRedExp and AlphaRedJumpW at 0 = the old tree (AlphaRed is at most
// AlphaRedLo, so depth - AlphaRed >= 1).
static inline int alpha_raised_cut(const NodeState &n, int jump) {
  const int by_jump = std::clamp((int)((long long)jump * AlphaRedJumpW / 4096), -1, 2);
  return std::max(AlphaRed + by_exp4(n.expert, AlphaRedExp0, AlphaRedExp1, AlphaRedExp2, AlphaRedExp3) + by_jump, 0);
}

// A lower bound in the hash well above beta, found after the pruning before the moves, closes the node without the
// move loop (the step stays: its removal, SmallProbCut 0, leaned negative on 27-28/10/2026, -1.81 +- 8.26 over 1,724
// games at 10+0.1 after -8 at the start). Our form (28/10/2026, plan point 3: replace, do not remove): the margin
// over beta is ProbCutTTMargin plus SpcGapW per ply by which the entry is shallower than the node (an entry deeper
// than the node gives a negative term, at most 4 plies of it) plus a term per expert of the node (SpcExp). An entry
// up to SmallPcSlack plies shorter than the node is accepted. The node returns beta plus that margin. SpcGapW and
// SpcExp at 0 = the old tree (bench 279330).
static inline int hash_bound_cut(const NodeState &n) {
  const HashView &h = n.h;
  if (!bound_covers(h.bound, true) || h.depth < n.depth - SmallPcSlack || val_decided(n.beta) || !val_known(h.value) ||
      val_decided(h.value))
    return NO_VALUE;
  const int shallower = std::max(n.depth - h.depth, -4);
  const int margin = ProbCutTTMargin + SpcGapW * shallower + by_exp4(n.expert, SpcExp0, SpcExp1, SpcExp2, SpcExp3);
  return h.value >= n.beta + margin ? n.beta + margin : NO_VALUE;
}

// Tablebases in interior nodes.
static int tablebase_probe(ThreadData &td, NodeFrame *f, NodeState &n) {
  if (n.root || n.excluded)
    return NO_VALUE;
  unsigned men = syzygy_max_pieces();
  if (g_syzygy_probe_limit >= 0 && (unsigned)g_syzygy_probe_limit < men)
    men = (unsigned)g_syzygy_probe_limit;
  if (!men || td.castle || td.fifty || g_tb_root_draw_search)   // TbDrawSearch: no probe while choosing among draws
    return NO_VALUE;
  const unsigned pieces = (unsigned)count_bits(td.occupancies[both]);
  if (pieces > men || (pieces == men && n.depth < g_syzygy_probe_depth))
    return NO_VALUE;
  int tb;
  if (!syzygy_probe_wdl(td, td.ply, tb))
    return NO_VALUE;
  td.tb_hits++;
  if (td.thread_id == 0)
    g_poll_countdown = 0;
  const int bound = tb > 0 ? BOUND_LOW : tb < 0 ? BOUND_UP : BOUND_EXACT;
  if (bound == BOUND_EXACT)
    tb = contempt(td);   // tablebase draw: the same value as the other draws (0 without Contempt)
  if (bound == BOUND_EXACT || (bound == BOUND_LOW ? tb >= n.beta : tb <= n.alpha)) {
    hash_write(td, tb, f->tt_pv, bound, std::min(max_ply - 1, n.depth + TbHashDepth), 0, tt_eval_none);
    return tb;
  }
  if (n.pv) {
    if (bound == BOUND_LOW) {
      n.best = tb;
      n.alpha = std::max(n.alpha, n.best);
    } else
      n.cap_value = tb;
  }
  return NO_VALUE;
}

// --------------------------------------------------------------------------
// Stage 2: evaluation and cuts before the move loop.
// --------------------------------------------------------------------------
static void node_evaluate(ThreadData &td, NodeFrame *f, NodeState &n) {
  n.eval_raw = tt_eval_none;
  n.disagree = -1;
  if (n.in_check) {
    f->static_eval = n.eval = f[-2].static_eval;
  } else if (n.excluded) {
    n.eval = f->static_eval;
    n.eval_raw = n.h.hit ? n.h.eval_raw : tt_eval_none;
  } else {
    nn_last_disagree = -1;
    if (C2PkStale)   // shallow node: a pawn-king miss reuses the last outputs (01_params.inc)
      nn_set_pk_stale(td.nnpos, n.depth <= C2PkStale);
    const int plain = node_eval(td, n.h, n.eval_raw);
    n.disagree = nn_last_disagree;   // -1 when node_eval took the evaluation from the hash
    // Conversion push (C2ConvPkSrc 1): the network has just evaluated this very position, so its l1 sums give the
    // exact pawn-king effect for one tail pass. From the hash: read at the move loop (move_loop, c2_conv_side).
    if (c2_conv_exact() && n.disagree >= 0)
      n.c2_pk = c2_pk_effect_fresh(td);
    // PkCrit (28/10/2026, ours, OFF): the same exact effect, read only at a fresh evaluation (reused when the
    // conversion push has just read it); a node evaluated from the hash stays unmarked.
    if ((PkCritRed | PkCritPrune) && n.disagree >= 0)
      n.pk_crit = std::abs(n.c2_pk != INT_MIN ? n.c2_pk : c2_pk_effect_fresh(td)) >= PkCritFrom;
    f->static_eval = n.eval = corrected_eval(plain, n.signal + n.signal_x);
    if (n.h.hit) {
      if (val_known(n.h.value) && bound_covers(n.h.bound, n.h.value > n.eval))
        n.eval = n.h.value;
    } else
      hash_write(td, VAL_NONE, f->tt_pv, BOUND_NIL, HD_EVAL, 0, n.eval_raw);
  }
  // The opponent's view got worse since its own evaluation: read by the hindsight step below (the node's own trend,
  // improving, is set after the hash cutoff and the tablebases, node_trend).
  n.opp_worse = f->static_eval > -f[-1].static_eval;
  // C2 idea 7 (regime selectivity, 21/10/2026; OFF by default, SPRT pending): the comparisons with earlier plies only
  // inside one expert pair. Frames before the root carry no pair, hence the ply guards.
  bool c2_prior_ok = true;
  if (C2SameRegImp) {
    const int sr = C2SameRegImp, pair = f->c2pair;
    const bool switched = f->ply >= 1 && f[-1].c2pair != pair;
    if ((sr & 2) && switched)
      n.opp_worse = false;
    c2_prior_ok = !((sr & 4) && switched);
  }
  // C2 idea 8 (regime selectivity): material direction of the node, read by the null move, the pruning of single
  // moves and the reductions.
  if (g_c2_imb && !n.in_check && !val_decided(f->static_eval))
    n.c2_imb = c2_imbalance(td, f->static_eval);
  // Hindsight on the parent's reduction (PriorRed*, CENSUS 21/10/2026: were 3, 2, 2).
  if (c2_prior_ok) {
    if (n.prior_red >= PriorRedHigh && !n.opp_worse)
      n.depth++;
    if (n.prior_red >= PriorRedLow && n.depth >= PriorRedMinDepth && f->static_eval + f[-1].static_eval > PriorRedEvalSum)
      n.depth--;
  }
}

// The node's trend: is its static evaluation above the one of two plies ago? Read only by the stages that search
// (pruning before the moves, move loop), so it is set once the hash cutoff and the tablebases have let the node go on
// (order of ours, 28/10/2026). C2SameRegImp bit 1 compares inside one expert pair (four plies back, or the default of
// bit 8, when the pair changed two plies ago).
static inline void node_trend(const NodeFrame *f, NodeState &n) {
  n.improving = f->static_eval > f[-2].static_eval;
  if (C2SameRegImp & 1) {
    const int pair = f->c2pair;
    if (!n.in_check && f->ply >= 2 && f[-2].c2pair != pair)
      n.improving = (f->ply >= 4 && f[-4].c2pair == pair) ? f->static_eval > f[-4].static_eval : (C2SameRegImp & 8) != 0;
  }
}

// --------------------------------------------------------------------------
// Block 3 of 8.1 (20/10/2026): pruning before the moves, in our form.
// The node is tested in four steps, each a function of its own: the evaluation gates (razoring below alpha, reverse
// futility above beta), the null-move probe, the shallower search of a node without a hash move, and the capture
// probe (ProbCut). Every number that was written in the code is a parameter at its old value, and the terms per
// expert of the network start at 0, so the tree is the one of 8.0 (bench unchanged).
// --------------------------------------------------------------------------

// Evaluation gates. Below alpha by a margin quadratic in depth, the quiescence search answers for the node
// (razoring); above beta by a margin that grows with depth, the node holds without a search (reverse futility) and
// returns a blend of beta and the evaluation. Both margins have a term per expert (RazorExp ours at 0; RfpExp from
// 8.0); the reverse-futility margin also widens with the correction and, past a threshold, with the disagreement
// between the two outputs of the network (RfpDisagree, ours, off). gd_razor and gd_rfp_pct belong to the analysis
// mode (AnGold) and are neutral in games; n.crystal is always false outside AnCrystal.
template <bool AN>
static int reverse_futility(ThreadData &td, NodeFrame *f, NodeState &n);

// Order of ours (27/10/2026): reverse futility first, razoring after. At the current values the two cannot both fire
// (razoring needs the evaluation below alpha minus a positive margin, reverse futility at or above beta), so the tree
// is the same; the razoring margin is still computed first for the data build (pl_razor).
// Order of ours (28/10/2026): both gates give their verdict first; then the change of the evaluation since the previous
// ply teaches the move that led here (on_eval_change), and only then does a gate act. The verdicts read the evaluation
// and the window, the lesson writes move statistics, so neither reads what the other writes; the lesson still comes
// before the quiescence search of razoring, which reads those statistics, and it is taken by every node that reaches
// the gates, as before.
template <bool AN>
static int eval_gates(ThreadData &td, NodeFrame *f, NodeState &n) {
  int held = NO_VALUE;
  bool razor_on = false;
  if (!(AN && n.crystal)) {
    const int razor = RazorMul * n.depth * n.depth + RazorLin * n.depth +
                      by_expert(n.expert, RazorExp0, RazorExp1, RazorExp2, RazorExp3) +
                      cell_off(C2C_RAZ, n) + (AN ? td.gd_razor : 0);
    PL_SET(pl_razor, razor);
    held = reverse_futility<AN>(td, f, n);
    // RazorAllOnly (02/11/2026, ours, OFF; idea retried from the C20 list): razoring only where a fail low is
    // expected anyway, the ALL nodes.
    razor_on = held == NO_VALUE && !n.pv && (!RazorAllOnly || n.all) && n.eval < n.alpha - razor;
  }
  on_eval_change(td, f, n);
  if (held != NO_VALUE)
    return held;
  if (razor_on) {
    PL_RULE(1);
    return quiesce(td, f, n.alpha, n.beta, false);
  }
  return NO_VALUE;
}

template <bool AN>
static int reverse_futility(ThreadData &td, NodeFrame *f, NodeState &n) {
  const int max_depth = n.hunt_mate ? RfpHuntDepth : e_rfp_max_depth(td);
  if (f->tt_pv || n.depth >= max_depth || n.eval < n.beta || (n.h.move && !n.tact_hash) || val_lost(n.beta) ||
      val_won(n.eval))
    return NO_VALUE;
  int mult = std::min(RfpMultBase + n.depth * RfpMultDepth, RfpMultCap) + (AN ? td.dz_rfp : deep_term(td, DeepRfp));
  if (AN && td.gd_rfp_pct != 100)
    mult = mult * td.gd_rfp_pct / 100;
  mult -= RfpNoTTHit * !f->tt_hit;
  mult += by_expert(n.expert, RfpExp0, RfpExp1, RfpExp2, RfpExp3) + cell_off(C2C_RFP, n);
  if (g_c2_node)   // C2 idea 1 (21/10/2026, agent A): the multiplier per Consilium Plebis expert. OFF by default; SPRT pending
    mult += c2_tab[C2T_RFP][n.c2e];
  const int lean = RfpImproving * n.improving + RfpOppWorse * n.opp_worse;   // the node's trend, per 1024 of mult
  // Margin in our form (27/10/2026, docs/audit_8.1/R9): + RfpMarginBase, a constant that does not grow with depth, and
  // the correction read with its sign as well as its size: corr = signal / RfpCorrDiv adds |corr| (as before; integer
  // division truncates toward zero, so |s|/D = |s/D|) plus RfpCorrSign/1024 of corr (positive = less trust in an
  // evaluation the correction has raised). Both at 0: the old margin.
  const int corr = n.signal / RfpCorrDiv;
  int margin = mult * n.depth - lean * mult / 1024 + std::abs(corr) + RfpCorrSign * corr / 1024 + RfpMarginBase;
  // C2 idea 7 (21/10/2026, agent A): the two perspectives evaluate with different experts (the routing table puts the
  // material of the two sides in different classes): the evaluation of an unbalanced position is trusted less (or more,
  // with a negative value). OFF by default; SPRT pending.
  if (C2AsymRfp && n.c2_asym)
    margin += C2AsymRfp;
  // RfpBehind / RfpAll (28/10/2026, ours, OFF; search-ideas agent, prunelog of 27/10, match_moe/idee_ricerca_2026-10-28):
  // at equal depth and surplus eval - beta, the cuts of a side whose static evaluation is far below zero were wrong
  // x2.1 / x2.9 / x5.1 at depths 1-2 / 3-5 / 6-12 (static < -700), and the cuts of ALL nodes x2.73 at every depth. So
  // the margin grows with the deficit beyond RfpBehindFrom (capped at 2048 units), RfpBehind/4096 per unit plus
  // RfpBehindD/4096 per unit and ply, and by RfpAll in ALL nodes. All 0: the old margin.
  if (RfpBehind | RfpBehindD) {
    const int def = std::min(std::max(-f->static_eval - RfpBehindFrom, 0), 2048);
    margin += def * (RfpBehind + RfpBehindD * n.depth) / 4096;
  }
  if (RfpAll && n.all)
    margin += RfpAll;
  // RfpDisagree: in the prunelog data the error of the static evaluation is 12-45% larger in the quarter of nodes
  // with the most disagreement, at equal size of the evaluation, and flat below; so the margin grows only past a
  // threshold per expert (its upper quartile).
  if (RfpDisagree && n.disagree >= 0) {
    const int over = n.disagree - by_expert(n.expert, RfpDisThr0, RfpDisThr1, RfpDisThr2, RfpDisThr3);
    if (over > 0)
      margin += RfpDisagree * std::min(over, 4096) / 1024;
  }
  PL_SET(pl_rfp, margin);
  if (n.eval - margin < n.beta)
    return NO_VALUE;
  PL_RULE(2);
  return (RfpBetaWeight * n.beta + (1024 - RfpBetaWeight) * n.eval) / 1024;
}

// Null-move probe: the side to move passes; if the reduced search still stays above beta, the node is cut. Deep
// nodes verify the cut with a reduced search of their own, during which no null move is tried before NmpVerifyNum
// quarters of the remaining depth. Gates: an expected cut node, an evaluation above beta minus a margin (with a term
// per expert, NmpExp, ours at 0), a piece other than pawns, beta not deep in a loss, and the endgame rules that skip
// the null move with very few legal moves. DeepNmp is in 1024ths of a ply, rounded with ten bits of the key.
template <bool AN>
static int null_probe(ThreadData &td, NodeFrame *f, NodeState &n) {
  const int us = td.side;
  const bool pieces = td.occupancies[us] & ~td.bitboards[us == white ? P : p] & ~td.bitboards[us == white ? K : k];
  int floor = n.beta - NmpDepthMul * n.depth - e_nmp_improving(td) * n.improving + NmpBase +
              by_expert(n.expert, NmpExp0, NmpExp1, NmpExp2, NmpExp3) + cell_off(C2C_NMP, n);
  // C2 idea 1 (21/10/2026, agent A; OFF by default, SPRT pending): the floor per Consilium Plebis expert.
  if (g_c2_node)
    floor += c2_tab[C2T_NMP][n.c2e];
  // C2 idea 8 (regime selectivity, 21/10/2026; OFF by default, SPRT pending): ahead in material class and evaluation,
  // passing is rarely wrong, so the floor is lower.
  if (C2ImbAheadNmp && n.c2_imb > 0)
    floor -= C2ImbAheadNmp;
  // NmpSibCut (02/11/2026, ours, OFF; idea retried from the C20 list): every earlier sibling that was cut by its own
  // null move says the parent's move left the opponent little to do, so passing is less risky here too: the floor
  // drops by NmpSibCut per such sibling, at most NmpSibCap of them.
  if (NmpSibCut)
    floor -= NmpSibCut * std::min<int>(f->null_cuts, NmpSibCap);
  PL_SET(pl_nmp_floor, floor - n.beta);
  if (!n.cut || f->static_eval < floor || n.excluded || !pieces || td.ply < td.nmp_min_ply || n.beta < -NmpBetaFloor ||
      (AN && gd_skip_null(td, n)) ||
      (AN && (td.an_bits & AN_NMPZUG) && td.root_depth >= 11 && td.side != td.root_side && few_moves(td)) ||
      (NmpFewMoves && count_bits(td.occupancies[both]) <= NmpFewPieces && few_moves(td, NmpFewMoves)))
    return NO_VALUE;
  // NmpNpmFloor (22/10/2026, was C2NmpMask): little non-pawn material left for the side that would pass, where
  // zugzwang is common. Counted on the board, so it does not depend on the network.
  if (NmpNpmFloor) {
    const int o = 6 * us;
    const int npm = 3 * count_bits(td.bitboards[N + o] | td.bitboards[B + o]) + 5 * count_bits(td.bitboards[R + o]) +
                    9 * count_bits(td.bitboards[Q + o]);
    if (npm <= NmpNpmFloor)
      return NO_VALUE;
  }
  // Reduction in our form (27/10/2026, docs/audit_8.1/R9): the whole plies of the inherited rule, NmpRedBase +
  // d/NmpDepthDiv + the evaluation term, plus a part in 1024ths of a ply rounded with ten bits of the key: DeepNmp,
  // NmpRedDepth per ply of depth (a continuous slope on top of the integer d/NmpDepthDiv) and NmpRedImp on an
  // improving node. Both at 0: the old reduction.
  const int r = NmpRedBase + n.depth / NmpDepthDiv + std::max((f->static_eval - n.beta) / NmpEvalDiv, 0) +
                (((AN ? td.dz_nmp : deep_term(td, DeepNmp)) + NmpRedDepth * n.depth + NmpRedImp * n.improving +
                  (int)((td.hash_key >> 20) & 1023)) >> 10);
  NullUndo nu;
  step_in_null(td, f, nu);
  f[1].in_check = false;
  const int nv = -explore<AN>(td, f + 1, -n.beta, -n.beta + 1, n.depth - r, false, false);
  step_out_null(td, nu);
  if (nv < n.beta || val_won(nv))
    return NO_VALUE;
  if (td.nmp_min_ply || n.depth < e_nmp_verify_depth(td)) {
    PL_RULE(3);
    f->null_cuts += f->null_cuts < 255;
    return nv;
  }
  td.nmp_min_ply = td.ply + NmpVerifyNum * (n.depth - r) / 4;
  const int v = explore<AN>(td, f, n.beta - 1, n.beta, n.depth - r, false, false);
  td.nmp_min_ply = 0;
  if (v < n.beta)
    return NO_VALUE;
  PL_RULE(4);
  f->null_cuts += f->null_cuts < 255;
  return nv;
}

// Capture probe (ProbCut): a capture that clears a raised beta in quiescence and then in a reduced search cuts the
// node. The raise has a term per expert (ProbCutExp, ours at 0); the search is ProbCutRed plies shallower, and
// ProbCutRedImp more when the node is improving.
template <bool AN>
static int capture_probe(ThreadData &td, NodeFrame *f, NodeState &n) {
  const int cut_beta = n.beta + ProbCutMargin - ProbCutImproving * n.improving + ProbCutDepth * n.depth / 16 +
                       by_expert(n.expert, ProbCutExp0, ProbCutExp1, ProbCutExp2, ProbCutExp3) + cell_off(C2C_PCUT, n);
  if (n.depth < ProbCutMinDepth || val_decided(n.beta) || (val_known(n.h.value) && n.h.value < cut_beta))
    return NO_VALUE;
  MoveQueue q;
  queue_start(q, td, f, n.h.move, n.depth, QK_CUTS);
  q.cut_threshold = cut_beta - f->static_eval;
  MoveSafety s;
  s.ready = false;
  const int cut_depth = n.depth - ProbCutRed - ProbCutRedImp * n.improving;
  int m;
  while ((m = queue_next(q)) != 0) {
    const bool gen_legal = queue_move_legal(q, m);
    if (m == n.excluded || (!gen_legal && !legal_before(td, s, m)))
      continue;
    const bool gives = checks_before(td, s, m);
    UndoInfo u;
    if (!step_in(td, f, m, u, gen_legal))
      continue;
    f[1].in_check = gives;
    int v = -quiesce(td, f + 1, -cut_beta, -cut_beta + 1, false);
    if (v >= cut_beta && cut_depth > 0)
      v = -explore<AN>(td, f + 1, -cut_beta, -cut_beta + 1, cut_depth, !n.cut, false);
    step_out(td, m, u);
    if (v < cut_beta)
      continue;
    hash_write(td, v, f->tt_pv, BOUND_LOW, cut_depth + ProbCutHashDepth, m, n.eval_raw);
    if (!val_decided(v)) {
      PL_RULE(5);
      return v - (cut_beta - n.beta);
    }
  }
  return NO_VALUE;
}

template <bool AN>
static int prune_before_moves(ThreadData &td, NodeFrame *f, NodeState &n) {
  int v;   // the evaluation gates also hold the lesson of the evaluation change (on_eval_change)
  if ((v = eval_gates<AN>(td, f, n)) != NO_VALUE)
    return v;
  if ((v = null_probe<AN>(td, f, n)) != NO_VALUE)
    return v;
  n.improving |= f->static_eval >= n.beta;
  // A node without a hash move, off the previous line and not an all node, is searched one ply shallower.
  if (!f->follow_pv && !n.all && n.depth >= IirDepth && !n.h.move)
    n.depth--;
  return capture_probe<AN>(td, f, n);
}

// --------------------------------------------------------------------------
// Stage 3: the move loop.
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// Block 3 of 8.1 (20/10/2026): pruning of single moves, in our form. Three gates; since 28/10/2026 (order of ours) a
// move meets first its own gate, the quiet gate (history, futility on a depth corrected by the history, exchange test)
// or the capture gate (futility and exchange test of captures and checks), and then the move count (past a threshold
// per node the queue stops handing out quiet moves after this one).
// Numbers once written in the code are parameters at their old values; terms per expert start at 0.
// --------------------------------------------------------------------------

// Quiet gate: a quiet move with a very bad continuation history is skipped; otherwise the history corrects its depth
// (divided by a divisor per depth, our parabola HistDivMid/Curve/Slope with the level HistDivScale and the slope
// HistDivTilt, see make_hist_div; as a multiplication by the reciprocal: exact for these sizes), and
// the move is skipped when the evaluation plus a margin on that depth stays at or below alpha (futility, which also
// raises the node's best value to the margin), or when the exchange test fails by more than a margin quadratic in
// the corrected depth.
template <bool AN>
static bool quiet_gate(ThreadData &td, NodeFrame *f, NodeState &n, MoveQueue &q, int m, int red_depth, int ctx) {
  const int us = td.side, pc = move_piece(m), to = move_to(m);
  if (PkCritPrune && n.pk_crit && kPieceKind[pc] == P)   // PkCrit: the structure decides here, pawn moves are searched
    return false;
  if (BreakPrune && pawn_break(td, m))   // pawn breaks (28/10/2026, ours, OFF): searched, not pruned by this gate
    return false;
  // Node type and "static above alpha" in the quiet gate (28/10/2026, ours, OFF; prunelog of 27/10: quiet moves pruned in
  // CUT nodes wrong x1.8-2.3, in ALL nodes x0.4; with static >= alpha history prune x3.0, exchange test x2.7, futility
  // x2.0 even with QuietFutAlpha). Positive values prune less: the history threshold grows by (HistCut in CUT nodes -
  // HistAll in ALL nodes + HistAbove when static >= alpha) per ply, the futility margin by FutCut / -FutAll, the exchange
  // test accepts SeeAbove more per ply of corrected depth when static >= alpha. All 0: the old gate.
  const bool above = f->static_eval >= n.alpha;
  int hist = (*f[-1].cont_hist)[pc][to] + (*f[-2].cont_hist)[pc][to] + pawn_row(td)[pc][to];
  if (hist < -(QuietHistPrune * n.depth + QuietHistBase +
               (HistCut * n.cut - HistAll * n.all + HistAbove * above) * n.depth)) {
    PL_RULE(13);
    return true;
  }
  hist += QuietMainHistMul * qh_main(td, us, quiet_key(m), ctx) / 32;
  assert(std::abs(hist) < (1 << 27));
  const int d_index = std::min(n.depth, 16) - 1;
  const unsigned long long mag = (unsigned long long)(hist < 0 ? -hist : hist);
  int shift = (int)((mag * hist_recip[d_index]) >> 40);
  if (DeepHist)
    shift = shift * (1024 + deep_term(td, DeepHist)) / 1024;
  red_depth += hist < 0 ? -shift : shift;
  int fut = f->static_eval + QuietFutDepth * red_depth + e_quiet_fut_alpha(td) * (f->static_eval > n.alpha) +
            QuietFutBase + (red_depth > 0 ? QuietFutQuad * red_depth * red_depth / 16 : 0) +
            (AN ? td.dz_fut : deep_term(td, DeepFut)) +
            by_expert(n.expert, FutExp0, FutExp1, FutExp2, FutExp3) + cell_off(C2C_FUT, n);
  if (g_c2_node)   // C2 idea 1 (21/10/2026, agent A): the margin per Consilium Plebis expert. OFF by default; SPRT pending
    fut += c2_tab[C2T_FUT][n.c2e];
  fut += FutCut * n.cut - FutAll * n.all;   // node type (28/10/2026, ours, OFF; see above)
  PL_SET(pl_fut, fut - n.alpha);
  if (!n.in_check && red_depth < QuietFutMaxDepth && fut <= n.alpha) {
    if (n.best <= fut && !val_decided(n.best) && !val_won(fut))
      n.best = fut;
    PL_RULE(14);
    return true;
  }
  red_depth = std::max(red_depth, 0);
  if (!see_holds_q(td, q, m, -(QuietSeeMul * red_depth * red_depth + QuietSeeLin * red_depth +
                               SeeAbove * above * red_depth))) {
    PL_RULE(15);
    return true;
  }
  return false;
}

// Capture gate: a capture (or a check) is skipped when even its victim and a margin leave the evaluation at or
// below alpha, or when the exchange test fails by more than a margin. The last piece is never offered with alpha
// below the draw (it may lead to stalemate).
template <bool AN>
static bool capture_gate(ThreadData &td, NodeFrame *f, const NodeState &n, MoveQueue &q, int m, bool gives,
                         int red_depth, U64 own_pieces, bool pr_exp) {
  const int pc = move_piece(m), to = move_to(m);
  const int vk = victim_kind(td, m);
  const int ch = td.capt_hist[pc][to][vk];
  if (!gives && red_depth < CaptFutMaxDepth) {
    const int fut = f->static_eval + CaptFutBase + CaptFutDepth * red_depth +
                    (piece_values_mat(td)[td.piece_on[to] & 15] + pawn_struct_adj(td, to, td.piece_on[to])) *
                        CaptFutVictim / 1024 +
                    CaptFutHist * ch / 1024 +
                    (pr_exp ? by_expert(n.expert, CaptFutExp0, CaptFutExp1, CaptFutExp2, CaptFutExp3) : 0) +
                    CaptFutCut * n.cut - CaptFutAll * n.all   // node type (28/10/2026, ours, OFF; positive = prune less)
                    ;
    if (fut <= n.alpha) {
      PL_RULE(11);
      return true;
    }
  }
  int margin = CaptSeeDepth * n.depth + CaptSeeQuad * n.depth * n.depth / 16 + ch * CaptSeeHist / 1024 +
               (pr_exp ? by_expert(n.expert, CaptSeeExp0, CaptSeeExp1, CaptSeeExp2, CaptSeeExp3) : 0);
  // C2 idea 8 (regime selectivity, 21/10/2026; OFF by default, SPRT pending): ahead, a capture that takes the
  // opponent's last piece of a type simplifies toward the won ending; its exchange test is relaxed.
  if (C2ImbAhead && n.c2_imb > 0 && vk >= N && vk <= Q &&
      count_bits(td.bitboards[vk + 6 * (td.side ^ 1)]) == 1)
    margin += C2ImbAhead;
  const bool last_piece = count_bits(own_pieces) == 1 && (own_pieces >> move_from(m) & 1);
  if ((n.alpha >= contempt(td) || !last_piece) && !see_holds_q(td, q, m, -margin)) {
    PL_RULE(12);
    return true;
  }
  return false;
}

// Move count (our form, 27/10/2026): past LmpBase + (LmpLin*d + LmpQuad*d^2)/16 moves, scaled by LmpNotImp/1024 when
// the node is not improving, the queue stops handing out quiet moves. Computed in sixteenths of a move and 1024ths of
// the scale, one shift at the end: at 0/16/512 it equals the old (LmpBase + d^2) halved when not improving.
// LmpExp scales the threshold per expert (from PrExpRootDepth, pr_exp).
template <bool AN>
static void count_gate(ThreadData &td, NodeFrame *f, const NodeState &n, MoveQueue &q, bool pr_exp) {
  const long long lmp_16 = std::max(16LL * LmpBase + (long long)LmpLin * n.depth + (long long)LmpQuad * n.depth * n.depth, 0LL);
  int lmp_count = (int)((lmp_16 * (n.improving ? 1024 : LmpNotImp)) >> 14);
  {   // per-band scale (from PrExpRootDepth) and the C2Cells offset of the node's cell, /1024
    const int lx = (pr_exp && (LmpExp0 | LmpExp1 | LmpExp2 | LmpExp3) ? by_expert(n.expert, LmpExp0, LmpExp1, LmpExp2,
                                                                                  LmpExp3) : 0) + cell_off(C2C_LMP, n);
    if (lx)
      lmp_count = lmp_count * (1024 + lx) / 1024;
  }
  // C2 idea 8 (regime selectivity, 21/10/2026; OFF by default, SPRT pending): behind, the defender looks wider.
  if (C2ImbBehindLmp && n.c2_imb < 0)
    lmp_count = lmp_count * (1024 + C2ImbBehindLmp) / 1024;
  // Node type and "static above alpha" in the move count (28/10/2026, ours, OFF; prunelog of 27/10): moves pruned in CUT
  // nodes were wrong x1.8-2.3, in ALL nodes x0.4; LMP with static >= alpha x6.1. The threshold scales by /1024 (positive
  // = more moves searched): LmpCut in CUT nodes, minus LmpAll in ALL nodes, LmpAbove when the static evaluation is at
  // or above alpha. All 0: the old count.
  if (LmpCut | LmpAll | LmpAbove) {
    const int s = LmpCut * n.cut - LmpAll * n.all + LmpAbove * (f->static_eval >= n.alpha);
    lmp_count = lmp_count * (1024 + s) / 1024;
  }
  if (f->move_count >= lmp_count && !(AN && (td.an_bits & AN_LMPNEAR) && (n.pv || f->ply <= 2))) {   // AnLmpNear
#ifdef TRIUMV_PRUNELOG
    q.pl_count_off = true;
#else
    q.quiets_off = true;
#endif
  }
}

// Pruning of a move before it is played; true = skip it. n.crystal belongs to AnCrystal only.
template <bool AN>
static bool skip_move(ThreadData &td, NodeFrame *f, NodeState &n, MoveQueue &q, int m, bool tact, bool gives,
                      int r, int ctx) {
  const int us = td.side;
  if (n.root || val_lost(n.best) || (AN && n.crystal))
    return false;
  const U64 own_pieces = td.occupancies[us] & ~td.bitboards[us == white ? P : p] & ~td.bitboards[us == white ? K : k];
  if (!own_pieces)
    return false;
#ifdef TRIUMV_PRUNELOG
  // Data build: the move-count rule skips the later quiet moves one by one (and does not count them), so that each
  // can be sampled; the queue keeps handing them out.
  if (q.pl_count_off && !tact) {
    f->move_count = --n.moves;
    PL_RULE(16);
    return true;
  }
#endif
  // PrExpRootDepth (0 = always) lets the per-expert levers act only once the iteration is that deep (pruning less paid
  // only with depth to spare: block 3 data, 20/10/2026).
  const bool pr_exp = td.root_depth >= PrExpRootDepth;
  const int red_depth = n.depth - 1 - r / 1024;
  // Order of ours (28/10/2026): the move is judged first by its own gate; the move count, which decides only whether
  // the queue goes on handing out quiet moves after this one, comes last. The gates read neither the count nor the
  // queue's quiet switch, and the count reads nothing the gates write (the quiet gate may raise the node's best value;
  // the guard on a lost best value above was read before it).
  // A quiet move goes through the quiet gate (not on the previous line in a PV node), a capture or a check through the
  // capture gate.
  const bool skip = (!tact && !gives) ? !(f->follow_pv && n.pv) && quiet_gate<AN>(td, f, n, q, m, red_depth, ctx)
                                      : capture_gate<AN>(td, f, n, q, m, gives, red_depth, own_pieces, pr_exp);
  count_gate<AN>(td, f, n, q, pr_exp);
  return skip;
}

#ifdef TRIUMV_PRUNELOG
static inline int pl_clip(int v) { return std::clamp(v, -32000, 32000); }
// Consilium Plebis experts of the two perspectives at this node, read even when the C2 margin tables are off.
static inline void pl_c2_experts(ThreadData &td, int8_t &us, int8_t &them) {
  us = (int8_t)c2_expert(td, td.side);
  them = (int8_t)c2_expert(td, td.side ^ 1);
}

// Block 3 data (prunelog.h): a move that a pruning rule skipped, searched anyway with a null window at alpha, at the
// depth its reduction would have given it and at the full depth.
template <bool AN>
static void prunelog_move(ThreadData &td, NodeFrame *f, NodeState &n, int m, bool tact, bool gives, int r,
                          bool gen_legal) {
  if (val_decided(n.alpha) || n.depth > PRUNELOG_MAX_DEPTH + 1)
    return;
  PruneMoveRec rec{};
  const int pc = move_piece(m), to = move_to(m);
  rec.rule = (int8_t)pl_rule;
  rec.depth = (int16_t)n.depth;
  rec.expert = (int16_t)n.expert;
  rec.ply = (int16_t)f->ply;
  rec.move_no = (int16_t)(n.moves + (pl_rule == 16));
  rec.flags = (uint16_t)(n.improving | (n.pv << 1) | (n.cut << 2) | (tact << 3) | (gives << 4) | (f->tt_pv << 5));
  rec.static_eval = pl_clip(f->static_eval - n.alpha);
  rec.alpha = n.alpha;
  rec.signal = n.signal;
  rec.disagree = n.disagree;
  rec.r = r;
  rec.victim = (int16_t)(tact ? victim_kind(td, m) : -1);
  rec.stat = tact ? td.capt_hist[pc][to][victim_kind(td, m)]
                  : (*f[-1].cont_hist)[pc][to] + (*f[-2].cont_hist)[pc][to] + pawn_row(td)[pc][to];
  rec.pieces = (int16_t)count_bits(td.occupancies[both]);
  // the quiet futility value is computed just before rules 14 and 15 for this move; for the others it is stale
  rec.fut = pl_rule == 14 || pl_rule == 15 ? pl_fut : PL_NONE;
  pl_c2_experts(td, rec.c2_us, rec.c2_them);
  const int d_full = n.depth - 1, d_red = std::clamp(n.depth - 1 - r / 1024, 0, d_full);
  rec.d_red = (int8_t)d_red;
  rec.d_full = (int8_t)d_full;
  UndoInfo u;
  pl_in_probe++;
  if (!step_in(td, f, m, u, gen_legal)) {
    pl_in_probe--;
    return;
  }
  f[1].in_check = gives;
  const U64 n0 = td.nodes;
  const int v1 = -explore<AN>(td, f + 1, -(n.alpha + 1), -n.alpha, d_red, true, false);
  const U64 n1 = td.nodes;
  const int v2 = d_full == d_red ? v1 : -explore<AN>(td, f + 1, -(n.alpha + 1), -n.alpha, d_full, true, false);
  const U64 n2 = td.nodes;
  step_out(td, m, u);
  pl_in_probe--;
  if (stop_threads.load(std::memory_order_relaxed))
    return;
  rec.v_red = pl_clip(v1 - n.alpha);
  rec.v_full = pl_clip(v2 - n.alpha);
  rec.n_red = (uint32_t)std::min<U64>(n1 - n0, 0xffffffffu);
  rec.n_full = (uint32_t)std::min<U64>(n2 - n1, 0xffffffffu);
  prunelog_write(td.thread_id, "prunemove", 1, rec);
}

// Correction tables of the node before their weights (02/11/2026, measurement of the Corr*W weights): the same reads
// as corr_signal (09_history.inc), term by term.
static void pl_corr_parts(const ThreadData &td, const NodeFrame *f, const NodeState &n, PruneNodeRec &rec) {
  const int us = td.side;
  const U64 phk = corr_phase_key(td);
  const U64 fk = corr_fifty_key(td);
  rec.corr_pawn = corr_slot(td.pawn_key ^ phk ^ fk, us).pawn;
  rec.corr_minor = corr_slot(td.mm_key[0] ^ phk, us).minor;
  rec.corr_npw = corr_slot(td.np_key[white] ^ fk, us).np_white;
  rec.corr_npb = corr_slot(td.np_key[black] ^ fk, us).np_black;
  const int prev = f[-1].move;
  if (move_ok(prev)) {
    const int to = move_to(prev), pc = td.piece_on[to];
    rec.corr_c24 = (*f[-2].cont_corr)[pc][to] + (*f[-4].cont_corr)[pc][to];
    rec.corr_c6 = (*f[-6].cont_corr)[pc][to];
    rec.corr_c3 = (*f[-3].cont_corr)[pc][to];
  } else
    rec.corr_flags |= 1;
  if (us == white)
    rec.corr_flags |= 2;
  rec.corr_band = (int8_t)search_band(td);
  rec.signal_x = n.signal_x;
}

// Block 3 data (prunelog.h): a node before the node pruning, searched first with a wide window at its own depth.
template <bool AN>
static int prunelog_node(ThreadData &td, NodeFrame *f, NodeState &n) {
  PruneNodeRec rec{};
  pl_corr_parts(td, f, n, rec);
  rec.depth = (int16_t)n.depth;
  rec.expert = (int16_t)n.expert;
  rec.ply = (int16_t)f->ply;
  rec.flags = (uint16_t)(n.improving | (n.pv << 1) | (n.cut << 2) | (f->tt_pv << 3) | ((n.h.move != 0) << 4) |
                         (n.tact_hash << 5) | (n.opp_worse << 6) | ((n.disagree < 0) << 9));
  rec.eval = n.eval;
  rec.static_eval = f->static_eval;
  rec.eval_raw = n.eval_raw;
  rec.signal = n.signal;
  rec.disagree = n.disagree;
  rec.alpha = pl_clip(n.alpha - n.eval);
  rec.beta = pl_clip(n.beta - n.eval);
  rec.pieces = (int16_t)count_bits(td.occupancies[both]);
  const int lo = n.eval - PRUNELOG_WINDOW, hi = n.eval + PRUNELOG_WINDOW;
  const U64 n0 = td.nodes;
  pl_in_probe++;
  const int pv_save = f->tt_pv, follow_save = f->follow_pv;
  const int v = explore<AN>(td, f, lo, hi, n.depth, false, true);
  f->tt_pv = pv_save;
  f->follow_pv = follow_save;
  pl_in_probe--;
  rec.probe = pl_clip(v - n.eval);
  rec.probe_nodes = (uint32_t)std::min<U64>(td.nodes - n0, 0xffffffffu);
  if (v <= lo)
    rec.flags |= 128;
  if (v >= hi)
    rec.flags |= 256;
  pl_rule = 0;
  pl_razor = pl_rfp = pl_nmp_floor = PL_NONE;
  const int ret = prune_before_moves<AN>(td, f, n);
  rec.rule = (int8_t)(ret == NO_VALUE ? 0 : pl_rule);
  rec.ret = ret == NO_VALUE ? 0 : pl_clip(ret - n.eval);
  rec.razor = pl_razor;
  rec.rfp = pl_rfp;
  rec.nmp_floor = pl_nmp_floor;
  pl_c2_experts(td, rec.c2_us, rec.c2_them);
  if (!stop_threads.load(std::memory_order_relaxed))
    prunelog_write(td.thread_id, "prunenode", 0, rec);
  return ret;
}
#endif

#ifdef TRIUMV_ORDLOG
// Move-ordering data (ordlog.h): attacks of a piece kind of `side` from sq.
static inline U64 ol_piece_att(int pt, int side, int sq, U64 occ) {
  switch (pt) {
  case P: return pawn_attacks[side][sq];
  case N: return knight_attacks[sq];
  case B: return get_bishop_attacks(sq, occ);
  case R: return get_rook_attacks(sq, occ);
  case Q: return get_queen_attacks(sq, occ);
  default: return king_attacks[sq];
  }
}
// Kind (P..K) of the cheapest piece of `by` attacking sq with occupancy occ (pieces in `gone` left out), -1 if none.
static inline int ol_min_attacker(const ThreadData &td, int sq, int by, U64 occ, U64 gone = 0) {
  const U64 a = attackers_with(td, sq, by, occ) & occ & ~gone;
  if (!a)
    return -1;
  const int off = by == white ? 0 : 6;
  for (int k = P; k <= K; k++)
    if (a & td.bitboards[k + off])
      return k;
  return -1;
}
static inline int ol_rel_rank(int side, int sq) { return side == white ? 8 - sq / 8 : sq / 8 + 1; }
static inline int ol_clip(int v, int lim) { return std::clamp(v, -lim, lim); }
static thread_local uint32_t ol_node_seq = 0;

// The oracle of a node that failed high: the queue order replayed, then every legal move searched with a null window
// at beta and depth - 1 without reductions; one node record and one record per legal move (ordlog.h).
template <bool AN>
static void ordlog_oracle(ThreadData &td, NodeFrame *f, NodeState &n, int depth0, int cut_move, int cut_step,
                          const int *fmv, const signed char *ffate, const signed char *fred, int fn) {
  if (!td_lists_legal())   // Chess960: the legal generator is not used
    return;
  const int us = td.side, them = us ^ 1, us_off = us == white ? 0 : 6, eoff = 6 - us_off;
  const U64 occ = td.occupancies[both];
  // 1. The queue order of the node, replayed from its hash move (same statistics as the real search had at its end).
  static thread_local int omv[256], ostep[256], osc[256];
  int no = 0;
  {
    MoveQueue q;
    queue_start(q, td, f, n.h.move, depth0, n.in_check ? QK_EVADE : QK_MAIN);
    int m;
    while (no < 256 && (m = queue_next(q)) != 0) {
      omv[no] = m;
      ostep[no] = m == q.hash_move ? -1 : q.step;
      osc[no] = 0;
      no++;
    }
    for (int i = 0; i < no; i++)
      if (ostep[i] != -1)
        for (int j = 0; j < q.n_all; j++)
          if (q.list[j].move == omv[i]) { osc[i] = q.list[j].score; break; }
  }
  int legal[256];
  const int nl = legalgen::generate_legal_tv<legalgen::ALL>(td, legal);
  if (nl <= 1)
    return;

  // 2. Node record.
  OrdNodeRec nr{};
  nr.node_id = ++ol_node_seq;
  nr.depth = (int16_t)depth0;
  nr.ply = (int16_t)f->ply;
  nr.expert = (int16_t)n.expert;
  nr.pieces = (int16_t)count_bits(occ);
  nr.n_legal = (int16_t)nl;
  nr.cut_index = (int16_t)(cut_move ? n.moves : 0);   // 0: the node failed low (no cutoff in the real search)
  nr.cut_step = (int8_t)cut_step;
  for (int j = 0; j < no; j++)
    if (omv[j] == cut_move) { nr.cut_order = (int16_t)(j + 1); break; }
  nr.flags = (uint16_t)(n.pv | (n.cut << 1) | (n.all << 2) | (n.improving << 3) | (f->tt_pv << 4) |
                        (n.in_check << 5) | ((n.h.move != 0) << 6) | (n.tact_hash << 7) | (n.opp_worse << 8) |
                        ((cut_move && cut_move == n.h.move) << 9) | ((cut_move == 0) << 10));
  nr.beta = n.beta;
  nr.eval_beta = ol_clip(n.eval - n.beta, 32000);
  nr.static_beta = ol_clip(f->static_eval - n.beta, 32000);
  nr.c2ex = (int16_t)c2_experts_td(td);
  C2Mat cm;
  c2_mat_init(td, cm);
  const int lm = f[-1].move, pm = f[-2].move;
  const int lmto = lm > 0 ? move_to(lm) : -1;
  const int lmpc = lmto >= 0 ? td.piece_on[lmto] : -1;
  nr.last_kind = (int8_t)(lmpc >= 0 && lmpc < 12 ? kPieceKind[lmpc] : -1);
  nr.last_victim = (int8_t)(lm > 0 && f[-1].captured >= 0 && f[-1].captured < 12 ? kPieceKind[f[-1].captured] : -1);
  int hang_n = 0, hang_max = -1;
  const int *pv = piece_values_mat(td);   // per-expert values of the node (see.cpp)
  for (U64 t = td.occupancies[us] & ~td.bitboards[K + us_off]; t; pop_lsb_bb(t)) {
    const int sq = get_ls1b_index(t), k = kPieceKind[td.piece_on[sq]];
    const int a = ol_min_attacker(td, sq, them, occ);
    if (a >= 0 && (pv[a + eoff] < pv[td.piece_on[sq]] || !(attackers_with(td, sq, us, occ) & occ & ~(1ULL << sq))))
      hang_n++, hang_max = std::max(hang_max, k);
  }
  nr.hanging_n = (int8_t)hang_n;
  nr.hanging_max = (int8_t)hang_max;

  // 3. Features of every legal move, then its oracle search.
  static thread_local OrdMoveRec recs[256];
  MoveSafety s;
  s.ready = false;
  const int eksq = get_ls1b_index(td.bitboards[K + eoff]);
  const NodeFrame save = *f;
  const U64 n_start = td.nodes;
  bool stopped = false;
  ol_in_probe++;
  for (int i = 0; i < nl; i++) {
    const int m = legal[i];
    OrdMoveRec &r = recs[i];
    r = OrdMoveRec{};
    r.node_id = nr.node_id;
    r.move = m;
    r.step = -2;
    for (int j = 0; j < no; j++)
      if (omv[j] == m) { r.order = (int16_t)(j + 1); r.step = (int8_t)ostep[j]; r.score = osc[j]; break; }
    const int pc = move_piece(m), from = move_from(m), to = move_to(m), pt = kPieceKind[pc];
    const int prom = get_move_promoted(m) ? promo_kind(m) : -1, apt = prom >= 0 ? prom : pt;
    const bool ep = get_move_enpassant(m) != 0, cap = get_move_capture(m) != 0;
    const int vk = ep ? P : cap ? victim_kind(td, m) : -1;
    r.pt = (int8_t)pt;
    r.victim = (int8_t)vk;
    r.promo = (int8_t)prom;
    r.castle = get_move_castling(m) != 0;
    r.ep = ep;
    const bool gives = checks_before(td, s, m);
    r.gives = gives;
    r.see0 = see_holds(td, m, 0);
    r.see_win = see_holds(td, m, 1);
    const U64 fromb = 1ULL << from, tob = 1ULL << to;
    const int fa = ol_min_attacker(td, from, them, occ);
    r.from_att = fa >= 0;
    r.from_lesser = fa >= 0 && pv[fa + eoff] < pv[pc];
    r.from_hang = fa >= 0 && pt != K &&
                  (pv[fa + eoff] < pv[pc] || !(attackers_with(td, from, us, occ) & occ & ~fromb));
    U64 occ2 = (occ ^ fromb) | tob, gone = cap ? tob : 0;
    if (ep) {
      const int cs = us == white ? to + 8 : to - 8;
      occ2 ^= 1ULL << cs;
      gone = 1ULL << cs;
    }
    const int ta = ol_min_attacker(td, to, them, occ2, gone);
    r.to_att_min = (int8_t)(ta + 1);
    r.to_def = (attackers_with(td, to, us, occ2) & occ2 & ~fromb & ~tob) != 0;
    const U64 att = ol_piece_att(apt, us, to, occ2);
    const U64 enemy = td.occupancies[them] & ~gone & ~td.bitboards[K + eoff];
    int nt = 0, mx = -1, ng = 0;
    for (U64 t = att & enemy; t; pop_lsb_bb(t)) {
      const int sq = get_ls1b_index(t), k = kPieceKind[td.piece_on[sq]];
      nt++;
      mx = std::max(mx, k);
      const bool undef = !(attackers_with(td, sq, them, occ2) & occ2 & ~gone);
      ng += pv[td.piece_on[sq]] > pv[apt + us_off] || undef;
    }
    r.n_targets = (int8_t)nt;
    r.max_target = (int8_t)mx;
    r.n_good_targets = (int8_t)ng;
    r.fork = ng >= 2 || (gives && ng >= 1);
    r.kdist = (int8_t)std::max(std::abs(to / 8 - eksq / 8), std::abs(to % 8 - eksq % 8));
    r.katt = (int8_t)count_bits(att & king_attacks[eksq]);
    if (lmto >= 0) {
      r.recapture = to == lmto;
      r.hits_last = to != lmto && ((att >> lmto) & 1);
      r.escapes_last = lmpc >= 0 && lmpc < 12 && ((ol_piece_att(kPieceKind[lmpc], them, lmto, occ) >> from) & 1);
    }
    r.same_piece = pm > 0 && move_to(pm) == from;
    if (pt == P && prom < 0) {
      const int file = to % 8, row = to / 8;
      U64 ahead = 0;
      for (int fl = std::max(0, file - 1); fl <= std::min(7, file + 1); fl++)
        for (int rw = us == white ? 0 : row + 1; rw < (us == white ? row : 8); rw++)
          ahead |= 1ULL << (rw * 8 + fl);
      r.passed = !(ahead & td.bitboards[P + eoff]);
    }
    r.rank_to = (int8_t)ol_rel_rank(us, to);
    r.advance = (int8_t)(ol_rel_rank(us, to) - ol_rel_rank(us, from));
    r.killer = f->ply < 256 && ol_killer[f->ply] == m;
    r.counter = lmpc >= 0 && lmpc < 12 && ol_counter[lmpc][lmto] == m;
    r.c2_switch = cap ? (int8_t)c2_switches(td, cm, m) : 0;
    for (int j = 0; j < fn; j++)
      if (fmv[j] == m) { r.fate = ffate[j]; r.red = fred[j]; break; }
    if (cap || ep) {
      r.h_capt = td.capt_hist[pc][to][vk];
    } else {
      const int key = quiet_key(m), ctx = ThreatHist && prom < 0 ? quiet_ctx_slow(td, m) : 0;
      r.h_main = qh_order(td, us, key, ctx);
      r.h_pawn = pawn_row(td)[pc][to];
      r.h_c1 = (*f[-1].cont_hist)[pc][to];
      r.h_c2 = (*f[-2].cont_hist)[pc][to];
      r.h_far = (*f[-3].cont_hist)[pc][to] + (*f[-4].cont_hist)[pc][to] + (*f[-6].cont_hist)[pc][to];
      r.h_low = f->ply < LOW_PLY_SIZE ? td.low_ply_hist[f->ply][key] : 0;
    }
    // the oracle: null window at beta, depth - 1, no reduction
    UndoInfo u;
    if (!step_in(td, f, m, u, true)) {
      r.refutes = -1;
      continue;
    }
    f[1].in_check = gives;
    if (CausalRed)
      f[1].refute = 0;
    const U64 n0 = td.nodes;
    const int v = -explore<AN>(td, f + 1, -n.beta, -n.beta + 1, depth0 - 1, false, false);
    step_out(td, m, u);
    if (stop_threads.load(std::memory_order_relaxed)) {
      stopped = true;
      break;
    }
    r.refutes = v >= n.beta;
    r.margin = (int16_t)ol_clip(v - n.beta, 3000);
    r.nodes = (uint32_t)std::min<U64>(td.nodes - n0, 0xffffffffu);
  }
  ol_in_probe--;
  *f = save;
  if (stopped)
    return;
  nr.probe_nodes = (uint32_t)std::min<U64>(td.nodes - n_start, 0xffffffffu);
  ordlog_write(td.thread_id, "ordnode", 0, &nr, 1);
  ordlog_write(td.thread_id, "ordmove", 1, recs, (size_t)nl);
}
#endif

// Push of a passed pawn to the 6th or 7th rank in an endgame (ours). The network sees passed pawns with its
// PassedPawns block; the search would treat them as any quiet move and could reduce them by several plies.
static bool passed_push(const ThreadData &td, int m) {
  if (kPieceKind[move_piece(m)] != P || get_move_capture(m) || get_move_promoted(m))
    return false;
  const int to = move_to(m), us = td.side, row = to / 8, file = to % 8;   // row 0 = ottava traversa
  if (us == white ? (row != 1 && row != 2) : (row != 5 && row != 6))
    return false;
  const int *pv = piece_values_mat(td);
  int npm = 0;
  for (int t = N; t <= Q; t++)
    npm += pv[t] * count_bits(td.bitboards[t]) + pv[t + 6] * count_bits(td.bitboards[t + 6]);
  if (npm > PassedPushNpm)
    return false;
  U64 files = 0;
  for (int fl = std::max(0, file - 1); fl <= std::min(7, file + 1); fl++)
    files |= 0x0101010101010101ULL << fl;
  const int row_start = to - file;
  const U64 ahead = us == white ? (1ULL << row_start) - 1 : ~((1ULL << (row_start + 8)) - 1);
  return !(files & ahead & td.bitboards[us == white ? p : P]);
}

// Is the move a back-and-forth shuffle without progress (same piece between the same squares)?
static inline bool shuffles(const ThreadData &td, const NodeFrame *f, int m) {
  if (is_tactical(m) || td.fifty < ShuffleFifty || td.plies_from_null < ShuffleNull || f->ply < ShufflePly)
    return false;
  const int m2 = f[-2].move, m4 = f[-4].move;
  return move_ok(m2) && move_ok(m4) && move_from(m) == move_to(m2) && move_from(m2) == move_to(m4);
}

// Singular extension of the hash move. Returns the extension; a value >= beta closes the node (in *cutoff).
// SING_SKIPPED: the exclusion search was not done (not the hash move or conditions missing); move_loop reads it
// as extension 0, and only in that case tries the LDSE (Ldse*).
static constexpr int SING_SKIPPED = -99;
template <bool AN>
static int singular_probe(ThreadData &td, NodeFrame *f, NodeState &n, int m, int new_depth, int *cutoff) {
  // Block 4 of 8.1 in our form (19/10/2026). The hash move is tested for being the only good move: the other moves are
  // searched at about half depth against a bar below the hash value (the exclusion search). Three outcomes:
  //  - every other move stays under the bar: the hash move is extended, by more plies the further they stay under;
  //  - the exclusion search itself clears beta: the node has several good moves and returns at once;
  //  - otherwise the hash move may be searched shallower (negative extension) when the node expects a cutoff.
  // The bar and the thresholds of the extra plies are built from one vector of node facts, with a weight per level
  // and an offset per expert of the network. The starting values reproduce the previous tree; the weights marked
  // "ours" start at 0 and wait for the tuning.
  const HashView &h = n.h;
  if (n.root || m != h.move || n.excluded || n.depth < SingMinDepth + SingMinDepthTtPv * f->tt_pv ||
      !val_known(h.value) || val_decided(h.value) || !bound_covers(h.bound, true) || h.bound == BOUND_UP ||
      h.depth < n.depth - SingHashSlack || shuffles(td, f, m) || n.hunt_mate)
    return SING_SKIPPED;

  // The bar: hash value minus a margin per ply of depth. Ours, at 0: the expert, how much deeper than needed the hash
  // entry is, and how far the hash value sits from the static evaluation (a node in motion trusts its hash less).
  const int surplus = h.depth - (n.depth - SingHashSlack);
  const int moving = std::min(std::abs(h.value - f->static_eval), 1024) / 64;
  const int margin = SingMarginBase + (AN ? td.dz_sing : deep_term(td, DeepSing)) +
                     SingMarginTtPv * (f->tt_pv && !n.pv) + by_exp4(n.expert, SingExp0, SingExp1, SingExp2, SingExp3) +
                     SingSurplus * surplus + SingMoving * moving;
  // Our form (27/10/2026, docs/audit_8.1/R9): the bar also sits SingBarBase lower at every depth (a constant beside the
  // margin per ply), and the exclusion search is SingDepthOfs sixteenths of a ply deeper than new_depth*SingDepthMul/2048
  // (the offset enters the numerator, so the truncation is shared). Both at 0: the old bar and depth.
  const int s_beta = h.value - margin * n.depth / SingMarginDiv - SingBarBase;
  const int s_depth = (new_depth * SingDepthMul + 128 * SingDepthOfs) / 2048;
  f->excluded = m;
  const int v = explore<AN>(td, f, s_beta - 1, s_beta, s_depth, n.cut, false);
  f->excluded = 0;

  if (v < s_beta) {
    // Extra plies: a ladder on the gap under the bar. The first rung is a threshold built from the node facts, plus
    // an offset per expert; every further rung sits one step higher, and the step has its own weights of the same
    // facts. A move earns one extra ply per rung it clears, up to SingRungs. The step may shrink or grow with depth
    // (SingStepDepth, ours, at 0); with two rungs the ladder gives the tree of the previous separate thresholds.
    const int corr_adj = std::abs(n.signal) / SingCorrDiv;
    const int beyond = f->ply > td.root_depth, quiet = !n.tact_hash;
    const int deep_adj = AN ? td.dz_ext : deep_term(td, DeepExt);
    const int common = corr_adj + deep_adj + by_exp4(n.expert, SingLvlExp0, SingLvlExp1, SingLvlExp2, SingLvlExp3);
    const int hist = SingTtHistMul * td.tt_move_hist / SingTtHistDiv;
    const int first = SingDoubleBase + SingDoublePv * n.pv - SingDoubleQuiet * quiet - hist - SingDoubleDeep * beyond -
                      common;
    const int step = SingStepBase + SingStepPv * n.pv - SingStepQuiet * quiet + SingStepTtPv * f->tt_pv + hist -
                     SingStepDeep * beyond + SingStepDepth * n.depth / 16;
    const int gap = s_beta - v;
    int extra = 0;
    for (int k = 0; k < SingRungs; k++)
      extra += gap > first + k * step;
    n.depth++;
    return 1 + extra;
  }
  if (v >= n.beta && !val_decided(v)) {   // several good moves: the node returns, and the hash move learns it
    on_multi_cut(td, f, n, v, s_depth);
    *cutoff = v;
    return 0;
  }
  // Negative extension, split by its two reasons: the hash value already clears beta, or the node expects a cutoff.
  if (h.value >= n.beta)
    return -SingNegHigh;
  if (n.cut)
    return -SingNegCut;
  return 0;
}

// Updates the root move just searched (statistics, line, best-move changes).
// Blend of a running average with a fresh value, the fresh one weighing w/32 (truncated toward zero).
static inline long long blend32(long long old, long long fresh, int w) { return (fresh * w + old * (32 - w)) / 32; }

// Bookkeeping of one searched root move (rewritten in our structure 27/10/2026, same results): the effort spent on it,
// the two running averages read by the aspiration window (score, and signed square for its width), and, for the first
// move or one that raised alpha, its score, bound and line. A search weighs in the averages by its share of the effort
// spent on the move so far, in 32nds between RootAvgMin and RootAvgMax (RootAvgChi scales the past effort; the square
// is capped at RootMsqMax; CENSUS 21/10/2026, were 3, 12, 24, 16).
static void root_record(ThreadData &td, NodeState &n, int m, int value, U64 spent, int move_no) {
  RootLine *const end = td.root + td.root_count;
  RootLine *const rl = std::find_if(td.root, end, [m](const RootLine &r) { return r.pv[0] == m; });
  if (rl == end)
    return;
  const U64 before = std::max<U64>(1, rl->effort);
  rl->effort += spent;
  const U64 share = 64 * spent / (2 * spent + (U64)RootAvgChi * before);
  const int w = (int)std::clamp<U64>(share, (U64)RootAvgMin, (U64)RootAvgMax);
  const int w_sq = std::min(w, (int)RootMsqMax);
  const long long signed_sq = (long long)value * std::abs(value);
  rl->avg_score = rl->avg_score == -infinity ? value : (int)blend32(rl->avg_score, value, w);
  rl->mean_sq = rl->mean_sq == -(long long)infinity * infinity ? signed_sq : blend32(rl->mean_sq, signed_sq, w_sq);

  // A later move that did not raise alpha keeps no score and no line.
  if (move_no != 1 && value <= n.alpha) {
    rl->score = -infinity;
    return;
  }
  rl->score = value;
  rl->sel_depth = td.sel_depth;
  rl->lower = value >= n.beta;
  rl->upper = !rl->lower && value <= n.alpha;
  rl->uci_score = rl->lower ? n.beta : rl->upper ? n.alpha : value;
  rl->pv_len = 1;
  for (int i = 1; i < td.pv_length[1] && rl->pv_len < max_ply; i++)
    rl->pv[rl->pv_len++] = td.pv_table[1][i];
  if (move_no != 1 && td.pv_idx == 0)   // a new best move in the main line
    td.best_move_changes++;
}

// Conversion push (the author's idea: push toward the endgame when the pawn-king subnetwork or an endgame expert
// thinks it is winning; 21/10/2026, OFF by default, SPRT pending): +1 when the side to move is ahead, -1 behind, 0
// in between, on the blend (C2ConvEvalW * static evaluation + pawn-king term) / 1024 against C2ConvMargin, with the
// rule shared with the evaluation version C2ConvW (c2_conv_lean, consilium_bridge.h).
//  - pawn-king term: C2ConvPkSrc 0 = linear skip x C2ConvPkW (the first form); 1 = exact effect x C2ConvEffW, the
//    node's own value (node_evaluate read it after a fresh evaluation; else it is computed here, never a value left
//    by another position);
//  - C2ConvExpMax < 7: the static evaluation counts only when both perspectives use an expert <= C2ConvExpMax, the
//    endgame experts: the push follows an expert that was trained on such endgames.
static int c2_conv_side(ThreadData &td, const NodeFrame *f, NodeState &n) {
  long long s = 0;
  if (C2ConvEvalW && (C2ConvExpMax >= 7 || std::max(c2_expert(td, white), c2_expert(td, black)) <= C2ConvExpMax))
    s += (long long)C2ConvEvalW * f->static_eval;
  if (C2ConvPkSrc == 0) {
    if (C2ConvPkW)
      s += (long long)C2ConvPkW * c2_pk_skip(td);
  } else if (C2ConvEffW) {
    if (n.c2_pk == INT_MIN)
      n.c2_pk = c2_pk_effect_now(td);
    s += (long long)C2ConvEffW * n.c2_pk;
  }
  const int lean = c2_conv_lean((int)std::clamp(s / 1024, -(long long)infinity, (long long)infinity), C2ConvMargin,
                                infinity);
  return (lean > 0) - (lean < 0);
}

// C2ConvRoot (the author's idea, 21/10/2026 evening): the same rule judged once, at the root, from the root side's
// view, with the exact pawn-king effect of the root and `value` (the static evaluation, or the score of the last
// completed iteration). deepen() stores the result in td.c2_conv_root; move_loop reads it with the sign of the side
// to move. A decided value (mate) or check at the root gives no decision.
static int c2_conv_root_decide(ThreadData &td, int value) {
  if (val_decided(value))
    return 0;
  long long s = 0;
  if (C2ConvEvalW && (C2ConvExpMax >= 7 || std::max(c2_expert(td, white), c2_expert(td, black)) <= C2ConvExpMax))
    s += (long long)C2ConvEvalW * value;
  if (C2ConvEffW)
    s += (long long)C2ConvEffW * c2_pk_effect_now(td);
  const int lean = c2_conv_lean((int)std::clamp(s / 1024, -(long long)infinity, (long long)infinity), C2ConvMargin,
                                infinity);
  return (lean > 0) - (lean < 0);
}

// A fail high that is not a decided value comes back as the average of the score, weighted by the depth, and beta:
// (best * d + beta) / (d + 1). W1 (08/10/2026): the division by d + 1 is a multiplication by the 40-bit reciprocal;
// |x| < 2^15 * (max_ply + 8) < 2^24, exact (numpy: 0 differences for |x| <= 32768 * (d + 1), d < 260). Called at the
// cutoff in move_loop (28/10/2026); the result stays >= beta.
static inline void soften_fail_high(NodeState &n) {
  if (n.best < n.beta || val_decided(n.best) || val_decided(n.alpha))
    return;
  const long long x = (long long)n.best * n.depth + n.beta;
  const unsigned long long mag = (unsigned long long)(x < 0 ? -x : x);
  const int q = (int)((mag * succ_recip[n.depth]) >> 40);
  n.best = x < 0 ? -q : q;
}

template <bool AN>
static int move_loop(ThreadData &td, NodeFrame *f, NodeState &n) {
  const int us = td.side;
  MoveQueue q;
  queue_start(q, td, f, n.h.move, n.depth, n.in_check ? QK_EVADE : QK_MAIN);
  MoveSafety s;
  s.ready = false;
  q.checks = &s.checks;
  int value = n.best, m;
  // C2 idea 3, conversion push (the author's idea; 21/10/2026, agent A; OFF by default, SPRT pending): is the side to
  // move ahead (+1) or behind (-1) by C2ConvMargin on a blend of the corrected static evaluation and the pawn-king term
  // (what pawns and kings are worth to the network, the part that remains after trades; c2_conv_side above)? Read by
  // the ordering of the captures (fill_tactics) and by the reduction below.
  const int c2_conv = (C2ConvOrder | C2ConvRed) && !n.in_check && !val_decided(f->static_eval)
                    ? (C2ConvRoot ? (us == td.root_side ? td.c2_conv_root : -td.c2_conv_root) : c2_conv_side(td, f, n))
                    : 0;
  q.c2_conv = c2_conv;
  const bool c2_conv_red = c2_conv && C2ConvRed, c2_pk_red = (C2PkRed | C2PkVol) != 0;   // once per node
  int c2_pk_here = INT_MIN;   // C2PkRed/C2PkVol: pawn-king skip of the node, read at its first pawn or king move
  int red_window = -1, red_window_term = 0, red_window_ours = 0;   // see red_entry_v80; ours: per 1024 of root_delta
  // RdKingDanger (ours, off): squares next to our king that the opponent attacks, once per node.
  const int king_danger = RdKingDanger ? std::min(king_ring_attacked(td, us), 4) : 0;
  // SingleReply (ours, off): in check with a single legal reply, that reply is searched SingleReply plies deeper and
  // without the exclusion search, which has nothing to compare it with. The legal generator gives the exact count.
  bool only_reply = false;
  if (SingleReply && n.in_check && !n.root && td_lists_legal()) {
    int buf[256];
    only_reply = legalgen::generate_legal_tv<legalgen::ALL>(td, buf) == 1;
  }
  int first_quiet_no = 0;                     // LmrRank: number of the node's first ordered quiet move
  // Idea 2: fixed reduction term for the node (0 if off).
  const int node_red = AN ? td.dz_lmr : deep_term(td, DeepLmr);
  const int gd_style = AN ? td.gd_style : 0;   // AnGold: style of the root (0 in games)
#ifdef TRIUMV_ORDLOG
  const int ol_depth0 = n.depth;   // the node's depth before AlphaRed (the oracle searches at depth - 1)
  bool ol_cut = false;             // the loop ended with a cutoff
  int ol_mv[256], ol_n = 0;        // the fate of every move handed out (OrdMoveRec::fate, red)
  signed char ol_fate[256], ol_red[256];
  int ol_fate_cur = 3, ol_red_cur = 0;
#endif

  while ((m = queue_next(q)) != 0) {
    const bool gen_legal = queue_move_legal(q, m);
    if (m == n.excluded || (!gen_legal && !legal_before(td, s, m)))
      continue;
    int root_rank = 0;   // position of the move in the root list (order of the previous iteration)
    if (n.root) {
      bool listed = false;
      for (int i = td.pv_idx; i < td.pv_last; i++)
        if (td.root[i].pv[0] == m) { listed = true; root_rank = i - td.pv_idx; break; }
      if (!listed)
        continue;
    }
    f->move_count = ++n.moves;
    // "info currmove" at the root (like 7.0: main thread only, only after 3 s, corrected on 16/10/2026): the GUIs show the
    // move being examined. Lost with the rewrite of 04/10, reported by Maurizio Platino on 12/10/2026 (Fritz 18).
    // The time keeps it out of bench and short games; at the root the loop runs few times, zero cost.
    if (n.root && td.thread_id == 0 && get_time_ms() - g_limits.start_ms > 3000) {
      std::lock_guard<std::mutex> lock(output_mutex);
      printf("info depth %d currmove ", td.root_depth);
      print_move(m);
      printf(" currmovenumber %d\n", n.moves + td.pv_idx);
      fflush(stdout);
    }
    const bool tact = is_tactical(m);
    const int pc = move_piece(m), to = move_to(m);
    const bool gives = checks_before(td, s, m);
    int new_depth = n.depth - 1;
    if (n.beta - n.alpha != red_window) {
      red_window = n.beta - n.alpha;
      // (28/10/2026 night, speed, same value) Most nodes have a null window, whose term is the same in the whole
      // iteration: it is divided once and kept in the thread with the two values it came from (before, a 32-bit
      // division at almost every node).
      if (red_window == 1) {
        if (td.rw1_delta != td.root_delta || td.rw1_mul != LmrDeltaMul || td.rw1_delta == 0) {
          td.rw1_delta = td.root_delta, td.rw1_mul = LmrDeltaMul;
          td.rw1_term = LmrDeltaMul / td.root_delta;
        }
        red_window_term = td.rw1_term;
      } else
        red_window_term = red_window * LmrDeltaMul / td.root_delta;
    }
    // LmrRank (block 2 of 8.1, 16/10/2026, ours; docs/development/8.1/SEARCH_2_REDUCTIONS.md in the repo): for a
    // quiet move of the ordered list, the base reduction does not read only its order number. The "effective" number
    // is the number of the node's first quiet move plus one move every LmrRankScale points of gap from the score of the
    // best quiet move: the tenth move with a score close to the first is not the tenth of a tail of bad
    // moves, and in the unordered tail (below the limit of fill_quiets) the gap is worth more than the generation
    // order. The two numbers are mixed with a weight per expert (LmrRankMix0-3). Off: n.moves, as before.
    int red_move_no = n.moves;
    if (LmrRank && q.rank_ok) {
      if (!first_quiet_no)
        first_quiet_no = n.moves;
      const int eff = first_quiet_no + std::max(0, q.quiet_top - q.last_score) / LmrRankScale;
      const int mix = by_expert(n.expert, LmrRankMix0, LmrRankMix1, LmrRankMix2, LmrRankMix3);
      red_move_no = std::clamp((n.moves * (1024 - mix) + eff * mix) / 1024, 1, 255);
    }
    int r = red_entry_v80(td, f, n, red_move_no, red_window_term, gd_style);
    if (g_c2_node)   // C2 idea 1 (21/10/2026, agent A): reduction per Consilium Plebis expert (entry: the pruning reads it too)
      r += c2_tab[C2T_LMR][n.c2e];
#ifdef TRIUMV_REDLOG
    const int redlog_entry = r;   // the value skip_move reads
#endif

    const bool passer = PassedPushRed && !tact && passed_push(td, m);
    const int ctx = tact ? 0 : quiet_ctx(q, m);   // ThreatHist: 0 if off
    if (!passer && skip_move<AN>(td, f, n, q, m, tact, gives, r, ctx)) {
#ifdef TRIUMV_PRUNELOG
      if (!AN && prunelog_move_pick(td.thread_id, pl_rule))
        prunelog_move<AN>(td, f, n, m, tact, gives, r, gen_legal);
#endif
#ifdef TRIUMV_ORDLOG
      if (ol_n < 256)
        ol_mv[ol_n] = m, ol_fate[ol_n] = 1, ol_red[ol_n] = 0, ol_n++;
#endif
      continue;
    }

    int cut_value = NO_VALUE;
    int ext = only_reply ? SingleReply : singular_probe<AN>(td, f, n, m, new_depth, &cut_value);
    if (cut_value != NO_VALUE)
      return cut_value;
    const bool sing_skipped = ext == SING_SKIPPED;
    if (sing_skipped)
      ext = 0;
    // LDSE (ours, 05/10/2026; the idea is in Reckless and Stormphrax): at low depth, where the singular search is not
    // tried, the hash move with a lower or exact bound is extended by one ply if the static evaluation is below
    // alpha by LdseMargin. The hash says that move held a value, the eval says the position is bad:
    // the node rests on it, and near the horizon one more ply decides whether that value was true.
    // LdseMode (bits): 1 = cut nodes only (Reckless form), 2 = lower bound only, not exact (Stormphrax
    // form), 4 = not with entries from quiescence only.
    // LdseMax (05/10/2026): with Ldse on, every cut node along a line can extend again, and the depth
    // drops by one ply every two instead of one per ply. At fixed depth the tree stays the same in the typical
    // position (geometric mean x1.02-x1.06 over 80 positions, depth 8-14) but some rook endgames grow x5-x16. The
    // cap counts the Ldse extensions on the line (NodeFrame::ldse_path); 0 = no cap, as before.
    const bool ldse = LdseMargin && ext == 0 && sing_skipped && m == n.h.move && !n.root && !n.excluded &&
                      !n.in_check && n.depth <= LdseDepth && val_known(n.h.value) && bound_covers(n.h.bound, true) &&
                      f->static_eval <= n.alpha - LdseMargin && (n.cut || !(LdseMode & 1)) &&
                      (n.h.bound == BOUND_LOW || !(LdseMode & 2)) && (n.h.depth >= 0 || !(LdseMode & 4)) &&
                      (!LdseMax || f->ldse_path < LdseMax);
    if (ldse)
      ext = 1;
    f->ldse = ldse;

    // C2 ideas 3 and 5: what the reduction block needs from the board before the move.
    const bool c2_trade = c2_conv_red && c2_even_trade(td, m);
    const bool c2_pkm = c2_pk_red && (kPieceKind[pc] == P || kPieceKind[pc] == K || victim_kind(td, m) == P ||
                                      get_move_enpassant(m));
    if (c2_pkm && c2_pk_here == INT_MIN)
      c2_pk_here = c2_pk_skip(td);
    const bool brk = BreakRed && pawn_break(td, m);   // pawn break, read before the move is made
    const U64 nodes_before = td.nodes;
    UndoInfo u;
    if (!step_in(td, f, m, u, gen_legal)) {
      f->move_count = --n.moves;
      continue;
    }
    f[1].in_check = gives;
    f->hctx = (unsigned char)ctx;
    if (CausalRed)
      f[1].refute = 0;
    new_depth += ext;
    // C2 idea 11 (regime-settled horizon, 21/10/2026; OFF by default, SPRT pending): the move just made switched an
    // expert of either perspective and its child would be a quiescence node: one ply of real moves instead, once per
    // line, and with C2SettleHorizon 1 not in all nodes. The deep-reduction steps below do not take it back.
    bool c2_settle = false;
    if (C2SettleHorizon) {
      c2_settle = new_depth <= 0 && !f->settle_path && (!n.all || C2SettleHorizon == 2) && c2_last_switched(td);
      if (c2_settle)
        new_depth = 1;
      f->settle = c2_settle;
    }

    if (tact) {
      // Piece actually captured (for en passant it is the pawn, not the empty arrival square).
      const int took = kPieceKind[u.captured_piece & 15];
      // The captured piece at its per-expert value; the move is made, so the table is the child's, whose owner
      // regime already counts the capture (28/10/2026).
      f->stat_score = CaptStatMul * piece_values_mat(td)[u.captured_piece & 15] / 128 + td.capt_hist[pc][to][took];
    }
    else
      f->stat_score = (StatQuietW * qh_main(td, us, quiet_key(m), ctx) + StatCont1W * (*f[-1].cont_hist)[pc][to] +
                       StatCont2W * (*f[-2].cont_hist)[pc][to]) / 1024;
    r = red_final_v80(td, f, n, m, r, tact, gives, passer, node_red);
    // AnGold, tweaks of the reductions far from the root (units 1/1024 ply): in attack captures and checks
    // -60, in defence the quiet moves -40, in manoeuvring the quiet moves without check +80 from depth 9.
    if (gd_style && n.depth > 5 && f->ply > 1) {
      if (gd_style == 1 && (tact || gives))
        r -= 60;
      else if (gd_style == 3 && !tact)
        r -= 40;
      else if (gd_style == 2 && !tact && !gives && n.depth > 8)
        r += 80;
    }

#ifdef TRIUMV_REDLOG
    // Block 2 data (18/10/2026): the features of a sampled reduced move, the reduction it gets and, written after its
    // searches, what happened (redlog.h, version 2). Some moves that fail low at the reduced depth are searched again
    // at full depth only to label them (probe): that extra search changes the tree of the data build only.
    const bool redlog_on = !AN && n.depth >= 2 && n.moves > 1 && redlog_pick(td.thread_id);
    const int redlog_flags =
        (n.improving << 0) | (f->tt_pv << 1) | (n.pv << 2) | (n.cut << 3) | (n.all << 4) | (tact << 5) |
        (gives << 6) | ((m == n.h.move) << 7) | (n.tact_hash << 8) | ((n.h.value > n.alpha) << 9) |
        ((n.h.depth >= n.depth) << 10) | (passer << 11) |
        ((CausalRed && n.causal && !(((1ULL << move_from(m)) | (1ULL << to)) & n.causal)) << 12) |
        ((!n.h.move) << 13) | (1 << 14) | (n.opp_worse << 15);
    const int redlog_gap = val_decided(n.alpha) ? 0 : n.alpha - n.eval, redlog_r = r, redlog_cuts = f[1].cutoff_cnt;
    const int redlog_alpha = n.alpha;
    // version 3 fields: the own score of the removed own ordering form is always 0 (kept for the file format).
    const bool redlog_own_ok = false;
    const int redlog_own = 0, redlog_raw_moves = n.moves;
#endif
    // Order of ours (28/10/2026): the moves searched without the reduction rule come first in the text, the reduced
    // moves after; the two branches exclude each other, so this is the same code.
    if (n.depth < LmrMinDepth || n.moves <= 1) {
      // A move outside the reduction rule (shallow node or first move) gets a null-window search at full depth, less
      // one or two plies when its reduction is large, unless it is the first move of a PV node (searched below).
      if (!n.pv || n.moves > 1) {
        if (!n.h.move)
          r += e_lmr_no_tt_full(td);
#ifdef TRIUMV_REDLOG
        if (!AN && !n.pv && n.moves == 1 && n.depth >= 2 && new_depth >= 3 && redres_pick(td.thread_id))
          redres_probes<AN>(td, f, n, 2, new_depth, new_depth, new_depth - 2, 0, red_move_no, r, redlog_flags);
#endif
        const int deep_red = c2_settle ? 0 : (r > LmrDeepRed1) + (r > LmrDeepRed2 && new_depth > LmrDeepRedMin);
#ifdef TRIUMV_ORDLOG
        ol_red_cur = deep_red, ol_fate_cur = deep_red > 0 ? 2 : 3;
#endif
        value = -explore<AN>(td, f + 1, -(n.alpha + 1), -n.alpha, new_depth - deep_red, !n.cut, false);
      }
    } else {
      // NegExtMax (08/10/2026): with a negative reduction the move is searched up to 2 plies DEEPER, and along a line
      // these extensions add up like those of Ldse before its guard (LdseMax). Same remedy: a cap on the
      // number of extensions from negative reduction on the line (NodeFrame::negext_path).
      // BAKE NEGEXTMAX (11/10/2026, see 01_params.inc): fixed cap of one extension per line, option retired.
      // Reduction levers of our own (19/10/2026, all off): signals the formula does not read. Each one only lowers the
      // reduction, and the sum stops at zero (a negative reduction would become an extension).
      //  RdDisagree   the two parts of the network disagree on this node (|psqt - positional|): the evaluation is
      //               unsure, per 1024 units of disagreement, up to 8;
      //  RdBoundCapt  a capture that moves the position to another expert of the network (piece count 10, 16, 24 ->
      //               one less): the next evaluations come from another expert;
      //  RdKingDanger squares next to our own king under attack, up to 4;
      //  RdPawnBreak  in a closed position (at least 3 pawns blocked head to head) a pawn push that touches an enemy
      //               pawn: the few breaks of a frozen structure are often the plan of the whole game (user's idea).
      // (RdKingAtt, king-zone hits of the moved piece: no positive value on 19/10, removed on 28/10/2026.)
      if (RdDisagree | RdBoundCapt | RdKingDanger | RdPawnBreak) {
        int less = RdKingDanger * king_danger;
        if (RdPawnBreak && !get_move_capture(m) && kPieceKind[pc] == P) {
          const int mover = td.side ^ 1;   // the move is made: the side to move is the opponent
          const U64 wp = td.bitboards[P], bp = td.bitboards[p];
          const int blocked = count_bits((wp >> 8) & bp);   // a8 = 0: the square in front of a white pawn is sq - 8
          if (blocked >= 3 && (pawn_attacks[mover][to] & td.bitboards[mover == white ? p : P]))
            less += RdPawnBreak;
        }
        if (RdDisagree && n.disagree > 0)
          less += RdDisagree * std::min(n.disagree, 8192) / 1024;
        if (RdBoundCapt && get_move_capture(m)) {
          const int pieces = count_bits(td.occupancies[both]);   // after the capture
          less += RdBoundCapt * (pieces == 9 || pieces == 15 || pieces == 23);
        }
        // Positive: less reduction, down to zero at most. Negative weights (allowed since 19/10 evening) add
        // reduction instead, with no floor needed.
        if (less > 0)
          r = std::max(r - less, std::min(r, 0));
        else
          r -= less;
      }
      // C2 search ideas 2, 3, 5, 7 (21/10/2026, agent A; all OFF by default, SPRT pending). Same rule as the levers
      // above: positive = less reduction, down to zero at most; negative = more.
      //  C2SwitchRed  the capture or promotion just made gave another expert to either perspective (the make's flag):
      //               the child is evaluated by another learned regime than the parent;
      //  C2ConvRed    a trade of pieces of the same class while ahead (less) or behind (more) on the conversion blend;
      //  C2PkRed/Vol  a pawn or king move or a pawn capture: d = change of the pawn-king skip, mover's view, clamped to
      //               +-C2PkScale, signed (moves the subnetwork likes are reduced less) and absolute (volatility);
      //  C2AsymLmr    the two perspectives use different experts.
      if (g_c2_red) {
        int less = 0;
        if (C2SwitchRed && c2_last_switched(td))
          less += C2SwitchRed;
        if (c2_trade)
          less += c2_conv * C2ConvRed;
        if (C2AsymLmr && n.c2_asym)
          less += C2AsymLmr;
        if (c2_pkm) {
          const int d = std::clamp(-c2_pk_skip(td) - c2_pk_here, -C2PkScale, C2PkScale);   // child: the other side
          less += (C2PkRed * d + C2PkVol * std::abs(d)) / C2PkScale;
        }
        if (less > 0)
          r = std::max(r - less, std::min(r, 0));
        else
          r -= less;
      }
      // Pawn breaks (28/10/2026, ours, OFF): a quiet pawn push that attacks an enemy pawn, same sign rule.
      if (BreakRed && brk) {
        if (BreakRed > 0)
          r = std::max(r - BreakRed, std::min(r, 0));
        else
          r -= BreakRed;
      }
      // PkCrit (28/10/2026, ours, OFF): a pawn move in a node decided by the pawn structure, same sign rule.
      if (PkCritRed && n.pk_crit && kPieceKind[pc] == P) {
        if (PkCritRed > 0)
          r = std::max(r - PkCritRed, std::min(r, 0));
        else
          r -= PkCritRed;
      }
      // C2 idea 8 (regime selectivity, 21/10/2026; OFF by default, SPRT pending): behind in material class and
      // evaluation, checks and losing captures (the queue's losing stage: sacrifices) are the counterplay the defender
      // must find, so they are reduced less (positive: down to zero at most; negative: more).
      if (C2ImbBehind && n.c2_imb < 0 && (gives || (tact && q.step == QS_TACT_LOSE))) {
        if (C2ImbBehind > 0)
          r = std::max(r - C2ImbBehind, std::min(r, 0));
        else
          r -= C2ImbBehind;
      }
      const int neg_cap = f->negext_path < 1 ? NegExtPlies : 0;   // at most one such extension per line
      if (AN && n.no_lmr && r > 0)   // AnKingDanger / AnCycle: no reduction, the negative reductions stay
        r = 0;
      if (AN && n.root && (td.an_bits & AN_ROOTALT) && root_rank >= 1 && root_rank <= 2 && r > 0)   // AnRootAlt
        r = 0;
      if (AN && tact && (td.an_bits & AN_CAPTLMR) && !(n.cut && f[-1].move_count > 1) && r > 0)    // AnCaptLmr
        r = 0;
      const int d = std::max(1, std::min(new_depth - r / 1024, new_depth + neg_cap)) + n.pv;
      f->negext = d - n.pv > new_depth;
      f->reduction = new_depth - d;
#ifdef TRIUMV_ORDLOG
      ol_red_cur = new_depth - d, ol_fate_cur = new_depth > d ? 2 : 3;
#endif
      value = -explore<AN>(td, f + 1, -(n.alpha + 1), -n.alpha, d, true, false);
      f->reduction = 0;
      f->negext = 0;
#ifdef TRIUMV_REDLOG
      const int value_red = value;
#endif
      if (value > n.alpha) {
#ifdef TRIUMV_REDLOG
        if (redlog_on && d < new_depth)
          redres_probes<AN>(td, f, n, 1, d, new_depth, new_depth - 1, value, red_move_no, redlog_r, redlog_flags);
#endif
        const int full_depth = new_depth;   // before the re-search depth moves it
        if (ResOwn) {   // ours (01_params.inc, off): the margin over alpha must grow with the reduction and the correction
          const int u = std::max(ResU0 + ResUc * (std::abs(n.signal) >> 16) / 64 + ResUr * (new_depth - d), 1);
          const int margin = (value - n.alpha) * 64;
          new_depth += (d < new_depth && margin > ResKp * u) - (margin < ResKm * u);
        } else
          new_depth += (d < new_depth && value > n.best + DeeperMargin) - (value < n.best + ShallowerMargin);
        if (new_depth > d)
          value = -explore<AN>(td, f + 1, -(n.alpha + 1), -n.alpha, new_depth, !n.cut, false);
        on_research(td, f, n, pc, to, value > n.alpha, full_depth - d);   // what the re-search found decides
      }
#ifdef TRIUMV_REDLOG
      if (redlog_on && !stop_threads.load(std::memory_order_relaxed)) {
        // outcome bits: 1 reduced search above alpha, 2 re-searched, 4 final value above alpha, 8 probed, 16 probe
        // above alpha (full-depth null-window search of a move that failed low reduced)
        const int full = n.depth - 1 + ext;
        int outcome = (value_red > redlog_alpha) | ((new_depth > d && value_red > redlog_alpha) << 1) |
                      ((value > redlog_alpha) << 2);
        if (value_red <= redlog_alpha && d < full && redlog_probe(td.thread_id)) {
          const int vp = -explore<AN>(td, f + 1, -(redlog_alpha + 1), -redlog_alpha, full, !n.cut, false);
          if (!stop_threads.load(std::memory_order_relaxed))
            outcome |= 8 | ((vp > redlog_alpha) << 4);
        }
        redlog_write(td.thread_id, n.depth, n.moves, red_move_no, n.expert, f->stat_score, redlog_gap, redlog_cuts,
                     redlog_entry, redlog_r, n.signal, red_window_term, f->ply, redlog_flags, outcome, d, full,
                     std::clamp(n.h.value - f->static_eval, -32000, 32000), val_known(n.h.value), redlog_own,
                     redlog_raw_moves, redlog_own_ok);
      }
#endif
    }
    if (n.pv && (n.moves == 1 || value > n.alpha)) {
      // The hash move of a PV node is not left to the quiescence when its entry has some depth (a decided value from
      // any search, or a plain one from deeper than PvHashDepth): it gets at least PvMinDepth plies.
      if (m == n.h.move && ((val_known(n.h.value) && val_decided(n.h.value) && n.h.depth > 0) || n.h.depth > PvHashDepth))
        new_depth = std::max(new_depth, PvMinDepth);
      value = -explore<AN>(td, f + 1, -n.beta, -n.alpha, new_depth, false, true);
    }
    step_out(td, m, u);
#ifdef TRIUMV_ORDLOG
    if (ol_n < 256)
      ol_mv[ol_n] = m, ol_fate[ol_n] = (signed char)ol_fate_cur, ol_red[ol_n] = (signed char)ol_red_cur, ol_n++;
    ol_fate_cur = 3, ol_red_cur = 0;
#endif

    if (stop_threads.load(std::memory_order_relaxed))
      return 0;
    if (CausalRed && !tact && value <= n.alpha && f[1].refute)
      n.causal = causal_mask(td, f[1].refute);
    if (n.root)
      root_record(td, n, m, value, td.nodes - nodes_before, n.moves);

    const int nudge = (value == n.best && td.ply + NudgePly >= td.root_depth && (int(td.nodes) & 14) == 0 &&
                       !val_won(std::abs(value) + 1)) ? 1 : 0;
    if (value + nudge > n.best) {
      n.best = value;
      if (value + nudge > n.alpha) {
        n.best_move = m;
        n.best_ctx = ctx;
        n.best_score = (!tact && q.rank_ok) ? q.last_score : INT_MIN;
        if (n.pv)
          pv_extend(td, m);
        if (value >= n.beta) {
          f->cutoff_cnt += (ext < CutoffExtCap) || n.pv;
          if (CausalRed)
            f->refute = m;   // for the parent: the move that refutes its own
#ifdef TRIUMV_ORDLOG
          if (!AN && !ol_in_probe) {   // ordlog.h: light cutoff record, oracle, then the killer/counter memories
            const int cstep = m == n.h.move ? -1 : q.step;
            if (ordlog_cut_pick(td.thread_id)) {
              OrdCutRec cr{};
              cr.depth = (int16_t)ol_depth0;
              cr.ply = (int16_t)f->ply;
              cr.expert = (int16_t)n.expert;
              cr.cut_index = (int16_t)n.moves;
              cr.step = (int8_t)cstep;
              cr.flags = (int8_t)(n.pv | (n.cut << 1) | (n.in_check << 2) | (tact << 3) | ((m == n.h.move) << 4));
              ordlog_write(td.thread_id, "ordcut", 2, &cr, 1);
            }
            if (!n.root && !n.excluded && ol_depth0 >= ORDLOG_MIN_DEPTH && ol_depth0 <= ORDLOG_MAX_DEPTH &&
                !val_decided(n.beta) && ordlog_pick(td.thread_id))
              ordlog_oracle<AN>(td, f, n, ol_depth0, m, cstep, ol_mv, ol_fate, ol_red, ol_n);
            if (f->ply < 256)
              ol_killer[f->ply] = m;
            if (f[-1].move > 0) {
              const int lt = move_to(f[-1].move), lp = td.piece_on[lt];
              if (lp >= 0 && lp < 12)
                ol_counter[lp][lt] = m;
            }
          }
          ol_cut = true;
#endif
          // Order of ours (28/10/2026): a node that fails high learns from its cutoff here, where it happens, and then
          // softens its value; node_close only stores the result. Nothing after the break reads or writes the move
          // statistics before the store, and the softening reads the depth and the window, which no longer change.
          // The line of the node's hash bucket is fetched first, with the updates covering the wait.
          TT_PREFETCH(&hash_table[tt_base_index(tt_key(td))]);
          on_node_outcome(td, f, n);   // 13_learn.inc
          soften_fail_high(n);
          break;
        }
        if (n.depth > AlphaRedLo && n.depth < AlphaRedHi && !val_decided(value) &&
            !(AN && (td.an_bits & AN_KEEPDEPTH) && f->ply <= kAnKeepPly))   // AnKeepDepth: not near the root
          n.depth = std::max(n.depth - alpha_raised_cut(n, value - n.alpha), 1);
        n.alpha = value;
      }
    }
    if (m != n.best_move && n.moves <= TRACKED) {
      if (tact) n.tacts[n.n_tacts++] = m;
      else {
        n.quiet_ctx[n.n_quiets] = (unsigned char)ctx;
        n.quiet_score[n.n_quiets] = q.rank_ok ? q.last_score : INT_MIN;
        n.quiets[n.n_quiets++] = m;
      }
    }
  }
#ifdef TRIUMV_ORDLOG
  // ordlog.h: a node that failed low, with at least one move searched. If the oracle finds a refuting move here, the
  // real search missed it (pruned, reduced, or searched too late).
  if (!AN && !ol_cut && !ol_in_probe && n.moves > 0 && !n.root && !n.excluded && ol_depth0 >= ORDLOG_MIN_DEPTH &&
      ol_depth0 <= ORDLOG_MAX_DEPTH && !val_decided(n.beta) && n.best < n.beta && ordlog_low_pick(td.thread_id))
    ordlog_oracle<AN>(td, f, n, ol_depth0, 0, -3, ol_mv, ol_fate, ol_red, ol_n);
#endif
  return NO_VALUE;
}

// --------------------------------------------------------------------------
// Stage 4: end of the node.
// --------------------------------------------------------------------------
// The end of a node (rewritten 27/10/2026 in four steps of ours, same tree; since 28/10/2026 a fail high learns and
// softens its value at the cutoff, in move_loop): let the statistics learn from the outcome of a node that did not fail
// high, store the result, and teach the correction tables the error of the static evaluation.

static int node_close(ThreadData &td, NodeFrame *f, NodeState &n) {
  // A node that failed high (only a cutoff in move_loop gives best >= beta, and the softening keeps it there) has
  // already fetched its hash line, learned and softened its value. For the others the line of the node's hash bucket,
  // read at entry, has often left the cache after the subtree: it is fetched again now, with the statistics to update
  // covering the wait, before the hash_write at the end.
  const bool failed_high = n.best >= n.beta;
  if (!failed_high)
    TT_PREFETCH(&hash_table[tt_base_index(tt_key(td))]);
  if (!n.moves)   // no legal move searched: the exclusion search gives alpha, otherwise mate or stalemate (Contempt)
    n.best = n.excluded ? n.alpha : n.in_check ? mated_at(td.ply) : contempt(td);
  else if (!failed_high)
    on_node_outcome(td, f, n);   // 13_learn.inc

  // A PV node never returns more than the cap (decided value of a tablebase hit), and a node that failed low inherits
  // the PV mark of its parent's hash entry.
  if (n.pv)
    n.best = std::min(n.best, n.cap_value);
  if (n.best <= n.alpha)
    f->tt_pv = f->tt_pv || f[-1].tt_pv;

  // Correction tables before the store (27/10/2026, same tree: the two touch different tables, and the store at the
  // very end gives the prefetch above the most time). CorrLearnMode (06/10/2026, report N_CORREZIONI 4.5): at 0 the
  // usual condition.
  const bool learn_ok = !((CorrLearnMode & 1) && n.excluded && !n.moves) && !((CorrLearnMode & 2) && n.excluded) &&
                        !((CorrLearnMode & 4) && val_decided(n.best));
  const bool dir_ok = (n.best > f->static_eval) == (n.best_move != 0) ||
                      ((CorrLearnMode & 8) && n.pv && n.best_move && n.best < n.beta);
#ifdef TRIUMV_CORRSTATS
  // Residual before and after the correction (cstats.h), BEFORE the direction filter (which would choose the sign):
  // nodes out of check, not excluded, without a better capture, with a known network eval.
  if (!n.in_check && !n.excluded && !(n.best_move && get_move_capture(n.best_move)) && n.eval_raw != tt_eval_none &&
      !val_decided(n.best)) {
    const int plain = eval_refade(n.eval_raw, td.fifty);
    const int fb = td.fifty < 20 ? 0 : td.fifty < 60 ? 1 : 2;
    const int kind = n.best_move ? (n.best >= n.beta ? 0 : 1) : 2;   // fail-high, esatto, fail-low
    CSTAT(net_phase(count_bits(td.occupancies[both])), fb, kind, n.best - plain, n.best - f->static_eval);
  }
#endif
  if (!n.in_check && !(n.best_move && get_move_capture(n.best_move)) && dir_ok && learn_ok)
    on_eval_error(td, f, n);   // 13_learn.inc

  // Store the result (not for an exclusion search, nor for a secondary line at the root).
  if (!n.excluded && !(n.root && td.pv_idx)) {
    const int bound = n.best >= n.beta ? BOUND_LOW : (n.pv && n.best_move) ? BOUND_EXACT : BOUND_UP;
    const int stored_depth = n.moves ? n.depth : std::min(max_ply - 1, n.depth + MateHashDepth);
    hash_write(td, n.best, f->tt_pv, bound, stored_depth, n.best_move, n.eval_raw);
  }
  return n.best;
}

// --------------------------------------------------------------------------
template <bool AN>
static int explore(ThreadData &td, NodeFrame *f, int alpha, int beta, int depth, bool cut, bool pv) {
  if (depth <= 0)
    return ((AN && (td.an_bits & AN_QSCHK)) || (QsChecksPieces && count_bits(td.occupancies[both]) <= QsChecksPieces))
               ? quiesce<true>(td, f, alpha, beta, pv)
               : quiesce<false>(td, f, alpha, beta, pv);
  td.pv_length[td.ply] = td.ply;
  NodeState n;
  n.root = f == &td.frames[FRAME_OFFSET];
  n.pv = pv || n.root;
  n.cut = cut;
  n.all = !(n.pv || cut);
  n.alpha = alpha;
  n.beta = beta;
  n.depth = std::min(depth, max_ply - 1);
  n.in_check = f->in_check;
  n.prior_capture = f[-1].captured >= 0;
  n.hunt_mate = td.root_depth >= (AN ? td.hunt_from : HuntDepth) && std::abs(td.root[td.pv_idx].score) >= HuntScore;
  n.best = -infinity;
  n.best_move = 0;
  n.best_ctx = 0;
  n.moves = 0;
  n.cap_value = infinity;
  n.n_quiets = n.n_tacts = 0;
  n.best_score = INT_MIN;
  n.causal = 0;
  // LdseMax: Ldse extensions on the line up to here. f->ldse goes back to 0 until move_loop extends a move, so
  // the children of the null move and of probcut do not inherit one.
  f->ldse = 0;
  f->ldse_path = f[-1].ldse_path + f[-1].ldse;
  // Same count for the extensions from negative reduction (move_loop; BAKE NEGEXTMAX 11/10/2026).
  f->negext = 0;
  f->negext_path = f[-1].negext_path + f[-1].negext;
  if (C2SettleHorizon) {   // C2 idea 11: the settling plies along the line, the same count
    f->settle = 0;
    f->settle_path = f[-1].settle_path + f[-1].settle;
  }
  // Expert holds (25/10/2026, 01_params.inc, off): the children of a shallow node (C2HoldDepth) keep this node's
  // experts (step_in passes the flag to the make).
  f->nn_hold = (unsigned char)(C2HoldDepth && n.depth <= C2HoldDepth);

  int v = node_open(td, f, n);
  if (v != NO_VALUE) return v;
  n.crystal = AN && crystal_node(td, f);   // after node_open, which decides follow_pv
  // AnKingDanger / AnCycle (Pos Solver, 14/10/2026): in games an_bits = 0 and the two flags stay false.
  n.no_early = n.no_lmr = false;
  if (AN && (td.an_bits & (AN_KDANGER | AN_CYCLE)) && !n.root) {
    if ((td.an_bits & AN_CYCLE) && td_upcoming_repetition(td))
      n.no_early = n.no_lmr = true;
    if (td.an_bits & AN_KDANGER) {
      if (!n.no_early && king_in_danger(td, td.side))
        n.no_early = true;
      if (!n.no_lmr && f->ply <= 6 && king_in_danger(td, td.side ^ 1))
        n.no_lmr = true;
    }
  }
  // The evaluation comes before the hash cutoff (the hindsight depth change of the parent's reduction needs it); the
  // cutoff before the evaluation was tested and removed on 27/10/2026 (HashCutEarly, 01_params.inc).
  node_evaluate(td, f, n);
  if ((v = hash_cutoff(td, f, n)) != NO_VALUE) return v;
  if ((v = tablebase_probe(td, f, n)) != NO_VALUE) return v;
  node_trend(f, n);
  if (!n.in_check && !n.no_early) {
#ifdef TRIUMV_PRUNELOG
    if (!AN && !n.root && n.depth <= PRUNELOG_MAX_DEPTH && !val_decided(n.beta) && !val_decided(n.eval) &&
        prunelog_node_pick(td.thread_id)) {
      if ((v = prunelog_node<AN>(td, f, n)) != NO_VALUE) return v;
    } else
#endif
    if ((v = prune_before_moves<AN>(td, f, n)) != NO_VALUE) return v;
  }

  if ((v = hash_bound_cut(n)) != NO_VALUE)
    return v;

  if ((v = move_loop<AN>(td, f, n)) != NO_VALUE)
    return v;
  return node_close(td, f, n);
}

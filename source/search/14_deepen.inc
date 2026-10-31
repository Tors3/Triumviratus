// search/14_deepen.inc -- Iterative deepening, time management, info lines.
// Part of threads.cpp: NOT a compilation unit of its own.

// ============================================================================
// TIME MANAGEMENT
// Two stages. At the start of a search, a budget for the move: the clock is spread over a horizon of moves (the
// reserve), a share of the reserve is the target time and a multiple of the target is the ceiling. After every
// completed iteration, a verdict: the target is scaled by four factors read from the search (the trend of the score,
// how long the best move has held, how often it changed, how much of the effort it takes) and the search stops once
// the scaled target or the ceiling is spent. Every constant is a Tm* parameter (01_params.inc); the corrections of
// our own (behind on the clock, early middlegame, per expert, expected reply, surprise, defence, jump guard) act on
// top of the two stages.
// ============================================================================

// Effective values of the six budget levers of TM_LONG_LEVERS (21/10/2026): TL(name) is the parameter blended toward
// its long-game value by the game time, fixed at the first timed move of each game; between games (and for searches
// without a clock) the parameter itself. With weight 0 every value equals the int parameter exactly.
#define TL_DECLARE(name, val) static double TL_##name = name;
TM_LONG_LEVERS(TL_DECLARE)
#undef TL_DECLARE
#define TL(name) TL_##name

static void tm_long_blend(double game_ms) {
  const double w = std::clamp((game_ms - TmLongFrom) / std::max(1.0, (double)(TmLongTo - TmLongFrom)), 0.0, 1.0);
#define TL_BLEND(name, val) TL_##name = name + ((val) - name) * w;
  TM_LONG_LEVERS(TL_BLEND)
#undef TL_BLEND
}

struct ClockState {
  int    optimum = 0, maximum = 0;  // target and ceiling of this move, ms
  double prev_time_red = TmPrevRed / 1000.0;   // stability ramp of the previous move (settle factor)
  bool   have_prev = false;         // is there a previous move in this game?
  int    prev_best = 0, prev_avg = 0;
  int    iter_values[4] = {0, 0, 0, 0};   // root score of the last four iterations (trend factor)
  int    iter_idx = 0;
  double best_changes = 0;          // changes of the best move, halved at each iteration (churn factor)
  bool   increase_depth = true;
  // Expected reply (TmExpect*): the position expected after our move and the reply of the principal variation, our
  // next move as predicted, and the score and depth of the search that gave them.
  U64    expect_key = 0;
  int    expect_move = 0, expect_score = 0, expect_depth = 0;
  bool   expected = false;          // this search starts from the expected position
  bool   surprise = false;          // there was an expected reply and the opponent played another one
  double start_clock = 0;           // clock at the first move of the game (TmSurpriseClock)
};
static ClockState g_clock;

#ifdef TRIUMV_TMLOG
// Data build for a stop rule of our own (20/10/2026, -DTRIUMV_TMLOG, never in a playing build). The main thread writes
// one line per completed iteration of a timed search ("I": the factors of iteration_verdict, the time the verdict
// grants and whether it would stop) and one line at the end of the search ("E": the move played). When the verdict
// says stop, the search goes on until TRIUMV_TMLOG_EXT percent of the granted time (or the ceiling), so the data show
// whether the best move would still have changed. File tmlog_<pid>.txt in the folder of TRIUMV_TMLOG_DIR (default .).
#ifndef TRIUMV_TMLOG_EXT
#define TRIUMV_TMLOG_EXT 200
#endif
static int g_tmlog_ply = 0;
static long g_tmlog_search = 0;
static FILE *tmlog_file() {
  static FILE *f = nullptr;
  if (!f) {
#ifdef _WIN32
    const unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    const char *dir = getenv("TRIUMV_TMLOG_DIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/tmlog_%lu.txt", dir && *dir ? dir : ".", pid);
    f = fopen(path, "a");
  }
  return f;
}
#endif

static void clock_memory_reset() {
  tm_long_blend(0);   // the game time is not known before its first timed move
  g_clock.prev_time_red = TmPrevRed / 1000.0;
  g_clock.have_prev = false;
  g_clock.expect_key = 0;
  g_clock.expected = false;
  g_clock.surprise = false;
  g_clock.start_clock = 0;
}

int clock_hard_limit() { return g_clock.maximum; }

// Budget of a move: the reserve (ms that can be spent over the moves still to play), the share of the reserve that
// is the target, and the multiple of the target that is the ceiling.
struct ClockBudget {
  double reserve, share, ceil_mult;
};

// Budget of our own (20/10/2026; the only one since 21/10, when the inherited budget was removed). The moves still to
// play are estimated from the material and the ply of the game, a straight fit on 67,706 of our games (moves left =
// 21.1 + 1.09 x pieces - 0.087 x ply, at least TmMlMin); with moves to go the cycle caps the estimate. The reserve is the clock plus the increments of those moves less the
// overhead of each; the target is a share of the reserve divided by the moves left, where the share grows with the
// log of the reserve in seconds (spending more early at longer time controls), capped by a fraction of the clock.
// The ceiling is a fixed multiple of the target (the clock cap of clock_plan still applies). Starting values give
// the same spending profile as the inherited formula from 5 s to 3 minutes (studio_esperti/tm_own_sim.py).
static void budget_own(ClockBudget &b, double clock, double inc, int overhead, int game_ply) {
  double ml = TL(TmMlBase) / 100.0 + TL(TmMlPiece) / 100.0 * count_bits(occupancies[both]) - TL(TmMlPly) / 1000.0 * game_ply;
  ml = std::max(ml, (double)TmMlMin);
  if (g_limits.movestogo)
    ml = std::min(ml, (double)g_limits.movestogo);
  b.reserve = std::max(1.0, clock + inc * (ml - 1) - (double)overhead * (ml + 1));
  const double share = TL(TmOwnShare) / 1000.0 + TmOwnShareLog / 1000.0 * std::log10(std::max(1.0, b.reserve / 1000.0));
  b.share = std::min(share / ml, TL(TmOwnCap) / 1000.0 * clock / b.reserve);
  b.ceil_mult = TL(TmOwnCeil) / 100.0;
}

// --- C2 time signals (agent B, 21/10/2026) ---
// TmC2Edge: how close the root sits to an edge of the routing table. Each non-pawn piece type that a side has is taken
// off once (a single capture); the result is the share of those captures after which the expert of either perspective
// changes. Near an edge the next trade puts the evaluation in another expert's hands, where the search sees a jump.
static double c2_edge_share(const C2Count &k) {
  const int e0 = c2_experts(k);
  int tried = 0, switched = 0;
  for (int c = 0; c < 2; c++)
    for (int t = N; t <= Q; t++) {
      const int have = t == N ? k.n[c] : t == B ? k.b[c] : t == R ? k.r[c] : k.q[c];
      if (!have)
        continue;
      C2Count k2 = k;
      c2_count_add(k2, t + 6 * c, -1);
      tried++;
      switched += c2_experts(k2) != e0;
    }
  return tried ? (double)switched / tried : 0.0;
}

// TmC2Trans: do the first `plies` moves of a line, played from the thread's root board, leave either perspective on
// another C2 expert? Captures and promotions are followed on a copy of the mailbox (the captured piece is read from
// it); castling moves its rook, en passant removes a pawn (no effect on the signature, kept for later captures).
static bool c2_line_switches(const ThreadData &td, const int *line, int plies) {
  int mb[64];
  memcpy(mb, td.piece_on, sizeof(mb));
  C2Count k = c2_count(td.bitboards);
  const int e0 = c2_experts(k);
  for (int i = 0; i < plies; i++) {
    const int m = line[i], from = get_move_source(m), to = get_move_target(m), pc = get_move_piece(m);
    const int pr = get_move_promoted(m);
    if (get_move_castling(m)) {
      int rook_pc, rf, rt;
      td_castle_rook(to, rook_pc, rf, rt);
      mb[from] = mb[rf] = -1;
      mb[to] = pc;
      mb[rt] = rook_pc;
      continue;
    }
    if (get_move_enpassant(m))
      mb[pc < 6 ? to + 8 : to - 8] = -1;   // a8 = 0: White captures towards lower squares
    else if (get_move_capture(m) && mb[to] >= 0)
      c2_count_add(k, mb[to], -1);
    if (pr)
      c2_count_add(k, pr, +1);
    mb[from] = -1;
    mb[to] = pr ? pr : pc;
  }
  return c2_experts(k) != e0;
}

static void clock_plan(int us, int game_ply, int overhead) {
  if (!g_limits.time[us]) {
    g_clock.optimum = g_clock.maximum = INT_MAX / 2;
    return;
  }
  const double clock = g_limits.time[us], inc = g_limits.inc[us];
  if (g_clock.start_clock <= 0) {
    g_clock.start_clock = clock;   // first move of the game (TmSurpriseClock)
    tm_long_blend(clock + TmLongIncMoves * inc);
  }
#ifdef TRIUMV_TMLOG
  g_tmlog_ply = game_ply;
#endif
  ClockBudget b;
  budget_own(b, clock, inc, overhead, game_ply);
  double opt_share = b.share;
  // TmBehindMul (07/10/2026, the user's idea): with less clock than the opponent the share shrinks (relative
  // advantage, only when negative). Not with movestogo == 1: at the end of a cycle the opponent's clock may already
  // hold the time of the next cycle.
  if (TmBehindMul && g_limits.movestogo != 1) {
    const double mine = g_limits.time[us], theirs = g_limits.time[us ^ 1];
    opt_share *= 1 + TmBehindMul / 1000.0 * std::min((mine - theirs) / (1.0 + mine + theirs), 0.0);
  }
  // TmEarlySave (08/10/2026, baked at 200 on 11/10/2026): with >= 24 pieces errors are rare for both sides; the share
  // shrinks and the time is kept for the phases with 10-23 pieces. The global board is the root board (start_search
  // copies it into the thread before calling clock_plan).
  if (TmEarlySave && count_bits(occupancies[both]) >= 24)
    opt_share *= 1 - TmEarlySave / 1000.0;
  // Time per expert of the network (13/10/2026, the user's idea): the share is multiplied by (1 + TmExpK / 1000)
  // according to the expert of the root; the ceiling follows. All 0: identical times.
  if (TmExp0 | TmExp1 | TmExp2 | TmExp3) {
    const int e = net_phase(count_bits(occupancies[both]));
    const int k = e == 0 ? TmExp0 : e == 1 ? TmExp1 : e == 2 ? TmExp2 : TmExp3;
    opt_share *= 1 + k / 1000.0;
  }
  // C2 time levers (agent B, 21/10/2026; OFF by default, SPRT pending). TmC2Exp0..7: the share by the Consilium Plebis
  // expert of the side to move at the root, the routing region the network itself chose (TmExp0..3 above follow the
  // piece bands of the old network). TmC2Edge: more time the closer the root is to a routing edge (c2_edge_share).
  // The global board is the root board here. All 0: identical times.
  if (TmC2Exp0 | TmC2Exp1 | TmC2Exp2 | TmC2Exp3 | TmC2Exp4 | TmC2Exp5 | TmC2Exp6 | TmC2Exp7 | TmC2Edge) {
    const C2Count k = c2_count(bitboards);
    const int per_expert[8] = {TmC2Exp0, TmC2Exp1, TmC2Exp2, TmC2Exp3, TmC2Exp4, TmC2Exp5, TmC2Exp6, TmC2Exp7};
    opt_share *= 1 + per_expert[c2_expert(k, us)] / 1000.0;
    if (TmC2Edge)
      opt_share *= 1 + TmC2Edge / 1000.0 * c2_edge_share(k);
  }
  g_clock.optimum = (int)std::max(1.0, opt_share * b.reserve);
  g_clock.maximum =
      (int)std::max((double)g_clock.optimum, std::min(TmMaxClock / 1e4 * clock - overhead, b.ceil_mult * g_clock.optimum));
  if (g_ponder_option)
    g_clock.optimum += g_clock.optimum / TmPonderDiv;
}

static inline double lerp_clamped(double x, double x0, double x1, double y0, double y1, double lo, double hi) {
  return std::clamp(y0 + (y1 - y0) * (x - x0) / (x1 - x0), lo, hi);
}

// Verdict of a completed iteration: four factors that scale the target of the move.
//   trend:  more time when the score falls, against the average of the previous move and against the iteration
//           four back, clamped to [TmFallMin, TmFallMax]; with TmJumpGuard a large jump up never cuts the time;
//   settle: less time the longer the best move has held (a ramp on the plies since it last changed), relative to the
//           ramp of the previous move;
//   churn:  more time when the best move changed often (changes summed over threads, halved at each iteration);
//   focus:  less time when the best move takes most of the nodes (a ramp on its share of the nodes, x100000).
struct IterVerdict {
  double trend, settle_ramp, settle, churn, focus;
};

static IterVerdict iteration_verdict(int best, int iter_value, double prev_avg, int plies_held, U64 effort) {
  IterVerdict v;
  v.trend = (TmFallBase / 100.0 + TmFallPrev / 100.0 * (prev_avg - best) + TmFallIter / 100.0 * (iter_value - best)) /
            100.0;
  v.trend = std::clamp(v.trend, TmFallMin / 1000.0, TmFallMax / 1000.0);
  // TmJumpGuard (11/10/2026): a very large jump up against the previous move is more often an error of the search
  // than a discovery; the time that would refute it is not cut. Threshold in printed cp (units_to_cp), as in a GUI.
  if (TmJumpGuard && g_clock.have_prev && !val_decided(best) && !val_decided(g_clock.prev_avg) &&
      units_to_cp(best - g_clock.prev_avg) >= TmJumpGuard)
    v.trend = std::max(v.trend, 1.0);
  v.settle_ramp = lerp_clamped(plies_held, TmRedLo / 1e2, TmRedHi / 1e2, TmRedY0 / 1000.0, TmRedY1 / 1000.0,
                               TmRedMin / 1000.0, TmRedMax / 1000.0);
  v.settle = (TmSettlePrev / 1000.0 + g_clock.prev_time_red) / (TmSettleDiv / 1000.0 * v.settle_ramp);
  v.churn = TmInstabBase / 1000.0 + TmInstabMul / 1000.0 * g_clock.best_changes / num_threads;
  v.focus = lerp_clamped((double)effort, TmEffortLo, TmEffortHi, TmEffortY0 / 1000.0, TmEffortY1 / 1000.0,
                         TmEffortMin / 1000.0, TmEffortMax / 1000.0);
  return v;
}

// ============================================================================
// RIGHE INFO
// ============================================================================
static void wdl_from_cp(int cp, int &w, int &d, int &l) {
  const double pw = 1.0 / (1.0 + std::exp(-(cp - 100.0) / 100.0));
  const double pl = 1.0 / (1.0 + std::exp(-(-cp - 100.0) / 100.0));
  w = (int)(pw * 1000 + 0.5);
  l = (int)(pl * 1000 + 0.5);
  d = std::max(0, 1000 - w - l);
}

// Tablebase wins (23/10/2026, display only): they were printed through units_to_cp like an evaluation, which gave
// about +-94.55 (and a value that moved with EvalScale). Now the usual convention: cp 20000 minus the distance in plies
// from the root, so a GUI shows +-199.xx; mates stay "mate N".
static void print_score(char *out, size_t sz, int v) {
  if (v >= MATE_FLOOR)
    snprintf(out, sz, "mate %d", (mate_value - v + 1) / 2);
  else if (v <= -MATE_FLOOR)
    snprintf(out, sz, "mate %d", -(mate_value + v) / 2);
  else if (v >= TB_WIN_FLOOR)
    snprintf(out, sz, "cp %d", 20000 - (TB_WIN_TOP - v));
  else if (v <= -TB_WIN_FLOOR)
    snprintf(out, sz, "cp %d", -20000 + (TB_WIN_TOP + v));
  else
    snprintf(out, sz, "cp %d", units_to_print_cp(v));   // screen scale (09_history.inc, 28/10/2026)
}

static void report_lines(ThreadData &td, int depth) {
  U64 nodes = 0, tb = 0;
  for (auto &t : thread_data) {
    nodes += t.nodes;
    tb += t.tb_hits;
  }
  const int elapsed = std::max(1, get_time_ms() - g_limits.start_ms);
  const int lines = std::min(g_multipv, td.root_count);
  std::lock_guard<std::mutex> lock(output_mutex);
  for (int i = 0; i < lines; i++) {
    const RootLine &rl = td.root[i];
    const bool use_prev = rl.score == -infinity;
    if (depth == 1 && use_prev && i > 0)
      continue;
    const int d = use_prev ? std::max(1, depth - 1) : depth;
    int v = use_prev ? rl.prev_score : rl.uci_score;
    if (v == -infinity)
      v = 0;
    char sc[32];
    print_score(sc, sizeof sc, v);
    printf("info depth %d seldepth %d multipv %d score %s", d, rl.sel_depth, i + 1, sc);
    if (!use_prev && rl.lower)
      printf(" lowerbound");
    else if (!use_prev && rl.upper)
      printf(" upperbound");
    if (g_show_wdl) {
      int w, dd, l;
      if (v >= TB_WIN_FLOOR) { w = 1000; dd = l = 0; }        // mates and tablebase wins
      else if (v <= -TB_WIN_FLOOR) { l = 1000; w = dd = 0; }
      else wdl_from_cp(units_to_print_cp(v), w, dd, l);
      printf(" wdl %d %d %d", w, dd, l);
    }
    printf(" nodes %llu nps %llu hashfull %d tbhits %llu time %d pv", (unsigned long long)nodes,
           (unsigned long long)(nodes * 1000 / elapsed), hashfull(), (unsigned long long)tb, elapsed);
    const int *pv = use_prev ? rl.prev_pv : rl.pv;
    const int len = use_prev ? rl.prev_pv_len : rl.pv_len;
    for (int j = 0; j < len; j++) {
      printf(" ");
      print_move(pv[j]);
    }
    printf("\n");
  }
  fflush(stdout);
}

// ============================================================================
// ITERATIVE DEEPENING (one thread)
// ============================================================================
static inline bool line_before(const RootLine &a, const RootLine &b) {
  return a.score != b.score ? a.score > b.score : a.prev_score > b.prev_score;
}

static void settle_lines(ThreadData &td, int from, int to) {
  std::stable_sort(td.root + from, td.root + to, line_before);
}

// AnDirect (Pos Solver, 15/10/2026): the position collections give the most direct win as the key, and whoever
// analyses wants to see that one; we often choose a quiet move with a slightly higher score.
// With two exact lines both above DirectFrom and at most DirectGap apart, if the second is forcing (check,
// capture, promotion) and the first is not, they are swapped. It must be called with the board at the root.
// Direct move at the root (AnDirect, AnDirLine): check, capture, promotion, or sacrifice (quiet move with
// negative static exchange). Without the sacrifice AnDirect lost IQ4 162 (key Bd5, swapped with the check Nf5+).
static bool root_direct(ThreadData &td, MoveSafety &s, int m) {
  return is_tactical(m) || checks_before(td, s, m) || !see_holds(td, m, 0);
}

// The first line is quiet and clearly winning (threshold DirectFrom): the conditions of AnDirect and AnDirLine.
static bool root_quiet_win(ThreadData &td, MoveSafety &s) {
  const RootLine &a = td.root[0];
  return a.score != -infinity && !a.upper && !a.lower && !val_decided(a.score) &&
         units_to_cp(a.score) >= DirectFrom && !root_direct(td, s, a.pv[0]);
}

// Evening of 15/10/2026: returns true if it swapped; deepen restores the order before the next iteration, so
// the tree stays that of AnShadow and only the line shown and the bestmove change.
static bool an_direct(ThreadData &td, int lines) {
  if (!(td.an_bits & AN_DIRECT) || lines < 2)
    return false;
  RootLine &a = td.root[0], &b = td.root[1];
  if (a.score == -infinity || b.score == -infinity || a.upper || a.lower || b.upper || b.lower ||
      val_decided(a.score) || val_decided(b.score))
    return false;
  const int ca = units_to_cp(a.score), cb = units_to_cp(b.score);
  if (cb < DirectFrom || ca - cb > DirectGap)
    return false;
  MoveSafety s;
  s.ready = false;
  if (root_direct(td, s, b.pv[0]) && !root_direct(td, s, a.pv[0])) {
    std::swap(a, b);
    return true;
  }
  return false;
}

static void prepare_frames(ThreadData &td) {
  memset(td.frames, 0, sizeof(td.frames));
  for (int i = 0; i < (int)(sizeof(td.frames) / sizeof(td.frames[0])); i++) {
    NodeFrame &fr = td.frames[i];
    fr.cont_hist = &g_stats.follow[0][0][NO_PC][0];
    fr.cont_corr = &td.cont_corr[NO_PC][0];
    fr.captured = -1;
    fr.static_eval = VAL_NONE;
    fr.ply = i - FRAME_OFFSET;
  }
}

static void set_thread_optimism(ThreadData &td, int avg) {
  // optimism in network units: the mean score goes back to those units before the formula.
  const int net = (int)((long long)avg * 1024 / EvalUnits);
  const int o = OptStrength * net / (std::abs(net) + OptDiv);
  td.opt[td.side] = o;
  td.opt[td.side ^ 1] = -o;
  nn_pos_set_optimism(td.nnpos, td.opt[0], td.opt[1]);
}

// ---------------------------------------------------------------------------
// ANALYSIS MODE (09/10/2026; options and origin of the ideas in search/01_params.inc).
// ---------------------------------------------------------------------------

// GoldDigger (AnGold). Mechanism of ShashChess (amchess), build -DGOLD_DIGGER, read in the public source: the root
// is classified (Shashin style from the win probability of the evaluation: defence/"Petrosian" <= 24%,
// balance/"Capablanca" ~50%, attack/"Tal" 80-89%, clear advantage >= 95%) and marked with three static facts (a
// possible capture with a piece worth more than the captured one, a pawn beyond the middle of the board, a king attacked with
// weight > 5). From there: more cautious razoring, node futility margin (our RFP) x1.5 or x1.2, null move
// skipped when the tactical heat reaches 4 (gd_skip_null, 13_search.inc), reductions at 97% or 103% and small tweaks
// per style, aspiration window narrower in balance and wider in attack. Our code on our
// search; not ported: fortresses, move-count threshold per style, and everything ShashChess does even without
// GoldDigger. Static once per search, style recomputed at every iteration with the score of the previous iteration.
struct GdRoot { bool sacrificial, near_promo, king_danger, many_moves; };

static GdRoot gd_root_facts(ThreadData &td) {
  GdRoot g{};
  for (int i = 0; i < td.root_count; i++) {   // root moves: already legal
    const int m = td.root[i].pv[0];
    if (!get_move_capture(m) || kPieceKind[move_piece(m)] == K)
      continue;
    const int *pv = piece_values_mat(td);   // per-expert values of the root (see.cpp)
    int mover = pv[move_piece(m)];
    if (get_move_promoted(m))
      mover += pv[get_move_promoted(m) & 15] - pv[move_piece(m)];
    const int victim = get_move_enpassant(m) ? pv[td.side == white ? p : P] : pv[td.piece_on[move_to(m)] & 15];
    if (mover > victim) { g.sacrificial = true; break; }
  }
  // Pedoni bianchi in 5a-7a (righe 1-3 con a8 = 0, case 8-31), neri in 2a-4a (righe 4-6, case 32-55).
  g.near_promo = (td.bitboards[P] & 0x00000000FFFFFF00ULL) || (td.bitboards[p] & 0x00FFFFFF00000000ULL);
  for (int c = white; c <= black && !g.king_danger; c++) {
    U64 a = td_attackers_to(td, get_ls1b_index(td.bitboards[c == white ? K : k]), c ^ 1);
    int sum = 0;
    while (a) {
      const int sq = get_ls1b_index(a);
      const int kind = kPieceKind[td.piece_on[sq] & 15];
      sum += (kind == Q || kind == R) ? 3 : 1;
      pop_lsb_bb(a);
    }
    g.king_danger = sum > 5;
  }
  g.many_moves = td.root_count > 30;
  return g;
}

static void gd_iteration(ThreadData &td, const GdRoot &g, int score) {
  int w, d, l;
  wdl_from_cp(units_to_cp(score), w, d, l);
  const int wp = (w + d / 2 + 5) / 10;   // win probability in percent
  const bool petrosian = wp <= 24, aggressive = wp >= 80 && wp <= 89, high_tal = wp >= 95;
  const bool strategical = wp >= 45 && wp <= 55 && !g.king_danger && !g.near_promo;
  td.gd_style = aggressive ? 1 : strategical ? 2 : petrosian ? 3 : high_tal ? 4 : 0;
  td.gd_heat = 10 * g.king_danger + 10 * g.sacrificial + 2 * high_tal + 2 * aggressive + 2 * g.near_promo +
               g.many_moves;
  td.gd_rfp_pct = (aggressive || g.sacrificial) ? 150 : strategical ? 120 : 100;
  td.gd_razor = (aggressive || g.sacrificial) ? 150 : strategical ? 50 : 0;
}

// Effective values of the analysis mode for the current iteration (after lt_update). In games (an_bits = 0)
// they reproduce exactly the old computation: deep_w from the parameters DeepKnee/DeepSpan, levers = Deep* x deep_w / 1024.
static void an_iteration(ThreadData &td, bool plain = false) {   // plain: AnQDeep, preset AnDeep 0 for the iteration
  const bool on = td.an_bits != 0;
  int deep = on && !plain ? g_an_deep : 0;
  if (on && (td.an_bits & AN_SMP) && (td.thread_id & 1))   // AnSmp: the odd helper threads with the wider preset
    deep = 3;
  const bool ext = on && (td.an_bits & AN_EXT);
  if (deep || ext) {
    const DeepPreset &p = kAnDeep[deep];
    td.deep_w = std::clamp((td.root_depth - kAnDeepKnee) * 1024 / kAnDeepSpan, 0, 1024);
    const int w = td.deep_w;
    td.dz_rfp = p.rfp * w / 1024;
    td.dz_fut = p.fut * w / 1024;
    td.dz_lmr = p.lmr * w / 1024;
    td.dz_nmp = p.nmp * w / 1024;
    td.dz_sing = (ext ? kAnSing : 0) * w / 1024;
    td.dz_ext = (ext ? kAnExt : 0) * w / 1024;
  } else {
    // Idea 2 (Deep*): weight 0..1024 of the "deep" terms, from DeepKnee to DeepKnee + DeepSpan.
    td.deep_w = std::clamp((td.root_depth - DeepKnee) * 1024 / DeepSpan, 0, 1024);
    const int w = td.deep_w;
    td.dz_rfp = DeepRfp * w / 1024;
    td.dz_fut = DeepFut * w / 1024;
    td.dz_lmr = DeepLmr * w / 1024;
    td.dz_nmp = DeepNmp * w / 1024;
    td.dz_sing = DeepSing * w / 1024;
    td.dz_ext = DeepExt * w / 1024;
  }
  if (on && (td.an_bits & AN_NMPV))
    td.lt_nmp_verify_depth = kAnNmpVerify;
  td.hunt_from = (on && (td.an_bits & AN_HUNT)) ? kAnHuntFrom : HuntDepth;
  td.crystal_ply = (on && (td.an_bits & AN_CRYSTAL)) ? std::max(2, td.root_depth / 2) : -1;
}

static void deepen(ThreadData &td) {
  const bool main = td.thread_id == 0;
#ifdef TRIUMV_TMLOG
  if (main)
    g_tmlog_search++;
#endif
  NodeFrame *root = &td.frames[FRAME_OFFSET];
  prepare_frames(td);
  // SmpRouteShift (21/10/2026): the odd helpers evaluate with the shifted routing table (built in start_search), set
  // here, in the thread itself, before its first evaluation; every other thread with the network's table.
  nn_thread_router(SmpRouteShift && (td.thread_id & 1));
  root->in_check = td_is_square_attacked(td, get_ls1b_index(td.bitboards[td.side == white ? K : k]), td.side ^ 1);
  td.root_side = td.side;   // Contempt: the draw is worth less for the side to move at the root
  // DrawDefend: from the score with which the previous move closed (fixed for the whole search and the same for all the
  // threads; recomputing it at every iteration would turn it off as soon as the search finds the draw it was looking for).
  td.draw_defend = 0;
  if (DrawDefend && g_clock.have_prev && g_clock.prev_best != -infinity && g_clock.prev_best <= -DrawDefendFrom)
    td.draw_defend = (int)((long long)DrawDefend *
                           std::min<long long>(1024, (long long)(-g_clock.prev_best - DrawDefendFrom) * 1024 / DrawDefendSpan) /
                           1024);
  // C2ConvRoot (the author's idea, 21/10/2026 evening): the conversion push decided once here, from the root's static
  // evaluation and exact pawn-king effect (mode 2 decides again at every iteration, below).
  td.c2_conv_root = 0;
  if (C2ConvRoot && (C2ConvOrder | C2ConvRed) && !root->in_check)
    td.c2_conv_root = c2_conv_root_decide(td, static_evaluate(td));
  td.deep_w = 0;
  td.lt_lmr_log_mul = -1;   // forces the construction of the thread's table at the first iteration (root_depth is 0)
  lt_update(td);
  // Analysis mode: decided once per search (a setoption during the search applies from the next one).
  // Pos Solver (TRIUMV_POS_SOLVER): always on, whatever the GUI sends (user 14/10/2026).
  const bool analyse = PSV(false, true) || g_analyse_mode || (g_analyse_infinite && g_limits.infinite);
  td.an_bits = analyse ? (g_an_bits | AN_ACTIVE) : 0;
  // One line per search, from the main thread only and only in analysis (in games an_bits = 0: no printing), with
  // the preset in use: whoever analyses sees that the mode is on and with which components.
  if (main && (td.an_bits & AN_ACTIVE)) {
    static const struct { const char *name; int bit; } kAnNames[] = {
      {"AnExt", AN_EXT}, {"AnNmpVerify", AN_NMPV}, {"AnQsChecks", AN_QSCHK}, {"AnSmp", AN_SMP},
      {"AnCrystal", AN_CRYSTAL}, {"AnHunt", AN_HUNT}, {"AnGold", AN_GOLD},
      {"AnKingDanger", AN_KDANGER}, {"AnCycle", AN_CYCLE},
    {"AnShadow", AN_SHADOW}, {"AnRootAlt", AN_ROOTALT}, {"AnKeepDepth", AN_KEEPDEPTH},
    {"AnLmpNear", AN_LMPNEAR}, {"AnCaptLmr", AN_CAPTLMR}, {"AnNmpZug", AN_NMPZUG}, {"AnStopKeep", AN_STOPKEEP}, {"AnDirect", AN_DIRECT}, {"AnDirLine", AN_DIRLINE}, {"AnShQuiet", AN_SHQUIET}, {"AnQDeep", AN_QDEEP}};
    printf("info string Analysis mode activated AnDeep=%d", g_an_deep);
    for (const auto &o : kAnNames)
      if (td.an_bits & o.bit)
        printf(" %s", o.name);
    printf("\n");
    fflush(stdout);
  }
  an_iteration(td);
  td.gd_heat = -1000;
  td.gd_rfp_pct = 100;
  td.gd_razor = 0;
  td.gd_style = 0;
  const bool gold = td.an_bits & AN_GOLD;
  GdRoot gd_facts{};
  if (gold) {
    gd_facts = gd_root_facts(td);
    gd_iteration(td, gd_facts, static_evaluate(td));
  }

  if (main) {
    for (int &v : g_clock.iter_values)
      v = g_clock.have_prev ? g_clock.prev_best : 0;
    g_clock.iter_idx = 0;
    g_clock.best_changes = 0;
  }
  // AnShadow (Pos Solver, 14/10/2026 notte): si cercano almeno 2 linee a finestra piena, se ne stampano g_multipv.
  const int shown = std::min(g_multipv, td.root_count);
  const int lines = std::min(std::max(g_multipv, (td.an_bits & AN_SHADOW) ? 2 : 1), td.root_count);
  int again = 0;
  double time_red = 1;
  int last_best_depth = 0, last_best_score = -infinity;
  int last_best_pv[max_ply + 4], last_best_len = 0;

  fill_with(&td.low_ply_hist[0][0], (size_t)LOW_PLY_SIZE * QH_SIZE, LowPlyFill);
  for (int c = 0; c < 2; c++)
    for (int i = 0; i < QH_CTX_SIZE; i++)
      td.quiet_hist[c][i] = (int16_t)(td.quiet_hist[c][i] * QuietHistDecay / 1024);

  // SmpSkew (block 5; off at 0): the odd helper threads start SmpSkew plies ahead, so the threads finish their
  // iterations at different times and the shared table receives a wider mix of depths (a known lazy-SMP variant).
  // Thread 0 never changes. Test 17/10/2026, with SmpVoteDepth, 4 threads 2+0.05: +1.72 +- 4.61 over 5,648 games; off,
  // queued for the end of 8.1.
  if (SmpSkew && (td.thread_id & 1))
    td.root_depth = SmpSkew;

  int best = -infinity;
  bool direct_swapped = false;
  int quiet_hist[3] = {-infinity, -infinity, -infinity};   // AnShQuiet: score of the last three iterations
  while (td.root_depth + 1 < max_ply && !stop_threads.load(std::memory_order_relaxed) &&
         !(g_limits.depth && main && td.root_depth >= g_limits.depth)) {
    td.root_depth++;
    // C2ConvRoot 2: the decision follows the score of the last completed iteration (the search's judgement of the
    // root, instead of its static evaluation).
    if (C2ConvRoot == 2 && (C2ConvOrder | C2ConvRed) && !root->in_check && td.root_depth > 1 &&
        td.root[0].score > -infinity)
      td.c2_conv_root = c2_conv_root_decide(td, td.root[0].score);
    if (direct_swapped) {   // AnDirect: the search restarts from the true order of the lines
      std::swap(td.root[0], td.root[1]);
      direct_swapped = false;
    }
    lt_update(td);   // "long time" ramp: effective values of the Lt* levers for this root depth
    // Quiet and stable position as seen by the search (AnShQuiet, AnQDeep): from QuietFrom, score within QuietCp for
    // three iterations, first two moves not direct.
    bool quiet_now = false;
    if ((td.an_bits & (AN_SHQUIET | AN_QDEEP)) && td.root_count >= 2 && td.root_depth > QuietFrom &&
        quiet_hist[2] != -infinity && !val_decided(quiet_hist[0]) &&
        std::abs(units_to_cp(quiet_hist[0] - quiet_hist[1])) <= QuietCp &&
        std::abs(units_to_cp(quiet_hist[0] - quiet_hist[2])) <= QuietCp) {
      MoveSafety s;
      s.ready = false;
      quiet_now = !root_direct(td, s, td.root[0].pv[0]) && !root_direct(td, s, td.root[1].pv[0]);
    }
    an_iteration(td, quiet_now && (td.an_bits & AN_QDEEP));   // leve Deep* pesate (deep_w) e i componenti accesi
    if (gold && td.root_depth > 1 && td.root[0].score > -infinity)
      gd_iteration(td, gd_facts, td.root[0].score);
    if (main)   // TmChangesKeep / 1000.0 = 0.5 at the default: the same double as the former halving
      g_clock.best_changes *= TmChangesKeep / 1000.0;
    for (int i = 0; i < td.root_count; i++) {
      RootLine &rl = td.root[i];
      rl.prev_score = rl.score;
      memcpy(rl.prev_pv, rl.pv, sizeof(int) * rl.pv_len);
      rl.prev_pv_len = rl.pv_len;
      rl.prev_exact = i < lines;
    }
    if (!g_clock.increase_depth)
      again++;

    // Variants of AnShadow (ShadowFrom/ShadowMargin): in this iteration only the lines shown are searched.
    int iter_lines = lines;
    if (lines > shown &&
        (td.root_depth < ShadowFrom ||
         (ShadowMargin && td.root[shown].score != -infinity && td.root[0].score != -infinity &&
          td.root[0].score - td.root[shown].score > ShadowMargin)))
      iter_lines = shown;
    // AnShQuiet: quiet and stable position -> a single line in this iteration.
    if ((td.an_bits & AN_SHQUIET) && quiet_now && iter_lines > shown)
      iter_lines = shown;
    for (td.pv_idx = 0; td.pv_idx < iter_lines; td.pv_idx++) {
      td.pv_last = td.root_count;
      // AnDirLine: with the first line quiet and winning, the hidden line of AnShadow searches only the direct moves,
      // brought to the head of the rest of the list (relative order unchanged).
      if ((td.an_bits & AN_DIRLINE) && td.pv_idx == shown && td.pv_idx >= 1) {
        MoveSafety s;
        s.ready = false;
        if (root_quiet_win(td, s)) {
          RootLine *first = td.root + td.pv_idx, *last = td.root + td.root_count;
          RootLine *mid = std::stable_partition(first, last, [&](const RootLine &rl) { return root_direct(td, s, rl.pv[0]); });
          if (mid != first)   // without direct moves the second line stays the normal one
            td.pv_last = (int)(mid - td.root);
        }
      }
      RootLine &cur = td.root[td.pv_idx];
      memcpy(td.last_pv, cur.prev_pv, sizeof(int) * cur.prev_pv_len);
      td.last_pv_len = cur.prev_pv_len;
      td.sel_depth = 0;

      // Aspiration window (block 5): AspBase + an offset per thread, (id % SmpAspCycle) x SmpAspStep (default id % 8),
      // + the spread of the root scores. The offset makes the threads fail high and low at different moments.
      int delta = AspBase + td.thread_id % SmpAspCycle * SmpAspStep + (int)(std::llabs(cur.mean_sq) / AspMeanSqDiv);
      if (td.gd_style == 2)   // AnGold: a narrower window in balanced positions, a wider one in attack
        delta = std::max(6, delta * 9 / 10);
      else if (td.gd_style == 1 || td.gd_style == 4)
        delta = delta * 9 / 8;
      const int avg = cur.avg_score;
      int alpha = std::max(avg - delta, -infinity);
      int beta = std::min(avg + delta, (int)infinity);
      set_thread_optimism(td, avg);
      nn_root_sync(td);

      int failed_high = 0;
      for (;;) {
        // AspFhRed plies off for each fail high in a row (was 1); AspAgainMul quarters per search repeated at the same
        // depth (was 3).
        const int depth = std::max(1, td.root_depth - failed_high * AspFhRed - AspAgainMul * (again + 1) / 4 -
                                      (td.pv_idx >= shown ? ShadowRed : 0));   // ShadowRed: a shorter second line
        td.root_delta = beta - alpha;
        best = td.an_bits ? explore<true>(td, root, alpha, beta, depth, false, true)    // analysis mode
                          : explore<false>(td, root, alpha, beta, depth, false, true);  // games: constant terms
        settle_lines(td, td.pv_idx, td.pv_last);
        if (stop_threads.load(std::memory_order_relaxed))
          break;
        U64 total = 0;
        if (main && shown == 1 && td.pv_idx == 0 && (best <= alpha || best >= beta)) {
          for (auto &t : thread_data) total += t.nodes;
          if (total > 10000000)
            report_lines(td, td.root_depth);
        }
        // Outside the window: separate growth after a fail low and a fail high (AspGrowLo, AspGrowHi; one AspGrow served
        // both), and after a fail low beta keeps AspLoKeep/1024 of the window (ours, off; 0 = beta falls to alpha).
        int grow;
        if (best <= alpha) {
          beta = alpha + (int)((long long)(beta - alpha) * AspLoKeep / 1024);
          alpha = std::max(best - delta, -infinity);
          failed_high = 0;
          grow = AspGrowLo;
          if (main)
            g_stop_at_ponderhit.store(false, std::memory_order_relaxed);
        } else if (best >= beta) {
          alpha = std::max(beta - delta, alpha);
          beta = std::min(best + delta, (int)infinity);
          failed_high++;
          grow = AspGrowHi;
        } else
          break;
        delta += grow * delta / 128;
      }

      settle_lines(td, 0, td.pv_idx + 1);
      if (td.pv_idx + 1 == iter_lines && !stop_threads.load(std::memory_order_relaxed))
        direct_swapped = an_direct(td, iter_lines);   // AnDirect before printing: the GUI sees the direct move
      if (main && !stop_threads.load(std::memory_order_relaxed)) {
        U64 total = 0;
        for (auto &t : thread_data) total += t.nodes;
        if (td.pv_idx + 1 == iter_lines || total > 10000000)
          report_lines(td, td.root_depth);
      }
      if (stop_threads.load(std::memory_order_relaxed))
        break;
    }
    // AnStopKeep (night of 14/10/2026, only with several lines): stopped mid-iteration, the lines from pv_idx down have
    // scores of different depths; the one with a fresh upper bound (fail low not yet resolved)
    // goes back to the full value of the previous iteration instead of ending up behind old scores of the others.
    if ((td.an_bits & AN_STOPKEEP) && lines > 1 && stop_threads.load(std::memory_order_relaxed)) {
      for (int i = td.pv_idx; i < td.root_count; i++) {
        RootLine &rl = td.root[i];
        if (rl.upper && rl.prev_score != -infinity && rl.prev_pv_len) {
          rl.score = rl.uci_score = rl.prev_score;
          memcpy(rl.pv, rl.prev_pv, sizeof(int) * rl.prev_pv_len);
          rl.pv_len = rl.prev_pv_len;
          rl.upper = rl.lower = false;
        }
      }
      settle_lines(td, td.pv_idx, td.root_count);
      settle_lines(td, 0, lines);
    }
    td.pv_idx = 0;
    if (!stop_threads.load(std::memory_order_relaxed) && td.root[0].score != -infinity) {   // AnShQuiet
      quiet_hist[2] = quiet_hist[1];
      quiet_hist[1] = quiet_hist[0];
      quiet_hist[0] = td.root[0].score;
    }
    if (!direct_swapped)   // search stopped mid-iteration: the rule applies to the reprinted line too
      direct_swapped = an_direct(td, iter_lines);

    // A mate already proved is not forgotten: if the iteration loses it, we go back to the line that had it.
    RootLine &top = td.root[0];
    const bool stopped = stop_threads.load(std::memory_order_relaxed);
    const bool forgot_mate = last_best_score != -infinity && val_mating(last_best_score) &&
                             (std::abs(top.score) < std::abs(last_best_score) || top.lower || top.upper);
    if (!stopped) {
      if (!last_best_len || last_best_pv[0] != top.pv[0])
        last_best_depth = td.root_depth;
      if (!forgot_mate) {
        memcpy(last_best_pv, top.pv, sizeof(int) * top.pv_len);
        last_best_len = top.pv_len;
        last_best_score = top.score;
      }
      td.completed_depth = td.root_depth;
    }
    const bool aborted_loss = stopped && val_lost(top.score) && top.score != -infinity && !top.lower && !top.upper;
    if ((aborted_loss || (top.score != -infinity && forgot_mate)) && last_best_len) {
      for (int i = 0; i < td.root_count; i++)
        if (td.root[i].pv[0] == last_best_pv[0]) {
          RootLine keep = td.root[i];
          for (int j = i; j > 0; j--)
            td.root[j] = td.root[j - 1];
          td.root[0] = keep;
          break;
        }
      td.root[0].score = td.root[0].uci_score = last_best_score;
      memcpy(td.root[0].pv, last_best_pv, sizeof(int) * last_best_len);
      td.root[0].pv_len = last_best_len;
      td.root[0].lower = td.root[0].upper = false;
    }

    // V0 (08/10/2026): with the search stopped mid-iteration the lines have been reordered with the partial results
    // (only fully searched moves: root_record does not run after a stop), but the line was printed only at the end of an
    // iteration. The bestmove could then not be the first move of the last pv printed (fastchess: "Bestmove
    // does not match beginning of last PV", ~once every 3 games). The final line is reprinted, after the
    // recovery of the mate and of the lost line.
    // Night of 14/10/2026: also with MultiPV >= 2 (before only with one line): the bestmove could differ from line 1
    // shown (Arasan 36: line 1 c5b6, bestmove f2g3). In games MultiPV is 1: nothing changes.
    if (main && stopped && td.completed_depth > 0)
      report_lines(td, td.root_depth);

    if (g_limits.mate && !stopped && val_mating(td.root[0].score) &&
        mate_value - std::abs(td.root[0].score) <= 2 * g_limits.mate)
      stop_threads.store(true, std::memory_order_relaxed);

    if (!main)
      continue;

    for (auto &t : thread_data) {
      g_clock.best_changes += t.best_move_changes;
      t.best_move_changes = 0;
    }

    if (g_limits.use_tm() && !stop_threads.load(std::memory_order_relaxed) &&
        !g_stop_at_ponderhit.load(std::memory_order_relaxed)) {
      const U64 effort = td.root[0].effort * 100000 / std::max<U64>(1, td.nodes);
      const double prev_avg = g_clock.have_prev ? g_clock.prev_avg : (double)infinity;
      const IterVerdict v =
          iteration_verdict(best, g_clock.iter_values[g_clock.iter_idx], prev_avg, td.root_depth - last_best_depth, effort);
      time_red = v.settle_ramp;
      double total_time = g_clock.optimum * v.trend * v.settle * v.churn * v.focus;
      if (td.root_count == 1)
        total_time = std::min<double>(TmSingleMs, total_time);
      // Expected reply (TmExpect*, see ClockState): the time is reduced only while all three
      // conditions hold, rechecked at every iteration; if one fails, the move gets the full time back.
      if (g_clock.expected && TmExpectScale < 1000 && td.root_depth >= g_clock.expect_depth - TmExpectDepth &&
          td.root[0].pv[0] == g_clock.expect_move && td.root[0].score >= g_clock.expect_score - TmExpectMargin)
        total_time *= TmExpectScale / 1000.0;
      // Surprise (TmSurpriseScale): the opponent did not play the expected reply, more time for this move, but
      // only with the clock still above TmSurpriseClock % of the one at the start of the game.
      // TmSurpriseBehind (30/10/2026, OFF): silent too while our clock is behind the opponent's by more than
      // TmSurpriseBehind/1000 of theirs.
      const double sur_mine = g_limits.time[td.root_side], sur_theirs = g_limits.time[td.root_side ^ 1];
      const bool sur_behind = TmSurpriseBehind && sur_theirs > sur_mine &&
                              (sur_theirs - sur_mine) > TmSurpriseBehind / 1000.0 * sur_theirs;
      if (g_clock.surprise && TmSurpriseScale > 1000 && !sur_behind &&
          100.0 * g_limits.time[td.root_side] >= TmSurpriseClock * g_clock.start_clock) {
        // TmSurpriseGrade (0 = off, identical timing): weight of the extra time according to how much the reply changed
        // the score.
        if (TmSurpriseGrade) {
          const double w = std::clamp(1.0 + (double)(g_clock.expect_score - best) / TmSurpriseGrade, 0.0, 2.0);
          total_time *= 1.0 + (TmSurpriseScale / 1000.0 - 1.0) * w;
        } else
          total_time *= TmSurpriseScale / 1000.0;
      }
      // TmDefendMul (08/10/2026, audit X3; BAKED at 500 on 11/10/2026): in defence (root between -TmDefendTo and -TmDefendFrom) more time; the
      // cap stays the move's maximum.
      // TmDefendClock / TmDefendFade (30/10/2026, OFF): the extra time of defence shrinks with the clock, so that a
      // long defence does not leave the engine short of time later. TmDefendClock: with less clock than the opponent
      // the extra is scaled by
      // 1 - TmDefendClock/1000 x relative deficit. TmDefendFade: below TmDefendFade/1000 of the starting clock the
      // extra fades linearly to 0. Both 0: identical timing.
      if (TmDefendMul && best <= -TmDefendFrom && best > -TmDefendTo) {
        double extra = TmDefendMul / 1000.0;
        const double mine = g_limits.time[td.root_side], theirs = g_limits.time[td.root_side ^ 1];
        if (TmDefendClock && mine < theirs)
          extra *= std::max(0.0, 1.0 - TmDefendClock / 1000.0 * (theirs - mine) / theirs);
        if (TmDefendFade && g_clock.start_clock > 0)
          extra *= std::min(1.0, mine / (TmDefendFade / 1000.0 * g_clock.start_clock));
        total_time *= 1 + extra;
      }
      // TmTrans (20/10/2026, ours, 0 = off): the captures in the first TmTransPlies moves of the principal variation
      // take the game into another expert of the network (another material band). Such trades are hard to undo, so
      // the move gets TmTransMul more time; the other moves give back TmTransRest, so that the average time per move
      // can be kept level in self-play.
      if (TmTransMul) {
        const RootLine &top = td.root[0];
        int caps = 0;
        for (int i = 0; i < std::min(top.pv_len, TmTransPlies); i++)
          caps += get_move_capture(top.pv[i]) != 0;
        const int pieces = count_bits(occupancies[both]);
        bool trans = net_phase(pieces - caps) != net_phase(pieces);
        // TmC2Trans (agent B, 21/10/2026; OFF by default, SPRT pending): with Consilium Plebis the experts are not the piece
        // bands above. 1 = the trigger is a change of the C2 expert of either perspective after the same PV moves
        // (captures and promotions); 2 = either trigger.
        if (TmC2Trans) {
          const bool c2 = c2_line_switches(td, top.pv, std::min(top.pv_len, TmTransPlies));
          trans = TmC2Trans == 1 ? c2 : (trans || c2);
#ifdef TRIUMV_TMDEBUG
          printf("info string tm c2trans d %d band %d c2 %d\n", td.root_depth,
                 (int)(net_phase(pieces - caps) != net_phase(pieces)), (int)c2);
#endif
        }
        total_time *= trans ? 1 + TmTransMul / 1000.0 : 1 - TmTransRest / 1000.0;
      }
      const int elapsed = get_time_ms() - g_limits.start_ms;
#ifdef TRIUMV_TMDEBUG   // time-management diagnostics (08/10/2026): the factors at every completed iteration
      printf("info string tm d %d el %d opt %d max %d fall %.3f red %.3f tr %.3f inst %.3f ch %.2f heavy %.3f "
             "effort %llu exp %d sur %d best %d prev %.0f iter %d tot %.0f\n",
             td.root_depth, elapsed, g_clock.optimum, g_clock.maximum, v.trend, v.settle, time_red, v.churn,
             g_clock.best_changes, v.focus, (unsigned long long)effort, (int)g_clock.expected, (int)g_clock.surprise, best,
             prev_avg, g_clock.iter_values[g_clock.iter_idx], total_time);
      fflush(stdout);
#endif
#ifdef TRIUMV_TMLOG
      const bool verdict_stop = elapsed > std::min(total_time, (double)g_clock.maximum);
      if (FILE *lf = tmlog_file()) {
        fprintf(lf, "I %ld %d %d %d %d %d %d %d %d %.0f %d %d %llu %.3f %d %.4f %.4f %.4f %.4f %.0f %d\n",
                g_tmlog_search, g_tmlog_ply, count_bits(occupancies[both]), td.root_depth, elapsed, g_clock.optimum,
                g_clock.maximum, td.root[0].pv[0], best, g_clock.have_prev ? prev_avg : 99999.0,
                g_clock.iter_values[g_clock.iter_idx], td.root_depth - last_best_depth, (unsigned long long)effort,
                g_clock.best_changes, num_threads, v.trend, v.settle, v.churn, v.focus, total_time, (int)verdict_stop);
        fflush(lf);
      }
      if (elapsed > std::min(total_time * (TRIUMV_TMLOG_EXT / 100.0), (double)g_clock.maximum) ||
          td.root[lines - 1].score >= mating_in(3) || td.root[0].score == mated_at(2)) {
#else
      if (elapsed > std::min(total_time, (double)g_clock.maximum) ||
          td.root[lines - 1].score >= mating_in(3) || td.root[0].score == mated_at(2)) {
#endif
        if (g_pondering.load(std::memory_order_relaxed))
          g_stop_at_ponderhit.store(true, std::memory_order_relaxed);
        else
          stop_threads.store(true, std::memory_order_relaxed);
      } else
        g_clock.increase_depth = g_pondering.load(std::memory_order_relaxed) || elapsed <= total_time * (TmDeepenShare / 1000.0);
    }
    g_clock.iter_values[g_clock.iter_idx] = best;
    g_clock.iter_idx = (g_clock.iter_idx + 1) & 3;
  }
  if (main)
    g_clock.prev_time_red = time_red;
#ifdef TRIUMV_TMLOG
  if (main && g_limits.use_tm())
    if (FILE *lf = tmlog_file()) {
      fprintf(lf, "E %ld %d %d %d %d\n", g_tmlog_search, td.root[0].pv[0], td.root[0].score, td.completed_depth,
              get_time_ms() - g_limits.start_ms);
      fflush(lf);
    }
#endif
}

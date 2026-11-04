// search/13_learn.inc -- What the search learns: every update of a move statistic and of the correction tables.
// Part of threads.cpp, included by 13_search.inc right after NodeState: NOT a translation unit of its own.
//
// C5a of the 8.1 plan (27/10/2026, docs/audit_8.1/PIANO_UNICO_8.1_2026-10-27.md; R10 item 4). The updates were spread
// over 09_history.inc and the four stages of the node; here they are one module of ours, written as events and
// amounts. An event is a fact the search has just established (this move cut, these moves failed before it, the
// evaluation moved by this much, the reduced move cleared alpha, ...). Its amount comes from the node (depth, expert,
// scores) and the levers of 01_params.inc, and each table the event reaches takes the amount through a scale of its
// own. Arithmetic and order of the updates are the previous ones, so the tree is identical (bench 279330 with the
// permuted ep486). The values move only in the SPSA blocks Q, O1 and O2.
//
//   event                                   amount                                       tables reached
//   cut by a quiet move (on_best_move)      reward(depth) x HlQBonus x HistGrad factor   quiet set
//   quiet moves tried before it             penalty(depth) x HlQMalus x HlQDecay^i       quiet set
//   cut by a capture                        reward x HlCBonus                            capture
//   captures tried before the best move     -penalty x HlCMalus                          capture
//   parent's move searched at once          -penalty x PrevContMalus                     parent's followups
//   hash cutoff (on_hash_cut)               HashCutQuiet amount; -HashCutPrevMalus       quiet set; parent's followups
//   nothing raised alpha, quiet prior move  prior amount x node scale                    parent's followups, main, pawn
//   nothing raised alpha, capture prior     PrCapt                                       capture
//   evaluation change (on_eval_change)      clamp(change) x EvalDiffMul x HlEval         opponent's main, pawn
//   reduced move cleared alpha (on_research) FollowFh*                                   followups
//   hash-move trust (on_node_outcome,       TtMoveHist*; SingFhTt*                       hash-move statistic
//   on_multi_cut)
//   evaluation error (on_eval_error,        (value - eval) x depth x CorrBonus*;         correction tables
//   on_multi_cut)                           CorrSing*
//
// The quiet set of a move: its main statistic (with the threat-context correction), the first-ply table, the
// followups (the continuation tables of the six moves before), the pawn-structure table and, with C2StructHistW,
// the structure table.

// The writer of one node: its thread, frame and expert. Built where an event happens; the gravity of the quiet and
// continuation statistics is read once for the expert.
struct Learner {
  ThreadData &td;
  NodeFrame *f;
  int ex;     // expert of the node (search_band)
  int grav;   // gravity of the quiet and continuation statistics for this expert

  Learner(ThreadData &t, NodeFrame *fr, int e) : td(t), f(fr), ex(e), grav(hist_grav(e)) {}

  // Followups: the move (pc, to) in the continuation table of each of the six moves before frame `at`. Every distance
  // has a weight (ContDist), the weight grows with the number of distances that already rate the move well (ContAgree),
  // near (1-2) and far (3-6) distances have a weight per expert, and distance 1 takes a fixed extra (ContNear). In
  // check only ContChk distances learn. ContDist and ContAgree are still the inherited values (lever block O2).
  void followups(NodeFrame *at, int pc, int to, int amount) const {
    const int near_w = by_exp4(ex, HlFNear0, HlFNear1, HlFNear2, HlFNear3);
    const int far_w = by_exp4(ex, HlFFar0, HlFFar1, HlFFar2, HlFFar3);
    // Per distance 1..6: its own weight, the weight of its group (near or far), and how many frames back to go.
    const int dist_w[7] = {0, ContDist1, ContDist2, ContDist3, ContDist4, ContDist5, ContDist6};
    const int group_w[7] = {0, near_w, near_w, far_w, far_w, far_w, far_w};
    const int agree_w[7] = {ContAgree0, ContAgree1, ContAgree2, ContAgree3, ContAgree4, ContAgree5, ContAgree6};
    const int near_extra = by_exp4(ex, ContNear0, ContNear1, ContNear2, ContNear3);
    const int reach = at->in_check ? by_exp4(ex, ContChk0, ContChk1, ContChk2, ContChk3) : 6;
    int rated_well = 0, d = 0;
    for (NodeFrame *back = at - 1; ++d <= reach; --back) {
      if (!move_ok(back->move))
        continue;
      int16_t &e = (*back->cont_hist)[pc][to];
      rated_well += e > 0;
      const int part = amount * dist_w[d] * agree_w[rated_well] / 65536 * group_w[d] / 1024;
      hist_learn(e, part + (d == 1) * near_extra, LIM_FOLLOW, grav);
    }
  }

  // The quiet set of move m of the side to move. The main statistic takes the amount itself; the other four take it
  // through their scale and the expert's auxiliary weight (HlAux); the pawn table has one scale for rewards and one for
  // penalties (split at PawnHistSplit).
  void quiet(int m, int amount, int ctx) const {
    const int key = quiet_key(m), pc = move_piece(m), to = move_to(m);
    const int aux = by_exp4(ex, HlAux0, HlAux1, HlAux2, HlAux3);
    qh_push(td, td.side, key, ctx, amount, grav);
    if (f->ply < LOW_PLY_SIZE)
      hist_learn(td.low_ply_hist[f->ply][key], amount * LowPlyScale / 1024 * aux / 1024, LIM_QUIET, grav);
    followups(f, pc, to, amount * ContUpdScale / 1024 * aux / 1024);
    const int pawn_scale = amount > PawnHistSplit ? PawnBonusScale : PawnMalusScale;
    hist_learn(pawn_row(td)[pc][to], amount * pawn_scale / 1024 * aux / 1024, LIM_PAWN, grav);
    if (C2StructHistW)   // C2 ordering idea 6: the structure table learns only while the ordering reads it
      if (PieceToRow *row = c2_struct_row(td))
        hist_learn((*row)[pc][to], amount * C2StructHistUpd / 1024 * aux / 1024, LIM_PAWN, grav);
  }

  // Capture statistic of move m (piece, target square, victim), with the capture gravity of the expert.
  void capture(int m, int amount) const {
    hist_learn(td.capt_hist[move_piece(m)][move_to(m)][victim_kind(td, m)], amount, LIM_CAPT, hist_cgrav(ex));
  }

  // Correction tables of the node: the structure keys (pawns, minor pieces, non-pawns of each colour), the pairs of
  // moves that led here and the optional keys, each through its update scale (/128), with the expert's correction
  // gravity (CorrGrav). The keys are the ones corr_signal reads (09_history.inc).
  void corrections(int amount) const {
    PROF_GUARD(prof_corr);
    const int us = td.side, g = corr_grav(ex);
    const U64 phk = corr_phase_key(td), fk = corr_fifty_key(td);
    const U64 kp = td.pawn_key ^ phk ^ fk, km = td.mm_key[0] ^ phk, kw = td.np_key[white] ^ fk, kb = td.np_key[black] ^ fk;
    hist_learn(corr_slot(td, kp, us).pawn, amount * CorrPawnUpd / 128, LIM_CORR, g);
    hist_learn(corr_slot(td, km, us).minor, amount * CorrMinorUpd / 128, LIM_CORR, g);
    hist_learn(corr_slot(td, kw, us).np_white, amount * CorrNonPawnUpd / 128, LIM_CORR, g);
    hist_learn(corr_slot(td, kb, us).np_black, amount * CorrNonPawnUpd / 128, LIM_CORR, g);
    U64 k1, k2;
    if (CorrPairCont && corr_pair_keys(td, k1, k2)) {   // CorrPairCont: the two position pairs, in corr_x
      auto *cx = static_cast<int16_t (*)[2]>(td.st_corr_x);
      hist_learn(cx[k1 & td.st_corr_mask][us], amount * CorrCont2Upd / 128, LIM_CORR, g);
      hist_learn(cx[k2 & td.st_corr_mask][us], amount * CorrCont2Upd / 128, LIM_CORR, g);
    }
    if (move_ok(f[-1].move)) {
      const int to = move_to(f[-1].move), pc = td.piece_on[to];
      hist_learn((*f[-2].cont_corr)[pc][to], amount * CorrCont2Upd / 128, LIM_CORR, g);
      hist_learn((*f[-4].cont_corr)[pc][to], amount * CorrCont4Upd / 128, LIM_CORR, g);
      if (CorrCont6W)
        hist_learn((*f[-6].cont_corr)[pc][to], amount * CorrCont6Upd / 128, LIM_CORR, g);
      if (CorrContOppW)
        hist_learn((*f[-3].cont_corr)[pc][to], amount * CorrContOppUpd / 128, LIM_CORR, g);
    }
    if (CorrExpertW)
      hist_learn(static_cast<int16_t (*)[2]>(td.st_corr_x)[corr_expert_key(td) & td.st_corr_mask][us], amount * CorrExpertUpd / 128, LIM_CORR, g);
    U64 pk_key;   // C2 corrections (agent B)
    if (CorrPkW && c2_pk_key(td, pk_key))
      hist_learn(static_cast<int16_t (*)[2]>(td.st_corr_x)[pk_key & td.st_corr_mask][us], amount * CorrPkUpd / 128, LIM_CORR, g);
  }

  // Hash-move statistic: how far the hash move can be trusted outside the PV.
  void hash_move_trust(int amount) const { hist_learn(td.tt_move_hist, amount, LIM_TTMOVE, ttm_grav(ex)); }
};

// ----------------------------------------------------------------------------
// The amounts of a node that found its best move: the reward by depth (with the hash-move and parent-score terms,
// and outside the PV a share per move tried) and the penalty by depth.
// ----------------------------------------------------------------------------
static inline int best_reward(const NodeFrame *f, const NodeState &n) {
  int r = hist_amount(n.expert, n.depth, BonusMul, BonusSub, BonusMax) + (n.best_move == n.h.move ? BonusTTMove : 0) +
          f[-1].stat_score / BonusPrevStatDiv;
  if (!n.pv)
    r += (int)((long long)r * (n.n_quiets + n.n_tacts) / BonusTriedDiv);
  return r;
}
static inline int fail_penalty(const NodeState &n) {
  return hist_amount(n.expert, n.depth, MalusMul, MalusSub, MalusMax);
}

// HistGrad (22/10/2026, ours, HistGradMix 0 = off): the factors of a listwise ranker for the best quiet move and the
// quiet moves tried before it. Their ordering scores give a softmax p (temperature HistGradT, in score units); the
// gradient of the log-loss of "the best move is the right one" is 1 - p for the best move and -p_i for each other
// move. A best move the ordering already expected learns little, a surprising one a lot; a tried move rated low by the
// ordering is barely punished, one rated high in full. HistGradMix blends this with the fixed amounts (1024 = full);
// the factors average 1 over a uniform p, so the size of the update is kept. Factors /1024, clamped to [0, 3072].
static inline int hist_grad_factors(const NodeState &n, int fq[TRACKED]) {
  for (int i = 0; i < n.n_quiets; i++)
    fq[i] = 1024;
  if (!HistGradMix || n.best_score == INT_MIN)
    return 1024;
  const float t = 1.0f / std::max(HistGradT, 1);
  int top = n.best_score, ranked = 1;
  for (int i = 0; i < n.n_quiets; i++)
    if (n.quiet_score[i] != INT_MIN)
      top = std::max(top, n.quiet_score[i]), ranked++;
  float w[TRACKED], z = std::exp((n.best_score - top) * t);
  const float w_best = z;
  for (int i = 0; i < n.n_quiets; i++)
    z += (w[i] = n.quiet_score[i] != INT_MIN ? std::exp((n.quiet_score[i] - top) * t) : 0.0f);
  for (int i = 0; i < n.n_quiets; i++)
    if (n.quiet_score[i] != INT_MIN)
      fq[i] = std::clamp(1024 + int(HistGradMix * (ranked * w[i] / z - 1.0f)), 0, 3072);
  return std::clamp(1024 + int(HistGradMix * (2.0f * (1.0f - w_best / z) - 1.0f)), 0, 3072);
}

// ----------------------------------------------------------------------------
// Events
// ----------------------------------------------------------------------------

// The node found its best move. A quiet best move is rewarded in its quiet set and each quiet move tried before it is
// penalised, the penalty decaying by HlQDecay per move; a capture is rewarded in the capture statistic. The parent's
// quiet move, searched at once (first move, or second after the hash move), was weak. Captures tried before the best
// move are penalised last. Per expert (HlQ*, HlC*): in an endgame a quiet move that cuts among few alternatives says
// more than one among forty in the opening.
static void on_best_move(const Learner &L, const NodeState &n) {
  PROF_GUARD(prof_hist);
  const int bm = n.best_move, reward = best_reward(L.f, n), penalty = fail_penalty(n);
  if (!is_tactical(bm)) {
    int fq[TRACKED];
    const int fb = hist_grad_factors(n, fq);
    L.quiet(bm, reward * by_exp4(n.expert, HlQBonus0, HlQBonus1, HlQBonus2, HlQBonus3) / 1024 * fb / 1024, n.best_ctx);
    const int decay = by_exp4(n.expert, HlQDecay0, HlQDecay1, HlQDecay2, HlQDecay3);
    int p = penalty * by_exp4(n.expert, HlQMalus0, HlQMalus1, HlQMalus2, HlQMalus3) / 1024;
    for (int i = 0; i < n.n_quiets; i++) {
      p = p * decay / 1024;
      L.quiet(n.quiets[i], -p * fq[i] / 1024, n.quiet_ctx[i]);
    }
  } else
    L.capture(bm, reward * by_exp4(n.expert, HlCBonus0, HlCBonus1, HlCBonus2, HlCBonus3) / 1024);

  if (n.prev_to >= 0 && L.f[-1].move_count == 1 + (L.f[-1].tt_hit ? 1 : 0) && !n.prior_capture)
    L.followups(L.f - 1, L.td.piece_on[n.prev_to], n.prev_to, -penalty * PrevContMalus / 1024);

  const int capt_penalty = by_exp4(n.expert, HlCMalus0, HlCMalus1, HlCMalus2, HlCMalus3);
  for (int i = 0; i < n.n_tacts; i++)
    L.capture(n.tacts[i], -penalty * capt_penalty / 1024);
}

// Nothing raised alpha and the opponent's previous move was quiet, so it was good: its followups, main and pawn
// statistics are rewarded by a scale built from the node (the move's own statistic, depth, how late it came, how far
// the result fell below the two static evaluations). Per expert: the move number from which the previous move counts
// as late (PrLate) and a gain on the scale (PrGain, /1024).
static void on_prior_refuted(const Learner &L, const NodeState &n) {
  const NodeFrame *f = L.f;
  const bool late = f[-1].move_count > by_exp4(n.expert, PrLate0, PrLate1, PrLate2, PrLate3);
  const bool below_ours = !n.in_check && n.best <= f->static_eval - PriorScaleEvalMargin;
  const bool below_theirs = !f[-1].in_check && n.best <= -f[-1].static_eval - PriorScaleOppMargin;
  int scale = std::min(PriorScaleDepthMul * n.depth, PriorScaleDepthMax) - PriorScaleBase -
              f[-1].stat_score / PriorScaleStatDiv + PriorScaleLate * late + PriorScaleEval * below_ours +
              PriorScaleOppEval * below_theirs;
  scale = std::max(scale, 0) * by_exp4(n.expert, PrGain0, PrGain1, PrGain2, PrGain3) / 1024;
  const int amount = hist_amount(n.expert, n.depth, PriorBonusDepthMul, PriorBonusSub, PriorBonusMax) * scale;
  const int prev = f[-1].move, pc = L.td.piece_on[n.prev_to];
  L.followups(L.f - 1, pc, n.prev_to, amount * PriorContW / 16384);
  qh_push(L.td, L.td.side ^ 1, quiet_key(prev), f[-1].hctx, amount * PriorQuietW / 32768, L.grav);
  if (pc % 6 != P && !get_move_promoted(prev))
    hist_learn(pawn_row(L.td)[pc][n.prev_to], amount * PriorPawnW / 8192, LIM_PAWN, L.grav);
}

// The outcome of a node that searched moves: the best move and the ones tried before it, with the hash-move trust
// outside the PV; with no best move, the opponent's previous move, quiet (on_prior_refuted) or a capture (its capture
// statistic, PrCapt per expert).
static void on_node_outcome(ThreadData &td, NodeFrame *f, const NodeState &n) {
  const Learner L(td, f, n.expert);
  if (n.best_move) {
    on_best_move(L, n);
    if (!n.pv)
      L.hash_move_trust(n.best_move == n.h.move ? TtMoveHistGood : -TtMoveHistBad);
    return;
  }
  if (n.prev_to < 0)
    return;
  if (!n.prior_capture)
    on_prior_refuted(L, n);
  else
    hist_learn(td.capt_hist[td.piece_on[n.prev_to]][n.prev_to][f[-1].captured % 6],
               by_exp4(n.expert, PrCapt0, PrCapt1, PrCapt2, PrCapt3), LIM_CAPT, hist_cgrav(n.expert));
}

// A hash cutoff above beta with a hash move: a quiet hash move is rewarded in its quiet set, and the parent's quiet
// move, if it came early (before HashPrevLate), takes a fixed penalty in its followups.
static void on_hash_cut(ThreadData &td, NodeFrame *f, const NodeState &n) {
  const Learner L(td, f, n.expert);
  const int hm = n.h.move;
  // HashCutEarlyParent (R19 B2.c; 0 = always): the quiet hash move is rewarded only when the parent's move
  // came early in its list, where a cutoff below it was expected and the reward is not a fluke of a late move.
  if (!n.tact_hash && (!HashCutEarlyParent || f[-1].move_count < HashCutEarlyParent))
    L.quiet(hm, hist_amount(n.expert, n.depth, HashCutQuietMul, 0, HashCutQuietMax), quiet_ctx_slow(td, hm));
  if (n.prev_to >= 0 && f[-1].move_count < HashPrevLate && !n.prior_capture)
    L.followups(f - 1, td.piece_on[n.prev_to], n.prev_to, -HashCutPrevMalus);
}

// The static evaluation changed since the previous ply: the change, clamped and offset, teaches the opponent's quiet
// move that led here (main statistic, and the pawn table when the node had no hash entry). Weight per expert (HlEval);
// EvPen scales the penalties (negative change) apart from the rewards.
static void on_eval_change(ThreadData &td, NodeFrame *f, const NodeState &n) {
  if (!move_ok(f[-1].move) || f[-1].in_check || n.prior_capture)
    return;
  // EvalDiffShallow (R19 B2.b; 0 = always): deep nodes with a hash entry learn it only below that depth.
  if (EvalDiffShallow && n.depth >= EvalDiffShallow && n.h.hit)
    return;
  const Learner L(td, f, n.expert);
  const int change = std::clamp(-(f[-1].static_eval + f->static_eval), -EvalDiffLo, EvalDiffHi) + EvalDiffOfs;
  const int pm = f[-1].move;
  int w = by_exp4(n.expert, HlEval0, HlEval1, HlEval2, HlEval3);
  if (change < 0)
    w = w * by_exp4(n.expert, EvPen0, EvPen1, EvPen2, EvPen3) / 1024;
  qh_push(td, td.side ^ 1, quiet_key(pm), f[-1].hctx, change * EvalDiffMul * w / 1024, L.grav);
  const int pc = td.piece_on[n.prev_to];
  if (!n.h.hit && pc % 6 != P && !get_move_promoted(pm))
    hist_learn(pawn_row(td)[pc][n.prev_to], change * EvalDiffPawnMul * w / 1024, LIM_PAWN, L.grav);
}

// A reduced move cleared alpha and was searched again (ours, 19/10/2026): confirmed, it earns FollowFhBonus plus
// FollowFhRed per ply of reduction it overcame (up to 8), since a move that beat a deep reduction was badly placed in
// the ordering; refuted by the re-search, it earns FollowFhMiss. Followups only.
static void on_research(ThreadData &td, NodeFrame *f, const NodeState &n, int pc, int to, bool confirmed,
                        int overcome) {
  const Learner L(td, f, n.expert);
  L.followups(f, pc, to, confirmed ? FollowFhBonus + FollowFhRed * std::min(overcome, 8) : FollowFhMiss);
}

// The singular test found several moves above beta at reduced depth (value v at depth s_depth): the node returns,
// the hash move loses trust, and if the value is above the static evaluation the correction tables learn the gap.
static void on_multi_cut(ThreadData &td, NodeFrame *f, const NodeState &n, int v, int s_depth) {
  const Learner L(td, f, n.expert);
  L.hash_move_trust(-SingFhTtBase - SingFhTtDepth * n.depth);
  if (!n.in_check && v > f->static_eval)
    L.corrections(std::clamp((v - f->static_eval) * s_depth * CorrSingMul / 1024, -CorrSingMax, CorrSingMax));
}

// End of the node: the error of the static evaluation against the searched value teaches the correction tables.
// Depth enters as d + q*d*d/64 with q per expert (CorrDepthQ; 0 = linear: x * 64d * k / (128 * 64) truncates like
// x * d * k / 128); the scale differs with and without a best move (CorrBonusHi/Lo) and the amount is clamped.
static void on_eval_error(ThreadData &td, NodeFrame *f, const NodeState &n) {
  const long long dq =
      64LL * n.depth + (long long)by_exp4(n.expert, CorrDepthQ0, CorrDepthQ1, CorrDepthQ2, CorrDepthQ3) * n.depth * n.depth;
  const int amount =
      (int)std::clamp((long long)(n.best - f->static_eval) * dq * (n.best_move ? CorrBonusHi : CorrBonusLo) / 8192,
                      (long long)-CorrBonusMaxLo, (long long)CorrBonusMaxHi);
  Learner(td, f, n.expert).corrections(CorrLearnW * amount / 1024);
}

// The correction tables learn the gap to a value the node did not search but knows (R19, CorrRepLearn: the draw of a
// reachable repetition), with the same depth term and the fail-low scale of on_eval_error, weighted by w / 1024.
static void on_eval_toward(ThreadData &td, NodeFrame *f, const NodeState &n, int target, int w) {
  const long long dq =
      64LL * n.depth + (long long)by_exp4(n.expert, CorrDepthQ0, CorrDepthQ1, CorrDepthQ2, CorrDepthQ3) * n.depth * n.depth;
  const int amount = (int)std::clamp((long long)(target - f->static_eval) * dq * CorrBonusLo / 8192,
                                     (long long)-CorrBonusMaxLo, (long long)CorrBonusMaxHi);
  Learner(td, f, n.expert).corrections((int)((long long)CorrLearnW * amount / 1024 * w / 1024));
}

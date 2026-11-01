// search/01_params.inc -- Search parameters and the globals shared with the other units.
//
// BAKE P2 (01/11/2026, author): the history divisor in our form replaces the inherited 16-entry table (HistDivForm
// 0->1; table and switch removed the same day, bench same) and the SPSA of block P2 (pruning of single moves, 22
// levers, 15+0.15 without HT, lab aligned with fishtest) stopped at iteration 1229, tail mean of the trend:
// QuietFutBase 160->162, QuietFutQuad 0->1, QuietFutDepth 96->92, QuietFutAlpha 90->94, QuietHistPrune 4045->3860,
// QuietHistBase 0->-277, QuietMainHistMul 69->68, QuietSeeLin 0->4, CaptFutBase 190->182, CaptFutDepth 281->284,
// CaptFutHist 134->135, CaptFutVictim 1024->1047, CaptSeeDepth 179->176, CaptSeeHist 34->35, CaptSeeQuad 0->-1, LmpLin
// 0->-3, LmpNotImp 512->482, HistDivMid 2960->3113, HistDivCurve 364->355, HistDivSlope 0->22. SPRT of the vector
// against the previous values, both sides with the form, UHO, [-1, 2], three parts pooled (15+0.15, 8+0.08, 15+0.15):
// +1.71 +- 3.78 / 7,734 (socket 0 -1.80 +- 5.33, socket 1 +5.21 +- 5.37), LLR +0.58. Adopted to remove the inherited
// constants with no loss shown; the package against the previous code (table and old values) was stopped by the
// author at 422 games (-0.82 +- 15.06): the form had been chosen to depart from the table. Backup _pre_bake_p2_2026-11-01.
//
// BAKE I (31/10/2026, author): the SPSA of block I (aspiration windows and optimism, 5 levers, 8+0.08 without HT), the
// first run with the lab aligned with fishtest (stochastic rounding, lr 0.032), paused at iteration 616: AspGrowLo
// 51->50, AspGrowHi 51->52, AspMeanSqDiv 10193->10050, OptStrength 182->183, OptDiv 288->284. SPRT against the previous
// defaults, 8+0.08 without HT, UHO, [-1, 2], three parts pooled (PC reboot, a pause for another test): +1.83 +- 2.78
// over 15,006 games (socket 0 +1.90, socket 1 +1.76; LLR +1.18 on [-1, 2], +2.32 on the non-regression bounds [-3, 1]),
// stopped by the author at 15,000. A lean, to be reconfirmed at a long time control. Backup _pre_bake_i_2026-10-31.
//
// Part of threads.cpp: NOT a compilation unit of its own. Included only by threads.cpp.
//
// BLOCK 4 LADDER (19/10/2026 night, user: "fully our own"): the extra plies of the singular extension become a ladder,
// a first rung (the old Double threshold) and a step with its own weights (SingStep*, from the old Triple threshold
// minus the Double one), up to SingRungs rungs; SingStepDepth (step per depth) and a third rung are ours, off. The
// divisor of the tt-move history term goes from 114178 to 2^17 (SingTtHistMul 1194 -> 1371). Bench unchanged 209655
// (MinGW, clean build); SingRungs=3 166809, SingStepDepth=16 271925. Backup _pre_step_b4_2026-10-19 (search/ only).
//
// BAKE BLOCK 4 S4 (19/10/2026 evening, user): the SPSA S4 of block 4 (30 levers, 10+0.1 without HT, stopped at about
// iteration 1,450) gives its vector: 18 levers that moved (SingMarginBase 73->76, SingMarginTtPv 66->70,
// SingDoubleBase -2->-4, SingDoublePv 225->215, SingDoubleQuiet 152->145, SingTripleBase 60->62, SingTriplePv
// 279->294, SingTripleQuiet 188->183, SingTripleDeep 43->47, SingCorrDiv 198368->186725, SingFhTtBase 421->404,
// CorrSingMul 177->180, SingNegHigh 3->4, and the terms of our own SingExp1 -1, SingExp2 3, SingMoving 2,
// SingLvlExp0 1, SingLvlExp1 1), and the small moves of six that stayed near their start (SingMarginDiv 63->64,
// SingDepthMul 1024->1026, SingDoubleDeep 38->39, SingTtHistMul 1175->1194, SingTripleTtPv 81->80, SingFhTtDepth
// 110->109). Against the previous values, pooled (10+0.1 without HT and 8+0.08 with HT): +0.22 +- 2.89 Elo over
// 14,266 games, level. Still the numbers of Stockfish 19: SingNegCut 3, SingMinDepth 6 (+1 on tt_pv), SingHashSlack 3,
// SingTtHistDiv 114178. Backup _pre_bake_s4_2026-10-19.
// UNBAKE BLOCK 2 (19/10/2026 midday, user): the reduction of our own with RfLearn, baked in the morning (parity against
// the formula of 8.0 at 8+0.08 with HT and 2+0.02), lost on a Zen 4 at long time controls against 8.0 with both builds
// universal (-4.43 +- 7.35 over 1,962 games at 12+0.12; the dev build -5.35 +- 4.37 over 5,520 at 15+0.15), while the
// rest of 8.1 is the tree of 8.0 and speed. Block 2 goes back to the formula of 8.0 and stays on hold; RedForm and
// RfLearn remain as options, off. Bench 126352. Backup of the baked state: _pre_unbake_blocco2_2026-10-19.
// RESTORE BLOCK 2 (19/10/2026, user): block 2 goes back to the tree of 8.0. 8.1 against 8.0 lost at every long time
// control (10+0.1 without HT -4.5 +- 9.7 over 1,312 after the block 1 revert) and searched 0.35 ply less deep at the
// same time per move. The reduction of our own (RedForm) and the re-search rule of our own (ResOwn) stay in the code
// as options, off, with their fitted values, to be measured again one at a time; RfVolatile stays at 0. The previous
// formula, its parameters and the old margins are back from _pre_bake_redform_2026-10-18. Default tree: that of 8.0
// with the promotion gain in the exchange evaluation (bench 126352, 137419 without it). Backup of the source before
// this change: _pre_ripristino_blocco2_2026-10-19. The notes below up to BAKE BLOCK 2 REDUCTION describe the bakes
// this restore undoes.
// BAKE RESOWN (19/10/2026, user): the re-search depth after a reduced move fails high is ours (margin over alpha
// against the uncertainty of the node, ResU0..ResKm, fit on 9.2 million moves) and replaces the margins on value - best
// (DeeperMargin 51, ShallowerMargin 8, removed with the option ResOwn). SPRT ResOwn=1 against the old rule at 10+0.1
// and 6+0.06 without HT and 8+0.08 with HT, pooled: +0.22 +- 4.42 Elo over 6,238 games, accepted as level. Bench 223002 (before
// 141759; the tm80 binary with ResOwn=1 gives the same 223002). Backup _pre_bake_resown_2026-10-19.
// BLOCK 2 REFIT (18/10/2026): the reduction fitted again on the engine with block 1 reverted (26.5 million moves);
// against the old formula +0.32 +- 5.20 over 4,278 games (10+0.1 and 6+0.06 without HT, closed as level). Bench 141759.
// REVERT BLOCK 1 TUNING (18/10/2026, user): the block 1 values go back to those that reproduce the tree of 8.0 (the
// code stays ours). At long time controls 8.1 lost to 8.0 (20+0.2 without HT -7.7 +- 6.3 / 2,848; on a second machine
// at 12+0.12 -10.2), and the build with the old reduction formula still lost (-5.9 +- 5.4 / 4,120 at 12+0.12): the
// rewrites are byte-identical, so the loss came from the tuned values, chosen with a selection rule that also passes
// random walks. Reverted: the 24 levers of the BAKE BLOCK 1 below, ThrEsc/ThrEnt 25 -> 20, ThreatHist 3 -> 0 (weights
// 1024). The promotion gain in the exchange evaluation (Bastiii) stays. Backup _pre_revert_blocco1_2026-10-18.
// UNBAKE RFVOLATILE (19/10/2026, user): back to 0, the lean below was never confirmed by an SPRT; back in the queue. Bench 463725
// (before 223002; the tm80 binary with ResOwn=1 RfVolatile=0 gives the same).
// BAKE RFVOLATILE (18/10/2026, user, provisional until the block 2 SPSA): RfVolatile 0 -> -8, late moves reduced
// less where the hash value is far from the corrected static evaluation (13_search.inc, red_final_ours). +1.70 +- 3.62
// Elo over 8,808 games (12+0.12 with HT and 8+0.08 without, pooled); -16 was level (+0.78 +- 6.88 at 4+0.04, about
// 2,500 games; -1.0 in a package with ChkDisc). Bench 208468 (before 268168).
// BAKE BLOCK 2 REDUCTION (18/10/2026, user): the late-move reduction of our own (power law per expert, trust factor,
// node and move facts, all-node scale; values from the fit on 20.7 million moves) replaces the previous formula. Tests
// of the fitted form against the previous one, pooled: +0.51 +- 4.03 Elo over 7,542 games (8+0.08 and 12+0.12, HT).
// Removed with it: the log table and LmrLogMul, LmrBase, LmrNotImproving, LmrDeltaMul, LmrTtPv*, LmrOffset,
// LmrMoveCount, LmrCorrDiv, LmrCutNode, LmrCutNoTT, LmrTTCapture, LmrCutoff*, LmrTTMove, LmrStatDiv, LmrAllNode,
// LmrAllDivBase, LmrExp0-3, LmrScaleExp0-3 and their Lt offsets. Backup _backup/Triumviratus_8.0_pre_bake_redform_2026-10-18.
// BAKE BLOCK 1 (17/10/2026 evening, user): the values of our own for block 1 of 8.1. ThreatHist 0 -> 3 with ThreatCtxW
// 1156 and ThreatCtxUpd 1083 (provisional, own SPRT at the end of the queue), ThrEsc 20 -> 25/25/25/27, ThrEnt 20 -> 25,
// and the 24 "safe" levers that moved in the SPSA runs (movement >= 0.25 c and |z| >= 1 or monotonic, low or moderate
// noise risk by simulation; CorrDepthQ and CorrGain excluded as a likely artefact of the schedule): HeCont3 1280,
// HlCMalus0 1280, HtVict0 7001, HtVict1 6015, HmPawn0 2279, HmPawn2 2007, HmFar1/2/3 1028/1005/1158, HmNear3 1154,
// HmMain1 2049, HlFNear2 821, HlFFar3 879, CorrGrav1 1061, HlDepthQ1 -1, HlQMalus2 847, HlQMalus3 941, HlQDecay1 922,
// StGood1 1138, PrCapt0 807, HlEval3 1148, LpDecay1 1122, PrLate2 8. SPRT of the 24 levers (same threat structure on
// both sides) +1.47 +- 4.05 over 7,792 at 8+0.08 HT; closing SPRT at 2+0.05 running. Bench 126352 -> 280323.
// Backup _backup/Triumviratus_8.0_pre_bake_blocco1_2026-10-17.
//
// BAKE EXP1 (12/10/2026 afternoon, user: "let us bake it, recording the idea in NOVELTIES; the next SPSA continues the
// current tuning"): search margins per expert of the network (RfpExp/FutExp/LmrExp 0-3, bands of net_phase) at the
// vector of the SPSA EXP1, mean of iterations 5687-6314: RfpExp 3/1/-3/3, FutExp -3/-14/-2/8, LmrExp -25/102/-6/101.
// Three SPRTs of the vector against the defaults, pooled: +1.64 +- 4.27 Elo over 6,562 games (half SPSA 10+0.1 HT
// +2.47; end SPSA 20+0.2 HT tm35 +1.53 +- 6.00 / 3,176; tm40 10+0.1 without HT +0.89 +- 8.58 / ~1,400). The SPSA ran
// with hyperthreading and the old clock: TO BE REDONE from these values (one engine per core). The options stay.
// Fixed-depth tree -2/-6% (84 positions, D10-D20). Bench 221809 (before 325747).
// BAKE TIME IN DEFENCE (11/10/2026 evening, user: "I think it is acceptable now"): TmDefendMul 0 -> 500 (time of the
// move x1.5 with the root between -TmDefendTo and -TmDefendFrom, i.e. defending but not lost) and TmEarlySave 0 -> 200
// (target share -20% with >= 24 pieces), the two levers of audit X3 (defence with a short clock), together. SPRT UHO 40+0.4, tm30 (bench 325747), TmFenPly=1 on both sides: +7.27 +- 7.08 Elo over
// 2,150 games (LLR +0.73, zero excluded; socket 0 +5.17, socket 1 +9.38). Bench unchanged (time management). To be
// split later (which of the two gains) and reviewed at a longer TC: the slower socket gained more.
// BAKE NEGEXTMAX (11/10/2026 morning, user): a cap of ONE extension from a negative reduction per line (13_search.inc,
// move_loop; NodeFrame::negext/negext_path), option retired. SPRT UHO 12+0.12 (a depth lever), tm29, two runs pooled
// (negext1b_12 + negext1c_12): +3.88 +- 3.26 Elo over 11,384 games (LLR +1.72, zero excluded; socket 0 +3.05,
// socket 1 +4.71), stopped by the user to build the release of 11/10 ("let us bake it and record it"). First run
// +3.59 +- 4.10 over 7,052. Value 1 because it is the only cap that clearly changes the fixed-depth tree (08/10: -6.6%
// nodes, -0.55 ply of seldepth; from 2 upward the cap is rarely reached). Bench 325747 (before 460041).
// To be checked at long TC in the A/B of the release 20261011 against the public 20261010 (30 s / 20 s).
// BAKE INSUFMATERIAL (11/10/2026 night, user): draw by insufficient material after a capture (idea from Reckless,
// rule also in Stormphrax, Berserk, Viridithas, Caissa, Coda; our code, rewritten on 15/10; agent AE, §42).
// SPRT 8+0.08 on the endgame book endgame_12_18 without draw adjudication, tm28, two runs pooled (aeinsuf_eg_8
// + aeinsuf_eg_8b): +1.13 +- 1.56 Elo over 17,162 games (LLR +0.97; socket 0 +0.77, socket 1 +1.50). First run
// +1.47 +- 2.39 over 7,000, then the user: "another 10,000 and then bake". At fixed depth the tree does not grow (nodes
// -0.1% on 200 endgames of the book at depth 15, +2.2% on 164 endgames at depth 16 with median 0, -0.9% on the reference
// bench at depth 14). Bench 460041 (before 297400).
// BAKE REPCYCLEOTHER (10/10/2026 evening, user): the "other" filter in the cycle test (td_upcoming_repetition,
// 10_order.inc), option retired. SPRT 8+0.08 UHO with hyperthreading, tm26: +5.62 +- 4.47 Elo over 6,500 games (LLR
// +1.48, zero excluded; socket 0 +9.72, socket 1 +1.49), stopped by the user. Bench 297400 (before 222811).
// BAKE CAUSALRED (10/10/2026, user): CausalRed 0 -> 512 (the user's idea of June, "causal pruning"). SPRT at 25+0.25
// UHO, two runs pooled (tags causal512 + causal512b): +4.27 +- 4.13 Elo over 6,594 games, LLR +1.17, mean depth of
// the candidate +0.25 ply on both sockets. Stopped by the user before 20,000 games: the measurement is enough because
// zero is excluded, and the closing SPSAs retune the value (CausalRed among the levers). Bench 222811 (before 269775).
// BAKE PR4 (09/10/2026, user): 46 pruning, extension and ordering levers from the SPSA PR4 (20+0.2, stopped at
// iteration 6000), mean of iterations 5703-6002. SPRT of the vector against the defaults: every test of the cross
// (20+0.2 HT, 15+0.15 and 10+0.1 on the cores only, 30+0.3 HT with UHO +110/+129) positive, socket 0 more than socket 1.
// It turns on the ordering with chess knowledge (KingShield, Outpost*, AttackOrder*, PassedPush, PromoOrder,
// BishopPairCapt, 0 before). Values old->new:
//  BonusMul 139->140, BonusMax 1565->1615, BonusTTMove 371->343, MalusMul 911->853, MalusMax 2186->2156,
//  QuietBonusScale 798->860, QuietMalusScale 1031->1043, CaptBonusScale 1515->1368, CaptMalusScale 1370->1270,
//  ContUpdScale 784->874, GoodQuietLimit -14491->-15563, QuietSortDepthMul 3487->3223, CheckOrderBonus 15142->13323,
//  GoodCaptSeeDiv 18->19, RazorMul 442->452, RfpBetaWeight 712->690, NmpDepthMul 14->15, NmpBase 314->302,
//  NmpEvalDiv 235->223, ProbCutMargin 231->209, ProbCutTTMargin 400->415, CaptFutBase 177->190,
//  CaptFutDepth 279->281, CaptSeeDepth 186->179, QuietHistPrune 3910->4045, QuietFutDepth 122->96,
//  QuietFutBase 166->160, QuietSeeMul 25->24, QsFutMargin 285->312, QsSeeMargin 80->74, SingDoublePv 217->225,
//  SingTripleBase 67->60, LmrTtPvSub 3227->2915, LmrTTMove 2082->2037, DeeperMargin 50->51, LdseMargin 80->76,
//  AspBase 5->6, AspGrow 48->51, KingShield 0->5371, OutpostOrder 0->4179, OutpostOrderB 0->1078,
//  AttackOrder 0->5807, AttackOrderQ 0->6371, PassedPush 0->3975, PromoOrder 0->2412, BishopPairCapt 0->601
// BAKE RW1 (04/10/2026): 49 values are the mean of iterations 4481-5481 of the SPSA "RW1" (51 levers, 20+0.2,
// Tuning_SPSA/spsa_prune/vettore_rw1_it5481.json), played by our engine with Consilium; PassedPushRed (ours) enters
// on at 1125. Gate SPRT against the starting values at 10+0.1: +9.67 +- 9.38 (LLR 0.61, stopped and accepted by the
// user). Bench 141196 (before 172833). Outside the SPSA, hence still at their starting values: the time management,
// the depth thresholds and LmrLogMul.
// BAKE LDSE (05/10/2026): LdseMargin 0 -> 80, LdseMode 0 -> 1 (extension of the hash move at low depth, cut nodes
// only; 13_search.inc, move_loop). SPRT at 10+0.1 on the binary t80_d1005, UHO book, 40/30 games per socket: +4.52
// +- 3.34 over 11,604 games (LLR 2.04 on [-2.94, 2.94], nElo [0, 3]); socket 0 (4 RAM modules, faster and deeper)
// +6.8 +- 4.4, socket 1 (2 modules) +1.5 +- 5.1: it seems to gain more with more depth. Stopped and baked on the
// user's decision before 20,000 games. Bench 498873 (before 141196; with LdseMargin=0 back to 141196): the extension
// near the horizon widens fixed-depth trees a lot. On endgames (endgame_12_18 book, 10+0.1) Ldse off against on
// -0.88 +- 4.05 over 2,786 games: not harmful.
// BAKE GUARD (05/10/2026): LdseMax 0 -> 1, at most one Ldse extension per line (NodeFrame::ldse_path). In rook endings
// the extension repeated along the line (tree x5-x16 at fixed depth). SPRT against Ldse without a cap at 15+0.15,
// UHO: +6.25 +- 7.89 over 2,112 games (LLR 0.54; socket 0 +7.4, socket 1 +4.6), stopped and baked on the user's
// decision. Bench 308883 (with LdseMax=0 back to 498873).
// BAKE SURPRISE (05/10/2026): TmSurpriseScale 1000 -> 1200 (time of the move x1.2 when the opponent did not play the
// reply of our principal variation; 14_deepen.inc). SPRT at 40+0.4, UHO, seed 7200: +6.23 +- 6.75 over 2,174 games
// (LLR 0.65, LOS ~96%; socket 0 +6.2, socket 1 +6.3), stopped and baked on the user's decision. From the PGNs: on the
// moves after a surprise (~26%) +0.35-0.4 ply. At 16+0.16 it was neutral (+0.7): it depends on the TC (see the
// parameter). The bench (fixed depth) does not use the clock: it stays 308883.
// BAKE HASHQSDEPTH (06/10/2026): HashQsDepth 0 -> 1 (the quiescence writes and reads depth 0 in the hash, not -1;
// 12_quiesce.inc). SPRT at 10+0.1, UHO, 40/30: +3.38 +- 2.58 over 20,162 games (LLR 2.39, zero excluded); socket 0
// +1.0 +- 3.5, socket 1 (~0.4 ply deeper) +6.6 +- 3.9. Baked on the user's decision. Bench 222624 (with
// HashQsDepth=0 back to 308883).
// BAKE SEEPINNED (06/10/2026): SeePinned 0 -> 1 (in the SEE the pieces pinned to their king do not recapture while
// the pinner is on the board; see.cpp, 11_queue.inc). SPRT at 10+0.1, UHO, 40/30: +4.50 +- 6.78 over 2,934 games,
// socket 0 (40 games, hyperthreads, shallower) -2.1 +- 9.0, socket 1 (~0.4 ply deeper) +13.4 +- 10.2. The user's
// reading: neutral at a low effective TC, positive as soon as the search digs a little deeper; stopped and baked on
// his decision. Fixed-depth tree almost unchanged (median x1.00 at depth 10-16; the bench jumps because of a single
// position). Bench 507070 (with SeePinned=0 back to 222624).
// BAKE CLEAN-UP (06/10/2026): NullFiftyKeep 0 -> 1 (the null move does not increase the fifty-move counter) and
// EvalRefadeBridge 0 -> 1 (an evaluation read back from cache/hash is damped again with the same formula as the
// bridge: the same position is worth the same fresh or read back). They act with a high counter: SPRT on endgames
// (endgame_12_18, 5+0.05, -NoDrawAdj, 40/40) +1.60 +- 2.61 over 9,774 games (socket 0 +1.1, socket 1 +2.1); on the
// UHO book at 6+0.06 -2.19 +- 7.45 over 2,698 (no harm in the middlegame). Fixed-depth tree: middlegame x0.97-1.16,
// endgames x1.05 -> x0.91 at depth 16. Baked on the user's decision as a consistency fix. Bench 370886 (with both at
// 0 back to 507070).
// BAKE CORRPHASE (06/10/2026): CorrPhase 0 -> 1 (pawn and minor corrections separated per expert of the network: a
// constant per phase band mixed into the key). First test -5.10 +- 8.13 over 1,976 games, but the make prefetched the
// rows with the unsalted keys (report docs/audit_8.0/N_CORREZIONI.md, E1, fixed). Again, with the right prefetch and
// the clean-up baked: 12+0.12, UHO, 34/34, +3.22 +- 3.84 over 8,634 games (LLR 1.00; socket 0 +2.2, socket 1 +4.3),
// stopped by a blackout 1,400 games short of the chosen threshold (10,000) and baked on the user's decision. Bench
// 309067 (with CorrPhase=0 back to 370886).
// BAKE HASHDAMP (06/10/2026 night): HashDamp 0 -> 41 (a hash cut from a lower bound pushed toward beta,
// 13_search.inc). Three tests, all positive: 15+0.15 on t80_nm2 (05/10, before the bakes) +2.64 +- 6.79 / 2,768;
// 15+0.15 on t80_tm12 +2.18 +- 5.35 / 4,138; 20+0.2 on t80_tm13 +2.02 +- 4.96 / 4,818 (socket 0 +2.9, socket 1 +1.2).
// On the current code together +2.09 +- 3.64 / 8,956; all three +2.22 +- 3.21 / 11,774 (P(Elo > 0) ~ 91%). Baked on
// the user's decision before 20,000 games. Bench 430151 (with HashDamp=0 back to 309067). Fixed-depth tree
// (tree_depth.py, tm13): middlegame x0.94/0.92/0.91/1.02 at depth 12/14/16/18, endgames x0.87/0.92/1.02/0.99: no
// growth with depth.
// CLEAN-UP OF 08/10/2026: the closed options were retired (off, identical tree, bench unchanged 430151). Their code is
// in the source published on GitHub up to commit bf1a29e (folder source/) and in
// ../_backup/Triumviratus_8.0_pre_pulizia_2026-10-08, should they ever be tried again:
//   RazorAllLin (linear razoring in all nodes; 600 at 10+0.1 -2.32 +- 2.52 / 20,032), NmpPriorFH (lower null-move
//   threshold after successful null moves among the siblings; 48 at 15+0.15 +0.46 +- 2.66 / 17,308), FhRecovery (depth
//   that climbs back gradually after a fail-high at the root; -1.82 +- 4.54 / 5,932), RfpOppCapture (smaller
//   "improving" discount of the RFP when the opponent has an easy capture; 512 at 15+0.15 -3.47 +- 8.37 / 1,704),
//   NmpOppCapture (no null move in that case; -0.34 +- 4.04 / 8,164), RootRedCap (cap on the reduction of root moves;
//   1024 -5.80 +- 5.93 / ~1,000), QsQuietHash (quiet hash move searched in the quiescence; -1.05 +- 3.92), CorrFade
//   (correction damped with the fifty-move counter; 1 at 10+0.1 on endgames -2.70 +- 4.27 / 2,572), TransitionExt
//   (extension of the capture that leaves only pawns; -18.2 +- 16.4 / 402), TTMoveRefresh (new move also in the deeper
//   entry that is kept; -0.65 +- 2.77 / 17,574), TmExpectFloor (depth floor after the expected reply; -5.5 +- 13.4 /
//   696 with the ceiling cap, +0.6 +- 10.8 / 1,090 with the target cap). Also retired: the prefetch trials
//   -DTRIUMV_PREFETCH_THR_PSQT and -DTRIUMV_REFRESH_PREFETCH of nnue_accumulator.cpp (measured and lost).
// Rewrite of 04/10/2026: the search margins are expressed in their own units (pawn ~ 356); the network's evaluation is
// brought to these units by EvalUnits (09_history.inc), so the margins do not depend on the network's scale. Every
// parameter is a UCI option in the development builds and a constant in the release build (TRIUMV_RELEASE), where the
// compiler folds it into the code.

// TRIUMV_POS_SOLVER (14/10/2026, user: "an analysis-only build, with the flag we know what it is meant for"):
// executable "Triumviratus - 8.0 Pos Solver" for hard positions. Analysis mode ALWAYS on (user) and the tree of the
// pre-release of 11/10, which solves more on IQ4 (POS_SOLVER.md: 157 against 148 at 1M nodes): per-expert margins
// EXP1 at zero and LDSE on (LdseMargin 76). PSV(game, solver) chooses the default; without the flag the engine is the
// playing one, bench 137419 unchanged.
#ifdef TRIUMV_POS_SOLVER
  #define PSV(game, solver) (solver)
#else
  #define PSV(game, solver) (game)
#endif

// ---------------------------------------------------------------------------
// Table of the parameters: name, default, minimum, maximum.
// ---------------------------------------------------------------------------
#define TRIUMV_SEARCH_PARAMS(P)                                                                            \
  /* evaluation */                                                                                        \
  P(EvalUnits, 1347, 900, 1800)          /* /1024: our units -> units of the margins (pawn ~ 356)       */  \
  /* Terms from the residual Res* (09/10/2026, idea P1: constants in cp added to the static evaluation for the     \
     concepts the network underrates, tools/residual_features.py) REMOVED on the evening of 11/10/2026: group A     \
     (ResPasserRank=4 + ResPasserFree=9, endgames 8+0.1) -6.48 +- 6.17 over 1,502; group B (ResKingOpen=7 +          \
     ResSpace=3 + ResPasserConn=9, UHO 8+0.08) -16.03 +- 15.95 over 564. A fixed constant counts twice what the      \
     network (PassedPawns) and the search (PassedPushRed) already know, and moves every pruning margin. In their     \
     place, on the evening of 11/10 (user: "correct them in the TT or use the information in another way"), options \
     OFF: corrections learned during the game (09_history.inc), KingOpenRfp and PasserFreeRed (search side,          \
     13_search.inc). CorrKingPawnW (key = all pawns + 2x2 zone of the kings) CLOSED and REMOVED: -4.68 +- 7.19 over  \
     2,378 at 8+0.08 (5000 and 20000 worse than 10000); in its place two coarse keys without the pawn_key (user:     \
     "only with a coarser key"). CorrShelterW (key = files of the kings + pawns of both colours on the 3 files around \
     each king, middlegame only; 4.6 positions per key in games against 3.3 for the pawns) CLOSED and REMOVED:        \
     CorrShelterW=8000 against default -5.63 +- 5.53 over 3,898 at 8+0.08 UHO, build tm34 (both sockets negative).  \
     Code in the backup _pre_rimozione_shelter_2026-10-11. CorrPassKingW (key = passed pawns + those outside the     \
     enemy king's square + phase) CLOSED and REMOVED: CorrPassKingW=8000 against default -5.24 +- 8.49 over 744 on   \
     endgames 8+0.1 without adjudication, tm34 (user: "the measurement wants it gone"). Backup                       \
     _pre_rimozione_passking_2026-10-11. KingOpenRfp (middlegame > 15 pieces: RFP margin + N for each file without   \
     our pawns around our king, i.e. less pruning with an exposed king) CLOSED NEUTRAL and REMOVED: KingOpenRfp=40     \
     against default -0.37 +- 3.68 over ~9,300 at 8+0.08 UHO (three runs: +6.45 / 3,072, about -3, about -3); SPSA   \
     RES1 (6+0.06, 1,991 iterations) zero gradient, 40 -> 41.6; together with PasserFreeRed=997 -3.76 +- 6.27 over    \
     2,222 at 10+0.1. User: "let us leave it" the 15 s test. Backup _pre_rimozione_kingopen_2026-10-11.              \
     PasserFreeRed (endgames <= 15 pieces: less LMR reduction for the quiet push of a passer outside the enemy        \
     king's square) CLOSED NEUTRAL and REMOVED: PasserFreeRed=1024 against default -2.07 +- 3.42 over ~4,300 on        \
     endgames 8+0.1 without adjudication (first run +2.55 / 829); in the SPSA RES1 1024 -> 997, zero gradient.        \
     Backup _pre_rimozione_blend_2026-10-11. RESIDUAL LINE CLOSED: constant terms, four learned corrections         \
     (KingPawn, Shelter, PassKing; Expert was already neutral) and two search levers, all neutral or negative.      */ \
  P(OptStrength, 183, 0, 600)            /* optimism = strength * s / (|s| + div), in network units     */   \
  P(OptDiv, 284, 1, 600)                                                                                  \
  /* aspiration windows */                                                                                \
  P(AspBase, 6, 1, 40)                                                                                    \
  P(AspMeanSqDiv, 10050, 1000, 40000)                                                                     \
  /* BLOCK 5 - aspiration windows (14_deepen.inc). Defaults give the old behaviour.                              */ \
  /*   AspGrowLo / AspGrowHi  growth of delta (/128) after a fail low / high (one AspGrow = 51 served both)     */ \
  /*   AspFhRed               plies off the search for each fail high in a row (was 1)                         */ \
  /*   AspLoKeep              ours, off: after a fail low beta keeps AspLoKeep/1024 of the window              */ \
  /* New levers: own SPRT at the end of the 8.1 queue, then SPSA (possibly per expert).                         */ \
  P(AspGrowLo, 50, 10, 128)                                                                               \
  P(AspGrowHi, 52, 10, 128)                                                                               \
  P(AspFhRed, 1, 0, 3)                                                                                    \
  P(AspLoKeep, 0, 0, 1024)                                                                                \
  /* BLOCK 5 - threads, lazy SMP (15_threads.inc, 14_deepen.inc). Defaults give the old behaviour.              */ \
  /*   SmpVoteBase, SmpVoteScore  share of a thread in the final vote: (score - lowest + base) x weight / 1024   */ \
  /*   SmpVoteDepth               ours, off: support added per completed ply                                     */ \
  /*   SmpAspCycle, SmpAspStep    aspiration offset per thread, (id % cycle) x step (was id % 8)                 */ \
  /*   SmpSkew                    off: odd helpers start this many plies ahead (known lazy-SMP variant)          */ \
  /* Test 17/10/2026 (SmpSkew 1 + SmpVoteDepth 10, 4 threads 2+0.05): +1.72 +- 4.61 / 5,648 games; queued.      */ \
  P(SmpVoteBase, 14, 0, 200)                                                                              \
  P(SmpVoteScore, 1024, 0, 4096)                                                                          \
  P(SmpVoteDepth, 0, 0, 400)                                                                              \
  P(SmpAspCycle, 8, 1, 16)                                                                                \
  P(SmpAspStep, 1, 0, 8)                                                                                  \
  P(SmpSkew, 0, 0, 3)                                                                                     \
  /* SmpRouteShift (21/10/2026, ours; idea 9 of docs/rete/C2_IDEE_FABLE_2026-10-21.md; Consilium Plebis builds only; OFF, */ \
  /*  SPRT pending): the odd helper threads evaluate with the routing table shifted by this many material classes   */ \
  /*  (negative = toward less material, "one trade ahead"; experts clamped to 0..7), so the threads search with a   */ \
  /*  slightly different network and the shared table receives a small ensemble, at no cost per node. Thread 0     */ \
  /*  never changes. SmpRouteVote 0: those threads do not vote for the bestmove (their scores come from shifted     */ \
  /*  experts); 1: they vote like the others.                                                                      */ \
  P(SmpRouteShift, 0, -2, 2)                                                                              \
  P(SmpRouteVote, 0, 0, 1)                                                                                \
  /* Expert holds (25/10/2026; Consilium Plebis builds only; all OFF, to be tested on a late export at time control, */ \
  /*  since the gain is speed and the price is accuracy). A child keeps an expert instead of the one its material    */ \
  /*  calls for, so the accumulator is updated instead of refreshed. The experts are numbered by material class, so */ \
  /*  "within N" means at most N classes away (1 = adjacent experts).                                               */ \
  /*  C2HoldDepth  (idea of Probably James, Discord 24/10/2026) no expert change below a node of depth <=          */ \
  /*               C2HoldDepth, nor in quiescence: the child keeps its parent's experts while the true one is within */ \
  /*               C2HoldDist                                                                                        */ \
  /*  C2HoldQs     (author, 25/10/2026) only inside quiescence: a capture made from a quiescence node keeps the      */ \
  /*               parent's experts while the true one is within C2HoldDist; the main search is untouched            */ \
  /*  C2HoldDist   the distance allowed by C2HoldDepth and C2HoldQs (1 = adjacent experts, 7 = always)               */ \
  /* With a hold on, nnueverify and the expert check of nnperft disagree with the board by design.                   */ \
  /* REMOVED 27/10/2026 (author: closed, backup _backup/Triumviratus_8.1_pre_rimozione_hold_2026-10-27):             */ \
  /*  C2HoldRoot (the root's experts kept while the true one is within N classes, the C2 form of ExpertFix):        */ \
  /*  1 = -21.8 +- 1.9 / 32,420 g, 10+0.1, ep310 (25/10), H0.                                                       */ \
  /*  C2HoldPv (the experts change only on PV nodes; same source as C2HoldDepth): 1 = -15.2 +- 11.3 / 918 g,          */ \
  /*  10+0.1 noHT, ep486 + adopted margins (27/10), stopped. Holding the wrong expert after a capture on the non-PV   */ \
  /*  nodes costs more than the refreshes saved (+6% nps).                                                           */ \
  /* C2HoldDepth 4: -5.6 +- 8.0 / 1,750 g, 10+0.1, ep310 (25/10), stopped; ep486 + adopted margins (27/10): +0.35 */ \
  /* +- 3.33 / 10,020 g, 10+0.1 noHT, stopped at 10k (+8.7 at 2,078 g faded; s0 -1.7, s1 +2.4). 6: -2% nps, not run */ \
  /* 29/10/2026, final net, base 273963: speed -3.06% / -0.45% cycles per node (mg / eg); fixed 20k nodes -10.25    */ \
  /* +- 8.60 / 2,068 g: the quality lost per node is far more than ~3% of speed can buy back. CLOSED NEGATIVE by the */ \
  /* author, stays 0.                                                                                                */ \
  P(C2HoldDepth, 0, 0, 16)                                                                                \
  P(C2HoldQs, 0, 0, 1)                                                                                    \
  P(C2HoldDist, 1, 1, 7)                                                                                  \
  /* C2PkStale D (27/10/2026, the author's idea of 25/10: the pawn subnetwork only at some moments of the search; OFF  */ \
  /* at 0): at nodes of depth <= D and in quiescence a pawn-king cache miss reuses the outputs of the thread's last   */ \
  /* lookup instead of computing them (consilium/cn.cpp pawn_king). Saves up to ~2% of the time; the evaluation       */ \
  /* differs only after a pawn or king move that the cache has not seen.                                             */ \
  /* 4 vs 0, ep486 (27/10): +1.94 +- 3.37 / 10,012 g, 10+0.1 noHT, stopped at 10k (s0 +1.2, s1 +2.7): not shown.     */ \
  /* 29/10/2026, final net, base 273963: speed -2.50% / -0.87% cycles per node (mg / eg); 4+0.04 noHT +0.82 +- 3.73  */ \
  /* / 8,924 g; fixed 80k nodes +2.31 +- 6.44 / 2,854 g; fixed 10k nodes -1.62 +- 6.46 / 4,714 g. CLOSED NEUTRAL by   */ \
  /* the author (at long time controls neither the error nor the speed is visible): stays 0, no SPSA.               */ \
  P(C2PkStale, 0, 0, 16)                                                                                  \
  /* PawnHistLog (29/10/2026, speed idea for an SPRT): log2 of the pawn-history rows per thread slot. 13 = the 8192 rows */ \
  /* of before (12.6 MB with one thread); smaller tables keep more of the rows the quiet score reads in cache, at the   */ \
  /* price of more pawn structures sharing a row. Takes effect at the next new game (search_clear reallocates).       */ \
  P(PawnHistLog, 13, 9, 16)                                                                               \
  /* history: bonus e malus */                                                                            \
  P(BonusMul, 140, 50, 300)                                                                               \
  P(BonusSub, 81, 0, 300)                                                                                 \
  P(BonusMax, 1615, 500, 3000)                                                                            \
  P(BonusTTMove, 343, 0, 1000)                                                                            \
  P(BonusPrevStatDiv, 28, 4, 200)                                                                         \
  P(MalusMul, 853, 300, 2000)                                                                             \
  P(MalusSub, 235, 0, 800)                                                                                \
  P(MalusMax, 2156, 800, 5000)                                                                            \
  /* ===== BLOCK 1 - histories and move ordering. Parameters x4 = one per expert (0: <=9 pieces, 1: 10-15,      */ \
  /* 2: 16-23, 3: >=24). Every default gives the old behaviour (bench 137419). Letters = sub-blocks of           */ \
  /* docs/development/8.1/SEARCH_1_ORDERING.md; code map in docs/conceptual_maps/BLOCK_1_CODE_MAP.md. =====      */ \
  /* E. How much the histories learn, /1024: reward of the best quiet move (HlQBonus), penalty of the quiet       */ \
  /*    moves tried before it (HlQMalus) and its decay along the list (HlQDecay), reward and penalty of captures  */ \
  /*    (HlCBonus, HlCMalus). They replace five single scales with the same values (removed).                    */ \
  P(HlQBonus0, 860, 300, 2000)  P(HlQBonus1, 860, 300, 2000)  P(HlQBonus2, 860, 300, 2000)  P(HlQBonus3, 860, 300, 2000)  \
  P(HlQMalus0, 1043, 300, 2500) P(HlQMalus1, 1043, 300, 2500) P(HlQMalus2, 1043, 300, 2500)  P(HlQMalus3, 1043, 300, 2500)  \
  P(HlQDecay0, 921, 600, 1024)  P(HlQDecay1, 921, 600, 1024)  P(HlQDecay2, 921, 600, 1024)  P(HlQDecay3, 921, 600, 1024)  \
  P(HlCBonus0, 1368, 400, 3000) P(HlCBonus1, 1368, 400, 3000) P(HlCBonus2, 1368, 400, 3000) P(HlCBonus3, 1368, 400, 3000) \
  P(HlCMalus0, 1270, 400, 3000) P(HlCMalus1, 1270, 400, 3000) P(HlCMalus2, 1270, 400, 3000) P(HlCMalus3, 1270, 400, 3000) \
  /* F. Gravity of the saturating update, /1024 (lower = closer to the limit): HlGrav quiet histories (main,      */ \
  /*    first plies, pawn, continuations), HlCGrav captures, CorrGrav corrections, TtmGrav hash-move history.     */ \
  /* G. HlAux scale of the first-ply, pawn and continuation updates; HlFNear / HlFFar continuations at 1-2 and   */ \
  /*    3-6 plies; HlEval update from the evaluation difference; EvPen separate weight of its penalties.          */ \
  /* E. HlDepthQ quadratic term of every amount by depth (q*d*d/64, 0 = linear).                                */ \
  /* C. HtCapt / HtVict capture history and victim value in a capture score (/1024; were 1 and 7);              */ \
  /*    HeMain / HeCont history and cont1 in the order of quiet evasions (only the ratio matters).                */ \
  P(HlGrav0, 1024, 768, 1536)   P(HlGrav1, 1024, 768, 1536)   P(HlGrav2, 1024, 768, 1536)   P(HlGrav3, 1024, 768, 1536)   \
  P(HlCGrav0, 1024, 768, 1536)  P(HlCGrav1, 1024, 768, 1536)  P(HlCGrav2, 1024, 768, 1536)  P(HlCGrav3, 1024, 768, 1536)  \
  P(HlAux0, 1024, 256, 2048)    P(HlAux1, 1024, 256, 2048)    P(HlAux2, 1024, 256, 2048)    P(HlAux3, 1024, 256, 2048)    \
  P(HlFNear0, 1024, 256, 2048)  P(HlFNear1, 1024, 256, 2048)  P(HlFNear2, 1024, 256, 2048)   P(HlFNear3, 1024, 256, 2048)  \
  P(HlFFar0, 1024, 256, 2048)   P(HlFFar1, 1024, 256, 2048)   P(HlFFar2, 1024, 256, 2048)   P(HlFFar3, 1024, 256, 2048)    \
  P(HlDepthQ0, 0, -64, 64)      P(HlDepthQ1, 0, -64, 64)     P(HlDepthQ2, 0, -64, 64)      P(HlDepthQ3, 0, -64, 64)      \
  P(HlEval0, 1024, 0, 2048)     P(HlEval1, 1024, 0, 2048)     P(HlEval2, 1024, 0, 2048)     P(HlEval3, 1024, 0, 2048)     \
  P(HtCapt0, 1024, 256, 2048)   P(HtCapt1, 1024, 256, 2048)   P(HtCapt2, 1024, 256, 2048)   P(HtCapt3, 1024, 256, 2048)   \
  P(HtVict0, 7168, 3584, 10752) P(HtVict1, 7168, 3584, 10752) P(HtVict2, 7168, 3584, 10752) P(HtVict3, 7168, 3584, 10752) \
  P(HeMain0, 1024, 256, 2048)   P(HeMain1, 1024, 256, 2048)   P(HeMain2, 1024, 256, 2048)   P(HeMain3, 1024, 256, 2048)   \
  P(HeCont0, 1024, 0, 2048)     P(HeCont1, 1024, 0, 2048)     P(HeCont2, 1024, 0, 2048)     P(HeCont3, 1024, 0, 2048)     \
  P(EvPen0, 1024, 256, 2048)    P(EvPen1, 1024, 256, 2048)    P(EvPen2, 1024, 256, 2048)    P(EvPen3, 1024, 256, 2048)    \
  P(CorrGrav0, 1024, 768, 1536) P(CorrGrav1, 1024, 768, 1536) P(CorrGrav2, 1024, 768, 1536) P(CorrGrav3, 1024, 768, 1536) \
  /* I. CorrGain weight of the correction in the evaluation (/1024); CorrDepthQ quadratic depth term of its      */ \
  /*    reward, d + q*d*d/64 (0 = linear).                                                                       */ \
  P(CorrGain0, 1024, 256, 2048) P(CorrGain1, 1024, 256, 2048) P(CorrGain2, 1024, 256, 2048) P(CorrGain3, 1024, 256, 2048) \
  P(CorrDepthQ0, 0, -64, 64)    P(CorrDepthQ1, 0, -64, 64)    P(CorrDepthQ2, 0, -64, 64)    P(CorrDepthQ3, 0, -64, 64)    \
  P(TtmGrav0, 1024, 768, 1536)  P(TtmGrav1, 1024, 768, 1536)  P(TtmGrav2, 1024, 768, 1536)  P(TtmGrav3, 1024, 768, 1536)  \
  P(PrevContMalus, 713, 0, 1500)                                                                          \
  P(LowPlyScale, 712, 0, 1500)                                                                            \
  P(ContUpdScale, 874, 300, 1500)                                                                         \
  P(PawnBonusScale, 1104, 300, 2000)                                                                      \
  P(PawnMalusScale, 459, 100, 1500)                                                                       \
  P(EvalDiffMul, 11, 0, 40)               /* history of the previous move from the eval difference */   \
  P(EvalDiffPawnMul, 13, 0, 40)                                                                           \
  /* ordinamento */                                                                                       \
  /* D. Groups of the move queue (11_queue.inc), replacing three fixed constants (19, -15563, 3223):             */ \
  /*    StGood  how much a capture score loosens its exchange test: SEE >= -score x StGood / 19456              */ \
  /*            (1024 = the old -score/19, same truncation)                                                     */ \
  /*    StQuiet boundary between good quiet moves (before losing captures) and bad ones                         */ \
  /*    StSort  quiet moves with score >= -StSort x depth are sorted, the rest keep generation order            */ \
  P(StGood0, 1024, 256, 3072)   P(StGood1, 1024, 256, 3072)   P(StGood2, 1024, 256, 3072)   P(StGood3, 1024, 256, 3072)   \
  P(StQuiet0, -15563, -30000, 0) P(StQuiet1, -15563, -30000, 0) P(StQuiet2, -15563, -30000, 0) P(StQuiet3, -15563, -30000, 0) \
  P(StSort0, 3223, 1000, 8000)  P(StSort1, 3223, 1000, 8000)  P(StSort2, 3223, 1000, 8000)  P(StSort3, 3223, 1000, 8000)  \
  /* B. Checks in the quiet-move score (11_queue.inc):                                                          */ \
  /*    ChkBonus bonus of a direct check, ChkSee exchange loss it may accept (were fixed at 13323 and -75)        */ \
  /*    ChkDisc  ours, off: bonus for a quiet move that discovers a check; own SPRT at the end of the queue       */ \
  P(ChkBonus0, 13323, 0, 40000) P(ChkBonus1, 13323, 0, 40000) P(ChkBonus2, 13323, 0, 40000) P(ChkBonus3, 13323, 0, 40000) \
  P(ChkSee0, -75, -400, 100)    P(ChkSee1, -75, -400, 100)    P(ChkSee2, -75, -400, 100)    P(ChkSee3, -75, -400, 100)    \
  P(ChkDisc0, 0, 0, 40000)      P(ChkDisc1, 0, 0, 40000)      P(ChkDisc2, 0, 0, 40000)      P(ChkDisc3, 0, 0, 40000)      \
  /* B. Threats in the quiet-move score, points per unit of material of the moving piece (11_queue.inc):        */ \
  /*    ThrEsc  the piece leaves a square attacked by a lower-value enemy piece                                 */ \
  /*    ThrEnt  the piece enters such a square (one weight, ThreatOrderMul = 20, served both; tuned value 25)   */ \
  /*    ThrSafe ours, off: extra bonus when the threatened piece escapes to an unattacked square; own SPRT      */ \
  /*    (30/10/2026) ThrEsc/ThrEnt in 1/64 of a point (1280 = the old 20; the term is shifted right by 6, exact   */ \
  /*    for multiples of 64), so that the SPSA moves them on a fine scale instead of steps of 2 on 20.           */ \
  P(ThrEsc0, 1280, 0, 3840)  P(ThrEsc1, 1280, 0, 3840)  P(ThrEsc2, 1280, 0, 3840)  P(ThrEsc3, 1280, 0, 3840)  \
  P(ThrEnt0, 1280, 0, 3840)  P(ThrEnt1, 1280, 0, 3840)  P(ThrEnt2, 1280, 0, 3840)  P(ThrEnt3, 1280, 0, 3840)  \
  P(ThrSafe0, 0, 0, 60)  P(ThrSafe1, 0, 0, 60)  P(ThrSafe2, 0, 0, 60)  P(ThrSafe3, 0, 0, 60)              \
  /* A. LpDecay: how fast the first-ply history weight falls, 8 x entry x 1024 / (1024 + ply x LpDecay);        */ \
  /*    1024 = the old 8 x entry / (1 + ply), 0 = the same weight at every ply. Max 3072: beyond it the 40-bit  */ \
  /*    reciprocal of 11_queue.inc is no longer exact.                                                          */ \
  P(LpDecay0, 1024, 0, 3072) P(LpDecay1, 1024, 0, 3072) P(LpDecay2, 1024, 0, 3072) P(LpDecay3, 1024, 0, 3072) \
  /* Threat-context history (09_history.inc), shared by all experts: 0 = off (old tree), 1-3 = factored form   */ \
  /* (flat table always updated + a correction per context, read only by the ordering; 3 is the value used in   */ \
  /* the tuning runs). The separate-table forms were removed on 17/10/2026 (H0 at 8+0.08).                      */ \
  /* BAKE 17/10/2026 (block 1, see the head of the file): ThreatHist 0 -> 3, ThreatCtxW 1024 -> 1156,           */ \
  /* ThreatCtxUpd 1024 -> 1083 (provisional: own SPRT of ThreatHist at the end of the 8.1 queue).               */ \
  P(ThreatHist, 0, 0, 3)                                                                                  \
  P(ThreatCtxW, 1024, 0, 3072)           /* weight of the context correction in the ordering, /1024 */          \
  P(ThreatCtxUpd, 1024, 0, 3072)         /* scale of the update of the context correction, /1024 */             \
  /* ThreatBal (ours; one threat balance of a quiet move in place of the threat term and the safe-square attack     */ \
  /*  bonuses) lost -17.3 +- 12.7 over 684 games in a package and was REMOVED on 28/10/2026 with its weights.     */ \
  /* LmrRank (block 2 of 8.1, 16/10/2026, ours): for the ordered quiet moves, the move number of the base reduction */ \
  /* mixed with an "effective" number from the gap between its ordering score and the best quiet move of the node   */ \
  /* (13_search.inc). 0 = off (tree unchanged). Scale: points of gap for one more move; mix per expert, /1024 (0 = */ \
  /* the order number only, 1024 = the effective number only).                                                       */ \
  /* A. Reading the histories in the quiet-move score (11_queue.inc), /1024: main history (old weight x2), pawn  */ \
  /*    history (x2), near continuations cont1+cont2 (x1), far ones cont3+cont4+cont6 (x1), first plies (x1),    */ \
  /*    and HmAgree (ours): share of the smaller of cont1 and cont2 when they agree in sign (0 = none).          */ \
  P(HmMain0, 2048, 0, 6144) P(HmMain1, 2048, 0, 6144) P(HmMain2, 2048, 0, 6144) P(HmMain3, 2048, 0, 6144) \
  P(HmPawn0, 2048, 0, 6144) P(HmPawn1, 2048, 0, 6144) P(HmPawn2, 2048, 0, 6144) P(HmPawn3, 2048, 0, 6144) \
  P(HmNear0, 1024, 0, 3072) P(HmNear1, 1024, 0, 3072) P(HmNear2, 1024, 0, 3072) P(HmNear3, 1024, 0, 3072) \
  P(HmFar0, 1024, 0, 3072)  P(HmFar1, 1024, 0, 3072)  P(HmFar2, 1024, 0, 3072)  P(HmFar3, 1024, 0, 3072)  \
  P(HmLow0, 1024, 0, 3072)  P(HmLow1, 1024, 0, 3072)  P(HmLow2, 1024, 0, 3072)  P(HmLow3, 1024, 0, 3072)  \
  P(HmAgree0, 0, -1024, 1024) P(HmAgree1, 0, -1024, 1024) P(HmAgree2, 0, -1024, 1024) P(HmAgree3, 0, -1024, 1024) \
  P(LmrRank, 0, 0, 1)                                                                                     \
  P(LmrRankScale, 4000, 500, 20000)                                                                       \
  P(LmrRankMix0, 512, 0, 1024)                                                                            \
  P(LmrRankMix1, 512, 0, 1024)                                                                            \
  P(LmrRankMix2, 512, 0, 1024)                                                                            \
  P(LmrRankMix3, 512, 0, 1024)                                                                            \
  /* pruning before the moves */                                                                          \
  /* Block 3 in our form (20/10/2026, 13_search.inc eval_gates, null_probe, capture_probe, capture_gate,           */ \
  /* quiet_gate): numbers once written in the code as parameters at their old values, terms per expert of our    */ \
  /* own at 0 (same tree, bench unchanged).                                                                       */ \
  P(RazorExp0, 0, -400, 400) P(RazorExp1, 0, -400, 400) P(RazorExp2, 0, -400, 400) P(RazorExp3, 0, -400, 400)     \
  P(RfpHuntDepth, 6, 2, 20)              /* reverse futility below this depth while hunting a mate         */ \
  P(NmpExp0, 0, -200, 200) P(NmpExp1, 0, -200, 200) P(NmpExp2, 0, -200, 200) P(NmpExp3, 0, -200, 200)             \
  P(NmpBetaFloor, 2000, 500, 4000)       /* no null move with beta below -NmpBetaFloor                     */ \
  P(NmpDepthDiv, 3, 2, 6)                /* null-move reduction: + depth / NmpDepthDiv                     */ \
  P(NmpVerifyNum, 3, 1, 4)               /* verification: no null move for NmpVerifyNum/4 of the depth     */ \
  P(IirDepth, 6, 3, 12)                  /* one ply less without a hash move from this depth               */ \
  P(ProbCutMinDepth, 3, 2, 8)                                                                              \
  P(ProbCutRed, 3, 2, 6)                 /* ProbCut search ProbCutRed plies shallower, +RedImp improving   */ \
  P(ProbCutRedImp, 2, 0, 4)                                                                                \
  P(ProbCutExp0, 0, -200, 200) P(ProbCutExp1, 0, -200, 200) P(ProbCutExp2, 0, -200, 200)                         \
  P(ProbCutExp3, 0, -200, 200)                                                                             \
  /* Move count in our form (27/10/2026, docs/audit_8.1/R9): threshold = LmpBase + (LmpLin*d + LmpQuad*d^2)/16,    */ \
  /* times LmpNotImp/1024 when the node is not improving. At 0/16/512 it is the old (LmpBase + d^2) halved when not   */ \
  /* improving (bench unchanged); the SPSA moves the three levers.                                                  */ \
  P(LmpBase, 3, 0, 12)                                                                                    \
  P(LmpLin, -3, -32, 64)                                                                                   \
  P(LmpQuad, 16, 4, 48)                                                                                   \
  P(LmpNotImp, 482, 256, 1024)                                                                            \
  P(CaptFutMaxDepth, 8, 2, 16)           /* capture futility below this corrected depth                    */ \
  P(QuietFutMaxDepth, 12, 4, 20)         /* quiet futility below this corrected depth                      */ \
  P(RazorMul, 452, 100, 1200)                                                                             \
  P(RfpMaxDepth, 19, 4, 30)                                                                               \
  P(RfpMultBase, 50, 10, 120)                                                                             \
  P(RfpMultDepth, 4, 0, 16)                                                                               \
  P(RfpMultCap, 85, 30, 200)                                                                              \
  P(RfpNoTTHit, 20, 0, 60)                                                                                \
  P(RfpImproving, 2852, 0, 5000)                                                                          \
  P(RfpOppWorse, 335, 0, 1500)                                                                            \
  P(RfpCorrDiv, 198435, 50000, 800000)                                                                    \
  /* Reverse futility in our form (27/10/2026, docs/audit_8.1/R9; 13_search.inc eval_gates): margin = mult*d -      */ \
  /* lean*mult/1024 + |corr| + RfpCorrSign*corr/1024 + RfpMarginBase, corr = signal/RfpCorrDiv. RfpMarginBase is a   */ \
  /* constant in evaluation units (pawn = 208) that does not grow with depth; RfpCorrSign reads the sign of the     */ \
  /* correction (positive = wider margin when the correction raised the evaluation). Neutral 0/0 = the old margin.  */ \
  P(RfpMarginBase, 0, -100, 200)                                                                          \
  P(RfpCorrSign, 0, -1024, 1024)                                                                          \
  /* RfpDisagree (20/10/2026, ours, off): reverse-futility margin + RfpDisagree/1024 per unit of |psqt - positional| */ \
  /* above RfpDisThr of the expert (upper quartile of the disagreement in the prunelog data, 6,000 positions).     */ \
  P(RfpDisagree, 0, -1024, 1024)         /* 256: -6.60 +- 9.04 (1,422, 12+0.12 noHT, UHO): DISCARDED 20/10 */ \
  P(RfpDisThr0, 1326, 0, 8192) P(RfpDisThr1, 773, 0, 8192) P(RfpDisThr2, 763, 0, 8192) P(RfpDisThr3, 545, 0, 8192) \
  P(RfpBetaWeight, 690, 0, 1024)                                                                          \
  P(NmpDepthMul, 15, 0, 40)                                                                               \
  P(NmpImproving, 47, 0, 150)                                                                             \
  P(NmpBase, 302, 0, 800)                                                                                 \
  P(NmpRedBase, 7, 3, 12)                                                                                 \
  P(NmpEvalDiv, 223, 64, 1024)                                                                            \
  /* Null-move reduction in our form (27/10/2026, docs/audit_8.1/R9; 13_search.inc null_probe): R = NmpRedBase +    */ \
  /* d/NmpDepthDiv + max((eval - beta)/NmpEvalDiv, 0) + (DeepNmp + NmpRedDepth*d + NmpRedImp*improving + 10 key   */ \
  /* bits) / 1024. NmpRedDepth is a continuous slope (1024ths of a ply per ply of depth) on top of the integer      */ \
  /* d/NmpDepthDiv; NmpRedImp is 1024ths of a ply on an improving node. Neutral 0/0 = the old R (bench unchanged).   */ \
  P(NmpRedDepth, 0, -128, 128)                                                                            \
  P(NmpRedImp, 0, -1024, 1024)                                                                            \
  P(NmpVerifyDepth, 16, 6, 30)                                                                            \
  /* Endgame rule (report R2, 18/10/2026): no null move when the side to move has at most NmpFewMoves legal moves  */ \
  /*  and at most NmpFewPieces pieces are on the board (zugzwang is likely there). 0 = off, identical tree.        */ \
  P(NmpFewMoves, 0, 0, 10)                                                                                \
  P(NmpFewPieces, 15, 2, 32)                                                                              \
  /* NmpNpmFloor (22/10/2026, was C2NmpMask, which read the same material through the network's routing table): no   */ \
  /*  null move when the side to move has at most this much non-pawn material (N = B = 3, R = 5, Q = 9). 3 = a minor */ \
  /*  piece or less, 5 = also a lone rook; zugzwang is common there. 0 = off (only pawns: always skipped).           */ \
  /*  BAKE 0 -> 3 (22/10/2026, author): q54 (as C2NmpMask 1, same rule and bench) on the ep107 export, endgame_12_18 */ \
  /*  without draw adjudication, 8+0.08: +1.32 +- 2.29 over 6,844 (s0 +1.0, s1 +1.6; LLR [-3,1] +1.30, accepted by */ \
  /*  the author before the close). Bench with ep107: 188906 (187691 with it at 0). Confirmation on 8moves pending. */ \
  P(NmpNpmFloor, 3, 0, 9)                                                                                 \
  P(ProbCutMargin, 209, 80, 500)                                                                          \
  P(ProbCutImproving, 64, 0, 200)                                                                         \
  P(ProbCutTTMargin, 415, 150, 900)                                                                       \
  /* pruning in the move loop */                                                                          \
  P(CaptFutBase, 182, 50, 600)                                                                            \
  P(CaptFutDepth, 284, 80, 500)                                                                           \
  P(CaptFutHist, 135, 0, 400)            /* /1024 */                                                      \
  P(CaptSeeDepth, 176, 50, 400)                                                                           \
  P(CaptSeeHist, 35, 0, 120)             /* /1024 */                                                      \
  /* Block 3, levers per expert (20/10/2026, ours, off): the prunelog data (tools/prunelog_analyze.py) show the     */ \
  /* endgame expert losing 2-7 times more good moves to the move rules (capture futility 4.5% vs 1.3%, move count */ \
  /* 2.2% vs 0.3%, capture exchange 2.0% vs 0.6%). LmpExp in 1/1024 of the move-count threshold; CaptFutExp and   */ \
  /* CaptSeeExp in evaluation units added to the margin (pawn = 208). Positive = less pruning.                     */ \
  P(LmpExp0, 0, -512, 2048) P(LmpExp1, 0, -512, 2048) P(LmpExp2, 0, -512, 2048) P(LmpExp3, 0, -512, 2048)           \
  P(CaptFutExp0, 0, -400, 800) P(CaptFutExp1, 0, -400, 800) P(CaptFutExp2, 0, -400, 800)                           \
  P(CaptFutExp3, 0, -400, 800)                                                                                       \
  P(CaptSeeExp0, 0, -400, 800) P(CaptSeeExp1, 0, -400, 800) P(CaptSeeExp2, 0, -400, 800)                           \
  P(CaptSeeExp3, 0, -400, 800)                                                                                       \
  P(PrExpRootDepth, 0, 0, 40)            /* the levers above act from this iteration depth on (0 = always) */ \
  /* 20/10/2026, endgame package LmpExp0=1024, CaptFutExp0=208, CaptSeeExp0=104 (endgame book, no draw            */ \
  /* adjudication): 8+0.08 HT -6.81 +- 6.77 (918); 10+0.1 noHT +0.20 +- 4.46 (1,748); with PrExpRootDepth=20 at    */ \
  /* 20+0.2 HT -1.62 +- 6.16 (856, endgame depth 32-35, gate open). Level once there is time: off, kept for SPSA. */ \
  /* Opposite package, prune more in experts 2-3 (LmpExp2/3=-256, CaptFutExp2/3=-104, UHO): 10+0.1 noHT +1.30 +-  */ \
  /* 3.75 (8,016), socket 1 at 12+0.12 noHT -15.90 +- 13.81 (634), all +0.04 +- 3.62 (8,650); clear minus on the  */ \
  /* Zen 4 laptop at 12+0.12. DISCARDED 20/10/2026.                                                                */ \
  P(QuietHistPrune, 3860, 1000, 10000)                                                                    \
  P(QuietMainHistMul, 68, 0, 200)        /* /32 */                                                        \
  P(QuietFutDepth, 92, 40, 300)                                                                          \
  P(QuietFutAlpha, 94, 0, 300)                                                                            \
  P(QuietFutBase, 162, 0, 400)                                                                            \
  P(QuietSeeMul, 24, 5, 80)                                                                               \
  /* Block 3 forms of ours (27/10/2026, docs/audit_8.1/R9): one term per rule that the inherited form lacks, at its */ \
  /* neutral value (bench unchanged); with the coefficients above they form the rule our way once the SPSA (or the */ \
  /* prunelog estimate) moves them. d = depth, rd = depth corrected by the reduction (and the history, quiet moves). */ \
  /*  RazorLin       razoring margin RazorMul*d^2 + RazorLin*d                                                      */ \
  /*  ProbCutDepth   ProbCut raise + ProbCutDepth*d/16                                                              */ \
  /*  CaptFutVictim  capture futility counts the victim at CaptFutVictim/1024 of its value                          */ \
  /*  CaptSeeQuad    capture exchange margin CaptSeeDepth*d + CaptSeeQuad*d^2/16                                     */ \
  /*  QuietHistBase  quiet history prune below -(QuietHistPrune*d + QuietHistBase)                                  */ \
  /*  QuietFutQuad   quiet futility + QuietFutQuad*rd^2/16 (rd > 0)                                                  */ \
  /*  QuietSeeLin    quiet exchange margin QuietSeeMul*rd^2 + QuietSeeLin*rd                                         */ \
  P(RazorLin, 0, -400, 400)                                                                               \
  P(ProbCutDepth, 0, -160, 320)                                                                           \
  P(CaptFutVictim, 1047, 512, 1536)                                                                       \
  P(CaptSeeQuad, -1, -64, 128)                                                                             \
  P(QuietHistBase, -277, -4000, 8000)                                                                        \
  P(QuietFutQuad, 1, -64, 128)                                                                            \
  P(QuietSeeLin, 4, -60, 120)                                                                             \
  /* estensioni */                                                                                        \
  P(SingMarginBase, 76, 20, 150)                                                                          \
  P(SingMarginTtPv, 70, 0, 200)                                                                           \
  P(SingDoubleBase, -4, -100, 100)                                                                        \
  P(SingDoublePv, 215, 0, 500)                                                                            \
  P(SingDoubleQuiet, 145, 0, 400)                                                                         \
  /* Ladder of the extra plies (19/10/2026, ours): the first rung is the Double threshold above, every further rung */ \
  /* sits one step higher. Step = Base + Pv*pv - Quiet*quiet + TtPv*tt_pv + hist term - Deep*beyond + Depth*depth/16. */ \
  /* Start values = the old separate third threshold rewritten as a step (same tree); SingStepDepth and a third    */ \
  /* rung (SingRungs 3) are ours and start off.                                                                     */ \
  P(SingStepBase, 66, -100, 300)                                                                          \
  P(SingStepPv, 79, -100, 300)                                                                            \
  P(SingStepQuiet, 38, -100, 300)                                                                         \
  P(SingStepTtPv, 80, 0, 300)                                                                             \
  P(SingStepDepth, 0, -64, 64)           /* ours: step per 16 plies of depth. DISCARDED 19/10/2026, loses  */ \
                                         /* both ways (8+0.08 HT, UHO): -32 -3.05 +- 5.53 (4,106), +32     */ \
                                         /* -5.49 +- 8.18 (1,836); a step constant with depth stays.       */ \
  P(SingRungs, 2, 1, 3)                  /* ours: extra plies at most (1 + SingRungs in all). 3 DISCARDED  */ \
                                         /* 19/10/2026: -13.61 +- 9.47 (1,456, 8+0.08 HT, UHO).            */ \
  /* Block 4 in our form (19/10/2026, 13_search.inc singular_probe): the thresholds of the previous code become  */ \
  /* parameters at their old values, and new terms of our own start at 0 (same tree, bench unchanged).          */ \
  P(SingMinDepth, 6, 3, 12)              /* the exclusion search from this depth, one more on tt_pv nodes  */ \
  P(SingMinDepthTtPv, 1, 0, 3)                                                                             \
  P(SingHashSlack, 3, 0, 6)              /* the hash entry may be this many plies shallower than the node   */ \
  P(SingMarginDiv, 64, 32, 128)          /* the margin of the bar is per this many plies of depth          */ \
  P(SingDepthMul, 1026, 512, 1536)       /* depth of the exclusion search, new_depth * Mul / 2048          */ \
  /* Singular bar in our form (27/10/2026, docs/audit_8.1/R9): s_beta = hash value - margin*d/SingMarginDiv -        */ \
  /* SingBarBase (evaluation units, a constant at every depth); s_depth = (new_depth*SingDepthMul + 128*SingDepthOfs) */ \
  /* / 2048 (SingDepthOfs in sixteenths of a ply). Neutral 0/0 = the old bar and depth (bench unchanged).           */ \
  P(SingBarBase, 0, -32, 64)                                                                               \
  P(SingDepthOfs, 0, -32, 32)                                                                              \
  P(SingExp0, 0, -40, 40)                /* ours: margin of the bar per expert of the network              */ \
  P(SingExp1, -1, -40, 40)                                                                                  \
  P(SingExp2, 3, -40, 40)                                                                                  \
  P(SingExp3, 0, -40, 40)                                                                                  \
  P(SingSurplus, 0, -16, 16)             /* ours: margin per ply of hash depth beyond what is needed       */ \
  P(SingMoving, 2, -16, 16)              /* ours: margin per 64 units of |hash value - static evaluation|  */ \
  P(SingLvlExp0, 1, -100, 100)           /* ours: thresholds of the extra plies per expert                 */ \
  P(SingLvlExp1, 1, -100, 100)                                                                             \
  P(SingLvlExp2, 0, -100, 100)                                                                             \
  P(SingLvlExp3, 0, -100, 100)                                                                             \
  P(SingNegHigh, 4, 0, 4)                /* negative extension when the hash value clears beta             */ \
  P(SingNegCut, 3, 0, 4)                 /* negative extension on an expected cut node. 2: -1.98 +- 4.51   */ \
                                         /* (6,322, 8+0.08 HT, UHO, 19/10/2026), stopped; 3 stays.         */ \
  P(SingleReply, 0, 0, 3)                /* ours, off (19/10/2026): plies for the only legal reply to check */ \
                                         /* 1: -10.76 +- 13.26 (678, 8+0.08 HT, UHO), stopped. Single      */ \
                                         /* replies: 0.6% of game positions, 6.5% of checks; bench +4%.    */ \
  /* SBAKE LDSE 14/10/2026 (user): non-regression at 40+0.4 HT of time + Ldse together (tm53), release side            \
     -9.37 +- 10.91 over 890 (was -13.7 at 660); from the PGNs both sides spend the same time per phase (+-4%) but the  \
     side without the levers goes 0.1-0.25 ply deeper: Ldse suspected (tuned at 10+0.1, long-TC check stopped at 216).  \
     Back to 0 (off); to be retried alone at 30 s. Before: 76. */                                                      \
  /* Two-line search (AnShadow, Pos Solver, 15/10/2026): cheaper variants. ShadowFrom: the second line only from  */\
  /* this root depth; ShadowRed: the second line N plies shorter; ShadowMargin: the second line only if its last    */\
  /* score is within the margin (internal units, ~526 = 100 cp) of the first (0 = always). Default = full AnShadow; */\
  /* without AnShadow none of the three counts.                                                                     */\
  P(ShadowFrom, 1, 1, 40)                                                                                 \
  P(ShadowRed, 0, 0, 8)                                                                                   \
  P(ShadowMargin, 0, 0, 4000)                                                                             \
  /* AnDirect (15/10/2026): winning threshold and maximum distance between the two lines, in printed cp.        */\
  P(DirectFrom, 300, 0, 3000)                                                                             \
  P(QuietFrom, 10, 1, 40)                                                                                 \
  P(QuietCp, 15, 0, 200)                                                                                  \
  P(DirectGap, 400, 0, 2000)                                                                              \
  P(LdseMargin, PSV(0, 76), 0, 400)      /* BAKE LDSE 05/10/2026 (was 0, see the header): at depth <=      */ \
  P(LdseDepth, 7, 1, 12)                 /* LdseDepth the hash move with a lower bound is extended if      */ \
  P(LdseMode, 1, 0, 7)                   /* the eval is below alpha - m. Mode (was 0) bits: 1 cut nodes     */ \
                                         /* only, 2 lower bound only, 4 not from the quiescence             */ \
  P(LdseMax, 1, 0, 8)                    /* BAKE 05/10/2026 (was 0, see the header): cap on the Ldse        */ \
                                         /* extensions along a line (0 = no cap)                            */ \
  /* NegExtMax: baked at 1 on 11/10/2026 and retired as an option (see the header); it capped the extensions from  */ \
  /* negative reductions per line (up to +2 plies), like LdseMax; 0 = no cap (the tree of before).                 */ \
  /* reductions (1/1024 of a ply) */                                                                      \
  P(LmrLogMul, 2872, 1500, 4000)         /* /128 */                                                       \
  P(LmrDeltaMul, 577, 0, 1500)                                                                            \
  P(LmrNotImproving, 197, 0, 600)        /* /512 */                                                       \
  P(LmrBase, 998, 0, 2500)                                                                                \
  P(LmrTtPvAdd, 929, 0, 2500)                                                                             \
  P(LmrTtPvSub, 2915, 0, 6000)                                                                            \
  P(LmrTtPvPv, 1004, 0, 3000)                                                                             \
  P(LmrTtPvAlpha, 885, 0, 3000)                                                                           \
  P(LmrTtPvDepth, 816, 0, 3000)                                                                           \
  P(LmrTtPvCut, 940, 0, 3000)                                                                             \
  P(LmrOffset, 697, -1500, 2500)                                                                          \
  P(LmrMoveCount, 60, 0, 300)                                                                             \
  P(LmrCorrDiv, 26310, 5000, 100000)                                                                      \
  P(LmrCutNode, 4093, 1000, 7000)                                                                         \
  P(LmrCutNoTT, 933, 0, 3000)                                                                             \
  P(LmrTTCapture, 1079, 0, 3000)                                                                          \
  P(LmrCutoffBase, 264, 0, 1500)                                                                          \
  P(LmrCutoffMany, 1095, 0, 3000)                                                                         \
  P(LmrCutoffAll, 1138, 0, 3000)                                                                          \
  P(LmrTTMove, 2037, 0, 5000)                                                                             \
  P(CausalRed, 512, 0, 4096)             /* BAKE 10/10/2026 (was 0, see the header): 1/1024 ply more for the \
                                            quiet moves off the squares of a sibling's refutation             \
                                            (13_search.inc, causal_mask; 0 = off). 12/10 evening,             \
                                            non-regression at 10+0.1 HT (tm42, CausalRed=0 against 512): from \
                                            the baked side -4.78 +- 9.12 over 1,528, stopped by the user; it  \
                                            stays 512 (SPSA EXP2: 512 -> ~464, no clear trend; the AG1        \
                                            variants were removed on 13/10) */                                 \
  P(LmrStatDiv, 377, 100, 1500)          /* /4096 */                                                      \
  P(LmrAllNode, 276, 0, 800)                                                                              \
  P(LmrNoTTFull, 1127, 0, 3000)                                                                           \
  /* Block 2 in our form (20/10/2026, red_entry_v80 / red_final_v80 and the search of the move): numbers once      */ \
  /* written in the code as parameters at their old values (same tree).                                          */ \
  P(LmrGapMul, 3, 0, 8)                  /* quiet moves: + LmrGapMul * clamp(alpha - eval, -Lo, Hi)        */ \
  P(LmrGapLo, 64, 0, 256)                                                                                  \
  P(LmrGapHi, 96, 0, 256)                                                                                  \
  P(LmrMinDepth, 2, 1, 4)                /* reduced search of the later moves from this depth              */ \
  P(NegExtPlies, 2, 0, 3)                /* a negative reduction extends at most this much, once per line  */ \
  P(LmrDeepRedMin, 2, 1, 4)              /* first move: the second ply off only above this depth           */ \
  P(DeeperMargin, 51, 0, 200)                                                                             \
  P(ShallowerMargin, 8, 0, 60)                                                                            \
  /* ResOwn (19/10/2026, ours, off): re-search depth after a reduced move fails high from its margin over alpha  */ \
  /* against the uncertainty of the node, u = ResU0 + ResUc*corr/64 + ResUr*(plies of reduction); one ply deeper */ \
  /* above ResKp*u/64, one shallower below ResKm*u/64 (fit on 9.2 million moves, redres_fit.py). Against the old */ \
  /* margins +0.22 +- 4.42 over 6,238 games (10+0.1 and 6+0.06 noHT, 8+0.08 HT); baked on 19/10 and taken back  */ \
  /* out the same night with the rest of block 2 (see the RESTORE note at the top).                             */ \
  P(ResOwn, 0, 0, 1)                                                                                      \
  P(ResU0, 8, 1, 64)                                                                                      \
  P(ResUc, 1, 0, 16)                                                                                      \
  P(ResUr, 8, 0, 32)                                                                                      \
  P(ResKp, 32, 0, 256)                                                                                    \
  P(ResKm, 4, 0, 64)                                                                                      \
  /* Quiescence, block 5 in our form (17/10/2026; 12_quiesce.inc). Defaults = the old tree.                   */ \
  /*  QsFut   margin over the stand for a capture to be worth searching, per expert (was 312 for all)          */ \
  /*  QsSee   exchange loss a capture may accept, per expert (was 74 for all)                                  */ \
  /*  QsGate  plain captures searched before the futility gate closes (was 2)                                  */ \
  /*  QsVictimW weight of the victim's value in the futility estimate, /1024                                   */ \
  /*  QsSeeFutW weight of the gap (alpha - futility base) in the exchange test of a plain capture, /1024       */ \
  P(QsFut0, 312, 100, 600)  P(QsFut1, 312, 100, 600)  P(QsFut2, 312, 100, 600)  P(QsFut3, 312, 100, 600)       \
  P(QsSee0, 74, 0, 300)     P(QsSee1, 74, 0, 300)     P(QsSee2, 74, 0, 300)     P(QsSee3, 74, 0, 300)          \
  P(QsGate, 2, 0, 8)                                                                                      \
  P(QsVictimW, 1024, 512, 1536)                                                                           \
  /*  QsMatStand (29/10/2026, ours, OFF; speed session R15 §3f, approved by the author): in a non-PV quiescence   */ \
  /*  node out of check whose evaluation is not in the hash, if the material balance (SeeVal*) minus QsMatStand  */ \
  /*  is already at or above beta, the node stands on the material and returns beta without calling the network */ \
  /*  (~5,000 cycles per evaluation, 0.40 fresh evaluations per node). 0 = off, identical tree. Build with       */ \
  /*  -DTRIUMV_QSMAT_STATS to count how often it fires and how often the network would have said < beta.       */ \
  /*  QsMatScale: SEE units (pawn 208) to evaluation units (pawn ~356), /1024.                                  */ \
  P(QsMatStand, 0, 0, 4000)                                                                               \
  P(QsMatScale, 1752, 1024, 2560)                                                                         \
  /*  QsMatMode: 0 = the material balance, 1 = from the parent (its corrected evaluation minus what the move    */ \
  /*  that led here took; 12_quiesce.inc qs_stand_estimate).                                                      */ \
  P(QsMatMode, 0, 0, 1)                                                                                   \
  P(QsSeeFutW, 1024, 512, 1536)                                                                           \
  /* correction history */                                                                                \
  P(CorrPawnW, 17291, 0, 40000)                                                                           \
  P(CorrMinorW, 10827, 0, 40000)                                                                          \
  P(CorrNonPawnW, 11613, 0, 40000)                                                                        \
  /* Correction combination in our form (27/10/2026, docs/audit_8.1/R9; 09_history.inc corr_signal): the non-pawn  */ \
  /* tables of the two colours weigh CorrNonPawnW * (own + opp) + CorrNpSideW * (own - opp), own = side to move.    */ \
  /* Neutral 0 = one weight for both (the old sum, bench unchanged); the SPSA (block Q) moves it.                   */ \
  P(CorrNpSideW, 0, -10000, 10000)                                                                        \
  P(CorrContW, 8022, 0, 30000)                                                                            \
  P(CorrMinorUpd, 150, 0, 400)           /* /128 */                                                       \
  P(CorrNonPawnUpd, 186, 0, 400)                                                                          \
  /* Corrections, proposals of the report docs/audit_8.0/N_CORREZIONI.md (06/10/2026); off = tree unchanged */ \
  P(CorrCont6W, 0, 0, 30000)             /* continuation also with our move at -6 (0 = off) */            \
                                         /* 06/10: =4011 at 15+0.15 -0.80 +- 5.56 / 3,894 (sockets disagree, */ \
                                         /* stopped; NPS cost -0.1%): off, to be re-evaluated                */ \
  P(CorrCont6Upd, 38, 0, 256)            /* step of the continuation at -6, /128 */                       \
  P(CorrContOppW, 0, 0, 30000)           /* user's idea (06/10): continuation with the opponent's PLAN, the */ \
                                         /* pair (their move at -3, their move at -1); 0 = off           */ \
                                         /* =4011 at 12+0.12 -5.15 +- 12.68 / 742; =8000 at 8+0.08 -4.81 +- 8.67 */ \
                                         /* / 1,806. The cells carry signal (corr. with the residual +0.24), */ \
                                         /* NPS cost ~1%, tree unchanged: off, goes into the SPSA CORR1 from 4011 */ \
  P(CorrContOppUpd, 70, 0, 256)          /* step of the opponent continuation, /128 */                    \
  P(CorrLearnMode, 0, 0, 15)             /* bit 1: nothing if excluded without moves; 2: nothing in excluded */ \
                                         /* nodes; 4: nothing from decided values; 8: exact PV nodes in every */ \
                                         /* direction. 06/10 at 12+0.12: =1 does not change the bench (skipped); */ \
                                         /* =2 -1.64 +- 7.80 / 2,114 (off); 4 and 8 not yet tried            */ \
  P(CorrExpertW, 0, 0, 40000)          /* per-expert pawn table ADDED to the shared one (0 =            */ \
                                         /* off); it enters only the evaluation, not the selectivity       */ \
  P(CorrExpertUpd, 128, 0, 400)          /* step of the per-expert table, /128 */                         \
  P(CorrExpertBands, 4, 2, 4)            /* 4 = one expert per band; 2 = two bands (<=15 / >=16 pieces) */ \
  /* Constants of the correction update, formerly hard-coded: now levers for the SPSA CORR1 (report        \
     N_CORREZIONI 4.3). The defaults are the old values: identical tree. */                                \
  P(CorrPawnUpd, 128, 32, 256)           /* step of the pawn table, /128 */                               \
  P(CorrCont2Upd, 130, 0, 256)           /* step of the continuation at -2, /128 */                       \
  P(CorrCont4Upd, 70, 0, 256)            /* step of the continuation at -4, /128 */                       \
  P(CorrBonusHi, 12, 4, 32)              /* bonus per depth when there is a better move, /128 */          \
  P(CorrBonusLo, 18, 4, 40)              /* bonus per depth on a fail low, /128 */                        \
  P(CorrBonusMaxHi, 256, 64, 600)        /* cap of the positive bonus */                                  \
  P(CorrBonusMaxLo, 256, 64, 600)        /* cap of the negative bonus */                                  \
  P(CorrSingMul, 180, 0, 600)            /* bonus of the singular multi-cut per depth, /1024 */           \
  P(SingCorrDiv, 186725, 50000, 800000)  /* |signal| in the double/triple margins of the singular */      \
  /* 08/10/2026: the continuation term after the null move, formerly the constant 64049, is a lever. It serves the \
     SPSA CORR1: adding CorrCont6W the total correction grows (here it also enters RFP, LMR and the singular      \
     margins), so the weights must be rebalanced together and not one at a time as in the trial of 06/10.         \
     The SPSA searches the values together. */              \
  P(CorrNoMoveW, 64049, 0, 200000)       /* continuation term after the null move */                      \
  /* our techniques on top of the new search (04/10/2026), off = tree unchanged */                        \
  /* LEAN (28/10/2026 night, SPRT lean vs base 6+0.06 noHT [-1, 2]: +4.21 +- 2.79 over 15,194 games, H1, both     \
     sockets positive; the terms never passed an SPRT alone: KingShield -1.57, OutpostOrder +0.21, trio +0.39 on    \
     8.0, then valued by the 8.0 SPSAs; no long-TC retest by the author's decision): the six chess-knowledge         \
     terms of the quiet ordering at 0 (were KingShield 5371, OutpostOrder 4179, AttackOrder 5807, AttackOrderQ     \
     6371, PassedPush 3975, OutpostOrderB 1078). In the release the compiler removes their maps and tests: -1.5%   \
     cycles per node in the middlegame, -2.4% in endgames (Xeon, PGO, xperf). Bench 219346 -> 273963. */          \
  P(KingShield, 0, 0, 40000)                /* ordering malus for the pawn leaving the king's shield */   \
  P(KingShieldNpm, 3600, 0, 8000)        /* ...only if the opponent has at least this non-pawn material */ \
  P(OutpostOrder, 0, 0, 40000)              /* 08/10/2026 (user): ordering bonus for the knight going to an */ \
                                         /* outpost (defended by a pawn, never attackable by pawns) */     \
  P(OutpostSafe, 1, 0, 2)                /* 1: also without enemy bishops of the square's colour; 2: and without knights */ \
  /* Ordering with chess knowledge (08/10/2026, user, after the outposts), OFF by default. AttackOrder            \
     and AttackOrderQ: the idea of rewarding the quiet move that attacks from a safe square comes from Reckless    \
     (published SPRTs +3.7 and +5.7 at 40+0.4); here with a single rule: target = enemy piece (not pawn, not king) \
     heavier or undefended. In July, in the 7.0 search, -9.7 +- 10.4 over 1,290 games: retried on the new search.  \
     PassedPush (passer push), OutpostOrderB (bishop outpost), BishopPairCapt (capture of a bishop of the pair)    \
     are ours; PromoOrder (queen promotion first among the tactical moves) is also in Reckless. */                  \
  P(AttackOrder, 0, 0, 40000)               /* pawn, knight, bishop, rook attacking a target from a safe square */ \
  P(AttackOrderQ, 0, 0, 40000)              /* ...the queen (undefended targets)                               */ \
  /* AttackSafeSee (evening of 08/10/2026, check docs/audit_8.0/Y2_VERIFICA_ORDINAMENTO.md): with 0 the target       \
     square must be free of any enemy attack, but the target almost always sees the square it is attacked from        \
     (pawn on bishop, piece on queen: 40% of the attacks on heavier pieces never rewarded). With 1 the square must     \
     not be attacked by lower-valued pieces and the exchange on the square must not lose material (SEE >= 0). The     \
     SPRT of the trio (08/10, +0.39 +- 5.42) was with 0. */                                                           \
  P(AttackSafeSee, 1, 0, 1)                                                                               \
  P(PassedPush, 0, 0, 40000)                /* passed pawn advancing to a square that does not lose material */ \
  P(PassedPushRank, 6, 4, 7)             /* ...from the (relative) rank PassedPushRank up                  */ \
  P(OutpostOrderB, 0, 0, 40000)             /* bishop on an outpost (same mask as the knights)                */ \
  P(PromoOrder, 2412, 0, 40000)             /* queen promotion, added to the score of the tactical moves       */ \
  P(BishopPairCapt, 601, 0, 4000)          /* capture of a bishop when the opponent has the pair              */ \
  /* Audit X3 (08/10/2026, docs/audit_8.0/X3_DOVE_PERDIAMO.md), OFF by default: at 0 tree and timing identical.      \
     ProgressFade:                                                                                                    \
     extra damping with the fifty-move counter beyond ProgressFrom (12_quiesce.inc). TmDefendMul: more time           \
     when the root is defending. TmEarlySave: optimum share reduced with >= 24 pieces (14_deepen.inc). */             \
  /* ProgressFade/ProgressFrom: closed neutral and REMOVED in the cleanup of the evening of 11/10/2026 (see below, CLEANUP). */ \
  P(TmDefendMul, 500, 0, 1000)           /* BAKE 11/10/2026 (was 0, see the header): /1000, time x (1+Mul) in defence */ \
  P(TmDefendFrom, 178, 0, 2000)          /* root <= -From (178 ~ half a pawn)                              */ \
  P(TmDefendTo, 1424, 200, 4000)         /* ...and > -To (beyond it is lost: normal time)                  */ \
  /* TmDefendClock: adopted at 1000 on 30/10/2026 evening, back to 0 the same night (author: "facciamo solo           \
     TmSurpriseBehind"). Self-play 30+0.3 HT together with TmSurpriseBehind: -2.0 +- 7.0 / 2,114.                    */ \
  P(TmDefendClock, 0, 0, 1000)           /* 30/10/2026, OFF: /1000, defence extra x (1 - k x relative clock deficit) */ \
  P(TmDefendFade, 0, 0, 1000)            /* 30/10/2026, OFF: /1000 of the start clock below which the extra fades to 0 */ \
  P(TmEarlySave, 200, 0, 500)            /* BAKE 11/10/2026 (was 0): /1000, optimum share x (1-Save) with >= 24 pieces */ \
  /* Idea F (13/10/2026, user): time per network expert. Optimum share x (1 + TmExpK/1000) according to the expert   \
     of the root (net_phase: <=9, 10-15, 16-23, >=24 pieces), after TmEarlySave. 0 = unchanged. To be tried as a     \
     REDISTRIBUTION (more time in endgames, less in the opening): in self-play the side that spends more wins. */      \
  P(TmExp0, 0, -500, 1000)               /* 20/10: TMLOG data show the verdict stops a little early in endgames  */ \
                                         /* (move changes later 4.9% vs 3.2-4.1%, least time). 300 on the endgame */ \
                                         /* book, no draw adj, 10+0.1 noHT: +0.66 +- 2.16 over 5,226 (first 1,372 */ \
                                         /* +3.55): neutral, back to 0 (user). Kept for the TM tuning.              */ \
  P(TmExp1, 0, -500, 1000)                                                                                 \
  P(TmExp2, 0, -500, 1000)                                                                                 \
  P(TmExp3, 0, -500, 1000)                                                                                 \
  /* TmJumpGuard (11/10/2026, user, guard; OFF): if the root score rises with respect to the previous move          \
     by at least Guard PRINTED centipawns (units_to_cp, the scale seen in the GUI: +1.00 = 50% wins),                \
     falling does not drop below 1: no time cut on a suspicious jump. Tournament case 11/10, game 250 (audit         \
     X5): +0.37 -> +3.68 at depth 19, falling at its minimum 0.576, 1.7 s with 48 s on the clock, game lost (31.Nxf6?). \
     Only for big jumps (trial value 200 = two "printed pawns"); decided scores (mate, tablebase) excluded. */         \
  /* TmJumpGuard ON at 200 on the evening of 11/10/2026 (user: "we keep the guard, it changes neither the tree nor   \
     the behaviour except on big jumps"). SPRT UHO 8+0.08 (critical TC), tm32 (bench 325747, unchanged with the     \
     guard), TmFenPly=1 on both: -2.52 +- 7.33 over 2,210 games (sockets -1.89 / -3.14), flat. It fires in 0.25% of  \
     the moves (jump >= +2.00 over the previous move), in one game out of nine. */                                    \
  P(TmJumpGuard, 209, 0, 2000)           /* printed cp of jump for the guard (0 = off)                      */ \
  P(HashDamp, 41, 0, 200)                /* BAKE 06/10/2026 (was 0, see the header): cutoff from a lower  */ \
                                         /* bound pushed towards beta (tenths)                           */ \
  P(HashNearMiss, 0, 0, 400)            /* 12/10/2026, retry: entry one ply short, far below alpha      */ \
  P(HashFiftyStep, 0, 0, 50)            /* ours (05/10/2026): the hash key carries the band of the fifty- */ \
  P(HashFiftyFrom, 40, 0, 100)           /* move counter, one every Step plies above From (0 = off)      */ \
  P(PassedPushRed, 1125, 0, 4096)           /* endgames: less reduction and no pruning for the push of a */ \
  P(PassedPushNpm, 5133, 0, 12000)       /* passed pawn to the 6th/7th, below this non-pawn material     */ \
  /* Our ideas of 04/10/2026 (docs/audit_8.0/M_STUDIO_IDEE_RICERCA.md), all OFF by default:                 \
     at 0 the tree is identical (bench 141196) and in the release the code disappears. They are turned on one   \
     at a time in the SPRTs of the plan (STATO_PROGETTO.md 4.0quinquies). Removed on 05/10/2026 because          \
     negative (§3 of the state): 1 Disagree* (both signs), 4 PhaseEdgeCapMargin and PhaseEdgeImproving,         \
     5 PassedReplyRed, 6 NmpVerifyDepthEnd/Npm, 10 LateBranch, and HashNearMiss. */                             \
  P(DeepKnee, 16, 4, 40)                 /* idea 2 (ours, LTC): from this root depth...                  */ \
  P(DeepSpan, 12, 1, 40)                 /* ...over these plies the weight of the Deep* terms rises from 0 to 1 */ \
  P(DeepRfp, 0, -200, 200)               /* "deep" offsets (0 = off), searched by an SPSA LTC of ours    */ \
  P(DeepFut, 0, -200, 200)                                                                                \
  P(DeepLmr, 0, -2048, 2048)             /* /1024 of a ply */                                             \
  P(DeepNmp, 0, -2048, 2048)             /* /1024 of a ply */                                             \
  P(DeepSing, 0, -60, 60)                                                                                 \
  /* Evening of 08/10/2026 (docs/audit_8.0/Y1_RICERCA_PROFONDA.md), for the SPSA DEEP1: DeepRfp now outside the cap   \
     of the multiplier (inside, the cap triggered from depth 9 and the lever acted almost only one way); DeepNmp       \
     continuous (rounding with the key bits, a fraction |v|/1024 of the nodes changes by one ply); three new levers    \
     on the terms that grow in value with thinking time. All at 0 = tree unchanged. */                                 \
  P(DeepHist, 0, -800, 800)              /* /1024: weight of the history in the reduced depth of futility */      \
  P(DeepTtPv, 0, -2048, 2048)            /* /1024 of a ply: less reduction in ttPv nodes                  */      \
  P(DeepExt, 0, -120, 120)               /* margins of the double and triple singular extensions          */      \
  /* 10/10/2026, margins per network expert (ours; expert = piece band <=9, 10-15, 16-23, >=24, as               \
     net_phase): added to the reverse futility multiplier (RfpExp, base RfpMultBase 50) and to the margin of the     \
     quiet-move futility (FutExp). All at 0 = tree unchanged. BAKE EXP1 12/10/2026 (see the header): values of the   \
     SPSA EXP1, [EXP1 0->new]. LmrExp and LmrScaleExp removed on 18/10/2026 with block 2 and RESTORED on 19/10/2026  \
     with the return to the 8.0 reduction (RedForm 0): they act in the active reduction; RfA0-3, ReA0-3 only with RedForm. */ \
  P(RfpExp0, PSV(3, 0), -40, 40)         /* expert 0: <= 9 pieces  [EXP1 0->3]; Pos Solver 0 */           \
  P(RfpExp1, PSV(1, 0), -40, 40)         /* expert 1: 10-15 pieces [EXP1 0->1] */                         \
  P(RfpExp2, PSV(-3, 0), -40, 40)        /* expert 2: 16-23 pieces [EXP1 0->-3] */                        \
  P(RfpExp3, PSV(3, 0), -40, 40)         /* expert 3: >= 24 pieces [EXP1 0->3] */                         \
  P(FutExp0, PSV(-3, 0), -200, 200)      /* [EXP1 0->-3] */                                               \
  P(FutExp1, PSV(-14, 0), -200, 200)     /* [EXP1 0->-14] */                                              \
  P(FutExp2, PSV(-2, 0), -200, 200)      /* [EXP1 0->-2] */                                               \
  P(FutExp3, PSV(8, 0), -200, 200)       /* [EXP1 0->8] */                                                \
  P(LmrExp0, PSV(-25, 0), -1536, 1536)   /* [EXP1 0->-25] */                                              \
  P(LmrExp1, PSV(102, 0), -1536, 1536)   /* [EXP1 0->102] */                                              \
  P(LmrExp2, PSV(-6, 0), -1536, 1536)    /* [EXP1 0->-6] */                                               \
  P(LmrExp3, PSV(101, 0), -1536, 1536)   /* [EXP1 0->101] */                                              \
  /* 13/10/2026 (user: "the margins add up, should they not replace them?"): LmrExp only shifts the level of the     \
     reduction; LmrScaleExp also gives each expert the SLOPE: it scales the base reduction (log x log table,         \
     window, LmrBase) in 1024ths before the other corrections. 0 = tree unchanged. -64/96/0/96 +1.11 +- 3.65 over    \
     9,118 (8+0.08); LmrScaleExp1 -128 0.00 +- 6.03 over 3,160; under trial in the defence package (exp1fix). */      \
  P(LmrScaleExp0, 0, -256, 256)          /* /1024: +102 = base reduction +10%                             */ \
  P(LmrScaleExp1, 0, -256, 256)                                                                           \
  P(LmrScaleExp2, 0, -256, 256)                                                                           \
  P(LmrScaleExp3, 0, -256, 256)                                                                           \
  P(Contempt, 0, 0, 200)                 /* idea 3: draw = -c for the root colour (rating-list Elo) */    \
  /* DrawDefend (13/10/2026, user: "the dynamic draw when behind, you must implement it"). Measured against a reference \
     (tools/defense_vs_engine.py): we hold 37% of the worse positions, the reference 51%. If the previous move closed            \
     below -DrawDefendFrom, for the root colour the draw is worth up to +DrawDefend (linear ramp over                 \
     DrawDefendSpan, full beyond): the search prefers the lines that simplify, repeat or reach a fortress.             \
     It goes through contempt(), so it applies to repetitions, fifty moves, stalemate, insufficient material and       \
     tablebases. Internal units (178 ~ half a pawn). 0 = off, tree unchanged. */                                       \
  P(DrawDefend, 0, 0, 600)                                                                                \
  P(DrawDefendFrom, 356, 0, 3000)        /* ~1 pawn                                                      */ \
  P(DrawDefendSpan, 356, 1, 3000)        /* full bonus from ~2 pawns                                     */ \
  /* CLEANUP evening of 13/10/2026 (user: "push everything, except the dead code, which must go 100%"): CLOSED and   \
     off options, removed with their code; with the options off the tree was identical (bench 221809 before and      \
     after). Code in _backup/Triumviratus_8.0_pre_pulizia_morti_2026-10-13. Attempts and measurements                 \
     (STATO_PROGETTO.md, 12-13/10):                                                                                   \
     - AG1 (docs/audit_8.0/AG1_RICERCA_IDEE_20261012.md): KnowRedGood/Shield (ordering knowledge in the LMR)          \
       closed negative (256/256 -13.6 +- 13.6); EgCheckRed (quiet checks reduced less in endgames) neutral, in the     \
       SPSA EXP2 towards 0 (z -3.3); CausalRedDepth/CausalAgree/CausalNull, NmpExp/SingExp/PcExp0-3 (per-expert       \
       margins of null move, singular, ProbCut), ExpHistW/ExpHistLmrW (per-expert history), OppPasserRfp/Nmp: neutral \
       in the SPRTs and without trend in the SPSA EXP2 (43 levers, 1,440 iterations; mean vector -4.50 +- 7.26 over   \
       2,470).                                                                                                         \
     - Idea A TensionRed/Min (double threat reduced less) -1.2 +- 5.7; idea C ThreatMoveOrder/Red (reply to the        \
       threat of the failed null move) -2.1 +- 7.0; ExpertValuesW (per-expert piece values from Consilium in the SEE,  \
       MVV and capture futility) -4.4 +- 9.4; all without trend in EXP2.                                               \
     - Idea B LmrLearnW (LMR learned from the logs, lmr_learned.inc, instrumentation TRIUMV_LMRLOG) closed H0: -1600   \
       at -1.36 +- 1.44 over ~60,000.                                                                                  \
     - FutDepthExp0-3 (per-expert futility slope) without trend in EXP2.                                              \
     - NullThreatExt (null move failing by a lot -> +1 ply) -0.42 +- 4.52 over 5,900; TransitionExt (capture leaving  \
       only pawns -> +1 ply) closed again at the retry of 12/10.                                                       \
     - PvAhead (helper dedicated to the position at ply 2 of the PV) removed earlier: _backup/Triumviratus_8.0_pvahead_*. */ \
  /* CLEANUP evening of 11/10/2026 (user: "clean the code if we are sure about removing things, documenting the      \
     attempt"): CLOSED and off options, removed with their branch; with the options off the tree was identical (bench \
     325747 before and after). Code in _backup/Triumviratus_8.0_pre_pulizia_2026-10-11. Attempts and measurements:    \
     - AspDecisive (agent AE, PlentyChess/Reckless/Viridithas: aspiration fail high decided without reducing the      \
       depth): +0.7 at 7,000 games UHO 8+0.08, flat.                                                                   \
     - ExclClean (AE: in the singular verification node no history from the eval difference and no razoring, RFP,    \
       ProbCut and hash cutoff): =3 -0.39 +- 3.46 over 10,764, flat.                                                   \
     - NmpHashUpper (AE: no null move if the hash says value < beta): +0.2 at 7,000, flat.                            \
     - SingKeepCount (AE: move_count of the frame restored after the excluded search): -0.6 at 7,000, flat.           \
     - RepRootTwofold (AE: returning to the root position is already a draw): +0.59 +- 3.53 over 5,272 (8moves        \
       without adjudication), a choice of style, not a correction.                                                     \
     - RepCycleFifty (ours: cycle = draw only from a fifty-move counter): 20 -4.1 +- 9.4 at 8 s, 6 -9.5 +- 11.0.       \
     - QsPasserPush + QsPushNpm/See/Ply (idea 7: push to the 7th as a tactical move in quiescence): three flat        \
       trials, the last -2.04 +- 6.38 over ~2,900 (before +1.13 +- 3.79 and -0.77 +- 7.48).                            \
     - PhaseEdgeNoFut (idea 4: the capture that changes expert is not pruned): +1.34 +- 2.30 over 24,292, then in     \
       the package of the lean ones -0.1 +- 6.7 over 3,120.                                                            \
     - ProgressFade/From (audit X3: extra damping with the fifty-move counter): +0.32 +- 2.07 over 11,000 endgames.   \
     - BLEND BETWEEN EXPERTS (Z1/Z2, options EvalExpertBlendLo/Mid/Hi/Side/MinDepth, probe TRIUMV_EXPERT_PROBE),      \
       REMOVED ENTIRELY at 23:50: at the border between two experts (9|10, 15|16, 23|24 pieces) average of the two     \
       evaluations, the neighbour rebuilt from the finny table. Full +15.4% cycles mg / +17.8% eg, SPRT 8 s -24 +- 11  \
       over 1,034; at fixed nodes +5.3 +- 7.9 over 1,978; variant md3 (only at depth >= 3, +3.9% cycles) at 15 s       \
       -3.06 +- 9.59 over 1,248; tile variant (TRIUMV_BLEND_TILE) fewer instructions but IPC 1.38 -> 1.30, worse       \
       cycles. Reports docs/audit_8.0/Z1_CONFINI_ESPERTI_CALIBRAZIONE.md, Z2_BLEND_COSTO.md,                          \
       BLEND_ESPERTI_CHIUSURA.md; code in _backup/Triumviratus_8.0_pre_rimozione_blend_2026-10-11. */                  \
  /* CorrPhase (BAKE 06/10/2026, see the header; idea 9, pawn/minor corrections separate per expert): option       */ \
  /*     retired on 28/10/2026, the separation is fixed code (corr_phase_key, 09_history.inc).                     */ \
  /* CorrPhaseQueen (17/10/2026, off = old tables): with CorrPhase on, eight regimes (piece band x queens on the  */ \
  /* board) instead of four; same tables, different key (09_history.inc, corr_phase_key). Own SPRT before use.    */ \
  P(CorrPhaseQueen, 0, 0, 1)                                                                               \
  P(CorrFiftyStep, 0, 0, 50)             /* ours (05/10/2026): pawn and non-pawn correction separate     */ \
                                         /* per band of the fifty-move counter (Step plies, 0 = off)     */ \
  /* Audit of the core (05/10/2026): the adapters around the search (hash, SEE, null move, evaluation) reviewed   \
     one by one, each with an alternative form. */                                                                  \
  /* InsufMaterial: baked on 11/10/2026 and retired as an option (see the header). */                             \
  P(SeePinned, 1, 0, 1)                 /* BAKE 06/10/2026 (was 0, see the header): 1 = in the SEE pinned */ \
                                         /* pieces do not recapture while the pinner is there            */ \
  /* SeePromo (17/10/2026, review by Bastiii): baked and retired the same day. Promotions go to the SEE, which adds */ \
  /* the promotion gain (see_holds in 11_queue.inc, see.cpp). SPRT on endgames at 8+0.08, non-regression.          */ \
  /* HashQsDepth, NullFiftyKeep, EvalRefadeBridge: baked on 06/10 (see the header) and retired as options the same  \
     evening (fixed code in 12_quiesce.inc and 09_history.inc). SeePinned stays an option (CorrPhase retired 28/10), \
     candidate for an ablation at long TCs. */                                                                     \
  /* "Long time" ramp (05/10/2026): the selectivity levers move from the RW1 value (tuned at 20+0.2, root ~15)        \
     to the value of the SPSA LTC1 (40+0.4, root ~17) according to the root depth: weight 0 up to LtKnee, 1 from       \
     LtKnee + LtSpan up, with no extrapolation beyond. The Lt are offsets from the base value: at 0 identical tree,   \
     and in the release the code disappears. */                                                               \
  P(LtKnee, 15, 4, 40)                                                                                     \
  P(LtSpan, 2, 1, 20)                                                                                      \
  P(LmrLogMulLt, 0, -1000, 1000)                                                                           \
  P(RfpMaxDepthLt, 0, -10, 10)                                                                             \
  P(NmpVerifyDepthLt, 0, -10, 10)                                                                          \
  P(LmrNotImprovingLt, 0, -300, 300)                                                                       \
  P(LmrOffsetLt, 0, -1500, 1500)                                                                           \
  P(LmrAllNodeLt, 0, -400, 400)                                                                            \
  P(LmrNoTTFullLt, 0, -1500, 1500)                                                                         \
  P(NmpImprovingLt, 0, -100, 100)                                                                          \
  P(QuietFutAlphaLt, 0, -200, 200)                                                                         \
  /* CODE CONSTANTS TURNED INTO PARAMETERS (evening of 12/10/2026, user: "everything as variables, frozen in the       \
     release"). They were numbers written in the search code, almost all equal to those of Stockfish 19 (tools/       \
     search_attribution.py: 74 of 87 constants >= 100). Default = the old value: identical tree, bench unchanged.     \
     In the release they are constexpr like the other parameters; in dev the table of the reciprocals of HistDiv* is \
     recomputed at every setoption (param_tables_refresh, 13_search.inc). They serve the SPSAs.                      \
     Limits of the statistics (09_history.inc, hist_learn) */                                                    \
  P(HistLimQuiet, 7183, 2000, 20000)     /* quiet histories (butterfly, low ply, per expert)               */ \
  P(HistLimCapt, 10692, 3000, 30000)     /* capture history                                                */ \
  P(HistLimFollow, 30000, 8000, 32000)   /* continuation histories (int16: at most 32767)                  */ \
  /* Learner::followups (13_learn.inc): weight per distance 1..6 plies and factor by the number of distances already rewarding */ \
  P(ContDist1, 520, 0, 1500)                                                                               \
  P(ContDist2, 390, 0, 1500)                                                                               \
  P(ContDist3, 145, 0, 1000)                                                                               \
  P(ContDist4, 251, 0, 1000)                                                                               \
  P(ContDist5, 66, 0, 1000)                                                                                \
  P(ContDist6, 209, 0, 1000)                                                                               \
  P(ContAgree0, 94, 40, 200)                                                                               \
  P(ContAgree1, 103, 40, 200)                                                                              \
  P(ContAgree2, 110, 40, 200)                                                                              \
  P(ContAgree3, 106, 40, 200)                                                                              \
  P(ContAgree4, 119, 40, 200)                                                                              \
  P(ContAgree5, 126, 40, 200)                                                                              \
  P(ContAgree6, 121, 40, 200)                                                                              \
  /* G. Block 1, per expert: ContNear fixed reward of the continuation one ply back (was 73 for all);            */ \
  /*    ContChk how many continuations are updated when the node is in check (was 2).                            */ \
  P(ContNear0, 73, 0, 300)  P(ContNear1, 73, 0, 300)  P(ContNear2, 73, 0, 300)  P(ContNear3, 73, 0, 300)       \
  P(ContChk0, 2, 1, 6)      P(ContChk1, 2, 1, 6)      P(ContChk2, 2, 1, 6)      P(ContChk3, 2, 1, 6)           \
  /* Cutoff from the hash, block 5 in our form (17/10/2026; 13_search.inc, hash_cutoff). Defaults = old tree.  */ \
  /*  HashCutQuiet* reward to a quiet hash move, min(Mul*depth, Max); HashCutPrevMalus penalty to the previous  */ \
  /*  move when it came early, before move HashPrevLate (was 5).                                               */ \
  /*  HashLowSlack  plies of depth an upper bound may lack and still cut (was 1)                               */ \
  /*  HashAnyNode   depth above which an entry cuts whatever the node type expects (was 4)                    */ \
  /*  HashVerify    depth from which the child of the hash move must confirm the cutoff (was 7)               */ \
  /*  HashWeaken    depth above which an entry that contradicts the window loses a ply (was 5)                */ \
  /*  HashFiftyGuard fifty-move count from which no cutoff is taken (was 96)                                  */ \
  P(HashCutQuietMul, 112, 0, 400)                                                                          \
  P(HashCutQuietMax, 695, 0, 2500)                                                                         \
  P(HashCutPrevMalus, 2210, 0, 6000)                                                                       \
  P(HashPrevLate, 5, 1, 12)                                                                                \
  P(HashLowSlack, 1, 0, 3)                                                                                 \
  P(HashAnyNode, 4, 1, 10)                                                                                 \
  P(HashVerify, 7, 3, 16)                                                                                  \
  P(HashWeaken, 5, 1, 12)                                                                                  \
  P(HashFiftyGuard, 96, 80, 100)                                                                           \
  /* before the moves (13_search.inc) */                                                                      \
  P(PriorRedEvalSum, 166, 0, 500)        /* depth-- after a strong reduction if the two evals sum beyond  */ \
  P(EvalDiffLo, 189, 0, 600)             /* clamp(-(prev eval + eval), -Lo, Hi) + Ofs: history of the move */ \
  P(EvalDiffHi, 194, 0, 600)             /* that brought us here                                           */ \
  P(EvalDiffOfs, 60, -200, 200)                                                                            \
  /* singular extension (13_search.inc, singular) */                                                           \
  P(SingTtHistMul, 1371, 0, 4600)        /* first rung - and step + Mul * tt_move_hist / Div; 19/10/2026:  */ \
  P(SingTtHistDiv, 131072, 20000, 400000) /* Div 114178 -> 2^17 with Mul 1194 -> 1371 (same weight, +0.03%) */ \
  P(SingDoubleDeep, 39, 0, 150)          /* beyond the root depth: first rung lower                        */ \
  P(SingStepDeep, 8, -100, 150)          /* beyond the root depth: step smaller (ours, was Triple - Double) */ \
  P(SingFhTtBase, 404, 0, 1500)          /* tt_move_hist after the multi-cut: -(Base + Depth*depth)      */ \
  P(SingFhTtDepth, 109, 0, 400)                                                                            \
  /* move_loop (13_search.inc) */                                                                             \
  P(CaptStatMul, 873, 0, 2500)           /* stat_score of the captures: Mul * victim value / 128            */ \
  P(StatQuietW, 2252, 0, 6000)           /* stat_score of the quiet moves: (W*hist + C1*cont1 + C2*cont2)/1024 */ \
  P(StatCont1W, 1126, 0, 4000)                                                                             \
  P(StatCont2W, 1093, 0, 4000)                                                                             \
  P(LmrAllDivBase, 268, 1, 1500)         /* r * LmrAllNode / (256*depth + Base) in all nodes             */ \
  P(FollowFhBonus, 1334, 0, 4000)        /* follow-up reward when the re-search confirms the fail high     */ \
  P(FollowFhRed, 0, -200, 400)           /* ours (19/10): plus this per ply of reduction overcome, up to 8 */ \
  P(FollowFhMiss, 1334, -2000, 4000)     /* ours (19/10): reward when the re-search refutes it (old: same) */ \
  P(PvHashDepth, 1, 0, 4)                /* PV hash move: at least PvMinDepth plies if its entry is deeper */ \
  P(PvMinDepth, 1, 1, 3)                 /* than PvHashDepth (or holds a decided value)                    */ \
  P(LmrDeepRed1, 5234, 2000, 9000)       /* reduced search: -1 ply if r > Red1, -1 if r > Red2            */ \
  P(LmrDeepRed2, 5487, 2000, 9000)                                                                         \
  /* C2 regime selectivity (21/10/2026, ours; docs/rete/C2_IDEE_FABLE_2026-10-21.md ideas 7, 8, 11). Read only by    */ \
  /*  Consilium Plebis builds (13_search.inc, under TRIUMV_C2); other builds have the options and ignore them. OFF by     */ \
  /*  default (all 0 = identical tree); SPRT pending for each.                                                      */ \
  /*  C2SameRegImp (bits): comparisons of static evaluations across plies only inside one expert pair. 1 = improving */ \
  /*   against f[-4] when the pair changed since f[-2] but f[-4] has the pair of the node, else false (8 = else      */ \
  /*   true, variant 1b); 2 = opp_worse false when the pair changed at the last move; 4 = no PriorRed* depth steps    */ \
  /*   when the pair changed at the last move.                                                                       */ \
  P(C2SameRegImp, 0, 0, 15)                                                                                \
  /*  Imbalance (C2Imb*): d = our material class minus theirs, read from the two experts (each follows the other    */ \
  /*   side's non-pawn material). Ahead: d >= C2ImbGap and static eval > C2ImbEval; behind: d <= -C2ImbGap and eval */ \
  /*   < -C2ImbEval. Behind: checks and losing captures reduced less by C2ImbBehind (1/1024 ply), move-count        */ \
  /*   threshold x (1024 + C2ImbBehindLmp)/1024. Ahead: the exchange test of a capture of the opponent's last piece  */ \
  /*   of a type relaxed by C2ImbAhead (SEE units), the null-move floor lowered by C2ImbAheadNmp.                    */ \
  P(C2ImbBehind, 0, -3072, 3072)                                                                           \
  P(C2ImbBehindLmp, 0, -512, 2048)                                                                         \
  P(C2ImbAhead, 0, -400, 800)                                                                              \
  P(C2ImbAheadNmp, 0, -200, 400)                                                                           \
  P(C2ImbGap, 1, 1, 7)                                                                                     \
  P(C2ImbEval, 0, -400, 1000)                                                                              \
  /*  C2SettleHorizon: a capture or promotion that switches an expert of either perspective and would leave its     */ \
  /*   child to the quiescence search gets one ply instead, once per line; 1 = not in all nodes, 2 = in all nodes.   */ \
  P(C2SettleHorizon, 0, 0, 2)                                                                              \
  /* REMOVED 22/10/2026 (user): block 2 of our own - RedForm (Rf and Re levers, fitted power-law reduction), RfVolatile,*/ \
  /* RfLearn/RfLearnC/RfLearnOwn (reduction learned from outcomes). Never above parity: RedForm alone -1.8 +- 7.6,   */ \
  /* 12 s -3.05 +- 4.74, with the own ordering and RfLearnOwn -27.8 +- 14.7 over 602 (q37). The reduction stays the  */ \
  /* inherited formula; own terms enter it one at a time (OrdRedK, removed with the own ordering form). Code in       */ \
  /* _backup/Triumviratus_8.1_pre_rimozione_lmr_nostro_2026-10-22; tools redfit_engine.py and redout_fit.py keep the fits. */ \
  /* Reduction levers of our own (19/10/2026, all off; 13_search.inc before neg_cap). Each lowers the reduction,   */ \
  /* the sum stops at zero. Units 1/1024 ply: RdDisagree per 1024 of |psqt - positional| (up to 8), RdBoundCapt   */ \
  /* once for a capture into another expert, RdKingDanger per attacked square next to our king (up to 4).          */ \
  /* RdKingAtt (king-zone hits of the moved piece) REMOVED 28/10/2026: no positive value on 19/10.                  */ \
  P(RdDisagree, 0, -1024, 1024)             /* 19/10/2026: 1024 -4.94 +- 6.16 (3,308, 8+0.08 HT, UHO), stopped. */ \
                                            /* |psqt - positional| on game positions: median 239, 90% 961.      */ \
  P(RdBoundCapt, 0, -3072, 3072)            /* 19/10/2026: 3072 +1.54 +- 5.89 (3,394, 8+0.08 HT, UHO). With     */ \
                                            /* RdDisagree=-512: +0.75 +- 3.25 (11,652), lean, to be closed.     */ \
  P(RdKingDanger, 0, -1024, 1024)                                                                            \
  P(RdPawnBreak, 0, -3072, 3072)            /* once for a pawn break in a closed structure (>= 3 blocked).     */ \
                                            /* CLOSED 19/10/2026: 2048 +0.19 +- 5.74 (3,724, 8+0.08 HT, UHO),  */ \
                                            /* +0.61 +- 4.80 pooled with 7+0.07 (5,142); 3072 8moves -3.65.    */ \
                                            /* Breaks occur in 6% of game positions, 30% of games; played      */ \
                                            /* breaks almost unchanged (597 vs 569): the search finds them.    */ \
  /* end of the node (13_search.inc, node_close) */                                                           \
  P(TtMoveHistGood, 918, 0, 3000)        /* tt_move_hist: + if the best is the hash move, - otherwise     */ \
  P(TtMoveHistBad, 747, 0, 3000)                                                                           \
  P(PriorScaleBase, 241, -500, 1000)     /* no move above alpha: reward to the opponent's move            */ \
  P(PriorScaleStatDiv, 98, 10, 500)                                                                        \
  P(PriorScaleDepthMul, 59, 0, 300)                                                                        \
  P(PriorScaleDepthMax, 420, 0, 1500)                                                                      \
  P(PriorScaleLate, 186, 0, 600)                                                                           \
  P(PriorScaleEval, 142, 0, 600)                                                                           \
  P(PriorScaleEvalMargin, 106, 0, 400)                                                                     \
  P(PriorScaleOppEval, 159, 0, 600)                                                                        \
  P(PriorScaleOppMargin, 68, 0, 400)                                                                       \
  P(PriorBonusDepthMul, 150, 0, 500)                                                                       \
  P(PriorBonusSub, 85, -300, 500)                                                                          \
  P(PriorBonusMax, 1337, 200, 4000)                                                                        \
  P(PriorContW, 263, 0, 1500)            /* /16384 */                                                     \
  P(PriorQuietW, 215, 0, 1500)           /* /32768 */                                                     \
  P(PriorPawnW, 324, 0, 1500)            /* /8192 */                                                      \
  /* H. Block 1, per expert, the previous move when no move raised alpha: PrLate move number from which it counts */ \
  /*    as late (was 9); PrGain gain on the reward scale (/1024); PrCapt reward of the opponent's capture that    */ \
  /*    led here (was 892 for all).                                                                              */ \
  P(PrLate0, 9, 2, 30)     P(PrLate1, 9, 2, 30)     P(PrLate2, 9, 2, 30)     P(PrLate3, 9, 2, 30)                 \
  P(PrGain0, 1024, 256, 2048) P(PrGain1, 1024, 256, 2048) P(PrGain2, 1024, 256, 2048) P(PrGain3, 1024, 256, 2048) \
  P(PrCapt0, 892, 0, 3000) P(PrCapt1, 892, 0, 3000) P(PrCapt2, 892, 0, 3000) P(PrCapt3, 892, 0, 3000)             \
  P(CorrLearnW, 1061, 0, 3000)           /* /1024: weight of the correction update at the end of the node  */ \
  /* Quiet-history pruning, divisor per depth 1..16 (13_search.inc make_hist_div, prune_quiet): our parabola       */ \
  /*  HistDivMid + HistDivCurve*(d-8)^2/64 + HistDivSlope*(d-8)/8, times (1024 + HistDivScale + HistDivTilt*(d-8))   */ \
  /*  /1024, clamped to [256, 8192]. Adopted with the P2 bake of 01/11/2026 (HistDivForm 1); the inherited 16-entry  */ \
  /*  table HistDiv1-16 and the HistDivForm switch were removed the same day (source in _pre_rimozione_histdiv).     */ \
  P(HistDivScale, 0, -512, 1024)                                                                           \
  P(HistDivTilt, 0, -64, 64)                                                                               \
  P(HistDivMid, 3113, 1500, 6000)                                                                          \
  P(HistDivCurve, 355, -1000, 2000)                                                                        \
  P(HistDivSlope, 22, -800, 800)                                                                            \
  /* Quiescence (12_quiesce.inc): a value above beta is pulled toward it, (Mix*best + (1024-Mix)*beta)/1024.   */ \
  /*  QsFhMix when the stand alone already fails high; QsStandMix for the value the node returns at the end.    */ \
  P(QsFhMix, 441, 0, 1024)                                                                                 \
  P(QsStandMix, 462, 0, 1024)                                                                              \
  /* PIECE VALUES (28/10/2026, author: one value per piece per expert, replacing the inherited values everywhere;  */ \
  /*  see.cpp): value = SeeVal<type> + C2Val<type><expert>, the expert that reads the owner's material. One table   */ \
  /*  for the exchange evaluation, the futility and quiescence margins, the capture order, the threat terms, the    */ \
  /*  material thresholds and the stat score of a capture (unit_value of the search is gone). Bases at the old     */ \
  /*  table 208/781/825/1276/2538 (inherited values) and offsets at 0 = the old tree. The C2SeeReg switch (offsets   */ \
  /*  in some places only) was removed: partial form +0.56 +- 5.19 over 4,352 games at 8+0.08 HT, neutral.          */ \
  /*  BAKED 28/10/2026: the knight, bishop, rook and queen offsets measured on the final network (removal of every   */ \
  /*  piece of 8,000 positions, match_moe/piece_values/REPORT_ALL_Plebis_finale.md), +0.35 +- 2.04 over 30,104 games */ \
  /*  at 4+0.04 noHT on [-1, 2] (author's rule: H1 or positive at 30,000). Pawn offsets and pawn structure (below)  */ \
  /*  on the new base: H0, -8.00 +- 4.06 over 7,688 games at the same conditions; they stay 0.                    */ \
  /*  UNBAKED 28/10/2026 (author, for the pre-release): 8.1 vs 8.0 at 30+0.3 gave +10.7 +- 8.0 over 1,756 games   */ \
  /*  with the offsets against +20.8 +- 9.4 for the 26-27/10 build without them; measured only at 4+0.04, so all  */ \
  /*  offsets back to 0 (bench 219346). The measured vector stays as an SPSA start: N -182 0 -56 166 -5 10 27 11, */ \
  /*  B -159 0 -72 243 -1 20 17 24, R 0 55 -21 0 57 26 -17 -28, Q 0 0 0 600 -120 -193 -43 46 (experts 0..7).        */ \
  P(SeeValP, 208, 100, 400) P(SeeValN, 781, 500, 1100) P(SeeValB, 825, 500, 1100)                          \
  P(SeeValR, 1276, 900, 1800) P(SeeValQ, 2538, 1800, 3400)                                                 \
  P(C2ValP0, 0, -150, 150) P(C2ValP1, 0, -150, 150) P(C2ValP2, 0, -150, 150) P(C2ValP3, 0, -150, 150)      \
  P(C2ValP4, 0, -150, 150) P(C2ValP5, 0, -150, 150) P(C2ValP6, 0, -150, 150) P(C2ValP7, 0, -150, 150)      \
  P(C2ValN0, 0, -300, 300) P(C2ValN1, 0, -300, 300) P(C2ValN2, 0, -300, 300) P(C2ValN3, 0, -300, 300)      \
  P(C2ValN4, 0, -300, 300) P(C2ValN5, 0, -300, 300) P(C2ValN6, 0, -300, 300) P(C2ValN7, 0, -300, 300)      \
  P(C2ValB0, 0, -300, 300) P(C2ValB1, 0, -300, 300) P(C2ValB2, 0, -300, 300) P(C2ValB3, 0, -300, 300)      \
  P(C2ValB4, 0, -300, 300) P(C2ValB5, 0, -300, 300) P(C2ValB6, 0, -300, 300) P(C2ValB7, 0, -300, 300)      \
  P(C2ValR0, 0, -400, 400) P(C2ValR1, 0, -400, 400) P(C2ValR2, 0, -400, 400) P(C2ValR3, 0, -400, 400)      \
  P(C2ValR4, 0, -400, 400) P(C2ValR5, 0, -400, 400) P(C2ValR6, 0, -400, 400) P(C2ValR7, 0, -400, 400)      \
  P(C2ValQ0, 0, -800, 800) P(C2ValQ1, 0, -800, 800) P(C2ValQ2, 0, -800, 800) P(C2ValQ3, 0, -800, 800)      \
  P(C2ValQ4, 0, -800, 800) P(C2ValQ5, 0, -800, 800) P(C2ValQ6, 0, -800, 800) P(C2ValQ7, 0, -800, 800)      \
  /*  Pawn structure in the value of a pawn taken (see.h pawn_struct_adj, 28/10/2026), SEE units, 0 = old values: */ \
  P(PawnIsoAdj, 0, -150, 150)            /* isolated (no own pawn on the side files)                     */  \
  P(PawnDblAdj, 0, -150, 150)            /* doubled (another own pawn on its file)                       */  \
  P(PawnSupAdj, 0, -150, 150)            /* supported (defended by an own pawn)                          */  \
  P(PawnPassAdj, 0, -150, 200)           /* passed, on its 5th or 6th rank                               */  \
  P(PawnR7Adj, 0, -100, 400)             /* on its 7th rank                                              */  \
  /* Endgame rule (report R2, 18/10/2026): at the first quiescence ply called by the main search, quiet moves that */ \
  /*  give check with SEE >= 0 are also searched when at most QsChecksPieces pieces are on the board (the quiesce */ \
  /*  QCHK form of analysis mode). 0 = off, identical tree.                                                        */ \
  P(QsChecksPieces, 0, 0, 32)                                                                              \
  /* C2 search ideas (21/10/2026, agent A): search levers that read what only Consilium Plebis gives (the expert of each */ \
  /*  perspective from the routing table of the net, the per-move expert change written by the make, the pawn-king   */ \
  /*  subnetwork). Code under TRIUMV_C2 in 09_history, 11_queue, 12_quiesce, 13_search: in other builds the options */ \
  /*  exist and do nothing. ALL OFF BY DEFAULT (identical tree; C2 bench 329525 ep19, 336968 ep22); SPRT pending. REPORT_A.md. */ \
  /*  On the net of 21/10 the routing follows the OPPONENT's non-pawn material (99.4% of the cells): the expert of   */ \
  /*  the side to move's perspective is 0 = opponent with at most a minor, 1 = a rook, 2 = two minors or rook+minor,  */ \
  /*  3 = a queen, 4 = 10-13 units, 5 = 14-18, 6 = 19-25, 7 = 26-31 (N=B=3, R=5, Q=9); the opponent's perspective     */ \
  /*  reads OUR material on the same scale.                                                                           */ \
  /*  1. Margins per C2 expert, added to the old per-band terms: RFP multiplier (as RfpExp), quiet futility (as       */ \
  /*     FutExp), reduction in 1/1024 ply (as LmrExp), null-move floor (as NmpExp). C2ExpSide picks the expert:        */ \
  /*     0 = side to move's perspective (opponent's material), 1 = opponent's perspective (our material).              */ \
  /*  BAKE 28/10/2026 (author, for the release): the margins of e5/e6/e7 from the rough SPSA of 25/10 on the new     */ \
  /*  network, used in every Plebis test since (vs Consilium +10.8/+10.3 at 100k, +11.2 at 30k): 5 = -1/2/-6/2,   */ \
  /*  6 = 4/22/-72/13, 7 = 0/-1/5/-3 (Rfp/Fut/Lmr/Nmp). A dedicated on/off SPRT goes into the SPSA phase.          */ \
  /*  UNBAKED 29/10/2026 (author): the on/off SPRT (T6, final net, base 273963, 8+0.08 noHT) gave the margins ON     */ \
  /*  -6.49 +- 8.89 over 1,446 g (socket 1 clearly negative), so all back to 0; the 32 margins are re-tuned from 0    */ \
  /*  by an SPSA with ranges of both signs (backup _backup/Triumviratus_8.1_pre_margini_e567_zero_2026-10-29).       */ \
  P(C2ExpSide, 0, 0, 1)                                                                                    \
  P(C2Rfp0, 0, -40, 40) P(C2Rfp1, 0, -40, 40) P(C2Rfp2, 0, -40, 40) P(C2Rfp3, 0, -40, 40)                  \
  P(C2Rfp4, 0, -40, 40) P(C2Rfp5, 0, -40, 40) P(C2Rfp6, 0, -40, 40) P(C2Rfp7, 0, -40, 40)                    \
  P(C2Fut0, 0, -200, 200) P(C2Fut1, 0, -200, 200) P(C2Fut2, 0, -200, 200) P(C2Fut3, 0, -200, 200)          \
  P(C2Fut4, 0, -200, 200) P(C2Fut5, 0, -200, 200) P(C2Fut6, 0, -200, 200) P(C2Fut7, 0, -200, 200)            \
  P(C2Lmr0, 0, -1536, 1536) P(C2Lmr1, 0, -1536, 1536) P(C2Lmr2, 0, -1536, 1536) P(C2Lmr3, 0, -1536, 1536)  \
  P(C2Lmr4, 0, -1536, 1536) P(C2Lmr5, 0, -1536, 1536) P(C2Lmr6, 0, -1536, 1536) P(C2Lmr7, 0, -1536, 1536)    \
  P(C2Nmp0, 0, -200, 200) P(C2Nmp1, 0, -200, 200) P(C2Nmp2, 0, -200, 200) P(C2Nmp3, 0, -200, 200)          \
  P(C2Nmp4, 0, -200, 200) P(C2Nmp5, 0, -200, 200) P(C2Nmp6, 0, -200, 200) P(C2Nmp7, 0, -200, 200)            \
  /*  2. Expert-switching captures (a capture or promotion after which the routing table gives another expert to    */ \
  /*     either perspective): ordering bonus in the main search, less reduction (1/1024 ply, positive = less, stops  */ \
  /*     at zero), and a wider futility margin for such captures in the quiescence (evaluation units).               */ \
  P(C2SwitchOrder, 0, -8192, 8192)                                                                         \
  /*     C2SwitchRed CLOSED NEUTRAL (22/10/2026, author): 1024 on the ep107 export, 5+0.05, both sockets, rose to  */ \
  /*     +3.53 +- 3.43 over 10,438 and fell to +0.81 +- 2.77 over 16,232 (s0 +2.0, s1 -0.4); baked for an hour and  */ \
  /*     taken out again. The network does not need the switching captures searched deeper. Stays 0.             */ \
  P(C2SwitchRed, 0, -3072, 3072)                                                                           \
  P(C2SwitchQs, 0, -400, 800)                                                                              \
  /*  3. Conversion push (the author's idea): when the side to move is ahead by more than C2ConvMargin on the blend   */ \
  /*     (C2ConvEvalW * static evaluation + C2ConvPkW * pawn-king skip) / 1024, a trade of pieces of the same class   */ \
  /*     gets an ordering bonus (C2ConvOrder) and less reduction (C2ConvRed, 1/1024 ply); when it is behind by the    */ \
  /*     margin the same trades get the opposite. C2ConvOrder and C2ConvRed switch it on; the other three only shape. */ \
  P(C2ConvOrder, 0, -8192, 8192)                                                                           \
  P(C2ConvRed, 0, -3072, 3072)                                                                             \
  P(C2ConvMargin, 300, 0, 3000)                                                                            \
  /*     The skip is small: in the bench nodes |skip| < 8 network units in 83% and < 16 in 98% (pawn ~ 208-356     */ \
  /*     search units), so C2ConvPkW must be ~8-16x to weigh like a fraction of a pawn (8192 = 8x).                  */ \
  P(C2ConvEvalW, 1024, 0, 4096)                                                                            \
  P(C2ConvPkW, 8192, 0, 32768)                                                                             \
  /*     Conversion push, second form (21/10/2026; OFF: the defaults below leave the blend above unchanged).         */ \
  /*     C2ConvPkSrc picks the pawn-king term of the blend: 0 = the linear skip x C2ConvPkW (above); 1 = the EXACT    */ \
  /*     effect of the subnetwork (cn::pk_effect, what it adds to the positional output; skip ~1% of it) x C2ConvEffW, */ \
  /*     computed for the node's own position (one tail pass at a fresh evaluation, else a forward pass at the move   */ \
  /*     loop; only when C2ConvOrder/C2ConvRed are on). Positional units ~ 1/1.35 search units: 1024 = 0.75 of a     */ \
  /*     static-evaluation point, 1347 = par. Bench 21/10: |effect| mean 74, 27% of evaluations above 100.           */ \
  /*     C2ConvExpMax ("an expert thinks the endgame is winning"): the static evaluation enters the blend only when   */ \
  /*     both perspectives use a C2 expert <= C2ConvExpMax (0-3 = endgame experts, the opponent with at most a lone   */ \
  /*     queen), so the push follows the opinion of an endgame expert; 7 = every expert (the first form).            */ \
  /*     The "ahead" rule is c2_conv_lean (consilium_bridge.h), shared with C2ConvW: ahead when blend > C2ConvMargin. */ \
  /*     Same judgement as C2ConvW: C2ConvEvalW 0, C2ConvPkSrc 1, C2ConvEffW 1024, C2ConvMargin = C2ConvFrom.        */ \
  P(C2ConvPkSrc, 0, 0, 1)                                                                                  \
  P(C2ConvEffW, 1024, 0, 8192)                                                                             \
  P(C2ConvExpMax, 7, 0, 7)                                                                                 \
  /*     C2ConvRoot (the author's idea, 21/10/2026 evening; OFF, SPRT pending): the judgement above costs ~11% of the */ \
  /*     cycles per node with the exact effect (xperf, middlegame). Here the network decides only at the root, with   */ \
  /*     the exact pawn-king effect (x C2ConvEffW, whatever C2ConvPkSrc says), the experts (C2ConvExpMax) and a value: */ \
  /*     1 = the root's static evaluation, once per search; 2 = the score of the last completed iteration, again at   */ \
  /*     every iteration (the first one uses the static evaluation). The decision (root side ahead +1 / behind -1)    */ \
  /*     holds in the whole tree: +d at the root side's nodes, -d at the opponent's, with no network work per node.   */ \
  P(C2ConvRoot, 0, 0, 2)                                                                                   \
  /*  4. Pawn history per expert: the key of the pawn-structure history gets a constant per expert (1 = the side to */ \
  /*     move's expert, 2 = the pair of experts), as CorrPhase does for the corrections. 0 = one table as before.    */ \
  P(C2PawnHist, 0, 0, 2)                                                                                   \
  /*  5. Pawn-king volatility: d = change of the pawn-king skip (mover's view) made by a pawn or king move or a     */ \
  /*     pawn capture, clamped to +-C2PkScale. Reduction (1/1024 ply at d = C2PkScale): C2PkRed signed (moves the     */ \
  /*     subnetwork likes are reduced less, the others more), C2PkVol on |d|; C2PkOrder orders quiet pawn and king   */ \
  /*     moves. In the bench |d| < 8 in 57% of these moves, < 16 in 84%, < 32 in 99% (network units): scale 32.       */ \
  P(C2PkRed, 0, -3072, 3072)                                                                               \
  P(C2PkVol, 0, -3072, 3072)                                                                               \
  P(C2PkOrder, 0, -16384, 16384)                                                                           \
  P(C2PkScale, 32, 4, 1024)                                                                                \
  /*  5b. PkCrit (28/10/2026, ours; author: "the network values the pawns but the signal does not enter the search"):*/ \
  /*     a node is decided by the pawn structure when the pawn-king subnetwork adds at least PkCritFrom to the        */ \
  /*     positional output (|cn::pk_effect|, side to move, positional units; bench 21/10: mean 74, 27% above 100).   */ \
  /*     Read only right after a fresh network evaluation of the node (one tail pass, nn_pk_effect_fresh); a node    */ \
  /*     whose evaluation comes from the hash is not marked. In a marked node: PkCritRed = less reduction for every  */ \
  /*     pawn move (1/1024 ply; positive down to zero at most, negative = more), PkCritPrune 1 = quiet pawn moves    */ \
  /*     skip the quiet gate (history prune, futility, exchange test). Both 0 = off, identical tree.                 */ \
  /*  5c. Pawn breaks (28/10/2026, ours; author: "implement something that changes the behaviour on pawn breaks and  */ \
  /*     test it directly"). Study of 28/10 (match_moe/pawn_study, reference Stockfish dev at 20M nodes, partial): a    */ \
  /*     break is the best move the search finds least (37.5% at depth 14, 50% at 20, settled only from depth 13.5)  */ \
  /*     and the costliest to miss (61 cp), only 8 positions so far. A break = a quiet pawn push that attacks an enemy  */ \
  /*     pawn from its new square, in any structure (RdPawnBreak, closed structures only, was neutral on 19/10).        */ \
  /*     BreakOrder: ordering bonus of the quiet stage; BreakRed: less reduction (1/1024 ply, positive down to zero at  */ \
  /*     most, negative = more); BreakPrune 1: breaks skip the quiet gate. All 0 = off, identical tree.               */ \
  P(BreakOrder, 0, -16384, 16384)                                                                          \
  P(BreakRed, 0, -3072, 3072)                                                                              \
  P(BreakPrune, 0, 0, 1)                                                                                   \
  /*  5d. Trust in the cuts by node type and by the side's standing (28/10/2026, ours; search-ideas agent, prunelog of */ \
  /*     27/10, 3M nodes and 14.7M pruned moves each checked by a probe, match_moe/idee_ricerca_2026-10-28; STATO     */ \
  /*     §0.2 C1-C4). All 0 = off, identical tree; to be tuned by a short SPSA, then SPRT. A high error ratio is not  */ \
  /*     Elo by itself (RfpDisagree: x1.53 errors, -6.6 Elo).                                                         */ \
  /*     RfpBehind/RfpBehindD/RfpBehindFrom: reverse futility of a side far behind (static < -700: wrong x2.1 / x2.9 / */ \
  /*     x5.1 at depths 1-2 / 3-5 / 6-12); RfpAll: reverse futility in ALL nodes (x2.73 at every depth).              */ \
  P(RfpBehind, 0, 0, 2048)                                                                                 \
  P(RfpBehindD, 0, 0, 256)                                                                                 \
  P(RfpBehindFrom, 150, 0, 1000)                                                                           \
  P(RfpAll, 0, -100, 400)                                                                                  \
  /*     Move pruning by node type (CUT wrong x1.8-2.3, ALL x0.4, every filter) and with static >= alpha (LMP x6.1,     */ \
  /*     history prune x3.0, quiet exchange test x2.7). Positive = prune less. LMP threshold /1024 (LmpCut, -LmpAll,  */ \
  /*     LmpAbove); history threshold per ply (HistCut, -HistAll, HistAbove); quiet futility margin (FutCut, -FutAll);*/ \
  /*     quiet exchange test, extra loss accepted per ply when static >= alpha (SeeAbove); capture futility           */ \
  /*     (CaptFutCut, -CaptFutAll).                                                                                  */ \
  P(LmpCut, 0, -512, 2048) P(LmpAll, 0, -512, 2048) P(LmpAbove, 0, -512, 2048)                             \
  P(HistCut, 0, -2000, 4000) P(HistAll, 0, -2000, 4000) P(HistAbove, 0, -2000, 4000)                       \
  P(FutCut, 0, -200, 300) P(FutAll, 0, -200, 300)                                                          \
  P(SeeAbove, 0, -40, 120)                                                                                 \
  P(CaptFutCut, 0, -200, 300) P(CaptFutAll, 0, -200, 300)                                                  \
  /*  5e. TbDrawSearch (28/10/2026, ours; evaluation-ideas agent, STATO §0.2 T8): a tablebase draw at the root with    */ \
  /*     more than one drawing move: search among those moves (in-search probe off) instead of playing the DTZ move,  */ \
  /*     to keep chances against an opponent without tablebases. 0 = the DTZ move at once (old behaviour).           */ \
  P(TbDrawSearch, 0, 0, 1)                                                                                 \
  P(PkCritFrom, 150, 0, 4000)                                                                              \
  P(PkCritRed, 0, -3072, 3072)                                                                             \
  P(PkCritPrune, 0, 0, 1)                                                                                  \
  /*  6. (C2NmpMask, no null move per expert, became NmpNpmFloor on 22/10/2026: it only read material.)             */ \
  /*  7. Asymmetric experts: the two perspectives use different experts (the routing table separates the material */ \
  /*     of the two sides): RFP margin plus C2AsymRfp (evaluation units), reduction minus C2AsymLmr (1/1024 ply).     */ \
  P(C2AsymRfp, 0, -400, 400)                                                                               \
  P(C2AsymLmr, 0, -3072, 3072)                                                                             \
  /* between one iteration and the next (14_deepen.inc) */                                                    \
  P(LowPlyFill, 102, -500, 500)          /* initial value of the low-ply history at every search            */ \
  P(QuietHistDecay, 729, 0, 1024)        /* /1024: quiet histories kept between moves                        */ \
  /* --- C2 ordering (agent ORDER, 21/10/2026): ideas 5 and 6 of C2_IDEE_FABLE_2026-10-21. OFF by default; SPRT/SPSA */ \
  /*  pending. Code under TRIUMV_C2 in 09_history.inc and 11_queue.inc; in other builds the options do nothing.       */ \
  /*  Experts (block "C2 search ideas" above): e_us = expert of the side to move's perspective, which reads the        */ \
  /*  OPPONENT's non-pawn material (0 = at most a minor ... 7 = 26-31); e_them reads OUR material on the same scale.  */ \
  /*  5. Regime-gated chess-knowledge ordering. With C2OrdReg 1 each baked ordering term is multiplied by a scale     */ \
  /*     per expert (/1024; 1024 = the term as it is, so C2OrdReg 1 with every scale at 1024 is the identical tree): */ \
  /*     C2KsExp[e_us]      KingShield (our shelter matters when the opponent has attacking material);               */ \
  /*     C2AtkExp[e_them]   AttackOrder, AttackOrderQ (our attacks matter when we have the material);                */ \
  /*     C2PassExp[e_us]    PassedPush, PromoOrder (a passer runs when the opponent has little to stop it);          */ \
  /*     C2MinorExp[e_them] OutpostOrder, OutpostOrderB, BishopPairCapt (whether we still have the minors).          */ \
  /*     The reduction credits (PassedPushRed with PassedPushNpm) live in 13_search.inc: not gated here;   */ \
  /*     c2_reg_scale (09_history.inc) serves them when they are.                                                     */ \
  P(C2OrdReg, 0, 0, 1)                                                                                     \
  P(C2KsExp0, 1024, 0, 4096) P(C2KsExp1, 1024, 0, 4096) P(C2KsExp2, 1024, 0, 4096)                        \
  P(C2KsExp3, 1024, 0, 4096) P(C2KsExp4, 1024, 0, 4096) P(C2KsExp5, 1024, 0, 4096)                        \
  P(C2KsExp6, 1024, 0, 4096) P(C2KsExp7, 1024, 0, 4096)                                                   \
  P(C2AtkExp0, 1024, 0, 4096) P(C2AtkExp1, 1024, 0, 4096) P(C2AtkExp2, 1024, 0, 4096)                     \
  P(C2AtkExp3, 1024, 0, 4096) P(C2AtkExp4, 1024, 0, 4096) P(C2AtkExp5, 1024, 0, 4096)                     \
  P(C2AtkExp6, 1024, 0, 4096) P(C2AtkExp7, 1024, 0, 4096)                                                 \
  P(C2PassExp0, 1024, 0, 4096) P(C2PassExp1, 1024, 0, 4096) P(C2PassExp2, 1024, 0, 4096)                  \
  P(C2PassExp3, 1024, 0, 4096) P(C2PassExp4, 1024, 0, 4096) P(C2PassExp5, 1024, 0, 4096)                  \
  P(C2PassExp6, 1024, 0, 4096) P(C2PassExp7, 1024, 0, 4096)                                               \
  P(C2MinorExp0, 1024, 0, 4096) P(C2MinorExp1, 1024, 0, 4096) P(C2MinorExp2, 1024, 0, 4096)               \
  P(C2MinorExp3, 1024, 0, 4096) P(C2MinorExp4, 1024, 0, 4096) P(C2MinorExp5, 1024, 0, 4096)               \
  P(C2MinorExp6, 1024, 0, 4096) P(C2MinorExp7, 1024, 0, 4096)                                             \
  /*  6. Structure history: a quiet-move history [piece][to] keyed by the pawn-king signature of Consilium Plebis (the    */ \
  /*     signs of the C2StructHistBits most variable pawn-king outputs against their medians, side to move's view;   */ \
  /*     cache hit only, the subnetwork is never run for it: a miss gives no term and no update). Ordering only      */ \
  /*     (score_quiets: + C2StructHistW/1024 x entry); learned with the other quiet histories (x C2StructHistUpd     */ \
  /*     /1024, pawn-history limit, gravity of the node) while C2StructHistW is not 0. One table shared by the       */ \
  /*     threads, 4096 x 12 x 64 int16 = 6 MB on large pages, allocated in C2 builds only.                           */ \
  P(C2StructHistW, 0, -4096, 4096)                                                                         \
  P(C2StructHistUpd, 1024, 0, 4096)                                                                        \
  P(C2StructHistBits, 12, 1, 12)                                                                           \
  P(TmEffortLo, 82932, 40000, 100000)    /* share of the nodes on the best move (x100000): time ramp      */ \
  P(TmEffortHi, 101788, 60000, 140000)                                                                     \
  /* time management (in thousandths) */                                                                  \
  P(TmFallBase, 1170, 500, 2500)         /* /100: fallingEval = (base + a*(prev-best) + b*(iter-best)) */ \
  P(TmFallPrev, 240, 0, 600)                                                                              \
  P(TmFallIter, 114, 0, 400)                                                                              \
  P(TmInstabBase, 1091, 500, 2000)                                                                        \
  P(TmInstabMul, 2120, 0, 5000)                                                                           \
  P(TmFenPly, 0, 0, 1)                   /* 1: game ply = move number of the FEN + moves played           */ \
  /* Expected reply (ours, 05/10/2026, user's idea): if the opponent plays the reply of our principal variation,    \
     the hash is already warm and the move we had foreseen usually holds. The time for the move is reduced, but    \
     only when the search has found again the previous depth (minus TmExpectDepth), the best move is the expected  \
     one and the evaluation has not dropped beyond TmExpectMargin. At 1000 off: identical timing. */                \
  P(TmExpectScale, 1000, 200, 1000)      /* /1000: factor on the time for the move (1000 = off) */         \
  P(TmExpectDepth, 2, 0, 8)              /* from the depth of the previous move minus this value */        \
  P(TmExpectMargin, 70, 0, 400)          /* maximum drop of the evaluation, margin units (70 ~ 20 cp) */   \
  /* Surprise (ours, 05/10/2026, user's idea; the cue is an observation by Mark Tang on Stoofvlees, which           \
     replies instantly between two moves even at long TC): the reverse of the expected reply. If the opponent does   \
     NOT play the reply of our principal variation, the position is less clear than we thought: the time for the    \
     move x TmSurpriseScale/1000. The time the stability rule saves on expected replies thus goes to the uncertain   \
     moves. At 1000 off: identical timing.                                                                           \
     ⚠️ IT DEPENDS STRONGLY ON THE TC (user, 05/10/2026): x1.2 neutral at 16+0.16 (+0.7 +- 7.1 over 2,446), positive at \
     40+0.4 (+6.23 +- 6.75 over 2,174, from the PGNs +0.35-0.4 ply on the moves after a surprise). If it enters an SPSA \
     it must be tuned AT THE TARGET TC (long), or kept fixed: at short TC the SPSA would push it towards 1000. */     \
  P(TmSurpriseScale, 1200, 1000, 2000)   /* BAKE 05/10/2026 (was 1000, see the header): x1.2 after a surprise */ \
  /* Threshold (05/10/2026, user): the surprise counts only while the clock is still above this percentage of        \
     the one at the start of the game. With little time one cannot stop and think: play at the optimum time. At       \
     16+0.16 without threshold, x1.2: +0.7 +- 9.0 over 1,480 games (from the PGNs: surprises ~25% of the moves up to  \
     move 50, 52% in endgames, where the clock is shorter). With threshold 40 and x1.6 (one ply more on surprises) at  \
     20+0.2: -10.1 +- 7.5 over 1,998 games, closed: the time taken in the middlegame costs more than it returns.       \
     0 = always. */                                                                                                   \
  P(TmSurpriseClock, 0, 0, 100)          /* % of the starting clock below which the surprise is silent */  \
  /* TmSurpriseBehind (30/10/2026, author: "se siamo sopra pensa di piu' in caso di sorpresa, se siamo sotto la       \
     disattiva"): the surprise is silent while our clock is behind the opponent's by more than this share of theirs  \
     (/1000); 1 = any deficit above 0.1%. Replay of 8 lost games (573 moves, match_moe/rigioca_tempo.py): in defence  \
     the surprise fired on 32% of the moves, which cost 3.1x the target against 1.6x for the others and took 40% of   \
     the defence time. Adopted at 1 the same evening, then back to 0 (author: "scegli una da tenere"): together with  \
     TmDefendClock 1000, self-play 30+0.3 HT -2.0 +- 7.0 / 2,114. ALONE: self-play 25+0.25 HT on avt-fringe-2026-eq   \
     +0.94 +- 5.30 / 2,578 (socket 0 -5.9, socket 1 +7.8). ADOPTED AT 1 on 30/10/2026 21:11 by the author ("sappiamo  \
     che non perde, ma almeno risolviamo un po' quel problema"): neutral, and it removes part of the clock spent while  \
     behind. 0 = the old rule (surprise whatever the clock).                                                          */ \
  P(TmSurpriseBehind, 1, 0, 1000)        /* /1000: relative clock deficit beyond which the surprise is silent */ \
  /* Graded surprise (09/10/2026, proposal 5 of the novelties register, user: "develop it and keep it off"). The     \
     surprise rule is yes/no; here the extra time depends on how much the unexpected reply changed the game: d =     \
     score expected at the end of the previous search (same perspective, two plies later) minus the root score at    \
     this iteration. Weight w = clamp(1 + d / TmSurpriseGrade, 0, 2): d = 0 -> w = 1 (as today); the opponent found  \
     something (d = +Grade) -> w = 2, double extra time; the opponent erred (d = -Grade) -> w = 0, no extra time.     \
     Time x (1 + (TmSurpriseScale/1000 - 1) x w). Recomputed at every iteration, like TmExpect*. The rank of the      \
     reply among the opponent's moves is not known (the search orders only our root moves): the score is the         \
     available measure. 0 = off (yes/no rule). To be measured at 40+0.4, where the surprise paid off; trial value     \
     300 internal units (~85 cp). */                                                                                  \
  P(TmSurpriseGrade, 0, 0, 2000)         /* internal units: d for the double weight (0 = off)              */  \
  P(TmBehindMul, 600, 0, 2000)           /* /1000 (07/10/2026, user's idea): with less clock than the          */ \
                                         /* opponent the optimum shrinks by TmBehindMul x relative deficit   */ \
                                         /* ((us - them) / (us + them), only if negative); 0 = off           */ \
                                         /* BAKED 07/10 at 600: 6+0.06 +5.23 +- 4.72 / 6,378 (zero excluded, */ \
                                         /* s0 +2.5 s1 +8.0); 30+0.3 neutral +0.36 +- 5.13 / 3,876. Bench same. */ \
  P(TmSingleMs, 500, 0, 2000)            /* 19/10/2026 (user): at most this many ms with one legal move    */ \
  P(TmRedLo, 481, 0, 3000)               /* /100: ramp of time_red on the stability of the best move       */ \
  P(TmRedHi, 1984, 500, 4000)            /* /100 (report Q2_TEMPO_INSTABILITA: the only factor that tells    */ \
                                         /* quiet and critical positions apart; trial at 1200 = shorter ramp, 40+0.4) */ \
  P(TmMaxClock, 8097, 6500, 8800)        /* /1e4: the maximum does not exceed this fraction of the clock */ \
  /* Time management in our form (20/10/2026): the remaining constants of the verdict (14_deepen.inc) as        */     \
  /*  parameters. The inherited budget (power law of the share, opening factor, horizon of 50 moves, movestogo   */    \
  /*  branch) and its 24 constants were REMOVED on 21/10/2026 (user): budget_own is the only budget (copy in      */    \
  /*  _backup/Triumviratus_8.0_pre_bake_tv1_2026-10-21).                                                          */   \
  P(TmPonderDiv, 4, 1, 16)               /* with ponder the target grows by 1/this */ \
  P(TmPrevRed, 821, 500, 1500)           /* /1000: settle ramp assumed before the first move */ \
  P(TmFallMin, 636, 300, 1000)           /* /1000: clamp of the trend factor */ \
  P(TmFallMax, 1555, 1000, 3000)         /* /1000 */ \
  P(TmRedY0, 638, 300, 1500)             /* /1000: settle ramp, value at TmRedLo plies held */ \
  P(TmRedY1, 1570, 800, 3000)            /* /1000: value at TmRedHi plies held */ \
  P(TmRedMin, 593, 300, 1500)            /* /1000: clamp of the settle ramp */ \
  P(TmRedMax, 1650, 800, 3000)           /* /1000 */ \
  P(TmSettlePrev, 1601, 500, 3000)       /* /1000: settle = (this + previous ramp) / (div x ramp) */ \
  P(TmSettleDiv, 2277, 1000, 4000)       /* /1000 */ \
  P(TmEffortY0, 853, 500, 1500)          /* /1000: focus ramp, value at TmEffortLo */ \
  P(TmEffortY1, 732, 300, 1200)          /* /1000: value at TmEffortHi */ \
  P(TmEffortMin, 669, 300, 1200)         /* /1000: clamp of the focus ramp */ \
  P(TmEffortMax, 800, 500, 1500)         /* /1000 */ \
  P(TmDeepenShare, 528, 100, 1000)       /* /1000: past this share of the time the next iteration keeps its depth */ \
  P(TmTransMul, 500, 0, 2000)            /* /1000 (20/10/2026, ours): more time when the PV trades into another expert. */ \
                                         /* BAKED 20/10 at 500 (user): +2.35 +- 4.21 over 5,758 (10+0.1 noHT), ~1% of moves */ \
  P(TmTransPlies, 4, 1, 12)              /* PV moves whose captures are counted */ \
  P(TmTransRest, 0, 0, 500)              /* /1000: time given back by the other moves (keeps self-play level) */ \
  /* C2 ideas outside the search loop (21/10/2026, agent B). Read only by Consilium Plebis builds (-DTRIUMV_C2): without   */ \
  /*  it the options exist and do nothing. Every lever at default = identical behaviour (C2 bench 329525 ep19, 336968 ep22). */ \
  /*  OFF by default; SPRT pending for each. Code: 06_nndirty.inc (C2 signals), 09_history.inc (corrections),        */ \
  /*  14_deepen.inc (time), consilium_bridge.cpp (evaluation terms, pushed there by start of search, 15_threads.inc). */ \
  /*  TmC2Trans: what triggers TmTransMul/TmTransRest. 0 = the old piece bands (net_phase), 1 = the C2 expert of     */ \
  /*  either perspective differs after the first TmTransPlies moves of the PV (captures and promotions), 2 = either. */ \
  P(TmC2Trans, 0, 0, 2)                                                                                    \
  P(TmC2Edge, 0, 0, 1000)                /* /1000: share x (1 + Edge x share of the single non-pawn captures that */ \
                                         /* would switch a C2 expert at the root): more time near a routing edge  */ \
  P(TmC2Exp0, 0, -500, 1000)             /* /1000: share x (1 + TmC2ExpK/1000) by the C2 expert of the root's     */ \
  P(TmC2Exp1, 0, -500, 1000)             /* side to move (learned routing, not piece bands). For SPSA; all 0 =    */ \
  P(TmC2Exp2, 0, -500, 1000)             /* identical times                                                       */ \
  P(TmC2Exp3, 0, -500, 1000)                                                                               \
  P(TmC2Exp4, 0, -500, 1000)                                                                               \
  P(TmC2Exp5, 0, -500, 1000)                                                                               \
  P(TmC2Exp6, 0, -500, 1000)                                                                               \
  P(TmC2Exp7, 0, -500, 1000)                                                                               \
  P(CorrC2Key, 0, 0, 2)                  /* salt of the pawn/minor corrections (CorrPhase): 0 = piece band; 1 =   */ \
                                         /* C2 expert of the side to move; 2 = C2 expert pair (own, opponent)     */ \
  P(CorrPkW, 0, 0, 40000)                /* correction keyed by the pawn-king subnetwork's output, quantized to   */ \
                                         /* CorrPkBits signs (0 = off; table shared with CorrExpertW)            */ \
  P(CorrPkUpd, 128, 0, 400)              /* step of that table, /128                                              */ \
  P(CorrPkBits, 12, 4, 32)               /* signs of the pk outputs in the key (most variable outputs first)      */ \
  P(CorrPkCompute, 0, 0, 1)              /* 0: key only from the pawn-king cache (a miss = no term); 1: compute it */ \
  P(C2ConvW, 0, 0, 4096)                 /* endgame conversion (the author's idea): what the pawn-king subnetwork */ \
                                         /* adds to the positional output, beyond C2ConvFrom and up to C2ConvCap  */ \
                                         /* (positional units), x C2ConvW/1024 x share of non-pawn material      */ \
                                         /* already traded. Bench 21/10: |that| mean 74 (|positional| 415), 27%  */ \
                                         /* of evaluations above 100, 4.4% above 200                              */ \
  P(C2ConvFrom, 100, 0, 2000)            /* dead zone and cap of c2_conv_lean (consilium_bridge.h), the "ahead"   */ \
  P(C2ConvCap, 400, 0, 4000)             /* rule shared with the search version (C2ConvOrder/C2ConvRed)           */ \
  P(C2EvalExp0, 0, -256, 256)            /* /1024: evaluation x (1 + K/1024) by the C2 expert of the side to move */ \
  P(C2EvalExp1, 0, -256, 256)            /* (both network heads, before the evaluation constants). For SPSA      */ \
  P(C2EvalExp2, 0, -256, 256)                                                                              \
  P(C2EvalExp3, 0, -256, 256)                                                                              \
  P(C2EvalExp4, 0, -256, 256)                                                                              \
  P(C2EvalExp5, 0, -256, 256)                                                                              \
  P(C2EvalExp6, 0, -256, 256)                                                                              \
  P(C2EvalExp7, 0, -256, 256)                                                                              \
  P(C2StmOffset, 0, -64, 64)             /* 26/10/2026, OFF: subtracted from the C2 static evaluation (network    */ \
                                         /* units, side to move). The side-to-move offset against Consilium jumps */ \
                                         /* +4..+19 between exports (C2_SCALA); exports above +16 scored 4-8 Elo  */ \
                                         /* lower at 100k nodes (ep387 18.6, ep435 17.4 vs ep358/394 ~11-12)       */ \
                                         /* 6 on ep435, 60k nodes, margins e5/e6/e7 both sides: -1.2 +- 7.6 over  */ \
                                         /* 2,238 games (stopped 26/10 09:28): neutral, stays OFF                 */ \
  /* EgScale* (27/10/2026, ours, OFF at 100): the static evaluation times scale / 100 in three drawish endgame      */ \
  /* classes (at most 20 men; static_evaluate, 09_history.inc). From the results of 6,203 games at 20-30 s: at the */ \
  /* same search eval (+0.80..+1.50) the side ahead scored 0.71 with pawns on one wing only, 0.58 with pure        */ \
  /* opposite-colour bishops, 0.80 in rook endgames, against 0.84 in the other endgames; the network alone is more */ \
  /* drawish than the search there (Plebis residual probe), so the search inflates those advantages.               */ \
  /* SPRT 27/10 (Ocb 70, Rook 90, Wing 80 from the fit eg_scale_fit.py; ep486 perm, endgame_12_18 without draw     */ \
  /* adjudication, 10+0.1 noHT 20+20): 0.00 +- 3.86 over 2,356 games (Ptnml 0/89/1000/89/0; s0 +3.3, s1 -3.2),      */ \
  /* stopped by the author: neutral. Stays OFF (100); levers for the evaluation block of the SPSA.                  */ \
  P(EgScaleOcb, 100, 20, 120)            /* K + pawns + one bishop each, opposite colours                         */ \
  P(EgScaleRook, 100, 20, 120)           /* K + pawns + rooks only, both sides with a rook                        */ \
  P(EgScaleWing, 100, 20, 120)           /* pawns on one wing only (none on files a-c or none on f-h), otherwise  */ \
  /* C2Regime (26/10/2026, OFF, closed): the per-band tables chosen by the C2 expert alone (slot = the most         */ \
  /* common piece band of the expert) gave -5.0 +- 6.1 over 3,476 games at 60k nodes on ep443 (stopped by the       */ \
  /* author): the losses sat where an expert spans two bands (9-16 and 25-28 pieces), i.e. where the pawns          */ \
  /* decide the band. Replaced by C2Cells.                                                                          */ \
  /* C2Cells (26/10/2026, ours, OFF): one regime system for the search, cells = C2 expert of the side to move       */ \
  /* (C2ExpSide as for the C2 tables) x pawn-structure class: 0 few pawns (<= 6 in total), 1 closed (>= 3 rams),    */ \
  /* 2 a passed pawn on either side, 3 normal (09_history.inc, c2_pawn_class). With 1, every table indexed by       */ \
  /* the piece band of Consilium (RfpExp, FutExp, LmrExp, NmpExp, RazorExp, ProbCutExp, CaptFut/SeeExp,             */ \
  /* LmpExp, LmrScale, LmrRankMix, Hl*, RfpDisThr, StGood, HeMain/HeCont, quiescence, CorrGain, CorrPhase,          */ \
  /* CorrExpertW, ThreatBalW) reads the slot C2CellBand<e><p> of the cell; default slots: the most common           */ \
  /* piece band of each cell over 49,594 match positions with the ep443 routing (76.6% agreement with the           */ \
  /* band, against about 66% for the expert alone). On top, offsets per cell (C2c<family><e><p>, 0 = none)          */ \
  /* for reverse futility (multiplier), quiet futility, LMR (/1024), null-move floor, razoring, ProbCut             */ \
  /* margin and move count (/1024): the levers of the SPSA on the final network.                                    */ \
  /* C2Cells 1 = tables by cell, corrections keyed by the cell's slot (4 keys); 2 = corrections keyed by cell (32).   */ \
  /* 2 lost -21.3 +- 16.8 / 458 at 60k nodes (26/10). The OFFSETS C2c* do not need C2Cells (author, 26/10: split    */ \
  /* byte-identical): they add to the piece-band tables, any offset not 0 turns the cells on, all 0 = identical tree.*/ \
  P(C2Cells, 0, 0, 2)                                                                                                 \
  P(C2CellBand00, 0, 0, 3) P(C2CellBand01, 1, 0, 3) P(C2CellBand02, 1, 0, 3) P(C2CellBand03, 1, 0, 3)                 \
  P(C2CellBand10, 0, 0, 3) P(C2CellBand11, 1, 0, 3) P(C2CellBand12, 1, 0, 3) P(C2CellBand13, 1, 0, 3)                 \
  P(C2CellBand20, 1, 0, 3) P(C2CellBand21, 2, 0, 3) P(C2CellBand22, 1, 0, 3) P(C2CellBand23, 1, 0, 3)                 \
  P(C2CellBand30, 0, 0, 3) P(C2CellBand31, 2, 0, 3) P(C2CellBand32, 1, 0, 3) P(C2CellBand33, 1, 0, 3)                 \
  P(C2CellBand40, 1, 0, 3) P(C2CellBand41, 2, 0, 3) P(C2CellBand42, 2, 0, 3) P(C2CellBand43, 2, 0, 3)                 \
  P(C2CellBand50, 1, 0, 3) P(C2CellBand51, 2, 0, 3) P(C2CellBand52, 2, 0, 3) P(C2CellBand53, 2, 0, 3)                 \
  P(C2CellBand60, 2, 0, 3) P(C2CellBand61, 3, 0, 3) P(C2CellBand62, 2, 0, 3) P(C2CellBand63, 3, 0, 3)                 \
  P(C2CellBand70, 2, 0, 3) P(C2CellBand71, 3, 0, 3) P(C2CellBand72, 3, 0, 3) P(C2CellBand73, 3, 0, 3)                 \
  P(C2cRfp00, 0, -40, 40) P(C2cRfp01, 0, -40, 40) P(C2cRfp02, 0, -40, 40) P(C2cRfp03, 0, -40, 40)                     \
  P(C2cRfp10, 0, -40, 40) P(C2cRfp11, 0, -40, 40) P(C2cRfp12, 0, -40, 40) P(C2cRfp13, 0, -40, 40)                     \
  P(C2cRfp20, 0, -40, 40) P(C2cRfp21, 0, -40, 40) P(C2cRfp22, 0, -40, 40) P(C2cRfp23, 0, -40, 40)                     \
  P(C2cRfp30, 0, -40, 40) P(C2cRfp31, 0, -40, 40) P(C2cRfp32, 0, -40, 40) P(C2cRfp33, 0, -40, 40)                     \
  P(C2cRfp40, 0, -40, 40) P(C2cRfp41, 0, -40, 40) P(C2cRfp42, 0, -40, 40) P(C2cRfp43, 0, -40, 40)                     \
  P(C2cRfp50, 0, -40, 40) P(C2cRfp51, 0, -40, 40) P(C2cRfp52, 0, -40, 40) P(C2cRfp53, 0, -40, 40)                     \
  P(C2cRfp60, 0, -40, 40) P(C2cRfp61, 0, -40, 40) P(C2cRfp62, 0, -40, 40) P(C2cRfp63, 0, -40, 40)                     \
  P(C2cRfp70, 0, -40, 40) P(C2cRfp71, 0, -40, 40) P(C2cRfp72, 0, -40, 40) P(C2cRfp73, 0, -40, 40)                     \
  P(C2cFut00, 0, -200, 200) P(C2cFut01, 0, -200, 200) P(C2cFut02, 0, -200, 200) P(C2cFut03, 0, -200, 200)             \
  P(C2cFut10, 0, -200, 200) P(C2cFut11, 0, -200, 200) P(C2cFut12, 0, -200, 200) P(C2cFut13, 0, -200, 200)             \
  P(C2cFut20, 0, -200, 200) P(C2cFut21, 0, -200, 200) P(C2cFut22, 0, -200, 200) P(C2cFut23, 0, -200, 200)             \
  P(C2cFut30, 0, -200, 200) P(C2cFut31, 0, -200, 200) P(C2cFut32, 0, -200, 200) P(C2cFut33, 0, -200, 200)             \
  P(C2cFut40, 0, -200, 200) P(C2cFut41, 0, -200, 200) P(C2cFut42, 0, -200, 200) P(C2cFut43, 0, -200, 200)             \
  P(C2cFut50, 0, -200, 200) P(C2cFut51, 0, -200, 200) P(C2cFut52, 0, -200, 200) P(C2cFut53, 0, -200, 200)             \
  P(C2cFut60, 0, -200, 200) P(C2cFut61, 0, -200, 200) P(C2cFut62, 0, -200, 200) P(C2cFut63, 0, -200, 200)             \
  P(C2cFut70, 0, -200, 200) P(C2cFut71, 0, -200, 200) P(C2cFut72, 0, -200, 200) P(C2cFut73, 0, -200, 200)             \
  P(C2cLmr00, 0, -1536, 1536) P(C2cLmr01, 0, -1536, 1536) P(C2cLmr02, 0, -1536, 1536) P(C2cLmr03, 0, -1536, 1536)     \
  P(C2cLmr10, 0, -1536, 1536) P(C2cLmr11, 0, -1536, 1536) P(C2cLmr12, 0, -1536, 1536) P(C2cLmr13, 0, -1536, 1536)     \
  P(C2cLmr20, 0, -1536, 1536) P(C2cLmr21, 0, -1536, 1536) P(C2cLmr22, 0, -1536, 1536) P(C2cLmr23, 0, -1536, 1536)     \
  P(C2cLmr30, 0, -1536, 1536) P(C2cLmr31, 0, -1536, 1536) P(C2cLmr32, 0, -1536, 1536) P(C2cLmr33, 0, -1536, 1536)     \
  P(C2cLmr40, 0, -1536, 1536) P(C2cLmr41, 0, -1536, 1536) P(C2cLmr42, 0, -1536, 1536) P(C2cLmr43, 0, -1536, 1536)     \
  P(C2cLmr50, 0, -1536, 1536) P(C2cLmr51, 0, -1536, 1536) P(C2cLmr52, 0, -1536, 1536) P(C2cLmr53, 0, -1536, 1536)     \
  P(C2cLmr60, 0, -1536, 1536) P(C2cLmr61, 0, -1536, 1536) P(C2cLmr62, 0, -1536, 1536) P(C2cLmr63, 0, -1536, 1536)     \
  P(C2cLmr70, 0, -1536, 1536) P(C2cLmr71, 0, -1536, 1536) P(C2cLmr72, 0, -1536, 1536) P(C2cLmr73, 0, -1536, 1536)     \
  P(C2cNmp00, 0, -200, 200) P(C2cNmp01, 0, -200, 200) P(C2cNmp02, 0, -200, 200) P(C2cNmp03, 0, -200, 200)             \
  P(C2cNmp10, 0, -200, 200) P(C2cNmp11, 0, -200, 200) P(C2cNmp12, 0, -200, 200) P(C2cNmp13, 0, -200, 200)             \
  P(C2cNmp20, 0, -200, 200) P(C2cNmp21, 0, -200, 200) P(C2cNmp22, 0, -200, 200) P(C2cNmp23, 0, -200, 200)             \
  P(C2cNmp30, 0, -200, 200) P(C2cNmp31, 0, -200, 200) P(C2cNmp32, 0, -200, 200) P(C2cNmp33, 0, -200, 200)             \
  P(C2cNmp40, 0, -200, 200) P(C2cNmp41, 0, -200, 200) P(C2cNmp42, 0, -200, 200) P(C2cNmp43, 0, -200, 200)             \
  P(C2cNmp50, 0, -200, 200) P(C2cNmp51, 0, -200, 200) P(C2cNmp52, 0, -200, 200) P(C2cNmp53, 0, -200, 200)             \
  P(C2cNmp60, 0, -200, 200) P(C2cNmp61, 0, -200, 200) P(C2cNmp62, 0, -200, 200) P(C2cNmp63, 0, -200, 200)             \
  P(C2cNmp70, 0, -200, 200) P(C2cNmp71, 0, -200, 200) P(C2cNmp72, 0, -200, 200) P(C2cNmp73, 0, -200, 200)             \
  P(C2cRaz00, 0, -400, 400) P(C2cRaz01, 0, -400, 400) P(C2cRaz02, 0, -400, 400) P(C2cRaz03, 0, -400, 400)             \
  P(C2cRaz10, 0, -400, 400) P(C2cRaz11, 0, -400, 400) P(C2cRaz12, 0, -400, 400) P(C2cRaz13, 0, -400, 400)             \
  P(C2cRaz20, 0, -400, 400) P(C2cRaz21, 0, -400, 400) P(C2cRaz22, 0, -400, 400) P(C2cRaz23, 0, -400, 400)             \
  P(C2cRaz30, 0, -400, 400) P(C2cRaz31, 0, -400, 400) P(C2cRaz32, 0, -400, 400) P(C2cRaz33, 0, -400, 400)             \
  P(C2cRaz40, 0, -400, 400) P(C2cRaz41, 0, -400, 400) P(C2cRaz42, 0, -400, 400) P(C2cRaz43, 0, -400, 400)             \
  P(C2cRaz50, 0, -400, 400) P(C2cRaz51, 0, -400, 400) P(C2cRaz52, 0, -400, 400) P(C2cRaz53, 0, -400, 400)             \
  P(C2cRaz60, 0, -400, 400) P(C2cRaz61, 0, -400, 400) P(C2cRaz62, 0, -400, 400) P(C2cRaz63, 0, -400, 400)             \
  P(C2cRaz70, 0, -400, 400) P(C2cRaz71, 0, -400, 400) P(C2cRaz72, 0, -400, 400) P(C2cRaz73, 0, -400, 400)             \
  P(C2cPcut00, 0, -200, 200) P(C2cPcut01, 0, -200, 200) P(C2cPcut02, 0, -200, 200) P(C2cPcut03, 0, -200, 200)         \
  P(C2cPcut10, 0, -200, 200) P(C2cPcut11, 0, -200, 200) P(C2cPcut12, 0, -200, 200) P(C2cPcut13, 0, -200, 200)         \
  P(C2cPcut20, 0, -200, 200) P(C2cPcut21, 0, -200, 200) P(C2cPcut22, 0, -200, 200) P(C2cPcut23, 0, -200, 200)         \
  P(C2cPcut30, 0, -200, 200) P(C2cPcut31, 0, -200, 200) P(C2cPcut32, 0, -200, 200) P(C2cPcut33, 0, -200, 200)         \
  P(C2cPcut40, 0, -200, 200) P(C2cPcut41, 0, -200, 200) P(C2cPcut42, 0, -200, 200) P(C2cPcut43, 0, -200, 200)         \
  P(C2cPcut50, 0, -200, 200) P(C2cPcut51, 0, -200, 200) P(C2cPcut52, 0, -200, 200) P(C2cPcut53, 0, -200, 200)         \
  P(C2cPcut60, 0, -200, 200) P(C2cPcut61, 0, -200, 200) P(C2cPcut62, 0, -200, 200) P(C2cPcut63, 0, -200, 200)         \
  P(C2cPcut70, 0, -200, 200) P(C2cPcut71, 0, -200, 200) P(C2cPcut72, 0, -200, 200) P(C2cPcut73, 0, -200, 200)         \
  P(C2cLmp00, 0, -1024, 1024) P(C2cLmp01, 0, -1024, 1024) P(C2cLmp02, 0, -1024, 1024) P(C2cLmp03, 0, -1024, 1024)     \
  P(C2cLmp10, 0, -1024, 1024) P(C2cLmp11, 0, -1024, 1024) P(C2cLmp12, 0, -1024, 1024) P(C2cLmp13, 0, -1024, 1024)     \
  P(C2cLmp20, 0, -1024, 1024) P(C2cLmp21, 0, -1024, 1024) P(C2cLmp22, 0, -1024, 1024) P(C2cLmp23, 0, -1024, 1024)     \
  P(C2cLmp30, 0, -1024, 1024) P(C2cLmp31, 0, -1024, 1024) P(C2cLmp32, 0, -1024, 1024) P(C2cLmp33, 0, -1024, 1024)     \
  P(C2cLmp40, 0, -1024, 1024) P(C2cLmp41, 0, -1024, 1024) P(C2cLmp42, 0, -1024, 1024) P(C2cLmp43, 0, -1024, 1024)     \
  P(C2cLmp50, 0, -1024, 1024) P(C2cLmp51, 0, -1024, 1024) P(C2cLmp52, 0, -1024, 1024) P(C2cLmp53, 0, -1024, 1024)     \
  P(C2cLmp60, 0, -1024, 1024) P(C2cLmp61, 0, -1024, 1024) P(C2cLmp62, 0, -1024, 1024) P(C2cLmp63, 0, -1024, 1024)     \
  P(C2cLmp70, 0, -1024, 1024) P(C2cLmp71, 0, -1024, 1024) P(C2cLmp72, 0, -1024, 1024) P(C2cLmp73, 0, -1024, 1024)     \
  /* ExpertFix (20/10/2026, OFF, 32 = -27.7 +- 18.2 over 352 g. at 10+0.1 HT) removed on 26/10/2026 with the    */ \
  /* Consilium network it acted on; its Consilium Plebis form C2HoldRoot was removed on 27/10/2026 (H0).            */ \
  /* Base of our own (budget_own, 14_deepen.inc, 20/10/2026): moves left from material and ply, share of the      */ \
  /*  reserve growing with its log. Switch TmOwn baked on 20/10 (-0.23 +- 3.87 over 7,468 at 10+0.1 noHT, laptop    */ \
  /*  8+0.08 -1.06 +- 5.39 over 3,922; 4% less time per move, same depth) and retired on 21/10 with the old budget. */ \
  /*  Verdict of our own, two attempts on 20/10/2026, both REMOVED (copy in _backup/..._pre_pulizia_verdetto_):   */ \
  /*  fitted chance that the move still changes (data build -DTRIUMV_TMLOG, 651,766 searches): -27 over 556, more */ \
  /*  very short and very long moves; own forms of the stability/score/node factors fitted to follow the         */ \
  /*  inherited verdict (log corr 0.96): -12 over 830. The verdict stays the inherited one (iteration_verdict).    */ \
  P(TmMlBase, 2113, 0, 6000)             /* /100: moves left = base + piece x pieces - ply x game ply       */ \
  P(TmMlPiece, 109, 0, 400)              /* /100                                                            */ \
  P(TmMlPly, 87, 0, 400)                 /* /1000                                                           */ \
  P(TmMlMin, 10, 2, 40)                  /* moves: floor of the estimate                                    */ \
  P(TmOwnShare, 700, 200, 2500)          /* /1000: share of the reserve = share + log x log10(reserve s)    */ \
  P(TmOwnShareLog, 500, 0, 1500)         /* /1000                                                           */ \
  P(TmOwnCap, 200, 50, 500)              /* /1000: the target never exceeds this fraction of the clock     */ \
  P(TmOwnCeil, 600, 150, 1200)           /* /100: ceiling = this x target (and the clock cap)              */ \
  /* Ramp to the long-game values (21/10/2026, ours): the six budget levers above (TmMl*, TmOwnShare/Cap/Ceil) keep */ \
  /*  their fitted values at short time controls and take those of TM_LONG_LEVERS (below) at long ones, linear in  */ \
  /*  between, on the game time fixed at the first move: start clock + TmLongIncMoves x increment (25+0.25 = 35 s, */ \
  /*  15+0.15 = 21 s).                                                                                              */ \
  P(TmLongFrom, 21000, 0, 600000)        /* ms of game time: at or below, the values above                 */ \
  P(TmLongTo, 35000, 0, 600000)          /* ms of game time: at or above, the long values                  */ \
  P(TmLongIncMoves, 40, 0, 200)          /* moves of increment counted in the game time                    */ \
  /* ------------------------------------------------------------------------------------------------------------ */ \
  /* CENSUS 21/10/2026 (CENSUS.md in the work folder of the census): the numbers still written in the search, each  */ \
  /* the value of Stockfish 19 in the same role, made parameters at their old values for the SPSA on Consilium Plebis.   */ \
  /* No formula changed: bench 209655 (Consilium) and 336968 (Consilium Plebis) as before. Integer parameters replace   */ \
  /* integer literals one for one (same operations, same truncation); TmChangesKeep / 1000.0 = 0.5 is exact. The    */ \
  /* ranges keep the sign and every order the code needs (RootAvgMin < RootAvgMax for the clamp, AlphaRed at most  */ \
  /* the smallest AlphaRedLo so that the depth stays >= 1). Never measured by us: probable flat levers are marked.  */ \
  /* -- pruning and reductions --                                                                                  */ \
  P(PriorRedHigh, 3, 1, 8)               /* parent reduced >= this many plies, opponent not worsening: depth+1 */ \
  P(PriorRedLow, 2, 1, 8)                /* parent reduced >= this, the two evals above PriorRedEvalSum: depth-1 */ \
  P(PriorRedMinDepth, 2, 1, 8)           /* ... only from this depth                                        */ \
  P(SmallPcSlack, 4, 0, 8)               /* hash lower bound >= beta + margin cuts this short of depth      */ \
  /* Structure of the node (27-28/10/2026, docs/audit_8.1/R11 §4.2: the four top engines compared all take the hash   */ \
  /*  cutoff and the tablebases before the static evaluation, none has the hash-bound cut before the move loop, three */ \
  /*  have no depth reduction after alpha is raised). Removing steps cost Elo, so the steps stay in forms of ours.     */ \
  /*  HashCutEarly, REMOVED 27/10/2026 (the evaluation stays before the hash cutoff): 1 = hash cutoff and tablebases  */ \
  /*  before node_evaluate, -1.65 +- 4.14 / 6,754 at 10+0.1 noHT; 2 = as 1 plus a hash entry one ply deeper after a   */ \
  /*  parent's reduction >= PriorRedHigh, -0.60 +- 2.87 / 15,104 at 10+0.1 HT and -3.30 +- 5.87 / 3,262 at 15+0.15     */ \
  /*  noHT (author: leans negative, no loss accepted). Code in _backup/Triumviratus_8.1_pre_rimozione_hashcutearly.   */ \
  /*  SmallProbCut, REMOVED 28/10/2026: 0 (no hash-bound cut before the move loop) -1.81 +- 8.26 / 1,724 at 10+0.1     */ \
  /*  noHT, leaning negative from the start; the step stays in our form (hash_bound_cut, 13_search.inc):             */ \
  P(SpcGapW, 0, -60, 60)                 /* margin per ply the hash entry is shallower than the node        */ \
  P(SpcExp0, 0, -200, 200)               /* margin term per expert of the node                              */ \
  P(SpcExp1, 0, -200, 200)                                                                                 \
  P(SpcExp2, 0, -200, 200)                                                                                 \
  P(SpcExp3, 0, -200, 200)                                                                                 \
  P(AlphaRedLo, 3, 3, 8)                 /* alpha raised, AlphaRedLo < depth < AlphaRedHi: depth -= cut      */ \
  P(AlphaRedHi, 12, 6, 24)                                                                                 \
  P(AlphaRed, 3, 1, 3)                   /* <= minimum of AlphaRedLo: the depth stays >= 1                   */ \
  /*  The cut in our form (28/10/2026, alpha_raised_cut in 13_search.inc): AlphaRed + per-expert plies + jump term.   */ \
  P(AlphaRedExp0, 0, -2, 2)              /* plies of cut per expert of the node                             */ \
  P(AlphaRedExp1, 0, -2, 2)                                                                                \
  P(AlphaRedExp2, 0, -2, 2)                                                                                \
  P(AlphaRedExp3, 0, -2, 2)                                                                                \
  P(AlphaRedJumpW, 0, -64, 64)           /* plies = (value - old alpha) * W / 4096, within [-1, 2]           */ \
  P(CutoffExtCap, 2, 1, 4)               /* a fail high counts as a child cutoff with extension below this   */ \
  P(NudgePly, 2, 0, 6)                   /* equal score may take the best move within this of the root depth */ \
  P(ShuffleFifty, 10, 4, 30)             /* shuffling move (no singular test): fifty counter >= this,        */ \
  P(ShuffleNull, 6, 2, 16)               /*   plies from the null move >= this,                              */ \
  P(ShufflePly, 20, 8, 40)               /*   ply >= this (SF: non-regression rule, "not to be tuned")       */ \
  P(HuntDepth, 16, 8, 30)                /* mate hunt (RfpHuntDepth, no singular test) from this root depth  */ \
  P(HuntScore, 2000, 1000, 4000)         /*   with |root score| >= this                                      */ \
  P(ProbCutHashDepth, 1, 0, 3)           /* ProbCut stores its value at cut depth + this                     */ \
  P(TbHashDepth, 6, 0, 12)               /* tablebase result stored at depth + this                          */ \
  P(MateHashDepth, 6, 0, 12)             /* node without legal moves stored at depth + this                  */ \
  /* -- extensions --                                                                                              */ \
  P(CorrSingMax, 256, 64, 600)           /* clamp of the correction reward after a multi-cut (limit / 4)     */ \
  /* -- histories and ordering --                                                                                  */ \
  P(BonusTriedDiv, 256, 64, 1024)        /* non-PV: bonus grows by bonus * moves tried / this                */ \
  /* HistGrad (22/10/2026, ours): quiet history updates scaled by the gradient of a listwise ranker over the        */ \
  /* ordering scores of the best move and of the quiet moves tried (13_learn.inc, hist_grad_factors). HistGradT is the    */ \
  /* temperature in score units: the own form is a log-odds x 1000 (about 1000), the inherited score is in history   */ \
  /* units (several thousand).                                                                                  */ \
  /* BAKE HISTGRAD (22/10/2026, user): HistGradMix 0 -> 256 on the inherited ordering. Mix 512 lost (-4.0 +- 6.3     */ \
  /* over 3,066). Mix 256 at 5+0.05: socket 0 +1.58 +- 5.52 (4,190), socket 1 -3.37 +- 5.77 (3,810), Zen 4 laptop     */ \
  /* +4.16 +- 8.40 (1,920); all +0.37 +- 3.56 over 10,220, without socket 1 +2.39 +- 4.62 (laptop as referee). Mix    */ \
  /* and temperature go to the history SPSA after the network (O1). HistGradMix=0 = the inherited update exactly.    */ \
  /* New canary (MinGW dev build, Consilium Plebis ep27 net): bench 420601 (150757 with HistGradMix=0).                 */ \
  P(HistGradMix, 256, 0, 1024)                                                                             \
  P(HistGradT, 3000, 200, 40000)                                                                           \
  P(PawnHistSplit, -4, -64, 0)           /* pawn history: PawnBonusScale above this amount, PawnMalusScale at or below */ \
  P(HistLimPawn, 8192, 2048, 16384)      /* limit of the pawn history (LIM_PAWN)                             */ \
  P(HistLimTtMove, 8192, 2048, 16384)    /* limit of the hash-move history (LIM_TTMOVE)                      */ \
  P(HistLimCorr, 1024, 256, 4096)        /* limit of the correction histories (LIM_CORR)                     */ \
  P(CorrEvalDiv, 131072, 65536, 262144)  /* correction signal per evaluation unit: one scale for all Corr*W  */ \
  P(HistInitQuiet, -5, -64, 0)           /* starting values of the tables at a new game: quiet history,      */ \
  P(HistInitCapt, -742, -2000, 0)        /*   capture history,                                               */ \
  P(HistInitFollow, -586, -2000, 0)      /*   continuation histories,                                        */ \
  P(HistInitPawn, -1338, -3000, 0)       /*   pawn history,                                                  */ \
  P(HistInitCorr, -5, -64, 0)            /*   correction histories,                                          */ \
  P(HistInitContCorr, 5, 0, 64)          /*   continuation correction history                                */ \
  /* -- aspiration and iterative deepening --                                                                      */ \
  P(AspAgainMul, 3, 0, 4)                /* depth -= AspAgainMul * (searches again + 1) / 4                  */ \
  P(RootAvgChi, 3, 1, 8)                 /* root average: weight/32 = clamp(64 n / (2 n + chi * prev), min, max) */ \
  P(RootAvgMin, 12, 4, 16)                                                                                 \
  P(RootAvgMax, 24, 17, 32)                                                                                \
  P(RootMsqMax, 16, 4, 32)               /* cap of the weight of the mean square (aspiration width)          */ \
  P(TmChangesKeep, 500, 0, 1000)         /* /1000: share of the best-move changes kept at each iteration     */ \
  /* -- hash table (tt.h reads the copies g_tt_*, synced at setoption like SeePinned) --                           */ \
  P(TtKeepMargin, 3, 0, 8)               /* the old entry stays when deeper by more than this (0 = always write) */ \
  P(TtKeepPv, 2, 0, 6)                   /*   plus this on PV entries                                        */ \
  P(TtAgeWeight, 8, 1, 16)               /* victim value = depth - this * age                                */

// OWN ORDERING FORM (OrdOwn, OrdMapNo/OrdMapMask, OrdOwnTact, OrdRedK, OqGood/OtGood and the Oq/Ot/Oe weights):
// REMOVED 22/10/2026 (user). Fitted on a move-ordering oracle of 597k nodes it ordered better than the inherited
// scores (refuting quiet move first 68.8% against 66.1%) and still lost in games: with the full move-number map
// -11.8 +- 9.2 over 1,596 (q35 + q38), with the map in the log table only -12.8 +- 10.6 over 1,140, with an own-score
// term in the reduction -18.3 +- 13.9. 86% of the cutoffs fall on the first move, so the ordering gain was about 1%
// of the tree, less than the cost and the loss of balance with the inherited reductions and pruning. Code in
// _backup/Triumviratus_8.1_pre_rimozione_ordown_2026-10-22 (search/, threads.h, redlog.h, tools/ordlog_*.py).

// ---------------------------------------------------------------------------
// TUNING BUILD OF THE TIME MANAGEMENT (20/10/2026): -DTRIUMV_RELEASE -DTRIUMV_TUNE_TIME. Every parameter is a constant
// as in the release (the dev build, with ~750 variables read from memory, is 10% slower: an SPSA against another
// engine must not pay that), except the levers listed here, which stay variables, are listed as UCI options and are
// set by setoption. The list is the SPSA TV1 (Tuning_SPSA/spsa_lab/presets/crea_preset_tv1.py): the six levers of our
// own base, the 22 of the iteration verdict, TmPrevRed and TmDeepenShare. To tune others, add a TP_TUNE_ line.
// The selection by name uses a probe: TP_TUNE_<name> expands to "~, 1," only for the listed names, so TP_TUNED(name)
// takes the second argument, 1 for them and 0 for every other name.
// ---------------------------------------------------------------------------
#define TP_PROBE ~, 1,
#define TP_ARG2_(a, b, ...) b
#define TP_ARG2(...) TP_ARG2_(__VA_ARGS__)
#define TP_TUNED_(x) TP_ARG2(x, 0, )
#define TP_TUNED(name) TP_TUNED_(TP_TUNE_##name)
#define TP_CAT_(a, b) a##b
#define TP_CAT(a, b) TP_CAT_(a, b)
#define TP_TUNE_TmOwnShare TP_PROBE
#define TP_TUNE_TmMlBase TP_PROBE
#define TP_TUNE_TmMlPiece TP_PROBE
#define TP_TUNE_TmMlPly TP_PROBE
#define TP_TUNE_TmOwnCeil TP_PROBE
#define TP_TUNE_TmOwnCap TP_PROBE
#define TP_TUNE_TmFallBase TP_PROBE
#define TP_TUNE_TmFallPrev TP_PROBE
#define TP_TUNE_TmFallIter TP_PROBE
#define TP_TUNE_TmFallMin TP_PROBE
#define TP_TUNE_TmFallMax TP_PROBE
#define TP_TUNE_TmRedLo TP_PROBE
#define TP_TUNE_TmRedHi TP_PROBE
#define TP_TUNE_TmRedY0 TP_PROBE
#define TP_TUNE_TmRedY1 TP_PROBE
#define TP_TUNE_TmRedMin TP_PROBE
#define TP_TUNE_TmRedMax TP_PROBE
#define TP_TUNE_TmSettlePrev TP_PROBE
#define TP_TUNE_TmSettleDiv TP_PROBE
#define TP_TUNE_TmInstabBase TP_PROBE
#define TP_TUNE_TmInstabMul TP_PROBE
#define TP_TUNE_TmEffortLo TP_PROBE
#define TP_TUNE_TmEffortHi TP_PROBE
#define TP_TUNE_TmEffortY0 TP_PROBE
#define TP_TUNE_TmEffortY1 TP_PROBE
#define TP_TUNE_TmEffortMin TP_PROBE
#define TP_TUNE_TmEffortMax TP_PROBE
#define TP_TUNE_TmJumpGuard TP_PROBE
#define TP_TUNE_TmPrevRed TP_PROBE
#define TP_TUNE_TmDeepenShare TP_PROBE
// 21/10/2026: the C2 time levers of agent B for their SPSA (built with -DTRIUMV_C2 as well); TV1 presets ignore them.
#define TP_TUNE_TmC2Edge TP_PROBE
#define TP_TUNE_TmC2Exp0 TP_PROBE
#define TP_TUNE_TmC2Exp1 TP_PROBE
#define TP_TUNE_TmC2Exp2 TP_PROBE
#define TP_TUNE_TmC2Exp3 TP_PROBE
#define TP_TUNE_TmC2Exp4 TP_PROBE
#define TP_TUNE_TmC2Exp5 TP_PROBE
#define TP_TUNE_TmC2Exp6 TP_PROBE
#define TP_TUNE_TmC2Exp7 TP_PROBE

#if defined(TRIUMV_RELEASE) && defined(TRIUMV_TUNE_TIME)
  #define TP_DECL_0(name, def) static constexpr int name = def;
  #define TP_DECL_1(name, def) static int name = def;
  #define TP_DECLARE(name, def, lo, hi) TP_CAT(TP_DECL_, TP_TUNED(name))(name, def)
#elif defined(TRIUMV_RELEASE)
  #define TP_DECLARE(name, def, lo, hi) static constexpr int name = def;
#else
  #define TP_DECLARE(name, def, lo, hi) static int name = def;
#endif
TRIUMV_SEARCH_PARAMS(TP_DECLARE)
#undef TP_DECLARE

// ---------------------------------------------------------------------------
// SPSA TV1 OF THE TIME MANAGEMENT (21/10/2026). Vector at iteration 1172 (30 levers, 25+0.25, run
// 8.1_tempo/20261020_233631_TV1). Self-play of the whole vector against the previous values: 25+0.25 noHT +4.2 +- 5.8
// over 2,914 games (socket 0 -0.5, socket 1 +8.8); 15+0.15 laptop -0.9 +- 16.4 over 400; 10+0.1 noHT -1.2 +- 9.5 over
// 1,200; 8+0.08 HT 30+30 -5.4 +- 7.2 over 2,260 (socket 0 -10.4, socket 1 -0.3). The vector moves time to the first
// moves after the book (+10% over moves 1-10, -7% to -12% over moves 31-50, same shape at 8 s and 25 s): at long time
// controls the extra depth early pays, at short ones the depth lost in the transition to the endgame costs more.
// BAKED 21/10 (user): the 24 levers of the iteration verdict (TmFall*, TmRed*, TmSettle*, TmInstab*, TmEffort*,
// TmJumpGuard, TmPrevRed, TmDeepenShare) at the vector, at every time control (previous values in
// _backup/Triumviratus_8.0_pre_bake_tv1_2026-10-21). The six budget levers, whose distribution of the time is what
// changes with the time control, follow the ramp (TmLongFrom, TmLongTo, 14_deepen.inc) from their fitted values to the
// long values below.
// ---------------------------------------------------------------------------
#define TM_LONG_LEVERS(X)                                                                                        \
  X(TmOwnShare, 673) X(TmMlBase, 1976) X(TmMlPiece, 97) X(TmMlPly, 85) X(TmOwnCeil, 685) X(TmOwnCap, 219)

// ---------------------------------------------------------------------------
// Options that are not numeric search parameters.
// ---------------------------------------------------------------------------
static int  g_multipv            = 1;
static bool g_show_wdl           = false;
static bool g_ponder_option      = false;    // "Ponder": the GUI ponders -> +25% of optimum time
int         g_syzygy_probe_depth = 1;
int         g_syzygy_probe_limit = 7;
static bool g_syzygy_rule50      = true;
static bool g_eval_off           = false;    // NPS diagnostics: material-only evaluation

// ---------------------------------------------------------------------------
// ANALYSIS MODE (09/10/2026). Turned on by the standard UCI option UCI_AnalyseMode (GUIs set it to true when
// analysing), or by `go infinite` if AnalyseOnInfinite is on (default on). Off, every effective value equals the
// game value: tree, bench (430151) and time unchanged by construction.
// On, it changes ONLY the selectivity of the search (no time, no SMP outside AnSmp): it serves to solve more
// positions of the test suites, not to play.
// The components (An* options, settable also in the release but listed only in development builds) are measured one
// by one: bench tools\solve_suite.py at fixed nodes (2,000,000, 1 thread, Hash 64), suites test_suites\iq4.epd and
// test_suites\arasan2026.epd, solved at 250k/500k/1M/2M nodes. The measurements and the choice of the preset are below,
// next to kAnDefault.
//
// Origin of the ideas. "Depth instead of pruning" in analysis is inspired by (not ported from) the GoldDigger of
// ShashChess (amchess): here it is built with our Deep* levers (14_deepen.inc), our code. AnGold instead is the
// mechanism of the GoldDigger itself (idea of ShashChess/amchess), rewritten by us on our search: see
// gd_root_setup in 14_deepen.inc. AnCrystal is inspired by Crystal (an engine made to remove the blind
// spots of aggressive pruning; here: no pruning along the principal variation), others' idea and our code. The quiet checks in the first ply of quiescence (AnQsChecks), the
// SMP threads with reduced pruning (AnSmp) and the early mate hunt (AnHunt) are ours.
// ---------------------------------------------------------------------------
enum AnalysisBits {
  AN_EXT = 1,        // wider margins of the double/triple extensions (DeepExt 60, DeepSing -30)
  AN_NMPV = 2,       // null-move verification from depth 6 (NmpVerifyDepth 16 -> 6)
  AN_QSCHK = 4,      // first ply of quiescence: also the quiet moves that give check
  AN_SMP = 8,        // odd helper threads with the wider Deep* preset (menopot)
  AN_CRYSTAL = 16,   // no pruning on the previous principal variation in the first plies
  AN_HUNT = 32,      // hunt_mate from root depth 8 instead of 16
  AN_GOLD = 64,      // GoldDigger (ShashChess) on our search
  // Pos Solver (14/10/2026, ideas of Crystal by Joseph Ellis, our code; measurements in POS_SOLVER.md):
  AN_KDANGER = 128,  // side to move's king in danger: no pruning before the moves; opponent's king in danger
                     // near the root (ply <= 6): no LMR (only negative reductions)
  AN_CYCLE = 256,    // repetition coming: no pruning before the moves and no LMR (fortresses, perpetuals)
  // Pos Solver, second round (night of 14/10/2026, ours): MultiPV 2 at 1 s had given +15 on Arasan (POS_SOLVER.md).
  AN_SHADOW = 512,   // the search searches 2 lines at full window (like MultiPV 2) but prints as many as the GUI asks
  AN_ROOTALT = 1024, // root: the two chasers of the previous iteration without positive LMR
  AN_KEEPDEPTH = 2048, // in PV nodes at ply <= kAnKeepPly no "depth -= 3" after alpha rises (the following siblings
                       // stay at full depth: a sacrifice ordered late is not searched shorter)
  // Ideas of Crystal (Joseph Ellis), our code:
  AN_LMPNEAR = 4096,   // no move-count pruning (quiet moves off) in PV nodes and at ply <= 2
  AN_CAPTLMR = 8192,   // captures without positive LMR, except in cut nodes whose parent was not at its first move
  AN_NMPZUG = 16384,   // cautious null move: from root depth 11, for the root's opponent, only with
                       // more than 5 (pseudo-legal) moves available (zugzwang near the solution)
  AN_STOPKEEP = 32768, // MultiPV stopped mid-iteration: the fresh upper bound of a line that was failing
                       // low does not put it behind the old scores of the others (it goes back to its full value)
  AN_DIRECT = 65536,   // between two clearly winning moves (the two exact lines of AnShadow), the forcing one (check or
                       // capture) if the higher one is quiet and they are at most DirectGap cp apart (15/10/2026)
  AN_DIRLINE = 131072, // with the first line quiet and winning (beyond DirectFrom), the second line of AnShadow is
                       // searched only among the direct moves: AnDirect compares the best quiet move with the best
                       // direct one, even when the direct one is not second by score (15/10/2026)
  AN_SHQUIET = 262144, // position "quiet" as seen by the search: from QuietFrom, with the first two moves not direct and
                       // the score stable (within QuietCp for three iterations), the second line of AnShadow is
                       // suspended and the depth goes back to full (SBD: the two lines cost ~10 positions) (15/10/2026)
  AN_QDEEP = 524288,   // in the same "quiet" iterations of AnShQuiet, normal pruning (preset AnDeep 0) instead of
                       // prune+lmr: more depth where the right move is quiet (15/10/2026)
  AN_ACTIVE = 1 << 30
};
// Presets of the Deep* levers (a single table: change here). DeepKnee 4 and DeepSpan 1: full weights from depth 5.
struct DeepPreset { int rfp, fut, lmr, nmp; };
static constexpr int kAnDeepCount = 5;
static constexpr DeepPreset kAnDeep[kAnDeepCount] = {
  {0, 0, 0, 0},              // 0: none (the Deep* levers of the parameters)
  {100, 150, 0, 0},          // 1: prune    (DeepRfp 100, DeepFut 150)
  {0, 0, -1024, 0},          // 2: lmr      (DeepLmr -1024)
  {100, 150, -1024, -1024},  // 3: menopot  (all four)
  {100, 150, -1024, 0},      // 4: prunelmr (prune + lmr, null move unchanged)
};
static constexpr int kAnDeepKnee = 4, kAnDeepSpan = 1;
static constexpr int kAnExt = 60, kAnSing = -30;   // AN_EXT
static constexpr int kAnNmpVerify = 6;             // AN_NMPV
static constexpr int kAnHuntFrom = 8;              // AN_HUNT
static constexpr int kAnKeepPly = 2;               // AN_KEEPDEPTH
// Default: first ALL OFF (morning of 09/10/2026, the preset was composed from setoption); after the measurements below the
// preset of the Deep* levers is prune+lmr (kAnDefaultDeep 4) and the An* components stay off.
// MEASUREMENTS 09/10/2026 (bench: tools\solve_suite.py, fixed nodes 2,000,000, 1 thread, Hash 64; positions solved at
// 250k/500k/1M/2M nodes; dev MinGW build an_v2, which reproduces the base and prune of the runs on tm21 and release 20261009):
//   iq4 (183):        base 132/141/149/152; prune 145/148/154/155; qschk 140/147/153/154; crystal 137/142/144/147;
//                     hunt 134/142/148/151; gold 141/148/152/154; prunelmr 140/145/149/153; prune+qschk 140/144/154/156;
//                     prune+crystal 141/149/153/156; prune+hunt 143/147/153/153; prune+gold 140/143/149/153;
//                     prune+nmpv 140/148/154/156; prune+ext 143/149/154/159.
//   arasan2026 (200): base 121/139/149/162; prune 136/148/157/168; qschk 128/139/148/159; crystal 127/142/153/162;
//                     hunt 121/139/149/162; gold 131/140/155/166; prune+qschk 127/145/157/168;
//                     prune+crystal 133/145/153/162; prune+hunt 136/148/157/168; prune+gold 136/147/161/167;
//                     prune+nmpv 125/138/149/163; prune+ext 127/138/149/158.
//   Measurements of the Deep* levers alone (prune, lmr, menopot, ext, nmpv6) on other suites: test_suites\risultati.
// CHOSEN DEFAULT (09/10/2026): AnDeep 4 = prune+lmr (DeepRfp 100, DeepFut 150, DeepLmr -1024), components off.
// Measurements: build release_8.0_analisi, fixed nodes, test_suites\risultati am_*_8M and al_difficili_*_64M (table with
// tools\am_tabella.py):
//   - 8M nodes, positions solved beyond the base (iq4 / arasan2026 / ecmgcp): lmr +4/+4/+4, prune+lmr +4/+3/+7;
//     at 4M prune+lmr +20 in total against +14 for lmr;
//   - the 71 positions the base does not solve at 8M, at 64M: base 15, lmr 28, prune+lmr 36;
//   - discarded: lmr+qschk -1/+2/+2 and lmr+gold -2/+3/-2.
// UCI_AnalyseMode and go infinite (AnalyseOnInfinite) both use prune+lmr; in games an_bits = 0 and nothing changes.
// Check: bench 430151 unchanged; with UCI_AnalyseMode=true the bench gives 632173 (that of prunelmr).
static constexpr int kAnDefaultDeep = 4;
// Pos Solver (night of 14/10/2026): AnShadow by default (two-line search, printing the lines the GUI asks for).
// Measurements on the dev binary t80_ps4 (POS_SOLVER.md): Arasan 2026 157 -> 170 at 1M nodes, 166 -> 178 at 2M, 151 -> 158
// at 1 s; IQ4 158 -> 157 at 1M, 161 -> 163 at 4M, 153 -> 152 at 1 s. In the playing engine everything stays off.
// v3 (night of 15/10/2026): AnDirect too (threshold 300 cp, distance 400 cp, sacrifices included among the direct moves).
// Tree identical to AnShadow; IQ4 4M 163 -> 166 (+34, +112, +178, none lost), Arasan 2M 178 -> 178 identical;
// distances 600 and 800 the same, threshold 200 loses Arasan 70.
// v4 (15/10/2026): AnShQuiet too (second line suspended in quiet and stable positions, QuietFrom 10, QuietCp 15).
// 8 suites at 1M nodes +20 (SBD 98 -> 110); high budgets (IQ4 4M, Arasan 2M, SBD/difficult/ECM 4M) 664 -> 671.
static constexpr int kAnDefaultBits = PSV(0, AN_SHADOW | AN_DIRECT | AN_SHQUIET);
static bool g_analyse_mode     = false;   // UCI_AnalyseMode
// AnalyseOnInfinite on by default (09/10/2026, user: "go infinite must work like the mode we choose"): many GUIs
// and the test-suite programs analyse with `go infinite` without setting UCI_AnalyseMode. In games `go infinite`
// never arrives (ponder has its own flag, g_limits.ponder), so play does not change.
static bool g_analyse_infinite = true;    // AnalyseOnInfinite
static int  g_an_deep          = kAnDefaultDeep;
static int  g_an_bits          = kAnDefaultBits;
static bool g_data_log_enabled   = false;
static std::string g_data_log_file = "triumviratus_dataset.txt";
void set_data_log_enabled(bool enabled) { g_data_log_enabled = enabled; }

// Globals read by other translation units (tt.h, see.cpp, movegen.cpp). The names stay those of before.
tt_entry* hash_table      = nullptr;
U64       hash_entries    = 0;
bool      g_tt_pow2       = false;
int       current_age     = 0;
bool      g_tt_twolevel   = true;
#ifndef TRIUMV_RELEASE
bool      g_ttmove24      = true;
// Copies of TtKeepMargin, TtKeepPv, TtAgeWeight for tt.h (CENSUS 21/10/2026): the new entry is written unless the old
// one is deeper by more than 3 plies (+2 on PV entries); victim = depth - 8 * age.
int       g_tt_keep_margin = TtKeepMargin;
int       g_tt_keep_pv     = TtKeepPv;
int       g_tt_age_weight  = TtAgeWeight;
bool      g_tt_move_keep  = true; // without a new move the old one is kept
// The copies start from the parameter's default (06/10/2026: with SeePinned baked at 1 the copy stayed at 0 until a
// setoption arrived, and the SEE stayed off: bench 222624 instead of 507070).
int       g_see_pinned    = SeePinned;            // copy of SeePinned for see.cpp (see td_see_ge)
bool      g_tt_age_refresh = false;
bool      g_see_fix       = true;
// --- C2 piece values (agent SEE) --- (21/10/2026): copies for see.cpp, from the parameter defaults, synced at setoption.
int       g_see_val[12]   = {SeeValP, SeeValN, SeeValB, SeeValR, SeeValQ, 20000,
                             SeeValP, SeeValN, SeeValB, SeeValR, SeeValQ, 20000};
int       g_c2_val_off[5][8] = {
    {C2ValP0, C2ValP1, C2ValP2, C2ValP3, C2ValP4, C2ValP5, C2ValP6, C2ValP7},
    {C2ValN0, C2ValN1, C2ValN2, C2ValN3, C2ValN4, C2ValN5, C2ValN6, C2ValN7},
    {C2ValB0, C2ValB1, C2ValB2, C2ValB3, C2ValB4, C2ValB5, C2ValB6, C2ValB7},
    {C2ValR0, C2ValR1, C2ValR2, C2ValR3, C2ValR4, C2ValR5, C2ValR6, C2ValR7},
    {C2ValQ0, C2ValQ1, C2ValQ2, C2ValQ3, C2ValQ4, C2ValQ5, C2ValQ6, C2ValQ7}};
int       g_pawn_adj[5]   = {PawnIsoAdj, PawnDblAdj, PawnSupAdj, PawnPassAdj, PawnR7Adj};
bool      g_pawn_adj_on   = PawnIsoAdj || PawnDblAdj || PawnSupAdj || PawnPassAdj || PawnR7Adj;
static void see_values_sync() {
  {
    const int pa[5] = {PawnIsoAdj, PawnDblAdj, PawnSupAdj, PawnPassAdj, PawnR7Adj};
    std::memcpy(g_pawn_adj, pa, sizeof(pa));
    g_pawn_adj_on = PawnIsoAdj || PawnDblAdj || PawnSupAdj || PawnPassAdj || PawnR7Adj;
  }
  const int v[6] = {SeeValP, SeeValN, SeeValB, SeeValR, SeeValQ, 20000};
  for (int i = 0; i < 6; i++)
    g_see_val[i] = g_see_val[i + 6] = v[i];
  const int off[5][8] = {
      {C2ValP0, C2ValP1, C2ValP2, C2ValP3, C2ValP4, C2ValP5, C2ValP6, C2ValP7},
      {C2ValN0, C2ValN1, C2ValN2, C2ValN3, C2ValN4, C2ValN5, C2ValN6, C2ValN7},
      {C2ValB0, C2ValB1, C2ValB2, C2ValB3, C2ValB4, C2ValB5, C2ValB6, C2ValB7},
      {C2ValR0, C2ValR1, C2ValR2, C2ValR3, C2ValR4, C2ValR5, C2ValR6, C2ValR7},
      {C2ValQ0, C2ValQ1, C2ValQ2, C2ValQ3, C2ValQ4, C2ValQ5, C2ValQ6, C2ValQ7}};
  std::memcpy(g_c2_val_off, off, sizeof(off));
}
#else
// Release (06/10/2026): this copy is a constant in see.cpp; it must match the parameter's default.
static_assert(SeePinned == 1, "see.cpp congela g_see_pinned = 1 nella release");
// --- C2 piece values (agent SEE) --- (21/10/2026): see.cpp reads see_piece_values (see.h) and no regime table.
static_assert(SeeValP == see_piece_values[P] && SeeValN == see_piece_values[N] && SeeValB == see_piece_values[B] &&
                  SeeValR == see_piece_values[R] && SeeValQ == see_piece_values[Q],
              "see.cpp freezes the SEE piece values in the release");
static_assert([] {
  const int off[5][8] = {
      {C2ValP0, C2ValP1, C2ValP2, C2ValP3, C2ValP4, C2ValP5, C2ValP6, C2ValP7},
      {C2ValN0, C2ValN1, C2ValN2, C2ValN3, C2ValN4, C2ValN5, C2ValN6, C2ValN7},
      {C2ValB0, C2ValB1, C2ValB2, C2ValB3, C2ValB4, C2ValB5, C2ValB6, C2ValB7},
      {C2ValR0, C2ValR1, C2ValR2, C2ValR3, C2ValR4, C2ValR5, C2ValR6, C2ValR7},
      {C2ValQ0, C2ValQ1, C2ValQ2, C2ValQ3, C2ValQ4, C2ValQ5, C2ValQ6, C2ValQ7}};
  for (int t = 0; t < 5; t++)
    for (int e = 0; e < 8; e++)
      if (off[t][e] != see_val_offsets[t][e]) return false;
  return true;
}(), "see.h freezes the per-expert piece-value offsets in the release");
static_assert(PawnIsoAdj == see_pawn_adj[0] && PawnDblAdj == see_pawn_adj[1] && PawnSupAdj == see_pawn_adj[2] &&
                  PawnPassAdj == see_pawn_adj[3] && PawnR7Adj == see_pawn_adj[4] && PawnIsoAdj == 0 && PawnDblAdj == 0 &&
                  PawnSupAdj == 0 && PawnPassAdj == 0 && PawnR7Adj == 0,
              "see.h freezes the pawn-structure corrections (and g_pawn_adj_on = false) in the release");
// Release: tt.h holds these copies as constants; they must equal the parameters.
static_assert(TtKeepMargin == g_tt_keep_margin && TtKeepPv == g_tt_keep_pv && TtAgeWeight == g_tt_age_weight,
              "tt.h freezes the hash-table copies in the release");
#endif
bool      g_large_pages   = true;
bool      g_ep_key_fix    = true;
static const bool g_evasion_gen = true;

static inline bool parse_bool_value(int v) { return v != 0; }

void print_search_options() {
#ifndef TRIUMV_RELEASE
#define TP_PRINT(name, def, lo, hi) printf("option name " #name " type spin default %d min %d max %d\n", def, lo, hi);
  TRIUMV_SEARCH_PARAMS(TP_PRINT)
#undef TP_PRINT
#elif defined(TRIUMV_TUNE_TIME)   // only the tuned levers (see TP_TUNED above)
#define TP_PRINT(name, def, lo, hi)                                                                        \
  if (TP_TUNED(name)) printf("option name " #name " type spin default %d min %d max %d\n", def, lo, hi);
  TRIUMV_SEARCH_PARAMS(TP_PRINT)
#undef TP_PRINT
#endif
}

// Options of the analysis mode: the combinations are composed from setoption.
// 14/10/2026 (user: "in the normal release I do not want the An options to be visible"): the playing release lists only
// UCI_AnalyseMode, the standard option GUIs handle on their own; AnalyseOnInfinite, AnDeep and the An* components are
// visible in the development builds and in the Pos Solver. Hidden does not mean off: setoption still accepts them.
void print_analysis_options() {
  printf("option name UCI_AnalyseMode type check default %s\n", PSV(false, true) ? "true" : "false");
  // 28/10/2026 (author: a Pos Solver for Maurizio Platino "without all the options visible"): -DTRIUMV_HIDE_AN_OPTIONS
  // hides them in the Pos Solver too; their defaults (kAnDefaultBits) still apply and setoption still accepts them.
#if defined(TRIUMV_RELEASE) && (!defined(TRIUMV_POS_SOLVER) || defined(TRIUMV_HIDE_AN_OPTIONS))
  return;
#endif
  printf("option name AnalyseOnInfinite type check default true\n");
  // Components (they act only with UCI_AnalyseMode, or with AnalyseOnInfinite and go infinite).
  printf("option name AnDeep type spin default %d min 0 max %d\n", kAnDefaultDeep, kAnDeepCount - 1);
  static const struct { const char *n; int bit; } an_opts[] = {
    {"AnExt", AN_EXT}, {"AnNmpVerify", AN_NMPV}, {"AnQsChecks", AN_QSCHK}, {"AnSmp", AN_SMP},
    {"AnCrystal", AN_CRYSTAL}, {"AnHunt", AN_HUNT}, {"AnGold", AN_GOLD},
    {"AnKingDanger", AN_KDANGER}, {"AnCycle", AN_CYCLE},
    {"AnShadow", AN_SHADOW}, {"AnRootAlt", AN_ROOTALT}, {"AnKeepDepth", AN_KEEPDEPTH},
    {"AnLmpNear", AN_LMPNEAR}, {"AnCaptLmr", AN_CAPTLMR}, {"AnNmpZug", AN_NMPZUG}, {"AnStopKeep", AN_STOPKEEP}, {"AnDirect", AN_DIRECT}, {"AnDirLine", AN_DIRLINE}, {"AnShQuiet", AN_SHQUIET}, {"AnQDeep", AN_QDEEP}};
  for (const auto &o : an_opts)
    printf("option name %s type check default %s\n", o.n, (kAnDefaultBits & o.bit) ? "true" : "false");
}

#ifndef TRIUMV_RELEASE
static void param_tables_refresh();   // 13_search.inc
#endif

bool set_search_param(const char* name, int value) {
  if (!strcmp(name, "MultiPV")) { g_multipv = value < 1 ? 1 : (value > 64 ? 64 : value); return true; }
  if (!strcmp(name, "UCI_ShowWDL")) { g_show_wdl = parse_bool_value(value); return true; }
  if (!strcmp(name, "Ponder")) { g_ponder_option = parse_bool_value(value); return true; }
  if (!strcmp(name, "SyzygyProbeDepth")) { g_syzygy_probe_depth = value < 1 ? 1 : value; return true; }
  if (!strcmp(name, "SyzygyProbeLimit")) { g_syzygy_probe_limit = value < 0 ? 0 : (value > 7 ? 7 : value); return true; }
  if (!strcmp(name, "Syzygy50MoveRule")) {
    g_syzygy_rule50 = parse_bool_value(value);
    syzygy_set_ignore_50_move_rule(!g_syzygy_rule50);
    return true;
  }
  if (!strcmp(name, "EvalOff")) { g_eval_off = parse_bool_value(value); return true; }
  // Analysis mode: also in the release (the An* components are not listed, but can be set).
  if (!strcmp(name, "UCI_AnalyseMode")) { g_analyse_mode = parse_bool_value(value); return true; }
  if (!strcmp(name, "AnalyseOnInfinite")) { g_analyse_infinite = parse_bool_value(value); return true; }
  if (!strcmp(name, "AnDeep")) { g_an_deep = std::clamp(value, 0, kAnDeepCount - 1); return true; }
  {
    static const struct { const char *n; int bit; } an_opts[] = {
      {"AnExt", AN_EXT}, {"AnNmpVerify", AN_NMPV}, {"AnQsChecks", AN_QSCHK}, {"AnSmp", AN_SMP},
      {"AnCrystal", AN_CRYSTAL}, {"AnHunt", AN_HUNT}, {"AnGold", AN_GOLD},
      {"AnKingDanger", AN_KDANGER}, {"AnCycle", AN_CYCLE},
    {"AnShadow", AN_SHADOW}, {"AnRootAlt", AN_ROOTALT}, {"AnKeepDepth", AN_KEEPDEPTH},
    {"AnLmpNear", AN_LMPNEAR}, {"AnCaptLmr", AN_CAPTLMR}, {"AnNmpZug", AN_NMPZUG}, {"AnStopKeep", AN_STOPKEEP}, {"AnDirect", AN_DIRECT}, {"AnDirLine", AN_DIRLINE}, {"AnShQuiet", AN_SHQUIET}, {"AnQDeep", AN_QDEEP}};
    for (const auto &o : an_opts)
      if (!strcmp(name, o.n)) {
        g_an_bits = parse_bool_value(value) ? (g_an_bits | o.bit) : (g_an_bits & ~o.bit);
        return true;
      }
  }
#ifndef TRIUMV_RELEASE
  // The options read outside threads.cpp (tt.h, see.cpp) have a global copy: it is realigned at every setoption.
  // Evening of 12/10/2026: also the tables built from the parameters (reciprocals of HistDiv*, powers of block 2).
  const auto sync = [] {
    g_see_pinned = SeePinned;
    g_tt_keep_margin = TtKeepMargin;
    g_tt_keep_pv = TtKeepPv;
    g_tt_age_weight = TtAgeWeight;
    see_values_sync();   // piece values: SeeVal* and C2Val*
    param_tables_refresh();
  };
#define TP_SET(pname, def, lo, hi)                                                                         \
  if (!strcmp(name, #pname)) { pname = value < lo ? lo : (value > hi ? hi : value); sync(); return true; }
  TRIUMV_SEARCH_PARAMS(TP_SET)
#undef TP_SET
#elif defined(TRIUMV_TUNE_TIME)
  // Only the tuned levers are variables; the time levers feed no table built from the parameters, so no sync.
#define TP_SET_1(pname, lo, hi)                                                                           \
  if (!strcmp(name, #pname)) { pname = value < lo ? lo : (value > hi ? hi : value); return true; }
#define TP_SET_0(pname, lo, hi)
#define TP_SET(pname, def, lo, hi) TP_CAT(TP_SET_, TP_TUNED(pname))(pname, lo, hi)
  TRIUMV_SEARCH_PARAMS(TP_SET)
#undef TP_SET
#undef TP_SET_0
#undef TP_SET_1
#endif
  return false;
}

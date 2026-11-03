// search/09_history.inc -- Static evaluation, move statistics, correction of the evaluation.
// Part of threads.cpp: NOT a translation unit of its own.

// ============================================================================
// EVALUATION
// ============================================================================

// "eval" command: static evaluation of the global board (side to move) and the raw network output. The network reads
// our bitboards as they are (07/10/2026, single board).
int debug_eval_position() {
  return nn_eval(side == white, bitboards, occupancies, fifty);
}
int debug_eval_position_raw() {
  int raw = 0;
  nn_eval(side == white, bitboards, occupancies, fifty, &raw);
  return raw;
}

// Back to the root with the network's chain of accumulators (at every iteration). The network reads the thread's
// board directly: there is no copy to realign, only the dirty stack to reset.
static inline void nn_root_sync(ThreadData &td) {
  nn_pos_set(td.nnpos);
#ifdef TRIUMV_VERIFY_NNSYNC
  td.nnstack->st[0].key = nn_piece_key(td);
#endif
}

// The network evaluates the thread's board. First the missing threats and pawns (nn_dirty_catch_up) and, with
// -DTRIUMV_VERIFY_NNSYNC, the check that the stack describes this board (06_nndirty.inc).
static inline int td_nn_eval(ThreadData &td) {
  {
    PROF_GUARD(prof_catchup);
    nn_dirty_catch_up(td);
  }
  nn_verify_sync(td);
  return nn_pos_eval(td.nnpos, td.bitboards, td.occupancies, td.piece_on, td.side == white, td.fifty);
}

// The evaluation is damped with the fifty-move counter: v * (199 - fifty) / 199. The hash keeps the value WITHOUT the
// damping, so that it holds for every transposition, and damps it again when read.
// (08/10/2026, speed, same value) The divisor 199 - fifty changes at run time and the division was an idiv (~26
// cycles of latency) at every fresh evaluation, twice. Now it multiplies by the reciprocal M = ceil(2^40 / D) and
// truncates toward zero: exact while |x| * (M*D - 2^40) < 2^40, that is |x| < 2^40 / 199; here |x| = |v| * 199 < 2^28
// (checked for every fifty and |v| <= 2^20: no difference). Identical tree.
static constexpr auto unfade_recip = [] {
  std::array<unsigned long long, 101> r{};
  for (int f = 0; f <= 100; f++)
    r[f] = ((1ULL << 40) + (unsigned long long)(198 - f)) / (unsigned long long)(199 - f);
  return r;
}();
static inline int eval_unfade(int v, int fifty) {
  fifty = std::min(fifty, 100);
  const long long x = (long long)v * 199;
  const unsigned long long mag = (unsigned long long)(x < 0 ? -x : x);
  const int q = (int)((mag * unfade_recip[fifty]) >> 40);
  return x < 0 ? -q : q;
}
static inline int eval_refade(int v, int fifty) {
  if (fifty > 100) fifty = 100;
  // The same truncation as the bridge (nn_scale, nn_finalize: v - v*f/199), so the same position has the same value
  // fresh, from the cache or from the hash (05/10/2026; before, v * (199 - fifty) / 199).
  return v - v * fifty / 199;
}

// Expert of the network (material band) by number of pieces: <= 9, 10-15, 16-23, >= 24, the bands of the network's
// piece-square block. Used by the per-expert parameters and CorrPhase.
static inline int net_phase(int pieces) { return pieces <= 9 ? 0 : pieces <= 15 ? 1 : pieces <= 23 ? 2 : 3; }

// Network evaluation (network units).
static inline int net_evaluate(ThreadData &td) {
  PROF_GUARD(prof_eval);
  if (g_eval_off) {
    static const int mat[6] = {100, 320, 330, 500, 900, 0};
    int s = 0;
    for (int pc = 0; pc < 6; pc++)
      s += mat[pc] * (count_bits(td.bitboards[pc]) - count_bits(td.bitboards[pc + 6]));
    return td.side == white ? s : -s;
  }
  return td_nn_eval(td);   // (15/10/2026) no per-thread evaluation cache, see threads.h
}

// Evaluation in the units of the search margins: x EvalUnits/1024, inside the tablebase band.
// C2StmOffset (26/10/2026, OFF): a constant taken from the side to move's evaluation, against the side-to-move offset
// of some Consilium Plebis exports. Every static evaluation of the search passes here.
// EgScale* (27/10/2026, ours; 01_params.inc): the scale of a drawish endgame class, 100 outside them. Classes in the
// order of the fit (match_moe fortress data): pure opposite-colour bishops, opposite-colour bishops with other pieces
// (no scale: they scored like the other endgames), rook endgames, pawns on one wing only. At most 20 men.
static inline int eg_scale(const ThreadData &td) {
  if (count_bits(td.occupancies[both]) > 20)
    return 100;
  const U64 wb = td.bitboards[B], bb = td.bitboards[b];
  const U64 minors_majors_w = td.bitboards[N] | td.bitboards[R] | td.bitboards[Q];
  const U64 minors_majors_b = td.bitboards[n] | td.bitboards[r] | td.bitboards[q];
  constexpr U64 light = 0xAA55AA55AA55AA55ULL;   // a8 = 0 is light
  if (count_bits(wb) == 1 && count_bits(bb) == 1 && !(wb & light) != !(bb & light))
    return (minors_majors_w | minors_majors_b) ? 100 : EgScaleOcb;
  if (!(wb | bb | td.bitboards[N] | td.bitboards[n] | td.bitboards[Q] | td.bitboards[q]) && td.bitboards[R] &&
      td.bitboards[r])
    return EgScaleRook;
  const U64 pawns = td.bitboards[P] | td.bitboards[p];
  constexpr U64 qside = 0x0707070707070707ULL, kside = 0xE0E0E0E0E0E0E0E0ULL;   // files a-c, f-h
  if (pawns && (!(pawns & qside) || !(pawns & kside)))
    return EgScaleWing;
  return 100;
}

static inline int static_evaluate(ThreadData &td) {
  int v = (int)((long long)(net_evaluate(td) - C2StmOffset) * EvalUnits / 1024);
  if (EgScaleOcb != 100 || EgScaleRook != 100 || EgScaleWing != 100) {
    const int s = eg_scale(td);
    if (s != 100)
      v = (int)((long long)v * s / 100);
  }
  return std::clamp(v, -TB_WIN_FLOOR + 1, TB_WIN_FLOOR - 1);
}


// Corrections learned with keys on residual concepts (11/10/2026): all closed and removed. CorrKingPawnW (all pawns +
// the 2x2 zones of the kings) -4.68 +- 7.19 over 2,378 games, key too fine; CorrShelterW (pawns on the three files
// around the kings) -5.63 +- 5.53 over 3,898; CorrPassKingW (passed pawns outside the enemy king's square) -5.24 +-
// 8.49 over 744 endgames. The existing corrections seem to take the signal that games can teach already.

// Value for the screen: from search units to centipawns (network scale and normalisation by 400).
// (06/10/2026) 449 -> 400, so that +1.00 = 50% chance of winning, fitted on our own self-play data
// (match_moe/fit_cp_scale.py): with 449 the 50% point fell at +0.89 printed, flat with the material, at 10+0.1
// (563,496 positions) and 40+0.4 (99,438): one constant is enough. Display only: the search uses internal units and
// the bench does not change.
static inline int units_to_cp(int v) {
  const long long net = (long long)v * 1024 / EvalUnits;
  const int es = nn_get_eval_scale();
  const long long cp = es == 100 ? net : net * 100 / es;
  return (int)(cp * 100 / 400);
}

// Value printed to the GUI (28/10/2026, 8.1 with Consilium Plebis): the same scale fitted again on the moves of 8.1 in
// 3,588 games against 8.0 (183,630 positions, match_moe/fit_cp_scale_81.py): 50% wins fell at +0.94 printed with 400,
// flat with the material (0.92-0.985 in every band), so the screen divides by 376. Only the printed cp and WDL use it;
// every decision of the search that reads centipawns (TmJumpGuard, DirectFrom, QuietCp, the root style) keeps
// units_to_cp, so the games do not change.
static inline int units_to_print_cp(int v) {
  const long long net = (long long)v * 1024 / EvalUnits;
  const int es = nn_get_eval_scale();
  const long long cp = es == 100 ? net : net * 100 / es;
  return (int)(cp * 100 / 376);
}

// ============================================================================
// MOVE STATISTICS
// ============================================================================

// Piece values: one per-expert table for the whole engine since 28/10/2026 (piece_values_mat, see.cpp; the inherited
// unit_value table of the search is gone).

// Every statistic of the engine (move histories, correction histories, hash-move history) is updated by
// hist_learn, below. The former update (stat_push, in the inherited form) was removed on 17/10/2026: hist_learn with
// gravity 1024 gives the same integer result.

// (12/10/2026) The limits are parameters (HistLim*, 01_params.inc); in the release they are constants. Pawn, hash-move
// and correction limits since the CENSUS of 21/10/2026 (were 8192, 8192, 1024).
#ifdef TRIUMV_RELEASE
constexpr int LIM_QUIET  = HistLimQuiet;
constexpr int LIM_CAPT   = HistLimCapt;
constexpr int LIM_FOLLOW = HistLimFollow;
constexpr int LIM_PAWN   = HistLimPawn;
constexpr int LIM_CORR   = HistLimCorr;
constexpr int LIM_TTMOVE = HistLimTtMove;
#else
#define LIM_QUIET  HistLimQuiet
#define LIM_CAPT   HistLimCapt
#define LIM_FOLLOW HistLimFollow
#define LIM_PAWN   HistLimPawn
#define LIM_CORR   HistLimCorr
#define LIM_TTMOVE HistLimTtMove
#endif

static inline bool move_ok(int m) { return m > 0; }
static inline int  move_from(int m) { return get_move_source(m); }
static inline int  move_to(int m) { return get_move_target(m); }
static inline int  move_piece(int m) { return get_move_piece(m); }
// Promotion kind 0..4 (none, N, B, R, Q): part of the key of the quiet statistics. A table on the 4 bits of the field
// instead of `pr ? pr % 6 : 0` (same values): promo_kind runs for every move and cost 0.6% of the cycles.
static constexpr signed char kPromoKind[16] = {0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3};
static inline int  promo_kind(int m) { return kPromoKind[get_move_promoted(m)]; }
static inline int  quiet_key(int m) { return move_from(m) | (move_to(m) << 6) | (promo_kind(m) << 12); }
// "Tactical" move: a capture or a queen promotion (code Q = 4 or q = 10: bits 4 and 10 of the mask 0x410). The other
// promotions are ordered among the quiet moves.
static inline bool is_tactical(int m) { return (get_move_capture(m) | ((0x410u >> get_move_promoted(m)) & 1)) != 0; }
// Piece type P..K from a code P..k without a modulo (06/10/2026, speed, same values). Index 15 is "no piece"
// (-1 & 15) and gives CAPT_NONE, so an empty square also goes through the table without a branch.
static constexpr signed char kPieceKind[16] = {0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, CAPT_NONE, CAPT_NONE, CAPT_NONE, CAPT_NONE};
// Type of the captured piece (CAPT_NONE for en passant and for a promotion without capture, like an empty square).
static inline int  victim_kind(const ThreadData &td, int m) {
  return kPieceKind[td.piece_on[move_to(m)] & 15];
}

// ============================================================================
// --- C2 signals for the search (agent A) --- (21/10/2026)
// Consilium Plebis picks the expert of each perspective with the routing table of the net over the non-pawn material
// signatures (cell = sig(own) * 54 + sig(opponent)); the make writes both experts in the state of each move
// (dp.phase: White in the low nibble, Black in the high one) and which of them changed (dp.phaseChanged). On the net
// of 21/10 the table follows the opponent's signature in 99.4% of the cells: the side to move's expert tells how much
// material the opponent has, the opponent's expert how much we have (01_params.inc, block "C2 search ideas").
// ============================================================================
static inline int c2_sig_of(const ThreadData &td, int c) {
  const int off = 6 * c;
  return cn_sig(count_bits(td.bitboards[N + off]), count_bits(td.bitboards[B + off]), count_bits(td.bitboards[R + off]),
                count_bits(td.bitboards[Q + off]));
}
// Expert of perspective c (white or black) in the current position: from the state of the last move made; the root's
// state has none (and a null move adds no state), so with one state the board is read.
static inline int c2_expert(const ThreadData &td, int c) {
  const NnStack &S = *td.nnstack;
  if (S.size >= 2)
    return (S.st[S.size - 1].dp.phase >> (4 * c)) & 15;
  return nn_router_table()[c2_sig_of(td, c) * 54 + c2_sig_of(td, c ^ 1)];
}
// C2Cells (26/10/2026, OFF; 01_params.inc): the cell of the node, C2 expert of the side to move (C2ExpSide as for the
// C2 tables) x pawn-structure class, and the slot of the four-entry per-band tables each cell reads.
// Pawn class, side-independent, in this order: 0 few pawns (<= 6 in total), 1 closed (>= 3 rams: a white pawn with a
// black pawn right in front), 2 a passed pawn on either side, 3 normal. Squares a8 = 0: north is -8, south +8.
static inline int c2_pawn_class(const ThreadData &td) {
  const U64 wp = td.bitboards[P], bp = td.bitboards[p];
  if (count_bits(wp | bp) <= 6)
    return 0;
  if (count_bits((wp >> 8) & bp) >= 3)
    return 1;
  auto spread = [](U64 x) { return x | ((x << 1) & not_a_file) | ((x >> 1) & not_h_file); };
  U64 s = spread(bp) << 8;   // squares south of a black pawn's file and neighbours: a white pawn there is not passed
  s |= s << 8, s |= s << 16, s |= s << 32;
  U64 n = spread(wp) >> 8;   // the same for black pawns, north of white pawns
  n |= n >> 8, n |= n >> 16, n |= n >> 32;
  return ((wp & ~s) | (bp & ~n)) ? 2 : 3;
}
#ifdef TRIUMV_RELEASE
static constexpr unsigned char g_cell_band[32] = {
    C2CellBand00, C2CellBand01, C2CellBand02, C2CellBand03, C2CellBand10, C2CellBand11, C2CellBand12, C2CellBand13,
    C2CellBand20, C2CellBand21, C2CellBand22, C2CellBand23, C2CellBand30, C2CellBand31, C2CellBand32, C2CellBand33,
    C2CellBand40, C2CellBand41, C2CellBand42, C2CellBand43, C2CellBand50, C2CellBand51, C2CellBand52, C2CellBand53,
    C2CellBand60, C2CellBand61, C2CellBand62, C2CellBand63, C2CellBand70, C2CellBand71, C2CellBand72, C2CellBand73};
#else
static unsigned char g_cell_band[32];   // filled by c2_tables_refresh (13_search.inc) at start and at every setoption
#endif
static inline int c2_cell(const ThreadData &td) {
  return (c2_expert(td, C2ExpSide ? td.side ^ 1 : td.side) & 7) * 4 + c2_pawn_class(td);
}
// Did the move just made (top state of the stack) change the expert of either perspective?
static inline bool c2_last_switched(const ThreadData &td) {
  const NnStack &S = *td.nnstack;
  return S.size >= 2 && S.st[S.size - 1].dp.phaseChanged != 0;
}
// Before a move: the counts of the node and its experts, to tell whether a capture or a promotion switches.
struct C2Mat {
  int cnt[2][4];   // N, B, R, Q of White and Black
  int e[2];        // experts of the two perspectives now
};
static inline void c2_mat_init(const ThreadData &td, C2Mat &cm) {
  for (int c = 0; c < 2; c++)
    for (int t = 0; t < 4; t++)
      cm.cnt[c][t] = count_bits(td.bitboards[N + t + 6 * c]);
  const unsigned char *rt = nn_router_table();
  const int s0 = cn_sig(cm.cnt[0][0], cm.cnt[0][1], cm.cnt[0][2], cm.cnt[0][3]);
  const int s1 = cn_sig(cm.cnt[1][0], cm.cnt[1][1], cm.cnt[1][2], cm.cnt[1][3]);
  cm.e[0] = rt[s0 * 54 + s1];
  cm.e[1] = rt[s1 * 54 + s0];
}
// Would move m change the expert of either perspective? Only a capture of a piece and a promotion change material;
// pawn captures and en passant never do (CAPT_NONE or a pawn victim).
static inline bool c2_switches(const ThreadData &td, const C2Mat &cm, int m) {
  const int vk = victim_kind(td, m), pr = get_move_promoted(m);
  const bool piece_victim = vk >= N && vk <= Q;
  if (!piece_victim && !pr)
    return false;
  int c[2][4];
  std::memcpy(c, cm.cnt, sizeof(c));
  if (piece_victim)
    c[td.side ^ 1][vk - N]--;
  if (pr)
    c[td.side][kPieceKind[pr] - N]++;
  const unsigned char *rt = nn_router_table();
  const int s0 = cn_sig(c[0][0], c[0][1], c[0][2], c[0][3]), s1 = cn_sig(c[1][0], c[1][1], c[1][2], c[1][3]);
  return rt[s0 * 54 + s1] != cm.e[0] || rt[s1 * 54 + s0] != cm.e[1];
}
// Same class of piece for the conversion push (C2Conv*): knight and bishop, rook and rook, queen and queen.
static inline bool c2_even_trade(const ThreadData &td, int m) {
  if (!get_move_capture(m))
    return false;
  const int vk = victim_kind(td, m), mk = kPieceKind[move_piece(m)];
  if (vk < N || vk > Q || mk < N || mk > Q)
    return false;
  return vk == mk || (vk <= B && mk <= B);
}
// Pawn-king skip of the current position, side to move's view (consilium_bridge.h nn_pk_skip).
static inline int c2_pk_skip(const ThreadData &td) { return nn_pk_skip(td.nnpos, td.bitboards, td.side == white); }
// Conversion push with the exact pawn-king effect (C2ConvPkSrc 1, 21/10/2026): the value of the current position,
// side to move's view, positional units. _fresh only right after this position was evaluated by the network
// (node_evaluate, nn_last_disagree >= 0); _now at any time (catches up the dirty stack, as td_nn_eval).
static inline bool c2_conv_exact() {   // per-node exact effect; not with C2ConvRoot (decided at the root only)
  return (C2ConvOrder | C2ConvRed) && C2ConvPkSrc == 1 && C2ConvEffW && !C2ConvRoot;
}
static inline int c2_pk_effect_fresh(const ThreadData &td) {
  return nn_pk_effect_fresh(td.nnpos, td.bitboards, td.occupancies, td.piece_on, td.side == white);
}
static inline int c2_pk_effect_now(ThreadData &td) {
  nn_dirty_catch_up(td);
  nn_verify_sync(td);
  return nn_pk_effect_now(td.nnpos, td.bitboards, td.occupancies, td.piece_on, td.side == white);
}
// The same for the position after the quiet move m of the side to move (a pawn or king move that captures nothing),
// from the mover's view: the board is copied with the piece moved and the subnetwork is asked for the other side.
static inline int c2_pk_skip_after(const ThreadData &td, int m) {
  U64 bb[12];
  std::memcpy(bb, td.bitboards, sizeof(bb));
  const int pc = move_piece(m);
  bb[pc] ^= (1ULL << move_from(m)) | (1ULL << move_to(m));
  return -nn_pk_skip(td.nnpos, bb, td.side != white);
}
// Pawn break (28/10/2026, ours; BreakOrder/BreakRed/BreakPrune, 01_params.inc): a quiet pawn push of the side to move
// (no capture, no promotion) that attacks an enemy pawn from its new square, in any structure. Asked before the move is
// made (td.side is the mover).
static inline bool pawn_break(const ThreadData &td, int m) {
  if (kPieceKind[move_piece(m)] != P || get_move_capture(m) || get_move_promoted(m))
    return false;
  return (pawn_attacks[td.side][move_to(m)] & td.bitboards[td.side == white ? p : P]) != 0;
}
// C2PawnHist: constant per expert mixed into the key of the pawn-structure history (1 = expert of the side to move,
// 2 = pair of experts). One salt per expert, as CorrPhase has one per band for the corrections.
static inline U64 c2_pawn_salt(const ThreadData &td) {
  static constexpr U64 salt[8] = {0xA0761D6478BD642FULL, 0xE7037ED1A0B428DBULL, 0x8EBC6AF09C88C6E3ULL,
                                  0x589965CC75374CC3ULL, 0x1D8E4E27C47D124FULL, 0x72BE5D74F27B896FULL,
                                  0x31F9C59D0BB8A3D1ULL, 0xD2B74407B1CE6E93ULL};
  const U64 own = salt[c2_expert(td, td.side)];
  return C2PawnHist == 1 ? own : own ^ (salt[c2_expert(td, td.side ^ 1)] >> 7 | salt[c2_expert(td, td.side ^ 1)] << 57);
}

// ============================================================================
// --- C2 ordering (agent ORDER) --- (21/10/2026; ideas 5 and 6 of docs/rete/C2_IDEE_FABLE_2026-10-21.md)
// OFF by default; SPRT/SPSA pending. Parameters: 01_params.inc, block "C2 ordering" after QuietHistDecay.
//
// Idea 5, regime-gated chess-knowledge ordering (C2OrdReg). The ordering terms of PR4 (KingShield, AttackOrder*,
// PassedPush, PromoOrder, Outpost*, BishopPairCapt) were tuned with one value for every material regime. Each matters
// in a regime that one of the two C2 experts names: e_us (side to move's perspective) reads the opponent's non-pawn
// material, e_them ours. Our shelter and the opponent's passers' stoppers depend on what the opponent has (e_us);
// our attacks and our minors' squares on what we have (e_them). A scale per expert (/1024) multiplies each group; at
// 1024 everywhere the tree is the identical one. The experts are one byte of the make's state (c2_experts_td): the
// scales are read once per scoring pass, and only with C2OrdReg on.
// ============================================================================
struct C2RegScales {
  int ks;      // C2KsExp[e_us]: KingShield
  int atk;     // C2AtkExp[e_them]: AttackOrder, AttackOrderQ
  int pass;    // C2PassExp[e_us]: PassedPush, PromoOrder
  int minor;   // C2MinorExp[e_them]: OutpostOrder, OutpostOrderB, BishopPairCapt
};
// `us` = the side whose terms are scaled: the side to move while ordering; the side that has just moved in the two
// reduction credits (13_search.inc), which run after the move is made.
static inline C2RegScales c2_reg_scales(const ThreadData &td, int us) {
  const int e = c2_experts_td(td);
  const int eu = (e >> (4 * us)) & 7, et = (e >> (4 * (us ^ 1))) & 7;
  const int ks[8] = {C2KsExp0, C2KsExp1, C2KsExp2, C2KsExp3, C2KsExp4, C2KsExp5, C2KsExp6, C2KsExp7};
  const int atk[8] = {C2AtkExp0, C2AtkExp1, C2AtkExp2, C2AtkExp3, C2AtkExp4, C2AtkExp5, C2AtkExp6, C2AtkExp7};
  const int pass[8] = {C2PassExp0, C2PassExp1, C2PassExp2, C2PassExp3, C2PassExp4, C2PassExp5, C2PassExp6, C2PassExp7};
  const int minor[8] = {C2MinorExp0, C2MinorExp1, C2MinorExp2, C2MinorExp3,
                        C2MinorExp4, C2MinorExp5, C2MinorExp6, C2MinorExp7};
  return {ks[eu], atk[et], pass[eu], minor[et]};
}
// One term v of a group with scale s (/1024). The terms stay below 40,000 and the scales below 4,097: no overflow.
static inline int c2_reg_apply(int v, int s) { return v * s / 1024; }

// Idea 6, structure history keyed by the pawn-king signature (C2StructHistW). The pawn history (pawn_row) is keyed by
// the exact pawn structure, so nothing passes from one structure to the next. Consilium Plebis's pawn-king subnetwork
// groups structures the network reads alike: the signs of its C2StructHistBits most variable outputs against their
// medians (nn_pk_signature, the key of CorrPkW; ~230 values over the bench at 12 bits). A quiet-move table [piece][to]
// per signature learns "the plan for this kind of structure" once. It differs from C2PawnHist (the exact key salted by
// expert: finer, not coarser) and from CorrPkW (a correction, not moves). Ordering only: the reductions and the history
// pruning keep reading the flat tables (the lesson of ThreatHist). Cache hit only (compute = 0): the subnetwork is
// never run for this key; on a miss (-1) there is no term and no update. A one-entry memo per thread keeps the key of
// the last position asked (the update of a node asks it once per quiet move learned).
struct alignas(64) C2StructMemo {
  U64 hash;
  int bits, key;
};
static C2StructMemo g_c2_struct_memo[MAX_THREADS];
static inline int c2_struct_key(const ThreadData &td) {
  C2StructMemo &mm = g_c2_struct_memo[td.thread_id];
  const int bits = std::clamp(C2StructHistBits, 1, 12);   // the table has 1 << 12 rows
  if (mm.hash == td.hash_key && mm.bits == bits)
    return mm.key;
  const long long s = nn_pk_signature(td.nnpos, td.bitboards, td.side == white, bits, 0);
  if (s >= 0)   // misses are not kept: the entry may be in the cache at the next question
    mm.hash = td.hash_key, mm.bits = bits, mm.key = (int)s;
  return (int)s;
}
// The row of the current position, or null on a cache miss.
static inline PieceToRow *c2_struct_row(const ThreadData &td) {
  const int k = c2_struct_key(td);
  return k >= 0 ? &g_stats.struct_rows[k] : nullptr;
}

// Slot of the four-entry per-band tables for this node: the piece band of Consilium (net_phase), or with C2Cells the
// slot of the node's cell (C2CellBand). Every per-band lookup of the search goes through here.
static inline int search_band(const ThreadData &td) {
  if (C2Cells)
    return g_cell_band[c2_cell(td)];
  return net_phase(count_bits(td.occupancies[both]));
}

static inline PieceToRow &pawn_row(const ThreadData &td) {
  if (C2PawnHist)   // C2 idea 4 (21/10/2026, agent A): the pawn history split by expert; OFF by default, SPRT pending
    return static_cast<PieceToRow *>(td.st_pawn)[(td.pawn_key ^ c2_pawn_salt(td)) & td.st_pawn_mask];
  return static_cast<PieceToRow *>(td.st_pawn)[td.pawn_key & td.st_pawn_mask];
}

// ============================================================================
// WRITING THE STATISTICS (block 1 of 8.1)
// Every update of a statistic goes through the functions below, with the parameters of the network expert chosen by
// the material of the node (net_phase: 0 = 9 pieces or fewer, 1 = 10-15, 2 = 16-23, 3 = 24 or more).
// Origin: the saturating update and an amount linear in depth are known ideas of move histories, used by most
// engines. Our form: a gravity per expert, a quadratic term in depth, weights per expert.
// At the starting values every formula gives the old integer result, so the tree is identical until tuning moves
// them (bench 137419). Parameters: 01_params.inc, sub-blocks E, F, G of docs/development/8.1/SEARCH_1_ORDERING.md.
// ============================================================================
static inline int by_exp4(int e, int v0, int v1, int v2, int v3) { return e == 0 ? v0 : e == 1 ? v1 : e == 2 ? v2 : v3; }
static inline int hist_grav(int e)   { return by_exp4(e, HlGrav0, HlGrav1, HlGrav2, HlGrav3); }
static inline int hist_cgrav(int e)  { return by_exp4(e, HlCGrav0, HlCGrav1, HlCGrav2, HlCGrav3); }
static inline int corr_grav(int e)   { return by_exp4(e, CorrGrav0, CorrGrav1, CorrGrav2, CorrGrav3); }
static inline int ttm_grav(int e)    { return by_exp4(e, TtmGrav0, TtmGrav1, TtmGrav2, TtmGrav3); }

// Saturating update: an entry moves toward the reward b and slows near the limit L,
//   e += b - G/1024 * e*|b|/L
// G = 1024 is the old update with the same truncation (e*|b|*1024 / (L*1024) = e*|b| / L). Lower G: the entry gets
// closer to the limit (keeps more); higher G: it forgets sooner. The int16 clamp never acts at G = 1024.
// (20/10/2026) The quotient in double instead of the 64-bit division (same values, checked call by call) was neutral
// in the release build on the Xeon (-0.15% cycles in the middlegame, -0.03% in endgames): not kept.
static inline void hist_learn(int16_t &e, int bonus, int limit, int grav) {
  const int b = std::clamp(bonus, -limit, limit);
  const int v = e;
#if defined(__AVX2__) && !defined(__AVX512F__)
  // (29/10/2026 night, speed, same values) At the default gravity G = 1024 (every expert in the release) the quotient
  // is v*|b| / limit with the same truncation, and it fits 32 bits (|v| <= 32767, |b| <= limit <= 32000, the largest
  // limit of 01_params.inc). -0.8% / -1.5% cycles (mg / eg) on the Xeon avx2 variant; kept off the AVX-512 builds,
  // where it measured +0.2% / +0.4%.
  if (grav == 1024) {
    const int pull = v * (b < 0 ? -b : b) / limit;
    e = (int16_t)std::clamp(v + b - pull, -32767, 32767);
    return;
  }
#endif
  const long long pull = (long long)v * (b < 0 ? -b : b) * grav / ((long long)limit * 1024);
  e = (int16_t)std::clamp((long long)v + b - pull, -32767LL, 32767LL);
}
// Same rule for an int statistic (tt_move_hist).
static inline void hist_learn(int &e, int bonus, int limit, int grav) {
  const int b = std::clamp(bonus, -limit, limit);
  const long long pull = (long long)e * (b < 0 ? -b : b) * grav / ((long long)limit * 1024);
  e = (int)((long long)e + b - pull);
}

// Amount of a reward or penalty at depth d: min(mul*d - sub + q*d*d/64, cap), q per expert (HlDepthQ; 0 = the old
// linear form). Every reward and penalty by depth comes from here.
static inline int hist_amount(int e, int depth, int mul, int sub, int cap) {
  const int q = by_exp4(e, HlDepthQ0, HlDepthQ1, HlDepthQ2, HlDepthQ3);
  return std::min(mul * depth - sub + q * depth * depth / 64, cap);
}

// The updates themselves (continuations, the quiet set, captures, corrections) and the events that call them are in
// 13_learn.inc (C5a, 27/10/2026).

// ============================================================================
// QUIET HISTORY BY THREAT CONTEXT (ThreatHist, block 1 of 8.1)
// The same quiet move means different things when the piece escapes a threat or walks into one. Context, two bits:
// 1 = the from-square is threatened, 2 = the to-square is. Threatened = attacked by an enemy piece of lower value
// (the attack that costs material even on a defended square): for a pawn a pawn, for a knight or bishop a pawn, for a
// rook a minor piece or a pawn, for a queen also a rook. The king and promotions have no context. The maps are the
// ones score_quiets already builds (MoveQueue::menace); a single move (hash move, evasions) asks the squares.
// Origin: indexing the quiet history by threats is a known idea (other engines, e.g. Ethereal). Ours: the
// lower-value context, the factored form and the weights.
//
// Factored form: the flat table (index key) receives every update; three correction tables, one per context, learn
// only how the move does in that context against its flat history. The ordering reads the sum (qh_order); the
// reductions and the history pruning read the flat table only (qh_main), so their parameters keep their meaning.
// ThreatCtxW = weight of the correction in the ordering, ThreatCtxUpd = scale of its update (/1024).
// ThreatHist 0 = no context (ctx is always 0: flat table only, bench 137419); 1-3 = factored form (3 is used by the
// tuning runs and tests, bench 270429).
//
// History: 16/10/2026, the earlier form with separate tables per context lost (H0, -3.32 +- 1.93 over 33,510 games at
// 8+0.08): its context repeated the lesser[] map of the fixed threat term, it split the updates of a move over four
// tables, and it also entered the reductions and the history pruning. Factored form after a short SPSA: -0.89 +- 4.25
// over 6,626 games. The separate-table forms (1, and 2 = any attacker) were removed on 17/10/2026.
static inline int qh_order(const ThreadData &td, int side, int key, int ctx) {
  const int16_t *h = td.quiet_hist[side];
  return h[key] + (ctx ? h[QH_SIZE + ((ctx - 1) << 12) + key] * ThreatCtxW / 1024 : 0);
}
static inline int qh_main(const ThreadData &td, int side, int key, int) {
  return td.quiet_hist[side][key];
}
static inline void qh_push(ThreadData &td, int side, int key, int ctx, int bonus, int grav) {
  int16_t *h = td.quiet_hist[side];
  hist_learn(h[key], bonus, LIM_QUIET, grav);
  if (ctx)
    hist_learn(h[QH_SIZE + ((ctx - 1) << 12) + key], bonus * ThreatCtxUpd / 1024, LIM_QUIET, grav);
}

// Is square sq threatened for our piece of type pt? Same definition as the maps of score_quiets, node occupancy.
static inline bool square_menaced(const ThreadData &td, int sq, int pt) {
  const int us = td.side, eoff = us == white ? 6 : 0;
  const U64 occ = td.occupancies[both];
  const U64 ep = td.bitboards[P + eoff], en = td.bitboards[N + eoff], eb = td.bitboards[B + eoff],
            er = td.bitboards[R + eoff];
  if (pawn_attacks[us][sq] & ep)
    return true;
  if (pt <= B)
    return false;
  if ((knight_attacks[sq] & en) || (get_bishop_attacks(sq, occ) & eb))
    return true;
  return pt == Q && (get_rook_attacks(sq, occ) & er);
}

// Threat context of one quiet move from its two squares (hash move before the maps exist, evasions).
static inline int quiet_ctx_slow(const ThreadData &td, int m) {
  if (!ThreatHist || get_move_promoted(m))
    return 0;
  const int pt = kPieceKind[move_piece(m)];
  if (pt == K)
    return 0;
  return (int)square_menaced(td, move_from(m), pt) | (int)square_menaced(td, move_to(m), pt) << 1;
}

// ============================================================================
// CORRECTION OF THE STATIC EVALUATION (sub-block I of block 1)
// The tables learn the difference between the searched value and the static evaluation, by structure keys (pawns,
// minor pieces, non-pawns of each colour), by the pair of moves that led to the node, and optionally by expert.
// Ours in 8.1: the weight of the correction per expert (CorrGain), a quadratic depth term in its reward (CorrDepthQ,
// 13_search.inc) and the gravity per expert of the update (CorrGrav).
// ============================================================================

static inline CorrSlot &corr_slot(const ThreadData &td, U64 key, int side_to_move) {
  return static_cast<CorrSlot (*)[2]>(td.st_corr)[key & td.st_corr_mask][side_to_move];
}

// CorrPhase (idea 9, 04/10/2026): with four experts the systematic error on one structure changes with the expert. A
// constant per band, mixed into the pawn and minor-piece keys, separates the corrections. (06/10/2026, speed, same
// values) The key is read from a table by number of pieces, without the comparisons of net_phase (~2.1 calls per node).
static constexpr auto corr_phase_keys_by_count = [] {
  constexpr U64 keys[4] = {0x9E3779B97F4A7C15ULL, 0xC2B2AE3D27D4EB4FULL, 0x165667B19E3779F9ULL, 0xD6E8FEB86659FD93ULL};
  std::array<U64, 65> t{};   // up to 64: even a FEN with too many pieces stays inside the table
  for (int c = 0; c <= 64; c++)
    t[c] = keys[c <= 9 ? 0 : c <= 15 ? 1 : c <= 23 ? 2 : 3];   // = net_phase(c)
  return t;
}();
// --- C2 corrections (agent B) ---
// CorrC2Key (21/10/2026, ours): CorrPhase separates the corrections by the expert of the network, but its bands are
// the piece counts of Consilium (8.0). Consilium Plebis chooses the expert of each perspective from a learned routing table
// over the non-pawn material, so the band no longer names the expert that made the error. Mode 1 salts the pawn and
// minor keys with the C2 expert of the side to move (8 regimes), mode 2 with the pair (own, opponent) (64 regimes).
// Same tables, another key; every caller goes through corr_phase_key. OFF by default; SPRT pending.
static constexpr auto c2_corr_salts = [] {
  std::array<U64, 72> t{};   // 0..7: own expert; 8 + 8 x own + opponent: the pair
  U64 x = 0x243F6A8885A308D3ULL;
  for (auto &v : t) {   // a fixed sequence of mixed constants (splitmix-style finaliser)
    x += 0x9E3779B97F4A7C15ULL;
    U64 z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    v = z ^ (z >> 31);
  }
  return t;
}();
static inline U64 c2_corr_salt(const ThreadData &td) {
  const int e = c2_experts_td(td), us = td.side;
  const int own = (e >> (4 * us)) & 15, opp = (e >> (4 * (us ^ 1))) & 15;
  return c2_corr_salts[CorrC2Key == 1 ? own : 8 + 8 * own + opp];
}

// CorrPhaseQueen (17/10/2026, off = old tables; report R2 of the 8.1 rewrite): eight regimes instead of four, the piece
// band times "queens on the board". Inside a band the queens double the evaluation swings, so the systematic error on
// one structure differs with and without them. Same tables, a different key: with queens the band key is XORed with a
// fixed salt. Callers (prefetch in the make, corr_signal, Learner::corrections) all go through here, so they stay consistent.
static inline U64 corr_phase_key(const ThreadData &td) {
  if (CorrC2Key)
    return c2_corr_salt(td);
  // C2Cells 1: the key of the cell's slot (four keys, as with the piece bands: a piece count inside that band selects it
  // from the same table); 2: one key per cell (32). 26/10: with 2 the tables lost -21.3 +- 16.8 over 458 games at 60k
  // nodes (each cell's corrections learn from an eighth of the data), stopped for the test of 1.
  static constexpr int slot_count[4] = {2, 12, 20, 30};
  const U64 k = C2Cells >= 2 ? (U64)(c2_cell(td) + 1) * 0x9E3779B97F4A7C15ULL
              : corr_phase_keys_by_count[C2Cells ? slot_count[search_band(td)] : count_bits(td.occupancies[both])];
  return (CorrPhaseQueen && (td.bitboards[Q] | td.bitboards[q])) ? k ^ 0x6A09E667F3BCC909ULL : k;
}

// CorrFifty (ours, 05/10/2026, from an idea seen in Reckless): the systematic error on one structure changes with the
// distance from the fifty-move rule (the evaluation is damped by the counter, and in long endgames the position is
// worth something else). A constant per band of CorrFiftyStep plies (at most 16 bands), mixed into the pawn and
// non-pawn keys, separates the corrections. At 0 the key is the usual one.
static inline U64 corr_fifty_key(const ThreadData &td) {
  static constexpr U64 keys[16] = {0, 0x78FAC4B00841A538ULL, 0x5EB2C61E9C3747D9ULL, 0x59701BD515B6EE59ULL,
                                   0x2FEFFD3F283182A5ULL, 0x2DBE4F221F31A14FULL, 0xE19215DA308D5DA7ULL,
                                   0x4AA99EFB91350BC7ULL, 0xC9C3DD626F1AE010ULL, 0x642CD00D7AA43578ULL,
                                   0x4E3591F9E5FC0494ULL, 0x3FBCACD31257FB99ULL, 0x0C995B8887B93F26ULL,
                                   0x8408B9E4AF9644A9ULL, 0x10A6D573C576C901ULL, 0x5DDBAD22D85BAB7BULL};
  return CorrFiftyStep ? keys[std::min(td.fifty / CorrFiftyStep, 15)] : 0;
}

static int corr_signal(const ThreadData &td, const NodeFrame *f) {
  PROF_GUARD(prof_corr);
  const int us = td.side;
  const U64 phk = corr_phase_key(td);
  const U64 fk = corr_fifty_key(td);
  const int pawn = corr_slot(td, td.pawn_key ^ phk ^ fk, us).pawn;
  const int minor = corr_slot(td, td.mm_key[0] ^ phk, us).minor;
  const int npw = corr_slot(td, td.np_key[white] ^ fk, us).np_white;
  const int npb = corr_slot(td, td.np_key[black] ^ fk, us).np_black;
  const int prev = f[-1].move;
  int cont = CorrNoMoveW;   // fixed term after a null move (08/10/2026: a constant before, now a lever)
  if (move_ok(prev)) {
    const int to = move_to(prev), pc = td.piece_on[to];
    cont = CorrContW * ((*f[-2].cont_corr)[pc][to] + (*f[-4].cont_corr)[pc][to]);
    if (CorrCont6W)   // f[-6] always exists: there are 7 frames with the sentinel row before the root
      cont += CorrCont6W * (*f[-6].cont_corr)[pc][to];
    // CorrContOppW (the user's idea, 06/10/2026): the pair of the OPPONENT's last two moves (at -3 and -1), that is
    // its plan, instead of our move with its reply. Same table; the version with the last move alone, without
    // context, was closed (-7.6 +- 7.8 at 20+0.2).
    if (CorrContOppW)
      cont += CorrContOppW * (*f[-3].cont_corr)[pc][to];
  }
  // Weight of the correction per expert (CorrGain, /1024; 1024 = old value, s*1024/1024 = s). The tables were already
  // separate by expert band (CorrPhase); the weight was one for all.
  // Our form (27/10/2026, docs/audit_8.1/R9): the two non-pawn tables, one per colour, may weigh differently for the
  // side to move and its opponent: CorrNonPawnW * (own + opp) + CorrNpSideW * (own - opp). At 0: one weight for both.
  const int np_side = us == white ? npw - npb : npb - npw;
  const int s = CorrPawnW * pawn + CorrMinorW * minor + CorrNonPawnW * (npw + npb) + CorrNpSideW * np_side + cont;
  const int ex = search_band(td);
  return (int)((long long)s * by_exp4(ex, CorrGain0, CorrGain1, CorrGain2, CorrGain3) / 1024);
}

// CorrExpertW (06/10/2026): a pawn table per expert of the network, added to the shared corrections instead of
// replacing their key (as CorrPhase does). The shared table stays as the base; the per-expert one learns only the
// expert's own difference. The term enters the evaluation but NOT n.signal, which steers RFP, LMR and the singular
// extension, so that only accuracy is measured. At 0 the term is 0.
static inline U64 corr_expert_key(const ThreadData &td) {
  static constexpr U64 keys[4] = {0x3C6EF372FE94F82BULL, 0xA54FF53A5F1D36F1ULL, 0x510E527FADE682D1ULL,
                                  0x9B05688C2B3E6C1FULL};
  const int pcs = count_bits(td.occupancies[both]);
  const int band = CorrExpertBands == 2 ? (pcs <= 15 ? 0 : 3) : search_band(td);
  return td.pawn_key ^ keys[band];
}

// CorrPkW (21/10/2026, ours, C2 corrections of agent B): a correction keyed by the pawn structure as the network has
// learned it. The pawn-king subnetwork of Consilium Plebis maps pawns and kings to 32 outputs; the key keeps only the signs
// of the CorrPkBits most variable outputs against their medians (consilium/cn.cpp, pk_signature), so structures the
// net reads alike share an entry. The pawn-key tables of 11/10/2026 (CorrKingPawnW, all pawns + king zones) failed
// with a key too fine; this one is coarse by construction and comes from the network itself. Like CorrExpertW it
// enters the evaluation only, not n.signal; it shares that table (CorrExpertW stays 0). OFF by default; SPRT pending.
// Cost: the key reads the thread's pawn-king cache. With CorrPkCompute 0 a board whose entry is not there (the node
// reads the key before its evaluation, which fills the entry) gets no term and teaches nothing; with 1 the subnetwork
// runs on such a miss (bench 21/10: one more miss per six nodes, ~-20% speed at 1 against ~-2% at 0, see REPORT_B).
static inline bool c2_pk_key(const ThreadData &td, U64 &key) {
  const long long s = nn_pk_signature(td.nnpos, td.bitboards, td.side == white, CorrPkBits, CorrPkCompute);
  key = ((U64)s * 0x9E3779B97F4A7C15ULL) ^ 0x5851F42D4C957F2DULL;
  return s >= 0;
}

static inline int corr_signal_extra(const ThreadData &td) {
  int s = 0;
  if (CorrExpertW)
    s += CorrExpertW * static_cast<int16_t (*)[2]>(td.st_corr_x)[corr_expert_key(td) & td.st_corr_mask][td.side];
  U64 pk_key;
  if (CorrPkW && c2_pk_key(td, pk_key))
    s += CorrPkW * static_cast<int16_t (*)[2]>(td.st_corr_x)[pk_key & td.st_corr_mask][td.side];
  return s;
}

static inline int corrected_eval(int v, int signal) {
  return std::clamp(v + signal / CorrEvalDiv, -TB_WIN_FLOOR + 1, TB_WIN_FLOOR - 1);   // CorrEvalDiv: was 131072
}

// The update of the correction tables (Learner::corrections) is in 13_learn.inc, with the events that teach them.

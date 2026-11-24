// search/10_plan.inc -- Long-term plan of the game, read by the reduction of quiet moves (PlanLmr, PlanLmrLow). Part of
// threads.cpp: it is NOT a translation unit of its own. All OFF at Plan 0 (01_params.inc): no plan is kept, no node
// changes, the tree is the one without this file.
//
// History. The author's idea of 31/10-01/11/2026 (docs/innovazioni, sections 15-20): a player forms a plan that lasts
// many moves; the engine should keep one too. The study built a goal map (targets judged by the network, routes of
// single pieces on the frozen board) and this long-term form, and removed both from the source on 01/11/2026
// (archivio_idee/): their signal was mostly the network's static view, which the search already has. The one form
// never measured alone was PlanLmrLow (UHO, out-of-sample ratio of lost cuts R 1.18: the plan was worth about five
// times more for the quiet moves the history ranks lowest). Brought back on 24/11/2026 for the 9.0 tree (Lex
// Hortensia), author's decision after the opponent-plan correction (CorrContOppW) held on Lex
// (docs/PIANO_9.0_RETE_FISSATA s.14). Kept from the 8.1 form: persistence, the four target sources, the map, PlanLmr and
// PlanLmrLow. Left in the archive: the preparing moves (PlanPrep), prophylaxis (PlanDef), the closed-position gate
// (PlanClosed), the ordering terms, the root tie and the data builds.
//
//  1. Persistence. A weight per target (colour, piece type, square), kept between the searches of a game as a moving
//     average: at each search w = w x PlanKeep/1024 + fresh gain x (1024 - PlanKeep)/1024, so a target found move after
//     move grows towards its gain and one found once fades. The plan ends (all weights to 0) when the expert pair of
//     the root changes (material regime), when more than PlanPawnBreak pawn squares changed since the previous search,
//     or when the root score fell by PlanContra after a move that advanced the plan. ucinewgame clears it
//     (search_clear). It depends only on the sequence of root positions and on the searches.
//  2. Fresh targets of each search (PlanSources): bit 1 single placements judged by the network (each piece moved on
//     the frozen board to the empty squares it reaches in PlanReach moves; the gain over the unchanged position, the
//     PlanK best squares per piece, from PlanGainMin, capped at PlanCap); bit 2 pairs, the PlanPairTop best placements
//     of different pieces judged together (at most PlanPairMax probes per side), kept when the pair beats its better
//     single by PlanPairMin; bit 4 pawn structure: pawn pushes up to PlanPawnPush squares when they reach a lever or
//     the pawn is passed, knights and bishops on outposts and rooks behind own passed pawns, up to PlanRoute moves
//     away; bit 8 the squares the pieces reach at the end of the line played (PlanPvPly plies, gain PlanPvGain).
//  3. The map. At the start of each search the PlanTargets heaviest targets of each colour with weight >= PlanMin give
//     the potentials: routes of up to PlanRoute moves on the root's board, potential gain x (R + 1 - d) / (R + 1). One
//     map for all the threads (g_plan_map, written on the main thread before the helpers start, then read only).
//  4. Uses. The value of a quiet move is the change of its side's potential (arrival minus start), saturated at
//     PlanScale. PlanLmr: PlanLmr/1024 ply less reduction for a move of the plan, in proportion to its value; PlanLmrLow:
//     more of the same in proportion to how low the move's statistic score is (full at -8192). Only in nodes with the
//     root's expert pair (the routes belong to that board), never below zero reduction through these terms.
// Cost with Plan 1: about 2 x 400 relocations judged by the network per search, on the main thread before the helpers
// start; per node one comparison of the expert pair, per quiet move two table reads.

constexpr int PLAN_SINGLES = 160;   // single placements kept per side for the pairs

// A target: side, piece type (P..K), square, square of the probed piece (64 = none), gain in evaluation units.
struct PlanTarget {
  unsigned char colour, kind, sq, from;
  int16_t gain;
};
// The plan's map: for each colour the potential of a square for each piece type (pot); valid in the nodes with the
// expert pair `pair`.
struct PlanMap {
  U64 key = 0;     // root position the map was built in
  int pair = -1;   // expert pair of that position; -1 = no map
  bool on = false; // the map has targets
  int16_t pot[2][6][64];
};
struct GamePlan {
  int32_t w[2][6][64];   // weight of each target, 16 x evaluation units (the average needs the fraction)
  U64 pawns[2];          // the pawns at the previous search
  int pair = -1;         // the root's expert pair at the previous search (-1 = no previous search)
  int score = -infinity; // root score at the end of the previous search, the root side's view
  int side = -1;         // root side of the previous search
  bool followed = false; // the move chosen by the previous search advanced the plan
  bool any = false;      // some weight > 0
  bool skip = true;      // this search keeps no plan (Plan 0, root in check)
};
static GamePlan g_plan;
static PlanMap g_plan_map;

static void plan_clear() {
  g_plan = GamePlan{};
  std::memset(g_plan.w, 0, sizeof(g_plan.w));
  g_plan.pawns[0] = g_plan.pawns[1] = 0;
  g_plan_map.on = false;
  g_plan_map.pair = -1;
  g_plan_map.key = 0;
}

static void plan_reset_weights() {
  std::memset(g_plan.w, 0, sizeof(g_plan.w));
  g_plan.any = false;
  g_plan.followed = false;
}

// Value of a quiet move m of `us` on the plan's map (the move's squares and piece only).
static inline int plan_value(int us, int m) {
  const int from = move_from(m), to = move_to(m), kind = kPieceKind[move_piece(m)];
  return g_plan_map.pot[us][kind][to] - g_plan_map.pot[us][kind][from];
}

// The plan's map holds in this node: built for this search, and the same material regime as the root.
static inline bool plan_node(const ThreadData &td) { return g_plan_map.on && c2_experts_td(td) == g_plan_map.pair; }

// Squares one move away for a piece of type `kind` and colour c on the frozen occupancy `occ`, endpoints included. A
// pawn steps one square towards the opponent and never onto the first or the last rank (a8 = 0: White steps down).
static inline U64 plan_step(int kind, int c, int sq, U64 occ) {
  constexpr U64 pawn_ranks = 0x00FFFFFFFFFFFF00ULL;
  switch (kind) {
  case P:  return (c == white ? (1ULL << sq) >> 8 : (1ULL << sq) << 8) & pawn_ranks;
  case N:  return knight_attacks[sq];
  case B:  return get_bishop_attacks(sq, occ);
  case R:  return get_rook_attacks(sq, occ);
  case Q:  return get_queen_attacks(sq, occ);
  default: return king_attacks[sq];
  }
}

// Breadth-first search from `src` up to `reach` moves: dist[] = moves needed (-1 = not reached). Only empty squares are
// expanded; occupied squares get a distance as endpoints. For N, B, R, Q and K the moves are symmetric, so the same
// search read from a target gives the distance TO it; for pawns the backward search steps with the other colour.
static U64 plan_bfs(int kind, int c, int src, U64 occ, int reach, signed char *dist) {
  for (int i = 0; i < 64; i++)
    dist[i] = -1;
  dist[src] = 0;
  U64 seen = 1ULL << src, frontier = seen;
  for (int d = 1; d <= reach && frontier; d++) {
    U64 next = 0;
    for (U64 t = frontier; t; pop_lsb_bb(t)) {
      const U64 fresh = plan_step(kind, c, get_ls1b_index(t), occ) & ~seen;
      seen |= fresh;
      next |= fresh & ~occ;
      for (U64 u = fresh; u; pop_lsb_bb(u))
        dist[get_ls1b_index(u)] = (signed char)d;
    }
    frontier = next;
  }
  return seen;
}

// Squares attacked by colour c on the occupancy occ.
static U64 plan_attacks_by(const ThreadData &td, int c, U64 occ) {
  const int o = 6 * c;
  const U64 pw = td.bitboards[P + o];
  U64 a = c == white ? ((pw >> 7) & not_a_file) | ((pw >> 9) & not_h_file)
                     : ((pw << 7) & not_h_file) | ((pw << 9) & not_a_file);
  for (U64 t = td.bitboards[N + o]; t; pop_lsb_bb(t)) a |= knight_attacks[get_ls1b_index(t)];
  for (U64 t = td.bitboards[B + o] | td.bitboards[Q + o]; t; pop_lsb_bb(t)) a |= get_bishop_attacks(get_ls1b_index(t), occ);
  for (U64 t = td.bitboards[R + o] | td.bitboards[Q + o]; t; pop_lsb_bb(t)) a |= get_rook_attacks(get_ls1b_index(t), occ);
  a |= king_attacks[get_ls1b_index(td.bitboards[K + o])];
  return a;
}

// The side to move passes, for the probes of the opponent's pieces (board and key only: the network does not depend
// on the side to move, and nothing of the search frames is touched).
struct PlanNull {
  int enpassant;
  U64 hash;
};
static inline void plan_null_in(ThreadData &td, PlanNull &g) {
  g.enpassant = td.enpassant;
  g.hash = td.hash_key;
  if (td.enpassant != no_sq)
    td.hash_key ^= enpassant_keys[td.enpassant];
  td.enpassant = no_sq;
  td.hash_key ^= side_key;
  td.side ^= 1;
}
static inline void plan_null_out(ThreadData &td, const PlanNull &g) {
  td.side ^= 1;
  td.enpassant = g.enpassant;
  td.hash_key = g.hash;
}

// One relocation judged by the network: the piece pc of the side to move goes from `from` to `to` as a plain quiet
// move, the other side is to move. The evaluation from the mover's point of view, or INT_MIN when the relocation leaves
// the mover's king in check or gives check. A relocation along a line with pieces between `from` and `to` is not
// probed: the network's incremental update of the threats takes a move along a line to cross only empty squares
// (nn_step_move skips the rays that contain both ends); those targets are reached by a detour.
static int plan_probe(ThreadData &td, int pc, int from, int to) {
  if (nn_ray_pass[from][to] && (between_tbl[from][to] & td.occupancies[both]))
    return INT_MIN;
  UndoInfo u;
  const int m = encode_move(from, to, pc, 0, 0, 0, 0, 0);
  if (!td_make_move(td, m, u))
    return INT_MIN;
  int v = INT_MIN;
  if (!td_is_square_attacked(td, get_ls1b_index(td.bitboards[td.side == white ? K : k]), td.side ^ 1))
    v = -static_evaluate(td);
  td_unmake_move(td, m, u);
  return v;
}

// Two relocations of colour c (the side to move) judged together: a first, then, with c to move again, b. INT_MIN
// when either is illegal, gives check, or slides through pieces (plan_probe's rule).
static int plan_probe2(ThreadData &td, int pa, int fa, int ta, int pb, int fb, int tb) {
  if (nn_ray_pass[fa][ta] && (between_tbl[fa][ta] & td.occupancies[both]))
    return INT_MIN;
  UndoInfo ua, ub;
  const int ma = encode_move(fa, ta, pa, 0, 0, 0, 0, 0);
  if (!td_make_move(td, ma, ua))
    return INT_MIN;
  int v = INT_MIN;
  if (!td_is_square_attacked(td, get_ls1b_index(td.bitboards[td.side == white ? K : k]), td.side ^ 1)) {
    PlanNull g;
    plan_null_in(td, g);
    if (!(nn_ray_pass[fb][tb] && (between_tbl[fb][tb] & td.occupancies[both]))) {
      const int mb = encode_move(fb, tb, pb, 0, 0, 0, 0, 0);
      if (td_make_move(td, mb, ub)) {
        if (!td_is_square_attacked(td, get_ls1b_index(td.bitboards[td.side == white ? K : k]), td.side ^ 1))
          v = -static_evaluate(td);
        td_unmake_move(td, mb, ub);
      }
    }
    plan_null_out(td, g);
  }
  td_unmake_move(td, ma, ua);
  return v;
}

// Structure squares of colour c (a8 = 0, White moves to lower indices): outposts (ranks 4-6 of c that no enemy pawn on
// an adjacent file can reach to attack, defended by an own pawn), the squares behind the own passed pawns on their
// file, and the passed pawns themselves.
static void plan_structure(int c, U64 own_pw, U64 enemy_pw, U64 &outposts, U64 &behind, U64 &passed) {
  outposts = behind = passed = 0;
  for (int sq = 0; sq < 64; sq++) {
    const int file = sq & 7, row = sq >> 3;
    const int rank = c == white ? 8 - row : row + 1;   // 1..8 from c's side
    U64 ahead = 0;   // files file-1..file+1 strictly ahead of sq, from c's point of view
    for (int f = std::max(0, file - 1); f <= std::min(7, file + 1); f++)
      for (int r = 0; r < 8; r++)
        if (c == white ? r < row : r > row)
          ahead |= 1ULL << (r * 8 + f);
    if (rank >= 4 && rank <= 6 && !(ahead & enemy_pw & ~(0x0101010101010101ULL << file)) &&
        (pawn_attacks[c ^ 1][sq] & own_pw))
      outposts |= 1ULL << sq;
    if ((own_pw >> sq & 1) && !(ahead & enemy_pw)) {
      passed |= 1ULL << sq;
      for (int r = 0; r < 8; r++)
        if (c == white ? r > row : r < row)
          behind |= 1ULL << (r * 8 + file);
    }
  }
}

// Fresh targets of colour c, the side to move on td; base = evaluation of the unchanged position with the other side
// to move, from c's point of view. The best gain of each (piece type, square) goes into fresh[][], the single
// placements kept into singles[] for the pairs.
static void plan_collect_side(ThreadData &td, int c, int base, int16_t (*fresh)[64], PlanTarget *singles, int &ns) {
  const int o = 6 * c, them = c ^ 1;
  const U64 occ = td.occupancies[both];
  const U64 own_pw = td.bitboards[P + o], epw = td.bitboards[P + 6 * them];
  const U64 by_pawn = c == white ? ((epw << 7) & not_h_file) | ((epw << 9) & not_a_file)
                                 : ((epw >> 7) & not_a_file) | ((epw >> 9) & not_h_file);
  const U64 by_all = plan_attacks_by(td, them, occ);
  const bool king_walks = !td.bitboards[Q + 6 * them];
  const bool structure = PlanSources & 4;
  U64 outposts = 0, behind = 0, passed = 0;
  if (structure)
    plan_structure(c, own_pw, epw, outposts, behind, passed);
  signed char dist[64], dist2[64];
  const int keep = std::min((int)PlanK, 8);
  for (int kind = P; kind <= K; kind++) {
    if (kind == K && !king_walks)
      continue;
    for (U64 pieces = td.bitboards[kind + o]; pieces; pop_lsb_bb(pieces)) {
      const int from = get_ls1b_index(pieces);
      U64 cand, extra = 0;
      if (kind == P) {
        cand = plan_bfs(P, c, from, occ, structure ? (int)PlanPawnPush : 2, dist) & ~occ;
        if (structure && !(passed >> from & 1))   // beyond two pushes: only a lever square, or a passed pawn's path
          for (U64 t = cand; t; pop_lsb_bb(t)) {
            const int to = get_ls1b_index(t);
            if (dist[to] > 2 && !(pawn_attacks[c][to] & epw))
              cand &= ~(1ULL << to);
          }
        for (U64 t = cand; t; pop_lsb_bb(t))
          if (dist[get_ls1b_index(t)] > 2)
            extra |= 1ULL << get_ls1b_index(t);
      } else {
        cand = plan_bfs(kind, c, from, occ, PlanReach, dist) & ~occ;
        if (structure && (kind == N || kind == B || kind == R)) {
          const U64 distant = plan_bfs(kind, c, from, occ, PlanRoute, dist2) & ~occ & (kind == R ? behind : outposts);
          extra = distant & ~cand;
          cand |= distant;
        }
        cand &= kind == K ? ~by_all : ~by_pawn;
      }
      int best_gain[8], best_sq[8], nb = 0;
      for (; cand; pop_lsb_bb(cand)) {
        const int to = get_ls1b_index(cand);
        const int v = plan_probe(td, kind + o, from, to);
        if (v == INT_MIN)
          continue;
        const int gain = std::min(v - base, (int)PlanCap);
        if (gain < PlanGainMin)
          continue;
        int pos;
        if (nb < keep)
          pos = nb++;
        else if (gain > best_gain[keep - 1])
          pos = keep - 1;
        else
          continue;
        while (pos > 0 && best_gain[pos - 1] < gain) {
          best_gain[pos] = best_gain[pos - 1];
          best_sq[pos] = best_sq[pos - 1];
          pos--;
        }
        best_gain[pos] = gain;
        best_sq[pos] = to;
      }
      for (int i = 0; i < nb; i++) {
        int16_t &fr = fresh[kind][best_sq[i]];
        if ((PlanSources & 1) || (extra >> best_sq[i] & 1))
          fr = (int16_t)std::max<int>(fr, best_gain[i]);
        if (ns < PLAN_SINGLES)
          singles[ns++] = PlanTarget{(unsigned char)c, (unsigned char)kind, (unsigned char)best_sq[i],
                                     (unsigned char)from, (int16_t)best_gain[i]};
      }
    }
  }
}

// Pairs of colour c (the side to move on td): the PlanPairTop best single placements of different pieces, two at a
// time, at most PlanPairMax probes. A pair kept credits both its targets with PlanPairW/1024 of its joint gain.
static void plan_pairs(ThreadData &td, int c, int base, const PlanTarget *s, int ns, int16_t (*fresh)[64]) {
  int idx[12], ni = 0;
  bool used[PLAN_SINGLES] = {};
  const int top = std::min((int)PlanPairTop, 12);
  while (ni < top) {   // best first, one placement per piece
    int bi = -1;
    for (int i = 0; i < ns; i++) {
      if (used[i])
        continue;
      bool same_piece = false;
      for (int j = 0; j < ni; j++)
        same_piece |= s[idx[j]].from == s[i].from;
      if (same_piece) {
        used[i] = true;
        continue;
      }
      if (bi < 0 || s[i].gain > s[bi].gain)
        bi = i;
    }
    if (bi < 0)
      break;
    used[bi] = true;
    idx[ni++] = bi;
  }
  int probes = 0;
  for (int i = 0; i < ni && probes < PlanPairMax; i++)
    for (int j = i + 1; j < ni && probes < PlanPairMax; j++) {
      const PlanTarget &a = s[idx[i]], &b = s[idx[j]];
      if (a.sq == b.sq)
        continue;
      probes++;
      const int v = plan_probe2(td, a.kind + 6 * c, a.from, a.sq, b.kind + 6 * c, b.from, b.sq);
      if (v == INT_MIN)
        continue;
      const int joint = std::min(v - base, (int)PlanCap);
      if (joint < std::max(a.gain, b.gain) + PlanPairMin)
        continue;
      const int credit = joint * PlanPairW / 1024;
      fresh[a.kind][a.sq] = (int16_t)std::max<int>(fresh[a.kind][a.sq], credit);
      fresh[b.kind][b.sq] = (int16_t)std::max<int>(fresh[b.kind][b.sq], credit);
    }
}

// The map of the plan from the weights: the PlanTargets heaviest targets of each colour, weight >= PlanMin, and their
// routes (PlanRoute moves, pawns too) on the root's board.
static void plan_map_build(ThreadData &td) {
  std::memset(g_plan_map.pot, 0, sizeof(g_plan_map.pot));
  const U64 occ = td.occupancies[both];
  const int reach = PlanRoute;
  signed char dist[64];
  int n = 0;
  for (int c = 0; c < 2; c++) {
    int wv[6 * 64], wi[6 * 64], nc = 0;
    for (int i = 0; i < 6 * 64; i++) {
      const int w = g_plan.w[c][i / 64][i % 64];
      if (w >= 16 * PlanMin)
        wv[nc] = w, wi[nc++] = i;
    }
    const int take = std::min(nc, std::min((int)PlanTargets, 64));
    for (int t = 0; t < take; t++) {   // selection of the heaviest, in order (nc is at most a few hundred)
      int b = t;
      for (int j = t + 1; j < nc; j++)
        if (wv[j] > wv[b] || (wv[j] == wv[b] && wi[j] < wi[b]))
          b = j;
      std::swap(wv[t], wv[b]);
      std::swap(wi[t], wi[b]);
      const int kind = wi[t] / 64, sq = wi[t] % 64, gain = std::min(wv[t] / 16, (int)PlanCap);
      // backwards: the squares from which the piece reaches the target (pawns step with the other colour)
      plan_bfs(kind, kind == P ? c ^ 1 : c, sq, occ, reach, dist);
      int16_t *pot = g_plan_map.pot[c][kind];
      for (int x = 0; x < 64; x++)
        if (dist[x] >= 0) {
          const int v = gain * (reach + 1 - dist[x]) / (reach + 1);
          if (v > pot[x])
            pot[x] = (int16_t)v;
        }
      n++;
    }
  }
  g_plan_map.on = n > 0;
  g_plan_map.pair = g_plan.pair;
  g_plan_map.key = td.hash_key;
}

// At the start of the search (start_search, before the helpers start), on the main thread at the root: ends the plan
// when the regime or the pawn structure changed, adds the fresh targets to the average and builds the plan's map.
static void plan_begin(ThreadData &td, bool in_check) {
  g_plan_map.on = false;
  g_plan_map.pair = -1;
  g_plan.skip = !Plan || in_check;   // in check no plan is formed, and the plan of the game stays as it was
  if (g_plan.skip)
    return;
  nn_root_sync(td);
  const int pair = c2_experts_td(td);
  if (g_plan.pair >= 0 && g_plan.any) {
    const int moved = count_bits(td.bitboards[P] ^ g_plan.pawns[white]) + count_bits(td.bitboards[p] ^ g_plan.pawns[black]);
    if (pair != g_plan.pair || moved > PlanPawnBreak)
      plan_reset_weights();
  }
  g_plan.pair = pair;
  g_plan.pawns[white] = td.bitboards[P];
  g_plan.pawns[black] = td.bitboards[p];

  static int16_t fresh[2][6][64];   // main thread only, before the helpers start
  std::memset(fresh, 0, sizeof(fresh));
  PlanTarget singles[PLAN_SINGLES];
  const int us = td.side, them = us ^ 1;
  PlanNull g;
  plan_null_in(td, g);
  const int base_us = -static_evaluate(td);   // the position with the opponent to move, from our point of view
  plan_null_out(td, g);
  int ns = 0;
  plan_collect_side(td, us, base_us, fresh[us], singles, ns);
  if (PlanSources & 2)
    plan_pairs(td, us, base_us, singles, ns, fresh[us]);
  const int base_them = -static_evaluate(td);   // the position with us to move, from the opponent's point of view
  plan_null_in(td, g);
  ns = 0;
  plan_collect_side(td, them, base_them, fresh[them], singles, ns);
  if (PlanSources & 2)
    plan_pairs(td, them, base_them, singles, ns, fresh[them]);
  plan_null_out(td, g);

  const int keep = PlanKeep, add = 1024 - keep;
  bool any = false;
  for (int c = 0; c < 2; c++)
    for (int kind = 0; kind < 6; kind++)
      for (int sq = 0; sq < 64; sq++) {
        int32_t &w = g_plan.w[c][kind][sq];
        w = (int32_t)(((long long)w * keep + (long long)fresh[c][kind][sq] * 16 * add) / 1024);
        any |= w > 0;
      }
  g_plan.any = any;
  plan_map_build(td);
}

// At the end of the search (start_search, the threads stopped, before the bestmove), on the main thread at the root:
// the evaluation's verdict on the plan, the squares reached at the end of the line played, and the anchors for the next
// search. `line` = the line of the thread that gives the bestmove.
static void plan_end(ThreadData &td, const RootLine &line) {
  if (g_plan.skip)
    return;
  const int score = line.score, side = td.side;
  if (g_plan.any && g_plan.followed && g_plan.side == side && score != -infinity && g_plan.score != -infinity &&
      !val_decided(score) && !val_decided(g_plan.score) && g_plan.score - score >= PlanContra)
    plan_reset_weights();
  if ((PlanSources & 8) && line.pv_len >= 2) {   // the squares reached at the end of the line, both colours
    const int len = std::min(line.pv_len, std::min((int)PlanPvPly, 32));
    U64 root_bb[12];
    std::memcpy(root_bb, td.bitboards, sizeof(root_bb));
    UndoInfo u[32];
    int played = 0;
    for (; played < len; played++) {
      const int m = line.pv[played];
      if (!move_ok(m) || !td_is_pseudo_legal(td, m) || !td_make_move(td, m, u[played]))
        break;
    }
    U64 leaf[12];
    std::memcpy(leaf, td.bitboards, sizeof(leaf));
    while (played > 0) {
      played--;
      td_unmake_move(td, line.pv[played], u[played]);
    }
    const U64 occ = td.occupancies[both];
    const int add = (int)((long long)PlanPvGain * 16 * (1024 - PlanKeep) / 1024);
    for (int pc = P; pc <= k; pc++)
      for (U64 b = leaf[pc] & ~root_bb[pc] & ~occ; b; pop_lsb_bb(b)) {
        int32_t &w = g_plan.w[pc >= p][kPieceKind[pc]][get_ls1b_index(b)];
        w = std::min<int32_t>(w + add, 16 * (int32_t)PlanCap);
        g_plan.any = true;
      }
  }
  const int bm = line.pv_len ? line.pv[0] : 0;
  g_plan.followed = g_plan_map.on && bm && !get_move_capture(bm) && plan_value(side, bm) >= PlanRootMin;
  g_plan.score = score;
  g_plan.side = side;
}

// The reduction terms of a quiet move m of `us` (units 1/1024 ply, positive = less reduction): PlanLmr in proportion
// to the move's value on the plan, PlanLmrLow also in proportion to how low its statistic score is. 0 off the plan.
static inline int plan_lmr_less(int us, int m, int stat_score) {
  const int v = std::clamp(plan_value(us, m), -(int)PlanScale, (int)PlanScale);
  if (v <= 0)
    return 0;
  int less = PlanLmr * v / PlanScale;
  if (PlanLmrLow)
    less += (int)((long long)PlanLmrLow * std::clamp(-stat_score, 0, 8192) / 8192 * v / PlanScale);
  return less;
}

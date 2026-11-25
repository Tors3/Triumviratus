// search/15_tdperft.inc -- `tdperft N`: perft on the PER-THREAD board (the search one), with checks.
// Part of threads.cpp: it is NOT a translation unit of its own. Development builds only (#ifndef TRIUMV_FROZEN).
//
// Born in audit D (26/09/2026, 171 positions, 482M nodes, 0 errors) and brought into the source on 27/09 for
// Chess960 (tools/perft960.py --cmd tdperft). The classic perft uses the GLOBAL make_move: this one instead goes through
// td_generate_moves / td_make_move / td_unmake_move, and at every node checks:
//   - hash, pawn key, non-pawn and minor/major keys recomputed from scratch against the incremental ones;
//   - the mailbox against the bitboards;
//   - td_is_pseudo_legal on the PARENT's moves (like a TT or killer move arriving from another position):
//     false positive = accepts a move that is not generated here (plfp, must be 0).
#ifndef TRIUMV_FROZEN
static U64 tdp_key_mism = 0, tdp_pl_fp = 0, tdp_pl_fn = 0, tdp_nodes_checked = 0;
static void tdp_check_keys(ThreadData &td) {
  U64 h = 0, pk = 0, npw = 0, npb = 0, mm0 = 0, mm1 = 0;
  for (int pc = P; pc <= k; pc++) {
    U64 bb = td.bitboards[pc];
    while (bb) {
      int sq = get_ls1b_index(bb);
      h ^= piece_keys[pc][sq];
      if (pc == P || pc == p) pk ^= piece_keys[pc][sq];
      if (pc >= N && pc <= K) npw ^= piece_keys[pc][sq];
      if (pc >= n && pc <= k) npb ^= piece_keys[pc][sq];
      if (pc == N || pc == B || pc == n || pc == b) mm0 ^= piece_keys[pc][sq];
      if (pc == R || pc == Q || pc == r || pc == q) mm1 ^= piece_keys[pc][sq];
      if (td.piece_on[sq] != pc) {
        if (tdp_key_mism < 5) printf("info string MAILBOX MISMATCH sq=%d\n", sq);
        tdp_key_mism++;
      }
      pop_bit(bb, sq);
    }
  }
  if (td.enpassant != no_sq) h ^= enpassant_keys[td.enpassant];
  h ^= castle_keys[td.castle];
  if (td.side == black) h ^= side_key;
  tdp_nodes_checked++;
  const bool np_bad = (npw != td.np_key[white] || npb != td.np_key[black]);
  U64 occ_w = 0, occ_b = 0;
  for (int pc = P; pc <= K; pc++) occ_w |= td.bitboards[pc];
  for (int pc = p; pc <= k; pc++) occ_b |= td.bitboards[pc];
  const bool occ_bad = occ_w != td.occupancies[white] || occ_b != td.occupancies[black] ||
                       (occ_w | occ_b) != td.occupancies[both];
  if (h != td.hash_key || pk != td.pawn_key || np_bad || occ_bad ||
      mm0 != td.mm_key[0] || mm1 != td.mm_key[1]) {
    if (tdp_key_mism < 5)
      printf("info string KEY MISMATCH hash=%d pawn=%d np=%d mm=%d occ=%d\n", h != td.hash_key,
             pk != td.pawn_key, (npw != td.np_key[white]) || (npb != td.np_key[black]),
             (mm0 != td.mm_key[0]) || (mm1 != td.mm_key[1]), occ_bad);
    tdp_key_mism++;
  }
}
// GL (15/10/2026): at every node the legal generator (movegen_legal.h) must give exactly the moves of the usual
// generator that the make accepts, for all moves and for the two stages of the queue (tacticals and quiets), in check and not.
static U64 tdp_legal_mism = 0;
static void tdp_old_legal(ThreadData &td, moves *out, bool captures_only, bool quiets_only, bool promo_quiet,
                          bool promo_queen_only) {
  moves ml[1];
  if (td.side == white)
    td_generate_moves_side<white>(td, ml, captures_only, quiets_only, -1, promo_quiet, promo_queen_only, false);
  else
    td_generate_moves_side<black>(td, ml, captures_only, quiets_only, -1, promo_quiet, promo_queen_only, false);
  out->count = 0;
  for (int i = 0; i < ml->count; i++) {
    UndoInfo u;
    td.ply++;
    td.repetition_table[++td.repetition_index] = td.hash_key;
    if (td_make_move(td, ml->moves[i], u)) {
      out->moves[out->count++] = ml->moves[i];
      td_unmake_move(td, ml->moves[i], u);
    }
    td.ply--;
    td.repetition_index--;
  }
}
static void tdp_check_legal(ThreadData &td) {
  if (!td_lists_legal())
    return;
  static const struct { bool c, q, pq, pqo; const char *name; } shapes[3] = {
      {false, false, false, false, "all"}, {true, false, true, true, "tactics"}, {false, true, false, false, "quiets"}};
  for (const auto &s : shapes) {
    moves want[1], got[1];
    tdp_old_legal(td, want, s.c, s.q, s.pq, s.pqo);
    td_generate_moves(td, got, s.c, s.q, -1, s.pq, s.pqo);
    std::sort(want->moves, want->moves + want->count);
    std::sort(got->moves, got->moves + got->count);
    if (want->count != got->count || !std::equal(want->moves, want->moves + want->count, got->moves)) {
      if (tdp_legal_mism < 5)
        printf("info string LEGAL MOVEGEN MISMATCH (%s): %d moves, expected %d\n", s.name, got->count, want->count);
      tdp_legal_mism++;
    }
  }
}
static U64 tdp_rec(ThreadData &td, int depth, const moves *parent) {
  moves ml[1];
  td_generate_moves(td, ml);
  // The pseudo-legality of the parent's moves is checked against the usual pseudo-legal list.
  tdp_check_legal(td);
  moves pl_list[1];
  if (td.side == white)
    td_generate_moves_side<white>(td, pl_list, false, false, -1, false, false, false);
  else
    td_generate_moves_side<black>(td, pl_list, false, false, -1, false, false, false);
  const moves *gen = td_lists_legal() ? pl_list : ml;
  if (parent) {  // parent's moves (like a killer/TT move from another position): pseudo-legal only if generated here
    for (int i = 0; i < parent->count; i++) {
      const int m = parent->moves[i];
      bool in_list = false;
      for (int j = 0; j < gen->count; j++) if (gen->moves[j] == m) { in_list = true; break; }
      const bool pl = td_is_pseudo_legal(td, m);
      if (pl && !in_list) { if (tdp_pl_fp < 5) printf("info string PSEUDO-LEGAL FALSE POSITIVE move=%d\n", m); tdp_pl_fp++; }
      if (!pl && in_list) tdp_pl_fn++;
    }
  }
  if (depth == 0) return 1;
  U64 n = 0;
  for (int i = 0; i < ml->count; i++) {
    const int mv = ml->moves[i];
    UndoInfo u;
    td.ply++;
    td.repetition_table[++td.repetition_index] = td.hash_key;
    if (!td_make_move(td, mv, u)) { td.ply--; td.repetition_index--; tdp_check_keys(td); continue; }
    tdp_check_keys(td);
    n += tdp_rec(td, depth - 1, ml);
    td_unmake_move(td, mv, u);
    td.ply--;
    td.repetition_index--;
  }
  return n;
}
void td_perft_driver(int depth) {
  ThreadData &td = thread_data[0];
  copy_board_to_thread(td);
  nn_root_sync(td);
  tdp_key_mism = tdp_pl_fp = tdp_pl_fn = tdp_nodes_checked = 0;
  tdp_check_keys(td);
  tdp_legal_mism = 0;
  tdp_check_legal(td);
  // true perft: counts the leaves at depth depth (tdp_rec counts the leaves when depth==0)
  U64 leaves = 0;
  moves ml[1];
  td_generate_moves(td, ml);
  for (int i = 0; i < ml->count; i++) {
    const int mv = ml->moves[i];
    UndoInfo u;
    td.ply++;
    td.repetition_table[++td.repetition_index] = td.hash_key;
    if (!td_make_move(td, mv, u)) { td.ply--; td.repetition_index--; tdp_check_keys(td); continue; }
    tdp_check_keys(td);
    leaves += depth > 1 ? tdp_rec(td, depth - 1, ml) : 1;
    td_unmake_move(td, mv, u);
    td.ply--;
    td.repetition_index--;
  }
  tdp_check_keys(td);
  printf("tdperft %d Nodes: %llu keymism %llu plfp %llu plfn %llu checked %llu\n", depth,
         (unsigned long long)leaves, (unsigned long long)tdp_key_mism, (unsigned long long)tdp_pl_fp,
         (unsigned long long)tdp_pl_fn, (unsigned long long)tdp_nodes_checked);
  fflush(stdout);
  printf("tdperft legal movegen: %s, mismatches %llu\n", td_lists_legal() ? "on" : "off (Chess960)",
         (unsigned long long)tdp_legal_mism);
  fflush(stdout);
}

// `nnperft N` (07/10/2026, single board): perft on the per-thread board that EVALUATES with the network, with the
// nnueverify check on (incremental chain against full refresh, whole accumulators). It evaluates at every
// leaf and at one internal node in three (chosen by the key), so that the incremental chain has different lengths; at
// one internal node in five not in check a null move is also inserted. It covers all the moves of the suite:
// castling (Chess960 too), en passant, promotions, promotion captures. The keys and the
// mailbox are checked too (as in tdperft) and, since v2, that the dirty stack has one state for every move made from the root.
static U64 nnp_evals = 0;
static int nnp_moves = 0;  // (non-null) moves made from the root
static bool nnp_lean = false;  // nnspeed: only the evaluation, no checks
static inline void nnp_eval(ThreadData &td) {
  td_nn_eval(td);
  nnp_evals++;
  if (nnp_lean) return;
  tdp_check_keys(td);
  if (td.nnstack->size != 1 + nnp_moves) {
    if (tdp_key_mism < 5) printf("info string NN STACK MISMATCH stati=%d mosse=%d\n", td.nnstack->size, nnp_moves);
    tdp_key_mism++;
  }
  // Review 21/10/2026: the C2 experts the search ideas read from the stack (c2_experts_td, c2_expert) must be those of
  // the board, also after null moves, promotions and en passant.
  if (c2_experts_td(td) != c2_experts(c2_count(td.bitboards))) {
    if (tdp_key_mism < 5) printf("info string C2 EXPERT MISMATCH stack=%02x board=%02x\n", c2_experts_td(td),
                                 c2_experts(c2_count(td.bitboards)));
    tdp_key_mism++;
  }
}
static U64 nnp_rec(ThreadData &td, int depth) {
  const bool evaluated = depth == 0 || td.hash_key % 3 == 0;
  if (evaluated) {
    td.nn_disagree = -1;   // set again only by an incremental network evaluation (the fresh case below)
    nnp_eval(td);
  }
  // Conversion push (C2ConvPkSrc 1, 21/10/2026): the exact pawn-king effect the search would read, right after an
  // evaluation (fresh) and, at one internal node in seven not evaluated, on demand (now); the bridge compares both with
  // the value from scratch under nnueverify (a difference counts in "bad"). Only with the option set.
  if (C2ConvPkSrc && !nnp_lean) {
    if (evaluated && td.nn_disagree >= 0)
      c2_pk_effect_fresh(td);
    else if (!evaluated && td.hash_key % 7 == 0)
      c2_pk_effect_now(td);
  }
  if (depth == 0) return 1;
  U64 n = 0;
  const int ksq = get_ls1b_index(td.side == white ? td.bitboards[K] : td.bitboards[k]);
  if (depth >= 2 && td.hash_key % 5 == 0 && !td_is_square_attacked(td, ksq, td.side ^ 1)) {
    // null move: only the side changes (and the en passant); the network records nothing
    const int ep = td.enpassant;
    const U64 hk = td.hash_key;
    if (ep != no_sq) td.hash_key ^= enpassant_keys[ep];
    td.hash_key ^= side_key;
    td.enpassant = no_sq;
    td.side ^= 1;
    nnp_rec(td, depth - 1);
    td.side ^= 1;
    td.enpassant = ep;
    td.hash_key = hk;
  }
  moves ml[1];
  td_generate_moves(td, ml);
  for (int i = 0; i < ml->count; i++) {
    const int mv = ml->moves[i];
    UndoInfo u;
    td.ply++;
    td.repetition_table[++td.repetition_index] = td.hash_key;
    if (!td_make_move(td, mv, u)) { td.ply--; td.repetition_index--; continue; }
    nnp_moves++;
    n += nnp_rec(td, depth - 1);
    td_unmake_move(td, mv, u);
    nnp_moves--;
    td.ply--;
    td.repetition_index--;
  }
  return n;
}
void td_nnperft_driver(int depth) {
  ThreadData &td = thread_data[0];
  copy_board_to_thread(td);
  nn_root_sync(td);
  c2_eval_push();   // the C2 evaluation terms as set now (review 21/10/2026), so that the check covers them
  nn_set_verify(1);
  const unsigned long long c0 = nn_verify_count(), b0 = nn_verify_bad();
  tdp_key_mism = 0;
  nnp_evals = 0;
  nnp_moves = 0;
  const U64 leaves = nnp_rec(td, depth);
  tdp_check_keys(td);
  nn_set_verify(0);
  printf("nnperft %d Nodes: %llu evals %llu verified %llu bad %llu keymism %llu\n", depth,
         (unsigned long long)leaves, (unsigned long long)nnp_evals, nn_verify_count() - c0, nn_verify_bad() - b0,
         (unsigned long long)tdp_key_mism);
  fflush(stdout);
}

// `nnspeed N <fen file>` (21/10/2026, cost of Consilium Plebis): nnperft's tree (evaluation at the leaves and at one internal
// node in three, a null move at one internal node in five) WITHOUT the verification and the key checks, from every
// position of the file. The tree depends only on the move generator, so two networks do exactly the same work and the
// time per evaluation compares their inference (incremental updates through make/unmake included, as in a search).
void td_nnspeed_driver(int depth, const char *path) {
  extern void parse_fen(const char *fen);
  FILE *f = fopen(path, "r");
  if (!f) {
    printf("info string nnspeed: cannot open %s\n", path);
    fflush(stdout);
    return;
  }
  char line[512];
  U64 leaves = 0, evals = 0;
  int npos = 0;
  nnp_lean = true;
  const auto t0 = std::chrono::steady_clock::now();
  while (fgets(line, sizeof line, f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    if (!len) continue;
    parse_fen(line);
    ThreadData &td = thread_data[0];
    copy_board_to_thread(td);
    nn_root_sync(td);
    nnp_evals = 0;
    nnp_moves = 0;
    leaves += nnp_rec(td, depth);
    evals += nnp_evals;
    npos++;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  nnp_lean = false;
  fclose(f);
  printf("nnspeed %d positions %d nodes %llu evals %llu time %.0f ms %.2f ns/eval\n", depth, npos,
         (unsigned long long)leaves, (unsigned long long)evals, ms, evals ? 1e6 * ms / double(evals) : 0.0);
  fflush(stdout);
}
#endif  // TRIUMV_FROZEN

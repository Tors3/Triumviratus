// search/10_order.inc -- Move validation, repetitions, move ordering.
// Part of threads.cpp: it is NOT a translation unit of its own.
// The first two sections (validation, upcoming repetition) are the engine's own; the repetition table was
// rewritten on 20/10/2026 (open addressing instead of the cuckoo pair of tables).

// Pseudo-legality validator. The staged picker yields the TT move, killers and
// the counter-move WITHOUT first generating the move list, so it must confirm
// each is a real move in THIS position: td_make_move trusts the encoded bits
// and would corrupt the board on a TT collision. This MUST NEVER return true
// for an illegal move; when unsure it returns false and the move simply
// reappears during normal generation (we only lose a little laziness, never
// correctness). Mirrors td_generate_moves.
static inline bool td_is_pseudo_legal(ThreadData &td, int move) {
  if (move == 0)
    return false;
  const int source = get_move_source(move);
  const int target = get_move_target(move);
  const int piece = get_move_piece(move);
  const int promoted = get_move_promoted(move);
  const int capture = get_move_capture(move) ? 1 : 0;
  const int dbl = get_move_double(move) ? 1 : 0;
  const int enpass = get_move_enpassant(move) ? 1 : 0;
  const int castling = get_move_castling(move) ? 1 : 0;

  // The moving piece must belong to the side to move and sit on `source`.
  if (td.side == white) {
    if (piece < P || piece > K)
      return false;
  } else {
    if (piece < p || piece > k)
      return false;
  }
  if (!get_bit(td.bitboards[piece], source))
    return false;

  // Chess960: castling must be checked BEFORE the "friendly destination" test below, because the king can
  // arrive on the square of its own rook or stay still. Same rule as the generation (chess960.h).
  if (g_chess960 && castling) {
    if (get_move_capture(move) || promoted || enpass || dbl)
      return false;
    const int idx = castle_index(target);
    const int us = td.side;
    if (piece != (us == white ? K : k) || (idx >> 1) != us || source != castle_king_sq[us] ||
        target != castle_king_to(idx))
      return false;
    auto att = [&td, us](int sq) { return td_is_square_attacked(td, sq, us ^ 1) != 0; };
    return c960_can_castle(td.castle, idx, td.occupancies[both], att);
  }

  const U64 occ = td.occupancies[both];
  const U64 friends = td.occupancies[td.side];
  const U64 enemies = td.occupancies[td.side ^ 1];

  // A target holding a friendly piece is never legal (also rejects
  // source==target).
  if (get_bit(friends, target))
    return false;

  const bool isPawn = (piece == P || piece == p);
  const bool isKing = (piece == K || piece == k);

  // ---- Castling ----
  if (castling) {
    if (!isKing || capture || promoted || enpass || dbl)
      return false;
    if (td.side == white) {
      if (source != e1)
        return false;
      if (target == g1)
        return (td.castle & wk) && !get_bit(occ, f1) && !get_bit(occ, g1) &&
               !td_is_square_attacked(td, e1, black) &&
               !td_is_square_attacked(td, f1, black);
      if (target == c1)
        return (td.castle & wq) && !get_bit(occ, d1) && !get_bit(occ, c1) &&
               !get_bit(occ, b1) && !td_is_square_attacked(td, e1, black) &&
               !td_is_square_attacked(td, d1, black);
      return false;
    } else {
      if (source != e8)
        return false;
      if (target == g8)
        return (td.castle & bk) && !get_bit(occ, f8) && !get_bit(occ, g8) &&
               !td_is_square_attacked(td, e8, white) &&
               !td_is_square_attacked(td, f8, white);
      if (target == c8)
        return (td.castle & bq) && !get_bit(occ, d8) && !get_bit(occ, c8) &&
               !get_bit(occ, b8) && !td_is_square_attacked(td, e8, white) &&
               !td_is_square_attacked(td, d8, white);
      return false;
    }
  }

  // Only pawns may carry promotion / double-push / en-passant flags.
  if (!isPawn && (promoted || dbl || enpass))
    return false;

  // ---- Pawns ----
  if (isPawn) {
    const int up = (td.side == white) ? -8 : 8; // board layout: a8=0 ... h1=63
    const bool toLastRank =
        (td.side == white) ? (target <= h8) : (target >= a1);

    // A pawn reaching the last rank MUST promote (and the promoted piece must
    // belong to the side to move); promotion off the last rank is illegal.
    if (promoted) {
      if (!toLastRank)
        return false;
      if (td.side == white) {
        if (promoted < N || promoted > Q)
          return false;
      } else {
        if (promoted < n || promoted > q)
          return false;
      }
    } else if (toLastRank && !enpass) {
      return false;
    }

    if (capture) {
      if (!get_bit(pawn_attacks[td.side][source], target))
        return false;
      if (enpass) {
        if (dbl)
          return false;
        if (td.enpassant == no_sq || target != td.enpassant)
          return false;
        const int capsq = (td.side == white) ? target + 8 : target - 8;
        const int eppawn = (td.side == white) ? p : P;
        return get_bit(td.bitboards[eppawn], capsq) != 0;
      }
      return get_bit(enemies, target) != 0; // ordinary capture
    } else {
      if (enpass)
        return false;
      if (get_bit(occ, target))
        return false; // push onto empty square only
      if (dbl) {
        const bool onStart = (td.side == white)
                                 ? (source >= a2 && source <= h2)
                                 : (source >= a7 && source <= h7);
        if (!onStart)
          return false;
        if (target != source + 2 * up)
          return false;
        return !get_bit(occ, source + up); // intermediate square empty
      }
      return (target == source + up);
    }
  }

  // ---- Knight / Bishop / Rook / Queen / King (non-castling) ----
  U64 att;
  switch (piece) {
  case N:
  case n:
    att = knight_attacks[source];
    break;
  case B:
  case b:
    att = get_bishop_attacks(source, occ);
    break;
  case R:
  case r:
    att = get_rook_attacks(source, occ);
    break;
  case Q:
  case q:
    att = get_queen_attacks(source, occ);
    break;
  case K:
  case k:
    att = king_attacks[source];
    break;
  default:
    return false;
  }
  if (!get_bit(att, target))
    return false;
  // The capture flag must agree with whether an enemy occupies the target
  // square.
  return (capture != 0) == (get_bit(enemies, target) != 0);
}

// ============================================================================
// UPCOMING REPETITION
// Can the side to move close a cycle with one reversible move? The idea is Marcel van Kervinck's (2013): the key
// difference between the current position and an older one is the key of a single reversible move exactly when one
// move of ours, on a free path, would recreate that position. We keep the keys of all reversible moves (a piece other
// than a pawn going between two squares it attacks on an empty board, 3,668 of them) in an open-addressing table with
// linear probing, indexed by the top bits of the key.
// ============================================================================
static constexpr int REV_SLOTS = 8192;           // power of two; 3,668 keys, load 0.45
static U64 rev_key[REV_SLOTS];                   // 0 = empty slot (a real key is never 0)
static int rev_move[REV_SLOTS];                  // from | to << 6
// (between_tbl is defined earlier, before td_generate_moves: the evasions use it too.)

static inline int rev_slot(U64 key) { return (int)(key >> 51); }   // 13 top bits

// The move whose key is `key`, or -1 if no reversible move has that key.
static inline int rev_move_of(U64 key) {
  for (int i = rev_slot(key);; i = (i + 1) & (REV_SLOTS - 1)) {
    if (rev_key[i] == key)
      return rev_move[i];
    if (!rev_key[i])
      return -1;
  }
}

static void init_cycle_tables() {
  memset(rev_key, 0, sizeof(rev_key));
  memset(rev_move, 0, sizeof(rev_move));

  for (int s1 = 0; s1 < 64; s1++) {   // empty-board attacks (07_makemove.inc)
    empty_rook_att[s1] = get_rook_attacks(s1, 0ULL);
    empty_bishop_att[s1] = get_bishop_attacks(s1, 0ULL);
  }
  // Squares strictly between two aligned squares: the rays from each end with the other end as the only blocker
  // intersect exactly there (0 for knight jumps and squares not on a line).
  for (int s1 = 0; s1 < 64; s1++)
    for (int s2 = 0; s2 < 64; s2++) {
      between_tbl[s1][s2] = 0;
      if (s1 == s2)
        continue;
      if (get_rook_attacks(s1, 0ULL) & (1ULL << s2))
        between_tbl[s1][s2] = get_rook_attacks(s1, 1ULL << s2) & get_rook_attacks(s2, 1ULL << s1);
      else if (get_bishop_attacks(s1, 0ULL) & (1ULL << s2))
        between_tbl[s1][s2] = get_bishop_attacks(s1, 1ULL << s2) & get_bishop_attacks(s2, 1ULL << s1);
    }

  static const int movers[10] = {N, B, R, Q, K, n, b, r, q, k};
  for (int pc : movers)
    for (int from = 0; from < 64; from++) {
      const int t = pc % 6;   // 1 knight, 2 bishop, 3 rook, 4 queen, 5 king
      U64 reach = t == 1 ? knight_attacks[from] : t == 2 ? get_bishop_attacks(from, 0ULL)
                : t == 3 ? get_rook_attacks(from, 0ULL) : t == 4 ? get_queen_attacks(from, 0ULL) : king_attacks[from];
      reach &= ~((2ULL << from) - 1);   // each unordered pair once: destinations above `from`
      while (reach) {
        const int to = get_ls1b_index(reach);
        reach &= reach - 1;
        const U64 key = piece_keys[pc][from] ^ piece_keys[pc][to] ^ side_key;
        int i = rev_slot(key);
        while (rev_key[i])
          i = (i + 1) & (REV_SLOTS - 1);
        rev_key[i] = key;
        rev_move[i] = from | (to << 6);
      }
    }
}

// True if the side to move has a reversible move that recreates a position of the reversible window (the last
// min(fifty, plies since the null move) plies), on our flat history: repetition_table[repetition_index] is the
// position one ply ago, [repetition_index - d] the one d + 1 plies ago. Entries pushed by null moves are inert: their
// key difference is side and en passant only, never two piece keys.
// (25/11/2026, speed, same logic) Called at most nodes of the search and of the quiescence (whenever alpha is below
// the draw), and most calls end on the window test: that test is inline (td_upcoming_repetition, below) and only a
// window of three plies or more pays the call, as in drawn_here (12_quiesce.inc).
__attribute__((noinline)) static bool td_upcoming_repetition_slow(ThreadData &td, int window) {
  PROF_GUARD(prof_rep);
  const U64 now = td.hash_key;
  const U64 *hist = td.repetition_table + td.repetition_index;   // hist[-d]: d + 1 plies ago
  // Key change made by the opponent's moves inside the window; a cycle closable by one move of ours needs their
  // pieces back where they were, i.e. this sum back to zero (baked 10/10/2026, +5.62 +- 4.47 over 6,500 games).
  U64 theirs = now ^ hist[0] ^ side_key;
  for (int back = 3; back <= window; back += 2) {
    const U64 older = hist[-(back - 1)];
    theirs ^= hist[-(back - 2)] ^ older ^ side_key;
    if (theirs)
      continue;
    const int mv = rev_move_of(now ^ older);
    if (mv < 0)
      continue;
    const int a = mv & 63, b = (mv >> 6) & 63;
    if (between_tbl[a][b] & td.occupancies[both])
      continue;   // path blocked
    if (td.ply > back)
      return true;   // the whole cycle is inside the searched line
    // Reaching behind the root: only a move of the side to move, and only if the older position was already a
    // repetition in the game.
    const int on = get_bit(td.occupancies[both], a) ? a : b;
    if (!get_bit(td.occupancies[td.side], on))
      continue;
    for (int k = back + 2; k <= window; k += 2)
      if (hist[-(k - 1)] == older)
        return true;
  }
  return false;
}
static inline bool td_upcoming_repetition(ThreadData &td) {
  const int window = std::min(std::min(td.fifty, td.plies_from_null), td.repetition_index);
  if (window < 3)
    return false;
  return td_upcoming_repetition_slow(td, window);
}

// Draw by repetition. The current position is repeated if it appears again in the reversible window
// (min(fifty, plies since the null move)); it counts as a draw if the previous occurrence is inside the searched tree, or if
// it was itself already a repetition (three times in the game history).
static bool repeated_draw(const ThreadData &td) {
  int end = td.fifty < td.plies_from_null ? td.fifty : td.plies_from_null;
  if (end > td.repetition_index)
    end = td.repetition_index;
  for (int i = 4; i <= end; i += 2) {
    if (td.repetition_table[td.repetition_index - (i - 1)] != td.hash_key)
      continue;
    if (i < td.ply)
      return true;
    for (int j = i + 4; j <= end; j += 2)
      if (td.repetition_table[td.repetition_index - (j - 1)] == td.hash_key)
        return true;
    return false;
  }
  return false;
}

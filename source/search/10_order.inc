// search/10_order.inc -- Validazione delle mosse, ripetizioni, ordinamento delle mosse.
// Parte di threads.cpp: NON e' un'unita' di compilazione a se'.
// Le prime due sezioni (validazione e cuckoo) vengono dal motore precedente, invariate.

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

  // Chess960: l'arrocco va verificato PRIMA del controllo "destinazione amica" qui sotto, perche' il re puo'
  // arrivare sulla casa della propria torre o restare fermo. Stessa regola della generazione (chess960.h).
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
// UPCOMING REPETITION (P1.2, 2026-06-09) — cuckoo, Kervinck / SF has_game_cycle
// ============================================================================
// cuckoo_key[i] = piece_keys[pc][s1] ^ piece_keys[pc][s2] ^ side_key per OGNI
// mossa reversibile (pezzi non-pedone, coppie raggiungibili su board vuota;
// 3668 in tutto, come SF). cuckoo_move[i] = s1 | (s2 << 6). between_tbl[s1][s2]
// = case STRETTAMENTE tra s1 e s2 (0 per cavallo / case non allineate).
static U64 cuckoo_key[8192];
static int cuckoo_move[8192];
// (between_tbl definita piu' sopra, prima di td_generate_moves: serve anche
//  alle evasioni P2.3, non solo all'upcoming-repetition.)

static inline int cuckoo_h1(U64 h) { return (int)(h & 0x1FFF); }
static inline int cuckoo_h2(U64 h) { return (int)((h >> 16) & 0x1FFF); }

static void init_cuckoo() {
  memset(cuckoo_key, 0, sizeof(cuckoo_key));
  memset(cuckoo_move, 0, sizeof(cuckoo_move));

  for (int s1 = 0; s1 < 64; s1++) {   // attacchi a scacchiera vuota (07_makemove.inc)
    empty_rook_att[s1] = get_rook_attacks(s1, 0ULL);
    empty_bishop_att[s1] = get_bishop_attacks(s1, 0ULL);
  }
  for (int s1 = 0; s1 < 64; s1++)
    for (int s2 = 0; s2 < 64; s2++) {
      between_tbl[s1][s2] = 0;
      if (s1 == s2)
        continue;
      // Intersezione dei raggi con l'altra casa come unica occupazione =
      // case strettamente comprese (gli estremi si elidono da soli).
      if (get_rook_attacks(s1, 0ULL) & (1ULL << s2))
        between_tbl[s1][s2] =
            get_rook_attacks(s1, 1ULL << s2) & get_rook_attacks(s2, 1ULL << s1);
      else if (get_bishop_attacks(s1, 0ULL) & (1ULL << s2))
        between_tbl[s1][s2] = get_bishop_attacks(s1, 1ULL << s2) &
                              get_bishop_attacks(s2, 1ULL << s1);
    }

  static const int rev_pieces[10] = {N, B, R, Q, K, n, b, r, q, k};
  for (int pi = 0; pi < 10; pi++) {
    const int pc = rev_pieces[pi];
    for (int s1 = 0; s1 < 64; s1++) {
      U64 att;
      switch (pc % 6) { // 1=N 2=B 3=R 4=Q 5=K (mai pedoni)
      case 1:
        att = knight_attacks[s1];
        break;
      case 2:
        att = get_bishop_attacks(s1, 0ULL);
        break;
      case 3:
        att = get_rook_attacks(s1, 0ULL);
        break;
      case 4:
        att = get_queen_attacks(s1, 0ULL);
        break;
      default:
        att = king_attacks[s1];
        break;
      }
      for (int s2 = s1 + 1; s2 < 64; s2++) {
        if (!(att & (1ULL << s2)))
          continue;
        U64 key = piece_keys[pc][s1] ^ piece_keys[pc][s2] ^ side_key;
        int mv = s1 | (s2 << 6);
        int i = cuckoo_h1(key);
        while (true) { // inserimento cuckoo (2 sedi possibili)
          U64 tk = cuckoo_key[i];
          cuckoo_key[i] = key;
          key = tk;
          int tm = cuckoo_move[i];
          cuckoo_move[i] = mv;
          mv = tm;
          if (key == 0)
            break; // slot era vuoto: catena chiusa
          i = (i == cuckoo_h1(key)) ? cuckoo_h2(key) : cuckoo_h1(key);
        }
      }
    }
  }
}

// True se il lato al tratto ha una mossa reversibile che chiude un ciclo verso
// una posizione gia' vista nelle ultime `fifty` semimosse (port di SF
// has_game_cycle sulla nostra repetition_table piatta: [repetition_index] =
// posizione 1 ply fa, [repetition_index-1] = 2 ply fa, ...). Le entry create
// dalle NULL move sono inerti: la loro moveKey differisce solo per side/ep, mai
// per due piece-key, e non puo' combaciare con una chiave cuckoo -> nessun
// falso positivo.
static bool td_upcoming_repetition(ThreadData &td) {
  PROF_GUARD(prof_rep);
  int end = td.fifty < td.plies_from_null ? td.fifty : td.plies_from_null;
  if (end > td.repetition_index)
    end = td.repetition_index;
  if (end < 3)
    return false;
  const U64 orig = td.hash_key;
  // RepCycleOther (10/10/2026, segnalato dall'agente velocita'): il filtro "other" di SF19. Somma le differenze di
  // chiave delle mosse dell'AVVERSARIO lungo la finestra; il ciclo vale solo se si annullano (i suoi pezzi sono
  // tornati dov'erano), cioe' se a chiuderlo basta una mossa del lato al tratto. Senza, la tabella cuckoo trova anche
  // cicli fatti da una mossa dell'avversario (~0,08-0,24% dei nodi). Spenta = forma di SF16, albero invariato.
  // Chiave di k semimosse fa: repetition_table[repetition_index - (k - 1)].
  U64 other = orig ^ td.repetition_table[td.repetition_index] ^ side_key;
  for (int i = 3; i <= end; i += 2) {
    const U64 prev = td.repetition_table[td.repetition_index - (i - 1)];
    other ^= td.repetition_table[td.repetition_index - (i - 2)] ^ prev ^ side_key;
    if (RepCycleOther && other)
      continue;
    const U64 move_key = orig ^ prev;
    int j;
    if ((j = cuckoo_h1(move_key), cuckoo_key[j] == move_key) ||
        (j = cuckoo_h2(move_key), cuckoo_key[j] == move_key)) {
      const int s1 = cuckoo_move[j] & 63, s2 = (cuckoo_move[j] >> 6) & 63;
      if (between_tbl[s1][s2] & td.occupancies[both])
        continue; // percorso bloccato
      if (td.ply > i)
        return true; // ciclo tutto dentro il search path
      // Ciclo che pesca dietro la radice: vale solo se la mossa e' del lato al
      // tratto E la vecchia posizione era gia' una ripetizione (regola SF).
      const int occ_sq = get_bit(td.occupancies[both], s1) ? s1 : s2;
      if (!get_bit(td.occupancies[td.side], occ_sq))
        continue;
      for (int k2 = i + 2; k2 <= end; k2 += 2)
        if (td.repetition_table[td.repetition_index - (k2 - 1)] == prev)
          return true;
    }
  }
  return false;
}

// Patta per ripetizione. La posizione corrente e' ripetuta se compare di nuovo nella finestra reversibile
// (min(fifty, plies dalla mossa nulla)); conta come patta se la volta precedente e' dentro l'albero cercato, o se
// era gia' a sua volta una ripetizione (tre volte nella storia della partita).
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

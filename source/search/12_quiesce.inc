// search/12_quiesce.inc -- Strumenti dei nodi (tabella hash, legalita', scacco, passi avanti e indietro) e
// ricerca di quiescenza.
// Parte di threads.cpp: NON e' un'unita' di compilazione a se'.

// ============================================================================
// TABELLA HASH: lettura e scrittura nelle unita' e nelle convenzioni della ricerca
// La profondita' si salva spostata di 3: -1 = quiescenza, -2 = solo valutazione, -3 = vuota.
// ============================================================================
constexpr int HD_QUIESCE = -1;
constexpr int HD_EVAL    = -2;
constexpr int HD_SHIFT   = 3;
// La quiescenza scrive e legge profondita' 0 in hash (05/10/2026, audit del nucleo; BAKE HASHQSDEPTH 06/10, opzione
// ritirata la sera: +3,38 +- 2,58 su 20.162 partite). C'e' un livello in piu' fra "solo valutazione" (-2) e
// quiescenza: le entry di ProbCut a cut_depth + 1 = -1 restano sotto la quiescenza (non tagliano in quiesce) e a
// profondita' 4 il piccolo ProbCut accetta le entry di quiescenza. Lo schema storico usava HD_QUIESCE = -1.
static inline int hd_qs() { return 0; }

enum { BOUND_UP = hash_flag_alpha, BOUND_LOW = hash_flag_beta, BOUND_EXACT = hash_flag_exact, BOUND_NIL = hash_flag_none };

struct HashView {
  bool hit;
  bool pv;
  int  move;
  int  value;       // VAL_NONE se assente
  int  depth;
  int  bound;
  int  eval_raw;    // valutazione salvata (senza smorzamento dei 50 mosse), tt_eval_none se assente
};

// Il limite confermato dall'entry vale per il confronto richiesto? (lower = serve un limite inferiore)
static inline bool bound_covers(int bound, bool lower) {
  return bound == BOUND_EXACT || bound == (lower ? BOUND_LOW : BOUND_UP);
}

static inline int to_hash_value(int v, int ply) {
  if (!val_known(v)) return v;
  return val_won(v) ? v + ply : val_lost(v) ? v - ply : v;
}

static inline int from_hash_value(int v, int ply, int fifty) {
  if (!val_known(v)) return VAL_NONE;
  if (val_won(v)) {
    if (v >= MATE_FLOOR && mate_value - v > 100 - fifty) return TB_WIN_FLOOR - 1;
    if (TB_WIN_TOP - v > 100 - fifty) return TB_WIN_FLOOR - 1;
    return v - ply;
  }
  if (val_lost(v)) {
    if (v <= -MATE_FLOOR && mate_value + v > 100 - fifty) return -TB_WIN_FLOOR + 1;
    if (TB_WIN_TOP + v > 100 - fifty) return -TB_WIN_FLOOR + 1;
    return v + ply;
  }
  return v;
}

// Lettura e scrittura passano da tt_key (03_tables.inc): la chiave della scacchiera piu', se accesa, la fascia del
// contatore delle 50 mosse (HashFiftyStep).
static inline void hash_read(const ThreadData &td, HashView &h) {
  int move, value, depth, flag, ev;
  bool pv;
  const bool usable = probe_tt(tt_key(td), move, value, depth, flag, ev, pv);
  h.hit = usable || flag == BOUND_NIL;
  if (!h.hit) {
    h.pv = false; h.move = 0; h.value = VAL_NONE; h.depth = -HD_SHIFT; h.bound = BOUND_NIL; h.eval_raw = tt_eval_none;
    return;
  }
  h.pv = pv;
  h.move = move;
  h.depth = depth - HD_SHIFT;
  h.bound = flag;
  h.value = flag == BOUND_NIL ? VAL_NONE : from_hash_value(value, td.ply, td.fifty);
  h.eval_raw = ev;
}

static inline void hash_write(const ThreadData &td, int value, bool pv, int bound, int depth, int move, int eval_raw) {
  store_tt(tt_key(td), move, to_hash_value(value, td.ply), depth + HD_SHIFT, bound, 0, pv, eval_raw);
}

// Toglie un ply all'entry: un limite che non ha tagliato a profondita' sufficiente vale meno.
static inline void hash_soften(U64 key) {
  tt_entry *e = tt_find(key);
  if (!e) return;
  const U64 d = e->data;
  const int dep = unpack_depth(d);
  if (dep == 0) return;
  const U64 nd = (d & ~(0xFFULL << 40)) | ((U64)(dep - 1) << 40);
  const U64 w = e->kw ^ d;
  e->data = nd;
  e->kw = w ^ nd;
}

// ProgressFade (08/10/2026, audit X3, spenta): smorzamento in piu' della valutazione oltre ProgressFrom semimosse del
// contatore delle 50 mosse, applicato DOPO la lettura (in hash resta il valore senza smorzamento, come per quello della
// rete), tetto a meta' valore. Chi e' avanti preferisce le mosse che azzerano il contatore (pedoni, catture).
static inline int progress_fade(int v, int fifty) {
  if (!ProgressFade || fifty <= ProgressFrom)
    return v;
  const int k = std::min((fifty - ProgressFrom) * ProgressFade, 512);
  return v - (int)((long long)v * k / 1024);
}

// Valutazione statica del nodo: dalla hash se c'e', altrimenti dalla rete. raw = formato da salvare in hash.
static inline int node_eval(ThreadData &td, const HashView &h, int &raw) {
  if (h.hit && h.eval_raw != tt_eval_none) {
    raw = h.eval_raw;
    return progress_fade(eval_refade(h.eval_raw, td.fifty), td.fifty);
  }
  const int v = static_evaluate(td);
  raw = eval_unfade(v, td.fifty);
  return progress_fade(v, td.fifty);
}

// Idea 3 (Contempt, 04/10/2026): la patta vale -c per il colore al tratto alla radice e +c per l'avversario.
// Punta all'Elo di lista (CCRL: ~90% di patte), non al self-play. 0 = spento, valori di patta di prima.
static inline int contempt(const ThreadData &td) {
  return Contempt ? (td.side == td.root_side ? -Contempt : Contempt) : 0;
}

// Patta a valore quasi nullo: +-1 secondo i nodi, per non trattare tutte le patte come identiche.
static inline int draw_score(const ThreadData &td) { return -1 + (int)(td.nodes & 2) + contempt(td); }

// ============================================================================
// LEGALITA' E SCACCO PRIMA DI MUOVERE
// ============================================================================

// a, b, c sulla stessa linea (riga, colonna o diagonale)?
static inline bool on_line(int a, int b, int c) {
  return (between_tbl[a][b] >> c & 1) || (between_tbl[a][c] >> b & 1) || (between_tbl[b][c] >> a & 1);
}

static inline U64 attackers_with(const ThreadData &td, int sq, int by, U64 occ) {
  const int off = by == white ? 0 : 6;
  return (pawn_attacks[by ^ 1][sq] & td.bitboards[P + off]) | (knight_attacks[sq] & td.bitboards[N + off]) |
         (king_attacks[sq] & td.bitboards[K + off]) |
         (get_bishop_attacks(sq, occ) & (td.bitboards[B + off] | td.bitboards[Q + off])) |
         (get_rook_attacks(sq, occ) & (td.bitboards[R + off] | td.bitboards[Q + off]));
}

// Contesto del nodo per legalita' (inchiodature del nostro re) e scacco (case di scacco sul re nemico),
// calcolato una volta alla prima mossa.
struct MoveSafety {
  bool ready;
  int  our_king, their_king;
  U64  pinned;
  LmpChkCtx checks;
};

static void safety_init(ThreadData &td, MoveSafety &s) {
  const int us = td.side, them = us ^ 1;
  const int off = us == white ? 0 : 6, eoff = them == white ? 0 : 6;
  s.our_king = get_ls1b_index(td.bitboards[K + off]);
  s.their_king = get_ls1b_index(td.bitboards[K + eoff]);
  const U64 occ = td.occupancies[both];
  U64 snipers = (empty_rook_att[s.our_king] & (td.bitboards[R + eoff] | td.bitboards[Q + eoff])) |
                (empty_bishop_att[s.our_king] & (td.bitboards[B + eoff] | td.bitboards[Q + eoff]));
  s.pinned = 0;
  while (snipers) {
    const int sn = get_ls1b_index(snipers);
    const U64 between = between_tbl[s.our_king][sn] & occ;
    if (between && !(between & (between - 1)) && (between & td.occupancies[us]))
      s.pinned |= between;
    pop_lsb_bb(snipers);
  }
  if (!s.checks.ready)   // l'ordinamento delle quiete puo' averle gia' calcolate
    td_lmp_chk_init(td, s.checks);
  s.ready = true;
}

// 06/10/2026 (velocita', stessa logica): legal_before chiamata ~2,2 volte per nodo (contatori, bench 14 su fens30) e,
// nel binario PGO, fuori linea con il prologo completo (otto registri salvati e ripresi) anche per il caso comune:
// pezzo non re, niente arrocco ne' e.p., di solito non inchiodato, ~10 istruzioni di lavoro. Ora il caso comune e'
// inline e i tre casi rari (arrocco, en passant, mosse di re) stanno in legal_before_slow, fuori linea.
__attribute__((noinline)) static bool legal_before_slow(ThreadData &td, MoveSafety &s, int m) {
  const int from = move_from(m), to = move_to(m), us = td.side, them = us ^ 1;
  if (get_move_castling(m))
    return g_chess960 || !td_is_square_attacked(td, to, them);
  if (get_move_enpassant(m)) {
    const int capsq = us == white ? to + 8 : to - 8;
    const U64 occ = (td.occupancies[both] ^ (1ULL << from) ^ (1ULL << capsq)) | (1ULL << to);
    const int eoff = them == white ? 0 : 6;
    return !(get_rook_attacks(s.our_king, occ) & (td.bitboards[R + eoff] | td.bitboards[Q + eoff])) &&
           !(get_bishop_attacks(s.our_king, occ) & (td.bitboards[B + eoff] | td.bitboards[Q + eoff]));
  }
  if (from == s.our_king)
    return !attackers_with(td, to, them, td.occupancies[both] ^ (1ULL << from));
  return !(s.pinned >> from & 1) || on_line(from, to, s.our_king);
}

static inline bool legal_before(ThreadData &td, MoveSafety &s, int m) {
  PROF_GUARD(prof_gc);
  if (!s.ready) safety_init(td, s);
  const int from = move_from(m);
  if ((m & 0xC00000) || from == s.our_king)   // arrocco (bit 23), en passant (bit 22), re
    return legal_before_slow(td, s, m);
  return !(s.pinned >> from & 1) || on_line(from, move_to(m), s.our_king);
}

static bool checks_before(ThreadData &td, MoveSafety &s, int m) {
  PROF_GUARD(prof_gc);
  if (!s.ready) safety_init(td, s);
  const int from = move_from(m), to = move_to(m), pc = move_piece(m), pt = pc - (td.side == white ? 0 : 6);
  const bool castling = get_move_castling(m) != 0;
  if (!castling && (s.checks.chk[pt] >> to & 1))
    return true;
  if ((s.checks.disc >> from & 1) && (castling || !on_line(from, to, s.their_king)))
    return true;
  const U64 occ = td.occupancies[both];
  const int off = td.side == white ? 0 : 6;
  if (get_move_promoted(m)) {
    const U64 o = occ ^ (1ULL << from);
    U64 att = 0;
    switch (get_move_promoted(m) % 6) {
    case N: att = knight_attacks[to]; break;
    case B: att = get_bishop_attacks(to, o); break;
    case R: att = get_rook_attacks(to, o); break;
    default: att = get_queen_attacks(to, o); break;
    }
    return att >> s.their_king & 1;
  }
  if (get_move_enpassant(m)) {
    const int capsq = td.side == white ? to + 8 : to - 8;
    const U64 o = (occ ^ (1ULL << from) ^ (1ULL << capsq)) | (1ULL << to);
    return (get_rook_attacks(s.their_king, o) & (td.bitboards[R + off] | td.bitboards[Q + off])) ||
           (get_bishop_attacks(s.their_king, o) & (td.bitboards[B + off] | td.bitboards[Q + off]));
  }
  if (castling) {
    int rook_pc, rf, rt;
    td_castle_rook(to, rook_pc, rf, rt);
    return s.checks.chk[R] >> rt & 1;
  }
  return false;
}

// ============================================================================
// PASSI: entrare in un figlio e tornare indietro
// ============================================================================
static inline bool step_in(ThreadData &td, NodeFrame *f, int m, UndoInfo &u) {
  td.repetition_table[++td.repetition_index] = td.hash_key;
  td.ply++;
  // ⛔ 04/10/2026 — TOLTA la "legalita' decisa una volta" (make con known_legal = true quando legal_before aveva
  // deciso): nessun guadagno misurabile, e legal_before NON e' completo sotto scacco. Vale per una mossa pseudo-legale
  // a re non sotto scacco o per una parata del generatore delle parate; la mossa hash, le killer e la contromossa
  // passano solo dalla pseudo-legalita', e una di loro che non parava lo scacco veniva giocata. Il re si catturava al
  // ply dopo e la posizione della rete restava senza re: crash in release (1 motore su 16 nel training PGO, 3 partite
  // perse per disconnessione su 197 contro SF19). La verifica con -DTRIUMV_VERIFY_LEGAL su 3,9 milioni di nodi non
  // l'aveva trovato.
  // ✅ 04/10/2026 sera — RIPRESA SOLO DOVE E' COMPLETA: a re NON sotto scacco legal_before decide da solo (inchiodature,
  // re che non entra in presa, en passant, arrocco) e la make salta il test; sotto scacco il test resta. Arrocco del
  // 960 sempre alla make. Con -DTRIUMV_VERIFY_LEGAL la make rifa' sempre il test e stampa ogni disaccordo.
#ifdef TRIUMV_VERIFY_LEGAL
  if (!f->in_check && !td_make_move(td, m, u)) {
    printf("info string LEGALITA' SBAGLIATA: mossa %d ritenuta legale\n", m);
    fflush(stdout);
    td.ply--;
    td.repetition_index--;
    return false;
  }
  if (f->in_check && !td_make_move(td, m, u)) {
#else
  if (!td_make_move(td, m, u, !f->in_check && !(g_chess960 && get_move_castling(m)))) {
#endif
    td.ply--;
    td.repetition_index--;
    return false;
  }
  td.nodes++;
  f->move = m;
  f->captured = u.captured_piece;
  f->cont_hist = &g_stats.follow[f->in_check][is_tactical(m)][move_piece(m)][move_to(m)];
  f->cont_corr = &td.cont_corr[move_piece(m)][move_to(m)];
  return true;
}

static inline void step_out(ThreadData &td, int m, UndoInfo &u) {
  td_unmake_move(td, m, u);
  td.ply--;
  td.repetition_index--;
}

struct NullUndo {
  int enpassant, fifty, plies_from_null;
  U64 hash;
};

static inline void step_in_null(ThreadData &td, NodeFrame *f, NullUndo &u) {
  td.repetition_table[++td.repetition_index] = td.hash_key;
  td.ply++;
  u.enpassant = td.enpassant;
  u.fifty = td.fifty;
  u.plies_from_null = td.plies_from_null;
  u.hash = td.hash_key;
  if (td.enpassant != no_sq)
    td.hash_key ^= enpassant_keys[td.enpassant];
  td.enpassant = no_sq;
  td.hash_key ^= side_key;
  td.side ^= 1;
  // La mossa nulla non incrementa il contatore delle 50 mosse (05/10/2026, audit del nucleo; BAKE PULIZIA 06/10,
  // opzione NullFiftyKeep ritirata la sera).
  td.plies_from_null = 0;
  // La rete non registra nulla: i pezzi non si muovono, e gli accumulatori non dipendono dal lato al tratto.
  f->move = NULL_MOVE;
  f->captured = -1;
  f->cont_hist = &g_stats.follow[0][0][NO_PC][0];
  f->cont_corr = &td.cont_corr[NO_PC][0];
}

static inline void step_out_null(ThreadData &td, NullUndo &u) {
  td.side ^= 1;
  td.enpassant = u.enpassant;
  td.fifty = u.fifty;
  td.plies_from_null = u.plies_from_null;
  td.hash_key = u.hash;
  td.ply--;
  td.repetition_index--;
}

// Patta per la regola delle 50 mosse (salvo matto) o per ripetizione.
// 06/10/2026 (velocita', stessa logica): drawn_here gira a ogni nodo (~1,1 volte per nodo) e nel binario PGO e' fuori
// linea con un frame da 1,1 KB (la lista di mosse del caso "50 mosse sotto scacco"). In oltre l'80% delle chiamate la
// finestra reversibile e' sotto i 4 ply e repeated_draw non fa nemmeno un giro: quel caso ora esce inline.
__attribute__((noinline)) static bool drawn_here_slow(ThreadData &td, bool in_check) {
  PROF_GUARD(prof_rep);
  if (td.fifty > 99) {
    if (!in_check)
      return true;
    moves ml[1];
    td_generate_moves(td, ml, false, false, 1);
    for (int i = 0; i < ml->count; i++) {
      UndoInfo u;
      td.repetition_table[++td.repetition_index] = td.hash_key;
      td.ply++;
      const bool ok = td_make_move(td, ml->moves[i], u) != 0;
      if (ok)
        td_unmake_move(td, ml->moves[i], u);
      td.ply--;
      td.repetition_index--;
      if (ok)
        return true;
    }
  }
  return repeated_draw(td);
}

static inline bool drawn_here(ThreadData &td, bool in_check) {
  if (td.fifty <= 99) {   // stessa finestra di repeated_draw: con meno di 4 ply non c'e' ripetizione possibile
    int end = td.fifty < td.plies_from_null ? td.fifty : td.plies_from_null;
    if (end > td.repetition_index)
      end = td.repetition_index;
    if (end < 4)
      return false;
  }
  return drawn_here_slow(td, in_check);
}

static bool has_legal_move(ThreadData &td) {
  moves ml[1];
  td_generate_moves(td, ml, false);
  for (int i = 0; i < ml->count; i++) {
    UndoInfo u;
    td.repetition_table[++td.repetition_index] = td.hash_key;
    td.ply++;
    const bool ok = td_make_move(td, ml->moves[i], u) != 0;
    if (ok)
      td_unmake_move(td, ml->moves[i], u);
    td.ply--;
    td.repetition_index--;
    if (ok)
      return true;
  }
  return false;
}

static inline void pv_extend(ThreadData &td, int m) {
  const int ply = td.ply;
  td.pv_table[ply][ply] = m;
  const int child_len = td.pv_length[ply + 1];
  for (int i = ply + 1; i < child_len; i++)
    td.pv_table[ply][i] = td.pv_table[ply + 1][i];
  td.pv_length[ply] = child_len > ply + 1 ? child_len : ply + 1;
}

// ============================================================================
// QUIESCENZA
// ============================================================================

// Idea 7 (QsPasserPush, 04/10/2026): nei finali la spinta di un pedone in 7a traversa (minaccia di promozione
// immediata) e' tattica. Un pedone che arriva in 7a e' passato per costruzione: davanti ha solo l'ottava. Una sola
// spinta per nodo; 0 se non ce n'e' o se c'e' troppo materiale (QsPushNpm; fino al 10/10/2026 era PassedPushNpm, la
// soglia di PassedPushRed, con lo stesso valore di default).
static int qs_passer_push(const ThreadData &td) {
  int npm = 0;
  for (int t = N; t <= Q; t++)
    npm += unit_value[t] * (count_bits(td.bitboards[t]) + count_bits(td.bitboards[t + 6]));
  if (npm > QsPushNpm)
    return 0;
  const int us = td.side, pc = us == white ? P : p;
  // a8 = 0: la 6a traversa del bianco e' la riga 2 (case 16-23), quella del nero la riga 5 (case 40-47).
  U64 cand = td.bitboards[pc] & (us == white ? 0x0000000000FF0000ULL : 0x0000FF0000000000ULL);
  while (cand) {
    const int from = get_ls1b_index(cand);
    const int to = us == white ? from - 8 : from + 8;
    if (!((td.occupancies[both] >> to) & 1))
      return encode_move(from, to, pc, 0, 0, 0, 0, 0);
    pop_lsb_bb(cand);
  }
  return 0;
}

// QCHK (AnQsChecks, modalita' analisi, 09/10/2026, idea nostra): primo ply della quiescenza chiamato dalla ricerca
// principale. Dopo le tattiche si provano anche le mosse quiete che danno scacco con SEE >= 0; il figlio (sotto
// scacco) e' una quiescenza normale. QCHK = false e' la quiescenza di sempre, identica.
// qply: ply di quiescenza dal nodo della ricerca principale (0 al primo), per QsPushPly.
template <bool QCHK = false>
static int quiesce(ThreadData &td, NodeFrame *f, int alpha, int beta, bool pv, int qply = 0) {
  td.pv_length[td.ply] = td.ply;
  if (alpha < contempt(td) && td_upcoming_repetition(td)) {   // soglia = patta per chi muove, come in explore
    alpha = draw_score(td);
    if (alpha >= beta)
      return alpha;
  }
  if (pv && td.sel_depth < td.ply + 1)
    td.sel_depth = td.ply + 1;
  const bool in_check = f->in_check;

  if (td.ply >= max_ply)
    return in_check ? 0 : static_evaluate(td);
  if (drawn_here(td, in_check))
    return contempt(td);

  HashView h;
  hash_read(td, h);
  f->tt_hit = h.hit;
  const bool hash_pv = h.hit && h.pv;
  if (!pv && h.depth >= hd_qs() && val_known(h.value) && bound_covers(h.bound, h.value >= beta))
    return h.value;

  int best, fut_base, eval_raw = tt_eval_none;
  if (in_check) {
    best = fut_base = -infinity;
  } else {
    const int signal = corr_signal(td, f);
    const int plain = node_eval(td, h, eval_raw);
    f->static_eval = best = corrected_eval(plain, signal + corr_signal_extra(td));
    if (h.hit && val_known(h.value) && !val_decided(h.value) && bound_covers(h.bound, h.value > best))
      best = h.value;
    if (best >= beta) {
      if (!val_decided(best))
        best = (441 * best + 583 * beta) / 1024;
      if (!h.hit)
        hash_write(td, VAL_NONE, false, BOUND_LOW, HD_EVAL, 0, eval_raw);
      return best;
    }
    if (best > alpha)
      alpha = best;
    fut_base = f->static_eval + QsFutMargin;
  }

  const int prev = f[-1].move;
  const int prev_to = move_ok(prev) ? move_to(prev) : -1;
  MoveQueue q;
  queue_start(q, td, f, h.move, 0, in_check ? QK_EVADE : QK_QUIESCE);
  MoveSafety safety;
  safety.ready = false;
  int best_move = 0, count = 0, m;
  // Spinta in 7a (QsPasserPush): giocata dopo le tattiche, una volta sola. Se e' gia' la mossa hash la coda l'ha
  // emessa per prima: riproporla la cercava due volte nello stesso nodo (05/10/2026, audit dei porting).
  const int qs_push = QsPasserPush && !in_check && qply < QsPushPly ? qs_passer_push(td) : 0;
  bool push_left = qs_push != 0 && qs_push != q.hash_move;
  while ((m = queue_next(q)) != 0 || (push_left && (push_left = false, (m = qs_push) != 0))) {
    if (!legal_before(td, safety, m))
      continue;
    const bool gives = checks_before(td, safety, m);
    const bool push = qs_push && m == qs_push;
    const bool tact = is_tactical(m) || push;
    count++;

    if (!val_lost(best)) {
      if (!gives && move_to(m) != prev_to && !val_lost(fut_base) && !get_move_promoted(m)) {
        if (count > 2)
          continue;
        const int fut = fut_base + (push ? unit_value[Q] - unit_value[P] : unit_value[victim_kind(td, m)]);
        if (fut <= alpha) {
          best = std::max(best, fut);
          continue;
        }
        if (!see_holds(td, m, alpha - fut_base)) {
          best = std::max(best, std::min(alpha, fut_base));
          continue;
        }
      }
      if (!tact)
        continue;
      if (!see_holds(td, m, push ? -QsPushSee : -QsSeeMargin))
        continue;
    }

    UndoInfo u;
    if (!step_in(td, f, m, u)) {
      count--;
      continue;
    }
    f[1].in_check = gives;
    const int value = -quiesce<false>(td, f + 1, -beta, -alpha, pv, qply + 1);
    step_out(td, m, u);

    if (value > best) {
      best = value;
      if (value > alpha) {
        best_move = m;
        if (pv)
          pv_extend(td, m);
        if (value < beta)
          alpha = value;
        else
          break;
      }
    }
  }

  if constexpr (QCHK) {
    if (!in_check && best < beta) {
      moves ml[1];
      td_generate_moves(td, ml, /*captures_only=*/false, /*quiets_only=*/true, /*known_in_check=*/0);
      for (int i = 0; i < ml->count && best < beta; i++) {
        const int qm = ml->moves[i];
        if (is_tactical(qm) || qm == q.hash_move || !legal_before(td, safety, qm) || !checks_before(td, safety, qm) ||
            !see_holds(td, qm, 0))
          continue;
        UndoInfo u;
        if (!step_in(td, f, qm, u))
          continue;
        count++;
        f[1].in_check = true;
        const int value = -quiesce<false>(td, f + 1, -beta, -alpha, pv, qply + 1);
        step_out(td, qm, u);
        if (stop_threads.load(std::memory_order_relaxed))
          return 0;
        if (value > best) {
          best = value;
          if (value > alpha) {
            best_move = qm;
            if (pv)
              pv_extend(td, qm);
            if (value < beta)
              alpha = value;
          }
        }
      }
    }
  }

  if (!count) {
    if (in_check)
      return mated_at(td.ply);
    // Nessuna cattura: se oltre al re restano solo pedoni bloccati e l'ultima mossa ha catturato un pezzo
    // (non un pedone), controlla lo stallo (la quiescenza non vede le mosse quiete).
    const int us = td.side, off = us == white ? 0 : 6;
    const U64 pawns = td.bitboards[P + off];
    const U64 pushes = us == white ? pawns >> 8 : pawns << 8;
    const U64 non_pawn = td.occupancies[us] & ~pawns & ~td.bitboards[K + off];
    const bool took_piece = f[-1].captured >= 0 && f[-1].captured % 6 >= N;
    if (!(pushes & ~td.occupancies[both]) && !non_pawn && took_piece && !has_legal_move(td))
      best = contempt(td);
  }

  if (!val_decided(best) && best > beta)
    best = (462 * best + 562 * beta) / 1024;
  hash_write(td, best, hash_pv, best >= beta ? BOUND_LOW : BOUND_UP, hd_qs(), best_move, eval_raw);
  return best;
}

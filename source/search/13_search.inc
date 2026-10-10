// search/13_search.inc -- Ricerca principale.
// Parte di threads.cpp: NON e' un'unita' di compilazione a se'.
//
// Un nodo passa per quattro fasi, ognuna una funzione: ingresso (patte, matti, hash, tablebase), valutazione e
// tagli prima delle mosse, ciclo delle mosse, apprendimento a fine nodo. Lo stato del nodo vive in NodeState.

// AN (09/10/2026): la ricerca esiste in due copie, partita (AN = false) e modalita' analisi (AN = true), scelte una
// volta alla radice (deepen). In partita ogni termine dell'analisi e' una costante: con i parametri Deep* congelati a 0
// della release il compilatore li toglie, come prima della modalita' analisi (xperf: la versione con i termini letti a
// ogni nodo costava ~1% di cicli in partita). Le due copie danno gli stessi valori di prima nei rispettivi modi.
template <bool AN>
static int explore(ThreadData &td, NodeFrame *f, int alpha, int beta, int depth, bool cut, bool pv);

constexpr int TRACKED = 32;   // mosse provate ricordate per i malus

struct NodeState {
  bool pv, root, cut, all;
  bool in_check, improving, opp_worse, tact_hash, prior_capture, hunt_mate;
  bool crystal;                   // AnCrystal: nodo della linea principale precedente vicino alla radice (crystal_node)
  int  alpha, beta, depth;
  int  best, best_move, cap_value;
  int  moves;                     // mosse legali cercate (il contatore del frame lo riusa la ricerca singolare)
  int  eval, eval_raw, signal;
  int  signal_x;                  // correzioni sperimentali solo nella valutazione, non nella selettivita' (CorrExpertW)
  int  prev_to;
  int  excluded;
  int  prior_red;
  HashView h;
  int  quiets[TRACKED], n_quiets;
  int  tacts[TRACKED], n_tacts;
  U64  causal;                    // CausalRed: case della confutazione dell'ultima quieta fallita bassa (0 = nessuna)
  int  expert;                    // esperto della rete per il materiale del nodo (net_phase), per i margini *Exp*
};

// Margini per esperto (10/10/2026, nostra): Consilium valuta con quattro esperti per fascia di materiale (net_phase),
// e la fiducia nella valutazione statica non e' la stessa nei quattro. Le leve RfpExp*, FutExp*, LmrExp* spostano
// il margine della futility inversa, quello della futility delle quiete e la riduzione LMR secondo l'esperto del nodo
// (estensione delle correzioni per esperto, CorrPhase +3,2). Tutte a 0 = albero invariato; nella release, con i
// parametri costanti a 0, il compilatore toglie anche il calcolo dell'esperto.
static inline int by_expert(int e, int v0, int v1, int v2, int v3) {
  return e == 0 ? v0 : e == 1 ? v1 : e == 2 ? v2 : v3;
}

// CausalRed (09/10/2026, idea di giugno "potatura causale", riscritta sulla ricerca attuale). Quando una nostra mossa
// quieta fallisce bassa, la mossa dell'avversario che la confuta (il taglio beta nel figlio) dice dove sta il problema:
// la casa da cui parte, quella dove arriva e le case che il pezzo attacca da li'. Le quiete sorelle provate dopo che
// non toccano nessuna di queste case (ne' partenza ne' arrivo) di solito non cambiano nulla contro quella confutazione,
// e si riducono di piu'. Mai potate: la ricerca ridotta che supera alpha si ripete a profondita' piena come sempre.
// Gli scacchi, la mossa hash e le spinte dei passati restano fuori. Precedente noto: la "minaccia" della mossa nulla
// usata da Stockfish 1.x-2.x per potare le mosse non collegate (connected_threat); qui la minaccia e' la confutazione
// delle sorelle e la mossa si riduce, non si pota.
static inline U64 causal_mask(const ThreadData &td, int refute) {
  const int from = get_move_source(refute), to = get_move_target(refute), pc = get_move_piece(refute);
  const U64 occ = td.occupancies[both];
  U64 att;
  switch (pc % 6) {
    case 0: att = pawn_attacks[pc >= 6 ? black : white][to]; break;
    case 1: att = knight_attacks[to]; break;
    case 2: att = get_bishop_attacks(to, occ); break;
    case 3: att = get_rook_attacks(to, occ); break;
    case 4: att = get_queen_attacks(to, occ); break;
    default: att = king_attacks[to]; break;
  }
  return att | (1ULL << from) | (1ULL << to);
}

static constexpr int NO_VALUE = 1 << 30;   // "nessun valore": la fase non ha chiuso il nodo

// Reciproci a 40 bit di 256*depth + 268 (divisore della riduzione ai nodi all, vedi move_loop): M = ceil(2^40 / D).
static constexpr auto all_node_recip = [] {
  std::array<unsigned long long, max_ply + 8> r{};
  for (int d = 0; d < max_ply + 8; d++) {
    const unsigned long long D = 256ULL * d + 268;
    r[d] = ((1ULL << 40) + D - 1) / D;
  }
  return r;
}();
// W1 (08/10/2026): reciproci di depth + 1 (media del valore di taglio in node_close), stesso troncamento.
static constexpr auto succ_recip = [] {
  std::array<unsigned long long, max_ply + 8> r{};
  for (int d = 0; d < max_ply + 8; d++)
    r[d] = ((1ULL << 40) + (unsigned long long)d) / (unsigned long long)(d + 1);
  return r;
}();

// ---------------------------------------------------------------------------
// Rampa "tempo lungo" (05/10/2026). Ogni leva ha uno scostamento Lt*: il valore effettivo e' base + Lt * peso, con il
// peso (0..1024) fissato per iterazione dalla profondita' di radice (lt_update, 14_deepen.inc). Con lo scostamento a 0
// l'accessor restituisce la leva base: nella release (costanti) il ramo sparisce e il codice e' quello di prima.
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
    if (mul != td.lt_lmr_log_mul) {   // stessa formula di build_reduction_table, col moltiplicatore effettivo
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
// Sempre il valore del thread (09/10/2026): lt_update lo calcola per iterazione e la modalita' analisi (AN_NMPV) lo
// sovrascrive; letto solo dopo una mossa nulla riuscita, quindi senza costo. In partita vale NmpVerifyDepth.
static inline int e_nmp_verify_depth(const ThreadData &td)  { return td.lt_nmp_verify_depth; }
static inline const int *e_red_log(const ThreadData &td)    { return LmrLogMulLt ? td.lt_red_log : red_log; }

// Profondita' ridotta di base: tabella logaritmica, finestra relativa e assenza di miglioramento.
// `window_term` = finestra * LmrDeltaMul / root_delta: lo calcola move_loop solo quando la finestra cambia, perche'
// la divisione per root_delta (non costante) costava ~30 cicli a ogni mossa (04/10/2026, stesso valore).
static inline int base_reduction(const ThreadData &td, bool improving, int depth, int move_no, int window_term) {
  const int *rl = e_red_log(td);
  const int scale = rl[std::min(depth, 255)] * rl[std::min(move_no, 255)];
  return scale - window_term + (!improving ? scale * e_lmr_not_improving(td) / 512 : 0) + LmrBase;
}

// ---------------------------------------------------------------------------
// Idee nostre del 04/10/2026 (docs/audit_8.0/M_STUDIO_IDEE_RICERCA.md). Tutte spente di default: con i parametri
// a 0 i termini valgono 0 e nella release (parametri costanti) il compilatore li toglie.
// ---------------------------------------------------------------------------

// Idea 2 (Deep*): scostamento "profondo" pesato da td.deep_w (0..1024, cresce con la profondita' di radice).
// Dal 09/10/2026 le sei leve che la modalita' analisi usa (Rfp, Fut, Lmr, Nmp, Sing, Ext) arrivano gia' pesate nei
// campi td.dz_* (deepen, per iterazione): stesso valore di prima, ma la modalita' analisi le puo' accendere anche
// nella release, dove i parametri sono costanti. deep_term resta per DeepHist e DeepTtPv.
static inline int deep_term(const ThreadData &td, int offset) { return offset * td.deep_w / 1024; }

// Crystal (AnCrystal, modalita' analisi): il nodo sta sulla linea principale dell'iterazione precedente, vicino
// alla radice? In partita crystal_ply = -1 e il test e' sempre falso.
static inline bool crystal_node(const ThreadData &td, const NodeFrame *f) {
  return f->ply <= td.crystal_ply && f->follow_pv;
}

// GoldDigger (AnGold, idea di ShashChess/amchess, codice nostro): la mossa nulla si salta quando il "calore" tattico
// arriva a 4. La parte della radice (re in pericolo o sacrificio possibile +10, posizione di vantaggio netto +2,
// attacco +2, pedone avanzato +2, piu' di 30 mosse legali +1) e' in td.gd_heat; qui si aggiungono il materiale non
// pedoni di chi muove (> 2000, +1) e la profondita' (> 8 e > 12, +1 ciascuna). In partita gd_heat = -1000.
static inline bool gd_skip_null(const ThreadData &td, const NodeState &n) {
  if (td.gd_heat < -100)
    return false;
  const int off = td.side == white ? 0 : 6;
  int npm = 0;
  for (int t = N; t <= Q; t++)
    npm += unit_value[t] * count_bits(td.bitboards[t + off]);
  return td.gd_heat + (npm > 2000) + (n.depth > 8) + (n.depth > 12) >= 4;
}

// Idea 4 (PhaseEdge*): la cattura cambia esperto della rete (attraversa 24, 16 o 10 pezzi)?
static inline bool capture_crosses(const ThreadData &td, int m) {
  if (!get_move_capture(m))
    return false;
  const int pieces = count_bits(td.occupancies[both]);
  return net_phase(pieces) != net_phase(pieces - 1);
}

// Controllo del tempo e dei limiti (solo il thread 0, ogni 512 nodi).
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

// Statistiche di un nodo che ha trovato la mossa migliore: premio a lei, malus alle mosse provate prima.
static void reward_best(ThreadData &td, NodeFrame *f, NodeState &n) {
  PROF_GUARD(prof_hist);
  const int bm = n.best_move;
  int bonus = std::min(BonusMul * n.depth - BonusSub, BonusMax) + (bm == n.h.move ? BonusTTMove : 0) +
              f[-1].stat_score / BonusPrevStatDiv;
  const int malus = std::min(MalusMul * n.depth - MalusSub, MalusMax);
  if (!n.pv)
    bonus += (int)((long long)bonus * (n.n_quiets + n.n_tacts) / 256);

  if (!is_tactical(bm)) {
    learn_quiet(td, f, bm, bonus * QuietBonusScale / 1024);
    int m = malus * QuietMalusScale / 1024;
    for (int i = 0; i < n.n_quiets; i++) {
      m = m * QuietMalusDecay / 1024;
      learn_quiet(td, f, n.quiets[i], -m);
    }
  } else
    learn_capture(td, bm, bonus * CaptBonusScale / 1024);

  // La mossa che ha portato qui, se quieta e cercata subito (prima mossa, o seconda dopo la mossa hash), era debole.
  if (n.prev_to >= 0 && f[-1].move_count == 1 + (f[-1].tt_hit ? 1 : 0) && !n.prior_capture)
    learn_followups(f - 1, td.piece_on[n.prev_to], n.prev_to, -malus * PrevContMalus / 1024);

  for (int i = 0; i < n.n_tacts; i++)
    learn_capture(td, n.tacts[i], -malus * CaptMalusScale / 1024);
}

// --------------------------------------------------------------------------
// Fase 1: ingresso del nodo. Ritorna un valore se il nodo si chiude qui.
// --------------------------------------------------------------------------
static int node_open(ThreadData &td, NodeFrame *f, NodeState &n) {
  // La soglia e' il valore della patta per chi muove (0 senza Contempt): cosi' alpha non scende mai (05/10/2026,
  // audit dei porting: con la soglia 0 Contempt abbassava alpha dal lato di radice).
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
  n.signal = corr_signal(td, f);
  n.signal_x = corr_signal_extra(td);
  n.excluded = f->excluded;
  n.expert = net_phase(count_bits(td.occupancies[both]));

  hash_read(td, n.h);
  f->tt_hit = n.h.hit;
  if (n.root)
    n.h.move = td.root[td.pv_idx].pv[0];
  if (!n.excluded)
    f->tt_pv = n.pv || (n.h.hit && n.h.pv);
  n.tact_hash = n.h.move && is_tactical(n.h.move);
  return NO_VALUE;
}

// Taglio dalla hash. Ritorna un valore se si taglia.
// Valore restituito da un taglio della hash. HashDamp (nostra, da Coda): un limite inferiore viene spinto verso beta.
static inline int hash_cut_value(const NodeState &n) {
  const int v = n.h.value;
  if (HashDamp && n.h.bound == BOUND_LOW && v >= n.beta && !val_decided(v))
    return (HashDamp * v + 10 * n.beta) / (HashDamp + 10);
  return v;
}

static int hash_cutoff(ThreadData &td, NodeFrame *f, NodeState &n) {
  const HashView &h = n.h;
  const int need = n.depth - (h.value <= n.beta ? 1 : 0);
  const bool enough = h.depth > need;
  // HashNearMiss (Coda: entry un ply piu' corta che taglia oltre un margine) TOLTA il 05/10/2026: -6,7 +- 10,6 su
  // 1.142 partite a 20+0.2 nella versione migliore (solo verso il basso, nodi non-cut); STATO_PROGETTO.md §3.
  if (n.pv || n.excluded || !enough || !val_known(h.value))
    return NO_VALUE;
  const bool high = h.value >= n.beta;
  if (bound_covers(h.bound, high) && (n.cut == high || n.depth > 4)) {
    if (h.move && high) {
      if (!n.tact_hash)
        learn_quiet(td, f, h.move, std::min(112 * n.depth, 695));
      if (n.prev_to >= 0 && f[-1].move_count < 5 && !n.prior_capture)
        learn_followups(f - 1, td.piece_on[n.prev_to], n.prev_to, -2210);
    }
    if (td.fifty >= 96)
      return NO_VALUE;
    // In profondita' si controlla che il figlio della mossa hash confermi lo stesso esito.
    if (n.depth >= 7 && h.move && td_is_pseudo_legal(td, h.move) && !val_decided(h.value)) {
      MoveSafety s;
      s.ready = false;
      if (legal_before(td, s, h.move)) {
        UndoInfo u;
        td.repetition_table[++td.repetition_index] = td.hash_key;
        td.ply++;
        if (td_make_move(td, h.move, u)) {
          int cm, cv, cd, cf, ce;
          bool cp;
          const bool hit = probe_tt(tt_key(td), cm, cv, cd, cf, ce, cp) || cf == BOUND_NIL;
          const int child = hit && cf != BOUND_NIL ? from_hash_value(cv, td.ply, td.fifty) : VAL_NONE;
          td_unmake_move(td, h.move, u);
          td.ply--;
          td.repetition_index--;
          if (!val_known(child) || (h.value >= n.beta) == (-child >= n.beta))
            return hash_cut_value(n);
          return NO_VALUE;
        }
        td.ply--;
        td.repetition_index--;
      }
    }
    return hash_cut_value(n);
  }
  // Nessun taglio pur con un limite profondo che contraddice la finestra: l'entry vale meno.
  if (h.bound != BOUND_EXACT && h.bound == (high ? BOUND_UP : BOUND_LOW) && n.depth > 5)
    hash_soften(tt_key(td));
  return NO_VALUE;
}

// Tablebase nei nodi interni.
static int tablebase_probe(ThreadData &td, NodeFrame *f, NodeState &n) {
  if (n.root || n.excluded)
    return NO_VALUE;
  unsigned men = syzygy_max_pieces();
  if (g_syzygy_probe_limit >= 0 && (unsigned)g_syzygy_probe_limit < men)
    men = (unsigned)g_syzygy_probe_limit;
  if (!men || td.castle || td.fifty)
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
    tb = contempt(td);   // patta da tablebase: lo stesso valore delle altre patte (0 senza Contempt)
  if (bound == BOUND_EXACT || (bound == BOUND_LOW ? tb >= n.beta : tb <= n.alpha)) {
    hash_write(td, tb, f->tt_pv, bound, std::min(max_ply - 1, n.depth + 6), 0, tt_eval_none);
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
// Fase 2: valutazione e tagli prima del ciclo delle mosse.
// --------------------------------------------------------------------------
static void node_evaluate(ThreadData &td, NodeFrame *f, NodeState &n) {
  n.eval_raw = tt_eval_none;
  if (n.in_check) {
    f->static_eval = n.eval = f[-2].static_eval;
  } else if (n.excluded) {
    n.eval = f->static_eval;
    n.eval_raw = n.h.hit ? n.h.eval_raw : tt_eval_none;
  } else {
    const int plain = node_eval(td, n.h, n.eval_raw);
    f->static_eval = n.eval = corrected_eval(plain, n.signal + n.signal_x);
    if (n.h.hit) {
      if (val_known(n.h.value) && bound_covers(n.h.bound, n.h.value > n.eval))
        n.eval = n.h.value;
    } else
      hash_write(td, VAL_NONE, f->tt_pv, BOUND_NIL, HD_EVAL, 0, n.eval_raw);
  }
  n.improving = f->static_eval > f[-2].static_eval;
  n.opp_worse = f->static_eval > -f[-1].static_eval;
  if (n.prior_red >= 3 && !n.opp_worse)
    n.depth++;
  if (n.prior_red >= 2 && n.depth >= 2 && f->static_eval + f[-1].static_eval > 166)
    n.depth--;
}

template <bool AN>
static int prune_before_moves(ThreadData &td, NodeFrame *f, NodeState &n) {
  const int us = td.side;
  // La differenza di valutazione col ply precedente insegna qualcosa sulla mossa che ci ha portati qui.
  if (move_ok(f[-1].move) && !f[-1].in_check && !n.prior_capture) {
    const int diff = std::clamp(-(f[-1].static_eval + f->static_eval), -189, 194) + 60;
    const int pm = f[-1].move;
    stat_push(td.quiet_hist[us ^ 1][quiet_key(pm)], diff * EvalDiffMul, LIM_QUIET);
    const int pc_now = td.piece_on[n.prev_to];
    if (!n.h.hit && pc_now % 6 != P && !get_move_promoted(pm))
      stat_push(pawn_row(td)[pc_now][n.prev_to], diff * EvalDiffPawnMul, LIM_PAWN);
  }

  // Valutazione molto sotto alpha: basta la quiescenza.
  // gd_razor (AnGold) e' 0 fuori dalla modalita' analisi; n.crystal e' sempre falso.
  if (!n.pv && n.eval < n.alpha - RazorMul * n.depth * n.depth - (AN ? td.gd_razor : 0) && !(AN && n.crystal))
    return quiesce(td, f, n.alpha, n.beta, false);

  // Valutazione abbastanza sopra beta: il nodo regge senza cercare.
  if (!f->tt_pv && n.depth < (n.hunt_mate ? 6 : e_rfp_max_depth(td)) && n.eval >= n.beta && (!n.h.move || n.tact_hash) &&
      !val_lost(n.beta) && !val_won(n.eval) && !(AN && n.crystal)) {
    int mult = std::min(RfpMultBase + n.depth * RfpMultDepth, RfpMultCap) + (AN ? td.dz_rfp : deep_term(td, DeepRfp));
    if (AN && td.gd_rfp_pct != 100)   // AnGold: margine allargato (150% in attacco o con sacrifici, 120% in manovra)
      mult = mult * td.gd_rfp_pct / 100;
    mult -= RfpNoTTHit * !f->tt_hit;
    mult += by_expert(n.expert, RfpExp0, RfpExp1, RfpExp2, RfpExp3);
    const int imp = RfpImproving * n.improving;
    const int margin = mult * n.depth - (imp + RfpOppWorse * n.opp_worse) * mult / 1024 +
                       std::abs(n.signal) / RfpCorrDiv;
    if (n.eval - margin >= n.beta)
      return (RfpBetaWeight * n.beta + (1024 - RfpBetaWeight) * n.eval) / 1024;
  }

  // Mossa nulla: se anche passando la mano si resta sopra beta, il nodo taglia.
  const int non_pawn = td.occupancies[us] & ~td.bitboards[us == white ? P : p] & ~td.bitboards[us == white ? K : k]
                           ? 1 : 0;
  if (n.cut && f->static_eval >= n.beta - NmpDepthMul * n.depth - e_nmp_improving(td) * n.improving + NmpBase &&
      !n.excluded && non_pawn && td.ply >= td.nmp_min_ply && n.beta >= -2000 && !(AN && gd_skip_null(td, n))) {
    // DeepNmp in 1024-esimi di ply, arrotondato con 10 bit della chiave (fissi per posizione): a 0 il termine e' 0,
    // altrimenti una frazione |v|/1024 dei nodi riduce di un ply in piu' (o in meno) e lo SPSA ne vede la pendenza.
    const int r = NmpRedBase + n.depth / 3 + std::max((f->static_eval - n.beta) / NmpEvalDiv, 0) +
                  (((AN ? td.dz_nmp : deep_term(td, DeepNmp)) + (int)((td.hash_key >> 20) & 1023)) >> 10);
    NullUndo nu;
    step_in_null(td, f, nu);
    f[1].in_check = false;
    const int nv = -explore<AN>(td, f + 1, -n.beta, -n.beta + 1, n.depth - r, false, false);
    step_out_null(td, nu);
    if (nv >= n.beta && !val_won(nv)) {
      if (td.nmp_min_ply || n.depth < e_nmp_verify_depth(td))
        return nv;
      td.nmp_min_ply = td.ply + 3 * (n.depth - r) / 4;
      const int v = explore<AN>(td, f, n.beta - 1, n.beta, n.depth - r, false, false);
      td.nmp_min_ply = 0;
      if (v >= n.beta)
        return nv;
    }
  }

  n.improving |= f->static_eval >= n.beta;

  // Senza mossa hash, fuori dalla linea precedente, si scende di un ply.
  if (!f->follow_pv && !n.all && n.depth >= 6 && !n.h.move)
    n.depth--;

  // Tagli con le catture: una cattura che supera beta di un margine anche a profondita' ridotta.
  const int cut_beta = n.beta + ProbCutMargin - ProbCutImproving * n.improving;
  if (n.depth >= 3 && !val_decided(n.beta) && !(val_known(n.h.value) && n.h.value < cut_beta)) {
    MoveQueue q;
    queue_start(q, td, f, n.h.move, n.depth, QK_CUTS);
    q.cut_threshold = cut_beta - f->static_eval;
    MoveSafety s;
    s.ready = false;
    const int cut_depth = n.depth - (n.improving ? 5 : 3);
    int m;
    while ((m = queue_next(q)) != 0) {
      if (m == n.excluded || !legal_before(td, s, m))
        continue;
      const bool gives = checks_before(td, s, m);
      UndoInfo u;
      if (!step_in(td, f, m, u))
        continue;
      f[1].in_check = gives;
      int v = -quiesce(td, f + 1, -cut_beta, -cut_beta + 1, false);
      if (v >= cut_beta && cut_depth > 0)
        v = -explore<AN>(td, f + 1, -cut_beta, -cut_beta + 1, cut_depth, !n.cut, false);
      step_out(td, m, u);
      if (v >= cut_beta) {
        hash_write(td, v, f->tt_pv, BOUND_LOW, cut_depth + 1, m, n.eval_raw);
        if (!val_decided(v))
          return v - (cut_beta - n.beta);
      }
    }
  }
  return NO_VALUE;
}

// --------------------------------------------------------------------------
// Fase 3: il ciclo delle mosse.
// --------------------------------------------------------------------------

// Potatura di una mossa prima di giocarla. true = salta la mossa.
// `edge`: la mossa e' una cattura che cambia esperto della rete (PhaseEdge*), calcolato dal chiamante.
template <bool AN>
static bool skip_move(ThreadData &td, NodeFrame *f, NodeState &n, MoveQueue &q, int m, bool tact, bool gives,
                      int r, bool edge) {
  const int us = td.side;
  if (n.root || val_lost(n.best) || (AN && n.crystal))   // n.crystal: solo AnCrystal
    return false;
  const U64 own_pieces = td.occupancies[us] & ~td.bitboards[us == white ? P : p] & ~td.bitboards[us == white ? K : k];
  if (!own_pieces)
    return false;
  // (3 + d^2) / (2 - improving) diviso 1 o 2: uno shift, non una divisione hardware (04/10/2026, stesso valore).
  if (f->move_count >= (3 + n.depth * n.depth) >> (1 - (int)n.improving))
    q.quiets_off = true;
  int red_depth = n.depth - 1 - r / 1024;
  const int pc = move_piece(m), to = move_to(m);

  if (tact || gives) {
    const int vk = victim_kind(td, m);
    const int ch = td.capt_hist[pc][to][vk];
    if (!gives && red_depth < 8 && !(PhaseEdgeNoFut && edge)) {
      const int fut = f->static_eval + CaptFutBase + CaptFutDepth * red_depth + unit_value[vk] + CaptFutHist * ch / 1024;
      if (fut <= n.alpha)
        return true;
    }
    const int margin = CaptSeeDepth * n.depth + ch * CaptSeeHist / 1024;
    // Il sacrificio dell'ultimo pezzo con alpha sotto la patta non si pota (puo' portare allo stallo).
    const int own_count = count_bits(own_pieces);
    const bool last_piece = own_count == 1 && (own_pieces >> move_from(m) & 1);
    if ((n.alpha >= contempt(td) || !last_piece) && !see_holds_q(td, q, m, -margin))
      return true;
    return false;
  }

  if (f->follow_pv && n.pv)
    return false;
  const int d_index = std::min(n.depth, 16) - 1;
  // hist / hist_div[d] con divisore variabile era una divisione hardware a ogni quieta (04/10/2026). Ora moltiplica
  // per il reciproco M = ceil(2^40 / D) e tronca verso zero: per D <= 3637 e' esatto fino a |hist| < 2^27 (errore
  // M*D - 2^40 < D), e |hist| qui sta sotto 2^19 (somme di statistiche limitate). Stesso valore, stesso albero.
  static constexpr int hist_div[16] = {3637, 2787, 2761, 2939, 3171, 3347, 3147, 2762,
                                       2772, 3106, 3107, 3060, 3112, 2991, 3090, 3542};
  static constexpr auto hist_recip = [] {
    std::array<unsigned long long, 16> r{};
    for (int i = 0; i < 16; i++)
      r[i] = ((1ULL << 40) + (unsigned long long)hist_div[i] - 1) / (unsigned long long)hist_div[i];
    return r;
  }();
  int hist = (*f[-1].cont_hist)[pc][to] + (*f[-2].cont_hist)[pc][to] + pawn_row(td)[pc][to];
  if (hist < -QuietHistPrune * n.depth)
    return true;
  hist += QuietMainHistMul * td.quiet_hist[us][quiet_key(m)] / 32;
  assert(std::abs(hist) < (1 << 27));
  {
    const unsigned long long mag = (unsigned long long)(hist < 0 ? -hist : hist);
    int q = (int)((mag * hist_recip[d_index]) >> 40);
    if (DeepHist)
      q = q * (1024 + deep_term(td, DeepHist)) / 1024;
    red_depth += hist < 0 ? -q : q;
  }
  const int fut = f->static_eval + QuietFutDepth * red_depth + e_quiet_fut_alpha(td) * (f->static_eval > n.alpha) + QuietFutBase +
                  (AN ? td.dz_fut : deep_term(td, DeepFut)) + by_expert(n.expert, FutExp0, FutExp1, FutExp2, FutExp3);
  if (!n.in_check && red_depth < 12 && fut <= n.alpha) {
    if (n.best <= fut && !val_decided(n.best) && !val_won(fut))
      n.best = fut;
    return true;
  }
  red_depth = std::max(red_depth, 0);
  return !see_holds_q(td, q, m, -QuietSeeMul * red_depth * red_depth);
}

// Spinta di un pedone passato in 6a o 7a traversa in un finale (nostra). La rete vede i pedoni passati con il suo
// blocco PassedPawns; la ricerca li tratterebbe come quiete qualsiasi e potrebbe ridurli di parecchi ply.
static bool passed_push(const ThreadData &td, int m) {
  if (kPieceKind[move_piece(m)] != P || get_move_capture(m) || get_move_promoted(m))
    return false;
  const int to = move_to(m), us = td.side, row = to / 8, file = to % 8;   // row 0 = ottava traversa
  if (us == white ? (row != 1 && row != 2) : (row != 5 && row != 6))
    return false;
  int npm = 0;
  for (int t = N; t <= Q; t++)
    npm += unit_value[t] * (count_bits(td.bitboards[t]) + count_bits(td.bitboards[t + 6]));
  if (npm > PassedPushNpm)
    return false;
  U64 files = 0;
  for (int fl = std::max(0, file - 1); fl <= std::min(7, file + 1); fl++)
    files |= 0x0101010101010101ULL << fl;
  const int row_start = to - file;
  const U64 ahead = us == white ? (1ULL << row_start) - 1 : ~((1ULL << (row_start + 8)) - 1);
  return !(files & ahead & td.bitboards[us == white ? p : P]);
}

// La mossa e' uno spostamento avanti e indietro senza progresso (stessa pedina fra le stesse case)?
static inline bool shuffles(const ThreadData &td, const NodeFrame *f, int m) {
  if (is_tactical(m) || td.fifty < 10 || td.plies_from_null < 6 || f->ply < 20)
    return false;
  const int m2 = f[-2].move, m4 = f[-4].move;
  return move_ok(m2) && move_ok(m4) && move_from(m) == move_to(m2) && move_from(m2) == move_to(m4);
}

// Estensione singolare della mossa hash. Ritorna l'estensione; un valore >= beta chiude il nodo (in *cutoff).
// SING_SKIPPED: la ricerca di esclusione non si e' fatta (mossa non hash o condizioni mancanti); move_loop la legge
// come estensione 0, e solo in quel caso prova la LDSE (Ldse*).
static constexpr int SING_SKIPPED = -99;
template <bool AN>
static int singular_probe(ThreadData &td, NodeFrame *f, NodeState &n, int m, int new_depth, int *cutoff) {
  const HashView &h = n.h;
  if (n.root || m != h.move || n.excluded || n.depth < 6 + f->tt_pv || !val_known(h.value) || val_decided(h.value) ||
      !bound_covers(h.bound, true) || h.bound == BOUND_UP || h.depth < n.depth - 3 || shuffles(td, f, m) || n.hunt_mate)
    return SING_SKIPPED;
  const int s_beta = h.value - (SingMarginBase + (AN ? td.dz_sing : deep_term(td, DeepSing)) + SingMarginTtPv * (f->tt_pv && !n.pv)) * n.depth / 63;
  const int s_depth = new_depth / 2;
  f->excluded = m;
  const int v = explore<AN>(td, f, s_beta - 1, s_beta, s_depth, n.cut, false);
  f->excluded = 0;
  if (v < s_beta) {
    const int corr_adj = std::abs(n.signal) / SingCorrDiv;
    const int dbl = SingDoubleBase + SingDoublePv * n.pv - SingDoubleQuiet * !n.tact_hash - corr_adj -
                    1175 * td.tt_move_hist / 114178 - (f->ply > td.root_depth) * 38 - (AN ? td.dz_ext : deep_term(td, DeepExt));
    const int tpl = SingTripleBase + SingTriplePv * n.pv - SingTripleQuiet * !n.tact_hash + SingTripleTtPv * f->tt_pv -
                    corr_adj - (f->ply > td.root_depth) * 43 - (AN ? td.dz_ext : deep_term(td, DeepExt));
    n.depth++;
    return 1 + (v < s_beta - dbl) + (v < s_beta - tpl);
  }
  if (v >= n.beta && !val_decided(v)) {
    stat_push(td.tt_move_hist, -421 - 110 * n.depth, LIM_TTMOVE);
    if (!n.in_check && v > f->static_eval) {
      const int bonus = std::clamp((v - f->static_eval) * s_depth * CorrSingMul / 1024, -LIM_CORR / 4, LIM_CORR / 4);
      corr_learn(td, f, bonus);
    }
    *cutoff = v;
    return 0;
  }
  if (h.value >= n.beta || n.cut)
    return -3;
  return 0;
}

// Aggiorna la mossa di radice appena cercata (statistiche, linea, cambi di mossa migliore).
static void root_record(ThreadData &td, NodeState &n, int m, int value, U64 spent, int move_no) {
  RootLine *rl = nullptr;
  for (int i = 0; i < td.root_count; i++)
    if (td.root[i].pv[0] == m) { rl = &td.root[i]; break; }
  if (!rl)
    return;
  rl->effort += spent;
  const U64 prev = std::max<U64>(1, rl->effort - spent);
  const U64 w = std::clamp<U64>((32 * spent * 2) / (spent * 2 + 3 * prev), 12, 24);
  const U64 w_sq = std::min<U64>(w, 16);
  const long long v2 = (long long)value * std::abs(value);
  rl->avg_score = rl->avg_score == -infinity ? value : (int)((value * (long long)w + rl->avg_score * (long long)(32 - w)) / 32);
  rl->mean_sq = rl->mean_sq == -(long long)infinity * infinity ? v2
              : (v2 * (long long)w_sq + rl->mean_sq * (long long)(32 - w_sq)) / 32;
  if (move_no == 1 || value > n.alpha) {
    rl->score = rl->uci_score = value;
    rl->sel_depth = td.sel_depth;
    rl->lower = rl->upper = false;
    if (value >= n.beta) { rl->lower = true; rl->uci_score = n.beta; }
    else if (value <= n.alpha) { rl->upper = true; rl->uci_score = n.alpha; }
    rl->pv_len = 1;
    for (int i = 1; i < td.pv_length[1] && rl->pv_len < max_ply; i++)
      rl->pv[rl->pv_len++] = td.pv_table[1][i];
    if (move_no > 1 && !td.pv_idx)
      td.best_move_changes++;
  } else
    rl->score = -infinity;
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
  int red_window = -1, red_window_term = 0;   // vedi base_reduction
  // Idea 2: termine di riduzione fisso per il nodo (0 se spento).
  const int node_red = AN ? td.dz_lmr : deep_term(td, DeepLmr);
  const int gd_style = AN ? td.gd_style : 0;   // AnGold: stile della radice (0 in partita)

  while ((m = queue_next(q)) != 0) {
    if (m == n.excluded || !legal_before(td, s, m))
      continue;
    if (n.root) {
      bool listed = false;
      for (int i = td.pv_idx; i < td.pv_last; i++)
        if (td.root[i].pv[0] == m) { listed = true; break; }
      if (!listed)
        continue;
    }
    f->move_count = ++n.moves;
    const bool tact = is_tactical(m);
    const int pc = move_piece(m), to = move_to(m);
    const bool gives = checks_before(td, s, m);
    int new_depth = n.depth - 1;
    if (n.beta - n.alpha != red_window) {
      red_window = n.beta - n.alpha;
      red_window_term = red_window * LmrDeltaMul / td.root_delta;
    }
    int r = base_reduction(td, n.improving, n.depth, n.moves, red_window_term);
    if (gd_style)   // AnGold: riduzione di base al 97% in attacco, al 103% in manovra
      r = gd_style == 1 ? r * 97 / 100 : gd_style == 2 ? r * 103 / 100 : r;
    if (f->tt_pv)
      r += LmrTtPvAdd;
    r += by_expert(n.expert, LmrExp0, LmrExp1, LmrExp2, LmrExp3);

    const bool passer = PassedPushRed && !tact && passed_push(td, m);
    // Idea 4: cattura che cambia esperto della rete (si calcola prima della mossa).
    const bool edge = PhaseEdgeNoFut && tact && capture_crosses(td, m);
    if (!passer && skip_move<AN>(td, f, n, q, m, tact, gives, r, edge))
      continue;

    int cut_value = NO_VALUE;
    int ext = singular_probe<AN>(td, f, n, m, new_depth, &cut_value);
    if (cut_value != NO_VALUE)
      return cut_value;
    const bool sing_skipped = ext == SING_SKIPPED;
    if (sing_skipped)
      ext = 0;
    // LDSE (nostra, 05/10/2026; l'idea e' in Reckless e Stormphrax): a profondita' bassa, dove la singolare non si
    // prova, la mossa hash con limite inferiore o esatto si estende di un ply se la valutazione statica sta sotto
    // alpha di LdseMargin. La hash dice che quella mossa ha tenuto un valore, l'eval dice che la posizione e' brutta:
    // il nodo si regge su di lei, e vicino all'orizzonte un ply in piu' decide se quel valore era vero.
    // LdseMode (bit): 1 = solo nei nodi cut (forma di Reckless), 2 = solo limite inferiore, non esatto (forma di
    // Stormphrax), 4 = non con entry della sola quiescenza.
    // LdseMax (05/10/2026): con Ldse accesa ogni nodo cut lungo una linea puo' estendere di nuovo, e la profondita'
    // scende di un ply ogni due invece di uno per ply. A profondita' fissa l'albero resta lo stesso nella posizione
    // tipica (media geometrica x1,02-x1,06 su 80 posizioni, prof. 8-14) ma alcuni finali di torre crescono x5-x16. Il
    // tetto conta le estensioni Ldse sulla linea (NodeFrame::ldse_path); 0 = nessun tetto, come prima.
    const bool ldse = LdseMargin && ext == 0 && sing_skipped && m == n.h.move && !n.root && !n.excluded &&
                      !n.in_check && n.depth <= LdseDepth && val_known(n.h.value) && bound_covers(n.h.bound, true) &&
                      f->static_eval <= n.alpha - LdseMargin && (n.cut || !(LdseMode & 1)) &&
                      (n.h.bound == BOUND_LOW || !(LdseMode & 2)) && (n.h.depth >= 0 || !(LdseMode & 4)) &&
                      (!LdseMax || f->ldse_path < LdseMax);
    if (ldse)
      ext = 1;
    f->ldse = ldse;

    const U64 nodes_before = td.nodes;
    UndoInfo u;
    if (!step_in(td, f, m, u)) {
      f->move_count = --n.moves;
      continue;
    }
    f[1].in_check = gives;
    if (CausalRed)
      f[1].refute = 0;
    new_depth += ext;

    if (f->tt_pv)
      r -= LmrTtPvSub + deep_term(td, DeepTtPv) + n.pv * LmrTtPvPv + (n.h.value > n.alpha) * LmrTtPvAlpha +
           (n.h.depth >= n.depth) * (LmrTtPvDepth + n.cut * LmrTtPvCut);
    r += e_lmr_offset(td);
    r -= n.moves * LmrMoveCount;
    r -= std::abs(n.signal) / LmrCorrDiv;
    if (n.cut)
      r += LmrCutNode + LmrCutNoTT * !n.h.move;
    if (n.tact_hash && !(PhaseEdgeNoFut && edge))
      r += LmrTTCapture;
    r += node_red;
    if (f[1].cutoff_cnt > 1)
      r += LmrCutoffBase + LmrCutoffMany * (f[1].cutoff_cnt > 2) + LmrCutoffAll * n.all;
    else if (m == n.h.move)
      r -= LmrTTMove;
    if (tact) {
      // Pezzo davvero catturato (per l'en passant e' il pedone, non la casa d'arrivo vuota).
      const int took = kPieceKind[u.captured_piece & 15];
      f->stat_score = 873 * unit_value[took] / 128 + td.capt_hist[pc][to][took];
    }
    else
      f->stat_score = (2252 * td.quiet_hist[us][quiet_key(m)] + 1126 * (*f[-1].cont_hist)[pc][to] +
                       1093 * (*f[-2].cont_hist)[pc][to]) / 1024;
    r -= f->stat_score * LmrStatDiv / 4096;
    if (!tact && !val_decided(n.alpha))
      r += 3 * std::clamp(n.alpha - n.eval, -64, 96);
    if (n.all) {
      // r * LmrAllNode / (256*depth + 268), troncato verso zero come la divisione intera: moltiplicazione per il
      // reciproco a 40 bit della tabella (esatta finche' |r*LmrAllNode| < 2^24: il divisore arriva a 35.000, quindi
      // l'errore M*D - 2^40 < D non puo' far scattare l'unita'); oltre, mai visto, si divide davvero
      // (04/10/2026 sera: era una divisione hardware per ogni mossa dei nodi all).
      const long long x = (long long)r * e_lmr_all_node(td);
      const unsigned long long mag = (unsigned long long)(x < 0 ? -x : x);
      const int q = mag < (1ULL << 24) ? (int)((mag * all_node_recip[n.depth]) >> 40)
                                       : (int)(mag / (unsigned long long)(256 * n.depth + 268));
      r += x < 0 ? -q : q;
    }
    if (passer)
      r -= PassedPushRed;
    // CausalRed (0 = spenta): la quieta non tocca le case della confutazione di una sorella fallita bassa.
    if (CausalRed && n.causal && !tact && !gives && !passer && m != n.h.move &&
        !(((1ULL << move_from(m)) | (1ULL << to)) & n.causal))
      r += CausalRed;
    // AnGold, ritocchi delle riduzioni lontano dalla radice (unita' 1/1024 di ply): in attacco catture e scacchi
    // -60, in difesa le quiete -40, in manovra le quiete senza scacco +80 da profondita' 9.
    if (gd_style && n.depth > 5 && f->ply > 1) {
      if (gd_style == 1 && (tact || gives))
        r -= 60;
      else if (gd_style == 3 && !tact)
        r -= 40;
      else if (gd_style == 2 && !tact && !gives && n.depth > 8)
        r += 80;
    }

    if (n.depth >= 2 && n.moves > 1) {
      // NegExtMax (08/10/2026): con riduzione negativa la mossa si cerca fino a 2 ply PIU' a fondo, e lungo una linea
      // queste estensioni si sommano come quelle di Ldse prima della sua guardia (LdseMax). Stesso rimedio: un tetto al
      // numero di estensioni da riduzione negativa sulla linea (NodeFrame::negext_path). 0 = nessun tetto, come prima.
      const int neg_cap = (!NegExtMax || f->negext_path < NegExtMax) ? 2 : 0;
      const int d = std::max(1, std::min(new_depth - r / 1024, new_depth + neg_cap)) + n.pv;
      if (NegExtMax)
        f->negext = d - n.pv > new_depth;
      f->reduction = new_depth - d;
      value = -explore<AN>(td, f + 1, -(n.alpha + 1), -n.alpha, d, true, false);
      f->reduction = 0;
      if (NegExtMax)
        f->negext = 0;
      if (value > n.alpha) {
        new_depth += (d < new_depth && value > n.best + DeeperMargin) - (value < n.best + ShallowerMargin);
        if (new_depth > d)
          value = -explore<AN>(td, f + 1, -(n.alpha + 1), -n.alpha, new_depth, !n.cut, false);
        learn_followups(f, pc, to, 1334);
      }
    } else if (!n.pv || n.moves > 1) {
      if (!n.h.move)
        r += e_lmr_no_tt_full(td);
      value = -explore<AN>(td, f + 1, -(n.alpha + 1), -n.alpha,
                       new_depth - (r > 5234) - (r > 5487 && new_depth > 2), !n.cut, false);
    }
    if (n.pv && (n.moves == 1 || value > n.alpha)) {
      if (m == n.h.move && ((val_known(n.h.value) && val_decided(n.h.value) && n.h.depth > 0) || n.h.depth > 1))
        new_depth = std::max(new_depth, 1);
      value = -explore<AN>(td, f + 1, -n.beta, -n.alpha, new_depth, false, true);
    }
    step_out(td, m, u);

    if (stop_threads.load(std::memory_order_relaxed))
      return 0;
    if (CausalRed && !tact && value <= n.alpha && f[1].refute)
      n.causal = causal_mask(td, f[1].refute);
    if (n.root)
      root_record(td, n, m, value, td.nodes - nodes_before, n.moves);

    const int nudge = (value == n.best && td.ply + 2 >= td.root_depth && (int(td.nodes) & 14) == 0 &&
                       !val_won(std::abs(value) + 1)) ? 1 : 0;
    if (value + nudge > n.best) {
      n.best = value;
      if (value + nudge > n.alpha) {
        n.best_move = m;
        if (n.pv)
          pv_extend(td, m);
        if (value >= n.beta) {
          f->cutoff_cnt += (ext < 2) || n.pv;
          if (CausalRed)
            f->refute = m;   // per il padre: la mossa che confuta la sua
          break;
        }
        if (n.depth > 3 && n.depth < 12 && !val_decided(value))
          n.depth -= 3;
        n.alpha = value;
      }
    }
    if (m != n.best_move && n.moves <= TRACKED) {
      if (tact) n.tacts[n.n_tacts++] = m;
      else      n.quiets[n.n_quiets++] = m;
    }
  }
  return NO_VALUE;
}

// --------------------------------------------------------------------------
// Fase 4: fine del nodo.
// --------------------------------------------------------------------------
static int node_close(ThreadData &td, NodeFrame *f, NodeState &n) {
  const int us = td.side;
  // La linea del bucket hash di questo nodo, letta all'ingresso, dopo il sottoalbero e' spesso uscita dalla cache:
  // la si richiama adesso, con le statistiche da aggiornare a coprire l'attesa, prima della hash_write in fondo.
  TT_PREFETCH(&hash_table[tt_base_index(tt_key(td))]);
  if (n.best >= n.beta && !val_decided(n.best) && !val_decided(n.alpha)) {
    // W1 (08/10/2026): divisione per depth + 1 col reciproco a 40 bit; |x| < 2^15 * (max_ply + 8) < 2^24, esatto
    // (numpy: 0 differenze per |x| <= 32768 * (d + 1), d < 260).
    const long long x = (long long)n.best * n.depth + n.beta;
    const unsigned long long mag = (unsigned long long)(x < 0 ? -x : x);
    const int q = (int)((mag * succ_recip[n.depth]) >> 40);
    n.best = x < 0 ? -q : q;
  }

  if (!n.moves)
    n.best = n.excluded ? n.alpha : n.in_check ? mated_at(td.ply) : contempt(td);   // stallo: patta (Contempt)
  else if (n.best_move) {
    reward_best(td, f, n);
    if (!n.pv)
      stat_push(td.tt_move_hist, n.best_move == n.h.move ? 918 : -747, LIM_TTMOVE);
  } else if (!n.prior_capture && n.prev_to >= 0) {
    // Nessuna mossa ha superato alpha: la mossa avversaria che ci ha portati qui era buona.
    int scale = -241 - f[-1].stat_score / 98 + std::min(59 * n.depth, 420) + 186 * (f[-1].move_count > 9) +
                142 * (!n.in_check && n.best <= f->static_eval - 106) +
                159 * (!f[-1].in_check && n.best <= -f[-1].static_eval - 68);
    scale = std::max(scale, 0);
    const int bonus = std::min(150 * n.depth - 85, 1337) * scale;
    const int prev_pc = td.piece_on[n.prev_to];
    learn_followups(f - 1, prev_pc, n.prev_to, bonus * 263 / 16384);
    stat_push(td.quiet_hist[us ^ 1][quiet_key(f[-1].move)], bonus * 215 / 32768, LIM_QUIET);
    if (prev_pc % 6 != P && !get_move_promoted(f[-1].move))
      stat_push(pawn_row(td)[prev_pc][n.prev_to], bonus * 324 / 8192, LIM_PAWN);
  } else if (n.prior_capture && n.prev_to >= 0) {
    stat_push(td.capt_hist[td.piece_on[n.prev_to]][n.prev_to][f[-1].captured % 6], 892, LIM_CAPT);
  }

  if (n.pv)
    n.best = std::min(n.best, n.cap_value);
  if (n.best <= n.alpha)
    f->tt_pv = f->tt_pv || f[-1].tt_pv;

  if (!n.excluded && !(n.root && td.pv_idx))
    hash_write(td, n.best, f->tt_pv,
               n.best >= n.beta ? BOUND_LOW : (n.pv && n.best_move) ? BOUND_EXACT : BOUND_UP,
               n.moves ? n.depth : std::min(max_ply - 1, n.depth + 6), n.best_move, n.eval_raw);

  // CorrLearnMode (06/10/2026, rapporto N_CORREZIONI 4.5): a 0 la condizione di sempre.
  const bool learn_ok = !((CorrLearnMode & 1) && n.excluded && !n.moves) && !((CorrLearnMode & 2) && n.excluded) &&
                        !((CorrLearnMode & 4) && val_decided(n.best));
  const bool dir_ok = (n.best > f->static_eval) == (n.best_move != 0) ||
                      ((CorrLearnMode & 8) && n.pv && n.best_move && n.best < n.beta);
#ifdef TRIUMV_CORRSTATS
  // Residuo prima e dopo la correzione (cstats.h), PRIMA del filtro di direzione (che sceglierebbe il segno):
  // nodi fuori scacco, non esclusi, senza cattura migliore, con un'eval della rete nota.
  if (!n.in_check && !n.excluded && !(n.best_move && get_move_capture(n.best_move)) && n.eval_raw != tt_eval_none &&
      !val_decided(n.best)) {
    const int plain = eval_refade(n.eval_raw, td.fifty);
    const int fb = td.fifty < 20 ? 0 : td.fifty < 60 ? 1 : 2;
    const int kind = n.best_move ? (n.best >= n.beta ? 0 : 1) : 2;   // fail-high, esatto, fail-low
    CSTAT(net_phase(count_bits(td.occupancies[both])), fb, kind, n.best - plain, n.best - f->static_eval);
  }
#endif
  if (!n.in_check && !(n.best_move && get_move_capture(n.best_move)) && dir_ok && learn_ok) {
    const int bonus = std::clamp((n.best - f->static_eval) * n.depth * (n.best_move ? CorrBonusHi : CorrBonusLo) / 128,
                                 -CorrBonusMaxLo, CorrBonusMaxHi);
    corr_learn(td, f, 1061 * bonus / 1024);
  }
  return n.best;
}

// --------------------------------------------------------------------------
template <bool AN>
static int explore(ThreadData &td, NodeFrame *f, int alpha, int beta, int depth, bool cut, bool pv) {
  if (depth <= 0)
    return (AN && (td.an_bits & AN_QSCHK)) ? quiesce<true>(td, f, alpha, beta, pv) : quiesce<false>(td, f, alpha, beta, pv);
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
  n.hunt_mate = td.root_depth >= (AN ? td.hunt_from : 16) && std::abs(td.root[td.pv_idx].score) >= 2000;
  n.best = -infinity;
  n.best_move = 0;
  n.moves = 0;
  n.cap_value = infinity;
  n.n_quiets = n.n_tacts = 0;
  n.causal = 0;
  // LdseMax: estensioni Ldse sulla linea fino a qui. f->ldse torna a 0 finche' move_loop non estende una mossa, cosi'
  // i figli della mossa nulla e del probcut non ne ereditano una.
  f->ldse = 0;
  f->ldse_path = f[-1].ldse_path + f[-1].ldse;
  if (NegExtMax) {   // NegExtMax: stesso conteggio per le estensioni da riduzione negativa (move_loop)
    f->negext = 0;
    f->negext_path = f[-1].negext_path + f[-1].negext;
  }

  int v = node_open(td, f, n);
  if (v != NO_VALUE) return v;
  n.crystal = AN && crystal_node(td, f);   // dopo node_open, che decide follow_pv
  node_evaluate(td, f, n);
  if ((v = hash_cutoff(td, f, n)) != NO_VALUE) return v;
  if ((v = tablebase_probe(td, f, n)) != NO_VALUE) return v;
  if (!n.in_check) {
    if ((v = prune_before_moves<AN>(td, f, n)) != NO_VALUE) return v;
  }

  // Limite inferiore dalla hash ben sopra beta: basta quello.
  const int hash_beta = n.beta + ProbCutTTMargin;
  if (bound_covers(n.h.bound, true) && n.h.depth >= n.depth - 4 && n.h.value >= hash_beta && !val_decided(n.beta) &&
      val_known(n.h.value) && !val_decided(n.h.value))
    return hash_beta;

  if ((v = move_loop<AN>(td, f, n)) != NO_VALUE)
    return v;
  return node_close(td, f, n);
}

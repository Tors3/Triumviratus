// search/11_queue.inc -- Ordinamento delle mosse.
// Parte di threads.cpp: NON e' un'unita' di compilazione a se'.

// SeePinned (05/10/2026, audit del nucleo): inchiodati e inchiodatori dei due colori, una volta per posizione, per la
// SEE (see.cpp). Per ogni re: i pezzi lunghi avversari sulle sue linee a scacchiera vuota sono i cecchini; un
// cecchino con un solo pezzo in mezzo, e quel pezzo nostro, lo inchioda. SF fa lo stesso conto in ogni posizione.
static void see_pins_update(ThreadData &td) {
  for (int c = white; c <= black; c++) {
    const int off = c == white ? 0 : 6, eoff = 6 - off;
    const int ksq = get_ls1b_index(td.bitboards[K + off]);
    U64 snipers = (empty_rook_att[ksq] & (td.bitboards[R + eoff] | td.bitboards[Q + eoff])) |
                  (empty_bishop_att[ksq] & (td.bitboards[B + eoff] | td.bitboards[Q + eoff]));
    const U64 occ = td.occupancies[both] ^ snipers;
    U64 pinned = 0, pinners = 0;
    while (snipers) {
      const int sn = get_ls1b_index(snipers);
      const U64 b = between_tbl[ksq][sn] & occ;
      if (b && !(b & (b - 1)) && (b & td.occupancies[c])) {
        pinned |= b;
        pinners |= 1ULL << sn;
      }
      pop_lsb_bb(snipers);
    }
    td.see_pinned[c] = pinned;
    td.see_pinners[c ^ 1] = pinners;
  }
  td.see_pin_key = td.hash_key;
}

// SEE con la convenzione della ricerca: promozioni, arrocco ed en passant valgono 0.
static inline bool see_holds(ThreadData &td, int m, int threshold) {
  if (get_move_promoted(m) || get_move_castling(m) || get_move_enpassant(m))
    return 0 >= threshold;
  PROF_GUARD(prof_see);
  if (SeePinned && td.see_pin_key != td.hash_key)
    see_pins_update(td);
  return td_see_ge(td, m, threshold) != 0;
}

// ============================================================================
// Una coda per nodo. Le mosse escono a gruppi: la mossa hash, le tattiche che reggono lo scambio, le quiete con
// buone statistiche, le tattiche perdenti, le quiete rimaste. Sotto scacco si ordinano tutte le evasioni insieme;
// in quiescenza e per i tagli (probcut) si vedono solo le tattiche.
// Le mosse sono pseudo-legali: la legalita' la decide td_make_move.
// ============================================================================
enum QueueKind { QK_MAIN, QK_EVADE, QK_QUIESCE, QK_CUTS };

enum QueueStep {
  QS_HASH,
  QS_TACT_FILL, QS_TACT_WIN, QS_QUIET_FILL, QS_QUIET_HIGH, QS_TACT_LOSE, QS_QUIET_LOW,
  QS_EVADE_FILL,
  QS_ONLY_TACT_FILL,
  QS_DRAIN,
  QS_DONE
};

struct ScoredMove {
  int move;
  int score;
};

struct MoveQueue {
  ThreadData   *td;
  NodeFrame    *f;
  int           kind;
  int           step;
  int           hash_move;
  int           depth;
  int           cut_threshold;   // QK_CUTS: soglia di SEE
  bool          quiets_off;      // potatura per numero di mosse: niente piu' quiete
  LmpChkCtx    *checks;          // case di scacco del nodo, condivise con la legalita' (null = calcolate qui)
  int           n_tact, n_all;   // tattiche in [0, n_tact), quiete in [n_tact, n_all)
  int           cur;
  int           n_lose, cur_lose;
  ScoredMove    list[256];
  unsigned char losers[256];
  // Attacchi dell'avversario nella posizione del nodo, calcolati da score_quiets (06/10/2026, SEE delle quiete):
  // opp_att = tutte le case che attacca, opp_slider = quelle viste dai suoi pezzi lunghi. Valide se opp_ready.
  bool          opp_ready;
  U64           opp_att, opp_slider;
  // V2 (08/10/2026): confine fra quiete buone e cattive gia' noto dopo l'ordinamento (quiet_split >= 0), oppure -1
  // (si scorre la lista confrontando ogni punteggio, come prima).
  int           quiet_split;
};

// SEE di una mossa quieta decisa senza scambio (06/10/2026, velocita', albero identico). Per una quieta semplice
// (casa d'arrivo vuota, niente promozione, arrocco, en passant) td_see_ge esce con "soglia > 0 -> no" o, per
// soglia <= 0, con "si'" appena l'avversario non ha attaccanti sulla casa d'arrivo. Gli attaccanti li conta con
// l'occupazione SENZA la casa di partenza: togliere quella casa apre linee solo ai pezzi lunghi che gia' la vedono.
// Quindi, se la casa d'arrivo non e' attaccata dall'avversario e nessun suo pezzo lungo vede la casa di partenza,
// il risultato e' gia' noto: 0 >= soglia. La mappa la prepara score_quiets per le quiete della coda; la mossa hash
// (che esce prima della mappa) e le parate dallo scacco passano dalla SEE di sempre.
// Contatori (bench 14 su fens30): ~0,57 SEE di quiete per nodo.
static inline bool see_holds_q(ThreadData &td, const MoveQueue &q, int m, int threshold) {
  constexpr int SPECIAL = 0x100000 | 0xf0000 | 0x400000 | 0x800000;   // cattura, promozione, e.p., arrocco
  if (q.opp_ready && !(m & SPECIAL) && !((q.opp_att >> move_to(m)) & 1) && !((q.opp_slider >> move_from(m)) & 1)) {
#ifdef TRIUMV_VERIFY_SEEQ   // verifica: la SEE vera deve dare lo stesso esito
    if ((0 >= threshold) != see_holds(td, m, threshold)) {
      printf("info string SEEQ SBAGLIATA: mossa %d soglia %d\n", m, threshold);
      fflush(stdout);
      abort();
    }
#endif
    return 0 >= threshold;
  }
  return see_holds(td, m, threshold);
}

// Inserimento stabile: le mosse con punteggio >= limit vanno in testa in ordine decrescente, le altre restano
// dopo, nell'ordine di generazione.
// 04/10/2026 (xperf): qui nasce l'11% dei salti mal predetti del motore (~6 per nodo; l'ordinamento intero ne fa 10,
// SF 2,7). PROVATA e TOLTA la variante a scambio (la mossa buona prende il posto della prima bassa): salti mal
// predetti invariati (55,4 -> 55,1 per nodo), cicli per nodo +3,8%, ordine delle mosse basse rimescolato. I salti
// vengono dai confronti, che dipendono dai dati, non dagli spostamenti: serve un ordinamento senza salti.
// 04/10/2026 — SENZA SALTI, stesso ordine di prima (albero identico). Ogni mossa sopra la soglia ha una chiave unica
// (punteggio << 8 | 255 - posizione): a parita' di punteggio vince la prima generata, come nell'inserimento stabile.
// La sua posizione finale e' il numero di chiavi piu' grandi della sua, contato con un ciclo che il compilatore
// vettorizza; le mosse sotto la soglia seguono nell'ordine di generazione. Le divisioni fra le due liste si fanno con
// incrementi condizionati, non con salti. n <= 256, quindi la posizione sta in 8 bit.
// Misura (xperf, PGO, nodi identici): con chiavi a 64 bit salti mal predetti -10,8% per nodo ma istruzioni +1,2% e
// cicli invariati. Chiavi a 32 bit (Wide = false): 16 confronti per istruzione AVX-512 invece di 8. Valgono per
// tattiche e quiete, i cui punteggi stanno ben dentro +-2^22 (la saturazione e' solo una sicurezza). Dal 05/10/2026
// anche le parate dallo scacco (fill_evasions): prima usavano 1 << 28 per le catture e chiavi a 64 bit.
// 04/10/2026 notte — SENZA COPIE: le mosse arrivano dalla lista del generatore (mv) con i punteggi accanto (sc) e
// vengono scritte una volta sola, gia' in ordine, nella lista della coda (out). Prima passavano da tre liste
// (generatore -> coda -> copia temporanea dell'ordinamento). Stesso ordine, stesso albero.
#if defined(__AVX512F__)
#include <immintrin.h>
#endif
// 05/10/2026 — CONTEGGIO A MASCHERE (AVX-512). Il ciclo `r += key[j] > x` ha un conteggio nh che cambia a ogni
// chiamata: il compilatore lo vettorizza ma lascia una coda scalare di nh % 16 giri (ognuno confronto, somma e salto)
// e una riduzione orizzontale per ogni mossa. Qui le chiavi oltre nh valgono INT_MIN (sotto ogni chiave vera, che e'
// almeno -(1 << 30)), cosi' un gruppo parziale si confronta intero: per ogni mossa 16 confronti alla volta ->
// maschera -> popcount, niente coda. La chiave si scrive gia' nel primo passaggio, nello slot corrente della lista
// alta, e conta solo se la mossa ci entra: il secondo ciclo su hi[] e' sparito. Stesse chiavi, stesse posizioni.
// xperf 6 giri, build PGO avx512: cicli per nodo -0,70%. Le build avx2 tengono il ciclo scalare.
// Restituisce quante mosse sono finite nella parte ordinata (punteggio >= limit).
template <bool Wide>
static int rank_into(ScoredMove *out, const int *mv, const int *sc, int n, int limit) {
  PROF_GUARD(prof_mp);
  using Key = std::conditional_t<Wide, long long, int>;
  alignas(64) Key key[256 + 16];   // 16 slot in piu' per il riempimento INT_MIN del percorso vettoriale
  unsigned char hi[256], lo[256];
  int nh = 0, nl = 0;
  for (int i = 0; i < n; i++) {
    const int s  = sc[i];
    const int up = s >= limit;
    hi[nh] = (unsigned char)i;
    lo[nl] = (unsigned char)i;
    if constexpr (Wide)
      key[nh] = ((long long)s << 8) | (255 - i);
    else
      key[nh] = (int)((unsigned)std::clamp(s, -(1 << 22), (1 << 22) - 1) << 8) | (255 - i);
    nh += up;
    nl += 1 - up;
  }
#if defined(__AVX512F__)
  if constexpr (!Wide) {
    _mm512_storeu_si512(key + nh, _mm512_set1_epi32(INT_MIN));   // riempimento fino al gruppo di 16 successivo
    const int nchunk = (nh + 15) >> 4;
    for (int k = 0; k < nh; k++) {
      const __m512i x = _mm512_set1_epi32(key[k]);
      int r = 0;
      for (int c = 0; c < nchunk; c++)
        r += count_bits((U64)_mm512_cmpgt_epi32_mask(_mm512_load_si512(key + 16 * c), x));
      out[r] = {mv[hi[k]], sc[hi[k]]};
    }
  } else
#endif
  for (int k = 0; k < nh; k++) {
    const Key x = key[k];
    int r = 0;
    for (int j = 0; j < nh; j++)
      r += key[j] > x;
    out[r] = {mv[hi[k]], sc[hi[k]]};
  }
  for (int k = 0; k < nl; k++)
    out[nh + k] = {mv[lo[k]], sc[lo[k]]};
  return nh;
}

static void queue_start(MoveQueue &q, ThreadData &td, NodeFrame *f, int hash_move, int depth, int kind) {
  q.td = &td;
  q.f = f;
  q.kind = kind;
  q.depth = depth;
  q.quiets_off = false;
  q.checks = nullptr;
  q.cut_threshold = 0;
  q.n_tact = q.n_all = q.cur = 0;
  q.n_lose = q.cur_lose = 0;
  q.opp_ready = false;
  q.quiet_split = -1;
  bool hash_ok = hash_move > 0 && td_is_pseudo_legal(td, hash_move);
  if (kind == QK_CUTS)
    hash_ok = hash_ok && is_tactical(hash_move);
  q.hash_move = hash_ok ? hash_move : 0;
  q.step = QS_HASH;
}

// PromoOrder e BishopPairCapt (08/10/2026, spente = albero invariato): la promozione a donna e la cattura di un
// alfiere della coppia avversaria salgono nell'ordine. Il punteggio entra anche nella soglia SEE delle tattiche
// vincenti (-score / GoodCaptSeeDiv), come per ogni altro termine.
static inline int score_tactic(const ThreadData &td, int m) {
  const int vk = victim_kind(td, m);
  int s = td.capt_hist[move_piece(m)][move_to(m)][vk] + 7 * unit_value[vk];
  if (PromoOrder && promo_kind(m) == Q)
    s += PromoOrder;
  if (BishopPairCapt && vk == B && count_bits(td.bitboards[td.side == white ? b : B]) >= 2)
    s += BishopPairCapt;
  return s;
}

// AA2 (10/10/2026): case attaccate dall'insieme dei cavalli in `b`, con gli spostamenti di mask_knight_attacks
// (attacks.cpp) applicati alla bitboard intera: la stessa unione del ciclo su knight_attacks[]. Numerazione a8 = 0:
// la colonna a e' il bit 0 di ogni byte, quindi uno spostamento a destra di 1 o 2 bit va verso la colonna a.
static inline U64 knight_fill(U64 b) {
  const U64 l1 = (b >> 1) & 0x7F7F7F7F7F7F7F7FULL, l2 = (b >> 2) & 0x3F3F3F3F3F3F3F3FULL;
  const U64 r1 = (b << 1) & 0xFEFEFEFEFEFEFEFEULL, r2 = (b << 2) & 0xFCFCFCFCFCFCFCFCULL;
  const U64 h1 = l1 | r1, h2 = l2 | r2;
  return (h1 << 16) | (h1 >> 16) | (h2 << 8) | (h2 >> 8);
}

// Reciproci a 40 bit di 1 + ply per la storia dei primi ply (score_quiets): |8 * voce| < 2^16, divisore <= 5, quindi
// moltiplicare per M = ceil(2^40 / D) e troncare da' la divisione intera (errore M*D - 2^40 < D, come W1).
static constexpr auto low_ply_recip = [] {
  std::array<unsigned long long, LOW_PLY_SIZE> r{};
  for (int p = 0; p < LOW_PLY_SIZE; p++)
    r[p] = ((1ULL << 40) + (unsigned long long)p) / (unsigned long long)(p + 1);
  return r;
}();

// Punteggi delle quiete: statistiche (pesate), bonus agli scacchi diretti che non perdono materiale, e la mossa
// di un pezzo che esce da (o entra in) una casa attaccata da un pezzo nemico di valore minore.
static void score_quiets(MoveQueue &q, const int *mv, int *sc, int n) {
  PROF_GUARD(prof_score);
  ThreadData &td = *q.td;
  NodeFrame *f = q.f;
  const int us = td.side, them = us ^ 1;
  const int eoff = them == white ? 0 : 6, us_off = 6 - eoff;
  const U64 occ = td.occupancies[both];

  // Case attaccate da tutti i pedoni avversari insieme: gli stessi due shift di mask_pawn_attacks (attacks.cpp)
  // applicati alla bitboard intera, quindi la stessa unione del ciclo per pedone (04/10/2026).
  const U64 epawns = td.bitboards[P + eoff];
  const U64 by_pawn = them == white ? ((epawns >> 7) & not_a_file) | ((epawns >> 9) & not_h_file)
                                    : ((epawns << 7) & not_h_file) | ((epawns << 9) & not_a_file);
  // AA2 (10/10/2026, velocita', stessi punteggi): mappe dell'avversario e attacchi da casa sicura (AttackOrder*) da
  // UN calcolo DualMagic per pezzo lungo nemico (get_slider_pair, AA1), invece dei quattro cicli by_* e dei cinque cicli
  // di attack_to, che ricalcolavano gli attacchi degli stessi pezzi (alfiere, torre e donna di nuovo, due volte per la
  // donna). Contatori (bench 15 su fens30): 0,16 riempimenti per nodo, 0,84 calcoli per nodo prima, ~0,35 ora; nove
  // cicli a lunghezza variabile per riempimento diventano tre (piu' corti), cavalli e pedoni passano per insiemi.
  // Primo passo, sui pezzi lunghi: by_bishop, by_rook, by_queen e le parti di attack_to che non dipendono da `safe`
  // (le torri e la donna sono sempre bersagli per l'alfiere, la donna per la torre).
  const U64 eN = td.bitboards[N + eoff], eB = td.bitboards[B + eoff], eR = td.bitboards[R + eoff],
            eQ = td.bitboards[Q + eoff];
  const U64 by_knight = knight_fill(eN);
  U64 by_bishop = 0, by_rook = 0, by_queen = 0;
  U64 aa_b = 0, aa_r = 0, aa_q = 0;            // attack_to[B], [R], [Q] (vedi sotto per i bersagli)
  U64 sl_b[16], sl_r[16];                      // attacchi di alfiere e di torre dei pezzi lunghi, per il secondo passo
  unsigned char sl_slot[64];                   // indice in sl_b/sl_r per casa (scritto solo per i pezzi lunghi)
  int nsl = 0;
  for (U64 t = eB | eR | eQ; t; pop_lsb_bb(t)) {
    const int sq = get_ls1b_index(t);
    const SliderPair sp = get_slider_pair(sq, occ);
    const U64 b = pair_bishop(sp), r = pair_rook(sp);
    const U64 mB = 0 - ((eB >> sq) & 1), mR = 0 - ((eR >> sq) & 1), mQ = 0 - ((eQ >> sq) & 1);
    by_bishop |= b & mB;
    by_rook |= r & mR;
    by_queen |= (b | r) & mQ;
    aa_b |= b & (mR | mQ);
    aa_r |= r & mQ;
    sl_b[nsl] = b;
    sl_r[nsl] = r;
    sl_slot[sq] = (unsigned char)nsl++;
  }
  const U64 by_minor = by_knight | by_bishop;
  // Mappa per see_holds_q (la donna e il re non entrano in lesser[], solo qui).
  q.opp_slider = by_bishop | by_rook | by_queen;
  q.opp_att = by_pawn | by_knight | q.opp_slider | king_attacks[get_ls1b_index(td.bitboards[K + eoff])];
  q.opp_ready = true;
  U64 lesser[6];
  lesser[P] = 0;
  lesser[N] = lesser[B] = by_pawn;
  lesser[R] = by_minor | by_pawn;
  lesser[Q] = by_rook | lesser[R];
  lesser[K] = 0;

  LmpChkCtx local_checks;
  LmpChkCtx &checks = q.checks ? *q.checks : local_checks;
  if (!checks.ready)
    td_lmp_chk_init(td, checks);

  // Scudo del re (nostra): col re sulla traversa di casa, i nostri pedoni accanto al re si muovono per ultimi.
  // Vale fino al mediogioco: solo finche' l'avversario ha materiale d'attacco (KingShieldNpm, pezzi non pedoni).
  U64 shield = 0;
  if (KingShield) {
    const int ksq = get_ls1b_index(td.bitboards[us == white ? K : k]);
    const bool home = us == white ? ksq >= a1 : ksq <= h8;
    const int their_npm = unit_value[N] * count_bits(td.bitboards[N + eoff]) +
                          unit_value[B] * count_bits(td.bitboards[B + eoff]) +
                          unit_value[R] * count_bits(td.bitboards[R + eoff]) +
                          unit_value[Q] * count_bits(td.bitboards[Q + eoff]);
    if (home && their_npm >= KingShieldNpm)
      shield = king_attacks[ksq] & td.bitboards[us == white ? P : p];
  }

  // Avamposti (08/10/2026, utente): il cavallo che va su una casa della meta' avversaria (traverse 4-6 dal nostro
  // lato), difesa da un nostro pedone e che nessun pedone avversario potra' mai attaccare (nessun pedone nemico davanti
  // sulle colonne vicine), si prova prima. OutpostSafe: 1 = in piu' l'avversario non ha alfieri del colore della casa,
  // 2 = in piu' non ha cavalli (la casa non si puo' contestare con un pezzo minore). Numerazione a8 = 0: il bianco
  // avanza verso indici piu' bassi.
  // Case davanti ai pedoni avversari, nella loro direzione di marcia (front), e case che quei pedoni potranno
  // attaccare avanzando (span). Un nostro pedone fuori da entrambe e' passato.
  U64 front = 0, span = 0;
  if (OutpostOrder || OutpostOrderB || PassedPush) {
    front = epawns;
    if (them == white) { front |= front >> 8; front |= front >> 16; front |= front >> 32; }
    else               { front |= front << 8; front |= front << 16; front |= front << 32; }
    span = them == white ? ((front >> 7) & not_a_file) | ((front >> 9) & not_h_file)
                         : ((front << 7) & not_h_file) | ((front << 9) & not_a_file);
  }
  U64 outpost = 0;
  if (OutpostOrder || OutpostOrderB) {
    const U64 ours_p = td.bitboards[P + us_off];
    const U64 guarded = us == white ? ((ours_p >> 7) & not_a_file) | ((ours_p >> 9) & not_h_file)
                                    : ((ours_p << 7) & not_h_file) | ((ours_p << 9) & not_a_file);
    const U64 rows = us == white ? 0x000000FFFFFF0000ULL : 0x0000FFFFFF000000ULL;   // traverse 4-6 / 5-3
    outpost = rows & guarded & ~span;
    if (OutpostSafe >= 1) {
      constexpr U64 light = 0xAA55AA55AA55AA55ULL;   // a8 (indice 0) e' chiara
      const U64 eb = td.bitboards[B + eoff];
      if (eb & light)  outpost &= ~light;
      if (eb & ~light) outpost &= light;
    }
    if (OutpostSafe >= 2 && td.bitboards[N + eoff])
      outpost = 0;
  }

  // Spinta del passato (08/10/2026): case d'arrivo dalla traversa relativa PassedPushRank alla settima dove un nostro
  // pedone e' passato. La SEE (>= 0) si chiede solo alle mosse di pedone che ci arrivano.
  U64 passed_to = 0;
  if (PassedPush) {
    U64 rows = 0;
    for (int rr = PassedPushRank; rr <= 7; rr++)
      rows |= 0xFFULL << (8 * (us == white ? 8 - rr : rr - 1));
    passed_to = rows & ~(front | span);
  }

  // Attacchi da casa sicura (08/10/2026; l'idea e' in Reckless, la regola e' nostra): per ogni tipo di pezzo, le case
  // non attaccate dall'avversario da cui quel pezzo attaccherebbe un bersaglio. Bersagli: pezzi nemici (non pedoni,
  // non il re) di classe piu' alta (pedone < cavallo = alfiere < torre < donna), o della stessa classe o piu' bassa
  // ma non difesi. Le case si trovano al contrario, dagli attacchi del bersaglio (pedone: quelli di un pedone nemico).
  U64 attack_to[6] = {0, 0, 0, 0, 0, 0};
  if (AttackOrder || AttackOrderQ) {
    const U64 safe = ~q.opp_att;
    const U64 minors = eN | eB;
    // AA2, secondo passo. Bersagli: dell'alfiere er|eq|(minors&safe), della torre eq|((minors|er)&safe), della donna
    // (minors|er)&safe, del cavallo come l'alfiere, del pedone minors|er|eq. Le parti senza `safe` sono gia' in aa_b e
    // aa_r; alfieri e torri al sicuro riusano i loro attacchi del primo passo; i cavalli al sicuro (0-2) sono gli
    // unici bersagli che chiedono un calcolo nuovo. Cavalli e pedoni come insiemi (knight_fill, shift dei pedoni).
    for (U64 t = (eB | eR) & safe; t; pop_lsb_bb(t)) {
      const int sq = get_ls1b_index(t), k = sl_slot[sq];
      aa_b |= sl_b[k] & (0 - ((eB >> sq) & 1));
      aa_r |= sl_r[k];
      aa_q |= sl_b[k] | sl_r[k];
    }
    for (U64 t = eN & safe; t; pop_lsb_bb(t)) {
      const SliderPair sp = get_slider_pair(get_ls1b_index(t), occ);
      aa_b |= pair_bishop(sp);
      aa_r |= pair_rook(sp);
      aa_q |= pair_bishop(sp) | pair_rook(sp);
    }
    if (AttackOrder) {
      const U64 tp = minors | eR | eQ;   // gli attacchi di un pedone nemico posto su ogni bersaglio
      attack_to[P] = them == white ? ((tp >> 7) & not_a_file) | ((tp >> 9) & not_h_file)
                                   : ((tp << 7) & not_h_file) | ((tp << 9) & not_a_file);
      attack_to[N] = knight_fill(eR | eQ | (minors & safe));
      attack_to[B] = aa_b;
      attack_to[R] = aa_r;
    }
    if (AttackOrderQ)
      attack_to[Q] = aa_q;
#ifdef TRIUMV_VERIFY_AA2   // verifica: i cicli di prima (08/10/2026) devono dare le stesse mappe
    {
      const U64 er = eR, eq = eQ;
      U64 old[6] = {0, 0, 0, 0, 0, 0}, ok = 0, ob = 0, orr = 0, oq = 0;
      for (U64 t = eN; t; pop_lsb_bb(t)) ok |= knight_attacks[get_ls1b_index(t)];
      for (U64 t = eB; t; pop_lsb_bb(t)) ob |= get_bishop_attacks(get_ls1b_index(t), occ);
      for (U64 t = eR; t; pop_lsb_bb(t)) orr |= get_rook_attacks(get_ls1b_index(t), occ);
      for (U64 t = eQ; t; pop_lsb_bb(t)) oq |= get_queen_attacks(get_ls1b_index(t), occ);
      if (AttackOrder) {
        for (U64 t = minors | er | eq; t; pop_lsb_bb(t)) old[P] |= pawn_attacks[them][get_ls1b_index(t)];
        for (U64 t = er | eq | (minors & safe); t; pop_lsb_bb(t)) old[N] |= knight_attacks[get_ls1b_index(t)];
        for (U64 t = er | eq | (minors & safe); t; pop_lsb_bb(t)) old[B] |= get_bishop_attacks(get_ls1b_index(t), occ);
        for (U64 t = eq | ((minors | er) & safe); t; pop_lsb_bb(t)) old[R] |= get_rook_attacks(get_ls1b_index(t), occ);
      }
      if (AttackOrderQ)
        for (U64 t = (minors | er) & safe; t; pop_lsb_bb(t)) old[Q] |= get_queen_attacks(get_ls1b_index(t), occ);
      bool same = ok == by_knight && ob == by_bishop && orr == by_rook && oq == by_queen;
      for (int k = P; k <= Q; k++) same = same && old[k] == attack_to[k];
      if (!same) {
        printf("info string AA2 SBAGLIATA: mappe d'attacco diverse\n");
        fflush(stdout);
        abort();
      }
    }
#endif
    // AttackSafeSee 0: casa d'arrivo non attaccata da nessun pezzo nemico. 1: non attaccata da pezzi di valore minore
    // (lesser[]), e lo scambio sulla casa si controlla mossa per mossa con la SEE (sotto).
    for (int k = P; k <= Q; k++) attack_to[k] &= AttackSafeSee ? ~lesser[k] : safe;
  }
  const int attack_bonus[6] = {AttackOrder, AttackOrder, AttackOrder, AttackOrder, AttackOrderQ, 0};

  const PieceToRow &c1 = *f[-1].cont_hist, &c2 = *f[-2].cont_hist, &c3 = *f[-3].cont_hist,
                   &c4 = *f[-4].cont_hist, &c6 = *f[-6].cont_hist;
  const PieceToRow &prow = pawn_row(td);
  const int ply = f->ply;
  // AA2: la divisione per 1 + ply (ply < 5: il 17% delle quiete punteggiate a bench 13, ~8% a 15-16) era una idiv
  // per mossa, perche' il divisore non e' costante; ora il reciproco del ply del nodo (stesso valore, vedi sopra).
  const unsigned long long lp_recip = ply < LOW_PLY_SIZE ? low_ply_recip[ply] : 0;

  for (int i = 0; i < n; i++) {
    const int m = mv[i];
    const int pc = move_piece(m), to = move_to(m), from = move_from(m), pt = pc - us_off;   // mossa nostra
    const int key = quiet_key(m);
    int s = 2 * td.quiet_hist[us][key] + 2 * prow[pc][to];
    s += c1[pc][to] + c2[pc][to] + c3[pc][to] + c4[pc][to] + c6[pc][to];
    if (((checks.chk[pt] >> to) & 1) && see_holds_q(td, q, m, -75))
      s += CheckOrderBonus;
    s += unit_value[pt] * ThreatOrderMul * (int((lesser[pt] >> from) & 1) - int((lesser[pt] >> to) & 1));
    if (ply < LOW_PLY_SIZE) {
      const int x = 8 * td.low_ply_hist[ply][key];
      const int lq = (int)(((unsigned long long)(x < 0 ? -x : x) * lp_recip) >> 40);
#ifdef TRIUMV_VERIFY_AA2
      if ((x < 0 ? -lq : lq) != x / (1 + ply)) { printf("info string AA2 SBAGLIATA: low ply\n"); fflush(stdout); abort(); }
#endif
      s += x < 0 ? -lq : lq;
    }
    if (shield >> from & 1)
      s -= KingShield;
    if (pt == N && (outpost >> to & 1))
      s += OutpostOrder;
    if (pt == B && (outpost >> to & 1))
      s += OutpostOrderB;
    if ((attack_to[pt] >> to & 1) && (!AttackSafeSee || see_holds_q(td, q, m, 0)))
      s += attack_bonus[pt];
    if (pt == P && (passed_to >> to & 1) && see_holds_q(td, q, m, 0))
      s += PassedPush;
    sc[i] = s;
  }
}

// 05/10/2026 — FUORI DA queue_next (noinline). Incorporate, le tre fill portavano in queue_next le loro liste (mosse
// del generatore, punteggi, chiavi dell'ordinamento): frame da 5.048 byte, oltre la pagina da 4 KB, e su Windows ogni
// chiamata di queue_next, cioe' una per mossa, passava dalla sonda dello stack (__chkstk). Le fill girano una volta
// per fase del nodo: il loro frame resta sotto i 4 KB e la sonda sparisce (verificato nel disassemblato PGO).
// xperf 6 giri, release PGO avx512, 57,8 M nodi identici: istruzioni -2,76%, cicli -0,81%; insieme alle liste della
// valutazione fuori dallo stack (AccumulatorStack::BothLists) cicli -1,55%.
__attribute__((noinline)) static void fill_tactics(MoveQueue &q) {
  ThreadData &td = *q.td;
  moves ml[1];
  int sc[256];
  td_generate_moves(td, ml, /*captures_only=*/true, /*quiets_only=*/false, /*known_in_check=*/0,
                    /*promo_quiet=*/true, /*promo_queen_only=*/true);
  // La mossa hash e' gia' uscita (QS_HASH): si toglie qui compattando sul posto, senza salti, e i cicli di
  // queue_next non la confrontano piu' a ogni mossa (04/10/2026 sera; stesso ordine delle altre, stesso albero).
  int n = 0;
  for (int i = 0; i < ml->count; i++) {
    const int m = ml->moves[i];
    ml->moves[n] = m;
    n += m != q.hash_move;
  }
  for (int i = 0; i < n; i++)
    sc[i] = score_tactic(td, ml->moves[i]);
  q.n_tact = q.n_all = n;
  rank_into<false>(q.list, ml->moves, sc, n, INT_MIN);
  q.cur = 0;
}

__attribute__((noinline)) static void fill_quiets(MoveQueue &q) {
  ThreadData &td = *q.td;
  moves ml[1];
  int sc[256];
  td_generate_moves(td, ml, /*captures_only=*/false, /*quiets_only=*/true, /*known_in_check=*/0);
  // La promozione a donna e' gia' fra le tattiche e la mossa hash e' gia' uscita: si tolgono compattando sul posto,
  // senza salti.
  int n = 0;
  for (int i = 0; i < ml->count; i++) {
    const int m = ml->moves[i];
    ml->moves[n] = m;
    n += (promo_kind(m) != Q) & (m != q.hash_move);
  }
  q.n_all = q.n_tact + n;
  score_quiets(q, ml->moves, sc, n);
  const int limit = -QuietSortDepthMul * q.depth;
  const int nh = rank_into<false>(q.list + q.n_tact, ml->moves, sc, n, limit);
  q.cur = q.n_tact;
  // V2 (08/10/2026, velocita', stesso ordine): le quiete buone (punteggio > GoodQuietLimit) escono in QS_QUIET_HIGH,
  // le altre in QS_QUIET_LOW, ognuna nell'ordine della lista; prima ogni fase scorreva tutta la lista e confrontava
  // ogni punteggio (le buone venivano riviste e scartate una seconda volta in QS_QUIET_LOW). Se limit <=
  // GoodQuietLimit + 1 (profondita' >= 5 con i valori attuali), la parte non ordinata ha punteggi < limit, quindi
  // tutti <= GoodQuietLimit, e la parte ordinata e' decrescente: le buone sono esattamente un prefisso della lista.
  // Il confine si trova una volta (ciclo sui soli nh ordinati) e le due fasi diventano due intervalli contigui, con lo
  // stesso ordine di uscita di prima. Altrimenti (profondita' basse) si resta allo scorrimento con i confronti.
  if (limit <= GoodQuietLimit + 1) {
    int b = 0;
    while (b < nh && q.list[q.n_tact + b].score > GoodQuietLimit)
      b++;
    q.quiet_split = q.n_tact + b;
#ifdef TRIUMV_VERIFY_QSPLIT   // verifica: prima del confine solo buone, dopo solo cattive (stesso ordine di prima)
    for (int i = q.n_tact; i < q.n_all; i++)
      if ((q.list[i].score > GoodQuietLimit) != (i < q.quiet_split)) {
        printf("info string QSPLIT SBAGLIATO: indice %d punteggio %d confine %d\n", i, q.list[i].score, q.quiet_split);
        fflush(stdout);
        abort();
      }
#endif
  }
}

__attribute__((noinline)) static void fill_evasions(MoveQueue &q) {
  ThreadData &td = *q.td;
  moves ml[1];
  int sc[256];
  td_generate_moves(td, ml, false, false, /*known_in_check=*/1);
  int n = 0;   // senza la mossa hash, gia' uscita (vedi fill_tactics)
  for (int i = 0; i < ml->count; i++) {
    const int m = ml->moves[i];
    ml->moves[n] = m;
    n += m != q.hash_move;
  }
  const PieceToRow &c1 = *q.f[-1].cont_hist;
  // Le tattiche davanti a tutte le quiete: 1 << 20 basta (|quiete| <= LIM_QUIET + LIM_FOLLOW = 37.183) e sta nei 22
  // bit delle chiavi a 32 bit; prima 1 << 28 con chiavi a 64 bit (frame di fill_evasions oltre i 4 KB). Stesso
  // ordine, stesso albero (05/10/2026).
  for (int i = 0; i < n; i++) {
    const int m = ml->moves[i];
    sc[i] = is_tactical(m) ? unit_value[victim_kind(td, m)] + (1 << 20)
                           : td.quiet_hist[td.side][quiet_key(m)] + c1[move_piece(m)][move_to(m)];
  }
  q.n_all = q.n_tact = n;
  rank_into<false>(q.list, ml->moves, sc, n, INT_MIN);
  q.cur = 0;
}

// Prossima mossa pseudo-legale, 0 a coda finita. La mossa hash esce una volta sola.
static int queue_next(MoveQueue &q) {
  ThreadData &td = *q.td;
  for (;;) {
    switch (q.step) {
    case QS_HASH:
      q.step = q.kind == QK_EVADE ? QS_EVADE_FILL : q.kind == QK_MAIN ? QS_TACT_FILL : QS_ONLY_TACT_FILL;
      if (q.hash_move)
        return q.hash_move;
      break;

    case QS_TACT_FILL:
      fill_tactics(q);
      q.step = QS_TACT_WIN;
      break;

    case QS_TACT_WIN:
      while (q.cur < q.n_tact) {
        const ScoredMove sm = q.list[q.cur++];
        if (see_holds(td, sm.move, -sm.score / GoodCaptSeeDiv))
          return sm.move;
        q.losers[q.n_lose++] = (unsigned char)(q.cur - 1);
      }
      q.step = QS_QUIET_FILL;
      break;

    case QS_QUIET_FILL:
      if (!q.quiets_off)
        fill_quiets(q);
      q.step = QS_QUIET_HIGH;
      break;

    case QS_QUIET_HIGH:
      if (q.quiet_split >= 0) {   // V2: le buone sono il prefisso [n_tact, quiet_split)
        if (!q.quiets_off && q.cur < q.quiet_split)
          return q.list[q.cur++].move;
      } else
        while (!q.quiets_off && q.cur < q.n_all) {
          const ScoredMove sm = q.list[q.cur++];
          if (sm.score > GoodQuietLimit)
            return sm.move;
        }
      q.step = QS_TACT_LOSE;
      break;

    case QS_TACT_LOSE:
      if (q.cur_lose < q.n_lose)
        return q.list[q.losers[q.cur_lose++]].move;
      q.cur = q.quiet_split >= 0 ? q.quiet_split : q.n_tact;
      q.step = QS_QUIET_LOW;
      break;

    case QS_QUIET_LOW:
      if (q.quiet_split >= 0) {   // V2: le cattive sono il resto [quiet_split, n_all), gia' nell'ordine giusto
        if (!q.quiets_off && q.cur < q.n_all)
          return q.list[q.cur++].move;
      } else
        while (!q.quiets_off && q.cur < q.n_all) {
          const ScoredMove sm = q.list[q.cur++];
          if (sm.score <= GoodQuietLimit)
            return sm.move;
        }
      q.step = QS_DONE;
      break;

    case QS_EVADE_FILL:
      fill_evasions(q);
      q.step = QS_DRAIN;
      break;

    case QS_ONLY_TACT_FILL:
      fill_tactics(q);
      q.step = QS_DRAIN;
      break;

    case QS_DRAIN:
      while (q.cur < q.n_tact) {
        const ScoredMove sm = q.list[q.cur++];
        if (q.kind == QK_CUTS && !see_holds(td, sm.move, q.cut_threshold))
          continue;
        return sm.move;
      }
      q.step = QS_DONE;
      break;

    default:
      return 0;
    }
  }
}

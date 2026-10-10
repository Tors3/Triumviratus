// search/07_makemove.inc -- Attacchi, make_move e unmake_move per thread.
// Parte di threads.cpp: NON e' un'unita' di compilazione a se'. Va inclusa solo da threads.cpp, nell'ordine
// fissato li' (divisione del 27/09/2026, codice generato identico: vedi l'intestazione di threads.cpp).

// Chiavi aggiuntive delle correzioni (09_history.inc): servono al prefetch in td_make_move.
static inline U64 corr_phase_key(const ThreadData &td);
static inline U64 corr_fifty_key(const ThreadData &td);
static inline U64 corr_expert_key(const ThreadData &td);

// ============================================================================
// ATTACK DETECTION
// ============================================================================

static inline int td_is_square_attacked(ThreadData &td, int square,
                                        int attacker_side) {
  PROF_GUARD(prof_isatk);
#ifdef TRIUMV_PROFILE
  prof_n_isatk++;
#endif
  if (attacker_side == white) {
    if (pawn_attacks[black][square] & td.bitboards[P])
      return 1;
    if (knight_attacks[square] & td.bitboards[N])
      return 1;
    if (king_attacks[square] & td.bitboards[K])
      return 1;
    // AA1 (10/10/2026): alfiere e torre da un solo calcolo (magic.h).
    const SliderPair sp = get_slider_pair(square, td.occupancies[both]);
    if (pair_bishop(sp) & (td.bitboards[B] | td.bitboards[Q]))
      return 1;
    if (pair_rook(sp) & (td.bitboards[R] | td.bitboards[Q]))
      return 1;
  } else {
    if (pawn_attacks[white][square] & td.bitboards[p])
      return 1;
    if (knight_attacks[square] & td.bitboards[n])
      return 1;
    if (king_attacks[square] & td.bitboards[k])
      return 1;
    const SliderPair sp = get_slider_pair(square, td.occupancies[both]);
    if (pair_bishop(sp) & (td.bitboards[b] | td.bitboards[q]))
      return 1;
    if (pair_rook(sp) & (td.bitboards[r] | td.bitboards[q]))
      return 1;
  }
  return 0;
}

// Case strettamente tra due quadrati allineati (0 per cavallo/non-allineati). Riempita da init_cuckoo
// (10_order.inc); usata qui (scacchi di scoperta), dalle evasioni (08_movegen.inc), dalla legalita' e
// dall'upcoming-repetition.
static U64 between_tbl[64][64];
// Attacchi di torre e alfiere a scacchiera VUOTA (06/10/2026, velocita', albero identico): servono a trovare i cecchini
// allineati con un re (td_lmp_chk_init, safety_init, see_pins_update: ~3,3 letture per nodo). Prima passavano da
// get_rook_attacks(sq, 0) / get_bishop_attacks(sq, 0), cioe' dalla voce 0 della riga della casa nelle tabelle PEXT:
// righe da 32 KB (torre) e 4 KB (alfiere): le 64 voci "vuote" di ciascun pezzo cadono nello STESSO insieme della L1
// (bit 6-11 dell'indirizzo uguali, 8 vie), insieme alle voci 0-7 di ogni riga. Qui stanno in 1 KB contiguo. Stessi valori.
// Riempite da init_cuckoo (10_order.inc) insieme a between_tbl.
alignas(64) static U64 empty_rook_att[64];
alignas(64) static U64 empty_bishop_att[64];

// ---------------------------------------------------------------------------
// LMPCheckGuard: "questa quiet da' scacco?" calcolato PRIMA della make.
// Il gives_check del motore (:4631+) vive DOPO la make; la LMP pota prima,
// quindi serve il test SF-style con le check-squares: le case da cui un pezzo
// di tipo T attaccherebbe il re nemico, calcolate UNA VOLTA per nodo
// (pigramente, solo se la LMP scatta davvero) e poi O(1) per mossa.
struct LmpChkCtx {
  bool ready = false;
  U64 chk[6] = {0, 0, 0,
                0, 0, 0}; // indicizzato per tipo: P,N,B,R,Q,K (K inutilizzato)
  U64 disc = 0;           // nostri pezzi che, muovendosi, scoprono uno scacco
};

static inline void td_lmp_chk_init(ThreadData &td, LmpChkCtx &c) {
  const int us = td.side;
  const int off = (us == white) ? 0 : 6;  // offset nostro nei bitboards
  const int eoff = (us == white) ? 6 : 0; // offset nemico
  const int eksq = get_ls1b_index(td.bitboards[K + eoff]);
  const U64 occ = td.occupancies[both];

  c.chk[N] = knight_attacks[eksq];
  const SliderPair sp = get_slider_pair(eksq, occ);   // AA1 (10/10/2026): un calcolo per i due
  c.chk[B] = pair_bishop(sp);
  c.chk[R] = pair_rook(sp);
  c.chk[Q] = c.chk[B] | c.chk[R];
  c.chk[P] =
      pawn_attacks[us ^ 1][eksq]; // da dove un NOSTRO pedone attacca quel re
  c.chk[K] = 0;

  // Scacchi di scoperta: un nostro pezzo e' l'unico schermo fra un nostro
  // slider e il re nemico. Il re nemico NON e' sotto scacco (e' il nostro
  // tratto), quindi qualunque nostro R/Q (risp. B/Q) che compare togliendo lo
  // schermo e' per forza dietro di esso.
  // 04/10/2026 sera — dai CECCHINI, non dai candidati: si parte dai nostri pezzi lunghi allineati col re nemico
  // (attacchi su scacchiera vuota) e si guarda che cosa c'e' in mezzo: un solo pezzo, nostro, e' lo schermo. Stesso
  // insieme di prima (il re nemico non e' sotto scacco, quindi ogni cecchino ha almeno un pezzo davanti), ma una
  // lettura della tabella "in mezzo" per cecchino invece di una lettura PEXT per ogni nostro pezzo visto dal re.
  const U64 ours = td.occupancies[us];
  const U64 orq = td.bitboards[R + off] | td.bitboards[Q + off];
  const U64 obq = td.bitboards[B + off] | td.bitboards[Q + off];
  c.disc = 0;
  U64 snipers = (empty_rook_att[eksq] & orq) | (empty_bishop_att[eksq] & obq);
  while (snipers) {
    const int sn = get_ls1b_index(snipers);
    const U64 between = between_tbl[eksq][sn] & occ;
    if (between && !(between & (between - 1)) && (between & ours))
      c.disc |= between;
    pop_lsb_bb(snipers);
  }
  c.ready = true;
}

// ponytail: sovrastima di proposito. Un pezzo "disc" che si muove RESTANDO
// sulla linea del re non scopre nulla, ma qui conta comunque come scacco (manca
// una tabella aligned()) — e lo stesso per l'arrocco, dove la torre puo' dare
// scacco dalla casa d'arrivo. Sovrastimare = potare MENO = direzione sicura per
// un guard. Se il patch rende, si affina.
static inline bool td_lmp_gives_check(ThreadData &td, int move, LmpChkCtx &c) {
  PROF_GUARD(prof_gc);
  if (!c.ready)
    td_lmp_chk_init(td, c);
  const int tgt = get_move_target(move);
  const int pt = get_move_piece(move) % 6; // P..K indipendente dal colore
  return (c.chk[pt] & (1ULL << tgt)) ||
         (c.disc & (1ULL << get_move_source(move)));
}

// Casa di partenza/arrivo della TORRE nell'arrocco, dedotte dalla casa d'arrivo del re. Unica fonte per
// tutti i siti (occupazioni, chiavi, mailbox, make, rollback, unmake, NNUE). La partenza viene da
// castle_rook_sq (chess960.h): h1/a1/h8/a8 negli scacchi normali, la casa letta dalla FEN nel 960.
static inline void td_castle_rook(int target, int &rook_pc, int &rf, int &rt) {
  const int idx = castle_index(target);
  rook_pc = idx < 2 ? R : r;
  rf = castle_rook_sq[idx];
  rt = castle_rook_to(idx);
}

// Incremental occupancy update. Flips only the bits that the move touches,
// instead of OR-recomputing all 12 bitboards (+2.89% NPS, node-identical). XOR
// is self-inverse, so calling it again with the same move reverses it (used on
// make-forward, illegal rollback, and unmake). 'us' must be the MOVING side at
// the call site. AA3 (10/10/2026): rollback e unmake rimettono le occupazioni (e le chiavi di td_keys_update) dalla
// copia in UndoInfo; qui resta solo la make.
// captured_piece == -1 means no capture. For castling the rook squares derive
// from the king target. Produces occupancies identical to the full recompute.
static inline void td_occ_update(ThreadData &td, int us, int source, int target,
                                 int captured_square, int captured_piece,
                                 int castling) {
  // XOR fra le due case (non OR): uguale per ogni mossa normale (source != target), e nel Chess960 il re che
  // arrocca senza muoversi (source == target) non cambia nulla, invece di svuotare la propria casa. Con le
  // quattro case di re e torre in XOR anche le sovrapposizioni del 960 (re dove partiva la torre) tornano giuste.
  td.occupancies[us] ^= (1ULL << source) ^ (1ULL << target);
  // capture (== target, or e.p. square). AA3 (10/10/2026): senza salto, maschera nulla senza vittima.
  td.occupancies[us ^ 1] ^= captured_piece != -1 ? 1ULL << (captured_square & 63) : 0;
  if (castling) {
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    td.occupancies[us] ^= (1ULL << rf) ^ (1ULL << rt);
  }
  td.occupancies[both] = td.occupancies[white] | td.occupancies[black];
}

#ifdef TRIUMV_MAILBOX_VERIFY
// Verifica esaustiva mailbox<->bitboard, per la validazione. Il perft del motore usa
// make_move GLOBALE e non passa mai di qui, quindi senza questo controllo non ci
// sarebbe modo di provare che i rami arrocco/e.p./promozione siano coperti: un
// disallineamento non da' crash ne' firma diversa, solo un ordinamento che sbaglia.
//   build:  -DTRIUMV_MAILBOX_VERIFY   poi   echo bench | motore
static inline void td_mailbox_verify(ThreadData &td, const char *where) {
  for (int sq = 0; sq < 64; sq++) {
    int expect = -1;
    for (int pc = P; pc <= k; pc++)
      if (get_bit(td.bitboards[pc], sq)) { expect = pc; break; }
    if (td.piece_on[sq] != expect) {
      fprintf(stderr, "[MAILBOX] %s: sq=%d mailbox=%d bitboards=%d\n", where, sq,
              td.piece_on[sq], expect);
      fflush(stderr);
      abort();
    }
  }
}
  #define MB_VERIFY(td, where) td_mailbox_verify((td), (where))
#else
  #define MB_VERIFY(td, where) ((void) 0)
#endif

// MAILBOX — le due meta' NON sono self-inverse come td_occ_update (che e' uno XOR):
// qui si scrivono valori, quindi servono andata e ritorno distinti. Vanno chiamate
// negli STESSI TRE SITI di td_occ_update, o la mailbox si disallinea dai bitboard
// e l'ordinamento sbaglia in silenzio (nessun crash, nessuna firma diversa).
static inline void td_mailbox_apply(ThreadData &td, int piece, int source, int target,
                                    int promoted, int captured_piece,
                                    int captured_square, int castling) {
  // La vittima PRIMA del pezzo che arriva: nell'e.p. `captured_square` != target,
  // in una cattura normale coincidono e deve vincere il pezzo che arriva.
  if (castling) {
    // Prima le due partenze, poi i due arrivi: nel Chess960 il re puo' arrivare dove partiva la torre (o
    // non muoversi), e scrivere "re arrivato" prima di "torre partita" cancellerebbe il re.
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    td.piece_on[source] = -1;
    td.piece_on[rf] = -1;
    td.piece_on[target] = piece;
    td.piece_on[rt] = rook_pc;
    return;
  }
  // AA3 (10/10/2026): senza salto sulla cattura; senza vittima la prima scrittura cade sulla casa di partenza. L'indice
  // si sceglie con una maschera: con `?:` clang mette un salto per non leggere captured_square.
  td.piece_on[source ^ ((source ^ captured_square) & -int(captured_piece != -1))] = -1;
  td.piece_on[source] = -1;
  td.piece_on[target] = promoted ? promoted : piece;
}

static inline void td_mailbox_revert(ThreadData &td, int piece, int source, int target,
                                     int promoted, int captured_piece,
                                     int captured_square, int castling) {
  (void) promoted;  // in uscita il pezzo torna quello di partenza, promosso o no
  if (castling) {
    // Speculare a td_mailbox_apply: prima si svuotano i due arrivi, poi si rimettono re e torre.
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    td.piece_on[target] = -1;
    td.piece_on[rt] = -1;
    td.piece_on[source] = piece;
    td.piece_on[rf] = rook_pc;
    return;
  }
  // Ordine speculare: si svuota target e POI si rimette la vittima, cosi' quando
  // captured_square == target (cattura normale) la vittima riprende il suo posto.
  // AA3 (10/10/2026): senza salto sulla cattura. Senza vittima la terza scrittura rimette -1 su target.
  td.piece_on[target] = -1;
  td.piece_on[source] = piece;
  td.piece_on[target ^ ((target ^ captured_square) & -int(captured_piece != -1))] = captured_piece;
}

// ============================================================================
// MAKE MOVE (returns 1 if legal)
// ============================================================================

// Chiavi parziali della posizione (XOR self-inverse: la stessa chiamata nei 3 siti di td_occ_update — make-forward,
// rollback della mossa illegale, unmake):
//   pawn_key   (P2.1)          solo pedoni: via da source, su target solo se NON promuove; pedone catturato via da
//                              captured_square (anche e.p.). Letta da pawn_row e dalla correzione pedoni.
//   np_key[c]  (CorrNonPawn)   non-pedoni per colore, re compreso: il mover non-pedone, il pezzo PROMOSSO che appare
//                              su target, il non-pedone catturato (chiave del colore catturato), la torre dell'arrocco.
//   mm_key[0/1] (NPS 25/09)    minori (N/B) e maggiori (R/Q), stessi eventi. Prima td_corr_index_minor/major
//                              riscandivano 8 bitboard fino a quattro volte per nodo (~370 cicli/nodo il 25/09).
// 04/10/2026 sera — UNA funzione SENZA SALTI al posto delle tre (td_pawn_key_update, td_np_key_update,
// td_mm_key_update), che tra loro facevano una decina di salti sul tipo di pezzo, mal predetti (xperf: ~0,75 per
// nodo). Le sei chiavi sono un array contiguo (threads.h: pawn, minori, maggiori, np bianco, np nero, pozzo) e due
// tabelle dicono, per codice pezzo, in quali due chiavi finisce il suo XOR; cio' che non va in nessuna chiave finisce
// nel pozzo. L'indice 15 e' "nessun pezzo" (captured_piece = -1): piece_keys ha 16 righe, le ultime quattro a zero.
// Stessi valori di prima per costruzione => nodi identici. Resta un solo salto, quello dell'arrocco (raro).
enum { KEY_PAWN = 0, KEY_MINOR = 1, KEY_MAJOR = 2, KEY_NP_W = 3, KEY_NP_B = 4, KEY_SINK = 5 };
static_assert(offsetof(ThreadData, mm_key) == offsetof(ThreadData, pawn_key) + 8 * KEY_MINOR &&
              offsetof(ThreadData, np_key) == offsetof(ThreadData, pawn_key) + 8 * KEY_NP_W &&
              offsetof(ThreadData, key_sink) == offsetof(ThreadData, pawn_key) + 8 * KEY_SINK,
              "td_keys_update: le chiavi parziali devono essere contigue in ThreadData");
//                                            P         N          B          R          Q          K
static constexpr unsigned char kKeyA[16] = {KEY_PAWN, KEY_MINOR, KEY_MINOR, KEY_MAJOR, KEY_MAJOR, KEY_SINK,
                                            KEY_PAWN, KEY_MINOR, KEY_MINOR, KEY_MAJOR, KEY_MAJOR, KEY_SINK,
                                            KEY_SINK, KEY_SINK, KEY_SINK, KEY_SINK};
static constexpr unsigned char kKeyB[16] = {KEY_SINK, KEY_NP_W, KEY_NP_W, KEY_NP_W, KEY_NP_W, KEY_NP_W,
                                            KEY_SINK, KEY_NP_B, KEY_NP_B, KEY_NP_B, KEY_NP_B, KEY_NP_B,
                                            KEY_SINK, KEY_SINK, KEY_SINK, KEY_SINK};
static inline void td_keys_update(ThreadData &td, int piece, int source, int target, int promoted,
                                  int captured_piece, int captured_square, int castling) {
  U64 *const keys = &td.pawn_key;
  // Il mover: via da source, su target (per il pedone che promuove il target lo prende il pezzo promosso).
  const U64 kd = piece_keys[piece][source] ^ (piece_keys[piece][target] & (0 - (U64)(promoted == 0)));
  keys[kKeyA[piece]] ^= kd;
  keys[kKeyB[piece]] ^= kd;
  // Il pezzo promosso appare su target (promoted = 0 -> chiave azzerata, finisce in pawn/pozzo come zero).
  const U64 kp = piece_keys[promoted][target] & (0 - (U64)(promoted != 0));
  keys[kKeyA[promoted]] ^= kp;
  keys[kKeyB[promoted]] ^= kp;
  // Il catturato: -1 diventa 15 (riga a zero, chiavi nel pozzo).
  const int ci = captured_piece & 15;
  const U64 kc = piece_keys[ci][captured_square & 63];
  keys[kKeyA[ci]] ^= kc;
  keys[kKeyB[ci]] ^= kc;
  if (castling) {
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    const U64 kr = piece_keys[rook_pc][rf] ^ piece_keys[rook_pc][rt];
    keys[rook_pc == R ? KEY_NP_W : KEY_NP_B] ^= kr;
    keys[KEY_MAJOR] ^= kr;
  }
}

// Chiave STIMATA del figlio (SF key_after): pezzo da/a, vittima dalla mailbox (ancora pre-mossa), lato, e.p.
// azzerato. Ignora arrocco, promozione, nuova casa e.p. e diritti d'arrocco: in quei casi (rari) la stima sbaglia e
// la make rifa' i prefetch con la chiave vera. Senza salti: una casa vuota ha -1 nella mailbox, cioe' la riga 15 di
// piece_keys, che e' a zero.
// ⛔ 04/10/2026 notte — PROVATO E TOLTO: emettere questi prefetch gia' in move_loop e quiesce, appena la mossa esce
// dalla coda (prima di scacco, potatura e singolare), per dare piu' anticipo alla probe del figlio. xperf 6 giri, nodi
// identici: cicli +0,03%, istruzioni +0,70%. La probe non aspettava piu' di quanto la make gia' copra; i prefetch
// delle mosse poi potate erano solo istruzioni in piu'.
static inline U64 td_child_key_guess(const ThreadData &td, int move) {
  const int source = get_move_source(move), target = get_move_target(move), piece = get_move_piece(move);
  U64 key = td.hash_key ^ piece_keys[piece][source] ^ piece_keys[piece][target] ^ side_key;
  key ^= piece_keys[td.piece_on[target] & 15][target];
  if (td.enpassant != no_sq)
    key ^= enpassant_keys[td.enpassant];
  return key;
}
// Le due linee che il figlio legge per prime: il bucket hash e la cache delle valutazioni (1 MB per thread).
// 05/10/2026: il profilo attribuiva l'1,8% dei cicli alla riga della eval cache; PROVATO a toglierla: xperf 6 giri a
// macchina quieta, 55,8 M nodi identici, cicli +0,89%, istruzioni +0,28%. Il prefetch serve: quei campioni erano la
// traduzione dell'indirizzo del bucket hash (riga sopra) attribuita all'istruzione dopo. Resta.
static inline void td_prefetch_child(const ThreadData &td, U64 key) {
  TT_PREFETCH(&hash_table[tt_base_index(key)]);
  TT_PREFETCH(&td.eval_cache[key & ThreadData::EVAL_CACHE_MASK]);
}

// known_legal: la ricerca ha gia' deciso la legalita' (legal_before a re non sotto scacco, vedi step_in) e il test
// "il nostro re e' attaccato?" qui sotto sarebbe un doppione. Perft, radice e validazioni lo lasciano a false.
static inline int td_make_move(ThreadData &td, int move, UndoInfo &undo, bool known_legal = false) {
  PROF_GUARD(prof_make);
  undo.old_castle = td.castle;
  undo.old_enpassant = td.enpassant;
  // AA3 (10/10/2026): chiavi, 50 mosse, ply dalla nulla e occupazioni di prima, per la unmake (vedi UndoInfo).
  std::memcpy(undo.keys, &td.hash_key, sizeof(undo.keys));
  std::memcpy(undo.occ, td.occupancies, sizeof(undo.occ));

  int source = get_move_source(move);
  int target = get_move_target(move);
  int piece = get_move_piece(move);
  int promoted = get_move_promoted(move);
  int capture = get_move_capture(move);
  int double_push = get_move_double(move);
  int enpass = get_move_enpassant(move);
  int castling = get_move_castling(move);

  // NPS 25/09/2026 — PREFETCH ANTICIPATO (SF key_after), vedi td_child_key_guess. Il profilo del 25/09 dava ~6% del
  // tempo al carico dell'entry TT nel probe del figlio: il prefetch partiva in fondo alla make e aveva poco anticipo.
  // Nodi identici: i prefetch non cambiano nulla di funzionale.
  const U64 pf_key = td_child_key_guess(td, move);
  td_prefetch_child(td, pf_key);

  // Pezzo catturato e sua casa, dalla scacchiera ancora intatta (07/10/2026: calcolati qui in cima, prima di ogni
  // modifica, perche' servono anche alle dirty della rete qui sotto; stessi valori di prima).
  // AA3 (10/10/2026, velocita', stessi valori): scelte di valore invece di salti. La cattura e' un dato della mossa
  // (24-28% delle make nei contatori, bench 15-16 su fens30/eg30), e il salto su di lei qui, piu' sotto e nella unmake
  // era mal predetto: un predittore a 2 bit per sito sbaglia ~0,23-0,28 volte per nodo in ognuno dei due siti.
  {
    const int start = (td.side == white) ? p : P;
    const int end = (td.side == white) ? k : K;
    // Il test di range replica esattamente il ciclo sostituito, che cercava solo fra i pezzi AVVERSARI e lasciava
    // -1 se non trovava nulla.
    const int mb_pc = td.piece_on[target];
    const int cp_normal = (mb_pc >= start && mb_pc <= end) ? mb_pc : -1;
    const int csq = enpass ? ((td.side == white) ? target + 8 : target - 8) : target;
    const int cp = enpass ? ((td.side == white) ? p : P) : cp_normal;
    undo.captured_square = capture ? csq : -1;
    undo.captured_piece = capture ? cp : -1;
  }

  // Le dirty della rete per questa mossa, sulla scacchiera PRIMA della mossa (06_nndirty.inc): un nuovo stato sulla
  // pila dell'handle. Se la mossa risulta illegale, lo stato si toglie qui sotto insieme al resto.
  {
    int rook_pc = -1, rf = 0, rt = 0;
    if (castling)
      td_castle_rook(target, rook_pc, rf, rt);
    nn_make_dirty(td, piece, source, target, promoted, undo.captured_piece, undo.captured_square, rook_pc, rf, rt);
  }

  pop_bit(td.bitboards[piece], source);
  set_bit(td.bitboards[piece], target);
  td.hash_key ^= piece_keys[piece][source];
  td.hash_key ^= piece_keys[piece][target];

  // AA3: 50 mosse e vittima senza salti. Senza vittima la maschera e' nulla (bitboard 0 invariata) e la riga 15 di
  // piece_keys e' a zero (vedi td_keys_update).
  // Con || e ?: clang emette tre salti (cattura, pedone bianco, pedone nero): qui and di bit, nessun salto.
  const int fifty_runs = int(piece != P) & int(piece != p) & int(capture == 0);
  td.fifty = (td.fifty + 1) & -fifty_runs;
  {
    const int cp = undo.captured_piece;
    const U64 cbit = cp != -1 ? 1ULL << (undo.captured_square & 63) : 0;
    td.bitboards[cp != -1 ? cp : 0] &= ~cbit;
    td.hash_key ^= piece_keys[cp & 15][undo.captured_square & 63];
  }

  if (promoted) {
    pop_bit(td.bitboards[piece], target);
    td.hash_key ^= piece_keys[piece][target];
    set_bit(td.bitboards[promoted], target);
    td.hash_key ^= piece_keys[promoted][target];
  }

  if (td.enpassant != no_sq)
    td.hash_key ^= enpassant_keys[td.enpassant];
  td.enpassant = no_sq;

  if (double_push) {
    int ep_sq = (td.side == white) ? target + 8 : target - 8;
    // F-017 EPKeyFix (default ON): ep nel board/key SOLO se un pedone nemico
    // puo' catturarlo (pseudo-legale, come SF). Niente "phantom ep" -> le
    // trasposizioni a2a3+a3a4 vs a2a4 hanno la stessa chiave = piu' TT-hit.
    // Nessun effetto sul movegen: senza capturer la casella ep era comunque
    // inusabile.
    if ((pawn_attacks[td.side][ep_sq] &
                          td.bitboards[(td.side == white) ? p : P])) {
      td.enpassant = ep_sq;
      td.hash_key ^= enpassant_keys[ep_sq];
    }
  }

  if (castling) {
    // Re e torre in bitboard diverse: anche con le sovrapposizioni del 960 pop/set non interferiscono.
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    pop_bit(td.bitboards[rook_pc], rf);
    set_bit(td.bitboards[rook_pc], rt);
    td.hash_key ^= piece_keys[rook_pc][rf];
    td.hash_key ^= piece_keys[rook_pc][rt];
  }

  td.hash_key ^= castle_keys[td.castle];
  td.castle &= castling_rights[source];
  td.castle &= castling_rights[target];
  td.hash_key ^= castle_keys[td.castle];

  td_occ_update(td, td.side, source, target, undo.captured_square,
                undo.captured_piece, castling);
  td_keys_update(td, piece, source, target, promoted, undo.captured_piece, undo.captured_square, castling);
  td_mailbox_apply(td, piece, source, target, promoted, undo.captured_piece,
                   undo.captured_square, castling);  // sito 1/3: make-forward
  MB_VERIFY(td, "make-forward");

  td.side ^= 1;
  td.hash_key ^= side_key;

  if (!known_legal &&
      td_is_square_attacked(td, get_ls1b_index((td.side == white) ? td.bitboards[k] : td.bitboards[K]), td.side)) {
    td.side ^= 1;

    if (promoted) {
      pop_bit(td.bitboards[promoted], target);
      set_bit(td.bitboards[piece], source);
    } else {
      pop_bit(td.bitboards[piece], target);
      set_bit(td.bitboards[piece], source);
    }

    if (undo.captured_piece != -1) {
      set_bit(td.bitboards[undo.captured_piece], undo.captured_square);
    }

    if (castling) {
      int rook_pc, rf, rt;
      td_castle_rook(target, rook_pc, rf, rt);
      pop_bit(td.bitboards[rook_pc], rt);
      set_bit(td.bitboards[rook_pc], rf);
    }

    td.castle = undo.old_castle;
    td.enpassant = undo.old_enpassant;
    // AA3: chiavi, 50 mosse e occupazioni dalla copia (plies_from_null non era ancora cambiato).
    std::memcpy(&td.hash_key, undo.keys, sizeof(undo.keys));
    std::memcpy(td.occupancies, undo.occ, sizeof(undo.occ));
    td_mailbox_revert(td, piece, source, target, promoted, undo.captured_piece,
                      undo.captured_square, castling);  // sito 2/3: mossa illegale
    MB_VERIFY(td, "rollback-illegale");
    nn_unmake_dirty(td);  // lo stato della rete scritto in cima non serve piu'

    return 0;
  }

  // Prefetch delle correzioni che il figlio legge per prime: struttura pedonale e pezzi minori (chiavi gia'
  // aggiornate qui sopra). td.side e' gia' quello del figlio. Dal 04/10/2026 le tabelle sono condivise fra i
  // thread e grandi (1 MB per thread), non piu' 64 KB in L2: il prefetch qui ha di nuovo dei miss da nascondere.
  // 06/10/2026 (rapporto docs/audit_8.0/N_CORREZIONI.md): le righe si anticipano con le STESSE chiavi che legge
  // corr_signal, sale di fase (CorrPhase) e di fascia delle 50 mosse (CorrFiftyStep) compresi. Prima si usavano le
  // chiavi senza sale: a opzioni spente identico, ad opzioni accese due prefetch a vuoto e nessuna riga anticipata,
  // cioe' CorrPhase e CorrFifty pagavano velocita' negli SPRT. A opzioni spente i sali valgono 0: codice equivalente.
  const U64 pf_phk = corr_phase_key(td), pf_fk = corr_fifty_key(td);
  TT_PREFETCH(&g_stats.corr[(td.pawn_key ^ pf_phk ^ pf_fk) & g_stats.corr_mask][td.side]);
  TT_PREFETCH(&g_stats.corr[(td.mm_key[0] ^ pf_phk) & g_stats.corr_mask][td.side]);
  // 04/10/2026 sera: anche i due slot dei non-pedoni, che corr_signal legge subito dopo (tabella da 1,3 MB per
  // thread: fuori da L2, ogni slot non anticipato e' un'attesa di L3 nel figlio).
  TT_PREFETCH(&g_stats.corr[(td.np_key[white] ^ pf_fk) & g_stats.corr_mask][td.side]);
  TT_PREFETCH(&g_stats.corr[(td.np_key[black] ^ pf_fk) & g_stats.corr_mask][td.side]);
  if (CorrExpertW)
    TT_PREFETCH(&g_stats.corr_x[corr_expert_key(td) & g_stats.corr_mask][td.side]);

  // La chiave della hash puo' portare la fascia delle 50 mosse (tt_key, HashFiftyStep): se differisce dalla stima si
  // richiama la riga giusta. A opzione spenta tt_key e' td.hash_key e il test e' quello di prima.
  const U64 tk = tt_key(td);
  if (tk != pf_key) { // stima sbagliata (arrocco, promozione, e.p., fascia delle 50 mosse)
    TT_PREFETCH(&hash_table[tt_base_index(tk)]);
    TT_PREFETCH(&td.eval_cache[td.hash_key & ThreadData::EVAL_CACHE_MASK]);
  }

  // ⛔ 04/10/2026 — PROVATO E TOLTO: prefetch qui, alla mossa, delle righe HalfKA che l'aggiornamento del figlio
  // usera' (pezzo che parte/arriva/catturato, due prospettive, fascia del figlio). xperf 6 giri, nodi identici:
  // cicli +0,58%, miss di L3 per nodo invariati (19,9 contro 19,8). Non anticipa nulla di utile.
#ifdef TRIUMV_VERIFY_NNSYNC
  td.nnstack->st[td.nnstack->size - 1].key = nn_piece_key(td);  // controllo di sincronia (06_nndirty.inc)
#endif

  td.plies_from_null++; // P2.2: una mossa reale in piu' dall'ultima null

  return 1;
}

// ============================================================================
// UNMAKE MOVE
// ============================================================================

static inline void td_unmake_move(ThreadData &td, int move, UndoInfo &undo) {
  PROF_GUARD(prof_make);
  nn_unmake_dirty(td);   // lo stato della rete di questa mossa esce dalla pila
  // P2.2 (plies_from_null, speculare al ++ del make riuscito): torna con la copia di AA3 qui sotto.
  td.side ^= 1;

  int source = get_move_source(move);
  int target = get_move_target(move);
  int piece = get_move_piece(move);
  int promoted = get_move_promoted(move);
  int castling = get_move_castling(move);

  if (promoted) {
    pop_bit(td.bitboards[promoted], target);
    set_bit(td.bitboards[piece], source);
  } else {
    pop_bit(td.bitboards[piece], target);
    set_bit(td.bitboards[piece], source);
  }

  // AA3 (10/10/2026): la vittima torna senza salto (senza vittima maschera nulla sulla bitboard 0).
  {
    const int cp = undo.captured_piece;
    const U64 cbit = cp != -1 ? 1ULL << (undo.captured_square & 63) : 0;
    td.bitboards[cp != -1 ? cp : 0] |= cbit;
  }

  if (castling) {
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    pop_bit(td.bitboards[rook_pc], rt);
    set_bit(td.bitboards[rook_pc], rf);
  }

  td.castle = undo.old_castle;
  td.enpassant = undo.old_enpassant;
  // AA3: chiave, chiavi parziali, 50 mosse, plies_from_null e occupazioni dalla copia della make, al posto di
  // td_occ_update e td_keys_update (stessi valori: sono quelli di prima della mossa).
  std::memcpy(&td.hash_key, undo.keys, sizeof(undo.keys));
  std::memcpy(td.occupancies, undo.occ, sizeof(undo.occ));
  td_mailbox_revert(td, piece, source, target, promoted, undo.captured_piece,
                    undo.captured_square, castling);  // sito 3/3: unmake
  MB_VERIFY(td, "unmake");
}

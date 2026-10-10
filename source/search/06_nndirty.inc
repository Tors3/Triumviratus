// search/06_nndirty.inc -- Le dirty della rete, scritte dalla make (07/10/2026, scacchiera unica v2).
// Parte di threads.cpp: NON e' un'unita' di compilazione a se'. Va inclusa solo da threads.cpp, prima di
// 07_makemove.inc.
//
// La make scrive sulla pila dell'handle della rete (NnStack, nn_dirty.h; td.nnstack) uno stato per mossa con il solo
// pezzo mosso (NnDirtyPiece), nella numerazione del motore; la unmake lo toglie. Pedoni toccati (NnDirtyPawns) e tuple
// di minaccia che cambiano (NnDirtyThreats) si calcolano solo quando si arriva a una valutazione (nn_dirty_catch_up),
// per gli stati che ne mancano: molti nodi finiscono prima di valutare (tagli della TT, scacco, potature) e non le
// pagano. La rete legge le dirty cosi' come sono e aggiorna gli accumulatori (nnue/nnue/nnue_accumulator.cpp); non ha
// una scacchiera sua.
//
// Il recupero ricava la scacchiera prima della prima mossa in sospeso da quella del thread, disfacendo le mosse in
// sospeso su copie locali (bitboard, mailbox), poi rifa' le mosse in avanti. Per ogni mossa (nn_threat_steps) la si
// scompone nei passi di una make (togliere, mettere, spostare, sostituire un pezzo) sui bitboard locali e per ogni passo
// si emettono le tuple che cambiano, sullo stato di quel passo. I pezzi delle case lette si prendono dalla mailbox
// PRIMA della mossa: le letture cadono sempre su case occupate nello stato del passo e non toccate dalla mossa (i pezzi
// lunghi, i bersagli, gli attaccanti; mai la casa del passo stesso). Lo garantisce il controllo nnperft (accumulatori
// interi confrontati col refresh a ogni valutazione).
// Misure xperf (07/10/2026, DEVELOPMENT_8.0 §28): calcolare le minacce in ogni make costava +0,9-1,2% di cicli per
// nodo rispetto alla vecchia copia pigra della scacchiera; questo recupero, con il confronto della finny table senza
// salti, fa -0,4% (mediogioco) e -0,7% (finali) rispetto alla copia.

#if defined(USE_AVX512) && !defined(TRIUMV_NO_THREATS_VECTOR_F)
  #define NN_THREATS_VECTOR
  #include <immintrin.h>
#endif

// Raggio da s1 attraverso s2 fino al bordo (s2 compresa, s1 esclusa); 0 se le case non sono allineate o coincidono.
static U64 nn_ray_pass[64][64];

// Tabelle dei blocchi da innesto (_wip graftfix, 10/10/2026; docs/audit_8.0/PASSEDREL_COSTO.md, P1/P1b). Case del
// motore (a8 = 0; il bianco promuove verso la riga 0). Definizioni: PASSED_REL_SPEC in
// nnue/nnue/features/passed_rel.cpp.
//   nn_pr_catch[c][s]  case del re NEMICO da cui il passato di c in s e' preso: Chebyshev(re, promozione) <= mosse
//                      (mosse = 5 dalla seconda traversa); bit 0 dello stato = casa del re nemico FUORI da questa.
//   nn_pr_ahead[c][s]  case davanti a s fino alla promozione compresa, s esclusa; bit 2 = nessun pezzo qui.
//   nn_pr_adj[f]       colonne adiacenti a f; bit 1 = un altro passato di c qui.
//   nn_pr_inv[c][k]    (_wip graft_passedrel2, Q2) le case s dei passati di c con k in nn_pr_catch[c][s]: una mossa del
//                      re nemico da `da` ad `a` cambia il bit 0 SOLO dei passati in nn_pr_inv[c][da] ^ nn_pr_inv[c][a].
//   nn_sq_bit[s]       1 << s, e 0 per s = NN_SQ_NONE (64): le case toccate da una mossa senza salti.
static U64 nn_pr_catch[2][64];
static U64 nn_pr_ahead[2][64];
static U64 nn_pr_adj[8];
static U64 nn_pr_inv[2][64];
static U64 nn_sq_bit[65];
// PassedState (PassedPawns v3, 10/10/2026): case da guardare di un passato di c in s. nn_ps_stop = la casa davanti
// (bianco s - 8, nero s + 8; 0 sulle traverse estreme), nn_ps_prot = le due case in diagonale dietro (bianco s + 7,
// s + 9; nero s - 7, s - 9), dove un pedone proprio lo protegge.
static U64 nn_ps_stop[2][64];
static U64 nn_ps_prot[2][64];

static void nn_graft_tables_init() {
  const U64 fileA = 0x0101010101010101ULL;
  for (int c = 0; c < 2; c++)
    for (int s = 0; s < 64; s++) {
      const int row = s >> 3, f = s & 7, promoRow = c == white ? 0 : 7;
      int moves = c == white ? row : 7 - row;
      if ((c == white && row == 6) || (c == black && row == 1))
        moves = 5;
      U64 cb = 0;
      for (int k = 0; k < 64; k++) {
        const int dr = (k >> 3) > promoRow ? (k >> 3) - promoRow : promoRow - (k >> 3);
        const int df = (k & 7) > f ? (k & 7) - f : f - (k & 7);
        if ((dr > df ? dr : df) <= moves)
          cb |= 1ULL << k;
      }
      nn_pr_catch[c][s] = cb;
      const U64 file = fileA << f;
      nn_pr_ahead[c][s] = c == white ? file & ((1ULL << (row * 8)) - 1)
                                     : (row == 7 ? 0 : file & ~((1ULL << ((row + 1) * 8)) - 1));
    }
  for (int f = 0; f < 8; f++)
    nn_pr_adj[f] = (f > 0 ? fileA << (f - 1) : 0) | (f < 7 ? fileA << (f + 1) : 0);
  for (int c = 0; c < 2; c++)
    for (int sq = 0; sq < 64; sq++) {
      nn_pr_inv[c][sq] = 0;
      for (int s = 0; s < 64; s++)
        if ((nn_pr_catch[c][s] >> sq) & 1)
          nn_pr_inv[c][sq] |= 1ULL << s;
    }
  for (int s = 0; s < 64; s++)
    nn_sq_bit[s] = 1ULL << s;
  nn_sq_bit[64] = 0;
  for (int s = 0; s < 64; s++) {  // PassedState
    const int row = s >> 3, f = s & 7;
    nn_ps_stop[white][s] = row > 0 ? 1ULL << (s - 8) : 0;
    nn_ps_stop[black][s] = row < 7 ? 1ULL << (s + 8) : 0;
    U64 pw = 0, pb = 0;
    if (row < 7) {
      if (f > 0) pw |= 1ULL << (s + 7);
      if (f < 7) pw |= 1ULL << (s + 9);
    }
    if (row > 0) {
      if (f > 0) pb |= 1ULL << (s - 9);
      if (f < 7) pb |= 1ULL << (s - 7);
    }
    nn_ps_prot[white][s] = pw;
    nn_ps_prot[black][s] = pb;
  }
}

static void nn_dirty_init() {
  for (int s1 = 0; s1 < 64; s1++)
    for (int s2 = 0; s2 < 64; s2++) {
      const U64 b1 = 1ULL << s1, b2 = 1ULL << s2;
      U64 r = 0;
      if (get_rook_attacks(s1, 0) & b2)
        r = get_rook_attacks(s1, 0) & (get_rook_attacks(s2, b1) | b2);
      else if (get_bishop_attacks(s1, 0) & b2)
        r = get_bishop_attacks(s1, 0) & (get_bishop_attacks(s2, b1) | b2);
      nn_ray_pass[s1][s2] = r;
    }
  nn_graft_tables_init();
}

// Fascia di materiale della rete (HalfKA a esperti) per numero di pezzi: <= 9, 10-15, 16-23, >= 24 (half_ka_v2_hm.h).
static constexpr unsigned char nn_phase_of_count[33] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2,
                                                        2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3};

// Scacchiera di lavoro dei passi: bitboard per tipo (P..K, entrambi i colori), per colore e occupazione.
struct NnTB {
  U64 type[6];
  U64 color[2];
  U64 occ;
};

static inline void nn_tb_toggle(NnTB &tb, int pc, U64 b) {
  tb.type[pc >= 6 ? pc - 6 : pc] ^= b;
  tb.color[pc >= 6] ^= b;
  tb.occ ^= b;
}

// Lettura del pezzo su una casa (codice 0..11, -1 = vuota) dalla mailbox locale del recupero, che e' sempre quella
// prima della mossa in esame.
struct NnLookMailbox {
  const int *mb;
  int operator()(int s) const { return mb[s]; }
#ifdef NN_THREATS_VECTOR
  __m512i chunk(int c) const { return _mm512_loadu_si512(mb + 16 * c); }
#endif
};

static inline void nn_add_threat(NnDirtyThreats *dts, bool add, int pc, int tpc, int pcSq, int tSq) {
  dts->list[dts->n++] = nn_threat(pc, tpc, pcSq, tSq, add);
}

// Un pezzo lungo che minaccia una donna e' una feature solo se e' a sua volta una donna (le coppie alfiere/torre ->
// donna sono escluse dalla tabella delle feature).
static inline bool nn_can_slider_threat(int pc, int slider) {
  return pc % 6 != NN_QUEEN || slider % 6 == NN_QUEEN;
}

#ifdef NN_THREATS_VECTOR
// Emissione vettoriale (AVX-512 F/BW): ogni gruppo di 16 case si compatta con vpcompressd sui suoi 16 bit della
// maschera, con la casa e il pezzo gia' al loro posto nella tupla. Le scritture da 16 voci oltre il conteggio cadono
// nelle 16 voci di scorta della lista (nn_dirty.h). Quattro gruppi fissi: nessun salto dipende dalla maschera. Nella
// nostra numerazione il gruppo c sono le case 16c..16c+15, contigue anche nella mailbox.
template <int SqShift, int PcShift, typename Look>
static inline void nn_write_multi(const Look &look, U64 mask, std::uint32_t tmpl, NnDirtyThreats *dts) {
  std::uint32_t *write = dts->list + dts->n;
  dts->n += count_bits(mask);
  alignas(64) static constexpr std::uint32_t lane[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const __m512i lanev = _mm512_load_si512(lane);
  const __m512i template_v = _mm512_set1_epi32(int(tmpl));
  for (int c = 0; c < 4; c++) {
    const __mmask16 m = __mmask16(mask >> (16 * c));
    const __m512i pieces = _mm512_slli_epi32(look.chunk(c), PcShift);
    const __m512i squares = _mm512_slli_epi32(_mm512_add_epi32(lanev, _mm512_set1_epi32(16 * c)), SqShift);
    const __m512i dirties = _mm512_ternarylogic_epi32(template_v, squares, pieces, 254 /* A | B | C */);
    _mm512_storeu_si512(write, _mm512_maskz_compress_epi32(m, dirties));
    write += count_bits((U64)m);
  }
}
#endif

static inline U64 nn_piece_attacks(int pc, int s, U64 occupied) {
  switch (pc >= 6 ? pc - 6 : pc) {
  case NN_PAWN: return pawn_attacks[pc >= 6][s];
  case NN_KNIGHT: return knight_attacks[s];
  case NN_BISHOP: return get_bishop_attacks(s, occupied);
  case NN_ROOK: return get_rook_attacks(s, occupied);
  default: return get_bishop_attacks(s, occupied) | get_rook_attacks(s, occupied);  // NN_QUEEN
  }
}

// Le tuple di minaccia che cambiano quando il pezzo pc compare in s (put) o ne sparisce (!put), sullo stato `tb` di
// quel momento. ComputeRay: minacce scoperte dei pezzi lunghi lungo i raggi che passano per s; noRaysContaining
// salta un raggio scoperto che contiene tutta la mossa (da|a).
template <bool ComputeRay, typename Look>
static inline void nn_piece_threats(const NnTB &tb, const Look &look, int pc, bool put, int s, NnDirtyThreats *dts,
                                    U64 noRaysContaining = ~0ULL) {
  const int pt = pc >= 6 ? pc - 6 : pc;
  const U64 occupied = tb.occ;
  const U64 rookQueens = tb.type[NN_ROOK] | tb.type[NN_QUEEN];
  const U64 bishopQueens = tb.type[NN_BISHOP] | tb.type[NN_QUEEN];
  const U64 rAttacks = get_rook_attacks(s, occupied);
  const U64 bAttacks = get_bishop_attacks(s, occupied);
  const U64 occupiedNoK = occupied ^ tb.type[NN_KING];

  // Filtro delle coppie non valide (cadrebbero comunque su feature morte).
  U64 sliders = (rookQueens & rAttacks) | (bishopQueens & bAttacks);
  const U64 directSliders = pt == NN_QUEEN ? sliders & tb.type[NN_QUEEN] : sliders;

  auto process_sliders = [&](bool addDirectAttacks) {
    while (sliders) {
      const int sliderSq = get_ls1b_index(sliders);
      pop_lsb_bb(sliders);
      const int slider = look(sliderSq);
      const U64 ray = nn_ray_pass[sliderSq][s];
      const U64 discovered = ray & (rAttacks | bAttacks) & occupiedNoK;
      if (discovered && (ray & noRaysContaining) != noRaysContaining) {
        const int threatenedSq = get_ls1b_index(discovered);
        const int threatenedPc = look(threatenedSq);
        if (nn_can_slider_threat(threatenedPc, slider))
          nn_add_threat(dts, !put, slider, threatenedPc, sliderSq, threatenedSq);
      }
      if (addDirectAttacks && nn_can_slider_threat(pc, slider))
        nn_add_threat(dts, put, slider, pc, sliderSq, s);
    }
  };

  if (pt == NN_KING) {
    if constexpr (ComputeRay)
      process_sliders(false);
    return;
  }

  // Le relazioni pedone -> pedone non sono nel feature set: un pedone minaccia solo cavalli e torri, e i pedoni
  // contano come attaccanti solo di cavalli e torri.
  U64 threatened = nn_piece_attacks(pc, s, occupied) & occupiedNoK;
  U64 incoming_threats = knight_attacks[s] & tb.type[NN_KNIGHT];
  if (pt == NN_KNIGHT || pt == NN_ROOK) {
    const U64 wp = tb.type[NN_PAWN] & tb.color[white], bp = tb.type[NN_PAWN] & tb.color[black];
    incoming_threats |= (pawn_attacks[white][s] & bp) | (pawn_attacks[black][s] & wp);
  }

  switch (pt) {
  case NN_PAWN: threatened &= tb.type[NN_KNIGHT] | tb.type[NN_ROOK]; break;
  case NN_BISHOP:
  case NN_ROOK:
    threatened &= tb.type[NN_PAWN] | tb.type[NN_KNIGHT] | tb.type[NN_BISHOP] | tb.type[NN_ROOK];
    break;
  default: break;  // gia' occupiedNoK
  }

#ifdef NN_THREATS_VECTOR
  // I pezzi lunghi non sono ancora consumati qui: i loro attacchi DIRETTI entrano fra gli attaccanti, e
  // process_sliders si chiama solo per le minacce scoperte.
  nn_write_multi<NN_THR_TSQ, NN_THR_TPC>(look, threatened, nn_threat(pc, 0, s, 0, put), dts);
  nn_write_multi<NN_THR_PCSQ, NN_THR_PC>(look, directSliders | incoming_threats, nn_threat(0, pc, 0, s, put), dts);
#else
  while (threatened) {
    const int threatenedSq = get_ls1b_index(threatened);
    pop_lsb_bb(threatened);
    nn_add_threat(dts, put, pc, look(threatenedSq), s, threatenedSq);
  }
#endif

  if constexpr (ComputeRay) {
#ifdef NN_THREATS_VECTOR
    process_sliders(false);
#else
    process_sliders(true);
#endif
  } else {
    incoming_threats |= directSliders;
  }

#ifndef NN_THREATS_VECTOR
  while (incoming_threats) {
    const int srcSq = get_ls1b_index(incoming_threats);
    pop_lsb_bb(incoming_threats);
    nn_add_threat(dts, put, look(srcSq), pc, srcSq, s);
  }
#endif
}

// I passi di una make, ciascuno con le sue tuple. Togliere: le minacce si calcolano prima, col pezzo ancora li'.
// Mettere: dopo. Spostare: prima e dopo, saltando i raggi scoperti che contengono da|a. Sostituire (cattura): il
// vecchio pezzo esce e il nuovo entra con la casa sempre occupata, senza raggi scoperti.
template <typename Look>
static inline void nn_step_remove(NnTB &tb, const Look &look, int pc, int s, NnDirtyThreats *dts) {
  nn_piece_threats<true>(tb, look, pc, false, s, dts);
  nn_tb_toggle(tb, pc, 1ULL << s);
}
template <typename Look>
static inline void nn_step_put(NnTB &tb, const Look &look, int pc, int s, NnDirtyThreats *dts) {
  nn_tb_toggle(tb, pc, 1ULL << s);
  nn_piece_threats<true>(tb, look, pc, true, s, dts);
}
template <typename Look>
static inline void nn_step_move(NnTB &tb, const Look &look, int pc, int from, int to, NnDirtyThreats *dts) {
  const U64 fromTo = (1ULL << from) | (1ULL << to);
  nn_piece_threats<true>(tb, look, pc, false, from, dts, fromTo);
  nn_tb_toggle(tb, pc, fromTo);
  nn_piece_threats<true>(tb, look, pc, true, to, dts, fromTo);
}
template <typename Look>
static inline void nn_step_swap(NnTB &tb, const Look &look, int oldPc, int newPc, int s, NnDirtyThreats *dts) {
  nn_tb_toggle(tb, oldPc, 1ULL << s);
  nn_piece_threats<false>(tb, look, oldPc, false, s, dts);
  nn_tb_toggle(tb, newPc, 1ULL << s);
  nn_piece_threats<false>(tb, look, newPc, true, s, dts);
}

// Tutte le tuple di una mossa, dalla scacchiera prima della mossa (tb, che esce allo stato dopo la mossa).
template <typename Look>
static inline void nn_threat_steps(NnTB &tb, const Look &look, const NnDirtyPiece &dp, NnDirtyThreats *dts) {
  dts->n = 0;
  const int pc = dp.pc, from = dp.from;
  if (dp.to != NN_SQ_NONE && dp.add_sq != NN_SQ_NONE) {
    // Arrocco: prima si tolgono re e torre, poi si rimettono (nel Chess960 le case si sovrappongono).
    nn_step_remove(tb, look, pc, from, dts);
    nn_step_remove(tb, look, dp.remove_pc, dp.remove_sq, dts);
    nn_step_put(tb, look, pc, dp.to, dts);
    nn_step_put(tb, look, dp.add_pc, dp.add_sq, dts);
    return;
  }
  const bool promo = dp.add_sq != NN_SQ_NONE;
  const int to = promo ? dp.add_sq : dp.to;
  const int arriving = promo ? dp.add_pc : pc;
  if (dp.remove_sq != NN_SQ_NONE) {
    if (dp.remove_sq != to) {  // en passant: prima il pedone catturato, poi il pedone che muove
      nn_step_remove(tb, look, dp.remove_pc, dp.remove_sq, dts);
      nn_step_move(tb, look, pc, from, to, dts);
    } else {
      nn_step_remove(tb, look, pc, from, dts);
      nn_step_swap(tb, look, dp.remove_pc, arriving, to, dts);
    }
  } else if (promo) {
    nn_step_remove(tb, look, pc, from, dts);
    nn_step_put(tb, look, arriving, to, dts);
  } else
    nn_step_move(tb, look, pc, from, to, dts);
}

// I pedoni che la mossa toglie e aggiunge, coi pedoni di prima (blocchi PawnPair e PassedPawns). L'arrocco non tocca
// pedoni: la torre e' in remove_pc, non un pedone.
static inline void nn_fill_pawns(NnDirtyPawns &dpw, const NnDirtyPiece &dp, U64 whitePawns, U64 blackPawns) {
  const bool moverIsPawn = dp.pc == P || dp.pc == p;
  const bool victimIsPawn = dp.remove_sq != NN_SQ_NONE && (dp.remove_pc == P || dp.remove_pc == p);
  dpw.nRemoved = 0;
  dpw.addedSq = NN_SQ_NONE;
  dpw.any = moverIsPawn || victimIsPawn;
  if (!dpw.any)
    return;
  dpw.before[white] = whitePawns;
  dpw.before[black] = blackPawns;
  if (moverIsPawn) {
    dpw.removedSq[dpw.nRemoved] = dp.from;
    dpw.removedC[dpw.nRemoved] = dp.pc >= 6;
    dpw.nRemoved++;
    if (dp.to != NN_SQ_NONE) {  // la promozione non rimette un pedone
      dpw.addedSq = dp.to;
      dpw.addedC = dp.pc >= 6;
    }
  }
  if (victimIsPawn) {
    dpw.removedSq[dpw.nRemoved] = dp.remove_sq;  // en passant: la casa del pedone, non `to`
    dpw.removedC[dpw.nRemoved] = dp.remove_pc >= 6;
    dpw.nRemoved++;
  }
}

// Chiave della sola disposizione dei pezzi (pedoni + non pedoni dei due colori): non cambia con la mossa nulla, il
// lato al tratto, l'en passant e gli arrocchi. Serve al controllo di sincronia (-DTRIUMV_VERIFY_NNSYNC).
static inline U64 nn_piece_key(const ThreadData &td) { return td.pawn_key ^ td.np_key[white] ^ td.np_key[black]; }

// Make: un nuovo stato sulla pila, scritto sulla scacchiera PRIMA della mossa (la make non ha ancora toccato nulla).
// cap = pezzo catturato (-1 nessuno), capSq = sua casa; rookPc = -1 se non e' un arrocco, altrimenti la torre e le sue
// case rf -> rt.
static inline void nn_make_dirty(ThreadData &td, int piece, int source, int target, int promoted, int cap, int capSq,
                                 int rookPc, int rf, int rt) {
  NnStack &S = *td.nnstack;
  NnState &st = S.st[S.size++];
  st.computed[white] = st.computed[black] = 0;
  // Blocchi da innesto (_wip graftfix, P2): posizione nella pila e diff vuota, nella stessa linea di `computed`.
  st.idx = (std::uint8_t)(S.size - 1);
  st.graftRem = st.graftAdd = 0;

  NnDirtyPiece &dp = st.dp;
  dp.pc = (std::uint8_t)piece;
  dp.from = (std::uint8_t)source;
  if (rookPc >= 0) {
    dp.to = (std::uint8_t)target;
    dp.remove_pc = dp.add_pc = (std::uint8_t)rookPc;
    dp.remove_sq = (std::uint8_t)rf;
    dp.add_sq = (std::uint8_t)rt;
  } else {
    dp.to = (std::uint8_t)(promoted ? NN_SQ_NONE : target);
    dp.remove_pc = (std::uint8_t)(cap & 15);
    dp.remove_sq = (std::uint8_t)(cap != -1 ? capSq : NN_SQ_NONE);
    dp.add_pc = (std::uint8_t)promoted;
    dp.add_sq = (std::uint8_t)(promoted ? target : NN_SQ_NONE);
  }
  const int pieces = count_bits(td.occupancies[both]);
  dp.phase = nn_phase_of_count[pieces - (cap != -1)];
  dp.phaseChanged = nn_phase_of_count[pieces] != dp.phase;
  // Minacce e pedoni: nn_dirty_catch_up, solo se si arriva a una valutazione.
}

// La mossa appena registrata non si fa (illegale) o si disfa (unmake): lo stato esce dalla pila.
static inline void nn_unmake_dirty(ThreadData &td) {
  NnStack &S = *td.nnstack;
  --S.size;
  if (S.ready > S.size)
    S.ready = S.size;
}

// Disfare e rifare una mossa sulle copie locali del recupero. Qui i salti restano: le mosse in sospeso sono quasi sempre una e i rami sono ben previsti. Provata una versione senza
// salti (casa 64 = bitboard 0, XOR sempre, mailbox locale di 65 caselle con scritture sempre eseguite): meno istruzioni
// e meno salti mal previsti, ma +1,2 punti di cicli per nodo in mediogioco (xperf 07/10): letture di tabelle e scritture
// subito rilette a blocchi di 64 byte dal codice vettoriale costano piu' di rami prevedibili.
static inline void nn_dp_toggle(NnTB &tb, const NnDirtyPiece &dp) {  // XOR: vale sia avanti sia indietro
  nn_tb_toggle(tb, dp.pc, 1ULL << dp.from);
  if (dp.to != NN_SQ_NONE) nn_tb_toggle(tb, dp.pc, 1ULL << dp.to);
  if (dp.remove_sq != NN_SQ_NONE) nn_tb_toggle(tb, dp.remove_pc, 1ULL << dp.remove_sq);
  if (dp.add_sq != NN_SQ_NONE) nn_tb_toggle(tb, dp.add_pc, 1ULL << dp.add_sq);
}
// Prima si vuotano le case d'arrivo, poi si rimettono i pezzi di partenza: nell'arrocco Chess960 le case si
// sovrappongono.
static inline void nn_mb_undo(int *mb, const NnDirtyPiece &dp) {
  if (dp.to != NN_SQ_NONE) mb[dp.to] = -1;
  if (dp.add_sq != NN_SQ_NONE) mb[dp.add_sq] = -1;
  mb[dp.from] = dp.pc;
  if (dp.remove_sq != NN_SQ_NONE) mb[dp.remove_sq] = dp.remove_pc;
}
static inline void nn_mb_redo(int *mb, const NnDirtyPiece &dp) {
  mb[dp.from] = -1;
  if (dp.remove_sq != NN_SQ_NONE) mb[dp.remove_sq] = -1;
  if (dp.to != NN_SQ_NONE) mb[dp.to] = dp.pc;
  if (dp.add_sq != NN_SQ_NONE) mb[dp.add_sq] = dp.add_pc;
}

// Minacce e pedoni degli stati che ne mancano ([ready, size)), prima di ogni valutazione: la scacchiera prima della
// prima mossa in sospeso si ricava da quella del thread disfacendo le mosse in sospeso, poi le mosse si rifanno in
// avanti. Di solito il padre e' gia' stato valutato e la mossa in sospeso e' una sola.
// Voci del blocco da innesto PassedRel (09/10/2026; gli altri blocchi tolti il 10/10/2026) della scacchiera locale del
// recupero: stessa funzione della rete (PawnGrafts::entries_of, un'altra unita'), sui bitboard di pedoni e re e
// sull'occupazione. E' il RIFERIMENTO: dal _wip graftfix (10/10/2026) il recupero la usa solo alla radice e nel
// controllo -DTRIUMV_VERIFY_GRAFT; piu' sotto c'e' la versione in linea (nn_prel_step). Qui arriva solo
// mask & NN_GRAFT_LIST_MASK, cioe' PassedRel o PassedState (al piu' 16 voci, out con NN_GRAFT_MAX posti).
static inline int nn_graft_tb(const NnTB &tb, unsigned mask, unsigned short *out) {
  unsigned long long bb12[12] = {};
  for (int t = 0; t < 6; t++) {  // PassedState (10/10/2026) legge tutti i pezzi; PassedRel pedoni e re
    bb12[t] = tb.type[t] & tb.color[white];
    bb12[t + NN_BLACK] = tb.type[t] & tb.color[black];
  }
  return nn_graft_entries(mask, bb12, tb.occ, out);
}

// Le case davanti ai passati (fino alla promozione) dalle voci di PassedRel (blocco 0: colore << 9 | casa << 3 |
// stato, case del motore a8 = 0; il bianco promuove verso la riga 0): un pezzo che vi entra o ne esce puo' cambiare
// la "strada libera". Comprende anche le case dei passati stessi.
static inline U64 nn_graft_passer_span(const unsigned short *e, int n) {
  U64 span = 0;
  for (int i = 0; i < n && (e[i] >> 12) == 0; i++) {
    const int c = (e[i] >> 9) & 1, s = (e[i] >> 3) & 63, row = s >> 3;
    const U64 file = 0x0101010101010101ULL << (s & 7);
    span |= c == 0 ? file & ((2ULL << s) - 1) : file & ~((1ULL << (row * 8)) - 1);
  }
  return span;
}

// Le voci che escono e che entrano, dalle due liste crescenti (merge lineare), in coda a rem[r..] e add[d..].
static inline void nn_graft_merge(const unsigned short *a, int na, const unsigned short *b, int nb, unsigned short *rem,
                                  int &r, unsigned short *add, int &d) {
  int i = 0, j = 0;
  while (i < na || j < nb) {
    if (j >= nb || (i < na && a[i] < b[j]))
      rem[r++] = a[i++];
    else if (i >= na || b[j] < a[i])
      add[d++] = b[j++];
    else
      i++, j++;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// _wip graftfix (10/10/2026; docs/audit_8.0/PASSEDREL_COSTO.md, P1, P1b e la parte KingFiles). Il recupero aggiorna
// le voci blocco per blocco, ricalcolando solo cio' che la mossa puo' cambiare, con funzioni in linea in questa unita'
// (la versione di riferimento, PawnGrafts::entries_of, resta per la radice, per Space/LockedPawns e per i controlli).
// ---------------------------------------------------------------------------------------------------------------

// (nn_bswap64 tolta il 10/10/2026, _wip graft_kingfiles: la usava solo nn_kf_owner, sostituita dalla firma.)

// Passati del colore c: stessa funzione di PassedPawns::passers (nnue/nnue/features/passed_pawns.cpp, che la rete non
// puo' esportare qui: motore e rete non condividono intestazioni). Un pedone e' passato se non cade nell'"ombra" (case
// strettamente dietro, viste da c) dei pedoni avversari allargati alle colonne vicine, ne' in quella dei propri sulla
// stessa colonna. Nella nostra numerazione il bianco avanza verso gli indici piu' bassi: la sua ombra sale di indice.
static inline U64 nn_passers(int c, U64 own, U64 opp) {
  const U64 fileA = 0x0101010101010101ULL, fileH = fileA << 7;
  const auto back = [c](U64 b, int n) { return c == white ? b << n : b >> n; };
  U64 oppShadow = back(opp | ((opp & ~fileH) << 1) | ((opp & ~fileA) >> 1), 8);
  oppShadow |= back(oppShadow, 8);
  oppShadow |= back(oppShadow, 16);
  oppShadow |= back(oppShadow, 32);
  U64 ownShadow = back(own, 8);
  ownShadow |= back(ownShadow, 8);
  ownShadow |= back(ownShadow, 16);
  ownShadow |= back(ownShadow, 32);
  return own & ~oppShadow & ~ownShadow;
}

// PassedRel da capo (P1b), voci del blocco 0 (colore << 9 | casa << 3 | stato) in ordine crescente, con la span
// (case davanti ai passati, passati compresi) come sottoprodotto. Stesse voci di PassedRel::entries_of riordinate da
// PawnGrafts::entries_of; lo controlla -DTRIUMV_VERIFY_GRAFT.
// _wip graft_passedrel2: i passati (pw, pb) si passano gia' calcolati (nn_prel_step li ha in mano).
static inline int nn_prel_entries_p(const NnTB &tb, U64 pw, U64 pb, unsigned short *out, U64 &span) {
  const U64 P[2] = {pw, pb};
  int n = 0;
  span = 0;
  for (int c = 0; c < 2; c++) {
    if (!P[c])
      continue;
    const int ek = get_ls1b_index(tb.type[NN_KING] & tb.color[c ^ 1]);
    for (U64 b = P[c]; b; b &= b - 1) {
      const int s = get_ls1b_index(b);
      const U64 ahead = nn_pr_ahead[c][s];
      span |= ahead | (1ULL << s);
      const unsigned st = unsigned(!((nn_pr_catch[c][s] >> ek) & 1)) | unsigned((nn_pr_adj[s & 7] & P[c]) != 0) << 1 |
                          unsigned(!(ahead & tb.occ)) << 2;
      out[n++] = (unsigned short)(c << 9 | s << 3 | st);
    }
  }
  return n;
}
// (nn_prel_entries e nn_prel_restate, usate solo dal recupero generico nn_graft_step: tolte il 10/10/2026 con KingFiles.)

// KingFiles e KingFilesQ (firma dei re, nn_kf_*) e il recupero generico nn_graft_step: tolti il 10/10/2026 con i
// blocchi (non davano Elo; STATO_PROGETTO, TEST APERTI). Codice in _backup/graft_engine_removed_2026-10-10/
// engine_removed_kingfiles/06_nndirty.inc. Nelle liste della pila resta solo PassedRel: un solo percorso, nn_prel_step.

// ---------------------------------------------------------------------------------------------------------------
// _wip graft_passedrel2 (10/10/2026; docs/audit_8.0/GRAFT_PASSEDREL_COSTO2.md, Q1-Q3). La sola PassedRel
// (nn_graft_mask == PASSED_REL, il caso delle reti in prova) ha un percorso suo. La misura del giro precedente (xperf,
// rete a pesi zero, +2,57% di cicli in mediogioco) pagava in nn_graft_step, per OGNI stato: lettura di graftCnt, i
// conti di moved, e a ogni evento di pedone (30-35% degli stati) due passers(), il merge, la scrittura della lista e
// dei conteggi nella pila anche quando nulla cambiava. Qui la decisione "niente da fare" si prende su due bitboard
// tenuti in registro per tutto il recupero (span e passati della lista valida):
//   - mossa senza pedoni e nessun passato: nulla (mediogioco: ~57% delle posizioni del bench SF senza passati);
//   - evento di pedone senza passati prima ne' dopo: due passers() e nulla;
//   - stessi passati e nessuna casa davanti toccata: nulla;
//   - mossa del re nemico che non attraversa il bordo di un quadrato (nn_pr_inv) e non tocca la span: nulla;
//   - altrimenti, fuori linea: bit 0 e 2 (nn_prel_restate_out) o da capo con merge (nn_prel_rebuild).
// Nessuna scrittura nella pila se la lista non cambia (si lavora su vettori locali). graftCnt non si tiene: con la
// sola PassedRel tutte le voci sono del blocco 0 (graftN).
// ---------------------------------------------------------------------------------------------------------------

// Lista dello stato i e diff dalle voci nuove (nb, m), tolte e aggiunte; aggiorna span e passati della lista.
static inline void nn_prel_commit(NnStack &S, int i, NnState &st, const unsigned short *nb, int m,
                                  const unsigned short *rem, int r, const unsigned short *add, int d, U64 span,
                                  U64 pass) {
  unsigned short *B = S.graftList[i];
  for (int k = 0; k < m; k++)
    B[k] = nb[k];
  std::uint16_t *e = S.graft[i].e;
  for (int k = 0; k < r; k++)
    e[k] = rem[k];
  for (int k = 0; k < d; k++)
    e[r + k] = add[k];
  st.graftRem = (std::uint8_t)r;
  st.graftAdd = (std::uint8_t)d;
  S.graftN[i] = (std::uint8_t)m;
  S.graftSpan[i] = span;
  S.graftPass[i] = pass;
  NSTAT(GRAFT_CHG);
}

// L'insieme dei passati (o il pezzo su una loro casa) e' cambiato: voci da capo (P1b) e merge con la lista di src.
__attribute__((noinline)) static bool nn_prel_rebuild(NnStack &S, int src, int i, NnState &st, const NnTB &tb,
                                                      U64 pw, U64 pb, U64 &span, U64 &pass) {
  NSTAT(GRAFT_REBUILD);
  unsigned short nb[16], rem[16], add[16];
  U64 nspan;
  const int m = nn_prel_entries_p(tb, pw, pb, nb, nspan);
  int r = 0, d = 0;
  nn_graft_merge(S.graftList[src], S.graftN[src], nb, m, rem, r, add, d);
  if (r + d == 0)  // stesse voci: stessi passati, la lista di src vale anche per i
    return false;
  span = nspan;
  pass = pw | pb;
  nn_prel_commit(S, i, st, nb, m, rem, r, add, d, span, pass);
  return true;
}

// Stessi passati e stesso bit 1: si riscrivono i bit 0 (re nemico) e 2 (strada libera) di ogni voce. Le voci restano
// nello stesso ordine, quindi tolte e aggiunte escono crescenti, come dal merge. Nulla si scrive se nessuna cambia
// (una casa davanti toccata non cambia per forza la "strada libera").
__attribute__((noinline)) static bool nn_prel_restate_out(NnStack &S, int src, int i, NnState &st, const NnTB &tb,
                                                          U64 span, U64 pass) {
  NSTAT(GRAFT_RESTATE);
  const unsigned short *a = S.graftList[src];
  const int n = S.graftN[src];
  const int ekOf[2] = {get_ls1b_index(tb.type[NN_KING] & tb.color[black]),   // re nemico dei passati bianchi
                       get_ls1b_index(tb.type[NN_KING] & tb.color[white])};  // e dei neri
  unsigned short nb[16], rem[16], add[16];
  int r = 0;
  for (int j = 0; j < n; j++) {
    const unsigned e = a[j], c = (e >> 9) & 1, s = (e >> 3) & 63;
    const unsigned stt = (e & 2u) | unsigned(!((nn_pr_catch[c][s] >> ekOf[c]) & 1)) |
                         unsigned(!(nn_pr_ahead[c][s] & tb.occ)) << 2;
    const unsigned short ne = (unsigned short)((e & ~7u) | stt);
    nb[j] = ne;
    if (ne != e) {
      rem[r] = (unsigned short)e;
      add[r] = ne;
      r++;
    }
  }
  if (!r)
    return false;
  nn_prel_commit(S, i, st, nb, n, rem, r, add, r, span, pass);
  return true;
}

// Case toccate dalla mossa (partenza, arrivo, catturato, pezzo che arriva), senza salti.
static inline U64 nn_dp_squares(const NnDirtyPiece &dp) {
  return nn_sq_bit[dp.from] | nn_sq_bit[dp.to] | nn_sq_bit[dp.remove_sq] | nn_sq_bit[dp.add_sq];
}

// Un passo del recupero per la sola PassedRel. span e pass (lista di src) restano in registro nel chiamante e si
// aggiornano se la lista cambia. Restituisce true se la lista cambia (nuova lista, diff e conteggi scritti).
// pw, pb = passati bianchi e neri dopo la mossa (R4: li calcola il recupero per PassedPawns v1, validi con pawns.any).
static inline bool nn_prel_step(NnStack &S, int src, int i, NnState &st, const NnTB &tb, U64 &span, U64 &pass,
                                U64 pw, U64 pb) {
  NSTAT(GRAFT_ST);
  const NnDirtyPiece &dp = st.dp;
  if (st.pawns.any) {
    if (!(pw | pb | pass))
      return false;  // nessun passato prima ne' dopo: il caso comune in mediogioco
    NSTAT(GRAFT_PASS);
    const U64 moved = nn_dp_squares(dp);
    // Insieme diverso, o una casa di passato toccata (un pedone che ne cattura un altro sulla stessa casa lascia
    // l'insieme uguale ma cambia il colore della voce): da capo. Altrimenti i passati e i loro colori sono gli
    // stessi, il re non si e' mosso, e cambia al piu' il bit 2 dei passati con una casa davanti toccata.
    if ((pw | pb) != pass || (moved & pass))
      return nn_prel_rebuild(S, src, i, st, tb, pw, pb, span, pass);
    // 10/10/2026: un re che cattura un pedone e' un evento di pedone ma sposta il re nemico dei passati dell'altro
    // colore: il bit 0 va riscritto anche se la span non e' toccata (trovato con -DTRIUMV_VERIFY_GRAFT, Kxp 33->26).
    if (!(span & moved) && dp.pc % 6 != NN_KING)
      return false;
    return nn_prel_restate_out(S, src, i, st, tb, span, pass);
  }
  if (!pass)
    return false;  // niente passati e la mossa non tocca pedoni
  NSTAT(GRAFT_PASS);
  const U64 moved = nn_dp_squares(dp);
  U64 chg0 = 0;
  if (dp.pc == NN_KING || dp.pc == NN_KING + NN_BLACK) {
    // Il re che si muove e' il re NEMICO dei passati dell'altro colore: il loro bit 0 cambia solo se il re attraversa
    // il bordo del quadrato (arrocco compreso: dp.to e' la casa d'arrivo del re, la torre e' in moved).
    const int c = dp.pc == NN_KING ? black : white;
    chg0 = pass & tb.color[c] & (nn_pr_inv[c][dp.from] ^ nn_pr_inv[c][dp.to]);
  }
  if (!chg0 && !(span & moved))
    return false;
  return nn_prel_restate_out(S, src, i, st, tb, span, pass);
}

// ---------------------------------------------------------------------------------------------------------------
// PassedState (PassedPawns v3, 10/10/2026; docs/audit_8.0/PASSER_BLOCK_STUDIO.md §C.4, definizioni in
// nnue/nnue/features/passed_state.h). Stessa struttura di nn_prel_step: liste della pila, diff uguali; cambiano le
// voci (colore << 13 | casa << 7 | stato) e le guardie. graftPass = passati; graftSpan non serve (0 per le liste di
// PassedState). Cosa cambia lo stato di un passato:
//   - l'insieme dei passati, e quindi conn (eventi di pedone)                    -> da capo con merge;
//   - stop, un pezzo entra o esce dalla casa davanti (ogni mossa)                -> sul posto;
//   - prot, un pedone entra o esce da una casa di protezione (eventi di pedone)  -> sul posto;
//   - mu, classe dei pezzi dell'avversario (cattura di un pezzo, promozione)      -> sul posto (classi in registro);
//   - mu 0 / 4, re nemico che attraversa il bordo del quadrato, solo se il suo lato ha solo re e pedoni: anche nel
//     ramo degli eventi di pedone (il re che cattura un pedone; correzione dello schizzo dell'agente).
// Una mossa del re lontana dalle case davanti dei passati non fa nulla salvo nei finali di soli pedoni: e' qui il
// risparmio rispetto a PassedRel.
// O2 (_wip pst_opt, 10/10/2026; docs/audit_8.0/PASSEDSTATE_COSTO.md §2): recupero snello, stesse voci e stesse diff.
//   - classi pigre: si tengono (cls) solo finche' la lista ha passati; nessun calcolo per chiamata ne' per stato senza
//     passati (prima: classi a ogni recupero e a ogni cattura di un pezzo, anche senza passati);
//   - guardie divise e lati rilevanti: dai passati (pw, pb) e dalle case toccate si ricava con spostamenti la maschera
//     `todo` dei passati il cui stato puo' cambiare (casa davanti toccata; casa di protezione toccata, solo con eventi
//     di pedone; classe dell'avversario cambiata, solo per il colore che ha passati; re nemico che attraversa il
//     quadrato). Un pezzo che passa per una casa di protezione o una cattura del lato senza passati avversari non fa
//     piu' nulla;
//   - ricalcolo sul posto senza merge (nn_pst_restate_out, gemello di nn_prel_restate_out) quando l'insieme dei
//     passati non cambia: solo le voci in `todo`, che restano nello stesso ordine (tolte e aggiunte crescenti, come
//     dal merge). Il rebuild con merge resta per l'insieme dei passati cambiato (o una loro casa toccata).
// ---------------------------------------------------------------------------------------------------------------

// Classi dei pezzi dei due lati (bianco | nero << 2): 0 nessuno, 1 solo minori, 2 torri senza donne, 3 donne.
static inline unsigned nn_pst_classes(const NnTB &tb) {
  unsigned m = 0;
  for (int c = 0; c < 2; c++) {
    const U64 col = tb.color[c];
    const unsigned k = (tb.type[NN_QUEEN] & col)                         ? 3u
                       : (tb.type[NN_ROOK] & col)                        ? 2u
                       : ((tb.type[NN_KNIGHT] | tb.type[NN_BISHOP]) & col) ? 1u
                                                                         : 0u;
    m |= k << (2 * c);
  }
  return m;
}

// Stato del passato s di colore c (stessa formula di PassedState::entries_of). mb = mailbox dopo la mossa, own =
// passati di c.
static inline unsigned nn_pst_state(const NnTB &tb, const int *mb, int c, int s, U64 own, unsigned cls) {
  const U64 stopB = nn_ps_stop[c][s];
  unsigned stop = 0;
  if (stopB & tb.occ) {
    const int pc = mb[get_ls1b_index(stopB)];
    const int t = pc % 6, pcColor = pc >= NN_BLACK;
    stop = pcColor == c ? 1u : (t == NN_KNIGHT || t == NN_BISHOP) ? 2u : t == NN_KING ? 4u : 3u;
  }
  const unsigned prot = (nn_ps_prot[c][s] & tb.type[NN_PAWN] & tb.color[c]) != 0;
  const unsigned conn = (nn_pr_adj[s & 7] & own) != 0;
  unsigned mu = (cls >> (2 * (c ^ 1))) & 3u;
  if (!mu) {
    const int ek = get_ls1b_index(tb.type[NN_KING] & tb.color[c ^ 1]);
    if (!((nn_pr_catch[c][s] >> ek) & 1))
      mu = 4;
  }
  return stop + 5 * (prot + 2 * (conn + 2 * mu));
}

// Voci da capo (bianchi poi neri, case crescenti: ordinate come vuole il merge).
static inline int nn_pst_entries(const NnTB &tb, const int *mb, U64 pw, U64 pb, unsigned cls, unsigned short *out) {
  const U64 P[2] = {pw, pb};
  int n = 0;
  for (int c = 0; c < 2; c++)
    for (U64 b = P[c]; b; b &= b - 1) {
      const int s = get_ls1b_index(b);
      out[n++] = (unsigned short)(c << 13 | s << 7 | nn_pst_state(tb, mb, c, s, P[c], cls));
    }
  return n;
}

// Da capo con merge contro la lista di src; false se le voci sono le stesse (la lista di src vale anche per i).
// O2: le classi si calcolano qui da tb (cls in uscita = classi dello stato i): prima del rebuild possono non essere
// state tenute (lista senza passati).
__attribute__((noinline)) static bool nn_pst_rebuild(NnStack &S, int src, int i, NnState &st, const NnTB &tb,
                                                     const int *mb, U64 pw, U64 pb, unsigned &cls, U64 &pass) {
  NSTAT(GRAFT_REBUILD);
  cls = nn_pst_classes(tb);
  unsigned short nb[16], rem[16], add[16];
  const int m = nn_pst_entries(tb, mb, pw, pb, cls, nb);
  int r = 0, d = 0;
  nn_graft_merge(S.graftList[src], S.graftN[src], nb, m, rem, r, add, d);
  if (r + d == 0)
    return false;
  pass = pw | pb;
  nn_prel_commit(S, i, st, nb, m, rem, r, add, d, 0, pass);
  return true;
}

// O2: stessi passati (stesse case, stessi colori): si riscrive lo stato delle sole voci con la casa in `todo`. Le voci
// restano nello stesso ordine, quindi tolte e aggiunte escono crescenti, come dal merge (una voce con la stessa casa
// e lo stesso stato nelle due liste non entra nella diff, ne' qui ne' nel merge). Nulla si scrive se nessuna cambia
// (una casa davanti toccata non cambia per forza la categoria del pezzo che la occupa).
__attribute__((noinline)) static bool nn_pst_restate_out(NnStack &S, int src, int i, NnState &st, const NnTB &tb,
                                                         const int *mb, U64 todo, U64 pw, U64 pb, unsigned cls,
                                                         U64 pass) {
  NSTAT(GRAFT_RESTATE);
  const unsigned short *a = S.graftList[src];
  const int n = S.graftN[src];
  unsigned short nb[16], rem[16], add[16];
  int r = 0;
  for (int j = 0; j < n; j++) {
    const unsigned e = a[j];
    const int c = int(e >> 13), s = int((e >> 7) & 63);
    unsigned short ne = (unsigned short)e;
    if ((todo >> s) & 1)
      ne = (unsigned short)((e & ~127u) | nn_pst_state(tb, mb, c, s, c == white ? pw : pb, cls));
    nb[j] = ne;
    if (ne != e) {
      rem[r] = (unsigned short)e;
      add[r] = ne;
      r++;
    }
  }
  if (!r)
    return false;
  nn_prel_commit(S, i, st, nb, n, rem, r, add, r, 0, pass);
  return true;
}

// Un passo del recupero per PassedState. pass (lista di src) e cls restano in registro nel chiamante; cls = classi
// dello stato prima, valide solo se pass != 0 (O2, classi pigre: le rinfresca questo passo a ogni mossa che le puo'
// cambiare, il rebuild le calcola da capo). pw, pb = passati dopo la mossa (R4); senza evento di pedone sono anche
// quelli prima, cioe' quelli della lista. Restituisce true se la lista cambia.
static inline bool nn_pst_step(NnStack &S, int src, int i, NnState &st, const NnTB &tb, const int *mb, U64 &pass,
                               unsigned &cls, U64 pw, U64 pb) {
  NSTAT(GRAFT_ST);
  const NnDirtyPiece &dp = st.dp;
  const U64 fileA = 0x0101010101010101ULL, fileH = fileA << 7;
  if (st.pawns.any) {
    if (!(pw | pb | pass))
      return false;  // nessun passato prima ne' dopo: il caso comune in mediogioco
    NSTAT(GRAFT_PASS);
    // Insieme diverso, o una casa di passato toccata (un pedone che ne cattura un altro sulla stessa casa lascia
    // l'insieme uguale ma cambia il colore della voce): da capo con merge.
    if ((pw | pb) != pass || (nn_dp_squares(dp) & pass))
      return nn_pst_rebuild(S, src, i, st, tb, mb, pw, pb, cls, pass);
  } else {
    if (!pass)
      return false;  // niente passati e la mossa non tocca pedoni
    NSTAT(GRAFT_PASS);
  }
  // Qui: stessi passati con gli stessi colori (pw, pb), conn invariato. Passati il cui stato puo' cambiare:
  const U64 moved = nn_dp_squares(dp);
  // stop: casa davanti toccata (bianco s - 8, nero s + 8)
  U64 todo = ((moved << 8) & pw) | ((moved >> 8) & pb);
  // prot: casa di protezione toccata (bianco s + 7 / s + 9, nero s - 9 / s - 7, come nn_ps_prot), solo con i pedoni
  if (st.pawns.any)
    todo |= ((((moved >> 7) & ~fileA) | ((moved >> 9) & ~fileH)) & pw) |
            ((((moved << 9) & ~fileA) | ((moved << 7) & ~fileH)) & pb);
  // mu: le classi cambiano solo con la cattura di un pezzo (non pedone) o con una promozione; conta solo la classe
  // dell'avversario di chi ha passati (bianchi: classe del nero, bit 2-3; neri: del bianco, bit 0-1).
  if ((dp.remove_sq != NN_SQ_NONE && dp.add_sq == NN_SQ_NONE && dp.remove_pc != NN_PAWN &&
       dp.remove_pc != NN_PAWN + NN_BLACK) ||
      dp.to == NN_SQ_NONE) {
    const unsigned ncls = nn_pst_classes(tb), d = ncls ^ cls;
    cls = ncls;
    todo |= ((d & 12u) ? pw : 0) | ((d & 3u) ? pb : 0);
  }
  if (dp.pc == NN_KING || dp.pc == NN_KING + NN_BLACK) {
    // Il re che muove e' il re nemico dei passati dell'altro colore: conta il quadrato solo se il suo lato ha solo re e
    // pedoni (mu 0 / 4). Anche con un evento di pedone (il re che cattura un pedone). Arrocco: dp.to = casa del re.
    const int kc = dp.pc >= NN_BLACK, c = kc ^ 1;
    if (!((cls >> (2 * kc)) & 3u))
      todo |= (c == white ? pw : pb) & (nn_pr_inv[c][dp.from] ^ nn_pr_inv[c][dp.to]);
  }
  if (!todo)
    return false;
  return nn_pst_restate_out(S, src, i, st, tb, mb, todo, pw, pb, cls, pass);
}

#ifdef TRIUMV_VERIFY_GRAFT
// Controllo dei blocchi da innesto (_wip graftfix, come TRIUMV_VERIFY_NNSYNC): dopo ogni stato del recupero, la lista
// valida per lo stato (S.graftList[src]) deve essere quella della funzione di riferimento della rete
// (PawnGrafts::entries_of sulla scacchiera del recupero), con span e passati coerenti, e la diff scritta
// (st.graftRem/graftAdd, S.graft[i]) deve essere il merge fra la lista dello stato prima e quella di riferimento.
// Abort al primo disaccordo. Da usare con -DTRIUMV_GRAFT_RANDOM=<mask> e nnperft. gm = blocco delle liste
// (PassedRel); LockedPawns lo controlla la rete con la stessa macro (PawnGrafts::verify_pawn_*,
// nnue/nnue/features/pawn_grafts.cpp), a ogni refresh con miss della cache "pe" e a ogni diff incrementale.
// O2 (_wip pst_opt): con PassedState la span e' 0 e, se la lista ha passati, le classi in registro (cls) devono essere
// quelle della scacchiera (classi pigre).
static void nn_graft_verify(const NnStack &S, int prevSrc, int src, int i, const NnState &st, const NnTB &tb,
                            unsigned gm, U64 span, U64 pass, unsigned cls) {
  unsigned short ref[NN_GRAFT_MAX];
  const int n = nn_graft_tb(tb, gm, ref);
  const char *what = nullptr;
  if (n != S.graftN[src])
    what = "numero di voci";
  for (int k = 0; !what && k < n; k++)
    if (ref[k] != S.graftList[src][k])
      what = "voce";
  const U64 refSpan = gm == NN_GRAFT_PST ? 0 : nn_graft_passer_span(ref, n);
  if (!what && (span != refSpan || S.graftSpan[src] != span))
    what = "span";
  if (!what && gm == NN_GRAFT_PST && pass && cls != nn_pst_classes(tb))
    what = "classe dei pezzi (cls)";
  if (!what) {
    const U64 wp = tb.type[NN_PAWN] & tb.color[white], bp = tb.type[NN_PAWN] & tb.color[black];
    const U64 refPass = nn_passers(white, wp, bp) | nn_passers(black, bp, wp);
    if (pass != refPass || S.graftPass[src] != pass)
      what = "insieme dei passati";
  }
  if (!what && st.idx != i)
    what = "idx";
  unsigned short rem[NN_GRAFT_MAX], add[NN_GRAFT_MAX];
  int r = 0, d = 0;
  nn_graft_merge(S.graftList[prevSrc], S.graftN[prevSrc], ref, n, rem, r, add, d);
  if (!what && (r != st.graftRem || d != st.graftAdd))
    what = "dimensione della diff";
  for (int k = 0; !what && k < r; k++)
    if (rem[k] != S.graft[i].e[k])
      what = "voce tolta";
  for (int k = 0; !what && k < d; k++)
    if (add[k] != S.graft[i].e[r + k])
      what = "voce aggiunta";
  if (what) {
    fprintf(stderr, "[GRAFT] %s diversa dal riferimento: stato %d src %d prima %d maschera %u mossa %d->%d pezzo %d\n",
            what, i, src, prevSrc, gm, int(st.dp.from), int(st.dp.to), int(st.dp.pc));
    fflush(stderr);
    abort();
  }
}
#endif

static inline void nn_dirty_catch_up(ThreadData &td) {
  NnStack &S = *td.nnstack;
  const int first = S.ready, last = S.size;
  if (first >= last)
    return;
  NnTB tb;
  for (int t = 0; t < 6; t++)
    tb.type[t] = td.bitboards[t] | td.bitboards[t + 6];
  tb.color[white] = td.occupancies[white];
  tb.color[black] = td.occupancies[black];
  tb.occ = td.occupancies[both];
  alignas(64) int mb[64];
  std::memcpy(mb, td.piece_on, sizeof(mb));
  for (int i = last - 1; i >= first; i--) {
    nn_dp_toggle(tb, S.st[i].dp);
    nn_mb_undo(mb, S.st[i].dp);
  }
  // Blocchi da innesto (solo se la rete ne ha): le voci dopo una mossa sono quelle prima della successiva. Si
  // aggiorna solo cio' che la mossa puo' cambiare: nn_prel_step (PassedRel) o nn_pst_step (PassedState, 10/10/2026),
  // che si escludono (KingFiles, Space, LockedPawns tolti il 10/10/2026).
  const unsigned gm = nn_graft_mask & NN_GRAFT_LIST_MASK;
  // R4 (10/10/2026): passati per PassedPawns v1 con ogni rete, uno stato alla volta (NnStack::v1PassW/B). Quelli dello
  // stato first - 1 sono gia' nella pila, salvo alla radice (~0): tb e' ora la sua scacchiera.
  U64 v1w = S.v1PassW[first - 1], v1b = S.v1PassB[first - 1];
  if (v1w == ~0ULL) {
    const U64 wp = tb.type[NN_PAWN] & tb.color[white], bp = tb.type[NN_PAWN] & tb.color[black];
    v1w = S.v1PassW[first - 1] = nn_passers(white, wp, bp);
    v1b = S.v1PassB[first - 1] = nn_passers(black, bp, wp);
  }
  // La lista di partenza (stato first - 1) e' gia' nella pila: si calcola da capo solo alla radice.
  int src = 0;
  U64 span = 0, pass = 0;
  unsigned cls = 0;
  if (gm) {
    src = S.graftSrc[first - 1];
    if (src == 255) {  // tb e' ora la scacchiera dello stato first - 1
      src = first - 1;
      S.graftN[src] = (std::uint8_t)nn_graft_tb(tb, gm, S.graftList[src]);
      S.graftSpan[src] = gm == NN_GRAFT_PST ? 0 : nn_graft_passer_span(S.graftList[src], S.graftN[src]);
      S.graftPass[src] = v1w | v1b;  // passati dello stato first - 1 (R4)
      S.graftSrc[src] = (std::uint8_t)src;
    }
    span = S.graftSpan[src];
    pass = S.graftPass[src];
    // PassedState: classi dello stato first - 1, poi aggiornate da nn_pst_step. O2: solo se la lista ha passati (senza,
    // le calcola il rebuild che li fa comparire).
    if (gm == NN_GRAFT_PST && pass)
      cls = nn_pst_classes(tb);
  }
  for (int i = first; i < last; i++) {
    NnState &st = S.st[i];
    nn_fill_pawns(st.pawns, st.dp, tb.type[NN_PAWN] & tb.color[white], tb.type[NN_PAWN] & tb.color[black]);
    nn_threat_steps(tb, NnLookMailbox{mb}, st.dp, &st.threats);  // tb esce allo stato dopo la mossa
    nn_mb_redo(mb, st.dp);
    if (st.pawns.any) {  // R4: i passati cambiano solo con un evento di pedone
      const U64 wp = tb.type[NN_PAWN] & tb.color[white], bp = tb.type[NN_PAWN] & tb.color[black];
      v1w = nn_passers(white, wp, bp);
      v1b = nn_passers(black, bp, wp);
    }
    S.v1PassW[i] = v1w;
    S.v1PassB[i] = v1b;
    if (gm) {
#ifdef TRIUMV_VERIFY_GRAFT
      const int prevSrc = src;
#endif
      if (gm == NN_GRAFT_PST ? nn_pst_step(S, src, i, st, tb, mb, pass, cls, v1w, v1b)
                             : nn_prel_step(S, src, i, st, tb, span, pass, v1w, v1b))
        src = i;
      S.graftSrc[i] = (std::uint8_t)src;
#ifdef TRIUMV_VERIFY_GRAFT
      nn_graft_verify(S, prevSrc, src, i, st, tb, gm, span, pass, cls);
#endif
    }
  }
  S.ready = last;
}

// Controllo di sincronia (-DTRIUMV_VERIFY_NNSYNC, come TRIUMV_VERIFY_SEEQ): la pila della rete deve descrivere la
// scacchiera del thread. Ogni stato porta la chiave dei pezzi dopo la sua mossa (la scrive la make riuscita; la radice
// nn_root_sync). A ogni valutazione, per TUTTA la catena: la chiave di ogni stato e' quella dello stato prima con la
// mossa descritta dalle sue dirty (pezzo che parte e arriva, catturato, promosso, torre dell'arrocco), e la chiave in
// cima e' quella della scacchiera. Uno stato in piu', in meno o con dirty sbagliate rompe la catena. Abort al primo
// disaccordo.
#ifdef TRIUMV_VERIFY_NNSYNC
static inline U64 nn_dp_key_delta(const NnDirtyPiece &dp) {
  U64 d = piece_keys[dp.pc][dp.from];
  if (dp.to != NN_SQ_NONE)
    d ^= piece_keys[dp.pc][dp.to];
  if (dp.remove_sq != NN_SQ_NONE)
    d ^= piece_keys[dp.remove_pc][dp.remove_sq];
  if (dp.add_sq != NN_SQ_NONE)
    d ^= piece_keys[dp.add_pc][dp.add_sq];
  return d;
}
#endif
static inline void nn_verify_sync(const ThreadData &td) {
#ifdef TRIUMV_VERIFY_NNSYNC
  const NnStack &S = *td.nnstack;
  bool ok = S.size >= 1 && S.size <= NN_STACK_SIZE && S.ready <= S.size && S.st[S.size - 1].key == nn_piece_key(td);
  for (int i = 1; ok && i < S.size; i++)
    ok = S.st[i].key == (S.st[i - 1].key ^ nn_dp_key_delta(S.st[i].dp));
  if (!ok) {
    fprintf(stderr, "[NNSYNC] pila della rete fuori sincronia: stati=%d ply=%d\n", S.size, td.ply);
    fflush(stderr);
    abort();
  }
#else
  (void)td;
#endif
}

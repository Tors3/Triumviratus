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

// (Tabelle dei blocchi da innesto nn_pr_* e nn_ps_*, con nn_graft_tables_init: tolte il 10/10/2026 con i blocchi,
// codice in _backup/Triumviratus_8.0_pre_rimozione_graft_2026-10-10 e nel repo del training,
// 04_consilium/graft_engine_storico.)

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
  // Posizione nella pila (nn_stack_of: passati di PassedPawns v1, R4), nella stessa linea di `computed`.
  st.idx = (std::uint8_t)(S.size - 1);

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



// (Blocchi da innesto PassedRel e PassedState: recupero delle loro voci nella pila, nn_prel_* e nn_pst_*, e controllo
// -DTRIUMV_VERIFY_GRAFT, tolti il 10/10/2026 con i blocchi; codice in _backup/Triumviratus_8.0_pre_rimozione_graft_
// 2026-10-10 e nel repo del training, 04_consilium/graft_engine_storico.)

// Minacce e pedoni degli stati che ne mancano ([ready, size)), prima di ogni valutazione: la scacchiera prima della
// prima mossa in sospeso si ricava da quella del thread disfacendo le mosse in sospeso, poi le mosse si rifanno in
// avanti. Di solito il padre e' gia' stato valutato e la mossa in sospeso e' una sola.
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
  // R4 (10/10/2026): passati per PassedPawns v1, uno stato alla volta (NnStack::v1PassW/B). Quelli dello stato
  // first - 1 sono gia' nella pila, salvo alla radice (~0): tb e' ora la sua scacchiera.
  U64 v1w = S.v1PassW[first - 1], v1b = S.v1PassB[first - 1];
  if (v1w == ~0ULL) {
    const U64 wp = tb.type[NN_PAWN] & tb.color[white], bp = tb.type[NN_PAWN] & tb.color[black];
    v1w = S.v1PassW[first - 1] = nn_passers(white, wp, bp);
    v1b = S.v1PassB[first - 1] = nn_passers(black, bp, wp);
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

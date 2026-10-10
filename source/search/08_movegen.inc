// search/08_movegen.inc -- Generazione delle mosse per thread.
// Parte di threads.cpp: NON e' un'unita' di compilazione a se'. Va inclusa solo da threads.cpp, nell'ordine
// fissato li' (divisione del 27/09/2026, codice generato identico: vedi l'intestazione di threads.cpp).
// ============================================================================
// MOVE GENERATION
// ============================================================================

// between_tbl (case strettamente tra due quadrati allineati) e' definita in 07_makemove.inc, che la usa per gli
// scacchi di scoperta; riempita da init_cuckoo (10_order.inc), usata anche qui dalle evasioni (P2.3).

// Bitboard degli attaccanti di `sq` del colore `by` (P2.3 EvasionGen).
static inline U64 td_attackers_to(ThreadData &td, int sq, int by) {
  const int off = (by == white) ? 0 : 6;
  const SliderPair sp = get_slider_pair(sq, td.occupancies[both]);   // AA1 (10/10/2026): un calcolo per i due
  return (pawn_attacks[by ^ 1][sq] & td.bitboards[P + off]) |
         (knight_attacks[sq] & td.bitboards[N + off]) |
         (king_attacks[sq] & td.bitboards[K + off]) |
         (pair_bishop(sp) &
          (td.bitboards[B + off] | td.bitboards[Q + off])) |
         (pair_rook(sp) &
          (td.bitboards[R + off] | td.bitboards[Q + off]));
}

// 🔴 quiets_only (2A, 29/07): il movepicker chiamava questa funzione con captures_only=false
// nello stage QUIET, cioe' generando TUTTE le pseudo-legali — comprese le catture che aveva
// gia' generato nello stage TACTICAL — per poi marcarle MP_CONSUMED e non renderle mai
// (move picker, search/10_picker.inc). Ogni nodo che supera le good-tactical senza cutoff pagava la
// generazione a vuoto di ~7 catture: pop_lsb, encode, store in lista, e un get_move_capture
// a valle. Con quiets_only si generano solo gli approdi su case VUOTE.
//
// NODE-IDENTICAL, e si dimostra: le catture erano gia' CONSUMED (mai rese), l'ordine relativo
// delle quiete non cambia, e la selection-sort a :5322 rompe i pari col PRIMO indice — quindi
// toglierle dalla lista non cambia ne' l'insieme ne' l'ordine delle mosse rese.
// ⇒ il bench DEVE restare identico: se cambia di un nodo la patch e' sbagliata.
//
// ⚠️ Tre cose NON passano da allowed_squares e vanno escluse a mano:
//   - catture di pedone (usano `& enemies` diretto)
//   - EN PASSANT: e' una cattura che atterra su una casa VUOTA, quindi passerebbe il filtro
//   - (arrocco e promozioni quiete vanno bene da soli: atterrano su case vuote)
//
// 🔴 known_in_check (2C, 29/07): -1 = ignoto (si calcola qui, come prima), 0/1 = il chiamante
// lo sa gia'. Il gate evasion chiedeva `td_is_square_attacked(ksq, ~side)` a OGNI chiamata, e
// nel caso comune — non sotto scacco — quel test NON ha early-exit: deve provare pedoni,
// cavalli, re, e due lookup magic (alfiere e torre, due possibili cache-miss su tabelle
// grosse) solo per rispondere "no". E si pagava TRE volte per nodo: una nella search, una per
// lo stage tattico del movepicker e una per lo stage quieto.
// ⚠️ Passare un valore SBAGLIATO cambia le mosse generate sotto scacco: il gate e' il bench.
// promo_quiet: genera anche le promozioni QUIETE quando captures_only e' true.
// 🔴 Serve SOLO alla qsearch. Il MovePicker NON deve passarlo: il suo stage quieto
// (MPS_GEN_QUIET) le genera gia', e attivarlo anche sullo stage tattico le
// DUPLICHEREBBE nella main search. Default false = tutti i chiamanti invariati.
#define MG_ADD(m) (mg_out[mg_cnt++] = (m))

#if defined(__AVX512F__)
#include <immintrin.h>
// 05/10/2026 — EMISSIONE VETTORIALE delle mosse di un pezzo (AVX-512F). Il ciclo `while (attacks)` scrive una mossa
// per bit e la sua uscita, con un numero di giri che cambia a ogni pezzo (0-27), e' un salto mal predetto: ~9 pezzi
// per generazione e due generazioni per nodo quando si arriva alle quiete. Qui le 64 case sono quattro gruppi FISSI
// di 16: per ogni gruppo si costruisce il vettore delle 16 mosse (origine e pezzo in comune, casa d'arrivo per
// corsia, bit di cattura preso dalla maschera dei nemici) e lo si compatta con vpcompressd sui 16 bit di `attacks`
// del gruppo. Stesso ordine LSB del ciclo, stesse mosse. Le scritture da 16 slot oltre il conteggio restano dentro
// la lista (256 slot per al massimo 218 mosse pseudo-legali). -DTRIUMV_NO_MOVEGEN_VEC riporta al ciclo.
// xperf 6 giri, build PGO avx512: cicli per nodo -1,51%, salti mal predetti -3,4%. Su AVX2 (compattazione con
// vpermd) +0,29%: la build avx2 resta sul ciclo.
static inline int mg_emit_vec(int *mg_out, int mg_cnt, U64 attacks, U64 enemies, int base) {
  alignas(64) static const int kLane[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const __m512i lane  = _mm512_load_si512(kLane);
  const __m512i basev = _mm512_set1_epi32(base);
  const __m512i cap   = _mm512_set1_epi32(1 << 20);   // bit di cattura di encode_move
  for (int c = 0; c < 4; c++) {   // quattro gruppi fissi: il compilatore li svolge
    const __mmask16 tm  = (__mmask16)(attacks >> (16 * c));
    const __mmask16 em  = (__mmask16)(enemies >> (16 * c));
    const __m512i   tgt = _mm512_slli_epi32(_mm512_add_epi32(lane, _mm512_set1_epi32(16 * c)), 6);
    __m512i mv = _mm512_or_si512(basev, tgt);
    mv         = _mm512_mask_or_epi32(mv, em, mv, cap);
    _mm512_storeu_si512(mg_out + mg_cnt, _mm512_maskz_compress_epi32(tm, mv));
    mg_cnt += count_bits(tm);
  }
  return mg_cnt;
}
#endif

// Una mossa per ogni bit di `attacks`, in ordine LSB, col bit di cattura come bit (senza salto sul dato). Il
// percorso vettoriale si usa quando NON si generano solo catture: con gli approdi pieni il ciclo fa 4-15 giri a
// pezzo, con le sole catture 0-1 e il costo fisso dei quattro gruppi non rende.
static inline int mg_emit_targets(int *mg_out, int mg_cnt, U64 attacks, U64 enemies, int source_square, int piece,
                                  bool captures_only) {
#if defined(__AVX512F__) && !defined(TRIUMV_NO_MOVEGEN_VEC)
  if (!captures_only)
    return mg_emit_vec(mg_out, mg_cnt, attacks, enemies, source_square | (piece << 12));
#else
  (void)captures_only;
#endif
  // AA4 (10/10/2026, velocita', stesse mosse nello stesso ordine): con le sole catture un pezzo ne ha 0 (68%), 1 (25%),
  // 2 (6%) o piu' (2%) (contatori, bench 15-16 su fens30/eg30, ~1,1 pezzi per nodo): l'uscita del ciclo dipendeva da
  // quel numero, e un predittore a 2 bit per sito la sbaglia ~0,53-0,55 volte per nodo. La prima cattura si scrive
  // sempre nello slot corrente e il contatore avanza di 0 o 1 (come i pedoni, 04/10/2026); senza catture lo slot resta
  // fuori dal conteggio (la casa 63 e' solo un valore lecito per l'indice). Il ciclo resta per la seconda e oltre.
  if (captures_only) {
    const int first = get_ls1b_index(attacks | (1ULL << 63));
    mg_out[mg_cnt] = encode_move(source_square, first, piece, 0, (int)((enemies >> first) & 1), 0, 0, 0);
    mg_cnt += attacks != 0;
    attacks &= attacks - 1;
  }
  while (attacks) {
    const int target_square = get_ls1b_index(attacks);
    mg_out[mg_cnt++] = encode_move(source_square, target_square, piece, 0,
                                   (int)((enemies >> target_square) & 1), 0, 0, 0);
    pop_lsb_bb(attacks);   // 2D: target_square E' l'LSB di attacks
  }
  return mg_cnt;
}
// NPS 25/09/2026 — generatore TEMPLATE sul colore. Il profilo xperf (campioni su
// BranchMispredictions) dava a td_generate_moves ~2,3 mispredict/nodo contro ~0,5 del
// movegen di SF, concentrati sul ciclo dei tipi di pezzo: con il colore a runtime ogni
// iterazione rifaceva i test `piece == P`, `(td.side == white) ? piece == N : ...`.
// Con MG_SIDE costante il ciclo si svolge e quei test spariscono. Stesso ordine di
// generazione => nodi identici. Il wrapper td_generate_moves() sotto conserva la firma.
template <int MG_SIDE>
static void td_generate_moves_side(ThreadData &td, moves *move_list, bool captures_only,
                                   bool quiets_only, int known_in_check, bool promo_quiet,
                                   bool promo_queen_only,
                              // Modo 7: oltre alla donna genera anche il CAVALLO. Torre e
                              // alfiere restano fuori perche' sono strettamente DOMINATI
                              // dalla donna (fa tutto cio' che fanno loro); l'unica eccezione
                              // e' evitare lo stallo, che e' materia di ricerca completa in
                              // finale, non di qsearch. Il cavallo no: attacca case che la
                              // donna non attacca, quindi esistono posizioni in cui la
                              // promozione a cavallo con scacco (forchetta re+donna) e'
                              // l'UNICA mossa vincente, e oggi la qsearch non la vede.
                                   bool promo_knight) {
  PROF_GUARD(prof_mg);
#ifdef TRIUMV_PROFILE
  prof_n_mg++;   // SF: 0,52 chiamate/nodo, 156 cicli l'una (kiwipete)
#endif
  // 🔴 CONTATORE LOCALE (09/09/2026). Prima ogni mossa passava da
  // `add_move(move_list, m)` = `move_list->moves[move_list->count++] = m`, e
  // `count` vive NELLA STESSA STRUCT dell'array: il compilatore non puo'
  // dimostrare che `moves[i]` non aliasi `count` (non sa che i < 256), quindi
  // ricarica il contatore a ogni mossa. Tre accessi in memoria invece di uno.
  // Stockfish scrive con `*moveList++`, cioe' un puntatore in un registro.
  // Qui il contatore sta in un registro e si deposita UNA volta, in fondo.
  // Node-identical: stesse mosse, stesso ordine.
  int  mg_cnt = 0;
  int *const mg_out = move_list->moves;
  int source_square, target_square;
  U64 bitboard, attacks;

  // --- MAGIA BITWISE: Precalcoliamo le maschere ---
  U64 enemies =
      (MG_SIDE == white) ? td.occupancies[black] : td.occupancies[white];
  U64 friends =
      (MG_SIDE == white) ? td.occupancies[white] : td.occupancies[black];
  // Se vogliamo solo catture, le uniche case valide sono quelle nemiche.
  // Altrimenti, tutte le case tranne le nostre.
  U64 allowed_squares = captures_only ? enemies
                      : quiets_only   ? ~td.occupancies[both]   // solo case VUOTE
                                      : ~friends;

  // P2.3 EvasionGen: sotto scacco le sole pseudo-legali che POSSONO essere
  // legali sono: mosse di re, catture dello scaccante, blocchi sul raggio
  // (scacco singolo; doppio scacco: solo il re). Restringiamo le maschere QUI:
  // l'ordine delle mosse superstiti resta IDENTICO e quelle scartate sarebbero
  // state comunque rigettate dal make-filter (re ancora in scacco) =>
  // node-identical. Le case di blocco sono VUOTE per definizione (un pezzo
  // nemico sul raggio = niente scacco), quindi checker|between funziona sia
  // per le catture sia per i blocchi. E.p. e arrocco: invariati (l'e.p. puo'
  // risolvere lo scacco in modi speciali -> lo filtra il make; l'arrocco in
  // scacco e' gia' auto-escluso dal test e1/e8-attaccata).
  U64 evasion_mask = ~0ULL;
  if (g_evasion_gen) {
    const int ksq =
        get_ls1b_index((MG_SIDE == white) ? td.bitboards[K] : td.bitboards[k]);
    // 2C: se il chiamante lo sa gia', non si ri-scandiscono tutti i tipi di attaccante.
    const bool in_chk = (known_in_check >= 0)
                            ? (known_in_check != 0)
                            : td_is_square_attacked(td, ksq, (MG_SIDE ^ 1));
    if (in_chk) {
      U64 checkers = td_attackers_to(td, ksq, (MG_SIDE ^ 1));
      if (checkers & (checkers - 1)) {
        evasion_mask = 0ULL; // doppio scacco: solo il re
      } else {
        const int csq = get_ls1b_index(checkers);
        evasion_mask = checkers | between_tbl[csq][ksq];
      }
    }
  }
  // Maschera per i pezzi NON-re (il re tiene allowed_squares pieno).
  const U64 piece_allowed = allowed_squares & evasion_mask;

  // 🔴 SOLO i 6 tipi di pezzo del lato al tratto (09/09/2026). Il ciclo scorreva
  // tutti e 12, compresi i sei avversari: quelle iterazioni non generano nulla
  // (ogni ramo interno confronta col codice-pezzo del lato al tratto) ma pagano
  // comunque ~8 confronti a testa. Eredita' della struttura BBC.
  // P..K = 0..5, p..k = 6..11, quindi il blocco del lato e' contiguo.
  constexpr int mg_first = (MG_SIDE == white) ? P : p;
#if defined(__clang__)
#pragma clang loop unroll(full)
#elif defined(__GNUC__)
#pragma GCC unroll 6
#endif
  for (int piece = mg_first; piece <= mg_first + 5; piece++) {
    bitboard = td.bitboards[piece];

    if (MG_SIDE == white) {
      if (piece == P) {
        // 04/10/2026 notte — PEDONI SENZA SALTI SUL DATO, stesso ordine di prima (albero identico). Il ciclo per
        // pedone faceva 4-5 salti dipendenti dai dati a pedone (casa libera, traversa, maschera delle parate, doppio
        // passo, 0-2 catture): il profilo xperf dava alla generazione ~1,7 salti mal predetti per nodo. Ora le
        // spinte si calcolano in blocco (shift della bitboard, maschere di vuoto e di parata) e per ogni pedone si
        // SCRIVE sempre la mossa nello slot e si avanza il contatore solo se vale (0 o 1): nessun salto. L'ordine
        // resta quello del ciclo: per pedone (LSB), spinta, doppio passo, cattura a sinistra, a destra, e.p.
        // I pedoni in 7a (promozioni) tengono il codice di prima: sono rari e vengono per primi nell'ordine LSB
        // (case 8..15), quindi si fanno prima degli altri.
        U64 promo = bitboard & 0x000000000000FF00ULL;
        while (promo) {
          source_square = get_ls1b_index(promo);
          target_square = source_square - 8;

          // Mosse silenziose dei pedoni: bloccate se captures_only   true
          // (P2.3: ogni approdo deve stare in evasion_mask — sotto scacco
          //  solo i blocchi sul raggio sopravvivono, fuori scacco ~0ULL no-op)
          // 🔴 ECCEZIONE PromoQS: la promozione quieta e' TATTICA per tutto il
          // resto del motore (score 750.000 in td_score_move, esente da LMP,
          // futility, history-prune e SEE) ma nasce SOLO qui dentro. Con
          // captures_only la qsearch non la vedeva mai. Il ramo interno separa
          // gia' 7a (promozioni) dal resto (spinte normali): entrando solo con
          // il pedone in 7a, l'else e' irraggiungibile e non si generano quiete.
          if (!captures_only ||
              (promo_quiet && source_square >= a7 && source_square <= h7)) {
            if (!(target_square < a8) &&
                !get_bit(td.occupancies[both], target_square)) {
              if (source_square >= a7 && source_square <= h7) {
                if ((evasion_mask >> target_square) & 1) {
                  MG_ADD( encode_move(source_square, target_square,
                                                  piece, Q, 0, 0, 0, 0));
                  if (!promo_queen_only) {
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, R, 0, 0, 0, 0));
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, B, 0, 0, 0, 0));
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, N, 0, 0, 0, 0));
                  } else if (promo_knight) {
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, N, 0, 0, 0, 0));
                  }
                }
              } else {
                if ((evasion_mask >> target_square) & 1)
                  MG_ADD( encode_move(source_square, target_square,
                                                  piece, 0, 0, 0, 0, 0));
                if ((source_square >= a2 && source_square <= h2) &&
                    !get_bit(td.occupancies[both], target_square - 8) &&
                    ((evasion_mask >> (target_square - 8)) & 1))
                  MG_ADD(
                           encode_move(source_square, target_square - 8, piece,
                                       0, 0, 1, 0, 0));
              }
            }
          }

          // Catture dei pedoni
          attacks = quiets_only ? 0ULL   // 2A: le catture di pedone non passano da allowed_squares
                  : (pawn_attacks[MG_SIDE][source_square] & enemies & evasion_mask);
          while (attacks) {
            target_square = get_ls1b_index(attacks);
            if (source_square >= a7 && source_square <= h7) {
              MG_ADD( encode_move(source_square, target_square,
                                              piece, Q, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, R, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, B, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, N, 1, 0, 0, 0));
            } else {
              MG_ADD( encode_move(source_square, target_square,
                                              piece, 0, 1, 0, 0, 0));
            }
            pop_lsb_bb(attacks);   // 2D: target_square E' l'LSB di attacks
          }

          // En passant (  sempre una cattura, lo generiamo sempre)
          if (!quiets_only && td.enpassant != no_sq) {   // 2A: e.p. atterra su casa VUOTA
            U64 enpassant_attacks =
                pawn_attacks[MG_SIDE][source_square] & (1ULL << td.enpassant);
            if (enpassant_attacks) {
              int target_enpassant = get_ls1b_index(enpassant_attacks);
              MG_ADD( encode_move(source_square, target_enpassant,
                                              piece, 0, 1, 0, 1, 0));
            }
          }
          pop_lsb_bb(promo);
        }
        {
          const U64 rest      = bitboard & ~0x000000000000FF00ULL;
          const U64 empty     = ~td.occupancies[both];
          const U64 push1     = captures_only ? 0ULL : (rest >> 8) & empty;           // casa davanti libera
          const U64 single_to = push1 & evasion_mask;
          // Doppio passo: dalla 2a (arrivi in 3a = case 40..47), con la 4a libera e nella maschera delle parate.
          const U64 double_to = ((push1 & 0x0000FF0000000000ULL) >> 8) & empty & evasion_mask;
          const U64 cap_mask  = quiets_only ? 0ULL : enemies & evasion_mask;
          const U64 ep_bb     = (!quiets_only && td.enpassant != no_sq) ? (1ULL << td.enpassant) : 0ULL;
          const int ep_sq     = td.enpassant;   // se assente la mossa scritta non viene contata
          // 06/10/2026 (velocita', albero identico): si scorrono solo i pedoni che hanno almeno una mossa. Un pedone
          // senza spinta, senza doppio passo e senza cattura/e.p. scriveva cinque slot senza contarne nessuno: nelle
          // sole catture (quiescenza e fase tattica, ~1,4 pedoni per nodo) erano quasi tutti cosi'. I pedoni che
          // catturano su `tgt` sono le case attaccate da un pedone NERO posto su `tgt` (stesse maschere di
          // mask_pawn_attacks). Stesso insieme di mosse contate, stesso ordine (LSB): nodi identici.
          const U64 tgt       = cap_mask | ep_bb;
          U64 bb = rest & ((single_to << 8) | (double_to << 16) | ((tgt << 7) & not_h_file) | ((tgt << 9) & not_a_file));
          while (bb) {
            const int s  = get_ls1b_index(bb);
            const U64 pa = pawn_attacks[white][s];
            const U64 caps = pa & cap_mask;
            mg_out[mg_cnt] = encode_move(s, s - 8, piece, 0, 0, 0, 0, 0);
            mg_cnt += (int)((single_to >> (s - 8)) & 1);
            mg_out[mg_cnt] = encode_move(s, s - 16, piece, 0, 0, 1, 0, 0);
            mg_cnt += (int)((double_to >> (s - 16)) & 1);
            mg_out[mg_cnt] = encode_move(s, s - 9, piece, 0, 1, 0, 0, 0);
            mg_cnt += (int)((caps >> (s - 9)) & 1);
            mg_out[mg_cnt] = encode_move(s, s - 7, piece, 0, 1, 0, 0, 0);
            mg_cnt += (int)((caps >> (s - 7)) & 1);
            mg_out[mg_cnt] = encode_move(s, ep_sq, piece, 0, 1, 0, 1, 0);
            mg_cnt += (int)((pa & ep_bb) != 0);
            pop_lsb_bb(bb);
          }
        }
      }
      if (piece == K) {
        // Arrocco: bloccato se captures_only   true
        if (!captures_only && g_chess960 && td.castle) {
          // Chess960 (chess960.h): regola generale di SF. Il ramo normale qui sotto resta identico.
          auto att = [&td](int sq) { return td_is_square_attacked(td, sq, black) != 0; };
          for (int idx = 0; idx < 2; idx++)
            if (c960_can_castle(td.castle, idx, td.occupancies[both], att))
              MG_ADD( encode_move(castle_king_sq[white], castle_king_to(idx), piece, 0, 0, 0, 0, 1));
        } else if (!captures_only) {
          if (td.castle & wk) {
            if (!get_bit(td.occupancies[both], f1) &&
                !get_bit(td.occupancies[both], g1)) {
              if (!td_is_square_attacked(td, e1, black) &&
                  !td_is_square_attacked(td, f1, black))
                MG_ADD( encode_move(e1, g1, piece, 0, 0, 0, 0, 1));
            }
          }
          if (td.castle & wq) {
            if (!get_bit(td.occupancies[both], d1) &&
                !get_bit(td.occupancies[both], c1) &&
                !get_bit(td.occupancies[both], b1)) {
              if (!td_is_square_attacked(td, e1, black) &&
                  !td_is_square_attacked(td, d1, black))
                MG_ADD( encode_move(e1, c1, piece, 0, 0, 0, 0, 1));
            }
          }
        }
      }
    } else {
      if (piece == p) {
        // Specchio del bianco (vedi sopra). I pedoni neri che promuovono stanno in 2a (case 48..55), gli ULTIMI
        // nell'ordine LSB: prima gli altri senza salti, poi loro con il codice di prima.
        {
          const U64 rest      = bitboard & ~0x00FF000000000000ULL;
          const U64 empty     = ~td.occupancies[both];
          const U64 push1     = captures_only ? 0ULL : (rest << 8) & empty;
          const U64 single_to = push1 & evasion_mask;
          // Doppio passo: dalla 7a (arrivi in 6a = case 16..23), con la 5a libera e nella maschera delle parate.
          const U64 double_to = ((push1 & 0x0000000000FF0000ULL) << 8) & empty & evasion_mask;
          const U64 cap_mask  = quiets_only ? 0ULL : enemies & evasion_mask;
          const U64 ep_bb     = (!quiets_only && td.enpassant != no_sq) ? (1ULL << td.enpassant) : 0ULL;
          const int ep_sq     = td.enpassant;
          // Solo i pedoni che hanno almeno una mossa (vedi il bianco): catturano su `tgt` le case attaccate da un
          // pedone BIANCO posto su `tgt`.
          const U64 tgt       = cap_mask | ep_bb;
          U64 bb = rest & ((single_to >> 8) | (double_to >> 16) | ((tgt >> 7) & not_a_file) | ((tgt >> 9) & not_h_file));
          while (bb) {
            const int s  = get_ls1b_index(bb);
            const U64 pa = pawn_attacks[black][s];
            const U64 caps = pa & cap_mask;
            mg_out[mg_cnt] = encode_move(s, s + 8, piece, 0, 0, 0, 0, 0);
            mg_cnt += (int)((single_to >> (s + 8)) & 1);
            mg_out[mg_cnt] = encode_move(s, s + 16, piece, 0, 0, 1, 0, 0);
            mg_cnt += (int)((double_to >> (s + 16)) & 1);
            mg_out[mg_cnt] = encode_move(s, s + 7, piece, 0, 1, 0, 0, 0);
            mg_cnt += (int)((caps >> (s + 7)) & 1);
            mg_out[mg_cnt] = encode_move(s, s + 9, piece, 0, 1, 0, 0, 0);
            mg_cnt += (int)((caps >> (s + 9)) & 1);
            mg_out[mg_cnt] = encode_move(s, ep_sq, piece, 0, 1, 0, 1, 0);
            mg_cnt += (int)((pa & ep_bb) != 0);
            pop_lsb_bb(bb);
          }
        }
        U64 promo = bitboard & 0x00FF000000000000ULL;
        while (promo) {
          source_square = get_ls1b_index(promo);
          target_square = source_square + 8;

          if (!captures_only ||
              (promo_quiet && source_square >= a2 && source_square <= h2)) {
            if (!(target_square > h1) &&
                !get_bit(td.occupancies[both], target_square)) {
              if (source_square >= a2 && source_square <= h2) {
                if ((evasion_mask >> target_square) & 1) {
                  MG_ADD( encode_move(source_square, target_square,
                                                  piece, q, 0, 0, 0, 0));
                  if (!promo_queen_only) {
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, r, 0, 0, 0, 0));
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, b, 0, 0, 0, 0));
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, n, 0, 0, 0, 0));
                  } else if (promo_knight) {
                    MG_ADD( encode_move(source_square, target_square,
                                                    piece, n, 0, 0, 0, 0));
                  }
                }
              } else {
                if ((evasion_mask >> target_square) & 1)
                  MG_ADD( encode_move(source_square, target_square,
                                                  piece, 0, 0, 0, 0, 0));
                if ((source_square >= a7 && source_square <= h7) &&
                    !get_bit(td.occupancies[both], target_square + 8) &&
                    ((evasion_mask >> (target_square + 8)) & 1))
                  MG_ADD(
                           encode_move(source_square, target_square + 8, piece,
                                       0, 0, 1, 0, 0));
              }
            }
          }

          attacks = quiets_only ? 0ULL   // 2A: le catture di pedone non passano da allowed_squares
                  : (pawn_attacks[MG_SIDE][source_square] & enemies & evasion_mask);
          while (attacks) {
            target_square = get_ls1b_index(attacks);
            if (source_square >= a2 && source_square <= h2) {
              MG_ADD( encode_move(source_square, target_square,
                                              piece, q, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, r, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, b, 1, 0, 0, 0));
              MG_ADD( encode_move(source_square, target_square,
                                              piece, n, 1, 0, 0, 0));
            } else {
              MG_ADD( encode_move(source_square, target_square,
                                              piece, 0, 1, 0, 0, 0));
            }
            pop_lsb_bb(attacks);   // 2D: target_square E' l'LSB di attacks
          }
          if (!quiets_only && td.enpassant != no_sq) {   // 2A: e.p. atterra su casa VUOTA
            U64 enpassant_attacks =
                pawn_attacks[MG_SIDE][source_square] & (1ULL << td.enpassant);
            if (enpassant_attacks) {
              int target_enpassant = get_ls1b_index(enpassant_attacks);
              MG_ADD( encode_move(source_square, target_enpassant,
                                              piece, 0, 1, 0, 1, 0));
            }
          }
          pop_lsb_bb(promo);
        }
      }
      if (piece == k) {
        if (!captures_only && g_chess960 && td.castle) {
          auto att = [&td](int sq) { return td_is_square_attacked(td, sq, white) != 0; };
          for (int idx = 2; idx < 4; idx++)
            if (c960_can_castle(td.castle, idx, td.occupancies[both], att))
              MG_ADD( encode_move(castle_king_sq[black], castle_king_to(idx), piece, 0, 0, 0, 0, 1));
        } else if (!captures_only) {
          if (td.castle & bk) {
            if (!get_bit(td.occupancies[both], f8) &&
                !get_bit(td.occupancies[both], g8)) {
              if (!td_is_square_attacked(td, e8, white) &&
                  !td_is_square_attacked(td, f8, white))
                MG_ADD( encode_move(e8, g8, piece, 0, 0, 0, 0, 1));
            }
          }
          if (td.castle & bq) {
            if (!get_bit(td.occupancies[both], d8) &&
                !get_bit(td.occupancies[both], c8) &&
                !get_bit(td.occupancies[both], b8)) {
              if (!td_is_square_attacked(td, e8, white) &&
                  !td_is_square_attacked(td, d8, white))
                MG_ADD( encode_move(e8, c8, piece, 0, 0, 0, 0, 1));
            }
          }
        }
      }
    }

    // --- PEZZI (Qui applichiamo la maschera allowed_squares) ---
    if ((MG_SIDE == white) ? piece == N : piece == n) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = knight_attacks[source_square] & piece_allowed;
        mg_cnt = mg_emit_targets(mg_out, mg_cnt, attacks, enemies, source_square, piece, captures_only);
        pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
      }
    }

    if ((MG_SIDE == white) ? piece == B : piece == b) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = get_bishop_attacks(source_square, td.occupancies[both]) &
                  piece_allowed;
        mg_cnt = mg_emit_targets(mg_out, mg_cnt, attacks, enemies, source_square, piece, captures_only);
        pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
      }
    }

    if ((MG_SIDE == white) ? piece == R : piece == r) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = get_rook_attacks(source_square, td.occupancies[both]) &
                  piece_allowed;
        mg_cnt = mg_emit_targets(mg_out, mg_cnt, attacks, enemies, source_square, piece, captures_only);
        pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
      }
    }

    if ((MG_SIDE == white) ? piece == Q : piece == q) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = get_queen_attacks(source_square, td.occupancies[both]) &
                  piece_allowed;
        mg_cnt = mg_emit_targets(mg_out, mg_cnt, attacks, enemies, source_square, piece, captures_only);
        pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
      }
    }

    if ((MG_SIDE == white) ? piece == K : piece == k) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = king_attacks[source_square] & allowed_squares;
        mg_cnt = mg_emit_targets(mg_out, mg_cnt, attacks, enemies, source_square, piece, captures_only);
        pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
      }
    }
  }
  move_list->count = mg_cnt;
}

static void td_generate_moves(ThreadData &td, moves *move_list,
                              bool captures_only = false,
                              bool quiets_only   = false,
                              int  known_in_check = -1,
                              bool promo_quiet   = false,
                              bool promo_queen_only = false,
                              bool promo_knight = false) {
  if (td.side == white)
    td_generate_moves_side<white>(td, move_list, captures_only, quiets_only, known_in_check,
                                  promo_quiet, promo_queen_only, promo_knight);
  else
    td_generate_moves_side<black>(td, move_list, captures_only, quiets_only, known_in_check,
                                  promo_quiet, promo_queen_only, promo_knight);
}

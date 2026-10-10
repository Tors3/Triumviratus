// search/01_params.inc -- Parametri della ricerca e globali condivise con le altre unita'.
// Parte di threads.cpp: NON e' un'unita' di compilazione a se'. Va inclusa solo da threads.cpp.
//
// BAKE CAUSALRED (10/10/2026, utente): CausalRed 0 -> 512 (idea dell'utente di giugno, "potatura causale"). SPRT a
// 25+0.25 UHO, due tranche in pool (tag causal512 + causal512b): +4,27 +- 4,13 Elo su 6.594 partite, LLR +1,17,
// profondita' media cand +0,25 ply su entrambi i socket. Fermato dall'utente prima delle 20.000 partite: la misura
// basta perche' lo zero e' escluso, e il valore lo ritarano gli SPSA di chiusura (CausalRed fra le leve). Bench 222811
// (prima 269775).
// BAKE PR4 (09/10/2026, utente): 46 leve di potatura, estensioni e ordinamento dallo SPSA PR4 (20+0.2, fermato
// al giro 6000), media delle iterazioni 5703-6002. SPRT del vettore contro i default: incrocio di tutti i test
// (20+0.2 HT, 15+0.15 e 10+0.1 solo core, 30+0.3 HT con UHO +110/+129) positivo, socket 0 piu' del socket 1.
// Accende l'ordinamento con conoscenza scacchistica (KingShield, Outpost*, AttackOrder*, PassedPush, PromoOrder,
// BishopPairCapt, prima a 0). Valori vecchio->nuovo:
//  BonusMul 139->140, BonusMax 1565->1615, BonusTTMove 371->343, MalusMul 911->853, MalusMax 2186->2156,
//  QuietBonusScale 798->860, QuietMalusScale 1031->1043, CaptBonusScale 1515->1368, CaptMalusScale 1370->1270,
//  ContUpdScale 784->874, GoodQuietLimit -14491->-15563, QuietSortDepthMul 3487->3223, CheckOrderBonus 15142->13323,
//  GoodCaptSeeDiv 18->19, RazorMul 442->452, RfpBetaWeight 712->690, NmpDepthMul 14->15, NmpBase 314->302,
//  NmpEvalDiv 235->223, ProbCutMargin 231->209, ProbCutTTMargin 400->415, CaptFutBase 177->190,
//  CaptFutDepth 279->281, CaptSeeDepth 186->179, QuietHistPrune 3910->4045, QuietFutDepth 122->96,
//  QuietFutBase 166->160, QuietSeeMul 25->24, QsFutMargin 285->312, QsSeeMargin 80->74, SingDoublePv 217->225,
//  SingTripleBase 67->60, LmrTtPvSub 3227->2915, LmrTTMove 2082->2037, DeeperMargin 50->51, LdseMargin 80->76,
//  AspBase 5->6, AspGrow 48->51, KingShield 0->5371, OutpostOrder 0->4179, OutpostOrderB 0->1078,
//  AttackOrder 0->5807, AttackOrderQ 0->6371, PassedPush 0->3975, PromoOrder 0->2412, BishopPairCapt 0->601
// BAKE RW1 (04/10/2026): 49 valori sono la media delle iterazioni 4481-5481 dello SPSA "RW1" (51 leve, 20+0.2,
// Tuning_SPSA/spsa_prune/vettore_rw1_it5481.json), giocato dal nostro motore con Consilium; PassedPushRed (nostra)
// entra accesa a 1125. SPRT di gate contro i valori di partenza a 10+0.1: +9,67 +- 9,38 (LLR 0,61, fermato e
// accettato dall'utente). Bench 141196 (prima 172833). Fuori dallo SPSA, quindi ancora ai valori di partenza: la
// gestione del tempo, le soglie di profondita' e LmrLogMul.
// BAKE LDSE (05/10/2026): LdseMargin 0 -> 80, LdseMode 0 -> 1 (estensione della mossa hash a profondita' bassa, solo
// nei nodi cut; 13_search.inc, move_loop). SPRT a 10+0.1 sul binario t80_d1005, libro UHO, 40/30 partite per
// socket: +4,52 +- 3,34 su 11.604 partite (LLR 2,04 su [-2,94, 2,94], nElo [0, 3]); socket 0 (4 banchi di RAM, piu'
// veloce e piu' profondo) +6,8 +- 4,4, socket 1 (2 banchi) +1,5 +- 5,1: sembra rendere di piu' con piu' profondita'.
// Fermato e bakato su decisione dell'utente prima delle 20.000 partite. Bench 498873 (prima 141196; con
// LdseMargin=0 torna 141196): l'estensione vicino all'orizzonte allarga molto gli alberi a profondita' fissa.
// Sui finali (libro endgame_12_18, 10+0.1) Ldse spenta contro accesa -0,88 +- 4,05 su 2.786 partite: non dannosa.
// BAKE GUARDIA (05/10/2026): LdseMax 0 -> 1, al massimo un'estensione Ldse per linea (NodeFrame::ldse_path). Nei
// finali di torre l'estensione si ripeteva lungo la linea (albero x5-x16 a profondita' fissa). SPRT contro Ldse senza
// tetto a 15+0.15, UHO: +6,25 +- 7,89 su 2.112 partite (LLR 0,54; socket 0 +7,4, socket 1 +4,6), fermato e bakato
// su decisione dell'utente. Bench 308883 (con LdseMax=0 torna 498873).
// BAKE SORPRESA (05/10/2026): TmSurpriseScale 1000 -> 1200 (tempo della mossa x1,2 quando l'avversario non ha giocato
// la risposta della nostra linea principale; 14_deepen.inc). SPRT a 40+0.4, UHO, seme 7200: +6,23 +- 6,75 su 2.174
// partite (LLR 0,65, LOS ~96%; socket 0 +6,2, socket 1 +6,3), fermato e bakato su decisione dell'utente. Dai PGN: sulle
// mosse dopo una sorpresa (~26%) +0,35-0,4 ply. A 16+0.16 era neutra (+0,7): dipende dal TC (vedi il parametro).
// Il bench (profondita' fissa) non usa il tempo: resta 308883.
// BAKE HASHQSDEPTH (06/10/2026): HashQsDepth 0 -> 1 (la quiescenza scrive e legge profondita' 0 in hash, non -1;
// 12_quiesce.inc). SPRT a 10+0.1, UHO, 40/30: +3,38 +- 2,58 su 20.162 partite (LLR 2,39, zero escluso); socket 0
// +1,0 +- 3,5, socket 1 (~0,4 ply piu' profondo) +6,6 +- 3,9. Bakato su decisione dell'utente. Bench 222624 (con
// HashQsDepth=0 torna 308883).
// BAKE SEEPINNED (06/10/2026): SeePinned 0 -> 1 (nella SEE i pezzi inchiodati al re non ricatturano finche'
// l'inchiodatore e' sulla scacchiera; see.cpp, 11_queue.inc). SPRT a 10+0.1, UHO, 40/30: +4,50 +- 6,78 su 2.934
// partite, socket 0 (40 partite, hyperthread, meno profondo) -2,1 +- 9,0, socket 1 (~0,4 ply piu' profondo) +13,4
// +- 10,2. Lettura dell'utente: neutra a TC effettivo basso, positiva appena la ricerca scava un po' di piu'; fermato
// e bakato su sua decisione. Albero a profondita' fissa quasi invariato (mediana x1,00 a prof. 10-16; il bench salta
// per una sola posizione). Bench 507070 (con SeePinned=0 torna 222624).
// BAKE PULIZIA (06/10/2026): NullFiftyKeep 0 -> 1 (la mossa nulla non incrementa il contatore delle 50 mosse) e
// EvalRefadeBridge 0 -> 1 (la valutazione riletta da cache/hash si rismorza con la stessa formula del ponte: la stessa
// posizione vale uguale fresca o riletta). Agiscono col contatore alto: SPRT sui finali (endgame_12_18, 5+0.05,
// -NoDrawAdj, 40/40) +1,60 +- 2,61 su 9.774 partite (socket 0 +1,1, socket 1 +2,1); sul libro UHO a 6+0.06 -2,19 +-
// 7,45 su 2.698 (nessun danno nel mediogioco). Albero a profondita' fissa: mediogioco x0,97-1,16, finali x1,05 -> x0,91
// a prof. 16. Bakato su decisione dell'utente come correzione di coerenza. Bench 370886 (con le due a 0 torna 507070).
// BAKE CORRPHASE (06/10/2026): CorrPhase 0 -> 1 (correzioni dei pedoni e dei minori separate per esperto della rete:
// costante per fascia di fase mescolata nella chiave). Prima prova -5,10 +- 8,13 su 1.976 partite, ma la make
// anticipava le righe con le chiavi senza sale (rapporto docs/audit_8.0/N_CORREZIONI.md, E1, corretto). Di nuovo, con
// il prefetch giusto e la pulizia bakata: 12+0.12, UHO, 34/34, +3,22 +- 3,84 su 8.634 partite (LLR 1,00; socket 0
// +2,2, socket 1 +4,3), fermata da un blackout a 1.400 partite dalla soglia scelta (10.000) e bakata su decisione
// dell'utente. Bench 309067 (con CorrPhase=0 torna 370886).
// BAKE HASHDAMP (06/10/2026 notte): HashDamp 0 -> 41 (taglio della hash da limite inferiore spinto verso beta,
// 13_search.inc). Tre prove, tutte positive: 15+0.15 su t80_nm2 (05/10, prima dei bake) +2,64 +- 6,79 / 2.768;
// 15+0.15 su t80_tm12 +2,18 +- 5,35 / 4.138; 20+0.2 su t80_tm13 +2,02 +- 4,96 / 4.818 (socket 0 +2,9, socket 1 +1,2).
// Sul codice attuale insieme +2,09 +- 3,64 / 8.956; tutte e tre +2,22 +- 3,21 / 11.774 (P(Elo > 0) ~ 91%). Bakata su
// decisione dell'utente prima delle 20.000 partite. Bench 430151 (con HashDamp=0 torna 309067). Albero a profondita'
// fissa (tree_depth.py, tm13): mediogioco x0,94/0,92/0,91/1,02 alle prof. 12/14/16/18, finali x0,87/0,92/1,02/0,99:
// nessuna crescita con la profondita'.
// PULIZIA DEL 08/10/2026: ritirate le opzioni chiuse (spente, albero identico, bench invariato 430151). Il loro codice
// e' nel sorgente pubblicato su GitHub fino al commit bf1a29e (cartella source/) e in
// ../_backup/Triumviratus_8.0_pre_pulizia_2026-10-08, se un giorno si vogliono riprovare:
//   RazorAllLin (razoring lineare nei nodi all; 600 a 10+0.1 -2,32 +- 2,52 / 20.032), NmpPriorFH (soglia della nulla
//   piu' bassa dopo nulle riuscite fra i fratelli; 48 a 15+0.15 +0,46 +- 2,66 / 17.308), FhRecovery (profondita' che
//   risale per gradi dopo un fail-high alla radice; -1,82 +- 4,54 / 5.932), RfpOppCapture (sconto "improving" della
//   RFP ridotto se l'avversario ha una cattura facile; 512 a 15+0.15 -3,47 +- 8,37 / 1.704), NmpOppCapture (niente
//   nulla in quel caso; -0,34 +- 4,04 / 8.164), RootRedCap (tetto alla riduzione delle mosse di radice; 1024 -5,80
//   +- 5,93 / ~1.000), QsQuietHash (mossa hash quieta cercata in quiescenza; -1,05 +- 3,92), CorrFade (correzione
//   smorzata col contatore delle 50 mosse; 1 a 10+0.1 sui finali -2,70 +- 4,27 / 2.572), TransitionExt (estensione
//   della cattura che lascia solo pedoni; -18,2 +- 16,4 / 402), TTMoveRefresh (mossa nuova anche nell'entry piu'
//   profonda conservata; -0,65 +- 2,77 / 17.574), TmExpectFloor (pavimento di profondita' dopo la risposta prevista;
//   -5,5 +- 13,4 / 696 col tetto massimo, +0,6 +- 10,8 / 1.090 col tetto ottimale). Ritirate anche le prove di
//   prefetch -DTRIUMV_PREFETCH_THR_PSQT e -DTRIUMV_REFRESH_PREFETCH di nnue_accumulator.cpp (misurate e perse).
// Riscrittura del 04/10/2026: i margini della ricerca sono espressi in unita' proprie (pedone ~ 356); la valutazione
// della rete viene portata a queste unita' da EvalUnits (09_history.inc), cosi' i margini non dipendono dalla scala
// della rete. Ogni parametro e' un'opzione UCI nelle build di sviluppo e una
// costante nella build di release (TRIUMV_RELEASE), dove il compilatore la piega nel codice.

// ---------------------------------------------------------------------------
// Tabella dei parametri: nome, default, minimo, massimo.
// ---------------------------------------------------------------------------
#define TRIUMV_SEARCH_PARAMS(P)                                                                            \
  /* valutazione */                                                                                       \
  P(EvalUnits, 1347, 900, 1800)          /* /1024: unita' nostre -> unita' dei margini (pedone ~ 356) */  \
  /* Termini dal residuo (09/10/2026, idea P1, utente: "candidati per correggere la search"). Studio: 3.888        \
     posizioni varie, punteggio a profondita' 1 contro ricerca a 300k nodi; il residuo e' regredito su concetti      \
     scacchistici a parita' di punteggio, materiale e pezzi (tools/residual_features.py, studio_neuroni/res). Stima \
     CONGIUNTA, centipedoni stampati (scala 400), nostro meno loro: finali (<= 15 pezzi) passato +8 per traversa    \
     (t 5,6), passato che il re nemico non raggiunge +18 (t 2,2), spazio +8 per casa (t 4,0); mediogioco (> 15)     \
     colonne aperte vicino al proprio re -14 (t -4,5), spazio +6 (t 5,9), passati collegati +18 (t 3,3). I termini   \
     si sommano alla valutazione statica (fuori dalla cache), in cp stampati convertiti nelle unita' dei margini.   \
     Tutti a 0 = spenti, valutazione identica. Valori di prova: la meta' delle stime (regressione su poche migliaia \
     di posizioni). Il residuo contiene anche effetti della ricerca stessa (per esempio le spinte dei passati ridotte \
     meno, PassedPushRed): la prova in partita dice se correggere la valutazione rende. */                          \
  P(ResPasserRank, 0, 0, 40)             /* cp per traversa di ogni passato, solo finali (prova 4)        */  \
  P(ResPasserFree, 0, 0, 60)             /* cp per passato fuori dal quadrato del re nemico, finali (prova 9) */ \
  P(ResPasserConn, 0, 0, 60)             /* cp per passato con un passato sulla colonna vicina (prova 9)  */  \
  P(ResKingOpen, 0, 0, 40)               /* cp in meno per colonna senza nostri pedoni vicino al re, medio (prova 7) */ \
  P(ResSpace, 0, 0, 20)                  /* cp per casa di spazio (colonne c-f, traverse 2-4) (prova 3)   */  \
  P(OptStrength, 182, 0, 600)            /* optimism = forza * s / (|s| + div), in unita' della rete */   \
  P(OptDiv, 288, 1, 600)                                                                                  \
  /* finestre di aspirazione */                                                                           \
  P(AspBase, 6, 1, 40)                                                                                    \
  P(AspMeanSqDiv, 10193, 1000, 40000)                                                                     \
  P(AspGrow, 51, 10, 128)                /* /128 */                                                       \
  /* history: bonus e malus */                                                                            \
  P(BonusMul, 140, 50, 300)                                                                               \
  P(BonusSub, 81, 0, 300)                                                                                 \
  P(BonusMax, 1615, 500, 3000)                                                                            \
  P(BonusTTMove, 343, 0, 1000)                                                                            \
  P(BonusPrevStatDiv, 28, 4, 200)                                                                         \
  P(MalusMul, 853, 300, 2000)                                                                             \
  P(MalusSub, 235, 0, 800)                                                                                \
  P(MalusMax, 2156, 800, 5000)                                                                            \
  P(QuietBonusScale, 860, 400, 1600)     /* /1024 */                                                      \
  P(QuietMalusScale, 1043, 400, 2000)                                                                     \
  P(QuietMalusDecay, 921, 600, 1024)                                                                      \
  P(CaptBonusScale, 1368, 500, 2500)                                                                      \
  P(CaptMalusScale, 1270, 500, 2500)                                                                      \
  P(PrevContMalus, 713, 0, 1500)                                                                          \
  P(LowPlyScale, 712, 0, 1500)                                                                            \
  P(ContUpdScale, 874, 300, 1500)                                                                         \
  P(PawnBonusScale, 1104, 300, 2000)                                                                      \
  P(PawnMalusScale, 459, 100, 1500)                                                                       \
  P(EvalDiffMul, 11, 0, 40)               /* history della mossa precedente dalla differenza di eval */   \
  P(EvalDiffPawnMul, 13, 0, 40)                                                                           \
  /* ordinamento */                                                                                       \
  P(GoodQuietLimit, -15563, -30000, 0)                                                                    \
  P(QuietSortDepthMul, 3223, 1000, 8000)                                                                  \
  P(CheckOrderBonus, 13323, 0, 40000)                                                                     \
  P(ThreatOrderMul, 20, 0, 60)                                                                            \
  P(GoodCaptSeeDiv, 19, 4, 64)                                                                            \
  /* potatura prima del ciclo mosse */                                                                    \
  P(RazorMul, 452, 100, 1200)                                                                             \
  P(RfpMaxDepth, 19, 4, 30)                                                                               \
  P(RfpMultBase, 50, 10, 120)                                                                             \
  P(RfpMultDepth, 4, 0, 16)                                                                               \
  P(RfpMultCap, 85, 30, 200)                                                                              \
  P(RfpNoTTHit, 20, 0, 60)                                                                                \
  P(RfpImproving, 2852, 0, 5000)                                                                          \
  P(RfpOppWorse, 335, 0, 1500)                                                                            \
  P(RfpCorrDiv, 198435, 50000, 800000)                                                                    \
  P(RfpBetaWeight, 690, 0, 1024)                                                                          \
  P(NmpDepthMul, 15, 0, 40)                                                                               \
  P(NmpImproving, 47, 0, 150)                                                                             \
  P(NmpBase, 302, 0, 800)                                                                                 \
  P(NmpRedBase, 7, 3, 12)                                                                                 \
  P(NmpEvalDiv, 223, 64, 1024)                                                                            \
  P(NmpVerifyDepth, 16, 6, 30)                                                                            \
  P(ProbCutMargin, 209, 80, 500)                                                                          \
  P(ProbCutImproving, 64, 0, 200)                                                                         \
  P(ProbCutTTMargin, 415, 150, 900)                                                                       \
  /* potatura nel ciclo mosse */                                                                          \
  P(CaptFutBase, 190, 50, 600)                                                                            \
  P(CaptFutDepth, 281, 80, 500)                                                                           \
  P(CaptFutHist, 134, 0, 400)            /* /1024 */                                                      \
  P(CaptSeeDepth, 179, 50, 400)                                                                           \
  P(CaptSeeHist, 34, 0, 120)             /* /1024 */                                                      \
  P(QuietHistPrune, 4045, 1000, 10000)                                                                    \
  P(QuietMainHistMul, 69, 0, 200)        /* /32 */                                                        \
  P(QuietFutDepth, 96, 40, 300)                                                                          \
  P(QuietFutAlpha, 90, 0, 300)                                                                            \
  P(QuietFutBase, 160, 0, 400)                                                                            \
  P(QuietSeeMul, 24, 5, 80)                                                                               \
  /* estensioni */                                                                                        \
  P(SingMarginBase, 73, 20, 150)                                                                          \
  P(SingMarginTtPv, 66, 0, 200)                                                                           \
  P(SingDoubleBase, -2, -100, 100)                                                                        \
  P(SingDoublePv, 225, 0, 500)                                                                            \
  P(SingDoubleQuiet, 152, 0, 400)                                                                         \
  P(SingTripleBase, 60, 0, 300)                                                                           \
  P(SingTriplePv, 279, 0, 600)                                                                            \
  P(SingTripleQuiet, 188, 0, 500)                                                                         \
  P(SingTripleTtPv, 81, 0, 300)                                                                           \
  P(LdseMargin, 76, 0, 400)              /* BAKE LDSE 05/10/2026 (era 0, vedi in testa): a profondita' <= */ \
  P(LdseDepth, 7, 1, 12)                 /* LdseDepth la mossa hash con limite inferiore si estende se    */ \
  P(LdseMode, 1, 0, 7)                   /* l'eval sta sotto alpha - m. Mode (era 0) bit: 1 solo nodi cut, */ \
                                         /* 2 solo limite inferiore, 4 non da quiescenza                 */ \
  P(LdseMax, 1, 0, 8)                    /* BAKE 05/10/2026 (era 0, vedi in testa): tetto alle estensioni */ \
                                         /* Ldse lungo una linea (0 = nessun tetto)                      */ \
  P(NegExtMax, 0, 0, 8)                  /* 08/10/2026: tetto per linea alle estensioni da riduzione negativa */ \
                                         /* (fino a +2 ply), come LdseMax; 0 = nessun tetto (albero di prima) */ \
  /* riduzioni (1/1024 di ply) */                                                                         \
  P(LmrLogMul, 2872, 1500, 4000)         /* /128 */                                                       \
  P(LmrDeltaMul, 577, 0, 1500)                                                                            \
  P(LmrNotImproving, 197, 0, 600)        /* /512 */                                                       \
  P(LmrBase, 998, 0, 2500)                                                                                \
  P(LmrTtPvAdd, 929, 0, 2500)                                                                             \
  P(LmrTtPvSub, 2915, 0, 6000)                                                                            \
  P(LmrTtPvPv, 1004, 0, 3000)                                                                             \
  P(LmrTtPvAlpha, 885, 0, 3000)                                                                           \
  P(LmrTtPvDepth, 816, 0, 3000)                                                                           \
  P(LmrTtPvCut, 940, 0, 3000)                                                                             \
  P(LmrOffset, 697, -1500, 2500)                                                                          \
  P(LmrMoveCount, 60, 0, 300)                                                                             \
  P(LmrCorrDiv, 26310, 5000, 100000)                                                                      \
  P(LmrCutNode, 4093, 1000, 7000)                                                                         \
  P(LmrCutNoTT, 933, 0, 3000)                                                                             \
  P(LmrTTCapture, 1079, 0, 3000)                                                                          \
  P(LmrCutoffBase, 264, 0, 1500)                                                                          \
  P(LmrCutoffMany, 1095, 0, 3000)                                                                         \
  P(LmrCutoffAll, 1138, 0, 3000)                                                                          \
  P(LmrTTMove, 2037, 0, 5000)                                                                             \
  P(CausalRed, 512, 0, 4096)             /* BAKE 10/10/2026 (era 0, vedi in testa): 1/1024 ply in piu' alle  \
                                            quiete fuori dalle case della confutazione di una sorella (13_search.inc,\
                                            causal_mask; 0 = spenta) */                                             \
  P(LmrStatDiv, 377, 100, 1500)          /* /4096 */                                                      \
  P(LmrAllNode, 276, 0, 800)                                                                              \
  P(LmrNoTTFull, 1127, 0, 3000)                                                                           \
  P(DeeperMargin, 51, 0, 200)                                                                             \
  P(ShallowerMargin, 8, 0, 60)                                                                            \
  /* quiescenza */                                                                                        \
  P(QsFutMargin, 312, 100, 600)                                                                           \
  P(QsSeeMargin, 74, 0, 300)                                                                              \
  /* correction history */                                                                                \
  P(CorrPawnW, 17291, 0, 40000)                                                                           \
  P(CorrMinorW, 10827, 0, 40000)                                                                          \
  P(CorrNonPawnW, 11613, 0, 40000)                                                                        \
  P(CorrContW, 8022, 0, 30000)                                                                            \
  P(CorrMinorUpd, 150, 0, 400)           /* /128 */                                                       \
  P(CorrNonPawnUpd, 186, 0, 400)                                                                          \
  /* Correzioni, proposte del rapporto docs/audit_8.0/N_CORREZIONI.md (06/10/2026); spente = albero invariato */ \
  P(CorrCont6W, 0, 0, 30000)             /* continuazione anche con la nostra mossa a -6 (0 = spenta) */  \
                                         /* 06/10: =4011 a 15+0.15 -0,80 +- 5,56 / 3.894 (socket in disaccordo, */ \
                                         /* fermata; costo NPS -0,1%): spenta, da rivalutare                    */ \
  P(CorrCont6Upd, 38, 0, 256)            /* passo della continuazione a -6, /128 */                       \
  P(CorrContOppW, 0, 0, 30000)           /* idea dell'utente (06/10): continuazione col PIANO avversario, la */ \
                                         /* coppia (sua mossa a -3, sua mossa a -1); 0 = spenta          */ \
                                         /* =4011 a 12+0.12 -5,15 +- 12,68 / 742; =8000 a 8+0.08 -4,81 +- 8,67 */ \
                                         /* / 1.806. Le celle hanno segnale (corr. col residuo +0,24), costo  */ \
                                         /* NPS ~1%, albero invariato: spenta, va nello SPSA CORR1 da 4011     */ \
  P(CorrContOppUpd, 70, 0, 256)          /* passo della continuazione avversaria, /128 */                 \
  P(CorrLearnMode, 0, 0, 15)             /* bit 1: niente se escluso senza mosse; 2: niente nei nodi esclusi; */ \
                                         /* 4: niente da valori decisi; 8: nodi PV esatti in ogni direzione   */ \
                                         /* 06/10 a 12+0.12: =1 non cambia il bench (saltata); =2 -1,64 +-    */ \
                                         /* 7,80 / 2.114 (spenta); 4 e 8 non ancora provati                  */ \
  P(CorrExpertW, 0, 0, 40000)          /* tabella dei pedoni per esperto che si AGGIUNGE alla condivisa (0 = */ \
                                         /* spenta); entra solo nella valutazione, non nella selettivita'       */ \
  P(CorrExpertUpd, 128, 0, 400)          /* passo della tabella per esperto, /128 */                      \
  P(CorrExpertBands, 4, 2, 4)            /* 4 = un esperto per fascia; 2 = due fasce (<=15 / >=16 pezzi) */ \
  /* Costanti dell'aggiornamento delle correzioni, prima scritte fisse nel codice: ora leve per lo SPSA CORR1 (rapporto \
     N_CORREZIONI 4.3). I default sono i valori di prima: albero identico. */                                \
  P(CorrPawnUpd, 128, 32, 256)           /* passo della tabella dei pedoni, /128 */                       \
  P(CorrCont2Upd, 130, 0, 256)           /* passo della continuazione a -2, /128 */                       \
  P(CorrCont4Upd, 70, 0, 256)            /* passo della continuazione a -4, /128 */                       \
  P(CorrBonusHi, 12, 4, 32)              /* bonus per profondita' quando c'e' una mossa migliore, /128 */ \
  P(CorrBonusLo, 18, 4, 40)              /* bonus per profondita' nel fail-low, /128 */                   \
  P(CorrBonusMaxHi, 256, 64, 600)        /* tetto del bonus positivo */                                   \
  P(CorrBonusMaxLo, 256, 64, 600)        /* tetto del bonus negativo */                                   \
  P(CorrSingMul, 177, 0, 600)            /* bonus del multi-cut della singolare per profondita', /1024 */ \
  P(SingCorrDiv, 198368, 50000, 800000)  /* |signal| nei margini doppio/triplo della singolare */         \
  /* 08/10/2026: il termine della continuazione dopo la mossa nulla, prima la costante 64049, e' una leva. Serve allo \
     SPSA CORR1: aggiungendo CorrCont6W la correzione totale cresce (da noi entra anche in RFP, LMR e nei margini     \
     della singolare), quindi i pesi vanno riequilibrati insieme e non uno alla volta come nella prova del 06/10.     \
     Il riequilibrio con -6 e' anche nel lavoro di Stockfish dopo la 19; i valori li cerca il nostro SPSA. */          \
  P(CorrNoMoveW, 64049, 0, 200000)       /* termine della continuazione dopo la mossa nulla */            \
  /* tecniche nostre sopra la ricerca nuova (04/10/2026), spente = albero invariato */                    \
  P(KingShield, 5371, 0, 40000)             /* malus d'ordinamento al pedone che lascia lo scudo del re */   \
  P(KingShieldNpm, 3600, 0, 8000)        /* ...solo se l'avversario ha almeno questo materiale non pedoni */ \
  P(OutpostOrder, 4179, 0, 40000)           /* 08/10/2026 (utente): bonus d'ordinamento al cavallo che va su un */ \
                                         /* avamposto (difeso da un pedone, mai attaccabile da pedoni) */     \
  P(OutpostSafe, 1, 0, 2)                /* 1: anche senza alfieri avversari del colore della casa; 2: e senza cavalli */ \
  /* Ordinamento con conoscenza scacchistica (08/10/2026, utente, dopo gli avamposti), SPENTE di default. AttackOrder \
     e AttackOrderQ: l'idea di premiare la quieta che attacca da una casa sicura viene da Reckless (SPRT pubblicati   \
     +3,7 e +5,7 a 40+0.4); qui con una regola sola: bersaglio = pezzo nemico (non pedone, non re) piu' pesante o non \
     difeso. A luglio, nella ricerca della 7.0, -9,7 +- 10,4 su 1.290 partite: si riprova sulla ricerca nuova.        \
     PassedPush (spinta del passato), OutpostOrderB (avamposto dell'alfiere), BishopPairCapt (cattura di un alfiere  \
     della coppia) sono nostre; PromoOrder (promozione a donna avanti fra le tattiche) e' anche in Reckless. */         \
  P(AttackOrder, 5807, 0, 40000)            /* pedone, cavallo, alfiere, torre che attaccano un bersaglio da casa sicura */ \
  P(AttackOrderQ, 6371, 0, 40000)           /* ...la donna (bersagli non difesi)                               */ \
  /* AttackSafeSee (08/10/2026 sera, verifica docs/audit_8.0/Y2_VERIFICA_ORDINAMENTO.md): con 0 la casa d'arrivo deve \
     essere libera da ogni attacco nemico, ma il bersaglio vede quasi sempre la casa da cui lo si attacca (pedone su   \
     alfiere, pezzo su donna: 40% degli attacchi a pezzi piu' pesanti mai premiati). Con 1 la casa non deve essere     \
     attaccata da pezzi di valore minore e lo scambio sulla casa non deve perdere materiale (SEE >= 0). Lo SPRT del    \
     trio (08/10, +0,39 +- 5,42) era con 0. */                                                                        \
  P(AttackSafeSee, 1, 0, 1)                                                                               \
  P(PassedPush, 3975, 0, 40000)             /* pedone passato che avanza su una casa che non perde materiale  */ \
  P(PassedPushRank, 6, 4, 7)             /* ...dalla traversa (relativa) PassedPushRank in su              */ \
  P(OutpostOrderB, 1078, 0, 40000)          /* alfiere su un avamposto (stessa maschera dei cavalli)          */ \
  P(PromoOrder, 2412, 0, 40000)             /* promozione a donna, in piu' nel punteggio delle tattiche        */ \
  P(BishopPairCapt, 601, 0, 4000)          /* cattura di un alfiere quando l'avversario ha la coppia          */ \
  /* Audit X3 (08/10/2026, docs/audit_8.0/X3_DOVE_PERDIAMO.md), SPENTE di default: a 0 albero e tempi identici.       \
     Contro SF19 perdiamo i punti in difesa con poco orologio, soprattutto fra 10 e 23 pezzi. ProgressFade:          \
     smorzamento in piu' col contatore delle 50 mosse oltre ProgressFrom (12_quiesce.inc). TmDefendMul: piu' tempo   \
     quando la radice e' in difesa. TmEarlySave: quota ottimale ridotta con >= 24 pezzi (14_deepen.inc). */          \
  P(ProgressFade, 0, 0, 64)              /* /1024 per semimossa oltre ProgressFrom (0 = spenta)            */ \
  P(ProgressFrom, 10, 0, 80)             /* semimosse del contatore da cui parte lo smorzamento in piu'    */ \
  P(TmDefendMul, 0, 0, 1000)             /* /1000: tempo della mossa x (1 + Mul) in difesa (0 = spenta)     */ \
  P(TmDefendFrom, 178, 0, 2000)          /* radice <= -From (178 ~ mezzo pedone)                           */ \
  P(TmDefendTo, 1424, 200, 4000)         /* ...e > -To (oltre e' persa: tempo normale)                     */ \
  P(TmEarlySave, 0, 0, 500)              /* /1000: quota ottimale x (1 - Save) con >= 24 pezzi (0 = spenta) */ \
  P(HashDamp, 41, 0, 200)                /* BAKE 06/10/2026 (era 0, vedi in testa): taglio da limite      */ \
                                         /* inferiore spinto verso beta (decimi)                         */ \
  P(HashFiftyStep, 0, 0, 50)             /* nostra (05/10/2026): la chiave hash porta la fascia del contatore */ \
  P(HashFiftyFrom, 40, 0, 100)           /* delle 50 mosse, una ogni Step semimosse sopra From (0 = spenta) */ \
  P(PassedPushRed, 1125, 0, 4096)           /* finali: meno riduzione e niente potatura per la spinta di un */ \
  P(PassedPushNpm, 5133, 0, 12000)       /* pedone passato in 6a/7a, sotto questo materiale non pedoni   */ \
  /* Idee nostre del 04/10/2026 (docs/audit_8.0/M_STUDIO_IDEE_RICERCA.md), tutte SPENTE di default:         \
     a 0 l'albero e' identico (bench 141196) e nella release il codice sparisce. Si accendono una per volta  \
     negli SPRT del piano (STATO_PROGETTO.md 4.0quinquies). Tolte il 05/10/2026 perche' negative (§3 dello     \
     stato): 1 Disagree* (entrambi i segni), 4 PhaseEdgeCapMargin e PhaseEdgeImproving, 5 PassedReplyRed,       \
     6 NmpVerifyDepthEnd/Npm, 10 LateBranch, e HashNearMiss. */                                               \
  P(DeepKnee, 16, 4, 40)                 /* idea 2 (nostra, LTC): da questa profondita' di radice...     */ \
  P(DeepSpan, 12, 1, 40)                 /* ...in questi ply il peso dei termini Deep* sale da 0 a 1      */ \
  P(DeepRfp, 0, -200, 200)               /* scostamenti "profondi" (0 = spenti), li cerca un nostro SPSA LTC */ \
  P(DeepFut, 0, -200, 200)                                                                                \
  P(DeepLmr, 0, -2048, 2048)             /* /1024 di ply */                                               \
  P(DeepNmp, 0, -2048, 2048)             /* /1024 di ply */                                               \
  P(DeepSing, 0, -60, 60)                                                                                 \
  /* 08/10/2026 sera (docs/audit_8.0/Y1_RICERCA_PROFONDA.md), per lo SPSA DEEP1: DeepRfp ora fuori dal tetto del      \
     moltiplicatore (dentro, il tetto scattava da profondita' 9 e la leva agiva quasi solo in un verso); DeepNmp      \
     continua (arrotondamento coi bit della chiave, una frazione |v|/1024 dei nodi cambia di un ply); tre leve nuove   \
     sui termini che crescono di valore col tempo di riflessione. Tutte a 0 = albero invariato. */                    \
  P(DeepHist, 0, -800, 800)              /* /1024: peso della storia nella profondita' ridotta della futility */   \
  P(DeepTtPv, 0, -2048, 2048)            /* /1024 di ply: riduzione in meno nei nodi ttPv                 */      \
  P(DeepExt, 0, -120, 120)               /* margini delle estensioni doppia e tripla della singolare      */      \
  P(Contempt, 0, 0, 200)                 /* idea 3: patta = -c per il colore di radice (Elo di lista) */  \
  P(PhaseEdgeNoFut, 0, 0, 1)             /* idea 4: la cattura che cambia esperto della rete non si pota e non si riduce di piu' */ \
  P(QsPasserPush, 0, 0, 1)               /* idea 7: in quiescenza la spinta in 7a di un passato e' tattica */ \
  /* 10/10/2026, leve della spinta in quiescenza per uno SPSA (agiscono solo con QsPasserPush=1; i default rifanno la  \
     spinta di prima: con QsPasserPush=1 bench 211937). */                                                           \
  P(QsPushNpm, 5133, 0, 12000)           /* soglia di materiale non pedoni propria (prima: PassedPushNpm)  */     \
  P(QsPushSee, 74, 0, 600)               /* la spinta si gioca se SEE >= -QsPushSee (prima: -QsSeeMargin)  */     \
  P(QsPushPly, 64, 0, 64)                /* solo nei primi QsPushPly ply di quiescenza (64 = sempre)       */     \
  P(CorrPhase, 1, 0, 1)                  /* BAKE 06/10/2026 (era 0, vedi in testa): idea 9, correzione dei */ \
                                         /* pedoni/minori separata per esperto                           */ \
  P(CorrFiftyStep, 0, 0, 50)             /* nostra (05/10/2026): correzione di pedoni e non-pedoni separata */ \
                                         /* per fascia del contatore delle 50 mosse (Step semimosse, 0 = spenta) */ \
  /* Audit del nucleo (05/10/2026): gli adattatori intorno alla ricerca (hash, SEE, mossa nulla, valutazione) rivisti \
     uno per uno, ognuno con una forma alternativa. */                                                               \
  P(SeePinned, 1, 0, 1)                 /* BAKE 06/10/2026 (era 0, vedi in testa): 1 = nella SEE i pezzi */ \
                                         /* inchiodati non ricatturano finche' c'e' l'inchiodatore       */ \
  /* HashQsDepth, NullFiftyKeep, EvalRefadeBridge: bakate il 06/10 (vedi in testa) e ritirate la sera come opzioni  \
     (codice fisso in 12_quiesce.inc e 09_history.inc). SeePinned e CorrPhase restano opzioni: prove piu' sottili,  \
     candidate a un'ablazione a TC lunghi. */                                                                       \
  /* Rampa "tempo lungo" (05/10/2026): le leve di selettivita' passano dal valore RW1 (tarato a 20+0.2, radice ~15)   \
     al valore dello SPSA LTC1 (40+0.4, radice ~17) secondo la profondita' di radice: peso 0 fino a LtKnee, 1 da       \
     LtKnee + LtSpan in su, senza estrapolare oltre. Gli Lt sono scostamenti dal valore base: a 0 albero identico, e   \
     nella release il codice sparisce. */                                                                     \
  P(LtKnee, 15, 4, 40)                                                                                     \
  P(LtSpan, 2, 1, 20)                                                                                      \
  P(LmrLogMulLt, 0, -1000, 1000)                                                                           \
  P(RfpMaxDepthLt, 0, -10, 10)                                                                             \
  P(NmpVerifyDepthLt, 0, -10, 10)                                                                          \
  P(LmrNotImprovingLt, 0, -300, 300)                                                                       \
  P(LmrOffsetLt, 0, -1500, 1500)                                                                           \
  P(LmrAllNodeLt, 0, -400, 400)                                                                            \
  P(LmrNoTTFullLt, 0, -1500, 1500)                                                                         \
  P(NmpImprovingLt, 0, -100, 100)                                                                          \
  P(QuietFutAlphaLt, 0, -200, 200)                                                                         \
  /* gestione del tempo (in millesimi) */                                                                 \
  P(TmFallBase, 1148, 500, 2500)         /* /100: fallingEval = (base + a*(prev-best) + b*(iter-best)) */ \
  P(TmFallPrev, 230, 0, 600)                                                                              \
  P(TmFallIter, 110, 0, 400)                                                                              \
  P(TmInstabBase, 1077, 500, 2000)                                                                        \
  P(TmInstabMul, 2229, 0, 5000)                                                                           \
  P(TmFenPly, 0, 0, 1)                   /* 1: ply di partita = numero di mossa della FEN + mosse giocate */ \
  /* Risposta prevista (nostra, 05/10/2026, idea dell'utente): se l'avversario gioca la risposta della nostra linea  \
     principale, la hash e' gia' calda e la mossa che avevamo previsto di solito regge. Il tempo della mossa si        \
     riduce, ma solo quando la ricerca ha ritrovato la profondita' di prima (meno TmExpectDepth), la mossa migliore  \
     e' quella prevista e la valutazione non e' scesa oltre TmExpectMargin. A 1000 spenta: tempi identici. */        \
  P(TmExpectScale, 1000, 200, 1000)      /* /1000: fattore sul tempo della mossa (1000 = spenta) */          \
  P(TmExpectDepth, 2, 0, 8)              /* da profondita' della mossa prima - questo valore */             \
  P(TmExpectMargin, 70, 0, 400)          /* calo massimo della valutazione, unita' dei margini (70 ~ 20 cp) */ \
  /* Sorpresa (nostra, 05/10/2026, idea dell'utente; lo spunto e' un'osservazione di Mark Tang su Stoofvlees, che    \
     risponde subito fra due mosse anche a TC lungo): il rovescio della risposta prevista. Se l'avversario NON gioca  \
     la risposta della nostra linea principale, la posizione e' meno chiara di quanto pensavamo: il tempo della      \
     mossa x TmSurpriseScale/1000. Il tempo che la regola della stabilita' risparmia sulle risposte previste va cosi' \
     alle mosse incerte. A 1000 spenta: tempi identici.                                                              \
     ⚠️ DIPENDE MOLTO DAL TC (utente, 05/10/2026): x1,2 neutra a 16+0.16 (+0,7 +- 7,1 su 2.446), positiva a 40+0.4  \
     (+6,23 +- 6,75 su 2.174, dai PGN +0,35-0,4 ply sulle mosse dopo una sorpresa). Se entra in uno SPSA va tarata  \
     AL TC DI DESTINAZIONE (lungo), oppure tenuta fissa: a TC corto lo SPSA la spingerebbe verso 1000. */            \
  P(TmSurpriseScale, 1200, 1000, 2000)   /* BAKE 05/10/2026 (era 1000, vedi in testa): x1,2 dopo una sorpresa */ \
  /* Soglia (05/10/2026, utente): la sorpresa vale solo finche' l'orologio e' ancora sopra questa percentuale di     \
     quello di inizio partita. Con poco tempo non ci si puo' fermare a pensare: si gioca al tempo ottimale. A 16+0.16 \
     senza soglia, x1,2: +0,7 +- 9,0 su 1.480 partite (dai PGN: sorprese ~25% delle mosse fino alla 50, 52% nei      \
     finali, dove l'orologio e' piu' corto). Con soglia 40 e x1,6 (un ply in piu' sulle sorprese) a 20+0.2: -10,1 +- \
     7,5 su 1.998 partite, chiusa: il tempo preso al mediogioco costa piu' di quanto rende. 0 = sempre. */           \
  P(TmSurpriseClock, 0, 0, 100)          /* % dell'orologio di inizio partita sotto cui la sorpresa tace */  \
  /* Sorpresa graduata (09/10/2026, proposta 5 del registro delle novita', utente: "sviluppala e mettila off"). La   \
     regola della sorpresa e' si'/no; qui il tempo in piu' dipende da quanto la risposta inattesa ha cambiato la      \
     partita: d = punteggio atteso alla fine della ricerca precedente (stessa prospettiva, due mezze mosse dopo) meno \
     il punteggio della radice a questa iterazione. Peso w = clamp(1 + d / TmSurpriseGrade, 0, 2): d = 0 -> w = 1    \
     (come oggi); l'avversario ha trovato qualcosa (d = +Grade) -> w = 2, doppio tempo in piu'; ha sbagliato (d =   \
     -Grade) -> w = 0, nessun tempo in piu'. Tempo x (1 + (TmSurpriseScale/1000 - 1) x w). Si ricalcola a ogni      \
     iterazione, come TmExpect*. Il rango della risposta fra le mosse dell'avversario non e' noto (la ricerca ordina \
     solo le nostre mosse di radice): il punteggio ne e' la misura disponibile. 0 = spenta (regola si'/no). Da       \
     misurare a 40+0.4, dove la sorpresa ha reso; valore di prova 300 unita' interne (~85 cp). */                    \
  P(TmSurpriseGrade, 0, 0, 2000)         /* unita' interne: d per il peso doppio (0 = spenta)              */  \
  /* Ramo con incremento (blitz di CCRL), 05/10/2026: le costanti di clock_plan come parametri per uno SPSA del  \
     tempo contro un motore esterno (in self-play il lato che spende di piu' vince). Default = valori di prima: la \
     divisione IEEE e' arrotondata correttamente (12112 / 1e6 == 0.012112). */                                  \
  P(TmOptBase, 12112, 4000, 30000)       /* /1e6: quota ottimale = base + (ply + 3.23)^pow * c */           \
  P(TmOptPow, 46866, 25000, 70000)       /* /1e5: crescita della quota col ply di partita */                \
  P(TmOptC, 29869, 12000, 50000)         /* /1e7: c = TmOptC + TmOptCLog * log10(secondi sull'orologio) */   \
  P(TmOptCLog, 33554, 0, 90000)          /* /1e8 */                                                         \
  P(TmOptClockCap, 19404, 8000, 32000)   /* /1e5: tetto della quota ottimale sul tempo rimasto */           \
  P(TmMaxCBase, 33744, 20000, 50000)     /* /1e4: tempo massimo = (base + log * log10 s + ply/div) x ottimo */ \
  P(TmMaxCLog, 30608, 0, 60000)          /* /1e4 */                                                         \
  P(TmMaxPlyDiv, 12352, 4000, 30000)     /* /1e3 */                                                         \
  P(TmMaxMultCap, 6873, 3000, 10000)     /* /1e3: tetto del moltiplicatore del massimo (06/10/2026, rapporto */ \
                                         /* Q_TEMPO: sopra ~14 s di orologio decide lui; prova a 5000, 40+0.4) */ \
                                         /* 06/10: 5000 -27,1 +- 17,0 / 308, 8000 -3,6 +- 10,2 / ~950: resta 6873 */ \
  P(TmBehindMul, 600, 0, 2000)           /* /1000 (07/10/2026, idea dell'utente): con meno orologio            */ \
                                         /* dell'avversario l'ottimo si riduce di TmBehindMul x svantaggio   */ \
                                         /* relativo ((noi - lui) / (noi + lui), solo se negativo); 0 = spento */ \
                                         /* BAKED 07/10 a 600: 6+0.06 +5,23 +- 4,72 / 6.378 (zero escluso, */ \
                                         /* s0 +2,5 s1 +8,0); 30+0.3 neutra +0,36 +- 5,13 / 3.876. Bench ugual. */ \
  P(TmRedLo, 496, 0, 3000)               /* /100: rampa di time_red sulla stabilita' della mossa migliore  */ \
  P(TmRedHi, 1879, 500, 4000)            /* /100 (rapporto Q2_TEMPO_INSTABILITA: unico fattore che distingue */ \
                                         /* tranquille e critiche; prova a 1200 = rampa piu' corta, 40+0.4) */ \
  P(TmMaxClock, 8097, 6500, 8800)        /* /1e4: il massimo non supera questa frazione dell'orologio */    \
  /* idea 8: ramo movestogo (il 40/15 di CCRL), finora costanti mai tarate su di noi; default = valori attuali */ \
  P(TmMtgBase, 880, 300, 2000)           /* /1000: quota ottimale = (base + ply*1000/TmMtgPly) / orizzonte */ \
  P(TmMtgPly, 1164, 300, 4000)           /* /10 */                                                        \
  P(TmMtgClock, 880, 300, 1000)          /* /1000: tetto della quota sul tempo rimasto */                 \
  P(TmMtgMaxBase, 1300, 800, 3000)       /* /1000: massimo = (base + TmMtgMaxHorizon*orizzonte) x ottimo */ \
  P(TmMtgMaxHorizon, 110, 0, 400)        /* /1000 */

#ifdef TRIUMV_RELEASE
  #define TP_DECLARE(name, def, lo, hi) static constexpr int name = def;
#else
  #define TP_DECLARE(name, def, lo, hi) static int name = def;
#endif
TRIUMV_SEARCH_PARAMS(TP_DECLARE)
#undef TP_DECLARE

// ---------------------------------------------------------------------------
// Opzioni che non sono parametri numerici della ricerca.
// ---------------------------------------------------------------------------
static int  g_multipv            = 1;
static bool g_show_wdl           = false;
static bool g_ponder_option      = false;    // "Ponder": la GUI pondera -> +25% di tempo ottimale
int         g_syzygy_probe_depth = 1;
int         g_syzygy_probe_limit = 7;
static bool g_syzygy_rule50      = true;
static bool g_eval_off           = false;    // diagnostica NPS: valutazione di solo materiale

// ---------------------------------------------------------------------------
// MODALITA' ANALISI (09/10/2026). Si accende con l'opzione UCI standard UCI_AnalyseMode (le GUI la mettono a true
// quando si analizza), oppure con `go infinite` se AnalyseOnInfinite e' accesa (default accesa). Spenta, ogni
// valore effettivo coincide con quello di partita: albero, bench (430151) e tempo invariati per costruzione.
// Accesa cambia SOLO la selettivita' della ricerca (niente tempo, niente SMP fuori da AnSmp): serve a risolvere piu'
// posizioni delle raccolte di test, non a giocare.
// I componenti (opzioni An*, settabili anche nella release ma elencate solo nelle build di sviluppo) si misurano uno
// per uno: banco tools\solve_suite.py a nodi fissi (2.000.000, 1 thread, Hash 64), raccolte test_suites\iq4.epd e
// test_suites\arasan2026.epd, risolte a 250k/500k/1M/2M nodi. Le misure e la scelta del preset sono qui sotto, accanto
// a kAnDefault.
//
// Origine delle idee. "Profondita' al posto delle potature" in analisi e' ispirata (non portata) dal GoldDigger di
// ShashChess (amchess): qui e' realizzata con le nostre leve Deep* (14_deepen.inc), codice nostro. AnGold e' invece il
// meccanismo del GoldDigger stesso (idea di ShashChess/amchess), riscritto da noi sulla nostra ricerca: vedi
// gd_root_setup in 14_deepen.inc. AnCrystal e' ispirata a Crystal (derivato di Stockfish nato per togliere le cecita'
// da potature aggressive; qui: niente potature lungo la linea principale), idea di altri e codice nostro. Gli scacchi quieti nel primo ply della quiescenza (AnQsChecks), i
// thread SMP a potatura ridotta (AnSmp) e la caccia al matto anticipata (AnHunt) sono nostri.
// ---------------------------------------------------------------------------
enum AnalysisBits {
  AN_EXT = 1,        // margini delle estensioni doppie/triple piu' larghi (DeepExt 60, DeepSing -30)
  AN_NMPV = 2,       // verifica della mossa nulla da profondita' 6 (NmpVerifyDepth 16 -> 6)
  AN_QSCHK = 4,      // primo ply della quiescenza: anche le mosse quiete che danno scacco
  AN_SMP = 8,        // thread helper dispari col preset Deep* piu' largo (menopot)
  AN_CRYSTAL = 16,   // niente potature sulla linea principale precedente nei primi ply
  AN_HUNT = 32,      // hunt_mate da profondita' di radice 8 invece di 16
  AN_GOLD = 64,      // GoldDigger (ShashChess) sulla nostra ricerca
  AN_ACTIVE = 1 << 30
};
// Preset delle leve Deep* (una sola tabella: cambiare qui). DeepKnee 4 e DeepSpan 1: pesi pieni da profondita' 5.
struct DeepPreset { int rfp, fut, lmr, nmp; };
static constexpr int kAnDeepCount = 5;
static constexpr DeepPreset kAnDeep[kAnDeepCount] = {
  {0, 0, 0, 0},              // 0: nessuno (le leve Deep* dei parametri)
  {100, 150, 0, 0},          // 1: prune    (DeepRfp 100, DeepFut 150)
  {0, 0, -1024, 0},          // 2: lmr      (DeepLmr -1024)
  {100, 150, -1024, -1024},  // 3: menopot  (tutte e quattro)
  {100, 150, -1024, 0},      // 4: prunelmr (prune + lmr, mossa nulla invariata)
};
static constexpr int kAnDeepKnee = 4, kAnDeepSpan = 1;
static constexpr int kAnExt = 60, kAnSing = -30;   // AN_EXT
static constexpr int kAnNmpVerify = 6;             // AN_NMPV
static constexpr int kAnHuntFrom = 8;              // AN_HUNT
// Default: prima TUTTO SPENTO (09/10/2026 mattina, il preset si componeva da setoption); dopo le misure qui sotto il
// preset delle leve Deep* e' prune+lmr (kAnDefaultDeep 4) e i componenti An* restano spenti.
// MISURE 09/10/2026 (banco: tools\solve_suite.py, nodi fissi 2.000.000, 1 thread, Hash 64; posizioni risolte a
// 250k/500k/1M/2M nodi; build dev MinGW an_v2, che riproduce la base e prune delle corse su tm21 e release 20261009):
//   iq4 (183):        base 132/141/149/152; prune 145/148/154/155; qschk 140/147/153/154; crystal 137/142/144/147;
//                     hunt 134/142/148/151; gold 141/148/152/154; prunelmr 140/145/149/153; prune+qschk 140/144/154/156;
//                     prune+crystal 141/149/153/156; prune+hunt 143/147/153/153; prune+gold 140/143/149/153;
//                     prune+nmpv 140/148/154/156; prune+ext 143/149/154/159.
//   arasan2026 (200): base 121/139/149/162; prune 136/148/157/168; qschk 128/139/148/159; crystal 127/142/153/162;
//                     hunt 121/139/149/162; gold 131/140/155/166; prune+qschk 127/145/157/168;
//                     prune+crystal 133/145/153/162; prune+hunt 136/148/157/168; prune+gold 136/147/161/167;
//                     prune+nmpv 125/138/149/163; prune+ext 127/138/149/158.
//   Misure delle sole leve Deep* (prune, lmr, menopot, ext, nmpv6) su altre raccolte: test_suites\risultati.
// DEFAULT SCELTO (09/10/2026): AnDeep 4 = prune+lmr (DeepRfp 100, DeepFut 150, DeepLmr -1024), componenti spenti.
// Misure: build release_8.0_analisi, nodi fissi, test_suites\risultati am_*_8M e al_difficili_*_64M (tabella con
// tools\am_tabella.py):
//   - 8M nodi, posizioni risolte in piu' della base (iq4 / arasan2026 / ecmgcp): lmr +4/+4/+4, prune+lmr +4/+3/+7;
//     a 4M prune+lmr +20 in totale contro +14 di lmr;
//   - le 71 posizioni che la base non risolve a 8M, a 64M: base 15, lmr 28, prune+lmr 36;
//   - scartate: lmr+qschk -1/+2/+2 e lmr+gold -2/+3/-2.
// UCI_AnalyseMode e go infinite (AnalyseOnInfinite) usano entrambi prune+lmr; in partita an_bits = 0 e niente cambia.
// Verifica: bench 430151 invariato; con UCI_AnalyseMode=true il bench da' 632173 (quello di prunelmr).
static constexpr int kAnDefaultDeep = 4;
static constexpr int kAnDefaultBits = 0;
static bool g_analyse_mode     = false;   // UCI_AnalyseMode
// AnalyseOnInfinite accesa di default (09/10/2026, utente: "go infinite deve lavorare come la modalita' che scegliamo"):
// molte GUI e i programmi delle raccolte di test analizzano con `go infinite` senza impostare UCI_AnalyseMode. In partita
// `go infinite` non arriva mai (il ponder ha il suo flag, g_limits.ponder), quindi il gioco non cambia.
static bool g_analyse_infinite = true;    // AnalyseOnInfinite
static int  g_an_deep          = kAnDefaultDeep;
static int  g_an_bits          = kAnDefaultBits;
static bool g_data_log_enabled   = false;
static std::string g_data_log_file = "triumviratus_dataset.txt";
void set_data_log_enabled(bool enabled) { g_data_log_enabled = enabled; }

// Globali lette da altre unita' di compilazione (tt.h, see.cpp, movegen.cpp). I nomi restano quelli di prima.
tt_entry* hash_table      = nullptr;
U64       hash_entries    = 0;
bool      g_tt_pow2       = false;
int       current_age     = 0;
bool      g_tt_twolevel   = true;
#ifndef TRIUMV_RELEASE
bool      g_ttmove24      = true;
int       g_tt_keep_margin = 3;   // si sovrascrive se la nuova non e' piu' corta di 4 ply (+2 sui nodi PV)
bool      g_tt_move_keep  = true; // senza mossa nuova si conserva quella vecchia
// Le copie partono dal default del parametro (06/10/2026: con SeePinned bakata a 1 la copia restava a 0 finche' non
// arrivava un setoption, e la SEE restava spenta: bench 222624 invece di 507070).
int       g_see_pinned    = SeePinned;            // copia di SeePinned per see.cpp (vedi td_see_ge)
bool      g_tt_age_refresh = false;
bool      g_see_fix       = true;
#else
// Release (06/10/2026): questa copia e' una costante in see.cpp; deve coincidere col default del parametro.
static_assert(SeePinned == 1, "see.cpp congela g_see_pinned = 1 nella release");
#endif
bool      g_large_pages   = true;
bool      g_ep_key_fix    = true;
static const bool g_evasion_gen = true;

static inline bool parse_bool_value(int v) { return v != 0; }

void print_search_options() {
#ifndef TRIUMV_RELEASE
#define TP_PRINT(name, def, lo, hi) printf("option name " #name " type spin default %d min %d max %d\n", def, lo, hi);
  TRIUMV_SEARCH_PARAMS(TP_PRINT)
#undef TP_PRINT
#endif
}

// Opzioni della modalita' analisi, elencate anche nella release: le combinazioni si compongono da setoption.
void print_analysis_options() {
  printf("option name UCI_AnalyseMode type check default false\n");
  printf("option name AnalyseOnInfinite type check default true\n");
  // Componenti (agiscono solo con UCI_AnalyseMode, o con AnalyseOnInfinite e go infinite).
  printf("option name AnDeep type spin default %d min 0 max %d\n", kAnDefaultDeep, kAnDeepCount - 1);
  static const struct { const char *n; int bit; } an_opts[] = {
    {"AnExt", AN_EXT}, {"AnNmpVerify", AN_NMPV}, {"AnQsChecks", AN_QSCHK}, {"AnSmp", AN_SMP},
    {"AnCrystal", AN_CRYSTAL}, {"AnHunt", AN_HUNT}, {"AnGold", AN_GOLD}};
  for (const auto &o : an_opts)
    printf("option name %s type check default %s\n", o.n, (kAnDefaultBits & o.bit) ? "true" : "false");
}

bool set_search_param(const char* name, int value) {
  if (!strcmp(name, "MultiPV")) { g_multipv = value < 1 ? 1 : (value > 64 ? 64 : value); return true; }
  if (!strcmp(name, "UCI_ShowWDL")) { g_show_wdl = parse_bool_value(value); return true; }
  if (!strcmp(name, "Ponder")) { g_ponder_option = parse_bool_value(value); return true; }
  if (!strcmp(name, "SyzygyProbeDepth")) { g_syzygy_probe_depth = value < 1 ? 1 : value; return true; }
  if (!strcmp(name, "SyzygyProbeLimit")) { g_syzygy_probe_limit = value < 0 ? 0 : (value > 7 ? 7 : value); return true; }
  if (!strcmp(name, "Syzygy50MoveRule")) {
    g_syzygy_rule50 = parse_bool_value(value);
    syzygy_set_ignore_50_move_rule(!g_syzygy_rule50);
    return true;
  }
  if (!strcmp(name, "EvalOff")) { g_eval_off = parse_bool_value(value); return true; }
  // Modalita' analisi: anche nella release (i componenti An* non sono elencati, ma si possono impostare).
  if (!strcmp(name, "UCI_AnalyseMode")) { g_analyse_mode = parse_bool_value(value); return true; }
  if (!strcmp(name, "AnalyseOnInfinite")) { g_analyse_infinite = parse_bool_value(value); return true; }
  if (!strcmp(name, "AnDeep")) { g_an_deep = std::clamp(value, 0, kAnDeepCount - 1); return true; }
  {
    static const struct { const char *n; int bit; } an_opts[] = {
      {"AnExt", AN_EXT}, {"AnNmpVerify", AN_NMPV}, {"AnQsChecks", AN_QSCHK}, {"AnSmp", AN_SMP},
      {"AnCrystal", AN_CRYSTAL}, {"AnHunt", AN_HUNT}, {"AnGold", AN_GOLD}};
    for (const auto &o : an_opts)
      if (!strcmp(name, o.n)) {
        g_an_bits = parse_bool_value(value) ? (g_an_bits | o.bit) : (g_an_bits & ~o.bit);
        return true;
      }
  }
#ifndef TRIUMV_RELEASE
  // Le opzioni lette fuori da threads.cpp (tt.h, see.cpp) hanno una copia globale: si riallinea a ogni setoption.
  const auto sync = [] { g_see_pinned = SeePinned; };
#define TP_SET(pname, def, lo, hi)                                                                         \
  if (!strcmp(name, #pname)) { pname = value < lo ? lo : (value > hi ? hi : value); sync(); return true; }
  TRIUMV_SEARCH_PARAMS(TP_SET)
#undef TP_SET
#endif
  return false;
}

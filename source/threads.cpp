/*
 * TRIUMVIRATUS - Lazy SMP Multi-threaded Search
 *
 * Each thread runs an independent alpha-beta search, sharing only the
 * transposition table. Helper threads (id > 0) diversify their effort via
 * per-thread iterative-deepening depth skipping (LSMP_Skip* tables); the main
 * thread (id 0) drives time management and the PV. The legacy ABDADA busy-node
 * coordination was removed once Lazy SMP proved a clear win (+55 Elo @4CPU).
 */

#include "threads.h"
#include "attacks.h"
#include "evaluation.h"
#include "magic.h"
#include "misc.h"
#include "movegen.h"
#include "nnue_bridge.h"
#include "search.h"
#include "see.h"
#include "tt.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

#ifndef _WIN32
#include <unistd.h> // getpid() (su Windows il pid arriva da GetCurrentProcessId in windows.h)
#endif
#include "io.h"
#include "profile.h"
#include <fstream>
#include <string>

#ifdef TRIUMV_PROFILE
unsigned long long prof_eval = 0, prof_mg = 0, prof_make = 0, prof_tt = 0,
                   prof_score = 0;
unsigned long long prof_ft = 0, prof_fc0 = 0, prof_layers = 0;
unsigned long long prof_catchup = 0;
unsigned long long prof_feat_hist[PROF_FEAT_N] = {};
unsigned long long prof_psq_hist[PROF_PSQ_N]   = {};
unsigned long long prof_acc_inc = 0, prof_acc_refresh = 0, prof_ft_out = 0;
unsigned long long prof_n_inc = 0, prof_n_refresh = 0, prof_n_eval = 0;
unsigned long long prof_n_cols = 0, prof_n_upd = 0;
unsigned long long prof_n_thr_seen = 0, prof_n_thr_dead = 0;
unsigned long long prof_max_active = 0, prof_max_inc = 0;
unsigned long long prof_cols_thr = 0, prof_cols_pawn = 0, prof_n_refresh_calls = 0;
unsigned long long prof_pawn_hit = 0, prof_pawn_miss = 0;
unsigned long long prof_refresh_same_orient = 0, prof_refresh_cross_orient = 0;
unsigned long long prof_dead_pair[8][8] = {};
#endif
#include "defs.h"

#include "syzygy.h"

// ============================================================================
// GLOBALS
// ============================================================================

tt_entry *hash_table = nullptr;

// Stockfish piece codes indexed by Triumviratus piece
// (P,N,B,R,Q,K,p,n,b,r,q,k):
//   white W_PAWN..W_KING = 1..6, black B_PAWN..B_KING = 9..14. Defined here
//   (before td_make_move) so the make-move NNUE mirror hook can use it too.
static const int nn_piece_code[12] = {1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14};

// POLICY-NET: RIMOSSA dal motore (2026-06-11). Il capitolo e' stato chiuso con
// tre misure conclusive (notes/ANALISI_CODICE_OTTIMIZZAZIONI.md §P5): ordering
// −8.7, EntropyTM −16.7 LOS5%, PolicyEasyMove −14.35 LOS0.1% @2520g — la CNN
// (top1 ~24%) non ha segnali Elo-utili ne' per l'ordering (saturo, prior
// statico vs history adattiva) ne' per il time management (la confidenza di una
// rete debole non e' affidabilita'; l'entropia misura l'ignoranza della rete,
// non la complessita'). Il codice vive nella storia git (tag 3.8) per
// l'eventuale R&D futuro (MCTS@root).

// ---- Bundle 3.9 (2026-06-11): micro-fix "correttezza/gratis"
// (P1.4/P1.11/P1.13/ P1.10a/P2.1 dell'analisi). Ognuno dietro toggle default
// ON, ablazione stile 3.8.

// P1.4 Mate-distance pruning: stringe [alpha,beta] coi matti gia' provati piu'
// corti (un matto piu' lungo non puo' migliorare il risultato). 3 righe,
// rischio ~0.
static bool g_mate_dist = true;
void set_mate_dist(bool v) { g_mate_dist = v; }

// P1.11 Draw dither (SF value_draw): le patte per ripetizione/50-mosse
// ritornano ±1cp in funzione del node-count invece di 0 secco -> rompe la
// cecita' da ripetizione (evita oscillazioni 0.00 premature fra linee
// "ugualmente patte").
static bool g_draw_dither = true;
void set_draw_dither(bool v) { g_draw_dither = v; }

// P1.13 History bonus al ttMove quiet su TT-cutoff (SF): un cutoff servito
// dalla TT non passa dal loop mosse -> senza questo la history del ttMove si
// raffredda anche se la mossa continua a tagliare. Scala SPSA-tunable
// (TTCutBonusScale /100).
static bool g_ttcut_bonus = true;
void set_ttcut_bonus(bool v) { g_ttcut_bonus = v; }
int g_ttcut_bonus_scale =
    111; // /100 del td_stat_bonus(depth); spin "TTCutBonusScale"

// P1.10a TT age-refresh al probe-hit: un hit rinfresca l'age dell'entry, cosi'
// le posizioni CALDE ma scritte in search vecchie non vengono evictate per
// anzianita' (SF fa lo stesso). Definito qui, extern in tt.h (usato da
// probe_tt).
bool g_tt_age_refresh = true;
void set_tt_age_refresh(bool v) { g_tt_age_refresh = v; }

// P2.1 Pawn key INCREMENTALE: td.pawn_key mantenuta in make/unmake (XOR dei
// soli pedoni mossi/catturati/promossi, self-inverse come OccIncr) al posto del
// rescan delle bitboard pedoni a OGNI td_corr_index (corr 1x/nodo +
// pawn_history ~30 quiet/nodo). NODE-IDENTICAL per costruzione -> misura NPS a
// depth fissa.
static bool g_pawn_key_incr = true;
void set_pawn_key_incr(bool v) { g_pawn_key_incr = v; }

// CorrNonPawn: np_key[2] (Zobrist non-pedoni PER COLORE) incrementale in
// make/unmake, stesso schema self-inverse di P2.1. Mantenuta SEMPRE (costo ~2-6
// XOR/mossa) cosi' il toggle CorrNonPawn puo' accendersi al volo senza chiavi
// stale. NPKeyIncr=false = fallback rescan per l'oracle node-identity (deve
// dare nodi IDENTICI a parita' di CorrNonPawn, per costruzione).
static bool g_np_key_incr = true;
void set_np_key_incr(bool v) { g_np_key_incr = v; }

// ---- Wave 3b (2026-06-11): P1.1 static-eval-in-TT + P2.2 + P2.3 -------------

// P1.1 (UCI "TTStaticEval", default ON): usa l'eval statica salvata nella TT
// (8 bit: ±508cp passo 4, vedi tt.h) al posto della forward NNUE quando il
// probe la porta. Il motore e' EVAL-BOUND (58% del tempo-nodo = forward): ogni
// hit con eval = una forward risparmiata. Errore ±2cp + staleness-fifty
// accettati (stessa classe dell'eval-cache, gia' ON). Lo STORE dell'eval e'
// incondizionato (gratis); il toggle gata solo l'USO. Cambia i node-count ->
// SPRT.
static bool g_tt_static_eval = true;
void set_tt_static_eval(bool v) { g_tt_static_eval = v; }
bool g_eval_tt_write =
    false; // [5.1 PROVATO E SCARTATO] cache static eval su MISS (SF
           // search.cpp:830): su TT 1-via RADDOPPIA l'albero (2.12M->3.96M
           // @bench14, NON node-neutral) = regressione. SF lo ha gratis col
           // cluster 3-vie; noi no. Default OFF.

// P1.1 FIX fifty-staleness (2026-06-11): l'eval del wrapper SF e' SMORZATA dal
// rule50 (evaluate.cpp: v*(200-fifty)/214) -> salvarla cruda in TT (chiave
// SENZA fifty) la rende stantia: stored a fifty basso, riusata a fifty alto =
// eval inflazionata (misurato: -37% nodi startpos = over-pruning; +1328% nel
// finale bloccato = caos+corr avvelenata). Come SF (unadjustedStaticEval): in
// TT va l'eval DE-smorzata; all'uso si RI-smorza col fifty corrente. Errore
// residuo = solo arrotondamenti interi (~±3cp) invece di v*Δfifty/214 (fino a
// ~237cp).
static inline int tt_eval_undamp(int v, int fifty) {
  if (v == tt_eval_none)
    return v;
  if (fifty > 100)
    fifty = 100;
  return v * 214 / (200 - fifty);
}
static inline int tt_eval_redamp(int v, int fifty) {
  if (v == tt_eval_none)
    return v;
  if (fifty > 100)
    fifty = 100;
  return v * (200 - fifty) / 214;
}
// 5.1 LOSSLESS (EvalTTWrite): l'entry eval-only memorizza l'eval SMORZATA + il
// fifty di store. La si usa SOLO se fifty_store == fifty_now -> il valore e'
// ESATTAMENTE quello che td_evaluate darebbe (stessa posizione, stesso fifty,
// deterministico) = zero differenza = tree-neutro. fifty diverso -> trattata
// come miss (eval fresca): mai un valore approssimato (niente bloat).

// P2.2 (UCI "FastRepScan", default ON): td_is_repetition scandisce solo le
// ultime min(fifty, plies_from_null) entry invece dell'INTERA storia. fifty e'
// un bound esatto (una mossa irreversibile cambia il board per sempre: nessuna
// ricorrenza possibile oltre); plies_from_null esclude i segmenti oltre una
// null move (le null pushano entry "virtuali" senza toccare fifty — il vecchio
// scan completo le attraversava). Semantica = SF; node-count puo' cambiare di
// un soffio -> SPRT.
static bool g_fast_rep_scan = true;
void set_fast_rep_scan(bool v) { g_fast_rep_scan = v; }

// P1.12 (UCI "ThreadVoting", default OFF): selezione del risultato SMP per VOTO
// pesato stile SF invece di "vince il thread piu' profondo". Ogni thread vota
// la sua best move con peso (score - minScore + 14) * depth; vince la mossa col
// totale piu' alto (consenso fra thread > singolo thread profondo). Protezione
// matti: un matto provato vince direttamente (il piu' corto). Inerte a 1
// thread. Default OFF = selezione legacy identica; SPRT a Threads=8
// (convenzione SMP).
static bool g_thread_voting = false;
void set_thread_voting(bool v) { g_thread_voting = v; }

// ---- Toggle DA CO-TUNE per il mega-SPSA 4.0 (2026-06-12)
// --------------------- Strutture SF implementate dietro toggle default OFF =
// byte-identico. NON vanno A/B-ate da sole (lezione two-basin: bolt-on con
// costanti altrui = negativo, es. PawnHistory −18 bolt-on → +Elo dopo co-tune):
// si accendono nel co-tune con le loro costanti nel vettore SPSA, insieme a
// PriorBonus/LowPly.

// P1.3 (UCI "QSChecks"): alla PRIMA ply di qsearch tieni anche i QUIET CHECK
// diretti (check_sq + filtro SEE>=-75 anti-blunder). Tattica forzante vista
// prima, meno mate-blindness (SF: DEPTH_QS_CHECKS).
// QSChecks: scacchi QUIETI generati alla prima ply di qsearch.
// BAKED OFF 2026-07-25: **+9.71 +/- 5.99 Elo, nElo +19.25, LOS 99.93%, LLR 2.96
// (bound raggiunto), 3294 partite @20+0.2** spegnendoli. SF li ha rimossi nel
// lug-2024 (PR #5498, STC 199k/LTC 120k) e da noi la rimozione paga ANCHE di
// piu' di quanto valga per loro.
// ⚠️ Nota metodologica: la misura precedente diceva il CONTRARIO ("da noi
// tagliano: l'albero sul libro CRESCE del 19.2% se li togli") ed era stata fatta
// con QSMoveCap=1, quando la qsearch esaminava UNA sola mossa per nodo: gli
// scacchi quieti competevano con le catture per l'unico slot e potevano evincere
// la miglior cattura. Rimosso quel vincolo (QSMoveCap 1->3, bakato lo stesso
// giorno) il segno si e' ribaltato. Una misura vale solo nel regime in cui e'
// stata fatta.
static bool g_qs_checks = false;
// QSTTQuiets (porta FEDELE di Stormphrax, search.cpp:1557) — default OFF.
// Stormphrax non tiene gli scacchi quieti "sempre" come facevamo noi: genera
// TUTTE le mosse quiete in qsearch, ma SOLO quando la TT ha gia' l'evidenza che
// la mossa migliore e' quieta e il bound e' affidabile:
//     !PvNode && ttMove && ttFlag != UpperBound && !isNoisy(ttMove)
// Il movente da noi e' doppio, ed e' il motivo per cui questa e' l'ultima voce
// aperta della search:
//  (1) esterno: e' la forma che Stormphrax usa al posto del nostro vecchio
//      QSChecks sempre-on;
//  (2) interno: il 25/07 abbiamo spento QSChecks guadagnando +9.71 Elo (bound
//      chiuso @20+0.2) ma la suite hard-position di Maurizio e' calata di 4
//      posizioni. Statisticamente e' rumore (McNemar: servirebbero <=4 discordi
//      totali), pero' il meccanismo e' plausibile — gli scacchi quieti sono cio'
//      che trova le sequenze forzate — e abbiamo un precedente NOSTRO in cui
//      l'SPRT non vedeva il danno: NMPEvalScale, che SF ha revertato per
//      mate-finding dopo 290k partite verdi.
// Una sequenza forzata e' PRECISAMENTE il caso in cui la TT, da una visita piu'
// profonda, ha gia' memorizzato che la mossa chiave e' quieta. Questa condizione
// riammette le quiete li' e solo li': recupera la tattica senza ricomprarsi il
// 21.6% di albero che spegnere QSChecks ci ha fatto risparmiare.
// MODALITA' (misurate sul bench, base 205566):
//   0 = OFF, byte-identico.
//   1 = porta FEDELE Stormphrax: quando la condizione TT vale, ammette TUTTE le
//       quiete. Bench 371449 = +80.7%. Troppo caro da noi: la condizione TT e'
//       rara ma quando scatta apre l'intero ventaglio quieto, mentre il nostro
//       vecchio QSChecks teneva solo le quiete che danno SCACCO. (Stormphrax se
//       lo permette perche' rompe a legalMoves>=2; noi abbiamo QSMoveCap=3 ma
//       paghiamo comunque movegen+scoring di tutte le mosse.)
//   2 = **scacchi quieti TT-gated**: la condizione TT di Stormphrax applicata al
//       filtro NOSTRO (solo quiete che danno scacco diretto, SEE >= -75). E' la
//       forma piu' stretta delle tre e quella che risponde alla domanda vera:
//       recuperare le sequenze forzate SENZA ricomprare l'albero.
static int g_qs_tt_quiets = 0;
void set_qs_tt_quiets(int v) { g_qs_tt_quiets = v < 0 ? 0 : (v > 2 ? 2 : v); }
void set_qs_checks(bool v) { g_qs_checks = v; }

// P1.6 (UCI "NMPVerif"): robustezza null-move — (a) niente due null
// consecutive; (b) a depth >= NMPVerifDepth un fail-high della null va
// CONFERMATO da una search reale ridotta (anti-zugzwang; SF: nmpMinPly). Spin
// NMPVerifDepth.
static bool g_nmp_verif = true;
void set_nmp_verif(bool v) { g_nmp_verif = v; }
int g_nmp_verif_depth = 1; // spin "NMPVerifDepth" (SPSA)

// P1.7 (UCI "LMPImproving"): move-count pruning SF-style (base + d^2*quad/100)
// / (2 - improving), SENZA il cap depth<=8 della lmp_table (oggi oltre d8
// NESSUN move-count pruning = parte del fattore-nodi vs SF). Spins
// LMPBase/LMPQuad, scala comune LMPScale. Two-basin: costanti da co-tunare, non
// copiate.
static bool g_lmp_improving = true;
void set_lmp_improving(bool v) { g_lmp_improving = v; }
// ⭐ LMPCheckGuard (2026-07-18, da scan Reckless v0.9.0+): la LMP pota le quiet
// per move-count SENZA guardare se danno SCACCO. Reckless 5e5443d ("Guard LMP
// for moves that give check") vale +9.46 Elo LTC = il singolo patch piu' grosso
// del loro scan; la stessa famiglia (guard di pruning) aggrega ~+35. Noi il
// guard ce l'abbiamo GIA' sulla LMR (:4649, :4790) ma NON sulla LMP. Costo:
// quando il guard e' ON non si puo' piu' fare skip dell'intero stage quiet (una
// quiet tardiva puo' dare scacco) -> si testa mossa per mossa. Default OFF =
// byte-identico.
static bool g_lmp_check_guard = false;
int g_lmp_base = 16;  // spin "LMPBase"
int g_lmp_quad = 138; // spin "LMPQuad" (/100: 100 = d^2 pieno)

// P1.9 (spin "CheckExtDepth"): gate sulla check-extension incondizionata.
// ✅ BAKATO A 0 (2026-07-19) = estensione da scacco DISATTIVATA, come SF (che
// non ha
//    alcuna check-extension: solo le singolari — verificato su search.cpp
//    master). SPRT `CheckExtDepth=0` vs 30: **+7.98 +/- 8.14 Elo, nElo +16.43,
//    LOS 97.3%, 1654 partite
//    @30+0.3** (c3-88, conc 80, hash 128, UHO_4060_v4, stesso binario A/B).
//    Segno stabile e positivo da ~1000 partite. Misurato a TC LUNGO, dove oggi
//    abbiamo imparato che i risultati reggono (la singular brillava a STC e
//    affondava a LTC: vedi g_singular_exact_margin). EFFETTO ALBERO —
//    attenzione, il bench INGANNA su questa patch:
//      bench 8 posizioni d18: -80.8%  <- MA l'80% e' UNA sola posizione (finale
//      di promozioni) posizioni del LIBRO UHO d18:   -11.5%   d22: -7.0%   <-
//      il risparmio VERO in partita finale promozioni d18: 9.148.851
//      -> 1.062.876 (-88%), da 38x a 4.5x su SF18
//    ⚠️ I valori INTERMEDI sono peggiori di entrambi gli estremi (su
//    Kiwipete d22: 30 -> 2.33M, 8 -> 2.56M, 4 -> 4.59M (+97%!), 0 -> 1.96M):
//    estendere solo sotto una soglia crea profondita' incoerenti che fanno
//    saltare le corrispondenze in TT. La scelta e' BINARIA, niente da tunare.
//    Il vecchio comportamento si riottiene con CheckExtDepth=30 (o qualunque
//    valore >= depth max).
int g_check_ext_depth = 0;

// N1 (UCI "EvalCacheUndamp", default ON): eval-cache con chiave senza fifty e
// valore undamped (vedi td_evaluate). Piu' hit nei finali; OFF = legacy.
static bool g_evalcache_undamp = true;
// EvalCacheOptTag (2026-08-02) — default OFF = byte-identico.
// L'optimism entra DENTRO il valore ritornato da nn_scale (nnue_bridge.cpp:153) e
// g_optimism viene riscritto dal thread 0 a OGNI iterazione ID (threads.cpp:8667+).
// La cache e' pero' indicizzata solo su hash_key: un hit puo' quindi servire una eval
// calcolata con un contempt diverso da quello corrente. Non e' rumore innocuo — a score
// ±2000 l'optimism vale ~68 unita' sulla static eval, piu' di un passo di RFPMargin (53),
// e static_eval alimenta RFP, futility, razoring, improving e la correction history.
// Con il tag, un hit con optimism diverso diventa un miss e si ricalcola.
// ⚠️ E' un tradeoff: meno hit (piu' forward NNUE, che e' il 52% del wall) contro eval
// corrette. Per questo e' un toggle e non una correzione secca.
static bool g_evalcache_opt_split = false;
void set_evalcache_opt_split(bool v) { g_evalcache_opt_split = v; }
void set_evalcache_undamp(bool v) { g_evalcache_undamp = v; }

// N2 (UCI "ProbCutTT", default ON): il fail-high di ProbCut viene SALVATO in TT
// (mossa, score, depth-3, bound beta, eval del nodo) come SF -> le rivisite
// della stessa posizione tagliano dal probe senza rifare qsearch+verifica.
static bool g_probcut_tt = true;
void set_probcut_tt(bool v) { g_probcut_tt = v; }

// P2.3 (UCI "EvasionGen", default ON): sotto scacco genera solo le
// pseudo-legali UTILI (re + catture dello scaccante + blocchi sul raggio;
// doppio scacco: solo re) intersecando le maschere ESISTENTI -> meno make
// falliti. VERIFICATO (2026-06-11, harness simmetrico order-sensitive, 0
// mismatch su pos in-scacco d6 incl. captures-only): i flussi di mosse LEGALI
// (set E ordine) sono identici on/off a ogni chiamata. Residuo ±0.3% di
// node-count da effetti di 2° ordine non-semantici (bookkeeping) -> NON
// node-identical stretto: si valida con l'SPRT del bundle come gli altri
// toggle.
static bool g_evasion_gen = true;
void set_evasion_gen(bool v) { g_evasion_gen = v; }

// Eval-off DIAGNOSTIC toggle (UCI option "EvalOff", default false). Replaces
// the NNUE forward in td_evaluate() with a trivial material count, so an NPS
// test can measure how much per-node time is spent in evaluation (NNUE forward)
// vs the rest (movegen / make-unmake / mirror / search overhead). NOT for play.
static bool g_eval_off = false;
void set_eval_off(bool v) { g_eval_off = v; }

// Static-eval cache on/off (UCI option "EvalCache"). Default on; the toggle
// lets us A/B the cache (off vs on) back-to-back in a single build, immune to
// run-to-run NPS drift between separate builds.
static bool g_eval_cache = true;
void set_eval_cache(bool v) { g_eval_cache = v; }

// FinnyTables (NNUE accumulator refresh cache) on/off (UCI option
// "FinnyTables"). Default OFF => byte-identical to the pre-finny engine.
// Forwards to the bridge (the cache lives per-thread in the SfPos mirror). Pure
// NPS lever: eval is bit-identical either way, so validate with an interleaved
// A/B NPS test.
void set_finny(bool v) { nn_set_finny(v ? 1 : 0); }

// "Improving" heuristic on/off (UCI option "Improving"). Default on; off
// reproduces the exact pre-heuristic search for a clean A/B from one build.
static bool g_improving = true;
void set_improving(bool v) { g_improving = v; }

// Node-based time management on/off (UCI option "NodeTM"). Default on; off
// reproduces the pure best-move-stability time logic for a clean A/B.
static bool g_node_tm = true;
void set_node_tm(bool v) { g_node_tm = v; }

// Advanced singular extensions on/off (UCI option "SingularExt"). Default on;
// off reproduces the plain single-ply singular extension for a clean A/B.
static bool g_singular_ext = true;
void set_singular_ext(bool v) { g_singular_ext = v; }

// Correction history on/off (UCI option "CorrHist"). Default ON (PROVISIONAL):
// the cap=32 / lr-div=512 version leaned +Elo (ultrabullet 2+0.02: ~+5.5, LOS
// ~81%
// @1750 games, CI still touching 0). Ultrabullet UNDERSELLS a depth-dependent
// correction, so adopted provisionally pending a longer-TC (8+0.08) SPRT.
// Toggle off for a clean A/B; tunable via CorrCap / CorrLearnDiv.
static bool g_corr_hist = true;
void set_corr_hist(bool v) { g_corr_hist = v; }

// Multi-table correction history on/off (UCI option "CorrHistMulti"). Default
// OFF reproduces the pawn-only correction. When ON, two extra correction tables
// keyed on minor (N/B) and major (R/Q) material are summed with the pawn table
// before clamping — the net mis-evaluates material configurations too, not just
// pawns.
static bool g_corr_multi =
    true; // BAKED ON (2026-06-05): HM compound +6.2 LOS87.6% @1338
void set_corr_multi(bool v) { g_corr_multi = v; }

// Continuation correction history on/off (UCI option "CorrHistCont"). Default
// OFF = byte-identical (the cont_corr_hist table is never read/written when
// off). When ON, adds the SF continuation-correction term — keyed by the last
// two moves INTO the node (path-dependent static-eval correction) — to the corr
// sum. Its contribution is co-tunable via CorrContWeight (lets a co-tune
// balance cont vs pawn/minor/major).
// BAKED ON (2026-07-22): SPRT +13.97 Elo (MathFix + CorrHistCont + Turbo_x2)
static bool g_corr_cont = true;
void set_corr_cont(bool v) { g_corr_cont = v; }
int g_corr_cont_weight =
    100; // /100 del contributo cont alla somma corr. Spin CorrContWeight.

// Non-pawn correction history PER-LATO (port Pawnocchio/SF
// nonPawnCorrectionHistory, 2026-07-03). Due tabelle extra, chiave = Zobrist
// dei NON-pedoni di UN SOLO colore (re incluso): traccia l'errore-eval della
// configurazione bianca SEPARATAMENTE da quella nera (le esistenti minor/major
// mescolano i due colori in una chiave sola). Era una delle DUE uniche tecniche
// dei rivali studiati assenti da noi (l'altra, HindsightExt, ha pagato +12.7).
// Default OFF = byte-identico; A/B via UCI CorrNonPawn, contributo pesato /100
// via CorrNonPawnWeight (co-tunabile).
static bool g_corr_nonpawn = false;
void set_corr_nonpawn(bool v) { g_corr_nonpawn = v; }
int g_corr_np_weight = 100; // /100 sul contributo delle due tabelle non-pawn

// CorrMaterial (SF #5556 "Material Correction History", default OFF): tavola corr
// indicizzata dalla MATERIAL KEY (hash dei CONTEGGI di pezzo, non posizioni) ->
// generalizza su tutte le posizioni con lo stesso materiale. Segnale nuovo vs
// pawn/minor/major/nonpawn (che sono keyed per POSIZIONE): l'errore di eval nei
// finali/fortezze e' caratteristico del MATERIALE. SF: +Elo STC e LTC (bench
// 1460966). Mira dritto al nostro buco promozioni-endgame (38x nodi vs SF).
// SBAKATA 2026-07-25. Era stata bakata il 24/07 dentro il bundle lean (+10.43
// Elo) misurato a **10+0.1**, con la conferma a TC lungo segnata "in corso" e mai
// registrata. Rifatta a **20+0.2**: il bundle intero misura **-6.89 +/- 9.01,
// LOS 6.70%, 1514 partite** = segno ROVESCIATO. Stesso profilo di MalusQuad
// (+10.31 @10+0.1 -> -4.74 @20+0.2, famiglia poi chiusa): a TC corto questa
// classe di patch e' inflazionata. Fra i tre membri del bundle e' il piu' debole
// in isolamento (piatta @1290g, poi -3.17 LOS 30%) ed e' anche quello con il
// movente peggiore: la correzione da materiale e' informazione che la RETE ha
// gia'. Gli altri due restano ON (GoodCapHistDiv=32, QsStalemateCheck).
static bool g_corr_material = false;
int g_corr_material_weight = 100; // /100 sul contributo della tabella material

// PawnHistory (UCI "PawnHistory", default OFF = byte-identico). Termine di
// ordering per i quiet, pesato 2x come in SF, keyed sulla struttura pedonale.
// Tabella per-thread in ThreadData (SMP-safe). Indice ricalcolato in
// td_score_move dal board CORRENTE (durante lo scoring il board e' sempre
// quello del nodo -> niente staleness da ricorsione).
static bool g_pawn_hist =
    true; // BAKED #1 2026-06-07 (era false): co-tune neutro@8 / +3@20+0.08
void set_pawn_hist(bool v) { g_pawn_hist = v; }
int g_pawn_hist_weight =
    187; // [4.1 BAKE 126->139]   // [3.7 BAKE 195->83] peso pawn-history /100
         // (200 = 2.0x come SF). Spin PawnHistoryWeight (granularita' /100,
         // SPSA-friendly + permette frazioni <1).
// Peso della main (butterfly) history nello scoring quiet. SF la pesa 2x (come
// la pawn); noi storicamente 1x -> con pawn a 2x la pawn DOMINA la main =
// sbilanciato. Spin MainHistWeight per copiare il rapporto SF (main 2x, pawn
// 2x). Default 1 = byte-identico.
int g_mainhist_weight = 93; // [4.1 BAKE 122->168]   // [3.7 BAKE 209->131] /100
                            // (100 = 1.0x; SF usa 2.0x=200)
// Peso della continuation-history nello scoring quiet (ordering). Default 1 =
// invariato. Manopola del CO-TUNE: bilancia conthist vs main/pawn. NON tocca
// conthist in LMR/pruning.
int g_conthist_weight =
    135; // [4.1 BAKE 80->96]   // [3.7 BAKE 134->150] /100 (100 = 1.0x)
// Scala % della soglia LMP (late-move-pruning). Default 100 = invariato. <100
// pota prima (albero più stretto), >100 pota dopo. Co-tune: si ri-equilibra con
// l'ordering nuovo.
int g_lmp_scale = 35; // [3.7 BAKE 93->116]
// Forward-decl: td_corr_index (pawn-only Zobrist bucket) e' definita piu'
// sotto, ma serve qui sopra in td_score_move per la pawn-key.
static inline int td_corr_index(ThreadData &td);
// Forward-decl: td_corr_value serve anche in td_quiescence (P0.4), definita
// dopo.
static inline int td_corr_value(ThreadData &td, int idx);
// Forward-decl: init tabelle cuckoo/between per l'upcoming-repetition (P1.2),
// chiamata da init_threads (Zobrist + magic gia' inizializzati a quel punto).
static void init_cuckoo();

// ProbCut on/off (UCI option "ProbCut"). Default ON: validated +Elo (SPRT
// @8+0.08, LOS ~83% / leaning +6, textbook technique). When on: at sufficient
// depth, if a capture's reduced verification search beats beta+margin, the
// full-depth search would almost surely fail high too, so we prune the node
// early.
static bool g_probcut = true;
void set_probcut(bool v) { g_probcut = v; }

// Continuation-history pruning/reduction on/off (UCI option "ContHistPrune").
// Default ON (PROVISIONAL): tested jointly with CorrHist (ultrabullet ~+5.5,
// LOS ~81%); adopted provisionally pending a longer-TC SPRT. When on: (1) the
// LMR reduction also factors in the continuation history of the move w.r.t. the
// previous move (not just butterfly history), and (2) late quiet moves whose
// combined (butterfly + continuation) history is strongly negative are pruned
// at low depth. Toggle off for a clean A/B; tunable via ContHistDiv /
// HistPruneMargin.
static bool g_cont_hist_prune = true;
void set_cont_hist_prune(bool v) { g_cont_hist_prune = v; }

// Multi-ply continuation history on/off (UCI option "ContHistMulti"). Default
// OFF. When ON, move ordering and the cutoff history updates also use the move
// 2 and 4 plies back (not just 1 ply), giving longer-range "this reply works
// after that sequence" signal. Tables persist like the 1-ply continuation
// history.
static bool g_conthist_multi =
    true; // BAKED ON (2026-06-05): HM compound +6.2 LOS87.6% @1338
void set_conthist_multi(bool v) { g_conthist_multi = v; }

// Staged MovePicker on/off (UCI option "MovePicker"). ADOPTED, default ON:
// validated +16.6 Elo (LOS 96.6% @900 games, 5+0.1, vs the legacy path) once
// the Phase-2 skip_quiets/skip_bad_caps were fixed + tested at a real TC (at
// hyperbullet it was ~0). When ON, moves are produced lazily by stage (TT ->
// good captures/promos -> killers -> counter -> quiets -> bad captures): an
// early cutoff never pays to generate/score the quiet moves, and LMP/futility
// skip the whole quiet stage. Set OFF to reproduce the old generate-all +
// pick-next ordering for an A/B.
static bool g_move_picker = true;
void set_move_picker(bool v) { g_move_picker = v; }

// DiverseSMP (#2): Lazy-SMP helper threads (id>0) search with a small
// per-thread LMR reduction bias, widening the ensemble of trees to cut
// redundant work at high thread counts. Thread 0 stays canonical. Default off
// (behaviour-preserving).
// **BAKED ON (2026-06-04, bake-on-trust):** wider-only diversity (bias<=0, no
// hijack) e' SEMPRE stato positivo (+3..+10, mai negativo) su ~1440 partite a
// 4t+8t. Feature SMP: inerte a 1 thread, agisce solo a multi-thread (=
// condizione gauntlet/torneo). Un [0,5] SPRT non puo' confermare un ~+3 (LLR a
// zonzo), ma il segno e' robusto. Toggle conservato per A/B (off = baseline).
// amount=1.
static bool g_diverse_smp = true;
void set_diverse_smp(bool v) { g_diverse_smp = v; }
static int g_diverse_smp_amount =
    1; // max |bias| in plies (SPSA-tunable: DiverseSMPAmount)
// ⭐ DiverseSMPFine (2026-07-20, default 0 = comportamento storico,
// byte-identico): bias helper in MILLESIMI di ply invece che in ply interi.
// Diagnosi del perche' DiverseSMP non ha mai reso: il nostro bias minimo e' 1
// ply = 1024 unita' LMRFine, mentre Reckless (fb19535) vince con 96/48 unita'
// (~0.05-0.1 ply) e BOCCIA gia' a 512/256. Il nostro "minimo" e' 2-4x la loro
// variante fallita: non e' una perturbazione, e' un helper menomato. Con fine>0
// il bias diventa -mag*fine unita' (wider-only, stessa crescita per coppie di
// id, cap mag=8). Da testare a Threads>=8; a 1 thread e' morto per costruzione
// (bias solo sugli helper). ✅ BAKATO A 96 (2026-07-20): SPRT SMPFINE(96) vs
// BASE(legacy ply-intero) a Threads=8, 16+0.16,
//    hash 256: +4.91 +/- 7.97 Elo (nElo +10.17), LOS 88.63%, 1700 partite, LLR
//    non conclusivo ma lean stabilmente positivo (mai negativo lungo tutto il
//    run). A 8 thread un SPRT locale non chiude in tempi ragionevoli -> bakato
//    sul lean, non su una chiusura formale. A Threads=1 e' byte-identico (il
//    bias vive solo sugli helper, id>0).
int g_diverse_smp_fine = 96;

// --- Search "bundle" #3 — OUTCOME (2026-06-03): tested all-ON (-58 Elo), then
// isolated.
//  * MultiCut   — singular multi-cut, CONSERVATIVE gate (singular_beta >=
//  beta): the only
//                 winner -> +10.4 ±11.2 Elo LOS 96.6% @1100 (1t, 8+0.08). BAKED
//                 default ON.
//  * TripleExt  — +3-ply singular: -75 Elo (tree blow-up). DEAD, kept
//  default-off dormant.
//  * LMREnrich  — extra LMR on ttCapture/cut-node: -25 Elo (over-reduction).
//  DEAD, dormant.
//  * MultiCutAggr (SF gate s>=beta): -13 Elo (half-depth fail-high too weak).
//  REMOVED.
// (TripleExt −75, LMREnrich −25, DeeperShallower: misurati NEGATIVI/morti —
//  RIMOSSI nella pulizia 2026-06-11, vivono nella storia git.)
static bool g_multicut =
    true; // BAKED ON: conservative singular multi-cut (+10.4 Elo @1100)
void set_multicut(bool v) { g_multicut = v; }
// ⭐ MulticutTTMalus (2026-07-20, port SF a47a1c1804, verde a STC+LTC+VLTC-SMP —
// raro). Quando il multicut scatta, la ricerca di esclusione (SENZA la ttMove)
// ha fallito alto DA SOLA: la posizione e' vinta anche senza la mossa che la TT
// dichiarava migliore -> la ttMove non era poi cosi' speciale. Malus alla sua
// history, cosi' le prossime iterazioni la estendono/ordinano meno in alto.
// Default OFF = byte-identico (il ramo multicut gia' esistente e' invariato).
static bool g_multicut_ttmalus = false;
int g_multicut_ttmalus_scale =
    100; // % di td_stat_bonus(depth), come le altre scale malus/bonus

// ttPv (UCI spin "TTPvAmount", 0..2, default 0=OFF). SF: un bit della TT (bit
// 63 del data, era libero) ricorda se un nodo è/è stato PV. I nodi non-PV che
// la TT marca come ex-PV vengono ridotti MENO in LMR (reduction -= amount),
// perché un nodo già stato PV merita più attenzione. Il flag viene propagato
// negli store quando amount>0. REPARAMETRIZZATO (2026-06-24, lesson #14): da
// toggle bool a intero auto-spegnibile — 0=off (byte-identico), 1=−1ply (ex
// comportamento), 2=−2ply. Così il co-tune può dialarlo, e se resta dannoso
// (isolato era −2.6) lo SPSA lo riporta a 0 da solo.
int g_ttpv_amount = 1;

// ===========================================================================
// CANDIDATI "notte" (2026-06-04) — ognuno dietro toggle, default OFF =
// byte-identico al baseline (zero rischio al motore). Da SPRT-are UNO ALLA
// VOLTA (vedi i .bat in Tuning_SPSA/night/). Dato che "il search è spremuto",
// aspettativa media ≤0, ma col toggle OFF non toccano nulla finché non li
// provi.
// ===========================================================================

// (1) NMPEvalScale: aumenta la riduzione del null-move quando static_eval
// supera
//     beta di molto (più sopra beta = potatura più aggressiva). SF-standard.
static bool g_nmp_eval_scale = false;
void set_nmp_eval_scale(bool v) { g_nmp_eval_scale = v; }
// 3/08/2026: allineato alla forma che SF ha fatto PASSARE (356d7c5c),
//   R = 7 + depth/3 + max((ss->staticEval - beta) / 256, 0)
// Prima era /100 CON cap a 3 e sulla `eval` corretta dalla TT: cioe' la variante
// che SF aveva REVERTATO per mate-finding dopo 290k partite verdi.
int g_nmp_eval_div = 256;

// (2) RFPDepth8: estende la reverse-futility da depth<=6 a depth<=8.
static bool g_rfp_depth8 = false;
void set_rfp_depth8(bool v) { g_rfp_depth8 = v; }

// (3) RazorDepth4: estende il razoring da depth<=3 a depth<=4.
static bool g_razor_depth4 = false;
void set_razor_depth4(bool v) { g_razor_depth4 = v; }

// (4) QFutility: futility per-mossa in quiescence — salta le catture che non
//     possono alzare best_score fino ad alpha nemmeno incassando il pezzo
//     catturato (escluse ep/promo per sicurezza). Margine = QFutMargin.
static bool g_qfutility = false;
void set_qfutility(bool v) { g_qfutility = v; }
int g_qfut_margin = 199;
// QFutVicScale (2026-07-24): ponte cp->eval-net del termine-vittima, /10000
// INSIEME a EvalScale — gemello di g_capfut_vic_scale. Sostituisce il vecchio
// magic-number "* 3" hardcoded (tarato a EvalScale=56, mai ri-seguito): 500 a
// EvalScale=60 = 500*60/10000 = ESATTAMENTE 3.0 (byte-identico oggi) ma
// auto-ricalibrante ai bake futuri della rete. Deliberatamente > del capfut
// (2.35): qfut sovrastima per potare MENO le catture grosse (errore sicuro).
// Spin co-tunabile: entra nel co-tune B1-catture insieme a CapFutVicScale.
int g_qfut_vic_scale = 500;

// (5) HistBonusSF: bonus history lineare-clampato min(mult*d - sub, max) invece
//     di depth*depth. Cambia solo la MAGNITUDINE di bonus/malus della history.
//     **BAKED ON (2026-06-04): +24.06 Elo LOS 99.99% @810 (8+0.08, 1t).** Primo
//     +Elo di ricerca dopo una settimana. Toggle conservato per A/B (off =
//     depth*depth).
static bool g_hist_bonus_sf = true;
void set_hist_bonus_sf(bool v) { g_hist_bonus_sf = v; }
int g_hist_bonus_mult =
    490; // [4.1 BAKE 282->326] SPSA co-tune history/LMR @12+0.12 (+5-7 Elo)
int g_hist_bonus_sub = 299;  // [4.1 BAKE 59->35]
int g_hist_bonus_max = 2946; // [4.1 BAKE 1247->2439]

// CaptureHist (UCI option "CaptureHist"). Capture history: tabella
// [piece][to][victim] che impara quali catture producono cutoff, e ne bias-a
// l'ordinamento (sia good che bad captures, main search + qsearch). Default ON,
// con DUE fix (2026-06-04) che hanno tolto una REGRESSIONE da ~-15/-21 Elo che
// il motore spediva:
//   (1) SCALING: il caphist era sommato GREZZO a mvv_lva (range +/-7000 vs
//   100-605
//       -> schiacciava l'MVV-LVA ~10x). `CaptureHistDiv` default 16 lo riporta
//       a
//       +/-437 ~ mvv_lva (= scala SF (mvv*7+capthist)/16). A/B div1 ON vs OFF =
//       -21; div16 (solo scaling) = -12.
//   (2) MALUS: il malus alle catture provate-e-fallite era applicato solo
//   quando il
//       cutoff era causato da una cattura; SF lo applica a OGNI cutoff (anche
//       best quiet). Vedi il loop nell'update. Con (1)+(2): div16+fix ON vs OFF
//       = ~0 (neutro, -0.3@1098 e +0.8@458) -> regressione rimossa.
// Toggle conservato per A/B; OFF = contributo 0 + niente update (tabella a
// zero). `CaptureHistDiv` resta una leva SPSA (8/16/24) per un futuro tentativo
// di +Elo.
static bool g_capture_hist = true;
void set_capture_hist(bool v) { g_capture_hist = v; }
int g_caphist_div = 21; // REVERT 2026-07-23: bake SPSA B1 (15) evaporato a 4452 games (+1.64 LOS73%, LLR piatto) -> torna al default

// Lazy SMP is the ONLY parallel-search scheme (ADOPTED, +55 Elo @4CPU; direct
// A/B
// @2+0.02 4-thread was +102 Elo, LOS 99.99%). Helper threads search fully
// independently (shared TT only) and diversify via per-thread depth skipping
// (see LSMP_Skip* below). The legacy ABDADA busy-node coordination was removed.

// 4-way set-associative TT on/off (UCI option "TT4Way"). Default OFF reproduces
// the direct-mapped table; defined here, declared extern in tt.h (the bucket
// logic lives in tt.h). Matters more now that Lazy SMP has many threads
// hammering the TT. Toggle for a clean A/B.
bool g_tt_4way = false;
void set_tt_4way(bool v) { g_tt_4way = v; }
bool g_tt_twolevel =
    true; // 5.1 BAKE ON (TT2L on vs off: +4.55 LOS 86%% @1758g 12+0.12, mai
          // neg, -4%% nodi). bucket 2-slot depth-preferred + always-replace
bool g_large_pages = true; // LargePages: TT su large pages 2MB (default ON).
                           // OFF = new[] (baseline) per A/B NPS

// Sentinella "nessuna static eval a questo ply" per eval_stack (FIX P0.6):
// scritta ai nodi in scacco quando LazyEval salta la NNUE. Fuori dal range eval
// reale.
#define EVAL_NONE (-32100)

// TTEvalImprove (UCI "TTEvalImprove", default ON — P1.1 2026-06-09): usa
// tt_score (se il bound e' coerente) al posto della static eval nelle decisioni
// di pruning. E' informazione di qualita'-search gia' pagata: SF lo fa da
// sempre. Toggle per A/B.
static bool g_tt_eval_improve = true;
void set_tt_eval_improve(bool v) { g_tt_eval_improve = v; }

// UpcomingRep (UCI "UpcomingRep", default ON — P1.2 2026-06-09): rilevazione
// "ripetizione imminente" (cuckoo, Kervinck/SF has_game_cycle): ai nodi
// non-root con alpha < 0, se esiste una mossa reversibile che riporta in una
// posizione gia' vista, alza alpha a 0 (lato anti-shuffling). Tabelle cuckoo
// inizializzate in init_threads.
static bool g_upcoming_rep = true;
void set_upcoming_rep(bool v) { g_upcoming_rep = v; }

// ---- TOGGLE DI ABLAZIONE dei fix 3.8 (night tournament, 2026-06-09)
// ---------- Ogni fix P0 incondizionato della 3.8 e' gated da un toggle default
// ON: UN solo exe isola ogni fix via fastchess `option.X=false`. Tutti OFF (+
// TTEvalImprove e UpcomingRep OFF) = search-identico alla 3.7 (node-identity
// verificabile).
bool g_ttmove24 = true; // P0.1: TT move 24 bit (OFF = troncamento 21-bit
                        // come 3.7). extern in tt.h.
void set_ttmove24(bool v) { g_ttmove24 = v; }
bool g_tt_move_keep =
    true; // TTMoveKeep (default off = byte-identico): conserva la TT move sui
          // store fail-low senza mossa (SF). Alza ttrate ai cut-node. extern in
          // tt.h.
bool g_see_fix = true; // P0.2: SEE sui quiet + casa e.p. corretta (OFF = SEE=0
                       // sui quiet). extern in see.cpp.
void set_see_fix(bool v) { g_see_fix = v; }
static bool g_killer_lmr_fix =
    true; // P0.3: sconto LMR killer al ply del nodo (OFF = nessuno sconto, come
          // il dead-code 3.7)
void set_killer_lmr_fix(bool v) { g_killer_lmr_fix = v; }
static bool g_qsearch_corr = true; // P0.4: corr-history sullo stand-pat di
                                   // qsearch (OFF = solo nodi interni)
void set_qsearch_corr(bool v) { g_qsearch_corr = v; }
static bool g_improving_fix = true; // P0.6: sentinel EVAL_NONE + fallback ply-4
                                    // (OFF = confronto col vecchio 0)
void set_improving_fix(bool v) { g_improving_fix = v; }

// Lazy eval on/off (UCI option "LazyEval"). Default ON (CONFIRMED: combined
// with TimeMgmt, +9.1 Elo LOS 96% @1+0.01, estimate held over 3k+ games). Skips
// the NNUE static eval in nodes where the side to move is in check: there the
// static eval is not used by any forward-pruning rule (all gated !in_check), so
// computing it is wasted work -> a few % NPS for free.
static bool g_lazy_eval = true;
void set_lazy_eval(bool v) { g_lazy_eval = v; }

// Extra time management on/off (UCI option "TimeMgmt"). Default ON (CONFIRMED
// with LazyEval, +9.1 Elo LOS 96%). The main thread grants extra time when the
// root score DROPS vs the previous completed iteration (the position may be
// turning against us), on top of the existing best-move-instability extension.
static bool g_time_mgmt = true;
void set_time_mgmt(bool v) { g_time_mgmt = v; }

// Time-management constants, SPSA-tunable (mai tarate; default = valori a
// mano). Statici (usati in parse_go, uci_mt.cpp): quota base =
// remaining/TMMovesToGo + inc*TMIncFrac/100; maximum = optimum*TMMaxMult/100.
// Dinamici (loop ID, sotto): instabilita' best-move += headroom*TMInstab/100;
// eval che cala += headroom*d/TMDropDiv.
int g_tm_movestogo = 27; // assunzione moves-to-go quando ignota (minore = piu'
                         // tempo/mossa) [BAKE 2026-07-03 TM post-F-003, TC15
                         // SPSA avg30: 24->27, SPRT +20.0 LOS94%]
int g_tm_inc_frac = 94;  // % dell'incremento da spendere [BAKE 2026-07-03 TM
                         // post-F-003: invariato, 94->94]
int g_tm_max_mult = 614; // maximum = optimum * questo/100 (burst su posizioni
                         // difficili) [BAKE 2026-07-03 TM post-F-003: 592->614]
int g_tm_instab = 59;    // % dell'headroom aggiunta al soft su cambio best-move
                         // [BAKE 2026-07-03 TM post-F-003: 64->59]
int g_tm_drop_div =
    508; // divisore dell'estensione su score-drop (minore = piu' tempo) [BAKE
         // 2026-07-03 TM post-F-003: 570->508]

// T-01 TMStability (default OFF): riduzione del soft-time quando la best move a
// root resta INVARIATA per piu' iterazioni consecutive (SF: bestMove stabile =
// confidenza = si spende meno). Complementare al NodeTM (che guarda i nodi di
// QUESTA iterazione): la stabilita' guarda la persistenza TRA iterazioni.
// Segnale diverso, SF usa entrambi. Curva: dopo `thresh` iterazioni stabili,
// scala la DURATA optimum di -step/1000 per ogni iterazione stabile in piu',
// con pavimento `floor`% (mai sotto). Solo main thread, timeset. Toggle via
// set_search_param (UCI check "TMStability"), come AspAvg/QSDrawCheck.
static bool g_tm_stability = false;
int g_tm_stab_thresh =
    4; // iterazioni stabili prima che la riduzione inizi (SPSA)
int g_tm_stab_step =
    40; // riduzione per-mille per ogni iterazione stabile oltre thresh (SPSA)
int g_tm_stab_floor =
    55; // % minima della durata (mai ridurre sotto questo) (SPSA)

// ---- TM v2 (quarto audit, blocco Q-01..Q-08) — TUTTO default-OFF
// ----------------- Q-01 master `TMv2`: composizione MOLTIPLICATIVA stateless
// (Alexandria/Caissa) al posto della catena additiva
// instab+drop+NodeTM(+TMStability): ogni iterazione i fattori vengono
// ricalcolati FRESCHI dalla durata base (soft_time_limit-starttime), niente
// accumulo -> niente drift, e ogni fattore e' bidirezionale (su posizioni calme
// si spende <1.0x, il risparmio si ricicla sulle mosse difficili). Il vecchio
// path resta intatto per A/B (TMv2=false).
static bool g_tmv2 = true;
// Q-02 stabilita' bestmove extend-AND-reduce in una tabella 5 punti (Alexandria
// {2.38,1.29,1.07,0.91,0.71}): indice = iterazioni consecutive con best
// invariata (cap 4). Sostituisce l'estensione binaria +59% (che era
// extend-only).
int g_tmv2_stab[5] = {232, 172, 82, 112, 56}; // %
// Q-03 trend-eval bidirezionale (bucket Alexandria): counter di iterazioni con
// score entro +-window dalla media mobile avg_score (F-018.5a), cap 4 ->
// {1.25,1.15,1.03,0.92,0.87}: eval ferma = confidenza = risparmia; eval che
// salta (in ENTRAMBE le direzioni, non solo drop) = piu' tempo.
int g_tmv2_eval[5] = {139, 119, 101, 93, 88}; // %
int g_tmv2_eval_window = 10;
// Q-04 NodeTM bidirezionale: (base/100 - frac) * mult/100, clamp [min,max]/100
// (Alexandria 1.52/1.74 -> best che assorbe pochi nodi = alternative vive =
// fino a ~2.5x; best che domina = riduzione come il NodeTM classico).
int g_tmv2_node_base = 155;
int g_tmv2_node_mult = 130;
int g_tmv2_node_min = 36; // clamp inferiore %
int g_tmv2_node_max =
    263; // clamp superiore % (il ramo di ESTENSIONE che ci manca)
// Q-05 allocazione (parse_go, uci_mt.cpp): curva moves-left Caissa (mtg stimato
// cala col progredire della partita: anti overspend-apertura) + pool-increments
// Alexandria (l'inc entra nel montante diviso per mtg, NON come addendo fisso
// -> elimina alla radice la classe di bug F-012).
bool g_tmv2_alloc = true;
int g_tmv2_mtg_base = 35; // moves-left stimate a inizio partita
int g_tmv2_mtg_slope =
    67; // mtg -= slope*fullmove/100 (~35 a mossa 1 -> ~18 a mossa 40)
int g_tmv2_mtg_min = 14;
int g_tmv2_opt_pct = 124; // optimum = pool/mtg * questo/100
// Q-06 predicted-move cross-move (Caissa): a fine search salva l'hash della
// posizione ATTESA dopo pv[0]+pv[1]; al go successivo, avversario ha giocato la
// risposta predetta -> TT calda -> 0.915x, sorpresa -> 1.132x. Azzerato su
// ucinewgame.
bool g_tmv2_pred = true;
int g_tmv2_pred_hit = 945;   // per-mille
int g_tmv2_pred_miss = 1157; // per-mille
// TMPredBestThread: la predizione TM prende la PV dal thread CHE HA VINTO invece che
// sempre dal thread 0. A 1 thread e' identico (il vincitore e' sempre lo 0); a >1 thread
// riaccende una parte di TMv2 che oggi si spegne da sola ogni volta che vince un helper
// (536 volte su 5598 partite a 4 thread, misurato il 3/08/2026). Default OFF finche' non
// e' misurato: e' un cambio di comportamento nel regime multi-thread, che e' quello di
// CCRL (4 CPU nella 40/15 alta, 8 nella Blitz).
bool g_tm_pred_best_thread = false;
U64 g_tm_pred_hash = 0;
// Q-07 root-singularity stop (Caissa, search-based — NON il PolicyEasyMove
// bocciato): dopo timefrac% dell'optimum, a fine iterazione (depth>=9,
// |score|<scorecap), verifica dalla radice a depth/2 con la best ESCLUSA su
// finestra nulla a score - max(marginMin, marginBase - marginSlope*(depth-9)):
// se nessuna alternativa raggiunge la soglia la mossa e' "singolare" -> stop
// immediato.
static bool g_tmv2_rsing = true;
int g_tmv2_rs_margin_base = 405;
int g_tmv2_rs_margin_min = 225;
int g_tmv2_rs_margin_slope = 27;
int g_tmv2_rs_timefrac = 10;
int g_tmv2_rs_scorecap = 2627;
// Q-08: single-reply (unica mossa legale a root -> basta la prima iterazione
// completata, Caissa) + mate-stop (score di matto per N iterazioni consecutive
// -> stop, robusto ai matti instabili; SF ha PERSO partite CCC in attesa di
// questo fix).
static bool g_tmv2_single_reply = true;
static bool g_tmv2_mate_stop = true;
int g_tmv2_mate_iters = 7;

// ---- Gate per-TC del blocco TMv2 (2026-07-12, soglia base-time RIMOSSA 2026-07-24)
// ----------------------------------- Misura originale: TMv2 = -22.94 Elo
// @10+0.1 ma +23.81 @20+0.2 (segno opposto) -> nacque la soglia base-time
// (TMv2MinBaseMs=15000, scelta a mano, mai misurata). Sweep 24/07 su VM
// (screening 8s/12s/15s + conferma dedicata a piena concorrenza @8+0.08,
// nElo +31.10 +/-18.37, LOS 99.95%, LLR 54.3% verso [0,5]): NESSUNA
// regressione a nessun TC testato -> il vecchio cliff non si riproduce col
// codice attuale (probabilmente reso obsoleto dai bake successivi:
// ContHist/EPKeyFix/ecc.). Soglia base-time ELIMINATA; resta solo il gate
// sull'incremento (mai testato separatamente, prudenza invariata: TMv2 usa
// formule pool-increment che presuppongono un inc>0).
bool g_tmv2_tc_ok = true; // ricalcolato ogni parse_go (in uci_mt.cpp)
// TMv2IncGate (2026-08-02): se true (default storico) TMv2 e' spento a incremento zero.
// CCRL 40/15 e' 40 mosse in 15 minuti RIPETUTO, cioe' inc=0: il rating che ci misurano
// gira quindi sul TM v1 additivo, non sul TMv2 che vale +23,8 Elo. False = sempre attivo.
// BAKATO 2026-08-02 true -> false: TMv2 resta attivo anche a incremento zero.
// Misura: +27,55 ± 18,22 su 278 partite @40/40 inc=0 Hash 128 (regime CCRL), LLR 0,66
// in salita, intervallo [+9,3, +45,8] tutto sopra lo zero. SPRT non chiuso, ma il prior
// e' fortissimo: non e' una feature nuova, e' TMv2 (+23,8 Elo misurati nel regime con
// incremento) che questo gate spegneva in TUTTO il regime a inc=0, cioe' in CCRL 40/15.
bool g_tmv2_inc_gate = false;
void set_tmv2_inc_gate(bool v) { g_tmv2_inc_gate = v; }

// ---- Quarto audit, correttezza §D — BAKATI default-ON 2026-07-10
// ---------------- Giustificati su CORRETTEZZA (lettura + matetrack spot-check
// + non-regressione), NON su Elo: sono fix di edge-case che scattano
// rarissimamente, effetto Elo atteso ~0 e sotto la soglia di risoluzione dei
// nostri SPRT (vedi FOURTH_PASS_IMPL_LOG §"pattern negativo"). Bench di
// riferimento: 271715 (2026-07-21, solo S-11 tocca l'albero del bench). Q-29
// (Reckless fec9098): a root, non sovrascrivere un matto gia' provato da una
// iterazione completata con uno score piu' debole (TT collision / potatura).
static bool g_root_mate_restore = true;
// Q-30 (Reckless ae986df): r += 1 quando beta e' gia' decisivo (matefinding).
static bool g_lmr_decisive_beta = true;
// Q-32 (Stormphrax 3d27f98): alla probe TT, uno score decisivo la cui distanza
// di matto supera l'orizzonte regola-50 (100 - fifty plies) non e' realizzabile
// -> declassalo a sotto-decisivo (anti-GHI/shuffle). Complementare a TTCutFifty
// (che gata il CUTOFF, non la conversione dello score).
static bool g_tt_decisive_clamp = true;

// R-01 (quinto audit, SF 94beadff "important for elementary mate finding"): nel
// replacement scheme le entry con score DECISIVO (matto), flag non-EXACT e
// depth>=5 invecchiano piu' in fretta -> la TT non resta satura di score di
// matto stantii. Complementare a TTDecisiveClamp (quello declassa lo score
// letto, questo declassa il RANKING di rimpiazzo). Letto da tt_victim() in
// tt.h.
bool g_tt_secondary_age = false;

static inline int tt_clamp_decisive(int s, int fifty) {
  if (s > mate_score) {
    if (mate_value - s > 100 - fifty)
      return mate_score - 1;
  } else if (s < -mate_score) {
    if (mate_value + s > 100 - fifty)
      return -(mate_score - 1);
  }
  return s;
}
// S-11 (SF): stalemate-resource guard — non SEE-potare la cattura che sacrifica
// il nostro ULTIMO pezzo non-pedone quando siamo sotto (alpha<0). Fires raro.
static bool g_see_stalemate_guard = true;
// S-10 (SF): scrivi il risultato TB (WDL) in TT a depth+6 exact -> le rivisite
// colpiscono la TT invece di ri-probare fathom. NB: tb_score e' ply-embedded e
// sta sotto mate_score -> nessuna ply-normalization in TT (shuffle-risk lieve,
// accettato — vedi FOURTH_PASS_FINDINGS Q-32/S-10).
static bool g_tb_tt_store = true;
// Q-10 (Reckless 08f2cfa + SF d6483505, convergenza indipendente stesso mese):
// la verifica ProbCut gira a depth-1 in piu' quando improving (posizione in
// miglioramento = fail-high piu' credibile = conferma piu' economica basta).
static bool g_probcut_improving = true;
// Q-17 (Caissa): capture-history aggiornata anche ai beta-cutoff di QSEARCH.
static bool g_qs_capthist = true;
int g_qs_capthist_scale = 86; // % di td_stat_bonus(1)/malus — leva SPSA
                              // dedicata (bonus/malus qsearch fissi altrimenti)
// Q-16 (Caissa): extension=1 ai nodi PV se la mossa e' la ttMove ED e' una
// ricattura sulla casa dell'ultima mossa avversaria (gate strettissimo).
static bool g_recapture_ext =
    false; // Q-16 (Caissa): +2.05 Elo LOS 71.9% @2538g 15+0.15, ma LLR mai
           // concluso e raddoppia i nodi (bench 377398->780972) -> evidenza
           // troppo debole per il costo-albero, resta OFF. Riconsiderare dentro
           // il batch C con conferma piu' solida.
// Filtro di TENSIONE (2026-07-12): estende solo se lo scambio e' ~pari (|SEE|
// <= soglia), non ogni ricattura. Uno scambio nettamente sbilanciato (es.
// donna-per-pedone) e' gia' ovvio, l'estensione li' e' spreco puro di nodi; il
// caso utile e' quando la posizione dopo la sequenza resta davvero incerta.
// g_recapture_tension in centipawn, UCI RecaptureTension.
int g_recapture_tension = 70;
// Q-11 NodeCache (Caissa): bonus di ordering alle quiet vicino alla radice,
// proporzionale ai nodi che quella mossa e' costata nelle iterazioni/mosse
// precedenti. Vedi ThreadData::NCEntry (threads.h).
static bool g_node_cache = true;
int g_nc_bonus = 2080; // bonus max (quando la mossa ha assorbito TUTTI i nodi).
                       // Caissa: NodeCacheBonus 4096 [1000,8000]
int g_nc_min_sum =
    1066; // niente bonus finche' nodes_sum non supera questo (Caissa: 256)

// ---- Leve aggiunte 2026-07-10 per entrare nel MEGA co-tune (tutte 0 =
// comportamento
//      storico, bench bit-identico; il co-tune decide se valgono, niente SPRT
//      isolati) ----
// Q-18 (SF PR #6871, gainer): bonus della best move scalato sul numero di mosse
// cercate prima di lei. bonus *= (1024 + coef*moves_before) / 1024. 0 = piatto
// (off).
int g_hist_bonus_moves = 32; // coef, [0,512]
// Q-25 (Stormphrax 665b76b ri-tuna a 750/1024; Pawnocchio 3/4; Caissa 7/8): la
// history DECADE tra una mossa e l'altra invece di restare congelata. 1024 =
// nessun decadimento. Applicata solo a history_moves + capture_history (le
// conthist sono 2.4M entry: scansione troppo cara per il guadagno atteso; se il
// co-tune premia il decay, valutare anche quelle).
int g_hist_age = 940; // scala /1024, [512,1024]
// Q-26 (Caissa MoveOrderer.cpp:5-7, valori SPSA-tunati: quiet 802, cont 762,
// capt 346): history inizializzate a un PRIOR positivo invece che a zero
// ("innocente finche' provato colpevole" per le mosse mai viste). 0 = zero-init
// (storico). Co-tunare CON Q-25: fill e decay interagiscono (il decay tira
// verso 0, il prior sposta il punto di riposo).
int g_hist_init_quiet = 31; // [0,2000]
int g_hist_init_capt = 249; // [0,2000]
// Q-19 (Caissa Search.cpp:1897-1954): addenda al blocco LMRFine, in millesimi
// di ply. (a) ply-scaled PV: riduci MENO vicino alla radice — LMRFine non ha
// NESSUN termine di ply.
int g_lmrf_ply = 524; // [0,2048]  (Caissa: 1024)
// (b) killer/counter: Caissa le riduce ~2.6 ply in meno. Qui in millesimi,
// additivo al
//     `reduction--` intero di KillerLMRFix (che resta).
int g_lmrf_killer = 797; // [0,4000]

// Q-18 helper: bonus piatto -> scalato sul numero di mosse cercate prima della
// best.
static inline int td_hist_bonus_moves(int bonus, int moves_searched) {
  if (!g_hist_bonus_moves || moves_searched <= 1)
    return bonus;
  // moves_searched e' gia' incrementato: 1 = cutoff alla prima mossa (nessun
  // bonus extra).
  return (int)((long long)bonus *
               (1024 + (long long)g_hist_bonus_moves * (moves_searched - 1)) /
               1024);
}

// Aggressive LMR on/off (UCI "AggrLMR"). Default OFF. When ON, the history- and
// continuation-history-based LMR reductions use a SMALLER divisor and a WIDER
// clamp (g_aggr_lmr_div / g_aggr_lmr_clamp), so a very bad-history quiet can be
// reduced by several plies instead of just 1. Off reproduces the baseline
// exactly.
static bool g_aggr_lmr = false;
void set_aggr_lmr(bool v) { g_aggr_lmr = v; }

// ---- SPSA-tunable search parameters -----------------------------------------
// Exposed as UCI spin options (see set_search_param + uci_mt.cpp) so an
// external SPSA tuner (fastchess) can set them per-game without recompiling.
// Defaults = the current hand-set values. After a tuning run, bake the
// converged values in.
int g_rfp_margin = 53;  // reverse futility: static_eval - g*depth >= beta
                        // [SPSA-tuned: 30->21]
int g_razor_base = 272; // razoring: base + mult*depth below alpha -> qsearch
int g_razor_mult = 118;  // [SPSA-tuned: 102->139]
int g_fut_base = 181;   // futility: base + mult*depth (+improving bonus)
                        // [SPSA-tuned: 82->111]
int g_fut_mult = 138;    // [3.7 BAKE 53->41; BAKED #1 66->53]
int g_fut_depth =
    7; // gate profondita' del futility pruning (alzare = potare a nodi piu'
       // profondi = albero piu' stretto). UCI FutilityDepth, tunabile.
int g_fut_improving = 145; // extra futility margin when improving [SPSA-tuned:
                           // 60->93]
// Capture futility pruning (SF master Step 14, ported 2026-06-23). Default OFF
// (A/B). SF prunes a capture at low (reduced) depth when even winning the
// victim can't lift the static eval to alpha. Mirrors our qsearch
// capture-futility (best_score + value(victim) + margin) but in the main
// search. SPSA targets = the cp MARGINS (large range -> tunable);
// g_capfut_depth is a small-int GATE kept fixed (small ints round to noise
// under SPSA).
static bool g_cap_futility = true; // CaptureFutility (UCI spin 0/1)
int g_capfut_base = 156;           // REVERT 2026-07-23 (SPSA B1 evaporato @4452g)
int g_capfut_mult =
    203; // REVERT 2026-07-23 (SPSA B1 evaporato @4452g)
int g_capfut_chist =
    358; // REVERT 2026-07-23 (SPSA B1 evaporato @4452g)
         // [F-002 audit 2026-07-02: 125->123, SPSA+SPRT] capture-history
         // contribution, /1024 (SF=131; our capt-hist clamp +/-7000 -> max
         // ~896cp). Extremi testati: 60=-45 Elo, 140=-25 Elo (LOS 0.4%/2%,
         // entrambi conclusivi) -> ottimo STRETTO attorno al default; 123
         // neutro/lean positivo a 10+0.1 (+0.9) e 20+0.2 (+8.7), mai negativo
         // -> bakato.
int g_capfut_depth =
    8; // REVERT 2026-07-23 (SPSA B1 evaporato @4452g)
// PONTE DI SCALA cp -> unita'-eval per il termine vittima (BUG FIX 2026-07-16).
// see_piece_values[] (see.h) e' in cp CLASSICI (pedone=100); eval/alpha sono in
// unita'-eval, che girano a ~NORM_CP/100 = 3.92x la scala-100 e sono poi
// compresse da EvalScale (oggi 60) -> il fattore netto e'
// g_capfut_vic_scale*EvalScale/10000 = 2.35 a EvalScale=60. Senza ponte la
// vittima entrava a 1x = sotto-pesata ~2.4x -> `fut` troppo basso -> le catture
// BUONE venivano potate, l'esatto contrario di cio' che il commento al sito di
// uso dichiarava. Il gemello in qsearch (g_qfut_margin, ~riga 3360) ha da
// giugno un ponte *3 hardcoded e il suo commento documenta che ometterlo costo'
// -126 Elo: stesso bug, un sito fixato e questo no. Perche' segue EvalScale: *3
// fu tarato a EvalScale=56; a 60 il fattore giusto e' ~3.2. Legandolo a
// nn_get_eval_scale() il ponte non si ri-scalibra al prossimo bake di
// EvalScale. Default 392 = NORM_CP (la costante con cui threads.cpp:~5051
// converte score->cp mostrati): e' la stima onesta, non un numero inventato. E'
// spin perche' l'SPSA lo assesti nel blocco CapFut*.
int g_capfut_vic_scale = 582; // CapFutVicScale, /10000 insieme a EvalScale
// --- Other missing SF-master cut features (ported 2026-06-23, ALL default
// OFF/legacy = byte-identical).
//     SPSA targets = the cp MARGINS (wide range); toggles + small-int gates are
//     NOT primary SPSA targets. Block to be co-tuned together at long TC on the
//     own-lineage net (lesson #13: never gate isolated). ---
static bool g_opp_worsening =
    true; // OppWorsening (SF :841): shave RFP margin when opponent worsening
int g_opp_worse_margin =
    23; // cp shaved from RFP margin when opp worsening (SPSA target)
static bool g_triple_ext =
    true; // TripleExt (SF :1244): +3 singular extension beyond the double
int g_singular_tmargin =
    319; // triple-ext margin below singular_beta (> dmargin) (SPSA target)
// Negative extensions as CONTINUOUS tunable amounts (reach off=0). Default
// (1,0) = legacy
// (-1 on ttMove>=beta, nothing on cutNode). SF = (3,2). SPSA-tunable
// (small-int, calibrated step).
int g_negext_tt = 2;  // NegExtTT (SF=3): -extension when ttMove fails high over
                      // beta; 0=off, 1=legacy
int g_negext_cut = 3; // NegExtCut (SF=2): -extension on a cutNode (ttMove not
                      // fail-high); 0=legacy/off
static bool g_corrval_margin =
    true; // CorrValMargin (SF :980): fold |corr| into RFP margin (prune less
          // when eval heavily corrected)
int g_corrval_rfp = 41; // |corr|*g/128 added to RFP margin (SPSA target)
// CorrValExt (5.0-B, audit SF-master/Reckless 2026-06-24, default OFF): estende
// il fold di |corr| OLTRE l'RFP -> anche futility, SEE, LMR (eval pesantemente
// corretta = incerta -> pota/riduci di MENO). Pesi co-tunabili. OFF =
// byte-identico.
static bool g_corrval_ext = true;
int g_corrval_fut = 38; // futility: margine += |corr|*g/128 (prune less)
int g_corrval_see = 27; // SEE: soglia -= |corr|*g/128 (prune less)
int g_corrval_lmr = 83; // LMR: reduction -= |corr|*g/4096 ply (reduce less)
// ⭐ P1a CorrUncert (2026-07-14, IDEA ORIGINALE, default OFF): il DISACCORDO tra
// le tavole di correzione (pawn vs minor vs major) come segnale di INCERTEZZA
// locale della static eval. Distinto da CorrValMargin/CorrValExt (che usano
// |corr| = bias sistematico NOTO e stabile): qui misuriamo quanto le tavole
// LITIGANO tra loro = il residuo search-eval e' instabile/contestuale in questa
// regione = la static eval e' inaffidabile QUI -> margini di pruning piu'
// larghi (RFP/futility potano di meno). Bassa discordia = eval affidabile =
// pruning pieno. Nessun motore noto usa varianza/disaccordo delle correction
// history.
static bool g_corr_uncert = true;
int g_cu_rfp = 60; // RFP:      margine += uncert * g/64
int g_cu_fut = 71; // futility: margine += uncert * g/64
int g_cu_cap = 70; // cap (cp) del segnale di disaccordo

// ⭐ P4 TroubleMaking (2026-07-14, IDEA ORIGINALE, default OFF): in posizione
// PERSA (score < -Score), se un'altra mossa di root e' costata all'avversario
// uno sforzo COMPARABILE alla best (NodeCache root: nodi >= best*Effort% =
// difficile da refutare) e una verifica null-window a depth/2 conferma che vale
// quasi quanto la best (>= score-Margin), gioca QUELLA: massimizza le chances
// pratiche ("complica la vita") a costo teorico limitato e verificato.
static bool g_trouble = false;
int g_trouble_score = 150; // "persa" = score < -N (unita' search)
int g_trouble_effort = 60; // candidato se nodi >= best_nodes * N%
int g_trouble_margin = 40; // verifica: cand >= score - N

// BrilliantSac (6.0, default OFF): estende (+1) le catture PERDENTI di materiale
// (SEE < 0) ma con capture_history altissima -> sacrifici strutturali che il
// motore ha imparato essere buoni, da proteggere dalle riduzioni LMR pesanti.
// v1 FIX 2026-07-23: il default 10000 non scattava MAI (supera HISTORY_MAX=7000).
// STORIA (2026-07-23): v1 (soglia FISSA 5000) = +2.70+-7.25 LOS 77% @2190g, lean
// troppo debole per chiudere. v2 (soglia SCALATA sull'entita' del sac + !in_check)
// = -17.83 LOS 1.3% @390g, NEGATIVO NETTO (la scalatura estendeva su rumore).
// Ripristinata la v1 come RESTING STATE (default OFF) e candidato co-tune:
// `BrilliantSacMargin` gia' SPSA-ready. Non bakare standalone (+25% albero per un
// lean non confermato); lo SPSA decidera' margine + coordinamento coi margini vicini.
// STORIA SPSA B1_capture_brilliant (2026-07-23, VM trium-eu-96, mirror 16+0.16):
// bakato IN ANTICIPO su due checkpoint concordi (theta1 iter780 +5.82+-12.75 LOS81%;
// theta2 iter1090 +5.95+-10.35 LOS87%, LLR in salita). RICONFERMA a piu' partite:
// a 4452 games l'effetto e' EVAPORATO (+1.64+-5.18, LOS73%, LLR piatto 4.6%) ->
// classico regression-to-mean/winner's-curse. REVERT completo (2026-07-23) a tutti
// i default pre-bake. L'intero blocco (11 param + BrilliantSac) resta candidato
// SOLO per un mega co-tune con kill-switch a metà-run rispettato fino in fondo
// (non un bake anticipato su un trend a 2 checkpoint), non per un retest isolato.
static bool g_brilliant_sac = false;
int g_brilliant_sac_margin = 5000; // caphist >= N (max utile = HISTORY_MAX 7000)
// BrilliantSac-B (reduction-cap, 2026-07-23): invece di ESTENDERE (v1, che tocca
// solo la 1a mossa e gonfia l'albero +25%), protegge il sac strutturale dalla
// RIDUZIONE LMR (reduce-less). Complementare a v1: agisce sui sac ordinati TARDI
// (il caso comune) a costo albero molto minore -> piu' fedele all'intento
// "proteggere dalle riduzioni pesanti". Stesso trigger di v1 (is_brilliant).
static bool g_brilliant_sac_lmr = false;
int g_brilliant_sac_lmr_amt = 2; // plies di riduzione LMR risparmiati sul sac

// MalusScaled (5.0-B, default OFF): il malus history alle quiet
// provate-e-fallite scala col loro move-order (le tardive ricevono malus
// MINORE, come Reckless). OFF = malus piatto = byte-identico.
static bool g_malus_scaled = true;
int g_malus_scale_coef =
    59; // denom = 1024 + coef*indice -> malus = bonus*1024/denom
// ⭐ MalusQuad (2026-07-18, scan Reckless 5e669a6 "Apply decay to quiet move
// malus", STC +4.67 / LTC +5.18): stessa idea del decay che abbiamo gia', ma la
// caduta col move-order e' QUADRATICA invece che lineare -> le quiet molto
// tardive prendono un malus quasi nullo. E' l'unica patch dello scan che sia un
// upgrade diretto di codice nostro gia' presente e gia' positivo. Reckless usa
// coef 45 per il quadratico (il nostro 59 e' tunato per il LINEARE, non
// riusarlo). OFF = decay lineare attuale = byte-identico.
static bool g_malus_quad = false;
int g_malus_quad_coef =
    45; // denom = 1024 + coef*indice -> malus = bonus*(1024/denom)^2
// ⭐ Guard di razoring (scan Reckless: la famiglia "guard di pruning" aggrega
// ~+35 Elo LTC). Il razoring butta il nodo in qsearch quando l'eval e' molto
// sotto alpha. Ma la qsearch vede SOLO catture: se la posizione non e'
// tatticamente ovvia, quel giudizio non vale niente.
//   [1] RazorTTQuiet (2b0812d, STC +3.64 / LTC +2.49): razora SOLO se la TT
//   move e' rumorosa
//       (cattura/promozione). TT move quiet -> la posizione non e' tattica ->
//       NON razorare.
//       ⚠️ Fedele a Reckless: NESSUNA tt move = niente razoring (per loro
//       Move::NULL e' "quiet"). Il nostro razor e' gia' VERIFICATO (giriamo la
//       qsearch e torniamo solo se conferma), quindi e' gia' piu' sicuro del
//       loro: il guard potrebbe rendere meno. Da misurare.
//   [2] RazorTTLower (b2ad933, STC +1.10 / LTC +1.85): in piu', non razorare se
//   il bound in TT
//       e' LOWER (hash_flag_beta = ha gia' fallito alto qui) -> l'eval bassa e'
//       sospetta.
// Entrambi OFF = byte-identico.
static bool g_razor_ttquiet = false;
static bool g_razor_ttlower = false;
// ⭐ SingularExactMargin (3461653, STC +2.58 / LTC +4.94; con 00a31a7 la
// famiglia fa ~+8 LTC): il margine singular oggi e' uguale per tutti i nodi.
// Reckless lo DIMEZZA quando il bound in TT e' EXACT: score esatto = piu'
// affidabile = si puo' essere piu' generosi nel dichiarare singolare una mossa
// (singular_beta piu' alto -> piu' estensioni proprio dove il valore e'
// fidato). ⛔ SBAKATA 2026-07-19 dopo essere stata bakata il 18/07. STORIA
// COMPLETA, da non ripetere:
//    +8.60 +/- 7.62 (LOS 98.65%, 2304g @10+0.1)  ->  -8.60 +/- 10.54 (LOS 5.5%,
//    1010g @20+0.2) Le due misure NON erano in disaccordo: sono la stessa patch
//    sui DUE LATI DI UN CROSSOVER. Bench a profondita' variabile (l'unico
//    strumento che l'ha spiegato):
//        depth 13: -7.4%   depth 17: -11.1%   depth 19: +15.6%   depth 20:
//        +42.7%
//    MECCANISMO: il margine e' `mpd*depth` con mpd=1, quindi dimezzarlo toglie
//    depth/2 e l'aggressivita' SCALA con la profondita'. Peggio: le soglie di
//    doppia/tripla estensione sono misurate DA singular_beta, quindi alzandolo
//    diventa piu' facile anche il doppio e il triplo
//    -> cascata che esplode in profondita'.
//    TENTATIVI DI SALVATAGGIO, entrambi FALLITI in partita:
//      * SingularExactDecouple (doppia/tripla ancorate al margine pieno): d20
//      da +42.7% a +8.5%
//      * SingularExactMaxDepth=18 (cap): d20 a +0.006%, meccanicamente
//      PERFETTO...
//        ...ma SPRT CAP18 vs OFF @20+0.2 = -2.07 +/- 8.79, LOS 32%, 1508g ->
//        nemmeno il cap paga.
//    Conclusione: il beneficio a bassa profondita' non sopravvive quando la
//    ricerca passa il grosso del tempo sopra il crossover. Codice e toggle
//    TENUTI come documentazione della misura. LEZIONE: benchare a piu'
//    profondita' PRIMA di bakare qualsiasi patch di estensioni/riduzioni.
static bool g_singular_exact_margin = false;
// Vedi il commento al sito d'uso: ancora doppia/tripla estensione al margine
// PIENO.
static bool g_singular_exact_decouple = false;
int g_singular_exact_maxdepth =
    0; // 0 = nessun cap. Vedi il sito d'uso: sopra ~18 il dimezzamento esplode.
// DoDeeper/DoShallower (5.0-B, audit SF-master, default OFF): dopo che la
// re-search LMR a piena profondita' fallisce alto, aggiusta la profondita' in
// base a QUANTO batte il best corrente (molto sopra -> +1 ply; appena sopra ->
// -1 ply). OFF = full_depth invariato = byte-identico.
static bool g_do_deeper = false;
int g_dodeeper_base =
    43; // do_deeper se score > best + base + 2*full_depth (SPSA target)
int g_doshallower_margin =
    7; // do_shallower se score < best + margin (SPSA target)
// 5.1 HindsightExt (Pawnocchio): se un nodo non-PV e' stato RIDOTTO di >=
// margine plies e l'eval e' girata male (static_eval + prev_eval < 0),
// ri-estendi (+1): la riduzione era un errore "col senno di poi". Default OFF =
// byte-identico (reduction_stack mai letto).
static bool g_hindsight_ext =
    true; // 5.1 BAKE ON (isolata +12.7 LOS 97.6%% @682g vs 5.1; +61 LOS 100%%
          // vs 5.0). Pawnocchio-style.
int g_hindsight_margin =
    3; // [F-006 audit 2026-07-02: 3->4, SPRT +5.96 Elo LOS75% @408g] ply minimi
       // di riduzione perche' scatti l'hindsight (soglia piu' selettiva = meno
       // ri-estensioni sprecate)
// 5.1 HindsightRed (Stormphrax #300, complemento dell'extend): se un nodo
// non-PV e' stato RIDOTTO
// >= margine plies e l'eval e' girata BENE (static_eval + prev_eval >= soglia),
// RIDUCI (-1): la posizione regge nonostante la riduzione -> risparmia nodi.
// Default OFF (in test). Soglia in scala EvalScale-compressa (Stormphrax usa
// 202 in scala piena ~= 113 nostra).
static bool g_hindsight_red = false;
int g_hindsight_red_margin = 2; // ply minimi di riduzione (Stormphrax usa 2)
int g_hindsight_red_thresh =
    113; // static_eval + prev_eval >= soglia (co-tunabile)
// BadNoisy (5.0-B, audit SF-master qsearch move-count pruning, default OFF): a
// nodo qsearch ordinato best-first, dopo N catture provate le rimanenti sono
// quasi-sempre perdenti -> skip. Niente ricattura/promo. OFF = nessuno skip =
// byte-identico.
static bool g_bad_noisy = true;
int g_bad_noisy_count = 7; // REVERT 2026-07-23 (SPSA B1 evaporato @4452g)
                           // (SPSA target; 3=SF/aggr.)
// LMREnrich (archivio 4.2, audit SF, default OFF): se la TT-move è NOISY
// (cattura/ep/promo) mentre la mossa corrente è quiet (qui sempre, blocco
// is_quiet), le quiet sono meno probabili -> riduci di PIU' (SF: ttCapture &&
// !capture -> r += ...). Su b1a57 era −25 (over-reduction); ri-test sul net
// own-lineage.
static bool g_lmr_enrich = true;
int g_lmr_enrich_amount =
    2; // ply extra di riduzione quando la TT-move è noisy (SPSA target)
int g_razor_quad_coef = 82; // RazorQuadCoef (SF :967): quadratic razor term
                            // (base + mult*d + coef*d^2); 0 = linear/off
int g_rfp_depth_cap = 7; // RFPDepth: >0 overrides RFP depth cap (0=legacy 6/8);
                         // widen toward SF=17
int g_razor_depth_cap = 6; // RazorDepth: >0 overrides razor depth cap (0=legacy
                           // 3/4); widen toward SF (uncapped)
// IID — Internal Iterative DEEPENING (5.1, default OFF). L'IIR economico
// (depth-- senza TT move, linea ~2803) NASCONDE il problema dell'ordinamento
// scadente; l'IID lo RISOLVE: senza TT move fa una mini-ricerca ridotta PRIMA
// per OTTENERE una hash move (ordinamento ~SF), cosi' il taglio "morde"
// (chicken-egg col cut-block). Solo su pv/cut node, con freno anti-cascata (no
// IID dentro IID). OFF = byte-identico (blocco saltato). Costa +nodi (la
// mini-ricerca) ma ordina molto meglio.
static bool g_iid = false;
int g_iid_depth = 4; // IIDDepth: profondita' minima per attivare l'IID
int g_iid_reduction =
    2; // IIDReduction: ply tolti alla mini-ricerca (iid_depth = depth - r)
// Q-15 FollowPV (SF PR #6656): si salva la PV dell'iterazione precedente; i
// nodi che la RISEGUONO (stessa linea di mosse dalla root) NON fanno IIR ne'
// shallow-pruning (futility/LMP): sulla linea principale l'ordinamento e' gia'
// buono e potare li' rischia di scartare la mossa migliore. Flag propagato
// lungo la sola PV-line. 0 = OFF (byte-identico). Usa td.prev_pv[] (salvata a
// fine iterazione).
static bool g_follow_pv = false;
// ---- F-002/F-004/F-005 (audit 2026-07-02): costanti hardcoded promosse a
// tunable + LMR-catture. TUTTI i default = valore hardcoded storico =
// comportamento BIT-IDENTICO (verificato via bench); si accendono solo dal
// tuning/SPRT.
int g_qs_delta = 3000; // F-002: delta-pruning qsearch (era const DELTA=1000; a
                       // scala-56 ~1785cp reali)
int g_singular_mpd =
    1; // F-002: margine singular PER-DEPTH (era hardcoded 2*depth, unita'-eval)
int g_tm_drop_thresh = 6; // F-002: soglia score-drop TM (era hardcoded 8,
                          // unita'-eval) [BAKE 2026-07-03 TM post-F-003: 8->6]
int g_tm_drop_cap =
    160; // F-002: cap score-drop TM (era hardcoded 200, unita'-eval) [BAKE
         // 2026-07-03 TM post-F-003: 200->160]
int g_nodetm_base = 133; // F-002: NodeTM scale = (base - frac*100)/100
                         // (era 1.4) [BAKE 2026-07-03 TM post-F-003: 140->133]
int g_nodetm_min = 58;   // F-002: NodeTM clamp inferiore % (era 0.6) [BAKE
                         // 2026-07-03 TM post-F-003: 60->58]
// F-008 (audit 2026-07-02): SEE a soglia con early-exit (see.cpp: td_see_ge).
// SeeGE OFF (default) = td_see classico, byte-identico. SeeGEVerify = oracle:
// esegue ENTRAMBI i path a ogni chiamata e segnala i mismatch (0 attesi).
bool g_see_ge = true; // [F-008 2026-07-02: ON] +1.81% NPS su test rigoroso (150
                      // pos x2, depth 15, nodi BIT-IDENTICI 73589060=73589060
                      // -> solo velocita', zero rischio search)
bool g_see_ge_verify = false;
static std::atomic<long long> g_seege_checks{0}, g_seege_bad{0};
static void seege_verify_report() {
  if (g_seege_checks > 0) {
    printf("info string SeeGEVerify: %lld checks, %lld mismatch\n",
           (long long)g_seege_checks, (long long)g_seege_bad);
    fflush(stdout);
  }
}
int g_lmr_min_depth =
    3; // F-005: depth minima per la LMR (SPSA: stabile a 3, confermato)
int g_lmr_min_moves = 1; // [F-005 BAKE 2026-07-02: 3->2, SPSA+SPRT in blocco
                         // LMR] riduce dalla 3a mossa (SF dalla 2a)
static bool g_lmr_captures =
    true; // [F-004 BAKE 2026-07-02] LMR anche sulle CATTURE
          // (SF/Stormphrax-style). Vettore SPSA (MinMoves=2, EvalMargin=20)
          // SPRT: +7.38 LOS 87% @800g 8+0.08
// LMRCheckExempt (2026-07-26) — default 0 = byte-identico.
// Entrambi i rami LMR (quiet :7490 e catture :7741) sono gated da
// `!in_check && !gives_check`: **nessuna mossa che da' scacco viene MAI ridotta,
// e in un nodo in scacco nessuna mossa viene ridotta.** SF non ha nessuno dei
// due gate: la sua LMR (search.cpp:1327) e' condizionata solo da
// `depth >= 2 && moveCount > 1`.
// MOVENTE: e' la TERZA esenzione-scacchi del motore, e le prime due, tolte, hanno
// pagato — rimozione della check-extension **+7.98 Elo** (19/07) e rimozione degli
// scacchi quieti in qsearch **+9.71 Elo** (25/07, bound chiuso @20+0.2). Il motore
// era sistematicamente troppo generoso con gli scacchi in tre posti distinti.
//   0 = comportamento attuale (entrambe le esenzioni)
//   1 = si riducono anche le mosse che DANNO scacco  <- testare QUESTA per prima
//       (rischio piu' basso: tocca solo le mosse figlie, non il regime del nodo)
//   2 = si riduce anche nei nodi IN scacco (tattico, molto piu' rischioso)
// ⚠️ Tocca OGNI nodo: bench a profondita' 13/17/20 sul LIBRO dei gate PRIMA di
// qualunque SPRT — e' la regola nata da SingularExactMargin, che stringeva
// l'albero a d13/d17 e lo faceva ESPLODERE (+42.7%) a d20.
int g_lmr_check_exempt = 0;
int g_lmr_cap_scale = 52; // REVERT 2026-07-23 (SPSA B1 evaporato @4452g). F-004: % della lmr_table applicata alle catture (50
                          // = dimezzata; SPSA confermo' ~50)
int g_singular_dmargin = 59; // double-extension margin below singular_beta
                             // [SPSA-tuned: 63->43]
int g_hist_red_div = 3190;   // LMR history-reduction divisor [SPSA-tuned:
                             // 3500->1041]
int g_asp_init_delta = 19; // aspiration: initial window half-width [SPSA-tuned:
                           // 25->31]
// ⭐ AspStableShrink (2026-07-18, scan Reckless 4192617 "Shrink initial
// aspiration window if our search has been stable", STC +4.58 / LTC +2.08). Se
// la ricerca e' stata stabile (stessa best move E score fermo per N iterazioni
// consecutive), la finestra iniziale puo' partire piu' STRETTA: una finestra
// stretta che non fallisce = ricerca piu' economica; se poi fallisce si
// riallarga comunque. Loro: delta 23 - min(eval_stab, pv_stab, 7). Noi delta 19
// -> cap 4 (= il cap del nostro eval_stab_iters) e' la scala equivalente.
// ⚠️ Vive nel ramo TM (thread 0 + timeset) -> a profondita' fissa NON
// scatta: il BENCH NON CAMBIA, non e' un errore. La verifica byte-identica va
// fatta per costruzione, non col bench.
static bool g_asp_stable_shrink = false;
int g_asp_stable_cap =
    4;               // massimo restringimento del delta iniziale (AspStableCap)
int g_asp_grow = 34; // aspiration: growth % on fail [SPSA-tuned: 100->31]
// Q-28 AspScoreMult (Stormphrax 9081510+03d4c83): la finestra iniziale si
// ALLARGA col quadrato dello score (EMA). avgSq = (avgSq + score*|score|)/2
// aggiornato per iterazione; delta += (i64)|avgSq| * mult >> 20. Score grandi
// (verso il matto) -> finestra piu' larga -> meno ri-cerche. 0 = OFF (nessun
// termine, byte-identico). ⚠️ i64 obbligatorio: score*|score| ~9e8, *mult
// sfora i32 (overflow che Stormphrax ha dovuto fixare con un commit dedicato).
// NON accoppiare con AspAvg (revertito).
int g_asp_score_mult =
    38; // >> 20 scale; SPSA range [0,2000], init-when-ON ~100
int g_cmhc_scale =
    6; // Q-12 CMHC: bonus conthist *= (100 + scale*consistenza)/100. 0 = neutro
int g_probcut_margin =
    234; // ProbCut: capture verification must beat beta by this margin
int g_probcut_improve = 4; // Q-20b (Alexandria): abbassa probcut_beta di questo
                           // quando improving (probcut piu' facile). 0 = OFF

// ⭐ GATE STRUTTURALI (2026-07-17): interi hardcoded delle DECISIONI DI
// PROFONDITA' — mai tunati perche' troppo piccoli per SPSA (range<=4 =
// perturbazione annullata), NON perche' ottimi. Esposti qui coi valori attuali
// (byte-identico) per test DISCRETO via SPRT. SF/Berserk li tunano. SCREENING
// 2026-07-17 (12+0.12): l'IIR e' GIA' OTTIMO, tutte e 3 le varianti NEGATIVE.
// Chiuso.
//   IIRNotInCheck=true  -> -5.79  (LOS 3.7%)   il "fix" annulla-check-ext e'
//   negativo a STC IIRMinDepth=6       -> -10.08 (LOS 0.1%)   sparare MENO
//   (gate 4->6) fa male IIRMinDepth=3       -> -14.67 (LOS 0.5%)   sparare PIU'
//   (gate 4->3) fa peggio -> 4 e' il punto giusto
int g_iir_min_depth =
    4; // IIR: depth minima perche' scatti il depth-- senza TT move (:3894)
int g_iir_amount = 1; // IIR: di quanto riduce
static bool g_iir_not_in_check =
    false; // fix bug: IIR NON spara in scacco. Testato -5.79 -> resta OFF
// ⭐ SCREENING 2026-07-17 (12+0.12): gate 5->7 = +12.54 +/-9.52 (LOS 99.5%,
// 1330g) = PRIMO HIT VERO.
//   ProbCut deve sparare MENO (a depth 5-6 la verifica ridotta e'
//   inaffidabile). 5->4 era neutro (-0.46). mind9 = +4.42 (LOS 72%) < mind7 ->
//   picco ~7.
// ⚠️ RICONFERMA 20+0.2 (2026-07-18): il +12.5 e' EVAPORATO. PC7-solo =
// +0.67 (LOS 55%), INCR(PC7+SG4)
//   = +3 sfumando (LOS 76%). Puro TC-decay: a 20s la ricerca profonda rende
//   ProbCut gia' affidabile.
//   -> gate=5 RESTA. L'hit STC vale solo a blitz. Search-side confermato tunato
//   al TC di release.
int g_probcut_min_depth = 5; // ProbCut: depth minima del gate (:4157)
// SCREENING 2026-07-17 (12+0.12): off=4 e' gia' ottimo. off=3 (verifica piu'
// profonda) -1.57 neutro;
//   off=5 (verifica piu' bassa) -8.92 (LOS 5%) fa male. Chiuso: pc_depth =
//   depth-4 sta bene.
int g_probcut_depth_off = 4; // ProbCut: pc_depth = depth - questo (:4205)
// SCREENING 2026-07-17 (12+0.12): ttm=2 (piu' esigente, meno singular) -8.03
// (LOS 7%) male;
//   ttm=4 (meno esigente, PIU' singular) +6.58 +/-8.70 (LOS 93%) = candidato
//   (LOS<95%, da riconfermare 20s).
int g_singular_ttmargin = 3; // Singular: tt_depth >= depth - questo (:4483)
int g_singular_depth_div =
    2; // Singular: singular_depth = (depth-1) / questo (:4486)
// Q-20a (Alexandria): badNode = predicato IIR (nessuna TT move utile) -> il
// fail-high statico e' MENO contraddetto da info profonde -> margine RFP
// ridotto (prune piu' facile).
int g_rfp_badnode = 4; // rfp_margin -= questo se !tt_move. 0 = OFF
// Q-20c (Caissa Search.cpp:1786): la futility NON pota mai la PRIMA quiet del
// nodo (quietIndex>1): almeno una quiet va sempre guardata, il segnale statico
// da solo non basta a chiudere il nodo. Toggle. OFF = comportamento storico.
static bool g_fut_spare_quiet = false;
// Q-20d (Ethereal search.c:872 + move.c:576): delta-pruning qsearch col
// BEST-CASE reale (vittima piu' grossa presente + potenziale promozione) invece
// del margine fisso: se nemmeno il colpo migliore possibile porta sopra alpha,
// il nodo e' morto.
static bool g_qs_delta_bestcase = false;
int g_qs_bc_margin = 183; // cuscino sopra il best-case (scala-56, SPSA)
// Correction-history tunables (only active when CorrHist is on).
int g_corr_cap =
    50; // max correction applied to static eval (cp). [SPSA histmulti 32->44
        // RIGETTATO: -1.3 LOS23% @11438 -> default]
int g_corr_lr_div = 303; // learning-rate divisor (bigger = slower/steadier
                         // learning) [histmulti 512->565 rigettato]
// CorrAsym (Tier-1 corrections, port SF 7a7c033a): le correzioni POSITIVE
// (nudge verso l'alto) imparano piu' lente delle negative. /128 sul peso quando
// (target-cv)>0. 128 = simmetrico = byte-identico (default). Co-tune verso
// ~100-110 (SF ~18% piu' lente). Solo con CorrHist on.
int g_corr_asym = 137;
// Continuation-history pruning tunables (only active when ContHistPrune is on).
int g_conthist_red_div =
    3684; // LMR: continuation-history reduction divisor [SPSA-tuned 5000->6595;
          // histmulti 6595->7050 rigettato]
// Aggressive-LMR tunables (only active when AggrLMR is on).
int g_aggr_lmr_div = 903; // smaller divisor -> wider reduction range
int g_aggr_lmr_clamp = 4; // max +/- ply the history reductions may apply

// ---- StatScore-LMR levers (2026-06-06) --------------------------------------
// Scoperta: vs SF15.1 SOTTO-RIDUCIAMO -> ~6 ply meno profondi a pari tempo. SF
// guida la LMR con uno statScore CONTINUO (somma history / divisore, multi-ply)
// invece del nostro clamp +/-1. Tre toggle INDIPENDENTI (default OFF = byte-
// identico), ognuno A/B-abile da solo; lo SPSA tara i divisori dopo.
static bool g_statscore_lmr =
    true; // BAKED #1 2026-06-07. StatScoreLMR: butterfly history -> riduzione
          // continua (rimpiazza il clamp +/-1)
void set_statscore_lmr(bool v) { g_statscore_lmr = v; }
static bool g_conthist_lmr =
    true; // BAKED #1 2026-06-07. ContHistLMR: conthist(1/2/4 ply) -> riduzione
          // continua, scollegata da ContHistPrune
void set_conthist_lmr(bool v) { g_conthist_lmr = v; }
static bool g_cutnode_lmr =
    true; // CutNodeLMR: riduzione extra sui cut-node (SF li riduce di piu').
          // BAKED ON 2026-06-24 (2o segnale +: on+tuned = +11 @287g su
          // Blackwell).
void set_cutnode_lmr(bool v) { g_cutnode_lmr = v; }
// ---- Threat-ordering (#2 SF, 2026-06-07) ------------------------------------
// SF ordina i quiet anche per MINACCE: muovere un pezzo da una casa attaccata
// da uno di valore INFERIORE e' buono (lo salvi), entrarci e' cattivo (lo metti
// in presa). Struttura SF (regina vs minacce-di-torre, torre vs minore, minore
// vs pedone); UNA sola costante co-tunabile (ThreatScale) invece dei ~6 magici
// di SF -> "struttura SF, costante nostra". Default OFF = byte-identico.
// Minacce calcolate 1x/nodo (cache su hash_key) -> niente costo per-mossa.
static bool g_threat_ordering = true; // BAKED FULL 2026-06-07
void set_threat_ordering(bool v) { g_threat_ordering = v; }
// ThreatHist (UCI "ThreatHist", BAKED ON 2026-07-03): la main quiet history E'
// threat-indexed — history_moves[9][12][64], bucket = from_tier*3 + to_tier
// (tier 0 safe / 1 attaccata da pedone-o- minore / 2 da torre+). td_hbucket
// ritorna il bucket quando ON, 0 quando OFF (= tabella piatta).
static bool g_threat_hist =
    true; // [BAKE 2026-07-03 secondo audit: combo ThreatHist+flow, -30.48 base
          // LOS0.06% @320g 12+0.12]
void set_threat_hist(bool v) { g_threat_hist = v; }
// CapHistThreat (UCI "CapHistThreat", default OFF): l'analogo di ThreatHist sulla
// CAPTURE history. La nostra capture_history era [pezzo][arrivo][vittima] senza
// nessuna dimensione di minaccia, mentre Reckless (history.rs:40-56) e Stormphrax
// (history.h:217) la indicizzano anche sulla casa d'arrivo difesa: la stessa
// cattura vale in modo diverso se il pezzo che arriva puo' essere ripreso.
// ⚠️ Idea genuinamente NON-SF (SF non ce l'ha) — il prior forte e' interno: e'
// l'analogo esatto di ThreatHist sulle quiet, che da noi e' bakata ON.
// OFF = byte-identico (td_cbucket ritorna 0 -> solo il piano [..][0]).
static bool g_caphist_threat = false;
void set_caphist_threat(bool v) { g_caphist_threat = v; }
// ThreatHistWeight (2026-07-04 CABLATO): scala EXTRA sul contributo della
// threat-history nell'ordering (td_score_move), applicata SOPRA MainHistWeight.
// 100 = identita' (byte-identico al release SPRT +30 combo). Cablato e
// SPRT-ato: >100 negativo (130 perde LOS87.75% @194g, run#6), <100 POSITIVO (75
// vince LOS94.46%
// @400g, run#7) — coerente: MainHistWeight basso (67) => il sistema vuole MENO
// enfasi butterfly. [BAKE 2026-07-04: 100->75, SPRT +13.90 Elo per il
// candidato, LOS 94.46% @400g 10+0.1]
int g_threat_hist_weight =
    130; // /100, scala extra del termine threat-history in td_score_move [BAKE
         // 2026-07-04 75->60, SPRT -9.39 Elo base LOS82.13% @296g + gradiente
         // SPSA concorde (discesa 75->68-73)]
int g_threat_scale =
    4212; // contributo = scale/100 * pieceValue * (from_minacciato -
          // to_minacciato). Spin ThreatScale (SPSA). Default basso = nudge
          // (1500 = +75% nodi a d20 = disturba l'ordering tarato); il co-tune
          // trova il valore nostro.
// ---- Check-ordering (#3 SF, 2026-06-07) -------------------------------------
// SF da' un bonus ai quiet che danno SCACCO DIRETTO (forcing), filtrati per non
// essere blunder (SEE >= -75). Una sola costante co-tunabile (CheckBonus).
// check_sq[] (case da cui ogni tipo di pezzo da scacco al re nemico) calcolate
// 1x/nodo. Default OFF = byte-identico.
static bool g_check_ordering = true; // BAKED FULL 2026-06-07
void set_check_ordering(bool v) { g_check_ordering = v; }
int g_check_bonus = 13357; // bonus ordering per quiet che da scacco diretto
                           // (SF=16384). Spin CheckBonus (SPSA).
// ---- Offense-ordering (port Reckless movepick score_quiet, 2026-07-24) -------
// Reckless ordina i quiet premiando le mosse che portano un pezzo su una casa
// SICURA da cui ATTACCA un pezzo nemico vulnerabile (offense[pt]), e penalizzando
// lo spostamento di un pedone-scudo del proprio re (wall_pawns). ThreatOrdering
// (escape/entra-in-minaccia) e CheckOrdering (scacco diretto) li abbiamo GIA';
// questi due termini sono l'unica parte NUOVA del loro rework. Le case-offense
// sono calcolate 1x/nodo (cache offense_key). Le due magnitudini sono spin SPSA
// separate (isolabili: l'uno a 0 isola l'altro). Costanti iniziali riportate sulla
// NOSTRA scala history dal rapporto col nostro g_check_bonus (Reckless: offense
// 3446 / check 10723; wall 4494) — poi RI-MISURATE separatamente:
// - OffenseBonus: isolato a 4300 = NEGATIVO e stabile (-9.70 +/- 10.41 Elo,
//   LOS 3.38% @1290g, 12+0.12) -> spento (0) in attesa di un peso "molto grosso"
//   da ri-testare con WallPawnPenalty gia' attivo (prossimo esperimento).
// - WallPawnPenalty: la magnitudine conta. 1x(5600)=+3.24 nElo LOS85% @12778g
//   (mai chiuso, ma affidabile); 2x(11200)=identico (+3.25 @1426g, non sensibile
//   in questo range); 3x(16800)=**+8.43 +/- 6.93 nElo, LOS 99.14%, LLR 80.5%
//   verso [0,5] @9652g (12+0.12)** — segnale nettamente piu' forte. BAKATO
//   2026-07-24 sul trend (LOS>99%, mai chiuso ma non evapora, l'opposto del
//   pattern B1) in attesa di chiusura SPRT formale e di un mega co-tune finale
//   di 6.0 per la magnitudine esatta.
static bool g_quiet_offense = true; // BAKED 2026-07-24 (solo wall-pawn; offense-squares spento)
int g_offense_bonus = 0;       // spento: isolato NEGATIVO, vedi nota sopra
int g_wallpawn_penalty = 16800; // BAKED 2026-07-24 (3x, vedi nota sopra); era 5600
// ---- ContHist 3/6-ply (#4 SF, 2026-06-07) -----------------------------------
// SF somma la continuation-history a 1/2/3/4/6 ply; noi avevamo 1/2/4. #4
// aggiunge 3-ply (move_stack[ply-2]) e 6-ply (move_stack[ply-5]) all'ordering
// quiet, scalati da ContHist36Weight e poi dal ContHistWeight comune. Update
// sui cutoff in td_conthist_multi_update. SCOPE: solo ordering (NON la
// ContHistLMR). Default OFF = byte-identico.
static bool g_conthist36 = true; // BAKED FULL 2026-06-07
void set_conthist36(bool v) { g_conthist36 = v; }
int g_conthist36_weight = 37; // /100: peso dei termini 3/6-ply relativo agli
                              // altri conthist. Spin ContHist36Weight.
// ---- V2: prior-counter-move bonus + capture-history bonus (SF, 2026-06-07)
// -------- Su un nodo FAIL-LOW (nessuna mossa batte alpha) la mossa PRECEDENTE
// (quella che ha portato qui) era forte (ci ha messo in difficolta') -> bonus
// alla sua history. Quiet: main + continuation history; cattura: capture
// history (vittima da captured_stack). Una sola scala co-tunabile
// (PriorBonusScale) sul td_stat_bonus. Default OFF = byte-identico.
static bool g_prior_bonus = true;
void set_prior_bonus(bool v) { g_prior_bonus = v; }
int g_prior_bonus_scale =
    189; // /100 del td_stat_bonus(depth). Spin PriorBonusScale.
// ---- R-PB: il prior bonus di Reckless (src/search.rs:1123) in due pezzi separabili.
// Oggi noi premiamo la mossa precedente su OGNI all-node e con un bonus PIATTO. Loro
// fanno due cose diverse, e sono indipendenti, quindi qui sono due toggle distinti da
// misurare uno per volta. Entrambi default OFF = byte-identico.
//
// (1) g_prior_bonus_gate — premia solo gli all-node INATTESI (`cut_node || pv_node`).
//     Se il nodo doveva fallire basso e ha fallito basso, la mossa precedente non ci ha
//     detto niente di nuovo: il bonus e' rumore. E' anche il finding B7 del nostro audit
//     ("PriorBonus non gated"), rimasto aperto.
//
// (2) g_prior_bonus_factor — scala il bonus su QUANTO il fail-low e' stato una sorpresa:
//     quante mosse aveva gia' provato il padre, se la mossa era la sua TT-move, e due
//     misure di quanto lo score e' crollato sotto l'eval.
//     ⚠️ La scala non e' la loro: applicare `factor/128` sopra il nostro bonus attuale
//     (~5568 su HISTORY_MAX 7000) manderebbe OGNI update contro il clamp, cancellando
//     proprio la scalatura che si vuole introdurre. `g_pb_scale` e' tarato perche' il
//     bonus MEDIO resti quello di oggi: fattore medio ~480, 480*50/(100*128) = 1,88 volte
//     td_stat_bonus contro le 1,89 attuali. Cambia la FORMA, non il livello.
static bool g_prior_bonus_gate = false;
void set_prior_bonus_gate(bool v) { g_prior_bonus_gate = v; }
static bool g_prior_bonus_factor = false;
void set_prior_bonus_factor(bool v) { g_prior_bonus_factor = v; }
int g_pb_scale = 50;       // /100, poi /128 col fattore: preserva il bonus medio
int g_pb_base = 88;        // termine costante del fattore
int g_pb_mc = 17;          // per mossa gia' esaminata dal padre
int g_pb_mc_cap = 229;     // cap del termine sopra
int g_pb_ttm = 110;        // la mossa precedente era la TT-move del padre
int g_pb_ev = 144;         // score crollato sotto la nostra eval
int g_pb_ev_marg = 97;     // di quanto
int g_pb_swing = 306;      // score crollato sotto l'eval NEGATA del padre (swing grosso)
int g_pb_swing_marg = 136; // di quanto
// ---- #5: low-ply history (SF)
// ----------------------------------------------------- History per-ply usata
// SOLO nei primi LOW_PLY_MAX ply (vicino alla radice): cattura "questa mossa e'
// buona a questo ply basso". Peso /(1+ply). Default OFF = byte-identico.
static bool g_lowply = true;
void set_lowply(bool v) { g_lowply = v; }
int g_lowply_weight =
    179; // contributo ordering = g_lowply_weight * lowply / (100*(1+2*ply))
         // (SF-style decay). Spin LowPlyWeight.
// StatEvalDiffOrder (SF search.cpp ~728, "~9 Elo"): dopo la static eval, spinge
// la history (main + pawn) della mossa PRECEDENTE (dell'avversario) col negato
// della SOMMA delle due static-eval consecutive — segnale a costo ~0 che quella
// mossa ha prodotto uno swing di eval buono/cattivo. NUOVA feature aggiunta
// SOPRA il parco mega-SPSA (gli altri ordering-weight restano tunati). Spin
// "StatEvalDiffMult": 0 = OFF (byte-identico al 4.0), 14 = SF-esatto, >14 =
// piu' aggressivo di SF.
int g_seo_mult = 27; // [4.1 BAKE 14->6] (Ripristinato a 14 dopo fix SEO)
// cutoffCnt (SF): contatore fail-high per-ply. In LMR riduce di piu' se il
// figlio ha cuttato molto di recente. g_cutoffcnt_penalty: 0=off (default,
// byte-identico), 1=SF (r+=1 se cutoff_cnt figlio > 3). Spin CutoffCntPenalty
// [0,3]. Co-tunabile.
int g_cutoffcnt_penalty = 2;
// ==== F-018 (SECONDO AUDIT 2026-07-03): micro-tecniche da Obsidian/Berserk
// ==== TUTTE default OFF/neutro = byte-identico (bench-verificato). Una leva =
// un toggle/spin, testabili una alla volta o co-tunate a blocchi. Dettagli:
// Triumviratus_5/SECOND_PASS_FEATURES.md.
static bool g_postlmr_hist =
    false; // #1 bonus/malus conthist dopo la re-search post-LMR (mainline SF).
           // Il "+18.55" era same-binary; test PULITO build-matched (current vs
           // 549b737, 8+0.08) = -3.50 NEUTRO -> tenuto OFF, candidato a tuning
           // futuro (forse TC-dipendente).
int g_postlmr_scale = 106; //    /100 del td_stat_bonus applicato
static bool g_ttcut_refine =
    true; // #3a-c cutoff TT: depth+1 sui fail-high, coerenza cutnode,
          // fifty<soglia [BAKE 2026-07-03]
int g_ttcut_fifty =
    89; //    soglia fifty oltre cui il cutoff TT non e' affidabile (patta-50
        //    vicina) [BAKE 2026-07-04 blocco secondaudit_block SPSA, SPRT +8.34
        //    Elo LOS79.4% @500g (non conclusivo ma leaning+)]
// Q-14 TTResearchMargin (Ethereal, tecnica di Grant): fail-low anticipato a
// depth-1. Entry UPPER (hash_flag_alpha) a tt_depth>=depth-1 -> return se !PV
// && (cutnode || ttScore<=alpha) && ttScore+margine <= alpha. Duale del
// TTCutRefine (+1ply ai fail-high). Margine 141 Ethereal ~= 79 alla nostra
// scala-56 (init SPSA). Default OFF.
static bool g_tt_research = false;
int g_tt_research_margin = 74;
static bool g_ttcut_malus =
    false; // #3d malus alla quiet AVVERSARIA che porta a un TT-cut (duale di
           // TTCutBonus). BAKE 2026-07-06 (true/7) REVERTITO: SPRT +15.55 Elo
           // era falsato da oversubscription/TC senza incremento; retest pulito
           // (10+0.1, 258g) = -17.52 Elo, LOS 4.89%, LLR non conclusivo.
           // Riaperto, serve retest pulito prima di ridecidere.
int g_ttcut_malus_seen = 3; //    solo se il padre aveva visto <= N mosse
// #4 split good/bad capture a soglia -(mvv+caphist)/div (0=off, Obsidian 32).
// STORIA (2026-07-13/14): screening VM +6.96 @10+0.1 (LOS 97%) → bakato 32 →
// gate-blocco a 20+0.2 NEUTRO (+2.4 ±6.4 su 2606g) → REVERTITO a 0: cambia
// l'albero (bench 377398→487874) e COMPRIME il segnale dei gate-rete durante
// il training v2 (serie epoca-vs-epoca non piu' comparabile). Resta candidato
// del MEGA CO-TUNE post-v2 (li' i margini si ri-coordinano attorno allo split).
int g_goodcap_hist_div = 32; // BAKED 2026-07-24 (bundle lean SPRT +10.43 Elo; era 18)
static bool g_asp_avg =
    false; // #5a aspiration centrata sulla MEDIA degli score. Bake 2026-07-07
           // REVERTITO: -9.04 Elo era singolo draw + winner's curse; pacchetto
           // cumulativo pulito = +0.62 NULLO. OFF, re-ispezione a TC lungo
           // (TC-dipendente?)
static bool g_asp_fhred =
    true; // #5b root: depth-1 per ogni fail-high consecutivo (evita re-search
          // piene) [BAKE 2026-07-03]
int g_singular_mindepth =
    6; // #6a gate depth del blocco singular (8=storico; Obsidian 5, Berserk 6)
       // [BAKE 2026-07-04 blocco secondaudit_block SPSA, SPRT +8.34 Elo
       // LOS79.4% @500g (non conclusivo ma leaning+)]
int g_singular_de_cap =
    1; // #6b cap catena double-extension sul path (0=off; Berserk 6) [BAKE
       // 2026-07-04 blocco secondaudit_block SPSA, SPRT +8.34 Elo LOS79.4%
       // @500g (non conclusivo ma leaning+)]
static bool g_singular_plyguard =
    false; // #6c niente singular oltre ply >= 2*rootDepth (anti-esplosione)
int g_negext_alpha =
    1; // #6d negext se ttScore <= alpha (0=off; Berserk 1) [BAKE 2026-07-04
       // blocco secondaudit_block SPSA, SPRT +8.34 Elo LOS79.4% @500g (non
       // conclusivo ma leaning+)]
static bool g_fh_smooth =
    true; // #7 fail-high: return (score+beta)/2 su RFP e qsearch (bound TT meno
          // gonfiati) [BAKE 2026-07-03]
// FailHighSmooth, PESI SEPARATI (2026-07-25). Il midpoint era `(x+beta)/2`
// cablato su TRE ritorni semanticamente diversi, sotto un unico toggle e senza
// nessun parametro. Nessuno dei riferimenti fa cosi': Reckless (search.rs:1437
// `lerp`) usa CINQUE t distinti e tunati — 0.6945 su RFP, 0.8256 sullo stand-pat
// di qsearch, 0.5072 sul finale di qsearch, 0.2695 su NMP, 0.4027 su multicut —
// e Stormphrax ne espone due tunabili a SPSA. Due implementazioni indipendenti
// convergono su valori LONTANI dal nostro 0.5, e lo stand-pat di qsearch e' il
// sito piu' eseguito del motore.
// Scala /1024: 512 = midpoint = comportamento storico.
int g_fh_t_qs_standpat = 512; // Reckless: ~845/1024
int g_fh_t_qs_final = 512;    // Reckless: ~519/1024
int g_fh_t_rfp = 512;         // Reckless: ~711/1024
// ⚠️ A t = 512 si ritorna l'ESPRESSIONE ORIGINALE, non la lerp equivalente: con
// x+beta negativo e dispari le due troncano diversamente (es. x=-4, beta=-1 ->
// (x+beta)/2 = -2 ma x+(beta-x)/2 = -3). Il caso speciale garantisce che il
// default sia byte-identico; fuori dal default la lerp e' la forma giusta.
static inline int td_fail_firm(int x, int beta, int t) {
  if (t == 512)
    return (x + beta) / 2;
  return x + (int)((long long)(beta - x) * t / 1024);
}
static bool g_nmp_static_margin =
    false; // #8a NMP solo se static_eval >= beta - mult*depth + bias (SF)
int g_nmp_sm_mult = 23;
int g_nmp_sm_bias = 414;
static bool g_nmp_ttnoisy =
    false; // #8b R+1 se la TT move e' una cattura (Obsidian)
static bool g_alpha_depth_dec =
    true; // #9 depth-- per le mosse restanti quando una mossa alza alpha
          // (2<=depth<=11) [BAKE 2026-07-03]
int g_fhboost_margin =
    2; // #10a bonus history a depth+1 se best > beta+margine (Obsidian 95,
       // Berserk 75). Bake 2026-07-07 (margin=50) REVERTITO: i "conferma" 2x
       // avevano AspAvg ON; retest ISOLATO vs baseline pura = +1.74 NULLO. OFF
       // (0), re-ispezione a TC lungo
static bool g_hist_triv_guard = false; // #10b niente bonus al cutoff "gratis"
                                       // (depth<=3, nessun quiet prima)
int g_malus_pct =
    69; // #10c malus = bonus * pct/100 (costanti malus separate dal bonus)
static bool g_easycap_gate =
    false; // #11 niente NMP se abbiamo un pezzo in presa "facile" (Berserk)
int g_rfp_hist_thresh =
    64; // #12 RFP solo se hash-move non-quiet o con history > soglia (0=off)
static bool g_killer_reset =
    true; // #13 azzera i killer del ply FIGLIO a ogni nodo (anti-stale, +12.15 Elo)
// ⭐ CounterMove (default ON = comportamento storico, byte-identico). Toggle di
// RIMOZIONE: SF ha SEMPLIFICATO VIA la countermove heuristic (a45c2bc3, PR
// #5441, lug-2024) con un test non-regressivo da **978.000 partite STC** + 81k
// LTC: da loro valeva ZERO. Il movepicker di SF master non ha piu' lo stage
// refutation-countermove. Noi lo abbiamo ancora, di default. ⚠️ NON
// confondere con `g_prior_bonus` (bonus history alla prior countermove sul
// fail-low): quello SF lo TIENE e lo tuna ancora. Qui si spegne solo la TABELLA
// counter_moves e il suo stage. Movente: 19/07 il pattern e' "togliere paga,
// aggiungere no" (10 patch, 9 bocciate; l'unica vincente +7.98 toglieva la
// check-extension obsoleta).
static bool g_countermove = true;
int g_qs_move_cap =
    3; // #14 cap mosse esaminate in qsearch non-in-check (0=off; Obsidian 3).
       // BAKED 2026-07-25: era 1, cioe' la qsearch cercava UNA sola mossa per
       // nodo (una CATENA, non un albero) — reperto dell'audit finale 6.0.
       // Portato a 3 (il valore di Obsidian): **+5.21 +/- 6.47 Elo, LOS 94.28%,
       // 2868 partite @15+0.15**. Bakato su LOS, non su bound SPRT chiuso.
// ==== Bug-fix a toggle (cambiano i node-count -> SPRT prima di bakare ON) ====
static bool g_qs_draw_check =
    false; // F-015: draw-detection (ripetizione/50 mosse) in qsearch. Bake
           // 2026-07-07 REVERTITO: -10.73 Elo era singolo draw + winner's
           // curse; pacchetto cumulativo pulito = NULLO. OFF, re-ispezione a TC
           // lungo
bool g_ep_key_fix = true; // F-017: ep nella hash key SOLO se la cattura ep e'
                          // pseudo-legale (anche movegen.cpp) SPRT 16+0.16 1t
                          // 128MB UHO_4060_v4: +4.31 ±4.37 Elo, LOS 97.35%,
                          // 6282g fix di correttezza → default ON (2026-07-21).
// QsStalemateCheck (SF d2d046c-style, default OFF, mai testato 2026-07-24):
// qsearch genera SOLO catture (+ eventuali check) quando non e' sotto scacco,
// quindi non puo' MAI accorgersi che una posizione e' stallo (zero mosse
// legali, tranquille comprese) — la restituisce come un eval normale. Il buco
// e' concreto nei finali K+P (trucchi di stallo comuni). Gated alla sola
// transizione rara "questa cattura ha appena tolto l'ultima Torre/Donna dalla
// scacchiera" (vedi td_has_any_legal_move): un probe di legalita' a
// movegen-completo li' e' economico (il caso vero-stallo che deve scandire
// TUTTA la lista e' proprio quello raro che vogliamo scovare).
static bool g_qs_stalemate_check = true; // BAKED 2026-07-24 (bundle lean SPRT +10.43 Elo)
// Syzygy{ProbeDepth,ProbeLimit,50MoveRule} (2026-07-24): 3 opzioni UCI
// advertised (uci_mt.cpp) fin dalla release ma MAI wired — bug scoperto in
// audit (FASE C hygiene, PIANO_MASTER §5): una GUI che le settasse non
// avrebbe avuto alcun effetto. Default = comportamento storico ESATTO (depth
// gate sempre soddisfatto perche' qui depth>=1 gia' garantito dal caller;
// limit 7 = TB_LARGEST massimo di Syzygy, mai piu' stretto; 50MoveRule=true =
// cursed/blessed restano 0 come sempre) -> nessuna regressione col default.
int g_syzygy_probe_depth = 1;
int g_syzygy_probe_limit = 7;
// CutoffStats (diagnostica move-ordering, default OFF = byte-identico): se ON,
// conta sui beta-cutoff il first-move-cutoff rate (fh_first/fh_nodes) + indice
// medio della mossa al cutoff, e li stampa come "info string FMC ..." a fine
// ricerca. Il gap vs SF e' l'ordering (first-move-cutoff storico 84.76% SATURO)
// -> questo lo misura per guidare il lavoro futuro.
bool g_cutoff_stats = false;
// ProbCut-sotto-scacco (SF step 12): in scacco, se la TT ha una cattura con
// bound LOWER e score >= beta+margin a depth>=depth-4, ritorna beta+margin.
// 0=off (default). Spin ProbCutInCheckMargin [0,800] (SF=452). Co-tunabile.
int g_probcut_incheck_margin = 331; // [4.1 BAKE 0->523]
int g_lmr_ss_div =
    5430; // [4.1: tenuto 4.0 - il BAKE @12s 13790 GONFIAVA l'albero 2.5x (meno
          // riduzione) per ~pochi Elo: scelta = albero stretto > Elo]
int g_lmr_ss_offset = -2953; // [4.1: tenuto 4.0 - vedi sopra; il BAKE -2214 =
                             // parte del bloat LMR, revertito]
int g_lmr_ch_div = 10942;    // [3.7 BAKE 4437->3506; BAKED #1 era 10000].
                             // ContHistLMR: reduction -= (conthist1+2+4) / div
int g_cutnode_lmr_extra = 1; // CutNodeLMR: ply extra di riduzione sui cut-node
                             // (sopra il +1 esistente)
// NMP + LMR-enrichment tunables.
int g_nmp_base = 5; // null-move reduction: R = g_nmp_base + depth/g_nmp_div
int g_nmp_div = 3;
int g_lmr_eval_margin =
    43; // [F-004/005 BAKE 2026-07-02: 33->20, SPSA in blocco LMR] LMR: reduce
        // +1 more when static_eval + margin < alpha
int g_lmr_ttdepth =
    1; // LMR: reduce LESS by this when TT depth >= depth   [SPSA-tuned: 0->2]
// CORE LMR formula coefficients (*100).
int g_lmr_base_x100 = 22; // baseline reduction floor [3.7 BAKE 41->37; SPSA
                          // 75->47; BAKED #1 47->41]
int g_lmr_div_x100 = 447; // bigger divisor = LESS reduction [3.7 BAKE 345->310;
                          // SPSA 225->270; BAKED #1 270->345]
// ⭐ 5.1 STRUTTURALE — riduzione LMR "FINE" stile-SF in 1/1024 di ply (default
// OFF = byte-identico). La nostra LMR monta la riduzione in PLY INTERI (±1) =
// grezza: tagliando di piu' si sovra-pota OVUNQUE (l'SPSA del cut-block era
// piatto perche' la magnitudine non discrimina DOVE tagliare). SF
// (search.cpp:1287-1334) la monta in millesimi di ply fondendo ~12 segnali con
// peso fine, e taglia FORTE solo dove e' sicuro (cut/ALL-node, history pessima)
// proteggendo PV/ttPv/eval-incerta
// -> piu' riduzione TOTALE = piu' profondita' SENZA perdere tattica.
// Coefficienti in 1/1024 ply, SPSA-tunabili.
static bool g_lmr_fine = true; // 5.1 BAKE (spsa_struct iter1216, media ultimi
                               // 100 @20+0.2): ON di default
int g_lmrf_cut =
    3687; // [5.1 BAKE 3995->3184] cut-node: riduzione forte (il divario #1)
int g_lmrf_cut_nott =
    2397; // [5.1 BAKE 1059->2092] extra sui cut-node senza TT move
int g_lmrf_ttcap =
    200; // [5.1 BAKE 1039->390]  ttMove e' una cattura -> riduci di piu'
int g_lmrf_ttpv =
    617; // [5.1 BAKE 2766->2210] ttPv: riduci MENO (protezione del ramo ex-PV)
int g_lmrf_pv = 437; // [5.1 BAKE 1017->1551] PV node: riduci MENO
int g_lmrf_ss = 664; // [5.1 BAKE 445->923]   statScore (history continua): r -=
                     // statScore*g/4096
int g_lmrf_corr =
    866; // [5.1 BAKE 160->478]   correctionValue (eval incerta -> riduci meno)
int g_lmrf_all = 618;     // [5.1 BAKE 272->723]   scaling ALL-node: r +=
                          // r*g/(256*depth+285)  [ci mancava]
int g_lmrf_improv = 489;  // [5.1 BAKE 1024->411]  non-improving
int g_lmrf_evalcut = 979; // [5.1 BAKE 1024->1517] eval+margin < alpha
int g_lmrf_cutoff =
    1520; // [5.1 BAKE 1100->1626] figlio con cutoffCnt alto: riduci di piu'
// ⭐ ContHist4LMR (2026-07-20, spin, 0 = OFF byte-identico) — SEGNALE ORFANO,
// non feature nuova. Il ramo LMR *legacy* (`if (!g_lmr_fine)`, :4878-4965, oggi
// irraggiungibile perche' LMRFine e' bakato ON) consultava la continuation
// history a 1, 2 **e 4** ply indietro (:4937). Il port di LMRFine da SF usa
// `2*main + cont1 + cont2` e la 4-ply e' rimasta fuori: non un bug, un port
// fedele — ma da noi e' una PERDITA, perche' `cont_hist_4` continuiamo a
// MANTENERLA a ogni nodo (update :3834) e a leggerla nell'ordinamento (:2852).
// Paghiamo il costo e la LMR la ignora. Coefficiente SEPARATO (non dentro
// sscore) per due motivi: 0 resta byte-identico, e SPSA puo' pesare la 4-ply
// senza dover muovere LMRFSS, che governa main+cont1+cont2.
int g_lmrf_cont4 =
    0; // peso della conthist a 4 ply nella LMR fine (1/4096, come LMRFSS)
int g_lmr_expect =
    0; // LMRExpect (spin, 0=OFF byte-identico): bonus EXTRA al termine
       // cutoffCnt quando il nodo e' ALL (ne' PV ne' cut) = "aspettativa
       // rispettata". Reckless usa +256/+384 nelle stesse unita' (1/1024 ply).
// EvalOptimism (5.1): alimenta g_optimism (nnue_bridge) dalla root col contempt
// dinamico di SF (optimism = strength*score/(|score|+div)). ⚠️ DEFAULT = ON
// (bakato 5.1; il valore vive in nnue_bridge.cpp:138, non qui). Spegnendolo
// g_optimism resta 0 = byte-identico al pre-5.1 — ma NON e' lo stato di
// default.
extern int g_eval_optimism; // definito in nnue_bridge.cpp
extern std::atomic<int>
    g_optimism[2]; // F-019: atomic (main scrive, helper leggono)
int g_opt_strength =
    170; // [5.1 BAKE 137->89]  optimism = strength*score/(|score|+div)
int g_opt_div = 272; // [5.1 BAKE 81->72]
int g_histprune_margin =
    2097; // [3.7 BAKE 1602->1691; BAKED #1 era 1000]. history pruning: prune
          // late quiet if combined hist < -margin*depth
int g_conthist_prune_depth =
    2; // gate profondita' conthist-prune (SF usa lmrDepth<6). UCI
       // ContHistPruneDepth. PASSO2: col blocco LmrDepthPrune si alza ~6.
// LmrDepthPrune (SF): pota futility+conthist sulla profondita' RIDOTTA dalla
// LMR per la mossa (prune_depth = depth-1-lmr_table[depth][movecount]) invece
// che sulla depth piena del nodo. Fa scattare il pruning sulle mosse TARDIVE
// anche a node-depth alto (chiude il gap-midgame vs SF, che pota 3x di piu'
// li'). Cap nostri (6/4) = MENO aggressivo di SF (15/6). Spin LmrDepthPrune:
// 0=off (byte-identico), 1=on. Co-tunabile.
int g_lmrdepth_prune = 1;
// PASSO 1 (SF Step-14): protezione-history. Con LmrDepthPrune ON, prune_depth
// viene AGGIUSTATO dalla history della mossa: prune_depth +=
// history/g_lmrdepth_histdiv. History buona -> prune_depth sale (mossa
// protetta); scarsa -> scende (potata di piu'). E' il pezzo che mancava al port
// monco (potava le tardive alla cieca -> bloat). SF div=6437.
int g_lmrdepth_histdiv = 4509;
// SEE-pruning margins (ALSO the Phase-2 skip_bad_caps lever): a move is
// SEE-pruned at low depth if SEE < -g_see_cap_margin*depth (captures) or <
// -g_see_quiet_margin*depth*depth (quiets). Exposed so SPSA can tune them (UCI:
// SEECaptureMargin / SEEQuietMargin).
int g_see_cap_margin = 81; // REVERT 2026-07-23 (SPSA B1 evaporato @4452g)
int g_see_quiet_margin = 116; // [3.7 BAKE 98->96; BAKED #1 era 50]
int g_see_depth = 3; // gate profondita' del SEE pruning (alzare = potare piu'
                     // in profondita'). UCI SEEPruneDepth, tunabile.
// S-05 (2026-07-06, terzo audit): default OFF. Oggi il gate/margine SEE usa la
// depth PIENA del nodo -> una mossa tardiva/ridotta-LMR a node-depth alto non
// viene MAI SEE-potata (depth<=3 non scatta mai oltre il ply 3). `prune_depth`
// (gia' calcolato sopra per futility/conthist, vedi LmrDepthPrune) e'
// esattamente la profondita'-effettiva ridotta per le mosse quiet tardive (per
// le catture prune_depth==depth, quindi ON questo toggle non le tocca) — usarlo
// al posto di depth chiude il gap di DOMINIO (non di costanti) segnalato da SF
// Step 14.
static bool g_see_lmr_depth = false;
// Dedicated margin for the SEELmrDepth quiet path: g_see_quiet_margin is shared
// with the always-on capped path (depth<=g_see_depth), so tuning it would also
// perturb that default behavior. This one only feeds the prune_depth-based
// quadratic when the toggle is ON -> SPSA can search it independently. Same
// starting value as g_see_quiet_margin (185) until tuned.
int g_see_lmr_quiet_margin = 193;
// Dedicated cap on prune_depth for the SEELmrDepth path (separate from
// SEEPruneDepth, which still gates the raw-depth/capture path). Without ANY cap
// we pay a real td_see_at_least() call for every quiet move at every
// prune_depth, even where the quadratic margin is so permissive it
// mathematically can never prune -> wasted cycles past some point. Default 32
// is practically unbounded (prune_depth rarely reaches it), matching the tested
// no-cap behavior; SPSA can search LOWER values to trade reach for NPS.
int g_see_lmr_prune_cap = 27;
// S-05 forma LINEARE (2026-07-12, default OFF): il margine quiet del path
// SEELmr e' oggi quadratico (-coef*prune_depth^2), come il path di default. Il
// vecchio SPSA S-05 si e' hard-stoppato a 178/300 con gradiente PIATTO:
// l'ipotesi e' che il quadrato saturi il margine cosi' in fretta da rendere il
// coefficiente quasi indistinguibile per SPSA (a prune_depth 3 gia'
// -185*9=-1665, praticamente mai raggiungibile -> il gradiente non vede il
// coef). La forma lineare -coef*prune_depth cresce piano -> il coef resta
// nell'intervallo utile a piu' profondita' -> gradiente piu' pulito, e potatura
// meno aggressiva alle prune_depth alte. Quando g_see_lmr_linear e' ON il path
// SEELmr usa -g_see_lmr_linear_coef*prune_depth al posto del quadrato. Attiva
// il path da solo (non serve anche SEELmrDepth). coef alto = soglia piu'
// negativa = pota MENO (albero piu' grande).
static bool g_see_lmr_linear = false;
int g_see_lmr_linear_coef = 368; // center; gate 2-3 valori lato laptop
// MultiPV (analisi, stile Stockfish, 2026-07-13): quante varianti di root
// riportare. 1 = gioco normale, flusso INVARIATO (gate: bench identico). >1:
// dopo la linea principale di ogni iterazione il main thread ri-cerca la root a
// finestra piena escludendo le best move gia' assegnate e stampa "info ...
// multipv k ...". Solo analisi: TM/voting/FollowPV/optimism restano ancorati
// alla linea 1; gli helper lazy-SMP restano single-PV (riempiono la TT come
// sempre).
static int g_multipv = 1;
// TTPrefetch (Reckless #1085): prefetch del bucket TT del FIGLIO in
// td_make_move, appena la chiave e' definitiva (post-legalita'). Node-identical
// per costruzione: solo NPS. Il tentativo 2026-06-07 era NPS-neutral;
// ri-misurato 2026-07-13 (dopo il bake di TTTwoLevel, che ha cambiato il
// pattern di accesso TT) su VM GCP: +1.88% NPS su N=100 bench (OFF 1061381
// [1011790-1106739] vs ON 1081343 [1048327-1113268], range quasi disgiunti).
// BAKED ON di default (option resta per un'eventuale ablazione A/B).
static bool g_tt_prefetch = true;
// GoodCapTTQuiet (Reckless 0.10.0 #1107, default OFF): se la TT-move del nodo
// e' QUIET, la mossa migliore nota non e' una cattura -> per essere ordinata
// "good" una cattura deve VINCERE materiale netto (SEE >= +1; le pari scendono
// tra le bad). Reckless: +1.48 +-1.98 @30k games -> micro-gain, candidato
// co-tune, da gatare ad alto N. (La loro condizione extra noisy_count>2 e' un
// artefatto del loro picker a stadi; qui si usa la sola condizione
// tt-move-quiet.)
static bool g_goodcap_ttquiet = false;
// UCI_ShowWDL (analisi): quando ON, le info-line riportano " wdl W D L"
// (permille, stm-relative, somma 1000) accanto allo score. Modello logistico a
// un parametro ancorato all'UNICO punto di calibrazione noto (NORM_CP=392
// intrinseco = +100cp normalizzato = P(win)~50%, misurato su 191k posizioni).
// Larghezza b=100 -> patta ~46% a posizione pari (coerente col draw-ratio
// osservato ~49%). Default OFF = output storico invariato. Stima onesta, non un
// fit materiale-dipendente alla SF.
static bool g_show_wdl = false;
// (The SFNNv10 eval-wrapper tunables — g_small_net_threshold / g_eval_optimism
// / g_eval_pawn_scale / g_eval_complexity_div / g_eval_blend_delta — were
// removed in M2: the TRANN1 bridge applies Stockfish's fixed cp scaling, so
// they no longer exist. The matching UCI options + set_search_param cases were
// removed too.)

void init_lmr_table(); // defined below; re-run when LMR core coefficients
                       // change

// Dispatch a UCI spin option to the matching tunable. Returns true if matched.
bool set_search_param(const char *name, int value) {
  if (!strcmp(name, "RFPMargin")) {
    g_rfp_margin = value;
    return true;
  }
  if (!strcmp(name, "RazorBase")) {
    g_razor_base = value;
    return true;
  }
  if (!strcmp(name, "RazorMult")) {
    g_razor_mult = value;
    return true;
  }
  if (!strcmp(name, "FutilityBase")) {
    g_fut_base = value;
    return true;
  }
  if (!strcmp(name, "FutilityMult")) {
    g_fut_mult = value;
    return true;
  }
  if (!strcmp(name, "FutilityDepth")) {
    g_fut_depth = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "SEEPruneDepth")) {
    g_see_depth = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "SEELmrDepth")) {
    g_see_lmr_depth = value != 0;
    return true;
  }
  if (!strcmp(name, "SEELmrQuietMargin")) {
    g_see_lmr_quiet_margin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "SEELmrPruneCap")) {
    g_see_lmr_prune_cap = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "SEELmrLinear")) {
    g_see_lmr_linear = value != 0;
    return true;
  }
  if (!strcmp(name, "SEELmrLinearCoef")) {
    g_see_lmr_linear_coef = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "MultiPV")) {
    g_multipv = value < 1 ? 1 : (value > 64 ? 64 : value);
    return true;
  }
  if (!strcmp(name, "UCI_ShowWDL")) {
    g_show_wdl = value != 0;
    return true;
  }
  if (!strcmp(name, "TTPrefetch")) {
    g_tt_prefetch = value != 0;
    return true;
  }
  if (!strcmp(name, "GoodCapTTQuiet")) {
    g_goodcap_ttquiet = value != 0;
    return true;
  }
  if (!strcmp(name, "QsStalemateCheck")) {
    g_qs_stalemate_check = value != 0;
    return true;
  }
  if (!strcmp(name, "SyzygyProbeDepth")) {
    g_syzygy_probe_depth = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "SyzygyProbeLimit")) {
    g_syzygy_probe_limit = value < 0 ? 0 : (value > 7 ? 7 : value);
    return true;
  }
  if (!strcmp(name, "Syzygy50MoveRule")) {
    syzygy_set_ignore_50_move_rule(value == 0); // UCI true = rispetta (default) -> ignore=false
    return true;
  }
  if (!strcmp(name, "CorrUncert")) {
    g_corr_uncert = value != 0;
    return true;
  }
  if (!strcmp(name, "CorrUncertRFP")) {
    g_cu_rfp = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CorrUncertFut")) {
    g_cu_fut = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CorrUncertCap")) {
    g_cu_cap = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "BrilliantSac")) {
    g_brilliant_sac = value != 0;
    return true;
  }
  if (!strcmp(name, "BrilliantSacMargin")) {
    g_brilliant_sac_margin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "BrilliantSacLMR")) {
    g_brilliant_sac_lmr = value != 0;
    return true;
  }
  if (!strcmp(name, "BrilliantSacLMRAmt")) {
    g_brilliant_sac_lmr_amt = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "TroubleMaking")) {
    g_trouble = value != 0;
    return true;
  }
  if (!strcmp(name, "TroubleScore")) {
    g_trouble_score = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "TroubleEffort")) {
    g_trouble_effort = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TroubleMargin")) {
    g_trouble_margin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "FutilityImproving")) {
    g_fut_improving = value;
    return true;
  }
  if (!strcmp(name, "SingularDoubleMargin")) {
    g_singular_dmargin = value;
    return true;
  }
  if (!strcmp(name, "HistReductionDiv")) {
    g_hist_red_div = value;
    return true;
  }
  if (!strcmp(name, "AspInitDelta")) {
    g_asp_init_delta = value;
    return true;
  }
  if (!strcmp(name, "AspStableShrink")) {
    g_asp_stable_shrink = value != 0;
    return true;
  }
  if (!strcmp(name, "AspStableCap")) {
    g_asp_stable_cap = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "AspGrow")) {
    g_asp_grow = value;
    return true;
  }
  if (!strcmp(name, "AspScoreMult")) {
    g_asp_score_mult = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CMHCScale")) {
    g_cmhc_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "FollowPV")) {
    g_follow_pv = value != 0;
    return true;
  }
  if (!strcmp(name, "RFPBadNode")) {
    g_rfp_badnode = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "FutSpareQuiet")) {
    g_fut_spare_quiet = value != 0;
    return true;
  }
  if (!strcmp(name, "QSDeltaBestCase")) {
    g_qs_delta_bestcase = value != 0;
    return true;
  }
  if (!strcmp(name, "QSBCMargin")) {
    g_qs_bc_margin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "ProbCutMargin")) {
    g_probcut_margin = value;
    return true;
  }
  if (!strcmp(name, "ProbCutImprove")) {
    g_probcut_improve = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CorrCap")) {
    g_corr_cap = value;
    return true;
  }
  if (!strcmp(name, "CorrLearnDiv")) {
    g_corr_lr_div = value;
    return true;
  }
  if (!strcmp(name, "CorrAsym")) {
    g_corr_asym = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "CorrContWeight")) {
    g_corr_cont_weight = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CorrNonPawnWeight")) {
    g_corr_np_weight = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CorrMaterial")) {
    g_corr_material = value != 0;
    return true;
  }
  if (!strcmp(name, "CorrMaterialWeight")) {
    g_corr_material_weight = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "ContHistDiv")) {
    g_conthist_red_div = value;
    return true;
  }
  if (!strcmp(name, "HistPruneMargin")) {
    g_histprune_margin = value;
    return true;
  }
  if (!strcmp(name, "LmrDepthPrune")) {
    g_lmrdepth_prune = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LmrDepthHistDiv")) {
    g_lmrdepth_histdiv = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "ContHistPruneDepth")) {
    g_conthist_prune_depth = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "SEECaptureMargin")) {
    g_see_cap_margin = value;
    return true;
  }
  if (!strcmp(name, "SEEQuietMargin")) {
    g_see_quiet_margin = value;
    return true;
  }
  if (!strcmp(name, "CaptureFutility")) {
    g_cap_futility = value != 0;
    return true;
  }
  if (!strcmp(name, "CapFutBase")) {
    g_capfut_base = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CapFutMult")) {
    g_capfut_mult = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CapFutChist")) {
    g_capfut_chist = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CapFutDepth")) {
    g_capfut_depth = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "CapFutVicScale")) {
    g_capfut_vic_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "QFutVicScale")) {
    g_qfut_vic_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "OppWorsening")) {
    g_opp_worsening = value != 0;
    return true;
  }
  if (!strcmp(name, "OppWorseMargin")) {
    g_opp_worse_margin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "TripleExt")) {
    g_triple_ext = value != 0;
    return true;
  }
  if (!strcmp(name, "SingularTripleMargin")) {
    g_singular_tmargin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "NegExtTT")) {
    g_negext_tt = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "NegExtCut")) {
    g_negext_cut = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CorrValMargin")) {
    g_corrval_margin = value != 0;
    return true;
  }
  if (!strcmp(name, "CorrValRFP")) {
    g_corrval_rfp = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CorrValExt")) {
    g_corrval_ext = value != 0;
    return true;
  }
  if (!strcmp(name, "CorrValFut")) {
    g_corrval_fut = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CorrValSee")) {
    g_corrval_see = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CorrValLmr")) {
    g_corrval_lmr = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "IIRMinDepth")) {
    g_iir_min_depth = value < 1 ? 1 : value;
    return true;
  } // gate strutturali
  if (!strcmp(name, "IIRAmount")) {
    g_iir_amount = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "IIRNotInCheck")) {
    g_iir_not_in_check = value != 0;
    return true;
  }
  if (!strcmp(name, "ProbCutMinDepth")) {
    g_probcut_min_depth = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "ProbCutDepthOff")) {
    g_probcut_depth_off = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "SingularTTMargin")) {
    g_singular_ttmargin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "SingularDepthDiv")) {
    g_singular_depth_div = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "MalusScaled")) {
    g_malus_scaled = value != 0;
    return true;
  }
  if (!strcmp(name, "MalusScaleCoef")) {
    g_malus_scale_coef = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "MalusQuad")) {
    g_malus_quad = value != 0;
    return true;
  }
  if (!strcmp(name, "MalusQuadCoef")) {
    g_malus_quad_coef = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "RazorTTQuiet")) {
    g_razor_ttquiet = value != 0;
    return true;
  }
  if (!strcmp(name, "RazorTTLower")) {
    g_razor_ttlower = value != 0;
    return true;
  }
  if (!strcmp(name, "SingularExactMargin")) {
    g_singular_exact_margin = value != 0;
    return true;
  }
  if (!strcmp(name, "SingularExactDecouple")) {
    g_singular_exact_decouple = value != 0;
    return true;
  }
  if (!strcmp(name, "SingularExactMaxDepth")) {
    g_singular_exact_maxdepth = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "DoDeeper")) {
    g_do_deeper = value != 0;
    return true;
  }
  if (!strcmp(name, "DoDeeperBase")) {
    g_dodeeper_base = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "HindsightExt")) {
    g_hindsight_ext = value != 0;
    return true;
  }
  if (!strcmp(name, "HindsightMargin")) {
    g_hindsight_margin = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "HindsightRed")) {
    g_hindsight_red = value != 0;
    return true;
  }
  if (!strcmp(name, "HindsightRedMargin")) {
    g_hindsight_red_margin = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "HindsightRedThresh")) {
    g_hindsight_red_thresh = value;
    return true;
  }
  if (!strcmp(name, "DoShallowerMargin")) {
    g_doshallower_margin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "BadNoisy")) {
    g_bad_noisy = value != 0;
    return true;
  }
  if (!strcmp(name, "BadNoisyCount")) {
    g_bad_noisy_count = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "LMREnrich")) {
    g_lmr_enrich = value != 0;
    return true;
  }
  if (!strcmp(name, "LMREnrichAmount")) {
    g_lmr_enrich_amount = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "RazorQuadCoef")) {
    g_razor_quad_coef = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "RFPDepth")) {
    g_rfp_depth_cap = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "RazorDepth")) {
    g_razor_depth_cap = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "IID")) {
    g_iid = value != 0;
    return true;
  }
  if (!strcmp(name, "IIDDepth")) {
    g_iid_depth = value < 2 ? 2 : value;
    return true;
  }
  if (!strcmp(name, "IIDReduction")) {
    g_iid_reduction = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "LMRFine")) {
    g_lmr_fine = value != 0;
    return true;
  }
  if (!strcmp(name, "LMRFCut")) {
    g_lmrf_cut = value;
    return true;
  }
  if (!strcmp(name, "LMRFCutNoTT")) {
    g_lmrf_cut_nott = value;
    return true;
  }
  if (!strcmp(name, "LMRFTTCap")) {
    g_lmrf_ttcap = value;
    return true;
  }
  if (!strcmp(name, "LMRFTTPv")) {
    g_lmrf_ttpv = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRFPv")) {
    g_lmrf_pv = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRFSS")) {
    g_lmrf_ss = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRFCorr")) {
    g_lmrf_corr = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRFAll")) {
    g_lmrf_all = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRFImprov")) {
    g_lmrf_improv = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRFEvalCut")) {
    g_lmrf_evalcut = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRFCutoff")) {
    g_lmrf_cutoff = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRFCont4")) {
    g_lmrf_cont4 = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRExpect")) {
    g_lmr_expect = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "EvalOptimism")) {
    g_eval_optimism = value != 0;
    if (!g_eval_optimism) {
      g_optimism[0] = g_optimism[1] = 0;
    }
    return true;
  }
  if (!strcmp(name, "EvalOptStrength")) {
    g_opt_strength = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "EvalOptDiv")) {
    g_opt_div = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "SeeGE")) {
    g_see_ge = (value != 0);
    return true;
  }
  if (!strcmp(name, "SeeGEVerify")) {
    g_see_ge_verify = (value != 0);
    if (value) {
      g_seege_checks = 0;
      g_seege_bad = 0;
    } else
      seege_verify_report();
    return true;
  }
  if (!strcmp(name, "QSDeltaMargin")) {
    g_qs_delta = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "SingularMarginPD")) {
    g_singular_mpd = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TMDropThresh")) {
    g_tm_drop_thresh = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TMDropCap")) {
    g_tm_drop_cap = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "NodeTMBase")) {
    g_nodetm_base = value < 100 ? 100 : value;
    return true;
  }
  if (!strcmp(name, "NodeTMMin")) {
    g_nodetm_min = value < 10 ? 10 : (value > 100 ? 100 : value);
    return true;
  }
  if (!strcmp(name, "LMRMinDepth")) {
    g_lmr_min_depth = value < 2 ? 2 : value;
    return true;
  }
  if (!strcmp(name, "LMRMinMoves")) {
    g_lmr_min_moves = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "LMRCaptures")) {
    g_lmr_captures = value != 0;
    return true;
  }
  if (!strcmp(name, "LMRCapScale")) {
    g_lmr_cap_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRCheckExempt")) {
    g_lmr_check_exempt = value < 0 ? 0 : (value > 2 ? 2 : value);
    return true;
  }
  if (!strcmp(name, "TMMovesToGo")) {
    g_tm_movestogo = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TMIncFrac")) {
    g_tm_inc_frac = value;
    return true;
  }
  if (!strcmp(name, "TMMaxMult")) {
    g_tm_max_mult = value < 100 ? 100 : value;
    return true;
  }
  if (!strcmp(name, "TMInstab")) {
    g_tm_instab = value;
    return true;
  }
  if (!strcmp(name, "TMDropDiv")) {
    g_tm_drop_div = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TMStability")) {
    g_tm_stability = value != 0;
    return true;
  }
  if (!strcmp(name, "RootMateRestore")) {
    g_root_mate_restore = value != 0;
    return true;
  }
  if (!strcmp(name, "LMRDecisiveBeta")) {
    g_lmr_decisive_beta = value != 0;
    return true;
  }
  if (!strcmp(name, "TTDecisiveClamp")) {
    g_tt_decisive_clamp = value != 0;
    return true;
  }
  if (!strcmp(name, "TTSecondaryAge")) {
    g_tt_secondary_age = value != 0;
    return true;
  }
  if (!strcmp(name, "SEEStalemateGuard")) {
    g_see_stalemate_guard = value != 0;
    return true;
  }
  if (!strcmp(name, "TBScoreTT")) {
    g_tb_tt_store = value != 0;
    return true;
  }
  if (!strcmp(name, "ProbCutImproving")) {
    g_probcut_improving = value != 0;
    return true;
  }
  if (!strcmp(name, "QSCaptHist")) {
    g_qs_capthist = value != 0;
    return true;
  }
  if (!strcmp(name, "QSCaptHistScale")) {
    g_qs_capthist_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "NodeCache")) {
    g_node_cache = value != 0;
    return true;
  }
  if (!strcmp(name, "NodeCacheBonus")) {
    g_nc_bonus = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "NodeCacheMinSum")) {
    g_nc_min_sum = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "HistBonusMoves")) {
    g_hist_bonus_moves = value < 0 ? 0 : value;
    return true;
  } // Q-18
  if (!strcmp(name, "HistAge")) {
    g_hist_age = value < 1 ? 1 : (value > 1024 ? 1024 : value);
    return true;
  } // Q-25
  if (!strcmp(name, "HistInitQuiet")) {
    g_hist_init_quiet = value < 0 ? 0 : value;
    return true;
  } // Q-26
  if (!strcmp(name, "HistInitCapt")) {
    g_hist_init_capt = value < 0 ? 0 : value;
    return true;
  } // Q-26
  if (!strcmp(name, "LMRFPly")) {
    g_lmrf_ply = value < 0 ? 0 : value;
    return true;
  } // Q-19a
  if (!strcmp(name, "LMRFKiller")) {
    g_lmrf_killer = value < 0 ? 0 : value;
    return true;
  } // Q-19b
  if (!strcmp(name, "RecaptureExt")) {
    g_recapture_ext = value != 0;
    return true;
  }
  if (!strcmp(name, "RecaptureTension")) {
    g_recapture_tension = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "TMStabThresh")) {
    g_tm_stab_thresh = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "TMStabStep")) {
    g_tm_stab_step = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "TMStabFloor")) {
    g_tm_stab_floor = value < 10 ? 10 : (value > 100 ? 100 : value);
    return true;
  }
  // ---- TM v2 (quarto audit Q-01..Q-08) ----
  if (!strcmp(name, "TMv2")) {
    g_tmv2 = value != 0;
    return true;
  }
  if (!strcmp(name, "TMv2Stab0")) {
    g_tmv2_stab[0] = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Stab1")) {
    g_tmv2_stab[1] = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Stab2")) {
    g_tmv2_stab[2] = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Stab3")) {
    g_tmv2_stab[3] = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Stab4")) {
    g_tmv2_stab[4] = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Eval0")) {
    g_tmv2_eval[0] = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Eval1")) {
    g_tmv2_eval[1] = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Eval2")) {
    g_tmv2_eval[2] = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Eval3")) {
    g_tmv2_eval[3] = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Eval4")) {
    g_tmv2_eval[4] = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2EvalWindow")) {
    g_tmv2_eval_window = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TMv2NodeBase")) {
    g_tmv2_node_base = value < 100 ? 100 : value;
    return true;
  }
  if (!strcmp(name, "TMv2NodeMult")) {
    g_tmv2_node_mult = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2NodeMin")) {
    g_tmv2_node_min = value < 10 ? 10 : (value > 100 ? 100 : value);
    return true;
  }
  if (!strcmp(name, "TMv2NodeMax")) {
    g_tmv2_node_max = value < 100 ? 100 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Alloc")) {
    g_tmv2_alloc = value != 0;
    return true;
  }
  if (!strcmp(name, "TMv2MtgBase")) {
    g_tmv2_mtg_base = value < 2 ? 2 : value;
    return true;
  }
  if (!strcmp(name, "TMv2MtgSlope")) {
    g_tmv2_mtg_slope = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "TMv2MtgMin")) {
    g_tmv2_mtg_min = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TMv2OptPct")) {
    g_tmv2_opt_pct = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "TMv2Predicted")) {
    g_tmv2_pred = value != 0;
    if (!g_tmv2_pred)
      g_tm_pred_hash = 0;
    return true;
  }
  if (!strcmp(name, "TMPredBestThread")) {
    g_tm_pred_best_thread = value != 0;
    return true;
  }
  if (!strcmp(name, "TMv2PredHit")) {
    g_tmv2_pred_hit = value < 100 ? 100 : value;
    return true;
  }
  if (!strcmp(name, "TMv2PredMiss")) {
    g_tmv2_pred_miss = value < 100 ? 100 : value;
    return true;
  }
  if (!strcmp(name, "TMv2RootSingular")) {
    g_tmv2_rsing = value != 0;
    return true;
  }
  if (!strcmp(name, "TMv2RSMarginBase")) {
    g_tmv2_rs_margin_base = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TMv2RSMarginMin")) {
    g_tmv2_rs_margin_min = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TMv2RSMarginSlope")) {
    g_tmv2_rs_margin_slope = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "TMv2RSTimeFrac")) {
    g_tmv2_rs_timefrac = value < 0 ? 0 : (value > 100 ? 100 : value);
    return true;
  }
  if (!strcmp(name, "TMv2RSScoreCap")) {
    g_tmv2_rs_scorecap = value < 100 ? 100 : value;
    return true;
  }
  if (!strcmp(name, "TMv2SingleReply")) {
    g_tmv2_single_reply = value != 0;
    return true;
  }
  if (!strcmp(name, "TMv2MateStop")) {
    g_tmv2_mate_stop = value != 0;
    return true;
  }
  if (!strcmp(name, "TMv2MateIters")) {
    g_tmv2_mate_iters = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TTCutBonusScale")) {
    g_ttcut_bonus_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "NMPVerifDepth")) {
    g_nmp_verif_depth = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "LMPCheckGuard")) {
    g_lmp_check_guard = value != 0;
    return true;
  }
  if (!strcmp(name, "LMPBase")) {
    g_lmp_base = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMPQuad")) {
    g_lmp_quad = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CheckExtDepth")) {
    g_check_ext_depth = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LMRStatScoreDiv")) {
    g_lmr_ss_div = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "LMRStatScoreOffset")) {
    g_lmr_ss_offset = value;
    return true;
  }
  if (!strcmp(name, "LMRContHistDiv")) {
    g_lmr_ch_div = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "CutNodeLMRExtra")) {
    g_cutnode_lmr_extra = value;
    return true;
  }
  if (!strcmp(name, "TTPvAmount")) {
    g_ttpv_amount = value < 0 ? 0 : (value > 2 ? 2 : value);
    return true;
  }
  if (!strcmp(name, "ThreatScale")) {
    g_threat_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "ThreatHistWeight")) {
    g_threat_hist_weight = value < 0 ? 0 : value;
    return true;
  }
  // FailHighSmooth: i tre pesi separati (scala /1024, 512 = midpoint storico).
  if (!strcmp(name, "FHTQsStandPat")) {
    g_fh_t_qs_standpat = value;
    return true;
  }
  if (!strcmp(name, "FHTQsFinal")) {
    g_fh_t_qs_final = value;
    return true;
  }
  if (!strcmp(name, "FHTRfp")) {
    g_fh_t_rfp = value;
    return true;
  }
  if (!strcmp(name, "CheckBonus")) {
    g_check_bonus = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "QuietOffense")) {
    g_quiet_offense = value != 0;
    return true;
  }
  if (!strcmp(name, "OffenseBonus")) {
    g_offense_bonus = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "WallPawnPenalty")) {
    g_wallpawn_penalty = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "ContHist36Weight")) {
    g_conthist36_weight = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PriorBonusScale")) {
    g_prior_bonus_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PriorBonusFactorScale")) {
    g_pb_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PBBase")) {
    g_pb_base = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PBMoveCount")) {
    g_pb_mc = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PBMoveCountCap")) {
    g_pb_mc_cap = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PBTTMove")) {
    g_pb_ttm = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PBEval")) {
    g_pb_ev = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PBEvalMargin")) {
    g_pb_ev_marg = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PBSwing")) {
    g_pb_swing = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PBSwingMargin")) {
    g_pb_swing_marg = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "LowPlyWeight")) {
    g_lowply_weight = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "StatEvalDiffMult")) {
    g_seo_mult = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "CutoffCntPenalty")) {
    g_cutoffcnt_penalty = value < 0 ? 0 : value;
    return true;
  }
  // F-018 (secondo audit): micro-tecniche Obsidian/Berserk, default OFF/neutro.
  if (!strcmp(name, "PostLMRHist")) {
    g_postlmr_hist = value != 0;
    return true;
  }
  if (!strcmp(name, "PostLMRHistScale")) {
    g_postlmr_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "TTCutRefine")) {
    g_ttcut_refine = value != 0;
    return true;
  }
  if (!strcmp(name, "TTResearch")) {
    g_tt_research = value != 0;
    return true;
  }
  if (!strcmp(name, "TTResearchMargin")) {
    g_tt_research_margin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "TTCutFifty")) {
    g_ttcut_fifty = value < 1 ? 1 : value;
    return true;
  }
  if (!strcmp(name, "TTCutMalus")) {
    g_ttcut_malus = value != 0;
    return true;
  }
  if (!strcmp(name, "TTCutMalusSeen")) {
    g_ttcut_malus_seen = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "GoodCapHistDiv")) {
    g_goodcap_hist_div = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "AspAvg")) {
    g_asp_avg = value != 0;
    return true;
  }
  if (!strcmp(name, "AspFHRed")) {
    g_asp_fhred = value != 0;
    return true;
  }
  if (!strcmp(name, "SingularMinDepth")) {
    g_singular_mindepth = value < 4 ? 4 : value;
    return true;
  }
  if (!strcmp(name, "SingularDECap")) {
    g_singular_de_cap = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "SingularPlyGuard")) {
    g_singular_plyguard = value != 0;
    return true;
  }
  if (!strcmp(name, "NegExtAlpha")) {
    g_negext_alpha = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "FailHighSmooth")) {
    g_fh_smooth = value != 0;
    return true;
  }
  if (!strcmp(name, "NMPStaticMargin")) {
    g_nmp_static_margin = value != 0;
    return true;
  }
  if (!strcmp(name, "NMPStaticMult")) {
    g_nmp_sm_mult = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "NMPStaticBias")) {
    g_nmp_sm_bias = value;
    return true;
  }
  if (!strcmp(name, "NMPTTNoisy")) {
    g_nmp_ttnoisy = value != 0;
    return true;
  }
  if (!strcmp(name, "AlphaDepthDec")) {
    g_alpha_depth_dec = value != 0;
    return true;
  }
  if (!strcmp(name, "FHBoostMargin")) {
    g_fhboost_margin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "HistTrivGuard")) {
    g_hist_triv_guard = value != 0;
    return true;
  }
  if (!strcmp(name, "MalusPct")) {
    g_malus_pct = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "EasyCapGate")) {
    g_easycap_gate = value != 0;
    return true;
  }
  if (!strcmp(name, "RFPHistThresh")) {
    g_rfp_hist_thresh = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "KillerReset")) {
    g_killer_reset = value != 0;
    return true;
  }
  if (!strcmp(name, "CounterMove")) {
    g_countermove = value != 0;
    return true;
  }
  if (!strcmp(name, "QSMoveCap")) {
    g_qs_move_cap = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "QSDrawCheck")) {
    g_qs_draw_check = value != 0;
    return true;
  }
  if (!strcmp(name, "EPKeyFix")) {
    g_ep_key_fix = value != 0;
    return true;
  }
  if (!strcmp(name, "CutoffStats")) {
    g_cutoff_stats = value != 0;
    return true;
  }
  if (!strcmp(name, "EvalTTWrite")) {
    g_eval_tt_write = value != 0;
    return true;
  }
  if (!strcmp(name, "TTTwoLevel")) {
    g_tt_twolevel = value != 0;
    return true;
  }
  if (!strcmp(name, "TTMoveKeep")) {
    g_tt_move_keep = value != 0;
    return true;
  }
  if (!strcmp(name, "ProbCutInCheckMargin")) {
    g_probcut_incheck_margin = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "PawnHistoryWeight")) {
    g_pawn_hist_weight = value;
    return true;
  }
  if (!strcmp(name, "MainHistWeight")) {
    g_mainhist_weight = value < 1 ? 1 : value;
    return true;
  } // /100
  if (!strcmp(name, "ContHistWeight")) {
    g_conthist_weight = value < 1 ? 1 : value;
    return true;
  } // /100
  if (!strcmp(name, "LMPScale")) {
    g_lmp_scale = value < 10 ? 10 : value;
    return true;
  }
  if (!strcmp(name, "AggrLMRDiv")) {
    g_aggr_lmr_div = value;
    return true;
  }
  if (!strcmp(name, "AggrLMRClamp")) {
    g_aggr_lmr_clamp = value;
    return true;
  }
  if (!strcmp(name, "NMPBase")) {
    g_nmp_base = value;
    return true;
  }
  if (!strcmp(name, "NMPDiv")) {
    g_nmp_div = value;
    return true;
  }
  if (!strcmp(name, "LMREvalMargin")) {
    g_lmr_eval_margin = value;
    return true;
  }
  if (!strcmp(name, "LMRTTDepth")) {
    g_lmr_ttdepth = value;
    return true;
  }
  if (!strcmp(name, "LMRBase")) {
    g_lmr_base_x100 = value;
    init_lmr_table();
    return true;
  }
  if (!strcmp(name, "LMRDiv")) {
    g_lmr_div_x100 = value;
    init_lmr_table();
    return true;
  }
  if (!strcmp(name, "DiverseSMPAmount")) {
    g_diverse_smp_amount = value;
    return true;
  }
  if (!strcmp(name, "DiverseSMPFine")) {
    g_diverse_smp_fine = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "MulticutTTMalus")) {
    g_multicut_ttmalus = value != 0;
    return true;
  }
  if (!strcmp(name, "MulticutTTMalusScale")) {
    g_multicut_ttmalus_scale = value < 0 ? 0 : value;
    return true;
  }
  if (!strcmp(name, "NMPEvalDiv")) {
    g_nmp_eval_div = value;
    return true;
  }
  if (!strcmp(name, "QFutMargin")) {
    g_qfut_margin = value;
    return true;
  }
  if (!strcmp(name, "HistBonusMult")) {
    g_hist_bonus_mult = value;
    return true;
  }
  if (!strcmp(name, "HistBonusSub")) {
    g_hist_bonus_sub = value;
    return true;
  }
  if (!strcmp(name, "HistBonusMax")) {
    g_hist_bonus_max = value;
    return true;
  }
  if (!strcmp(name, "CaptureHistDiv")) {
    g_caphist_div = value < 1 ? 1 : value;
    return true;
  }
  return false;
}

// ---- Self-play data logging (Phase 1: policy-net training dataset) ----------
// When enabled, each completed root search appends one TSV record:
//   <FEN> \t <bestmove-uci> \t <score-cp (side-to-move relative)> \t <depth>
// Toggled via the DataLog / DataFile UCI options. The actual output file gets a
// ".<pid>" suffix per process, so multiple parallel self-play instances never
// collide; merge them afterwards (e.g. cat triumviratus_dataset.txt.* >
// all.txt).
static bool g_data_log_enabled = false;
static std::string g_data_log_file = "triumviratus_dataset.txt";

void set_data_log_enabled(bool enabled) { g_data_log_enabled = enabled; }
void set_data_log_file(const char *path) {
  if (path && *path)
    g_data_log_file = path;
}

static void log_search_record(int best_move, int score, int depth) {
  // Per-process file (".<pid>") so parallel self-play instances never
  // interleave/corrupt one another. Merge later: cat <DataFile>.* > all.txt
#ifdef _WIN32
  unsigned long tri_pid = (unsigned long)GetCurrentProcessId();
#else
  unsigned long tri_pid = (unsigned long)getpid();
#endif
  std::string path = g_data_log_file + "." + std::to_string(tri_pid);
  std::ofstream out(path, std::ios::app | std::ios::binary); // pure '\n'
  if (!out)
    return;
  out << board_to_fen() << '\t' << move_to_uci(best_move) << '\t' << score
      << '\t' << depth << '\n';
}
U64 hash_entries = 0;
int current_age = 0;

extern U64 nodes;

std::vector<std::thread> search_threads;
std::vector<ThreadData> thread_data;
std::atomic<bool> stop_threads(false);

// A5 FIX 2026-07-25 — pavimento anti-forfeit del taglio DURO.
// Il taglio su `stoptime` puo' scattare prima che il thread 0 abbia UNA mossa di
// radice (optimum puo' valere pochi ms: tempo residuo minimo, Move Overhead che
// mangia tutto, o il confronto sul tempo che sbaglia nella finestra di overflow
// int32 di GetTickCount, ogni 49.7 giorni di uptime). In quel caso la ricerca si
// smonta con best_move = 0 e il rescue di last-resort emette la PRIMA MOSSA
// LEGALE, cioe' una mossa a caso: si perde la posizione invece del tempo.
// Rimandiamo il taglio duro finche' non esiste una mossa vera. La finestra e'
// limitata a UNA mossa di radice a depth 1 (start_depth = 1 per il thread 0),
// quindi non puo' causare un forfeit. Stessa scelta di Reckless (time.rs:81-84).
// Costo: ZERO nel path caldo — grazie allo short-circuit questa funzione viene
// valutata solo DOPO che `get_time_ms() > stoptime` e' gia' vero, cioe' una volta
// per ricerca. Il taglio da comando `stop` e da g_node_limit non passa da qui.
static inline bool td_root_move_ready() {
  return !thread_data.empty() && (thread_data[0].best_move != 0 ||
                                  thread_data[0].pv_table[0][0] != 0);
}
U64 g_node_limit =
    0; // "go nodes N" budget (0 = off); checked per-node in (q)search.
// searchmoves (analisi, UCI "go searchmoves m1 m2 ..."): whitelist di mosse di
// root. count>0 => alla root si cercano SOLO queste (le altre saltate nel move
// loop). Vale anche per le linee MultiPV. Riempita da parse_go, azzerata a ogni
// nuovo 'go'.
int g_searchmoves[256];
int g_searchmoves_count = 0;
// "go mate N": fermati appena un'iterazione completata conferma un matto NOSTRO
// in <= N mosse (0 = off). Convenzione UCI; il display mate M usa la stessa
// formula.
int g_mate_in = 0;
std::mutex output_mutex;
int num_threads = 1;
int search_start_time = 0;
int soft_time_limit = 0;

// ============================================================================
// LMR TABLE
// ============================================================================

// F-008: helper SEE-a-soglia (toggle g_see_ge/g_see_ge_verify definiti sopra,
// prima di set_search_param). In verify si gioca col CLASSICO (invariato).
static inline int td_see_at_least(ThreadData &td, int move, int thr) {
  if (g_see_ge_verify) {
    const int ge = td_see_ge(td, move, thr);
    const int cl = (td_see(td, move) >= thr) ? 1 : 0;
    ++g_seege_checks;
    if (ge != cl) {
      const long long b = ++g_seege_bad;
      std::lock_guard<std::mutex> lk(output_mutex);
      printf("info string SeeGE MISMATCH #%lld move=%d thr=%d ge=%d classic=%d "
             "(checks=%lld)\n",
             b, move, thr, ge, cl, (long long)g_seege_checks);
      fflush(stdout);
    }
    return cl;
  }
  return g_see_ge ? td_see_ge(td, move, thr)
                  : ((td_see(td, move) >= thr) ? 1 : 0);
}

int lmr_table[64][64];

void init_lmr_table() {
  for (int depth = 0; depth < 64; depth++) {
    for (int moves = 0; moves < 64; moves++) {
      if (depth == 0 || moves == 0) {
        lmr_table[depth][moves] = 0;
      } else {
        lmr_table[depth][moves] =
            (int)(g_lmr_base_x100 / 100.0 +
                  log(depth) * log(moves) / (g_lmr_div_x100 / 100.0));
      }
    }
  }
}

// LMP thresholds
const int lmp_table[9] = {0, 3, 5, 8, 13, 20, 30, 42, 56};

// ============================================================================
// UNDO STRUCTURE
// ============================================================================

struct UndoInfo {
  int captured_piece;
  int captured_square;
  int old_castle;
  int old_enpassant;
  int old_fifty;
  U64 old_hash;
};

// ============================================================================
// THREAD INITIALIZATION
// ============================================================================

// Q-26 HistPrior (Caissa): dopo un azzeramento, riempi le history col PRIOR
// positivo (0 = zero-init storico). Da chiamare OVUNQUE si azzerino le history,
// cosi' il prior non dipende dal punto di reset (init_threads / ucinewgame /
// bench).
void apply_history_priors(ThreadData &td) {
  if (g_hist_init_quiet) {
    for (auto &bucket : td.history_moves)
      for (auto &piece : bucket)
        for (int &v : piece)
          v = g_hist_init_quiet;
  }
  if (g_hist_init_capt) {
    for (auto &piece : td.capture_history)
      for (auto &sq : piece)
        for (auto &vic : sq) // CapHistThreat: livello bucket-minaccia
          for (int &v : vic)
            v = g_hist_init_capt;
  }
}

// Q-25 HistAge (Stormphrax 665b76b = 750/1024; Pawnocchio 3/4; Caissa 7/8): la
// history DECADE tra una mossa e l'altra (evidenza vecchia = meno affidabile).
// 1024 = off. Solo history_moves + capture_history: le conthist sono
// 12*64*12*64 int16 = 2.4M entry per tabella, scansionarle a ogni `go`
// costerebbe piu' del guadagno atteso. Q-26 interagisce: il decay tira verso 0,
// il prior sposta il punto di riposo -> co-tunare.
static void age_histories(ThreadData &td) {
  if (g_hist_age >= 1024)
    return;
  for (auto &bucket : td.history_moves)
    for (auto &piece : bucket)
      for (int &v : piece)
        v = (int)((long long)v * g_hist_age / 1024);
  for (auto &piece : td.capture_history)
    for (auto &sq : piece)
      for (auto &vic : sq) // CapHistThreat: livello bucket-minaccia
        for (int &v : vic)
          v = (int)((long long)v * g_hist_age / 1024);
}

void init_threads(int thread_count) {
  if (thread_count < 1)
    thread_count = 1;
  if (thread_count > MAX_THREADS)
    thread_count = MAX_THREADS;

  // Free any existing incremental-NNUE handles before resizing (resize may
  // move/destroy elements); they are recreated fresh in the loop below.
  for (auto &td : thread_data)
    if (td.nnpos) {
      nn_pos_destroy(td.nnpos);
      td.nnpos = nullptr;
    }

  num_threads = thread_count;
  thread_data.resize(num_threads);

  static bool lmr_initialized = false;
  if (!lmr_initialized) {
    init_lmr_table();
    lmr_initialized = true;
  }

  // P1.2: tabelle cuckoo + between per l'upcoming-repetition. Qui Zobrist e
  // magic sono gia' pronti (init_bitboards gira prima in main). Una volta sola.
  static bool cuckoo_initialized = false;
  if (!cuckoo_initialized) {
    init_cuckoo();
    cuckoo_initialized = true;
  }

  for (int i = 0; i < num_threads; i++) {
    thread_data[i].thread_id = i;
    thread_data[i].nnpos = nn_pos_create();
    thread_data[i].nodes = 0;
    thread_data[i].tb_hits = 0;
    thread_data[i].best_move = 0;
    thread_data[i].best_score = -infinity;
    thread_data[i].depth = 0;
    memset(thread_data[i].killer_moves, 0, sizeof(thread_data[i].killer_moves));
    memset(thread_data[i].history_moves, 0,
           sizeof(thread_data[i].history_moves));
    memset(thread_data[i].counter_moves, 0,
           sizeof(thread_data[i].counter_moves));
    memset(thread_data[i].capture_history, 0,
           sizeof(thread_data[i].capture_history));
    memset(thread_data[i].pv_table, 0, sizeof(thread_data[i].pv_table));
    memset(thread_data[i].pv_length, 0, sizeof(thread_data[i].pv_length));
    memset(thread_data[i].eval_stack, 0, sizeof(thread_data[i].eval_stack));
    memset(thread_data[i].reduction_stack, 0,
           sizeof(thread_data[i].reduction_stack));
    // Zero the eval cache once. Entries stay valid across searches (the
    // 64-bit key guards correctness), so we deliberately do NOT clear it
    // per-go — that preserves hits from transpositions across moves.
    memset(thread_data[i].eval_cache, 0, sizeof(thread_data[i].eval_cache));
    memset(thread_data[i].corr_hist, 0, sizeof(thread_data[i].corr_hist));
    memset(thread_data[i].corr_hist_minor, 0,
           sizeof(thread_data[i].corr_hist_minor));
    memset(thread_data[i].corr_hist_major, 0,
           sizeof(thread_data[i].corr_hist_major));
    memset(thread_data[i].corr_hist_material, 0,
           sizeof(thread_data[i].corr_hist_material));
    memset(thread_data[i].corr_hist_np, 0, sizeof(thread_data[i].corr_hist_np));
    memset(thread_data[i].cont_corr_hist, 0,
           sizeof(thread_data[i].cont_corr_hist));
    memset(thread_data[i].pawn_history, 0, sizeof(thread_data[i].pawn_history));
    // Q-11 NodeCache: azzerata al boot. NON per-go (le entry sopravvivono
    // cross-move: e' il punto della tecnica). La chiave piena garantisce che
    // un'entry di un'altra posizione non venga mai letta come valida.
    memset(thread_data[i].node_cache, 0, sizeof(thread_data[i].node_cache));
    thread_data[i].nc_gen = 0;
    apply_history_priors(thread_data[i]); // Q-26 (no-op se prior = 0)
  }
}

void copy_board_to_thread(ThreadData &td) {
  memcpy(td.bitboards, bitboards, sizeof(bitboards));
  memcpy(td.occupancies, occupancies, sizeof(occupancies));
#ifndef TRIUMV_NO_MAILBOX
  // Mailbox: full init dalla root, poi mantenuta in make/unmake — stesso schema di
  // pawn_key e np_key qui sotto. Unico punto di inizializzazione della board di thread.
  for (int sq = 0; sq < 64; sq++)
    td.piece_on[sq] = -1;
  for (int pc = P; pc <= k; pc++) {
    U64 bb = td.bitboards[pc];
    while (bb) {
      int sq = get_ls1b_index(bb);
      td.piece_on[sq] = pc;
      pop_bit(bb, sq);
    }
  }
#endif
  td.side = side;
  td.enpassant = enpassant;
  td.castle = castle;
  td.hash_key = hash_key;
  td.fifty = fifty;
  // P2.1: pawn key incrementale — full init dal board di root, poi mantenuta
  // in make/unmake da td_pawn_key_update (XOR self-inverse, stile OccIncr).
  td.pawn_key = 0;
  for (int pc = P; pc <= p; pc += (p - P)) {
    U64 bb = td.bitboards[pc];
    while (bb) {
      int sq = get_ls1b_index(bb);
      td.pawn_key ^= piece_keys[pc][sq];
      pop_bit(bb, sq);
    }
  }
  // CorrNonPawn: np_key per colore — full init dal board di root, poi mantenute
  // in make/unmake da td_np_key_update (XOR self-inverse).
  td.np_key[white] = td.np_key[black] = 0;
  for (int pc = N; pc <= K; pc++) {
    U64 bb = td.bitboards[pc];
    while (bb) {
      int sq = get_ls1b_index(bb);
      td.np_key[white] ^= piece_keys[pc][sq];
      pop_bit(bb, sq);
    }
  }
  for (int pc = n; pc <= k; pc++) {
    U64 bb = td.bitboards[pc];
    while (bb) {
      int sq = get_ls1b_index(bb);
      td.np_key[black] ^= piece_keys[pc][sq];
      pop_bit(bb, sq);
    }
  }
  // P2.2: alla radice non ci sono null nel cammino -> finestra limitata solo
  // da fifty/storia. 1024 = "nessuna null vista".
  td.plies_from_null = 1024;
  td.in_nmp_verif = false; // P1.6
  td.seldepth = 0;
  memcpy(td.repetition_table, repetition_table, sizeof(repetition_table));
  td.repetition_index = repetition_index;
  td.ply = 0;
  td.nodes = 0;
  td.best_move = 0;
  td.best_score = -infinity;
  td.depth = 0;
  // Q-11 NodeCache (Caissa::OnNewSearch): nuova generazione. Le entry della
  // search precedente restano LEGGIBILI (statistiche cross-move) ma sono
  // rimpiazzabili. NB: parte da 1 al primo `go` (init a 0), cosi' le entry
  // vergini (gen=0) sono subito allocabili.
  td.nc_gen++;
  // Q-25 HistAge: una ricerca = una mossa giocata -> invecchia la history.
  // No-op a 1024. NB nel bench le history sono azzerate prima di ogni
  // posizione, quindi il decay agisce su tabelle vuote = bench invariato anche
  // con HistAge != 1024.
  age_histories(td);
}

// ============================================================================
// ATTACK DETECTION
// ============================================================================

static inline int td_is_square_attacked(ThreadData &td, int square,
                                        int attacker_side) {
  if (attacker_side == white) {
    if (pawn_attacks[black][square] & td.bitboards[P])
      return 1;
    if (knight_attacks[square] & td.bitboards[N])
      return 1;
    if (king_attacks[square] & td.bitboards[K])
      return 1;
    U64 occ = td.occupancies[both];
    if (get_bishop_attacks(square, occ) & (td.bitboards[B] | td.bitboards[Q]))
      return 1;
    if (get_rook_attacks(square, occ) & (td.bitboards[R] | td.bitboards[Q]))
      return 1;
  } else {
    if (pawn_attacks[white][square] & td.bitboards[p])
      return 1;
    if (knight_attacks[square] & td.bitboards[n])
      return 1;
    if (king_attacks[square] & td.bitboards[k])
      return 1;
    U64 occ = td.occupancies[both];
    if (get_bishop_attacks(square, occ) & (td.bitboards[b] | td.bitboards[q]))
      return 1;
    if (get_rook_attacks(square, occ) & (td.bitboards[r] | td.bitboards[q]))
      return 1;
  }
  return 0;
}

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
  c.chk[B] = get_bishop_attacks(eksq, occ);
  c.chk[R] = get_rook_attacks(eksq, occ);
  c.chk[Q] = c.chk[B] | c.chk[R];
  c.chk[P] =
      pawn_attacks[us ^ 1][eksq]; // da dove un NOSTRO pedone attacca quel re
  c.chk[K] = 0;

  // Scacchi di scoperta: un nostro pezzo e' l'unico schermo fra un nostro
  // slider e il re nemico. Il re nemico NON e' sotto scacco (e' il nostro
  // tratto), quindi qualunque nostro R/Q (risp. B/Q) che compare togliendo lo
  // schermo e' per forza dietro di esso.
  const U64 ours = td.occupancies[us];
  const U64 orq = td.bitboards[R + off] | td.bitboards[Q + off];
  const U64 obq = td.bitboards[B + off] | td.bitboards[Q + off];
  U64 cand = c.chk[R] & ours;
  while (cand) {
    int s = get_ls1b_index(cand);
    pop_bit(cand, s);
    if (get_rook_attacks(eksq, occ ^ (1ULL << s)) & orq)
      c.disc |= 1ULL << s;
  }
  cand = c.chk[B] & ours;
  while (cand) {
    int s = get_ls1b_index(cand);
    pop_bit(cand, s);
    if (get_bishop_attacks(eksq, occ ^ (1ULL << s)) & obq)
      c.disc |= 1ULL << s;
  }
  c.ready = true;
}

// ponytail: sovrastima di proposito. Un pezzo "disc" che si muove RESTANDO
// sulla linea del re non scopre nulla, ma qui conta comunque come scacco (manca
// una tabella aligned()) — e lo stesso per l'arrocco, dove la torre puo' dare
// scacco dalla casa d'arrivo. Sovrastimare = potare MENO = direzione sicura per
// un guard. Se il patch rende, si affina.
static inline bool td_lmp_gives_check(ThreadData &td, int move, LmpChkCtx &c) {
  if (!c.ready)
    td_lmp_chk_init(td, c);
  const int tgt = get_move_target(move);
  const int pt = get_move_piece(move) % 6; // P..K indipendente dal colore
  return (c.chk[pt] & (1ULL << tgt)) ||
         (c.disc & (1ULL << get_move_source(move)));
}

// Incremental occupancy update. Flips only the bits that the move touches,
// instead of OR-recomputing all 12 bitboards (+2.89% NPS, node-identical). XOR
// is self-inverse, so calling it again with the same move reverses it (used on
// make-forward, illegal rollback, and unmake). 'us' must be the MOVING side at
// the call site.
// captured_piece == -1 means no capture. For castling the rook squares derive
// from the king target. Produces occupancies identical to the full recompute.
static inline void td_occ_update(ThreadData &td, int us, int source, int target,
                                 int captured_square, int captured_piece,
                                 int castling) {
  td.occupancies[us] ^= (1ULL << source) | (1ULL << target);
  if (captured_piece != -1)
    td.occupancies[us ^ 1] ^=
        (1ULL << captured_square); // capture (== target, or e.p. square)
  if (castling) {
    int rf, rt;
    switch (target) {
    case g1:
      rf = h1;
      rt = f1;
      break;
    case c1:
      rf = a1;
      rt = d1;
      break;
    case g8:
      rf = h8;
      rt = f8;
      break;
    default:
      rf = a8;
      rt = d8;
      break; // c8
    }
    td.occupancies[us] ^= (1ULL << rf) | (1ULL << rt);
  }
  td.occupancies[both] = td.occupancies[white] | td.occupancies[black];
}

#ifndef TRIUMV_NO_MAILBOX
// Casa di partenza/arrivo della TORRE nell'arrocco, dedotte dalla casa d'arrivo del
// re. Stessa tabella implicita gia' usata da td_occ_update e dai due rollback.
static inline void td_castle_rook(int target, int &rook_pc, int &rf, int &rt) {
  switch (target) {
  case g1: rook_pc = R; rf = h1; rt = f1; break;
  case c1: rook_pc = R; rf = a1; rt = d1; break;
  case g8: rook_pc = r; rf = h8; rt = f8; break;
  default: rook_pc = r; rf = a8; rt = d8; break;  // c8
  }
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
  if (captured_piece != -1)
    td.piece_on[captured_square] = -1;
  td.piece_on[source] = -1;
  td.piece_on[target] = promoted ? promoted : piece;
  if (castling) {
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    td.piece_on[rf] = -1;
    td.piece_on[rt] = rook_pc;
  }
}

static inline void td_mailbox_revert(ThreadData &td, int piece, int source, int target,
                                     int promoted, int captured_piece,
                                     int captured_square, int castling) {
  (void) promoted;  // in uscita il pezzo torna quello di partenza, promosso o no
  // Ordine speculare: si svuota target e POI si rimette la vittima, cosi' quando
  // captured_square == target (cattura normale) la vittima riprende il suo posto.
  td.piece_on[target] = -1;
  td.piece_on[source] = piece;
  if (captured_piece != -1)
    td.piece_on[captured_square] = captured_piece;
  if (castling) {
    int rook_pc, rf, rt;
    td_castle_rook(target, rook_pc, rf, rt);
    td.piece_on[rt] = -1;
    td.piece_on[rf] = rook_pc;
  }
}
#endif

// ============================================================================
// MAKE MOVE (returns 1 if legal)
// ============================================================================

// P2.1: aggiorna td.pawn_key (Zobrist solo-pedoni) via XOR. Self-inverse ->
// stessa chiamata (stessi argomenti) nei 3 siti di td_occ_update: make-forward,
// rollback della mossa illegale, unmake. Pedone mosso: via da source, atterra
// su target solo se NON promuove. Pedone catturato (anche e.p.): via da
// captured_square. Castling e null move non toccano pedoni. Consumata da
// td_corr_index (g_pawn_key_incr).
static inline void td_pawn_key_update(ThreadData &td, int piece, int source,
                                      int target, int promoted,
                                      int captured_piece, int captured_square) {
  if (piece == P || piece == p) {
    td.pawn_key ^= piece_keys[piece][source];
    if (!promoted)
      td.pawn_key ^= piece_keys[piece][target];
  }
  if (captured_piece == P || captured_piece == p)
    td.pawn_key ^= piece_keys[captured_piece][captured_square];
}

// CorrNonPawn: aggiorna td.np_key[colore] (Zobrist NON-pedoni per colore) via
// XOR. Self-inverse -> stessa chiamata nei 3 siti di td_pawn_key_update. Casi:
// mover non-pedone (via da source, atterra su target); PROMOZIONE (il pezzo
// promosso APPARE su target — il pedone che sparisce e' affare del pawn_key);
// cattura di non-pedone (via da captured_square, chiave del colore CATTURATO);
// ARROCCO (la torre si muove — caso assente nel pawn-key, i pedoni non
// arroccano).
static inline void td_np_key_update(ThreadData &td, int piece, int source,
                                    int target, int promoted,
                                    int captured_piece, int captured_square,
                                    int castling) {
  if (piece != P && piece != p) {
    const int c = (piece >= p) ? black : white;
    td.np_key[c] ^= piece_keys[piece][source];
    td.np_key[c] ^= piece_keys[piece][target];
  } else if (promoted) {
    const int c = (promoted >= p) ? black : white;
    td.np_key[c] ^= piece_keys[promoted][target];
  }
  if (captured_piece != -1 && captured_piece != P && captured_piece != p) {
    const int c = (captured_piece >= p) ? black : white;
    td.np_key[c] ^= piece_keys[captured_piece][captured_square];
  }
  if (castling) {
    switch (target) {
    case g1:
      td.np_key[white] ^= piece_keys[R][h1] ^ piece_keys[R][f1];
      break;
    case c1:
      td.np_key[white] ^= piece_keys[R][a1] ^ piece_keys[R][d1];
      break;
    case g8:
      td.np_key[black] ^= piece_keys[r][h8] ^ piece_keys[r][f8];
      break;
    case c8:
      td.np_key[black] ^= piece_keys[r][a8] ^ piece_keys[r][d8];
      break;
    }
  }
}

static inline int td_make_move(ThreadData &td, int move, UndoInfo &undo) {
  PROF_GUARD(prof_make);
  undo.old_castle = td.castle;
  undo.old_enpassant = td.enpassant;
  undo.old_fifty = td.fifty;
  undo.old_hash = td.hash_key;
  undo.captured_piece = -1;
  undo.captured_square = -1;

  int source = get_move_source(move);
  int target = get_move_target(move);
  int piece = get_move_piece(move);
  int promoted = get_move_promoted(move);
  int capture = get_move_capture(move);
  int double_push = get_move_double(move);
  int enpass = get_move_enpassant(move);
  int castling = get_move_castling(move);

  pop_bit(td.bitboards[piece], source);
  set_bit(td.bitboards[piece], target);
  td.hash_key ^= piece_keys[piece][source];
  td.hash_key ^= piece_keys[piece][target];

  td.fifty++;
  if (piece == P || piece == p)
    td.fifty = 0;

  if (capture) {
    td.fifty = 0;

    if (enpass) {
      undo.captured_square = (td.side == white) ? target + 8 : target - 8;
      undo.captured_piece = (td.side == white) ? p : P;
    } else {
      undo.captured_square = target;
      int start = (td.side == white) ? p : P;
      int end = (td.side == white) ? k : K;
#ifndef TRIUMV_NO_MAILBOX
      // La mailbox e' ancora quella PRE-mossa qui: si aggiorna piu' sotto, insieme
      // a td_occ_update. Il test di range replica esattamente il ciclo sostituito,
      // che cercava solo fra i pezzi AVVERSARI e lasciava -1 se non trovava nulla.
      const int mb_pc = td.piece_on[target];
      if (mb_pc >= start && mb_pc <= end)
        undo.captured_piece = mb_pc;
#else
      for (int pc = start; pc <= end; pc++) {
        if (get_bit(td.bitboards[pc], target)) {
          undo.captured_piece = pc;
          break;
        }
      }
#endif
    }

    if (undo.captured_piece != -1) {
      pop_bit(td.bitboards[undo.captured_piece], undo.captured_square);
      td.hash_key ^= piece_keys[undo.captured_piece][undo.captured_square];
    }
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
    if (!g_ep_key_fix || (pawn_attacks[td.side][ep_sq] &
                          td.bitboards[(td.side == white) ? p : P])) {
      td.enpassant = ep_sq;
      td.hash_key ^= enpassant_keys[ep_sq];
    }
  }

  if (castling) {
    switch (target) {
    case g1:
      pop_bit(td.bitboards[R], h1);
      set_bit(td.bitboards[R], f1);
      td.hash_key ^= piece_keys[R][h1];
      td.hash_key ^= piece_keys[R][f1];
      break;
    case c1:
      pop_bit(td.bitboards[R], a1);
      set_bit(td.bitboards[R], d1);
      td.hash_key ^= piece_keys[R][a1];
      td.hash_key ^= piece_keys[R][d1];
      break;
    case g8:
      pop_bit(td.bitboards[r], h8);
      set_bit(td.bitboards[r], f8);
      td.hash_key ^= piece_keys[r][h8];
      td.hash_key ^= piece_keys[r][f8];
      break;
    case c8:
      pop_bit(td.bitboards[r], a8);
      set_bit(td.bitboards[r], d8);
      td.hash_key ^= piece_keys[r][a8];
      td.hash_key ^= piece_keys[r][d8];
      break;
    }
  }

  td.hash_key ^= castle_keys[td.castle];
  td.castle &= castling_rights[source];
  td.castle &= castling_rights[target];
  td.hash_key ^= castle_keys[td.castle];

  td_occ_update(td, td.side, source, target, undo.captured_square,
                undo.captured_piece, castling);
  if (g_pawn_key_incr)
    td_pawn_key_update(td, piece, source, target, promoted, undo.captured_piece,
                       undo.captured_square);
  td_np_key_update(td, piece, source, target, promoted, undo.captured_piece,
                   undo.captured_square, castling);
#ifndef TRIUMV_NO_MAILBOX
  td_mailbox_apply(td, piece, source, target, promoted, undo.captured_piece,
                   undo.captured_square, castling);  // sito 1/3: make-forward
  MB_VERIFY(td, "make-forward");
#endif

  td.side ^= 1;
  td.hash_key ^= side_key;

  int king_sq =
      get_ls1b_index((td.side == white) ? td.bitboards[k] : td.bitboards[K]);
  if (td_is_square_attacked(td, king_sq, td.side)) {
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
      switch (target) {
      case g1:
        pop_bit(td.bitboards[R], f1);
        set_bit(td.bitboards[R], h1);
        break;
      case c1:
        pop_bit(td.bitboards[R], d1);
        set_bit(td.bitboards[R], a1);
        break;
      case g8:
        pop_bit(td.bitboards[r], f8);
        set_bit(td.bitboards[r], h8);
        break;
      case c8:
        pop_bit(td.bitboards[r], d8);
        set_bit(td.bitboards[r], a8);
        break;
      }
    }

    td.castle = undo.old_castle;
    td.enpassant = undo.old_enpassant;
    td.fifty = undo.old_fifty;
    td.hash_key = undo.old_hash;

    td_occ_update(td, td.side, source, target, undo.captured_square,
                  undo.captured_piece, castling);
    if (g_pawn_key_incr)
      td_pawn_key_update(td, piece, source, target, promoted,
                         undo.captured_piece, undo.captured_square);
    td_np_key_update(td, piece, source, target, promoted, undo.captured_piece,
                     undo.captured_square, castling);
#ifndef TRIUMV_NO_MAILBOX
    td_mailbox_revert(td, piece, source, target, promoted, undo.captured_piece,
                      undo.captured_square, castling);  // sito 2/3: mossa illegale
    MB_VERIFY(td, "rollback-illegale");
#endif

    return 0;
  }

  // (TT + eval-cache prefetch tried here 2026-06-07: NODE-IDENTICAL but
  // NPS-neutral
  //  — we are eval-bound, not TT-memory-bound, so hiding TT latency doesn't
  //  help. Removed. The T0/pre-legality variant was -2.5% from L1 pollution.
  //  RIPROVATO 2026-07-13 dietro toggle "TTPrefetch" (default OFF =
  //  byte-identico, Reckless #1085): da giugno e' stato bakato TTTwoLevel
  //  (bucket 2 slot), il pattern di accesso TT e' cambiato -> vale una
  //  ri-misura NPS.)
  if (g_tt_prefetch)
    TT_PREFETCH(&hash_table[tt_base_index(td.hash_key)]);

  // Mirror the (now legal) move on the incremental NNUE position, in
  // Stockfish encoding. The moving piece is dirtyPiece[0] (king-refresh).
  {
    SfMove sm;
    sm.movedPiece = nn_piece_code[piece];
    sm.from = nnue_squares[source];
    sm.to = nnue_squares[target];
    sm.promoPiece = promoted ? nn_piece_code[promoted] : 0;
    sm.capturedPiece =
        (undo.captured_piece != -1) ? nn_piece_code[undo.captured_piece] : 0;
    sm.capturedSq =
        (undo.captured_piece != -1) ? nnue_squares[undo.captured_square] : -1;
    sm.rookPiece = 0;
    sm.rookFrom = -1;
    sm.rookTo = -1;
    if (castling) {
      int rpc = R, rf = h1, rt = f1;
      switch (target) {
      case g1:
        rpc = R;
        rf = h1;
        rt = f1;
        break;
      case c1:
        rpc = R;
        rf = a1;
        rt = d1;
        break;
      case g8:
        rpc = r;
        rf = h8;
        rt = f8;
        break;
      case c8:
        rpc = r;
        rf = a8;
        rt = d8;
        break;
      }
      sm.rookPiece = nn_piece_code[rpc];
      sm.rookFrom = nnue_squares[rf];
      sm.rookTo = nnue_squares[rt];
    }
    sm.rule50 = td.fifty;
    nn_pos_do(td.nnpos, &sm);
  }

  td.plies_from_null++; // P2.2: una mossa reale in piu' dall'ultima null

  return 1;
}

// ============================================================================
// UNMAKE MOVE
// ============================================================================

static inline void td_unmake_move(ThreadData &td, int move, UndoInfo &undo) {
  PROF_GUARD(prof_make);
  nn_pos_undo(td.nnpos); // retract the move on the incremental NNUE mirror
  td.plies_from_null--;  // P2.2 (speculare al ++ del make riuscito)
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

  if (undo.captured_piece != -1) {
    set_bit(td.bitboards[undo.captured_piece], undo.captured_square);
  }

  if (castling) {
    switch (target) {
    case g1:
      pop_bit(td.bitboards[R], f1);
      set_bit(td.bitboards[R], h1);
      break;
    case c1:
      pop_bit(td.bitboards[R], d1);
      set_bit(td.bitboards[R], a1);
      break;
    case g8:
      pop_bit(td.bitboards[r], f8);
      set_bit(td.bitboards[r], h8);
      break;
    case c8:
      pop_bit(td.bitboards[r], d8);
      set_bit(td.bitboards[r], a8);
      break;
    }
  }

  td.castle = undo.old_castle;
  td.enpassant = undo.old_enpassant;
  td.fifty = undo.old_fifty;
  td.hash_key = undo.old_hash;

  td_occ_update(td, td.side, source, target, undo.captured_square,
                undo.captured_piece, castling);
  if (g_pawn_key_incr)
    td_pawn_key_update(td, piece, source, target, promoted, undo.captured_piece,
                       undo.captured_square);
  td_np_key_update(td, piece, source, target, promoted, undo.captured_piece,
                   undo.captured_square, castling);
#ifndef TRIUMV_NO_MAILBOX
  td_mailbox_revert(td, piece, source, target, promoted, undo.captured_piece,
                    undo.captured_square, castling);  // sito 3/3: unmake
  MB_VERIFY(td, "unmake");
#endif
}

// ============================================================================
// MOVE GENERATION
// ============================================================================

// Case strettamente tra due quadrati allineati (0 per cavallo/non-allineati).
// Riempita da init_cuckoo; usata dall'upcoming-repetition (P1.2) e dalle
// evasioni (P2.3).
static U64 between_tbl[64][64];

// Bitboard degli attaccanti di `sq` del colore `by` (P2.3 EvasionGen).
static inline U64 td_attackers_to(ThreadData &td, int sq, int by) {
  const int off = (by == white) ? 0 : 6;
  const U64 occ = td.occupancies[both];
  return (pawn_attacks[by ^ 1][sq] & td.bitboards[P + off]) |
         (knight_attacks[sq] & td.bitboards[N + off]) |
         (king_attacks[sq] & td.bitboards[K + off]) |
         (get_bishop_attacks(sq, occ) &
          (td.bitboards[B + off] | td.bitboards[Q + off])) |
         (get_rook_attacks(sq, occ) &
          (td.bitboards[R + off] | td.bitboards[Q + off]));
}

// 🔴 quiets_only (2A, 29/07): il movepicker chiamava questa funzione con captures_only=false
// nello stage QUIET, cioe' generando TUTTE le pseudo-legali — comprese le catture che aveva
// gia' generato nello stage TACTICAL — per poi marcarle MP_CONSUMED e non renderle mai
// (threads.cpp:5299-5307). Ogni nodo che supera le good-tactical senza cutoff pagava la
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
static void td_generate_moves(ThreadData &td, moves *move_list,
                              bool captures_only = false,
                              bool quiets_only   = false,
                              int  known_in_check = -1) {
  PROF_GUARD(prof_mg);
  move_list->count = 0;
  int source_square, target_square;
  U64 bitboard, attacks;

  // --- MAGIA BITWISE: Precalcoliamo le maschere ---
  U64 enemies =
      (td.side == white) ? td.occupancies[black] : td.occupancies[white];
  U64 friends =
      (td.side == white) ? td.occupancies[white] : td.occupancies[black];
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
        get_ls1b_index((td.side == white) ? td.bitboards[K] : td.bitboards[k]);
    // 2C: se il chiamante lo sa gia', non si ri-scandiscono tutti i tipi di attaccante.
    const bool in_chk = (known_in_check >= 0)
                            ? (known_in_check != 0)
                            : td_is_square_attacked(td, ksq, td.side ^ 1);
    if (in_chk) {
      U64 checkers = td_attackers_to(td, ksq, td.side ^ 1);
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

  for (int piece = P; piece <= k; piece++) {
    bitboard = td.bitboards[piece];

    if (td.side == white) {
      if (piece == P) {
        while (bitboard) {
          source_square = get_ls1b_index(bitboard);
          target_square = source_square - 8;

          // Mosse silenziose dei pedoni: bloccate se captures_only   true
          // (P2.3: ogni approdo deve stare in evasion_mask — sotto scacco
          //  solo i blocchi sul raggio sopravvivono, fuori scacco ~0ULL no-op)
          if (!captures_only) {
            if (!(target_square < a8) &&
                !get_bit(td.occupancies[both], target_square)) {
              if (source_square >= a7 && source_square <= h7) {
                if ((evasion_mask >> target_square) & 1) {
                  add_move(move_list, encode_move(source_square, target_square,
                                                  piece, Q, 0, 0, 0, 0));
                  add_move(move_list, encode_move(source_square, target_square,
                                                  piece, R, 0, 0, 0, 0));
                  add_move(move_list, encode_move(source_square, target_square,
                                                  piece, B, 0, 0, 0, 0));
                  add_move(move_list, encode_move(source_square, target_square,
                                                  piece, N, 0, 0, 0, 0));
                }
              } else {
                if ((evasion_mask >> target_square) & 1)
                  add_move(move_list, encode_move(source_square, target_square,
                                                  piece, 0, 0, 0, 0, 0));
                if ((source_square >= a2 && source_square <= h2) &&
                    !get_bit(td.occupancies[both], target_square - 8) &&
                    ((evasion_mask >> (target_square - 8)) & 1))
                  add_move(move_list,
                           encode_move(source_square, target_square - 8, piece,
                                       0, 0, 1, 0, 0));
              }
            }
          }

          // Catture dei pedoni
          attacks = quiets_only ? 0ULL   // 2A: le catture di pedone non passano da allowed_squares
                  : (pawn_attacks[td.side][source_square] & enemies & evasion_mask);
          while (attacks) {
            target_square = get_ls1b_index(attacks);
            if (source_square >= a7 && source_square <= h7) {
              add_move(move_list, encode_move(source_square, target_square,
                                              piece, Q, 1, 0, 0, 0));
              add_move(move_list, encode_move(source_square, target_square,
                                              piece, R, 1, 0, 0, 0));
              add_move(move_list, encode_move(source_square, target_square,
                                              piece, B, 1, 0, 0, 0));
              add_move(move_list, encode_move(source_square, target_square,
                                              piece, N, 1, 0, 0, 0));
            } else {
              add_move(move_list, encode_move(source_square, target_square,
                                              piece, 0, 1, 0, 0, 0));
            }
            pop_lsb_bb(attacks);   // 2D: target_square E' l'LSB di attacks
          }

          // En passant (  sempre una cattura, lo generiamo sempre)
          if (!quiets_only && td.enpassant != no_sq) {   // 2A: e.p. atterra su casa VUOTA
            U64 enpassant_attacks =
                pawn_attacks[td.side][source_square] & (1ULL << td.enpassant);
            if (enpassant_attacks) {
              int target_enpassant = get_ls1b_index(enpassant_attacks);
              add_move(move_list, encode_move(source_square, target_enpassant,
                                              piece, 0, 1, 0, 1, 0));
            }
          }
          pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
        }
      }
      if (piece == K) {
        // Arrocco: bloccato se captures_only   true
        if (!captures_only) {
          if (td.castle & wk) {
            if (!get_bit(td.occupancies[both], f1) &&
                !get_bit(td.occupancies[both], g1)) {
              if (!td_is_square_attacked(td, e1, black) &&
                  !td_is_square_attacked(td, f1, black))
                add_move(move_list, encode_move(e1, g1, piece, 0, 0, 0, 0, 1));
            }
          }
          if (td.castle & wq) {
            if (!get_bit(td.occupancies[both], d1) &&
                !get_bit(td.occupancies[both], c1) &&
                !get_bit(td.occupancies[both], b1)) {
              if (!td_is_square_attacked(td, e1, black) &&
                  !td_is_square_attacked(td, d1, black))
                add_move(move_list, encode_move(e1, c1, piece, 0, 0, 0, 0, 1));
            }
          }
        }
      }
    } else {
      if (piece == p) {
        while (bitboard) {
          source_square = get_ls1b_index(bitboard);
          target_square = source_square + 8;

          if (!captures_only) {
            if (!(target_square > h1) &&
                !get_bit(td.occupancies[both], target_square)) {
              if (source_square >= a2 && source_square <= h2) {
                if ((evasion_mask >> target_square) & 1) {
                  add_move(move_list, encode_move(source_square, target_square,
                                                  piece, q, 0, 0, 0, 0));
                  add_move(move_list, encode_move(source_square, target_square,
                                                  piece, r, 0, 0, 0, 0));
                  add_move(move_list, encode_move(source_square, target_square,
                                                  piece, b, 0, 0, 0, 0));
                  add_move(move_list, encode_move(source_square, target_square,
                                                  piece, n, 0, 0, 0, 0));
                }
              } else {
                if ((evasion_mask >> target_square) & 1)
                  add_move(move_list, encode_move(source_square, target_square,
                                                  piece, 0, 0, 0, 0, 0));
                if ((source_square >= a7 && source_square <= h7) &&
                    !get_bit(td.occupancies[both], target_square + 8) &&
                    ((evasion_mask >> (target_square + 8)) & 1))
                  add_move(move_list,
                           encode_move(source_square, target_square + 8, piece,
                                       0, 0, 1, 0, 0));
              }
            }
          }

          attacks = quiets_only ? 0ULL   // 2A: le catture di pedone non passano da allowed_squares
                  : (pawn_attacks[td.side][source_square] & enemies & evasion_mask);
          while (attacks) {
            target_square = get_ls1b_index(attacks);
            if (source_square >= a2 && source_square <= h2) {
              add_move(move_list, encode_move(source_square, target_square,
                                              piece, q, 1, 0, 0, 0));
              add_move(move_list, encode_move(source_square, target_square,
                                              piece, r, 1, 0, 0, 0));
              add_move(move_list, encode_move(source_square, target_square,
                                              piece, b, 1, 0, 0, 0));
              add_move(move_list, encode_move(source_square, target_square,
                                              piece, n, 1, 0, 0, 0));
            } else {
              add_move(move_list, encode_move(source_square, target_square,
                                              piece, 0, 1, 0, 0, 0));
            }
            pop_lsb_bb(attacks);   // 2D: target_square E' l'LSB di attacks
          }
          if (!quiets_only && td.enpassant != no_sq) {   // 2A: e.p. atterra su casa VUOTA
            U64 enpassant_attacks =
                pawn_attacks[td.side][source_square] & (1ULL << td.enpassant);
            if (enpassant_attacks) {
              int target_enpassant = get_ls1b_index(enpassant_attacks);
              add_move(move_list, encode_move(source_square, target_enpassant,
                                              piece, 0, 1, 0, 1, 0));
            }
          }
          pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
        }
      }
      if (piece == k) {
        if (!captures_only) {
          if (td.castle & bk) {
            if (!get_bit(td.occupancies[both], f8) &&
                !get_bit(td.occupancies[both], g8)) {
              if (!td_is_square_attacked(td, e8, white) &&
                  !td_is_square_attacked(td, f8, white))
                add_move(move_list, encode_move(e8, g8, piece, 0, 0, 0, 0, 1));
            }
          }
          if (td.castle & bq) {
            if (!get_bit(td.occupancies[both], d8) &&
                !get_bit(td.occupancies[both], c8) &&
                !get_bit(td.occupancies[both], b8)) {
              if (!td_is_square_attacked(td, e8, white) &&
                  !td_is_square_attacked(td, d8, white))
                add_move(move_list, encode_move(e8, c8, piece, 0, 0, 0, 0, 1));
            }
          }
        }
      }
    }

    // --- PEZZI (Qui applichiamo la maschera allowed_squares) ---
    if ((td.side == white) ? piece == N : piece == n) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = knight_attacks[source_square] & piece_allowed;
        while (attacks) {
          target_square = get_ls1b_index(attacks);
          if (!get_bit(enemies, target_square))
            add_move(move_list, encode_move(source_square, target_square, piece,
                                            0, 0, 0, 0, 0));
          else
            add_move(move_list, encode_move(source_square, target_square, piece,
                                            0, 1, 0, 0, 0));
          pop_lsb_bb(attacks);   // 2D: target_square E' l'LSB di attacks
        }
        pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
      }
    }

    if ((td.side == white) ? piece == B : piece == b) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = get_bishop_attacks(source_square, td.occupancies[both]) &
                  piece_allowed;
        while (attacks) {
          target_square = get_ls1b_index(attacks);
          if (!get_bit(enemies, target_square))
            add_move(move_list, encode_move(source_square, target_square, piece,
                                            0, 0, 0, 0, 0));
          else
            add_move(move_list, encode_move(source_square, target_square, piece,
                                            0, 1, 0, 0, 0));
          pop_lsb_bb(attacks);   // 2D: target_square E' l'LSB di attacks
        }
        pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
      }
    }

    if ((td.side == white) ? piece == R : piece == r) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = get_rook_attacks(source_square, td.occupancies[both]) &
                  piece_allowed;
        while (attacks) {
          target_square = get_ls1b_index(attacks);
          if (!get_bit(enemies, target_square))
            add_move(move_list, encode_move(source_square, target_square, piece,
                                            0, 0, 0, 0, 0));
          else
            add_move(move_list, encode_move(source_square, target_square, piece,
                                            0, 1, 0, 0, 0));
          pop_lsb_bb(attacks);   // 2D: target_square E' l'LSB di attacks
        }
        pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
      }
    }

    if ((td.side == white) ? piece == Q : piece == q) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = get_queen_attacks(source_square, td.occupancies[both]) &
                  piece_allowed;
        while (attacks) {
          target_square = get_ls1b_index(attacks);
          if (!get_bit(enemies, target_square))
            add_move(move_list, encode_move(source_square, target_square, piece,
                                            0, 0, 0, 0, 0));
          else
            add_move(move_list, encode_move(source_square, target_square, piece,
                                            0, 1, 0, 0, 0));
          pop_lsb_bb(attacks);   // 2D: target_square E' l'LSB di attacks
        }
        pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
      }
    }

    if ((td.side == white) ? piece == K : piece == k) {
      while (bitboard) {
        source_square = get_ls1b_index(bitboard);
        attacks = king_attacks[source_square] & allowed_squares;
        while (attacks) {
          target_square = get_ls1b_index(attacks);
          if (!get_bit(enemies, target_square))
            add_move(move_list, encode_move(source_square, target_square, piece,
                                            0, 0, 0, 0, 0));
          else
            add_move(move_list, encode_move(source_square, target_square, piece,
                                            0, 1, 0, 0, 0));
          pop_lsb_bb(attacks);   // 2D: target_square E' l'LSB di attacks
        }
        pop_lsb_bb(bitboard);   // 2D: source_square E' l'LSB di bitboard
      }
    }
  }
}

// ============================================================================
// EVALUATION
// ============================================================================

// Build the Stockfish piece list (codes + squares) from a thread's board.
// Returns the number of pieces written into pieces[]/squares[].
static inline int nn_build_piece_list(ThreadData &td, int pieces[33],
                                      int squares[33]) {
  int count = 0;
  for (int bb_piece = P; bb_piece <= k; bb_piece++) {
    U64 bb = td.bitboards[bb_piece];
    while (bb) {
      int sq = get_ls1b_index(bb);
      pieces[count] = nn_piece_code[bb_piece];
      squares[count] =
          nnue_squares[sq]; // engine square -> SF square (a1=0..h8=63)
      count++;
      pop_bit(bb, sq);
    }
  }
  return count;
}

// DIAGNOSTIC ("eval" UCI command): static NNUE eval of the CURRENT global board
// (the one parse_fen / "position" populate). Builds the piece list straight
// from the global bitboards and calls the stateless oracle nn_eval (whose value
// is, by the NNUE_VERIFY invariant, identical to the incremental nn_pos_eval =
// Eval::evaluate, i.e. SF's "Final evaluation"). Returns centipawns relative to
// the side to move. Used to cross-check the SFNNv9/3072 port against the
// official Stockfish binary on identical FENs (a correct port agrees up to SF's
// UCI pawn-normalisation constant).
int debug_eval_position() {
  int pieces[33], squares[33];
  int count = 0;
  for (int bb_piece = P; bb_piece <= k; bb_piece++) {
    U64 bb = bitboards[bb_piece]; // GLOBAL board set by parse_fen
    while (bb) {
      int sq = get_ls1b_index(bb);
      pieces[count] = nn_piece_code[bb_piece];
      squares[count] = nnue_squares[sq];
      count++;
      pop_bit(bb, sq);
    }
  }
  return nn_eval(side == white, pieces, squares, count, fifty);
}

// Come debug_eval_position ma restituisce l'uscita GREZZA della rete (psqt + positional,
// pre-nn_scale). E' la grandezza che calcola anche il trainer, quindi e' l'unica
// confrontabile in un cross-check. Vedi il commento su nn_eval in nnue_bridge.cpp.
int debug_eval_position_raw() {
  int pieces[33], squares[33];
  int count = 0;
  for (int bb_piece = P; bb_piece <= k; bb_piece++) {
    U64 bb = bitboards[bb_piece];
    while (bb) {
      int sq = get_ls1b_index(bb);
      pieces[count] = nn_piece_code[bb_piece];
      squares[count] = nnue_squares[sq];
      count++;
      pop_bit(bb, sq);
    }
  }
  int raw = 0;
  nn_eval(side == white, pieces, squares, count, fifty, &raw);
  return raw;
}

// Re-sync the incremental NNUE mirror to the thread's current board (full
// refresh). Called at the root of each iterative-deepening iteration.
static inline void nn_root_sync(ThreadData &td) {
  int pieces[33], squares[33];
  int count = nn_build_piece_list(td, pieces, squares);
  nn_pos_set(td.nnpos, td.side == white, pieces, squares, count, td.fifty);
}

static inline int td_evaluate(ThreadData &td) {
  PROF_GUARD(prof_eval);
  // DIAGNOSTIC: skip the NNUE forward, return a cheap material eval. Used by
  // the "EvalOff" UCI option to isolate evaluation cost in an NPS profile.
  if (g_eval_off) {
    static const int mat[12] = {100, 320, 330, 500, 900, 0,
                                100, 320, 330, 500, 900, 0};
    int s = 0;
    for (int p = 0; p < 6; p++)
      s += mat[p] * count_bits(td.bitboards[p]);
    for (int p = 6; p < 12; p++)
      s -= mat[p] * count_bits(td.bitboards[p]);
    return (td.side == white) ? s : -s;
  }
#ifdef NNUE_VERIFY
  // Verification oracle: the incrementally maintained eval must EXACTLY equal
  // a full from-scratch refresh. Build with /DNNUE_VERIFY to enable.
  int pieces[33], squares[33];
  int count = nn_build_piece_list(td, pieces, squares);
  int full = nn_eval(td.side == white, pieces, squares, count, td.fifty);
  int inc = nn_pos_eval(td.nnpos, td.bitboards, td.occupancies);
  if (full != inc) {
    std::cerr << "NNUE MISMATCH ply=" << td.ply << " full=" << full
              << " inc=" << inc << " hash=0x" << std::hex << td.hash_key
              << std::dec << std::endl;
    std::exit(2);
  }
  return inc;
#else
  // Static-eval cache: hash_key mixed with fifty (see threads.h EvalCacheEntry
  // for why fifty is in the key). A hit skips the NNUE forward pass entirely;
  // safe because nn_pos_do/undo keep the lazy accumulator's dirty chain intact
  // whether or not we call nn_pos_eval here.
  if (g_eval_cache) {
    if (g_evalcache_undamp) {
      // N1 (2026-06-12): chiave SENZA fifty, valore UNDAMPED (rule50-indip.)
      // ri-smorzato col fifty corrente alla lettura — stesso design del
      // TT-eval16. Prima il fifty era nella chiave -> la stessa posizione a
      // fifty diverso era un MISS sistematico (finali shuffle). Costo: ±2cp
      // di arrotondamento sul roundtrip; il valore fresco resta esatto.
      ThreadData::EvalCacheEntry &ce =
          td.eval_cache[td.hash_key & ThreadData::EVAL_CACHE_MASK];
      // EvalCacheOptTag: l'optimism entra DENTRO il valore (nnue_bridge.cpp:153) e cambia
      // a ogni iterazione ⇒ un hit con opt diverso serve una eval calcolata con un altro
      // contempt. Con il tag quell'hit diventa un miss e si ricalcola.
      const int opt_now = g_optimism[td.side];
      if (ce.key == td.hash_key) {
        // EvalCacheOptSplit: l'optimism entra LINEARMENTE nell'eval, quindi invece di
        // buttare l'entry si CORREGGE il delta. `ce.coeff` e' d(eval)/d(optimism) in
        // millesimi (gia' comprensivo di EvalScale), invariante della posizione.
        int e = ce.eval;
        if (g_evalcache_opt_split)
          e += (opt_now - ce.opt) * ce.coeff / 1000;
        return tt_eval_redamp(e, td.fifty);
      }
      const int v = nn_pos_eval(td.nnpos, td.bitboards, td.occupancies);
      ce.key = td.hash_key;
      ce.eval = tt_eval_undamp(v, td.fifty);
      ce.opt = opt_now;
      ce.coeff = nn_last_opt_coeff();
      return v;
    }
    const U64 ck = td.hash_key ^ (0x9E3779B97F4A7C15ULL * (U64)(td.fifty + 1));
    ThreadData::EvalCacheEntry &ce =
        td.eval_cache[ck & ThreadData::EVAL_CACHE_MASK];
    const int opt_now = g_optimism[td.side];
    if (ce.key == ck)
      return g_evalcache_opt_split ? ce.eval + (opt_now - ce.opt) * ce.coeff / 1000 : ce.eval;
    const int v = nn_pos_eval(td.nnpos, td.bitboards, td.occupancies);
    ce.key = ck;
    ce.eval = v;
    ce.opt = opt_now;
    ce.coeff = nn_last_opt_coeff();
    return v;
  }
  // Incremental: the accumulator is updated only for the pieces that changed
  // (HalfKAv2_hm king-bucket refresh handled by Stockfish's transform()).
  return nn_pos_eval(td.nnpos, td.bitboards, td.occupancies);
#endif
}

// ============================================================================
// MOVE SCORING
// ============================================================================

// Maximum magnitude of a history score. Kept below the killer-move scores
// (8000) so quiet history can never outrank killers or captures, and bounded
// via "gravity" so the table cannot overflow over a long search.
#define HISTORY_MAX 7000

// Update a history entry with gravity: the value saturates towards
// +/-HISTORY_MAX, giving recent evidence more weight (Stockfish-style).
static inline void td_update_history(int &h, int bonus) {
  if (bonus > HISTORY_MAX)
    bonus = HISTORY_MAX;
  else if (bonus < -HISTORY_MAX)
    bonus = -HISTORY_MAX;
  h += bonus - h * (bonus < 0 ? -bonus : bonus) / HISTORY_MAX;
}

// Same gravity update for the int16_t continuation-history entries.
static inline void td_update_history(int16_t &h, int bonus) {
  if (bonus > HISTORY_MAX)
    bonus = HISTORY_MAX;
  else if (bonus < -HISTORY_MAX)
    bonus = -HISTORY_MAX;
  int v = (int)h + bonus - (int)h * (bonus < 0 ? -bonus : bonus) / HISTORY_MAX;
  h = (int16_t)v;
}

// Stat bonus per gli update history. Default = depth*depth (originale). Con
// HistBonusSF on usa la forma SF lineare-clampata min(mult*d - sub, max).
static inline int td_stat_bonus(int depth) {
  if (g_hist_bonus_sf) {
    int b = g_hist_bonus_mult * depth - g_hist_bonus_sub;
    if (b > g_hist_bonus_max)
      b = g_hist_bonus_max;
    if (b < 0)
      b = 0;
    return b;
  }
  return depth * depth;
}

// Move-ordering score bands (well separated so the additive quiet histories,
// range about +/-2*HISTORY_MAX, never overlap the "special" move scores):
//   TT move > good captures > killers > counter-move > quiet history > bad
//   captures
#define SCORE_TT_MOVE 2000000
#define SCORE_GOOD_CAPTURE 700000
#define SCORE_KILLER0 600000
#define SCORE_KILLER1 590000
#define SCORE_COUNTER 580000
#define SCORE_BAD_CAPTURE (-700000)

// Pezzo catturato sulla casa target (en passant -> P, coerente con lo scoring).
static inline int td_captured_piece(ThreadData &td, int target) {
  int start = (td.side == white) ? p : P;
  int end = (td.side == white) ? k : K;
  for (int pc = start; pc <= end; pc++)
    if (get_bit(td.bitboards[pc], target))
      return pc;
  return P;
}

// Threat-ordering helper: fill td.threat_by_{pawn,minor,rook} = squares
// attacked by enemy pieces grouped by the cheapest attacker's value. Called
// once per node (the caller checks td.threat_key == td.hash_key first). Only
// reached when ThreatOrdering is on, so it costs nothing when the toggle is
// off.
static inline void td_compute_threats(ThreadData &td) {
  const int them = td.side ^ 1; // opponent color (white=0 / black=1)
  const int base = them * 6;    // white pieces P..K at 0, black p..k at 6
  const U64 occ = td.occupancies[both];
  U64 byP = 0, byM, byR, bb;

  bb = td.bitboards[base + P]; // enemy pawns
  while (bb) {
    int s = get_ls1b_index(bb);
    pop_bit(bb, s);
    byP |= pawn_attacks[them][s];
  }
  byM = byP;
  bb = td.bitboards[base + N]; // enemy knights
  while (bb) {
    int s = get_ls1b_index(bb);
    pop_bit(bb, s);
    byM |= knight_attacks[s];
  }
  bb = td.bitboards[base + B]; // enemy bishops
  while (bb) {
    int s = get_ls1b_index(bb);
    pop_bit(bb, s);
    byM |= get_bishop_attacks(s, occ);
  }
  byR = byM;
  bb = td.bitboards[base + R]; // enemy rooks
  while (bb) {
    int s = get_ls1b_index(bb);
    pop_bit(bb, s);
    byR |= get_rook_attacks(s, occ);
  }

  // threat_all = byR + regina + re (TUTTE le minacce nemiche) per la
  // ThreatHist. Calcolato sempre qui (cheap); letto solo quando g_threat_hist
  // e' on.
  U64 byAll = byR;
  bb = td.bitboards[base + Q]; // enemy queens
  while (bb) {
    int s = get_ls1b_index(bb);
    pop_bit(bb, s);
    byAll |= get_queen_attacks(s, occ);
  }
  byAll |= king_attacks[get_ls1b_index(td.bitboards[base + K])]; // enemy king

  td.threat_by_pawn = byP;
  td.threat_by_minor = byM;
  td.threat_by_rook = byR;
  td.threat_all = byAll;
  td.threat_key = td.hash_key;
}

// tier-minaccia di una casa (VALUE-AWARE, cheapest-attacker): 0 = safe, 1 =
// attaccata da pawn/knight/ bishop (attaccante economico -> pericoloso per
// pezzi di valore), 2 = solo da rook/queen/king. Usa i tier che gia' calcoliamo
// (threat_by_minor = pawn+minori, threat_all = tutte).
static inline int td_sq_tier(ThreadData &td, int sq) {
  if (get_bit(td.threat_by_minor, sq))
    return 1;
  if (get_bit(td.threat_all, sq))
    return 2;
  return 0;
}
// ThreatHist REPLACE-TIERED: bucket-minaccia di una mossa = from_tier*3 +
// to_tier (0..8). Ritorna 0 quando ThreatHist e' OFF -> history_moves[0][..] =
// vecchia tabella = byte-identico. Da chiamare col board del nodo CORRENTE (i
// tier = minacce di QUESTO nodo).
static inline int td_hbucket(ThreadData &td, int move) {
  if (!g_threat_hist)
    return 0;
  if (td.threat_key != td.hash_key)
    td_compute_threats(td);
  return td_sq_tier(td, get_move_source(move)) * 3 +
         td_sq_tier(td, get_move_target(move));
}
// CapHistThreat: bucket della capture history = 1 se la casa di ARRIVO e'
// attaccata dal nemico (il pezzo che cattura puo' essere ripreso), 0 altrimenti.
// Binario e non tiered come td_hbucket: qui conta "mi riprendono si/no", non da
// quale pezzo — il "da quale pezzo" e' gia' nello split SEE good/bad capture.
// Ritorna 0 quando il toggle e' OFF -> tabella piatta = byte-identico.
// Da chiamare col board del nodo dove la cattura viene fatta.
static inline int td_cbucket(ThreadData &td, int target) {
  if (!g_caphist_threat)
    return 0;
  if (td.threat_key != td.hash_key)
    td_compute_threats(td);
  return get_bit(td.threat_all, target) ? 1 : 0;
}

// F-018.11 (EasyCapGate): vero se un NOSTRO pezzo e' attaccato da un pezzo
// nemico di valore INFERIORE (presa "facile": N/B/R/Q da pedone, R/Q da minore,
// Q da torre). Riusa i tier di td_compute_threats (cache su hash_key -> gratis
// se gia' calcolati).
static inline bool td_has_easy_capture(ThreadData &td) {
  if (td.threat_key != td.hash_key)
    td_compute_threats(td);
  const int base = td.side * 6;
  return (td.threat_by_pawn &
          (td.bitboards[base + N] | td.bitboards[base + B] |
           td.bitboards[base + R] | td.bitboards[base + Q])) ||
         (td.threat_by_minor &
          (td.bitboards[base + R] | td.bitboards[base + Q])) ||
         (td.threat_by_rook & td.bitboards[base + Q]);
}

// Check-ordering helper: check_sq[pt] = squares from which a piece of type pt
// would give a DIRECT check to the enemy king (= squares a pt-attacker reaches
// the king square from). Computed once per node (caller checks td.check_key ==
// td.hash_key). Only when CheckOrdering on.
static inline void td_compute_checks(ThreadData &td) {
  const int them = td.side ^ 1;
  const U64 occ = td.occupancies[both];
  int ksq = get_ls1b_index(td.bitboards[them * 6 + K]); // enemy king square
  td.check_sq[0] =
      pawn_attacks[them][ksq]; // pawn (our pawn checks from these squares)
  td.check_sq[1] = knight_attacks[ksq];             // knight
  td.check_sq[2] = get_bishop_attacks(ksq, occ);    // bishop
  td.check_sq[3] = get_rook_attacks(ksq, occ);      // rook
  td.check_sq[4] = td.check_sq[2] | td.check_sq[3]; // queen
  td.check_sq[5] = 0;                               // king cannot give check
  td.check_key = td.hash_key;
}

// Offense-ordering helper (QuietOffense, port Reckless movepick score_quiet).
// Fills td.offense_sq[pt] = SAFE squares (not attacked by any enemy piece) from
// which a piece of type pt would attack a VULNERABLE enemy piece, and
// td.wall_pawns = our king-shield pawns. Computed once per node (caller checks
// td.offense_key == td.hash_key). Only reached when the toggle is on. Uses OUR
// attack tables per-piece (coordinate-safe), NOT Reckless' setwise fills.
// ⚠️ Le locali sono off_* e MAI p/n/b/r/q: quelli sono gli ENUM dei pezzi neri
// (6..11) e una locale che li ombreggia trasforma td.bitboards[p] in una read
// OOB selvaggia (stessa trappola documentata in syzygy.cpp::to_fathom; costato
// un crash 0xC0000005 posizione-dipendente il 2026-07-24).
static inline void td_compute_offense(ThreadData &td) {
  const int side = td.side;
  const int them = side ^ 1;
  const int bt = them * 6;    // enemy pieces P..K at bt..bt+5
  const int bu = side * 6;    // our pieces

  // wall pawns: se il re e' sulla propria traversa di casa, i pedoni adiacenti
  // sono lo scudo -> penalizza spostarli. home row: bianco rank1 (sq 56..63),
  // nero rank8 (sq 0..7). ⬆ SPOSTATI IN CIMA 2026-07-25: sono l'unico termine
  // vivo di questa funzione e non dipendono dalle minacce.
  int myk = get_ls1b_index(td.bitboards[bu + K]);
  const U64 home = (side == white) ? 0xFF00000000000000ULL : 0x00000000000000FFULL;
  td.wall_pawns = get_bit(home, myk)
                      ? (king_attacks[myk] & (td.bitboards[P] | td.bitboards[p]))
                      : 0;
  td.offense_key = td.hash_key;

  // NPS 2026-07-25: le offense_sq costano ~10 loop di attacchi scorrevoli PER
  // NODO, e il loro UNICO consumatore e' `h += g_offense_bonus` (:4762). Con
  // g_offense_bonus = 0 — che e' il default, perche' il termine offense-squares
  // e' risultato NEGATIVO in isolamento (−9.70 ± 10.41, LOS 3.38% @1290g) —
  // era lavoro interamente buttato a ogni nodo, incluso il td_compute_threats
  // che serviva solo a loro.
  // Azzerare le maschere e' BYTE-IDENTICO: get_bit(0, target) = 0, quindi
  // `h += 0`, che e' esattamente cio' che accadeva prima con bonus = 0.
  // Rimettendo OffenseBonus != 0 si ritorna al percorso completo.
  // Gate 2026-07-25: con questo early-return disattivato il bench resta
  // 205566, identico -> byte-identita' provata empiricamente, non solo per
  // costruzione.
  if (!g_offense_bonus) {
    for (int i = 0; i < 6; i++)
      td.offense_sq[i] = 0;
    return;
  }

  if (td.threat_key != td.hash_key)
    td_compute_threats(td); // need threat_all / threat_by_pawn
  const U64 occ = td.occupancies[both];
  const U64 threats = td.threat_all;
  const U64 safe = ~threats;

  const U64 eB = td.bitboards[bt + B];
  const U64 eR = td.bitboards[bt + R];
  const U64 eQ = td.bitboards[bt + Q];
  U64 bb;

  // non_pawn_threats = enemy knight|bishop|rook|queen|king attacks (per la lever)
  U64 nonpawn = 0;
  bb = td.bitboards[bt + N];
  while (bb) { int s = get_ls1b_index(bb); pop_bit(bb, s); nonpawn |= knight_attacks[s]; }
  bb = eB;
  while (bb) { int s = get_ls1b_index(bb); pop_bit(bb, s); nonpawn |= get_bishop_attacks(s, occ); }
  bb = eR;
  while (bb) { int s = get_ls1b_index(bb); pop_bit(bb, s); nonpawn |= get_rook_attacks(s, occ); }
  bb = eQ;
  while (bb) { int s = get_ls1b_index(bb); pop_bit(bb, s); nonpawn |= get_queen_attacks(s, occ); }
  nonpawn |= king_attacks[get_ls1b_index(td.bitboards[bt + K])];

  // --- pawn offense: case sicure da cui un nostro pedone attacca un pezzo nemico
  //     + attacchi di pedone avanzato (lever ranks, difesi solo da pedone) ---
  U64 off_p = 0;
  bb = td.occupancies[them];
  while (bb) { int e = get_ls1b_index(bb); pop_bit(bb, e); off_p |= pawn_attacks[them][e]; }
  off_p &= safe;
  // lever ranks in coord a8=0: bianco = rank 5-6 (sq 16..31), nero = rank 3-4 (sq 32..47)
  const U64 lever = (side == white) ? 0x00000000FFFF0000ULL : 0x0000FFFF00000000ULL;
  off_p |= td.threat_by_pawn & lever & ~nonpawn;

  // --- knight offense: attacca alfiere-sicuro / torre / donna nemici ---
  U64 knight_vuln = (eB & safe) | eR | eQ;
  U64 off_n = 0;
  bb = knight_vuln;
  while (bb) { int e = get_ls1b_index(bb); pop_bit(bb, e); off_n |= knight_attacks[e]; }
  off_n &= safe;

  // --- bishop offense: attacca una torre nemica ---
  U64 off_b = 0;
  bb = eR;
  while (bb) { int e = get_ls1b_index(bb); pop_bit(bb, e); off_b |= get_bishop_attacks(e, occ); }
  off_b &= safe;

  // --- rook offense: colonna del re nemico (case sicure) ---
  int ek = get_ls1b_index(td.bitboards[bt + K]);
  U64 off_r = (0x0101010101010101ULL << (ek & 7)) & safe;

  // --- queen offense: attacca ortogonalmente un alfiere / diagonalmente una torre ---
  U64 off_q = 0;
  bb = eB & safe;
  while (bb) { int e = get_ls1b_index(bb); pop_bit(bb, e); off_q |= get_rook_attacks(e, occ); }
  bb = eR & safe;
  while (bb) { int e = get_ls1b_index(bb); pop_bit(bb, e); off_q |= get_bishop_attacks(e, occ); }
  off_q &= safe;

  td.offense_sq[0] = off_p;
  td.offense_sq[1] = off_n;
  td.offense_sq[2] = off_b;
  td.offense_sq[3] = off_r;
  td.offense_sq[4] = off_q;
  td.offense_sq[5] = 0;
  // wall_pawns e offense_key sono gia' stati impostati in cima alla funzione.
}

// ---- Q-11 NodeCache (port Caissa NodeCache.cpp) -----------------------------
// Rimpiazzo age-based: una entry di una search PRECEDENTE (gen != corrente)
// puo' essere rubata; una della STESSA search no (Caissa: "allocation failed"
// -> nullptr). Chiave piena verificata sempre -> un hit stantio e' impossibile,
// al massimo un miss.
static inline ThreadData::NCEntry *nc_get(ThreadData &td, U64 key) {
  ThreadData::NCEntry *e = &td.node_cache[key & (ThreadData::NC_SIZE - 1)];
  if (e->key == key) { // stessa posizione: RIUSA le statistiche (cross-move!)
    e->gen = td.nc_gen;
    return e;
  }
  if (e->gen != td.nc_gen) { // slot di una search vecchia: rialloca
    e->key = key;
    e->gen = td.nc_gen;
    e->nodes_sum = 0;
    memset(e->mv, 0, sizeof(e->mv));
    memset(e->mv_nodes, 0, sizeof(e->mv_nodes));
    return e;
  }
  return nullptr; // collisione dentro la stessa search: non rubare
}

static inline ThreadData::NCEntry *nc_try(ThreadData &td, U64 key) {
  ThreadData::NCEntry *e = &td.node_cache[key & (ThreadData::NC_SIZE - 1)];
  return (e->key == key) ? e : nullptr;
}

// Dimezza tutti i contatori (anti-overflow su accumulo cross-move,
// Caissa::ScaleDown).
static inline void nc_scale_down(ThreadData::NCEntry *e) {
  e->nodes_sum = 0;
  for (int i = 0; i < ThreadData::NC_MOVES; i++) {
    e->mv_nodes[i] >>= 1;
    e->nodes_sum += e->mv_nodes[i];
  }
}

static inline void nc_add(ThreadData::NCEntry *e, int move, U64 nodes) {
  int free_i = -1, min_i = -1;
  U64 min_n = ~0ULL;
  for (int i = 0; i < ThreadData::NC_MOVES; i++) {
    if (e->mv[i] == move) { // gia' tracciata: accumula
      e->mv_nodes[i] += nodes;
      e->nodes_sum += nodes;
      if (e->mv_nodes[i] > (1ULL << 40))
        nc_scale_down(e);
      return;
    }
    if (e->mv[i] == 0) {
      if (free_i < 0)
        free_i = i;
    } else if (e->mv_nodes[i] < min_n) {
      min_n = e->mv_nodes[i];
      min_i = i;
    }
  }
  // nuova mossa: slot libero, altrimenti rimpiazza la MENO visitata (solo se
  // costa meno di noi)
  int idx =
      (free_i >= 0) ? free_i : ((min_i >= 0 && min_n < nodes) ? min_i : -1);
  if (idx < 0)
    return;
  e->nodes_sum -= e->mv_nodes[idx];
  e->mv[idx] = move;
  e->mv_nodes[idx] = nodes;
  e->nodes_sum += nodes;
}

static inline int td_score_move(ThreadData &td, int move, int tt_move) {
  PROF_GUARD(prof_score);
  if (move == tt_move)
    return SCORE_TT_MOVE;

  int piece = get_move_piece(move);
  int target = get_move_target(move);

  // Le promozioni sono bombe tattiche: vanno ordinate insieme alle catture
  // forti, non confuse con le mosse silenziose lente.
  if (get_move_promoted(move)) {
    return SCORE_GOOD_CAPTURE + 50000;
  }

  if (get_move_capture(move)) {
    int victim = P;
    int start = (td.side == white) ? p : P;
    int end = (td.side == white) ? k : K;
#ifndef TRIUMV_NO_MAILBOX
    // Il test di range replica il ciclo sostituito: cercava solo fra i pezzi
    // AVVERSARI, e se non trovava nulla lasciava il default P. Succede all'e.p.,
    // dove la casa d'arrivo e' VUOTA e la vittima sta altrove.
    const int mb_pc = td.piece_on[target];
    if (mb_pc >= start && mb_pc <= end)
      victim = mb_pc;
#else
    for (int pc = start; pc <= end; pc++) {
      if (get_bit(td.bitboards[pc], target)) {
        victim = pc;
        break;
      }
    }
#endif

    int caphist = g_capture_hist
                      ? td.capture_history[piece][target][victim]
                                          [td_cbucket(td, target)] /
                            g_caphist_div // qui era diviso 16
                      : 0;

    // GoodCapTTQuiet (Reckless #1107, default OFF): TT-move quiet = la mossa
    // migliore nota NON e' una cattura -> le catture per essere "good" devono
    // vincere materiale NETTO (SEE >= +1; pari e perdenti scendono tra le bad).
    // Bypassa anche il lazy-shortcut equal-capture qui sotto (QxQ = SEE 0 =
    // bad).
    if (g_goodcap_ttquiet && tt_move && !get_move_capture(tt_move) &&
        !get_move_promoted(tt_move)) {
      if (td_see_at_least(td, move, 1))
        return SCORE_GOOD_CAPTURE + mvv_lva[piece][victim] + caphist;
      else
        return SCORE_BAD_CAPTURE + mvv_lva[piece][victim] + caphist;
    }

    // Lazy SEE: capturing a piece of equal-or-greater value is good by
    // definition (no SEE needed). Only run SEE for "attacker > victim"
    // captures, which might be losing. Losing captures (SEE < 0) are
    // ordered AFTER all quiets (still searched, just last).
    // F-018.4 GoodCapHistDiv (0=off): soglia good/bad DIPENDENTE dallo score
    // (Obsidian: -score/32) — una cattura con caphist ottima resta "good" anche
    // con SEE leggermente negativo, una con storia pessima deve vincere
    // materiale netto.
    int gc_thresh = g_goodcap_hist_div ? -(mvv_lva[piece][victim] + caphist) /
                                             g_goodcap_hist_div
                                       : 0;
    if (see_piece_values[piece] <= see_piece_values[victim] ||
        td_see_at_least(td, move, gc_thresh))
      return SCORE_GOOD_CAPTURE + mvv_lva[piece][victim] + caphist;
    else
      return SCORE_BAD_CAPTURE + mvv_lva[piece][victim] + caphist;
  }

  // Quiet moves
  if (td.killer_moves[0][td.ply] == move)
    return SCORE_KILLER0;
  if (td.killer_moves[1][td.ply] == move)
    return SCORE_KILLER1;

  int prev = td.move_stack[td.ply];
  if (g_countermove && prev &&
      td.counter_moves[get_move_piece(prev)][get_move_target(prev)] == move)
    return SCORE_COUNTER;

  // Butterfly history (MainHistWeight, default 1; SF=2) + continuation history
  // (ContHistWeight, default 1). I conthist sono sommati e poi pesati, cosi' il
  // co-tune puo' bilanciare conthist vs main/pawn senza toccare il loro uso in
  // LMR.
  int h = g_mainhist_weight *
          td.history_moves[td_hbucket(td, move)][piece][target] / 100;
  // ThreatHistWeight (cablato 2026-07-04): scala extra del termine
  // (threat-indexed) di butterfly history nell'ordering. 100 = identita' ->
  // nessun effetto (baseline). Solo con ThreatHist ON.
  if (g_threat_hist && g_threat_hist_weight != 100)
    h = h * g_threat_hist_weight / 100;
  int ch = 0;
  if (prev)
    ch += td.continuation_history[get_move_piece(prev)][get_move_target(prev)]
                                 [piece][target];
  // Longer-range continuation history: the same quiet scored in the context of
  // the move 2 and 4 plies back. Off by default (ContHistMulti).
  if (g_conthist_multi) {
    if (td.ply >= 1) {
      int p2 = td.move_stack[td.ply - 1];
      if (p2)
        ch += td.cont_hist_2[get_move_piece(p2)][get_move_target(p2)][piece]
                            [target];
    }
    if (td.ply >= 3) {
      int p4 = td.move_stack[td.ply - 3];
      if (p4)
        ch += td.cont_hist_4[get_move_piece(p4)][get_move_target(p4)][piece]
                            [target];
    }
  }
  // ContHist36 (#4): continuation history a 3-ply (ply-2) e 6-ply (ply-5),
  // scalata da ContHist36Weight, sommata in ch. Default OFF = byte-identico.
  if (g_conthist36) {
    int ch36 = 0;
    if (td.ply >= 2) {
      int p3 = td.move_stack[td.ply - 2];
      if (p3)
        ch36 += td.cont_hist_3[get_move_piece(p3)][get_move_target(p3)][piece]
                              [target];
    }
    if (td.ply >= 5) {
      int p6 = td.move_stack[td.ply - 5];
      if (p6)
        ch36 += td.cont_hist_6[get_move_piece(p6)][get_move_target(p6)][piece]
                              [target];
    }
    ch += g_conthist36_weight * ch36 / 100;
  }
  h += g_conthist_weight * ch / 100;
  // PawnHistory (SF-style, weight 2x): quiet ordering by pawn structure. Indice
  // ricalcolato dal board corrente (= board del nodo durante lo scoring ->
  // corretto, niente staleness). Gated -> OFF = byte-identico.
  if (g_pawn_hist) {
    int pk = td_corr_index(td) & ThreadData::PAWN_HIST_MASK;
    h += g_pawn_hist_weight * (int)td.pawn_history[pk][piece][target] / 100;
  }
  // Threat-ordering (ThreatOrdering, default OFF -> byte-identico):
  // +scala*valore se il pezzo lascia una casa attaccata da uno INFERIORE,
  // -scala*valore se ci entra. Tier come SF (Q vs minacce-di-torre, R vs
  // minore, minore vs pedone); pedoni/re non hanno un "minore" -> nessun
  // termine. Minacce calcolate 1x/nodo (cache hash_key).
  if (g_threat_ordering) {
    if (td.threat_key != td.hash_key)
      td_compute_threats(td);
    U64 lesser = 0;
    bool has = true;
    switch (piece % 6) { // 0=P 1=N 2=B 3=R 4=Q 5=K
    case 4:
      lesser = td.threat_by_rook;
      break; // queen
    case 3:
      lesser = td.threat_by_minor;
      break; // rook
    case 1:
    case 2:
      lesser = td.threat_by_pawn;
      break; // knight / bishop
    default:
      has = false;
      break; // pawn / king
    }
    if (has) {
      int from = get_move_source(move);
      int term =
          (get_bit(lesser, from) ? 1 : 0) - (get_bit(lesser, target) ? 1 : 0);
      if (term)
        h += g_threat_scale * see_piece_values[piece] * term / 100;
    }
  }
  // (ThreatHist REPLACE: la threat-indicizzazione e' GIA' dentro
  // history_moves[bucket][..] sopra,
  //  via td_hbucket. Niente termine additivo separato.)
  // Check-ordering (CheckOrdering, default OFF -> byte-identico): bonus ai
  // quiet che danno SCACCO DIRETTO e non sono blunder (SEE>=-75), come SF.
  // check_sq calcolate 1x/nodo.
  if (g_check_ordering) {
    if (td.check_key != td.hash_key)
      td_compute_checks(td);
    if (get_bit(td.check_sq[piece % 6], target) &&
        td_see_at_least(td, move, -75))
      h += g_check_bonus;
  }
  // Offense-ordering (QuietOffense, default OFF -> byte-identico; port Reckless):
  // +bonus se la quiet porta il pezzo su una casa-offense (attacca sicuro un
  // pezzo nemico vulnerabile), -malus se muove un pedone-scudo del re. offense_sq
  // e wall_pawns calcolate 1x/nodo (cache offense_key).
  if (g_quiet_offense) {
    if (td.offense_key != td.hash_key)
      td_compute_offense(td);
    if (get_bit(td.offense_sq[piece % 6], target))
      h += g_offense_bonus;
    if (get_bit(td.wall_pawns, get_move_source(move)))
      h -= g_wallpawn_penalty;
  }
  // Low-ply history (#5, default OFF -> byte-identico): segnale per-ply
  // near-root, peso che decade con la profondita' (1+2*ply), come SF.
  if (g_lowply && td.ply < ThreadData::LOW_PLY_MAX) {
    h += g_lowply_weight * td.lowply_history[td.ply][piece][target] /
         (100 * (1 + 2 * td.ply));
  }
  // Q-11 NodeCache (Caissa MoveOrderer.cpp:431-438, default OFF): vicino alla
  // radice, premia la quiet che ha gia' assorbito molti nodi (= linea
  // profonda/critica). La chiave piena e' verificata da nc_try -> nessun
  // rischio di dati stantii, e la banda (max g_nc_bonus ~4096) resta dentro
  // quella delle history quiet (±14000), lontana da killer/counter (580000+).
  if (g_node_cache && td.ply < ThreadData::NC_MAX_PLY) {
    ThreadData::NCEntry *e = nc_try(td, td.hash_key);
    if (e && e->nodes_sum > (U64)g_nc_min_sum) {
      // nc_add riempie sempre il primo slot libero -> gli occupati sono un
      // prefisso denso: al primo mv==0 non c'e' piu' nulla da cercare.
      for (int i = 0; i < ThreadData::NC_MOVES && e->mv[i]; i++) {
        if (e->mv[i] == move) {
          h += (int)((U64)g_nc_bonus * e->mv_nodes[i] / e->nodes_sum);
          break;
        }
      }
    }
  }
  return h;
}

static inline void td_sort_moves(ThreadData &td, moves *move_list,
                                 int tt_move) {
  int scores[256];

  for (int i = 0; i < move_list->count; i++) {
    // Punteggio base (TT, catture, history, killer)
    scores[i] = td_score_move(td, move_list->moves[i], tt_move);
  }

  // --- Ordinamento standard (Selection Sort) ---
  for (int i = 0; i < move_list->count - 1; i++) {
    int best = i;
    for (int j = i + 1; j < move_list->count; j++) {
      if (scores[j] > scores[best])
        best = j;
    }
    if (best != i) {
      int tmp_move = move_list->moves[i];
      move_list->moves[i] = move_list->moves[best];
      move_list->moves[best] = tmp_move;

      int tmp_score = scores[i];
      scores[i] = scores[best];
      scores[best] = tmp_score;
    }
  }
}

// Calcola solo gli score ma NON ordina (la selezione e' pick-next).
static inline void td_score_all_moves(ThreadData &td, moves *move_list,
                                      int tt_move, int *scores) {
  for (int i = 0; i < move_list->count; i++)
    scores[i] = td_score_move(td, move_list->moves[i], tt_move);
}

// ============================================================================
// STAGED MOVE PICKER  (UCI option "MovePicker", default OFF)
// ============================================================================

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

// Stage order matches the legacy score bands so the search is (near)
// byte-identical.
enum {
  MPS_TT = 0,        // hash move (validated)
  MPS_GEN_TACTICAL,  // generate captures + capture-promotions, score them
  MPS_GOOD_TACTICAL, // promotions + SEE>=0 captures, by score
  MPS_KILLER0,
  MPS_KILLER1,
  MPS_COUNTER,
  MPS_GEN_QUIET,    // generate all moves, keep & score the quiets
  MPS_QUIET,        // quiets by history, low to high consumed
  MPS_BAD_TACTICAL, // SEE<0 captures, by score
  MPS_DONE
};

// Sentinel placed in a score slot once that move has been yielded. Far below
// any real move score (good caps ~+7e5, bad caps ~-7e5), so it never collides.
static const int MP_CONSUMED = -2000000000;

struct MovePicker {
  bool staged;
  int stage;
  // 2C: lo scacco al nodo, gia' calcolato dalla search. Resta valido per tutta la vita del
  // picker: fra i due stage di generazione le mosse vengono fatte e disfatte, ma make/unmake
  // ripristina esattamente la posizione, quindi il nodo e' sempre lo stesso.
  int in_check;
  int tt_move, killer0, killer1, counter;
  moves *caps;
  int *cap_scores;
  int cap_n; // capture/promotion buffer
  moves *quiets;
  int *q_scores;
  int q_n;   // quiet buffer (legacy: all moves)
  int l_idx; // legacy pick-next cursor
  // Phase-2 skip flags: set by the search loop when a pruning condition is met
  // BEFORE the first quiet/bad-cap is even generated. mp_next() reads these and
  // jumps past the entire stage — zero generation cost for pruned nodes.
  bool skip_quiets;   // skip MPS_GEN_QUIET + MPS_QUIET + killers/counter
  bool skip_bad_caps; // skip MPS_BAD_TACTICAL
};

// caps/cap_scores: scratch buffers for the tactical stages (staged mode only).
// quiets/q_scores: in legacy mode these are the fully generated+scored move
// list; in staged mode they are reused as the quiet-stage buffer.
static inline void mp_init(MovePicker &mp, ThreadData &td, bool staged,
                           int tt_move, moves *caps, int *cap_scores,
                           moves *quiets, int *q_scores, int in_check) {
  mp.staged = staged;
  mp.in_check = in_check;   // 2C: lo passa la search, che l'ha gia' calcolato
  mp.tt_move = tt_move;
  mp.caps = caps;
  mp.cap_scores = cap_scores;
  mp.cap_n = 0;
  mp.quiets = quiets;
  mp.q_scores = q_scores;
  mp.q_n = 0;
  mp.l_idx = 0;
  mp.stage = MPS_TT;
  mp.skip_quiets = false;
  mp.skip_bad_caps = false;
  if (staged) {
    mp.killer0 = td.killer_moves[0][td.ply];
    mp.killer1 = td.killer_moves[1][td.ply];
    int prev = td.move_stack[td.ply];
    // counter=0 spegne di fatto TUTTO il meccanismo: lo stage refutation
    // (:3175) e il dedup
    // (:3189) sono gia' guardati da `mp.counter &&`.
    mp.counter =
        (g_countermove && prev)
            ? td.counter_moves[get_move_piece(prev)][get_move_target(prev)]
            : 0;
  } else {
    mp.killer0 = mp.killer1 = mp.counter = 0;
  }
}

// Returns the next move to search, 0 when exhausted.
static int mp_next(ThreadData &td, MovePicker &mp) {
  // --- Legacy: pick-next over the fully scored move list (bit-identical to the
  //     original in-loop selection sort). ---
  if (!mp.staged) {
    moves *ml = mp.quiets;
    int *sc = mp.q_scores;
    int i = mp.l_idx;
    if (i >= ml->count)
      return 0;
    int best = i;
    for (int j = i + 1; j < ml->count; j++)
      if (sc[j] > sc[best])
        best = j;
    if (best != i) {
      int tm = ml->moves[i];
      ml->moves[i] = ml->moves[best];
      ml->moves[best] = tm;
      int ts = sc[i];
      sc[i] = sc[best];
      sc[best] = ts;
    }
    mp.l_idx = i + 1;
    return ml->moves[i];
  }

  // --- Staged ---
  switch (mp.stage) {
  case MPS_TT:
    mp.stage = MPS_GEN_TACTICAL;
    if (mp.tt_move && td_is_pseudo_legal(td, mp.tt_move))
      return mp.tt_move;
    [[fallthrough]];

  case MPS_GEN_TACTICAL:
    td_generate_moves(td, mp.caps, /*captures_only=*/true, /*quiets_only=*/false,
                      mp.in_check);   // 2C
    mp.cap_n = mp.caps->count;
    for (int i = 0; i < mp.cap_n; i++)
      mp.cap_scores[i] = td_score_move(td, mp.caps->moves[i], mp.tt_move);
    mp.stage = MPS_GOOD_TACTICAL;
    [[fallthrough]];

  case MPS_GOOD_TACTICAL:
    // Good tacticals score > 0 (promotions 750k, SEE>=0 captures ~700k+); bad
    // captures score ~-700k and are deferred to MPS_BAD_TACTICAL.
    for (;;) {
      int best = -1, bs = 0;
      for (int i = 0; i < mp.cap_n; i++)
        if (mp.cap_scores[i] > bs) {
          bs = mp.cap_scores[i];
          best = i;
        }
      if (best < 0)
        break;
      int m = mp.caps->moves[best];
      mp.cap_scores[best] = MP_CONSUMED;
      if (m != mp.tt_move)
        return m; // tt already yielded
    }
    mp.stage = MPS_KILLER0;
    [[fallthrough]];

  case MPS_KILLER0:
    mp.stage = MPS_KILLER1;
    if (!mp.skip_quiets && mp.killer0 && mp.killer0 != mp.tt_move &&
        td_is_pseudo_legal(td, mp.killer0))
      return mp.killer0;
    [[fallthrough]];

  case MPS_KILLER1:
    mp.stage = MPS_COUNTER;
    if (!mp.skip_quiets && mp.killer1 && mp.killer1 != mp.tt_move &&
        mp.killer1 != mp.killer0 && td_is_pseudo_legal(td, mp.killer1))
      return mp.killer1;
    [[fallthrough]];

  case MPS_COUNTER:
    mp.stage = MPS_GEN_QUIET;
    if (!mp.skip_quiets && mp.counter && mp.counter != mp.tt_move &&
        mp.counter != mp.killer0 && mp.counter != mp.killer1 &&
        td_is_pseudo_legal(td, mp.counter))
      return mp.counter;
    [[fallthrough]];

  case MPS_GEN_QUIET:
    if (mp.skip_quiets) {
      mp.stage = MPS_BAD_TACTICAL;
      goto bad_tactical;
    }
    // 2A: quiets_only. Prima era `false` = tutte le pseudo-legali, quindi ogni cattura veniva
    // rigenerata qui dopo essere gia' stata prodotta da MPS_GEN_TACTICAL, e subito buttata
    // marcandola CONSUMED. Node-identical: le catture non venivano rese comunque.
    td_generate_moves(td, mp.quiets, /*captures_only=*/false, /*quiets_only=*/true,
                      mp.in_check);   // 2C
    mp.q_n = mp.quiets->count;
    for (int i = 0; i < mp.q_n; i++) {
      int m = mp.quiets->moves[i];
      // Le catture le rendono gli stage tattici; TT/killer/counter sono gia' state rese ->
      // marcate CONSUMED per non ripeterle. Il test get_move_capture resta anche con
      // quiets_only: la lista ora non ne contiene, ma il costo e' nullo e toglierlo
      // renderebbe la funzione dipendente dalla modalita' del chiamante.
      if (get_move_capture(m) || m == mp.tt_move || m == mp.killer0 ||
          m == mp.killer1 || m == mp.counter)
        mp.q_scores[i] = MP_CONSUMED;
      else
        mp.q_scores[i] = td_score_move(td, m, mp.tt_move);
    }
    mp.stage = MPS_QUIET;
    [[fallthrough]];

  case MPS_QUIET: {
    // Phase-2: if LMP/futility flipped skip_quiets WHILE we were already in the
    // quiet stage, stop yielding the remaining quiets at once (the search would
    // prune every one of them anyway) and go straight to the bad captures.
    if (mp.skip_quiets) {
      mp.stage = MPS_BAD_TACTICAL;
      goto bad_tactical;
    }
    int best = -1, bs = MP_CONSUMED;
    for (int i = 0; i < mp.q_n; i++)
      if (mp.q_scores[i] != MP_CONSUMED && mp.q_scores[i] > bs) {
        bs = mp.q_scores[i];
        best = i;
      }
    if (best >= 0) {
      mp.q_scores[best] = MP_CONSUMED;
      return mp.quiets->moves[best];
    }
    mp.stage = MPS_BAD_TACTICAL;
    // fallthrough into bad-tactical
  }

  bad_tactical:
  case MPS_BAD_TACTICAL:
    if (mp.skip_bad_caps) {
      mp.stage = MPS_DONE;
      return 0;
    }
    for (;;) {
      int best = -1, bs = MP_CONSUMED;
      for (int i = 0; i < mp.cap_n; i++)
        if (mp.cap_scores[i] != MP_CONSUMED && mp.cap_scores[i] > bs) {
          bs = mp.cap_scores[i];
          best = i;
        }
      if (best < 0)
        break;
      int m = mp.caps->moves[best];
      mp.cap_scores[best] = MP_CONSUMED;
      if (m != mp.tt_move)
        return m;
    }
    mp.stage = MPS_DONE;
    return 0;

  default:
    return 0;
  }
}

// ============================================================================
// REPETITION DETECTION
// ============================================================================

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
  int end = td.fifty;
  if (end > td.repetition_index)
    end = td.repetition_index;
  if (end < 3)
    return false;
  const U64 orig = td.hash_key;
  for (int i = 3; i <= end; i += 2) {
    const U64 prev = td.repetition_table[td.repetition_index - (i - 1)];
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

static inline int td_is_repetition(ThreadData &td) {
  // P2.2 FastRepScan (default ON): finestra min(fifty, plies_from_null) invece
  // dell'INTERA storia. fifty = bound ESATTO (una mossa irreversibile azzera
  // fifty e cambia il board per sempre -> nessuna ricorrenza oltre);
  // plies_from_null esclude i segmenti oltre una null move — risolve il bug del
  // vecchio tentativo finestra-fifty (2026-06-06): le null pushano entry senza
  // toccare fifty e sfasavano la finestra. Niente step-2: la chiave include il
  // side_key, le entry dell'altro lato non possono mai combaciare.
  if (g_fast_rep_scan) {
    int limit = td.fifty < td.plies_from_null ? td.fifty : td.plies_from_null;
    int lo = td.repetition_index - limit;
    if (lo < 0)
      lo = 0;
    for (int i = td.repetition_index - 1; i >= lo; i--)
      if (td.repetition_table[i] == td.hash_key)
        return 1;
    return 0;
  }
  // Legacy: loop completo (entry null incluse), versione di riferimento.
  for (int i = 0; i < td.repetition_index; i++) {
    if (td.repetition_table[i] == td.hash_key)
      return 1;
  }
  return 0;
}

// ============================================================================
// QUIESCENCE SEARCH
// ============================================================================

// QsStalemateCheck support: movegen COMPLETO (catture + quiete), si ferma
// alla prima mossa legale trovata. Costo O(1) nel caso comune (un finale K+P
// ha quasi sempre una mossa di pedone/re); la scansione dell'intera lista
// avviene solo nel vero stallo, che e' esattamente il caso raro che questo
// probe deve scovare. Chiamata SOLO dal trigger gated in td_quiescence.
static bool td_has_any_legal_move(ThreadData &td) {
  moves ml[1];
  td_generate_moves(td, ml, false);
  for (int i = 0; i < ml->count; i++) {
    UndoInfo undo;
    td.ply++;
    td.repetition_table[++td.repetition_index] = td.hash_key;
    bool legal = td_make_move(td, ml->moves[i], undo) != 0;
    if (legal)
      td_unmake_move(td, ml->moves[i], undo);
    td.ply--;
    td.repetition_index--;
    if (legal)
      return true;
  }
  return false;
}

// qs_depth: 0 alla prima ply di qsearch (chiamate dal negamax), decresce nelle
// ricorsioni. Serve solo a P1.3 QSChecks (quiet check tenuti SOLO a qs_depth
// 0).
static int td_quiescence(ThreadData &td, int alpha, int beta,
                         int qs_depth = 0) {
  // Illegal-position / king-capture guard (see td_negamax for the full
  // rationale): never search a position where the side not to move is in check,
  // because making the king capture desyncs the NNUE accumulator and crashes.
  // (Rimozione provata 2026-06-06: node-identica ma NPS ~0 -> tenuta per
  // safety.)
  {
    int opp_king_sq =
        get_ls1b_index((td.side == white) ? td.bitboards[k] : td.bitboards[K]);
    if (td_is_square_attacked(td, opp_king_sq, td.side))
      return mate_value - td.ply;
  }

  // F-015 (RIPRISTINATO A TOGGLE 2026-07-04 sera): draw-detection anche in
  // qsearch (SF is_draw). L'hardening precedente e' stato ANNULLATO — il test
  // nps_ab_test.py a 80 posizioni dava -1.2% nodi (trascurabile), ma il bench a
  // 8 posizioni dava +17.6% (781516 vs 664743): scarto troppo grande per essere
  // solo "posizioni diverse", da capire prima di ri-proporre l'hardening.
  // Default OFF finche' non e' chiarito il meccanismo.
  if (g_qs_draw_check && td.ply && (td_is_repetition(td) || td.fifty >= 100))
    return g_draw_dither ? (1 - (int)(td.nodes & 2)) : 0;

  if ((td.nodes & 4095) == 0) {
    if (stop_threads.load(std::memory_order_relaxed))
      return 0;
    // A5: `td_root_move_ready()` = pavimento anti-forfeit, vedi la definizione.
    // Short-circuit: non viene mai valutata finche' il tempo non e' scaduto.
    if (timeset && get_time_ms() > stoptime && td_root_move_ready()) {
      stop_threads.store(true, std::memory_order_relaxed);
      return 0;
    }
  }

  td.nodes++;
  if (g_node_limit && td.nodes >= g_node_limit) {
    stop_threads.store(true, std::memory_order_relaxed);
    return 0;
  }
  if (td.ply > td.seldepth)
    td.seldepth = td.ply; // seldepth (info UCI)

  if (td.ply >= max_ply)
    return td_evaluate(td);

  bool pv_node = (beta - alpha > 1);

  // TT probe (qsearch entries stored at depth 0)
  int tt_move = 0, tt_score = 0, tt_depth = 0, tt_flag = hash_flag_alpha;
  int tt_eval = tt_eval_none; // P1.1
  bool tt_pv_q = false;
  bool tt_hit = probe_tt(td.hash_key, tt_move, tt_score, tt_depth, tt_flag,
                         tt_eval, tt_pv_q);
  if (tt_hit && !pv_node) {
    if (tt_score < -mate_score)
      tt_score += td.ply;
    if (tt_score > mate_score)
      tt_score -= td.ply;
    if (g_tt_decisive_clamp)
      tt_score = tt_clamp_decisive(tt_score, td.fifty); // Q-32
    if (tt_flag == hash_flag_exact)
      return tt_score;
    if (tt_flag == hash_flag_alpha && tt_score <= alpha)
      return tt_score;
    if (tt_flag == hash_flag_beta && tt_score >= beta)
      return tt_score;
  }

  // Sotto scacco: niente stand-pat, si cercano TUTTE le evasioni.
  int king_sq =
      get_ls1b_index((td.side == white) ? td.bitboards[K] : td.bitboards[k]);
  bool in_check = td_is_square_attacked(td, king_sq, td.side ^ 1);

  int best_score;
  int q_raw_eval =
      tt_eval_none; // P1.1: eval pura da salvare nello store qsearch
  if (in_check) {
    best_score = -infinity;
  } else {
    // P1.1 TTStaticEval: come nel negamax. flag_none = entry eval-only
    // (EvalTTWrite): UNADJUSTED -> nn_finalize(fifty corrente), esatto su ogni
    // trasposizione.
    q_raw_eval =
        (g_tt_static_eval && tt_eval != tt_eval_none)
            ? (tt_flag == hash_flag_none ? nn_finalize(tt_eval, td.fifty)
                                         : tt_eval_redamp(tt_eval, td.fifty))
            : td_evaluate(td);
    int stand_pat = q_raw_eval;
    // FIX P0.4 (2026-06-09): la correction history ora corregge ANCHE lo
    // stand-pat di quiescence (dove avviene la maggioranza delle eval);
    // prima si fermava ai nodi interni. Gated QsearchCorr per l'ablazione.
    if (g_qsearch_corr && g_corr_hist)
      stand_pat += td_corr_value(td, td_corr_index(td));
    // P1.1: tt_score con bound coerente = stima di qualita'-search migliore
    // dello stand-pat statico (stesso adjustment del negamax).
    if (g_tt_eval_improve && tt_hit && tt_score > -mate_score &&
        tt_score < mate_score &&
        (tt_flag == hash_flag_exact ||
         (tt_flag == hash_flag_beta && tt_score > stand_pat) ||
         (tt_flag == hash_flag_alpha && tt_score < stand_pat)))
      stand_pat = tt_score;
    best_score = stand_pat;
    // F-018.7 FailHighSmooth: midpoint verso beta sul fail-high statico
    // (Obsidian/Berserk).
    if (stand_pat >= beta)
      return (g_fh_smooth && stand_pat < mate_score && stand_pat > -mate_score)
                 ? td_fail_firm(stand_pat, beta, g_fh_t_qs_standpat)
                 : stand_pat;

    // F-002: era `const int DELTA = 1000` (unita'-eval, mai ricalibrato
    // post-EvalScale). Q-20d (Ethereal): delta-pruning col BEST-CASE reale —
    // vittima piu' grossa sul board + potenziale promozione (pedone in 7a
    // relativa). Se nemmeno il colpo migliore possibile + cuscino arriva ad
    // alpha, il nodo e' morto.
    if (g_qs_delta_bestcase) {
      int maxvic;
      if (td.side == white) {
        maxvic = td.bitboards[q]                       ? 900
                 : td.bitboards[r]                     ? 500
                 : (td.bitboards[b] | td.bitboards[n]) ? 330
                 : td.bitboards[p]                     ? 100
                                                       : 0;
        if (td.bitboards[P] & 0xFF00ULL)
          maxvic += 800; // promo: +Q-P
      } else {
        maxvic = td.bitboards[Q]                       ? 900
                 : td.bitboards[R]                     ? 500
                 : (td.bitboards[B] | td.bitboards[N]) ? 330
                 : td.bitboards[P]                     ? 100
                                                       : 0;
        if (td.bitboards[p] & 0x00FF000000000000ULL)
          maxvic += 800; // promo: +Q-P
      }
      // maxvic e' in cp classici (900/500/330/100), ma stand_pat/alpha sono
      // in unita'-motore COMPRESSE (EvalScale ~56-62%). Riportalo sulla stessa
      // scala, altrimenti sovrastima il guadagno-cattura e pota di meno del
      // dovuto (bug direzione-sicura, era near-noop col path default-OFF).
      maxvic = maxvic * nn_get_eval_scale() / 100;
      if (stand_pat + maxvic + g_qs_bc_margin < alpha)
        return stand_pat;
    } else if (stand_pat + g_qs_delta < alpha)
      return stand_pat;

    if (stand_pat > alpha)
      alpha = stand_pat;
  }

  moves move_list[1];
  // Se non siamo sotto scacco (!in_check), passa true per generare SOLO le
  // catture. P1.3 QSChecks (default OFF): alla prima ply di qsearch genera
  // TUTTO, poi il loop tiene solo catture + quiet che danno SCACCO DIRETTO.
  const bool qs_checks_here = g_qs_checks && qs_depth == 0 && !in_check;
  // QSTTQuiets (Stormphrax search.cpp:1557): genera anche le quiete quando la TT
  // dice che la best e' quieta E il bound e' affidabile. `hash_flag_alpha` e' il
  // nostro upper bound (fail-low), quindi escluderlo = il loro
  // `flag != UpperBound`. Nessun cap di qs_depth: la condizione TT e' gia' molto
  // piu' stretta di "prima ply". OFF -> byte-identico.
  const bool qs_tt_quiets = g_qs_tt_quiets && !pv_node && !in_check && tt_hit &&
                            tt_move && tt_flag != hash_flag_alpha &&
                            !get_move_capture(tt_move) &&
                            !get_move_promoted(tt_move);
  td_generate_moves(td, move_list,
                    !in_check && !qs_checks_here && !qs_tt_quiets);

  // Calcola punteggi senza ordinare (selezione pick-next).
  int move_scores[256];
  td_score_all_moves(td, move_list, tt_move, move_scores);

  int best_move = 0;
  int legal_moves = 0;
  int qcaptures =
      0; // catture ESAMINATE a questo nodo (per BadNoisy move-count pruning)
  int q_searched_caps[8]; // Q-17 QSCaptHist: catture cercate legalmente (cap 8,
                          // come Caissa)
  int nq_caps = 0;

  for (int count = 0; count < move_list->count; count++) {

    // F-018.14 QSMoveCap (0=off): le mosse sono ordinate best-first -> oltre le
    // prime N (non in scacco) il resto e' quasi sempre inutile (Obsidian: 3).
    if (g_qs_move_cap && !in_check && legal_moves >= g_qs_move_cap)
      break;

    // --- INIZIO PICK-NEXT: Cerca la mossa migliore tra quelle rimaste ---
    int best_idx = count;
    for (int i = count + 1; i < move_list->count; i++) {
      if (move_scores[i] > move_scores[best_idx]) {
        best_idx = i;
      }
    }

    // Scambiamo mossa e score per portarli in cima
    if (best_idx != count) {
      int tmp_move = move_list->moves[count];
      move_list->moves[count] = move_list->moves[best_idx];
      move_list->moves[best_idx] = tmp_move;

      int tmp_score = move_scores[count];
      move_scores[count] = move_scores[best_idx];
      move_scores[best_idx] = tmp_score;
    }

    // Ora move   garantita essere la migliore disponibile
    int move = move_list->moves[count];
    // --- FINE PICK-NEXT ---

    if (!in_check) {
      if (!get_move_capture(move)) {
        // QSTTQuiets: quando la condizione TT vale, le quiete passano SENZA il
        // filtro "deve dare scacco" — e' la forma di Stormphrax, dove il
        // generatore produce tutte le mosse e a limitare sono l'ordinamento
        // best-first, il SEE e il cap di move-count (da noi QSMoveCap=3).
        // Il filtro vero e' gia' nella condizione TT, che e' rarissima.
        // modo 1 = tutte le quiete ammesse (Stormphrax). modo 2 = ammesse solo
        // quelle che danno scacco, cioe' il filtro qui sotto resta attivo ma il
        // gate d'ingresso e' la condizione TT invece di "prima ply".
        if (g_qs_tt_quiets != 1) {
          // P1.3 QSChecks: tieni il quiet SOLO se da' scacco diretto e non
          // e' un check-blunder (SEE >= -75, stesso filtro del CheckOrdering).
          if (!qs_checks_here && !qs_tt_quiets)
            continue;
          if (td.check_key != td.hash_key)
            td_compute_checks(td);
          if (!get_bit(td.check_sq[get_move_piece(move) % 6],
                       get_move_target(move)))
            continue;
          if (!td_see_at_least(td, move, -75))
            continue;
        }
      } else {
        // BadNoisy (SF qsearch move-count pruning, gated OFF): le catture sono
        // ordinate best-first -> dopo le prime N le rimanenti sono quasi-sempre
        // perdenti. Niente skip su ricattura (to != prevSq) ne' promo/ep.
        if (g_bad_noisy && !get_move_promoted(move) &&
            !get_move_enpassant(move)) {
          int prev_bn = td.move_stack[td.ply];
          int prev_bn_sq = prev_bn ? get_move_target(prev_bn) : -1;
          if (get_move_target(move) != prev_bn_sq &&
              qcaptures >= g_bad_noisy_count)
            continue;
        }
        qcaptures++;
        // (4) QFutility: salta la cattura se best_score + valore(vittima) +
        // margine
        //     non arriva ad alpha (no ep/promo). OFF = nessun effetto.
        //     FIX SCALA (2026-06-24): best_score/alpha sono eval-NET (~3x la
        //     scala-100; pedone marginale misurato ~190-500cp via `eval`),
        //     mentre see_piece_values e' scala-100 -> il termine vittima va
        //     scalato, altrimenti la qsearch sovra-pota (era -126 Elo). La
        //     sovrastima (fattore ~3 > del capfut 2.35) e' voluta: pota MENO le
        //     catture grosse = errore nella direzione SICURA (l'eval-net satura).
        //     QFutMargin tara il residuo.
        //     FIX DRIFT (2026-07-24): il fattore era hardcoded "* 3" (tarato a
        //     EvalScale=56, mai ri-seguito) -> ora g_qfut_vic_scale *
        //     nn_get_eval_scale() / 10000, come il gemello capfut: byte-identico
        //     oggi (500*60/10000=3.0) ma auto-ricalibrante ai bake della rete.
        if (g_qfutility && !get_move_promoted(move) &&
            !get_move_enpassant(move)) {
          int vic = td_captured_piece(td, get_move_target(move));
          // SF: NON potare la RICATTURA sulla casa appena mossa dall'avversario
          // (to_sq != prevSq) — potarla = perdere la valutazione del recupero
          // (era il −17).
          int prev_mv = td.move_stack[td.ply];
          int prev_sq = prev_mv ? get_move_target(prev_mv) : -1;
          if (vic >= 0 && vic < 12 && get_move_target(move) != prev_sq) {
            int fv = best_score +
                     (int)((long long)see_piece_values[vic] * g_qfut_vic_scale *
                           nn_get_eval_scale() / 10000) +
                     g_qfut_margin;
            if (fv <= alpha) {
              if (fv > best_score)
                best_score = fv; // SF fail-soft: bestValue = max(bestValue, fv)
              continue;
            }
          }
        }
        if (!get_move_promoted(move) && !td_see_at_least(td, move, 0))
          continue;
      }
    }

    // QsStalemateCheck: la cattura toglie l'ultima Torre/Donna dalla
    // scacchiera (materiale pre-mossa mascherato sulla casa target)? Calcolato
    // PRIMA di muovere (serve il pezzo-vittima). Vedi definizione toggle sopra
    // e td_has_any_legal_move.
    bool cap_last_major = false;
    if (g_qs_stalemate_check && get_move_capture(move)) {
      int vic = td_captured_piece(td, get_move_target(move));
      if (vic == R || vic == Q || vic == r || vic == q) {
        U64 remaining_majors = (td.bitboards[R] | td.bitboards[Q] |
                                 td.bitboards[r] | td.bitboards[q]) &
                                ~(1ULL << get_move_target(move));
        cap_last_major = (remaining_majors == 0);
      }
    }

    UndoInfo undo;
    td.ply++;
    td.repetition_table[++td.repetition_index] = td.hash_key;

    if (!td_make_move(td, move, undo)) {
      td.ply--;
      td.repetition_index--;
      continue;
    }

    legal_moves++;
    td.move_stack[td.ply] = move; // mantieni il move-stack in qsearch (serve
                                  // alla recapture-exemption della QFutility)
    if (nq_caps < 8 && get_move_capture(move))
      q_searched_caps[nq_caps++] = move; // Q-17

    int score;
    if (cap_last_major) {
      int child_king_sq =
          get_ls1b_index((td.side == white) ? td.bitboards[K] : td.bitboards[k]);
      bool child_in_check = td_is_square_attacked(td, child_king_sq, td.side ^ 1);
      score = (!child_in_check && !td_has_any_legal_move(td))
                  ? 0 // stallo: pareggio morto, ne' catture ne' scacchi ne'
                      // mosse tranquille — qsearch normale non lo avrebbe MAI
                      // visto (genera solo catture)
                  : -td_quiescence(td, -beta, -alpha, qs_depth - 1);
    } else {
      score = -td_quiescence(td, -beta, -alpha, qs_depth - 1);
    }

    td_unmake_move(td, move, undo);
    td.ply--;
    td.repetition_index--;

    if (stop_threads.load(std::memory_order_relaxed))
      return 0;

    if (score > best_score) {
      best_score = score;
      best_move = move;
      if (score > alpha) {
        alpha = score;
        if (score >= beta)
          break;
      }
    }
  }

  if (in_check && legal_moves == 0)
    return -mate_value + td.ply;

  // Q-17 QSCaptHist (Caissa Search.cpp:1169-1245, default OFF): la maggioranza
  // delle catture passa in qsearch ma la capture-history imparava solo dal main
  // search. Al beta-cutoff: bonus alla cattura vincente, malus alle provate
  // prima (board gia' ripristinata dal loop -> td_captured_piece legge i pezzi
  // giusti).
  if (g_qs_capthist && best_score >= beta && best_move &&
      get_move_capture(best_move)) {
    int qb = td_stat_bonus(1) * g_qs_capthist_scale / 100;
    int bvic = td_captured_piece(td, get_move_target(best_move));
    if (bvic >= 0 && bvic < 12)
      td_update_history(
          td.capture_history[get_move_piece(best_move)]
                            [get_move_target(best_move)][bvic]
                            [td_cbucket(td, get_move_target(best_move))],
          qb);
    int qmal = qb * g_malus_pct / 100;
    for (int c = 0; c < nq_caps; c++) {
      int cm = q_searched_caps[c];
      if (cm == best_move)
        continue;
      int cvic = td_captured_piece(td, get_move_target(cm));
      if (cvic >= 0 && cvic < 12)
        td_update_history(
            td.capture_history[get_move_piece(cm)][get_move_target(cm)][cvic]
                              [td_cbucket(td, get_move_target(cm))],
            -qmal);
    }
  }

  // F-018.7 FailHighSmooth: midpoint verso beta anche sul best finale (come
  // Obsidian, che lo salva cosi' pure in TT: bound meno gonfiato = meno
  // re-search dei padri).
  if (g_fh_smooth && best_score >= beta && best_score < mate_score &&
      best_score > -mate_score)
    best_score = td_fail_firm(best_score, beta, g_fh_t_qs_final);

  int store_flag = (best_score >= beta) ? hash_flag_beta : hash_flag_alpha;
  store_tt(td.hash_key, best_move, best_score, 0, store_flag, td.ply, false,
           tt_eval_undamp(q_raw_eval, td.fifty));

  return best_score;
}

// ============================================================================
// NEGAMAX (Lazy SMP: each thread runs this independently, sharing only the TT)
// ============================================================================

// ---- Correction history ----------------------------------------------------
// Learns the systematic gap between the NNUE static eval and the value the
// search actually returns, bucketed by PAWN STRUCTURE + side to move (the net
// most often misjudges pawn structures). The learned offset is added to the raw
// static eval, so pruning/ordering see a value closer to what the search will
// confirm.
static constexpr int CORR_BITS = 14;
static constexpr int CORR_SIZE = 1 << CORR_BITS; // 16384 buckets
static constexpr int CORR_MASK = CORR_SIZE - 1;
static constexpr int CORR_GRAIN = 256; // stored value is (cp * GRAIN)
// Max correction (cp) and stored-value clamp are now runtime tunables:
// g_corr_cap and (g_corr_cap * CORR_GRAIN). See g_corr_cap / g_corr_lr_div
// above.

// Bucket index from the Zobrist key of a given set of piece types.
static inline int td_corr_index_pieces(ThreadData &td, const int *pcs, int n) {
  U64 k = 0;
  for (int j = 0; j < n; j++) {
    int pc = pcs[j];
    U64 bb = td.bitboards[pc];
    while (bb) {
      int sq = get_ls1b_index(bb);
      k ^= piece_keys[pc][sq];
      pop_bit(bb, sq);
    }
  }
  return (int)(k & CORR_MASK);
}

// Bucket index from the pawn-only Zobrist key (white pawns P, black pawns p).
// P2.1: con PawnKeyIncr (default ON) usa la chiave INCREMENTALE mantenuta in
// make/unmake invece di riscansionare le bitboard a ogni chiamata (corr 1x/nodo
// + pawn_history ~30 quiet/nodo). Stessa chiave per costruzione =
// node-identical.
static inline int td_corr_index(ThreadData &td) {
  if (g_pawn_key_incr)
    return (int)(td.pawn_key & CORR_MASK);
  const int pcs[2] = {P, p};
  return td_corr_index_pieces(td, pcs, 2);
}
// Minor-piece (N/B) and major-piece (R/Q) keyed indices (CorrHistMulti only).
static inline int td_corr_index_minor(ThreadData &td) {
  const int pcs[4] = {N, B, n, b};
  return td_corr_index_pieces(td, pcs, 4);
}
static inline int td_corr_index_major(ThreadData &td) {
  const int pcs[4] = {R, Q, r, q};
  return td_corr_index_pieces(td, pcs, 4);
}
// Material-key index (SF #5556): rolling hash dei 12 popcount = firma del
// MATERIALE (conteggi), indipendente dalle posizioni. Mascherato come le altre
// corr. Solo con g_corr_material on.
static inline int td_corr_index_material(ThreadData &td) {
  U64 mk = 0;
  for (int pc = P; pc <= k; pc++)
    mk = mk * 0x9E3779B97F4A7C15ULL + (U64)count_bits(td.bitboards[pc]);
  return (int)(mk & CORR_MASK);
}
// Non-pawn key di UN SOLO colore (re incluso, come SF nonPawnKey) — CorrNonPawn
// only. Default: chiave INCREMENTALE (np_key, make/unmake). NPKeyIncr=false =
// rescan (oracle: nodi identici per costruzione a parita' di CorrNonPawn).
static inline int td_corr_index_np_white(ThreadData &td) {
  if (g_np_key_incr)
    return (int)(td.np_key[white] & CORR_MASK);
  const int pcs[5] = {N, B, R, Q, K};
  return td_corr_index_pieces(td, pcs, 5);
}
static inline int td_corr_index_np_black(ThreadData &td) {
  if (g_np_key_incr)
    return (int)(td.np_key[black] & CORR_MASK);
  const int pcs[5] = {n, b, r, q, k};
  return td_corr_index_pieces(td, pcs, 5);
}

// Continuation-correction bucket for the current node: keyed by the move 2-ply
// back and the move 1-ply back (the path INTO this node). Returns nullptr if
// either is missing (root / null move / ply<2). Only touched when g_corr_cont
// is on. NB: move_stack[td.ply] is the move that led to THIS node (1-ply back),
// so 2-ply back is move_stack[td.ply-1] (same convention as the conthist-multi
// tables).
static inline int16_t *td_cont_corr_bucket(ThreadData &td) {
  if (td.ply < 1)
    return nullptr;
  int m1 = td.move_stack[td.ply];     // 1-ply back (move into this node)
  int m2 = td.move_stack[td.ply - 1]; // 2-ply back
  if (!m1 || !m2)
    return nullptr;
  return &td.cont_corr_hist[get_move_piece(m2)][get_move_target(m2)]
                           [get_move_piece(m1)][get_move_target(m1)];
}

// Correction (cp) to add to the raw static eval for this position. Sums the
// pawn table with the minor/major material tables when CorrHistMulti is on,
// then clamps the TOTAL correction to g_corr_cap.
static inline int td_corr_value(ThreadData &td, int idx) {
  int sum = td.corr_hist[td.side][idx];
  if (g_corr_multi) {
    sum += td.corr_hist_minor[td.side][td_corr_index_minor(td)];
    sum += td.corr_hist_major[td.side][td_corr_index_major(td)];
  }
  if (g_corr_nonpawn) {
    sum += g_corr_np_weight *
           (td.corr_hist_np[0][td.side][td_corr_index_np_white(td)] +
            td.corr_hist_np[1][td.side][td_corr_index_np_black(td)]) /
           100;
  }
  if (g_corr_cont) {
    if (int16_t *cc = td_cont_corr_bucket(td))
      sum += g_corr_cont_weight * (int)(*cc) / 100;
  }
  if (g_corr_material) {
    sum += g_corr_material_weight *
           td.corr_hist_material[td.side][td_corr_index_material(td)] / 100;
  }
  int corr = sum / CORR_GRAIN;
  if (corr > g_corr_cap)
    corr = g_corr_cap;
  if (corr < -g_corr_cap)
    corr = -g_corr_cap;
  return corr;
}

// P1a CorrUncert: disaccordo (somma delle distanze a coppie / 2) tra le tre
// tavole di correzione per QUESTA posizione, in cp, cappato a g_cu_cap.
// Chiamata solo con g_corr_uncert on (paga 2 indici extra minor/major).
static inline int td_corr_uncert(ThreadData &td, int idx) {
  int p = td.corr_hist[td.side][idx];
  int mi = td.corr_hist_minor[td.side][td_corr_index_minor(td)];
  int ma = td.corr_hist_major[td.side][td_corr_index_major(td)];
  int d1 = p - mi;
  if (d1 < 0)
    d1 = -d1;
  int d2 = p - ma;
  if (d2 < 0)
    d2 = -d2;
  int d3 = mi - ma;
  if (d3 < 0)
    d3 = -d3;
  int u = (d1 + d2 + d3) / (2 * CORR_GRAIN);
  return u > g_cu_cap ? g_cu_cap : u;
}

// One bucket's gravity update toward `target`, clamped to +/-lim.
// FIX DEFINITIVO (P0.X): Sostituzione della buggy EMA (che causava
// sottocorrezione cronica) con il gravity update puro in stile Stockfish.
// 'target' e' gia' l'errore residuo moltiplicato per CORR_GRAIN.
static inline void td_corr_bucket_update(int &cv, int target, int w, int lim) {
  int bonus = target * w / g_corr_lr_div;
  // CorrAsym: nudge asimmetrico verso l'alto (se presente)
  if (bonus > 0)
    bonus = bonus * g_corr_asym / 128;

  // Stockfish-style decay: proporzionale al valore corrente e alla magnitudine
  // del bonus
  int abs_bonus = bonus > 0 ? bonus : -bonus;
  int decay = (cv * abs_bonus) / lim;

  cv += bonus - decay;

  if (cv > lim)
    cv = lim;
  else if (cv < -lim)
    cv = -lim;
}

// Update the bucket(s) toward (best_score - static_eval), gated by the bound
// type so we never learn from a value that contradicts the bound direction.
static inline void td_corr_update(ThreadData &td, int idx, int static_eval,
                                  int best_score, int bound, int depth,
                                  bool in_check, int best_move,
                                  int excluded_move) {
  if (!g_corr_hist || in_check || excluded_move || best_move == 0)
    return;
  if (get_move_capture(best_move) || get_move_promoted(best_move))
    return; // quiet best only
  // 🔴 FIX 2026-08-02 — la soglia era `mate_score` (30000), ma uno score da tablebase vale
  // `TB_VALUE_WIN - ply` = **29873..29936** (syzygy.cpp:29): la banda TB passava INTERA.
  // Un verdetto Syzygy non e' una valutazione, e' una certezza: entrava in `diff` come se
  // fosse un errore di eval da ~29.900, saturava il bucket a ±CorrCap e ci restava,
  // avvelenando OGNI posizione con la stessa pawn key (14 bit ⇒ anche per collisione).
  // In piu' `decay = cv * abs_bonus / lim` a riga 6249 andava in **overflow di int**
  // (5,5e9 > INT32_MAX ⇒ UB, non semplice wrap).
  // Invisibile in tutti i nostri test, che girano SENZA tablebase — ed e' attivo nelle
  // partite di rating, che usano 6 pezzi. Stesso schema del gate TMv2 a incremento zero.
  // Nessun toggle: e' correttezza, e senza tablebase configurate e' un no-op esatto
  // (nessuno score cade fra 29873 e 30000 se non viene da Syzygy).
  constexpr int corr_max = mate_score - max_ply;   // 29936 = TB_VALUE_WIN
  if (best_score >= corr_max || best_score <= -corr_max)
    return;
  if (static_eval >= corr_max || static_eval <= -corr_max)
    return;
  int diff = best_score - static_eval;
  if (bound == hash_flag_beta && diff < 0)
    return; // lower bound: only push eval up
  if (bound == hash_flag_alpha && diff > 0)
    return; // upper bound: only push eval down
  int target = diff * CORR_GRAIN;
  int w = (depth < 16) ? depth : 16; // deeper search = more trust
  int lim = g_corr_cap * CORR_GRAIN; // clamp stored value to the cap
  td_corr_bucket_update(td.corr_hist[td.side][idx], target, w, lim);
  if (g_corr_multi) {
    td_corr_bucket_update(td.corr_hist_minor[td.side][td_corr_index_minor(td)],
                          target, w, lim);
    td_corr_bucket_update(td.corr_hist_major[td.side][td_corr_index_major(td)],
                          target, w, lim);
  }
  if (g_corr_nonpawn) {
    td_corr_bucket_update(
        td.corr_hist_np[0][td.side][td_corr_index_np_white(td)], target, w,
        lim);
    td_corr_bucket_update(
        td.corr_hist_np[1][td.side][td_corr_index_np_black(td)], target, w,
        lim);
  }
  if (g_corr_cont) {
    if (int16_t *cc = td_cont_corr_bucket(td)) {
      int cv = *cc; // gravity update on the int16 bucket
      // BUG FIX 2026-07-16: con CorrCap>=128, lim = cap*256 >= 32768 >
      // INT16_MAX
      // -> il clamp permetteva 32768, che castato a int16 INVERTE il segno
      // della correzione. Cappa lim al range int16 per QUESTA tabella (le altre
      // sono int).
      int lim16 = lim < 32767 ? lim : 32767;
      // FIX 3: Sparsita' della Tabella. La cont_corr ha 589k entry.
      // Per farle imparare qualcosa prima della fine della ricerca,
      // moltiplichiamo il peso dell'update (w) per 2. Cosi' i pattern rari
      // imparano in fretta.
      int w_cont = w * 2;
      td_corr_bucket_update(cv, target, w_cont, lim16);
      *cc = (int16_t)cv;
    }
  }
  if (g_corr_material) {
    td_corr_bucket_update(
        td.corr_hist_material[td.side][td_corr_index_material(td)], target, w,
        lim);
  }
}

// Multi-ply continuation-history update: apply `bonus` to the 2/4-ply
// (ContHistMulti) and 3/6-ply (ContHist36) buckets at a cutoff. No-op unless
// one of those is on. Must be called with td.ply == the current node's ply
// (same context as the 1-ply update).
static inline void td_conthist_multi_update(ThreadData &td, int move,
                                            int bonus) {
  if (!g_conthist_multi && !g_conthist36)
    return;
  int pc = get_move_piece(move), tg = get_move_target(move);
  if (g_conthist_multi) {
    if (td.ply >= 1) {
      int p2 = td.move_stack[td.ply - 1];
      if (p2)
        td_update_history(
            td.cont_hist_2[get_move_piece(p2)][get_move_target(p2)][pc][tg],
            bonus);
    }
    if (td.ply >= 3) {
      int p4 = td.move_stack[td.ply - 3];
      if (p4)
        td_update_history(
            td.cont_hist_4[get_move_piece(p4)][get_move_target(p4)][pc][tg],
            bonus);
    }
  }
  // #4: 3-ply (ply-2) e 6-ply (ply-5). Indipendente da ContHistMulti.
  if (g_conthist36) {
    if (td.ply >= 2) {
      int p3 = td.move_stack[td.ply - 2];
      if (p3)
        td_update_history(
            td.cont_hist_3[get_move_piece(p3)][get_move_target(p3)][pc][tg],
            bonus);
    }
    if (td.ply >= 5) {
      int p6 = td.move_stack[td.ply - 5];
      if (p6)
        td_update_history(
            td.cont_hist_6[get_move_piece(p6)][get_move_target(p6)][pc][tg],
            bonus);
    }
  }
}

// Q-12 CMHC (SF PR #6653): fattore di scala % del bonus CONTHIST per la
// CONSISTENZA della cutoff-move nei contesti della continuation-stack. Piu'
// contesti in cui la mossa ha gia' continuation-history POSITIVA -> update piu'
// grande (rinforza le mosse buone-in-piu'-contesti). Usa gli STESSI ply/tabelle
// che l'update tocca (1-ply sempre, 2/4-ply e 3/6-ply sotto i rispettivi
// toggle). 0 = neutro (fattore 100, byte-identico).
static inline int td_cmhc_factor(ThreadData &td, int move) {
  if (!g_cmhc_scale)
    return 100;
  int pc = get_move_piece(move), tg = get_move_target(move);
  int consistent = 0;
  if (td.ply >= 1) {
    int p = td.move_stack[td.ply - 1];
    if (p &&
        td.continuation_history[get_move_piece(p)][get_move_target(p)][pc][tg] >
            0)
      consistent++;
  }
  if (g_conthist_multi) {
    if (td.ply >= 1) {
      int p = td.move_stack[td.ply - 1];
      if (p &&
          td.cont_hist_2[get_move_piece(p)][get_move_target(p)][pc][tg] > 0)
        consistent++;
    }
    if (td.ply >= 3) {
      int p = td.move_stack[td.ply - 3];
      if (p &&
          td.cont_hist_4[get_move_piece(p)][get_move_target(p)][pc][tg] > 0)
        consistent++;
    }
  }
  if (g_conthist36) {
    if (td.ply >= 2) {
      int p = td.move_stack[td.ply - 2];
      if (p &&
          td.cont_hist_3[get_move_piece(p)][get_move_target(p)][pc][tg] > 0)
        consistent++;
    }
    if (td.ply >= 5) {
      int p = td.move_stack[td.ply - 5];
      if (p &&
          td.cont_hist_6[get_move_piece(p)][get_move_target(p)][pc][tg] > 0)
        consistent++;
    }
  }
  return 100 + g_cmhc_scale * consistent;
}

int td_negamax(ThreadData &td, int alpha, int beta, int depth, bool is_cut_node,
               int excluded_move = 0, bool follow_pv = false) {
  td.pv_length[td.ply] = td.ply;

  // Illegal-position / king-capture guard. If the side-to-move can capture the
  // opponent's king (i.e. the side NOT to move is in check), the position is
  // illegal - typically a malformed input FEN (e.g. a tablebase test position
  // with the enemy king left in check). Searching it would let us actually make
  // the king-capturing move; td_make_move accepts it (our own king is safe),
  // nn_pos_do then removes the enemy king from the NNUE mirror, and the next
  // eval indexes a king-bucket feature for a now-kingless side -> access
  // violation. Return an immediate winning score instead. In legal play the
  // side not to move is never in check, so this costs one attack probe and
  // never fires. (Gate ply==0 provato 2026-06-06: node-identico ma NPS ~0 ->
  // non vale; tenuto pieno. La probe e' troppo cheap per spostare gli NPS.)
  {
    int opp_king_sq =
        get_ls1b_index((td.side == white) ? td.bitboards[k] : td.bitboards[K]);
    if (td_is_square_attacked(td, opp_king_sq, td.side))
      return mate_value - td.ply;
  }

  // P1.11 draw dither (SF value_draw): ±1cp dal node-count invece di 0 secco,
  // rompe la cecita' da ripetizione fra linee "ugualmente patte".
  if (td.ply && (td_is_repetition(td) || td.fifty >= 100)) {
    // F-013 (2026-07-03): alla 100a semimossa il MATTO prevale sulla patta
    // (FIDE/SF rule50 gate). Ramo freddissimo: paga la verifica "esiste una
    // mossa legale?" solo con fifty>=100 E lato al tratto sotto scacco.
    if (td.fifty >= 100) {
      int ksq50 = get_ls1b_index((td.side == white) ? td.bitboards[K]
                                                    : td.bitboards[k]);
      if (td_is_square_attacked(td, ksq50, td.side ^ 1)) {
        moves ml50[1];
        td_generate_moves(td, ml50, false);
        bool any_legal = false;
        for (int i50 = 0; i50 < ml50->count; i50++) {
          UndoInfo u50;
          td.ply++;
          td.repetition_table[++td.repetition_index] = td.hash_key;
          if (!td_make_move(td, ml50->moves[i50], u50)) {
            td.ply--;
            td.repetition_index--;
            continue;
          }
          td_unmake_move(td, ml50->moves[i50], u50);
          td.ply--;
          td.repetition_index--;
          any_legal = true;
          break;
        }
        if (!any_legal)
          return -mate_value + td.ply; // scacco matto: niente patta-50
      }
    }
    return g_draw_dither ? (1 - (int)(td.nodes & 2)) : 0;
  }

  // P1.4 mate-distance pruning: un matto trovato a questa profondita' non puo'
  // battere uno gia' provato piu' corto -> stringe la finestra e taglia subito.
  // Bound conservativi (mate_value - ply, senza il -1: il check cattura-re
  // sopra puo' restituire un matto a ply+0, vedi return mate_value - td.ply).
  if (g_mate_dist && td.ply) {
    if (alpha < -mate_value + td.ply)
      alpha = -mate_value + td.ply;
    if (beta > mate_value - td.ply)
      beta = mate_value - td.ply;
    if (alpha >= beta)
      return alpha;
  }

  // P1.2 (2026-06-09): ripetizione IMMINENTE (cuckoo). Se esiste una mossa
  // reversibile che riporta in una posizione gia' vista, il lato al tratto puo'
  // forzare la patta senza che la search debba scoprirlo: con alpha sotto la
  // patta, alza alpha a 0 (anti-shuffling, come SF step 2).
  if (g_upcoming_rep && td.ply && alpha < 0 && td.fifty >= 3 &&
      td_upcoming_repetition(td)) {
    alpha = 0;
    if (alpha >= beta)
      return alpha;
  }

  bool pv_node = (beta - alpha > 1);
  int tt_move = 0;
  int tt_score = 0;
  int tt_depth = 0;
  int tt_flag = hash_flag_alpha;
  int tt_eval = tt_eval_none; // P1.1: static eval salvata nell'entry (o none)
  bool tt_pv = false;
  int score;

  // TT probe
  bool tt_hit = probe_tt(td.hash_key, tt_move, tt_score, tt_depth, tt_flag,
                         tt_eval, tt_pv);

  // ttPv (SF): un nodo è "PV-ish" se è un vero PV node o se la TT lo ricorda
  // ex-PV. store_pv viene scritto negli store TT (propaga il flag); il segnale
  // NUOVO è ridurre meno la LMR sui nodi non-PV che la TT marca ex-PV. Gated da
  // g_ttpv (OFF = byte-identico).
  bool store_pv = g_ttpv_amount > 0 && (pv_node || (tt_hit && tt_pv));

  // Skip the TT cutoff during a singular search (excluded_move set): we are
  // deliberately re-searching this position without the TT move.
  if (tt_hit && td.ply && !excluded_move) {
    if (tt_depth >= depth && !pv_node) {
      if (tt_score < -mate_score)
        tt_score += td.ply;
      if (tt_score > mate_score)
        tt_score -= td.ply;
      if (g_tt_decisive_clamp)
        tt_score = tt_clamp_decisive(tt_score, td.fifty); // Q-32

      // F-018.3a-c TTCutRefine (default OFF): (a) i fail-high richiedono 1 ply
      // di depth TT in piu'; (b) coerenza bound/tipo-nodo (cutNode ==
      // fail-high); (c) niente cutoff con la patta-50 vicina (lo score TT
      // ignora il fifty).
      bool ttcut_ok = true;
      if (g_ttcut_refine) {
        bool tt_fh = tt_score >= beta;
        ttcut_ok = (!tt_fh || tt_depth >= depth + 1) &&
                   (is_cut_node == tt_fh) && td.fifty < g_ttcut_fifty;
      }

      // P1.13 (SF): il cutoff servito dalla TT non passa dal loop mosse ->
      // senza questo bonus la history del ttMove quiet si raffredda anche se
      // la mossa continua a tagliare. Scala SPSA-tunable (TTCutBonusScale).
      if (g_ttcut_bonus && tt_move && tt_score >= beta &&
          tt_flag != hash_flag_alpha && !get_move_capture(tt_move)) {
        int bonus = td_stat_bonus(depth) * g_ttcut_bonus_scale / 100;
        td_update_history(
            td.history_moves[td_hbucket(td, tt_move)][get_move_piece(tt_move)]
                            [get_move_target(tt_move)],
            bonus);
      }

      if (ttcut_ok) {
        bool would_cut = (tt_flag == hash_flag_exact) ||
                         (tt_flag == hash_flag_alpha && tt_score <= alpha) ||
                         (tt_flag == hash_flag_beta && tt_score >= beta);
        if (would_cut) {
          // F-018.3d TTCutMalus (default OFF, duale di TTCutBonus): la quiet
          // AVVERSARIA che porta a un nodo che cutta subito su TT era una mossa
          // debole -> raffreddala nell'ordering del PADRE (solo se il padre
          // aveva visto poche mosse: il segnale conta all'inizio della lista).
          if (g_ttcut_malus && tt_score >= beta && tt_flag != hash_flag_alpha) {
            int pm = td.move_stack[td.ply];
            if (pm && td.captured_stack[td.ply] == -1 &&
                !get_move_promoted(pm) &&
                td.seen_stack[td.ply - 1] <= g_ttcut_malus_seen) {
              int tmalus = td_stat_bonus(depth);
              td_update_history(
                  td.history_moves[td.hbucket_stack[td.ply]][get_move_piece(pm)]
                                  [get_move_target(pm)],
                  -tmalus);
              if (td.ply >= 2) {
                int pm2 = td.move_stack[td.ply - 1];
                if (pm2)
                  td_update_history(
                      td.continuation_history[get_move_piece(
                          pm2)][get_move_target(pm2)][get_move_piece(pm)]
                                             [get_move_target(pm)],
                      -tmalus);
              }
            }
          }
          return tt_score; // fail-soft
        }
      }
    }

    // Q-14 TTResearchMargin (Ethereal): fail-low anticipato a depth-1. Solo
    // entry UPPER a tt_depth>=depth-1, !PV, e ttScore ben sotto alpha
    // (margine). Non si sovrappone al cutoff principale sopra: se
    // tt_depth>=depth e alpha-bound cuttava, il return e' gia' avvenuto. Adjust
    // matto locale (l'entry e' relativa alla root).
    if (g_tt_research && !pv_node && tt_depth >= depth - 1 &&
        tt_flag == hash_flag_alpha) {
      int adj = tt_score;
      if (adj < -mate_score)
        adj += td.ply;
      if (adj > mate_score)
        adj -= td.ply;
      if ((is_cut_node || adj <= alpha) && adj + g_tt_research_margin <= alpha)
        return adj; // fail-soft (adj < alpha garantito dal margine)
    }
  }

  // Time check
  if ((td.nodes & 4095) == 0) {
    if (stop_threads.load(std::memory_order_relaxed))
      return 0;
    // A5: `td_root_move_ready()` = pavimento anti-forfeit, vedi la definizione.
    // Short-circuit: non viene mai valutata finche' il tempo non e' scaduto.
    if (timeset && get_time_ms() > stoptime && td_root_move_ready()) {
      stop_threads.store(true, std::memory_order_relaxed);
      return 0;
    }
  }

  if (depth <= 0)
    return td_quiescence(td, alpha, beta);

  // Ply ceiling. Set pv_length here so the parent's PV-copy loop reads a sane
  // (terminating) bound from pv_length[ply+1] instead of OOB garbage.
  if (td.ply >= max_ply) {
    td.pv_length[td.ply] = td.ply;
    return td_evaluate(td);
  }

  td.nodes++;
  if (g_node_limit && td.nodes >= g_node_limit) {
    stop_threads.store(true, std::memory_order_relaxed);
    return 0;
  }
  if (td.ply > td.seldepth)
    td.seldepth = td.ply; // seldepth (info UCI)

  // Syzygy tablebase WDL probe. For a non-root node with no castling rights
  // and few enough pieces, the tablebase gives the exact game value (side-to-
  // move relative), so we short-circuit the search with it. Cursed/blessed
  // results map to 0 (50-move-rule draws); the actual win conversion in the
  // reached position is guaranteed by the root DTZ probe. count_bits runs
  // only when tablebases are loaded (syzygy_max_pieces() > 0).
  {
    unsigned tb_men = syzygy_max_pieces();
    // SyzygyProbeLimit: clamp opzionale sotto TB_LARGEST (default 7 = no-op).
    if (g_syzygy_probe_limit >= 0 && (unsigned)g_syzygy_probe_limit < tb_men)
      tb_men = (unsigned)g_syzygy_probe_limit;
    // S-10 guard fifty==0 (2026-07-09): fathom (tbprobe.h:223) fallisce SEMPRE
    // con rule50!=0 ma pagavamo comunque il marshalling to_fathom -> no-op
    // comportamentale.
    if (tb_men && td.ply && !excluded_move && td.castle == 0 && td.fifty == 0 &&
        depth >= g_syzygy_probe_depth &&
        (unsigned)count_bits(td.occupancies[both]) <= tb_men) {
      int tb_score;
      if (syzygy_probe_wdl(td, td.ply, tb_score)) {
        td.tb_hits++; // -> contatore "tbhits" (Fritz "TBAs")
        // S-10 TBScoreTT (default OFF): le rivisite colpiscono la TT invece
        // di ri-probare. depth+6 come SF (search.cpp step 5).
        if (g_tb_tt_store) {
          int sd = depth + 6;
          if (sd > 254)
            sd = 254;
          store_tt(td.hash_key, 0, tb_score, sd, hash_flag_exact, td.ply);
        }
        return tb_score;
      }
    }
  }

  int king_sq =
      get_ls1b_index((td.side == white) ? td.bitboards[K] : td.bitboards[k]);
  bool in_check = td_is_square_attacked(td, king_sq, td.side ^ 1);

  // P1.9: gate co-tunabile sulla check-extension (default 128 = sempre =
  // comportamento storico; il co-tune puo' abbassarlo, 0 = mai).
  if (in_check && depth <= g_check_ext_depth)
    depth++;

  // Internal Iterative DEEPENING (gated, default OFF): senza TT move, invece di
  // limitarsi a ridurre la profondita' (IIR economico, sotto), facciamo una
  // mini-ricerca ridotta PRIMA per OTTENERE una hash move buona -> ordinamento
  // ~SF
  // -> il taglio "morde". Solo su pv/cut node (dove l'ordinamento conta), con
  // freno anti-cascata thread_local (niente IID dentro la mini-ricerca:
  // eviterebbe la ricorsione esplosiva). OFF (g_iid=false) = blocco saltato =
  // byte-identico.
  static thread_local bool tl_in_iid = false;
  if (g_iid && !tt_move && !excluded_move && !tl_in_iid &&
      (pv_node || is_cut_node) && depth >= g_iid_depth) {
    int iid_depth = depth - g_iid_reduction;
    if (iid_depth >= 1) {
      tl_in_iid = true;
      td_negamax(td, alpha, beta, iid_depth, is_cut_node, excluded_move);
      tl_in_iid = false;
      // Ri-probe la TT per la mossa appena trovata (solo tt_move; il resto in
      // scratch
      // -> non tocchiamo tt_hit/tt_score/tt_depth/tt_flag: l'IID serve
      // all'ORDINAMENTO).
      int s2 = 0, d2 = 0, f2 = hash_flag_alpha, e2 = tt_eval_none;
      bool p2 = false;
      probe_tt(td.hash_key, tt_move, s2, d2, f2, e2, p2);
    }
  }

  // Internal Iterative Reduction: senza TT move l'ordinamento e' scadente,
  // riduciamo di 1 ply per ottenere a basso costo una hash move. (Se l'IID
  // sopra ha trovato una mossa, tt_move != 0 -> questa riduzione NON scatta.)
  if (depth >= g_iir_min_depth && !tt_move && !excluded_move && !follow_pv &&
      !(g_iir_not_in_check && in_check))
    depth -= g_iir_amount; // Q-15: niente IIR su follow-PV; fix opz. in-check
  // NB (2026-07-16): l'IIR spara ANCHE in scacco, annullando la check-ext (+1
  // poi -1). Fix "&& !in_check" candidato +1.5 LTC (SF/Alexandria) ma NON
  // confermabile coi tempi attuali (LTC = troppe partite) -> IN CODA al blocco
  // finale, NON bakato a fiducia.

  // cutoffCnt: azzera il contatore 2 ply avanti (come SF (ss+2)=0), cosi e'
  // fresco per i figli di questo nodo. Letto in LMR solo se
  // g_cutoffcnt_penalty>0.
  if (td.ply + 2 < max_ply + 8)
    td.cutoff_cnt[td.ply + 2] = 0;

  // ProbCut sotto scacco (SF step 12, default off): se la TT ricorda una
  // cattura con bound LOWER e score >= beta+margin a profondita' adeguata,
  // taglia subito (~4 Elo SF).
  if (g_probcut_incheck_margin && in_check && !pv_node && !excluded_move &&
      tt_hit && tt_move && get_move_capture(tt_move) &&
      tt_flag == hash_flag_beta && tt_depth >= depth - 4 &&
      tt_score >= beta + g_probcut_incheck_margin && tt_score < mate_score &&
      tt_score > -mate_score && beta < mate_score && beta > -mate_score)
    return beta + g_probcut_incheck_margin;

  // Correction history: bucket for this position (pawn structure + side). Index
  // computed once; reused to apply the correction here and to learn at node
  // exit.
  const int corr_idx = td_corr_index(td);
  int static_eval;
  int corr_val = 0; // correction applied to this node's static eval (used by
                    // CorrValMargin)
  int corr_uncert =
      0; // P1a: disaccordo tra le tavole corr = incertezza locale eval
  int node_raw_eval =
      tt_eval_none; // P1.1: eval PURA (pre-corr) da salvare in TT
  if (g_lazy_eval && in_check) {
    // In check, no forward-pruning rule reads static_eval (all gated
    // !in_check) and corr-update is skipped -> don't pay for the NNUE eval.
    static_eval = 0;
    // FIX P0.6: marcare lo slot come "nessuna eval" invece di 0: i discendenti
    // a ply+2/ply+4 non devono confrontare la loro eval con uno 0 fittizio.
    // (ImprovingFix OFF = vecchio comportamento: slot a 0.)
    td.eval_stack[td.ply] = g_improving_fix ? EVAL_NONE : 0;
  } else {
    // P1.1 TTStaticEval: l'eval salvata in TT evita la forward NNUE (58% del
    // tempo-nodo). In TT vive DE-smorzata dal rule50 -> ri-smorza col fifty
    // CORRENTE (fix staleness). raw resta PURA pre-correction (la corr e'
    // appresa/tempo-variante, si riapplica fresca qui sotto).
    // flag_none = entry eval-only (EvalTTWrite, SF-style): l'UNADJUSTED cachato
    // si ri-finalizza col fifty corrente (nn_finalize) -> ESATTO come
    // td_evaluate su ogni trasposizione. Entry reale: eval de-smorzata ->
    // redamp (path validato +44).
    node_raw_eval =
        (g_tt_static_eval && tt_eval != tt_eval_none)
            ? (tt_flag == hash_flag_none ? nn_finalize(tt_eval, td.fifty)
                                         : tt_eval_redamp(tt_eval, td.fifty))
            : td_evaluate(td);
    static_eval = node_raw_eval;
    if (g_corr_hist) {
      corr_val = td_corr_value(td, corr_idx);
      static_eval += corr_val;
    }
    if (g_corr_uncert && g_corr_hist && g_corr_multi)
      corr_uncert = td_corr_uncert(td, corr_idx);
    td.eval_stack[td.ply] = static_eval;
    // 5.1 (SF :830): su un MISS abbiamo appena pagato la forward NNUE -> cache
    // l'UNADJUSTED (nn_last_unadjusted, fifty-indep) nello slot naturale (no
    // pollution) cosi' i nodi potati non ricalcolano la forward alla rivisita.
    // tt_eval==none qui => td_evaluate() appena chiamato.
    if (g_eval_tt_write && tt_eval == tt_eval_none)
      tt_cache_eval(td.hash_key, nn_last_unadjusted());
  }

  // P1.1 (2026-06-09): "improved eval" stile SF — se la TT ha uno score il cui
  // bound e' coerente, e' una stima di qualita'-search migliore della static
  // eval: usala per le DECISIONI DI PRUNING
  // (RFP/NMP/razor/futility/probcut/LMR-margin). static_eval resta la base di
  // eval_stack/improving/corr-update (come SF, che tiene ss->staticEval puro e
  // adatta solo `eval`). Toggle TTEvalImprove per A/B.
  int eval = static_eval;
  if (g_tt_eval_improve && !in_check && tt_hit && tt_score > -mate_score &&
      tt_score < mate_score &&
      (tt_flag == hash_flag_exact ||
       (tt_flag == hash_flag_beta && tt_score > eval) ||
       (tt_flag == hash_flag_alpha && tt_score < eval)))
    eval = tt_score;

  // "Improving": is our static eval higher than it was two plies ago (our own
  // previous turn)? A rising trend means the position is going our way, so we
  // can trust forward pruning more and reduce more when it's NOT improving.
  // Meaningless in check (eval is noisy there) and at ply<2 (no history) ->
  // treated as not-improving. g_improving gates the whole heuristic so that,
  // when off, every consumer below collapses to the original search exactly.
  // FIX P0.6: se a ply-2 non c'era eval (nodo in scacco con LazyEval), fallback
  // a ply-4 (come SF); se nemmeno quella esiste -> not-improving (=
  // comportamento storico, senza pero' il confronto col vecchio 0 fittizio).
  bool improving = false;
  if (!in_check && td.ply >= 2) {
    int e2 = td.eval_stack[td.ply - 2];
    if (!g_improving_fix)
      improving =
          static_eval > e2; // legacy 3.7 (confronto anche col 0 fittizio)
    else if (e2 != EVAL_NONE)
      improving = static_eval > e2;
    else if (td.ply >= 4 && td.eval_stack[td.ply - 4] != EVAL_NONE)
      improving = static_eval > td.eval_stack[td.ply - 4];
  }

  // opponentWorsening (SF search.cpp :841): our static eval exceeds the negated
  // previous-ply eval -> the opponent's last move worsened their side -> we can
  // shave the RFP margin (prune a touch more). Gated like improving. Off = no
  // effect.
  bool opp_worsening = false;
  if (g_opp_worsening && !in_check && td.ply >= 1) {
    int e1 = td.eval_stack[td.ply - 1];
    if (e1 != EVAL_NONE)
      opp_worsening = static_eval > -e1;
  }

  // StatEvalDiffOrder (SF search.cpp ~728, "~9 Elo"): la differenza tra la
  // static eval di questo nodo e quella del padre e' un segnale a costo ~0
  // sulla qualita' della mossa PRECEDENTE (dell'avversario). Spingiamo la sua
  // history (main + pawn) col negato della somma delle due eval consecutive.
  // Gating come SF: nodo corrente NON in scacco (eval valida), padre NON in
  // scacco (eval_stack valida), ESISTE una mossa precedente, ed era QUIET.
  // g_seo_mult: 0=off, 14=SF, >14=aggr.
  if (g_seo_mult && !in_check && td.ply >= 1) {
    int prev_move = td.move_stack[td.ply];
    if (prev_move != 0 && td.captured_stack[td.ply] == -1 &&
        td.eval_stack[td.ply - 1] != EVAL_NONE) {
      int unscaled_eval_sum =
          (td.eval_stack[td.ply - 1] + static_eval) * 100 / nn_get_eval_scale();
      int bonus = -g_seo_mult * unscaled_eval_sum;
      if (bonus > 1455)
        bonus = 1455; // clamp PRIMA del raddoppio (come SF)
      if (bonus < -1723)
        bonus = -1723;
      bonus = bonus > 0 ? 2 * bonus : bonus / 2;
      int pp = get_move_piece(prev_move);
      int pt = get_move_target(prev_move);
      td_update_history(td.history_moves[td.hbucket_stack[td.ply]][pp][pt],
                        bonus);
      if (g_pawn_hist && pp != P && pp != p && !get_move_promoted(prev_move))
        td_update_history(
            td.pawn_history[corr_idx & ThreadData::PAWN_HIST_MASK][pp][pt],
            bonus / 4);
    }
  }

  // 5.1 HindsightExt (Pawnocchio): se QUESTO nodo non-PV e' stato RIDOTTO (LMR)
  // dal padre di
  // >= margine plies e l'eval e' girata male (static_eval + prev_eval < 0),
  // ri-estendi (+1): la riduzione era un errore "col senno di poi".
  // reduction_stack settato dal padre pre-recursione. Default OFF =
  // byte-identico (reduction_stack mai letto). Non in scacco/singular/PV/root.
  if (g_hindsight_ext && !pv_node && !excluded_move && !in_check &&
      td.ply >= 1 && td.eval_stack[td.ply - 1] != EVAL_NONE &&
      td.reduction_stack[td.ply] >= g_hindsight_margin &&
      static_eval + td.eval_stack[td.ply - 1] < 0)
    depth++;
  // 5.1 HindsightRed (Stormphrax #300, complemento): nodo ridotto >= margine ma
  // eval girata BENE (static_eval + prev_eval >= soglia) -> RIDUCI (-1): la
  // posizione regge, risparmia nodi. else-if -> mutuamente esclusivo con
  // l'extend (sum<0 vs sum>=soglia). depth>=2 per non azzerare.
  else if (g_hindsight_red && !pv_node && !excluded_move && !in_check &&
           depth >= 2 && td.ply >= 1 &&
           td.eval_stack[td.ply - 1] != EVAL_NONE &&
           td.reduction_stack[td.ply] >= g_hindsight_red_margin &&
           static_eval + td.eval_stack[td.ply - 1] >= g_hindsight_red_thresh)
    depth--;

  // Reverse futility pruning (skip when beta is a mate bound: don't cut a
  // potential mate search short based on a static evaluation). When improving,
  // shave one ply off the margin (easier cutoff): a rising eval is more likely
  // to hold above beta.
  int rfp_cap = g_rfp_depth_cap > 0 ? g_rfp_depth_cap : (g_rfp_depth8 ? 8 : 6);
  if (!pv_node && !in_check && depth <= rfp_cap && beta < mate_score) {
    // F-018.12 RFPHistThresh (0=off): se la hash move e' una quiet con history
    // scarsa, il fail-high statico non e' affidabile -> non tagliare (Berserk).
    bool rfp_ok = true;
    if (g_rfp_hist_thresh && tt_move && !get_move_capture(tt_move) &&
        !get_move_promoted(tt_move))
      rfp_ok =
          td.history_moves[td_hbucket(td, tt_move)][get_move_piece(tt_move)]
                          [get_move_target(tt_move)] > g_rfp_hist_thresh;
    int rfp_depth = depth - ((g_improving && improving) ? 1 : 0);
    int rfp_margin = g_rfp_margin * rfp_depth;
    if (opp_worsening)
      rfp_margin -= g_opp_worse_margin; // opp worse -> prune more
    if (g_rfp_badnode && !tt_move)
      rfp_margin -= g_rfp_badnode; // Q-20a: badNode -> prune more
    if (g_corrval_margin)
      rfp_margin += (corr_val < 0 ? -corr_val : corr_val) * g_corrval_rfp /
                    128; // heavy corr -> prune less
    if (g_corr_uncert)
      rfp_margin +=
          corr_uncert * g_cu_rfp /
          64; // P1a: tavole in disaccordo -> eval incerta -> pota meno
    if (rfp_margin < 0)
      rfp_margin = 0;
    if (rfp_ok && eval - rfp_margin >= beta)
      // F-018.7 FailHighSmooth: midpoint verso beta = bound TT meno gonfiato ->
      // meno re-search.
      return g_fh_smooth
                 ? td_fail_firm(eval - rfp_margin, beta, g_fh_t_rfp)
                 : eval - rfp_margin;
  }

  // Null move pruning (not when beta is a mate score)
  // P1.6 NMPVerif (default OFF): (a) mai due null consecutive (move_stack==0 =
  // il padre ha appena nullato); (b) durante una search di verifica niente
  // null.
  if (!pv_node && !in_check && td.ply && depth >= 3 && beta < mate_score &&
      eval >= beta &&
      // F-018.8a NMPStaticMargin (default OFF): la STATIC eval (non quella
      // TT-improved) deve superare beta di un margine depth-dipendente (SF:
      // 21*depth - 421).
      (!g_nmp_static_margin ||
       static_eval >= beta - g_nmp_sm_mult * depth + g_nmp_sm_bias) &&
      // F-018.11 EasyCapGate (default OFF): con un nostro pezzo in presa
      // "facile" la null lo lascia li' un ply in piu' -> fail-high fasulli
      // (Berserk).
      (!g_easycap_gate || !td_has_easy_capture(td)) &&
      (!g_nmp_verif || (td.move_stack[td.ply] != 0 && !td.in_nmp_verif))) {
    U64 our_pieces = (td.side == white) ? (td.bitboards[N] | td.bitboards[B] |
                                           td.bitboards[R] | td.bitboards[Q])
                                        : (td.bitboards[n] | td.bitboards[b] |
                                           td.bitboards[r] | td.bitboards[q]);

    if (our_pieces) {
      int old_ep = td.enpassant;
      U64 old_hash = td.hash_key;

      if (td.enpassant != no_sq)
        td.hash_key ^= enpassant_keys[td.enpassant];
      td.enpassant = no_sq;
      td.side ^= 1;
      td.hash_key ^= side_key;

      td.ply++;
      td.reduction_stack[td.ply] =
          0; // HindsightExt: il figlio null-move non e' "ridotto-LMR"
      td.de_stack[td.ply] =
          td.de_stack[td.ply -
                      1]; // F-018.6b: la catena double-ext attraversa la null
      td.repetition_table[++td.repetition_index] = td.hash_key;

      // Null move: the child has no "previous move" for counter-move.
      td.move_stack[td.ply] = 0;
      td.captured_stack[td.ply] = -1;

      // P2.2: il sottoalbero della null parte con 0 mosse reali dall'ultima
      // null (le ripetizioni non si contano attraverso una null, stile SF).
      const int saved_pfn = td.plies_from_null;
      td.plies_from_null = 0;

      int R = g_nmp_base + depth / g_nmp_div;
      // (1) NMPEvalScale: più sopra beta = riduzione maggiore (cap +3). OFF =
      // invariato.
      if (g_nmp_eval_scale) {
        // 🔴 `static_eval`, NON `eval`: SF usa ss->staticEval, e la variante sulla
        // eval TT-corretta e' quella che avevano revertato. E nessun cap: il loro
        // max(...,0) taglia solo sotto. Con /256 la riduzione extra cresce piano.
        int e = (static_eval - beta) / g_nmp_eval_div;
        if (e > 0)
          R += e;
      }
      // F-018.8b NMPTTNoisy (default OFF): TT move cattura = posizione tattica
      // gia' "risolta" da una noisy -> la null puo' permettersi un ply in piu'
      // (Obsidian).
      if (g_nmp_ttnoisy && tt_move && get_move_capture(tt_move))
        R++;
      if (R > depth - 1)
        R = depth - 1;

      nn_pos_do_null(td.nnpos, td.fifty);
      score = -td_negamax(td, -beta, -beta + 1, depth - 1 - R, !is_cut_node);
      nn_pos_undo(td.nnpos);

      td.plies_from_null = saved_pfn; // P2.2 restore
      td.ply--;
      td.repetition_index--;
      td.side ^= 1;
      td.hash_key = old_hash;
      td.enpassant = old_ep;

      if (stop_threads.load(std::memory_order_relaxed))
        return 0;
      // fail-soft: ritorna lo score reale, ma non un matto "finto" da null
      // move.
      if (score >= beta) {
        // P1.6 NMPVerif: a depth alta il fail-high della null va CONFERMATO
        // da una search REALE ridotta (anti-zugzwang). Durante la verifica
        // la null e' disattivata (td.in_nmp_verif). Verifica fallita = non
        // potare: il nodo prosegue normalmente.
        if (g_nmp_verif && depth >= g_nmp_verif_depth && !td.in_nmp_verif &&
            score < mate_score) {
          td.in_nmp_verif = true;
          int v = td_negamax(td, beta - 1, beta, depth - 1 - R, is_cut_node);
          td.in_nmp_verif = false;
          if (stop_threads.load(std::memory_order_relaxed))
            return 0;
          if (v >= beta)
            return (v >= mate_score) ? beta : v;
        } else {
          return (score >= mate_score) ? beta : score;
        }
      }
    }
  }

  // Razoring
  int razor_cap =
      g_razor_depth_cap > 0 ? g_razor_depth_cap : (g_razor_depth4 ? 4 : 3);
  bool razor_ok = true;
  if (g_razor_ttquiet) // razora solo con TT move rumorosa (nessuna TT move =>
                       // niente razoring)
    razor_ok = tt_move != 0 &&
               (get_move_capture(tt_move) || get_move_promoted(tt_move));
  if (g_razor_ttlower && tt_hit && tt_flag == hash_flag_beta)
    razor_ok = false; // bound LOWER = ha gia' fallito alto
  if (!pv_node && !in_check && razor_ok && depth <= razor_cap) {
    int razor_margin =
        g_razor_base + g_razor_mult * depth +
        g_razor_quad_coef * depth * depth; // coef 0 = legacy linear
    if (eval + razor_margin < alpha) {
      score = td_quiescence(td, alpha, beta);
      if (score < alpha)
        return score;
    }
  }

  // ProbCut: at sufficient depth, if some capture leads (via a cheap qsearch +
  // reduced-depth confirmation) to a score well above beta, the full-depth
  // search would almost certainly fail high too -> prune now. Captures only,
  // SEE-gated so we only try moves whose material swing can bridge the margin.
  // Skipped in PV / in check / near mate / during a singular search, and when a
  // deep-enough TT entry already proves the value stays below probcut_beta.
  if (g_probcut && !pv_node && !in_check && !excluded_move &&
      depth >= g_probcut_min_depth && beta < mate_score && beta > -mate_score) {
    int probcut_beta = beta + g_probcut_margin -
                       ((g_improving && improving) ? g_probcut_improve : 0);
    bool tt_blocks = tt_hit && tt_depth >= depth - 3 &&
                     tt_score < probcut_beta && tt_score > -mate_score;
    if (!tt_blocks) {
      moves pc_list[1];
      td_generate_moves(td, pc_list, true); // captures only
      int pc_scores[256];
      td_score_all_moves(td, pc_list, tt_move, pc_scores);

      for (int count = 0; count < pc_list->count; count++) {
        // Pick-next: bring the best-scored remaining move to the front.
        int best_idx = count;
        for (int i = count + 1; i < pc_list->count; i++)
          if (pc_scores[i] > pc_scores[best_idx])
            best_idx = i;
        if (best_idx != count) {
          int tm = pc_list->moves[count];
          pc_list->moves[count] = pc_list->moves[best_idx];
          pc_list->moves[best_idx] = tm;
          int ts = pc_scores[count];
          pc_scores[count] = pc_scores[best_idx];
          pc_scores[best_idx] = ts;
        }
        int move = pc_list->moves[count];

        if (!get_move_capture(move))
          continue;
        // The capture's material swing must be able to bridge the eval
        // up to probcut_beta, else it can't plausibly cause the cutoff.
        // BUG FIX 2026-07-16: td_see NON conta il guadagno di promozione
        // (~+800cp per Q)
        // -> sottovaluta una capture-promotion vincente e la filtra fuori dal
        // probcut = cutoff speculativo mancato. Le capture-promotion passano
        // sempre il gate SEE.
        if (!get_move_promoted(move) &&
            !td_see_at_least(td, move, probcut_beta - eval))
          continue;

        UndoInfo undo;
        // A1 FIX 2026-07-25: il figlio probcut inizializzava SOLO reduction_stack.
        // seen/de/hbucket/captured restavano quelli lasciati a questo ply dal
        // fratello precedente, e il figlio li LEGGE: move_stack != 0 (riga sotto)
        // apre i blocchi SEO (:6441) e TTCutMalus (:6173), che con un
        // captured_stack stale a -1 scrivono bonus/malus nella history QUIET per
        // una CATTURA, premiano la vittima sbagliata in capture_history via
        // hbucket_stack, e con un de_stack sporco abilitano doppia/tripla
        // estensione ereditata da un sottoalbero fratello (:7101).
        // Che fosse una dimenticanza lo prova reduction_stack, che c'era.
        // ponytail: terzo sito che spinge un ply figlio (main loop :7213,
        // null-move :6550, questo) con la stessa lista a mano. Se ne nasce un
        // quarto, estrarre un td_push_child() invece di ricopiarla.
        // Gate 2026-07-25: con questi 4 assegnamenti disattivati il bench torna
        // esattamente a 275063 (= la release pre-fix), quindi il delta e' TUTTO
        // di A1: 275063 -> 283729, +3.15% di albero.
        int pc_hbucket = td_hbucket(td, move); // pre-make: legge la board del padre
        td.seen_stack[td.ply] =
            count; // mosse viste a QUESTO nodo (il figlio le legge su TT-cut)
        td.ply++;
        td.reduction_stack[td.ply] =
            0; // HindsightExt: probcut child non e' "ridotto-LMR"
        td.de_stack[td.ply] =
            td.de_stack[td.ply - 1]; // il probcut non estende: la catena passa
        td.repetition_table[++td.repetition_index] = td.hash_key;
        if (!td_make_move(td, move, undo)) {
          td.ply--;
          td.repetition_index--;
          continue;
        }
        td.move_stack[td.ply] = move;
        td.hbucket_stack[td.ply] = pc_hbucket;
        td.captured_stack[td.ply] = undo.captured_piece;

        // Cheap qsearch screen first; only if it clears probcut_beta do we
        // pay for the reduced-depth (depth-4) confirmation search.
        // Q-10 ProbCutImproving (default OFF): -1 ply quando improving
        // (depth 0 -> td_negamax cade in qsearch, come SF/Reckless).
        int pc_depth = depth - g_probcut_depth_off -
                       ((g_probcut_improving && improving) ? 1 : 0);
        int pc_score = -td_quiescence(td, -probcut_beta, -probcut_beta + 1);
        if (pc_score >= probcut_beta)
          pc_score = -td_negamax(td, -probcut_beta, -probcut_beta + 1, pc_depth,
                                 !is_cut_node);

        td_unmake_move(td, move, undo);
        td.ply--;
        td.repetition_index--;

        if (stop_threads.load(std::memory_order_relaxed))
          return 0;

        if (pc_score >= probcut_beta) {
          // N2 ProbCutTT (SF): salva il fail-high in TT a pc_depth+1 (= depth-3
          // col Q-10 off) cosi' le rivisite tagliano dal probe senza rifare
          // qsearch+verifica.
          if (g_probcut_tt && !excluded_move)
            store_tt(td.hash_key, move, pc_score, pc_depth + 1, hash_flag_beta,
                     td.ply, store_pv, tt_eval_undamp(node_raw_eval, td.fifty));
          return pc_score; // fail-soft prune
        }
      }
    }
  }

  // ---- Move generation & ordering ------------------------------------------
  // Staged MovePicker (g_move_picker) produces moves lazily by stage, so an
  // early cutoff never pays to generate/score the quiet moves. Legacy path:
  // generate everything and score up-front (pick-next selection).
  const bool use_picker = g_move_picker;

  moves move_list[1]; // legacy: all moves | staged: quiet-stage buffer
  int move_scores[256];
  moves cap_buf[1]; // staged: capture/promotion-stage buffer
  int cap_score_buf[256];

  if (!use_picker) {
    td_generate_moves(td, move_list);
    // Assegna punteggi senza ordinare l'array (la selezione e' pick-next).
    td_score_all_moves(td, move_list, tt_move, move_scores);
  }

  MovePicker mp;
  mp_init(mp, td, use_picker, tt_move, cap_buf, cap_score_buf, move_list,
          move_scores, in_check ? 1 : 0);

  int best_move = 0;
  int best_score = -infinity;
  int hash_flag = hash_flag_alpha;
  int legal_moves = 0;

  // Node-based time management: at the root, track how many nodes the best move
  // costs. Reset per iteration; updated when a move becomes the new best.
  const bool is_root_node = (td.ply == 0);
  if (is_root_node)
    td.root_bestmove_nodes = 0;

  // Q-11 NodeCache (Caissa Search.cpp:1683-1688): entry per QUESTO nodo se e'
  // vicino alla radice. Riempita dopo ogni figlio (nc_add sotto). nullptr =
  // niente tracking.
  ThreadData::NCEntry *nc = nullptr;
  if (g_node_cache && td.ply < ThreadData::NC_MAX_PLY && !excluded_move)
    nc = nc_get(td, td.hash_key);
  // R-PB: la TT-move di QUESTO nodo, letta dal figlio come "la mossa che mi ha
  // generato era la TT-move di mio padre?". Scritta una volta sola qui, non nel
  // move loop: non dipende dalla mossa in corso.
  td.ttmove_stack[td.ply] = tt_move;

  int moves_searched = 0;
  int quiets_searched = 0;
  LmpChkCtx
      lmp_chk; // LMPCheckGuard: check-squares del nodo, riempite alla prima LMP

  // Quiet moves searched at this node (for history malus on a beta cutoff).
  int searched_quiets[64];
  int n_searched_quiets = 0;

  int searched_captures[64];
  int n_searched_captures = 0;

  // F-018.13 KillerReset (default OFF): azzera i killer del ply FIGLIO prima
  // del loop mosse — i killer ereditati da sottoalberi FRATELLI sono stale
  // (Obsidian/Berserk).
  if (g_killer_reset && td.ply + 1 < max_ply + 8) {
    td.killer_moves[0][td.ply + 1] = 0;
    td.killer_moves[1][td.ply + 1] = 0;
  }

  int move;
  while ((move = mp_next(td, mp)) != 0) {
    // Ora move   garantita essere la migliore
    // `move` is supplied by mp_next() in the while condition above.
    if (move == excluded_move)
      continue; // singular search: skip the TT move
    // P4 TroubleMaking: verifica del candidato -> alla root SOLO quella mossa
    if (is_root_node && td.root_only_move && move != td.root_only_move)
      continue;
    // searchmoves (analisi): se una whitelist e' attiva, alla root cerca SOLO
    // quelle mosse. Vale per la linea principale e per tutte le linee MultiPV.
    if (is_root_node && g_searchmoves_count) {
      bool in_list = false;
      for (int k = 0; k < g_searchmoves_count; k++)
        if (g_searchmoves[k] == move) {
          in_list = true;
          break;
        }
      if (!in_list)
        continue;
    }
    // MultiPV: alla root salta le best move gia' assegnate alle linee
    // precedenti (NON via excluded_move: quello e' del singular e sopprime lo
    // store TT).
    if (is_root_node && td.mpv_count) {
      bool mpv_skip = false;
      for (int k = 0; k < td.mpv_count; k++)
        if (td.mpv_excluded[k] == move) {
          mpv_skip = true;
          break;
        }
      if (mpv_skip)
        continue;
    }
    bool is_capture = get_move_capture(move);
    bool is_promotion = get_move_promoted(move);
    bool is_quiet = !is_capture && !is_promotion;

    // LmrDepthPrune: profondita' su cui gating-are futility+conthist. Di
    // default = depth piena (byte-identico). Con g_lmrdepth_prune: depth
    // ridotta dalla LMR per QUESTA mossa (mosse tardive -> riduzione grande ->
    // prune_depth piccola -> pota anche a node-depth alto, come SF). Cap nostri
    // (6/4) restano = meno aggressivo.
    int prune_depth = depth;
    if (g_lmrdepth_prune && is_quiet && depth >= 3 && moves_searched >= 1) {
      int d_idx = depth < 64 ? depth : 63;
      int m_idx = moves_searched < 64 ? moves_searched : 63;
      prune_depth = depth - 1 - lmr_table[d_idx][m_idx];
      // PASSO 1 (SF): protezione-history. prune_depth += history/div -> le
      // mosse con buona storia (probabili cutoff) salgono e NON vengono potate;
      // le scarse scendono. SF: history = 2*mainHist + contHist + pawnHist. E'
      // cio' che mancava al port monco.
      int prev_m = td.move_stack[td.ply];
      int hist =
          2 * td.history_moves[td_hbucket(td, move)][get_move_piece(move)]
                              [get_move_target(move)];
      if (prev_m)
        hist += td.continuation_history[get_move_piece(prev_m)][get_move_target(
            prev_m)][get_move_piece(move)][get_move_target(move)];
      if (g_pawn_hist)
        hist += td.pawn_history[td_corr_index(td) & ThreadData::PAWN_HIST_MASK]
                               [get_move_piece(move)][get_move_target(move)];
      prune_depth += hist / g_lmrdepth_histdiv;
      if (prune_depth < 0)
        prune_depth = 0;
    }

    // LMP  (Q-15: mai sui nodi che seguono la PV — l'ordinamento li' e' gia'
    // fidato)
    if (!pv_node && !in_check && is_quiet && best_score > -mate_score &&
        !follow_pv) {
      int lmp_threshold = -1; // -1 = nessun move-count pruning a questo nodo
      if (g_lmp_improving) {
        // P1.7 (default OFF, co-tune): formula SF (base + d^2*quad/100) /
        // (2 - improving), SENZA cap di profondita'. LMPScale resta la
        // scala comune.
        lmp_threshold = (g_lmp_base + depth * depth * g_lmp_quad / 100) /
                        (2 - ((g_improving && improving) ? 1 : 0));
        lmp_threshold = lmp_threshold * g_lmp_scale / 100;
      } else if (depth <= 8) {
        int lmp_idx = (depth < 8) ? depth : 8;
        lmp_threshold = lmp_table[lmp_idx] * g_lmp_scale /
                        100; // LMPScale (100 = invariato)
      }
      if (lmp_threshold >= 0 && quiets_searched >= lmp_threshold) {
        if (g_lmp_check_guard) {
          // Guard: le quiet che danno SCACCO non si potano mai (forzanti:
          // tattica e matti si trovano tardissimo se le tagli). Qui NON si puo'
          // spegnere l'intero stage quiet — una quiet piu' tardiva potrebbe
          // dare scacco — quindi si paga il test mossa per mossa invece dello
          // skip di stage.
          if (!td_lmp_gives_check(td, move, lmp_chk))
            continue;
        }
        // Phase-2: with the staged picker we can skip the entire quiet
        // stage instead of testing every move individually. The flag is
        // set once; mp_next() will never enter MPS_GEN_QUIET again.
        else if (use_picker) {
          mp.skip_quiets = true;
          continue;
        } else
          continue;
      }
    }

    // Futility pruning. When improving, widen the margin so we prune fewer
    // quiets (a rising eval deserves the benefit of the doubt); when not
    // improving, the base margin prunes more.
    if (!pv_node && !in_check && prune_depth <= g_fut_depth && is_quiet &&
        best_score > -mate_score && !follow_pv &&
        (!g_fut_spare_quiet ||
         quiets_searched >= 1)) { // Q-15 + Q-20c (risparmia la prima quiet)
      int futility_margin = g_fut_base + g_fut_mult * prune_depth +
                            ((g_improving && improving) ? g_fut_improving : 0);
      if (g_corrval_ext)
        futility_margin += (corr_val < 0 ? -corr_val : corr_val) *
                           g_corrval_fut / 128; // heavy corr -> prune less
      if (g_corr_uncert)
        futility_margin +=
            corr_uncert * g_cu_fut / 64; // P1a: disaccordo corr -> pota meno

      if (eval + futility_margin <= alpha) {
        // Phase-2: once futility fires, ALL remaining quiets at this node
        // fail the same static-eval test -> skip the entire stage.
        if (use_picker) {
          mp.skip_quiets = true;
        }
        quiets_searched++;
        continue;
      }
    }

    // Capture futility pruning (SF master Step 14). A capture searched at low
    // (reduced) depth that can't lift the static eval to alpha even after
    // winning the victim (plus a capture-history bias) is pruned. Good captures
    // carry a large see_piece_values[vic] term -> never pruned; only futile
    // (material-neutral/losing) captures at a node already below alpha get cut.
    // Like our quiet futility / qsearch capture-futility, this fires before
    // make (no pre-move gives-check gate, matching the quiet path).
    // NB: il termine vittima va convertito cp -> unita'-eval (vedi
    // g_capfut_vic_scale). Fino al 2026-07-16 entrava a 1x: sotto-pesato ~2.4x
    // -> si potavano proprio le catture buone che le due righe qui sopra
    // dichiarano intoccabili.
    if (g_cap_futility && !pv_node && !in_check && is_capture &&
        !is_promotion && best_score > -mate_score) {
      int d_idx = depth < 64 ? depth : 63;
      int m_idx = moves_searched < 64 ? moves_searched : 63;
      int cap_lmr_depth =
          depth - 1 - lmr_table[d_idx][m_idx]; // reduced-depth estimate
      if (cap_lmr_depth < 0)
        cap_lmr_depth = 0;
      if (cap_lmr_depth < g_capfut_depth) {
        int tgt = get_move_target(move);
        int vic = P; // default pawn (also correct for en passant)
        int s = (td.side == white) ? p : P;
        int e = (td.side == white) ? k : K;
        for (int pc = s; pc <= e; pc++)
          if (get_bit(td.bitboards[pc], tgt)) {
            vic = pc;
            break;
          }
        int ch = td.capture_history[get_move_piece(move)][tgt][vic]
                                   [td_cbucket(td, tgt)];
        int vic_ev = (int)((long long)see_piece_values[vic] *
                           g_capfut_vic_scale * nn_get_eval_scale() / 10000);
        int fut = eval + g_capfut_base + g_capfut_mult * cap_lmr_depth +
                  vic_ev + g_capfut_chist * ch / 1024;
        if (fut <= alpha)
          continue; // captures don't bump quiets_searched
      }
    }

    // SEE pruning: scarta a bassa profondita' le catture in perdita oltre un
    // margine, prima di cercarle (riduttore di nodi). Promozioni escluse.
    // S-05 SEELmrDepth (default OFF, corretto 2026-07-06): per le QUIET usa
    // prune_depth (LMR-ridotta) SENZA il cap g_see_depth (come SF Step 14:
    // nessun cap — il quadrato in prune_depth svanisce da solo alle mosse non
    // tardive/non ridotte, es. a prune_depth=15 il margine e' -185*225, mai
    // raggiungibile). Le catture restano invariate (prune_depth==depth per loro
    // comunque; SF le compensa con un termine capthist che non abbiamo ancora —
    // non toccarle senza quello).
    bool see_lmr_path = is_quiet && (g_see_lmr_depth || g_see_lmr_linear);
    int see_gate_depth = see_lmr_path ? prune_depth : depth;
    bool see_gate_ok = see_lmr_path ? (see_gate_depth <= g_see_lmr_prune_cap)
                                    : (see_gate_depth <= g_see_depth);
    if (!pv_node && !in_check && !is_promotion && see_gate_ok &&
        best_score > -mate_score) {
      int quiet_margin_coef =
          see_lmr_path ? g_see_lmr_quiet_margin : g_see_quiet_margin;
      int see_margin =
          is_capture ? (-g_see_cap_margin * depth)
          : (see_lmr_path && g_see_lmr_linear)
              ? (-g_see_lmr_linear_coef * see_gate_depth) // S-05 forma lineare
              : (-quiet_margin_coef * see_gate_depth * see_gate_depth);
      if (g_corrval_ext)
        see_margin -= (corr_val < 0 ? -corr_val : corr_val) * g_corrval_see /
                      128; // heavy corr -> prune less (soglia piu' negativa)
      if (!td_see_at_least(td, move, see_margin)) {
        // S-11 (SF, default OFF): se siamo sotto (alpha<0) e la cattura muove
        // il nostro ULTIMO pezzo non-pedone, il sacrificio puo' essere la
        // risorsa di STALLO -> non potarla (falla cercare). Fires raro.
        bool stalemate_resource = false;
        if (g_see_stalemate_guard && is_capture && alpha < 0) {
          int mpc = get_move_piece(move);
          bool nonpawn_mover = (td.side == white) ? (mpc >= N && mpc <= Q)
                                                  : (mpc >= n && mpc <= q);
          if (nonpawn_mover) {
            U64 others = (td.side == white)
                             ? (td.bitboards[N] | td.bitboards[B] |
                                td.bitboards[R] | td.bitboards[Q])
                             : (td.bitboards[n] | td.bitboards[b] |
                                td.bitboards[r] | td.bitboards[q]);
            stalemate_resource = count_bits(others) == 1;
          }
        }
        if (!stalemate_resource) {
          if (is_quiet)
            quiets_searched++;
          // Phase-2: if this move came from MPS_BAD_TACTICAL it means every
          // subsequent bad capture has an equal-or-worse SEE (they are yielded
          // by descending score). The SEE threshold is the same for all, so we
          // can skip the entire remaining bad-capture stage.
          if (use_picker && is_capture && mp.stage == MPS_BAD_TACTICAL)
            mp.skip_bad_caps = true;
          continue;
        }
      }
    }

    // History pruning: a late quiet move whose combined (butterfly +
    // continuation) history is strongly negative has repeatedly failed in this
    // context -> skip it at low depth. Threshold scales with depth so we prune
    // more aggressively the shallower we are. NB: td.ply not yet incremented
    // here, so the previous move is td.move_stack[td.ply].
    if (g_cont_hist_prune && !pv_node && !in_check && is_quiet &&
        prune_depth <= g_conthist_prune_depth && moves_searched > 0 &&
        best_score > -mate_score) {
      int prev = td.move_stack[td.ply];
      int hh = td.history_moves[td_hbucket(td, move)][get_move_piece(move)]
                               [get_move_target(move)];
      if (prev)
        hh += td.continuation_history[get_move_piece(prev)][get_move_target(
            prev)][get_move_piece(move)][get_move_target(move)];
      if (hh < -g_histprune_margin * depth) {
        quiets_searched++;
        continue;
      }
    }

    // Singular extension: before searching the TT move, check whether it is
    // much better than every alternative. Re-search this position EXCLUDING
    // the TT move at reduced depth with a window just below tt_score; if all
    // other moves fail low, the TT move is "singular" and gets +1 ply.
    // g_singular_dmargin: how far below singular_beta the alternatives must
    // fall for a DOUBLE extension (bigger = rarer/safer). SPSA-tunable.
    int extension = 0;
    // F-018.6a SingularMinDepth (default 8 = storico; Obsidian 5, Berserk 6: a
    // 6 il blocco scatta ~2x piu' spesso). F-018.6c SingularPlyGuard:
    // anti-esplosione oltre ply >= 2*rootDepth (entrambi i rivali ce l'hanno).
    if (excluded_move == 0 && move == tt_move && depth >= g_singular_mindepth &&
        td.ply && (!g_singular_plyguard || td.ply < 2 * td.root_depth) &&
        tt_hit && tt_depth >= depth - g_singular_ttmargin &&
        tt_flag != hash_flag_alpha && tt_score > -mate_score &&
        tt_score < mate_score) {
      // F-002: era hardcoded 2*depth. SingularExactMargin: bound EXACT ->
      // margine dimezzato (score fidato -> piu' facile dichiarare singolare ->
      // piu' estensioni dove serve). Margine PIENO: resta l'ancora per
      // doppia/tripla estensione (vedi sotto).
      const int singular_margin_full = g_singular_mpd * depth;
      // ⭐ SingularExactMaxDepth (0 = nessun cap = byte-identico): il
      // dimezzamento su bound EXACT cambia NATURA con la profondita'. Albero vs
      // baseline (bench VM, 19/07):
      //   depth 13 -7.4% | depth 17 -11.1% | depth 20 **+42.7%**
      // Sotto ~18 stringe (ed e' li' che a 10+0.1 valeva +8.60); sopra esplode,
      // ed e' la spiegazione del -8.60 misurato a 20+0.2. Cap = tenere il
      // beneficio e buttare il danno, stesso schema del TC-gate di TMv2.
      const bool exact_halve = g_singular_exact_margin &&
                               tt_flag == hash_flag_exact &&
                               (g_singular_exact_maxdepth <= 0 ||
                                depth <= g_singular_exact_maxdepth);
      int singular_beta = tt_score - (exact_halve ? singular_margin_full / 2
                                                  : singular_margin_full);
      // ⚠️ SingularExactDecouple (2026-07-19, default OFF = byte-identico).
      // Il dimezzamento su bound EXACT alza singular_beta, e le soglie di
      // doppia/tripla estensione sono misurate DA singular_beta -> si alzano
      // anche loro. Effetto voluto: piu' facile essere SINGOLARE. Effetto NON
      // voluto: piu' facile essere DOPPIO/TRIPLO. E poiche' il margine e'
      // `mpd*depth`, lo scarto assoluto cresce con la profondita' (a d8 sono 4
      // unita', a d24 sono 12): a TC lungo la cascata di doppie/triple esplode.
      // Sospetto per l'inversione di segno misurata: +8.60 @10+0.1 -> -8.60
      // @20+0.2. Con questo ON, doppia/tripla restano ancorate al margine
      // PIENO.
      const int de_beta = g_singular_exact_decouple
                              ? (tt_score - singular_margin_full)
                              : singular_beta;
      int singular_depth = (depth - 1) / g_singular_depth_div;
      int s = td_negamax(td, singular_beta - 1, singular_beta, singular_depth,
                         is_cut_node, tt_move);
      // Audit SF#5612 (2026-07): ricerca di esclusione abortita -> s=0 garbage
      // deciderebbe estensioni/multicut (e con MulticutTTMalus on, un malus
      // history spurio). Stessa guardia post-figlio del move loop.
      if (stop_threads.load(std::memory_order_relaxed))
        return 0;
      if (s < singular_beta) {
        extension = 1;
        // Double extension: the TT move beats every alternative by a wide
        // margin (hyper-forced) -> extend 2 plies. Non-PV only, to bound the
        // search blow-up that uncapped double extensions can cause.
        // F-018.6b SingularDECap (0=off): limita la CATENA di double-extension
        // sul path (Berserk ss->de <= 6) — senza cap le catene lunghe esplodono
        // l'albero.
        if (g_singular_ext && !pv_node && s < de_beta - g_singular_dmargin &&
            (!g_singular_de_cap || td.de_stack[td.ply] <= g_singular_de_cap)) {
          extension = 2;
          // Triple extension (SF :1244): beats alternatives by an even wider
          // margin -> +3. tmargin > dmargin so this nests inside the double.
          if (g_triple_ext && s < de_beta - g_singular_tmargin)
            extension = 3;
        }
      }
      // Multi-cut (BAKED, default on): the exclusion search (TT move removed)
      // failed high and singular_beta itself is >= beta -> more than one move
      // beats beta -> this expected cut-node is not singular -> prune the whole
      // subtree by returning singular_beta. Conservative gate on the BOUND (not
      // the value): fires rarely, only on a very high TT score, which keeps it
      // safe and +Elo where the aggressive SF gate (s>=beta) over-pruned (-13
      // Elo). Non-PV only (unsound in PV). Safe to return: no move has been
      // made yet this iteration.
      else if (g_multicut && !pv_node && singular_beta >= beta) {
        // MulticutTTMalus: la ricerca di esclusione ha fallito alto SENZA la
        // ttMove -> la posizione vince comunque -> la ttMove non era la ragione
        // del vantaggio. Malus alla sua history (solo se quiet: le catture
        // hanno una tabella a parte).
        if (g_multicut_ttmalus && !get_move_capture(tt_move) &&
            !get_move_promoted(tt_move)) {
          int malus = td_stat_bonus(depth) * g_multicut_ttmalus_scale / 100;
          td_update_history(
              td.history_moves[td_hbucket(td, tt_move)][get_move_piece(tt_move)]
                              [get_move_target(tt_move)],
              -malus);
        }
        return singular_beta;
      }
      // Negative extensions as tunable amounts (0 = off). ttMove assumed to
      // fail high over beta -> shrink by g_negext_tt (default 1=legacy, SF=3);
      // else on a cut node -> shrink by g_negext_cut (default 0=off, SF=2).
      // Co-tunable -> SPSA can dial a harmful negative extension back to 0.
      else if (g_negext_tt > 0 && tt_score >= beta) {
        extension = -g_negext_tt;
      }
      // F-018.6d NegExtAlpha (0=off): la TT move non batte nemmeno alpha -> il
      // nodo non e' singolare ne' promettente, riduci l'estensione (Berserk:
      // -1).
      else if (g_negext_alpha > 0 && tt_score <= alpha) {
        extension = -g_negext_alpha;
      } else if (g_negext_cut > 0 && is_cut_node) {
        extension = -g_negext_cut;
      }
    }

    // Q-16 RecaptureExt (Caissa Search.cpp:1868, default OFF): ai nodi PV la
    // ttMove che RICATTURA sulla casa dell'ultima mossa avversaria e'
    // quasi-forzata -> +1 ply. Gate strettissimo (PV && ttMove && stessa casa)
    // = costo nodi minimo. Pre-make: l'ultima mossa avversaria e'
    // td.move_stack[td.ply] (vedi history pruning sopra). Filtro TENSIONE
    // (2026-07-12): estende solo se |SEE(ricattura)| <= soglia, cioe' lo
    // scambio e' ~pari. Uno scambio nettamente sbilanciato e' gia' ovvio (SEE
    // alto in un verso o nell'altro) -> l'estensione li' non compra nulla,
    // costa solo nodi.
    if (g_recapture_ext && extension == 0 && pv_node && move == tt_move &&
        is_capture) {
      int prev_rc = td.move_stack[td.ply];
      if (prev_rc && get_move_target(prev_rc) == get_move_target(move)) {
        int see = td_see(td, move);
        if (see >= -g_recapture_tension && see <= g_recapture_tension)
          extension = 1;
      }
    }

    // BrilliantSac detection (6.0, condivisa da v1-estensione e B-reduction-cap):
    // cattura PERDENTE (SEE<0) con capture_history sopra soglia = sacrificio
    // strutturale imparato. Calcolata una volta se un toggle e' attivo; vittima
    // con fallback al pedone avversario (en passant). is_brilliant e' letto anche
    // nel blocco LMR piu' sotto (stesso scope del move loop).
    bool is_brilliant = false;
    if ((g_brilliant_sac || g_brilliant_sac_lmr) && is_capture && !is_promotion &&
        !td_see_at_least(td, move, 0)) {
      int tgt = get_move_target(move);
      int s = (td.side == white) ? p : P;
      int e = (td.side == white) ? k : K;
      int vic = s;
      for (int pc = s; pc <= e; pc++)
        if (get_bit(td.bitboards[pc], tgt)) {
          vic = pc;
          break;
        }
      is_brilliant =
          td.capture_history[get_move_piece(move)][tgt][vic]
                            [td_cbucket(td, tgt)] >= g_brilliant_sac_margin;
    }
    // v1: estende (+1) il sac. NB: questo ramo tocca solo moves_searched==0 (sac
    // ordinato 1o = rarissimo); il caso VERO (sac in fondo) e' gestito da sac_ext
    // nel ramo moves_searched>0 (v1 FIX 2026-07-25). Qui resta per completezza.
    if (g_brilliant_sac && extension == 0 && is_brilliant)
      extension = 1;

    // FIX P0.3 (2026-06-09): catturare lo stato "killer" PRIMA di incrementare
    // td.ply. Il vecchio check nel blocco LMR (`move ==
    // killer_moves[..][td.ply]`) leggeva i killer del ply FIGLIO (mosse
    // dell'avversario: il campo piece differisce sempre) -> non era MAI vero ->
    // lo sconto LMR per i killer era dead code.
    bool is_killer = is_quiet && (move == td.killer_moves[0][td.ply] ||
                                  move == td.killer_moves[1][td.ply]);

    // ThreatHist: bucket-minaccia di QUESTA mossa nel contesto del nodo PADRE
    // (board corrente, PRE-make) -> usato dal read LMR (post-make) e messo
    // nello stack per seo/prior del figlio.
    int move_hbucket = td_hbucket(td, move);

    // Q-15 FollowPV: il figlio "segue la PV" solo se QUESTO nodo la segue e la
    // mossa e' esattamente quella della PV precedente a questo ply. Calcolato
    // PRE-incremento di td.ply (= ply del nodo corrente).
    bool child_follow = g_follow_pv && follow_pv && td.ply < td.prev_pv_len &&
                        move == td.prev_pv[td.ply];

    UndoInfo undo;
    td.seen_stack[td.ply] =
        moves_searched; // F-018.3d: mosse gia' viste a QUESTO nodo (il figlio
                        // le legge su TT-cut)
    td.ply++;
    td.reduction_stack[td.ply] =
        0; // HindsightExt: default 0 (figlio non ridotto); override sotto solo
           // per il reduced-search LMR
    td.de_stack[td.ply] =
        td.de_stack[td.ply - 1] +
        (extension >= 2 ? 1 : 0); // F-018.6b: catena double-ext sul path
    td.repetition_table[++td.repetition_index] = td.hash_key;

    if (!td_make_move(td, move, undo)) {
      td.ply--;
      td.repetition_index--;
      continue;
    }

    legal_moves++;
    if (is_quiet)
      quiets_searched++;

    // "info currmove" a root (convenzione SF: solo main thread, solo analisi
    // lunga > 3s): le GUI mostrano la mossa in esame. Gated dal tempo -> zero
    // costo in gioco/bench (a root il loop gira una manciata di volte).
    if (is_root_node && &td == &thread_data[0] &&
        get_time_ms() - search_start_time > 3000) {
      std::lock_guard<std::mutex> lock(output_mutex);
      printf("info depth %d currmove ", td.root_depth);
      print_move(move);
      printf(" currmovenumber %d\n", legal_moves);
      fflush(stdout);
    }

    // Does this move give check? After make_move the side to move is the
    // opponent, so their king being attacked means our move checks. Checking
    // moves must NOT be reduced/pruned, otherwise forced mates and tactics
    // are found far too late (Stockfish keeps checks at full depth).
    int stm_king_sq =
        get_ls1b_index((td.side == white) ? td.bitboards[K] : td.bitboards[k]);
    bool gives_check = td_is_square_attacked(td, stm_king_sq, td.side ^ 1);

    // Record the move that leads to the child node (for counter-move).
    td.move_stack[td.ply] = move;
    td.hbucket_stack[td.ply] =
        move_hbucket; // ThreatHist: bucket della mossa entrata in questo ply
                      // (per seo/prior)
    td.captured_stack[td.ply] =
        undo.captured_piece; // victim (-1 if quiet); V2 capture-hist

    // PVS + LMR. Q-11: `nodes_before` serve sia al NodeTM (root) sia al
    // NodeCache (ply<3), quindi lo catturiamo sempre — costa una lettura di
    // td.nodes.
    U64 nodes_before = td.nodes;
    int postlmr_bonus =
        0; // F-018.1: esito della re-search post-LMR (applicato dopo l'unmake)
    if (moves_searched == 0) {
      score = -td_negamax(td, -beta, -alpha, depth - 1 + extension, false, 0,
                          child_follow);
    } else {
      int reduction = 0;

      // F-005: gate LMR promossi a tunable (erano hardcoded 3/3; SF riduce da
      // depth>=2, 2a mossa).
      if (depth >= g_lmr_min_depth && moves_searched >= g_lmr_min_moves &&
          is_quiet && (!in_check || g_lmr_check_exempt >= 2) &&
          (!gives_check || g_lmr_check_exempt >= 1)) {
        int d_idx = (depth < 63) ? depth : 63;
        int m_idx = (moves_searched < 63) ? moves_searched : 63;
        if (!g_lmr_fine) {
          reduction =
              lmr_table[d_idx]
                       [m_idx]; // assegna alla variabile ESTERNA (niente 'int'
                                // -> niente shadowing che disattivava la LMR)

          if (!pv_node)
            reduction++;
          // ttPv: i nodi non-PV che la TT ricorda come ex-PV vengono ridotti
          // meno (i veri PV node hanno già lo sconto via il !pv_node qui
          // sopra). È il segnale nuovo dello Step ttPv di SF. OFF
          // (g_ttpv=false) = nessun effetto.
          if (g_ttpv_amount > 0 && tt_hit && tt_pv && !pv_node)
            reduction -= g_ttpv_amount;
          if (g_killer_lmr_fix &&
              is_killer) // FIX P0.3: killer del ply del NODO (vedi sopra)
            reduction--;
          if (eval + g_lmr_eval_margin < alpha)
            reduction++;
          if (is_cut_node)
            reduction++;
          // CutNodeLMR (default OFF): SF riduce i cut-node piu' di noi.
          // Aggiunge g_cutnode_lmr_extra ply SOPRA il +1 qui sopra. OFF =
          // byte-identico.
          if (g_cutnode_lmr && is_cut_node)
            reduction += g_cutnode_lmr_extra;
          // LMREnrich (archivio 4.2, default OFF): TT-move noisy + mossa
          // corrente quiet
          // -> riduci di piu' (SF ttCapture). OFF = byte-identico.
          if (g_lmr_enrich && tt_move &&
              (get_move_capture(tt_move) || get_move_enpassant(tt_move) ||
               get_move_promoted(tt_move)))
            reduction += g_lmr_enrich_amount;
          // Enrichment (neutral at default 0): a deep TT entry for this node
          // means the ordering is trustworthy -> reduce a bit less.
          if (g_lmr_ttdepth && tt_hit && tt_depth >= depth)
            reduction -= g_lmr_ttdepth;
          // Reduce one extra ply when the position is NOT improving: a
          // flat/falling eval makes late quiets less likely to be the move.
          if (g_improving && !improving)
            reduction++;

          // History-based reduction. DEFAULT (StatScoreLMR off): butterfly
          // history clamped to +/-1 ply (conservativo). StatScoreLMR on: stile
          // SF, riduzione CONTINUA (niente clamp) -> quiet con ottima history
          // ridotti molti ply in piu', quelli con pessima history molti in meno
          // = il fix della SOTTO-RIDUZIONE (gap vs SF15.1). Divisore
          // SPSA-tunable.
          int raw_hist = td.history_moves[move_hbucket][get_move_piece(move)]
                                         [get_move_target(move)];
          if (g_statscore_lmr) {
            int ss = 2 * raw_hist - g_lmr_ss_offset;
            reduction -= ss / g_lmr_ss_div;
          } else {
            int hist_r, hclamp;
            if (g_aggr_lmr) {
              hist_r = raw_hist / g_aggr_lmr_div;
              hclamp = g_aggr_lmr_clamp;
            } else {
              hist_r = raw_hist / g_hist_red_div;
              hclamp = 1;
            }
            if (hist_r > hclamp)
              hist_r = hclamp;
            else if (hist_r < -hclamp)
              hist_r = -hclamp;
            reduction -= hist_r;
          }

          // Continuation-history reduction. ContHistLMR on: conthist CONTINUO a
          // 1/2/4 ply indietro (scollegato da ContHistPrune) = il segnale che
          // SF usa pesantemente nella LMR e noi ignoravamo di default.
          // Altrimenti il legacy +/-1 ch_r (solo se ContHistPrune on). NB:
          // td.ply gia' incrementato per il figlio -> mossa precedente a
          // move_stack[td.ply-1].
          if (g_conthist_lmr) {
            int prev = td.move_stack[td.ply - 1];
            int ch = 0;
            if (prev)
              ch +=
                  td.continuation_history[get_move_piece(prev)][get_move_target(
                      prev)][get_move_piece(move)][get_move_target(move)];
            if (td.ply >= 2) {
              int p2 = td.move_stack[td.ply - 2];
              if (p2)
                ch +=
                    td.cont_hist_2[get_move_piece(p2)][get_move_target(p2)]
                                  [get_move_piece(move)][get_move_target(move)];
            }
            if (td.ply >= 4) {
              int p4 = td.move_stack[td.ply - 4];
              if (p4)
                ch +=
                    td.cont_hist_4[get_move_piece(p4)][get_move_target(p4)]
                                  [get_move_piece(move)][get_move_target(move)];
            }
            reduction -= ch / g_lmr_ch_div;
          } else if (g_cont_hist_prune) {
            int prev = td.move_stack[td.ply - 1];
            if (prev) {
              int ch =
                  td.continuation_history[get_move_piece(prev)][get_move_target(
                      prev)][get_move_piece(move)][get_move_target(move)];
              int ch_r, cclamp;
              if (g_aggr_lmr) {
                ch_r = ch / g_aggr_lmr_div;
                cclamp = g_aggr_lmr_clamp;
              } else {
                ch_r = ch / g_conthist_red_div;
                cclamp = 1;
              }
              if (ch_r > cclamp)
                ch_r = cclamp;
              else if (ch_r < -cclamp)
                ch_r = -cclamp;
              reduction -= ch_r;
            }
          }

          // DiverseSMP: nudge this helper's reduction to diversify its tree.
          // The clamps below keep it in [0, depth-2], so the bias can't break
          // LMR.
          if (g_diverse_smp)
            reduction += td.lmr_bias;

          // cutoffCnt-LMR (SF, default off): qui td.ply == ply del FIGLIO (post
          // ply++), quindi td.cutoff_cnt[td.ply] e' il (ss+1)->cutoffCnt di SF.
          // Se il figlio ha cuttato molto di recente, riduci di piu'.
          if (g_cutoffcnt_penalty && td.cutoff_cnt[td.ply] > 3)
            reduction += g_cutoffcnt_penalty;

          // CorrValExt: eval pesantemente corretta = incerta -> riduci di MENO
          // (come SF/Reckless).
          if (g_corrval_ext)
            reduction -=
                (corr_val < 0 ? -corr_val : corr_val) * g_corrval_lmr / 4096;
        } else {
          // ⭐ Riduzione FINE stile-SF in 1/1024 di ply (port
          // search.cpp:1287-1334): blenda i segnali con peso fine e taglia
          // FORTE solo dove e' sicuro. r in millesimi di ply.
          long long r = (long long)lmr_table[d_idx][m_idx] *
                        1024; // base dalla nostra tabella
          if (tt_pv && !pv_node)
            r -= g_lmrf_ttpv; // protezione ex-PV
          if (pv_node)
            r -= g_lmrf_pv; // protezione PV
          r -= (long long)(corr_val < 0 ? -corr_val : corr_val) * g_lmrf_corr /
               4096; // eval incerta -> meno
          if (is_cut_node)
            r += g_lmrf_cut + (tt_move == 0
                                   ? g_lmrf_cut_nott
                                   : 0); // CUT-NODE forte (il divario #1)
          if (tt_move &&
              (get_move_capture(tt_move) || get_move_enpassant(tt_move) ||
               get_move_promoted(tt_move)))
            r += g_lmrf_ttcap;
          // statScore continuo (2*main + cont1 + cont2), come SF
          int sscore = 2 * td.history_moves[move_hbucket][get_move_piece(move)]
                                           [get_move_target(move)];
          {
            int prv = td.move_stack[td.ply - 1];
            if (prv)
              sscore +=
                  td.continuation_history[get_move_piece(prv)][get_move_target(
                      prv)][get_move_piece(move)][get_move_target(move)];
            if (td.ply >= 2) {
              int p2 = td.move_stack[td.ply - 2];
              if (p2)
                sscore +=
                    td.cont_hist_2[get_move_piece(p2)][get_move_target(p2)]
                                  [get_move_piece(move)][get_move_target(move)];
            }
          }
          r -= (long long)sscore * g_lmrf_ss / 4096;
          // ContHist4LMR: la conthist a 4 ply, che il ramo legacy usava e il
          // port di LMRFine ha lasciato fuori (vedi il commento alla
          // dichiarazione). Termine a se' stante: 0 = byte-identico. Stessa
          // convenzione di indice del resto del blocco (td.ply e' gia'
          // incrementato per il figlio).
          if (g_lmrf_cont4 && td.ply >= 4) {
            int p4 = td.move_stack[td.ply - 4];
            if (p4)
              r -=
                  (long long)td.cont_hist_4[get_move_piece(p4)][get_move_target(
                      p4)][get_move_piece(move)][get_move_target(move)] *
                  g_lmrf_cont4 / 4096;
          }
          if (g_improving && !improving)
            r += g_lmrf_improv;
          if (eval + g_lmr_eval_margin < alpha)
            r += g_lmrf_evalcut;
          if (td.cutoff_cnt[td.ply] > 1) {
            r += g_lmrf_cutoff;
            // LMRExpect (Reckless 0efe38e, STC +3.08 / LTC +4.13): "aspettativa
            // del nodo RISPETTATA". A un ALL-node (ne' PV ne' cut) ci si
            // aspetta che tutto fallisca basso; se per giunta il figlio ha
            // cuttato molto, l'aspettativa e' confermata
            // -> si puo' ridurre ancora di piu'. E' l'INTERAZIONE fra i due
            // segnali che gia' abbiamo separati (g_lmrf_cutoff e g_lmrf_all),
            // mai combinati. Stesse unita' di Reckless (1/1024 di ply): loro
            // +256/+384. 0 = byte-identico.
            if (g_lmr_expect && !pv_node && !is_cut_node)
              r += g_lmr_expect;
          }
          // scaling ALL-node (ne' PV ne' cut): taglia di piu' dove tutto
          // fallisce-basso
          if (!pv_node && !is_cut_node)
            r += r * g_lmrf_all / (256LL * depth + 285);
          // Q-19a (Caissa): termine di PLY — riduci MENO vicino alla radice.
          // LMRFine non aveva NESSUN termine di ply. 0 = off.  r -=
          // ply_coef*depth/(1+ply+depth)
          if (g_lmrf_ply && pv_node)
            r -= (long long)g_lmrf_ply * depth / (1 + td.ply + depth);
          // Q-19b (Caissa): killer/counter ridotte sensibilmente meno (in
          // millesimi, additivo all'intero `reduction--` di KillerLMRFix
          // sotto). 0 = off.
          if (g_lmrf_killer && is_killer)
            r -= g_lmrf_killer;
          if (g_diverse_smp)
            r += (long long)
                     td.lmr_bias_units; // = lmr_bias*1024 se DiverseSMPFine=0
          reduction = (int)(r / 1024);
          // protezioni intere mantenute (piccole)
          if (g_killer_lmr_fix && is_killer)
            reduction--;
          if (g_lmr_ttdepth && tt_hit && tt_depth >= depth)
            reduction -= g_lmr_ttdepth;
        }

        // Q-30 (Reckless ae986df, default OFF): beta gia' decisivo (stiamo
        // provando un matto) -> riduci di piu' le tardive. Non-regressione +
        // matefinding migliore per loro (+28 matti su matetrack). Vale per
        // entrambi i rami.
        if (g_lmr_decisive_beta && beta >= mate_score)
          reduction++;

        if (reduction < 0)
          reduction = 0;
        if (reduction > depth - 2)
          reduction = depth - 2;
      }
      // F-004 (audit 2026-07-02): LMR anche sulle CATTURE (SF/Stormphrax
      // riducono le noisy tardive; noi mai = gap strutturale). Riduzione dalla
      // stessa lmr_table scalata % (default 50 = dimezzata), MAI su
      // promozioni/scacchi, con esenzione RICATTURA sulla casa appena mossa
      // dall'avversario (stessa esenzione della QFutility: potare/ridurre il
      // recupero = perdere la valutazione dello scambio). Default OFF =
      // byte-identico.
      else if (g_lmr_captures && is_capture && !is_promotion &&
               (!in_check || g_lmr_check_exempt >= 2) &&
               (!gives_check || g_lmr_check_exempt >= 1) &&
               depth >= g_lmr_min_depth &&
               moves_searched >= g_lmr_min_moves) {
        int prev_mv = td.move_stack[td.ply - 1];
        int prev_sq = prev_mv ? get_move_target(prev_mv) : -1;
        if (get_move_target(move) != prev_sq) {
          int d_idx = (depth < 63) ? depth : 63;
          int m_idx = (moves_searched < 63) ? moves_searched : 63;
          reduction = lmr_table[d_idx][m_idx] * g_lmr_cap_scale / 100;
          if (!pv_node)
            reduction++;
          if (reduction < 0)
            reduction = 0;
          if (reduction > depth - 2)
            reduction = depth - 2;
        }
      }

      // BrilliantSac-B (reduction-cap, default OFF): il sac strutturale ordinato
      // tardi qui verrebbe ridotto -> risparmia N plies di riduzione (reduce-less),
      // dove v1 (estensione della 1a mossa) non arriva. is_brilliant true solo per
      // catture -> il path quiet non e' toccato.
      if (g_brilliant_sac_lmr && is_brilliant && reduction > 0) {
        reduction -= g_brilliant_sac_lmr_amt;
        if (reduction < 0)
          reduction = 0;
      }

      // v1 FIX (2026-07-25): un sac (SEE<0) e' ordinato in fondo -> NON e' mai
      // moves_searched==0, quindi l'`extension=1` di v1 (consumata solo nel ramo
      // della 1a mossa) era codice MORTO sul suo bersaglio. Qui, nel ramo
      // moves_searched>0 dove il sac arriva davvero, applichiamo il +1 alle
      // profondita' LMR/re-search: ora v1 estende sul serio. Default OFF -> 0.
      int sac_ext = (g_brilliant_sac && is_brilliant) ? 1 : 0;
      // Reduced-depth zero-window search (LMR). reduced_depth = depth-1-r.
      int reduced_depth = depth - 1 - reduction + sac_ext;
      td.reduction_stack[td.ply] =
          reduction; // HindsightExt: il figlio sa di essere stato ridotto di
                     // `reduction` plies
      score = -td_negamax(td, -alpha - 1, -alpha, reduced_depth, true, 0,
                          child_follow);

      // Full-depth re-search when the reduced search fails high.
      int full_depth = depth - 1 + sac_ext;
      if (score > alpha && reduction > 0) {
        td.reduction_stack[td.ply] =
            0; // HindsightExt: la re-search e' a profondita' PIENA ->
               // reduction=0 per il figlio
        // DoDeeper/DoShallower (SF :doDeeperSearch): se la re-search batte il
        // best di molto cerca piu' a fondo (+1), se lo batte di poco piu' a
        // corto (-1). best_score qui = best delle mosse PRECEDENTI (running
        // best, come SF bestValue).
        if (g_do_deeper) {
          bool do_deeper =
              score > best_score + g_dodeeper_base + 2 * full_depth;
          bool do_shallower = score < best_score + g_doshallower_margin;
          full_depth += (int)do_deeper - (int)do_shallower;
        }
        if (full_depth > reduced_depth) {
          score = -td_negamax(td, -alpha - 1, -alpha, full_depth, !is_cut_node,
                              0, child_follow);
          // F-018.1 PostLMRHist (default OFF): la re-search full-depth VERIFICA
          // la riduzione — se conferma il fail-high la mossa merita bonus
          // conthist, se smentisce merita malus (mainline SF, presente in
          // Obsidian e Berserk).
          if (g_postlmr_hist)
            postlmr_bonus = (score <= alpha  ? -td_stat_bonus(full_depth)
                             : score >= beta ? td_stat_bonus(full_depth)
                                             : 0) *
                            g_postlmr_scale / 100;
        }
      }

      // PV full-window re-search (uses the possibly-adjusted full_depth so a
      // doDeeper bump also deepens the PV confirmation, matching SF).
      if (score > alpha && score < beta) {
        score =
            -td_negamax(td, -beta, -alpha, full_depth, false, 0, child_follow);
      }
    }

    td_unmake_move(td, move, undo);
    td.ply--;
    td.repetition_index--;

    if (stop_threads.load(std::memory_order_relaxed))
      return 0;

    // Q-11 NodeCache (Caissa Search.cpp:2024-2029): registra i nodi spesi sotto
    // questa mossa. Dopo lo stop-check: una ricerca abortita darebbe un
    // conteggio troncato che inquinerebbe l'ordering delle iterazioni
    // successive.
    if (nc)
      nc_add(nc, move, td.nodes - nodes_before);

    // F-018.1 PostLMRHist: update conthist DOPO l'unmake (convenzioni ply del
    // nodo, come il blocco fail-high sotto). Su ogni tipo di mossa, come SF.
    if (postlmr_bonus) {
      int plh_prev = td.move_stack[td.ply];
      if (plh_prev)
        td_update_history(
            td.continuation_history[get_move_piece(plh_prev)][get_move_target(
                plh_prev)][get_move_piece(move)][get_move_target(move)],
            postlmr_bonus);
      td_conthist_multi_update(td, move, postlmr_bonus);
    }

    moves_searched++;
    if (is_quiet && n_searched_quiets < 64)
      searched_quiets[n_searched_quiets++] = move;
    else if (is_capture && n_searched_captures < 64)
      searched_captures[n_searched_captures++] = move;

    if (score > best_score) {
      best_score = score;
      best_move = move;
      // Node-based TM: record how many nodes this (new best) root move cost.
      if (is_root_node)
        td.root_bestmove_nodes = td.nodes - nodes_before;

      if (score > alpha) {
        alpha = score;
        hash_flag = hash_flag_exact;

        td.pv_table[td.ply][td.ply] = move;
        for (int i = td.ply + 1; i < td.pv_length[td.ply + 1]; i++)
          td.pv_table[td.ply][i] = td.pv_table[td.ply + 1][i];
        td.pv_length[td.ply] = td.pv_length[td.ply + 1];

        // F-018.9 AlphaDepthDec (default OFF): trovato un best "vero" (alpha
        // alzato, niente cutoff), le mosse RESTANTI meritano meno sforzo ->
        // depth-1 per il resto del loop (Berserk/SF; il TT store finale usa la
        // depth ridotta).
        if (g_alpha_depth_dec && score < beta && score > -mate_score &&
            score < mate_score && depth >= 2 && depth <= 11)
          depth--;

        if (score >= beta) {
          // CutoffStats (diagnostica, off-default): fail-high node.
          // moves_searched e' GIA' incrementato (riga sopra) -> cutoff sulla 1a
          // mossa <=> ==1.
          if (g_cutoff_stats) {
            td.fh_nodes++;
            td.fh_move_sum += (U64)moves_searched;
            if (moves_searched == 1)
              td.fh_first++;
            if (tt_move) {
              td.fh_tt++;
              if (moves_searched == 1)
                td.fh_tt_first++;
            }
            if (tt_hit)
              td.fh_probe++; // entry TT presente (anche senza mossa)
          }
          td.cutoff_cnt[td.ply] +=
              tt_move ? 1 : 2; // SF cutoffCnt: += 1 + !ttMove
          if (!excluded_move)
            store_tt(td.hash_key, move, best_score, depth, hash_flag_beta,
                     td.ply, store_pv, tt_eval_undamp(node_raw_eval, td.fifty));
          td_corr_update(td, corr_idx, static_eval, best_score, hash_flag_beta,
                         depth, in_check, move, excluded_move);

          if (is_quiet) {
            // Evita di clonare la stessa mossa in entrambi gli slot
            // killer (dimezzerebbe le chance di cutoff).
            if (move != td.killer_moves[0][td.ply]) {
              td.killer_moves[1][td.ply] = td.killer_moves[0][td.ply];
              td.killer_moves[0][td.ply] = move;
            }
            int prev_cm = td.move_stack[td.ply];
            int pcp = prev_cm ? get_move_piece(prev_cm) : 0;
            int pct = prev_cm ? get_move_target(prev_cm) : 0;
            if (g_countermove && prev_cm)
              td.counter_moves[pcp][pct] = move;
            // F-018.10a FHBoostMargin (0=off): fail-high LARGO = segnale piu'
            // forte
            // -> bonus/malus calcolati a depth+1 (Obsidian +95, Berserk +75).
            int bonus =
                td_stat_bonus(depth + ((g_fhboost_margin &&
                                        best_score > beta + g_fhboost_margin)
                                           ? 1
                                           : 0));
            // Q-18 (SF PR #6871, gainer): scala il bonus sul numero di mosse
            // cercate PRIMA della best — un cutoff tardivo e' un segnale piu'
            // informativo di uno alla prima mossa. 0 = off (bonus piatto,
            // comportamento storico).
            bonus = td_hist_bonus_moves(bonus, moves_searched);
            // PawnHistory: indice dal board corrente (= board del nodo qui,
            // la cutoff-move e' gia' stata unmade -> corretto). Locale = no
            // staleness da ricorsione.
            int pawn_pk = g_pawn_hist
                              ? (td_corr_index(td) & ThreadData::PAWN_HIST_MASK)
                              : 0;
            // F-018.10b HistTrivGuard (default OFF): cutoff "gratis" (depth
            // bassa, nessun quiet provato prima) -> niente bonus: gonfierebbe
            // mosse che hanno tagliato senza concorrenza
            // (Obsidian/Berserk/Ethereal).
            bool triv_cut =
                g_hist_triv_guard && depth <= 3 && n_searched_quiets <= 1;
            if (!triv_cut) {
              td_update_history(td.history_moves[move_hbucket][get_move_piece(
                                    move)][get_move_target(move)],
                                bonus);
              // Q-12 CMHC: il bonus CONTHIST (non la main-history) e' scalato
              // per la consistenza della mossa nei contesti conthist. 0 =
              // neutro.
              int chbonus = bonus * td_cmhc_factor(td, move) / 100;
              if (prev_cm)
                td_update_history(
                    td.continuation_history[pcp][pct][get_move_piece(move)]
                                           [get_move_target(move)],
                    chbonus);
              td_conthist_multi_update(td, move, chbonus);
              if (g_pawn_hist)
                td_update_history(td.pawn_history[pawn_pk][get_move_piece(move)]
                                                 [get_move_target(move)],
                                  bonus);
              if (g_lowply && td.ply < ThreadData::LOW_PLY_MAX)
                td_update_history(td.lowply_history[td.ply][get_move_piece(
                                      move)][get_move_target(move)],
                                  bonus);
            }
            // malus: quiet moves tried before the cutoff get penalized
            for (int q = 0; q < n_searched_quiets; q++) {
              int qm = searched_quiets[q];
              if (qm == move)
                continue;
              // MalusScaled (default OFF -> qmalus=bonus=piatto): le quiet
              // TARDIVE (q grande) ricevono malus MINORE (come Reckless): denom
              // = 1024 + coef*q. F-018.10c MalusPct (100=identico al bonus):
              // costanti malus separate.
              int qmalus;
              if (g_malus_quad) { // caduta quadratica (Reckless 5e669a6)
                int denom = 1024 + g_malus_quad_coef * q;
                qmalus = bonus * 1024 / denom;
                qmalus = qmalus * 1024 /
                         denom; // in due passi: bonus*1024*1024 sfonda int32
              } else
                qmalus = g_malus_scaled
                             ? bonus * 1024 / (1024 + g_malus_scale_coef * q)
                             : bonus;
              qmalus = qmalus * g_malus_pct / 100;
              td_update_history(
                  td.history_moves[td_hbucket(td, qm)][get_move_piece(qm)]
                                  [get_move_target(qm)],
                  -qmalus);
              if (prev_cm)
                td_update_history(
                    td.continuation_history[pcp][pct][get_move_piece(qm)]
                                           [get_move_target(qm)],
                    -qmalus);
              td_conthist_multi_update(td, qm, -qmalus);
              if (g_pawn_hist)
                td_update_history(td.pawn_history[pawn_pk][get_move_piece(qm)]
                                                 [get_move_target(qm)],
                                  -qmalus);
              if (g_lowply && td.ply < ThreadData::LOW_PLY_MAX)
                td_update_history(td.lowply_history[td.ply][get_move_piece(qm)]
                                                   [get_move_target(qm)],
                                  -qmalus);
            }
          } else if (is_capture && g_capture_hist) {
            // Capture history: BONUS alla cattura che ha causato il cutoff.
            int bonus = td_hist_bonus_moves(td_stat_bonus(depth),
                                            moves_searched); // Q-18
            int vic = td_captured_piece(td, get_move_target(move));
            td_update_history(
                td.capture_history[get_move_piece(move)][get_move_target(move)]
                                  [vic][td_cbucket(td, get_move_target(move))],
                bonus);
          }

          // SF-faithful: il MALUS alle catture provate-e-fallite si applica
          // a OGNI beta-cutoff (anche quando la best move e' QUIET), non solo
          // quando il cutoff e' causato da una cattura. Senza questo, le
          // catture speculative che falliscono prima di un cutoff quiet non
          // accumulano mai segnale negativo -> capture_history resta
          // netto-positiva -> vengono sovra-ordinate -> -Elo. (Era il bug che
          // rendeva la feature negativa: -21 @div1, -12 @div16.)
          if (g_capture_hist) {
            int cap_malus =
                td_stat_bonus(depth) * g_malus_pct / 100; // F-018.10c
            for (int c = 0; c < n_searched_captures; c++) {
              int cm = searched_captures[c];
              if (cm == move)
                continue; // la cattura-cutoff ha gia' il bonus
              int cvic = td_captured_piece(td, get_move_target(cm));
              td_update_history(
                  td.capture_history[get_move_piece(cm)][get_move_target(cm)]
                                    [cvic][td_cbucket(td, get_move_target(cm))],
                  -cap_malus);
            }
          }

          return best_score;
        }
      }
    }
  }

  // Checkmate / Stalemate
  if (legal_moves == 0) {
    if (in_check)
      return -mate_value + td.ply;
    return 0;
  }

  // V2 (PriorBonus, default OFF -> byte-identico): nodo FAIL-LOW (hash_flag
  // ancora alpha = nessuna mossa ha battuto alpha) -> la mossa PRECEDENTE
  // (quella che ci ha portato qui) era forte -> bonus alla sua history. Quiet:
  // main + 1-ply continuation; cattura: capture-hist (vittima da
  // captured_stack, mv1 e' gia' stata fatta = via dal board).
  if (g_prior_bonus && hash_flag == hash_flag_alpha &&
      (!g_prior_bonus_gate || is_cut_node || pv_node)) {
    int mv1 = td.move_stack[td.ply];
    if (mv1) {
      int p1 = get_move_piece(mv1), t1 = get_move_target(mv1);
      int pbonus = g_prior_bonus_scale * td_stat_bonus(depth) / 100;
      if (g_prior_bonus_factor && td.ply >= 1) {
        int mc = td.seen_stack[td.ply - 1];
        int mc_term = g_pb_mc * mc;
        if (mc_term > g_pb_mc_cap)
          mc_term = g_pb_mc_cap;
        int pev = td.eval_stack[td.ply - 1];
        int factor =
            g_pb_base + mc_term +
            (mv1 == td.ttmove_stack[td.ply - 1] ? g_pb_ttm : 0) +
            (!in_check && best_score <= static_eval - g_pb_ev_marg ? g_pb_ev
                                                                   : 0) +
            (pev != EVAL_NONE && best_score <= -pev - g_pb_swing_marg
                 ? g_pb_swing
                 : 0);
        pbonus = factor * g_pb_scale * td_stat_bonus(depth) / (100 * 128);
      }
      if (!get_move_capture(mv1)) {
        td_update_history(td.history_moves[td.hbucket_stack[td.ply]][p1][t1],
                          pbonus);
        if (td.ply >= 1) {
          int mv2 = td.move_stack[td.ply - 1];
          if (mv2)
            td_update_history(td.continuation_history[get_move_piece(
                                  mv2)][get_move_target(mv2)][p1][t1],
                              pbonus);
        }
      } else if (g_capture_hist) {
        int vic = td.captured_stack[td.ply];
        // CapHistThreat: bucket 0 FISSO qui. Questo e' il PriorBonus, che premia
        // la cattura del nodo PADRE: le minacce andrebbero lette sulla board del
        // padre, non su questa. ponytail: se la feature paga, la strada e' un
        // cbucket_stack come l'hbucket_stack che il lato quiet usa per lo stesso
        // motivo — non vale scriverlo prima di sapere se serve.
        if (vic >= 0)
          td_update_history(td.capture_history[p1][t1][vic][0], pbonus);
      }
    }
  }

  td_corr_update(td, corr_idx, static_eval, best_score, hash_flag, depth,
                 in_check, best_move, excluded_move);
  if (!excluded_move)
    store_tt(td.hash_key, best_move, best_score, depth, hash_flag, td.ply,
             store_pv, tt_eval_undamp(node_raw_eval, td.fifty));

  return best_score;
}

// ============================================================================
// UCI INFO
// ============================================================================

// multipv: 0 = gioco normale (token omesso, output storico invariato);
// k>=1 = analisi MultiPV, emette " multipv k" dopo il seldepth (ordine UCI
// standard). pv_override: stampa questa PV invece di td.pv_table[0] (serve al
// sort MultiPV, dove le linee vengono raccolte e stampate DOPO, in ordine di
// score).
static void print_search_info(ThreadData &td, int depth, int score,
                              int multipv = 0, const int *pv_override = nullptr,
                              int pv_len_override = -1) {
  int elapsed = get_time_ms() - search_start_time;
  if (elapsed == 0)
    elapsed = 1;

  char mpvtok[24] = "";
  if (multipv > 0)
    snprintf(mpvtok, sizeof(mpvtok), " multipv %d", multipv);

  // hashfull: sempre (economico, campiona 1000 slot). WDL: solo se UCI_ShowWDL.
  // Token pronti da inserire nell'info-line (ordine UCI: score .. wdl .. tbhits
  // .. hashfull .. pv).
  char hftok[24];
  snprintf(hftok, sizeof(hftok), " hashfull %d", hashfull());

  char wdltok[48] = "";
  if (g_show_wdl) {
    int W, D, L;
    if (score > mate_score && score < mate_value) {
      W = 1000;
      D = 0;
      L = 0;
    } // matto nostro
    else if (score > -mate_value && score < -mate_score) {
      W = 0;
      D = 0;
      L = 1000;
    } // veniamo mattati
    else {
      int es = nn_get_eval_scale();
      double cpn = (es == 100) ? (double)score : (double)score * 100.0 / es;
      cpn = cpn * 100.0 /
            392.0; // stessa normalizzazione del display cp (NORM_CP)
      const double b = 100.0;
      double w = 1.0 / (1.0 + exp(-(cpn - 100.0) / b));
      double l = 1.0 / (1.0 + exp(-(-cpn - 100.0) / b));
      W = (int)(w * 1000.0 + 0.5);
      L = (int)(l * 1000.0 + 0.5);
      D = 1000 - W - L;
      if (D < 0) {
        int s = W + L;
        W = W * 1000 / s;
        L = 1000 - W;
        D = 0;
      }
    }
    snprintf(wdltok, sizeof(wdltok), " wdl %d %d %d", W, D, L);
  }

  U64 total = 0, tbhits = 0;
  for (int i = 0; i < num_threads; i++) {
    total += thread_data[i].nodes;
    tbhits += thread_data[i].tb_hits;
  }

  U64 nps = (total * 1000) / elapsed;

  std::lock_guard<std::mutex> lock(output_mutex);

  if (score > -mate_value && score < -mate_score) {
    // F-014 (2026-07-03): tolto il "-1" (off-by-one ereditato da BBC): mattati
    // in N semimosse (score = -mate_value + N, N pari) -> UCI "mate -(N/2)",
    // non -(N/2)-1.
    printf("info depth %d seldepth %d%s score mate %d%s nodes %llu nps %llu "
           "time %d tbhits %llu%s pv ",
           depth, td.seldepth, mpvtok, -(score + mate_value) / 2, wdltok, total,
           nps, elapsed, tbhits, hftok);
  } else if (score > mate_score && score < mate_value) {
    printf("info depth %d seldepth %d%s score mate %d%s nodes %llu nps %llu "
           "time %d tbhits %llu%s pv ",
           depth, td.seldepth, mpvtok, (mate_value - score) / 2 + 1, wdltok,
           total, nps, elapsed, tbhits, hftok);
  } else {
    // Normalizza 'score cp' alla scala-pedone (SF-style): la search lavora sul
    // valore COMPRESSO da EvalScale, ma il display va riportato alla scala vera
    // -> cp = score*100/EvalScale. No-op a EvalScale=100 (display 5.0
    // invariato); a 56 ri-gonfia il numero deflazionato.
    int es = nn_get_eval_scale();
    int cp = (es == 100) ? score : (int)((long long)score * 100 / es);
    // [5.1 release 2026-07-03] Normalizzazione WDL del DISPLAY (SF
    // NormalizeToPawnValue-style): la scala intrinseca di rubicon-alea e' ~4x
    // gonfiata rispetto alla convenzione "100cp = 50% probabilita' di
    // vittoria". NORM_CP misurato empiricamente con fit logistico su 191.514
    // posizioni (1953 partite 5.1, TC 6-60s): P(win)=50% a +392 displayed
    // (win% 51.3 nel bucket 342-442). SOLO display: search/TM/TT intatti.
    constexpr int NORM_CP = 392;
    cp = (int)((long long)cp * 100 / NORM_CP);
    printf("info depth %d seldepth %d%s score cp %d%s nodes %llu nps %llu time "
           "%d tbhits %llu%s pv ",
           depth, td.seldepth, mpvtok, cp, wdltok, total, nps, elapsed, tbhits,
           hftok);
  }

  const int *pv = pv_override ? pv_override : td.pv_table[0];
  int pvlen = pv_override ? pv_len_override : td.pv_length[0];
  for (int i = 0; i < pvlen; i++) {
    print_move(pv[i]);
    printf(" ");
  }
  printf("\n");
  fflush(stdout);
}

// ============================================================================
// ITERATIVE DEEPENING
// ============================================================================

// Lazy SMP per-thread depth-skip pattern (Stockfish-style). Helper threads skip
// some iterations so they spend their time at different depths than the main
// thread, filling the shared TT from varied depths instead of duplicating work.
static const int LSMP_SkipSize[8] = {1, 1, 2, 2, 2, 3, 3, 3};
static const int LSMP_SkipPhase[8] = {0, 1, 0, 1, 2, 0, 1, 2};

static void thread_search(int thread_id, int max_depth) {
  ThreadData &td = thread_data[thread_id];

  copy_board_to_thread(td);

  memset(td.killer_moves, 0, sizeof(td.killer_moves));
  // RIMUOVE L'AMNESIA: la history persiste tra le mosse della partita, cosi'
  // l'ordinamento e' intelligente fin dal primo ms (la gravity in
  // td_update_history evita la saturazione).
  // memset(td.history_moves, 0, sizeof(td.history_moves));
  memset(td.counter_moves, 0, sizeof(td.counter_moves));
  // memset(td.continuation_history, 0, sizeof(td.continuation_history));
  memset(td.pv_table, 0, sizeof(td.pv_table));
  memset(td.pv_length, 0, sizeof(td.pv_length));
  // #5 low-ply history e' ply-indexed near-root -> NON deve persistere tra
  // mosse (mischierebbe posizioni diverse allo stesso ply). Azzerata per-search
  // se attiva (gated: OFF = nessun lavoro extra = byte-identico).
  if (g_lowply)
    memset(td.lowply_history, 0, sizeof(td.lowply_history));
  td.move_stack[0] = 0; // root has no previous move

  int prev_score = 0;
  int prev_best_move = 0;
  int prev_completed_score =
      0;                    // last completed iteration's score (TimeMgmt drop)
  int avg_score = infinity; // F-018.5a AspAvg: media mobile degli score
                            // (infinity = non ancora inizializzata)
  long long avg_sq_score = 0; // Q-28 AspScoreMult: EMA di score*|score| (valida
                              // sse avg_score != infinity)
  td.prev_pv_len =
      0; // Q-15: nessuna PV precedente alla prima iterazione di questa search
  int stable_iters =
      0; // T-01 TMStability: iterazioni consecutive con best move invariata
  int eval_stab_iters = 0; // TM v2 Q-03: iterazioni consecutive con score entro
                           // +-window da avg_score (cap 4)
  int mate_score_iters =
      0; // TM v2 Q-08b: iterazioni consecutive con score di matto
  int root_single_reply =
      0; // TM v2 Q-08a: 1 se a root c'e' UNA sola mossa legale

  // TM v2 Q-08a (Caissa): conta le mosse legali a root (si ferma a 2). Con una
  // sola risposta legale pensare e' inutile: basta la prima iterazione
  // completata. Stesso protocollo make/unmake del rescue anti-forfeit (ply +
  // repetition stack).
  // FIX 2026-07-26: tolto `&& g_tmv2_tc_ok`. Quel gate vale `inc > 0` e serve
  // ai fattori di ALLOCAZIONE di TMv2, che presuppongono formule pool-incremento.
  // Questo pero' non alloca niente: se a root c'e' UNA SOLA mossa legale,
  // pensare di piu' non puo' cambiare la mossa — vero con o senza incremento.
  // Conta perche' **CCRL 40/4 e CEGT girano a inc=0**: e' proprio il regime dove
  // veniamo valutati, ed era l'unico in cui questa sicurezza restava spenta.
  // Puo' solo fermarsi PRIMA, mai spendere di piu' -> non puo' causare forfeit.
  if (g_tmv2_single_reply && thread_id == 0 && timeset) {
    moves ml[1];
    td_generate_moves(td, ml, false);
    int legal = 0;
    for (int i = 0; i < ml->count && legal < 2; i++) {
      UndoInfo undo;
      td.ply++;
      td.repetition_table[++td.repetition_index] = td.hash_key;
      if (td_make_move(td, ml->moves[i], undo)) {
        td_unmake_move(td, ml->moves[i], undo);
        legal++;
      }
      td.ply--;
      td.repetition_index--;
    }
    root_single_reply = (legal == 1);
  }

  // MultiPV (analisi): quante linee cercare. Solo main thread; clampato alle
  // mosse LEGALI di root (con meno mosse che linee, la linea in eccesso non ha
  // mosse da cercare e tornerebbe uno score di matto/stallo fasullo). Stesso
  // protocollo make/unmake del conteggio Q-08a sopra.
  int mpv_nlines = 1;
  td.mpv_count = 0;
  if (g_multipv > 1 && thread_id == 0) {
    moves ml[1];
    td_generate_moves(td, ml, false);
    int legal = 0;
    for (int i = 0; i < ml->count; i++) {
      // con searchmoves attivo, contano solo le mosse in whitelist
      if (g_searchmoves_count) {
        bool in_list = false;
        for (int k = 0; k < g_searchmoves_count; k++)
          if (g_searchmoves[k] == ml->moves[i]) {
            in_list = true;
            break;
          }
        if (!in_list)
          continue;
      }
      UndoInfo undo;
      td.ply++;
      td.repetition_table[++td.repetition_index] = td.hash_key;
      if (td_make_move(td, ml->moves[i], undo)) {
        td_unmake_move(td, ml->moves[i], undo);
        legal++;
      }
      td.ply--;
      td.repetition_index--;
    }
    mpv_nlines = g_multipv < legal ? g_multipv : legal;
    if (mpv_nlines < 1)
      mpv_nlines = 1;
  }

  // MultiPV: buffer per raccogliere le linee secondarie dell'iterazione
  // corrente e stamparle ORDINATE per score (mai un multipv k+1 sopra il k
  // nelle GUI).
  struct MpvLine {
    int score;
    int len;
    int pv[max_ply + 8];
  };
  std::vector<MpvLine> mpv_buf;
  if (mpv_nlines > 1)
    mpv_buf.resize(mpv_nlines);

  // Stagger starting depths for thread diversity
  int start_depth = 1 + (thread_id % 2);

  // DiverseSMP (#2), WIDER-ONLY variant: per-thread LMR reduction bias. Helpers
  // (id>0) only ever search WIDER (negative bias = less reduction); the
  // magnitude grows with id but is capped at g_diverse_smp_amount. Thread 0
  // stays canonical (0). RATIONALE: an earlier symmetric (+/-) version hijacked
  // the root move -- a helper with a *positive* bias prunes harder, reaches a
  // higher nominal depth, and wins the depth-based result selection with an
  // over-reduced (worse) move. amt3 measured -79 Elo @8+0.08 from exactly this.
  // Forcing bias <= 0 means every helper reaches a LOWER depth than thread 0,
  // so it can never hijack the result; helpers contribute only via the shared
  // TT (seeding less-pruned entries). For inter-helper diversity run
  // DiverseSMPAmount >= 2 (else all helpers collapse to -1). Re-read each
  // search so DiverseSMPAmount (SPSA) takes effect live. Applied in the LMR
  // block only when g_diverse_smp is on (guarded there).
  if (thread_id == 0) {
    td.lmr_bias = 0;
    td.lmr_bias_units = 0;
  } else if (g_diverse_smp_fine > 0) {
    // DiverseSMPFine: perturbazione FINE (millesimi di ply), wider-only come
    // sopra. Stessa crescita per coppie di id, cap 8 (max ~0.75 ply con
    // fine=96).
    int mag = 1 + (thread_id - 1) / 2;
    if (mag > 8)
      mag = 8;
    td.lmr_bias = 0; // il path legacy (ply interi) resta spento
    td.lmr_bias_units = -mag * g_diverse_smp_fine;
  } else {
    int mag = 1 + (thread_id - 1) / 2;
    if (mag > g_diverse_smp_amount)
      mag = g_diverse_smp_amount;
    td.lmr_bias =
        -mag; // wider-only: helpers never search "deeper" -> no hijack
    td.lmr_bias_units = td.lmr_bias * 1024; // identico al vecchio lmr_bias*1024
  }

  for (int current_depth = start_depth; current_depth <= max_depth;
       current_depth++) {
    if (stop_threads.load(std::memory_order_relaxed))
      break;

    // Lazy SMP: helper threads (id > 0) skip a per-thread pattern of depths so
    // they diversify which iterations they invest in. The main thread (id 0)
    // never skips: it drives time management and prints the PV, so it must
    // progress through every depth monotonically.
    if (thread_id > 0) {
      int s = (thread_id - 1) % 8;
      if (((current_depth + LSMP_SkipPhase[s]) / LSMP_SkipSize[s]) & 1)
        continue;
    }

    // Re-sync the incremental NNUE mirror to the root board (full refresh)
    // so a previously aborted iteration cannot leave the accumulator stack
    // out of step with the search.
    nn_root_sync(td);

    td.root_depth =
        current_depth; // F-018.6c SingularPlyGuard: gate ply < 2*rootDepth
    if (mpv_nlines > 1)
      mpv_buf[0].len =
          0; // MultiPV: nessuna principale deferita da iterazioni stale

    // Aspiration window: start narrow around the previous score and widen
    // incrementally on failures, instead of re-searching the full window.
    // F-018.5a AspAvg (default OFF): centro = media mobile degli score invece
    // del prev_score secco -> il rumore iterazione-su-iterazione non sposta la
    // finestra.
    int alpha = -infinity, beta = infinity, delta = g_asp_init_delta;
    if (g_asp_stable_shrink) { // ricerca stabile -> parti piu' stretto
                               // (Reckless 4192617)
      int stab =
          eval_stab_iters < stable_iters ? eval_stab_iters : stable_iters;
      if (stab > g_asp_stable_cap)
        stab = g_asp_stable_cap;
      delta -= stab;
      if (delta < 1)
        delta = 1;
    }
    // Q-28 AspScoreMult: allarga il delta iniziale col quadrato-EMA dello score
    // (i64: |avgSq| fino a ~9e8, *mult sfora i32). 0 = OFF.
    if (g_asp_score_mult > 0 && avg_score != infinity) {
      long long widen = (avg_sq_score < 0 ? -avg_sq_score : avg_sq_score) *
                            (long long)g_asp_score_mult >>
                        20;
      delta += (int)widen;
    }
    if (current_depth >= 4) {
      int center =
          (g_asp_avg && avg_score != infinity) ? avg_score : prev_score;
      alpha = (center - delta > -infinity) ? center - delta : -infinity;
      beta = (center + delta < infinity) ? center + delta : infinity;
    }

    int score;
    U64 iter_nodes = 0;
    int fail_high_cnt = 0; // F-018.5b AspFHRed
    while (true) {
      U64 nodes_before_call = td.nodes;
      // F-018.5b AspFHRed (default OFF): ogni fail-high consecutivo ri-cerca a
      // depth-1 (Obsidian/Berserk/SF: la conferma del fail-high non serve a
      // profondita' piena).
      int search_depth = current_depth;
      if (g_asp_fhred && fail_high_cnt > 0) {
        search_depth = current_depth - fail_high_cnt;
        if (search_depth < 1)
          search_depth = 1;
      }
      score = td_negamax(td, alpha, beta, search_depth, false, 0,
                         g_follow_pv); // Q-15: la root segue sempre la PV
      iter_nodes =
          td.nodes - nodes_before_call; // nodes of the last (in-window) search
      if (stop_threads.load(std::memory_order_relaxed))
        break;

      if (score <= alpha) { // fail low: lower alpha, pull beta toward midpoint
        beta = (alpha + beta) / 2;
        alpha = (score - delta > -infinity) ? score - delta : -infinity;
        delta += delta * g_asp_grow / 100;
        fail_high_cnt = 0; // F-018.5b: la depth piena torna sul fail-low (SF)
      } else if (score >= beta) { // fail high: raise beta
        beta = (score + delta < infinity) ? score + delta : infinity;
        delta += delta * g_asp_grow / 100;
        if (score < mate_score)
          fail_high_cnt++;
        // Q-31 (Reckless 0010617): score diventato decisivo -> non trascinare
        // le riduzioni AspFHRed accumulate sui fail-high precedenti (il matto
        // va confermato quasi a depth piena, non a depth-N).
        else if (fail_high_cnt > 1)
          fail_high_cnt = 1;
      } else {
        break; // score is inside the window
      }
    }

    if (stop_threads.load(std::memory_order_relaxed))
      break;

    prev_score = score;
    // Q-28: aggiorna l'EMA quadratica PRIMA di avg_score (usa avg_score come
    // flag first-time)
    {
      long long sq = (long long)score * (score < 0 ? -score : score);
      avg_sq_score = (avg_score == infinity) ? sq : (avg_sq_score + sq) / 2;
    }
    avg_score =
        (avg_score == infinity) ? score : (avg_score + score) / 2; // F-018.5a
    // EvalOptimism (5.1): aggiorna il contempt dinamico per i sottoalberi
    // successivi (SF lo ricava dall'averageScore della root). Solo main thread
    // scrive (gli helper leggono soft).
    if (g_eval_optimism && thread_id == 0) {
      int a = score < 0 ? -score : score;
      int opt = g_opt_strength * score / (a + g_opt_div);
      g_optimism[td.side] = opt;
      g_optimism[td.side ^ 1] = -opt;
    }

    if (td.pv_length[0] > 0) {
      // Q-29 (Reckless fec9098, default OFF): "forgotten mate" — un'iterazione
      // completata puo' regredire su un matto GIA' provato (collisione TT /
      // potatura). Se lo score precedente era decisivo e il nuovo e' piu'
      // debole in valore assoluto, manteniamo mossa+score dell'iterazione
      // precedente.
      int prev_abs = td.best_score < 0 ? -td.best_score : td.best_score;
      int new_abs = score < 0 ? -score : score;
      if (g_root_mate_restore && td.depth > 0 && prev_abs >= mate_score &&
          new_abs < prev_abs) {
        if (thread_id == 0)
          print_search_info(td, current_depth, td.best_score,
                            mpv_nlines > 1 ? 1 : 0);
      } else {
        td.best_move = td.pv_table[0][0];
        td.best_score = score;
        td.depth = current_depth;

        // Q-15 FollowPV: salva la PV di QUESTA iterazione completata; la
        // prossima iterazione la riseguira' (niente IIR/shallow-pruning su
        // quella linea). Copia solo a iterazione in-window (qui).
        if (g_follow_pv) {
          int len = td.pv_length[0];
          if (len > max_ply)
            len = max_ply;
          for (int i = 0; i < len; i++)
            td.prev_pv[i] = td.pv_table[0][i];
          td.prev_pv_len = len;
        }

        // Only main thread prints
        if (thread_id == 0) {
          if (mpv_nlines > 1) {
            // MultiPV: stampa DEFERITA al blocco sotto — la principale entra
            // nel sort insieme alle secondarie (una secondaria a finestra
            // piena puo' superarla: multipv 1 = sempre la linea migliore).
            mpv_buf[0].score = score;
            mpv_buf[0].len = td.pv_length[0];
            for (int i = 0; i < td.pv_length[0]; i++)
              mpv_buf[0].pv[i] = td.pv_table[0][i];
          } else
            print_search_info(td, current_depth, score);
        }
      }
    }

    // ---- MultiPV: linee secondarie (solo analisi, mpv_nlines>1, thread 0)
    // ---- Dopo la linea principale dell'iterazione, ri-cerca la root a
    // finestra PIENA escludendo le best move gia' assegnate (skip nel move loop
    // di td_negamax via td.mpv_count). Niente aspirazione sulle secondarie:
    // costo accettabile in analisi, zero stato da trascinare tra iterazioni.
    // Tutto lo stato di gioco (best_move/score, TM, FollowPV, optimism, EMA) e'
    // gia' stato aggiornato SOPRA dalla sola linea principale e non viene
    // toccato. ponytail: niente sort per score — l'esclusione rende le linee
    // gia' non-crescenti a meno di rumore; aggiungere il sort se una GUI si
    // confonde.
    if (mpv_nlines > 1 && thread_id == 0 && td.pv_length[0] > 0 &&
        !stop_threads.load(std::memory_order_relaxed)) {
      // done = linee nel buffer da ordinare; base = indice multipv della prima.
      // Path normale: buf[0] = principale (deferita sopra) -> stampa 1..N tutte
      // ordinate. Path mate-restore (Q-29, gia' stampata come singola): base 2,
      // si ordinano solo le secondarie.
      int done = (mpv_buf[0].len > 0) ? 1 : 0;
      int base = done ? 1 : 2;
      td.mpv_excluded[0] = td.pv_table[0][0];
      for (int pvIdx = 1; pvIdx < mpv_nlines; pvIdx++) {
        td.mpv_count = pvIdx;
        nn_root_sync(
            td); // mirror NNUE ri-allineato alla root prima di ri-cercare
        int s2 =
            td_negamax(td, -infinity, infinity, current_depth, false, 0, false);
        if (stop_threads.load(std::memory_order_relaxed) || td.pv_length[0] < 1)
          break;
        td.mpv_excluded[pvIdx] = td.pv_table[0][0];
        MpvLine &L = mpv_buf[done++];
        L.score = s2;
        L.len = td.pv_length[0];
        for (int i = 0; i < L.len; i++)
          L.pv[i] = td.pv_table[0][i];
      }
      // sort di indici per score decrescente e stampa multipv base..: mai una
      // linea sotto con score sopra (convenzione SF).
      int ord[64];
      for (int a = 0; a < done; a++)
        ord[a] = a;
      std::sort(ord, ord + done, [&](int x, int y) {
        return mpv_buf[x].score > mpv_buf[y].score;
      });
      for (int a = 0; a < done; a++) {
        const MpvLine &L = mpv_buf[ord[a]];
        print_search_info(td, current_depth, L.score, base + a, L.pv, L.len);
      }
      // bestmove coerente col multipv 1: se una linea a finestra piena ha
      // superato la principale, riallinea best_move/score (solo analisi:
      // mpv_nlines>1 non esiste in partita, TM/voting non toccati altrove).
      if (base == 1 && done > 0) {
        td.best_move = mpv_buf[ord[0]].pv[0];
        td.best_score = mpv_buf[ord[0]].score;
      }
      td.mpv_count =
          0; // le iterazioni/ricerche successive ripartono senza esclusioni
    }

    // ---- P4 TroubleMaking (solo main thread, posizione persa) ----
    // Candidato = mossa root (≠ best) col max sforzo cumulativo nella NodeCache
    // (stessa entry per best e candidato -> confronto omogeneo). Se lo sforzo
    // e' comparabile (>= Effort% della best) una verifica null-window a depth/2
    // conferma che vale >= score-Margin -> best_move scambiata (score/PV
    // restano quelli teorici: cambio solo la mossa GIOCATA, non il display).
    if (g_trouble && thread_id == 0 && mpv_nlines == 1 && current_depth >= 8 &&
        td.best_move && score < -g_trouble_score && score > -mate_score &&
        !stop_threads.load(std::memory_order_relaxed)) {
      ThreadData::NCEntry *e = nc_try(td, td.hash_key);
      if (e) {
        int cand = 0;
        U64 cn = 0, bn = 0;
        for (int i = 0; i < ThreadData::NC_MOVES && e->mv[i]; i++) {
          if (e->mv[i] == td.best_move)
            bn = e->mv_nodes[i];
          else if (e->mv_nodes[i] > cn) {
            cand = e->mv[i];
            cn = e->mv_nodes[i];
          }
        }
        if (cand && bn && cn * 100 >= bn * (U64)g_trouble_effort) {
          int save_pv[max_ply + 8];
          int save_len = td.pv_length[0];
          memcpy(save_pv, td.pv_table[0], sizeof(save_pv));
          nn_root_sync(td);
          td.root_only_move = cand;
          int target = score - g_trouble_margin;
          int v = td_negamax(td, target - 1, target, current_depth / 2, false);
          td.root_only_move = 0;
          memcpy(td.pv_table[0], save_pv, sizeof(save_pv));
          td.pv_length[0] = save_len;
          if (!stop_threads.load(std::memory_order_relaxed) && v >= target) {
            td.best_move = cand;
            // ristampa l'info-line con la mossa scambiata in testa alla PV:
            // le GUI/fastchess confrontano bestmove col primo move della PV
            // (senza questa riga: warning "Bestmove does not match last PV").
            print_search_info(td, current_depth, score, 0, &cand, 1);
          }
        }
      }
    }

    // "go mate N": un'iterazione completata conferma un matto NOSTRO in <= N
    // mosse -> il compito e' assolto, stop (convenzione UCI; GUI/solver di
    // studi).
    if (g_mate_in > 0 && thread_id == 0 && score > mate_score &&
        score < mate_value && (mate_value - score) / 2 + 1 <= g_mate_in) {
      stop_threads.store(true, std::memory_order_relaxed);
      break;
    }

    // Soft time management: the main thread decides whether to start another
    // iteration. If the best move is unstable (just changed), allow more
    // time, up to the hard cap (stoptime).
    if (thread_id == 0 && timeset) {
      int soft = soft_time_limit; // absolute timestamp (starttime + optimum)

      // T-01 TMStability: iterazioni consecutive con best move invariata
      // (prev_best_move tiene ancora il valore dell'iterazione precedente,
      // aggiornato piu' sotto).
      if (current_depth > start_depth && td.best_move == prev_best_move)
        stable_iters++;
      else
        stable_iters = 0;

      // TM v2 Q-03: counter di stabilita' dell'EVAL (bidirezionale: misura se
      // lo score resta fermo attorno alla media mobile, non solo se cala).
      // ⚠️ 2026-07-18: SGANCIATO dal gate TMv2. Era calcolato solo con TMv2
      // attivo (quindi MAI sotto i 15s), ma ora serve anche ad AspStableShrink,
      // che vive a ogni TC. Il CONSUMO da parte di TMv2 e' invariato -> nessun
      // cambio di comportamento con AspStableShrink=false (il counter viene
      // solo tenuto aggiornato invece che a zero).
      if (current_depth > start_depth && avg_score != infinity) {
        int ediff = score - avg_score;
        if (ediff < 0)
          ediff = -ediff;
        if (ediff <= g_tmv2_eval_window) {
          if (eval_stab_iters < 4)
            eval_stab_iters++;
        } else
          eval_stab_iters = 0;
      }

      if (g_tmv2 && g_tmv2_tc_ok) {
        // ---- TM v2 (Q-01): composizione MOLTIPLICATIVA stateless ----
        // Tutti i fattori ricalcolati freschi dalla durata BASE ogni iterazione
        // (niente accumulo -> niente drift), ognuno bidirezionale.
        int si = stable_iters > 4 ? 4 : stable_iters;
        double scale = g_tmv2_stab[si] / 100.0;        // Q-02 extend-AND-reduce
        scale *= g_tmv2_eval[eval_stab_iters] / 100.0; // Q-03 trend-eval
        if (current_depth >= 6 &&
            iter_nodes > 0) { // Q-04 NodeTM con estensione
          double frac = (double)td.root_bestmove_nodes / (double)iter_nodes;
          if (frac > 1.0)
            frac = 1.0;
          double nf =
              (g_tmv2_node_base / 100.0 - frac) * (g_tmv2_node_mult / 100.0);
          double nmin = g_tmv2_node_min / 100.0, nmax = g_tmv2_node_max / 100.0;
          if (nf < nmin)
            nf = nmin;
          if (nf > nmax)
            nf = nmax;
          scale *= nf;
        }
        int dur = soft - starttime; // BASE optimum duration (ms)
        if (dur < 0)
          dur = 0;
        soft = starttime + (int)(dur * scale); // il clamp a stoptime e' sotto
      } else {

        if (current_depth > start_depth && td.best_move != prev_best_move)
          soft += (stoptime - soft_time_limit) * g_tm_instab / 100;

        // Score-drop time extension (TimeMgmt): a falling score vs the previous
        // completed iteration suggests the position is turning against us ->
        // grant extra time, scaled by the drop, capped at ~25% of the headroom.
        if (g_time_mgmt && current_depth > start_depth) {
          int drop = prev_completed_score - score; // >0 = got worse
          if (drop >
              g_tm_drop_thresh) { // F-002: era hardcoded 8/200 (unita'-eval)
            int d = (drop > g_tm_drop_cap) ? g_tm_drop_cap : drop;
            soft += (stoptime - soft_time_limit) * d / g_tm_drop_div;
          }
        }

        // Node-based time management. IMPORTANT: soft is an ABSOLUTE timestamp,
        // so we must scale the DURATION (soft - starttime), never the timestamp
        // itself. REDUCE-ONLY (scale <= 1.0): if the best move dominates the
        // node count we're confident -> stop sooner. We never inflate the
        // duration (that would push soft past stoptime -> burn the whole
        // clock).
        if (g_node_tm && current_depth >= 6 && iter_nodes > 0) {
          double frac = (double)td.root_bestmove_nodes / (double)iter_nodes;
          if (frac > 1.0)
            frac = 1.0;
          double scale =
              g_nodetm_base / 100.0 - frac; // F-002: era hardcoded 1.4/0.6
          if (scale > 1.0)
            scale = 1.0;
          double smin = g_nodetm_min / 100.0;
          if (scale < smin)
            scale = smin;
          int dur = soft - starttime; // current optimum duration (ms)
          if (dur < 0)
            dur = 0;
          soft = starttime + (int)(dur * scale);
        }

        // T-01 TMStability (default OFF): best move stabile da piu' iterazioni
        // -> confidenza -> riduci la durata optimum verso il pavimento.
        // REDUCE-ONLY (scale <= 1.0), si compone col NodeTM. Segnale diverso
        // dal NodeTM (persistenza tra iterazioni vs quota-nodi di questa
        // iterazione). Solo riduce, mai spinge soft oltre stoptime.
        if (g_tm_stability && stable_iters > g_tm_stab_thresh) {
          int over = stable_iters - g_tm_stab_thresh;
          double sscale = 1.0 - over * (g_tm_stab_step / 1000.0);
          double sfloor = g_tm_stab_floor / 100.0;
          if (sscale < sfloor)
            sscale = sfloor;
          int dur = soft - starttime;
          if (dur < 0)
            dur = 0;
          soft = starttime + (int)(dur * sscale);
        }

      } // fine path TM v1 (additivo)

      // A4 FIX 2026-07-25: "go movetime N" e' un tempo FISSO ordinato dalla GUI,
      // non una stima da correggere. I fattori qui sopra lo accorciavano (il
      // NodeTM e' reduce-only e scende fino a g_nodetm_min/100 = 58%): analisi e
      // GUI a tempo fisso ricevevano il 58% del tempo chiesto. Nessun effetto sui
      // gate ne' sul gioco a tempo+incremento, dove movetime == -1.
      if (movetime != -1)
        soft = soft_time_limit;

      if (soft > stoptime)
        soft = stoptime; // never exceed the hard cap

      prev_best_move = td.best_move;
      prev_completed_score = score;

      // TM v2 Q-08: stop immediati, indipendenti dal soft-time.
      // (a) single-reply: unica mossa legale a root -> la prima iterazione
      //     completata basta (pensare non cambia la mossa).
      // (b) mate-stop: score di matto per N iterazioni consecutive -> il matto
      //     e' confermato, il tempo extra non serve piu' (robusto ai matti
      //     instabili proprio perche' chiede N conferme, alla Caissa).
      // FIX 2026-07-26: tolto `&& g_tmv2_tc_ok` anche qui, stessa ragione del
      // single-reply a :8258. Un matto confermato per N iterazioni consecutive
      // resta confermato che ci sia o no l'incremento: il tempo extra non serve.
      // Non e' allocazione, e' uno stop -> puo' solo risparmiare tempo.
      if (g_tmv2_mate_stop) {
        int mabs = score < 0 ? -score : score;
        if (mabs >= mate_score)
          mate_score_iters++;
        else
          mate_score_iters = 0;
      }
      if ((root_single_reply && td.best_move) ||
          (g_tmv2_mate_stop &&
           mate_score_iters >= g_tmv2_mate_iters)) {
        stop_threads.store(true, std::memory_order_relaxed);
        break;
      }

      // TM v2 Q-07 (Caissa): root-singularity stop. Se la best e' cosi' sopra
      // le alternative da essere "singolare", le iterazioni extra non la
      // cambieranno: verifica economica (depth/2, finestra nulla, best ESCLUSA
      // -> riusa il meccanismo excluded_move della singular extension; nessuno
      // store TT durante l'esclusione, quindi niente inquinamento). Un
      // fail-high della verifica sovrascriverebbe la PV root: salvata e
      // ripristinata.
      if (g_tmv2_rsing && g_tmv2_tc_ok && td.best_move && current_depth >= 9) {
        int sabs = score < 0 ? -score : score;
        int elapsed = get_time_ms() - starttime;
        int opt_dur = soft_time_limit - starttime;
        if (sabs < g_tmv2_rs_scorecap &&
            elapsed > opt_dur * g_tmv2_rs_timefrac / 100 &&
            get_time_ms() < soft) {
          int margin = g_tmv2_rs_margin_base -
                       g_tmv2_rs_margin_slope * (current_depth - 9);
          if (margin < g_tmv2_rs_margin_min)
            margin = g_tmv2_rs_margin_min;
          int target = score - margin;
          int save_pv[max_ply + 8];
          int save_len = td.pv_length[0];
          memcpy(save_pv, td.pv_table[0], sizeof(save_pv));
          nn_root_sync(td); // mirror allineato alla root prima di ri-cercare
          int v = td_negamax(td, target - 1, target, current_depth / 2, false,
                             td.best_move);
          memcpy(td.pv_table[0], save_pv, sizeof(save_pv));
          td.pv_length[0] = save_len;
          if (!stop_threads.load(std::memory_order_relaxed) && v < target) {
            stop_threads.store(true, std::memory_order_relaxed);
            break;
          }
        }
      }

      if (get_time_ms() >= soft) {
        stop_threads.store(true, std::memory_order_relaxed);
        break;
      }
    }
  }
}

// ============================================================================
// THREAD MANAGEMENT
// ============================================================================

void stop_search_threads() {
  stop_threads.store(true, std::memory_order_relaxed);
}

void wait_for_threads() {
  for (auto &t : search_threads) {
    if (t.joinable())
      t.join();
  }
  search_threads.clear();
}

// ============================================================================
// ASYNCHRONOUS SEARCH DRIVER
//
// The UCI "go" must not block the input loop, otherwise "stop" and
// "go infinite" cannot work. So search_position_mt() runs on its own master
// thread (which in turn spawns the helper threads), while the UCI loop keeps
// reading stdin.
// ============================================================================

std::thread search_master;

// Join the master search thread if it is running/finished. Callers that may
// invoke this while an (possibly infinite) search is in progress must first
// call stop_search_threads() so the search actually terminates.
void wait_for_search_done() {
  if (search_master.joinable())
    search_master.join();
}

// Stop any search in progress, wait for it to fully finish, then start a new
// search on a background thread.
void launch_search(int depth) {
  stop_search_threads();  // stop a previous search (if any)
  wait_for_search_done(); // join it and its helpers

  // Syzygy ROOT probe (DTZ): if the GLOBAL (root) position is covered by the
  // installed tablebases, play the perfect move immediately instead of
  // searching. Done HERE, on the main UCI thread and BEFORE spawning the
  // worker, so it cannot race with parse_position() of the next command.
  // Needs .rtbz (DTZ) files; with WDL-only tables it simply fails and we fall
  // through to a normal search (whose in-search WDL probe already fixes the
  // evaluation of covered endgames).
  {
    int tb_move = 0, tb_score = 0;
    if (syzygy_probe_root(tb_move, tb_score)) {
      new_search();
      printf("info depth 1 score cp %d tbhits 1 pv ", tb_score);
      print_move(tb_move);
      printf("\nbestmove ");
      print_move(tb_move);
      printf("\n");
      fflush(stdout);
      return;
    }
  }

  stop_threads.store(false, std::memory_order_relaxed); // clear before spawning
  search_master = std::thread(search_position_mt, depth);
}

// ============================================================================
// MAIN SEARCH ENTRY
// ============================================================================

void search_position_mt(int depth) {
  search_start_time = get_time_ms();

  // Increment TT age
  new_search();

  // NOTE: stop_threads is cleared by launch_search() BEFORE this thread is
  // spawned, so that a "stop" arriving right after "go" is not lost here.
  // NOTE: the Syzygy ROOT probe is NOT done here. It reads the GLOBAL board and
  // must run on the main (UCI) thread BEFORE this worker is spawned - otherwise
  // it races with parse_position() of the next command. See launch_search().
  stopped = 0;

  for (int i = 0; i < num_threads; i++) {
    thread_data[i].nodes = 0;
    thread_data[i].tb_hits = 0;
    thread_data[i].best_move = 0;
    thread_data[i].best_score = -infinity;
    thread_data[i].depth = 0;
    thread_data[i].fh_nodes = 0;
    thread_data[i].fh_first = 0;
    thread_data[i].fh_move_sum = 0;
    thread_data[i].fh_tt = 0;
    thread_data[i].fh_tt_first = 0;
    thread_data[i].fh_probe = 0;
  }

  search_threads.clear();

  // Start helper threads
  for (int i = 1; i < num_threads; i++) {
    search_threads.emplace_back(thread_search, i, depth);
  }

  // Main thread search
  thread_search(0, depth);

  stop_threads.store(true, std::memory_order_relaxed);
  wait_for_threads();

  // Get best result from all threads
  int best_move = thread_data[0].best_move;
  int best_score = thread_data[0].best_score;
  int best_depth = thread_data[0].depth;

  if (g_thread_voting && num_threads > 1) {
    // P1.12: voto pesato stile SF. Un matto provato salta il voto (vince il
    // piu' corto = score piu' alto); altrimenti ogni thread vota la sua mossa
    // con peso (score - minScore + 14) * depth e vince il consenso.
    int mate_thread = -1;
    int min_score = infinity;
    for (int i = 0; i < num_threads; i++) {
      if (!thread_data[i].best_move)
        continue;
      if (thread_data[i].best_score < min_score)
        min_score = thread_data[i].best_score;
      if (thread_data[i].best_score >= mate_score &&
          (mate_thread < 0 ||
           thread_data[i].best_score > thread_data[mate_thread].best_score))
        mate_thread = i;
    }
    if (mate_thread >= 0) {
      best_move = thread_data[mate_thread].best_move;
      best_score = thread_data[mate_thread].best_score;
      best_depth = thread_data[mate_thread].depth;
    } else if (min_score < infinity) {
      int vote_moves[MAX_THREADS];
      long long vote_w[MAX_THREADS];
      int nv = 0;
      for (int i = 0; i < num_threads; i++) {
        if (!thread_data[i].best_move)
          continue;
        long long w = (long long)(thread_data[i].best_score - min_score + 14) *
                      (thread_data[i].depth > 0 ? thread_data[i].depth : 1);
        int j;
        for (j = 0; j < nv; j++)
          if (vote_moves[j] == thread_data[i].best_move)
            break;
        if (j == nv) {
          vote_moves[nv] = thread_data[i].best_move;
          vote_w[nv] = 0;
          nv++;
        }
        vote_w[j] += w;
      }
      int win = 0;
      for (int j = 1; j < nv; j++)
        if (vote_w[j] > vote_w[win])
          win = j;
      // fra i thread che votano la vincitrice: depth max, tie score max
      int bt = -1;
      for (int i = 0; i < num_threads; i++) {
        if (thread_data[i].best_move != vote_moves[win])
          continue;
        if (bt < 0 || thread_data[i].depth > thread_data[bt].depth ||
            (thread_data[i].depth == thread_data[bt].depth &&
             thread_data[i].best_score > thread_data[bt].best_score))
          bt = i;
      }
      if (bt >= 0) {
        best_move = thread_data[bt].best_move;
        best_score = thread_data[bt].best_score;
        best_depth = thread_data[bt].depth;
      }
    }
  } else {
    // Legacy: vince il thread piu' profondo (tie: score piu' alto).
    for (int i = 1; i < num_threads; i++) {
      if (thread_data[i].depth > best_depth ||
          (thread_data[i].depth == best_depth &&
           thread_data[i].best_score > best_score)) {
        if (thread_data[i].best_move != 0) {
          best_move = thread_data[i].best_move;
          best_score = thread_data[i].best_score;
          best_depth = thread_data[i].depth;
        }
      }
    }
  }

  // FIX 2026-07-26 — "Bestmove does not match beginning of last PV".
  // La bestmove puo' venire da un thread != 0 (sia col voting, sia col legacy
  // "vince il piu' profondo" qui sopra), ma le info-line le stampa SOLO il
  // thread 0: tutte le chiamate a print_search_info sono guardate da
  // thread_id == 0. Quando vince un helper, l'ultima PV stampata e' quella del
  // thread 0 mentre la mossa giocata e' quella dell'helper -> due ricerche
  // diverse. Le GUI e fastchess lo segnalano, e la PV mostrata in analisi e'
  // fuorviante.
  // ⚠️ NON tocca la forza di gioco: la mossa giocata resta quella scelta dalla
  // regola di selezione, che e' il comportamento voluto. E' un difetto di
  // REPORTING. Si vede solo da oggi perche' e' il primo match multi-thread:
  // tutti i gate fino al 26/07 erano a 1 thread.
  // Effetto collaterale che resta APERTO: la predizione TM qui sotto richiede
  // thread_data[0].pv_table[0][0] == best_move, quindi quando vince un helper
  // si disattiva da sola. Non e' un bug di correttezza, ma con 8 thread quella
  // parte di TMv2 e' di fatto spenta. Farla puntare al thread vincitore
  // cambierebbe il comportamento a >1 thread e va misurato, non fatto qui.
  // A 1 thread la condizione non scatta mai -> byte-identico.
  // Quale thread ha davvero esplorato la variante che stiamo per giocare, cioe' la cui
  // PV comincia con la bestmove. Serve DUE volte: per stampare una PV coerente con la
  // mossa (sotto) e per la predizione TM (piu' giu'). Prima era calcolato solo dentro
  // il ramo della stampa, e la predizione restava incollata al thread 0.
  int pv_src = (thread_data[0].pv_length[0] > 0 &&
                thread_data[0].pv_table[0][0] == best_move)
                 ? 0
                 : -1;
  if (pv_src < 0) {
    for (int i = 1; i < num_threads; i++) {
      if (thread_data[i].pv_length[0] > 0 &&
          thread_data[i].pv_table[0][0] == best_move) {
        pv_src = i;
        break;
      }
    }
  }

  if (best_move && pv_src != 0) {
    if (pv_src > 0) {
      print_search_info(thread_data[pv_src], best_depth, best_score, 0,
                        thread_data[pv_src].pv_table[0],
                        thread_data[pv_src].pv_length[0]);
    } else {
      // FALLBACK (2026-07-26, seconda passata): puo' non esserci NESSUN thread
      // la cui pv_table[0][0] sia la bestmove. `td.best_move` e `pv_table[0][0]`
      // non sono la stessa cosa: se un'iterazione viene interrotta a meta', la
      // PV e' gia' stata sovrascritta da quella incompleta mentre best_move
      // conserva l'ultima completata; e il mate-restore assegna best_move = cand
      // senza toccare la PV. La prima versione del fix cercava solo un thread
      // corrispondente e in quel caso non stampava nulla -> warning ancora li'.
      // Qui si stampa almeno la mossa come PV di lunghezza 1: e' meno
      // informativo ma e' VERO, e la coerenza bestmove/PV e' garantita sempre.
      print_search_info(thread_data[0], best_depth, best_score, 0, &best_move,
                        1);
    }
  }

  // TM v2 Q-06 (Caissa): salva l'hash della posizione PREVISTA dopo
  // best+risposta (pv[0], pv[1]): al prossimo "go", se l'avversario ha giocato
  // la risposta predetta la TT e' calda -> meno tempo (fattore in parse_go).
  // Usa la PV del thread 0 solo se coincide con la mossa scelta (col voting
  // puo' non esserlo). Le mosse PV sono state fatte/disfatte in QUESTE
  // posizioni durante la search, quindi td_make_move puo' fidarsi dei bit
  // codificati. Stato pulito su ucinewgame.
  if (g_tmv2_pred && g_tmv2_tc_ok) {
    g_tm_pred_hash = 0;
    // FIX 3/08/2026 (dietro TMPredBestThread). La condizione qui sotto vuole che la PV
    // cominci con la mossa scelta: col thread 0 fisso fallisce OGNI VOLTA che vince un
    // helper, e la predizione si spegne da sola. A 4 thread e' successo 536 volte su
    // 5598 partite, cioe' una fetta di TMv2 (+23,8 Elo) e' inattiva proprio nel regime
    // in cui CCRL ci misura. Qui si prende la PV dal thread vincitore.
    // 🔴 SICUREZZA: sotto si fanno make/unmake su `tdp`, quindi la sua posizione DEVE
    // essere la root. Un helper interrotto potrebbe non esserci tornato, e l'hash
    // predetto sarebbe di un'ALTRA posizione — un errore silenzioso che si vedrebbe
    // solo come predizione che non scatta mai. Si confronta l'hash con quello del
    // thread 0 e, se non coincide, si ricade sul comportamento attuale.
    int tsrc = 0;
    if (g_tm_pred_best_thread && pv_src > 0 &&
        thread_data[pv_src].hash_key == thread_data[0].hash_key)
      tsrc = pv_src;
    ThreadData &tdp = thread_data[tsrc];
    if (best_move && tdp.pv_length[0] >= 2 && tdp.pv_table[0][0] == best_move) {
      int mv0 = tdp.pv_table[0][0], mv1 = tdp.pv_table[0][1];
      UndoInfo u0, u1;
      tdp.ply++;
      tdp.repetition_table[++tdp.repetition_index] = tdp.hash_key;
      if (td_make_move(tdp, mv0, u0)) {
        tdp.ply++;
        tdp.repetition_table[++tdp.repetition_index] = tdp.hash_key;
        if (td_make_move(tdp, mv1, u1)) {
          g_tm_pred_hash = tdp.hash_key;
          td_unmake_move(tdp, mv1, u1);
        }
        tdp.ply--;
        tdp.repetition_index--;
        td_unmake_move(tdp, mv0, u0);
      }
      tdp.ply--;
      tdp.repetition_index--;
    }
  }

  // Self-play data logging (Phase 1): record the searched root position and
  // the chosen move before emitting it. Uses the global board (= root).
  if (g_data_log_enabled && best_move) {
    log_search_record(best_move, best_score, best_depth);
  }

  // Last-resort anti-forfeit: if the search was aborted (extreme time pressure)
  // before thread 0 produced any move or PV, best_move is 0 and we'd emit
  // "bestmove (none)" — an instant loss in any GUI/match. Instead, fall back to
  // the FIRST LEGAL root move so we never forfeit. Mirrors the search's
  // make/unmake protocol (ply + repetition stack) for legality testing.
  if (!best_move && !thread_data[0].pv_table[0][0]) {
    ThreadData &td0 = thread_data[0];
    moves rescue[1];
    td_generate_moves(td0, rescue, false); // all pseudo-legal moves
    for (int i = 0; i < rescue->count; i++) {
      UndoInfo undo;
      td0.ply++;
      td0.repetition_table[++td0.repetition_index] = td0.hash_key;
      if (td_make_move(td0, rescue->moves[i], undo)) {
        td_unmake_move(td0, rescue->moves[i], undo);
        td0.ply--;
        td0.repetition_index--;
        best_move = rescue->moves[i];
        break;
      }
      td0.ply--;
      td0.repetition_index--;
    }
  }

  // CutoffStats (diagnostica move-ordering, off-default): aggrega i contatori
  // di tutti i thread e stampa il first-move-cutoff rate + indice medio della
  // mossa al cutoff.
  if (g_cutoff_stats) {
    U64 fh = 0, first = 0, msum = 0, ftt = 0, ftt1 = 0, fprobe = 0;
    for (int i = 0; i < num_threads; i++) {
      fh += thread_data[i].fh_nodes;
      fprobe += thread_data[i].fh_probe;
      first += thread_data[i].fh_first;
      msum += thread_data[i].fh_move_sum;
      ftt += thread_data[i].fh_tt;
      ftt1 += thread_data[i].fh_tt_first;
    }
    if (fh) {
      U64 nott = fh - ftt, nott1 = first - ftt1;
      printf("info string FMC %.2f%% (fh=%llu first=%llu) avgidx %.3f | ttrate "
             "%.2f%% probehit %.2f%% FMC|tt %.2f%% FMC|nott %.2f%%\n",
             100.0 * (double)first / (double)fh, (unsigned long long)fh,
             (unsigned long long)first, (double)msum / (double)fh,
             100.0 * (double)ftt / (double)fh,
             100.0 * (double)fprobe / (double)fh,
             ftt ? 100.0 * (double)ftt1 / (double)ftt : 0.0,
             nott ? 100.0 * (double)nott1 / (double)nott : 0.0);
    }
  }

  printf("bestmove ");
  if (best_move) {
    print_move(best_move);
  } else if (thread_data[0].pv_table[0][0]) {
    print_move(thread_data[0].pv_table[0][0]);
  } else {
    printf(
        "(none)"); // truly no legal move (stalemate/checkmate handled earlier)
  }
  printf("\n");
  fflush(stdout);
}

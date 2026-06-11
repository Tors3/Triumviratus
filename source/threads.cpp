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
#include "tt.h"
#include "search.h"
#include "misc.h"
#include "movegen.h"
#include "evaluation.h"
#include "magic.h"
#include "attacks.h"
#include "see.h"
#include "sf_bridge.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <cstring>
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h>   // getpid() (su Windows il pid arriva da GetCurrentProcessId in windows.h)
#endif
#include <fstream>
#include <string>
#include "io.h"
#include "defs.h"

#include "syzygy.h"

 // ============================================================================
 // GLOBALS
 // ============================================================================

tt_entry* hash_table = nullptr;

// Stockfish piece codes indexed by Triumviratus piece (P,N,B,R,Q,K,p,n,b,r,q,k):
//   white W_PAWN..W_KING = 1..6, black B_PAWN..B_KING = 9..14. Defined here
//   (before td_make_move) so the make-move NNUE mirror hook can use it too.
static const int sf_piece_code[12] = { 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14 };

// POLICY-NET: RIMOSSA dal motore (2026-06-11). Il capitolo e' stato chiuso con tre
// misure conclusive (notes/ANALISI_CODICE_OTTIMIZZAZIONI.md §P5): ordering −8.7,
// EntropyTM −16.7 LOS5%, PolicyEasyMove −14.35 LOS0.1% @2520g — la CNN (top1 ~24%)
// non ha segnali Elo-utili ne' per l'ordering (saturo, prior statico vs history
// adattiva) ne' per il time management (la confidenza di una rete debole non e'
// affidabilita'; l'entropia misura l'ignoranza della rete, non la complessita').
// Il codice vive nella storia git (tag 3.8) per l'eventuale R&D futuro (MCTS@root).

// ---- Bundle 3.9 (2026-06-11): micro-fix "correttezza/gratis" (P1.4/P1.11/P1.13/
// P1.10a/P2.1 dell'analisi). Ognuno dietro toggle default ON, ablazione stile 3.8.

// P1.4 Mate-distance pruning: stringe [alpha,beta] coi matti gia' provati piu'
// corti (un matto piu' lungo non puo' migliorare il risultato). 3 righe, rischio ~0.
static bool g_mate_dist = true;
void set_mate_dist(bool v) { g_mate_dist = v; }

// P1.11 Draw dither (SF value_draw): le patte per ripetizione/50-mosse ritornano
// ±1cp in funzione del node-count invece di 0 secco -> rompe la cecita' da
// ripetizione (evita oscillazioni 0.00 premature fra linee "ugualmente patte").
static bool g_draw_dither = true;
void set_draw_dither(bool v) { g_draw_dither = v; }

// P1.13 History bonus al ttMove quiet su TT-cutoff (SF): un cutoff servito dalla TT
// non passa dal loop mosse -> senza questo la history del ttMove si raffredda anche
// se la mossa continua a tagliare. Scala SPSA-tunable (TTCutBonusScale /100).
static bool g_ttcut_bonus = true;
void set_ttcut_bonus(bool v) { g_ttcut_bonus = v; }
int g_ttcut_bonus_scale = 100;   // /100 del td_stat_bonus(depth); spin "TTCutBonusScale"

// P1.10a TT age-refresh al probe-hit: un hit rinfresca l'age dell'entry, cosi' le
// posizioni CALDE ma scritte in search vecchie non vengono evictate per anzianita'
// (SF fa lo stesso). Definito qui, extern in tt.h (usato da probe_tt).
bool g_tt_age_refresh = true;
void set_tt_age_refresh(bool v) { g_tt_age_refresh = v; }

// P2.1 Pawn key INCREMENTALE: td.pawn_key mantenuta in make/unmake (XOR dei soli
// pedoni mossi/catturati/promossi, self-inverse come OccIncr) al posto del rescan
// delle bitboard pedoni a OGNI td_corr_index (corr 1x/nodo + pawn_history ~30
// quiet/nodo). NODE-IDENTICAL per costruzione -> misura NPS a depth fissa.
static bool g_pawn_key_incr = true;
void set_pawn_key_incr(bool v) { g_pawn_key_incr = v; }

// ---- Wave 3b (2026-06-11): P1.1 static-eval-in-TT + P2.2 + P2.3 -------------

// P1.1 (UCI "TTStaticEval", default ON): usa l'eval statica salvata nella TT
// (8 bit: ±508cp passo 4, vedi tt.h) al posto della forward NNUE quando il probe
// la porta. Il motore e' EVAL-BOUND (58% del tempo-nodo = forward): ogni hit con
// eval = una forward risparmiata. Errore ±2cp + staleness-fifty accettati (stessa
// classe dell'eval-cache, gia' ON). Lo STORE dell'eval e' incondizionato (gratis);
// il toggle gata solo l'USO. Cambia i node-count -> SPRT.
static bool g_tt_static_eval = true;
void set_tt_static_eval(bool v) { g_tt_static_eval = v; }

// P1.1 FIX fifty-staleness (2026-06-11): l'eval del wrapper SF e' SMORZATA dal
// rule50 (evaluate.cpp: v*(200-fifty)/214) -> salvarla cruda in TT (chiave SENZA
// fifty) la rende stantia: stored a fifty basso, riusata a fifty alto = eval
// inflazionata (misurato: -37% nodi startpos = over-pruning; +1328% nel finale
// bloccato = caos+corr avvelenata). Come SF (unadjustedStaticEval): in TT va
// l'eval DE-smorzata; all'uso si RI-smorza col fifty corrente. Errore residuo =
// solo arrotondamenti interi (~±3cp) invece di v*Δfifty/214 (fino a ~237cp).
static inline int tt_eval_undamp(int v, int fifty) {
    if (v == tt_eval_none) return v;
    if (fifty > 100) fifty = 100;
    return v * 214 / (200 - fifty);
}
static inline int tt_eval_redamp(int v, int fifty) {
    if (v == tt_eval_none) return v;
    if (fifty > 100) fifty = 100;
    return v * (200 - fifty) / 214;
}

// P2.2 (UCI "FastRepScan", default ON): td_is_repetition scandisce solo le ultime
// min(fifty, plies_from_null) entry invece dell'INTERA storia. fifty e' un bound
// esatto (una mossa irreversibile cambia il board per sempre: nessuna ricorrenza
// possibile oltre); plies_from_null esclude i segmenti oltre una null move (le
// null pushano entry "virtuali" senza toccare fifty — il vecchio scan completo
// le attraversava). Semantica = SF; node-count puo' cambiare di un soffio -> SPRT.
static bool g_fast_rep_scan = true;
void set_fast_rep_scan(bool v) { g_fast_rep_scan = v; }

// P1.12 (UCI "ThreadVoting", default OFF): selezione del risultato SMP per VOTO
// pesato stile SF invece di "vince il thread piu' profondo". Ogni thread vota la
// sua best move con peso (score - minScore + 14) * depth; vince la mossa col
// totale piu' alto (consenso fra thread > singolo thread profondo). Protezione
// matti: un matto provato vince direttamente (il piu' corto). Inerte a 1 thread.
// Default OFF = selezione legacy identica; SPRT a Threads=8 (convenzione SMP).
static bool g_thread_voting = false;
void set_thread_voting(bool v) { g_thread_voting = v; }

// ---- Toggle DA CO-TUNE per il mega-SPSA 4.0 (2026-06-12) ---------------------
// Strutture SF implementate dietro toggle default OFF = byte-identico. NON vanno
// A/B-ate da sole (lezione two-basin: bolt-on con costanti altrui = negativo,
// es. PawnHistory −18 bolt-on → +Elo dopo co-tune): si accendono nel co-tune
// con le loro costanti nel vettore SPSA, insieme a PriorBonus/LowPly.

// P1.3 (UCI "QSChecks"): alla PRIMA ply di qsearch tieni anche i QUIET CHECK
// diretti (check_sq + filtro SEE>=-75 anti-blunder). Tattica forzante vista
// prima, meno mate-blindness (SF: DEPTH_QS_CHECKS).
static bool g_qs_checks = false;
void set_qs_checks(bool v) { g_qs_checks = v; }

// P1.6 (UCI "NMPVerif"): robustezza null-move — (a) niente due null consecutive;
// (b) a depth >= NMPVerifDepth un fail-high della null va CONFERMATO da una
// search reale ridotta (anti-zugzwang; SF: nmpMinPly). Spin NMPVerifDepth.
static bool g_nmp_verif = false;
void set_nmp_verif(bool v) { g_nmp_verif = v; }
int g_nmp_verif_depth = 12;   // spin "NMPVerifDepth" (SPSA)

// P1.7 (UCI "LMPImproving"): move-count pruning SF-style (base + d^2*quad/100) /
// (2 - improving), SENZA il cap depth<=8 della lmp_table (oggi oltre d8 NESSUN
// move-count pruning = parte del fattore-nodi vs SF). Spins LMPBase/LMPQuad,
// scala comune LMPScale. Two-basin: costanti da co-tunare, non copiate.
static bool g_lmp_improving = false;
void set_lmp_improving(bool v) { g_lmp_improving = v; }
int g_lmp_base = 3;     // spin "LMPBase"
int g_lmp_quad = 100;   // spin "LMPQuad" (/100: 100 = d^2 pieno)

// P1.9 (spin "CheckExtDepth", default 128 = SEMPRE = comportamento storico):
// gate sulla check-extension incondizionata (SF l'ha rimossa ~10 anni fa).
// Il co-tune puo' abbassarlo (0 = mai estendere); a 128 e' byte-identico.
int g_check_ext_depth = 128;

// N1 (UCI "EvalCacheUndamp", default ON): eval-cache con chiave senza fifty e
// valore undamped (vedi td_evaluate). Piu' hit nei finali; OFF = legacy.
static bool g_evalcache_undamp = true;
void set_evalcache_undamp(bool v) { g_evalcache_undamp = v; }

// N2 (UCI "ProbCutTT", default ON): il fail-high di ProbCut viene SALVATO in TT
// (mossa, score, depth-3, bound beta, eval del nodo) come SF -> le rivisite
// della stessa posizione tagliano dal probe senza rifare qsearch+verifica.
static bool g_probcut_tt = true;
void set_probcut_tt(bool v) { g_probcut_tt = v; }

// P2.3 (UCI "EvasionGen", default ON): sotto scacco genera solo le pseudo-legali
// UTILI (re + catture dello scaccante + blocchi sul raggio; doppio scacco: solo
// re) intersecando le maschere ESISTENTI -> meno make falliti. VERIFICATO
// (2026-06-11, harness simmetrico order-sensitive, 0 mismatch su pos in-scacco
// d6 incl. captures-only): i flussi di mosse LEGALI (set E ordine) sono identici
// on/off a ogni chiamata. Residuo ±0.3% di node-count da effetti di 2° ordine
// non-semantici (bookkeeping) -> NON node-identical stretto: si valida con
// l'SPRT del bundle come gli altri toggle.
static bool g_evasion_gen = true;
void set_evasion_gen(bool v) { g_evasion_gen = v; }

// Eval-off DIAGNOSTIC toggle (UCI option "EvalOff", default false). Replaces the
// NNUE forward in td_evaluate() with a trivial material count, so an NPS test can
// measure how much per-node time is spent in evaluation (NNUE forward) vs the
// rest (movegen / make-unmake / mirror / search overhead). NOT for play.
static bool g_eval_off = false;
void set_eval_off(bool v) { g_eval_off = v; }

// Static-eval cache on/off (UCI option "EvalCache"). Default on; the toggle lets
// us A/B the cache (off vs on) back-to-back in a single build, immune to
// run-to-run NPS drift between separate builds.
static bool g_eval_cache = true;
void set_eval_cache(bool v) { g_eval_cache = v; }


// FinnyTables (NNUE accumulator refresh cache) on/off (UCI option "FinnyTables").
// Default OFF => byte-identical to the pre-finny engine. Forwards to the bridge
// (the cache lives per-thread in the SfPos mirror). Pure NPS lever: eval is
// bit-identical either way, so validate with an interleaved A/B NPS test.
void set_finny(bool v) { sf_set_finny(v ? 1 : 0); }


// "Improving" heuristic on/off (UCI option "Improving"). Default on; off
// reproduces the exact pre-heuristic search for a clean A/B from one build.
static bool g_improving = true;
void set_improving(bool v) { g_improving = v; }

// Node-based time management on/off (UCI option "NodeTM"). Default on; off
// reproduces the pure best-move-stability time logic for a clean A/B.
static bool g_node_tm = true;
void set_node_tm(bool v) { g_node_tm = v; }

// Advanced singular extensions on/off (UCI option "SingularExt"). Default on; off
// reproduces the plain single-ply singular extension for a clean A/B.
static bool g_singular_ext = true;
void set_singular_ext(bool v) { g_singular_ext = v; }

// Correction history on/off (UCI option "CorrHist"). Default ON (PROVISIONAL):
// the cap=32 / lr-div=512 version leaned +Elo (ultrabullet 2+0.02: ~+5.5, LOS ~81%
// @1750 games, CI still touching 0). Ultrabullet UNDERSELLS a depth-dependent
// correction, so adopted provisionally pending a longer-TC (8+0.08) SPRT. Toggle
// off for a clean A/B; tunable via CorrCap / CorrLearnDiv.
static bool g_corr_hist = true;
void set_corr_hist(bool v) { g_corr_hist = v; }

// Multi-table correction history on/off (UCI option "CorrHistMulti"). Default OFF
// reproduces the pawn-only correction. When ON, two extra correction tables keyed
// on minor (N/B) and major (R/Q) material are summed with the pawn table before
// clamping — the net mis-evaluates material configurations too, not just pawns.
static bool g_corr_multi = true;   // BAKED ON (2026-06-05): HM compound +6.2 LOS87.6% @1338
void set_corr_multi(bool v) { g_corr_multi = v; }

// Continuation correction history on/off (UCI option "CorrHistCont"). Default OFF =
// byte-identical (the cont_corr_hist table is never read/written when off). When ON,
// adds the SF continuation-correction term — keyed by the last two moves INTO the
// node (path-dependent static-eval correction) — to the corr sum. Its contribution is
// co-tunable via CorrContWeight (lets a co-tune balance cont vs pawn/minor/major).
static bool g_corr_cont = false;
void set_corr_cont(bool v) { g_corr_cont = v; }
int g_corr_cont_weight = 100;   // /100 del contributo cont alla somma corr. Spin CorrContWeight.

// PawnHistory (UCI "PawnHistory", default OFF = byte-identico). Termine di ordering
// per i quiet, pesato 2x come in SF, keyed sulla struttura pedonale. Tabella per-thread
// in ThreadData (SMP-safe). Indice ricalcolato in td_score_move dal board CORRENTE
// (durante lo scoring il board e' sempre quello del nodo -> niente staleness da ricorsione).
static bool g_pawn_hist = true;     // BAKED #1 2026-06-07 (era false): co-tune neutro@8 / +3@20+0.08
void set_pawn_hist(bool v) { g_pawn_hist = v; }
int g_pawn_hist_weight = 83;   // [3.7 BAKE 195->83] peso pawn-history /100 (200 = 2.0x come SF). Spin PawnHistoryWeight (granularita' /100, SPSA-friendly + permette frazioni <1).
// Peso della main (butterfly) history nello scoring quiet. SF la pesa 2x (come la pawn);
// noi storicamente 1x -> con pawn a 2x la pawn DOMINA la main = sbilanciato. Spin
// MainHistWeight per copiare il rapporto SF (main 2x, pawn 2x). Default 1 = byte-identico.
int g_mainhist_weight = 131;   // [3.7 BAKE 209->131] /100 (100 = 1.0x; SF usa 2.0x=200)
// Peso della continuation-history nello scoring quiet (ordering). Default 1 = invariato.
// Manopola del CO-TUNE: bilancia conthist vs main/pawn. NON tocca conthist in LMR/pruning.
int g_conthist_weight = 150;   // [3.7 BAKE 134->150] /100 (100 = 1.0x)
// Scala % della soglia LMP (late-move-pruning). Default 100 = invariato. <100 pota prima
// (albero più stretto), >100 pota dopo. Co-tune: si ri-equilibra con l'ordering nuovo.
int g_lmp_scale = 116;   // [3.7 BAKE 93->116]
// Forward-decl: td_corr_index (pawn-only Zobrist bucket) e' definita piu' sotto, ma
// serve qui sopra in td_score_move per la pawn-key.
static inline int td_corr_index(ThreadData& td);
// Forward-decl: td_corr_value serve anche in td_quiescence (P0.4), definita dopo.
static inline int td_corr_value(ThreadData& td, int idx);
// Forward-decl: init tabelle cuckoo/between per l'upcoming-repetition (P1.2),
// chiamata da init_threads (Zobrist + magic gia' inizializzati a quel punto).
static void init_cuckoo();

// ProbCut on/off (UCI option "ProbCut"). Default ON: validated +Elo (SPRT @8+0.08,
// LOS ~83% / leaning +6, textbook technique). When on: at sufficient depth, if a
// capture's reduced verification search beats beta+margin, the full-depth search
// would almost surely fail high too, so we prune the node early.
static bool g_probcut = true;
void set_probcut(bool v) { g_probcut = v; }

// Continuation-history pruning/reduction on/off (UCI option "ContHistPrune").
// Default ON (PROVISIONAL): tested jointly with CorrHist (ultrabullet ~+5.5, LOS
// ~81%); adopted provisionally pending a longer-TC SPRT. When on: (1) the LMR
// reduction also factors in the continuation history of the move w.r.t. the
// previous move (not just butterfly history), and (2) late quiet moves whose
// combined (butterfly + continuation) history is strongly negative are pruned at
// low depth. Toggle off for a clean A/B; tunable via ContHistDiv / HistPruneMargin.
static bool g_cont_hist_prune = true;
void set_cont_hist_prune(bool v) { g_cont_hist_prune = v; }

// Multi-ply continuation history on/off (UCI option "ContHistMulti"). Default OFF.
// When ON, move ordering and the cutoff history updates also use the move 2 and 4
// plies back (not just 1 ply), giving longer-range "this reply works after that
// sequence" signal. Tables persist like the 1-ply continuation history.
static bool g_conthist_multi = true;   // BAKED ON (2026-06-05): HM compound +6.2 LOS87.6% @1338
void set_conthist_multi(bool v) { g_conthist_multi = v; }

// Staged MovePicker on/off (UCI option "MovePicker"). ADOPTED, default ON: validated
// +16.6 Elo (LOS 96.6% @900 games, 5+0.1, vs the legacy path) once the Phase-2
// skip_quiets/skip_bad_caps were fixed + tested at a real TC (at hyperbullet it was
// ~0). When ON, moves are produced lazily by stage (TT -> good captures/promos ->
// killers -> counter -> quiets -> bad captures): an early cutoff never pays to
// generate/score the quiet moves, and LMP/futility skip the whole quiet stage. Set
// OFF to reproduce the old generate-all + pick-next ordering for an A/B.
static bool g_move_picker = true;
void set_move_picker(bool v) { g_move_picker = v; }

// DiverseSMP (#2): Lazy-SMP helper threads (id>0) search with a small per-thread
// LMR reduction bias, widening the ensemble of trees to cut redundant work at high
// thread counts. Thread 0 stays canonical. Default off (behaviour-preserving).
// **BAKED ON (2026-06-04, bake-on-trust):** wider-only diversity (bias<=0, no hijack)
// e' SEMPRE stato positivo (+3..+10, mai negativo) su ~1440 partite a 4t+8t. Feature
// SMP: inerte a 1 thread, agisce solo a multi-thread (= condizione gauntlet/torneo).
// Un [0,5] SPRT non puo' confermare un ~+3 (LLR a zonzo), ma il segno e' robusto.
// Toggle conservato per A/B (off = baseline). amount=1.
static bool g_diverse_smp = true;
void set_diverse_smp(bool v) { g_diverse_smp = v; }
static int g_diverse_smp_amount = 1;   // max |bias| in plies (SPSA-tunable: DiverseSMPAmount)

// --- Search "bundle" #3 — OUTCOME (2026-06-03): tested all-ON (-58 Elo), then isolated.
//  * MultiCut   — singular multi-cut, CONSERVATIVE gate (singular_beta >= beta): the only
//                 winner -> +10.4 ±11.2 Elo LOS 96.6% @1100 (1t, 8+0.08). BAKED default ON.
//  * TripleExt  — +3-ply singular: -75 Elo (tree blow-up). DEAD, kept default-off dormant.
//  * LMREnrich  — extra LMR on ttCapture/cut-node: -25 Elo (over-reduction). DEAD, dormant.
//  * MultiCutAggr (SF gate s>=beta): -13 Elo (half-depth fail-high too weak). REMOVED.
// (TripleExt −75, LMREnrich −25, DeeperShallower: misurati NEGATIVI/morti —
//  RIMOSSI nella pulizia 2026-06-11, vivono nella storia git.)
static bool g_multicut = true;   // BAKED ON: conservative singular multi-cut (+10.4 Elo @1100)
void set_multicut(bool v) { g_multicut = v; }

// ttPv (UCI option "TTPv"). Default OFF. SF: un bit della TT (bit 63 del data, era
// libero) ricorda se un nodo è/è stato PV. I nodi non-PV che la TT marca come ex-PV
// vengono ridotti meno in LMR (reduction--), perché un nodo già stato PV merita più
// attenzione. Il flag viene propagato negli store. OFF = bit sempre 0 + LMR invariata
// (byte-identico per un A/B pulito).
static bool g_ttpv = false;
void set_ttpv(bool v) { g_ttpv = v; }

// ===========================================================================
// CANDIDATI "notte" (2026-06-04) — ognuno dietro toggle, default OFF =
// byte-identico al baseline (zero rischio al motore). Da SPRT-are UNO ALLA VOLTA
// (vedi i .bat in Tuning_SPSA/night/). Dato che "il search è spremuto",
// aspettativa media ≤0, ma col toggle OFF non toccano nulla finché non li provi.
// ===========================================================================

// (1) NMPEvalScale: aumenta la riduzione del null-move quando static_eval supera
//     beta di molto (più sopra beta = potatura più aggressiva). SF-standard.
static bool g_nmp_eval_scale = false;
void set_nmp_eval_scale(bool v) { g_nmp_eval_scale = v; }
int g_nmp_eval_div = 200;   // R += min((static_eval - beta)/div, 3)

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
int g_qfut_margin = 150;

// (5) HistBonusSF: bonus history lineare-clampato min(mult*d - sub, max) invece
//     di depth*depth. Cambia solo la MAGNITUDINE di bonus/malus della history.
//     **BAKED ON (2026-06-04): +24.06 Elo LOS 99.99% @810 (8+0.08, 1t).** Primo
//     +Elo di ricerca dopo una settimana. Toggle conservato per A/B (off = depth*depth).
static bool g_hist_bonus_sf = true;
void set_hist_bonus_sf(bool v) { g_hist_bonus_sf = v; }
int g_hist_bonus_mult = 169;   // bakato SPSA 155->169 (block +5.2 LOS84% @1260)
int g_hist_bonus_sub  = 84;    // bakato SPSA 90->84
int g_hist_bonus_max  = 1720;  // bakato SPSA 1600->1720

// CaptureHist (UCI option "CaptureHist"). Capture history: tabella
// [piece][to][victim] che impara quali catture producono cutoff, e ne bias-a
// l'ordinamento (sia good che bad captures, main search + qsearch). Default ON,
// con DUE fix (2026-06-04) che hanno tolto una REGRESSIONE da ~-15/-21 Elo che il
// motore spediva:
//   (1) SCALING: il caphist era sommato GREZZO a mvv_lva (range +/-7000 vs 100-605
//       -> schiacciava l'MVV-LVA ~10x). `CaptureHistDiv` default 16 lo riporta a
//       +/-437 ~ mvv_lva (= scala SF (mvv*7+capthist)/16). A/B div1 ON vs OFF = -21;
//       div16 (solo scaling) = -12.
//   (2) MALUS: il malus alle catture provate-e-fallite era applicato solo quando il
//       cutoff era causato da una cattura; SF lo applica a OGNI cutoff (anche best
//       quiet). Vedi il loop nell'update. Con (1)+(2): div16+fix ON vs OFF = ~0
//       (neutro, -0.3@1098 e +0.8@458) -> regressione rimossa.
// Toggle conservato per A/B; OFF = contributo 0 + niente update (tabella a zero).
// `CaptureHistDiv` resta una leva SPSA (8/16/24) per un futuro tentativo di +Elo.
static bool g_capture_hist = true;
void set_capture_hist(bool v) { g_capture_hist = v; }
int g_caphist_div = 14;   // bakato SPSA 16->14 (rock-solid in convergenza)

// Lazy SMP is the ONLY parallel-search scheme (ADOPTED, +55 Elo @4CPU; direct A/B
// @2+0.02 4-thread was +102 Elo, LOS 99.99%). Helper threads search fully
// independently (shared TT only) and diversify via per-thread depth skipping (see
// LSMP_Skip* below). The legacy ABDADA busy-node coordination was removed.

// 4-way set-associative TT on/off (UCI option "TT4Way"). Default OFF reproduces
// the direct-mapped table; defined here, declared extern in tt.h (the bucket
// logic lives in tt.h). Matters more now that Lazy SMP has many threads hammering
// the TT. Toggle for a clean A/B.
bool g_tt_4way = false;
void set_tt_4way(bool v) { g_tt_4way = v; }

// Sentinella "nessuna static eval a questo ply" per eval_stack (FIX P0.6): scritta
// ai nodi in scacco quando LazyEval salta la NNUE. Fuori dal range eval reale.
#define EVAL_NONE (-32100)

// TTEvalImprove (UCI "TTEvalImprove", default ON — P1.1 2026-06-09): usa tt_score
// (se il bound e' coerente) al posto della static eval nelle decisioni di pruning.
// E' informazione di qualita'-search gia' pagata: SF lo fa da sempre. Toggle per A/B.
static bool g_tt_eval_improve = true;
void set_tt_eval_improve(bool v) { g_tt_eval_improve = v; }

// UpcomingRep (UCI "UpcomingRep", default ON — P1.2 2026-06-09): rilevazione
// "ripetizione imminente" (cuckoo, Kervinck/SF has_game_cycle): ai nodi non-root con
// alpha < 0, se esiste una mossa reversibile che riporta in una posizione gia' vista,
// alza alpha a 0 (lato anti-shuffling). Tabelle cuckoo inizializzate in init_threads.
static bool g_upcoming_rep = true;
void set_upcoming_rep(bool v) { g_upcoming_rep = v; }

// ---- TOGGLE DI ABLAZIONE dei fix 3.8 (night tournament, 2026-06-09) ----------
// Ogni fix P0 incondizionato della 3.8 e' gated da un toggle default ON: UN solo
// exe isola ogni fix via fastchess `option.X=false`. Tutti OFF (+ TTEvalImprove e
// UpcomingRep OFF) = search-identico alla 3.7 (node-identity verificabile).
bool g_ttmove24 = true;               // P0.1: TT move 24 bit (OFF = troncamento 21-bit come 3.7). extern in tt.h.
void set_ttmove24(bool v) { g_ttmove24 = v; }
bool g_see_fix = true;                // P0.2: SEE sui quiet + casa e.p. corretta (OFF = SEE=0 sui quiet). extern in see.cpp.
void set_see_fix(bool v) { g_see_fix = v; }
static bool g_killer_lmr_fix = true;  // P0.3: sconto LMR killer al ply del nodo (OFF = nessuno sconto, come il dead-code 3.7)
void set_killer_lmr_fix(bool v) { g_killer_lmr_fix = v; }
static bool g_qsearch_corr = true;    // P0.4: corr-history sullo stand-pat di qsearch (OFF = solo nodi interni)
void set_qsearch_corr(bool v) { g_qsearch_corr = v; }
static bool g_improving_fix = true;   // P0.6: sentinel EVAL_NONE + fallback ply-4 (OFF = confronto col vecchio 0)
void set_improving_fix(bool v) { g_improving_fix = v; }

// Lazy eval on/off (UCI option "LazyEval"). Default ON (CONFIRMED: combined with
// TimeMgmt, +9.1 Elo LOS 96% @1+0.01, estimate held over 3k+ games). Skips the
// NNUE static eval in nodes where the side to move is in check: there the static
// eval is not used by any forward-pruning rule (all gated !in_check), so computing
// it is wasted work -> a few % NPS for free.
static bool g_lazy_eval = true;
void set_lazy_eval(bool v) { g_lazy_eval = v; }

// Extra time management on/off (UCI option "TimeMgmt"). Default ON (CONFIRMED with
// LazyEval, +9.1 Elo LOS 96%). The main thread grants extra time when the root
// score DROPS vs the previous completed iteration (the position may be turning
// against us), on top of the existing best-move-instability extension.
static bool g_time_mgmt = true;
void set_time_mgmt(bool v) { g_time_mgmt = v; }

// Time-management constants, SPSA-tunable (mai tarate; default = valori a mano).
// Statici (usati in parse_go, uci_mt.cpp): quota base = remaining/TMMovesToGo +
// inc*TMIncFrac/100; maximum = optimum*TMMaxMult/100. Dinamici (loop ID, sotto):
// instabilita' best-move += headroom*TMInstab/100; eval che cala += headroom*d/TMDropDiv.
int g_tm_movestogo = 30;    // assunzione moves-to-go quando ignota (minore = piu' tempo/mossa)
int g_tm_inc_frac  = 75;    // % dell'incremento da spendere
int g_tm_max_mult  = 400;   // maximum = optimum * questo/100 (burst su posizioni difficili)
int g_tm_instab    = 50;    // % dell'headroom aggiunta al soft su cambio best-move
int g_tm_drop_div  = 800;   // divisore dell'estensione su score-drop (minore = piu' tempo)

// Aggressive LMR on/off (UCI "AggrLMR"). Default OFF. When ON, the history- and
// continuation-history-based LMR reductions use a SMALLER divisor and a WIDER
// clamp (g_aggr_lmr_div / g_aggr_lmr_clamp), so a very bad-history quiet can be
// reduced by several plies instead of just 1. Off reproduces the baseline exactly.
static bool g_aggr_lmr = false;
void set_aggr_lmr(bool v) { g_aggr_lmr = v; }

// ---- SPSA-tunable search parameters -----------------------------------------
// Exposed as UCI spin options (see set_search_param + uci_mt.cpp) so an external
// SPSA tuner (fastchess) can set them per-game without recompiling. Defaults =
// the current hand-set values. After a tuning run, bake the converged values in.
int g_rfp_margin = 21;    // reverse futility: static_eval - g*depth >= beta   [SPSA-tuned: 30->21]
int g_razor_base = 300;   // razoring: base + mult*depth below alpha -> qsearch
int g_razor_mult = 139;   // [SPSA-tuned: 102->139]
int g_fut_base = 111;   // futility: base + mult*depth (+improving bonus)      [SPSA-tuned: 82->111]
int g_fut_mult = 41;    // [3.7 BAKE 53->41; BAKED #1 66->53]
int g_fut_improving = 93;    // extra futility margin when improving                [SPSA-tuned: 60->93]
int g_singular_dmargin = 43;    // double-extension margin below singular_beta         [SPSA-tuned: 63->43]
int g_hist_red_div = 1041;  // LMR history-reduction divisor                      [SPSA-tuned: 3500->1041]
int g_asp_init_delta = 31;    // aspiration: initial window half-width               [SPSA-tuned: 25->31]
int g_asp_grow = 31;    // aspiration: growth % on fail                        [SPSA-tuned: 100->31]
int g_probcut_margin = 180;   // ProbCut: capture verification must beat beta by this margin
// Correction-history tunables (only active when CorrHist is on).
int g_corr_cap = 32;    // max correction applied to static eval (cp). [SPSA histmulti 32->44 RIGETTATO: -1.3 LOS23% @11438 -> default]
int g_corr_lr_div = 512;   // learning-rate divisor (bigger = slower/steadier learning) [histmulti 512->565 rigettato]
// Continuation-history pruning tunables (only active when ContHistPrune is on).
int g_conthist_red_div = 6595;  // LMR: continuation-history reduction divisor [SPSA-tuned 5000->6595; histmulti 6595->7050 rigettato]
// Aggressive-LMR tunables (only active when AggrLMR is on).
int g_aggr_lmr_div = 2048;  // smaller divisor -> wider reduction range
int g_aggr_lmr_clamp = 3;     // max +/- ply the history reductions may apply

// ---- StatScore-LMR levers (2026-06-06) --------------------------------------
// Scoperta: vs SF15.1 SOTTO-RIDUCIAMO -> ~6 ply meno profondi a pari tempo. SF
// guida la LMR con uno statScore CONTINUO (somma history / divisore, multi-ply)
// invece del nostro clamp +/-1. Tre toggle INDIPENDENTI (default OFF = byte-
// identico), ognuno A/B-abile da solo; lo SPSA tara i divisori dopo.
static bool g_statscore_lmr = true;    // BAKED #1 2026-06-07. StatScoreLMR: butterfly history -> riduzione continua (rimpiazza il clamp +/-1)
void set_statscore_lmr(bool v) { g_statscore_lmr = v; }
static bool g_conthist_lmr  = true;    // BAKED #1 2026-06-07. ContHistLMR: conthist(1/2/4 ply) -> riduzione continua, scollegata da ContHistPrune
void set_conthist_lmr(bool v) { g_conthist_lmr = v; }
static bool g_cutnode_lmr   = false;   // CutNodeLMR: riduzione extra sui cut-node (SF li riduce di piu')
void set_cutnode_lmr(bool v) { g_cutnode_lmr = v; }
// ---- Threat-ordering (#2 SF, 2026-06-07) ------------------------------------
// SF ordina i quiet anche per MINACCE: muovere un pezzo da una casa attaccata da uno
// di valore INFERIORE e' buono (lo salvi), entrarci e' cattivo (lo metti in presa).
// Struttura SF (regina vs minacce-di-torre, torre vs minore, minore vs pedone); UNA
// sola costante co-tunabile (ThreatScale) invece dei ~6 magici di SF -> "struttura SF,
// costante nostra". Default OFF = byte-identico. Minacce calcolate 1x/nodo (cache su
// hash_key) -> niente costo per-mossa.
static bool g_threat_ordering = true;   // BAKED FULL 2026-06-07
void set_threat_ordering(bool v) { g_threat_ordering = v; }
int g_threat_scale = 383;    // contributo = scale/100 * pieceValue * (from_minacciato - to_minacciato). Spin ThreatScale (SPSA). Default basso = nudge (1500 = +75% nodi a d20 = disturba l'ordering tarato); il co-tune trova il valore nostro.
// ---- Check-ordering (#3 SF, 2026-06-07) -------------------------------------
// SF da' un bonus ai quiet che danno SCACCO DIRETTO (forcing), filtrati per non essere
// blunder (SEE >= -75). Una sola costante co-tunabile (CheckBonus). check_sq[] (case da
// cui ogni tipo di pezzo da scacco al re nemico) calcolate 1x/nodo. Default OFF = byte-identico.
static bool g_check_ordering = true;    // BAKED FULL 2026-06-07
void set_check_ordering(bool v) { g_check_ordering = v; }
int g_check_bonus = 4201;    // bonus ordering per quiet che da scacco diretto (SF=16384). Spin CheckBonus (SPSA).
// ---- ContHist 3/6-ply (#4 SF, 2026-06-07) -----------------------------------
// SF somma la continuation-history a 1/2/3/4/6 ply; noi avevamo 1/2/4. #4 aggiunge
// 3-ply (move_stack[ply-2]) e 6-ply (move_stack[ply-5]) all'ordering quiet, scalati da
// ContHist36Weight e poi dal ContHistWeight comune. Update sui cutoff in
// td_conthist_multi_update. SCOPE: solo ordering (NON la ContHistLMR). Default OFF = byte-identico.
static bool g_conthist36 = true;        // BAKED FULL 2026-06-07
void set_conthist36(bool v) { g_conthist36 = v; }
int g_conthist36_weight = 37;   // /100: peso dei termini 3/6-ply relativo agli altri conthist. Spin ContHist36Weight.
// ---- V2: prior-counter-move bonus + capture-history bonus (SF, 2026-06-07) --------
// Su un nodo FAIL-LOW (nessuna mossa batte alpha) la mossa PRECEDENTE (quella che ha
// portato qui) era forte (ci ha messo in difficolta') -> bonus alla sua history. Quiet:
// main + continuation history; cattura: capture history (vittima da captured_stack).
// Una sola scala co-tunabile (PriorBonusScale) sul td_stat_bonus. Default OFF = byte-identico.
static bool g_prior_bonus = false;
void set_prior_bonus(bool v) { g_prior_bonus = v; }
int g_prior_bonus_scale = 100;   // /100 del td_stat_bonus(depth). Spin PriorBonusScale.
// ---- #5: low-ply history (SF) -----------------------------------------------------
// History per-ply usata SOLO nei primi LOW_PLY_MAX ply (vicino alla radice): cattura
// "questa mossa e' buona a questo ply basso". Peso /(1+ply). Default OFF = byte-identico.
static bool g_lowply = false;
void set_lowply(bool v) { g_lowply = v; }
int g_lowply_weight = 30;    // contributo ordering = g_lowply_weight * lowply / (100*(1+2*ply)) (SF-style decay). Spin LowPlyWeight.
int g_lmr_ss_div    = 7585;  // [3.7 BAKE 12104->7585; BAKED #1 era 7000]. StatScoreLMR: reduction -= (2*butterfly - offset) / div
int g_lmr_ss_offset = 2435;   // [3.7 BAKE 2668->2435; BAKED #1 era 4600]. StatScoreLMR: offset del punto neutro (SF sottrae ~4600 -> mossa media RIDOTTA di piu' = albero stretto/profondo, direzione SF)
int g_lmr_ch_div    = 3506;   // [3.7 BAKE 4437->3506; BAKED #1 era 10000]. ContHistLMR: reduction -= (conthist1+2+4) / div
int g_cutnode_lmr_extra = 1;  // CutNodeLMR: ply extra di riduzione sui cut-node (sopra il +1 esistente)
// NMP + LMR-enrichment tunables.
int g_nmp_base = 3;     // null-move reduction: R = g_nmp_base + depth/g_nmp_div
int g_nmp_div = 4;
int g_lmr_eval_margin = 100;   // LMR: reduce +1 more when static_eval + margin < alpha
int g_lmr_ttdepth = 2;     // LMR: reduce LESS by this when TT depth >= depth   [SPSA-tuned: 0->2]
// CORE LMR formula coefficients (*100).
int g_lmr_base_x100 = 37;    // baseline reduction floor [3.7 BAKE 41->37; SPSA 75->47; BAKED #1 47->41]
int g_lmr_div_x100 = 310;   // bigger divisor = LESS reduction [3.7 BAKE 345->310; SPSA 225->270; BAKED #1 270->345]
int g_histprune_margin = 1691;  // [3.7 BAKE 1602->1691; BAKED #1 era 1000]. history pruning: prune late quiet if combined hist < -margin*depth
// SEE-pruning margins (ALSO the Phase-2 skip_bad_caps lever): a move is SEE-pruned at
// low depth if SEE < -g_see_cap_margin*depth (captures) or < -g_see_quiet_margin*depth*depth
// (quiets). Exposed so SPSA can tune them (UCI: SEECaptureMargin / SEEQuietMargin).
int g_see_cap_margin   = 90;
int g_see_quiet_margin = 96;    // [3.7 BAKE 98->96; BAKED #1 era 50]
// Defined in sfnnue/evaluate.cpp: the eval picks the Big or Small NNUE by whether
// |simpleEval| exceeds this threshold. Exposed here so SPSA can tune it.
extern int g_small_net_threshold;

// Defined in sfnnue/evaluate.cpp: eval-wrapper constants (optimism/damping/blend)
// hand-tuned by SF for SF's search. Exposed so SPSA can re-tune them for OUR search.
extern int g_eval_optimism;
extern int g_eval_pawn_scale;
extern int g_eval_complexity_div;
extern int g_eval_blend_delta;

void init_lmr_table();   // defined below; re-run when LMR core coefficients change

// Dispatch a UCI spin option to the matching tunable. Returns true if matched.
bool set_search_param(const char* name, int value) {
    if (!strcmp(name, "RFPMargin"))           { g_rfp_margin       = value; return true; }
    if (!strcmp(name, "RazorBase"))           { g_razor_base       = value; return true; }
    if (!strcmp(name, "RazorMult"))           { g_razor_mult       = value; return true; }
    if (!strcmp(name, "FutilityBase"))        { g_fut_base         = value; return true; }
    if (!strcmp(name, "FutilityMult"))        { g_fut_mult         = value; return true; }
    if (!strcmp(name, "FutilityImproving"))   { g_fut_improving    = value; return true; }
    if (!strcmp(name, "SingularDoubleMargin")){ g_singular_dmargin = value; return true; }
    if (!strcmp(name, "HistReductionDiv"))    { g_hist_red_div     = value; return true; }
    if (!strcmp(name, "AspInitDelta"))        { g_asp_init_delta   = value; return true; }
    if (!strcmp(name, "AspGrow"))             { g_asp_grow         = value; return true; }
    if (!strcmp(name, "ProbCutMargin"))       { g_probcut_margin   = value; return true; }
    if (!strcmp(name, "CorrCap"))             { g_corr_cap         = value; return true; }
    if (!strcmp(name, "CorrLearnDiv"))        { g_corr_lr_div      = value; return true; }
    if (!strcmp(name, "CorrContWeight"))      { g_corr_cont_weight = value < 0 ? 0 : value; return true; }
    if (!strcmp(name, "ContHistDiv"))         { g_conthist_red_div = value; return true; }
    if (!strcmp(name, "HistPruneMargin"))     { g_histprune_margin = value; return true; }
    if (!strcmp(name, "SEECaptureMargin"))    { g_see_cap_margin   = value; return true; }
    if (!strcmp(name, "SEEQuietMargin"))      { g_see_quiet_margin = value; return true; }
    if (!strcmp(name, "SmallNetThreshold"))   { g_small_net_threshold = value; return true; }
    if (!strcmp(name, "EvalOptimism"))        { g_eval_optimism       = value; return true; }
    if (!strcmp(name, "EvalPawnScale"))       { g_eval_pawn_scale     = value; return true; }
    if (!strcmp(name, "EvalComplexityDiv"))   { g_eval_complexity_div = value < 1 ? 1 : value; return true; }
    if (!strcmp(name, "EvalBlendDelta"))      { g_eval_blend_delta    = value; return true; }
    if (!strcmp(name, "TMMovesToGo"))         { g_tm_movestogo = value < 1 ? 1 : value; return true; }
    if (!strcmp(name, "TMIncFrac"))           { g_tm_inc_frac        = value; return true; }
    if (!strcmp(name, "TMMaxMult"))           { g_tm_max_mult = value < 100 ? 100 : value; return true; }
    if (!strcmp(name, "TMInstab"))            { g_tm_instab          = value; return true; }
    if (!strcmp(name, "TMDropDiv"))           { g_tm_drop_div = value < 1 ? 1 : value; return true; }
    if (!strcmp(name, "TTCutBonusScale"))     { g_ttcut_bonus_scale = value < 0 ? 0 : value; return true; }
    if (!strcmp(name, "NMPVerifDepth"))       { g_nmp_verif_depth = value < 1 ? 1 : value; return true; }
    if (!strcmp(name, "LMPBase"))             { g_lmp_base = value < 0 ? 0 : value; return true; }
    if (!strcmp(name, "LMPQuad"))             { g_lmp_quad = value < 0 ? 0 : value; return true; }
    if (!strcmp(name, "CheckExtDepth"))       { g_check_ext_depth = value < 0 ? 0 : value; return true; }
    if (!strcmp(name, "LMRStatScoreDiv"))     { g_lmr_ss_div = value < 1 ? 1 : value; return true; }
    if (!strcmp(name, "LMRStatScoreOffset"))  { g_lmr_ss_offset      = value; return true; }
    if (!strcmp(name, "LMRContHistDiv"))      { g_lmr_ch_div = value < 1 ? 1 : value; return true; }
    if (!strcmp(name, "CutNodeLMRExtra"))     { g_cutnode_lmr_extra  = value; return true; }
    if (!strcmp(name, "ThreatScale"))         { g_threat_scale = value < 0 ? 0 : value; return true; }
    if (!strcmp(name, "CheckBonus"))          { g_check_bonus  = value < 0 ? 0 : value; return true; }
    if (!strcmp(name, "ContHist36Weight"))    { g_conthist36_weight = value < 0 ? 0 : value; return true; }
    if (!strcmp(name, "PriorBonusScale"))     { g_prior_bonus_scale = value < 0 ? 0 : value; return true; }
    if (!strcmp(name, "LowPlyWeight"))        { g_lowply_weight = value < 0 ? 0 : value; return true; }
    if (!strcmp(name, "PawnHistoryWeight"))   { g_pawn_hist_weight   = value; return true; }
    if (!strcmp(name, "MainHistWeight"))      { g_mainhist_weight    = value < 1 ? 1 : value; return true; }   // /100
    if (!strcmp(name, "ContHistWeight"))      { g_conthist_weight    = value < 1 ? 1 : value; return true; }   // /100
    if (!strcmp(name, "LMPScale"))            { g_lmp_scale          = value < 10 ? 10 : value; return true; }
    if (!strcmp(name, "AggrLMRDiv"))          { g_aggr_lmr_div     = value; return true; }
    if (!strcmp(name, "AggrLMRClamp"))        { g_aggr_lmr_clamp   = value; return true; }
    if (!strcmp(name, "NMPBase"))             { g_nmp_base         = value; return true; }
    if (!strcmp(name, "NMPDiv"))              { g_nmp_div          = value; return true; }
    if (!strcmp(name, "LMREvalMargin"))       { g_lmr_eval_margin  = value; return true; }
    if (!strcmp(name, "LMRTTDepth"))          { g_lmr_ttdepth      = value; return true; }
    if (!strcmp(name, "LMRBase"))             { g_lmr_base_x100 = value; init_lmr_table(); return true; }
    if (!strcmp(name, "LMRDiv"))              { g_lmr_div_x100  = value; init_lmr_table(); return true; }
    if (!strcmp(name, "DiverseSMPAmount"))    { g_diverse_smp_amount = value; return true; }
    if (!strcmp(name, "NMPEvalDiv"))          { g_nmp_eval_div     = value; return true; }
    if (!strcmp(name, "QFutMargin"))          { g_qfut_margin      = value; return true; }
    if (!strcmp(name, "HistBonusMult"))       { g_hist_bonus_mult  = value; return true; }
    if (!strcmp(name, "HistBonusSub"))        { g_hist_bonus_sub   = value; return true; }
    if (!strcmp(name, "HistBonusMax"))        { g_hist_bonus_max   = value; return true; }
    if (!strcmp(name, "CaptureHistDiv"))      { g_caphist_div = value < 1 ? 1 : value; return true; }
    return false;
}

// ---- Self-play data logging (Phase 1: policy-net training dataset) ----------
// When enabled, each completed root search appends one TSV record:
//   <FEN> \t <bestmove-uci> \t <score-cp (side-to-move relative)> \t <depth>
// Toggled via the DataLog / DataFile UCI options. The actual output file gets a
// ".<pid>" suffix per process, so multiple parallel self-play instances never
// collide; merge them afterwards (e.g. cat triumviratus_dataset.txt.* > all.txt).
static bool        g_data_log_enabled = false;
static std::string g_data_log_file    = "triumviratus_dataset.txt";

void set_data_log_enabled(bool enabled) { g_data_log_enabled = enabled; }
void set_data_log_file(const char* path) { if (path && *path) g_data_log_file = path; }

static void log_search_record(int best_move, int score, int depth) {
    // Per-process file (".<pid>") so parallel self-play instances never
    // interleave/corrupt one another. Merge later: cat <DataFile>.* > all.txt
#ifdef _WIN32
    unsigned long tri_pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long tri_pid = (unsigned long)getpid();
#endif
    std::string path = g_data_log_file + "." + std::to_string(tri_pid);
    std::ofstream out(path, std::ios::app | std::ios::binary);  // pure '\n'
    if (!out) return;
    out << board_to_fen() << '\t' << move_to_uci(best_move)
        << '\t' << score << '\t' << depth << '\n';
}
U64 hash_entries = 0;
int current_age = 0;

extern U64 nodes;

std::vector<std::thread> search_threads;
std::vector<ThreadData> thread_data;
std::atomic<bool> stop_threads(false);
std::mutex output_mutex;
int num_threads = 1;
int search_start_time = 0;
int soft_time_limit = 0;

// ============================================================================
// LMR TABLE
// ============================================================================

int lmr_table[64][64];

void init_lmr_table() {
    for (int depth = 0; depth < 64; depth++) {
        for (int moves = 0; moves < 64; moves++) {
            if (depth == 0 || moves == 0) {
                lmr_table[depth][moves] = 0;
            }
            else {
                lmr_table[depth][moves] = (int)(g_lmr_base_x100 / 100.0 + log(depth) * log(moves) / (g_lmr_div_x100 / 100.0));
            }
        }
    }
}

// LMP thresholds
const int lmp_table[9] = { 0, 3, 5, 8, 13, 20, 30, 42, 56 };

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

void init_threads(int thread_count) {
    if (thread_count < 1) thread_count = 1;
    if (thread_count > MAX_THREADS) thread_count = MAX_THREADS;

    // Free any existing incremental-NNUE handles before resizing (resize may
    // move/destroy elements); they are recreated fresh in the loop below.
    for (auto& td : thread_data)
        if (td.sfpos) { sf_pos_destroy(td.sfpos); td.sfpos = nullptr; }

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
        thread_data[i].sfpos = sf_pos_create();
        thread_data[i].nodes = 0;
        thread_data[i].best_move = 0;
        thread_data[i].best_score = -infinity;
        thread_data[i].depth = 0;
        memset(thread_data[i].killer_moves, 0, sizeof(thread_data[i].killer_moves));
        memset(thread_data[i].history_moves, 0, sizeof(thread_data[i].history_moves));
        memset(thread_data[i].counter_moves, 0, sizeof(thread_data[i].counter_moves));
        memset(thread_data[i].capture_history, 0, sizeof(thread_data[i].capture_history));
        memset(thread_data[i].pv_table, 0, sizeof(thread_data[i].pv_table));
        memset(thread_data[i].pv_length, 0, sizeof(thread_data[i].pv_length));
        memset(thread_data[i].eval_stack, 0, sizeof(thread_data[i].eval_stack));
        // Zero the eval cache once. Entries stay valid across searches (the
        // 64-bit key guards correctness), so we deliberately do NOT clear it
        // per-go — that preserves hits from transpositions across moves.
        memset(thread_data[i].eval_cache, 0, sizeof(thread_data[i].eval_cache));
        memset(thread_data[i].corr_hist, 0, sizeof(thread_data[i].corr_hist));
        memset(thread_data[i].corr_hist_minor, 0, sizeof(thread_data[i].corr_hist_minor));
        memset(thread_data[i].corr_hist_major, 0, sizeof(thread_data[i].corr_hist_major));
        memset(thread_data[i].cont_corr_hist, 0, sizeof(thread_data[i].cont_corr_hist));
        memset(thread_data[i].pawn_history, 0, sizeof(thread_data[i].pawn_history));
    }
}

void copy_board_to_thread(ThreadData& td) {
    memcpy(td.bitboards, bitboards, sizeof(bitboards));
    memcpy(td.occupancies, occupancies, sizeof(occupancies));
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
        while (bb) { int sq = get_ls1b_index(bb); td.pawn_key ^= piece_keys[pc][sq]; pop_bit(bb, sq); }
    }
    // P2.2: alla radice non ci sono null nel cammino -> finestra limitata solo
    // da fifty/storia. 1024 = "nessuna null vista".
    td.plies_from_null = 1024;
    td.in_nmp_verif = false;   // P1.6
    td.seldepth = 0;
    memcpy(td.repetition_table, repetition_table, sizeof(repetition_table));
    td.repetition_index = repetition_index;
    td.ply = 0;
    td.nodes = 0;
    td.best_move = 0;
    td.best_score = -infinity;
    td.depth = 0;
}

// ============================================================================
// ATTACK DETECTION
// ============================================================================

static inline int td_is_square_attacked(ThreadData& td, int square, int attacker_side) {
    if (attacker_side == white) {
        if (pawn_attacks[black][square] & td.bitboards[P]) return 1;
        if (knight_attacks[square] & td.bitboards[N]) return 1;
        if (king_attacks[square] & td.bitboards[K]) return 1;
        U64 occ = td.occupancies[both];
        if (get_bishop_attacks(square, occ) & (td.bitboards[B] | td.bitboards[Q])) return 1;
        if (get_rook_attacks(square, occ) & (td.bitboards[R] | td.bitboards[Q])) return 1;
    }
    else {
        if (pawn_attacks[white][square] & td.bitboards[p]) return 1;
        if (knight_attacks[square] & td.bitboards[n]) return 1;
        if (king_attacks[square] & td.bitboards[k]) return 1;
        U64 occ = td.occupancies[both];
        if (get_bishop_attacks(square, occ) & (td.bitboards[b] | td.bitboards[q])) return 1;
        if (get_rook_attacks(square, occ) & (td.bitboards[r] | td.bitboards[q])) return 1;
    }
    return 0;
}

// Incremental occupancy update. Flips only the bits that the move touches,
// instead of OR-recomputing all 12 bitboards (+2.89% NPS, node-identical). XOR is
// self-inverse, so calling it again with the same move reverses it (used on
// make-forward, illegal rollback, and unmake). 'us' must be the MOVING side at
// the call site.
// captured_piece == -1 means no capture. For castling the rook squares derive
// from the king target. Produces occupancies identical to the full recompute.
static inline void td_occ_update(ThreadData& td, int us, int source, int target,
                                 int captured_square, int captured_piece, int castling) {
    td.occupancies[us] ^= (1ULL << source) | (1ULL << target);
    if (captured_piece != -1)
        td.occupancies[us ^ 1] ^= (1ULL << captured_square);  // capture (== target, or e.p. square)
    if (castling) {
        int rf, rt;
        switch (target) {
            case g1: rf = h1; rt = f1; break;
            case c1: rf = a1; rt = d1; break;
            case g8: rf = h8; rt = f8; break;
            default: rf = a8; rt = d8; break;  // c8
        }
        td.occupancies[us] ^= (1ULL << rf) | (1ULL << rt);
    }
    td.occupancies[both] = td.occupancies[white] | td.occupancies[black];
}

// ============================================================================
// MAKE MOVE (returns 1 if legal)
// ============================================================================

// P2.1: aggiorna td.pawn_key (Zobrist solo-pedoni) via XOR. Self-inverse -> stessa
// chiamata (stessi argomenti) nei 3 siti di td_occ_update: make-forward, rollback
// della mossa illegale, unmake. Pedone mosso: via da source, atterra su target solo
// se NON promuove. Pedone catturato (anche e.p.): via da captured_square. Castling
// e null move non toccano pedoni. Consumata da td_corr_index (g_pawn_key_incr).
static inline void td_pawn_key_update(ThreadData& td, int piece, int source, int target,
                                      int promoted, int captured_piece, int captured_square) {
    if (piece == P || piece == p) {
        td.pawn_key ^= piece_keys[piece][source];
        if (!promoted) td.pawn_key ^= piece_keys[piece][target];
    }
    if (captured_piece == P || captured_piece == p)
        td.pawn_key ^= piece_keys[captured_piece][captured_square];
}

static inline int td_make_move(ThreadData& td, int move, UndoInfo& undo) {
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
    if (piece == P || piece == p) td.fifty = 0;

    if (capture) {
        td.fifty = 0;

        if (enpass) {
            undo.captured_square = (td.side == white) ? target + 8 : target - 8;
            undo.captured_piece = (td.side == white) ? p : P;
        }
        else {
            undo.captured_square = target;
            int start = (td.side == white) ? p : P;
            int end = (td.side == white) ? k : K;
            for (int pc = start; pc <= end; pc++) {
                if (get_bit(td.bitboards[pc], target)) {
                    undo.captured_piece = pc;
                    break;
                }
            }
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

    if (td.enpassant != no_sq) td.hash_key ^= enpassant_keys[td.enpassant];
    td.enpassant = no_sq;

    if (double_push) {
        td.enpassant = (td.side == white) ? target + 8 : target - 8;
        td.hash_key ^= enpassant_keys[td.enpassant];
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

    td_occ_update(td, td.side, source, target, undo.captured_square, undo.captured_piece, castling);
    if (g_pawn_key_incr)
        td_pawn_key_update(td, piece, source, target, promoted, undo.captured_piece, undo.captured_square);

    td.side ^= 1;
    td.hash_key ^= side_key;

    int king_sq = get_ls1b_index((td.side == white) ? td.bitboards[k] : td.bitboards[K]);
    if (td_is_square_attacked(td, king_sq, td.side)) {
        td.side ^= 1;

        if (promoted) {
            pop_bit(td.bitboards[promoted], target);
            set_bit(td.bitboards[piece], source);
        }
        else {
            pop_bit(td.bitboards[piece], target);
            set_bit(td.bitboards[piece], source);
        }

        if (undo.captured_piece != -1) {
            set_bit(td.bitboards[undo.captured_piece], undo.captured_square);
        }

        if (castling) {
            switch (target) {
            case g1: pop_bit(td.bitboards[R], f1); set_bit(td.bitboards[R], h1); break;
            case c1: pop_bit(td.bitboards[R], d1); set_bit(td.bitboards[R], a1); break;
            case g8: pop_bit(td.bitboards[r], f8); set_bit(td.bitboards[r], h8); break;
            case c8: pop_bit(td.bitboards[r], d8); set_bit(td.bitboards[r], a8); break;
            }
        }

        td.castle = undo.old_castle;
        td.enpassant = undo.old_enpassant;
        td.fifty = undo.old_fifty;
        td.hash_key = undo.old_hash;

        td_occ_update(td, td.side, source, target, undo.captured_square, undo.captured_piece, castling);
        if (g_pawn_key_incr)
            td_pawn_key_update(td, piece, source, target, promoted, undo.captured_piece, undo.captured_square);

        return 0;
    }

    // (TT + eval-cache prefetch tried here 2026-06-07: NODE-IDENTICAL but NPS-neutral
    //  — we are eval-bound, not TT-memory-bound, so hiding TT latency doesn't help.
    //  Removed. The T0/pre-legality variant was -2.5% from L1 pollution.)

    // Mirror the (now legal) move on the incremental NNUE position, in
    // Stockfish encoding. The moving piece is dirtyPiece[0] (king-refresh).
    {
        SfMove sm;
        sm.movedPiece    = sf_piece_code[piece];
        sm.from          = nnue_squares[source];
        sm.to            = nnue_squares[target];
        sm.promoPiece    = promoted ? sf_piece_code[promoted] : 0;
        sm.capturedPiece = (undo.captured_piece != -1) ? sf_piece_code[undo.captured_piece] : 0;
        sm.capturedSq    = (undo.captured_piece != -1) ? nnue_squares[undo.captured_square] : -1;
        sm.rookPiece = 0; sm.rookFrom = -1; sm.rookTo = -1;
        if (castling) {
            int rpc = R, rf = h1, rt = f1;
            switch (target) {
                case g1: rpc = R; rf = h1; rt = f1; break;
                case c1: rpc = R; rf = a1; rt = d1; break;
                case g8: rpc = r; rf = h8; rt = f8; break;
                case c8: rpc = r; rf = a8; rt = d8; break;
            }
            sm.rookPiece = sf_piece_code[rpc];
            sm.rookFrom  = nnue_squares[rf];
            sm.rookTo    = nnue_squares[rt];
        }
        sm.rule50 = td.fifty;
        sf_pos_do(td.sfpos, &sm);
    }

    td.plies_from_null++;   // P2.2: una mossa reale in piu' dall'ultima null

    return 1;
}

// ============================================================================
// UNMAKE MOVE
// ============================================================================

static inline void td_unmake_move(ThreadData& td, int move, UndoInfo& undo) {
    sf_pos_undo(td.sfpos);   // retract the move on the incremental NNUE mirror
    td.plies_from_null--;    // P2.2 (speculare al ++ del make riuscito)
    td.side ^= 1;

    int source = get_move_source(move);
    int target = get_move_target(move);
    int piece = get_move_piece(move);
    int promoted = get_move_promoted(move);
    int castling = get_move_castling(move);

    if (promoted) {
        pop_bit(td.bitboards[promoted], target);
        set_bit(td.bitboards[piece], source);
    }
    else {
        pop_bit(td.bitboards[piece], target);
        set_bit(td.bitboards[piece], source);
    }

    if (undo.captured_piece != -1) {
        set_bit(td.bitboards[undo.captured_piece], undo.captured_square);
    }

    if (castling) {
        switch (target) {
        case g1: pop_bit(td.bitboards[R], f1); set_bit(td.bitboards[R], h1); break;
        case c1: pop_bit(td.bitboards[R], d1); set_bit(td.bitboards[R], a1); break;
        case g8: pop_bit(td.bitboards[r], f8); set_bit(td.bitboards[r], h8); break;
        case c8: pop_bit(td.bitboards[r], d8); set_bit(td.bitboards[r], a8); break;
        }
    }

    td.castle = undo.old_castle;
    td.enpassant = undo.old_enpassant;
    td.fifty = undo.old_fifty;
    td.hash_key = undo.old_hash;

    td_occ_update(td, td.side, source, target, undo.captured_square, undo.captured_piece, castling);
    if (g_pawn_key_incr)
        td_pawn_key_update(td, piece, source, target, promoted, undo.captured_piece, undo.captured_square);
}

// ============================================================================
// MOVE GENERATION
// ============================================================================

// Case strettamente tra due quadrati allineati (0 per cavallo/non-allineati).
// Riempita da init_cuckoo; usata dall'upcoming-repetition (P1.2) e dalle
// evasioni (P2.3).
static U64 between_tbl[64][64];

// Bitboard degli attaccanti di `sq` del colore `by` (P2.3 EvasionGen).
static inline U64 td_attackers_to(ThreadData& td, int sq, int by) {
    const int off = (by == white) ? 0 : 6;
    const U64 occ = td.occupancies[both];
    return (pawn_attacks[by ^ 1][sq]        & td.bitboards[P + off])
         | (knight_attacks[sq]              & td.bitboards[N + off])
         | (king_attacks[sq]                & td.bitboards[K + off])
         | (get_bishop_attacks(sq, occ)     & (td.bitboards[B + off] | td.bitboards[Q + off]))
         | (get_rook_attacks(sq, occ)       & (td.bitboards[R + off] | td.bitboards[Q + off]));
}

static void td_generate_moves(ThreadData& td, moves* move_list, bool captures_only = false) {
    move_list->count = 0;
    int source_square, target_square;
    U64 bitboard, attacks;

    // --- MAGIA BITWISE: Precalcoliamo le maschere ---
    U64 enemies = (td.side == white) ? td.occupancies[black] : td.occupancies[white];
    U64 friends = (td.side == white) ? td.occupancies[white] : td.occupancies[black];
    // Se vogliamo solo catture, le uniche case valide sono quelle nemiche.
    // Altrimenti, tutte le case tranne le nostre.
    U64 allowed_squares = captures_only ? enemies : ~friends;

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
        const int ksq = get_ls1b_index((td.side == white) ? td.bitboards[K] : td.bitboards[k]);
        if (td_is_square_attacked(td, ksq, td.side ^ 1)) {
            U64 checkers = td_attackers_to(td, ksq, td.side ^ 1);
            if (checkers & (checkers - 1)) {
                evasion_mask = 0ULL;                       // doppio scacco: solo il re
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
                        if (!(target_square < a8) && !get_bit(td.occupancies[both], target_square)) {
                            if (source_square >= a7 && source_square <= h7) {
                                if ((evasion_mask >> target_square) & 1) {
                                    add_move(move_list, encode_move(source_square, target_square, piece, Q, 0, 0, 0, 0));
                                    add_move(move_list, encode_move(source_square, target_square, piece, R, 0, 0, 0, 0));
                                    add_move(move_list, encode_move(source_square, target_square, piece, B, 0, 0, 0, 0));
                                    add_move(move_list, encode_move(source_square, target_square, piece, N, 0, 0, 0, 0));
                                }
                            }
                            else {
                                if ((evasion_mask >> target_square) & 1)
                                    add_move(move_list, encode_move(source_square, target_square, piece, 0, 0, 0, 0, 0));
                                if ((source_square >= a2 && source_square <= h2) && !get_bit(td.occupancies[both], target_square - 8) &&
                                    ((evasion_mask >> (target_square - 8)) & 1))
                                    add_move(move_list, encode_move(source_square, target_square - 8, piece, 0, 0, 1, 0, 0));
                            }
                        }
                    }

                    // Catture dei pedoni
                    attacks = pawn_attacks[td.side][source_square] & enemies & evasion_mask;
                    while (attacks) {
                        target_square = get_ls1b_index(attacks);
                        if (source_square >= a7 && source_square <= h7) {
                            add_move(move_list, encode_move(source_square, target_square, piece, Q, 1, 0, 0, 0));
                            add_move(move_list, encode_move(source_square, target_square, piece, R, 1, 0, 0, 0));
                            add_move(move_list, encode_move(source_square, target_square, piece, B, 1, 0, 0, 0));
                            add_move(move_list, encode_move(source_square, target_square, piece, N, 1, 0, 0, 0));
                        }
                        else {
                            add_move(move_list, encode_move(source_square, target_square, piece, 0, 1, 0, 0, 0));
                        }
                        pop_bit(attacks, target_square);
                    }

                    // En passant (  sempre una cattura, lo generiamo sempre)
                    if (td.enpassant != no_sq) {
                        U64 enpassant_attacks = pawn_attacks[td.side][source_square] & (1ULL << td.enpassant);
                        if (enpassant_attacks) {
                            int target_enpassant = get_ls1b_index(enpassant_attacks);
                            add_move(move_list, encode_move(source_square, target_enpassant, piece, 0, 1, 0, 1, 0));
                        }
                    }
                    pop_bit(bitboard, source_square);
                }
            }
            if (piece == K) {
                // Arrocco: bloccato se captures_only   true
                if (!captures_only) {
                    if (td.castle & wk) {
                        if (!get_bit(td.occupancies[both], f1) && !get_bit(td.occupancies[both], g1)) {
                            if (!td_is_square_attacked(td, e1, black) && !td_is_square_attacked(td, f1, black))
                                add_move(move_list, encode_move(e1, g1, piece, 0, 0, 0, 0, 1));
                        }
                    }
                    if (td.castle & wq) {
                        if (!get_bit(td.occupancies[both], d1) && !get_bit(td.occupancies[both], c1) && !get_bit(td.occupancies[both], b1)) {
                            if (!td_is_square_attacked(td, e1, black) && !td_is_square_attacked(td, d1, black))
                                add_move(move_list, encode_move(e1, c1, piece, 0, 0, 0, 0, 1));
                        }
                    }
                }
            }
        }
        else {
            if (piece == p) {
                while (bitboard) {
                    source_square = get_ls1b_index(bitboard);
                    target_square = source_square + 8;

                    if (!captures_only) {
                        if (!(target_square > h1) && !get_bit(td.occupancies[both], target_square)) {
                            if (source_square >= a2 && source_square <= h2) {
                                if ((evasion_mask >> target_square) & 1) {
                                    add_move(move_list, encode_move(source_square, target_square, piece, q, 0, 0, 0, 0));
                                    add_move(move_list, encode_move(source_square, target_square, piece, r, 0, 0, 0, 0));
                                    add_move(move_list, encode_move(source_square, target_square, piece, b, 0, 0, 0, 0));
                                    add_move(move_list, encode_move(source_square, target_square, piece, n, 0, 0, 0, 0));
                                }
                            }
                            else {
                                if ((evasion_mask >> target_square) & 1)
                                    add_move(move_list, encode_move(source_square, target_square, piece, 0, 0, 0, 0, 0));
                                if ((source_square >= a7 && source_square <= h7) && !get_bit(td.occupancies[both], target_square + 8) &&
                                    ((evasion_mask >> (target_square + 8)) & 1))
                                    add_move(move_list, encode_move(source_square, target_square + 8, piece, 0, 0, 1, 0, 0));
                            }
                        }
                    }

                    attacks = pawn_attacks[td.side][source_square] & enemies & evasion_mask;
                    while (attacks) {
                        target_square = get_ls1b_index(attacks);
                        if (source_square >= a2 && source_square <= h2) {
                            add_move(move_list, encode_move(source_square, target_square, piece, q, 1, 0, 0, 0));
                            add_move(move_list, encode_move(source_square, target_square, piece, r, 1, 0, 0, 0));
                            add_move(move_list, encode_move(source_square, target_square, piece, b, 1, 0, 0, 0));
                            add_move(move_list, encode_move(source_square, target_square, piece, n, 1, 0, 0, 0));
                        }
                        else {
                            add_move(move_list, encode_move(source_square, target_square, piece, 0, 1, 0, 0, 0));
                        }
                        pop_bit(attacks, target_square);
                    }
                    if (td.enpassant != no_sq) {
                        U64 enpassant_attacks = pawn_attacks[td.side][source_square] & (1ULL << td.enpassant);
                        if (enpassant_attacks) {
                            int target_enpassant = get_ls1b_index(enpassant_attacks);
                            add_move(move_list, encode_move(source_square, target_enpassant, piece, 0, 1, 0, 1, 0));
                        }
                    }
                    pop_bit(bitboard, source_square);
                }
            }
            if (piece == k) {
                if (!captures_only) {
                    if (td.castle & bk) {
                        if (!get_bit(td.occupancies[both], f8) && !get_bit(td.occupancies[both], g8)) {
                            if (!td_is_square_attacked(td, e8, white) && !td_is_square_attacked(td, f8, white))
                                add_move(move_list, encode_move(e8, g8, piece, 0, 0, 0, 0, 1));
                        }
                    }
                    if (td.castle & bq) {
                        if (!get_bit(td.occupancies[both], d8) && !get_bit(td.occupancies[both], c8) && !get_bit(td.occupancies[both], b8)) {
                            if (!td_is_square_attacked(td, e8, white) && !td_is_square_attacked(td, d8, white))
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
                    if (!get_bit(enemies, target_square)) add_move(move_list, encode_move(source_square, target_square, piece, 0, 0, 0, 0, 0));
                    else add_move(move_list, encode_move(source_square, target_square, piece, 0, 1, 0, 0, 0));
                    pop_bit(attacks, target_square);
                }
                pop_bit(bitboard, source_square);
            }
        }

        if ((td.side == white) ? piece == B : piece == b) {
            while (bitboard) {
                source_square = get_ls1b_index(bitboard);
                attacks = get_bishop_attacks(source_square, td.occupancies[both]) & piece_allowed;
                while (attacks) {
                    target_square = get_ls1b_index(attacks);
                    if (!get_bit(enemies, target_square)) add_move(move_list, encode_move(source_square, target_square, piece, 0, 0, 0, 0, 0));
                    else add_move(move_list, encode_move(source_square, target_square, piece, 0, 1, 0, 0, 0));
                    pop_bit(attacks, target_square);
                }
                pop_bit(bitboard, source_square);
            }
        }

        if ((td.side == white) ? piece == R : piece == r) {
            while (bitboard) {
                source_square = get_ls1b_index(bitboard);
                attacks = get_rook_attacks(source_square, td.occupancies[both]) & piece_allowed;
                while (attacks) {
                    target_square = get_ls1b_index(attacks);
                    if (!get_bit(enemies, target_square)) add_move(move_list, encode_move(source_square, target_square, piece, 0, 0, 0, 0, 0));
                    else add_move(move_list, encode_move(source_square, target_square, piece, 0, 1, 0, 0, 0));
                    pop_bit(attacks, target_square);
                }
                pop_bit(bitboard, source_square);
            }
        }

        if ((td.side == white) ? piece == Q : piece == q) {
            while (bitboard) {
                source_square = get_ls1b_index(bitboard);
                attacks = get_queen_attacks(source_square, td.occupancies[both]) & piece_allowed;
                while (attacks) {
                    target_square = get_ls1b_index(attacks);
                    if (!get_bit(enemies, target_square)) add_move(move_list, encode_move(source_square, target_square, piece, 0, 0, 0, 0, 0));
                    else add_move(move_list, encode_move(source_square, target_square, piece, 0, 1, 0, 0, 0));
                    pop_bit(attacks, target_square);
                }
                pop_bit(bitboard, source_square);
            }
        }

        if ((td.side == white) ? piece == K : piece == k) {
            while (bitboard) {
                source_square = get_ls1b_index(bitboard);
                attacks = king_attacks[source_square] & allowed_squares;
                while (attacks) {
                    target_square = get_ls1b_index(attacks);
                    if (!get_bit(enemies, target_square)) add_move(move_list, encode_move(source_square, target_square, piece, 0, 0, 0, 0, 0));
                    else add_move(move_list, encode_move(source_square, target_square, piece, 0, 1, 0, 0, 0));
                    pop_bit(attacks, target_square);
                }
                pop_bit(bitboard, source_square);
            }
        }
    }

}

// ============================================================================
// EVALUATION
// ============================================================================

// Build the Stockfish piece list (codes + squares) from a thread's board.
// Returns the number of pieces written into pieces[]/squares[].
static inline int sf_build_piece_list(ThreadData& td, int pieces[33], int squares[33]) {
    int count = 0;
    for (int bb_piece = P; bb_piece <= k; bb_piece++) {
        U64 bb = td.bitboards[bb_piece];
        while (bb) {
            int sq = get_ls1b_index(bb);
            pieces[count]  = sf_piece_code[bb_piece];
            squares[count] = nnue_squares[sq];   // engine square -> SF square (a1=0..h8=63)
            count++;
            pop_bit(bb, sq);
        }
    }
    return count;
}

// DIAGNOSTIC ("eval" UCI command): static NNUE eval of the CURRENT global board
// (the one parse_fen / "position" populate). Builds the piece list straight from
// the global bitboards and calls the stateless oracle sf_eval (whose value is,
// by the NNUE_VERIFY invariant, identical to the incremental sf_pos_eval =
// Eval::evaluate, i.e. SF's "Final evaluation"). Returns centipawns relative to
// the side to move. Used to cross-check the SFNNv9/3072 port against the official
// Stockfish binary on identical FENs (a correct port agrees up to SF's UCI
// pawn-normalisation constant).
int debug_eval_position() {
    int pieces[33], squares[33];
    int count = 0;
    for (int bb_piece = P; bb_piece <= k; bb_piece++) {
        U64 bb = bitboards[bb_piece];          // GLOBAL board set by parse_fen
        while (bb) {
            int sq = get_ls1b_index(bb);
            pieces[count]  = sf_piece_code[bb_piece];
            squares[count] = nnue_squares[sq];
            count++;
            pop_bit(bb, sq);
        }
    }
    return sf_eval(side == white, pieces, squares, count, fifty);
}

// Re-sync the incremental NNUE mirror to the thread's current board (full
// refresh). Called at the root of each iterative-deepening iteration.
static inline void sf_root_sync(ThreadData& td) {
    int pieces[33], squares[33];
    int count = sf_build_piece_list(td, pieces, squares);
    sf_pos_set(td.sfpos, td.side == white, pieces, squares, count, td.fifty);
}

static inline int td_evaluate(ThreadData& td) {
    // DIAGNOSTIC: skip the NNUE forward, return a cheap material eval. Used by the
    // "EvalOff" UCI option to isolate evaluation cost in an NPS profile.
    if (g_eval_off) {
        static const int mat[12] = { 100,320,330,500,900,0, 100,320,330,500,900,0 };
        int s = 0;
        for (int p = 0; p < 6;  p++) s += mat[p] * count_bits(td.bitboards[p]);
        for (int p = 6; p < 12; p++) s -= mat[p] * count_bits(td.bitboards[p]);
        return (td.side == white) ? s : -s;
    }
#ifdef NNUE_VERIFY
    // Verification oracle: the incrementally maintained eval must EXACTLY equal
    // a full from-scratch refresh. Build with /DNNUE_VERIFY to enable.
    int pieces[33], squares[33];
    int count = sf_build_piece_list(td, pieces, squares);
    int full  = sf_eval(td.side == white, pieces, squares, count, td.fifty);
    int inc   = sf_pos_eval(td.sfpos, td.bitboards, td.occupancies);
    if (full != inc) {
        std::cerr << "NNUE MISMATCH ply=" << td.ply << " full=" << full
                  << " inc=" << inc << " hash=0x" << std::hex << td.hash_key << std::dec
                  << std::endl;
        std::exit(2);
    }
    return inc;
#else
    // Static-eval cache: hash_key mixed with fifty (see threads.h EvalCacheEntry
    // for why fifty is in the key). A hit skips the NNUE forward pass entirely;
    // safe because sf_pos_do/undo keep the lazy accumulator's dirty chain intact
    // whether or not we call sf_pos_eval here.
    if (g_eval_cache) {
        if (g_evalcache_undamp) {
            // N1 (2026-06-12): chiave SENZA fifty, valore UNDAMPED (rule50-indip.)
            // ri-smorzato col fifty corrente alla lettura — stesso design del
            // TT-eval16. Prima il fifty era nella chiave -> la stessa posizione a
            // fifty diverso era un MISS sistematico (finali shuffle). Costo: ±2cp
            // di arrotondamento sul roundtrip; il valore fresco resta esatto.
            ThreadData::EvalCacheEntry& ce = td.eval_cache[td.hash_key & ThreadData::EVAL_CACHE_MASK];
            if (ce.key == td.hash_key) return tt_eval_redamp(ce.eval, td.fifty);
            const int v = sf_pos_eval(td.sfpos, td.bitboards, td.occupancies);
            ce.key = td.hash_key;
            ce.eval = tt_eval_undamp(v, td.fifty);
            return v;
        }
        const U64 ck = td.hash_key ^ (0x9E3779B97F4A7C15ULL * (U64)(td.fifty + 1));
        ThreadData::EvalCacheEntry& ce = td.eval_cache[ck & ThreadData::EVAL_CACHE_MASK];
        if (ce.key == ck) return ce.eval;
        const int v = sf_pos_eval(td.sfpos, td.bitboards, td.occupancies);
        ce.key = ck;
        ce.eval = v;
        return v;
    }
    // Incremental: the accumulator is updated only for the pieces that changed
    // (HalfKAv2_hm king-bucket refresh handled by Stockfish's transform()).
    return sf_pos_eval(td.sfpos, td.bitboards, td.occupancies);
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
static inline void td_update_history(int& h, int bonus) {
    if (bonus > HISTORY_MAX) bonus = HISTORY_MAX;
    else if (bonus < -HISTORY_MAX) bonus = -HISTORY_MAX;
    h += bonus - h * (bonus < 0 ? -bonus : bonus) / HISTORY_MAX;
}

// Same gravity update for the int16_t continuation-history entries.
static inline void td_update_history(int16_t& h, int bonus) {
    if (bonus > HISTORY_MAX) bonus = HISTORY_MAX;
    else if (bonus < -HISTORY_MAX) bonus = -HISTORY_MAX;
    int v = (int)h + bonus - (int)h * (bonus < 0 ? -bonus : bonus) / HISTORY_MAX;
    h = (int16_t)v;
}

// Stat bonus per gli update history. Default = depth*depth (originale). Con
// HistBonusSF on usa la forma SF lineare-clampata min(mult*d - sub, max).
static inline int td_stat_bonus(int depth) {
    if (g_hist_bonus_sf) {
        int b = g_hist_bonus_mult * depth - g_hist_bonus_sub;
        if (b > g_hist_bonus_max) b = g_hist_bonus_max;
        if (b < 0) b = 0;
        return b;
    }
    return depth * depth;
}

// Move-ordering score bands (well separated so the additive quiet histories,
// range about +/-2*HISTORY_MAX, never overlap the "special" move scores):
//   TT move > good captures > killers > counter-move > quiet history > bad captures
#define SCORE_TT_MOVE       2000000
#define SCORE_GOOD_CAPTURE   700000
#define SCORE_KILLER0        600000
#define SCORE_KILLER1        590000
#define SCORE_COUNTER        580000
#define SCORE_BAD_CAPTURE   (-700000)

// Pezzo catturato sulla casa target (en passant -> P, coerente con lo scoring).
static inline int td_captured_piece(ThreadData& td, int target) {
    int start = (td.side == white) ? p : P;
    int end   = (td.side == white) ? k : K;
    for (int pc = start; pc <= end; pc++)
        if (get_bit(td.bitboards[pc], target)) return pc;
    return P;
}

// Threat-ordering helper: fill td.threat_by_{pawn,minor,rook} = squares attacked by
// enemy pieces grouped by the cheapest attacker's value. Called once per node (the
// caller checks td.threat_key == td.hash_key first). Only reached when ThreatOrdering
// is on, so it costs nothing when the toggle is off.
static inline void td_compute_threats(ThreadData& td) {
    const int them = td.side ^ 1;          // opponent color (white=0 / black=1)
    const int base = them * 6;             // white pieces P..K at 0, black p..k at 6
    const U64 occ  = td.occupancies[both];
    U64 byP = 0, byM, byR, bb;

    bb = td.bitboards[base + P];           // enemy pawns
    while (bb) { int s = get_ls1b_index(bb); pop_bit(bb, s); byP |= pawn_attacks[them][s]; }
    byM = byP;
    bb = td.bitboards[base + N];           // enemy knights
    while (bb) { int s = get_ls1b_index(bb); pop_bit(bb, s); byM |= knight_attacks[s]; }
    bb = td.bitboards[base + B];           // enemy bishops
    while (bb) { int s = get_ls1b_index(bb); pop_bit(bb, s); byM |= get_bishop_attacks(s, occ); }
    byR = byM;
    bb = td.bitboards[base + R];           // enemy rooks
    while (bb) { int s = get_ls1b_index(bb); pop_bit(bb, s); byR |= get_rook_attacks(s, occ); }

    td.threat_by_pawn  = byP;
    td.threat_by_minor = byM;
    td.threat_by_rook  = byR;
    td.threat_key      = td.hash_key;
}

// Check-ordering helper: check_sq[pt] = squares from which a piece of type pt would give
// a DIRECT check to the enemy king (= squares a pt-attacker reaches the king square from).
// Computed once per node (caller checks td.check_key == td.hash_key). Only when CheckOrdering on.
static inline void td_compute_checks(ThreadData& td) {
    const int them = td.side ^ 1;
    const U64 occ  = td.occupancies[both];
    int ksq = get_ls1b_index(td.bitboards[them * 6 + K]);   // enemy king square
    td.check_sq[0] = pawn_attacks[them][ksq];               // pawn (our pawn checks from these squares)
    td.check_sq[1] = knight_attacks[ksq];                   // knight
    td.check_sq[2] = get_bishop_attacks(ksq, occ);          // bishop
    td.check_sq[3] = get_rook_attacks(ksq, occ);            // rook
    td.check_sq[4] = td.check_sq[2] | td.check_sq[3];       // queen
    td.check_sq[5] = 0;                                     // king cannot give check
    td.check_key   = td.hash_key;
}

static inline int td_score_move(ThreadData& td, int move, int tt_move) {
    if (move == tt_move) return SCORE_TT_MOVE;

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
        for (int pc = start; pc <= end; pc++) {
            if (get_bit(td.bitboards[pc], target)) {
                victim = pc;
                break;
            }
        }

        int caphist = g_capture_hist
            ? td.capture_history[piece][target][victim] / g_caphist_div   // qui era diviso 16
            : 0;

        // Lazy SEE: capturing a piece of equal-or-greater value is good by
        // definition (no SEE needed). Only run SEE for "attacker > victim"
        // captures, which might be losing. Losing captures (SEE < 0) are
        // ordered AFTER all quiets (still searched, just last).
        if (see_piece_values[piece] <= see_piece_values[victim] || td_see(td, move) >= 0)
            return SCORE_GOOD_CAPTURE + mvv_lva[piece][victim] + caphist;
        else
            return SCORE_BAD_CAPTURE + mvv_lva[piece][victim] + caphist;
    }

    // Quiet moves
    if (td.killer_moves[0][td.ply] == move) return SCORE_KILLER0;
    if (td.killer_moves[1][td.ply] == move) return SCORE_KILLER1;

    int prev = td.move_stack[td.ply];
    if (prev && td.counter_moves[get_move_piece(prev)][get_move_target(prev)] == move)
        return SCORE_COUNTER;

    // Butterfly history (MainHistWeight, default 1; SF=2) + continuation history
    // (ContHistWeight, default 1). I conthist sono sommati e poi pesati, cosi' il
    // co-tune puo' bilanciare conthist vs main/pawn senza toccare il loro uso in LMR.
    int h = g_mainhist_weight * td.history_moves[piece][target] / 100;
    int ch = 0;
    if (prev)
        ch += td.continuation_history[get_move_piece(prev)][get_move_target(prev)][piece][target];
    // Longer-range continuation history: the same quiet scored in the context of
    // the move 2 and 4 plies back. Off by default (ContHistMulti).
    if (g_conthist_multi) {
        if (td.ply >= 1) {
            int p2 = td.move_stack[td.ply - 1];
            if (p2) ch += td.cont_hist_2[get_move_piece(p2)][get_move_target(p2)][piece][target];
        }
        if (td.ply >= 3) {
            int p4 = td.move_stack[td.ply - 3];
            if (p4) ch += td.cont_hist_4[get_move_piece(p4)][get_move_target(p4)][piece][target];
        }
    }
    // ContHist36 (#4): continuation history a 3-ply (ply-2) e 6-ply (ply-5), scalata da
    // ContHist36Weight, sommata in ch. Default OFF = byte-identico.
    if (g_conthist36) {
        int ch36 = 0;
        if (td.ply >= 2) {
            int p3 = td.move_stack[td.ply - 2];
            if (p3) ch36 += td.cont_hist_3[get_move_piece(p3)][get_move_target(p3)][piece][target];
        }
        if (td.ply >= 5) {
            int p6 = td.move_stack[td.ply - 5];
            if (p6) ch36 += td.cont_hist_6[get_move_piece(p6)][get_move_target(p6)][piece][target];
        }
        ch += g_conthist36_weight * ch36 / 100;
    }
    h += g_conthist_weight * ch / 100;
    // PawnHistory (SF-style, weight 2x): quiet ordering by pawn structure. Indice
    // ricalcolato dal board corrente (= board del nodo durante lo scoring -> corretto,
    // niente staleness). Gated -> OFF = byte-identico.
    if (g_pawn_hist) {
        int pk = td_corr_index(td) & ThreadData::PAWN_HIST_MASK;
        h += g_pawn_hist_weight * (int)td.pawn_history[pk][piece][target] / 100;
    }
    // Threat-ordering (ThreatOrdering, default OFF -> byte-identico): +scala*valore se
    // il pezzo lascia una casa attaccata da uno INFERIORE, -scala*valore se ci entra.
    // Tier come SF (Q vs minacce-di-torre, R vs minore, minore vs pedone); pedoni/re
    // non hanno un "minore" -> nessun termine. Minacce calcolate 1x/nodo (cache hash_key).
    if (g_threat_ordering) {
        if (td.threat_key != td.hash_key) td_compute_threats(td);
        U64 lesser = 0; bool has = true;
        switch (piece % 6) {                 // 0=P 1=N 2=B 3=R 4=Q 5=K
            case 4: lesser = td.threat_by_rook;  break;   // queen
            case 3: lesser = td.threat_by_minor; break;   // rook
            case 1: case 2: lesser = td.threat_by_pawn; break; // knight / bishop
            default: has = false; break;                  // pawn / king
        }
        if (has) {
            int from = get_move_source(move);
            int term = (get_bit(lesser, from) ? 1 : 0) - (get_bit(lesser, target) ? 1 : 0);
            if (term) h += g_threat_scale * see_piece_values[piece] * term / 100;
        }
    }
    // Check-ordering (CheckOrdering, default OFF -> byte-identico): bonus ai quiet che danno
    // SCACCO DIRETTO e non sono blunder (SEE>=-75), come SF. check_sq calcolate 1x/nodo.
    if (g_check_ordering) {
        if (td.check_key != td.hash_key) td_compute_checks(td);
        if (get_bit(td.check_sq[piece % 6], target) && td_see(td, move) >= -75)
            h += g_check_bonus;
    }
    // Low-ply history (#5, default OFF -> byte-identico): segnale per-ply near-root,
    // peso che decade con la profondita' (1+2*ply), come SF.
    if (g_lowply && td.ply < ThreadData::LOW_PLY_MAX) {
        h += g_lowply_weight * td.lowply_history[td.ply][piece][target] / (100 * (1 + 2 * td.ply));
    }
    return h;
}

static inline void td_sort_moves(ThreadData& td, moves* move_list, int tt_move) {
    int scores[256];

    for (int i = 0; i < move_list->count; i++) {
        // Punteggio base (TT, catture, history, killer)
        scores[i] = td_score_move(td, move_list->moves[i], tt_move);
    }

    // --- Ordinamento standard (Selection Sort) ---
    for (int i = 0; i < move_list->count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < move_list->count; j++) {
            if (scores[j] > scores[best]) best = j;
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
static inline void td_score_all_moves(ThreadData& td, moves* move_list, int tt_move, int* scores) {
    for (int i = 0; i < move_list->count; i++)
        scores[i] = td_score_move(td, move_list->moves[i], tt_move);
}

// ============================================================================
// STAGED MOVE PICKER  (UCI option "MovePicker", default OFF)
// ============================================================================

// Pseudo-legality validator. The staged picker yields the TT move, killers and the
// counter-move WITHOUT first generating the move list, so it must confirm each is a
// real move in THIS position: td_make_move trusts the encoded bits and would corrupt
// the board on a TT collision. This MUST NEVER return true for an illegal move; when
// unsure it returns false and the move simply reappears during normal generation
// (we only lose a little laziness, never correctness). Mirrors td_generate_moves.
static inline bool td_is_pseudo_legal(ThreadData& td, int move) {
    if (move == 0) return false;
    const int source   = get_move_source(move);
    const int target   = get_move_target(move);
    const int piece    = get_move_piece(move);
    const int promoted = get_move_promoted(move);
    const int capture  = get_move_capture(move) ? 1 : 0;
    const int dbl      = get_move_double(move) ? 1 : 0;
    const int enpass   = get_move_enpassant(move) ? 1 : 0;
    const int castling = get_move_castling(move) ? 1 : 0;

    // The moving piece must belong to the side to move and sit on `source`.
    if (td.side == white) { if (piece < P || piece > K) return false; }
    else                  { if (piece < p || piece > k) return false; }
    if (!get_bit(td.bitboards[piece], source)) return false;

    const U64 occ     = td.occupancies[both];
    const U64 friends = td.occupancies[td.side];
    const U64 enemies = td.occupancies[td.side ^ 1];

    // A target holding a friendly piece is never legal (also rejects source==target).
    if (get_bit(friends, target)) return false;

    const bool isPawn = (piece == P || piece == p);
    const bool isKing = (piece == K || piece == k);

    // ---- Castling ----
    if (castling) {
        if (!isKing || capture || promoted || enpass || dbl) return false;
        if (td.side == white) {
            if (source != e1) return false;
            if (target == g1) return (td.castle & wk) &&
                !get_bit(occ, f1) && !get_bit(occ, g1) &&
                !td_is_square_attacked(td, e1, black) && !td_is_square_attacked(td, f1, black);
            if (target == c1) return (td.castle & wq) &&
                !get_bit(occ, d1) && !get_bit(occ, c1) && !get_bit(occ, b1) &&
                !td_is_square_attacked(td, e1, black) && !td_is_square_attacked(td, d1, black);
            return false;
        }
        else {
            if (source != e8) return false;
            if (target == g8) return (td.castle & bk) &&
                !get_bit(occ, f8) && !get_bit(occ, g8) &&
                !td_is_square_attacked(td, e8, white) && !td_is_square_attacked(td, f8, white);
            if (target == c8) return (td.castle & bq) &&
                !get_bit(occ, d8) && !get_bit(occ, c8) && !get_bit(occ, b8) &&
                !td_is_square_attacked(td, e8, white) && !td_is_square_attacked(td, d8, white);
            return false;
        }
    }

    // Only pawns may carry promotion / double-push / en-passant flags.
    if (!isPawn && (promoted || dbl || enpass)) return false;

    // ---- Pawns ----
    if (isPawn) {
        const int up = (td.side == white) ? -8 : 8;   // board layout: a8=0 ... h1=63
        const bool toLastRank = (td.side == white) ? (target <= h8) : (target >= a1);

        // A pawn reaching the last rank MUST promote (and the promoted piece must
        // belong to the side to move); promotion off the last rank is illegal.
        if (promoted) {
            if (!toLastRank) return false;
            if (td.side == white) { if (promoted < N || promoted > Q) return false; }
            else                  { if (promoted < n || promoted > q) return false; }
        }
        else if (toLastRank && !enpass) {
            return false;
        }

        if (capture) {
            if (!get_bit(pawn_attacks[td.side][source], target)) return false;
            if (enpass) {
                if (dbl) return false;
                if (td.enpassant == no_sq || target != td.enpassant) return false;
                const int capsq  = (td.side == white) ? target + 8 : target - 8;
                const int eppawn = (td.side == white) ? p : P;
                return get_bit(td.bitboards[eppawn], capsq) != 0;
            }
            return get_bit(enemies, target) != 0;       // ordinary capture
        }
        else {
            if (enpass) return false;
            if (get_bit(occ, target)) return false;     // push onto empty square only
            if (dbl) {
                const bool onStart = (td.side == white) ? (source >= a2 && source <= h2)
                                                        : (source >= a7 && source <= h7);
                if (!onStart) return false;
                if (target != source + 2 * up) return false;
                return !get_bit(occ, source + up);       // intermediate square empty
            }
            return (target == source + up);
        }
    }

    // ---- Knight / Bishop / Rook / Queen / King (non-castling) ----
    U64 att;
    switch (piece) {
        case N: case n: att = knight_attacks[source];               break;
        case B: case b: att = get_bishop_attacks(source, occ);       break;
        case R: case r: att = get_rook_attacks(source, occ);         break;
        case Q: case q: att = get_queen_attacks(source, occ);        break;
        case K: case k: att = king_attacks[source];                  break;
        default: return false;
    }
    if (!get_bit(att, target)) return false;
    // The capture flag must agree with whether an enemy occupies the target square.
    return (capture != 0) == (get_bit(enemies, target) != 0);
}

// Stage order matches the legacy score bands so the search is (near) byte-identical.
enum {
    MPS_TT = 0,         // hash move (validated)
    MPS_GEN_TACTICAL,   // generate captures + capture-promotions, score them
    MPS_GOOD_TACTICAL,  // promotions + SEE>=0 captures, by score
    MPS_KILLER0,
    MPS_KILLER1,
    MPS_COUNTER,
    MPS_GEN_QUIET,      // generate all moves, keep & score the quiets
    MPS_QUIET,          // quiets by history, low to high consumed
    MPS_BAD_TACTICAL,   // SEE<0 captures, by score
    MPS_DONE
};

// Sentinel placed in a score slot once that move has been yielded. Far below any
// real move score (good caps ~+7e5, bad caps ~-7e5), so it never collides.
static const int MP_CONSUMED = -2000000000;

struct MovePicker {
    bool   staged;
    int    stage;
    int    tt_move, killer0, killer1, counter;
    moves* caps;   int* cap_scores;  int cap_n;   // capture/promotion buffer
    moves* quiets; int* q_scores;     int q_n;     // quiet buffer (legacy: all moves)
    int    l_idx;                                  // legacy pick-next cursor
    // Phase-2 skip flags: set by the search loop when a pruning condition is met
    // BEFORE the first quiet/bad-cap is even generated. mp_next() reads these and
    // jumps past the entire stage — zero generation cost for pruned nodes.
    bool   skip_quiets;    // skip MPS_GEN_QUIET + MPS_QUIET + killers/counter
    bool   skip_bad_caps;  // skip MPS_BAD_TACTICAL
};

// caps/cap_scores: scratch buffers for the tactical stages (staged mode only).
// quiets/q_scores: in legacy mode these are the fully generated+scored move list;
// in staged mode they are reused as the quiet-stage buffer.
static inline void mp_init(MovePicker& mp, ThreadData& td, bool staged, int tt_move,
                           moves* caps, int* cap_scores, moves* quiets, int* q_scores) {
    mp.staged      = staged;
    mp.tt_move     = tt_move;
    mp.caps        = caps;    mp.cap_scores = cap_scores;  mp.cap_n = 0;
    mp.quiets      = quiets;  mp.q_scores   = q_scores;    mp.q_n   = 0;
    mp.l_idx       = 0;
    mp.stage       = MPS_TT;
    mp.skip_quiets   = false;
    mp.skip_bad_caps = false;
    if (staged) {
        mp.killer0 = td.killer_moves[0][td.ply];
        mp.killer1 = td.killer_moves[1][td.ply];
        int prev   = td.move_stack[td.ply];
        mp.counter = prev ? td.counter_moves[get_move_piece(prev)][get_move_target(prev)] : 0;
    }
    else {
        mp.killer0 = mp.killer1 = mp.counter = 0;
    }
}

// Returns the next move to search, 0 when exhausted.
static int mp_next(ThreadData& td, MovePicker& mp) {
    // --- Legacy: pick-next over the fully scored move list (bit-identical to the
    //     original in-loop selection sort). ---
    if (!mp.staged) {
        moves* ml = mp.quiets;
        int*   sc = mp.q_scores;
        int i = mp.l_idx;
        if (i >= ml->count) return 0;
        int best = i;
        for (int j = i + 1; j < ml->count; j++)
            if (sc[j] > sc[best]) best = j;
        if (best != i) {
            int tm = ml->moves[i]; ml->moves[i] = ml->moves[best]; ml->moves[best] = tm;
            int ts = sc[i];        sc[i]        = sc[best];        sc[best]        = ts;
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
        td_generate_moves(td, mp.caps, true);                 // captures + capture-promos
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
                if (mp.cap_scores[i] > bs) { bs = mp.cap_scores[i]; best = i; }
            if (best < 0) break;
            int m = mp.caps->moves[best];
            mp.cap_scores[best] = MP_CONSUMED;
            if (m != mp.tt_move) return m;                    // tt already yielded
        }
        mp.stage = MPS_KILLER0;
        [[fallthrough]];

    case MPS_KILLER0:
        mp.stage = MPS_KILLER1;
        if (!mp.skip_quiets &&
            mp.killer0 && mp.killer0 != mp.tt_move && td_is_pseudo_legal(td, mp.killer0))
            return mp.killer0;
        [[fallthrough]];

    case MPS_KILLER1:
        mp.stage = MPS_COUNTER;
        if (!mp.skip_quiets &&
            mp.killer1 && mp.killer1 != mp.tt_move && mp.killer1 != mp.killer0 &&
            td_is_pseudo_legal(td, mp.killer1))
            return mp.killer1;
        [[fallthrough]];

    case MPS_COUNTER:
        mp.stage = MPS_GEN_QUIET;
        if (!mp.skip_quiets &&
            mp.counter && mp.counter != mp.tt_move && mp.counter != mp.killer0 &&
            mp.counter != mp.killer1 && td_is_pseudo_legal(td, mp.counter))
            return mp.counter;
        [[fallthrough]];

    case MPS_GEN_QUIET:
        if (mp.skip_quiets) { mp.stage = MPS_BAD_TACTICAL; goto bad_tactical; }
        td_generate_moves(td, mp.quiets, false);              // all pseudo-legal moves
        mp.q_n = mp.quiets->count;
        for (int i = 0; i < mp.q_n; i++) {
            int m = mp.quiets->moves[i];
            // Captures are handled by the tactical stages; the TT/killer/counter
            // moves were already yielded -> mark them consumed so we never repeat.
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
        if (mp.skip_quiets) { mp.stage = MPS_BAD_TACTICAL; goto bad_tactical; }
        int best = -1, bs = MP_CONSUMED;
        for (int i = 0; i < mp.q_n; i++)
            if (mp.q_scores[i] != MP_CONSUMED && mp.q_scores[i] > bs) { bs = mp.q_scores[i]; best = i; }
        if (best >= 0) {
            mp.q_scores[best] = MP_CONSUMED;
            return mp.quiets->moves[best];
        }
        mp.stage = MPS_BAD_TACTICAL;
        // fallthrough into bad-tactical
    }

    bad_tactical:
    case MPS_BAD_TACTICAL:
        if (mp.skip_bad_caps) { mp.stage = MPS_DONE; return 0; }
        for (;;) {
            int best = -1, bs = MP_CONSUMED;
            for (int i = 0; i < mp.cap_n; i++)
                if (mp.cap_scores[i] != MP_CONSUMED && mp.cap_scores[i] > bs) { bs = mp.cap_scores[i]; best = i; }
            if (best < 0) break;
            int m = mp.caps->moves[best];
            mp.cap_scores[best] = MP_CONSUMED;
            if (m != mp.tt_move) return m;
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
// mossa reversibile (pezzi non-pedone, coppie raggiungibili su board vuota; 3668
// in tutto, come SF). cuckoo_move[i] = s1 | (s2 << 6). between_tbl[s1][s2] =
// case STRETTAMENTE tra s1 e s2 (0 per cavallo / case non allineate).
static U64 cuckoo_key[8192];
static int cuckoo_move[8192];
// (between_tbl definita piu' sopra, prima di td_generate_moves: serve anche
//  alle evasioni P2.3, non solo all'upcoming-repetition.)

static inline int cuckoo_h1(U64 h) { return (int)(h & 0x1FFF); }
static inline int cuckoo_h2(U64 h) { return (int)((h >> 16) & 0x1FFF); }

static void init_cuckoo() {
    memset(cuckoo_key,  0, sizeof(cuckoo_key));
    memset(cuckoo_move, 0, sizeof(cuckoo_move));

    for (int s1 = 0; s1 < 64; s1++)
        for (int s2 = 0; s2 < 64; s2++) {
            between_tbl[s1][s2] = 0;
            if (s1 == s2) continue;
            // Intersezione dei raggi con l'altra casa come unica occupazione =
            // case strettamente comprese (gli estremi si elidono da soli).
            if (get_rook_attacks(s1, 0ULL) & (1ULL << s2))
                between_tbl[s1][s2] = get_rook_attacks(s1, 1ULL << s2) &
                                      get_rook_attacks(s2, 1ULL << s1);
            else if (get_bishop_attacks(s1, 0ULL) & (1ULL << s2))
                between_tbl[s1][s2] = get_bishop_attacks(s1, 1ULL << s2) &
                                      get_bishop_attacks(s2, 1ULL << s1);
        }

    static const int rev_pieces[10] = { N, B, R, Q, K, n, b, r, q, k };
    for (int pi = 0; pi < 10; pi++) {
        const int pc = rev_pieces[pi];
        for (int s1 = 0; s1 < 64; s1++) {
            U64 att;
            switch (pc % 6) {                  // 1=N 2=B 3=R 4=Q 5=K (mai pedoni)
                case 1:  att = knight_attacks[s1];               break;
                case 2:  att = get_bishop_attacks(s1, 0ULL);     break;
                case 3:  att = get_rook_attacks(s1, 0ULL);       break;
                case 4:  att = get_queen_attacks(s1, 0ULL);      break;
                default: att = king_attacks[s1];                 break;
            }
            for (int s2 = s1 + 1; s2 < 64; s2++) {
                if (!(att & (1ULL << s2))) continue;
                U64 key = piece_keys[pc][s1] ^ piece_keys[pc][s2] ^ side_key;
                int mv  = s1 | (s2 << 6);
                int i   = cuckoo_h1(key);
                while (true) {                 // inserimento cuckoo (2 sedi possibili)
                    U64 tk = cuckoo_key[i];  cuckoo_key[i]  = key; key = tk;
                    int tm = cuckoo_move[i]; cuckoo_move[i] = mv;  mv  = tm;
                    if (key == 0) break;       // slot era vuoto: catena chiusa
                    i = (i == cuckoo_h1(key)) ? cuckoo_h2(key) : cuckoo_h1(key);
                }
            }
        }
    }
}

// True se il lato al tratto ha una mossa reversibile che chiude un ciclo verso una
// posizione gia' vista nelle ultime `fifty` semimosse (port di SF has_game_cycle
// sulla nostra repetition_table piatta: [repetition_index] = posizione 1 ply fa,
// [repetition_index-1] = 2 ply fa, ...). Le entry create dalle NULL move sono
// inerti: la loro moveKey differisce solo per side/ep, mai per due piece-key, e
// non puo' combaciare con una chiave cuckoo -> nessun falso positivo.
static bool td_upcoming_repetition(ThreadData& td) {
    int end = td.fifty;
    if (end > td.repetition_index) end = td.repetition_index;
    if (end < 3) return false;
    const U64 orig = td.hash_key;
    for (int i = 3; i <= end; i += 2) {
        const U64 prev     = td.repetition_table[td.repetition_index - (i - 1)];
        const U64 move_key = orig ^ prev;
        int j;
        if ((j = cuckoo_h1(move_key), cuckoo_key[j] == move_key) ||
            (j = cuckoo_h2(move_key), cuckoo_key[j] == move_key)) {
            const int s1 = cuckoo_move[j] & 63, s2 = (cuckoo_move[j] >> 6) & 63;
            if (between_tbl[s1][s2] & td.occupancies[both]) continue;   // percorso bloccato
            if (td.ply > i) return true;              // ciclo tutto dentro il search path
            // Ciclo che pesca dietro la radice: vale solo se la mossa e' del lato al
            // tratto E la vecchia posizione era gia' una ripetizione (regola SF).
            const int occ_sq = get_bit(td.occupancies[both], s1) ? s1 : s2;
            if (!get_bit(td.occupancies[td.side], occ_sq)) continue;
            for (int k2 = i + 2; k2 <= end; k2 += 2)
                if (td.repetition_table[td.repetition_index - (k2 - 1)] == prev)
                    return true;
        }
    }
    return false;
}

static inline int td_is_repetition(ThreadData& td) {
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
        if (lo < 0) lo = 0;
        for (int i = td.repetition_index - 1; i >= lo; i--)
            if (td.repetition_table[i] == td.hash_key) return 1;
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

// qs_depth: 0 alla prima ply di qsearch (chiamate dal negamax), decresce nelle
// ricorsioni. Serve solo a P1.3 QSChecks (quiet check tenuti SOLO a qs_depth 0).
static int td_quiescence(ThreadData& td, int alpha, int beta, int qs_depth = 0) {
    // Illegal-position / king-capture guard (see td_negamax for the full
    // rationale): never search a position where the side not to move is in check,
    // because making the king capture desyncs the NNUE accumulator and crashes.
    // (Rimozione provata 2026-06-06: node-identica ma NPS ~0 -> tenuta per safety.)
    {
        int opp_king_sq = get_ls1b_index((td.side == white) ? td.bitboards[k] : td.bitboards[K]);
        if (td_is_square_attacked(td, opp_king_sq, td.side))
            return mate_value - td.ply;
    }
    if ((td.nodes & 4095) == 0) {
        if (stop_threads.load(std::memory_order_relaxed)) return 0;
        if (timeset && get_time_ms() > stoptime) {
            stop_threads.store(true, std::memory_order_relaxed);
            return 0;
        }
    }

    td.nodes++;
    if (td.ply > td.seldepth) td.seldepth = td.ply;   // seldepth (info UCI)

    if (td.ply >= max_ply) return td_evaluate(td);

    bool pv_node = (beta - alpha > 1);

    // TT probe (qsearch entries stored at depth 0)
    int tt_move = 0, tt_score = 0, tt_depth = 0, tt_flag = hash_flag_alpha;
    int tt_eval = tt_eval_none;   // P1.1
    bool tt_pv_q = false;
    bool tt_hit = probe_tt(td.hash_key, tt_move, tt_score, tt_depth, tt_flag, tt_eval, tt_pv_q);
    if (tt_hit && !pv_node) {
        if (tt_score < -mate_score) tt_score += td.ply;
        if (tt_score > mate_score) tt_score -= td.ply;
        if (tt_flag == hash_flag_exact) return tt_score;
        if (tt_flag == hash_flag_alpha && tt_score <= alpha) return tt_score;
        if (tt_flag == hash_flag_beta && tt_score >= beta) return tt_score;
    }

    // Sotto scacco: niente stand-pat, si cercano TUTTE le evasioni.
    int king_sq = get_ls1b_index((td.side == white) ? td.bitboards[K] : td.bitboards[k]);
    bool in_check = td_is_square_attacked(td, king_sq, td.side ^ 1);

    int best_score;
    int q_raw_eval = tt_eval_none;   // P1.1: eval pura da salvare nello store qsearch
    if (in_check) {
        best_score = -infinity;
    }
    else {
        // P1.1 TTStaticEval: come nel negamax (TT = de-smorzata, ri-smorza qui).
        q_raw_eval = (g_tt_static_eval && tt_hit && tt_eval != tt_eval_none)
                     ? tt_eval_redamp(tt_eval, td.fifty) : td_evaluate(td);
        int stand_pat = q_raw_eval;
        // FIX P0.4 (2026-06-09): la correction history ora corregge ANCHE lo
        // stand-pat di quiescence (dove avviene la maggioranza delle eval);
        // prima si fermava ai nodi interni. Gated QsearchCorr per l'ablazione.
        if (g_qsearch_corr && g_corr_hist) stand_pat += td_corr_value(td, td_corr_index(td));
        // P1.1: tt_score con bound coerente = stima di qualita'-search migliore
        // dello stand-pat statico (stesso adjustment del negamax).
        if (g_tt_eval_improve && tt_hit &&
            tt_score > -mate_score && tt_score < mate_score &&
            (tt_flag == hash_flag_exact ||
             (tt_flag == hash_flag_beta  && tt_score > stand_pat) ||
             (tt_flag == hash_flag_alpha && tt_score < stand_pat)))
            stand_pat = tt_score;
        best_score = stand_pat;
        if (stand_pat >= beta) return stand_pat;

        const int DELTA = 1000;
        if (stand_pat + DELTA < alpha) return stand_pat;

        if (stand_pat > alpha) alpha = stand_pat;
    }

    moves move_list[1];
    // Se non siamo sotto scacco (!in_check), passa true per generare SOLO le catture.
    // P1.3 QSChecks (default OFF): alla prima ply di qsearch genera TUTTO, poi il
    // loop tiene solo catture + quiet che danno SCACCO DIRETTO.
    const bool qs_checks_here = g_qs_checks && qs_depth == 0 && !in_check;
    td_generate_moves(td, move_list, !in_check && !qs_checks_here);

    // Calcola punteggi senza ordinare (selezione pick-next).
    int move_scores[256];
    td_score_all_moves(td, move_list, tt_move, move_scores);

    int best_move = 0;
    int legal_moves = 0;

    for (int count = 0; count < move_list->count; count++) {

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
                // P1.3 QSChecks: tieni il quiet SOLO se da' scacco diretto e non
                // e' un check-blunder (SEE >= -75, stesso filtro del CheckOrdering).
                if (!qs_checks_here) continue;
                if (td.check_key != td.hash_key) td_compute_checks(td);
                if (!get_bit(td.check_sq[get_move_piece(move) % 6], get_move_target(move))) continue;
                if (td_see(td, move) < -75) continue;
            } else {
                // (4) QFutility: salta la cattura se best_score + valore(vittima) + margine
                //     non arriva ad alpha (no ep/promo). OFF = nessun effetto.
                if (g_qfutility && !get_move_promoted(move) && !get_move_enpassant(move)) {
                    int vic = td_captured_piece(td, get_move_target(move));
                    if (vic >= 0 && vic < 12 &&
                        best_score + see_piece_values[vic] + g_qfut_margin <= alpha) continue;
                }
                if (!get_move_promoted(move) && td_see(td, move) < 0) continue;
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

        int score = -td_quiescence(td, -beta, -alpha, qs_depth - 1);

        td_unmake_move(td, move, undo);
        td.ply--;
        td.repetition_index--;

        if (stop_threads.load(std::memory_order_relaxed)) return 0;

        if (score > best_score) {
            best_score = score;
            best_move = move;
            if (score > alpha) {
                alpha = score;
                if (score >= beta) break;
            }
        }
    }

    if (in_check && legal_moves == 0)
        return -mate_value + td.ply;

    int store_flag = (best_score >= beta) ? hash_flag_beta : hash_flag_alpha;
    store_tt(td.hash_key, best_move, best_score, 0, store_flag, td.ply, false, tt_eval_undamp(q_raw_eval, td.fifty));

    return best_score;
}

// ============================================================================
// NEGAMAX (Lazy SMP: each thread runs this independently, sharing only the TT)
// ============================================================================

// ---- Correction history ----------------------------------------------------
// Learns the systematic gap between the NNUE static eval and the value the search
// actually returns, bucketed by PAWN STRUCTURE + side to move (the net most often
// misjudges pawn structures). The learned offset is added to the raw static eval,
// so pruning/ordering see a value closer to what the search will confirm.
static constexpr int CORR_BITS  = 14;
static constexpr int CORR_SIZE  = 1 << CORR_BITS;       // 16384 buckets
static constexpr int CORR_MASK  = CORR_SIZE - 1;
static constexpr int CORR_GRAIN = 256;                  // stored value is (cp * GRAIN)
// Max correction (cp) and stored-value clamp are now runtime tunables: g_corr_cap
// and (g_corr_cap * CORR_GRAIN). See g_corr_cap / g_corr_lr_div above.

// Bucket index from the Zobrist key of a given set of piece types.
static inline int td_corr_index_pieces(ThreadData& td, const int* pcs, int n) {
    U64 k = 0;
    for (int j = 0; j < n; j++) {
        int pc = pcs[j];
        U64 bb = td.bitboards[pc];
        while (bb) { int sq = get_ls1b_index(bb); k ^= piece_keys[pc][sq]; pop_bit(bb, sq); }
    }
    return (int)(k & CORR_MASK);
}

// Bucket index from the pawn-only Zobrist key (white pawns P, black pawns p).
// P2.1: con PawnKeyIncr (default ON) usa la chiave INCREMENTALE mantenuta in
// make/unmake invece di riscansionare le bitboard a ogni chiamata (corr 1x/nodo +
// pawn_history ~30 quiet/nodo). Stessa chiave per costruzione = node-identical.
static inline int td_corr_index(ThreadData& td) {
    if (g_pawn_key_incr) return (int)(td.pawn_key & CORR_MASK);
    const int pcs[2] = { P, p };
    return td_corr_index_pieces(td, pcs, 2);
}
// Minor-piece (N/B) and major-piece (R/Q) keyed indices (CorrHistMulti only).
static inline int td_corr_index_minor(ThreadData& td) {
    const int pcs[4] = { N, B, n, b };
    return td_corr_index_pieces(td, pcs, 4);
}
static inline int td_corr_index_major(ThreadData& td) {
    const int pcs[4] = { R, Q, r, q };
    return td_corr_index_pieces(td, pcs, 4);
}

// Continuation-correction bucket for the current node: keyed by the move 2-ply back
// and the move 1-ply back (the path INTO this node). Returns nullptr if either is
// missing (root / null move / ply<2). Only touched when g_corr_cont is on. NB:
// move_stack[td.ply] is the move that led to THIS node (1-ply back), so 2-ply back
// is move_stack[td.ply-1] (same convention as the conthist-multi tables).
static inline int16_t* td_cont_corr_bucket(ThreadData& td) {
    if (td.ply < 1) return nullptr;
    int m1 = td.move_stack[td.ply];          // 1-ply back (move into this node)
    int m2 = td.move_stack[td.ply - 1];      // 2-ply back
    if (!m1 || !m2) return nullptr;
    return &td.cont_corr_hist[get_move_piece(m2)][get_move_target(m2)]
                             [get_move_piece(m1)][get_move_target(m1)];
}

// Correction (cp) to add to the raw static eval for this position. Sums the pawn
// table with the minor/major material tables when CorrHistMulti is on, then clamps
// the TOTAL correction to g_corr_cap.
static inline int td_corr_value(ThreadData& td, int idx) {
    int sum = td.corr_hist[td.side][idx];
    if (g_corr_multi) {
        sum += td.corr_hist_minor[td.side][td_corr_index_minor(td)];
        sum += td.corr_hist_major[td.side][td_corr_index_major(td)];
    }
    if (g_corr_cont) {
        if (int16_t* cc = td_cont_corr_bucket(td))
            sum += g_corr_cont_weight * (int)(*cc) / 100;
    }
    int corr = sum / CORR_GRAIN;
    if (corr >  g_corr_cap) corr =  g_corr_cap;
    if (corr < -g_corr_cap) corr = -g_corr_cap;
    return corr;
}

// One bucket's gravity update toward `target`, clamped to +/-lim.
static inline void td_corr_bucket_update(int& cv, int target, int w, int lim) {
    cv += (target - cv) * w / g_corr_lr_div;            // slower learning -> less noise
    if (cv >  lim) cv =  lim;
    else if (cv < -lim) cv = -lim;
}

// Update the bucket(s) toward (best_score - static_eval), gated by the bound type
// so we never learn from a value that contradicts the bound direction.
static inline void td_corr_update(ThreadData& td, int idx, int static_eval,
                                  int best_score, int bound, int depth,
                                  bool in_check, int best_move, int excluded_move) {
    if (!g_corr_hist || in_check || excluded_move || best_move == 0) return;
    if (get_move_capture(best_move) || get_move_promoted(best_move)) return;   // quiet best only
    if (best_score >=  mate_score || best_score <= -mate_score) return;
    if (static_eval >= mate_score || static_eval <= -mate_score) return;
    int diff = best_score - static_eval;
    if (bound == hash_flag_beta  && diff < 0) return;   // lower bound: only push eval up
    if (bound == hash_flag_alpha && diff > 0) return;   // upper bound: only push eval down
    int target = diff * CORR_GRAIN;
    int w = (depth < 16) ? depth : 16;                  // deeper search = more trust
    int lim = g_corr_cap * CORR_GRAIN;                  // clamp stored value to the cap
    td_corr_bucket_update(td.corr_hist[td.side][idx], target, w, lim);
    if (g_corr_multi) {
        td_corr_bucket_update(td.corr_hist_minor[td.side][td_corr_index_minor(td)], target, w, lim);
        td_corr_bucket_update(td.corr_hist_major[td.side][td_corr_index_major(td)], target, w, lim);
    }
    if (g_corr_cont) {
        if (int16_t* cc = td_cont_corr_bucket(td)) {
            int cv = *cc;                       // gravity update on the int16 bucket
            td_corr_bucket_update(cv, target, w, lim);
            *cc = (int16_t)cv;                  // lim < 32767 -> always fits int16
        }
    }
}

// Multi-ply continuation-history update: apply `bonus` to the 2/4-ply (ContHistMulti)
// and 3/6-ply (ContHist36) buckets at a cutoff. No-op unless one of those is on. Must be
// called with td.ply == the current node's ply (same context as the 1-ply update).
static inline void td_conthist_multi_update(ThreadData& td, int move, int bonus) {
    if (!g_conthist_multi && !g_conthist36) return;
    int pc = get_move_piece(move), tg = get_move_target(move);
    if (g_conthist_multi) {
        if (td.ply >= 1) {
            int p2 = td.move_stack[td.ply - 1];
            if (p2) td_update_history(td.cont_hist_2[get_move_piece(p2)][get_move_target(p2)][pc][tg], bonus);
        }
        if (td.ply >= 3) {
            int p4 = td.move_stack[td.ply - 3];
            if (p4) td_update_history(td.cont_hist_4[get_move_piece(p4)][get_move_target(p4)][pc][tg], bonus);
        }
    }
    // #4: 3-ply (ply-2) e 6-ply (ply-5). Indipendente da ContHistMulti.
    if (g_conthist36) {
        if (td.ply >= 2) {
            int p3 = td.move_stack[td.ply - 2];
            if (p3) td_update_history(td.cont_hist_3[get_move_piece(p3)][get_move_target(p3)][pc][tg], bonus);
        }
        if (td.ply >= 5) {
            int p6 = td.move_stack[td.ply - 5];
            if (p6) td_update_history(td.cont_hist_6[get_move_piece(p6)][get_move_target(p6)][pc][tg], bonus);
        }
    }
}

int td_negamax(ThreadData& td, int alpha, int beta, int depth, bool is_cut_node, int excluded_move = 0) {
    td.pv_length[td.ply] = td.ply;

    // Illegal-position / king-capture guard. If the side-to-move can capture the
    // opponent's king (i.e. the side NOT to move is in check), the position is
    // illegal - typically a malformed input FEN (e.g. a tablebase test position
    // with the enemy king left in check). Searching it would let us actually make
    // the king-capturing move; td_make_move accepts it (our own king is safe),
    // sf_pos_do then removes the enemy king from the NNUE mirror, and the next
    // eval indexes a king-bucket feature for a now-kingless side -> access
    // violation. Return an immediate winning score instead. In legal play the
    // side not to move is never in check, so this costs one attack probe and
    // never fires. (Gate ply==0 provato 2026-06-06: node-identico ma NPS ~0 -> non
    // vale; tenuto pieno. La probe e' troppo cheap per spostare gli NPS.)
    {
        int opp_king_sq = get_ls1b_index((td.side == white) ? td.bitboards[k] : td.bitboards[K]);
        if (td_is_square_attacked(td, opp_king_sq, td.side))
            return mate_value - td.ply;
    }

    // P1.11 draw dither (SF value_draw): ±1cp dal node-count invece di 0 secco,
    // rompe la cecita' da ripetizione fra linee "ugualmente patte".
    if (td.ply && (td_is_repetition(td) || td.fifty >= 100))
        return g_draw_dither ? (1 - (int)(td.nodes & 2)) : 0;

    // P1.4 mate-distance pruning: un matto trovato a questa profondita' non puo'
    // battere uno gia' provato piu' corto -> stringe la finestra e taglia subito.
    // Bound conservativi (mate_value - ply, senza il -1: il check cattura-re sopra
    // puo' restituire un matto a ply+0, vedi return mate_value - td.ply).
    if (g_mate_dist && td.ply) {
        if (alpha < -mate_value + td.ply) alpha = -mate_value + td.ply;
        if (beta  >  mate_value - td.ply) beta  =  mate_value - td.ply;
        if (alpha >= beta) return alpha;
    }

    // P1.2 (2026-06-09): ripetizione IMMINENTE (cuckoo). Se esiste una mossa
    // reversibile che riporta in una posizione gia' vista, il lato al tratto puo'
    // forzare la patta senza che la search debba scoprirlo: con alpha sotto la
    // patta, alza alpha a 0 (anti-shuffling, come SF step 2).
    if (g_upcoming_rep && td.ply && alpha < 0 && td.fifty >= 3 &&
        td_upcoming_repetition(td)) {
        alpha = 0;
        if (alpha >= beta) return alpha;
    }

    bool pv_node = (beta - alpha > 1);
    int tt_move = 0;
    int tt_score = 0;
    int tt_depth = 0;
    int tt_flag = hash_flag_alpha;
    int tt_eval = tt_eval_none;   // P1.1: static eval salvata nell'entry (o none)
    bool tt_pv = false;
    int score;

    // TT probe
    bool tt_hit = probe_tt(td.hash_key, tt_move, tt_score, tt_depth, tt_flag, tt_eval, tt_pv);

    // ttPv (SF): un nodo è "PV-ish" se è un vero PV node o se la TT lo ricorda ex-PV.
    // store_pv viene scritto negli store TT (propaga il flag); il segnale NUOVO è ridurre
    // meno la LMR sui nodi non-PV che la TT marca ex-PV. Gated da g_ttpv (OFF = byte-identico).
    bool store_pv = g_ttpv && (pv_node || (tt_hit && tt_pv));

    // Skip the TT cutoff during a singular search (excluded_move set): we are
    // deliberately re-searching this position without the TT move.
    if (tt_hit && td.ply && !excluded_move) {
        if (tt_depth >= depth && !pv_node) {
            if (tt_score < -mate_score) tt_score += td.ply;
            if (tt_score > mate_score) tt_score -= td.ply;

            // P1.13 (SF): il cutoff servito dalla TT non passa dal loop mosse ->
            // senza questo bonus la history del ttMove quiet si raffredda anche se
            // la mossa continua a tagliare. Scala SPSA-tunable (TTCutBonusScale).
            if (g_ttcut_bonus && tt_move && tt_score >= beta &&
                tt_flag != hash_flag_alpha && !get_move_capture(tt_move)) {
                int bonus = td_stat_bonus(depth) * g_ttcut_bonus_scale / 100;
                td_update_history(td.history_moves[get_move_piece(tt_move)][get_move_target(tt_move)], bonus);
            }

            if (tt_flag == hash_flag_exact) return tt_score;
            if (tt_flag == hash_flag_alpha && tt_score <= alpha) return tt_score; // fail-soft
            if (tt_flag == hash_flag_beta && tt_score >= beta) return tt_score; // fail-soft
        }
    }

    // Time check
    if ((td.nodes & 4095) == 0) {
        if (stop_threads.load(std::memory_order_relaxed)) return 0;
        if (timeset && get_time_ms() > stoptime) {
            stop_threads.store(true, std::memory_order_relaxed);
            return 0;
        }
    }

    if (depth <= 0) return td_quiescence(td, alpha, beta);

    // Ply ceiling. Set pv_length here so the parent's PV-copy loop reads a sane
    // (terminating) bound from pv_length[ply+1] instead of OOB garbage.
    if (td.ply >= max_ply) { td.pv_length[td.ply] = td.ply; return td_evaluate(td); }

    td.nodes++;
    if (td.ply > td.seldepth) td.seldepth = td.ply;   // seldepth (info UCI)

    // Syzygy tablebase WDL probe. For a non-root node with no castling rights
    // and few enough pieces, the tablebase gives the exact game value (side-to-
    // move relative), so we short-circuit the search with it. Cursed/blessed
    // results map to 0 (50-move-rule draws); the actual win conversion in the
    // reached position is guaranteed by the root DTZ probe. count_bits runs
    // only when tablebases are loaded (syzygy_max_pieces() > 0).
    {
        unsigned tb_men = syzygy_max_pieces();
        if (tb_men && td.ply && !excluded_move && td.castle == 0 &&
            (unsigned)count_bits(td.occupancies[both]) <= tb_men) {
            int tb_score;
            if (syzygy_probe_wdl(td, td.ply, tb_score))
                return tb_score;
        }
    }

    int king_sq = get_ls1b_index((td.side == white) ? td.bitboards[K] : td.bitboards[k]);
    bool in_check = td_is_square_attacked(td, king_sq, td.side ^ 1);

    // P1.9: gate co-tunabile sulla check-extension (default 128 = sempre =
    // comportamento storico; il co-tune puo' abbassarlo, 0 = mai).
    if (in_check && depth <= g_check_ext_depth) depth++;

    // Internal Iterative Reduction: senza TT move l'ordinamento e' scadente,
    // riduciamo di 1 ply per ottenere a basso costo una hash move.
    if (depth >= 4 && !tt_move && !excluded_move) depth--;

    // Correction history: bucket for this position (pawn structure + side). Index
    // computed once; reused to apply the correction here and to learn at node exit.
    const int corr_idx = td_corr_index(td);
    int static_eval;
    int node_raw_eval = tt_eval_none;   // P1.1: eval PURA (pre-corr) da salvare in TT
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
        node_raw_eval = (g_tt_static_eval && tt_eval != tt_eval_none)
                        ? tt_eval_redamp(tt_eval, td.fifty) : td_evaluate(td);
        static_eval = node_raw_eval;
        if (g_corr_hist) static_eval += td_corr_value(td, corr_idx);
        td.eval_stack[td.ply] = static_eval;
    }

    // P1.1 (2026-06-09): "improved eval" stile SF — se la TT ha uno score il cui
    // bound e' coerente, e' una stima di qualita'-search migliore della static eval:
    // usala per le DECISIONI DI PRUNING (RFP/NMP/razor/futility/probcut/LMR-margin).
    // static_eval resta la base di eval_stack/improving/corr-update (come SF, che
    // tiene ss->staticEval puro e adatta solo `eval`). Toggle TTEvalImprove per A/B.
    int eval = static_eval;
    if (g_tt_eval_improve && !in_check && tt_hit &&
        tt_score > -mate_score && tt_score < mate_score &&
        (tt_flag == hash_flag_exact ||
         (tt_flag == hash_flag_beta  && tt_score > eval) ||
         (tt_flag == hash_flag_alpha && tt_score < eval)))
        eval = tt_score;

    // "Improving": is our static eval higher than it was two plies ago (our own
    // previous turn)? A rising trend means the position is going our way, so we
    // can trust forward pruning more and reduce more when it's NOT improving.
    // Meaningless in check (eval is noisy there) and at ply<2 (no history) ->
    // treated as not-improving. g_improving gates the whole heuristic so that,
    // when off, every consumer below collapses to the original search exactly.
    // FIX P0.6: se a ply-2 non c'era eval (nodo in scacco con LazyEval), fallback
    // a ply-4 (come SF); se nemmeno quella esiste -> not-improving (= comportamento
    // storico, senza pero' il confronto col vecchio 0 fittizio).
    bool improving = false;
    if (!in_check && td.ply >= 2) {
        int e2 = td.eval_stack[td.ply - 2];
        if (!g_improving_fix)
            improving = static_eval > e2;            // legacy 3.7 (confronto anche col 0 fittizio)
        else if (e2 != EVAL_NONE)
            improving = static_eval > e2;
        else if (td.ply >= 4 && td.eval_stack[td.ply - 4] != EVAL_NONE)
            improving = static_eval > td.eval_stack[td.ply - 4];
    }

    // Reverse futility pruning (skip when beta is a mate bound: don't cut a
    // potential mate search short based on a static evaluation). When improving,
    // shave one ply off the margin (easier cutoff): a rising eval is more likely
    // to hold above beta.
    if (!pv_node && !in_check && depth <= (g_rfp_depth8 ? 8 : 6) && beta < mate_score) {
        int rfp_depth = depth - ((g_improving && improving) ? 1 : 0);
        if (eval - g_rfp_margin * rfp_depth >= beta)
            return eval - g_rfp_margin * rfp_depth;
    }

    // Null move pruning (not when beta is a mate score)
    // P1.6 NMPVerif (default OFF): (a) mai due null consecutive (move_stack==0 =
    // il padre ha appena nullato); (b) durante una search di verifica niente null.
    if (!pv_node && !in_check && td.ply && depth >= 3 && beta < mate_score && eval >= beta &&
        (!g_nmp_verif || (td.move_stack[td.ply] != 0 && !td.in_nmp_verif))) {
        U64 our_pieces = (td.side == white) ?
            (td.bitboards[N] | td.bitboards[B] | td.bitboards[R] | td.bitboards[Q]) :
            (td.bitboards[n] | td.bitboards[b] | td.bitboards[r] | td.bitboards[q]);

        if (our_pieces) {
            int old_ep = td.enpassant;
            U64 old_hash = td.hash_key;

            if (td.enpassant != no_sq) td.hash_key ^= enpassant_keys[td.enpassant];
            td.enpassant = no_sq;
            td.side ^= 1;
            td.hash_key ^= side_key;

            td.ply++;
            td.repetition_table[++td.repetition_index] = td.hash_key;

            // Null move: the child has no "previous move" for counter-move.
            td.move_stack[td.ply] = 0;
            td.captured_stack[td.ply] = -1;

            // P2.2: il sottoalbero della null parte con 0 mosse reali dall'ultima
            // null (le ripetizioni non si contano attraverso una null, stile SF).
            const int saved_pfn = td.plies_from_null;
            td.plies_from_null = 0;

            int R = g_nmp_base + depth / g_nmp_div;
            // (1) NMPEvalScale: più sopra beta = riduzione maggiore (cap +3). OFF = invariato.
            if (g_nmp_eval_scale) {
                int e = (eval - beta) / g_nmp_eval_div;
                if (e > 3) e = 3;
                if (e > 0) R += e;
            }
            if (R > depth - 1) R = depth - 1;

            sf_pos_do_null(td.sfpos, td.fifty);
            score = -td_negamax(td, -beta, -beta + 1, depth - 1 - R, !is_cut_node);
            sf_pos_undo(td.sfpos);

            td.plies_from_null = saved_pfn;   // P2.2 restore
            td.ply--;
            td.repetition_index--;
            td.side ^= 1;
            td.hash_key = old_hash;
            td.enpassant = old_ep;

            if (stop_threads.load(std::memory_order_relaxed)) return 0;
            // fail-soft: ritorna lo score reale, ma non un matto "finto" da null move.
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
                    if (stop_threads.load(std::memory_order_relaxed)) return 0;
                    if (v >= beta) return (v >= mate_score) ? beta : v;
                } else {
                    return (score >= mate_score) ? beta : score;
                }
            }
        }
    }

    // Razoring
    if (!pv_node && !in_check && depth <= (g_razor_depth4 ? 4 : 3)) {
        int razor_margin = g_razor_base + g_razor_mult * depth;
        if (eval + razor_margin < alpha) {
            score = td_quiescence(td, alpha, beta);
            if (score < alpha) return score;
        }
    }

    // ProbCut: at sufficient depth, if some capture leads (via a cheap qsearch +
    // reduced-depth confirmation) to a score well above beta, the full-depth
    // search would almost certainly fail high too -> prune now. Captures only,
    // SEE-gated so we only try moves whose material swing can bridge the margin.
    // Skipped in PV / in check / near mate / during a singular search, and when a
    // deep-enough TT entry already proves the value stays below probcut_beta.
    if (g_probcut && !pv_node && !in_check && !excluded_move &&
        depth >= 5 && beta < mate_score && beta > -mate_score) {
        int probcut_beta = beta + g_probcut_margin;
        bool tt_blocks = tt_hit && tt_depth >= depth - 3 &&
                         tt_score < probcut_beta && tt_score > -mate_score;
        if (!tt_blocks) {
            moves pc_list[1];
            td_generate_moves(td, pc_list, true);          // captures only
            int pc_scores[256];
            td_score_all_moves(td, pc_list, tt_move, pc_scores);

            for (int count = 0; count < pc_list->count; count++) {
                // Pick-next: bring the best-scored remaining move to the front.
                int best_idx = count;
                for (int i = count + 1; i < pc_list->count; i++)
                    if (pc_scores[i] > pc_scores[best_idx]) best_idx = i;
                if (best_idx != count) {
                    int tm = pc_list->moves[count];
                    pc_list->moves[count] = pc_list->moves[best_idx];
                    pc_list->moves[best_idx] = tm;
                    int ts = pc_scores[count];
                    pc_scores[count] = pc_scores[best_idx];
                    pc_scores[best_idx] = ts;
                }
                int move = pc_list->moves[count];

                if (!get_move_capture(move)) continue;
                // The capture's material swing must be able to bridge the eval
                // up to probcut_beta, else it can't plausibly cause the cutoff.
                if (td_see(td, move) < probcut_beta - eval) continue;

                UndoInfo undo;
                td.ply++;
                td.repetition_table[++td.repetition_index] = td.hash_key;
                if (!td_make_move(td, move, undo)) {
                    td.ply--;
                    td.repetition_index--;
                    continue;
                }
                td.move_stack[td.ply] = move;

                // Cheap qsearch screen first; only if it clears probcut_beta do we
                // pay for the reduced-depth (depth-4) confirmation search.
                int pc_score = -td_quiescence(td, -probcut_beta, -probcut_beta + 1);
                if (pc_score >= probcut_beta)
                    pc_score = -td_negamax(td, -probcut_beta, -probcut_beta + 1,
                                                  depth - 4, !is_cut_node);

                td_unmake_move(td, move, undo);
                td.ply--;
                td.repetition_index--;

                if (stop_threads.load(std::memory_order_relaxed)) return 0;

                if (pc_score >= probcut_beta) {
                    // N2 ProbCutTT (SF): salva il fail-high in TT a depth-3 cosi'
                    // le rivisite tagliano dal probe senza rifare qsearch+verifica.
                    if (g_probcut_tt && !excluded_move)
                        store_tt(td.hash_key, move, pc_score, depth - 3, hash_flag_beta,
                                 td.ply, store_pv, tt_eval_undamp(node_raw_eval, td.fifty));
                    return pc_score;                       // fail-soft prune
                }
            }
        }
    }

    // ---- Move generation & ordering ------------------------------------------
    // Staged MovePicker (g_move_picker) produces moves lazily by stage, so an early
    // cutoff never pays to generate/score the quiet moves. Legacy path: generate
    // everything and score up-front (pick-next selection).
    const bool use_picker = g_move_picker;

    moves move_list[1];        // legacy: all moves | staged: quiet-stage buffer
    int   move_scores[256];
    moves cap_buf[1];          // staged: capture/promotion-stage buffer
    int   cap_score_buf[256];

    if (!use_picker) {
        td_generate_moves(td, move_list);
        // Assegna punteggi senza ordinare l'array (la selezione e' pick-next).
        td_score_all_moves(td, move_list, tt_move, move_scores);
    }

    MovePicker mp;
    mp_init(mp, td, use_picker, tt_move, cap_buf, cap_score_buf, move_list, move_scores);

    int best_move = 0;
    int best_score = -infinity;
    int hash_flag = hash_flag_alpha;
    int legal_moves = 0;

    // Node-based time management: at the root, track how many nodes the best move
    // costs. Reset per iteration; updated when a move becomes the new best.
    const bool is_root_node = (td.ply == 0);
    if (is_root_node) td.root_bestmove_nodes = 0;
    int moves_searched = 0;
    int quiets_searched = 0;

    // Quiet moves searched at this node (for history malus on a beta cutoff).
    int searched_quiets[64];
    int n_searched_quiets = 0;

    int searched_captures[64];
    int n_searched_captures = 0;

    int move;
    while ((move = mp_next(td, mp)) != 0) {
        // Ora move   garantita essere la migliore
        // `move` is supplied by mp_next() in the while condition above.
        if (move == excluded_move) continue;   // singular search: skip the TT move
        bool is_capture = get_move_capture(move);
        bool is_promotion = get_move_promoted(move);
        bool is_quiet = !is_capture && !is_promotion;

        // LMP
        if (!pv_node && !in_check && is_quiet && best_score > -mate_score) {
            int lmp_threshold = -1;   // -1 = nessun move-count pruning a questo nodo
            if (g_lmp_improving) {
                // P1.7 (default OFF, co-tune): formula SF (base + d^2*quad/100) /
                // (2 - improving), SENZA cap di profondita'. LMPScale resta la
                // scala comune.
                lmp_threshold = (g_lmp_base + depth * depth * g_lmp_quad / 100)
                                / (2 - ((g_improving && improving) ? 1 : 0));
                lmp_threshold = lmp_threshold * g_lmp_scale / 100;
            } else if (depth <= 8) {
                int lmp_idx = (depth < 8) ? depth : 8;
                lmp_threshold = lmp_table[lmp_idx] * g_lmp_scale / 100;   // LMPScale (100 = invariato)
            }
            if (lmp_threshold >= 0 && quiets_searched >= lmp_threshold) {
                // Phase-2: with the staged picker we can skip the entire quiet
                // stage instead of testing every move individually. The flag is
                // set once; mp_next() will never enter MPS_GEN_QUIET again.
                if (use_picker) { mp.skip_quiets = true; continue; }
                else continue;
            }
        }

        // Futility pruning. When improving, widen the margin so we prune fewer
        // quiets (a rising eval deserves the benefit of the doubt); when not
        // improving, the base margin prunes more.
        if (!pv_node && !in_check && depth <= 6 && is_quiet && best_score > -mate_score) {
            int futility_margin = g_fut_base + g_fut_mult * depth + ((g_improving && improving) ? g_fut_improving : 0);
            if (eval + futility_margin <= alpha) {
                // Phase-2: once futility fires, ALL remaining quiets at this node
                // fail the same static-eval test -> skip the entire stage.
                if (use_picker) { mp.skip_quiets = true; }
                quiets_searched++;
                continue;
            }
        }

        // SEE pruning: scarta a bassa profondita' le catture in perdita oltre un
        // margine, prima di cercarle (riduttore di nodi). Promozioni escluse.
        if (!pv_node && !in_check && !is_promotion && depth <= 8 && best_score > -mate_score) {
            int see_margin = is_capture ? (-g_see_cap_margin * depth) : (-g_see_quiet_margin * depth * depth);
            if (td_see(td, move) < see_margin) {
                if (is_quiet) quiets_searched++;
                // Phase-2: if this move came from MPS_BAD_TACTICAL it means every
                // subsequent bad capture has an equal-or-worse SEE (they are yielded
                // by descending score). The SEE threshold is the same for all, so we
                // can skip the entire remaining bad-capture stage.
                if (use_picker && is_capture && mp.stage == MPS_BAD_TACTICAL)
                    mp.skip_bad_caps = true;
                continue;
            }
        }

        // History pruning: a late quiet move whose combined (butterfly +
        // continuation) history is strongly negative has repeatedly failed in this
        // context -> skip it at low depth. Threshold scales with depth so we prune
        // more aggressively the shallower we are. NB: td.ply not yet incremented
        // here, so the previous move is td.move_stack[td.ply].
        if (g_cont_hist_prune && !pv_node && !in_check && is_quiet &&
            depth <= 4 && moves_searched > 0 && best_score > -mate_score) {
            int prev = td.move_stack[td.ply];
            int hh = td.history_moves[get_move_piece(move)][get_move_target(move)];
            if (prev)
                hh += td.continuation_history[get_move_piece(prev)][get_move_target(prev)]
                                             [get_move_piece(move)][get_move_target(move)];
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
        if (excluded_move == 0 && move == tt_move && depth >= 8 && td.ply &&
            tt_hit && tt_depth >= depth - 3 && tt_flag != hash_flag_alpha &&
            tt_score > -mate_score && tt_score < mate_score) {
            int singular_beta = tt_score - 2 * depth;
            int singular_depth = (depth - 1) / 2;
            int s = td_negamax(td, singular_beta - 1, singular_beta, singular_depth, is_cut_node, tt_move);
            if (s < singular_beta) {
                extension = 1;
                // Double extension: the TT move beats every alternative by a wide
                // margin (hyper-forced) -> extend 2 plies. Non-PV only, to bound the
                // search blow-up that uncapped double extensions can cause.
                if (g_singular_ext && !pv_node && s < singular_beta - g_singular_dmargin) {
                    extension = 2;
                }
            }
            // Multi-cut (BAKED, default on): the exclusion search (TT move removed) failed
            // high and singular_beta itself is >= beta -> more than one move beats beta ->
            // this expected cut-node is not singular -> prune the whole subtree by returning
            // singular_beta. Conservative gate on the BOUND (not the value): fires rarely,
            // only on a very high TT score, which keeps it safe and +Elo where the aggressive
            // SF gate (s>=beta) over-pruned (-13 Elo). Non-PV only (unsound in PV). Safe to
            // return: no move has been made yet this iteration.
            else if (g_multicut && !pv_node && singular_beta >= beta) {
                return singular_beta;
            }
            // Negative extension: the TT move is NOT singular and its score already
            // fails high -> several moves are good (multi-cut-like), so the line is
            // less forcing. Search the TT move one ply shallower.
            else if (g_singular_ext && tt_score >= beta) {
                extension = -1;
            }
        }

        // FIX P0.3 (2026-06-09): catturare lo stato "killer" PRIMA di incrementare
        // td.ply. Il vecchio check nel blocco LMR (`move == killer_moves[..][td.ply]`)
        // leggeva i killer del ply FIGLIO (mosse dell'avversario: il campo piece
        // differisce sempre) -> non era MAI vero -> lo sconto LMR per i killer era
        // dead code.
        bool is_killer = is_quiet &&
            (move == td.killer_moves[0][td.ply] || move == td.killer_moves[1][td.ply]);

        UndoInfo undo;
        td.ply++;
        td.repetition_table[++td.repetition_index] = td.hash_key;

        if (!td_make_move(td, move, undo)) {
            td.ply--;
            td.repetition_index--;
            continue;
        }

        legal_moves++;
        if (is_quiet) quiets_searched++;

        // Does this move give check? After make_move the side to move is the
        // opponent, so their king being attacked means our move checks. Checking
        // moves must NOT be reduced/pruned, otherwise forced mates and tactics
        // are found far too late (Stockfish keeps checks at full depth).
        int stm_king_sq = get_ls1b_index((td.side == white) ? td.bitboards[K] : td.bitboards[k]);
        bool gives_check = td_is_square_attacked(td, stm_king_sq, td.side ^ 1);

        // Record the move that leads to the child node (for counter-move).
        td.move_stack[td.ply] = move;
        td.captured_stack[td.ply] = undo.captured_piece;   // victim (-1 if quiet); V2 capture-hist

        // PVS + LMR
        U64 root_nodes_before = is_root_node ? td.nodes : 0;
        if (moves_searched == 0) {
            score = -td_negamax(td, -beta, -alpha, depth - 1 + extension, false);
        }
        else {
            int reduction = 0;

            if (depth >= 3 && moves_searched >= 3 && is_quiet && !in_check && !gives_check) {
                int d_idx = (depth < 63) ? depth : 63;
                int m_idx = (moves_searched < 63) ? moves_searched : 63;
                reduction = lmr_table[d_idx][m_idx];   // assegna alla variabile ESTERNA (niente 'int' -> niente shadowing che disattivava la LMR)

                if (!pv_node) reduction++;
                // ttPv: i nodi non-PV che la TT ricorda come ex-PV vengono ridotti meno
                // (i veri PV node hanno già lo sconto via il !pv_node qui sopra). È il
                // segnale nuovo dello Step ttPv di SF. OFF (g_ttpv=false) = nessun effetto.
                if (g_ttpv && tt_hit && tt_pv && !pv_node) reduction--;
                if (g_killer_lmr_fix && is_killer)   // FIX P0.3: killer del ply del NODO (vedi sopra)
                    reduction--;
                if (eval + g_lmr_eval_margin < alpha) reduction++;
                if (is_cut_node) reduction++;
                // CutNodeLMR (default OFF): SF riduce i cut-node piu' di noi. Aggiunge
                // g_cutnode_lmr_extra ply SOPRA il +1 qui sopra. OFF = byte-identico.
                if (g_cutnode_lmr && is_cut_node) reduction += g_cutnode_lmr_extra;
                // Enrichment (neutral at default 0): a deep TT entry for this node
                // means the ordering is trustworthy -> reduce a bit less.
                if (g_lmr_ttdepth && tt_hit && tt_depth >= depth) reduction -= g_lmr_ttdepth;
                // Reduce one extra ply when the position is NOT improving: a
                // flat/falling eval makes late quiets less likely to be the move.
                if (g_improving && !improving) reduction++;

                // History-based reduction. DEFAULT (StatScoreLMR off): butterfly
                // history clamped to +/-1 ply (conservativo). StatScoreLMR on: stile
                // SF, riduzione CONTINUA (niente clamp) -> quiet con ottima history
                // ridotti molti ply in piu', quelli con pessima history molti in meno
                // = il fix della SOTTO-RIDUZIONE (gap vs SF15.1). Divisore SPSA-tunable.
                int raw_hist = td.history_moves[get_move_piece(move)][get_move_target(move)];
                if (g_statscore_lmr) {
                    int ss = 2 * raw_hist - g_lmr_ss_offset;
                    reduction -= ss / g_lmr_ss_div;
                } else {
                    int hist_r, hclamp;
                    if (g_aggr_lmr) { hist_r = raw_hist / g_aggr_lmr_div; hclamp = g_aggr_lmr_clamp; }
                    else            { hist_r = raw_hist / g_hist_red_div;  hclamp = 1; }
                    if (hist_r >  hclamp) hist_r =  hclamp;
                    else if (hist_r < -hclamp) hist_r = -hclamp;
                    reduction -= hist_r;
                }

                // Continuation-history reduction. ContHistLMR on: conthist CONTINUO a
                // 1/2/4 ply indietro (scollegato da ContHistPrune) = il segnale che SF
                // usa pesantemente nella LMR e noi ignoravamo di default. Altrimenti il
                // legacy +/-1 ch_r (solo se ContHistPrune on). NB: td.ply gia'
                // incrementato per il figlio -> mossa precedente a move_stack[td.ply-1].
                if (g_conthist_lmr) {
                    int prev = td.move_stack[td.ply - 1];
                    int ch = 0;
                    if (prev)
                        ch += td.continuation_history[get_move_piece(prev)][get_move_target(prev)]
                                                     [get_move_piece(move)][get_move_target(move)];
                    if (td.ply >= 2) { int p2 = td.move_stack[td.ply - 2];
                        if (p2) ch += td.cont_hist_2[get_move_piece(p2)][get_move_target(p2)]
                                                    [get_move_piece(move)][get_move_target(move)]; }
                    if (td.ply >= 4) { int p4 = td.move_stack[td.ply - 4];
                        if (p4) ch += td.cont_hist_4[get_move_piece(p4)][get_move_target(p4)]
                                                    [get_move_piece(move)][get_move_target(move)]; }
                    reduction -= ch / g_lmr_ch_div;
                } else if (g_cont_hist_prune) {
                    int prev = td.move_stack[td.ply - 1];
                    if (prev) {
                        int ch = td.continuation_history[get_move_piece(prev)][get_move_target(prev)]
                                                        [get_move_piece(move)][get_move_target(move)];
                        int ch_r, cclamp;
                        if (g_aggr_lmr) { ch_r = ch / g_aggr_lmr_div; cclamp = g_aggr_lmr_clamp; }
                        else            { ch_r = ch / g_conthist_red_div; cclamp = 1; }
                        if (ch_r >  cclamp) ch_r =  cclamp;
                        else if (ch_r < -cclamp) ch_r = -cclamp;
                        reduction -= ch_r;
                    }
                }

                // DiverseSMP: nudge this helper's reduction to diversify its tree.
                // The clamps below keep it in [0, depth-2], so the bias can't break LMR.
                if (g_diverse_smp) reduction += td.lmr_bias;

                if (reduction < 0) reduction = 0;
                if (reduction > depth - 2) reduction = depth - 2;
            }

            // Reduced-depth zero-window search (LMR). reduced_depth = depth-1-r.
            int reduced_depth = depth - 1 - reduction;
            score = -td_negamax(td, -alpha - 1, -alpha, reduced_depth, true);

            // Full-depth re-search when the reduced search fails high.
            int full_depth = depth - 1;
            if (score > alpha && reduction > 0) {
                if (full_depth > reduced_depth)
                    score = -td_negamax(td, -alpha - 1, -alpha, full_depth, !is_cut_node);
            }

            // PV full-window re-search (uses the possibly-adjusted full_depth so a
            // doDeeper bump also deepens the PV confirmation, matching SF).
            if (score > alpha && score < beta) {
                score = -td_negamax(td, -beta, -alpha, full_depth, false);
            }
        }

        td_unmake_move(td, move, undo);
        td.ply--;
        td.repetition_index--;

        if (stop_threads.load(std::memory_order_relaxed)) return 0;

        moves_searched++;
        if (is_quiet && n_searched_quiets < 64)
            searched_quiets[n_searched_quiets++] = move;
        else if (is_capture && n_searched_captures < 64)
            searched_captures[n_searched_captures++] = move;

        if (score > best_score) {
            best_score = score;
            best_move = move;
            // Node-based TM: record how many nodes this (new best) root move cost.
            if (is_root_node) td.root_bestmove_nodes = td.nodes - root_nodes_before;

            if (score > alpha) {
                alpha = score;
                hash_flag = hash_flag_exact;

                td.pv_table[td.ply][td.ply] = move;
                for (int i = td.ply + 1; i < td.pv_length[td.ply + 1]; i++)
                    td.pv_table[td.ply][i] = td.pv_table[td.ply + 1][i];
                td.pv_length[td.ply] = td.pv_length[td.ply + 1];

                if (score >= beta) {
                    if (!excluded_move) store_tt(td.hash_key, move, best_score, depth, hash_flag_beta, td.ply, store_pv, tt_eval_undamp(node_raw_eval, td.fifty));
                    td_corr_update(td, corr_idx, static_eval, best_score, hash_flag_beta, depth, in_check, move, excluded_move);

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
                        if (prev_cm)
                            td.counter_moves[pcp][pct] = move;
                        int bonus = td_stat_bonus(depth);
                        // PawnHistory: indice dal board corrente (= board del nodo qui,
                        // la cutoff-move e' gia' stata unmade -> corretto). Locale = no
                        // staleness da ricorsione.
                        int pawn_pk = g_pawn_hist ? (td_corr_index(td) & ThreadData::PAWN_HIST_MASK) : 0;
                        td_update_history(td.history_moves[get_move_piece(move)][get_move_target(move)], bonus);
                        if (prev_cm)
                            td_update_history(td.continuation_history[pcp][pct][get_move_piece(move)][get_move_target(move)], bonus);
                        td_conthist_multi_update(td, move, bonus);
                        if (g_pawn_hist)
                            td_update_history(td.pawn_history[pawn_pk][get_move_piece(move)][get_move_target(move)], bonus);
                        if (g_lowply && td.ply < ThreadData::LOW_PLY_MAX)
                            td_update_history(td.lowply_history[td.ply][get_move_piece(move)][get_move_target(move)], bonus);
                        // malus: quiet moves tried before the cutoff get penalized
                        for (int q = 0; q < n_searched_quiets; q++) {
                            int qm = searched_quiets[q];
                            if (qm == move) continue;
                            td_update_history(td.history_moves[get_move_piece(qm)][get_move_target(qm)], -bonus);
                            if (prev_cm)
                                td_update_history(td.continuation_history[pcp][pct][get_move_piece(qm)][get_move_target(qm)], -bonus);
                            td_conthist_multi_update(td, qm, -bonus);
                            if (g_pawn_hist)
                                td_update_history(td.pawn_history[pawn_pk][get_move_piece(qm)][get_move_target(qm)], -bonus);
                            if (g_lowply && td.ply < ThreadData::LOW_PLY_MAX)
                                td_update_history(td.lowply_history[td.ply][get_move_piece(qm)][get_move_target(qm)], -bonus);
                        }
                    }
                    else if (is_capture && g_capture_hist) {
                        // Capture history: BONUS alla cattura che ha causato il cutoff.
                        int bonus = td_stat_bonus(depth);
                        int vic = td_captured_piece(td, get_move_target(move));
                        td_update_history(td.capture_history[get_move_piece(move)][get_move_target(move)][vic], bonus);
                    }

                    // SF-faithful: il MALUS alle catture provate-e-fallite si applica
                    // a OGNI beta-cutoff (anche quando la best move e' QUIET), non solo
                    // quando il cutoff e' causato da una cattura. Senza questo, le catture
                    // speculative che falliscono prima di un cutoff quiet non accumulano
                    // mai segnale negativo -> capture_history resta netto-positiva ->
                    // vengono sovra-ordinate -> -Elo. (Era il bug che rendeva la feature
                    // negativa: -21 @div1, -12 @div16.)
                    if (g_capture_hist) {
                        int cap_malus = td_stat_bonus(depth);
                        for (int c = 0; c < n_searched_captures; c++) {
                            int cm = searched_captures[c];
                            if (cm == move) continue;   // la cattura-cutoff ha gia' il bonus
                            int cvic = td_captured_piece(td, get_move_target(cm));
                            td_update_history(td.capture_history[get_move_piece(cm)][get_move_target(cm)][cvic], -cap_malus);
                        }
                    }

                    return best_score;
                }
            }
        }
    }


    // Checkmate / Stalemate
    if (legal_moves == 0) {
        if (in_check) return -mate_value + td.ply;
        return 0;
    }

    // V2 (PriorBonus, default OFF -> byte-identico): nodo FAIL-LOW (hash_flag ancora alpha
    // = nessuna mossa ha battuto alpha) -> la mossa PRECEDENTE (quella che ci ha portato qui)
    // era forte -> bonus alla sua history. Quiet: main + 1-ply continuation; cattura: capture-hist
    // (vittima da captured_stack, mv1 e' gia' stata fatta = via dal board).
    if (g_prior_bonus && hash_flag == hash_flag_alpha) {
        int mv1 = td.move_stack[td.ply];
        if (mv1) {
            int p1 = get_move_piece(mv1), t1 = get_move_target(mv1);
            int pbonus = g_prior_bonus_scale * td_stat_bonus(depth) / 100;
            if (!get_move_capture(mv1)) {
                td_update_history(td.history_moves[p1][t1], pbonus);
                if (td.ply >= 1) {
                    int mv2 = td.move_stack[td.ply - 1];
                    if (mv2) td_update_history(td.continuation_history[get_move_piece(mv2)][get_move_target(mv2)][p1][t1], pbonus);
                }
            } else if (g_capture_hist) {
                int vic = td.captured_stack[td.ply];
                if (vic >= 0) td_update_history(td.capture_history[p1][t1][vic], pbonus);
            }
        }
    }

    td_corr_update(td, corr_idx, static_eval, best_score, hash_flag, depth, in_check, best_move, excluded_move);
    if (!excluded_move) store_tt(td.hash_key, best_move, best_score, depth, hash_flag, td.ply, store_pv, tt_eval_undamp(node_raw_eval, td.fifty));

    return best_score;
}

// ============================================================================
// UCI INFO
// ============================================================================

static void print_search_info(ThreadData& td, int depth, int score) {
    int elapsed = get_time_ms() - search_start_time;
    if (elapsed == 0) elapsed = 1;

    U64 total = 0;
    for (int i = 0; i < num_threads; i++) {
        total += thread_data[i].nodes;
    }

    U64 nps = (total * 1000) / elapsed;

    std::lock_guard<std::mutex> lock(output_mutex);

    if (score > -mate_value && score < -mate_score) {
        printf("info depth %d seldepth %d score mate %d nodes %llu nps %llu time %d pv ",
            depth, td.seldepth, -(score + mate_value) / 2 - 1, total, nps, elapsed);
    }
    else if (score > mate_score && score < mate_value) {
        printf("info depth %d seldepth %d score mate %d nodes %llu nps %llu time %d pv ",
            depth, td.seldepth, (mate_value - score) / 2 + 1, total, nps, elapsed);
    }
    else {
        printf("info depth %d seldepth %d score cp %d nodes %llu nps %llu time %d pv ",
            depth, td.seldepth, score, total, nps, elapsed);
    }

    for (int i = 0; i < td.pv_length[0]; i++) {
        print_move(td.pv_table[0][i]);
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
static const int LSMP_SkipSize[8]  = { 1, 1, 2, 2, 2, 3, 3, 3 };
static const int LSMP_SkipPhase[8] = { 0, 1, 0, 1, 2, 0, 1, 2 };

static void thread_search(int thread_id, int max_depth) {
    ThreadData& td = thread_data[thread_id];

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
    // #5 low-ply history e' ply-indexed near-root -> NON deve persistere tra mosse
    // (mischierebbe posizioni diverse allo stesso ply). Azzerata per-search se attiva
    // (gated: OFF = nessun lavoro extra = byte-identico).
    if (g_lowply) memset(td.lowply_history, 0, sizeof(td.lowply_history));
    td.move_stack[0] = 0;  // root has no previous move

    int prev_score = 0;
    int prev_best_move = 0;
    int prev_completed_score = 0;   // last completed iteration's score (TimeMgmt drop)

    // Stagger starting depths for thread diversity
    int start_depth = 1 + (thread_id % 2);

    // DiverseSMP (#2), WIDER-ONLY variant: per-thread LMR reduction bias. Helpers
    // (id>0) only ever search WIDER (negative bias = less reduction); the magnitude
    // grows with id but is capped at g_diverse_smp_amount. Thread 0 stays canonical (0).
    // RATIONALE: an earlier symmetric (+/-) version hijacked the root move -- a helper
    // with a *positive* bias prunes harder, reaches a higher nominal depth, and wins the
    // depth-based result selection with an over-reduced (worse) move. amt3 measured -79
    // Elo @8+0.08 from exactly this. Forcing bias <= 0 means every helper reaches a
    // LOWER depth than thread 0, so it can never hijack the result; helpers contribute
    // only via the shared TT (seeding less-pruned entries). For inter-helper diversity
    // run DiverseSMPAmount >= 2 (else all helpers collapse to -1).
    // Re-read each search so DiverseSMPAmount (SPSA) takes effect live. Applied in the
    // LMR block only when g_diverse_smp is on (guarded there).
    if (thread_id == 0) {
        td.lmr_bias = 0;
    } else {
        int mag = 1 + (thread_id - 1) / 2;
        if (mag > g_diverse_smp_amount) mag = g_diverse_smp_amount;
        td.lmr_bias = -mag;   // wider-only: helpers never search "deeper" -> no hijack
    }

    for (int current_depth = start_depth; current_depth <= max_depth; current_depth++) {
        if (stop_threads.load(std::memory_order_relaxed)) break;

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
        sf_root_sync(td);

        // Aspiration window: start narrow around the previous score and widen
        // incrementally on failures, instead of re-searching the full window.
        int alpha = -infinity, beta = infinity, delta = g_asp_init_delta;
        if (current_depth >= 4) {
            alpha = (prev_score - delta > -infinity) ? prev_score - delta : -infinity;
            beta  = (prev_score + delta <  infinity) ? prev_score + delta :  infinity;
        }

        int score;
        U64 iter_nodes = 0;
        while (true) {
            U64 nodes_before_call = td.nodes;
            score = td_negamax(td, alpha, beta, current_depth, false);
            iter_nodes = td.nodes - nodes_before_call;   // nodes of the last (in-window) search
            if (stop_threads.load(std::memory_order_relaxed)) break;

            if (score <= alpha) {            // fail low: lower alpha, pull beta toward midpoint
                beta = (alpha + beta) / 2;
                alpha = (score - delta > -infinity) ? score - delta : -infinity;
                delta += delta * g_asp_grow / 100;
            }
            else if (score >= beta) {        // fail high: raise beta
                beta = (score + delta < infinity) ? score + delta : infinity;
                delta += delta * g_asp_grow / 100;
            }
            else {
                break;                       // score is inside the window
            }
        }

        if (stop_threads.load(std::memory_order_relaxed)) break;

        prev_score = score;

        if (td.pv_length[0] > 0) {
            td.best_move = td.pv_table[0][0];
            td.best_score = score;
            td.depth = current_depth;

            // Only main thread prints
            if (thread_id == 0) {
                print_search_info(td, current_depth, score);
            }
        }

        // Soft time management: the main thread decides whether to start another
        // iteration. If the best move is unstable (just changed), allow more
        // time, up to the hard cap (stoptime).
        if (thread_id == 0 && timeset) {
            int soft = soft_time_limit;                    // absolute timestamp (starttime + optimum)
            if (current_depth > start_depth && td.best_move != prev_best_move)
                soft += (stoptime - soft_time_limit) * g_tm_instab / 100;

            // Score-drop time extension (TimeMgmt): a falling score vs the previous
            // completed iteration suggests the position is turning against us ->
            // grant extra time, scaled by the drop, capped at ~25% of the headroom.
            if (g_time_mgmt && current_depth > start_depth) {
                int drop = prev_completed_score - score;   // >0 = got worse
                if (drop > 8) {
                    int d = (drop > 200) ? 200 : drop;
                    soft += (stoptime - soft_time_limit) * d / g_tm_drop_div;
                }
            }

            // Node-based time management. IMPORTANT: soft is an ABSOLUTE timestamp,
            // so we must scale the DURATION (soft - starttime), never the timestamp
            // itself. REDUCE-ONLY (scale <= 1.0): if the best move dominates the
            // node count we're confident -> stop sooner. We never inflate the
            // duration (that would push soft past stoptime -> burn the whole clock).
            if (g_node_tm && current_depth >= 6 && iter_nodes > 0) {
                double frac = (double)td.root_bestmove_nodes / (double)iter_nodes;
                if (frac > 1.0) frac = 1.0;
                double scale = 1.4 - frac;                 // frac>=0.4 -> <1.0 (reduce); else capped
                if (scale > 1.0) scale = 1.0;
                if (scale < 0.6) scale = 0.6;
                int dur = soft - starttime;                // current optimum duration (ms)
                if (dur < 0) dur = 0;
                soft = starttime + (int)(dur * scale);
            }

            if (soft > stoptime) soft = stoptime;          // never exceed the hard cap

            prev_best_move = td.best_move;
            prev_completed_score = score;
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
    for (auto& t : search_threads) {
        if (t.joinable()) t.join();
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
    stop_search_threads();          // stop a previous search (if any)
    wait_for_search_done();         // join it and its helpers

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
            printf("info depth 1 score cp %d pv ", tb_score);
            print_move(tb_move);
            printf("\nbestmove ");
            print_move(tb_move);
            printf("\n");
            fflush(stdout);
            return;
        }
    }

    stop_threads.store(false, std::memory_order_relaxed);  // clear before spawning
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
        thread_data[i].best_move = 0;
        thread_data[i].best_score = -infinity;
        thread_data[i].depth = 0;
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
            if (!thread_data[i].best_move) continue;
            if (thread_data[i].best_score < min_score) min_score = thread_data[i].best_score;
            if (thread_data[i].best_score >= mate_score &&
                (mate_thread < 0 || thread_data[i].best_score > thread_data[mate_thread].best_score))
                mate_thread = i;
        }
        if (mate_thread >= 0) {
            best_move  = thread_data[mate_thread].best_move;
            best_score = thread_data[mate_thread].best_score;
            best_depth = thread_data[mate_thread].depth;
        } else if (min_score < infinity) {
            int       vote_moves[MAX_THREADS];
            long long vote_w[MAX_THREADS];
            int nv = 0;
            for (int i = 0; i < num_threads; i++) {
                if (!thread_data[i].best_move) continue;
                long long w = (long long)(thread_data[i].best_score - min_score + 14) *
                              (thread_data[i].depth > 0 ? thread_data[i].depth : 1);
                int j;
                for (j = 0; j < nv; j++) if (vote_moves[j] == thread_data[i].best_move) break;
                if (j == nv) { vote_moves[nv] = thread_data[i].best_move; vote_w[nv] = 0; nv++; }
                vote_w[j] += w;
            }
            int win = 0;
            for (int j = 1; j < nv; j++) if (vote_w[j] > vote_w[win]) win = j;
            // fra i thread che votano la vincitrice: depth max, tie score max
            int bt = -1;
            for (int i = 0; i < num_threads; i++) {
                if (thread_data[i].best_move != vote_moves[win]) continue;
                if (bt < 0 || thread_data[i].depth > thread_data[bt].depth ||
                    (thread_data[i].depth == thread_data[bt].depth &&
                     thread_data[i].best_score > thread_data[bt].best_score))
                    bt = i;
            }
            if (bt >= 0) {
                best_move  = thread_data[bt].best_move;
                best_score = thread_data[bt].best_score;
                best_depth = thread_data[bt].depth;
            }
        }
    } else {
        // Legacy: vince il thread piu' profondo (tie: score piu' alto).
        for (int i = 1; i < num_threads; i++) {
            if (thread_data[i].depth > best_depth ||
                (thread_data[i].depth == best_depth && thread_data[i].best_score > best_score)) {
                if (thread_data[i].best_move != 0) {
                    best_move = thread_data[i].best_move;
                    best_score = thread_data[i].best_score;
                    best_depth = thread_data[i].depth;
                }
            }
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
        ThreadData& td0 = thread_data[0];
        moves rescue[1];
        td_generate_moves(td0, rescue, false);   // all pseudo-legal moves
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

    printf("bestmove ");
    if (best_move) {
        print_move(best_move);
    }
    else if (thread_data[0].pv_table[0][0]) {
        print_move(thread_data[0].pv_table[0][0]);
    }
    else {
        printf("(none)");   // truly no legal move (stalemate/checkmate handled earlier)
    }
    printf("\n");
    fflush(stdout);
}
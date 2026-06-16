#pragma once
#ifndef THREADS_H
#define THREADS_H

#include "defs.h"
#include "search.h"
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>

#define MAX_THREADS 64

// Thread-local data structure
struct ThreadData {
    int thread_id;
    int lmr_bias;    // DiverseSMP: per-thread LMR reduction offset (helpers only; 0 for main)

    // Board state copy
    U64 bitboards[12];
    U64 occupancies[3];
    int side;
    int enpassant;
    int castle;
    U64 hash_key;
    U64 pawn_key;        // P2.1: Zobrist solo-pedoni, mantenuta incrementale in make/unmake
    int fifty;
    int plies_from_null; // P2.2: mosse reali dall'ultima null nel cammino corrente
    bool in_nmp_verif;   // P1.6: dentro una verification search (null disattivata)
    int seldepth;        // max ply raggiunto (info UCI)
    
    // Repetition detection
    U64 repetition_table[2048];   // 2026-06-12: era 1000, OOB su partite 440+ mosse (vedi defs.h)
    int repetition_index;
    
    // Search state
    int ply;
    U64 nodes;

    // Move that led to each ply (0 = root / null move). Used by the
    // counter-move heuristic to know the "previous move" at a node.
    int move_stack[max_ply + 8];
    // Captured piece of the move that led to each ply (-1 if quiet). Parallel to
    // move_stack; set together. Used by V2 (PriorBonus) capture-history extension to
    // know the victim of the prior move (already made, gone from the board).
    int captured_stack[max_ply + 8];

    // Move ordering. +8 slack on the ply dimension: a node entering at
    // ply == max_ply-1 increments td.ply (and reads pv_length[ply+1]), so the
    // ply index can transiently reach max_ply. Without the slack that is an OOB
    // write that aliases pv_table[0][0] -> wild loop bound -> access violation.
    int killer_moves[2][max_ply + 8];
    int history_moves[12][64];
    // Counter-move heuristic: counter_moves[prev_piece][prev_to] = the quiet
    // reply that most recently caused a beta cutoff after that previous move.
    int counter_moves[12][64];
    // Continuation history: [prev_piece][prev_to][piece][to]. A 2-ply history
    // that scores a quiet move in the context of the move that led to this
    // node. int16_t keeps it ~1.1 MB/thread (values are bounded to +/-HISTORY_MAX).
    int16_t continuation_history[12][64][12][64];
    // Extra continuation histories at 2-ply and 4-ply back (only used when
    // ContHistMulti is on): same layout, keyed on the move 2 / 4 plies earlier.
    // Like continuation_history, NOT reset per-search (persist across moves).
    int16_t cont_hist_2[12][64][12][64];
    int16_t cont_hist_4[12][64][12][64];
    // Extra continuation histories at 3-ply and 6-ply back (only used when ContHist36
    // is on): same layout. Like the others, NOT reset per-search (persist across moves;
    // zeroed by value-init on thread creation, same as continuation_history).
    int16_t cont_hist_3[12][64][12][64];
    int16_t cont_hist_6[12][64][12][64];
    // Capture history: [moving piece][to square][captured piece].
    int capture_history[12][64][12];

    // PV table. +8 slack on the ply dimension for the same reason as
    // killer_moves above (pv_length[ply+1] / pv_table[ply+1] lookahead at the
    // deepest searched node).
    int pv_length[max_ply + 8];
    int pv_table[max_ply + 8][max_ply + 8];

    // Static eval at each ply, for the "improving" heuristic: compare this
    // node's static eval to our own eval two plies ago (our previous turn).
    // A rising eval lets the search prune/reduce more aggressively.
    int eval_stack[max_ply + 8];
    // cutoffCnt per-ply (SF): quante volte un nodo a questo ply ha fatto fail-high
    // di recente. Letto nella LMR (riduci di piu' se il figlio cutta molto). +8 slack
    // come gli altri stack ply-indicizzati. Tracciato sempre; usato solo se penalty>0.
    int cutoff_cnt[max_ply + 8];

    // Results
    int best_move;
    int best_score;
    int depth;

    // Node-based time management: nodes spent on the current best root move in
    // the last completed iteration. Compared to the iteration's total nodes to
    // gauge confidence (best move dominating nodes => stop sooner).
    U64 root_bestmove_nodes = 0;

    // Move-ordering diagnostic (gated da g_cutoff_stats, default off). Contati per-thread
    // sui beta-cutoff della main search: fh_nodes = nodi fail-high; fh_first = cutoff sulla
    // 1a mossa cercata; fh_move_sum = somma indici-mossa al cutoff (per indice medio).
    // first-move-cutoff rate = fh_first/fh_nodes (84.76% storico; gap vs SF = ordering).
    U64 fh_nodes = 0;
    U64 fh_first = 0;
    U64 fh_move_sum = 0;
    U64 fh_tt = 0;        // cut-node con tt_move presente
    U64 fh_tt_first = 0;  // cut-node con tt_move presente E cutoff sulla 1a mossa

    // Correction history: learned (search - static_eval) gap bucketed by
    // [side][pawn-structure key]. Size MUST match CORR_SIZE (1<<14) in threads.cpp.
    // ~128 KB/thread. Cleared in init_threads.
    int corr_hist[2][1 << 14];

    // Extra correction tables (only used when CorrHistMulti is on): keyed by
    // [side][minor-piece key] and [side][major-piece key]. Same size as corr_hist.
    int corr_hist_minor[2][1 << 14];
    int corr_hist_major[2][1 << 14];

    // Continuation correction history (CorrHistCont, default OFF): SF-style learned
    // (search - static_eval) gap keyed by the TWO moves that led INTO this node —
    // the move 1-ply back and 2-ply back: [piece2ply][to2ply][piece1ply][to1ply].
    // Same shape as continuation_history but BOTH coords are already-played moves
    // (the path into the node), because it corrects the node's static eval, not a
    // candidate move. Stored in cp*CORR_GRAIN like the other corr tables; int16
    // keeps it ~1.1 MB/thread (clamp g_corr_cap*CORR_GRAIN < 32767 fits int16).
    int16_t cont_corr_hist[12][64][12][64];

    // Pawn history (PawnHistory toggle, default off): quiet-move ordering keyed by
    // pawn structure -> [pawn-key bucket][piece][to]. SF weights this 2x (as much as
    // main history); it was a whole missing dimension for us. int16 keeps it ~12.6
    // MB/thread. Per-thread (here) = SMP-safe. Cleared in init_threads.
    static constexpr int PAWN_HIST_SIZE = 1 << 13;          // 8192 pawn-structure buckets
    static constexpr int PAWN_HIST_MASK = PAWN_HIST_SIZE - 1;
    int16_t pawn_history[PAWN_HIST_SIZE][12][64];

    // Threat-ordering cache (ThreatOrdering, default OFF). Squares attacked by enemy
    // pieces grouped by the cheapest attacker's value. Computed once per node (keyed
    // by hash_key) and read in td_score_move. Unused/untouched when the toggle is off.
    U64 threat_key      = 0;   // hash_key the three bitboards below are valid for
    U64 threat_by_pawn  = 0;   // squares attacked by enemy pawns
    U64 threat_by_minor = 0;   // + enemy knights/bishops
    U64 threat_by_rook  = 0;   // + enemy rooks

    // Check-ordering cache (CheckOrdering, default OFF). check_sq[piece type] = squares
    // from which a piece of that type gives a DIRECT check to the enemy king. Computed
    // once per node (keyed by hash_key). Unused/untouched when the toggle is off.
    U64 check_key   = 0;
    U64 check_sq[6] = { 0, 0, 0, 0, 0, 0 };   // index by piece type 0=P 1=N 2=B 3=R 4=Q 5=K

    // Low-ply history (#5, LowPlyHistory, default OFF): [ply][piece][target], used only
    // in the first LOW_PLY_MAX plies for quiet ordering near the root. Cleared per-search
    // when the toggle is on (ply-indexed => transient, must NOT persist across moves).
    static constexpr int LOW_PLY_MAX = 4;
    int lowply_history[LOW_PLY_MAX][12][64];

    // Opaque per-thread handle for the incremental NNUE mirror (see sf_bridge).
    // Owned here: created in init_threads, destroyed on re-init / shutdown.
    void* sfpos = nullptr;

    // Per-thread static-eval cache. Maps a position to its NNUE eval so we can
    // skip the (expensive ~60% of node time) sf_pos_eval forward pass when the
    // SAME position is evaluated again — aspiration / PVS re-searches and
    // transpositions hit this a lot. Safe because the NNUE accumulator is lazy:
    // sf_pos_do/undo keep the dirty chain regardless, so a skipped eval is just
    // computed later by the first descendant that needs it.
    //   Key = hash_key mixed with fifty: SF's eval is dampened by the 50-move
    //   counter, which hash_key does NOT encode, so fifty MUST be in the key or
    //   we'd return a stale eval for the same position at a different fifty.
    static constexpr int EVAL_CACHE_BITS = 16;          // 65536 entries
    static constexpr int EVAL_CACHE_SIZE = 1 << EVAL_CACHE_BITS;
    static constexpr U64 EVAL_CACHE_MASK = EVAL_CACHE_SIZE - 1;
    struct EvalCacheEntry { U64 key; int eval; };
    EvalCacheEntry eval_cache[EVAL_CACHE_SIZE];          // ~1 MB / thread
};

// Global thread management
extern std::vector<std::thread> search_threads;
extern std::vector<ThreadData> thread_data;
extern std::atomic<bool> stop_threads;
extern std::atomic<U64> total_nodes;
extern std::mutex cout_mutex;
extern int num_threads;

// Thread management functions
extern void init_threads(int thread_count);
extern void copy_board_to_thread(ThreadData& td);
extern void start_search_threads(int depth);
extern void stop_search_threads();
extern void wait_for_threads();

// Multi-threaded search
extern void search_position_mt(int depth);

// Asynchronous driver: run the search on a background thread so the UCI loop
// stays responsive to "stop" / "go infinite".
extern std::thread search_master;
extern void launch_search(int depth);
extern void wait_for_search_done();

// Soft time limit (absolute ms, like stoptime). The main thread stops starting
// new iterative-deepening iterations once it passes this; stoptime stays the
// hard cap checked inside the search. Set by parse_go alongside stoptime.
extern int soft_time_limit;

// Self-play data logging (Phase 1: policy-net training data). When enabled, the
// engine appends "FEN<TAB>bestmove<TAB>score<TAB>depth" for each completed root
// search to the dataset file. Controlled via the DataLog / DataFile UCI options.
extern void set_data_log_enabled(bool enabled);
extern void set_data_log_file(const char* path);

// (Policy-net: RIMOSSA 2026-06-11 — capitolo chiuso con misure conclusive, vedi
//  notes/ANALISI_CODICE_OTTIMIZZAZIONI.md §P5. Il codice vive nella storia git.)

// ---- Bundle 3.9 (2026-06-11): micro-fix dietro toggle default ON --------------
extern void set_mate_dist(bool enabled);       // P1.4  mate-distance pruning
extern void set_draw_dither(bool enabled);     // P1.11 draw = ±1cp (anti shuffle-blindness)
extern void set_ttcut_bonus(bool enabled);     // P1.13 history bonus al ttMove sul TT-cutoff
extern int  g_ttcut_bonus_scale;               //       spin TTCutBonusScale (/100, SPSA)
extern void set_tt_age_refresh(bool enabled);  // P1.10a age refresh al probe-hit (def. threads.cpp, usato in tt.h)
extern void set_pawn_key_incr(bool enabled);   // P2.1  pawn key incrementale (node-identical)
// ---- Wave 3b (2026-06-11) ------------------------------------------------------
extern void set_tt_static_eval(bool enabled);  // P1.1  eval statica in TT (salva la forward NNUE)
extern void set_fast_rep_scan(bool enabled);   // P2.2  repetition scan a finestra min(fifty, plies_from_null)
extern void set_evasion_gen(bool enabled);     // P2.3  generazione evasioni mascherata (node-identical)
extern void set_thread_voting(bool enabled);   // P1.12 selezione SMP per voto pesato (default off)
// ---- Toggle da co-tune (default OFF, si accendono nel mega-SPSA 4.0) ------------
extern void set_qs_checks(bool enabled);       // P1.3 quiet check alla prima ply di qsearch
extern void set_nmp_verif(bool enabled);       // P1.6 NMP verification + no doppia null (spin NMPVerifDepth)
extern void set_lmp_improving(bool enabled);   // P1.7 LMP SF-style (spins LMPBase/LMPQuad), no cap d8
extern void set_evalcache_undamp(bool enabled);// N1 eval-cache senza fifty in chiave (default ON)
extern void set_probcut_tt(bool enabled);      // N2 probcut fail-high salvato in TT (default ON)

// DIAGNOSTIC ("eval" UCI command): static NNUE eval (cp, side-to-move relative)
// of the current global board. For cross-checking the NNUE port vs official SF.
extern int debug_eval_position();

// Eval-off diagnostic (UCI option "EvalOff") — NPS profiling only, not for play.
extern void set_eval_off(bool enabled);

// Static-eval cache on/off (UCI option "EvalCache") — A/B the eval cache.
extern void set_eval_cache(bool enabled);

// FinnyTables (NNUE accumulator refresh cache) on/off (UCI option "FinnyTables").
extern void set_finny(bool enabled);

// SingleBoard (single-board eval) and OccIncr (incremental occupancies) were
// consolidated into the code (2026-06-07): both unconditional now, no toggle.
// See sf_bridge.cpp (sf_pos_eval sync) and td_occ_update in threads.cpp.

// Time-management tunables (defined in threads.cpp; used by parse_go in uci_mt.cpp).
extern int g_tm_movestogo;
extern int g_tm_inc_frac;
extern int g_tm_max_mult;

// "Improving" heuristic on/off (UCI option "Improving") — A/B the eval-trend
// based pruning/reduction. Default on.
extern void set_improving(bool enabled);

// Node-based time management on/off (UCI option "NodeTM") — scale the optimum
// time DOWN when the best root move dominates the node count. Default on.
extern void set_node_tm(bool enabled);

// Advanced singular extensions on/off (UCI option "SingularExt") — double (+2)
// and negative (-1) extensions on top of the base singular extension. Default on.
extern void set_singular_ext(bool enabled);

// Correction history on/off (UCI option "CorrHist") — learned static-eval
// correction bucketed by pawn structure + side. Default off.
extern void set_corr_hist(bool enabled);

// Multi-table correction history on/off (UCI option "CorrHistMulti") — adds minor
// (N/B) and major (R/Q) material-keyed correction tables. Default off.
extern void set_corr_multi(bool enabled);

// Continuation correction history on/off (UCI option "CorrHistCont") — corrects the
// static eval by the learned gap keyed by the last two moves into the node. Default off.
extern void set_corr_cont(bool enabled);

// Pawn history on/off (UCI option "PawnHistory") — quiet-move ordering term keyed by
// pawn structure (SF-style, weighted 2x). Default off (byte-identical when off).
extern void set_pawn_hist(bool enabled);

// ProbCut on/off (UCI option "ProbCut") — prune when a capture's reduced
// verification search beats beta + ProbCutMargin. Default on (SPRT-validated +Elo).
extern void set_probcut(bool enabled);

// Continuation-history pruning/reduction on/off (UCI option "ContHistPrune") —
// uses continuation history in the LMR reduction and to prune bad late quiets.
// Default off (pending SPRT validation).
extern void set_cont_hist_prune(bool enabled);

// Multi-ply continuation history on/off (UCI option "ContHistMulti") — adds 2-ply
// and 4-ply continuation histories to ordering + updates. Default off.
extern void set_conthist_multi(bool enabled);

// Staged MovePicker on/off (UCI option "MovePicker") — lazy staged move generation
// (TT / good captures / killers / counter / quiets / bad captures) vs the default
// generate-all + pick-next. Default off (behaviour-preserving when off).
extern void set_move_picker(bool enabled);

// DiverseSMP (#2): give Lazy-SMP helper threads a small per-thread LMR reduction
// bias to diversify their search trees (cuts redundant work at high thread counts).
// Default off (behaviour-preserving). Magnitude = DiverseSMPAmount spin.
extern void set_diverse_smp(bool enabled);

// (TripleExt −75 / LMREnrich −25 / DeeperShallower: morti, RIMOSSI 2026-06-11.)
extern void set_multicut(bool enabled);       // BAKED on: conservative singular multi-cut

// ttPv (UCI "TTPv") — bit PV nella TT (bit 63): i nodi non-PV ricordati ex-PV vengono
// ridotti meno in LMR. Default off (behaviour-preserving = bit 0 + LMR invariata).
extern void set_ttpv(bool enabled);

// Candidati "notte" (2026-06-04) — tutti default OFF = byte-identico.
extern void set_nmp_eval_scale(bool enabled);  // NMPEvalScale: R null-move += min((eval-beta)/div,3)
extern void set_rfp_depth8(bool enabled);      // RFPDepth8: reverse-futility a depth<=8 (vs 6)
extern void set_razor_depth4(bool enabled);    // RazorDepth4: razoring a depth<=4 (vs 3)
extern void set_qfutility(bool enabled);       // QFutility: futility per-mossa in quiescence
extern void set_hist_bonus_sf(bool enabled);   // HistBonusSF: bonus history lineare-clampato

// CaptureHist (UCI "CaptureHist") — capture history [piece][to][victim] nell'ordering
// di good/bad captures (main + qsearch). Sempre stata attiva, mai validata in isolamento.
// Default ON (= comportamento attuale). OFF = contributo 0 + niente update (A/B pulito).
// Scaling via spin CaptureHistDiv (default 1 = byte-identico; >1 = caphist pesato meno).
extern void set_capture_hist(bool enabled);

// 4-way set-associative TT on/off (UCI option "TT4Way") — bucket of 4 entries
// with age-aware replacement, vs the direct-mapped default. Default off.
extern void set_tt_4way(bool enabled);

// TTEvalImprove (UCI "TTEvalImprove") — P1.1: use a bound-consistent tt_score in
// place of the static eval for pruning decisions (RFP/NMP/razor/futility/probcut/
// LMR-margin + qsearch stand-pat). Default ON.
extern void set_tt_eval_improve(bool enabled);

// UpcomingRep (UCI "UpcomingRep") — P1.2: cuckoo upcoming-repetition detection
// (SF has_game_cycle): raise alpha to draw when a reversible move can repeat a
// known position. Default ON.
extern void set_upcoming_rep(bool enabled);

// Toggle di ABLAZIONE dei fix 3.8 (default ON; OFF = comportamento 3.7). Con tutti
// OFF (incluso TTEvalImprove/UpcomingRep) la search e' identica alla 3.7.
extern void set_ttmove24(bool enabled);        // P0.1 TT move 24 bit
extern void set_see_fix(bool enabled);         // P0.2 SEE quiet + e.p.
extern void set_killer_lmr_fix(bool enabled);  // P0.3 sconto LMR killer
extern void set_qsearch_corr(bool enabled);    // P0.4 corr in qsearch
extern void set_improving_fix(bool enabled);   // P0.6 sentinel improving

// Lazy eval (UCI "LazyEval") — skip the NNUE static eval while in check. Default off.
extern void set_lazy_eval(bool enabled);

// Extra time management (UCI "TimeMgmt") — score-drop time extension. Default off.
extern void set_time_mgmt(bool enabled);

// Aggressive LMR (UCI "AggrLMR") — multi-ply history/conthist reductions via a
// smaller divisor + wider clamp (AggrLMRDiv / AggrLMRClamp spins). Default off.
extern void set_aggr_lmr(bool enabled);

// StatScore-LMR levers (2026-06-06) — fix per la SOTTO-RIDUZIONE vs SF15.1. Tre
// toggle indipendenti, default OFF = byte-identico. Tarabili via gli spin
// LMRStatScoreDiv / LMRStatScoreOffset / LMRContHistDiv / CutNodeLMRExtra.
extern void set_statscore_lmr(bool enabled);  // butterfly history -> riduzione continua (no clamp +/-1)
extern void set_conthist_lmr(bool enabled);   // conthist 1/2/4 ply -> riduzione continua nella LMR
extern void set_cutnode_lmr(bool enabled);    // riduzione extra sui cut-node
extern void set_threat_ordering(bool enabled); // ThreatOrdering: bonus/malus quiet per pezzo minacciato da uno di valore inferiore (SF-style)
extern void set_check_ordering(bool enabled);  // CheckOrdering: bonus quiet che danno scacco diretto, filtrati SEE>=-75 (SF-style)
extern void set_conthist36(bool enabled);      // ContHist36: aggiunge conthist 3-ply e 6-ply all'ordering quiet (SF #4)
extern void set_prior_bonus(bool enabled);     // PriorBonus (V2): su fail-low, bonus alla mossa precedente (conthist/main + capture-hist se cattura)
extern void set_lowply(bool enabled);          // LowPlyHistory (#5): history per-ply near-root nell'ordering quiet

// SPSA-tunable search parameters: set one by name (UCI spin option). Returns true
// if the name matched a known tunable. Used by an external SPSA tuner (fastchess).
extern bool set_search_param(const char* name, int value);

#endif

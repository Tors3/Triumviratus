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
    int fifty;
    
    // Repetition detection
    U64 repetition_table[1000];
    int repetition_index;
    
    // Search state
    int ply;
    U64 nodes;

    // Move that led to each ply (0 = root / null move). Used by the
    // counter-move heuristic to know the "previous move" at a node.
    int move_stack[max_ply + 8];

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

    // Results
    int best_move;
    int best_score;
    int depth;

    // Node-based time management: nodes spent on the current best root move in
    // the last completed iteration. Compared to the iteration's total nodes to
    // gauge confidence (best move dominating nodes => stop sooner).
    U64 root_bestmove_nodes = 0;

    // Correction history: learned (search - static_eval) gap bucketed by
    // [side][pawn-structure key]. Size MUST match CORR_SIZE (1<<14) in threads.cpp.
    // ~128 KB/thread. Cleared in init_threads.
    int corr_hist[2][1 << 14];

    // Extra correction tables (only used when CorrHistMulti is on): keyed by
    // [side][minor-piece key] and [side][major-piece key]. Same size as corr_hist.
    int corr_hist_minor[2][1 << 14];
    int corr_hist_major[2][1 << 14];

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

// Policy on/off (UCI option "UsePolicy") — A/B the hybrid policy vs pure NNUE.
extern void set_use_policy(bool enabled);

// Eval-off diagnostic (UCI option "EvalOff") — NPS profiling only, not for play.
extern void set_eval_off(bool enabled);

// Static-eval cache on/off (UCI option "EvalCache") — A/B the eval cache.
extern void set_eval_cache(bool enabled);

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

// Search bundle #3 — three small standard enrichments, each default off:
//  TripleExt  (+3-ply singular when hyper-forced), MultiCut (prune when the singular
//  exclusion search fails high >= beta), LMREnrich (extra LMR on ttCapture / cut-node).
extern void set_triple_ext(bool enabled);     // bundle #3 dead-end (dormant, default off)
extern void set_multicut(bool enabled);       // BAKED on: conservative singular multi-cut
extern void set_lmr_enrich(bool enabled);     // bundle #3 dead-end (dormant, default off)

// 4-way set-associative TT on/off (UCI option "TT4Way") — bucket of 4 entries
// with age-aware replacement, vs the direct-mapped default. Default off.
extern void set_tt_4way(bool enabled);

// Lazy eval (UCI "LazyEval") — skip the NNUE static eval while in check. Default off.
extern void set_lazy_eval(bool enabled);

// Extra time management (UCI "TimeMgmt") — score-drop time extension. Default off.
extern void set_time_mgmt(bool enabled);

// Aggressive LMR (UCI "AggrLMR") — multi-ply history/conthist reductions via a
// smaller divisor + wider clamp (AggrLMRDiv / AggrLMRClamp spins). Default off.
extern void set_aggr_lmr(bool enabled);

// SPSA-tunable search parameters: set one by name (UCI spin option). Returns true
// if the name matched a known tunable. Used by an external SPSA tuner (fastchess).
extern bool set_search_param(const char* name, int value);

#endif

#pragma once
#ifndef THREADS_H
#define THREADS_H

#include "defs.h"
#include "search.h"
#include <thread>
#include <vector>
#include <new>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>

struct NnStack;   // stack of the network's dirty entries (nn_dirty.h)

// ============================================================================
// Triumviratus search (8.1). The code is the engine's own: every block is written in its own form, with our
// structures and names. Many numeric constants still have inherited values (marked in search/01_params.inc); they
// will be retuned by SPSA. Engines studied are listed in the credits of the README.
// ============================================================================

#define MAX_THREADS 512

// Sizes of the history tables.
constexpr int QH_SIZE       = 64 * 64 * 5;   // move index: from | to<<6 | promotion type<<12
// Stage 1 of 8.1 (16/10/2026, ThreatHist): the quiet history has three more 64*64 tables, the corrections
// per threat context 1..3 of the factored form (qh_order/qh_push in 09_history.inc); the flat table stays
// at the usual index.
constexpr int QH_CTX_SIZE   = QH_SIZE + 3 * 64 * 64;
constexpr int LOW_PLY_SIZE  = 5;             // plies near the root with their own history
constexpr int NO_PC         = 12;            // "no piece": sentinel block of the continuations
constexpr int CAPT_NONE     = 6;             // captured type for promotions without capture and for e.p.

// A piece x arrival-square table: it is the "row" the continuation histories point to.
typedef int16_t PieceToRow[12][64];

// State of one ply of the current path.
struct NodeFrame {
    PieceToRow* cont_hist;        // continuation history chosen by the move leading to the child
    PieceToRow* cont_corr;        // continuation correction history, same key
    int  ply;
    int  move;                    // move made at this ply (0 = none, NULL_MOVE = null move)
    int  captured;                // piece captured by that move (P..k), -1 = none
    int  excluded;                // excluded move (singular search)
    int  static_eval;
    int  stat_score;
    int  move_count;
    int  cutoff_cnt;
    int  reduction;
    bool in_check;
    bool tt_pv;
    bool tt_hit;
    bool follow_pv;
    unsigned char ldse;           // the move being searched from this frame was extended by Ldse (LdseMax)
    unsigned char ldse_path;      // Ldse extensions along the line up to this node
    unsigned char negext;         // the move being searched from this frame is extended by a negative reduction (NegExtMax)
    unsigned char negext_path;    // extensions from negative reduction along the line up to this node
    int  refute;                  // CausalRed: move that gave the beta cutoff in this node (0 = none)
    unsigned char hctx;           // ThreatHist: threat context of the move made at this ply (0 = none)
    // C2 regime selectivity (21/10/2026; 13_search.inc), written only when the option that reads them is on.
    unsigned char c2pair;         // C2SameRegImp: Consilium Plebis experts of this node (White | Black << 4)
    unsigned char settle;         // C2SettleHorizon: the move searched from this frame got the settling ply
    unsigned char settle_path;    // C2SettleHorizon: settling plies along the line up to this node
    unsigned char nn_hold;        // C2HoldDepth / C2HoldQs: the children of this node keep its experts (06_nndirty.inc)
    unsigned char null_cuts;      // NmpSibCut: null-move cuts so far among the nodes of this frame, the children of one parent
};

// A root move with its line and the statistics collected on it.
struct RootLine {
    int       pv[max_ply + 4];
    int       pv_len;
    int       prev_pv[max_ply + 4];
    int       prev_pv_len;
    int       score;
    int       prev_score;
    int       avg_score;
    long long mean_sq;
    int       uci_score;
    bool      lower;              // the score is only a lower bound (fail high in the window)
    bool      upper;              // only an upper bound
    bool      prev_exact;
    int       sel_depth;
    U64       effort;             // nodes spent under this move
};

// Search limits, written by parse_go (uci_mt.cpp).
struct SearchLimits {
    int  time[2];
    int  inc[2];
    int  movestogo;
    int  movetime;
    int  depth;
    int  mate;
    U64  nodes;
    bool infinite;
    bool ponder;
    int  start_ms;
    bool use_tm() const { return time[0] || time[1]; }
};
extern SearchLimits g_limits;

constexpr int FRAME_OFFSET = 7;   // frame[FRAME_OFFSET] is the root; 7 sentinel frames before it

// alignas(128) (03/11/2026, speed at several threads, same tree): the ThreadData of the threads lie one after the other
// in ThreadDataVec, and without it the last line of one (nnstack, read at every make and unmake; nn_hold, written at
// every make) was the first line of the next (thread_id and the bitboards, written at every move): every move of a
// thread invalidated a line of its neighbour. Sampling at 8 threads on 8 cores of one socket against 1 thread (same
// binary, xperf 03/11/2026): the make's stack push 25x and the unmake's pop 12x more cycles per node. 128 and not 64:
// the spatial prefetcher of Intel cores fetches lines in pairs. Xeon, 5 positions x 10 s: nps +7.1% at 8 threads and
// +5.9% at 16; one thread level (xperf cycles per node +0.05% middlegame, +0.09% endgames).
struct alignas(128) ThreadData {
    int thread_id;
    // NumaSpread (03/11/2026, 15_threads.inc): the NUMA node the thread searches on, which is also the copy of the
    // shared histories it reads and writes (0 with one node); nn_home = the node its network handle was moved to.
    int numa_node = 0;
    int nn_home   = -1;
    // Its node's copy of the shared histories (stats_bind_threads, 03_tables.inc): base and mask once per thread,
    // so that every access is base[key & mask] as with one table.
    void*         st_follow = nullptr;   // PieceToRow (*)[2][NO_PC + 1][64]
    void*         st_pawn   = nullptr;   // PieceToRow*
    void*         st_corr   = nullptr;   // CorrSlot (*)[2]
    void*         st_corr_x = nullptr;   // int16_t (*)[2]
    unsigned long long st_pawn_mask = 0, st_corr_mask = 0;

    // Board of the thread
    U64 bitboards[12];
    U64 occupancies[3];
#if defined(USE_AVX512ICL)
    // avx512icl (05/11/2026, R26 §6): the same mailbox in bytes, one aligned cache line, kept by the make beside piece_on:
    // the network's catch-up builds the board before the pending moves from it in one register (06_nndirty.inc).
    alignas(64) std::int8_t pc8[64];
#endif
    int piece_on[64];             // piece on every square (P..k), -1 = empty
    int side;
    int enpassant;
    int castle;
    U64 hash_key;
    // Partial keys: td_keys_update (07_makemove.inc) sees them as a contiguous array of six U64 starting from
    // pawn_key (pawns, minors, majors, white and black non-pawns, sink) and writes into it with indices from a table,
    // without branches on the piece type. The order below is enforced by static_assert.
    U64 pawn_key;
    U64 mm_key[2];                // [0] = minor pieces (N,B,n,b), [1] = majors (R,Q,r,q)
    U64 np_key[2];                // non-pawns per colour, king included
    U64 key_sink;                 // sink: receives the XORs that belong to no key (never read)
    int fifty;
    int plies_from_null;
    int seldepth;

    U64 repetition_table[2048];
    int repetition_index;

    int ply;
    U64 nodes;
    U64 tb_hits;

    // Cammino corrente
    NodeFrame frames[max_ply + 16];
    int pv_length[max_ply + 8];
    int pv_table[max_ply + 8][max_ply + 8];
    signed char chk_hint[max_ply + 8];   // check on the child already computed by the parent (-1 = unknown)

    // Root
    RootLine root[256];
    int  root_count;
    int  pv_idx;
    int  pv_last;
    int  root_depth;
    int  completed_depth;
    int  root_delta;
    // (28/10/2026 night) LmrDeltaMul / root_delta of a null window, with the two values it came from (move_loop
    // divides again only when one of them changes).
    int  rw1_delta = 0, rw1_mul = 0, rw1_term = 0;
    int  sel_depth;
    int  nmp_min_ply;
    U64  best_move_changes;
    int  last_pv[max_ply + 4];        // line of the previous iteration for the move pv_idx
    int  last_pv_len;
    int  opt[2];                      // optimism of this thread (white side, black)
    int  root_side;                   // colour to move at the root (Contempt)
    bool smp_aff_on = false;          // SmpAff: this helper searches wider in the material regimes smp_aff_lo..hi
    int  smp_aff_lo = 0, smp_aff_hi = -1;
    int  draw_defend;                 // DrawDefend: draw value for the root colour in this search
    int  contempt_ahead;              // ContemptAhead (R19): draw value against the root colour in this search
    int  asp_prev_best = 0;           // AspStable (R19): best root move of the previous iteration
    int  asp_stable = 0;              //   iterations in a row with that best move
    void* st_cont_corr = nullptr;     // SmpContCorr (R19): the shared continuation-correction table of the node's copy
    int  c2_conv_root;                // C2ConvRoot: conversion decided at the root (+1 / -1 for the root side, 0 = none)
    int  deep_w;                      // weight 0..1024 of the Deep* terms for the current iteration
    // Analysis mode (09/10/2026, search/01_params.inc): EFFECTIVE values for the current iteration, already
    // weighted. In games they equal the parameters (Deep* at 0 = terms at 0), so the tree does not change.
    int  an_bits;                     // analysis components active in this search (0 = game)
    int  dz_rfp, dz_fut, dz_lmr, dz_nmp, dz_sing, dz_ext;   // = Deep* x deep_w / 1024
    int  hunt_from;                   // root depth from which hunt_mate applies (16 in games)
    int  crystal_ply;                 // AnCrystal: no pruning on the previous line up to this ply (-1)
    int  gd_heat;                     // AnGold: tactical "heat" of the root (-1000 in games = never)
    int  gd_rfp_pct;                  // AnGold: RFP margin in percent (100 = unchanged)
    int  gd_razor;                    // AnGold: razoring threshold lower by this much (0)
    int  gd_style;                    // AnGold: 0 neutral, 1 attack, 2 manoeuvre, 3 defence
    // "Long time" ramp (Lt*, 01_params.inc): weight and effective values of the levers for the current iteration.
    int  lt_w;
    int  lt_lmr_not_improving, lt_lmr_offset, lt_lmr_all_node, lt_lmr_no_tt_full;
    int  lt_nmp_improving, lt_quiet_fut_alpha, lt_rfp_max_depth, lt_nmp_verify_depth;
    int  lt_lmr_log_mul;              // effective value from which lt_red_log was built
    int  lt_red_log[256];             // reduction table of the thread when LmrLogMulLt != 0
    // SeePinned (05/10/2026): pinned pieces and pinners of both colours, computed once per position.
    U64  see_pin_key;                 // position for which the fields below hold
    U64  see_pinned[2];               // pieces of each colour pinned to their own king
    U64  see_pinners[2];              // sliders of each colour that pin an enemy piece
    // --- C2 piece values (agent SEE) --- (21/10/2026): the per-thread table of the regime piece values (see.cpp),
    // rebuilt only when the expert pair of the position changes; -1 = to rebuild (set at each search start).
    mutable int c2_see_pair;          // expert pair (White | Black << 4) the two tables below were built for
    mutable int c2_see_val[16];       // SEE value per piece code: SeeVal* + offset of the owner's regime
    mutable int c2_see_off[16];       // the offset alone; codes 12..15 = 0
    mutable int mat_val[16];          // material value per piece code (kings 0): the table the search reads

    // Result, read by the driver and by the vote between threads
    int best_move;
    int best_score;
    int depth;
    int best_reply;

    // History of this thread (the continuation, pawn and correction histories are shared: see 09_history.inc)
    int16_t    quiet_hist[2][QH_CTX_SIZE];
    int16_t    low_ply_hist[LOW_PLY_SIZE][QH_SIZE];
    int16_t    capt_hist[12][64][7];
    PieceToRow cont_corr[NO_PC + 1][64];
    int        tt_move_hist;

    // Rete incrementale
    void* nnpos = nullptr;
    NnStack* nnstack = nullptr;   // stack of the dirty entries of the nnpos handle: written by make and unmake (06_nndirty.inc)
    // Expert holds (25/10/2026, 06_nndirty.inc): the frame's nn_hold for the make in progress (step_in sets it and
    // clears it after the make, so the makes outside the search see 0). The root's experts of C2HoldRoot went with it
    // on 27/10/2026.
    unsigned char nn_hold = 0;
    // E1 (15/10/2026, speed at identical tree): REMOVED the per-thread evaluation cache (EVC8, 512 KB).
    // Counters: bench 20 2,519,149 lookups and 12 hits; a game of 40 plies at 500k nodes (cache never cleared
    // between moves) 9,382,726 lookups and 62 hits, and a perfect cache would have caught only 0.5% more. The
    // positions already evaluated are found by the TT, which keeps the evaluation (node_eval): the cache cost a read (often
    // outside L2), a write, two prefetches per move and 512 KB of L2 per thread.
};

// V1 (08/10/2026, speed, identical tree): the ThreadData on large pages. Each is ~3 MB read at random at every
// node (evaluation cache 1 MB, continuation corrections 1.4 MB, quiet histories): on 4 KB pages
// ~750 pages per thread, on 2 MB pages 2. Without the privilege the allocation falls back to normal pages.
#include "large_pages.h"
template <class T>
struct LargePageAllocator {
    using value_type = T;
    LargePageAllocator() = default;
    template <class U>
    LargePageAllocator(const LargePageAllocator<U>&) {}
    T* allocate(std::size_t n) {
        void* p = large_pages_alloc(n * sizeof(T));
        if (!p)
            throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) { large_pages_free(p); }
    template <class U>
    bool operator==(const LargePageAllocator<U>&) const { return true; }
    template <class U>
    bool operator!=(const LargePageAllocator<U>&) const { return false; }
};
using ThreadDataVec = std::vector<ThreadData, LargePageAllocator<ThreadData>>;

// Global thread management
extern std::vector<std::thread> search_threads;
extern ThreadDataVec thread_data;
extern std::atomic<bool> stop_threads;
extern int num_threads;
extern bool g_numa_spread;   // UCI NumaSpread (03/11/2026): the threads spread over the NUMA nodes (15_threads.inc)

extern void init_threads(int thread_count);
extern void copy_board_to_thread(ThreadData& td);
extern void stop_search_threads();
extern void wait_for_threads();
extern void search_position_mt(int depth);
extern std::thread search_master;
extern void launch_search(int depth);
extern void ponder_hit();
extern void wait_for_search_done();
extern void search_clear();            // ucinewgame: clears history and game state

extern void set_data_log_enabled(bool enabled);
extern void set_data_log_file(const char* path);

extern int debug_eval_position();
extern int debug_eval_position_raw();

// Search UCI options: printing and setting (01_params.inc).
extern void print_search_options();
extern void print_analysis_options();   // analysis mode (search/01_params.inc), also in the release
extern bool set_search_param(const char* name, int value);

#endif

// Triumviratus, a UCI chess engine.
// Copyright (C) 2026 Francesco Torsello
// SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)
// Additional term under section 7(b) of the GPLv3: anyone who copies, modifies or redistributes this file, in whole
// or in part, must preserve this notice and the attribution "Consilium inference: Francesco Torsello, Triumviratus
// (https://github.com/Tors3/Triumviratus)", in the file and in the credits (Appropriate Legal Notices) of any program
// that contains it.

// consilium/cn.h -- Consilium inference, written from the specification (block 6 of 8.1, 17/10/2026).
//
// Clean room: everything here comes from docs/audit_8.1/B6_SPECIFICA_CONSILIUM.md, which was derived from our trainer
// (serializer, quantization, data loader) and checked bit for bit against the previous inference used only as a
// black-box oracle. No line comes from the previous inference code (nnue/).
//
// The module reads the engine's board as it is (square a8 = 0, piece codes P N B R Q K p n b r q k = 0..11) and the
// per-move changes the engine's make writes on the stack (nn_dirty.h). It returns the two network outputs, psqt and
// positional, in internal units; what the engine does with them (nn_scale) stays in consilium_bridge.cpp.
//
// Since 18/10/2026 this is the engine's only inference; the previous one (nnue/nnue/) was removed.

#ifndef CN_H_INCLUDED
#define CN_H_INCLUDED

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../nn_dirty.h"

namespace cn {

// ---------------------------------------------------------------- network shape: TRANN5, Lex Hortensia
// TRANN5 (07/11/2026, docs/TRANN5_FORMAT.md): the Consilium Plebis inference with the changes of the Lex Hortensia
// design. Six HalfKA experts per perspective with the queen as a boundary (routing table of the file); feature rows of
// exactly 1024 bytes (no PSQT tail: 16 cache lines, never across a 4 KB page) and accumulators without PSQT sums; the
// PSQT from a piece-square table per output bucket (12 x 64 x 8 int32, 24 KB), kept per stack state from the moved
// pieces; 32 layer stacks (piece-count bucket x which sides have a queen, 08/11/2026); the pawn-king subnetwork with 128 hidden
// units, run only with no rook or queen and at most one minor on the board (the gate mean of the file elsewhere).
// The Consilium Plebis (TRANN4) inference is in the 8.1 source.
// Speed (Xeon Skylake-SP, xperf, deterministic PGO, cycles per node against 8.1 with Consilium Plebis, lab_90 of
// 07/11/2026, the same network built at load from the Consilium Plebis file): -3.0% middlegame / -3.2% endgames with
// large pages, -2.0% / -3.8% without.
constexpr int L1 = 1024, L1H = L1 / 2, L2 = 32, L3 = 32, NPSQT = 8, NBUCKETS = 32;
constexpr int NEXPERTS = 6;
constexpr int ROUTER_SIG = 54, ROUTER_CELLS = ROUTER_SIG * ROUTER_SIG;   // cell = sig(own) * 54 + sig(opponent)
constexpr int PK_INPUTS = 256, PK_H = 128, PK_ACTIVE = 18;              // pawn-king subnetwork
// Entries per thread (power of two), 192 bytes each: 8192 = 1.5 MB, more than the L2 of most processors.
// -DCN_PK_CACHE_LOG2=11 (384 KB) or 12 (768 KB) to measure; only the hit rate changes, never a value.
#ifndef CN_PK_CACHE_LOG2
    #define CN_PK_CACHE_LOG2 13
#endif
constexpr int PK_CACHE = 1 << CN_PK_CACHE_LOG2;
constexpr int ROWS_THR = 59808, ROWS_HKA = NEXPERTS * 32 * 704, ROWS_PP = 4560, ROWS_PAS = 96;
constexpr int HKA_PHASE_ROWS = 32 * 704;
constexpr int ROW8 = L1;   // int8 feature row: exactly 1024 bytes

// One output bucket. w1 is kept in two layouts: the file one (scalar path) and grouped by 4 inputs for the sparse
// SIMD product: w1g[g][o][k] = w1[o][4 l0_group(g) + k], in the order the SIMD forward stores l0 (cn.cpp).
struct alignas(64) Stack {
    std::int32_t b1[L2];
    std::int8_t  w1[L2][L1];
    std::int8_t  w1g[L1 / 4][L2][4];
    std::int32_t b2[L3];
    std::int8_t  w2[L3][2 * L2];
    std::int8_t  w2g[2 * L2 / 4][L3][4];   // w2g[g][o][k] = w2[o][4g + k]
    std::int32_t b3;
    std::int8_t  w3[2 * L2 + 2 * L3];
};

// All weights in one block of fixed size: it can be copied byte for byte (shared memory between processes).
struct alignas(64) Net {
    std::int16_t ftBias[L1];
    // Each feature row holds its 1024 int8 weights and nothing else (TRANN5).
    // (Widening the int8 blocks to int16 at load, one add per register instead of two, was measured on 17/10: +60%
    // L3 misses, -10% speed in the middlegame.)
    std::int8_t  wHka[ROWS_HKA][ROW8];   // int8 like the threat rows (trained inside +-127/256)
    std::int8_t  wThr[ROWS_THR][ROW8];   // stored in the order of thr_slot (cn.cpp)
    std::int8_t  wPP[ROWS_PP][ROW8];
    std::int8_t  wPas[ROWS_PAS][ROW8];
    Stack        stack[NBUCKETS];
    std::uint8_t router[ROUTER_CELLS];                // cell -> expert (section RTB1 of the file)
    alignas(64) std::int16_t pkB1[PK_H];              // pawn-king subnetwork (section PKN2)
    alignas(64) std::int16_t pkW1[PK_INPUTS][PK_H];   // one row per input
    alignas(64) std::int32_t pkB2[L2];
    alignas(64) std::int8_t  pkW2[L2][PK_H];
    alignas(64) std::int8_t  pkW2g[PK_H / 4][L2][4];  // SIMD product: pkW2g[g][o][k] = pkW2[o][4g + k]
    alignas(64) std::int32_t pkGateMean[L2];          // the outputs where the gate is closed (section PKN2)
    // PSQT table (section PST1): White's view, [engine piece][engine square][output bucket]; pieces 12-15 and square
    // 64 are zero rows, so that an absent piece or square of a move adds nothing without a branch.
    alignas(64) std::int32_t pst[16][65][NPSQT];
    std::uint64_t contentHash;   // FNV-1a of the file bytes, names the shared-memory copy
    // Every l1 weight of every stack within +-63 (set at load): two u8 x s8 pair sums then fit one int16, and the
    // sparse first layer without VNNI adds two groups before vpmaddwd (cn.cpp, CN_W1_PAIR).
    std::uint8_t  w1Pair;
};

// Reads a .nnue file (or an image of one in memory) into `net`. On failure returns false with a reason in `err`.
bool load_file(const char* path, Net* net, std::string* err);
bool load_memory(const unsigned char* data, std::size_t size, Net* net, std::string* err);
// The process-wide state a loaded network sets (the router copy read by the make, the pawn-king signals). load_memory
// calls it; a process that maps a copy loaded by another process (the Linux file alias of shared_net.h) must call it.
void adopt(const Net& net);

// PassedScale (26/10/2026): percent applied at load time to the PassedPawns rows; 100 = the file
// as it is. Read by the next load_*: the bridge reloads the network when the option changes.
void set_passed_scale(int pct);
int  passed_scale();

// The engine's board (consilium_bridge.h): twelve piece bitboards P..k, occupancy white/black/all, mailbox (-1 = empty).
struct Board {
    const std::uint64_t* bb;
    const std::uint64_t* occ;
    const int*           mb;
    int                  stm;   // 0 white, 1 black
};

// Accumulator of one perspective: the 1024 sums (32 cache lines; the PSQT is kept apart, Handle::psq).
struct alignas(64) Acc {
    std::int16_t v[L1];
};

// Refresh cache entry, one per (perspective, king square, phase): bias + HalfKA rows of the pieces it last saw, and
// those pieces. A refresh brings it to the board with only the pieces that changed, then adds the threat, pawn-pair
// and passed-pawn rows of the board.
// (TRANN5 lab, 08/11/2026) The pieces as a byte mailbox (0-11, 0xFF = empty): one 64-byte compare finds the squares
// that changed, and the entry is 33 cache lines instead of 34. -DCN_KC_BB: the twelve bitboards of 8.1.
struct alignas(64) KingCacheEntry {
    Acc           acc;
#ifdef CN_KC_BB
    std::uint64_t bb[12];
#else
    alignas(64) std::uint8_t mb[64];
#endif
};

// Per-thread state: the dirty stack the engine writes, one accumulator per stack state and perspective (bit 1 of
// NnState::computed[p]), the refresh cache, room for the row lists of one evaluation.
// Pawn-king cache entry: the exact key (pawns of both colours, king squares, side to move, bit 15 = valid) and the 32
// outputs of the subnetwork for it. Three cache lines exactly (21/10/2026: 148 bytes unaligned touched up to four).
struct alignas(64) PkEntry {
    std::uint64_t wp, bp;
    std::uint32_t kk;
    std::int32_t  out[L2];
};

// Pawn cache entry (02/11/2026, speed, same accumulators): the sum of the pawn-pair and passed-pawn rows of one pawn
// structure for one perspective and orientation, and its exact key (pawns of both colours; pm = perspective | 2 *
// orientation + 1, 0 = empty). A refresh adds this vector instead of the rows (about ten per refresh). 512 entries
// per thread (1.1 MB): the smaller caches beat the larger ones in cycles, an int16 entry being twice the bytes of an
// int8 row. Only at the refresh: on the pawn moves the few pawn-pair rows in use stay hot and beat the entries (the
// cache on the moves cost +5.5% / +3.5% cycles on the Xeon). xperf against the release (deterministic PGO, avx512):
// instructions -1.9% mg / -2.8% eg; cycles on the Xeon -1.04% / -0.48% mg, -1.04% eg; on the Zen 4 laptop (no large
// pages) within the noise (+0.57% / -0.42%).
// The three layouts of 02/11/2026 together (this cache, the threat groups and the pawn-pair order, cn.cpp): Xeon
// instructions -2.2% mg / -3.1% eg, cycles +0.12% / -0.86% mg and -1.21% / -0.58% eg in two runs; Zen 4 -0.49% / +0.35%
// mg and -0.26% / -1.38% eg in two runs, and in the final run of 8 rounds (low noise, median of the per-round ratios)
// 0.00% mg / +0.20% eg: level on the laptop, fewer instructions everywhere.
#ifndef CN_PAWN_CACHE_LOG2
    #define CN_PAWN_CACHE_LOG2 9
#endif
constexpr int PAWN_CACHE = 1 << CN_PAWN_CACHE_LOG2;
struct alignas(64) PawnCacheEntry {
    Acc           acc;
    std::uint64_t wp, bp;
    std::uint32_t pm;
};

struct alignas(64) Handle {
    NnStack       stack;
    Acc           acc[NN_STACK_SIZE][2];
    std::uint64_t cacheNet;           // contentHash of the network the cache was built from (0 = empty)
    KingCacheEntry    kcache[2][64][NEXPERTS];
    std::uint64_t pawnNet;            // contentHash of the network the pawn cache was built from
    PawnCacheEntry pawnc[PAWN_CACHE];
    std::uint64_t pkNet;              // contentHash of the network the pawn-king cache was built from
    PkEntry       pkc[PK_CACHE];
    // C2PkStale (27/10/2026): the cache entry of the last pawn-king lookup of this thread, and the flag the search
    // sets at shallow nodes: on a cache miss reuse that entry's outputs instead of computing (zeroed with the handle).
    const std::int32_t* pkLast;
    int           pkStale;
    alignas(64) std::int32_t x1[L2];  // l1 sums of the last evaluate (pawn-king outputs included): pk_effect
    // PSQT per stack state, White's view, all output buckets (valid when bit 2 of computed[0] is set).
    alignas(64) std::int32_t psq[NN_STACK_SIZE][NPSQT];
    alignas(64) unsigned char scratch[4 * 50 * 1024];
};

struct Out {
    int psqt;
    int positional;
};

void init();                      // index tables (once, at startup)
void reset(Handle& h);            // back to the root: one state, nothing computed
Out  evaluate(const Net& net, Handle& h, const Board& b, bool want_x1 = false);       // incremental along the stack
Out  evaluate_refresh(const Net& net, const Board& b, Acc acc[2]);  // from scratch, both perspectives
bool same_accumulators(const Handle& h, const Acc acc[2]);       // nnueverify: top of the stack == refresh
// Memory hint from the make: the HalfKA rows of a move (board before the move, phase after it). No state changes.
// TRIUMV_C2: phase = expert of White | expert of Black << 4.
void prefetch_move(const Net& net, const std::uint64_t* bb, int phase, int pc, int from, int toPc, int to, int capPc,
                   int capSq);

extern int g_bucket_override;     // EvalBucketOverride probe: -1 normal, 0..31 forces the layer stack
// Routing table of the last network loaded (copied at load: the make and Persp read it without the Net).
extern std::uint8_t g_router[ROUTER_CELLS];
// SmpRouteShift (21/10/2026): a second table with every expert shifted by a number of material classes, built by the
// main thread before the helpers start (router_alt_build), and the table the calling thread uses (nullptr = g_router).
// The make (nn_router_table in the bridge) and Persp (expert_of) read the same pointer, so a thread is consistent with
// itself; thread 0 always reads g_router.
extern std::uint8_t g_router_alt[ROUTER_CELLS];
extern thread_local const std::uint8_t* t_router;
// C2 speed (23/10/2026): set once some thread has chosen the shifted table (nn_thread_router(1)), never cleared.
// Until then every thread reads g_router and t_router is not read at all: with MinGW a thread_local is a call to the
// TLS emulation (~2.4% of the engine on 25/09/2026), and the make and Persp asked for it at every capture and
// evaluation. -DCN_NO_ROUTER_GUARD: t_router read every time, as before.
// A thread's t_router is non-null only after that same thread raised the flag, so a relaxed read is enough.
extern std::atomic<bool> g_router_alt_used;
inline const std::uint8_t* thread_router() {
#ifndef CN_NO_ROUTER_GUARD
    return g_router_alt_used.load(std::memory_order_relaxed) && t_router ? t_router : g_router;
#else
    return t_router ? t_router : g_router;
#endif
}
void router_alt_build(int shift);
inline int router_sig(int n, int b, int r, int q) {
    return (n < 2 ? n : 2) * 18 + (b < 2 ? b : 2) * 6 + (r < 2 ? r : 2) * 2 + (q < 1 ? q : 1);
}

// --- C2 signals for the search (agent A) ---
// Linear part of the pawn-king subnetwork in the evaluation: its outputs 30 and 31 enter the skip of the stack
// (x1[30] - x1[31]) and from there the positional output without an activation, so (pk[30] - pk[31]) * 600 / 16384 is
// the LINEAR part of what the pawns and kings add to the positional value, from the side to move's view (b.stm).
// Review 21/10/2026: not the whole of it; the 32 outputs also enter every l1 sum before the activations, and agent B
// measured this skip at ~1/15 of the full effect (mean 4.9 against 74.5, see pk_effect). Reads the
// thread's pawn-king cache and computes the entry on a miss (the evaluation of the same position then hits it). Only
// b.bb and b.stm are read.
int pk_skip(const Net& net, Handle& h, const Board& b);

// --- C2 signals (agent B) ---
// Read-only access to what the evaluation already computes (21/10/2026). pk_* read only b.bb and b.stm, except the
// pk_effect pair, which needs the whole board (output bucket).
const std::int32_t* pk_outputs(const Net& net, Handle& h, const Board& b);   // thread cache; computed on a miss
// The key of CorrPkW (bit i = output of rank i above its median); with compute = false, -1 when the board's entry is
// not in the thread's cache (no subnetwork pass).
std::int64_t  pk_signature(const Net& net, Handle& h, const Board& b, int bits, bool compute);
// What the pawn-king subnetwork adds to the positional output: `positional` minus the output with its 32 values taken
// out of the l1 sums. pk_effect: right after evaluate(h, b) returned `positional` (reads Handle::x1); pk_effect_refresh:
// from the accumulators of evaluate_refresh. Same integers in both.
int           pk_effect(const Net& net, Handle& h, const Board& b, int positional);
int           pk_effect_refresh(const Net& net, const Board& b, const Acc acc[2]);
int           expert_of_board(const Board& b, int p);                          // expert of perspective p
void          pk_measure(const Net& net);   // ranks and medians of the outputs (at load)

}  // namespace cn

#endif  // CN_H_INCLUDED

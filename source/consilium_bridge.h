// Triumviratus, a UCI chess engine.
// Copyright (C) 2026 Francesco Torsello
// SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)
// Additional term under section 7(b) of the GPLv3: anyone who copies, modifies or redistributes this file, in whole
// or in part, must preserve this notice and the attribution "Consilium inference: Francesco Torsello, Triumviratus
// (https://github.com/Tors3/Triumviratus)", in the file and in the credits (Appropriate Legal Notices) of any program
// that contains it.

#pragma once
#ifndef CONSILIUM_BRIDGE_H
#define CONSILIUM_BRIDGE_H

// consilium_bridge.h -- the engine's interface to the Consilium network (block 6 of 8.1; until 18/10/2026
// nnue_bridge.h). The network is evaluated by our module in consilium/; the rest of the engine sees only these
// plain C entry points and the dirty-stack types of nn_dirty.h.

#include "nn_dirty.h"   // the changes of each move (plain types, shared with the network module)

// Tables built once at startup: the feature indices of the network module.
void nn_init_tables(void);

// Name of the default network (CN_DEFAULT_NET_FILE, defined in consilium_bridge.cpp). Exposed here because the rest
// of the engine does not see the network module: one source for the name, no copies to keep in step.
const char* nn_default_net_name(void);
// Network shared between processes (shared_net.h): the outcome of the attach, "" in builds without it.
const char* nn_net_memory_status(void);

// Load the Consilium network from a file path. Returns 1 on success (file opened, hashes verified), 0 otherwise.
// Called once at startup, before any nn_pos_create().
int nn_load_net(const char* net_path);

// Reload the network at runtime (UCI option "EvalFile"). Safe to call when no search is running. Returns 1 on
// success, 0 if the path could not be loaded.
int nn_reload_big(const char* net_path);

// PassedScale (26/10/2026): percent on the PassedPawns rows of the network, applied at load time (cn::set_passed_scale);
// a change reloads the current network. Call with no search running. Returns 1 if the network was reloaded.
int nn_set_passed_scale(int pct);

// C2PkStale (27/10/2026): 1 = on a pawn-king cache miss the handle reuses the outputs of its last lookup (set by the
// search for shallow nodes before evaluating), 0 = compute as usual.
void nn_set_pk_stale(void* handle, int on);
// NumaSpread (03/11/2026, 15_threads.inc): the calling thread reads the network copy of its NUMA node; a thread's
// handle moved to the memory of its node.
void nn_bind_node_net(int node);
void nn_pos_rehome(void* handle, int node);

// No-ops kept for API stability: the refresh cache is always on, and "accstats" has no counters.
void nn_set_finny(int on);
void nn_acc_stats(void);

// Incremental evaluation (default on): accumulators updated from the parent state with the dirty stack. Off = a full
// refresh at every evaluation (diagnostic). Re-set the handle (nn_pos_set) after toggling.
void nn_set_incremental(int on);

// Diagnostic: compare the incremental evaluation with a full refresh at every evaluation (prints the first
// mismatches). Halves the speed; one thread recommended. Default off.
void nn_set_verify(int on);

// Evaluation scale in percent: matches the network's units to the search margins. UCI option "EvalScale" writes all
// eight output buckets; EvalScaleB0..B7 one each (bucket = (pieces - 1) / 4, the network's output bucket).
void nn_set_eval_scale(int pct);
void nn_set_eval_scale_bucket(int bucket, int pct);
int  nn_get_eval_scale_bucket(int bucket);   // current value (the default at startup), for the UCI line

// --- Constants of the evaluation scale ------------------------------------------------------------------------------
// The constants that turn the two network outputs (psqt, positional) into the evaluation: blend weights, damping by
// disagreement, material stretch, optimism terms and the mix of the fifty-move step. Tuned by our SPSA runs on this
// network (BAKE 09/09 and MOE1 30/09/2026). They change what the evaluation says, not only its size, which is the
// difference with EvalScale / EvalScaleB. The enumeration lets uci_mt.cpp declare the options without repeating names.
int         nn_eval_const_count(void);
const char* nn_eval_const_name(int i);
int         nn_eval_const_get(int i);
int         nn_eval_const_lo(int i);
int         nn_eval_const_hi(int i);
int         nn_set_eval_const(const char* name, int value);   // 1 if the name exists
#ifdef TRIUMV_FROZEN
void        nn_frozen_check(void);   // release builds: warns once when an option differs from the compiled value
#endif
int  nn_get_eval_scale(void);   // current EvalScale percent (to print "score cp" without it)
int  nn_last_opt_base(void* handle);    // last evaluation of the thread with optimism 0 (before fifty and scale)
int  nn_last_opt_coeff(void* handle);   // its optimism coefficient, in thousandths
int  nn_last_unadjusted(void* handle);  // last unadjusted value (before fifty and scale) of the thread
// The final evaluation from an unadjusted value: `bucket` = (pieces - 1) / 4 of the CURRENT position, which selects
// the per-bucket scale.
int  nn_finalize(int unadjusted, int rule50, int bucket);

// Evaluation from scratch (full refresh) of the engine's global board, for the "eval" command: bb[12] in the order
// P,N,B,R,Q,K,p,n,b,r,q,k, occ[3] = white/black/all, squares with a8 = 0 (the engine's numbering).
// Returns the evaluation (side to move, internal units == the engine's evaluation scale).
int nn_eval(int side_white, const unsigned long long* bb, const unsigned long long* occ, int rule50,
            int* raw_out = nullptr);

// ---------------------------------------------------------------------------------------------------------------------
// Per-thread handle: the dirty stack, the accumulators and the refresh cache. The network has no board of its own: it
// reads the engine's in nn_pos_eval. The engine's make writes the changes of each move on the handle's stack
// (nn_pos_stack, types in nn_dirty.h): one state per move made, removed at unmake; null moves add no state.
// ---------------------------------------------------------------------------------------------------------------------

// Create / destroy a per-thread handle (opaque).
void* nn_pos_create(void);
void  nn_pos_destroy(void* handle);
void  nn_pos_set_optimism(void* handle, int w, int b);   // OptPerThread

// Back to the root: the stack returns to one state, and the root accumulator is computed at the first evaluation.
void  nn_pos_set(void* handle);

// The handle's dirty stack, written directly by the engine's make and unmake.
NnStack* nn_pos_stack(void* handle);

// Consilium Plebis: the routing table of the loaded network (2,916 cells -> expert 0..7), for the make. The cell of a
// perspective is cn_sig(own side) * 54 + cn_sig(opponent); the signature counts N, B, R (capped at 2) and Q (at 1).
const unsigned char* nn_router_table(void);
// SmpRouteShift (21/10/2026): build the shifted copy (main thread, before the helpers start), and choose it (alt 1) or
// the network's table (alt 0) for the calling thread; the make, Persp and nn_router_table follow that choice.
void nn_router_alt_build(int shift);
void nn_thread_router(int alt);
static inline int cn_sig(int n, int b, int r, int q) {
    return (n < 2 ? n : 2) * 18 + (b < 2 ? b : 2) * 6 + (r < 2 ? r : 2) * 2 + (q < 1 ? q : 1);
}

// --- C2 signals for the search (agent A) ---
// What the pawn-king subnetwork adds linearly to the positional output (its two outputs that feed the skip of the
// stack), in the network's internal units, from the view of the side to move given by side_white. bb[12] as in
// nn_pos_eval; only pawns and kings are read. Uses the handle's pawn-king cache (computes and stores on a miss), so it
// is cheap for a position the thread evaluates anyway. 0 when no network is loaded.
int nn_pk_skip(void* handle, const unsigned long long* bb, int side_white);
// Empties the handle's pawn-king cache (search_clear, every new game); a null handle is ignored.
void nn_pk_cache_clear(void* handle);

// --- C2 signals (agent B) ---
// What only Consilium Plebis has, for the code outside the main search loop (21/10/2026). bb[12] as in nn_pos_eval.
// nn_pk_signature: the pawn-king subnetwork of the board (from the thread's cache, computed and stored on a miss),
// reduced to the signs of the `bits` most variable of its 32 outputs against their medians (bit i = output of rank i
// above its median; ranks and medians measured at load): a coarse key of the pawn structure as the network reads it
// (CorrPkW). With compute = 0 the subnetwork is never run: -1 when the board's entry is not in the cache. What the
// subnetwork adds to the evaluation (cn::pk_effect) is used inside the bridge (C2Conv*).
long long nn_pk_signature(void* handle, const unsigned long long* bb, int side_white, int bits, int compute);
// What the pawn-king subnetwork added to the positional output (side to move, positional units) in the last network
// evaluation of the thread (nn_pos_eval; not refreshed by an evaluation taken from the hash). Computed only while
// nn_want_pk_effect(1) or C2ConvW is on (one more pass of the layers after l1 per evaluation); otherwise stale/0.
// The search does not read it (it can belong to another position): it uses nn_pk_effect_fresh/now below.
void      nn_want_pk_effect(int on);
int       nn_last_pk_effect(void* handle);
// nn_x1_signs (CorrNetW, 09/11/2026): the state of the hidden layer of the layer stack in the thread's last network
// evaluation, as one bit per neuron (bit o = l1 sum o above zero, 30 neurons; the two skip sums are left out). Valid
// only right after nn_pos_eval evaluated this very position with the network (not an evaluation taken from the hash).
// The AVX-512 path always keeps the sums; the other paths keep them while nn_want_x1(1).
void      nn_want_x1(int on);
unsigned  nn_x1_signs(void* handle);

// --- Conversion push (the author's idea: push toward the endgame when it is winning), 21/10/2026 ---
// Shared by the search version (C2ConvOrder/C2ConvRed, 13_search.inc) and the evaluation version (C2ConvW,
// consilium_bridge.cpp c2_adjust), so that the two read "ahead or behind, and by how much" with one rule and can be
// tested separately or together. All OFF by default.
// c2_conv_lean: how far a signed opinion v (side to move's view) goes beyond the dead zone |v| <= from, capped at
// cap: 0 inside the zone, else sign(v) * (min(|v|, max(from, cap)) - from). Positive = the side to move is ahead.
static inline int c2_conv_lean(int v, int from, int cap) {
    const int mag = v < 0 ? -v : v;
    const int top = from > cap ? from : cap;
    const int a   = (mag < top ? mag : top) - from;
    return a <= 0 ? 0 : (v > 0 ? a : -a);
}
// The exact pawn-king effect (cn::pk_effect: what the subnetwork adds to the positional output, side to move's view,
// positional units) of the thread's CURRENT position, for the search version (C2ConvPkSrc 1). Never a value left by
// another position: nn_pk_effect_fresh is valid only right after nn_pos_eval evaluated this very position with the
// network (incremental path; the search knows it from nn_last_disagree >= 0) and costs one pass of the layers after
// l1 (none when C2ConvW already computed it in that evaluation); nn_pk_effect_now works at any time: it brings the
// accumulators of the top state up to date (the children need them anyway), reruns the forward pass and adds that
// pass (the engine must have caught up the dirty stack first, as for nn_pos_eval). 0 when no network is loaded.
// With nnueverify on, both are compared with the value from scratch (a difference counts as a bad evaluation).
int nn_pk_effect_fresh(void* handle, const unsigned long long* bb, const unsigned long long* occ, const int* mailbox,
                       int side_white);
int nn_pk_effect_now(void* handle, const unsigned long long* bb, const unsigned long long* occ, const int* mailbox,
                     int side_white);
// Evaluation terms of the search parameters C2Conv* and C2EvalExp0..7 (01_params.inc), set at the start of each
// search (15_threads.inc). All 0 = the evaluation is unchanged. conv = {weight /1024, from, cap}; exp_scale[8] /1024.
void     nn_set_c2_eval(const int* conv, const int* exp_scale);

// Memory hint from the make (board before the move, bb[12] as above; phase = material band after the move): asks for
// the network rows the move will change, so they are in cache when the child evaluates. Changes no state. Without a
// capture capPc/capSq repeat pc/from; a promotion arrives as toPc.
// The make calls it only where it pays (NN_MAKE_PREFETCH): xperf 18/10/2026, PGO, middlegame, -0.56% cycles per node on
// AVX2 and +2.8% on AVX-512 (there the update hides the latency by itself, and the call in every make costs more).
// So on in the AVX2 builds without AVX-512 (avx2, avx2-nopext, avxvnni); -DCN_MAKE_PREFETCH forces it on elsewhere,
// -DCN_NO_MAKE_PREFETCH turns it off.
#if !defined(CN_NO_MAKE_PREFETCH) && (defined(CN_MAKE_PREFETCH) || (defined(USE_AVX2) && !defined(USE_AVX512)))
    #define NN_MAKE_PREFETCH 1
#endif
void  nn_prefetch_move(const unsigned long long* bb, int phase, int pc, int from, int toPc, int to, int capPc,
                       int capSq);

// Evaluate the current position (internal units, side to move). The network reads the engine's board: bb[12] in the
// order P,N,B,R,Q,K,p,n,b,r,q,k, occ[3] = white/black/all, mailbox[64] with -1 = empty, all with a8 = 0. side_white and
// rule50 are the search's. The dirty states up to this position must be complete (the engine's nn_dirty_catch_up).
int   nn_pos_eval(void* handle, const unsigned long long* bb, const unsigned long long* occ, const int* mailbox,
                  int side_white, int rule50);

// |psqt - positional| of the last nn_pos_eval on this thread (19/10/2026): how much the two parts of the network
// disagree on the position, read by the search (RdDisagree). The caller resets it to -1 before an evaluation that may
// come from the hash instead of the network.
extern thread_local int nn_last_disagree;

// nnueverify: evaluations compared and evaluations that differed since startup.
unsigned long long nn_verify_count(void);
unsigned long long nn_verify_bad(void);

#endif // CONSILIUM_BRIDGE_H

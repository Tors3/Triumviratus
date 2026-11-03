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
#include "sstats.h"   // diagnostic counters, only with -DTRIUMV_SSTATS (03/10/2026)
#include "cstats.h"   // precision of the corrections, only with -DTRIUMV_CORRSTATS (06/10/2026)
#include "attacks.h"
#include "chess960.h"
#include "magic.h"
#include "misc.h"
#include "movegen.h"
#include "consilium_bridge.h"
#include "search.h"
#include "see.h"
#include "tt.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <atomic>
#include <chrono>
#include <thread>
#include <type_traits>
#include <cmath>
#if defined(__linux__)
#include <sched.h>    // NumaSpread on Linux: sched_getaffinity / sched_setaffinity (search/03_tables.inc)
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#endif
#include <cstdlib>
#include <cstring>
#include <iostream>

#ifndef _WIN32
#include <unistd.h> // getpid() (on Windows the pid comes from GetCurrentProcessId in windows.h)
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
unsigned short*    prof_cooc                   = nullptr;
unsigned long long prof_acc_inc = 0, prof_acc_refresh = 0, prof_ft_out = 0;
unsigned long long prof_n_inc = 0, prof_n_refresh = 0, prof_n_eval = 0;
unsigned long long prof_n_cols = 0, prof_n_upd = 0;
unsigned long long prof_n_thr_seen = 0, prof_n_thr_dead = 0;
unsigned long long prof_max_active = 0, prof_max_inc = 0;
unsigned long long prof_cols_thr = 0, prof_cols_pawn = 0, prof_n_refresh_calls = 0;
unsigned long long prof_cols_psq_inc = 0, prof_cols_thr_inc = 0, prof_cols_pawn_inc = 0;
unsigned long long prof_mp = 0, prof_hist = 0, prof_corr = 0, prof_gc = 0, prof_n_mg = 0;
unsigned long long prof_thr = 0, prof_see = 0, prof_isatk = 0, prof_rep = 0;
unsigned long long prof_idx_thr = 0, prof_idx_pawn = 0;
unsigned long long prof_n_thr_calls = 0, prof_n_see = 0, prof_n_isatk = 0;
unsigned long long prof_pawn_hit = 0, prof_pawn_miss = 0;
unsigned long long prof_refresh_same_orient = 0, prof_refresh_cross_orient = 0;
unsigned long long prof_dead_pair[8][8] = {};
#endif
#include "defs.h"

#include "syzygy.h"

#include <climits>
// windows.h (via defs.h) defines min and max as macros: with clang-cl they break std::min/std::max.
#undef min
#undef max

// ============================================================================
// threads.cpp -- the search. A single translation unit, split into search/*.inc included here in order
// (the hot-path functions are `static inline` and the compiler sees them together with their callers).
// The code, like the move generator, make/unmake and SEE, is the engine's own (see threads.h).
// ============================================================================
#include "search/01_params.inc"
#include "search/02_state.inc"
#include "search/03_tables.inc"
#include "search/06_nndirty.inc"
#include "search/07_makemove.inc"
#include "search/08_movegen.inc"
#include "search/09_history.inc"
#include "search/10_order.inc"
#include "search/11_queue.inc"
#include "search/12_quiesce.inc"
#include "search/13_search.inc"
#include "search/14_deepen.inc"
#include "search/15_threads.inc"
#include "search/16_tdperft.inc"

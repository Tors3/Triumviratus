// prunelog.h -- data collection for block 3 of 8.1 (only with -DTRIUMV_PRUNELOG; never in a release).
// Two records, written by threads.cpp (13_search.inc) in the working directory:
//  - prunenode_<thread>.bin: a sampled node before the node pruning (razoring, reverse futility, null move, ProbCut).
//    The node is first searched with a wide window at its own depth (the probe), which gives its value; then the
//    normal code runs and the record says which rule fired and what it returned.
//  - prunemove_<thread>.bin: a sampled move skipped by a move pruning rule, searched anyway with a null window at
//    alpha, at the depth the reduction would have given it and at the full depth.
// The probes change the tree of the data build only.
#pragma once
#include <cstdint>
#include <cstdio>

#ifndef PRUNELOG_NODE_EVERY
#define PRUNELOG_NODE_EVERY 256
#endif
#ifndef PRUNELOG_MOVE_EVERY
#define PRUNELOG_MOVE_EVERY 64
#endif
#ifndef PRUNELOG_MAX_DEPTH
#define PRUNELOG_MAX_DEPTH 12
#endif
#define PRUNELOG_WINDOW 3000   // half width of the probe window around the evaluation

// Rule that decided (pl_rule): node rules 1 razoring, 2 reverse futility, 3 null move, 4 null move after
// verification, 5 ProbCut; move rules 11 capture futility, 12 capture exchange, 13 history, 14 quiet futility,
// 15 quiet exchange, 16 move count.
static thread_local int pl_rule = 0;
static thread_local int pl_in_probe = 0;   // no sampling inside a probe
// Margins the rules actually used at the sampled node/move (26/10/2026, margin calibration for Consilium Plebis,
// docs/rete/C2_SCALA_2026-10-22.md): set where each rule computes them, PL_NONE when the rule did not get that far.
#define PL_NONE (-1000000)
static thread_local int pl_razor = PL_NONE, pl_rfp = PL_NONE, pl_nmp_floor = PL_NONE, pl_fut = PL_NONE;

#pragma pack(push, 1)
struct PruneNodeRec {
  int16_t depth, expert, ply;
  uint16_t flags;          // 1 improving, 2 pv, 4 cut, 8 tt_pv, 16 hash move, 32 tactical hash, 64 opp_worse,
                           // 128 probe below window, 256 probe above window, 512 eval from hash (no disagree)
  int32_t eval, static_eval, eval_raw, signal, disagree;
  int32_t alpha, beta;     // relative to eval
  int32_t probe;           // probe value relative to eval
  uint32_t probe_nodes;
  int8_t rule;
  int32_t ret;             // value the rule returned, relative to eval (0 when none)
  int16_t pieces;
  // 26/10/2026: margins used (razoring and reverse futility margins; null-move floor relative to beta; PL_NONE when
  // the rule stopped before computing it) and the Consilium Plebis experts of the side to move's and the opponent's
  // perspective (-1 in a Consilium build).
  int32_t razor, rfp, nmp_floor;
  int8_t c2_us, c2_them;
  // 02/11/2026: the correction tables as corr_signal reads them (09_history.inc), before their weights, so that the
  // weights can be measured by regression on the probe (pawn, minor, non-pawn of White and of Black, continuation at
  // -2 plus -4, at -6, at -3; corr_flags 1 = no previous move, the fixed CorrNoMoveW term instead of the
  // continuations; 2 = White to move, for the own/opponent split of CorrNpSideW), the band that picks CorrGain, and the experimental term that enters the evaluation only.
  int32_t corr_pawn, corr_minor, corr_npw, corr_npb, corr_c24, corr_c6, corr_c3, signal_x;
  int8_t corr_band, corr_flags;
};
struct PruneMoveRec {
  int16_t depth, expert, ply, move_no;
  uint16_t flags;          // 1 improving, 2 pv, 4 cut, 8 tactical, 16 gives check, 32 tt_pv
  int8_t rule, d_red, d_full;
  int32_t static_eval, alpha, signal, disagree;   // static_eval relative to alpha
  int32_t r, stat;         // reduction (1/1024) and history score of the move
  int32_t v_red, v_full;   // probe values relative to alpha
  uint32_t n_red, n_full;
  int16_t victim, pieces;
  int32_t fut;             // 26/10/2026: quiet futility value (static eval + margin) relative to alpha, or PL_NONE
  int8_t c2_us, c2_them;   // as in PruneNodeRec
};
#pragma pack(pop)

static unsigned prunelog_counters[1024 * 16];
static inline bool prunelog_node_pick(int tid) {
  return tid >= 0 && tid < 1024 && !pl_in_probe && ++prunelog_counters[tid * 16] % PRUNELOG_NODE_EVERY == 0;
}
// Moves: one sampling rate per rule, so that the common rules (move count, quiet futility) do not fill the file.
static inline bool prunelog_move_pick(int tid, int rule) {
  const int every = rule == 16 ? 16 * PRUNELOG_MOVE_EVERY : rule == 14 ? 4 * PRUNELOG_MOVE_EVERY
                                                                       : PRUNELOG_MOVE_EVERY / 2;
  const int slot = rule >= 11 && rule <= 16 ? rule - 10 : 7;
  return tid >= 0 && tid < 1024 && !pl_in_probe && ++prunelog_counters[tid * 16 + slot] % every == 0;
}

template <class R>
static inline void prunelog_write(int tid, const char *stem, int slot, const R &r) {
  static FILE *files[2][1024];
  if (tid < 0 || tid >= 1024)
    return;
  FILE *&fp = files[slot][tid];
  if (!fp) {
    char name[64];
    std::snprintf(name, sizeof name, "%s_%d.bin", stem, tid);
    fp = std::fopen(name, "ab");
    if (!fp)
      return;
  }
  std::fwrite(&r, sizeof r, 1, fp);
}

#define PL_RULE(x) (pl_rule = (x))
#define PL_SET(var, v) (var = (v))

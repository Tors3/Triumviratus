#pragma once
#ifndef SEE_H
#define SEE_H

#include "defs.h"
#include "threads.h"

// Piece values for SEE
static const int see_piece_values[12] = {
    100, 320, 330, 500, 900, 20000,  // P N B R Q K
    100, 320, 330, 500, 900, 20000   // p n b r q k
};

// Static Exchange Evaluation for thread-local state
int td_see(ThreadData& td, int move);

// F-008: SEE a SOGLIA con early-exit (stile SF see_ge). Ritorna 1 se
// td_see(move) >= threshold, 0 altrimenti — senza simulare lo scambio completo.
int td_see_ge(ThreadData& td, int move, int threshold);

#endif

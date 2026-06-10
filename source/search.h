#pragma once
#ifndef SEARCH_H
#define SEARCH_H

#include "defs.h"
#include "movegen.h"

// Score bounds for mating scores.
// NOTE: these MUST stay within the signed 16-bit range, because the
// transposition table packs the score into 16 bits (see tt.h). With the old
// values (49000/50000) every mate score overflowed int16 and was corrupted
// when stored/read from the TT. Stockfish uses the same idea (VALUE_MATE well
// under 32767). The gap mate_value - mate_score (1000) exceeds max_ply, so
// real (non-mate) evaluations never reach the mate band.
// GCC/libstdc++: <cmath> include <bits/specfun.h> che usa
// numeric_limits<T>::infinity(); preincludiamo QUI (prima del #define infinity)
// cosi' le loro include-guard impediscono che la macro li "avveleni" piu' avanti.
#include <cmath>
#include <limits>
#define infinity 32000
#define mate_value 31000
#define mate_score 30000

// max ply that we can reach within a search.
// P1.5 (2026-06-11): 64 -> 128. Con check-extension incondizionata + singular +2 le
// linee lunghe dei finali a LTC sbattevano sul tetto 64 e tornavano static-eval a
// meta' variante (SF usa 246). Sbloccato dalla rimozione della policy (l'array TLS
// policy_scores_by_ply[max_ply][4096] = 1MB/thread che lo rendeva costoso e' stato
// rimosso). Il gap mate_value - mate_score (1000) resta >> max_ply: nessun overlap.
#define max_ply 128

// MVV LVA [attacker][victim]
extern int mvv_lva[12][12];

// killer moves [id][ply]
extern int killer_moves[2][max_ply];

// history moves [piece][square]
extern int history_moves[12][64];

// PV length [ply]
extern int pv_length[max_ply];

// PV table [ply][ply]
extern int pv_table[max_ply][max_ply];

// follow PV & score PV move
extern int follow_pv, score_pv;

// Single-threaded search
extern void search_position(int depth);

#endif

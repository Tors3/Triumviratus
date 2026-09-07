#pragma once
#ifndef SEARCH_H
#define SEARCH_H

#include "defs.h"
#include "movegen.h"

// FIX Linux/GCC (2026-07-12): <cmath> DEVE essere incluso PRIMA della macro
// `infinity` sotto. Su libstdc++ (Debian/Ubuntu g++), <cmath> include a
// cascata i template TR1 (bessel/ellittiche/zeta) che usano testualmente
// `std::numeric_limits<_Tp>::infinity()` — il preprocessore sostituisce
// alla cieca QUALSIASI occorrenza del token "infinity", anche dentro gli
// header di sistema, corrompendo quelle chiamate se <cmath> viene
// (ri-)incluso DOPO la nostra macro. Pre-includerlo qui sfrutta gli
// include-guard di <cmath>: le inclusioni successive (es. threads.cpp)
// diventano no-op e non rivedono mai piu' il token grezzo. MSVC non ha
// questo problema (niente header TR1 libstdc++), quindi la build Windows
// non lo mostra — ma il fix e' innocuo e a costo zero anche li'.
#include <cmath>

// Score bounds for mating scores.
// NOTE: these MUST stay within the signed 16-bit range, because the
// transposition table packs the score into 16 bits (see tt.h). With the old
// values (49000/50000) every mate score overflowed int16 and was corrupted
// when stored/read from the TT. Stockfish uses the same idea (VALUE_MATE well
// under 32767). The gap mate_value - mate_score (1000) exceeds max_ply, so
// real (non-mate) evaluations never reach the mate band.
#define infinity 32000
#define mate_value 31000
#define mate_score 30000

// max ply that we can reach within a search.
// 🔴 2026-09-07: era 64. Oltre questo ply td_negamax (:7569) e td_quiescence
// (:6489) NON cercano piu': restituiscono la eval statica, in silenzio. A 10s e
// 1 thread la seldepth resta sotto i 30, ma le misure SMP del 07/09 (10 s, 16-40
// thread) hanno toccato **seldepth 42-54**, e a TC lungo con molti thread il
// tetto si raggiunge davvero — proprio nelle linee forzate, dove fermarsi costa
// di piu'. Stockfish usa MAX_PLY = 246.
// Costo: gli array ply-indicizzati di ThreadData sono [max_ply + 8]; il solo
// quadratico e' pv_table, 20 KB -> 72 KB. Su una ThreadData da 21,25 MB e' lo
// 0,24%. Le costanti derivate (TB_VALUE_WIN in syzygy.cpp, le bande corr_max in
// threads.cpp) sono scritte in funzione di max_ply e restano corrette da sole:
// il minimo score TB e' TB_VALUE_WIN - max_ply = mate_score - 2*max_ply, che e'
// esattamente il bordo che CorrTBGuard usa.
// Vincolo invariato: mate_value - mate_score = 1000 deve superare max_ply.
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

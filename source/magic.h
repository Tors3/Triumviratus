#pragma once
#ifndef MAGIC_H
#define MAGIC_H

#include "defs.h"

extern unsigned int random_state;
extern U64 generate_magic_number();
extern void init_magic_numbers();
extern U64 get_random_U64_number();
extern U64 find_magic_number(int square, int relevant_bits, int bishop);
extern void init_sliders_attacks(int bishop);
// ⛔ 04/10/2026 — PROVATE E TOLTE come inline in questo header: con la build PGO + ThinLTO +1,27% istruzioni e
// +0,77% cicli per nodo (xperf, 6 giri, nodi identici). Il compilatore le inlineava gia' dove rende; forzarle ovunque
// gonfia il codice caldo. Restano in magic.cpp.
// ⛔ 04/10/2026 notte — PROVATE E TOLTE le tabelle PER LINEA (traversa con shift, colonna e diagonali con PEXT su <= 6
// bit: 4 x 64 x 64 x 8 = 128 KB al posto dei 2,25 MB delle tabelle fancy): due letture piccole invece di una grande,
// nodi identici, ma cicli +0,55% e istruzioni +0,33% (xperf, 6 giri, PGO). Le tabelle grandi non mancano in cache
// quanto sembra: le righe lette davvero sono poche e calde (stessa lezione del 10/09 sulle tabelle impacchettate).
extern U64 get_bishop_attacks(int square, U64 occupancy);
extern U64 get_rook_attacks(int square, U64 occupancy);
extern U64 get_queen_attacks(int square, U64 occupancy);

// AA1 (10/10/2026, velocita', stessi attacchi): alfiere E torre dalla stessa casa con la stessa occupazione in UNA
// chiamata. Con DualMagic (P4, 08/10) ogni get_bishop_attacks / get_rook_attacks calcola gia' entrambi gli attacchi e
// ne butta uno: chi chiedeva i due per la stessa casa (SEE, case di scacco, legalita' del re, minacce della rete) pagava
// due volte lo stesso calcolo, fuori linea in magic.cpp. Contatori (bench 15 su fens30): 9,8 calcoli per nodo, ~4,4
// doppioni. La coppia torna in un registro xmm (ABI Windows x64: __m128i nel registro di ritorno), non in memoria come
// il get_both_attacks a puntatore di P4b (08/10, scartato). Corsia 0 = alfiere, corsia 1 = torre.
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
typedef __m128i SliderPair;
static inline U64 pair_bishop(SliderPair v) { return (U64)_mm_cvtsi128_si64(v); }
static inline U64 pair_rook(SliderPair v) { return (U64)_mm_cvtsi128_si64(_mm_unpackhi_epi64(v, v)); }
#else
struct SliderPair { U64 b, r; };
static inline U64 pair_bishop(SliderPair v) { return v.b; }
static inline U64 pair_rook(SliderPair v) { return v.r; }
#endif
extern SliderPair get_slider_pair(int square, U64 occupancy);

#endif

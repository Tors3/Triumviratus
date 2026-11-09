// Origin: this file descends from BBC (Bit Board Chess) by Maksim Korzh, https://github.com/maksimKorzh/bbc,
// released under the GNU General Public License v3; the project started from BBC in 2024 and has since rewritten
// and extended much of it. Triumviratus is distributed under the GNU GPL v3 (see COPYING).
#pragma once
#ifndef DEFS_H
#define DEFS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>   // __popcnt64 / _BitScanForward64 for the inline helpers below
#endif
#include <unordered_map>
#include <vector>

// Define version information
#ifdef TRIUMV_POS_SOLVER
#define VERSION " - 9.0 dev Pos Solver"   // analysis build (search/01_params.inc, PSV)
#else
#define VERSION " - 9.0 dev"
#endif
#define AUTHOR "Francesco Torsello"
#define NAME "Triumviratus"

// BUILD date, taken from the compiler: it changes by itself at every recompilation, so
// two builds of different days can be told apart in the GUI without touching the
// source. `__DATE__` is "Jul 31 2026" (day with a SPACE in front below 10):
// build_date() rewrites it as "2026-08-06", which sorts.
inline const char* build_date() {
  static char iso[11];
#ifdef TRIUMV_RELEASE_DAY
  // Release date fixed at build time (e.g. -DTRIUMV_RELEASE_DAY=20261009) instead of the compilation date.
  if (!iso[0])
    snprintf(iso, sizeof iso, "%04d-%02d-%02d", TRIUMV_RELEASE_DAY / 10000, TRIUMV_RELEASE_DAY / 100 % 100,
             TRIUMV_RELEASE_DAY % 100);
  return iso;
#endif
  if (!iso[0]) {
    const char* d    = __DATE__;   // through a pointer: `__DATE__ + 7` on a literal
    const char* mons = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char  mon3[4] = { d[0], d[1], d[2], '\0' };  // ONLY the 3 letters: the whole
    const char* hit  = strstr(mons, mon3);             // "Jul 31 2026" is not there to search for
    const int   mon  = hit ? int(hit - mons) / 3 + 1 : 0;
    snprintf(iso, sizeof iso, "%.4s-%02d-%02d", d + 7, mon,
             atoi(d + 4));          // atoi eats the space of days < 10
  }
  return iso;
}

// Define the size of a 64-bit unsigned integer
#define U64 unsigned long long

// FEN debug positions for testing
#define empty_board "8/8/8/8/8/8/8/8 b - - "
#define start_position "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 "
#define tricky_position "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1 "
#define killer_position "rnbqkb1r/pp1p1pPp/8/2p1pP2/1P1P4/3P3P/P1P1P3/RNBQKBNR w KQkq e6 0 1"
#define cmk_position "r2q1rk1/ppp2ppp/2n1bn2/2b1p3/3pP3/3P1NPP/PPP1NPB1/R1BQ1RK1 b - - 0 9 "
#define repetitions "2r3k1/R7/8/1R6/8/8/P4KPP/8 w - - 0 40 "

// Enumeration for chessboard squares
enum {
    a8, b8, c8, d8, e8, f8, g8, h8,
    a7, b7, c7, d7, e7, f7, g7, h7,
    a6, b6, c6, d6, e6, f6, g6, h6,
    a5, b5, c5, d5, e5, f5, g5, h5,
    a4, b4, c4, d4, e4, f4, g4, h4,
    a3, b3, c3, d3, e3, f3, g3, h3,
    a2, b2, c2, d2, e2, f2, g2, h2,
    a1, b1, c1, d1, e1, f1, g1, h1, no_sq
};

// Enumeration for piece types
enum { P, N, B, R, Q, K, p, n, b, r, q, k };

// Enumeration for colors
enum { white, black, both };

// Enumeration for special chess pieces (rook or bishop)
enum { rook, bishop };

// Castle permissions flags
enum { wk = 1, wq = 2, bk = 4, bq = 8 };

// Convert squares to coordinates
extern const char* square_to_coordinates[];

// ASCII representation of chess pieces
extern char ascii_pieces[13];

// Unicode representation of chess pieces
extern const char* unicode_pieces[12];

// Function to map a character to a chess piece
extern int mapCharToPiece(char c);

// Function to map a chess piece to its promotion character
extern char mapPieceToPromotion(int piece);

// Chessboard variables
extern U64 bitboards[12];
extern U64 occupancies[3];
extern int side;
extern int enpassant;
extern int castle;
extern U64 hash_key;
// 2048 (2026-06-12, was 1000): long game (440+ moves = 880 plies of history)
// + search path up to max_ply=128 exceeded 1000 = silent OOB write.
extern U64 repetition_table[2048];
extern int repetition_index;
extern int ply;
extern int fifty;
extern int g_fen_ply;   // game ply of the given FEN: 2*(fullmove-1) + black to move (TmFenPly, 05/10/2026)

// Time control variables
extern int quit;
extern int movestogo;
extern int movetime;
extern int time_uci;
extern int inc;
extern int starttime;
extern int stoptime;
extern int timeset;
extern int stopped;
extern int g_searchmoves[256];   // "go searchmoves ...": root move whitelist (analysis).
extern int g_searchmoves_count;  // 0 = off (search all root moves).
// The other limits of "go" (nodes, mate, infinite, ponder) are in SearchLimits (threads.h).

// Bit manipulations macros
#define set_bit(bitboard, square) ((bitboard) |= (1ULL << (square)))
#define get_bit(bitboard, square) ((bitboard) & (1ULL << (square)))
#define pop_bit(bitboard, square) ((bitboard) &= ~(1ULL << (square)))
// 2D: clears the LEAST SIGNIFICANT bit. A single instruction (BLSR with -mbmi2, otherwise
// two) instead of shift+not+and, and above all it does NOT depend on the result of get_ls1b_index:
// the chain `sq = ls1b(bb); ... ; pop_bit(bb, sq)` forces the clearing to wait for the
// tzcnt, this one does not.
// 🔴 It holds ONLY where the bit to remove really is the LSB (typically right after a
// get_ls1b_index on the SAME variable). Where a specific square is cleared - as in
// see.cpp, which removes `from`/`captured_sq`/`to` - pop_bit is needed and this macro would be
// SILENTLY WRONG.
#define pop_lsb_bb(bitboard) ((bitboard) &= (bitboard) - 1)
// 🔴 INLINE, not extern (09/09/2026). They were out-of-line functions in misc.cpp: in the
// PGO+ThinLTO binary 31 `call get_ls1b_index` and 4 `call count_bits` remained, and the -pg profile
// counted ~120 bit scans per node. A tzcnt costs 1 cycle, a call ~10, and it breaks the
// dependency chains in the `while (bb)` loops.
// IDENTICAL semantics (ls1b of 0 = -1): node-identical by construction.
static inline int count_bits(U64 bitboard) {
#ifdef _WIN32
    return (int)__popcnt64(bitboard);
#else
    return __builtin_popcountll(bitboard);
#endif
}
static inline int get_ls1b_index(U64 bitboard) {
    if (!bitboard) return -1;
#ifdef _WIN32
    unsigned long index;
    _BitScanForward64(&index, bitboard);
    return static_cast<int>(index);
#else
    return __builtin_ctzll(bitboard);
#endif
}

// Variables for Zobrist hashing
extern U64 piece_keys[16][64];   // rows 12..15 at zero (see init.cpp)
extern U64 enpassant_keys[64];
extern U64 castle_keys[16];
extern U64 side_key;

extern void init_random_keys();
extern U64 generate_hash_key();

// Move list structure
typedef struct {
    int moves[256];
    int count;
} moves;

// Initialization function
extern void init_bitboards();


#include "frozen.h"   // decides TRIUMV_FROZEN (see the note in there)

#endif

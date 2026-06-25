#pragma once
#ifndef IO_H
#define IO_H

#include "defs.h"
#include <string>

extern void print_bitboard(U64 bitboard);
extern void print_board();
extern void reset_board();
extern void parse_fen(const char* fen);

// Serialize the current global board to a FEN string (used for data logging).
extern std::string board_to_fen();
// Serialize a move to UCI long-algebraic notation, e.g. "e2e4" or "e7e8q".
extern std::string move_to_uci(int move);

#endif

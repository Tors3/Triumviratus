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
extern U64 get_bishop_attacks(int square, U64 occupancy);
extern U64 get_rook_attacks(int square, U64 occupancy);
extern U64 get_queen_attacks(int square, U64 occupancy);

#endif

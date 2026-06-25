#pragma once
#ifndef ATTACKS_H
#define ATTACKS_H

#include "defs.h"

extern const U64 not_a_file;
extern const U64 not_h_file;
extern const U64 not_hg_file;
extern const U64 not_ab_file;

// bishop relevant occupancy bit count for every square on board
extern const int bishop_relevant_bits[64];

// rook relevant occupancy bit count for every square on board
extern const int rook_relevant_bits[64];

// rook magic numbers
extern U64 rook_magic_numbers[64];

// bishop magic numbers
extern U64 bishop_magic_numbers[64];

extern U64 pawn_attacks[2][64];
extern U64 knight_attacks[64];
extern U64 king_attacks[64];
extern U64 bishop_masks[64];
extern U64 rook_masks[64];
extern U64 bishop_attacks[64][512];
extern U64 rook_attacks[64][4096];

extern U64 mask_pawn_attacks(int side, int square);
extern U64 mask_knight_attacks(int square);
extern U64 mask_king_attacks(int square);
extern U64 mask_bishop_attacks(int square);
extern U64 mask_rook_attacks(int square);
extern U64 bishop_attacks_on_the_fly(int square, U64 block);
extern U64 rook_attacks_on_the_fly(int square, U64 block);
extern void init_leapers_attacks();
extern U64 set_occupancy(int index, int bits_in_mask, U64 attack_mask);

#endif

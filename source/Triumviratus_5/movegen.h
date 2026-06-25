#pragma once
#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "defs.h"

// preserve board state
#define copy_board()                                                      \
    U64 bitboards_copy[12], occupancies_copy[3];                          \
    int side_copy, enpassant_copy, castle_copy, fifty_copy;               \
    memcpy(bitboards_copy, bitboards, 96);                                \
    memcpy(occupancies_copy, occupancies, 24);                            \
    side_copy = side, enpassant_copy = enpassant, castle_copy = castle;   \
    fifty_copy = fifty;                                                   \
    U64 hash_key_copy = hash_key;                                         \

// restore board state
#define take_back()                                                       \
    memcpy(bitboards, bitboards_copy, 96);                                \
    memcpy(occupancies, occupancies_copy, 24);                            \
    side = side_copy, enpassant = enpassant_copy, castle = castle_copy;   \
    fifty = fifty_copy;                                                   \
    hash_key = hash_key_copy;                                             \

// encode move
#define encode_move(source, target, piece, promoted, capture, double_push, enpassant, castling) \
    (source) |          \
    (target << 6) |     \
    (piece << 12) |     \
    (promoted << 16) |  \
    (capture << 20) |   \
    (double_push << 21) |    \
    (enpassant << 22) | \
    (castling << 23)    \

// EXTRACT DATAS FROM MOVE
#define get_move_source(move) (move & 0x3f)
#define get_move_target(move) ((move & 0xfc0) >> 6)
#define get_move_piece(move) ((move & 0xf000) >> 12)
#define get_move_promoted(move) ((move & 0xf0000) >> 16)
#define get_move_capture(move) (move & 0x100000)
#define get_move_double(move) (move & 0x200000)
#define get_move_enpassant(move) (move & 0x400000)
#define get_move_castling(move) (move & 0x800000)

// move types
enum { all_moves, only_captures };

// castling rights update constants
extern const int castling_rights[64];

extern int is_square_attacked(int square, int side);
extern void print_attacked_squares(int side);
extern void add_move(moves* move_list, int move);
extern void print_move(int move);
extern void print_move_list(moves* move_list);
extern int make_move(int move, int move_flag);
extern void generate_moves(moves* move_list);

#endif

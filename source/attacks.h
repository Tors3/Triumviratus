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
// (TABELLE DEGLI ALIANTI IMPACCHETTATE: provato e RIMOSSO il 10/09/2026.
//  La forma qui sotto usa un passo FISSO per casa, 2,25 MB in tutto, dove
//  Stockfish impacchetta ogni casa alla sua dimensione esatta e sta in 841 KB.
//  Compattate davvero: rook_attacks_store era sceso a 800 KB e bishop a 41 KB,
//  esattamente le loro dimensioni, con firma bench invariata.
//  RISULTATO: NEUTRO, 5.647,5 +- 11,6 cicli/nodo contro 5.624,0 +- 23,1, e i miss
//  L2 identici al centesimo (86,92 contro 86,98 per nodo).
//  🔑 L'errore di ragionamento: la cache tiene solo le linee TOCCATE, e il
//  riempimento inutilizzato non ne occupava nessuna. L'insieme davvero letto era
//  gia' della stessa dimensione, quindi non c'era pressione da togliere. In cambio
//  il vettore di puntatori aggiungeva un caricamento dipendente a ogni interrogazione.
//  Una tabella grande non costa per quanto e' dichiarata, ma per quanto se ne legge.)
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

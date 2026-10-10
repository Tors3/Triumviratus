// nn_attacks.h -- Gli attacchi del motore visti dalla rete (07/10/2026, scacchiera unica v2).
//
// La rete enumera le minacce attive nel refresh (FullThreats::append_active_indices) sulla nostra scacchiera, nella
// nostra numerazione (a8 = 0): usa quindi le NOSTRE tabelle di attacco (attacks.cpp, magic.cpp), non quelle della
// rete. Qui solo le dichiarazioni, identiche a quelle di attacks.h e magic.h, senza includere defs.h (le sue macro
// non devono entrare nelle unita' della rete).

#ifndef NN_ATTACKS_H_INCLUDED
#define NN_ATTACKS_H_INCLUDED

extern unsigned long long knight_attacks[64];
extern unsigned long long get_bishop_attacks(int square, unsigned long long occupancy);
extern unsigned long long get_rook_attacks(int square, unsigned long long occupancy);
extern unsigned long long get_queen_attacks(int square, unsigned long long occupancy);   // AA1 (10/10/2026)

#endif  // NN_ATTACKS_H_INCLUDED

/*
 * Static Exchange Evaluation (SEE)
 * Determines if a capture wins or loses material
 */

#include "see.h"
#include "defs.h"
#include "movegen.h"
#include "magic.h"
#include "attacks.h"

// Get least valuable attacker
static inline int get_lva(U64 bb[12], U64 occ[3], int square, int side, int* from_sq) {
    U64 attackers;
    
    // Pawns first (least valuable)
    if (side == white) {
        attackers = pawn_attacks[black][square] & bb[P];
    } else {
        attackers = pawn_attacks[white][square] & bb[p];
    }
    if (attackers) {
        *from_sq = get_ls1b_index(attackers);
        return (side == white) ? P : p;
    }
    
    // Knights
    int knight = (side == white) ? N : n;
    attackers = knight_attacks[square] & bb[knight];
    if (attackers) {
        *from_sq = get_ls1b_index(attackers);
        return knight;
    }
    
    // Bishops
    int bishop = (side == white) ? B : b;
    attackers = get_bishop_attacks(square, occ[both]) & bb[bishop];
    if (attackers) {
        *from_sq = get_ls1b_index(attackers);
        return bishop;
    }
    
    // Rooks
    int rook = (side == white) ? R : r;
    attackers = get_rook_attacks(square, occ[both]) & bb[rook];
    if (attackers) {
        *from_sq = get_ls1b_index(attackers);
        return rook;
    }
    
    // Queens
    int queen = (side == white) ? Q : q;
    attackers = get_queen_attacks(square, occ[both]) & bb[queen];
    if (attackers) {
        *from_sq = get_ls1b_index(attackers);
        return queen;
    }
    
    // King (last resort)
    int king = (side == white) ? K : k;
    attackers = king_attacks[square] & bb[king];
    if (attackers) {
        *from_sq = get_ls1b_index(attackers);
        return king;
    }
    
    return -1;
}

// SeeFix (UCI "SeeFix", default ON) — ablazione del FIX P0.2: quando OFF, le mosse
// quiet tornano a SEE=0 (come 3.7: quiet-SEE-pruning e filtro CheckOrdering inerti)
// e l'en-passant torna a rimuovere il pedone dalla casa sbagliata. Definita in threads.cpp.
extern bool g_see_fix;

// SEE implementation
int td_see(ThreadData& td, int move) {
    int from = get_move_source(move);
    int to = get_move_target(move);
    int piece = get_move_piece(move);
    int captured = -1;
    
    // Find captured piece
    int start = (td.side == white) ? p : P;
    int end = (td.side == white) ? k : K;
    
    for (int pc = start; pc <= end; pc++) {
        if (get_bit(td.bitboards[pc], to)) {
            captured = pc;
            break;
        }
    }
    
    // En passant: il pedone catturato NON sta su `to` ma su to±8.
    int captured_sq = to;
    if (get_move_enpassant(move)) {
        captured = (td.side == white) ? p : P;
        if (g_see_fix)
            captured_sq = (td.side == white) ? to + 8 : to - 8;   // FIX P0.2b: era rimosso da `to`
    }

    // Ablazione P0.2 (SeeFix off): comportamento 3.7 = SEE 0 per le quiet.
    if (captured == -1 && !g_see_fix) return 0;

    // FIX P0.2 (2026-06-09): SEE anche per le mosse QUIET (prima: return 0).
    // Per una quiet lo scambio inizia con la cattura avversaria meno cara su `to`
    // (gain[0] = 0). Con il vecchio "return 0": (a) il SEE-pruning dei quiet
    // (threads.cpp, SEEQuietMargin) non scattava MAI; (b) il filtro SEE>=-75 del
    // CheckOrdering era un no-op. NB: le promozioni restano fuori scope (il
    // chiamante le esclude gia' dal SEE-pruning).

    // Local copy for simulation
    U64 bb[12], occ[3];
    memcpy(bb, td.bitboards, sizeof(td.bitboards));
    memcpy(occ, td.occupancies, sizeof(td.occupancies));

    int gain[32];
    int depth = 0;
    int current_side = td.side;

    // Initial gain (0 per le quiet: nessun pezzo incassato dalla mossa stessa)
    gain[0] = (captured != -1) ? see_piece_values[captured] : 0;

    // Remove attacker from source
    pop_bit(bb[piece], from);
    pop_bit(occ[current_side], from);
    pop_bit(occ[both], from);

    // Remove captured from its square (== `to` salvo en passant)
    if (captured != -1) {
        pop_bit(bb[captured], captured_sq);
        pop_bit(occ[current_side ^ 1], captured_sq);
        if (captured_sq != to)                     // e.p.: libera anche occ[both]
            pop_bit(occ[both], captured_sq);
    }

    // Attacker now on target
    set_bit(bb[piece], to);
    set_bit(occ[current_side], to);
    set_bit(occ[both], to);
    
    int attacker_value = see_piece_values[piece];
    current_side ^= 1;
    
    // Simulate exchanges
    while (depth < 31) {
        depth++;
        
        int from_sq;
        int attacker = get_lva(bb, occ, to, current_side, &from_sq);
        
        if (attacker == -1) break;
        
        gain[depth] = attacker_value - gain[depth - 1];
        
        // Stand-pat pruning (swap-SEE classico: max(-gain[d-1], gain[d]) < 0).
        // BUG FIX 2026-07-16: la seconda clausola era `gain[d-1] < 0`, insoddisfacibile
        // (se gain[d-1]<0 allora gain[d]=val-gain[d-1]>0) -> break MORTO, prune mai attivo.
        // Corretto = `gain[d-1] > 0`. Value-preserving (stessa SEE, solo esce prima) -> bench invariato.
        if (gain[depth] < 0 && gain[depth - 1] > 0) break;
        
        attacker_value = see_piece_values[attacker];
        
        // Remove this attacker
        pop_bit(bb[attacker], from_sq);
        pop_bit(occ[current_side], from_sq);
        pop_bit(occ[both], from_sq);
        
        // Remove previous piece from target
        for (int pc = P; pc <= k; pc++) {
            if (get_bit(bb[pc], to)) {
                pop_bit(bb[pc], to);
                break;
            }
        }
        
        // Add attacker to target
        set_bit(bb[attacker], to);
        set_bit(occ[current_side], to);
        set_bit(occ[both], to);
        
        current_side ^= 1;
    }
    
    // Negamax the gain stack
    while (--depth > 0) {
        gain[depth - 1] = -((-gain[depth - 1] > gain[depth]) ? -gain[depth - 1] : gain[depth]);
    }

    return gain[0];
}

// ---------------------------------------------------------------------------
// F-008 (audit 2026-07-02): td_see_ge(move, threshold) — SEE a soglia con
// early-exit, stile Stockfish see_ge. TUTTI i call-site del motore chiedono solo
// "SEE >= soglia?" (mai il valore esatto): appena il bound e' matematicamente
// deciso si esce, evitando la simulazione completa dello scambio, il memcpy dei
// 15 bitboard e la ri-scansione O(12) del pezzo su 'to' del td_see classico.
// Swap-based: nessun pezzo viene "spostato" — si pela l'attaccante meno caro per
// lato aggiornando solo `occupied`; gli x-ray emergono ri-sondando gli attacchi
// slider con l'occupancy ridotta (bitboard `attackers` incrementale, non da zero).
// Il re usa la regola esplicita SF (cattura legale solo se indifeso): col valore
// 20000 di see_piece_values e' equivalente al comportamento del td_see classico.
// Semantica: ritorna (td_see(td, move) >= threshold). Validazione: toggle UCI
// SeeGEVerify = cross-check per-chiamata dei due path (0 mismatch attesi) +
// node-count bit-identico al bench con SeeGE on/off.
int td_see_ge(ThreadData& td, int move, int threshold) {
    int from  = get_move_source(move);
    int to    = get_move_target(move);
    int piece = get_move_piece(move);

    // Vittima su 'to' (scan del solo lato avversario, come td_see)
    int captured = -1;
    int start = (td.side == white) ? p : P;
    int end   = (td.side == white) ? k : K;
    for (int pc = start; pc <= end; pc++) {
        if (get_bit(td.bitboards[pc], to)) { captured = pc; break; }
    }

    U64 occupied = td.occupancies[both] ^ (1ULL << from);   // il mover lascia l'origine
    if (get_move_enpassant(move)) {
        captured = (td.side == white) ? p : P;
        occupied ^= (td.side == white) ? (1ULL << (to + 8)) : (1ULL << (to - 8));
    }

    // Bilancio se lo scambio si ferma subito dopo la nostra mossa.
    int swap = ((captured != -1) ? see_piece_values[captured] : 0) - threshold;
    if (swap < 0) return 0;      // nemmeno incassando la vittima si arriva alla soglia
    // L'avversario ricattura il nostro pezzo: se restiamo comunque sopra, deciso.
    swap = see_piece_values[piece] - swap;
    if (swap <= 0) return 1;

    // Attaccanti di 'to' di ENTRAMBI i lati. I bb[] restano interi (nessuna copia):
    // si maschera con `occupied` a ogni giro, i pezzi "pelati" spariscono da soli.
    const U64 diag_sliders = td.bitboards[B] | td.bitboards[b] | td.bitboards[Q] | td.bitboards[q];
    const U64 orth_sliders = td.bitboards[R] | td.bitboards[r] | td.bitboards[Q] | td.bitboards[q];
    U64 attackers =
          (pawn_attacks[black][to] & td.bitboards[P])
        | (pawn_attacks[white][to] & td.bitboards[p])
        | (knight_attacks[to] & (td.bitboards[N] | td.bitboards[n]))
        | (king_attacks[to]   & (td.bitboards[K] | td.bitboards[k]))
        | (get_bishop_attacks(to, occupied) & diag_sliders)
        | (get_rook_attacks(to, occupied)   & orth_sliders);

    int stm = td.side;
    int res = 1;                 // 1 = "la soglia regge" per il lato che ha mosso

    while (true) {
        stm ^= 1;
        attackers &= occupied;
        U64 stm_att = attackers & td.occupancies[stm];   // attackers e' gia' <= occupied
        if (!stm_att) break;                             // niente attaccanti: res corrente decide
        res ^= 1;

        U64 bb;
        const int base = (stm == white) ? P : p;
        if ((bb = stm_att & td.bitboards[base + 0])) {          // pedone
            if ((swap = see_piece_values[P] - swap) < res) break;
            occupied ^= (bb & (0 - bb));
            attackers |= get_bishop_attacks(to, occupied) & diag_sliders;  // x-ray diagonale
        }
        else if ((bb = stm_att & td.bitboards[base + 1])) {     // cavallo
            if ((swap = see_piece_values[N] - swap) < res) break;
            occupied ^= (bb & (0 - bb));
        }
        else if ((bb = stm_att & td.bitboards[base + 2])) {     // alfiere
            if ((swap = see_piece_values[B] - swap) < res) break;
            occupied ^= (bb & (0 - bb));
            attackers |= get_bishop_attacks(to, occupied) & diag_sliders;
        }
        else if ((bb = stm_att & td.bitboards[base + 3])) {     // torre
            if ((swap = see_piece_values[R] - swap) < res) break;
            occupied ^= (bb & (0 - bb));
            attackers |= get_rook_attacks(to, occupied) & orth_sliders;    // x-ray ortogonale
        }
        else if ((bb = stm_att & td.bitboards[base + 4])) {     // donna
            if ((swap = see_piece_values[Q] - swap) < res) break;
            occupied ^= (bb & (0 - bb));
            attackers |= (get_bishop_attacks(to, occupied) & diag_sliders)
                       | (get_rook_attacks(to, occupied)   & orth_sliders);
        }
        else {                                                  // re: cattura legale solo se INDIFESO
            return ((attackers & occupied) & td.occupancies[stm ^ 1]) ? (res ^ 1) : res;
        }
    }
    return res;
}

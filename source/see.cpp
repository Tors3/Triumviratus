/*
 * Static Exchange Evaluation (SEE)
 * Determines if a capture wins or loses material
 */

#include "see.h"
#include "defs.h"
#include "movegen.h"
#include "magic.h"
#include "attacks.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "nn_dirty.h"            // C2 piece values (agent SEE): the expert pair on the network's stack
#include "consilium_bridge.h"    // nn_router_table, cn_sig (the pair at the root)

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

// SeeFix (UCI "SeeFix", default ON) - ablation of FIX P0.2: when OFF, the quiet
// moves go back to SEE=0 (as in 3.7: quiet SEE pruning and the CheckOrdering filter inert)
// and en passant goes back to removing the pawn from the wrong square. Defined in threads.cpp.
#ifdef TRIUMV_RELEASE
// Release (06/10/2026): constants (defaults of 01_params.inc; SeePinned checked with static_assert).
constexpr bool g_see_fix = true;
constexpr int  g_see_pinned = 1;
static constexpr const int* g_see_val = see_piece_values;   // SeeVal* (agent SEE, 21/10/2026; static_assert there)
#else
extern bool g_see_fix;
extern int  g_see_pinned;      // SeePinned (copy of the search parameter, 01_params.inc)
#endif

// PIECE VALUES (28/10/2026, author: "one value per piece per expert, replacing the inherited values everywhere").
// One definition for the whole engine: value(piece) = SeeVal<type> + C2Val<type><expert>, every type with an offset
// per expert, pawns included (kings: 20000 in the exchange evaluation, 0 as material). Consilium Plebis routes each
// perspective to an expert by the OPPONENT's non-pawn material (e0 at most a minor, e1 a lone rook, e2 two minors or
// rook + minor, e3 a lone queen, e4 10-13, e5 14-18, e6 19-25, e7 26-31 points), so a piece is valued by the expert
// that reads its OWNER's material: White's pieces take the Black perspective's expert, Black's the White one's.
// The exchange evaluation, the capture and quiescence futility, the capture order, the threat terms of the quiet
// order, the material thresholds and the stat score of a capture all read this table (piece_values_mat for the
// search, see_values here); unit_value of the search is gone. With every offset at 0 the values are the old table
// 208/781/825/1276/2538 and the tree is the old one. The offsets come from the network: match_moe/piece_values.
// The tables (SEE values, material values, offsets; 16 piece codes each) live in the thread and are rebuilt only when
// the expert pair changes: the pair is one byte on the network's stack (dp.phase, written by the make), so the check
// per call is one load and one compare; at the root of a search (one state) the pair is read from the board.
// copy_board_to_thread marks the tables stale (c2_see_pair = -1) at each search start, which also catches a setoption.
// Least valuable attacker: the swap list keeps taking the attackers by kind (pawn, knight, bishop, rook, queen) even
// where the offsets would reorder two kinds; the values only change the balance of the exchange.
#ifdef TRIUMV_RELEASE
static constexpr const int (&g_c2_val_off)[5][8] = see_val_offsets;   // C2ValP..C2ValQ (static_assert in 01_params.inc)
#else
extern int g_c2_val_off[5][8];                     // copies of C2Val*, synced at setoption
#endif

// Expert pair of the thread's position, White | Black << 4 (the same byte as c2_experts_td, 06_nndirty.inc).
static inline int c2_pair_now(const ThreadData& td) {
    const NnStack& S = *td.nnstack;
    if (S.size >= 2)
        return S.st[S.size - 1].dp.phase;
    int sig[2];
    for (int c = 0; c < 2; c++)
        sig[c] = cn_sig(count_bits(td.bitboards[N + 6 * c]), count_bits(td.bitboards[B + 6 * c]),
                        count_bits(td.bitboards[R + 6 * c]), count_bits(td.bitboards[Q + 6 * c]));
    const unsigned char* rt = nn_router_table();
    return rt[sig[0] * 54 + sig[1]] | rt[sig[1] * 54 + sig[0]] << 4;
}

static void c2_see_rebuild(const ThreadData& td, int pair) {
    const int e_of_owner[2] = {(pair >> 4) & 7, pair & 7};   // White's pieces: Black's expert; Black's: White's
    for (int c = 0; c < 2; c++)
        for (int t = 0; t < 6; t++) {
            const int off = t <= Q ? g_c2_val_off[t][e_of_owner[c]] : 0;
            td.c2_see_off[6 * c + t] = off;
            td.c2_see_val[6 * c + t] = g_see_val[6 * c + t] + off;
            td.mat_val[6 * c + t] = t == K ? 0 : g_see_val[6 * c + t] + off;
        }
    for (int i = 12; i < 16; i++)
        td.c2_see_off[i] = td.c2_see_val[i] = td.mat_val[i] = 0;
    td.c2_see_pair = pair;
}

static inline void c2_see_sync(const ThreadData& td) {
    const int pair = c2_pair_now(td);
    if (pair != td.c2_see_pair)
        c2_see_rebuild(td, pair);
}

// Constant tables of the release (speed, 28/10/2026, same values): when every per-expert offset of the release is 0
// (the pre-release of 28/10 has them unbaked), the thread's tables would always hold the base values, so the release
// reads these two constant tables and skips the expert-pair check of every call and the rebuild of three tables at
// every change of expert. xperf on the Xeon (deterministic PGO, avx512, 3 rounds): -0.86% cycles per node in the
// middlegame, -0.53% in endgames. A release with offsets baked to non-zero values takes the thread's tables again by
// itself; the development builds always do (their offsets are options).
#ifdef TRIUMV_RELEASE
static constexpr bool see_offsets_all_zero() {
    for (int t = 0; t < 5; t++)
        for (int e = 0; e < 8; e++)
            if (see_val_offsets[t][e]) return false;
    return true;
}
static constexpr bool g_const_values = see_offsets_all_zero();
alignas(64) static constexpr int k_see_val16[16] = {see_piece_values[0], see_piece_values[1], see_piece_values[2],
    see_piece_values[3], see_piece_values[4], see_piece_values[5], see_piece_values[6], see_piece_values[7],
    see_piece_values[8], see_piece_values[9], see_piece_values[10], see_piece_values[11], 0, 0, 0, 0};
alignas(64) static constexpr int k_mat_val16[16] = {see_piece_values[0], see_piece_values[1], see_piece_values[2],
    see_piece_values[3], see_piece_values[4], 0, see_piece_values[6], see_piece_values[7], see_piece_values[8],
    see_piece_values[9], see_piece_values[10], 0, 0, 0, 0, 0};
#else
static constexpr bool g_const_values = false;
static constexpr const int* k_see_val16 = nullptr;
static constexpr const int* k_mat_val16 = nullptr;
#endif

const int* piece_values_mat(const ThreadData& td) {
    if constexpr (g_const_values) return k_mat_val16;
    c2_see_sync(td);
    return td.mat_val;
}

// Values the exchange evaluation reads (kings 20000).
static inline const int* see_values(const ThreadData& td) {
    if constexpr (g_const_values) return k_see_val16;
    c2_see_sync(td);
    return td.c2_see_val;
}

// SEE implementation
int td_see(ThreadData& td, int move) {
    const int* const val = see_values(td);   // piece values (the per-expert table of the thread)
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
    
    // En passant: the captured pawn is NOT on `to` but on to±8.
    int captured_sq = to;
    if (get_move_enpassant(move)) {
        captured = (td.side == white) ? p : P;
        if (g_see_fix)
            captured_sq = (td.side == white) ? to + 8 : to - 8;   // FIX P0.2b: it was removed from `to`
    }

    // Ablation P0.2 (SeeFix off): 3.7 behaviour = SEE 0 for quiet moves.
    if (captured == -1 && !g_see_fix) return 0;

    // FIX P0.2 (2026-06-09): SEE for QUIET moves too (before: return 0).
    // For a quiet move the exchange starts with the opponent's cheapest capture on `to`
    // (gain[0] = 0). With the old "return 0": (a) the quiet SEE pruning
    // (threads.cpp, SEEQuietMargin) NEVER fired; (b) the SEE>=-75 filter of
    // CheckOrdering was a no-op. NB: promotions stay out of scope (the
    // caller already excludes them from the SEE pruning).

    // Local copy for simulation
    U64 bb[12], occ[3];
    memcpy(bb, td.bitboards, sizeof(td.bitboards));
    memcpy(occ, td.occupancies, sizeof(td.occupancies));

    int gain[32];
    int depth = 0;
    int current_side = td.side;

    // Initial gain (0 for quiet moves: no piece collected by the move itself)
    gain[0] = (captured != -1) ? val[captured] + pawn_struct_adj(td, captured_sq, captured) : 0;

    // Remove attacker from source
    pop_bit(bb[piece], from);
    pop_bit(occ[current_side], from);
    pop_bit(occ[both], from);

    // Remove captured from its square (== `to` salvo en passant)
    if (captured != -1) {
        pop_bit(bb[captured], captured_sq);
        pop_bit(occ[current_side ^ 1], captured_sq);
        if (captured_sq != to)                     // e.p.: frees occ[both] too
            pop_bit(occ[both], captured_sq);
    }

    // Attacker now on target
    set_bit(bb[piece], to);
    set_bit(occ[current_side], to);
    set_bit(occ[both], to);
    
    int attacker_value = val[piece];
    current_side ^= 1;
    
    // Simulate exchanges
    while (depth < 31) {
        depth++;
        
        int from_sq;
        int attacker = get_lva(bb, occ, to, current_side, &from_sq);
        
        if (attacker == -1) break;
        
        gain[depth] = attacker_value - gain[depth - 1];
        
        // Stand-pat pruning (classic swap SEE: max(-gain[d-1], gain[d]) < 0).
        // BUG FIX 2026-07-16: the second clause was `gain[d-1] < 0`, unsatisfiable
        // (if gain[d-1]<0 then gain[d]=val-gain[d-1]>0) -> DEAD break, prune never active.
        // Correct = `gain[d-1] > 0`. Value-preserving (same SEE, it only exits earlier) -> bench unchanged.
        if (gain[depth] < 0 && gain[depth - 1] > 0) break;
        
        attacker_value = val[attacker];
        
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
// td_see_ge(move, threshold): is the static exchange on the target square worth at least `threshold` for the side
// that moves? Every caller in the engine asks only this question, never the exact value.
// 20/10/2026, written in the textbook swap-list form: the captures on `to` alternate between the sides, each with its
// least valuable attacker; gain[d] is the material balance, for the side that makes capture d, if the exchange stops
// right after it; the list is then resolved backwards, each side being free to stop instead of capturing. Rules:
// - pieces are taken off `occupied` only, and x-ray attackers behind them are added by probing the slider lines
//   again (only the lines a piece of that kind can open: a knight or a king opens none);
// - SeePinned (05/10/2026): while a pinner of the other side still stands (and is not the piece just captured on
//   `to`), our pinned pieces do not recapture (pins computed once per position by see_holds, 11_queue.inc);
// - the king captures only if the square is no longer attacked by the other side;
// - a promotion gains (promoted piece - pawn) and the piece then standing on `to` is the promoted one (SeePromo,
//   17/10/2026, review by Bastiii);
// - the least valuable attacker is found by an index (lowest set bit of the mask of the kinds present, W2,
//   08/10/2026), not by a chain of tests per kind.
// The two bounds of the resolved list are tested first (the walk is needed only between them).
static int see_ge_impl(ThreadData& td, int move, int threshold) {
    const int from = get_move_source(move);
    const int to   = get_move_target(move);
    int piece      = get_move_piece(move);
    // Piece values by piece code (the per-expert table of the thread, 28/10/2026),
    // where the two colours can differ (a kind index t is turned into a code with the side's base below).
    const int* const val = see_values(td);

    // Victim on `to`: for a pseudo-legal move `to` is empty (-1) or holds an enemy piece.
    int captured = td.piece_on[to];
    U64 occupied = td.occupancies[both] ^ (1ULL << from);   // the mover leaves its square
    if (get_move_enpassant(move)) {
        captured = (td.side == white) ? p : P;
        occupied ^= (td.side == white) ? (1ULL << (to + 8)) : (1ULL << (to - 8));
    }
    int promo_gain = 0;
    if (get_move_promoted(move)) {
        const int promoted = get_move_promoted(move);
        promo_gain = val[promoted] - val[piece];
        piece = promoted;
    }

    int gain[40];
    int d = 0;
    // The first victim with its pawn-structure correction (see.h; 0 unless a PawnAdj lever is set).
    gain[0] = ((captured != -1) ? val[captured] + pawn_struct_adj(td, get_move_enpassant(move) ? (td.side == white ? to + 8 : to - 8) : to, captured) : 0) + promo_gain;
    int standing = val[piece];   // value of the piece now on `to`, the next victim

    // Bounds of the resolved list, known before any attacker is looked up (20/10/2026, speed, same answers): the
    // backward pass only lowers gain[0], so the result is at most gain[0]; and it gives gain[0] >= min(gain[0],
    // -gain[1]) with gain[1] at most standing - gain[0], so the result is at least gain[0] - standing (after any
    // recapture we can stop). Most calls are decided here.
    if (gain[0] < threshold)
        return 0;
    if (gain[0] - standing >= threshold)
        return 1;

    const U64 diag_sliders = td.bitboards[B] | td.bitboards[b] | td.bitboards[Q] | td.bitboards[q];
    const U64 orth_sliders = td.bitboards[R] | td.bitboards[r] | td.bitboards[Q] | td.bitboards[q];
    const SliderPair sp0 = get_slider_pair(to, occupied);
    U64 attackers = (pawn_attacks[black][to] & td.bitboards[P])
                  | (pawn_attacks[white][to] & td.bitboards[p])
                  | (knight_attacks[to] & (td.bitboards[N] | td.bitboards[n]))
                  | (king_attacks[to] & (td.bitboards[K] | td.bitboards[k]))
                  | (pair_bishop(sp0) & diag_sliders)
                  | (pair_rook(sp0) & orth_sliders);

    // lines a removed piece can open toward `to`: diagonals after a pawn, bishop or queen; files and ranks after a
    // rook or queen (kinds 0..4 = pawn, knight, bishop, rook, queen)
    static constexpr U64 opens_diag[5] = {~0ULL, 0, ~0ULL, 0, ~0ULL};
    static constexpr U64 opens_orth[5] = {0, 0, 0, ~0ULL, ~0ULL};

    for (int side = td.side ^ 1;; side ^= 1) {
        attackers &= occupied;
        U64 mine = attackers & td.occupancies[side];
        if (g_see_pinned && (td.see_pinners[side ^ 1] & occupied & ~(1ULL << to)))
            mine &= ~td.see_pinned[side];
        if (!mine)
            break;
        const int base = (side == white) ? P : p;
        const unsigned kinds = unsigned((mine & td.bitboards[base + 0]) != 0)
                             | unsigned((mine & td.bitboards[base + 1]) != 0) << 1
                             | unsigned((mine & td.bitboards[base + 2]) != 0) << 2
                             | unsigned((mine & td.bitboards[base + 3]) != 0) << 3
                             | unsigned((mine & td.bitboards[base + 4]) != 0) << 4
                             | 32u;
        const int t = __builtin_ctz(kinds);
        if (t == 5) {   // only the king is left: it may capture only an undefended piece, and the exchange ends
            if (!(attackers & td.occupancies[side ^ 1])) {
                gain[d + 1] = standing - gain[d];
                d++;
            }
            break;
        }
        gain[d + 1] = standing - gain[d];
        d++;
        // The two colours have different values (each side's pieces are valued by its own expert, 28/10/2026). The
        // release used val[t] (White's values for both sides) until 28/10: harmless with one shared table, a different
        // tree with the per-expert offsets (bench 365341 instead of 366783).
        standing = val[base + t];
        const U64 bb = mine & td.bitboards[base + t];
        occupied ^= bb & (0 - bb);
        const SliderPair sp = get_slider_pair(to, occupied);
        attackers |= (pair_bishop(sp) & diag_sliders & opens_diag[t]) | (pair_rook(sp) & orth_sliders & opens_orth[t]);
    }
    // Backwards: the side making capture d takes it only if it does better than stopping before it.
    while (d > 0) {
        gain[d - 1] = (std::min)(gain[d - 1], -gain[d]);   // parentheses: windows.h min macro under clang-cl
        d--;
    }
    return gain[0] >= threshold;
}

// (27/10/2026) The previous index form see_ge_prev and its call-by-call check (-DTRIUMV_VERIFY_SEE2) were removed:
// the swap list above has been the only form since 20/10 (copy in _backup/Triumviratus_8.1_pre_testo_nostro_2026-10-27).
// The whole test, for the exchanges the inline bounds of td_see_ge (see.h) leave undecided (25/11/2026).
int td_see_ge_walk(ThreadData& td, int move, int threshold) {
    return see_ge_impl(td, move, threshold);
}
const int* see_values_ext(const ThreadData& td) { return see_values(td); }

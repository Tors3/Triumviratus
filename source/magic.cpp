#include "defs.h"
#include "attacks.h"
#include "magic.h"
#if defined(USE_PEXT)
#include <immintrin.h>   // _pext_u64 (BMI2): sliding-attack index senza moltiplicazione (come SF bmi2)
#endif

// find appropriate magic number
U64 find_magic_number(int square, int relevant_bits, int bishop)
{
    U64 occupancies[4096];
    U64 attacks[4096];
    U64 used_attacks[4096];

    U64 attack_mask = bishop ? mask_bishop_attacks(square) : mask_rook_attacks(square);
    int occupancy_indices = 1 << relevant_bits;

    for (int index = 0; index < occupancy_indices; index++)
    {
        occupancies[index] = set_occupancy(index, relevant_bits, attack_mask);
        attacks[index] = bishop ? bishop_attacks_on_the_fly(square, occupancies[index]) :
            rook_attacks_on_the_fly(square, occupancies[index]);
    }

    for (int random_count = 0; random_count < 100000000; random_count++)
    {
        U64 magic_number = generate_magic_number();

        if (count_bits((attack_mask * magic_number) & 0xFF00000000000000) < 6) continue;

        memset(used_attacks, 0ULL, sizeof(used_attacks));

        int index, fail;

        for (index = 0, fail = 0; !fail && index < occupancy_indices; index++)
        {
            int magic_index = (int)((occupancies[index] * magic_number) >> (64 - relevant_bits));

            if (used_attacks[magic_index] == 0ULL)
                used_attacks[magic_index] = attacks[index];
            else if (used_attacks[magic_index] != attacks[index])
                fail = 1;
        }

        if (!fail)
            return magic_number;
    }

    printf("  Magic number fails!\n");
    return 0ULL;
}

// init magic numbers
void init_magic_numbers()
{
    for (int square = 0; square < 64; square++)
        rook_magic_numbers[square] = find_magic_number(square, rook_relevant_bits[square], rook);

    for (int square = 0; square < 64; square++)
        bishop_magic_numbers[square] = find_magic_number(square, bishop_relevant_bits[square], bishop);
}

// init slider piece's attack tables
void init_sliders_attacks(int bishop)
{
    for (int square = 0; square < 64; square++)
    {
        bishop_masks[square] = mask_bishop_attacks(square);
        rook_masks[square] = mask_rook_attacks(square);

        U64 attack_mask = bishop ? bishop_masks[square] : rook_masks[square];
        int relevant_bits_count = count_bits(attack_mask);
        int occupancy_indices = (1 << relevant_bits_count);

        for (int index = 0; index < occupancy_indices; index++)
        {
            if (bishop)
            {
                U64 occupancy = set_occupancy(index, relevant_bits_count, attack_mask);
#if defined(USE_PEXT)
                int magic_index = (int)_pext_u64(occupancy, bishop_masks[square]);
#else
                int magic_index = (int)((occupancy * bishop_magic_numbers[square]) >> (64 - bishop_relevant_bits[square]));
#endif
                bishop_attacks[square][magic_index] = bishop_attacks_on_the_fly(square, occupancy);
            }
            else
            {
                U64 occupancy = set_occupancy(index, relevant_bits_count, attack_mask);
#if defined(USE_PEXT)
                int magic_index = (int)_pext_u64(occupancy, rook_masks[square]);
#else
                int magic_index = (int)((occupancy * rook_magic_numbers[square]) >> (64 - rook_relevant_bits[square]));
#endif
                rook_attacks[square][magic_index] = rook_attacks_on_the_fly(square, occupancy);
            }
        }
    }
}

#if defined(USE_AVX2) && !defined(TRIUMV_NO_DUALHQ)
// P4 (08/10/2026, velocita', stessi attacchi): la ricerca calcola gli attacchi dei pezzi lunghi con la hyperbola
// quintessence vettoriale della rete (DualMagic, nnue/attacks.h: alfiere e torre insieme, maschere di 64 byte per casa
// e una tabella da 2 KB per le traverse) invece che con le tabelle PEXT (2,25 MB, una lettura a caso per attacco). Il
// calcolo usa solo la geometria dei bit, quindi vale con la nostra numerazione (a8 = 0) come per la rete. xperf 12
// giri, PGO: cicli -0,77% mediogioco, -1,61% finali (istruzioni +0,8%, IPC 1,35 -> 1,38). -DTRIUMV_VERIFY_DUALHQ
// confronta ogni attacco con le tabelle PEXT (abort al primo disaccordo); -DTRIUMV_NO_DUALHQ torna alle tabelle.
// windows.h (via defs.h) definisce le macro min/max: con clang-cl rompono std::min in nnue/bitboard.h.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max
#include "nnue/attacks.h"
#pragma pop_macro("max")
#pragma pop_macro("min")
#include <cstdio>
#include <cstdlib>
static U64 pext_bishop_attacks(int square, U64 occupancy);
static U64 pext_rook_attacks(int square, U64 occupancy);
static inline std::pair<U64, U64> hq_both(int square, U64 occupancy) {
    const auto [b, r] = Triumviratus::Attacks::dual_magic(Triumviratus::Square(square)).both_attacks_bb(occupancy);
#ifdef TRIUMV_VERIFY_DUALHQ
    if (b != pext_bishop_attacks(square, occupancy) || r != pext_rook_attacks(square, occupancy)) {
        printf("info string DUALHQ SBAGLIATO: casa %d occ %llx\n", square, (unsigned long long)occupancy);
        fflush(stdout);
        abort();
    }
#endif
    return {b, r};
}
U64 get_bishop_attacks(int square, U64 occupancy) { return hq_both(square, occupancy).first; }
U64 get_rook_attacks(int square, U64 occupancy) { return hq_both(square, occupancy).second; }
U64 get_queen_attacks(int square, U64 occupancy) {
    const auto [b, r] = hq_both(square, occupancy);
    return b | r;
}
// AA1 (10/10/2026): la coppia alfiere/torre di un solo calcolo, nel registro xmm (magic.h).
SliderPair get_slider_pair(int square, U64 occupancy) {
    const auto [b, r] = hq_both(square, occupancy);
    return _mm_set_epi64x((long long)r, (long long)b);
}
#define TRIUMV_AA1_PAIR_DONE
// Le funzioni a tabelle restano, con un altro nome, per la verifica.
#define get_bishop_attacks pext_bishop_attacks
#define get_rook_attacks pext_rook_attacks
#define get_queen_attacks pext_queen_attacks
static U64 pext_queen_attacks(int square, U64 occupancy);
#endif

// get bishop attacks
U64 get_bishop_attacks(int square, U64 occupancy)
{
#if defined(USE_PEXT)
    return bishop_attacks[square][(int)_pext_u64(occupancy, bishop_masks[square])];
#else
    occupancy &= bishop_masks[square];
    occupancy *= bishop_magic_numbers[square];
    occupancy >>= 64 - bishop_relevant_bits[square];
    return bishop_attacks[square][occupancy];
#endif
}

// get rook attacks
U64 get_rook_attacks(int square, U64 occupancy)
{
#if defined(USE_PEXT)
    return rook_attacks[square][(int)_pext_u64(occupancy, rook_masks[square])];
#else
    occupancy &= rook_masks[square];
    occupancy *= rook_magic_numbers[square];
    occupancy >>= 64 - rook_relevant_bits[square];
    return rook_attacks[square][occupancy];
#endif
}

// get queen attacks
U64 get_queen_attacks(int square, U64 occupancy)
{
#if defined(USE_PEXT)
    return bishop_attacks[square][(int)_pext_u64(occupancy, bishop_masks[square])]
         | rook_attacks[square][(int)_pext_u64(occupancy, rook_masks[square])];
#else
    U64 queen_attacks = 0ULL;
    U64 bishop_occupancy = occupancy;
    U64 rook_occupancy = occupancy;

    bishop_occupancy &= bishop_masks[square];
    bishop_occupancy *= bishop_magic_numbers[square];
    bishop_occupancy >>= 64 - bishop_relevant_bits[square];
    queen_attacks = bishop_attacks[square][bishop_occupancy];

    rook_occupancy &= rook_masks[square];
    rook_occupancy *= rook_magic_numbers[square];
    rook_occupancy >>= 64 - rook_relevant_bits[square];
    queen_attacks |= rook_attacks[square][rook_occupancy];

    return queen_attacks;
#endif
}

#ifndef TRIUMV_AA1_PAIR_DONE
// AA1 senza DualMagic (build senza AVX2 o -DTRIUMV_NO_DUALHQ): le due tabelle, come prima.
SliderPair get_slider_pair(int square, U64 occupancy)
{
    const U64 b = get_bishop_attacks(square, occupancy), r = get_rook_attacks(square, occupancy);
#if defined(__x86_64__) || defined(_M_X64)
    return _mm_set_epi64x((long long)r, (long long)b);
#else
    return SliderPair{b, r};
#endif
}
#endif

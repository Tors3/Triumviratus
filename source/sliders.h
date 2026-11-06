// Triumviratus, a UCI chess engine.
// Copyright (C) 2026 Francesco Torsello
// SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)

// sliders.h -- attacks of the sliding pieces from the geometry of the bits (block 7 of 8.1, 18/10/2026).
//
// Every square lies on four lines: two diagonals, its file and its rank. On one line a slider attacks every square
// from the nearest occupied square on one side to the nearest occupied square on the other side, both included, or
// to the end of the line when that side is empty. Bishops use the two diagonals, rooks the file and the rank.
// Square numbering is the engine's (a8 = 0, h1 = 63); only the bit geometry matters, so the same code would hold for
// any numbering in which a rank is one byte.
//
// Three kernels, chosen at compile time; all give the same attacks, checked against an independent ray walk
// (b6_consilium/test_sliders.cpp: every square, every subset of the relevant squares, random boards).
//
//  * AVX-512 (F, CD, VL) -- "obstruction difference" (chessprogramming wiki) on the four lines at once. A line is
//    split into the squares below the slider (lower bit index) and above it:
//        lower   = occ & below,  upper = occ & above
//        blocker = highest bit of (lower | 1)               (bit 0 stands for "nothing below")
//        att     = (upper ^ (upper - blocker)) & line
//    upper - blocker clears the lowest occupied square above and sets every bit from the blocker up to it, so the xor
//    keeps exactly the run from the blocker below to the blocker above. (The name "near" would clash with a windows.h
//    macro.) The four lines sit in the four 64-bit lanes of one register; vplzcntq gives the highest bit of each lane,
//    so no table and no byte reversal are needed. Lanes are ordered (diagonal, file, antidiagonal, rank): or-ing the
//    two 128-bit halves gives (bishop, rook) in one xmm.
//
//  * AVX2 -- hyperbola quintessence: on a line with at most one square per rank (diagonals, file), reversing the
//    bytes of a bitboard reverses the order of the line, so
//        att = ((x - 2s) ^ bswap(bswap(x) - 2 bswap(s))) & line,   x = occ & line, s = the slider's bit
//    pair_hqs (default): the two diagonals in one xmm register (vpshufb reverses the bytes of each lane), while the
//    rook is computed in general registers at the same time: the file with the same formula and bswap, the rank from
//    a table of 8 x 64 bytes (attacks from each file for each occupancy of the six inner squares of the rank). The
//    two halves run on different execution ports and nothing crosses the halves of a ymm register.
//    pair_hq (-DSLIDERS_FORCE_HQ): diagonals and file in three lanes of one ymm register, rank from the table.
//
//  * Scalar (no AVX2, other processors) -- obstruction difference line by line.
//
// Measured in the whole engine on the Xeon Gold 6138 (PGO, cycles per node against the previous attacks):
// obstruction difference -0.98% middlegame, -1.20% endgame; pair_hqs -0.23% middlegame; pair_hq +0.16 to +0.35%.
//
// The pair is returned in one register on x86-64 (SliderPair, magic.h: lane 0 = bishop, lane 1 = rook).

#ifndef SLIDERS_H_INCLUDED
#define SLIDERS_H_INCLUDED

#include <cstdint>

#if defined(USE_AVX2) || defined(USE_AVX512)
    #include <immintrin.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
    #include <intrin.h>   // _BitScanReverse64, _byteswap_uint64
#endif

namespace sliders {

using u64 = std::uint64_t;

// Lines through a square, lane order (diagonal, file, antidiagonal, rank); the square itself is in none of them.
struct alignas(64) Lines {
    u64 below[4];   // squares of each line with a lower index than the square
    u64 line[4];    // all squares of each line
};
// The three lines with at most one square per rank (diagonal, file, antidiagonal; lane 3 unused, zero) and, per lane,
// twice the slider's bit and twice its byte-reversed bit, for the AVX2 kernel: loaded with the masks, so the only
// value moved from a general register into a vector one is the occupancy.
struct alignas(64) LinesHQ {
    u64 line[4];
    u64 twoS[4];
    u64 twoSRev[4];
    u64 pad[4];
};

extern Lines        g_lines[64];
extern LinesHQ      g_linesHQ[64];
extern std::uint8_t g_rank8[8][64];   // [file][inner occupancy of the rank] -> attacked squares of the rank

void init();   // fills the tables (once, at startup)

// ---------------------------------------------------------------- scalar kernel (any processor)
inline u64 bswap64(u64 x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(x);
#else
    return _byteswap_uint64(x);
#endif
}
inline u64 highest_bit(u64 x) {   // x != 0
#if defined(__GNUC__) || defined(__clang__)
    return 0x8000000000000000ULL >> __builtin_clzll(x);
#else
    unsigned long i;
    _BitScanReverse64(&i, x);
    return 1ULL << i;
#endif
}
inline u64 line_attacks(u64 occ, u64 below, u64 line) {
    const u64 lower = occ & below, upper = occ & line & ~below;
    const u64 blocker  = highest_bit(lower | 1);
    return (upper ^ (upper - blocker)) & line;
}
inline void pair_scalar(int sq, u64 occ, u64& bishop, u64& rook) {
    const Lines& L = g_lines[sq];
    bishop = line_attacks(occ, L.below[0], L.line[0]) | line_attacks(occ, L.below[2], L.line[2]);
    rook   = line_attacks(occ, L.below[1], L.line[1]) | line_attacks(occ, L.below[3], L.line[3]);
}

// ---------------------------------------------------------------- AVX-512 kernel: four lines in one register
#if defined(USE_AVX512) && defined(__AVX512CD__) && !defined(SLIDERS_FORCE_HQ)
    #define SLIDERS_HAVE_OD 1
inline __m128i pair_od(int sq, u64 occ) {
    const Lines&  L     = g_lines[sq];
    const __m256i o     = _mm256_set1_epi64x(static_cast<long long>(occ));
    const __m256i below = _mm256_load_si256(reinterpret_cast<const __m256i*>(L.below));
    const __m256i line  = _mm256_load_si256(reinterpret_cast<const __m256i*>(L.line));
    // vpternlogq truth tables, bit index = 4a + 2b + c: (a & b) | c -> 0xEA; a & b & ~c -> 0x40; (a ^ b) & c -> 0x28.
    const __m256i lower1 = _mm256_ternarylogic_epi64(o, below, _mm256_set1_epi64x(1), 0xEA);   // (o & below) | 1
    const __m256i upper  = _mm256_ternarylogic_epi64(o, line, below, 0x40);                    // o & line & ~below
    const __m256i z      = _mm256_lzcnt_epi64(lower1);
    const __m256i blocker  = _mm256_srlv_epi64(_mm256_set1_epi64x(static_cast<long long>(0x8000000000000000ULL)), z);
    const __m256i att   = _mm256_ternarylogic_epi64(upper, _mm256_sub_epi64(upper, blocker), line, 0x28);   // (a^b)&c
    return _mm_or_si128(_mm256_castsi256_si128(att), _mm256_extracti128_si256(att, 1));
}
#endif

// ---------------------------------------------------------------- AVX2 kernel: three lines + the rank table
#if defined(USE_AVX2) || defined(USE_AVX512)
    #define SLIDERS_HAVE_HQ 1
inline __m128i pair_hq(int sq, u64 occ) {
    const LinesHQ& H    = g_linesHQ[sq];
    const __m256i  rev  = _mm256_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8,
                                           7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
    const __m256i  line = _mm256_load_si256(reinterpret_cast<const __m256i*>(H.line));
    const __m256i  x    = _mm256_and_si256(_mm256_set1_epi64x(static_cast<long long>(occ)), line);
    // bswap(occ & line) is the reversed line occupancy: one shuffle, no reversed masks.
    const __m256i  fwd  = _mm256_sub_epi64(x, _mm256_load_si256(reinterpret_cast<const __m256i*>(H.twoS)));
    const __m256i  bwd  = _mm256_sub_epi64(_mm256_shuffle_epi8(x, rev),
                                           _mm256_load_si256(reinterpret_cast<const __m256i*>(H.twoSRev)));
    const __m256i  att  = _mm256_and_si256(_mm256_xor_si256(fwd, _mm256_shuffle_epi8(bwd, rev)), line);
    // Rank: the six inner squares of the slider's rank index the table; the result goes back to the rank's byte.
    const int  base = sq & 56;
    const u64  rank = u64(g_rank8[sq & 7][(occ >> (base + 1)) & 63]) << base;
    const __m128i lo = _mm_or_si128(_mm256_castsi256_si128(att), _mm256_extracti128_si256(att, 1));   // (b, file)
    return _mm_or_si128(lo, _mm_slli_si128(_mm_cvtsi64_si128(static_cast<long long>(rank)), 8));
}

// pair_hqs: the two diagonals in an xmm register, the rook in general registers (see the head of this file).
inline __m128i hq_lanes(__m128i o, const u64* line, const u64* twoS, const u64* twoSRev, __m128i& lineOut) {
    const __m128i rev = _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
    lineOut           = _mm_load_si128(reinterpret_cast<const __m128i*>(line));
    const __m128i x   = _mm_and_si128(o, lineOut);
    const __m128i fwd = _mm_sub_epi64(x, _mm_load_si128(reinterpret_cast<const __m128i*>(twoS)));
    const __m128i bwd = _mm_sub_epi64(_mm_shuffle_epi8(x, rev), _mm_load_si128(reinterpret_cast<const __m128i*>(twoSRev)));
    return _mm_and_si128(_mm_xor_si128(fwd, _mm_shuffle_epi8(bwd, rev)), lineOut);
}
inline u64 rank_attacks(int sq, u64 occ) {
    const int base = sq & 56;
    return u64(g_rank8[sq & 7][(occ >> (base + 1)) & 63]) << base;
}
struct alignas(64) LinesBishop {   // (diagonal, antidiagonal), twice the bit, twice the reversed bit; the file mask
    u64 line[2], twoS[2], twoSRev[2], file, pad;
};
extern LinesBishop g_linesB[64];
inline __m128i pair_hqs(int sq, u64 occ) {
    // The lines of the square in one register (R26, 06/11/2026, same attacks): as base + index (g_linesB + 64 sq) the
    // folded loads of hq_lanes are split into two uops each on Intel cores (three-operand VEX ops); AMD does not care.
    const LinesBishop* pB = &g_linesB[sq];
#if defined(__clang__) || defined(__GNUC__)
    __asm__("" : "+r"(pB));
#endif
    const LinesBishop& B = *pB;
    __m128i            l;
    const __m128i      d = hq_lanes(_mm_set1_epi64x(static_cast<long long>(occ)), B.line, B.twoS, B.twoSRev, l);
    // file: one square per rank, so bswap reverses it
    const u64 x    = occ & B.file;
    const u64 file = ((x - B.twoS[0]) ^ bswap64(bswap64(x) - B.twoSRev[0])) & B.file;
    const u64 rook = file | rank_attacks(sq, occ);
    return _mm_unpacklo_epi64(_mm_or_si128(d, _mm_unpackhi_epi64(d, d)), _mm_cvtsi64_si128(static_cast<long long>(rook)));
}
#endif

}  // namespace sliders

#endif  // SLIDERS_H_INCLUDED

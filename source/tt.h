/*
 * TRIUMVIRATUS - Transposition Table with ABDADA Support
 *
 * ABDADA = Alpha-Beta Distributed Avoiding Duplicate Analysis
 *
 * Key concept: When a thread starts searching a node, it marks it as "busy".
 * Other threads seeing this node skip it (for non-first moves) to avoid
 * duplicating work.
 */

#ifndef TT_ABDADA_H
#define TT_ABDADA_H

#include "defs.h"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <new>       // std::nothrow (init_hash_table)
#include "profile.h"

 // Hash flags
#define hash_flag_exact 0
#define hash_flag_alpha 1
#define hash_flag_beta 2
// hash_flag_none (5.1): BOUND_NONE for the "eval-only" entries (no valid score/move/depth).
// It is 3 = free value of the 2-bit flag field; the consumers compare == exact/alpha/beta
// (0/1/2) -> flag=3 IGNORED everywhere. Defined here (probe_tt below needs it).
#define hash_flag_none 3

// ABDADA busy flag - indicates node is being searched
#define TT_BUSY_DEPTH 255

/*
 * TT Entry structure with ABDADA support
 *
 * Layout (16 bytes for cache alignment):
 * - key: 8 bytes (position hash XOR data for lockless)
 * - data: 8 bytes packed (move, score, depth, flag, busy)
 */
#define tt_eval_none 32001   // (repeated below with the same text: TT16 already needs it here)

// ===== TT16 (NPS 25/09/2026) ==================================================
// 16-byte entry, bucket of 4 entries = 64 bytes = ONE cache line.
// Before (old TT, removed on 25/09/2026 after the SPRT: TT16 +6.30 ± 5.06 over 5,365 games;
// source in _backup/Triumviratus_8.0_src_2026-09-25_pre_cleanup/tt.h): 24-byte entries in buckets of 2, i.e. 48 bytes, and with
// the index modulo HALF the buckets it crossed two lines: two misses per probe. The
// xperf profile of 25/09 (samples on LLCMisses) gave the TT ~4.6% of the engine's misses
// against ~1.2% for SF, which uses aligned 32-byte clusters.
//   kw   = ((key & 0xFFFFFFFFFFFF) << 16 | eval16) XOR data
//   data = move | score | depth | flag | age | pv   (layout unchanged, pack_tt_data)
// The XOR binds the two words as before (lockless): a torn concurrent write
// makes the key check fail, and the eval sits INSIDE it, so it is protected
// by the same check (before it had its own 16-bit key fragment).
// The check uses the LOW 48 bits of the key; the index (tt_base_index, mulhi) uses
// the HIGH ones: the two stay almost independent.
struct alignas(16) tt_entry {
    U64 kw;
    U64 data;
};
constexpr int TT_WAYS = 4;
struct alignas(64) tt_bucket {
    tt_entry e[TT_WAYS];
};
static_assert(sizeof(tt_bucket) == 64, "TT16: il bucket deve essere una linea di cache");
constexpr U64 TT_TAG_MASK = 0xFFFFFFFFFFFFULL;
inline U64 tt_tag(U64 key) { return key & TT_TAG_MASK; }
inline int tt_eval16(int eval) {
    if (eval == tt_eval_none || eval > 30000 || eval < -30000) return 0;
    return (eval + 32768) & 0xFFFF;
}
inline int tt_unpack_eval16(U64 w) {
    const int e = (int)(w & 0xFFFF);
    if (e == 0) return tt_eval_none;
    const int v = e - 32768;
    return (v > 30000 || v < -30000) ? tt_eval_none : v;
}

// Data packing/unpacking.
// Bit layout of `data` (64 bit): move[0..23] score[24..39] depth[40..47]
// flag[48..49] spare[50..57] age[58..62] pv[63].
// FIX P0.1 (2026-06-09): 24-bit move (bits 21/22/23 = double/ep/castling).
// P1.1/4.0 (2026-06-11): STATIC EVAL in the TT on 16 BITS, in the `ext` field
// of the 24B entry (see above). The engine is eval-bound (58% of the node time =
// NNUE forward): a TT hit that carries the static eval saves the forward.
#define tt_eval_none 32001

inline U64 pack_tt_data(int move, int score, int depth, int flag, int age, int pv = 0) {
    return ((U64)(move & 0xFFFFFF)) |
        ((U64)((score + 32768) & 0xFFFF) << 24) |
        ((U64)(depth & 0xFF) << 40) |
        ((U64)(flag & 0x3) << 48) |
        ((U64)(age & 0x1F) << 58) |
        ((U64)(pv & 0x1) << 63);
}

inline int unpack_move(U64 data) { return data & 0xFFFFFF; }
inline int unpack_score(U64 data) { return ((data >> 24) & 0xFFFF) - 32768; }
inline int unpack_depth(U64 data) { return (data >> 40) & 0xFF; }
inline int unpack_flag(U64 data) { return (data >> 48) & 0x3; }
inline int unpack_age(U64 data) { return (data >> 58) & 0x1F; }
inline int unpack_pv(U64 data) { return (data >> 63) & 0x1; }

// 5.1 EvalTTWrite (SF-style): the eval-only entry stores the UNADJUSTED value (pre-rule50/scale,
// fifty-independent) via the normal pack_ext; on reading nn_finalize() re-finalizes it with the current
// fifty counter -> exact on ANY transposition (no same-fifty constraint = max cache hits).

// Global TT
extern tt_entry* hash_table;
extern U64 hash_entries;
// True when hash_entries is a power of two: then the bucket index is
// obtained with an AND instead of a `%`, giving the SAME index.
// ⚠️ IN PRACTICE IT NEVER TRIGGERS, and the previous comment said the opposite.
// hash_entries = bytes / sizeof(tt_entry) with sizeof(tt_entry) = 24: 24 does not divide
// a power of two into a power of two, so at NO realistic size does the
// arithmetic work out. At 256 MB there are 11,184,808 entries. Checked on 16, 64, 128, 256,
// 512 and 1024 MB: never. The branch stays because it is correct and would turn on by itself
// if one day the entry became 16 or 32 bytes, but do NOT believe in it as a
// live optimisation: today the modulo is always used.
// The attempt to remove that division with an exact reciprocal is documented
// further below, above tt_base_index: measured NEUTRAL and removed.
extern bool g_tt_pow2;
extern int current_age;

// 4-way set-associative TT on/off (UCI option "TT4Way"). Default off reproduces
// the original direct-mapped (1-way) table for a clean A/B. When on, each index
// maps to a bucket of 4 consecutive entries; probe scans the bucket, store picks
// an age-aware victim (prefer empty -> oldest -> shallowest).

// 5.1: "two-level" TT (UCI "TTTwoLevel") - scheme #1 in the papers (Maastricht): every
// index = bucket of 2 slots, slot0 = DEPTH-PREFERRED (keep the deep entries),
// slot1 = ALWAYS-REPLACE (keep the most RECENT = recency for revisits).
// Mutually exclusive with 4way. It targets the ttrate gap (move availability) measured 25% vs 46% SF.
extern bool g_tt_twolevel;

// TTMove24 (UCI "TTMove24", default ON) - ablation of FIX P0.1: when OFF the
// store truncates the move to 21 bits as in 3.7 (the double/ep/castling flags are lost
// again). Defined in threads.cpp.
// P1.10a (UCI "TTAgeRefresh", default ON) - a probe hit refreshes the age
// of the entry: HOT positions written in old searches are no longer
// evicted for age (SF does the same). Defined in threads.cpp.
#ifdef TRIUMV_RELEASE
// Release (06/10/2026): no option changes them, so constants the compiler folds into store_tt/probe.
// The values are the defaults of threads.cpp (01_params.inc).
constexpr bool g_ttmove24 = true;
constexpr int  g_tt_keep_margin = 3;
constexpr int  g_tt_keep_pv = 2;       // TtKeepPv (CENSUS 21/10/2026)
constexpr int  g_tt_age_weight = 8;    // TtAgeWeight (CENSUS 21/10/2026)
constexpr bool g_tt_move_keep = true;
constexpr bool g_tt_age_refresh = false;
#else
extern bool g_ttmove24;
extern int g_tt_keep_margin;   // TTKeepMargin (endgame study 26/09): see store_tt
extern int g_tt_keep_pv;       // TtKeepPv: extra margin of PV entries (CENSUS 21/10/2026)
extern int g_tt_age_weight;    // TtAgeWeight: plies per search of age in the victim value (CENSUS 21/10/2026)
extern bool g_tt_move_keep;   // TTMoveKeep: keeps the TT move on fail lows without a move (SF)
extern bool g_tt_age_refresh;
#endif

// LargePages (UCI, default ON) - allocates the TT on 2MB large pages (VirtualAlloc
// MEM_LARGE_PAGES, like the NNUE weights). Toggle for a clean NPS A/B on the SAME
// binary: OFF = new[] (normal heap = pre-change baseline). Defined in threads.cpp.
extern bool g_large_pages;

// External variables needed for compatibility functions
extern U64 hash_key;
extern U64 piece_keys[16][64];
extern U64 enpassant_keys[64];
extern U64 castle_keys[16];
extern U64 side_key;

// Constants (define if not already defined)
#ifndef mate_value
#define mate_value 31000
#endif
#ifndef mate_score
#define mate_score 30000
#endif

#include "large_pages.h"   // large_pages_alloc / free / available (no windows.h in this header)

// TT16: the table is an array of tt_bucket (alignas(64)): the aligned operator new of
// C++17 guarantees the alignment on the heap fallback too. hash_table stays a tt_entry*.
inline void init_hash_table(int mb) {
    static bool tt_on_large_pages = false;

    U64 size    = (U64)mb * 1024 * 1024;
    U64 buckets = size / sizeof(tt_bucket);
    if (buckets == 0) buckets = 1;

    if (hash_table) {
        if (tt_on_large_pages) large_pages_free(hash_table);
        else                   delete[] reinterpret_cast<tt_bucket*>(hash_table);
        hash_table = nullptr;
    }

    const U64 bytes = buckets * sizeof(tt_bucket);
    tt_bucket* tab = nullptr;
    if (g_large_pages) {
        tab = static_cast<tt_bucket*>(large_pages_alloc(bytes));
        if (!tab) {
            printf("info string Hash: %d MB su large pages non allocabili, ripiego su heap\n", mb);
            tab = new (std::nothrow) tt_bucket[buckets]();
            tt_on_large_pages = false;
        } else {
            std::memset(tab, 0, bytes);
            tt_on_large_pages = true;
        }
    } else {
        tab = new (std::nothrow) tt_bucket[buckets]();
        tt_on_large_pages = false;
    }
    if (!tab) {
        printf("info string Hash: %d MB NON allocabili, ripiego su 64 MB\n", mb);
        fflush(stdout);
        buckets = ((U64)64 * 1024 * 1024) / sizeof(tt_bucket);
        tab = new tt_bucket[buckets]();
        tt_on_large_pages = false;
    }
    hash_table   = reinterpret_cast<tt_entry*>(tab);
    hash_entries = buckets * TT_WAYS;
    g_tt_pow2    = false;   // not used by TT16 (index via mulhi)

    const int actual_mb = (int)(buckets * sizeof(tt_bucket) / (1024 * 1024));
    // 🔴 FIX 01/10/2026: tt_on_large_pages says only WHICH allocator the table comes from (needed by the free).
    // large_pages_alloc on Windows never returns null: without SeLockMemoryPrivilege it silently falls back
    // to 4 KB pages, and here "ON" was printed anyway. On the Xeon the privilege is missing: no engine has
    // ever had large pages, and the line said the opposite. large_pages_available() tries a real 2 MB allocation.
    static const bool lp_real = large_pages_available();
    const char* lp = !g_large_pages                  ? "off (disabled)"
                   : tt_on_large_pages && lp_real    ? "ON"
                                                     : "off (unavailable: no 'Lock pages in memory' privilege)";
    printf("info string Hash: %d MB, large pages %s\n", actual_mb, lp);
}

inline void clear_hash_table() {
    std::memset(hash_table, 0, hash_entries * sizeof(tt_entry));
    current_age = 0;
}

inline int hashfull() {
    U64 n = hash_entries < 1000 ? hash_entries : 1000;
    if (n == 0) return 0;
    int used = 0;
    // 17/10/2026 (review by Bastiii): only entries written in the current search count, so the GUI sees the pressure
    // of this search; before, entries kept from earlier moves counted too. Output only: the search does not read it.
    for (U64 i = 0; i < n; i++)
        if (hash_table[i].data != 0 && unpack_age(hash_table[i].data) == current_age) used++;
    return (int)((U64)used * 1000 / n);
}

// Increment age (call at start of each search)
inline void new_search() {
    current_age = (current_age + 1) & 0x1F;   // 5-bit age (see pack_tt_data)
}

// TT prefetch (toggle "TTPrefetch"). ⚠️ COMMENT CORRECTED 2026-07-26: it said
// "default OFF" and "worth re-measuring", but it had already been **BAKED ON** (see
// threads.cpp: `g_tt_prefetch = true`, +1.88% NPS measured, reconfirmed +2.75%
// in the audit of 25/07). The stale comment kept closed an NPS front that
// was already won: an auditor read it and reported the prefetch as not
// yet tried. If a comment contradicts the live default, the code wins.
// Retested 2026-07-13: the attempt of 2026-06-07 was
// NPS-neutral, but meanwhile TTTwoLevel (2-slot buckets) had been baked,
// which changes the access pattern -> hence the gain.
// Prefetch of the FIRST cache line of the bucket right after computing the child's
// key in td_make_move (post-legality: the pre-legality variant was -2.5%).
#if defined(_MSC_VER)
    #include <intrin.h>
    #define TT_PREFETCH(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)
#else
    #define TT_PREFETCH(addr) __builtin_prefetch(addr)
#endif

// ===== TT16: indirizzamento, probe, store =========================================
// Bucket index with the high multiplication (as SF): no 64-bit division.
// With the 24-byte entry the modulo was unavoidable; here the number of buckets is free
// and mulhi(key, buckets) is uniform on [0, buckets).
inline U64 tt_mulhi64(U64 a, U64 b) {
#if defined(__SIZEOF_INT128__)
    return (U64)(((unsigned __int128)a * (unsigned __int128)b) >> 64);
#elif defined(_MSC_VER)
    return __umulh(a, b);
#else
    const U64 aL = (uint32_t)a, aH = a >> 32, bL = (uint32_t)b, bH = b >> 32;
    const U64 c1 = (aL * bL) >> 32, c2 = aH * bL + c1, c3 = aL * bH + (uint32_t)c2;
    return aH * bH + (c2 >> 32) + (c3 >> 32);
#endif
}
inline U64 tt_base_index(U64 key) { return tt_mulhi64(key, hash_entries / TT_WAYS) * TT_WAYS; }
inline int tt_ways() { return TT_WAYS; }

// VECTOR TT (10/10/2026, speed, same way, same values). With AVX-512 the bucket (64 bytes, one line) fits in
// ONE register: v = [kw0 d0 kw1 d1 kw2 d2 kw3 d3]. Swapping the two words of every way (sw), v ^ sw has kw ^ data in the
// even lanes and v | sw says whether the way is empty: the comparison of the four ways is a masked vector compare.
// Bit 2i of the mask = way i non-empty with the right key; the lowest bit is the first way, as before.
// Why: in the clang -O3 code of probe_tt the scalar comparison cost ~13 instructions per way (two loads, xor,
// shift, compare, or, test, and and the copies of d[i] and w[i] on the stack for the variable index), ~58 per read out of
// ~90; here ~12, plus two extractions from the register for the way found. Counters (bench 12 on the 30 middlegame
// positions): 1.10 reads and 0.75 writes per node. The line is already fetched ahead by the make (child prefetch):
// the gain is in instructions, not in waiting. In the read the values of the way found are taken FROM THE REGISTER
// (same snapshot as the check, like the BUG FIX of 16/07/2026 on torn reads with several threads).
// It was among the ideas discarded by X4 (08/10, "~20 instructions per read, estimate < 0.2%", without measurement): the count on the
// real code is about double, and the same search also serves the write. -DTRIUMV_NO_TT_VEC goes back to the scalar
// ways; -DTRIUMV_VERIFY_TTSTORE compares this write too with tt_find/tt_victim.
#if defined(USE_AVX512) && defined(__AVX512F__) && !defined(TRIUMV_NO_TT_VEC)
#define TRIUMV_TT_VEC 1
#include <immintrin.h>
inline unsigned tt_match4(__m512i v, U64 tag, __m512i& w) {
    const __m512i sw = _mm512_shuffle_epi32(v, _MM_PERM_BADC);   // in every way: [data, kw]
    w = _mm512_xor_si512(v, sw);                                  // corsie pari: kw ^ data
    const __m512i o = _mm512_or_si512(v, sw);                     // kw | data (0 = via vuota)
    const __mmask8 tag_ok = _mm512_mask_cmpeq_epu64_mask(0x55, _mm512_srli_epi64(w, 16), _mm512_set1_epi64((long long)tag));
    return (unsigned)_mm512_mask_test_epi64_mask(tag_ok, o, o);
}
inline U64 tt_lane(__m512i v, int lane) {
    return (U64)_mm_cvtsi128_si64(_mm512_castsi512_si128(_mm512_permutexvar_epi64(_mm512_set1_epi64(lane), v)));
}
#elif defined(__AVX2__) && !defined(TRIUMV_NO_TT_VEC)
// (29/10/2026 night, speed, same way, same values) The vector TT on the AVX2 builds (Intel 12th gen and later, Core
// Ultra, AMD Zen 2/3), the bucket in two registers [kw0 d0 kw1 d1] [kw2 d2 kw3 d3]. Swapping the two words of every way
// gives kw ^ data and the emptiness test as in tt_match4; the four ways answer together. The words are stored as read
// (wv: kw ^ data at even slots, dv: the raw words), so the caller takes the way's values from the same snapshot as the
// check. Returns bit i = way i non-empty with the tag. -0.4% / -0.8% cycles (mg / eg) on the Xeon avx2 variant.
#define TRIUMV_TT_VEC2 1
#include <immintrin.h>
inline unsigned tt_match4_avx2(const void* bucket, U64 tag, U64* dv, U64* wv) {
    const __m256i v0 = _mm256_load_si256(reinterpret_cast<const __m256i*>(bucket));
    const __m256i v1 = _mm256_load_si256(reinterpret_cast<const __m256i*>(bucket) + 1);
    const __m256i s0 = _mm256_shuffle_epi32(v0, _MM_SHUFFLE(1, 0, 3, 2)), s1 = _mm256_shuffle_epi32(v1, _MM_SHUFFLE(1, 0, 3, 2));
    const __m256i w0 = _mm256_xor_si256(v0, s0), w1 = _mm256_xor_si256(v1, s1);   // even qwords: kw ^ data
    const __m256i z = _mm256_setzero_si256(), T = _mm256_set1_epi64x((long long)tag);
    const __m256i h0 = _mm256_andnot_si256(_mm256_cmpeq_epi64(_mm256_or_si256(v0, s0), z),
                                           _mm256_cmpeq_epi64(_mm256_srli_epi64(w0, 16), T));
    const __m256i h1 = _mm256_andnot_si256(_mm256_cmpeq_epi64(_mm256_or_si256(v1, s1), z),
                                           _mm256_cmpeq_epi64(_mm256_srli_epi64(w1, 16), T));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dv), v0);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dv) + 1, v1);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(wv), w0);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(wv) + 1, w1);
    const unsigned m0 = (unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(h0));   // qword lanes 0 and 2: ways 0 and 1
    const unsigned m1 = (unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(h1));   // ways 2 and 3
    return (m0 & 1) | ((m0 >> 1) & 2) | ((m1 & 1) << 2) | ((m1 << 1) & 8);
}
#endif

// Slot of the bucket that holds this position, or nullptr.
// Evening of 04/10/2026 - the four ways are compared BRANCHLESS (4-bit mask, then a single "found or not" branch):
// the loop with early exit was mispredicted almost at every read (xperf). With several equal ways the first wins,
// as before.
inline tt_entry* tt_find(U64 key) {
    tt_entry* b = &hash_table[tt_base_index(key)];
    const U64 tag = tt_tag(key);
    unsigned found = 0;
    for (int i = 0; i < TT_WAYS; i++) {
        const U64 k = b[i].kw, d = b[i].data;
        found |= (unsigned)((((k ^ d) >> 16) == tag) & ((k | d) != 0)) << i;
    }
    return found ? &b[get_ls1b_index(found)] : nullptr;
}

// Victim: empty slot, otherwise the lowest value of depth - 8*age distance
// (same rule as the old tt_victim, now on 4 ways inside a single line).
inline tt_entry* tt_victim(U64 key) {
    tt_entry* b = &hash_table[tt_base_index(key)];
    tt_entry* best = &b[0];
    int best_val = 1 << 30;
    for (int i = 0; i < TT_WAYS; i++) {
        tt_entry* e = &b[i];
        if (e->kw == 0 && e->data == 0) return e;
        int rel_age = (current_age - unpack_age(e->data)) & 0x1F;
        int depth = unpack_depth(e->data);
        int val = depth - g_tt_age_weight * rel_age;   // 04/10/2026: was 2; every search of age is worth 8 plies, as SF
        if (val < best_val) { best_val = val; best = e; }
    }
    return best;
}

inline bool probe_tt(U64 hash_key, int& tt_move, int& tt_score, int& tt_depth, int& tt_flag, int& tt_eval, bool& is_pv) {
    PROF_GUARD(prof_tt);
    tt_entry* b = &hash_table[tt_base_index(hash_key)];
    const U64 tag = tt_tag(hash_key);
#ifdef TRIUMV_TT_VEC
    {
        // Vector TT (10/10/2026, see tt_match4): same way, same values, taken from the register of the check.
        __m512i w4;
        const __m512i v4 = _mm512_load_si512(reinterpret_cast<const void*>(b));
        const unsigned m = tt_match4(v4, tag, w4);
#ifdef TRIUMV_VERIFY_TTSTORE   // check: the same way (or none) as the scalar search
        if ((m ? &b[get_ls1b_index(m) >> 1] : nullptr) != tt_find(hash_key)) {
            printf("info string TTVEC: via diversa nella lettura\n"); fflush(stdout); abort();
        }
#endif
        if (m) {
            const int lane = get_ls1b_index(m);   // 2 * via
            tt_entry* entry = &b[lane >> 1];
            const U64 data = tt_lane(v4, lane + 1);
            const U64 wi   = tt_lane(w4, lane);
#ifdef TRIUMV_VERIFY_TTSTORE   // (a single thread) the words extracted from the register are those of the way
            if (data != entry->data || wi != (entry->kw ^ entry->data)) {
                printf("info string TTVEC: parole diverse\n"); fflush(stdout); abort();
            }
#endif
            tt_move  = unpack_move(data);
            tt_score = unpack_score(data);
            tt_depth = unpack_depth(data);
            tt_flag  = unpack_flag(data);
            tt_eval  = tt_unpack_eval16(wi);
            is_pv    = (unpack_pv(data) != 0);
            if (g_tt_age_refresh && unpack_age(data) != current_age) {
                U64 new_data = (data & ~(0x1FULL << 58)) | ((U64)(current_age & 0x1F) << 58);
                entry->data = new_data;
                entry->kw   = wi ^ new_data;
            }
            if (tt_flag == hash_flag_none) return false;   // entry eval-only (EvalTTWrite)
            return true;
        }
        tt_move = 0; tt_score = 0; tt_depth = 0;
        tt_flag = hash_flag_alpha; tt_eval = tt_eval_none; is_pv = false;
        return false;
    }
#elif defined(TRIUMV_TT_VEC2)
    {
        // Vector TT on AVX2 (29/10/2026, see tt_match4_avx2): same way, same values, from the snapshot of the check.
        alignas(32) U64 dv[8], wv[8];
        const unsigned found = tt_match4_avx2(b, tag, dv, wv);
#ifdef TRIUMV_VERIFY_TTSTORE   // check: the same way (or none) as the scalar search
        if ((found ? &b[get_ls1b_index(found)] : nullptr) != tt_find(hash_key)) {
            printf("info string TTVEC2: via diversa nella lettura\n"); fflush(stdout); abort();
        }
#endif
        if (found) {
            const int i = get_ls1b_index(found);
            tt_entry* entry = &b[i];
            const U64 data = dv[2 * i + 1], wi = wv[2 * i];
            tt_move  = unpack_move(data);
            tt_score = unpack_score(data);
            tt_depth = unpack_depth(data);
            tt_flag  = unpack_flag(data);
            tt_eval  = tt_unpack_eval16(wi);
            is_pv    = (unpack_pv(data) != 0);
            if (g_tt_age_refresh && unpack_age(data) != current_age) {
                U64 new_data = (data & ~(0x1FULL << 58)) | ((U64)(current_age & 0x1F) << 58);
                entry->data = new_data;
                entry->kw   = wi ^ new_data;
            }
            if (tt_flag == hash_flag_none) return false;   // entry eval-only (EvalTTWrite)
            return true;
        }
        tt_move = 0; tt_score = 0; tt_depth = 0;
        tt_flag = hash_flag_alpha; tt_eval = tt_eval_none; is_pv = false;
        return false;
    }
#else
    // A single snapshot of the two words per way: the check and the unpack read the SAME values (no torn read
    // between check and use, cf. BUG FIX 2026-07-16). Evening of 04/10/2026: the four ways branchless, as in tt_find.
    U64 d[TT_WAYS], w[TT_WAYS];
    unsigned found = 0;
    for (int i = 0; i < TT_WAYS; i++) {
        const U64 k = b[i].kw;
        d[i] = b[i].data;
        w[i] = k ^ d[i];
        found |= (unsigned)(((w[i] >> 16) == tag) & ((k | d[i]) != 0)) << i;
    }
    if (found) {
        const int i = get_ls1b_index(found);
        tt_entry* entry = &b[i];
        const U64 data = d[i];
        tt_move  = unpack_move(data);
        tt_score = unpack_score(data);
        tt_depth = unpack_depth(data);
        tt_flag  = unpack_flag(data);
        tt_eval  = tt_unpack_eval16(w[i]);
        is_pv    = (unpack_pv(data) != 0);
        if (g_tt_age_refresh && unpack_age(data) != current_age) {
            U64 new_data = (data & ~(0x1FULL << 58)) | ((U64)(current_age & 0x1F) << 58);
            entry->data = new_data;
            entry->kw   = w[i] ^ new_data;
        }
        if (tt_flag == hash_flag_none) return false;   // entry eval-only (EvalTTWrite)
        return true;
    }
    tt_move = 0; tt_score = 0; tt_depth = 0;
    tt_flag = hash_flag_alpha; tt_eval = tt_eval_none; is_pv = false;
    return false;
#endif
}

inline bool probe_tt(U64 hash_key, int& tt_move, int& tt_score, int& tt_depth, int& tt_flag) {
    int eval_dummy; bool pv_dummy;
    return probe_tt(hash_key, tt_move, tt_score, tt_depth, tt_flag, eval_dummy, pv_dummy);
}

inline void store_tt(U64 hash_key, int move, int score, int depth, int flag, int ply = 0, bool pv = false, int eval = tt_eval_none) {
    PROF_GUARD(prof_tt);
    if (!g_ttmove24) move &= 0x1FFFFF;
    if (score > mate_score) score += ply;
    else if (score < -mate_score) score -= ply;

    int ev16 = tt_eval16(eval);
    // 06/10/2026 (speed, identical tree): ONE read of the bucket finds the position and, if missing, the victim.
    // Before, tt_find and tt_victim each recomputed the index (high multiplication) and reread the four ways, and
    // tt_victim chose with two branches per way on the data (empty way? lowest value?). Counters (bench 14 on
    // fens30): 0.75 writes per node, 0.44 with the victim search.
    tt_entry* const b = &hash_table[tt_base_index(hash_key)];
    const U64 tag = tt_tag(hash_key);
    U64 kk[TT_WAYS], dd[TT_WAYS];
    unsigned found = 0;
#ifdef TRIUMV_TT_VEC
    {
        // Vector TT (10/10/2026, see tt_match4): bit 2i -> bit i (pext), so the rest is the old code.
        __m512i w4;
        found = _pext_u32(tt_match4(_mm512_load_si512(reinterpret_cast<const void*>(b)), tag, w4), 0x55u);
    }
#elif defined(TRIUMV_TT_VEC2)
    {
        // Vector TT on AVX2 (29/10/2026): the four ways together; the raw words of the snapshot fill kk/dd for the victim.
        alignas(32) U64 dv[8], wv[8];
        found = tt_match4_avx2(b, tag, dv, wv);
        for (int i = 0; i < TT_WAYS; i++) {
            kk[i] = dv[2 * i];
            dd[i] = dv[2 * i + 1];
        }
    }
#else
    for (int i = 0; i < TT_WAYS; i++) {
        kk[i] = b[i].kw;
        dd[i] = b[i].data;
        found |= (unsigned)((((kk[i] ^ dd[i]) >> 16) == tag) & ((kk[i] | dd[i]) != 0)) << i;
    }
#endif
    tt_entry* entry;
    if (found) {
        entry = &b[get_ls1b_index(found)];   // with several equal ways the first wins, as in tt_find
#ifdef TRIUMV_VERIFY_TTSTORE
        if (entry != tt_find(hash_key)) { printf("info string TTSTORE: via diversa\n"); fflush(stdout); abort(); }
#endif
        const U64 old_data = entry->data;
        const U64 old_w    = entry->kw ^ old_data;
        // 17/10/2026 (review by Bastiii): with more threads another store can rewrite this way between the scan above
        // and these two reads, and then old_w is garbage. The words are used only if they still carry our tag;
        // otherwise the entry is simply overwritten below. With one thread the tag always matches: identical tree.
        if (((old_w >> 16) != tag)) goto write_entry;
        if (g_tt_move_keep && move == 0) move = unpack_move(old_data);
        if (ev16 == 0) ev16 = (int)(old_w & 0xFFFF);   // keeps the eval if the store does not carry one
        // TTKeepMargin (endgame study 26/09/2026, docs/audit_8.0/H_FINALI.md). In endgames, at depth >= 12,
        // the entry is there as often as in SF (74%) and is deep enough more often (32% against 26%), but its
        // bound serves the window less often (53% against 66%): we keep the DEEPER entry even if its
        // bound is old. SF overwrites if  depth + 2*pv > old - 4  (tt.cpp, TTWriter::write):
        // it prefers fresh information up to 3 plies shorter. With margin m > 0 the old one is kept
        // only if it is deeper by more than m plies (+2 on PV nodes). 0 = historical rule, byte-identical.
        const int keep_margin = g_tt_keep_margin > 0 ? g_tt_keep_margin + (pv ? g_tt_keep_pv : 0) : 0;
        if (unpack_age(old_data) == current_age && unpack_depth(old_data) > depth + keep_margin && flag != hash_flag_exact) {
            // Keeps the deeper entry; updates the eval if it was missing.
            const U64 w  = (old_w & ~0xFFFFULL) | (U64)ev16;
            if (w != old_w) entry->kw = w ^ old_data;
            return;
        }
    } else {
        // Branchless victim, same rule as tt_victim: the first empty way, otherwise the lowest value depth - 8 * age
        // and on ties the lowest way. Key = value * 4 + way (value in [-248, 255]), the empty ways at
        // -2^20 + way: the minimum key is the way chosen by tt_victim, and its two low bits are the way.
#ifdef TRIUMV_TT_VEC
        for (int i = 0; i < TT_WAYS; i++) {   // the line has just been read into the register: here it is in L1
            kk[i] = b[i].kw;
            dd[i] = b[i].data;
        }
#endif
        int best = 1 << 30;
        for (int i = 0; i < TT_WAYS; i++) {
            const int rel_age = (current_age - unpack_age(dd[i])) & 0x1F;
            const int val = unpack_depth(dd[i]) - g_tt_age_weight * rel_age;   // TtAgeWeight <= 16: val >= -496
            const int key = ((kk[i] | dd[i]) == 0 ? -(1 << 20) : val * 4) + i;
            best = key < best ? key : best;
        }
        entry = &b[best & 3];
#ifdef TRIUMV_VERIFY_TTSTORE   // check: the same way as tt_victim
        if (entry != tt_victim(hash_key)) { printf("info string TTSTORE: vittima diversa\n"); fflush(stdout); abort(); }
#endif
    }
write_entry:
    const U64 new_data = pack_tt_data(move, score, depth, flag, current_age, pv ? 1 : 0);
    const U64 w = (tt_tag(hash_key) << 16) | (U64)ev16;
    entry->data = new_data;
    entry->kw   = w ^ new_data;
}


// Cache-only of the static eval on a MISS (EvalTTWrite, default OFF): flag_none entry
// in the natural victim slot, so the real store finds it again and updates it in place.
inline void tt_cache_eval(U64 hash_key, int unadjusted) {
    const int ev16 = tt_eval16(unadjusted);
    if (ev16 == 0) return;
    tt_entry* entry = tt_find(hash_key);
    if (entry) {
        const U64 d = entry->data;
        if (unpack_flag(d) == hash_flag_none)
            entry->kw = ((tt_tag(hash_key) << 16) | (U64)ev16) ^ d;
        return;
    }
    entry = tt_victim(hash_key);
    const U64 d = pack_tt_data(0, 0, 0, hash_flag_none, current_age, 0);
    entry->data = d;
    entry->kw   = ((tt_tag(hash_key) << 16) | (U64)ev16) ^ d;
}

inline int get_tt_move(U64 hash_key) {
    tt_entry* entry = tt_find(hash_key);
    if (entry) return unpack_move(entry->data);
    return 0;
}

// ============================================================================
// COMPATIBILITY LAYER - Old API functions
// ============================================================================

#define no_hash_entry 100000

// Need ply as extern for compatibility
extern int ply;

/*
 * read_hash_entry - Compatible with old API (4 arguments)
 * Returns score if valid entry found, no_hash_entry otherwise
 */
inline int read_hash_entry(int alpha, int beta, int* best_move, int depth) {
    int tt_move, tt_score, tt_depth, tt_flag;

    if (!probe_tt(hash_key, tt_move, tt_score, tt_depth, tt_flag)) {
        return no_hash_entry;
    }

    *best_move = tt_move;

    if (tt_depth >= depth) {
        // Adjust mate scores using global ply
        if (tt_score < -mate_score) tt_score += ply;
        if (tt_score > mate_score) tt_score -= ply;

        if (tt_flag == hash_flag_exact) return tt_score;
        if (tt_flag == hash_flag_alpha && tt_score <= alpha) return alpha;
        if (tt_flag == hash_flag_beta && tt_score >= beta) return beta;
    }

    return no_hash_entry;
}

/*
 * write_hash_entry - Compatible with old API
 */
inline void write_hash_entry(int score, int best_move, int depth, int flag) {
    store_tt(hash_key, best_move, score, depth, flag);
}

#endif // TT_ABDADA_H
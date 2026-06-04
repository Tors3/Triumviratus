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

 // Hash flags
#define hash_flag_exact 0
#define hash_flag_alpha 1
#define hash_flag_beta 2

// ABDADA busy flag - indicates node is being searched
#define TT_BUSY_DEPTH 255

/*
 * TT Entry structure with ABDADA support
 *
 * Layout (16 bytes for cache alignment):
 * - key: 8 bytes (position hash XOR data for lockless)
 * - data: 8 bytes packed (move, score, depth, flag, busy)
 */
struct alignas(16) tt_entry {
    U64 key;           // hash_key XOR data (for lockless verification)

    // Packed data
    U64 data;          // Contains: move (21 bits) | score (16 bits) | depth (8 bits) | flag (2 bits) | busy (8 bits) | age (8 bits)
};

// Data packing/unpacking.
// Bit layout of `data` (64 bit): move[0..20] score[21..36] depth[37..44]
// flag[45..46] busy[47..54] age[55..62] pv[63]. Bit 63 (ttPv) era libero.
// `pv` ha default 0 -> i chiamanti che non lo passano impacchettano IDENTICO a prima.
inline U64 pack_tt_data(int move, int score, int depth, int flag, int busy, int age, int pv = 0) {
    return ((U64)(move & 0x1FFFFF)) |
        ((U64)((score + 32768) & 0xFFFF) << 21) |
        ((U64)(depth & 0xFF) << 37) |
        ((U64)(flag & 0x3) << 45) |
        ((U64)(busy & 0xFF) << 47) |
        ((U64)(age & 0xFF) << 55) |
        ((U64)(pv & 0x1) << 63);
}

inline int unpack_move(U64 data) { return data & 0x1FFFFF; }
inline int unpack_score(U64 data) { return ((data >> 21) & 0xFFFF) - 32768; }
inline int unpack_depth(U64 data) { return (data >> 37) & 0xFF; }
inline int unpack_flag(U64 data) { return (data >> 45) & 0x3; }
inline int unpack_busy(U64 data) { return (data >> 47) & 0xFF; }
inline int unpack_age(U64 data) { return (data >> 55) & 0xFF; }
inline int unpack_pv(U64 data) { return (data >> 63) & 0x1; }

// Global TT
extern tt_entry* hash_table;
extern U64 hash_entries;
extern int current_age;

// 4-way set-associative TT on/off (UCI option "TT4Way"). Default off reproduces
// the original direct-mapped (1-way) table for a clean A/B. When on, each index
// maps to a bucket of 4 consecutive entries; probe scans the bucket, store picks
// an age-aware victim (prefer empty -> oldest -> shallowest).
extern bool g_tt_4way;

// External variables needed for compatibility functions
extern U64 hash_key;
extern U64 piece_keys[12][64];
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

// Initialize hash table
inline void init_hash_table(int mb) {
    U64 size = (U64)mb * 1024 * 1024;
    hash_entries = size / sizeof(tt_entry);

    // Free old table if exists
    if (hash_table) {
        delete[] hash_table;
    }

    hash_table = new tt_entry[hash_entries]();

    // Clear table
    for (U64 i = 0; i < hash_entries; i++) {
        hash_table[i].key = 0;
        hash_table[i].data = 0;
    }
}

// Clear hash table
inline void clear_hash_table() {
    for (U64 i = 0; i < hash_entries; i++) {
        hash_table[i].key = 0;
        hash_table[i].data = 0;
    }
    current_age = 0;
}

// Increment age (call at start of each search)
inline void new_search() {
    current_age = (current_age + 1) & 0xFF;
}

// ---- Bucket addressing (1-way vs 4-way) ------------------------------------
// Number of slots per bucket.
inline int tt_ways() { return g_tt_4way ? 4 : 1; }

// First slot index of the bucket for this key. For 4-way, there are
// (hash_entries/4) buckets, each 4 slots wide; hash_entries is always a multiple
// of 4 (= mb * 65536), so base + 3 stays in bounds.
inline U64 tt_base_index(U64 key) {
    if (g_tt_4way) {
        U64 buckets = hash_entries >> 2;
        if (buckets == 0) buckets = 1;
        return (key % buckets) << 2;
    }
    return key % hash_entries;
}

// Return the slot in this key's bucket that currently holds this position, or
// nullptr if the position is not present.
inline tt_entry* tt_find(U64 key) {
    U64 base = tt_base_index(key);
    int ways = tt_ways();
    for (int i = 0; i < ways; i++) {
        tt_entry* e = &hash_table[base + i];
        if ((e->key ^ e->data) == key) return e;
    }
    return nullptr;
}

// Pick the slot to overwrite when the position is not already present. Prefer an
// empty slot; otherwise the one with the lowest "value" = depth - 2*age-distance
// (so old and shallow entries are evicted first).
inline tt_entry* tt_victim(U64 key) {
    U64 base = tt_base_index(key);
    int ways = tt_ways();
    tt_entry* best = &hash_table[base];
    int best_val = 1 << 30;
    for (int i = 0; i < ways; i++) {
        tt_entry* e = &hash_table[base + i];
        if (e->key == 0 && e->data == 0) return e;            // empty: take it
        int rel_age = (current_age - unpack_age(e->data)) & 0xFF;
        int val = unpack_depth(e->data) - 2 * rel_age;
        if (val < best_val) { best_val = val; best = e; }
    }
    return best;
}

/*
 * Probe TT with ABDADA support
 *
 * Returns: true if valid entry found
 * Sets: tt_move, tt_score, tt_depth, tt_flag, is_busy
 */
inline bool probe_tt(U64 hash_key, int& tt_move, int& tt_score, int& tt_depth, int& tt_flag, bool& is_busy, bool& is_pv) {
    tt_entry* entry = tt_find(hash_key);

    if (!entry) {
        tt_move = 0;
        tt_score = 0;
        tt_depth = 0;
        tt_flag = hash_flag_alpha;
        is_busy = false;
        is_pv = false;
        return false;
    }

    // Valid entry - unpack
    U64 data = entry->data;
    tt_move = unpack_move(data);
    tt_score = unpack_score(data);
    tt_depth = unpack_depth(data);
    tt_flag = unpack_flag(data);
    is_busy = (unpack_busy(data) > 0);
    is_pv = (unpack_pv(data) != 0);

    return true;
}

// Overload retro-compatibile (chiamanti che non vogliono il flag ttPv).
inline bool probe_tt(U64 hash_key, int& tt_move, int& tt_score, int& tt_depth, int& tt_flag, bool& is_busy) {
    bool is_pv_dummy;
    return probe_tt(hash_key, tt_move, tt_score, tt_depth, tt_flag, is_busy, is_pv_dummy);
}

/*
 * Quick check if node is busy (for ABDADA skip decision)
 */
inline bool is_node_busy(U64 hash_key) {
    tt_entry* entry = tt_find(hash_key);
    if (!entry) return false;
    return (unpack_busy(entry->data) > 0);
}

/*
 * Mark node as busy (called when starting to search a node)
 * Only marks if entry doesn't exist or is from different position
 */
inline void mark_busy(U64 hash_key, int depth) {
    tt_entry* entry = tt_find(hash_key);

    // If same position exists, increment busy counter
    if (entry) {
        U64 old_data = entry->data;
        int old_busy = unpack_busy(old_data);
        if (old_busy < 255) {
            U64 new_data = pack_tt_data(
                unpack_move(old_data),
                unpack_score(old_data),
                unpack_depth(old_data),
                unpack_flag(old_data),
                old_busy + 1,
                unpack_age(old_data),
                unpack_pv(old_data)
            );
            entry->data = new_data;
            entry->key = hash_key ^ new_data;
        }
    }
    else {
        // New position - claim a victim slot in the bucket and mark it busy.
        entry = tt_victim(hash_key);
        U64 new_data = pack_tt_data(0, 0, 0, hash_flag_alpha, 1, current_age);
        entry->data = new_data;
        entry->key = hash_key ^ new_data;
    }
}

/*
 * Unmark node as busy (called when done searching a node)
 */
inline void unmark_busy(U64 hash_key) {
    tt_entry* entry = tt_find(hash_key);

    // Verify same position
    if (entry) {
        U64 old_data = entry->data;
        int old_busy = unpack_busy(old_data);
        if (old_busy > 0) {
            U64 new_data = pack_tt_data(
                unpack_move(old_data),
                unpack_score(old_data),
                unpack_depth(old_data),
                unpack_flag(old_data),
                old_busy - 1,
                unpack_age(old_data),
                unpack_pv(old_data)
            );
            entry->data = new_data;
            entry->key = hash_key ^ new_data;
        }
    }
}

/*
 * Store result in TT (and clear busy flag for this thread)
 *
 * Replacement scheme:
 * 1. Always replace if same position
 * 2. Replace if new depth >= old depth
 * 3. Replace if old entry is from different age
 */
inline void store_tt(U64 hash_key, int move, int score, int depth, int flag, int ply = 0, bool pv = false) {
    // Normalize mate scores to be relative to THIS node before storing
    // (value_to_tt). The probe side performs the inverse adjustment. Without
    // this, a "mate in N from the root" would be cached as if it were "mate in
    // N from here", giving wrong mate distances on TT hits.
    if (score > mate_score) score += ply;
    else if (score < -mate_score) score -= ply;

    int old_busy = 0;
    tt_entry* entry = tt_find(hash_key);     // same-position slot in the bucket?

    if (entry) {
        U64 old_data = entry->data;
        int old_depth = unpack_depth(old_data);
        int old_age = unpack_age(old_data);
        old_busy = unpack_busy(old_data);

        // Decrement busy counter
        if (old_busy > 0) old_busy--;

        // Don't replace deeper entries from same age unless exact score
        if (old_age == current_age && old_depth > depth && flag != hash_flag_exact) {
            // Just update busy counter (preserve the ttPv bit)
            U64 new_data = pack_tt_data(
                unpack_move(old_data),
                unpack_score(old_data),
                old_depth,
                unpack_flag(old_data),
                old_busy,
                old_age,
                unpack_pv(old_data)
            );
            entry->data = new_data;
            entry->key = hash_key ^ new_data;
            return;
        }
    }
    else {
        // Not present: evict an age-aware victim from the bucket.
        entry = tt_victim(hash_key);
    }

    // Store new entry (carry the ttPv flag; `pv` già include l'ex-PV letto al probe)
    U64 new_data = pack_tt_data(move, score, depth, flag, old_busy, current_age, pv ? 1 : 0);
    entry->data = new_data;
    entry->key = hash_key ^ new_data;
}

/*
 * Get TT move only (for move ordering)
 */
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
    bool tt_busy;

    if (!probe_tt(hash_key, tt_move, tt_score, tt_depth, tt_flag, tt_busy)) {
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
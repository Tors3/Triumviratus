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
#include "profile.h"

 // Hash flags
#define hash_flag_exact 0
#define hash_flag_alpha 1
#define hash_flag_beta 2
// hash_flag_none (5.1): BOUND_NONE per le entry "eval-only" (no score/move/depth validi).
// Vale 3 = valore libero del campo flag a 2 bit; i consumatori confrontano == exact/alpha/beta
// (0/1/2) -> flag=3 IGNORATO ovunque. Definito qui (serve a probe_tt, piu' sotto).
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
// 4.0: entry da 24 byte. key/data restano lockless (XOR-verification); `ext`
// porta la STATIC EVAL a 16 BIT PIENI (4.0-base: niente quantizzazione ±508/4cp
// della v1 a 8 bit, che flippava RFP/futility ai margini). ext e' protetta da un
// frammento di chiave (16 bit): una race/torn-write al peggio invalida l'eval
// (si ricalcola la forward), mai un valore sbagliato accettato come buono.
// Straddling delle cache-line: irrilevante per noi — misurato 2026-06-07 che il
// motore e' EVAL-bound, non TT-latency-bound (prefetch TT = 0%).
struct alignas(8) tt_entry {
    U64 key;           // hash_key XOR data (for lockless verification)
    U64 data;          // move (24) | score (16) | depth (8) | flag (2) | spare (8) | age (5) | pv (1)
    U64 ext;           // eval16+32768 [0..15] (0 = assente) | keyfrag16 [16..31] | spare 32
};

// Data packing/unpacking.
// Bit layout of `data` (64 bit): move[0..23] score[24..39] depth[40..47]
// flag[48..49] spare[50..57] age[58..62] pv[63].
// FIX P0.1 (2026-06-09): mossa a 24 bit (i bit 21/22/23 = double/ep/castling).
// P1.1/4.0 (2026-06-11): STATIC EVAL nella TT a 16 BIT, nel campo `ext`
// dell'entry da 24B (vedi sopra). Il motore e' eval-bound (58% del tempo-nodo =
// forward NNUE): un TT-hit che porta l'eval statica salva la forward.
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

// ext: eval16 (offset +32768, 0 = assente) + frammento di chiave a 16 bit.
inline U64 pack_ext(int eval, U64 hash_key) {
    if (eval == tt_eval_none || eval > 30000 || eval < -30000) return 0;
    return ((U64)((eval + 32768) & 0xFFFF)) | (((hash_key >> 48) & 0xFFFF) << 16);
}
inline int unpack_ext_eval(U64 ext, U64 hash_key) {
    if (ext == 0) return tt_eval_none;
    if (((ext >> 16) & 0xFFFF) != ((hash_key >> 48) & 0xFFFF)) return tt_eval_none;
    int v = (int)(ext & 0xFFFF) - 32768;
    return (v > 30000 || v < -30000) ? tt_eval_none : v;
}
// 5.1 EvalTTWrite (SF-style): l'entry eval-only memorizza l'UNADJUSTED (pre-rule50/scale,
// fifty-independent) via pack_ext normale; in lettura nn_finalize() lo ri-finalizza col fifty
// corrente -> esatto su QUALSIASI trasposizione (niente vincolo same-fifty = max cache-hit).

// Global TT
extern tt_entry* hash_table;
extern U64 hash_entries;
extern int current_age;

// 4-way set-associative TT on/off (UCI option "TT4Way"). Default off reproduces
// the original direct-mapped (1-way) table for a clean A/B. When on, each index
// maps to a bucket of 4 consecutive entries; probe scans the bucket, store picks
// an age-aware victim (prefer empty -> oldest -> shallowest).
extern bool g_tt_4way;

// 5.1: TT "two-level" (UCI "TTTwoLevel") — schema #1 nei paper (Maastricht): ogni
// indice = bucket di 2 slot, slot0 = DEPTH-PREFERRED (tieni le entry profonde),
// slot1 = ALWAYS-REPLACE (tieni la piu' RECENTE = recency per la ri-visitazione).
// Mutuamente esclusivo col 4way. Mira al gap ttrate (move-availability) misurato 25% vs 46% SF.
extern bool g_tt_twolevel;

// TTMove24 (UCI "TTMove24", default ON) — ablazione del FIX P0.1: quando OFF lo
// store tronca la mossa a 21 bit come la 3.7 (i flag double/ep/castling si perdono
// di nuovo). Definita in threads.cpp.
extern bool g_ttmove24;
extern bool g_tt_move_keep;   // TTMoveKeep: conserva la TT move sui fail-low senza mossa (SF)

// P1.10a (UCI "TTAgeRefresh", default ON) — un probe-hit rinfresca l'age
// dell'entry: le posizioni CALDE ma scritte in search vecchie non vengono piu'
// evictate per anzianita' (SF fa lo stesso). Definita in threads.cpp.
extern bool g_tt_age_refresh;

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
    hash_entries &= ~3ULL;   // multiplo di 4: il bucket TT4Way (base+3) resta in bounds

    // Free old table if exists
    if (hash_table) {
        delete[] hash_table;
    }

    hash_table = new tt_entry[hash_entries]();

    // Clear table
    for (U64 i = 0; i < hash_entries; i++) {
        hash_table[i].key = 0;
        hash_table[i].data = 0;
        hash_table[i].ext = 0;
    }
}

// Clear hash table
inline void clear_hash_table() {
    for (U64 i = 0; i < hash_entries; i++) {
        hash_table[i].key = 0;
        hash_table[i].data = 0;
        hash_table[i].ext = 0;
    }
    current_age = 0;
}

// Increment age (call at start of each search)
inline void new_search() {
    current_age = (current_age + 1) & 0x1F;   // age a 5 bit (vedi pack_tt_data)
}

// ---- Bucket addressing (1-way vs 4-way) ------------------------------------
// Number of slots per bucket.
inline int tt_ways() { return g_tt_twolevel ? 2 : (g_tt_4way ? 4 : 1); }

// First slot index of the bucket for this key. For 4-way, there are
// (hash_entries/4) buckets, each 4 slots wide; hash_entries is always a multiple
// of 4 (= mb * 65536), so base + 3 stays in bounds.
inline U64 tt_base_index(U64 key) {
    if (g_tt_twolevel) {
        U64 buckets = hash_entries >> 1;     // entries/2 bucket da 2 slot (entries multiplo di 4 -> base+1 in bounds)
        if (buckets == 0) buckets = 1;
        return (key % buckets) << 1;
    }
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
        int rel_age = (current_age - unpack_age(e->data)) & 0x1F;
        int val = unpack_depth(e->data) - 2 * rel_age;
        if (val < best_val) { best_val = val; best = e; }
    }
    return best;
}

/*
 * Probe TT.
 * Returns: true if valid entry found.
 * Sets: tt_move, tt_score, tt_depth, tt_flag, tt_eval (centipawn o tt_eval_none), is_pv.
 * (ABDADA busy machinery RIMOSSO 2026-06-11: mai letto dalla search; i suoi bit
 *  ospitano ora la static eval — vedi pack_tt_data.)
 */
inline bool probe_tt(U64 hash_key, int& tt_move, int& tt_score, int& tt_depth, int& tt_flag, int& tt_eval, bool& is_pv) {
    PROF_GUARD(prof_tt);
    tt_entry* entry = tt_find(hash_key);

    if (!entry) {
        tt_move = 0;
        tt_score = 0;
        tt_depth = 0;
        tt_flag = hash_flag_alpha;
        tt_eval = tt_eval_none;
        is_pv = false;
        return false;
    }

    // Valid entry - unpack
    U64 data = entry->data;
    tt_move = unpack_move(data);
    tt_score = unpack_score(data);
    tt_depth = unpack_depth(data);
    tt_flag = unpack_flag(data);
    tt_eval = unpack_ext_eval(entry->ext, hash_key);   // eval16 (4.0), keyfrag-validata
    is_pv = (unpack_pv(data) != 0);

    // P1.10a age-refresh: l'entry e' CALDA (appena richiesta) -> portala all'age
    // corrente cosi' tt_victim non la preferisce per anzianita'. Store benigno
    // (lockless XOR-key ricalcolata; una race al peggio perde il refresh).
    if (g_tt_age_refresh && unpack_age(data) != current_age) {
        U64 new_data = (data & ~(0x1FULL << 58)) | ((U64)(current_age & 0x1F) << 58);
        entry->data = new_data;
        entry->key  = hash_key ^ new_data;
    }

    // 5.1 EvalTTWrite: l'entry eval-only (flag_none) fornisce SOLO tt_eval per saltare la
    // forward NNUE -> NON e' un vero TT-hit. Ritorna false cosi' IIR/IID/riduzioni gated su
    // tt_hit restano invariate (era la 2a meta' del tree-bloat). tt_eval/tt_flag(none) restano
    // popolati per il rescale lossless ai siti di lettura. Dormiente con EvalTTWrite OFF.
    if (tt_flag == hash_flag_none) return false;

    return true;
}

// Overload retro-compatibile (chiamanti che non vogliono eval/ttPv).
inline bool probe_tt(U64 hash_key, int& tt_move, int& tt_score, int& tt_depth, int& tt_flag) {
    int eval_dummy; bool pv_dummy;
    return probe_tt(hash_key, tt_move, tt_score, tt_depth, tt_flag, eval_dummy, pv_dummy);
}

/*
 * Store result in TT (and clear busy flag for this thread)
 *
 * Replacement scheme:
 * 1. Always replace if same position
 * 2. Replace if new depth >= old depth
 * 3. Replace if old entry is from different age
 */
inline void store_tt(U64 hash_key, int move, int score, int depth, int flag, int ply = 0, bool pv = false, int eval = tt_eval_none) {
    PROF_GUARD(prof_tt);
    // Ablazione P0.1 (TTMove24 off): emula il troncamento 21-bit della 3.7.
    if (!g_ttmove24) move &= 0x1FFFFF;

    // Normalize mate scores to be relative to THIS node before storing
    // (value_to_tt). The probe side performs the inverse adjustment. Without
    // this, a "mate in N from the root" would be cached as if it were "mate in
    // N from here", giving wrong mate distances on TT hits.
    if (score > mate_score) score += ply;
    else if (score < -mate_score) score -= ply;

    U64 new_ext = pack_ext(eval, hash_key);
    tt_entry* entry = tt_find(hash_key);     // same-position slot in the bucket?

    if (entry) {
        U64 old_data = entry->data;
        int old_depth = unpack_depth(old_data);
        int old_age = unpack_age(old_data);

        // SF-style move preservation (toggle TTMoveKeep, default off = byte-identico):
        // se questo store NON porta una mossa (fail-low con tutti i quiet potati ->
        // best_move=0), conserva la hash move gia' presente per QUESTA posizione invece
        // di azzerarla. Alza la ttrate ai cut-node (+7.7pt @suite) con albero +5%.
        if (g_tt_move_keep && move == 0) move = unpack_move(old_data);

        // P1.1: se questo store non porta un'eval fresca (es. nodo in scacco,
        // lazy-eval) ma l'entry della STESSA posizione ne aveva una, conservala.
        if (new_ext == 0 && unpack_ext_eval(entry->ext, hash_key) != tt_eval_none)
            new_ext = entry->ext;

        // Don't replace deeper entries from same age unless exact score
        if (old_age == current_age && old_depth > depth && flag != hash_flag_exact) {
            // Conserva l'entry piu' profonda; aggiorna solo l'eval se mancava.
            if (new_ext != entry->ext) entry->ext = new_ext;
            return;
        }
    }
    else if (g_tt_twolevel) {
        // Two-level: slot0 DEPTH-PREFERRED, slot1 ALWAYS-REPLACE. Scrivi in slot0 se
        // e' vuoto / di un'eta' vecchia (stale) / il nuovo e' >= profondo; altrimenti
        // slot1 (conserva la entry profonda in slot0, la recency in slot1).
        U64 base = tt_base_index(hash_key);
        tt_entry* s0 = &hash_table[base];
        bool s0_empty = (s0->key == 0 && s0->data == 0);
        if (s0_empty || unpack_age(s0->data) != current_age || depth >= unpack_depth(s0->data))
            entry = s0;
        else
            entry = &hash_table[base + 1];
    }
    else {
        // Not present: evict an age-aware victim from the bucket.
        entry = tt_victim(hash_key);
    }

    // Store new entry (carry the ttPv flag; `pv` già include l'ex-PV letto al probe)
    U64 new_data = pack_tt_data(move, score, depth, flag, current_age, pv ? 1 : 0);
    entry->data = new_data;
    entry->key = hash_key ^ new_data;
    entry->ext = new_ext;
}

extern bool g_eval_tt_write;   // 5.1: cache static eval su MISS (SF search.cpp:830) -> NPS

// Cache-only dello static eval su un nodo MISS (non ancora cercato), stile SF (:830).
// 5.1: memorizza l'UNADJUSTED (pre-rule50/scale, fifty-independent) -> la rivisita lo
// ri-finalizza con nn_finalize(corrente fifty), esatto su ogni trasposizione (max hit, no bloat).
inline void tt_cache_eval(U64 hash_key, int unadjusted) {
    U64 new_ext = pack_ext(unadjusted, hash_key);
    if (new_ext == 0) return;
    tt_entry* entry = tt_find(hash_key);
    if (entry) {
        // stessa posizione: aggiorna l'eval SOLO se gia' eval-only (stesso formato smorzato);
        // un'entry REALE ha l'eval de-smorzata -> NON toccarla (corruzione).
        if (unpack_flag(entry->data) == hash_flag_none) entry->ext = new_ext;
        return;
    }
    // CHIAVE (fix pollution, come SF :830 ttWriter): scrivi nello slot-VICTIM NATURALE della
    // posizione — lo STESSO che userebbe lo store reale a fine-nodo (replicando la victim-selection
    // di store_tt) — NON in uno slot vuoto a caso. Cosi' l'eval pre-riempie l'entry naturale e lo
    // store reale la rileva (tt_find) e la aggiorna IN PLACE: net occupancy ZERO = niente entry
    // fantasma = niente bloat. (La versione 'solo-slot-vuoti' creava 2 entry -> +50%% nodi.)
    if (g_tt_twolevel) {
        U64 base = tt_base_index(hash_key);
        tt_entry* s0 = &hash_table[base];
        bool s0_empty = (s0->key == 0 && s0->data == 0);
        entry = (s0_empty || unpack_age(s0->data) != current_age || 0 >= unpack_depth(s0->data))
                ? s0 : &hash_table[base + 1];
    } else {
        entry = tt_victim(hash_key);     // 4-way / 1-way: identico victim di store_tt
    }
    U64 d = pack_tt_data(0, 0, 0, hash_flag_none, current_age, 0);
    entry->data = d; entry->key = hash_key ^ d; entry->ext = new_ext;
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
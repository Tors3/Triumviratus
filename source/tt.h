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
#define tt_eval_none 32001   // (ripetuto piu' sotto con lo stesso testo: serve gia' qui a TT16)

// ===== TT16 (NPS 25/09/2026) ==================================================
// Entry da 16 byte, bucket da 4 entry = 64 byte = UNA linea di cache.
// Prima (vecchia TT, tolta il 25/09/2026 dopo lo SPRT: TT16 +6,30 ± 5,06 su 5.365 partite;
// sorgente in _backup/Triumviratus_7.1_src_2026-09-25_pre_cleanup/tt.h): entry da 24 byte in bucket da 2, cioe' 48 byte, e con
// l'indice a modulo META' dei bucket attraversava due linee: due miss per probe. Il
// profilo xperf del 25/09 (campioni su LLCMisses) dava alla TT ~4,6% dei miss del
// motore contro ~1,2% di SF, che usa cluster da 32 byte allineati.
//   kw   = ((chiave & 0xFFFFFFFFFFFF) << 16 | eval16) XOR data
//   data = mossa | score | depth | flag | age | pv   (layout invariato, pack_tt_data)
// Lo XOR lega le due parole come prima (lockless): una scrittura concorrente spezzata
// fa fallire la verifica della chiave, e l'eval ci sta DENTRO, quindi e' protetta
// dallo stesso controllo (prima aveva un suo frammento di chiave a 16 bit).
// La verifica usa i 48 bit BASSI della chiave; l'indice (tt_base_index, mulhi) usa
// quelli ALTI: le due cose restano quasi indipendenti.
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

// 5.1 EvalTTWrite (SF-style): l'entry eval-only memorizza l'UNADJUSTED (pre-rule50/scale,
// fifty-independent) via pack_ext normale; in lettura nn_finalize() lo ri-finalizza col fifty
// corrente -> esatto su QUALSIASI trasposizione (niente vincolo same-fifty = max cache-hit).

// Global TT
extern tt_entry* hash_table;
extern U64 hash_entries;
// Vero quando hash_entries e' una potenza di due: allora l'indice di bucket si
// ricava con un AND invece che con un `%`, dando lo STESSO indice.
// ⚠️ IN PRATICA NON SI ATTIVA MAI, e il commento precedente diceva il contrario.
// hash_entries = byte / sizeof(tt_entry) con sizeof(tt_entry) = 24: 24 non divide
// una potenza di due in una potenza di due, quindi a NESSUNA taglia realistica il
// conto torna. A 256 MB le entry sono 11.184.808. Verificato su 16, 64, 128, 256,
// 512 e 1024 MB: mai. Il ramo resta perche' e' corretto e si accenderebbe da solo
// se un giorno la entry diventasse di 16 o 32 byte, ma NON crederci come
// ottimizzazione viva: oggi si passa sempre dal modulo.
// Il tentativo di togliere quella divisione con un reciproco esatto e' documentato
// piu' sotto, sopra tt_base_index: misurato NEUTRO e rimosso.
extern bool g_tt_pow2;
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
extern int g_tt_keep_margin;   // TTKeepMargin (studio finali 26/09): vedi store_tt
extern bool g_tt_move_keep;   // TTMoveKeep: conserva la TT move sui fail-low senza mossa (SF)
extern bool g_tt_secondary_age;   // TTSecondaryAge (R-01): decisive non-EXACT depth>=5 invecchiano piu' in fretta nel replacement

// P1.10a (UCI "TTAgeRefresh", default ON) — un probe-hit rinfresca l'age
// dell'entry: le posizioni CALDE ma scritte in search vecchie non vengono piu'
// evictate per anzianita' (SF fa lo stesso). Definita in threads.cpp.
extern bool g_tt_age_refresh;

// LargePages (UCI, default ON) — alloca la TT su large pages 2MB (VirtualAlloc
// MEM_LARGE_PAGES, come i pesi NNUE). Toggle per l'A/B NPS pulito sullo STESSO
// binario: OFF = new[] (heap normale = baseline pre-modifica). Definita in threads.cpp.
extern bool g_large_pages;

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

// Large-page allocator (SF, nnue/memory.cpp). Forward-declared here to avoid
// pulling windows.h into tt.h (incluso da 5 TU). usize == std::size_t (misc.h).
namespace Triumviratus {
void* aligned_large_pages_alloc(std::size_t size);
void  aligned_large_pages_free(void* mem);
bool  has_large_pages();
}

// TT16: la tabella e' un array di tt_bucket (alignas(64)): l'operatore new allineato del
// C++17 garantisce l'allineamento anche sul ripiego heap. hash_table resta un tt_entry*.
inline void init_hash_table(int mb) {
    static bool tt_on_large_pages = false;

    U64 size    = (U64)mb * 1024 * 1024;
    U64 buckets = size / sizeof(tt_bucket);
    if (buckets == 0) buckets = 1;

    if (hash_table) {
        if (tt_on_large_pages) Triumviratus::aligned_large_pages_free(hash_table);
        else                   delete[] reinterpret_cast<tt_bucket*>(hash_table);
        hash_table = nullptr;
    }

    const U64 bytes = buckets * sizeof(tt_bucket);
    tt_bucket* tab = nullptr;
    if (g_large_pages) {
        tab = static_cast<tt_bucket*>(Triumviratus::aligned_large_pages_alloc(bytes));
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
    g_tt_pow2    = false;   // non usato da TT16 (indice via mulhi)

    const int actual_mb = (int)(buckets * sizeof(tt_bucket) / (1024 * 1024));
    const char* lp = !g_large_pages          ? "off (disabled)"
                   : tt_on_large_pages       ? "ON"
                                             : "off (unavailable)";
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
    for (U64 i = 0; i < n; i++)
        if (hash_table[i].data != 0) used++;
    return (int)((U64)used * 1000 / n);
}

// Increment age (call at start of each search)
inline void new_search() {
    current_age = (current_age + 1) & 0x1F;   // age a 5 bit (vedi pack_tt_data)
}

// TT prefetch (toggle "TTPrefetch"). ⚠️ COMMENTO CORRETTO 2026-07-26: diceva
// "default OFF" e "vale ri-misurare", ma era gia' stato **BAKATO ON** (vedi
// threads.cpp: `g_tt_prefetch = true`, +1.88% NPS misurato, riconfermato +2.75%
// nell'audit del 25/07). Il commento stantio ha tenuto chiuso un fronte NPS che
// era gia' vinto: un auditor l'ha letto e ha riportato il prefetch come non
// ancora provato. Se un commento contraddice il default vivo, vince il codice.
// Ritestato 2026-07-13 (Reckless #1085): il tentativo 2026-06-07 era
// NPS-neutral, ma nel frattempo era stato bakato TTTwoLevel (bucket a 2 slot),
// che cambia il pattern di accesso -> da li' il guadagno.
// Prefetch della PRIMA cacheline del bucket subito dopo il calcolo della chiave
// figlia in td_make_move (post-legalita': la variante pre-legalita' era -2.5%).
#if defined(_MSC_VER)
    #include <intrin.h>
    #define TT_PREFETCH(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)
#else
    #define TT_PREFETCH(addr) __builtin_prefetch(addr)
#endif

// ===== TT16: indirizzamento, probe, store =========================================
// Indice del bucket con la moltiplicazione alta (come SF): niente divisione a 64 bit.
// Con la entry da 24 byte il modulo era inevitabile; qui il numero di bucket e' libero
// e mulhi(key, buckets) e' uniforme su [0, buckets).
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

// Slot del bucket che contiene questa posizione, o nullptr.
inline tt_entry* tt_find(U64 key) {
    tt_entry* b = &hash_table[tt_base_index(key)];
    const U64 tag = tt_tag(key);
    for (int i = 0; i < TT_WAYS; i++)
        if (((b[i].kw ^ b[i].data) >> 16) == tag && (b[i].kw | b[i].data)) return &b[i];
    return nullptr;
}

// Vittima: slot vuoto, altrimenti il valore piu' basso di depth - 2*distanza d'eta'
// (stessa regola della vecchia tt_victim, ora su 4 vie dentro una sola linea).
inline tt_entry* tt_victim(U64 key) {
    tt_entry* b = &hash_table[tt_base_index(key)];
    tt_entry* best = &b[0];
    int best_val = 1 << 30;
    for (int i = 0; i < TT_WAYS; i++) {
        tt_entry* e = &b[i];
        if (e->kw == 0 && e->data == 0) return e;
        int rel_age = (current_age - unpack_age(e->data)) & 0x1F;
        int depth = unpack_depth(e->data);
        if (g_tt_secondary_age && depth >= 5 && unpack_flag(e->data) != hash_flag_exact) {
            int sc = unpack_score(e->data);
            if (sc > 30000 || sc < -30000) depth -= 8;
        }
        int val = depth - 2 * rel_age;
        if (val < best_val) { best_val = val; best = e; }
    }
    return best;
}

inline bool probe_tt(U64 hash_key, int& tt_move, int& tt_score, int& tt_depth, int& tt_flag, int& tt_eval, bool& is_pv) {
    PROF_GUARD(prof_tt);
    tt_entry* b = &hash_table[tt_base_index(hash_key)];
    const U64 tag = tt_tag(hash_key);
    for (int i = 0; i < TT_WAYS; i++) {
        tt_entry* entry = &b[i];
        // Un solo snapshot delle due parole: la verifica e l'unpack leggono gli STESSI
        // valori (niente torn read fra verifica e uso, cfr. BUG FIX 2026-07-16).
        const U64 data = entry->data;
        const U64 w    = entry->kw ^ data;
        if ((w >> 16) != tag || (entry->kw | data) == 0) continue;
        tt_move  = unpack_move(data);
        tt_score = unpack_score(data);
        tt_depth = unpack_depth(data);
        tt_flag  = unpack_flag(data);
        tt_eval  = tt_unpack_eval16(w);
        is_pv    = (unpack_pv(data) != 0);
        if (g_tt_age_refresh && unpack_age(data) != current_age) {
            U64 new_data = (data & ~(0x1FULL << 58)) | ((U64)(current_age & 0x1F) << 58);
            entry->data = new_data;
            entry->kw   = w ^ new_data;
        }
        if (tt_flag == hash_flag_none) return false;   // entry eval-only (EvalTTWrite)
        return true;
    }
    tt_move = 0; tt_score = 0; tt_depth = 0;
    tt_flag = hash_flag_alpha; tt_eval = tt_eval_none; is_pv = false;
    return false;
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
    tt_entry* entry = tt_find(hash_key);
    if (entry) {
        const U64 old_data = entry->data;
        const U64 old_w    = entry->kw ^ old_data;
        if (g_tt_move_keep && move == 0) move = unpack_move(old_data);
        if (ev16 == 0) ev16 = (int)(old_w & 0xFFFF);   // conserva l'eval se lo store non ne porta
        // TTKeepMargin (studio finali 26/09/2026, docs/audit_7.1/H_FINALI.md). Nei finali, a profondita' >= 12,
        // l'entry c'e' quanto in SF (74%) e e' profonda abbastanza piu' spesso (32% contro 26%), ma il suo
        // bound serve alla finestra meno spesso (53% contro 66%): teniamo l'entry PIU' PROFONDA anche se il
        // suo bound e' vecchio. SF sovrascrive se  depth + 2*pv > vecchia - 4  (tt.cpp, TTWriter::write):
        // preferisce l'informazione fresca fino a 3 ply piu' corta. Con margine m > 0 si tiene la vecchia
        // solo se e' piu' profonda di oltre m ply (+2 sui nodi PV). 0 = regola storica, byte-identico.
        const int keep_margin = g_tt_keep_margin > 0 ? g_tt_keep_margin + (pv ? 2 : 0) : 0;
        if (unpack_age(old_data) == current_age && unpack_depth(old_data) > depth + keep_margin && flag != hash_flag_exact) {
            // Conserva l'entry piu' profonda; aggiorna solo l'eval se mancava.
            const U64 w = (old_w & ~0xFFFFULL) | (U64)ev16;
            if (w != old_w) entry->kw = w ^ old_data;
            return;
        }
    } else {
        entry = tt_victim(hash_key);
    }
    const U64 new_data = pack_tt_data(move, score, depth, flag, current_age, pv ? 1 : 0);
    const U64 w = (tt_tag(hash_key) << 16) | (U64)ev16;
    entry->data = new_data;
    entry->kw   = w ^ new_data;
}

extern bool g_eval_tt_write;   // 5.1: cache static eval su MISS (SF search.cpp:830) -> NPS

// Cache-only dello static eval su un MISS (EvalTTWrite, default OFF): entry flag_none
// nello slot vittima naturale, cosi' lo store reale la ritrova e la aggiorna in place.
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
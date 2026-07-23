/*
 * syzygy.cpp - Syzygy tablebase probing for Triumviratus, via Fathom.
 *
 * See syzygy.h for the conventions bridge. The two public probes are:
 *   - syzygy_probe_wdl(): in-search, returns the game-theoretic value of a node
 *     (side-to-move relative) so the search never misjudges a covered endgame.
 *   - syzygy_probe_root(): at the root, returns the DTZ-optimal move so the
 *     engine actually CONVERTS wins / HOLDS draws with perfect technique.
 *
 * Fathom is dropped into ./fathom/ (tbprobe.c/.h + tbconfig.h + stdendian.h);
 * tbprobe.c is added to the build. tbprobe.h carries its own extern "C" guard.
 */
// Fathom FIRST, before the engine headers: defs.h does `#define U64 ...`, and
// pulling Fathom in ahead of that keeps the macro from touching its headers.
#if defined(_MSC_VER)
#include <intrin.h>           // _byteswap_uint64 (MSVC)
#endif
#include "fathom/tbprobe.h"   // tb_init/tb_free/tb_probe_wdl/tb_probe_root, TB_*

#include "syzygy.h"
#include "threads.h"     // ThreadData (full definition)
#include "movegen.h"     // generate_moves, get_move_*  (operate on global board)
#include "search.h"      // mate_score, max_ply

// TB win/loss magnitude. Must sit strictly BELOW mate scores (mate_score =
// 30000, mates live in (30000, 31000]) and far ABOVE any normal eval, so a TB
// result dominates eval but never masquerades as / collides with a mate.
//   TB win at ply p  =  TB_VALUE_WIN - p   in [29873, 29936] for p in [0,63]
static const int TB_VALUE_WIN = mate_score - max_ply;   // 30000 - 64 = 29936

// Have we ever loaded tables? Used so syzygy_free() is harmless when unused and
// so a re-init can release the previous set first.
static bool g_tb_initialized = false;

static inline U64 flip_vertical(U64 b) {
#if defined(_MSC_VER)
    return _byteswap_uint64(b);
#else
    return __builtin_bswap64(b);
#endif
}

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------
bool syzygy_init(const char* path)
{
    // Treat empty / "<empty>" (what some GUIs send to clear a string option) as
    // "disable probing".
    bool empty = (path == nullptr) || (path[0] == '\0') ||
                 (strcmp(path, "<empty>") == 0);

    if (g_tb_initialized) { tb_free(); g_tb_initialized = false; }

    if (empty) return false;

    bool ok = tb_init(path);
    g_tb_initialized = ok && (TB_LARGEST > 0);
    return g_tb_initialized;
}

void syzygy_free()
{
    if (g_tb_initialized) { tb_free(); g_tb_initialized = false; }
}

unsigned syzygy_max_pieces()
{
    return g_tb_initialized ? TB_LARGEST : 0u;
}

static bool g_ignore_50_move_rule = false;

void syzygy_set_ignore_50_move_rule(bool v)
{
    g_ignore_50_move_rule = v;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// Build the eight Fathom (a1=0) bitboards from the engine's (a8=0) board.
// NB: parameters are named wh/bl (not w/b) to avoid shadowing the piece enums
// b (black bishop = 8) and w; a `b` param would turn bb[b] into a huge OOB read.
static void to_fathom(const U64 bb[12], const U64 occ[3],
                      U64& wh, U64& bl, U64& kings, U64& queens, U64& rooks,
                      U64& bishops, U64& knights, U64& pawns)
{
    wh      = flip_vertical(occ[white]);
    bl      = flip_vertical(occ[black]);
    kings   = flip_vertical(bb[K] | bb[k]);
    queens  = flip_vertical(bb[Q] | bb[q]);
    rooks   = flip_vertical(bb[R] | bb[r]);
    bishops = flip_vertical(bb[B] | bb[b]);
    knights = flip_vertical(bb[N] | bb[n]);
    pawns   = flip_vertical(bb[P] | bb[p]);
}

static inline unsigned fathom_ep(int enpassant_sq)
{
    // Fathom uses 0 == "no en passant"; a real ep target is never on rank 1
    // (a1=0 in Fathom space), so 0 is unambiguous.
    return (enpassant_sq == no_sq) ? 0u : (unsigned)(enpassant_sq ^ 56);
}

// WDL (0..4, side-to-move relative) -> engine score. CURSED_WIN / BLESSED_LOSS
// are 50-move-rule draws: scored 0 so the engine respects the 50-move rule
// (real conversion is the root DTZ probe's job).
static inline int wdl_to_score(unsigned wdl, int ply)
{
    switch (wdl) {
        case TB_WIN:  return  TB_VALUE_WIN - ply;
        case TB_LOSS: return -(TB_VALUE_WIN - ply);
        case TB_CURSED_WIN:
            // 50-move-rule draw by default; Syzygy50MoveRule=true -> real win.
            return g_ignore_50_move_rule ? (TB_VALUE_WIN - ply) : 0;
        case TB_BLESSED_LOSS:
            return g_ignore_50_move_rule ? -(TB_VALUE_WIN - ply) : 0;
        default:      return 0;   // TB_DRAW
    }
}

// Fathom promotion code -> does it match this engine move's promoted piece?
// Engine promoted field is 0 (none) or a piece enum (white N..Q = 1..4,
// black n..q = 7..10).
static inline bool promo_matches(int eng_promoted, unsigned fathom_promo)
{
    unsigned want;
    switch (eng_promoted) {
        case 0:          want = TB_PROMOTES_NONE;   break;
        case Q: case q:  want = TB_PROMOTES_QUEEN;  break;
        case R: case r:  want = TB_PROMOTES_ROOK;   break;
        case B: case b:  want = TB_PROMOTES_BISHOP; break;
        case N: case n:  want = TB_PROMOTES_KNIGHT; break;
        default:         want = TB_PROMOTES_NONE;   break;
    }
    return want == fathom_promo;
}

// ---------------------------------------------------------------------------
// in-search WDL probe (ThreadData node)
// ---------------------------------------------------------------------------
bool syzygy_probe_wdl(const ThreadData& td, int ply, int& score)
{
    if (!g_tb_initialized) return false;

    U64 w, b, kings, queens, rooks, bishops, knights, pawns;
    to_fathom(td.bitboards, td.occupancies,
              w, b, kings, queens, rooks, bishops, knights, pawns);

    // castling==0: the caller already gated on td.castle==0 (WDL tables assume
    // no castling rights), so we pass 0 here unconditionally.
    unsigned wdl = tb_probe_wdl(w, b, kings, queens, rooks, bishops, knights,
                                pawns, (unsigned)td.fifty, 0u,
                                fathom_ep(td.enpassant),
                                /*turn=*/ td.side == white);
    if (wdl == TB_RESULT_FAILED) return false;

    score = wdl_to_score(wdl, ply);
    return true;
}

// ---------------------------------------------------------------------------
// root DTZ probe (GLOBAL board)
// ---------------------------------------------------------------------------
bool syzygy_probe_root(int& best_move, int& score)
{
    if (!g_tb_initialized) return false;
    if (castle != 0) return false;                                 // need no castling rights
    if ((unsigned)count_bits(occupancies[both]) > TB_LARGEST) return false;

    U64 w, b, kings, queens, rooks, bishops, knights, pawns;
    to_fathom(bitboards, occupancies,
              w, b, kings, queens, rooks, bishops, knights, pawns);

    unsigned res = tb_probe_root(w, b, kings, queens, rooks, bishops, knights,
                                 pawns, (unsigned)fifty, 0u,
                                 fathom_ep(enpassant),
                                 /*turn=*/ side == white,
                                 /*results=*/ NULL);
    if (res == TB_RESULT_FAILED) return false;   // includes "DTZ tables missing"

    unsigned wdl       = TB_GET_WDL(res);
    int      eng_from  = (int)TB_GET_FROM(res) ^ 56;
    int      eng_to    = (int)TB_GET_TO(res)   ^ 56;
    unsigned promo     = TB_GET_PROMOTES(res);

    // Re-derive the engine's encoded move by matching source/target/promotion
    // against the freshly generated move list. We NEVER play an unmatched move.
    moves move_list;
    generate_moves(&move_list);
    int matched = 0;
    for (int i = 0; i < move_list.count; i++) {
        int m = move_list.moves[i];
        if (get_move_source(m) != eng_from) continue;
        if (get_move_target(m) != eng_to)   continue;
        if (!promo_matches(get_move_promoted(m), promo)) continue;
        matched = m;
        break;
    }
    if (!matched) return false;

    best_move = matched;
    score = (wdl == TB_WIN) ?  TB_VALUE_WIN
          : (wdl == TB_LOSS) ? -TB_VALUE_WIN
          : 0;
    return true;
}

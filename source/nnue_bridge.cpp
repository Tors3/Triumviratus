// Triumviratus 5.0 NNUE bridge — routes the engine's eval through the VENDORED
// Stockfish-master SFNNv13 network code (nnue/): ThreatFeatureSet=FullThreats
// + PSQFeatureSet=HalfKAv2_hm, L1=1024, 8 LayerStacks. This is Stockfish's own
// ultramodern NNUE machinery adapted into our isolated Triumviratus:: namespace.
//
// M2 drive model (full refresh): each search thread owns an opaque handle holding
// an SF Position + a per-thread AccumulatorStack + AccumulatorCaches. The handle
// tracks only side-to-move and the fifty-move clock across make/undo; the board
// itself is rebuilt from the engine's bitboards once per evaluate() in nn_pos_eval
// (vflip + piece remap -> Position::set_pieces), then Network::evaluate runs a
// full refresh and we apply Stockfish's cp scaling inline. The bullet own-lineage
// net and the legacy SFNNv10 path were removed here in M2; M3 adds the incremental
// AccumulatorStack chain (DirtyPiece/DirtyThreats) for NPS.
//
// Square conventions: the engine uses a8=0..h1=63 (BBC); SF uses a1=0..h8=63. A
// per-rank byteswap (vflip) of an engine bitboard yields the SF-layout bitboard.

#include "nnue_bridge.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#if defined(_MSC_VER)
    #include <intrin.h>   // _byteswap_uint64, _BitScanForward64
#endif

#include "nnue/bitboard.h"
#include "nnue/memory.h"           // LargePagePtr / make_unique_large_page (pesi rete su large pages)
#include "nnue/position.h"
#include "nnue/types.h"
#include "nnue/evaluate.h"         // EvalFileDefaultName (nome del net embeddato)
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/nnue/nnue_misc.h"   // Eval::NNUE::EvalFile

#ifdef TRIUMV_EMBED_RESOURCE
// Windows: la rete di default sta in una risorsa RCDATA dell'exe (incbin non
// funziona con _MSC_VER, clang-cl incluso). Qui si definiscono i puntatori che
// nnue/nnue/network.cpp dichiara extern e si risolvono al primo load.
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
const unsigned char* gEmbeddedNNUEData = nullptr;
unsigned int         gEmbeddedNNUESize = 0;
static void embed_init_from_resource() {
    static bool done = false;
    if (done) return;
    done = true;
    // MAKEINTRESOURCEA(10) = RT_RCDATA in versione ANSI (RT_RCDATA segue UNICODE)
    HRSRC r = FindResourceA(nullptr, "NNUE_DEFAULT", MAKEINTRESOURCEA(10));
    if (!r) return;
    HGLOBAL h = LoadResource(nullptr, r);
    if (!h) return;
    gEmbeddedNNUEData = static_cast<const unsigned char*>(LockResource(h));
    gEmbeddedNNUESize = static_cast<unsigned int>(SizeofResource(nullptr, r));
}
#endif

using namespace Triumviratus;
using Triumviratus::Eval::NNUE::Network;
using Triumviratus::Eval::NNUE::AccumulatorStack;
using Triumviratus::Eval::NNUE::AccumulatorCaches;

// ---------------------------------------------------------------------------
// The single immutable network, loaded once at startup (nn_load_net) and
// optionally swapped at runtime (nn_reload_big). ~90 MB of weights, walked on
// EVERY eval -> the TLB-hottest data in the engine, so it lives on large pages
// (aligned_large_pages_alloc: Windows VirtualAlloc MEM_LARGE_PAGES with silent
// fallback to regular pages on failure/no-privilege; Linux 2MB-aligned +
// madvise(MADV_HUGEPAGE) = THP). AccumulatorCaches are built per handle FROM
// this net, so it must be loaded before any nn_pos_create().
// Immortalized (leak-at-exit by design): the large-page deleter may exit() on
// VirtualFree failure and the Linux path locks a mutex whose cross-TU static
// destruction order is unspecified -- never run it during static destruction.
// Reload-time frees (search stopped) are unaffected.
// ---------------------------------------------------------------------------
static LargePagePtr<Network>& g_net = *new LargePagePtr<Network>();

// Generation counter, bumped on every (re)load of g_net. AccumulatorCaches
// (finny) are seeded from the NET'S BIASES: caches built from an older net
// silently corrupt every refresh after an EvalFile reload (bug found
// 2026-07-14 — a semantically-identical permuted net benched differently via
// setoption but identically as startup default). Every cache holder compares
// its own generation and rebuilds when stale.
static std::atomic<int> g_net_gen{1};

// Vertical flip of a bitboard (engine a8=0 <-> SF a1=0 == per-rank byteswap).
static inline std::uint64_t vflip(std::uint64_t b) {
#if defined(_MSC_VER)
    return _byteswap_uint64(b);
#else
    return __builtin_bswap64(b);
#endif
}

static inline int ctz64(std::uint64_t b) {
#if defined(_MSC_VER)
    unsigned long s;
    _BitScanForward64(&s, b);
    return int(s);
#else
    return __builtin_ctzll(b);
#endif
}

// engine bb[] index -> SF piece code.
static const int sfc[12] = {W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                            B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING};

// Eval output scale (percent, default 100 = x1.0). The SFNNv13 cp formula lands on
// a DIFFERENT scale than the SFNNv10 eval-wrapper the engine's search margins were
// SPSA-tuned for (pawn ~56 vs ~332) -> the pruning thresholds are mis-sized. This
// multiplier re-aligns the eval with the existing margins; sweep it at fixed depth.
static int g_eval_scale_pct = 56;
// 5.1 EvalTTWrite: ultimo valore UNADJUSTED (pre-rule50, pre-EvalScale) calcolato da nn_scale
// su QUESTO thread = lo "unadjustedStaticEval" di SF, fifty-independent -> si cacha questo e si
// ri-finalizza col fifty corrente (hit su TUTTE le trasposizioni, sempre esatto).
static thread_local int g_last_unadjusted = 0;

// Stockfish's eval cp scaling (evaluate.cpp), inlined here with optimism=0 (the
// engine's static eval is unbiased; optimism is a search-only blend in SF). psqt
// and positional are the stm-relative raw NNUE outputs of Network::evaluate.
// material is computed from piece counts (set_pieces zeroes StateInfo, so we do
// NOT use pos.non_pawn_material()). Returns stm-relative internal-unit value.
// ⭐ 5.1 EVAL: optimism (SF evaluate.cpp:55,59), default OFF (g_optimism resta 0 -> termine nullo
// -> byte-identico). g_optimism[stm] e' aggiornato dalla root (search) in unita'-interne SF; il
// nostro static-eval lo ometteva (=0). Riacceso, ricalibra l'eval come fa SF (contempt dinamico).
int g_eval_optimism = 1;  // [5.1 BAKE] ON di default (spsa_struct lo ha tenuto a strength=89, non spento)
// F-019 (2026-07-03): atomic relaxed-di-fatto — il main scrive a fine iterazione, gli
// helper leggono nella eval; su int non-atomici era UB formale (TSan-visibile). Su x86
// load/store atomici su int = stessa istruzione: zero costo, stesso comportamento.
std::atomic<int> g_optimism[2] = {0, 0};

static inline int nn_scale(const Position& pos, Value psqt, Value positional, int rule50) {
    int nnue           = (125 * int(psqt) + 131 * int(positional)) / 128;
    int nnueComplexity = std::abs(int(psqt) - int(positional));
    nnue -= nnue * nnueComplexity / 18236;

    int npm = int(KnightValue) * pos.count<KNIGHT>() + int(BishopValue) * pos.count<BISHOP>()
            + int(RookValue) * pos.count<ROOK>() + int(QueenValue) * pos.count<QUEEN>();
    int material = 534 * pos.count<PAWN>() + npm;

    int v;
    if (g_eval_optimism) {
        int optimism = g_optimism[pos.side_to_move()];
        optimism += optimism * nnueComplexity / 476;   // SF: blend optimism con la complessita'
        v = int((std::int64_t(nnue) * (77871 + material)
                 + std::int64_t(optimism) * (7191 + material)) / 77871);
    } else
        v = int(std::int64_t(nnue) * (77871 + material) / 77871);
    g_last_unadjusted = v;   // PRE rule50/EvalScale: SF unadjustedStaticEval (fifty-independent)
    v -= v * rule50 / 199;
    if (g_eval_scale_pct != 100)
        v = int(std::int64_t(v) * g_eval_scale_pct / 100);  // re-calibrate to the search margins
    v = std::clamp(v, int(VALUE_TB_LOSS_IN_MAX_PLY) + 1, int(VALUE_TB_WIN_IN_MAX_PLY) - 1);
    return v;
}


// ---------------------------------------------------------------------------
// Net loading
// ---------------------------------------------------------------------------
static int load_net_impl(const char* path) {
    if (!path || !*path)
        return 0;
#ifdef TRIUMV_EMBED_RESOURCE
    embed_init_from_resource();
#endif
    // File presente sul disco? Se si', vince SEMPRE il file (path passato as-is:
    // un path completo non e' mai uguale al nome-default -> Network::load va sul
    // disco). Se NO: fallback sul net EMBEDDATO, ma solo se il nome richiesto e'
    // quello di default (EvalFile con un path esplicito sbagliato resta un errore).
    bool file_ok;
    {
        std::ifstream f(path, std::ios::binary);
        file_ok = f.good();
    }
    const char* load_name = path;
    if (!file_ok) {
        std::string p(path);
        size_t      sl   = p.find_last_of("/\\");
        std::string base = sl == std::string::npos ? p : p.substr(sl + 1);
        if (base != EvalFileDefaultName || !Eval::NNUE::embedded_net_available())
            return 0;
        load_name = EvalFileDefaultName;   // nome "nudo" -> Network::load instrada su <internal>
    }
    Eval::NNUE::EvalFile ef{};
    ef.defaultName = EvalFileDefaultName;  // serve al match nome-default -> embedded
    auto net = make_unique_large_page<Network>(ef);   // large pages (fallback automatico)
    net->load(".", load_name);  // dirs {"<internal>","",rootDir}: "" opens the path as given
    // verify() invokes the callback with a one-line SUCCESS info string when the net
    // loaded, or with a multi-line "ERROR: ..." block followed by exit(EXIT_FAILURE)
    // when it did not. So if verify() returns at all, the load succeeded; we just echo
    // the info line for visibility (do NOT treat the callback as a failure signal).
    // g_startup_quiet (main.cpp): when launched by a GUI, stay silent during the
    // net load so nothing prints before the "uci" handshake. verify() still runs
    // (it exits on a bad net); we just suppress the success info line.
    extern bool g_startup_quiet;
    net->verify(path, [](std::string_view s) {
        if (!g_startup_quiet) {
            std::printf("info string %.*s\n", int(s.size()), s.data());
            std::fflush(stdout);
        }
    });
    g_net = std::move(net);
    ++g_net_gen;   // invalidate every AccumulatorCaches built from the old net
    return 1;
}

int nn_load_net(const char* net_path) { return load_net_impl(net_path); }
int nn_reload_big(const char* net_path) { return load_net_impl(net_path); }

void nn_init_tables(void) {
    Bitboards::init();
    Attacks::init();  // slider magics live in attacks.cpp (separate from Bitboards::init)
}

// No-ops kept for API stability (both paths always use the refresh cache).
void nn_set_finny(int) {}
void nn_acc_stats(void) {}

// M3 toggles. Incremental is now the DEFAULT: validated bit-exact vs full-refresh
// (zero verify mismatches + IDENTICAL bench node counts at depth 12/14 across all 8
// positions, exercising captures/promotions/e.p./castling) at +16% NPS. Toggle OFF
// ("incremental off") to fall back to the M2 full-refresh A/B base.
static bool g_incremental = true;
static bool g_verify      = false;

// NB (2026-07-15): "PsqtFastPath" (eval = solo psqt del net ai nodi |psqt|>soglia,
// saltando pairwise+propagate) PROVATO e UCCISO CON MISURA su build PGO:
// thr700 fire 75% -> albero +154%; thr2000 fire 16% -> +28%; thr4000 fire 0.6%
// -> +4%; time-to-depth SEMPRE peggiore. Terza falsificazione della famiglia
// "eval economica ai nodi decisi" (smallnet SF -17 Elo, SPLE): la search e'
// co-adattata all'eval piena, ogni surrogato grossolano gonfia l'albero piu'
// di quanto il forward risparmiato ripaghi. NON riprovare varianti.
// N-1 lazy mirror apply: default ON (see nn_catch_up below). OFF = pre-N1 eager
// apply (nn_pos_do mirrors the move immediately), kept for bisection.
static bool g_lazy_mirror = true;
void        nn_set_incremental(int on) { g_incremental = on != 0; }
void        nn_set_verify(int on) { g_verify = on != 0; }
void        nn_set_lazy_mirror(int on) { g_lazy_mirror = on != 0; }
void        nn_set_eval_scale(int pct) { g_eval_scale_pct = pct < 1 ? 1 : pct; }
int         nn_get_eval_scale(void) { return g_eval_scale_pct; }   // per normalizzare 'score cp' in stampa (undo EvalScale, SF-style)
// 5.1 EvalTTWrite (SF-style): l'unadjusted dell'ultima nn_scale su questo thread (fifty-indep).
int         nn_last_unadjusted(void) { return g_last_unadjusted; }
// Ri-finalizza l'unadjusted col rule50 corrente: IDENTICO a un td_evaluate fresco (stesse op di
// nn_scale 111-114) per QUALSIASI fifty -> la cache eval e' esatta su ogni trasposizione.
int         nn_finalize(int unadjusted, int rule50) {
    int v = unadjusted;
    v -= v * rule50 / 199;
    if (g_eval_scale_pct != 100)
        v = int(std::int64_t(v) * g_eval_scale_pct / 100);
    return std::clamp(v, int(VALUE_TB_LOSS_IN_MAX_PLY) + 1, int(VALUE_TB_WIN_IN_MAX_PLY) - 1);
}

// ---------------------------------------------------------------------------
// Per-thread handle
// ---------------------------------------------------------------------------
namespace {

constexpr int SF_STACK = 1024;  // > MAX_PLY(246) + qsearch/extensions headroom

struct SfPos {
    Position                           pos;
    StateInfo                          si;
    std::unique_ptr<AccumulatorStack>  accStack;
    std::unique_ptr<AccumulatorCaches> caches;

    // Tracked across make/undo (nn_pos_eval gets the board but not stm/rule50).
    Color  stm;
    int    rule50;
    int    ply;
    Color  stmStack[SF_STACK];
    int    r50Stack[SF_STACK];
    // Incremental bookkeeping: the move recorded at each ply (for board undo) and
    // whether that ply pushes an accumulator state once applied (null moves do not
    // — the board is unchanged, so the accumulator chain stays at the same depth).
    SfMove mvStack[SF_STACK];
    bool   pushedAcc[SF_STACK];
    // N-1 lazy mirror apply: plies [0, appliedPly) have actually been replayed onto
    // pos/accStack; plies [appliedPly, ply) are pending (recorded but not yet
    // mirrored). nn_catch_up() advances appliedPly on demand, right before an eval.
    int    appliedPly = 0;

    int netGen;   // generation of g_net the caches were built from

    SfPos() : stm(WHITE), rule50(0), ply(0) {
        accStack = std::make_unique<AccumulatorStack>();
        caches   = std::make_unique<AccumulatorCaches>(*g_net);
        netGen   = g_net_gen;
    }
};

// Rebuild the handle's finny caches if the network was reloaded since they were
// built. Called at root set (never mid-search: EvalFile reload stops search first).
inline void ensure_caches_fresh(SfPos* p) {
    if (p->netGen != g_net_gen) {
        p->caches = std::make_unique<AccumulatorCaches>(*g_net);
        p->netGen = g_net_gen;
    }
}

inline Color flip(Color c) { return c == WHITE ? BLACK : WHITE; }

// Build an SF piece list from the engine bitboards bb[12] (vflip to SF coords).
inline int build_pl_from_bb(const unsigned long long* bb, Piece* pcs, Square* sqs) {
    int n = 0;
    for (int i = 0; i < 12; ++i) {
        std::uint64_t b = vflip(bb[i]);
        while (b) {
            int s = ctz64(b);
            b &= b - 1;
            pcs[n] = Piece(sfc[i]);
            sqs[n] = Square(s);
            ++n;
        }
    }
    return n;
}

// Full-refresh eval from the engine bitboards into the given (scratch) state.
inline int eval_full_from_bb(const unsigned long long* bb, Color stm, int rule50,
                             Position& pos, StateInfo& si, AccumulatorStack& acc,
                             AccumulatorCaches& cch) {
    Piece  pcs[64];
    Square sqs[64];
    int    n = build_pl_from_bb(bb, pcs, sqs);
    pos.set_pieces(pcs, sqs, n, stm, &si);
    acc.reset();
    auto [psqt, positional] = g_net->evaluate(pos, acc, cch);
    return nn_scale(pos, psqt, positional, rule50);
}

// TRANN1: delta dei PEDONI di una mossa (per il blocco PawnPair). Va chiamata
// PRIMA di mutare pos: il pair-diff si espande contro lo snapshot BEFORE.
// Arrocco non tocca mai pedoni -> any=false via i due check.
inline void fill_dirty_pawns(const Position& pos, const SfMove* m, DirtyPawns& dpw) {
    const Piece pc           = Piece(m->movedPiece);
    const bool  moverIsPawn  = type_of(pc) == PAWN;
    const bool  victimIsPawn = m->capturedPiece && type_of(Piece(m->capturedPiece)) == PAWN;

    dpw.nRemoved = 0;
    dpw.addedSq  = SQ_NONE;
    dpw.any      = moverIsPawn || victimIsPawn;
    if (!dpw.any)
        return;

    dpw.pawnsBefore[WHITE] = pos.pieces(WHITE, PAWN);
    dpw.pawnsBefore[BLACK] = pos.pieces(BLACK, PAWN);

    if (moverIsPawn)
    {
        dpw.removedSq[dpw.nRemoved] = Square(m->from);
        dpw.removedC[dpw.nRemoved]  = color_of(pc);
        dpw.nRemoved++;
        if (!m->promoPiece)  // la promozione non ri-aggiunge un pedone
        {
            dpw.addedSq = Square(m->to);
            dpw.addedC  = color_of(pc);
        }
    }
    if (victimIsPawn)
    {
        dpw.removedSq[dpw.nRemoved] = Square(m->capturedSq);  // ep: casa del pedone, non to
        dpw.removedC[dpw.nRemoved]  = color_of(Piece(m->capturedPiece));
        dpw.nRemoved++;
    }
}

// Apply SfMove m to pos (incremental), filling dp (DirtyPiece) + dts (DirtyThreats)
// + dpw (DirtyPawns, TRANN1). Mirrors Position::do_move's board mutation +
// DirtyPiece construction, driven by the already-decomposed SfMove (engine
// king-destination castling encoding).
inline void
apply_move(Position& pos, const SfMove* m, DirtyPiece& dp, DirtyThreats& dts, DirtyPawns& dpw) {
    const Piece  pc   = Piece(m->movedPiece);
    const Square from = Square(m->from);
    const Square to   = Square(m->to);

    fill_dirty_pawns(pos, m, dpw);  // PRIMA della mutazione (snapshot BEFORE)

    dp.pc        = pc;
    dp.from      = from;
    dp.to        = m->promoPiece ? SQ_NONE : to;
    dp.remove_sq = SQ_NONE;
    dp.add_sq    = SQ_NONE;

    if (m->rookPiece) {  // CASTLING: king from->to, rook rfrom->rto
        const Piece  rook  = Piece(m->rookPiece);
        const Square rfrom = Square(m->rookFrom);
        const Square rto   = Square(m->rookTo);
        dp.remove_pc = rook;
        dp.remove_sq = rfrom;
        dp.add_pc    = rook;
        dp.add_sq    = rto;
        // do_castling<true> order: remove both first (Chess960 overlap), then put both.
        pos.remove_piece(from, &dts);
        pos.remove_piece(rfrom, &dts);
        pos.put_piece(pc, to, &dts);
        pos.put_piece(rook, rto, &dts);
        return;
    }

    const bool ep = m->capturedPiece && (m->capturedSq != m->to);
    if (m->capturedPiece) {
        dp.remove_pc = Piece(m->capturedPiece);
        dp.remove_sq = Square(m->capturedSq);
    }
    if (m->promoPiece) {
        dp.add_pc = Piece(m->promoPiece);
        dp.add_sq = to;
    }

    if (ep) {
        pos.remove_piece(Square(m->capturedSq), &dts);  // remove e.p. pawn first (do_move order)
        pos.move_piece(from, to, &dts);                 // pawn from->to (pc == toPc)
    } else if (m->capturedPiece) {
        pos.remove_piece(from, &dts);
        pos.swap_piece(to, m->promoPiece ? Piece(m->promoPiece) : pc, &dts);
    } else if (m->promoPiece) {
        pos.remove_piece(from, &dts);
        pos.put_piece(Piece(m->promoPiece), to, &dts);
    } else {
        pos.move_piece(from, to, &dts);
    }
}

// Reverse apply_move on pos (no dts — undo just pops the accumulator). Mirrors the
// NET BOARD effect of Position::undo_move.
inline void unapply_move(Position& pos, const SfMove* m) {
    const Piece  pc   = Piece(m->movedPiece);
    const Square from = Square(m->from);
    const Square to   = Square(m->to);

    if (m->rookPiece) {  // CASTLING: do_castling<false> (remove to/rto, put from/rfrom)
        pos.remove_piece(to);
        pos.remove_piece(Square(m->rookTo));
        pos.put_piece(pc, from);
        pos.put_piece(Piece(m->rookPiece), Square(m->rookFrom));
        return;
    }

    if (m->promoPiece) {
        pos.remove_piece(to);     // remove the promoted piece
        pos.put_piece(pc, from);  // pawn back at from (pc is the pawn)
    } else {
        pos.move_piece(to, from);
    }
    if (m->capturedPiece)
        pos.put_piece(Piece(m->capturedPiece), Square(m->capturedSq));  // capsq handles e.p.
}

// N-1: replay any plies nn_pos_do/nn_pos_do_null left pending (mirrored bookkeeping
// only, no board/accumulator update) up to the current ply. Applied strictly in
// order so each apply_move sees the exact board state it would have under eager
// apply -> bit-identical DirtyThreats/accumulator chain, just computed lazily.
// In eager mode (g_lazy_mirror off) appliedPly is already kept in lockstep by
// nn_pos_do/do_null, so this loop is a no-op there.
inline void nn_catch_up(SfPos* p) {
    while (p->appliedPly < p->ply && p->appliedPly < SF_STACK) {
        int i = p->appliedPly;
        if (p->pushedAcc[i]) {
            auto dirties = p->accStack->push();
            apply_move(p->pos, &p->mvStack[i], std::get<0>(dirties), std::get<1>(dirties),
                       std::get<2>(dirties));
        }
        p->pos.set_side_to_move(flip(p->pos.side_to_move()));
        ++p->appliedPly;
    }
}

}  // namespace

void* nn_pos_create(void) { return new SfPos(); }
void  nn_pos_destroy(void* handle) { delete static_cast<SfPos*>(handle); }

void nn_pos_set(void* handle, int side_white, const int* pieces,
                const int* squares, int count, int rule50) {
    SfPos* p  = static_cast<SfPos*>(handle);
    ensure_caches_fresh(p);   // EvalFile reload -> caches seeded from old net's biases
    p->stm        = side_white ? WHITE : BLACK;
    p->rule50     = rule50;
    p->ply        = 0;
    p->appliedPly = 0;
    if (g_incremental) {
        Piece  pcs[64];
        Square sqs[64];
        for (int i = 0; i < count; ++i) {
            pcs[i] = Piece(pieces[i]);
            sqs[i] = Square(squares[i]);
        }
        p->pos.set_pieces(pcs, sqs, count, p->stm, &p->si);  // root board
        p->accStack->reset();                                // root accumulator computed lazily
    }
}

void nn_pos_do(void* handle, const struct SfMove* m) {
    SfPos* p = static_cast<SfPos*>(handle);
    if (p->ply < SF_STACK) {
        p->stmStack[p->ply]  = p->stm;
        p->r50Stack[p->ply]  = p->rule50;
        p->mvStack[p->ply]   = *m;
        p->pushedAcc[p->ply] = true;   // real move: pushes to accStack once applied
    }
    // N-1: lazy mode leaves the mirror/accStack untouched here (nn_catch_up applies
    // it later, only if an eval is actually reached). Eager fallback (g_lazy_mirror
    // off) applies immediately, same as pre-N1, keeping appliedPly in lockstep.
    if (g_incremental && !g_lazy_mirror) {
        auto dirties = p->accStack->push();  // {DirtyPiece&, DirtyThreats&, DirtyPawns&}
        apply_move(p->pos, m, std::get<0>(dirties), std::get<1>(dirties), std::get<2>(dirties));
        p->pos.set_side_to_move(flip(p->pos.side_to_move()));
        p->appliedPly = p->ply + 1;
    }
    ++p->ply;
    p->stm    = flip(p->stm);
    p->rule50 = m->rule50;
}

void nn_pos_do_null(void* handle, int rule50) {
    SfPos* p = static_cast<SfPos*>(handle);
    if (p->ply < SF_STACK) {
        p->stmStack[p->ply]  = p->stm;
        p->r50Stack[p->ply]  = p->rule50;
        p->pushedAcc[p->ply] = false;  // board unchanged -> no accumulator push
    }
    if (g_incremental && !g_lazy_mirror) {
        p->pos.set_side_to_move(flip(p->pos.side_to_move()));
        p->appliedPly = p->ply + 1;
    }
    ++p->ply;
    p->stm    = flip(p->stm);
    p->rule50 = rule50;
}

void nn_pos_undo(void* handle) {
    SfPos* p = static_cast<SfPos*>(handle);
    --p->ply;
    // N-1: only unwind the mirror/accStack if this ply was actually applied (either
    // by nn_catch_up because an eval needed it, or immediately in eager mode, where
    // appliedPly is always > ply here). Never-applied plies cost nothing to undo.
    if (g_incremental && p->appliedPly > p->ply) {
        if (p->ply >= 0 && p->ply < SF_STACK && p->pushedAcc[p->ply]) {
            p->accStack->pop();
            unapply_move(p->pos, &p->mvStack[p->ply]);
        }
        p->pos.set_side_to_move(flip(p->pos.side_to_move()));  // undo the make/null side flip
        p->appliedPly = p->ply;
    }
    if (p->ply >= 0 && p->ply < SF_STACK) {
        p->stm    = p->stmStack[p->ply];
        p->rule50 = p->r50Stack[p->ply];
    }
}

int nn_pos_eval(void* handle, const unsigned long long* bb, const unsigned long long* /*occ*/) {
    SfPos* p = static_cast<SfPos*>(handle);

    if (!g_incremental)
        return eval_full_from_bb(bb, p->stm, p->rule50, p->pos, p->si, *p->accStack, *p->caches);

    nn_catch_up(p);  // N-1: replay any moves nn_pos_do left pending before evaluating
    // Incremental: the maintained pos + accumulator chain are walked by Network::evaluate.
    auto [psqt, positional] = g_net->evaluate(p->pos, *p->accStack, *p->caches);
    int  inc                = nn_scale(p->pos, psqt, positional, p->rule50);

    if (g_verify) {
        // Compare against a full refresh built from the engine bitboards, on a separate
        // scratch state so the incremental chain is not disturbed.
        thread_local std::unique_ptr<AccumulatorStack>  sAcc;
        thread_local std::unique_ptr<AccumulatorCaches> sCch;
        thread_local Position                           sPos;
        thread_local StateInfo                          sSi;
        thread_local int                                sGen = 0;
        if (!sAcc || sGen != g_net_gen) {
            if (!sAcc) sAcc = std::make_unique<AccumulatorStack>();
            sCch = std::make_unique<AccumulatorCaches>(*g_net);
            sGen = g_net_gen;
        }
        int full = eval_full_from_bb(bb, p->stm, p->rule50, sPos, sSi, *sAcc, *sCch);
        if (inc != full) {
            static int reported = 0;
            if (reported++ < 64)
                std::printf("info string NNUE MISMATCH ply=%d inc=%d full=%d\n", p->ply, inc, full);
            std::fflush(stdout);
        }
    }
    return inc;
}

// Stateless full-refresh eval ("eval" command + NNUE_VERIFY oracle). pieces[] /
// squares[] are already in SF encoding (the engine's nn_build_piece_list maps via
// nn_piece_code[]/nnue_squares[]). Single-threaded (UI/debug only).
int nn_eval(int side_white, const int* pieces, const int* squares, int count, int rule50) {
    static std::unique_ptr<AccumulatorStack>  s_acc;
    static std::unique_ptr<AccumulatorCaches> s_cch;
    static Position                           s_pos;
    static StateInfo                          s_si;
    static int                                s_gen = 0;
    if (!s_acc || s_gen != g_net_gen) {
        if (!s_acc) s_acc = std::make_unique<AccumulatorStack>();
        s_cch = std::make_unique<AccumulatorCaches>(*g_net);
        s_gen = g_net_gen;
    }
    Piece  pcs[64];
    Square sqs[64];
    for (int i = 0; i < count; ++i) {
        pcs[i] = Piece(pieces[i]);
        sqs[i] = Square(squares[i]);
    }
    s_pos.set_pieces(pcs, sqs, count, side_white ? WHITE : BLACK, &s_si);
    s_acc->reset();
    auto [psqt, positional] = g_net->evaluate(s_pos, *s_acc, *s_cch);
    return nn_scale(s_pos, psqt, positional, rule50);
}

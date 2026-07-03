// Triumviratus 5.0 NNUE bridge — routes the engine's eval through the VENDORED
// Stockfish-master SFNNv13 network code (nnue/): ThreatFeatureSet=FullThreats
// + PSQFeatureSet=HalfKAv2_hm, L1=1024, 8 LayerStacks. This is Stockfish's own
// ultramodern NNUE machinery adapted into our isolated Stockfish:: namespace.
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
#include "nnue/position.h"
#include "nnue/types.h"
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/nnue/nnue_misc.h"   // Eval::NNUE::EvalFile

using namespace Stockfish;
using Stockfish::Eval::NNUE::Network;
using Stockfish::Eval::NNUE::AccumulatorStack;
using Stockfish::Eval::NNUE::AccumulatorCaches;

// ---------------------------------------------------------------------------
// The single immutable network, loaded once at startup (nn_load_net) and
// optionally swapped at runtime (nn_reload_big). ~90 MB of weights -> heap.
// AccumulatorCaches are built per handle FROM this net, so it must be loaded
// before any nn_pos_create().
// ---------------------------------------------------------------------------
static std::unique_ptr<Network> g_net;

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
    {
        std::ifstream f(path, std::ios::binary);
        if (!f.good())
            return 0;
    }
    Eval::NNUE::EvalFile ef{};
    auto net = std::make_unique<Network>(ef);
    net->load(".", path);  // dirs {"<internal>","",rootDir}: "" opens the path as given
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
void        nn_set_incremental(int on) { g_incremental = on != 0; }
void        nn_set_verify(int on) { g_verify = on != 0; }
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
    // Incremental bookkeeping: the move applied at each ply (for board undo) and
    // whether that ply pushed an accumulator state (null moves do not — the board
    // is unchanged, so the accumulator chain stays at the same depth).
    SfMove mvStack[SF_STACK];
    bool   pushedAcc[SF_STACK];

    SfPos() : stm(WHITE), rule50(0), ply(0) {
        accStack = std::make_unique<AccumulatorStack>();
        caches   = std::make_unique<AccumulatorCaches>(*g_net);
    }
};

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

// Apply SfMove m to pos (incremental), filling dp (DirtyPiece) + dts (DirtyThreats).
// Mirrors Position::do_move's board mutation + DirtyPiece construction, driven by
// the already-decomposed SfMove (engine king-destination castling encoding).
inline void apply_move(Position& pos, const SfMove* m, DirtyPiece& dp, DirtyThreats& dts) {
    const Piece  pc   = Piece(m->movedPiece);
    const Square from = Square(m->from);
    const Square to   = Square(m->to);

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

}  // namespace

void* nn_pos_create(void) { return new SfPos(); }
void  nn_pos_destroy(void* handle) { delete static_cast<SfPos*>(handle); }

void nn_pos_set(void* handle, int side_white, const int* pieces,
                const int* squares, int count, int rule50) {
    SfPos* p  = static_cast<SfPos*>(handle);
    p->stm    = side_white ? WHITE : BLACK;
    p->rule50 = rule50;
    p->ply    = 0;
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
        p->pushedAcc[p->ply] = false;
    }
    if (g_incremental) {
        auto dirties = p->accStack->push();  // {DirtyPiece&, DirtyThreats&}
        apply_move(p->pos, m, dirties.first, dirties.second);
        p->pos.set_side_to_move(flip(p->pos.side_to_move()));
        if (p->ply < SF_STACK)
            p->pushedAcc[p->ply] = true;
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
    if (g_incremental)
        p->pos.set_side_to_move(flip(p->pos.side_to_move()));
    ++p->ply;
    p->stm    = flip(p->stm);
    p->rule50 = rule50;
}

void nn_pos_undo(void* handle) {
    SfPos* p = static_cast<SfPos*>(handle);
    --p->ply;
    if (g_incremental) {
        if (p->ply >= 0 && p->ply < SF_STACK && p->pushedAcc[p->ply]) {
            p->accStack->pop();
            unapply_move(p->pos, &p->mvStack[p->ply]);
        }
        p->pos.set_side_to_move(flip(p->pos.side_to_move()));  // undo the make/null side flip
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
        if (!sAcc) {
            sAcc = std::make_unique<AccumulatorStack>();
            sCch = std::make_unique<AccumulatorCaches>(*g_net);
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
    if (!s_acc) {
        s_acc = std::make_unique<AccumulatorStack>();
        s_cch = std::make_unique<AccumulatorCaches>(*g_net);
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

// Bridge implementation: forwards to the standalone Stockfish NNUE probe.
// This is the ONLY translation unit (besides the sfnnue/ sources) that sees
// Stockfish headers, so the rest of the engine stays isolated from them.
//
// Thread-safety: each search thread owns its own incremental handle (its own
// Position mirror + StateInfo stack). The networks are immutable and only read,
// so concurrent eval calls are safe without locking. The original stateless
// sf_eval() builds a fresh local Position + thread_local scratch per call.

#include "sf_bridge.h"
#include <string>
#include <fstream>
#include "sfnnue/probe.h"
#include "sfnnue/position.h"
#include "sfnnue/evaluate.h"
#include "sfnnue/types.h"
#include "sfnnue/nnue/nnue_accumulator.h"

#include <vector>
#include <cassert>
#include <cstdio>
#include <cstdint>
#if defined(_MSC_VER)
    #include <cstdlib>   // _byteswap_uint64
#endif

using namespace Stockfish;

// Vertical flip of a bitboard (swap the 8 rank-bytes). The engine uses a8=0..
// h1=63 (BBC layout) while Stockfish uses a1=0..h8=63, and nnue_squares[] maps
// one to the other as sq ^ 56 == a per-rank reversal == byteswap. So an engine
// bitboard becomes the equivalent Stockfish bitboard via a single byteswap.
static inline std::uint64_t vflip(std::uint64_t b) {
#if defined(_MSC_VER)
    return _byteswap_uint64(b);
#else
    return __builtin_bswap64(b);
#endif
}

// DIAGNOSTIC (finny-tables analysis): print refresh vs incremental counts and
// the refresh fraction accumulated since the last call, then reset. If the
// refresh fraction is small, the finny cache cannot speed up the engine.
void sf_acc_stats(void) {
    unsigned long long r   = Stockfish::Eval::NNUE::g_dbg_refresh;
    unsigned long long inc = Stockfish::Eval::NNUE::g_dbg_incremental;
    unsigned long long tot = r + inc;
    double             pct = tot ? (100.0 * (double) r / (double) tot) : 0.0;
    printf("info string AccStats refresh=%llu incremental=%llu total=%llu refresh_pct=%.3f%%\n",
           r, inc, tot, pct);
    fflush(stdout);
    Stockfish::Eval::NNUE::g_dbg_refresh     = 0;
    Stockfish::Eval::NNUE::g_dbg_incremental = 0;
}

// FinnyTables (accumulator refresh cache) toggle. ON => each thread's cache
// turns a full refresh into a cheap piece diff. OFF => rebuild from biases
// (byte-identical to the pre-finny engine). Pure NPS lever; eval bit-identical
// either way, validated with an interleaved A/B NPS test (not SPRT).
// BAKED ON (2026-06-05): +6.9% NPS median (6/6 round positivi +4..+14%) +
// node-identity verificata (startpos d19 ON==OFF = 911516 nodi). Toggle tenuto A/B.
static bool g_finny = true;
void        sf_set_finny(int on) { g_finny = on != 0; }

// SINGLE-BOARD design (consolidated 2026-06-07, +2.9% NPS, eval BIT-IDENTICAL):
// the SF Position board is NOT maintained piece-by-piece on do/undo; only the
// StateInfo/dirtyPiece chain is kept, and the few board bitboards the NNUE reads
// are reconstructed from the engine's own bitboards once per evaluate() (see
// sf_pos_eval). The old dual-board path and its toggle were removed after the
// node-identity validation.

// Remember the small-net path so a runtime big-net reload (UCI "EvalFile") can
// re-init with the same small net (Probe::init takes both).
static std::string g_small_net_path;

void sf_init(const char* big_net, const char* small_net) {
    g_small_net_path = small_net ? small_net : "";
    Stockfish::Probe::init(big_net, small_net);
}

int sf_reload_big(const char* big_net_path) {
    if (!big_net_path || !*big_net_path) return 0;
    {
        std::ifstream f(big_net_path, std::ios::binary);
        if (!f.good()) return 0;   // don't blow away the loaded net for a bad path
    }
    Stockfish::Probe::init(big_net_path, g_small_net_path.c_str());
    return 1;
}

int sf_eval(int side_white, const int* pieces, const int* squares, int count, int rule50) {
    return Stockfish::Probe::eval(pieces, squares, count, side_white != 0, rule50);
}

// ---------------------------------------------------------------------------
// Incremental per-thread mirror
// ---------------------------------------------------------------------------
namespace {

// Max search ply we mirror. Triumviratus caps the main search at max_ply (64)
// and qsearch stays within ~max_ply+8, so 256 is a generous safety margin.
constexpr int SF_STACK = 256;

struct SfPos {
    Position              pos;
    std::vector<StateInfo> st;   // pre-sized => element pointers stay stable
    std::vector<SfMove>    undo;  // how to reverse each pushed move
    std::vector<bool>      isNull;
    int                    ply;
    // Per-thread "finny tables" cache (~0.7 MB). Heap-allocated with the SfPos
    // (handles are created with new), never shared between threads.
    Stockfish::Eval::NNUE::AccumulatorCaches caches;

    SfPos() : st(SF_STACK), undo(SF_STACK), isNull(SF_STACK, false), ply(0) {
        // Position non inizializza pos.st nel suo ctor: se un do/rescue gira PRIMA
        // di sf_pos_set, begin_state legge prev=pos.st garbage -> SEGV (su Linux;
        // su Windows il garbage era benigno). Puntiamo a st[0] (il vector e'
        // value-initialized = zero) cosi' la lettura e' sempre valida.
        pos.st = &st[0];
    }
};

inline Color flip(Color c) { return c == WHITE ? BLACK : WHITE; }

// Copy the per-move scalar state and mark both accumulators dirty.
inline void begin_state(StateInfo* ns, StateInfo* prev, int rule50) {
    ns->previous               = prev;
    ns->rule50                 = rule50;
    ns->nonPawnMaterial[WHITE] = prev->nonPawnMaterial[WHITE];
    ns->nonPawnMaterial[BLACK] = prev->nonPawnMaterial[BLACK];
    ns->accumulatorBig.computed[WHITE]   = ns->accumulatorBig.computed[BLACK]   = false;
    ns->accumulatorSmall.computed[WHITE] = ns->accumulatorSmall.computed[BLACK] = false;
}

}  // namespace

void* sf_pos_create(void) { return new SfPos(); }

void sf_pos_destroy(void* handle) { delete static_cast<SfPos*>(handle); }

void sf_pos_set(void* handle, int side_white, const int* pieces,
                const int* squares, int count, int rule50) {
    SfPos* p = static_cast<SfPos*>(handle);
    p->ply = 0;
    // set() memsets the Position and the root StateInfo (computed=false,
    // previous=nullptr), so the first evaluate() does a full refresh.
    p->pos.set(pieces, squares, count, side_white != 0, rule50, &p->st[0]);
    // Reset the finny-table cache to "empty board" at each search root. Cheap
    // (a memset) and keeps the cache correct without relying on cross-search
    // state; it warms up again within the search. Nets are loaded by now.
    if (g_finny)
        Stockfish::Eval::NNUE::clear_accumulator_caches(p->caches);
}

void sf_pos_do(void* handle, const struct SfMove* m) {
    SfPos*     p    = static_cast<SfPos*>(handle);
    StateInfo* prev = p->pos.st;
    assert(p->ply + 1 < SF_STACK);
    StateInfo* ns = &p->st[++p->ply];

    begin_state(ns, prev, m->rule50);

    // Build the dirty-piece list. The moving piece MUST be entry 0 because
    // Stockfish detects a king move (forcing a perspective refresh) via
    // dirtyPiece.piece[0].
    DirtyPiece& dp = ns->dirtyPiece;
    int         n  = 0;
    dp.piece[n] = Piece(m->movedPiece);
    dp.from[n]  = Square(m->from);
    dp.to[n]    = m->promoPiece ? SQ_NONE : Square(m->to);  // pawn vanishes on promotion
    ++n;
    if (m->promoPiece) {
        dp.piece[n] = Piece(m->promoPiece);
        dp.from[n]  = SQ_NONE;
        dp.to[n]    = Square(m->to);
        ++n;
    }
    if (m->capturedPiece) {
        dp.piece[n] = Piece(m->capturedPiece);
        dp.from[n]  = Square(m->capturedSq);
        dp.to[n]    = SQ_NONE;
        ++n;
    }
    if (m->rookPiece) {
        dp.piece[n] = Piece(m->rookPiece);
        dp.from[n]  = Square(m->rookFrom);
        dp.to[n]    = Square(m->rookTo);
        ++n;
    }
    dp.dirty_num = n;

    p->pos.st         = ns;
    p->pos.sideToMove = flip(p->pos.sideToMove);
    // Single-board: the SF board is NOT mutated here; it is reconstructed from the
    // engine bitboards in sf_pos_eval. Only the StateInfo/dirtyPiece chain (above)
    // is needed for the lazy accumulator.

    // Keep non-pawn material exact (eval uses it for net selection + scaling).
    if (m->capturedPiece) {
        PieceType pt = type_of(Piece(m->capturedPiece));
        if (pt != PAWN && pt != KING)
            ns->nonPawnMaterial[color_of(Piece(m->capturedPiece))] -= PieceValue[m->capturedPiece];
    }
    if (m->promoPiece)
        ns->nonPawnMaterial[color_of(Piece(m->promoPiece))] += PieceValue[m->promoPiece];
}

void sf_pos_do_null(void* handle, int rule50) {
    SfPos*     p    = static_cast<SfPos*>(handle);
    StateInfo* prev = p->pos.st;
    assert(p->ply + 1 < SF_STACK);
    StateInfo* ns = &p->st[++p->ply];

    begin_state(ns, prev, rule50);
    ns->dirtyPiece.dirty_num = 0;
    ns->dirtyPiece.piece[0]  = NO_PIECE;  // not a king => no forced refresh

    p->pos.st         = ns;
    p->pos.sideToMove = flip(p->pos.sideToMove);
}

void sf_pos_undo(void* handle) {
    SfPos* p = static_cast<SfPos*>(handle);

    p->pos.sideToMove = flip(p->pos.sideToMove);
    // Single-board: no SF board to restore (it is rebuilt from the engine bitboards
    // at the next eval); just pop the StateInfo chain.

    --p->ply;
    p->pos.st = &p->st[p->ply];
}

int sf_pos_eval(void* handle, const unsigned long long* bb, const unsigned long long* occ) {
    SfPos* p = static_cast<SfPos*>(handle);

    {
        // Rebuild the board fields the NNUE reads, from the engine's own bitboards.
        // Engine piece order: P,N,B,R,Q,K (0..5) white, p,n,b,r,q,k (6..11) black.
        // SF PieceType PAWN..KING = 1..6, ALL_PIECES = 0; Color WHITE=0,BLACK=1.
        // SF bitboard = byteswap(engine bitboard) (sq ^ 56 rank flip).
        Position& pos = p->pos;
        pos.byColorBB[WHITE]     = vflip(occ[0]);                 // white occupancy
        pos.byColorBB[BLACK]     = vflip(occ[1]);                 // black occupancy
        pos.byTypeBB[ALL_PIECES] = vflip(occ[2]);                 // ALL_PIECES == 0
        pos.byTypeBB[PAWN]       = vflip(bb[0]  | bb[6]);
        pos.byTypeBB[KNIGHT]     = vflip(bb[1]  | bb[7]);
        pos.byTypeBB[BISHOP]     = vflip(bb[2]  | bb[8]);
        pos.byTypeBB[ROOK]       = vflip(bb[3]  | bb[9]);
        pos.byTypeBB[QUEEN]      = vflip(bb[4]  | bb[10]);
        pos.byTypeBB[KING]       = vflip(bb[5]  | bb[11]);
        // Only count<PAWN>(c) and count<ALL_PIECES>() are read at runtime (release
        // strips the square<>() asserts), i.e. pieceCount[W_PAWN/B_PAWN] and
        // pieceCount[make_piece(c,ALL_PIECES)].
        pos.pieceCount[W_PAWN]                       = popcount(bb[0]);
        pos.pieceCount[B_PAWN]                       = popcount(bb[6]);
        pos.pieceCount[make_piece(WHITE, ALL_PIECES)] = popcount(occ[0]);
        pos.pieceCount[make_piece(BLACK, ALL_PIECES)] = popcount(occ[1]);
        // The mailbox board[] is only read by the non-finny refresh (piece_on()).
        // With the finny cache on (default) it is never touched, so rebuild it
        // only when finny is off.
        if (!g_finny) {
            for (int s = 0; s < SQUARE_NB; ++s) pos.board[s] = NO_PIECE;
            static const int sfc[12] = {W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                                        B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING};
            for (int i = 0; i < 12; ++i) {
                std::uint64_t b = bb[i];
                while (b) {
                    int s = lsb(Bitboard(b));     // engine square (bit index)
                    b &= b - 1;
                    pos.board[s ^ 56] = Piece(sfc[i]);
                }
            }
        }
    }

    return Stockfish::Eval::evaluate(p->pos, g_finny ? &p->caches : nullptr);
}

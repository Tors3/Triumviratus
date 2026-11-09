// Triumviratus, a UCI chess engine.
// Copyright (C) 2026 Francesco Torsello
// SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)
// Additional term under section 7(b) of the GPLv3: anyone who copies, modifies or redistributes this file, in whole
// or in part, must preserve this notice and the attribution "Consilium inference: Francesco Torsello, Triumviratus
// (https://github.com/Tors3/Triumviratus)", in the file and in the credits (Appropriate Legal Notices) of any program
// that contains it.

// consilium_bridge.cpp -- the engine's link with the Consilium network (block 6 of 8.1, 18/10/2026).
//
// The network is evaluated by the module in consilium/ (cn.h, cn.cpp), written from our own specification of the
// network (docs/development/8.1/CONSILIUM_INFERENCE.md); the previous inference is gone. This file holds what the
// engine adds around the module:
//   - loading the network (file, embedded resource, memory shared between processes);
//   - the per-thread handles, whose dirty stack the engine's make writes (search/06_nndirty.inc, nn_dirty.h);
//   - the five steps from the two network outputs to the static evaluation (nn_scale, nn_finalize);
//   - the UCI options of the evaluation constants and the diagnostic entry points (eval, evalbatch, nnueverify).
// Until 18/10/2026 this file was nnue_bridge.cpp.

#include "frozen.h"   // must come first: without it the release builds do not compile the tuned constants as literals
#include "consilium_bridge.h"

#include "profile.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#if defined(_MSC_VER)
    #include <intrin.h>   // _byteswap_uint64, _BitScanForward64
#endif

#ifdef TRIUMV_EMBED_RESOURCE
// Windows: the default network sits in an RCDATA resource of the exe (incbin does not work with _MSC_VER, clang-cl
// included). The pointers are resolved at the first load and read by load_net_impl below.
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

#include "consilium/cn.h"

#include "large_pages.h"   // the network and the per-thread handles live on large pages when possible
#include <mutex>
#if defined(__linux__)
#include <sys/syscall.h>   // NumaSpread: mbind of a thread's handle (nn_pos_rehome)
#include <unistd.h>
#endif

// The few types and values the rest of the bridge uses, in a namespace of their own.
namespace cnb {

using Value    = int;
using Bitboard = std::uint64_t;
enum Color { WHITE, BLACK };

inline int pop_lsb(Bitboard& b) {
    const int s = __builtin_ctzll(b);
    b &= b - 1;
    return s;
}

// The engine's board as nn_scale reads it (pieces, counts per type, side to move).
class NnBoard {
   public:
    NnBoard(const unsigned long long* bb, const unsigned long long* occ, const int* mb, Color stm) :
        piece_bb_(bb), occupied_(occ), mb_(mb), mover_(stm) {}
    int   count() const { return __builtin_popcountll(occupied_[2]); }
    int   count_type(int t) const { return __builtin_popcountll(piece_bb_[t] | piece_bb_[t + NN_BLACK]); }
    Color mover() const { return mover_; }
    // (30/10/2026) cn::Board holds std::uint64_t, which is unsigned long on Linux: same 64 bits, so the cast only
    // makes the Linux build compile (found by the labeler build on the VM).
    cn::Board view() const {
        return cn::Board{reinterpret_cast<const std::uint64_t*>(piece_bb_), reinterpret_cast<const std::uint64_t*>(occupied_),
                         mb_, mover_ == BLACK};
    }

   private:
    const unsigned long long* piece_bb_;
    const unsigned long long* occupied_;
    const int*                mb_;
    Color                     mover_;
};

}  // namespace cnb

    // Default network file (28/10/2026, author: "nn-consilium-plebis"; nn-consilium2.nnue before).
    // TRANN5 lab (07/11/2026): the Lex Hortensia test file of tools/make_t5.py (untrained, speed work only).
    #define CN_DEFAULT_NET_FILE "nn-lex-hortensia.nnue"

// GCC and clang builds from the Makefile (Linux, Android, MinGW): the default network is embedded with incbin when it
// sits at the build root (the Makefile sets NNUE_EMBEDDING_OFF otherwise). incbin.h is Dale Weiler's public-domain
// header (incbin/UNLICENCE). It defines gEmbeddedNNUEData and gEmbeddedNNUESize.
#if !defined(_MSC_VER) && !defined(NNUE_EMBEDDING_OFF) && !defined(UNIVERSAL_BINARY) && !defined(TRIUMV_EMBED_RESOURCE)
    #define INCBIN_SILENCE_BITCODE_WARNING
    #include "incbin/incbin.h"
INCBIN(EmbeddedNNUE, CN_DEFAULT_NET_FILE);
    #define TRIUMV_EMBED_INCBIN
#endif

#if (defined(_WIN32) || defined(__linux__)) && !defined(TRIUMV_NO_SHARED_NET) && !defined(TRIUMV_SHARED_NET)
    #define TRIUMV_SHARED_NET
#endif
#ifdef TRIUMV_SHARED_NET
    #include "shared_net.h"
#endif

using namespace cnb;

// The network: one block of fixed size on large pages, or the copy shared between processes (shared_net.h).
static cn::Net*        g_cn      = nullptr;
static const cn::Net*  g_cn_view = nullptr;
static std::string     g_net_mem_status;
static std::atomic<int> g_net_gen{1};
static std::string     g_net_path;   // path of the network loaded last (PassedScale reloads it)
// NumaSpread (03/11/2026): a search thread bound to a node reads the network copy of that node (nn_bind_node_net);
// every other thread reads the copy the load made.
static thread_local const cn::Net* t_net = nullptr;
static inline const cn::Net& net_ref() { return t_net ? *t_net : *g_cn_view; }
#define NET_REF (net_ref())

const char* nn_net_memory_status(void) { return g_net_mem_status.c_str(); }

static int load_net_impl(const char* path) {
    if (!path || !*path)
        return 0;
#ifdef TRIUMV_EMBED_RESOURCE
    embed_init_from_resource();
#endif
    extern bool g_startup_quiet;
#if defined(TRIUMV_SHARED_NET) && defined(__linux__)
    // Linux (01/11/2026): the shared weights of this file, when another process already made them, are mapped
    // without loading the file (shared_net.h, attach_by_file): no private copy at the start of each engine.
    {
        std::string st;
        const void* sh = TriumvShm::attach_by_file(path, std::uint64_t(cn::passed_scale()), sizeof(cn::Net), st);
        if (sh) {
            cn::adopt(*static_cast<const cn::Net*>(sh));   // the router copy and the pawn-king signals, as a load sets them
            cn::Net* old = g_cn;
            g_cn      = nullptr;
            g_cn_view = static_cast<const cn::Net*>(sh);
            g_net_path = path;
            g_net_mem_status = st;
            if (!g_startup_quiet) {
                std::printf("info string NNUE evaluation using %s (Consilium, module cn)\n", path);
                std::printf("info string NNUE weights: %s\n", st.c_str());
                std::fflush(stdout);
            }
            if (old)
                large_pages_free(old);
            ++g_net_gen;
            return 1;
        }
    }
#endif
    cn::Net* net = static_cast<cn::Net*>(large_pages_alloc(sizeof(cn::Net)));
    if (!net)
        return 0;
    std::string err;
    bool        ok;
    {
        std::ifstream f(path, std::ios::binary);
        if (f.good())
            ok = cn::load_file(path, net, &err);
        else {
            // No file: the embedded network, but only under the default name.
            std::string p(path);
            const size_t sl = p.find_last_of("/\\");
            const std::string base = sl == std::string::npos ? p : p.substr(sl + 1);
#if defined(TRIUMV_EMBED_RESOURCE) || defined(TRIUMV_EMBED_INCBIN)
            ok = base == CN_DEFAULT_NET_FILE && gEmbeddedNNUEData
              && cn::load_memory(gEmbeddedNNUEData, gEmbeddedNNUESize, net, &err);
#else
            ok = false;
            err = "file not found";
#endif
        }
    }
    if (!ok) {
        large_pages_free(net);
        std::printf("info string ERROR: network %s not loaded (%s)\n", path, err.c_str());
        std::fflush(stdout);
        return 0;
    }
    if (!g_startup_quiet) {
        std::printf("info string NNUE evaluation using %s (Consilium, module cn)\n", path);
        std::fflush(stdout);
    }
    cn::Net* old = g_cn;
    g_cn      = net;
    g_cn_view = net;
    g_net_path = path;
#ifdef TRIUMV_SHARED_NET
    {
        std::string st;
#ifdef __linux__   // with the file alias for the next processes (attach_by_file above)
        const void* sh = TriumvShm::attach(g_cn, sizeof(cn::Net), g_cn->contentHash ^ std::uint64_t(sizeof(cn::Net)), st,
                                           path, std::uint64_t(cn::passed_scale()));
#else
        const void* sh = TriumvShm::attach(g_cn, sizeof(cn::Net), g_cn->contentHash ^ std::uint64_t(sizeof(cn::Net)), st);
#endif
        if (sh) {
            g_cn_view = static_cast<const cn::Net*>(sh);
            large_pages_free(g_cn);
            g_cn = nullptr;
        }
        g_net_mem_status = st;
        if (!g_startup_quiet) {
            std::printf("info string NNUE weights: %s\n", st.c_str());
            std::fflush(stdout);
        }
    }
#endif
    if (old)
        large_pages_free(old);
    ++g_net_gen;
    return 1;
}

// =====================================================================================================================
// From the two network outputs to the static evaluation (block 6 of 8.1, 18/10/2026)
//
// The network gives two values from the side to move's point of view: psqt (the PSQT sums of the accumulators) and
// positional (the layer stack). The evaluation the search sees is built from them in five steps:
//   1. blend   : a weighted sum of the two heads (positional weight of its own in endgames);
//   2. agree   : damped when the two heads disagree, since a large |psqt - positional| marks a position the network
//                reads in two different ways;
//   3. material: stretched with the material left on the board, and the search's optimism added on the same scale;
//   4. fifty   : pulled towards zero as the fifty-move counter grows;
//   5. scale   : the per-bucket EvalScale that matches the search margins, then the bounds of a normal score.
// Steps 1-3 do not depend on the fifty-move counter: they are the "unadjusted" value the evaluation cache stores, and
// nn_finalize applies steps 4-5 to it with the counter of the position that reads it.
//
// Step 2 is the linear damping v - v*d/EvalComplexDiv; the constants of steps 1-3 were tuned by our SPSA runs on this
// network (BAKE 09/09/2026 and BAKE MOE1 30/09/2026).
// Step 4 mixes two forms (18/10/2026), EvalFiftyMix /1024: the linear pull v - v*r/199 (0, the default) and a form of
// our own that keeps the full value for the first EvalFiftyKeep plies of reversible play, then falls linearly to 0 at
// ply 100, where a draw can be claimed (1024). On 2 million positions from games (tools/evalshape_predict.py) the
// second form predicts the result better in every band of the counter, and the SPSA run T15b raised the mix.
// (A second form of step 2, v*A/(A + d), predicted no better and the same SPSA run drove its mix to zero: removed on
// 18/10/2026.)
// =====================================================================================================================

// Per-bucket EvalScale (percent). The eight output buckets of the network (by piece count) each get their own scale;
// the UCI option EvalScale writes all eight, EvalScaleB0..B7 one each (BAKE MOE1, 30/09/2026).
static int g_eval_scale_pct  = 60;
// Lex Hortensia (9.0, 09/11/2026): the 8.1 values {60, 54, 60, 57, 59, 58, 59, 58}, fitted with Consilium Plebis,
// divided per bucket by Lex's raw output against Plebis's on 60,000 game positions (slope 0.940-0.984, all buckets
// 0.953), so that the search reads Lex on the scale its margins were tuned on. Same network on both sides, 20k nodes:
// +0.9 +- 6.3 over 4,000 games against the 8.1 values. To be measured again on the final network.
static int g_eval_scale_b[8] = {62, 57, 64, 59, 61, 60, 61, 59};

// Terms of the last evaluation of one thread (kept in the thread's handle, not thread_local: the MinGW TLS emulation
// cost ~2.4% of the engine on 25/09/2026). unadjusted = steps 1-3 for the evaluation cache (EvalTTWrite); opt_base and
// opt_coeff split it as base + optimism * coeff / 1000 with EvalScale already inside coeff (EvalCacheOptSplit), so a
// cached entry can be finished with the optimism of the current iteration.
struct NnLast {
    int unadjusted = 0;
    int opt_base   = 0;
    int opt_coeff  = 0;
};

// Optimism: the search's bias towards its own side, written per side at the root of each iteration (by every
// thread for itself with OptPerThread, always on since the search rewrite of 04/10/2026). The two switches are no
// longer set anywhere (03/11/2026): constants, so the compiler drops their tests.
constexpr int    g_eval_optimism  = 1;
std::atomic<int> g_optimism[2]    = {0, 0};   // relaxed: written by the main thread, read by the evaluations
constexpr int    g_opt_per_thread = 1;

// Constants of steps 1-3 (UCI options, table g_eval_consts below). All tuned by our SPSA runs; the old values are in
// parentheses. EvalPsqtW is left untuned on purpose: with EvalPosW it spans the "scale everything" direction that
// EvalScale already covers, so only their ratio moves.
int g_ev_psqt_w    = 125;     // psqt weight (/128)
int g_ev_pos_w     = 131;     // positional weight (/128)                       (126, BAKE MOE1)
int g_ev_pos_w_end = 131;     // positional weight with <= 15 pieces (/128); 125 measured -6.6 +- 8.3 (01/10/2026)
int g_ev_cplx_div  = 18198;   // disagreement that cancels the value             (19139, BAKE MOE1)
int g_ev_pawn_mat  = 680;     // pawn in the material term                       (551)
int g_ev_mat_base  = 102051;  // material scale: v * (base + material) / base    (84768)
int g_ev_opt_cplx  = 432;     // optimism grows by optimism * d / this           (461)
int g_ev_opt_base  = 5831;    // optimism scale: optimism * (this + material)    (6456)
// Step 4: the mix of the two forms (/1024, 0 = linear pull, identical tree) and the plies kept at full value.
int g_ev_fifty_mix  = 0;
int g_ev_fifty_keep = 20;

// Values of the minor and major pieces in the material term, and the bounds of a normal (non-mate, non-TB) score.
constexpr int MAT_KNIGHT = 781, MAT_BISHOP = 825, MAT_ROOK = 1276, MAT_QUEEN = 2538;
// Pawn weight and material base of the optimism term: the default values of EvalPawnMat and EvalMatBase, fixed.
constexpr int OPT_PAWN_MAT = 680, OPT_MAT_BASE = 102051;
constexpr int EVAL_BOUND = 31506;   // |static eval| <= 31506: below every tablebase and mate score

// Release builds (TRIUMV_FROZEN) compile the tuned values as literals: the divisions of steps 1-3 then become
// multiply-and-shift with identical integer results (as variables they were hardware divisions, one of them 64-bit:
// 57 cycles per node on 09/09/2026). The variables still exist for the UCI table; nn_frozen_check reports a change
// that such a build ignores. Since 03/11/2026 the fifty-move form (EvalFiftyMix/Keep) and the switches no tester uses
// (optimism, incremental, g_c2_want_effect) are constants too: xperf -0.17% / -0.59% cycles per node (middlegame /
// endgames, Xeon, same tree).
#ifdef TRIUMV_FROZEN
#define g_ev_psqt_w 125
#define g_ev_pos_w 131
#define g_ev_pos_w_end 131
#define g_ev_cplx_div 18198
#define g_ev_pawn_mat 680
#define g_ev_mat_base 102051
#define g_ev_opt_cplx 432
#define g_ev_opt_base 5831
#define g_ev_fifty_mix 0
#define g_ev_fifty_keep 20
#endif

// Step 4 (fifty-move counter), shared by nn_scale and nn_finalize.
static inline int fifty_pull(int v, int rule50) {
    const int mix   = g_ev_fifty_mix;
    const int lin_v = v - v * rule50 / 199;
    if (mix == 0) return lin_v;
    const int r = std::min(rule50, 100), r0 = g_ev_fifty_keep;
    const int keep_v = r <= r0 ? v : int(std::int64_t(v) * (100 - r) / (100 - r0));
    return mix == 1024 ? keep_v : lin_v + int(std::int64_t(keep_v - lin_v) * mix / 1024);
}

// Step 5: per-bucket EvalScale and the bounds of a normal score.
static inline int scale_and_bound(int v, int scale_pct) {
    if (scale_pct != 100) v = int(std::int64_t(v) * scale_pct / 100);
    return std::clamp(v, -EVAL_BOUND, EVAL_BOUND);
}

static inline int nn_scale(const NnBoard& pos, Value psqt, Value positional, int rule50,
                           NnLast* last = nullptr, const int* opt_local = nullptr) {
    const int pieces = pos.count();

    // 1. blend
    const int wPos  = pieces <= 15 ? g_ev_pos_w_end : g_ev_pos_w;
    int       blend = (g_ev_psqt_w * int(psqt) + wPos * int(positional)) / 128;

    // 2. agree
    const int disagree = std::abs(int(psqt) - int(positional));
    blend -= blend * disagree / g_ev_cplx_div;

    // 3. material (and optimism on the same scale)
    const int pawns    = pos.count_type(NN_PAWN);   // counted once (Houze Guo, 03/11/2026)
    const int material = g_ev_pawn_mat * pawns + MAT_KNIGHT * pos.count_type(NN_KNIGHT)
                       + MAT_BISHOP * pos.count_type(NN_BISHOP) + MAT_ROOK * pos.count_type(NN_ROOK)
                       + MAT_QUEEN * pos.count_type(NN_QUEEN);
    // With a negative pawn weight the factor could reach zero or below and flip the sign of the evaluation: it never
    // goes under a quarter of the base (at the defaults it is always above the base, so nothing changes).
    const std::int64_t stretched = std::int64_t(blend) * std::max(g_ev_mat_base + material, g_ev_mat_base / 4);
    // The optimism has its own material sum and scale (18/10/2026): with the pawn weight and the material base it
    // shared with the evaluation, a fit of those two (tools/evalscale_fit.py: negative pawn, base x27) would have
    // flipped the sign of the optimism or made it vanish. At the defaults (pawn 680, base 102051) the result is
    // identical bit for bit to the shared form.
    const int          opt_material = material + (OPT_PAWN_MAT - g_ev_pawn_mat) * pawns;
    int                v;
    if (g_eval_optimism) {
        int opt = opt_local ? opt_local[pos.mover()] : g_optimism[pos.mover()].load();
        opt += opt * disagree / g_ev_opt_cplx;
        v = int((stretched + std::int64_t(opt) * (g_ev_opt_base + opt_material) * g_ev_mat_base / OPT_MAT_BASE)
                / g_ev_mat_base);
    } else
        v = int(stretched / g_ev_mat_base);

    const int scale_pct = g_eval_scale_b[(pieces - 1) / 4];   // the network's output bucket
    if (last) {
        last->opt_base   = int(stretched / g_ev_mat_base);
        last->opt_coeff  = int(std::int64_t(g_ev_opt_cplx + disagree) * (g_ev_opt_base + opt_material) * 1000 * scale_pct
                               / (std::int64_t(g_ev_opt_cplx) * OPT_MAT_BASE * 100));
        last->unadjusted = v;
    }

    // 4-5. fifty, scale and bounds
    return scale_and_bound(fifty_pull(v, rule50), scale_pct);
}

// ---------------------------------------------------------------------------
// Net loading entry points and options
// ---------------------------------------------------------------------------
const char* nn_default_net_name(void) { return CN_DEFAULT_NET_FILE; }

int nn_load_net(const char* net_path) { return load_net_impl(net_path); }
int nn_reload_big(const char* net_path) { return load_net_impl(net_path); }

int nn_set_passed_scale(int pct) {
    const int old = cn::passed_scale();
    cn::set_passed_scale(pct);
    if (cn::passed_scale() == old || g_net_path.empty())
        return 0;
    const std::string path = g_net_path;   // load_net_impl rewrites g_net_path
    if (load_net_impl(path.c_str()))
        return 1;
    cn::set_passed_scale(old);   // the old network stays loaded: keep the value it was read with
    return 0;
}

// (nn_export_graft, nn_export_pst, nn_pst_indices and nn_graft_entries, for the graft blocks: removed on 10/10/2026;
// the code is in the training repository, 04_consilium/graft_engine_storico.)

void nn_init_tables(void) {
    cn::init();   // feature index tables of the Consilium module (the slider tables are built by init_bitboards)
}

// No-ops kept for API stability (the module always uses its refresh cache).
void nn_set_finny(int) {}

void nn_acc_stats(void) {}

// Incremental evaluation is the default (identical to a full refresh at every node); "incremental off" refreshes every
// evaluation, as a diagnostic. nnueverify compares the two at every evaluation. Release builds (TRIUMV_FROZEN, 03/11/2026)
// keep it on as a constant: "incremental off" is a development diagnostic, and the test leaves every evaluation.
#ifdef TRIUMV_FROZEN
static constexpr bool g_incremental = true;
#else
static bool g_incremental = true;
#endif
static bool g_verify      = false;
static unsigned long long g_verify_count = 0, g_verify_bad = 0;   // evaluations compared / different

// (15/07/2026) A cheaper evaluation at decided nodes (only the psqt head when |psqt| is above a threshold) was measured
// and dropped: the tree grew by 154%, 28% and 4% at thresholds 700, 2000 and 4000, and time to depth was always worse.
// It was the third test of that family (a small second network -17 Elo, a lazy evaluation with the parent's
// positional part): the search is fitted to the full evaluation, and a rough substitute grows the tree more than the
// skipped forward pass saves.
#ifdef TRIUMV_FROZEN
void        nn_set_incremental(int on) { (void)on; }
#else
void        nn_set_incremental(int on) { g_incremental = on != 0; }
#endif
void        nn_set_verify(int on) { g_verify = on != 0; }
void nn_set_eval_scale(int pct) {   // EvalScale: all eight buckets
    g_eval_scale_pct = pct < 1 ? 1 : pct;
    for (int b = 0; b < 8; ++b) g_eval_scale_b[b] = g_eval_scale_pct;
}
void nn_set_eval_scale_bucket(int b, int pct) {   // EvalScaleB0..B7
    if (b >= 0 && b < 8) g_eval_scale_b[b] = pct < 1 ? 1 : pct;
}
int nn_get_eval_scale_bucket(int b) { return (b >= 0 && b < 8) ? g_eval_scale_b[b] : 60; }

// The UCI options of the evaluation constants, in one table: uci_mt declares and routes everything listed here.
// The lower bounds of the divisors are not cosmetic: a zero would be a division by zero inside the evaluation.
// EvalBucketOverride is a probe (-1 = normal, 0..7 forces the output bucket), not a playing option.
#define CN_BUCKET_OVERRIDE (&cn::g_bucket_override)

#ifdef TRIUMV_FROZEN   // the table needs the variables, not the literals
#undef g_ev_psqt_w
#undef g_ev_pos_w
#undef g_ev_pos_w_end
#undef g_ev_cplx_div
#undef g_ev_pawn_mat
#undef g_ev_mat_base
#undef g_ev_opt_cplx
#undef g_ev_opt_base
#undef g_ev_fifty_mix
#undef g_ev_fifty_keep
#endif
namespace {
struct EvalConst { const char* name; int* var; int lo; int hi; };
const EvalConst g_eval_consts[] = {
    {"EvalPsqtW",          &g_ev_psqt_w,      40,    260},
    {"EvalPosW",           &g_ev_pos_w,       40,    260},
    {"EvalPosWEnd",        &g_ev_pos_w_end,   40,    260},
    {"EvalComplexDiv",     &g_ev_cplx_div,    4000,  60000},
    {"EvalPawnMat",        &g_ev_pawn_mat,    -30000, 1200},     // negative: more pawns, smaller evaluation
    {"EvalMatBase",        &g_ev_mat_base,    20000,  5000000},  // large: almost no stretch with the material
    {"EvalOptComplexDiv",  &g_ev_opt_cplx,    100,   2000},
    {"EvalOptMatBase",     &g_ev_opt_base,    1000,  30000},
    {"EvalBucketOverride", CN_BUCKET_OVERRIDE, -1,   7},
    {"EvalFiftyMix",       &g_ev_fifty_mix,   0,     1024},
    {"EvalFiftyKeep",      &g_ev_fifty_keep,  0,     90},
};
}
#ifdef TRIUMV_FROZEN
// Live values against the compiled literals, once per search: a release build ignores a change of these options and
// says so. It sits where the variables are visible (between the #undef above and the #define below).
void nn_frozen_check() {
    struct FzRef { const int* p; int val; const char* name; };
    static const FzRef fz[] = {
        {&g_ev_psqt_w, 125, "EvalPsqtW"},          {&g_ev_pos_w, 131, "EvalPosW"},
        {&g_ev_pos_w_end, 131, "EvalPosWEnd"},     {&g_ev_cplx_div, 18198, "EvalComplexDiv"},
        {&g_ev_pawn_mat, 680, "EvalPawnMat"},      {&g_ev_mat_base, 102051, "EvalMatBase"},
        {&g_ev_opt_cplx, 432, "EvalOptComplexDiv"}, {&g_ev_opt_base, 5831, "EvalOptMatBase"},
        {&g_ev_fifty_mix, 0, "EvalFiftyMix"},      {&g_ev_fifty_keep, 20, "EvalFiftyKeep"},
    };
    static bool reported[sizeof(fz) / sizeof(fz[0])] = {false};
    for (std::size_t j = 0; j < sizeof(fz) / sizeof(fz[0]); j++) {
        if (reported[j] || *fz[j].p == fz[j].val) continue;
        reported[j] = true;
        printf("info string WARNING: %s is compiled as %d in this build; the value %d is ignored\n", fz[j].name,
               fz[j].val, *fz[j].p);
        fflush(stdout);
    }
}
#endif
#ifdef TRIUMV_FROZEN
#define g_ev_psqt_w 125
#define g_ev_pos_w 131
#define g_ev_pos_w_end 131
#define g_ev_cplx_div 18198
#define g_ev_pawn_mat 680
#define g_ev_mat_base 102051
#define g_ev_opt_cplx 432
#define g_ev_opt_base 5831
#define g_ev_fifty_mix 0
#define g_ev_fifty_keep 20
#endif
int         nn_eval_const_count(void) { return int(sizeof(g_eval_consts) / sizeof(g_eval_consts[0])); }
const char* nn_eval_const_name(int i) { return g_eval_consts[i].name; }
int         nn_eval_const_get(int i) { return *g_eval_consts[i].var; }
int         nn_eval_const_lo(int i) { return g_eval_consts[i].lo; }
int         nn_eval_const_hi(int i) { return g_eval_consts[i].hi; }
int         nn_set_eval_const(const char* name, int value) {
    for (const EvalConst& c : g_eval_consts)
        if (!std::strcmp(c.name, name)) {
            *c.var = value < c.lo ? c.lo : (value > c.hi ? c.hi : value);
            return 1;
        }
    return 0;
}
int nn_get_eval_scale(void) { return g_eval_scale_pct; }   // to print "score cp" without EvalScale

// Steps 4-5 on an unadjusted value from the evaluation cache, with the fifty-move counter of the position that reads
// it: the same operations as the end of nn_scale, so a cached evaluation equals a fresh one for any counter. `bucket`
// is the output bucket of that position, (pieces - 1) / 4.
int nn_finalize(int unadjusted, int rule50, int bucket) {
    return scale_and_bound(fifty_pull(unadjusted, rule50), g_eval_scale_b[bucket < 0 ? 0 : (bucket > 7 ? 7 : bucket)]);
}

// --- C2 signals (agent B) ---
// Two evaluation terms that only Consilium Plebis allows (21/10/2026), both applied to the two network outputs before the
// evaluation constants (nn_scale), in the incremental and in the from-scratch path alike (nnueverify compares them).
// The values are the search parameters C2Conv* and C2EvalExp0..7 (search/01_params.inc), set by nn_set_c2_eval at the
// start of each search. All 0 = nothing is computed and the evaluation is unchanged. OFF by default; SPRT pending.
//  - C2EvalExp0..7: a scale per expert of the side to move (learned routing). The old per-bucket scales (EvalScaleB)
//    follow the piece count and had no gradient in the END1 SPSA (01/10/2026); an expert of C2 is a region of the
//    routing table, and a capture that switches it compares values of two different experts.
//  - C2Conv*: endgame conversion, the author's idea. When the pawn-king subnetwork's opinion (what it adds to the
//    positional output, cn::pk_effect) favours one side by more than C2ConvFrom (capped at C2ConvCap), that side gains
//    C2ConvW/1024 of it in proportion to the non-pawn material already off the
//    board (62 = all of it on): trading pieces is worth more to the side the pawn structure and kings favour. The net
//    may know it already; the term is there to be measured.
namespace {
int  g_c2_conv[3] = {0, 0, 400};   // C2ConvW (/1024), C2ConvFrom, C2ConvCap (positional units)
int  g_c2_exp[8]  = {0, 0, 0, 0, 0, 0, 0, 0};
bool g_c2_exp_on  = false;
bool g_c2_on      = false;         // any of the terms on
// nn_want_pk_effect: keep cn::pk_effect per evaluation for the search. Nothing calls it any more (03/11/2026): in the
// release builds a constant, so the evaluation does not test it.
#ifdef TRIUMV_FROZEN
constexpr bool g_c2_want_effect = false;
#else
bool g_c2_want_effect = false;
#endif
// nn_want_x1: keep the l1 sums of every evaluation for nn_x1_signs (CorrNetW). The AVX-512 path keeps them anyway; a
// constant in the release builds, like the flag above (a release with CorrNetW on needs it true on the other paths).
#ifdef TRIUMV_FROZEN
constexpr bool g_want_x1 = false;
#else
bool g_want_x1 = false;
#endif

// pk_effect = what the pawn-king subnetwork adds to the positional output (cn::pk_effect), read only with C2ConvW on.
inline void c2_adjust(const NnBoard& board, int pk_effect, int& psqt, int& positional) {
    if (g_c2_exp_on) {
        const int k = g_c2_exp[cn::expert_of_board(board.view(), board.mover() == BLACK)];
        psqt       = int(std::int64_t(psqt) * (1024 + k) / 1024);
        positional = int(std::int64_t(positional) * (1024 + k) / 1024);
    }
    if (g_c2_conv[0]) {
        // How far the side to move is ahead (> 0) or behind on the pawn-king effect: the rule of the search version
        // (c2_conv_lean, consilium_bridge.h), with C2ConvFrom as its dead zone and C2ConvCap as its cap.
        const int lean = c2_conv_lean(pk_effect, g_c2_conv[1], g_c2_conv[2]);
        if (lean) {
            const int npm    = 3 * (board.count_type(NN_KNIGHT) + board.count_type(NN_BISHOP))
                             + 5 * board.count_type(NN_ROOK) + 9 * board.count_type(NN_QUEEN);
            const int traded = std::max(0, 62 - npm);
            positional += int(std::int64_t(lean) * g_c2_conv[0] * traded / (1024 * 62));
        }
    }
}
}  // namespace

#ifdef C2B_STATS
// Diagnostic only (-DC2B_STATS, one search thread): what the C2 signals look like over the evaluations of a run,
// printed at exit. |pk direct| against |positional|, the experts of the side to move, the balance of the signature bits
// and how many distinct 12- and 16-bit signatures occur.
struct C2Stats {
    unsigned long long n = 0, absd = 0, absPos = 0, hist[8] = {}, exp[8] = {}, bitOn[32] = {}, d12 = 0, d16 = 0;
    std::vector<unsigned char> seen12 = std::vector<unsigned char>(1 << 12), seen16 = std::vector<unsigned char>(1 << 16);
    void add(int d, int pos, int e, std::uint32_t sig) {
        n++;
        absd += std::abs(d);
        absPos += std::abs(pos);
        const int a = std::abs(d);
        hist[a < 25 ? 0 : a < 50 ? 1 : a < 100 ? 2 : a < 200 ? 3 : a < 400 ? 4 : a < 800 ? 5 : a < 1600 ? 6 : 7]++;
        exp[e]++;
        for (int i = 0; i < 32; i++) bitOn[i] += (sig >> i) & 1;
        if (!seen12[sig & 0xFFF]) seen12[sig & 0xFFF] = 1, d12++;
        if (!seen16[sig & 0xFFFF]) seen16[sig & 0xFFFF] = 1, d16++;
    }
    ~C2Stats() {
        if (!n) return;
        std::fprintf(stderr, "c2stats evals %llu mean|pkeffect| %.1f mean|positional| %.1f\n", n, double(absd) / n,
                     double(absPos) / n);
        std::fprintf(stderr, "c2stats |pkeffect| <25 <50 <100 <200 <400 <800 <1600 >=1600:");
        for (int i = 0; i < 8; i++) std::fprintf(stderr, " %.3f", double(hist[i]) / n);
        std::fprintf(stderr, "\nc2stats expert of side to move:");
        for (int i = 0; i < 8; i++) std::fprintf(stderr, " %.3f", double(exp[i]) / n);
        std::fprintf(stderr, "\nc2stats signature bit on-rate:");
        for (int i = 0; i < 32; i++) std::fprintf(stderr, " %.2f", double(bitOn[i]) / n);
        std::fprintf(stderr, "\nc2stats distinct signatures: 12 bits %llu / 4096, 16 bits %llu\n", d12, d16);
    }
};
C2Stats g_c2_stats;
#endif

void nn_set_c2_eval(const int* conv, const int* exp_scale) {
    bool any = false;
    for (int i = 0; i < 3; i++) g_c2_conv[i] = conv[i];
    for (int e = 0; e < 8; e++) any |= (g_c2_exp[e] = exp_scale[e]) != 0;
    g_c2_exp_on = any;
    g_c2_on     = any || g_c2_conv[0] != 0;
}

// ---------------------------------------------------------------------------
// Per-thread handle
// ---------------------------------------------------------------------------
namespace {

// Per-thread state: the module's handle (dirty stack + accumulators) on large pages, the optimism of the thread.
struct CnPos {
    cn::Handle* h;
    int         opt[2] = {0, 0};
    NnLast      last;
    int         pk_effect = 0;   // C2 signals (agent B): cn::pk_effect of the last evaluation that computed it
    int         last_pos  = 0;   // conversion push: positional output of the last incremental evaluation (its l1 sums
                                 // are in Handle::x1), for nn_pk_effect_fresh
    CnPos() {
        h = static_cast<cn::Handle*>(large_pages_alloc(sizeof(cn::Handle)));
        std::memset(static_cast<void*>(h), 0, sizeof(cn::Handle));
        cn::reset(*h);
    }
    ~CnPos() { large_pages_free(h); }
};

// Accumulators for evaluations from scratch (incremental off, nnueverify, "eval"): they do not touch the chain of
// the search.
struct CnScratch {
    alignas(64) cn::Acc acc[2];
};
CnScratch& scratch() {
    thread_local CnScratch s;
    return s;
}

inline int eval_full(const NnBoard& board, int rule50, int* raw_out = nullptr, const int* opt_local = nullptr,
                     int* psqt_out = nullptr, int* pos_out = nullptr) {
    const cn::Out o = cn::evaluate_refresh(NET_REF, board.view(), scratch().acc);
    if (raw_out) *raw_out = o.psqt + o.positional;
    if (psqt_out) *psqt_out = o.psqt;
    if (pos_out) *pos_out = o.positional;
    if (g_c2_on) {   // C2 evaluation terms (agent B), as in nn_pos_eval
        int ps = o.psqt, po = o.positional;
        c2_adjust(board, g_c2_conv[0] ? cn::pk_effect_refresh(NET_REF, board.view(), scratch().acc) : 0, ps, po);
        return nn_scale(board, ps, po, rule50, nullptr, opt_local);
    }
    return nn_scale(board, o.psqt, o.positional, rule50, nullptr, opt_local);
}

}  // namespace

void* nn_pos_create(void) { return new CnPos(); }
void  nn_pos_destroy(void* handle) { delete static_cast<CnPos*>(handle); }

int nn_last_unadjusted(void* handle) { return static_cast<CnPos*>(handle)->last.unadjusted; }
int nn_last_opt_base(void* handle) { return static_cast<CnPos*>(handle)->last.opt_base; }
int nn_last_opt_coeff(void* handle) { return static_cast<CnPos*>(handle)->last.opt_coeff; }

void nn_pos_set_optimism(void* handle, int w, int b) {
    CnPos* p  = static_cast<CnPos*>(handle);
    p->opt[0] = w;
    p->opt[1] = b;
}

void nn_pos_set(void* handle) { cn::reset(*static_cast<CnPos*>(handle)->h); }

NnStack* nn_pos_stack(void* handle) { return &static_cast<CnPos*>(handle)->h->stack; }

// The routing table of the calling thread (SmpRouteShift: the shifted copy on the odd helpers, else the network's).
const unsigned char* nn_router_table(void) { return cn::thread_router(); }
void nn_router_alt_build(int shift) { cn::router_alt_build(shift); }
// C2 speed (23/10/2026): the guard flag is raised before the thread's pointer, so a thread that reads the flag set
// reads its own pointer (written by itself); with the flag down, t_router is null on every thread anyway.
void nn_thread_router(int alt) {
    if (alt) cn::g_router_alt_used.store(true, std::memory_order_relaxed);
    cn::t_router = alt ? cn::g_router_alt : nullptr;
}

// --- C2 signals for the search (agent A) ---
int nn_pk_skip(void* handle, const unsigned long long* bb, int side_white) {
    if (!g_cn_view)
        return 0;
    const cn::Board b{reinterpret_cast<const std::uint64_t*>(bb), nullptr, nullptr, side_white ? 0 : 1};
    return cn::pk_skip(NET_REF, *static_cast<CnPos*>(handle)->h, b);
}
// pkNet = 0: the next probe finds a cache built for another network and clears it (cn::pk_outputs), the same path as
// a network change, so no second clearing code is needed.
void nn_pk_cache_clear(void* handle) {
    if (handle && static_cast<CnPos*>(handle)->h)
        static_cast<CnPos*>(handle)->h->pkNet = 0;
}

// --- C2 signals (agent B) ---
long long nn_pk_signature(void* handle, const unsigned long long* bb, int side_white, int bits, int compute) {
    const cn::Board b{reinterpret_cast<const std::uint64_t*>(bb), nullptr, nullptr, side_white ? 0 : 1};
    return cn::pk_signature(NET_REF, *static_cast<CnPos*>(handle)->h, b, bits, compute != 0);
}
#ifdef TRIUMV_FROZEN
void nn_want_pk_effect(int on) { (void)on; }
#else
void nn_want_pk_effect(int on) { g_c2_want_effect = on != 0; }
#endif
int  nn_last_pk_effect(void* handle) { return static_cast<CnPos*>(handle)->pk_effect; }

// CorrNetW: which of the 30 hidden neurons of the layer stack were above zero in the thread's last evaluation.
#ifdef TRIUMV_FROZEN
void nn_want_x1(int on) { (void)on; }
#else
void nn_want_x1(int on) { g_want_x1 = on != 0; }
#endif
unsigned nn_x1_signs(void* handle) {
    const std::int32_t* x1 = static_cast<CnPos*>(handle)->h->x1;
    unsigned            s  = 0;
    for (int o = 0; o < cn::L2 - 2; o++) s |= unsigned(x1[o] > 0) << o;
    return s;
}

// --- Conversion push, search version with the exact pawn-king effect (C2ConvPkSrc 1; 21/10/2026) ---
namespace {
// From scratch: accumulators refreshed, then the two tails (as eval_full).
inline int pk_effect_scratch(const NnBoard& board) {
    cn::evaluate_refresh(NET_REF, board.view(), scratch().acc);
    return cn::pk_effect_refresh(NET_REF, board.view(), scratch().acc);
}
// With nnueverify on (nnperft, or a search with the option), the value given to the search must equal the one from
// scratch for the same board: a value of another position counts as a mismatch, as for the evaluations.
inline int pk_effect_checked(const NnBoard& board, int eff, const char* path) {
    if (g_verify) {
        const int ref = pk_effect_scratch(board);
        if (ref != eff) {
            static int reported = 0;
            g_verify_bad++;
            if (reported++ < 64)
                std::printf("info string PK EFFECT MISMATCH %s search=%d scratch=%d\n", path, eff, ref);
            std::fflush(stdout);
        }
        g_verify_count++;   // nnperft's "verified" then counts these checks beside the evaluations
    }
    return eff;
}
}  // namespace

// Right after nn_pos_eval of this position: Handle::x1 and last_pos are its own, so one tail pass gives the effect;
// when C2ConvW (or nn_want_pk_effect) is on, that evaluation computed it already.
int nn_pk_effect_fresh(void* handle, const unsigned long long* bb, const unsigned long long* occ, const int* mailbox,
                       int side_white) {
    if (!g_cn_view)
        return 0;
#if !defined(USE_AVX512)
    // Without AVX-512 the last evaluation left the l1 sums in the handle only when the C2 terms asked for them
    // (cn::evaluate, want_x1; Houze Guo, 03/11/2026): otherwise this node is evaluated again with them.
    if (!(g_c2_conv[0] || g_c2_want_effect))
        return nn_pk_effect_now(handle, bb, occ, mailbox, side_white);
#endif
    CnPos*        p = static_cast<CnPos*>(handle);
    const NnBoard board(bb, occ, mailbox, side_white ? WHITE : BLACK);
    const int     eff = g_c2_conv[0] || g_c2_want_effect ? p->pk_effect
                                                         : cn::pk_effect(NET_REF, *p->h, board.view(), p->last_pos);
    return pk_effect_checked(board, eff, "fresh");
}
// At any time (static evaluation from the hash, excluded move): the accumulators of the top state brought up to date
// and the forward pass rerun, so x1 is this position's; without the incremental path, from scratch.
int nn_pk_effect_now(void* handle, const unsigned long long* bb, const unsigned long long* occ, const int* mailbox,
                     int side_white) {
    if (!g_cn_view)
        return 0;
    CnPos*        p = static_cast<CnPos*>(handle);
    const NnBoard board(bb, occ, mailbox, side_white ? WHITE : BLACK);
    if (!g_incremental)
        return pk_effect_scratch(board);
    const cn::Out o = cn::evaluate(NET_REF, *p->h, board.view(), true);   // pk_effect reads the l1 sums
    p->last_pos     = o.positional;
    return pk_effect_checked(board, cn::pk_effect(NET_REF, *p->h, board.view(), o.positional), "now");
}

void nn_prefetch_move(const unsigned long long* bb, int phase, int pc, int from, int toPc, int to, int capPc,
                      int capSq) {
    if (g_cn_view)
        cn::prefetch_move(NET_REF, reinterpret_cast<const std::uint64_t*>(bb), phase, pc, from, toPc, to, capPc, capSq);
}

// NumaSpread (03/11/2026): the thread's handle (accumulators, king, pawn and pawn-king caches) moved to the memory of
// `node`, the node the calling thread now runs on; the same contents, so the same values. The one pointer into the
// handle (pkLast) is moved with it. Windows asks for memory of the node; Linux places a page on the node of the thread
// that first writes it, so the calling thread (already bound to `node`) allocates and copies.
void nn_pos_rehome(void* handle, int node) {
#if defined(_WIN32) || defined(__linux__)
    CnPos*            p   = static_cast<CnPos*>(handle);
    const std::size_t sz  = sizeof(cn::Handle);
#if defined(_WIN32)
    void*             mem = with_lock_memory_privilege(
      [&](std::size_t lp) -> void* {
          return VirtualAllocExNuma(GetCurrentProcess(), nullptr, (sz + lp - 1) / lp * lp,
                                    MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE, DWORD(node));
      },
      []() -> void* { return nullptr; });
    if (!mem) mem = VirtualAllocExNuma(GetCurrentProcess(), nullptr, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, DWORD(node));
#else
    // The allocator can hand back memory another thread has touched (already placed elsewhere): mbind with
    // MPOL_MF_MOVE prefers `node` for the range and moves the pages already there (raw syscall, no libnuma; if it is
    // refused, the first write below still places the untouched pages).
    void* mem = large_pages_alloc(sz);
    if (mem && node >= 0 && node < 64) {
        constexpr std::size_t HP = 2 * 1024 * 1024;   // large_pages_alloc aligns and rounds to this
        unsigned long         mask = 1ul << node;
        syscall(SYS_mbind, mem, (sz + HP - 1) / HP * HP, 1 /* MPOL_PREFERRED */, &mask, 64, 2 /* MPOL_MF_MOVE */);
    }
#endif
    if (!mem) return;
    cn::Handle* old = p->h;
    cn::Handle* h   = static_cast<cn::Handle*>(mem);
    std::memcpy(static_cast<void*>(h), old, sz);
    if (h->pkLast)
        h->pkLast = reinterpret_cast<const std::int32_t*>(reinterpret_cast<const char*>(h->pkLast)
                                                          - reinterpret_cast<const char*>(old) + reinterpret_cast<const char*>(h));
    p->h = h;
    large_pages_free(old);
#else
    (void)handle, (void)node;
#endif
}

// NumaSpread (03/11/2026): the copy of the network on `node` for the calling thread, which runs on that node; made once
// per node and network (TriumvShm::attach_extra: the area of the node, created from the loaded copy).
void nn_bind_node_net(int node) {
#if defined(TRIUMV_SHARED_NET) && (defined(_WIN32) || defined(__linux__))
    static const cn::Net* per_node[64];
    static int            per_gen[64];
    static std::mutex     mx;
    if (!g_cn_view || node < 0 || node >= 64) {
        t_net = nullptr;
        return;
    }
    const int gen = g_net_gen.load();
    std::lock_guard<std::mutex> lk(mx);
    if (per_gen[node] != gen) {
        std::string st;
        const void* sh = TriumvShm::attach_extra(g_cn_view, sizeof(cn::Net),
                                                 g_cn_view->contentHash ^ std::uint64_t(sizeof(cn::Net)), st);
        per_node[node] = sh ? static_cast<const cn::Net*>(sh) : g_cn_view;
        per_gen[node]  = gen;
        std::printf("info string NUMA node %d network: %s\n", node, st.c_str());
        std::fflush(stdout);
    }
    t_net = per_node[node];
#else
    (void)node;
#endif
}

void nn_set_pk_stale(void* handle, int on) { static_cast<CnPos*>(handle)->h->pkStale = on; }

thread_local int nn_last_disagree = -1;

int nn_pos_eval(void* handle, const unsigned long long* bb, const unsigned long long* occ, const int* mailbox,
                int side_white, int rule50) {
    CnPos*        p = static_cast<CnPos*>(handle);
    const NnBoard board(bb, occ, mailbox, side_white ? WHITE : BLACK);
    const int*    opt = g_opt_per_thread ? p->opt : nullptr;
    if (!g_incremental)
        return eval_full(board, rule50, nullptr, opt);

    // The l1 sums are kept only when the C2 terms below read them (Houze Guo, 03/11/2026; the AVX-512 path keeps them
    // always, cn::evaluate). C2B_STATS reads them at every evaluation.
#ifdef C2B_STATS
    const bool want_x1 = true;
#else
    const bool want_x1 = g_c2_on || g_c2_want_effect || g_want_x1;
#endif
    const cn::Out o   = cn::evaluate(NET_REF, *p->h, board.view(), want_x1);
    int ps = o.psqt, po = o.positional;
    p->last_pos = o.positional;   // conversion push (nn_pk_effect_fresh): one store
    if (g_c2_on | g_c2_want_effect) {   // C2 evaluation terms (agent B); pk_effect reads the l1 sums evaluate just left
        const bool need = g_c2_conv[0] || g_c2_want_effect;
        const int  eff  = need ? cn::pk_effect(NET_REF, *p->h, board.view(), o.positional) : 0;
        p->pk_effect    = eff;
        if (g_c2_on) c2_adjust(board, eff, ps, po);
    }
#ifdef C2B_STATS
    g_c2_stats.add(cn::pk_effect(NET_REF, *p->h, board.view(), o.positional), o.positional,
                   cn::expert_of_board(board.view(), board.mover() == BLACK),
                   std::uint32_t(cn::pk_signature(NET_REF, *p->h, board.view(), 32, true)));
#endif
    const int inc = nn_scale(board, ps, po, rule50, nullptr, opt);
    nn_last_disagree  = std::abs(int(o.psqt) - int(o.positional));

    if (g_verify) {
        // Same position from scratch: final values and both accumulators must be identical.
        const int  full    = eval_full(board, rule50, nullptr, opt);
        const bool accSame = cn::same_accumulators(*p->h, scratch().acc);
        if (inc != full || !accSame) {
            static int reported = 0;
            g_verify_bad++;
            if (reported++ < 64)
                std::printf("info string NNUE MISMATCH stati=%d inc=%d full=%d acc=%s\n", p->h->stack.size, inc, full,
                            accSame ? "uguale" : "DIVERSO");
            std::fflush(stdout);
        }
        g_verify_count++;
    }
    return inc;
}

unsigned long long nn_verify_count(void) { return g_verify_count; }
unsigned long long nn_verify_bad(void) { return g_verify_bad; }

// "eval" command and the golden-reference batch: from scratch on the engine's global board (mailbox rebuilt here).
static void mailbox_of(const unsigned long long* bb, int* mb) {
    for (int s = 0; s < 64; ++s) mb[s] = -1;
    for (int pc = 0; pc < 12; ++pc)
        for (Bitboard b = bb[pc]; b;) mb[pop_lsb(b)] = pc;
}

int nn_eval(int side_white, const unsigned long long* bb, const unsigned long long* occ, int rule50, int* raw_out) {
    int mb[64];
    mailbox_of(bb, mb);
    const NnBoard board(bb, occ, mb, side_white ? WHITE : BLACK);
    return eval_full(board, rule50, raw_out);
}

int nn_eval_parts(int side_white, const unsigned long long* bb, const unsigned long long* occ, int rule50,
                  int* psqt_out, int* pos_out) {
    int mb[64];
    mailbox_of(bb, mb);
    const NnBoard board(bb, occ, mb, side_white ? WHITE : BLACK);
    return eval_full(board, rule50, nullptr, nullptr, psqt_out, pos_out);
}

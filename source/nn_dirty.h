// nn_dirty.h -- What a move changes for the network, written by the engine's make (07/10/2026, single board v2).
//
// A single numbering: ENGINE squares (a8 = 0 .. h1 = 63, 64 = no square) and engine codes (0..11 = P N B R Q K
// p n b r q k). The make writes here the moved piece (NnDirtyPiece), on a stack with one state per move (NnStack); the
// touched pawns (NnDirtyPawns) and the threat tuples that change (NnDirtyThreats) are added by the engine before an
// evaluation (search/06_nndirty.inc, nn_dirty_catch_up). The network (consilium/) reads them as they are. The tables of the
// feature indices work directly in this numbering (consilium/cn.cpp): no conversion
// at runtime.
//
// The file is included by the engine (threads.cpp, through consilium_bridge.h) and by the network (consilium/cn.h): simple types only,
// no type or macro of either one. The same layout in every translation unit: no field depends
// on macros.

#ifndef NN_DIRTY_H_INCLUDED
#define NN_DIRTY_H_INCLUDED

#include <cstddef>
#include <cstdint>

constexpr int NN_SQ_NONE = 64;  // "no square" (equal to the engine's no_sq)

// Engine piece types: code % 6. Black is white + 6.
enum : int { NN_PAWN = 0, NN_KNIGHT = 1, NN_BISHOP = 2, NN_ROOK = 3, NN_QUEEN = 4, NN_KING = 5, NN_BLACK = 6 };

// Stack of the dirty entries: one state for every move of the search from the root (null moves add no states: the
// board does not change). 247 = MAX_PLY (246) of the network + the root; the search does not exceed max_ply + 16 plies.
constexpr int NN_STACK_SIZE = 247;

struct NnDirtyPiece {
    std::uint8_t pc;          // moving piece
    std::uint8_t from, to;    // to = NN_SQ_NONE for promotions (the arriving piece is add_pc)
    std::uint8_t remove_pc;   // captured piece, or castling rook leaving
    std::uint8_t remove_sq;   // its square (en passant: the pawn's square, not `to`); NN_SQ_NONE if none
    std::uint8_t add_pc;      // promoted piece, or castling rook arriving
    std::uint8_t add_sq;      // its square; NN_SQ_NONE if none. Castling = to and add_sq both present.
    std::uint8_t phase;       // material band (HalfKA with experts) of the position AFTER the move
    std::uint8_t phaseChanged;  // 1 if a capture changed the band: accumulator refresh, as for the king
};

// A threat tuple in 32 bits: attacker square (0-7), attacked square (8-15), attacked piece (16-19),
// attacking piece (20-23), 1 = the threat appears / 0 = disappears (31).
constexpr int NN_THR_PCSQ = 0, NN_THR_TSQ = 8, NN_THR_TPC = 16, NN_THR_PC = 20;
inline std::uint32_t nn_threat(int pc, int tpc, int pcSq, int tSq, bool add) {
    return (std::uint32_t(add) << 31) | (std::uint32_t(pc) << NN_THR_PC) | (std::uint32_t(tpc) << NN_THR_TPC)
         | (std::uint32_t(tSq) << NN_THR_TSQ) | std::uint32_t(pcSq);
}

// A piece takes part in at most 8 outgoing and 16 incoming threats, and by moving it uncovers at most 8: a move
// that is not castling changes at most (8 + 16) * 3 + 8 = 80 of them, castling 36. The 16 extra entries take the
// 16-tuple vector writes beyond the count (AVX-512 emission).
constexpr int NN_THREATS_MAX = 96;
struct NnDirtyThreats {
    std::uint32_t n;
    std::uint32_t list[NN_THREATS_MAX];
};

// The pawns a move removes (at most 2: pawn takes pawn, en passant) and adds (at most 1; a promotion
// none), with the pawn bitboards BEFORE the move, for the PawnPair and PassedPawns blocks.
struct NnDirtyPawns {
    std::uint64_t before[2];      // white, black pawns before the move
    std::uint8_t  removedSq[2], removedC[2];
    std::uint8_t  addedSq, addedC;  // addedSq = NN_SQ_NONE if none
    std::uint8_t  nRemoved;
    std::uint8_t  any;            // 0 = the move touches no pawns: nothing to do downstream
};

// (Optional graft blocks, PassedRel and PassedState among others, with the mask of the loaded network, the diffs
// per state in NnState and the entry lists in NnStack: tried on 09-10/10/2026, none goes into 8.0, removed on
// 10/10/2026. Code in _backup/Triumviratus_8.0_pre_rimozione_graft_2026-10-10 and in the training repo,
// 04_consilium/graft_engine_storico.)

// Generation of the network weights: Network::read_parameters increments it at every network read. The "pe" cache of the pawn
// blocks (update_accumulator_refresh_cache) is thread_local and the key is made of pawns and orientation only: on a
// thread that survives a network change (the UCI thread: eval, nnueverify) an entry written with the old weights
// would give a wrong hit, so the entry also carries this generation (introduced with the graft blocks on
// 10/10/2026, kept because it holds for every network).
inline unsigned nn_net_epoch = 0;

// avx512icl (05/11/2026, R26 §6): 512 bytes on a 64-byte boundary. The threat list starts a cache line in every state
// (with 440 bytes its place in the line changed from state to state and most 64-byte stores of the vector emission
// split two lines), and the passers of a pawn event ([0 before, 1 after][white, black]) have their own place in the
// line of the dirty piece and the pawns, instead of NnStack::v1PassW/B, 108 KB away. The layout depends on
// USE_AVX512ICL, a build flag of the variant (universal/ubuild_lib.ps1, Makefile) and so the same in every
// translation unit. Measured with the byte board on the Zen 4; neutral on Skylake-SP, so the other variants keep 440.
#if defined(USE_AVX512ICL)
struct alignas(64) NnState {
    NnDirtyPiece  dp;            // 9 bytes
    std::uint8_t  computed[2];    // accumulator of this state computed, per perspective (white, black)
    std::uint8_t  idx;            // position in the stack (nn_stack_of); in the padding, same size
    NnDirtyPawns  pawns;
    std::uint64_t key;           // -DTRIUMV_VERIFY_NNSYNC: piece key after the move (synchronisation check)
    std::uint64_t pass[2][2];    // passers [0 before, 1 after][white, black] of a pawn event
    std::uint8_t  pad_[44];      // threats.list at offset 128
    NnDirtyThreats threats;
};
static_assert(offsetof(NnState, threats) + offsetof(NnDirtyThreats, list) == 128, "NnState: list off its line");
static_assert(sizeof(NnState) == 512, "NnState: 512 bytes");
#else
struct NnState {
    NnDirtyPiece  dp;            // 9 bytes
    std::uint8_t  computed[2];    // accumulator of this state computed, per perspective (white, black)
    std::uint8_t  idx;            // position in the stack (nn_stack_of); in the padding, same size
    NnDirtyPawns  pawns;
    std::uint64_t key;           // -DTRIUMV_VERIFY_NNSYNC: piece key after the move (synchronisation check)
    NnDirtyThreats threats;
};
#endif
static_assert(offsetof(NnState, pawns) == 16, "NnState: idx deve stare nel padding prima di `pawns`");

struct NnStack {
    int     size;   // states present; state 0 is the root
    int     ready;  // the states [ready, size) do not have threats and pawns yet (nn_dirty_catch_up)
    NnState st[NN_STACK_SIZE];
    // R4 (10/10/2026, docs/audit_8.0/GRAFT_PASSEDREL_COSTO3.md §4.1): white and black passers after the move of state i,
    // for PassedPawns v1 with EVERY network. Written by the catch-up (nn_dirty_catch_up: two passers() per pawn event,
    // copy otherwise); the accumulator takes before/after from [i - 1] and [i] instead of redoing eight passers() per
    // update. v1PassW[0] = ~0 = root to be computed (AccumulatorStack::reset). Separate colours: a pawn that
    // captures a passer on the same square leaves the square passed but changes the colour.
    std::uint64_t v1PassW[NN_STACK_SIZE];
    std::uint64_t v1PassB[NN_STACK_SIZE];
};

// The accumulator receives the states by reference: the passers of a state (R4) are found from its position in the
// stack (NnState::idx, written by the make, nn_make_dirty; the root by reset), with address arithmetic: no
// thread_local (with MinGW it goes through a call, emutls).
inline const NnStack& nn_stack_of(const NnState& s) {
    const NnState* base = &s - s.idx;
    return *reinterpret_cast<const NnStack*>(reinterpret_cast<const char*>(base) - offsetof(NnStack, st));
}

#endif  // NN_DIRTY_H_INCLUDED

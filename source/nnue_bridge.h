#pragma once
#ifndef SF_BRIDGE_H
#define SF_BRIDGE_H

// Thin bridge to the TRANN1 NNUE (in nnue/) — Triumviratus Rubicon Alea NNUE 1:
// FOUR input blocks (FullThreats + HalfKAv2_hm + PawnPair + PassedPawns), L1=1024,
// L2/L3, 8 LayerStacks. The evaluation machinery is derived from Stockfish-master's
// SFNNv13 (GPLv3, attribution in COPYING/README); the last two blocks are ours, so
// the net format is NOT SFNNv13 and an SFNNv13 net cannot be loaded (the reader
// accepts only the TRANN1 4-block hash and the 3-block v2 hash, which zero-fills
// the PassedPawns segment). Keeps all Stockfish headers/types/macros out of the
// rest of the engine so there is no clash with Triumviratus's own globals.

// Initialize the shared substrate tables: bitboards + slider-magic attacks.
// (Attacks::init() is SEPARATE from Bitboards::init() in the master.)
void nn_init_tables(void);

// Nome del net di default (macro EvalFileDefaultName, definita in nnue/evaluate.h).
// Esposto qui perche' il resto del motore NON include gli header Stockfish: un solo
// punto di verita' per il nome, niente stringhe duplicate da tenere in sync.
const char* nn_default_net_name(void);

// Load the TRANN1 network from a file path. Returns 1 on success (file opened +
// arch-hash verified), 0 otherwise. Must be called once at startup before any
// nn_pos_create() (the per-handle accumulator caches are built from the net).
int nn_load_net(const char* net_path);

// Reload the network at runtime (UCI option "EvalFile"). Safe to call when no
// search is running. Returns 1 on success, 0 if the path could not be loaded.
int nn_reload_big(const char* net_path);

// Toggle the per-thread accumulator refresh cache ("finny tables"). No-op in the
// M2 full-refresh path (the master always uses the cache to accelerate refresh;
// the eval value is identical regardless). Kept for API stability.
void nn_set_finny(int on);

// DIAGNOSTIC ("accstats" UCI command). No-op stub in M2 (the v13 incremental
// refresh counters arrive with M3).
void nn_acc_stats(void);

// M3 incremental eval. on != 0 uses the master AccumulatorStack chain (DirtyPiece +
// DirtyThreats per ply) instead of a full refresh per node. Default OFF (the
// validated M2 full-refresh) until the verify oracle is clean. The handle MUST be
// re-set (nn_pos_set) after toggling so its board/accumulator chain is consistent.
void nn_set_incremental(int on);

// DEBUG: cross-check incremental == full-refresh at every leaf eval (prints the
// first mismatches). Halves NPS; single-thread recommended. Default OFF.
void nn_set_verify(int on);

// N-1 (2026-07-05): lazy mirror apply. nn_pos_do/nn_pos_do_null normally defer the
// SF-mirror board mutation + DirtyThreats computation until an nn_pos_eval actually
// needs them (many nodes never evaluate: TT cutoffs return before td_evaluate,
// in-check nodes skip eval entirely) -> those plies are undone for free, never
// mirrored at all. Default ON. Toggle OFF to fall back to the pre-N1 eager apply
// (every legal move mirrored immediately in nn_pos_do), for bisection.
void nn_set_lazy_mirror(int on);

// Eval output scale in PERCENT (default 100 = x1.0). The SFNNv13 cp formula lands on
// a different scale than the engine's SPSA-tuned (for SFNNv10) search margins expect;
// this re-aligns the two. UCI option "EvalScale". Diagnostic sweep at fixed depth.
void nn_set_eval_scale(int pct);
int  nn_get_eval_scale(void);   // current EvalScale %% (per normalizzare 'score cp' in stampa)
int  nn_last_unadjusted(void);  // unadjusted (pre-rule50/scale) dell'ultima nn_scale (thread-local)
int  nn_finalize(int unadjusted, int rule50);  // ricostruisce l'eval finale dall'unadjusted

// Evaluate a position from scratch (full refresh).
//   side_white : 1 if white is to move, 0 if black
//   pieces[i]  : Stockfish piece code (W_PAWN=1..W_KING=6, B_PAWN=9..B_KING=14)
//   squares[i] : Stockfish square (a1=0, b1=1, ... h8=63)
//   count      : number of entries in pieces[]/squares[]
//   rule50     : halfmove (fifty-move) clock
// Returns the evaluation (stm-relative, Stockfish internal units == the engine's
// eval scale). Stateless oracle for the "eval" command + the NNUE_VERIFY check.
int nn_eval(int side_white, const int* pieces, const int* squares, int count, int rule50,
            int* raw_out = nullptr);

// ---------------------------------------------------------------------------
// Per-thread incremental position handle. In M2 it only tracks side-to-move and
// the fifty-move clock across make/undo (nn_pos_eval receives the board via the
// engine's own bitboards and rebuilds the SF Position each call). M3 will hang
// the real AccumulatorStack chain off the SfMove dirty lists.
// ---------------------------------------------------------------------------

// Description of a single move in Stockfish encoding (the moving piece must be
// movedPiece). M2 reads only rule50; the rest is populated by the engine for M3.
struct SfMove {
    int movedPiece;     // SF code of the moving piece
    int from;           // SF square the piece leaves
    int to;             // SF square the piece arrives on
    int promoPiece;     // SF code of the promotion result, or 0 if not a promotion
    int capturedPiece;  // SF code of the captured piece, or 0 if no capture
    int capturedSq;     // SF square of the captured piece (= to for a normal
                        //   capture, the e.p. pawn square for en passant), or -1
    int rookPiece;      // SF code of the castling rook, or 0 if not castling
    int rookFrom;       // SF square the rook leaves (castling), or -1
    int rookTo;         // SF square the rook arrives on (castling), or -1
    int rule50;         // fifty-move clock value for the resulting position
};

// Create / destroy a per-thread incremental position handle (opaque).
void* nn_pos_create(void);
void  nn_pos_destroy(void* handle);

// (Re)initialise the handle for a search from the root: set side-to-move + the
// fifty-move clock. Call at the start of a search from the root.
void  nn_pos_set(void* handle, int side_white, const int* pieces,
                 const int* squares, int count, int rule50);

// Apply / retract a move on the handle (track stm + rule50 for nn_pos_eval).
void  nn_pos_do(void* handle, const struct SfMove* m);
void  nn_pos_do_null(void* handle, int rule50);
void  nn_pos_undo(void* handle);

// Evaluate the current position (centipawns / internal units, stm-relative).
// bb / occ are the ENGINE's own bitboards (bb[12] in P,N,B,R,Q,K,p,n,b,r,q,k
// order; occ[3] = white,black,both) in the engine's a8=0..h1=63 layout. They are
// vflip'd + remapped into the SF board the NNUE reads.
int   nn_pos_eval(void* handle, const unsigned long long* bb, const unsigned long long* occ);

#endif // SF_BRIDGE_H

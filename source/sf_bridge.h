#pragma once
#ifndef SF_BRIDGE_H
#define SF_BRIDGE_H

// Thin bridge to the standalone Stockfish HalfKAv2_hm NNUE probe (in sfnnue/).
// Keeps all Stockfish headers/types out of the rest of the engine so there is
// no clash with Triumviratus's global macros/enums.

// Load the two networks (big + small). Call once at startup.
void sf_init(const char* big_net, const char* small_net);

// Toggle the per-thread accumulator refresh cache ("finny tables"). on != 0
// enables it. Default OFF => byte-identical to the pre-finny engine. Pure NPS
// optimisation: the evaluation value is bit-identical with it on or off.
void sf_set_finny(int on);

// DIAGNOSTIC: print accumulator refresh vs incremental counts (and refresh %)
// since the last call, then reset. Triggered by the UCI command "accstats".
void sf_acc_stats(void);

// Evaluate a position from scratch with the HalfKAv2_hm net (full refresh).
//   side_white : 1 if white is to move, 0 if black
//   pieces[i]  : Stockfish piece code (W_PAWN=1..W_KING=6, B_PAWN=9..B_KING=14)
//   squares[i] : Stockfish square (a1=0, b1=1, ... h8=63)
//   count      : number of entries in pieces[]/squares[]
//   rule50     : halfmove (fifty-move) clock
// Returns the evaluation in centipawns, relative to the side to move.
// This is the original stateless path; kept as a fallback / verification oracle.
int sf_eval(int side_white, const int* pieces, const int* squares, int count, int rule50);

// ---------------------------------------------------------------------------
// Incremental NNUE: a per-thread Stockfish Position mirror with a StateInfo
// stack, so the expensive accumulator is updated only for the pieces that
// change on each move (reusing Stockfish's own incremental machinery, including
// the HalfKAv2_hm king-bucket refresh). All squares/pieces are in Stockfish
// encoding (translate with sf_piece_code[]/nnue_squares[] before calling).
// ---------------------------------------------------------------------------

// Description of a single move in Stockfish encoding. The moving piece must be
// movedPiece (it becomes dirtyPiece[0], which Stockfish uses to detect a king
// move that forces a per-perspective refresh).
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
void* sf_pos_create(void);
void  sf_pos_destroy(void* handle);

// (Re)initialise the mirror from a full piece list (full refresh, resets the
// StateInfo stack to the root). Call at the start of a search from the root.
void  sf_pos_set(void* handle, int side_white, const int* pieces,
                 const int* squares, int count, int rule50);

// Apply / retract a move on the mirror, keeping the accumulator chain in sync.
// sf_pos_undo retracts the most recent move OR null move.
void  sf_pos_do(void* handle, const struct SfMove* m);
void  sf_pos_do_null(void* handle, int rule50);
void  sf_pos_undo(void* handle);

// Evaluate the current mirror position incrementally (centipawns, stm-relative).
// bb / occ are the ENGINE's own bitboards (bb[12] in P,N,B,R,Q,K,p,n,b,r,q,k
// order; occ[3] = white,black,both) in the engine's a8=0..h1=63 square layout.
// They are used only in SINGLE-BOARD mode to reconstruct the SF board the NNUE
// reads; in dual-board mode they are ignored (may be nullptr).
int   sf_pos_eval(void* handle, const unsigned long long* bb, const unsigned long long* occ);

#endif // SF_BRIDGE_H

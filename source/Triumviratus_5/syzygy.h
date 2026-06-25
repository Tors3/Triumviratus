#pragma once
#ifndef SYZYGY_H
#define SYZYGY_H

#include "defs.h"   // U64 and the board enums

// Forward declaration: full definition lives in threads.h. Taking it by
// reference here keeps syzygy.h light (no thread headers pulled into callers).
struct ThreadData;

// ---------------------------------------------------------------------------
// Syzygy tablebase support (thin wrapper over the Fathom probing library).
//
// CONVENTIONS BRIDGE. The engine uses squares a8=0..h1=63 (defs.h) and stores
// the board as 12 piece bitboards + 3 occupancies. Fathom (like python-chess)
// uses a1=0..h8=63 and takes per-piece-TYPE bitboards. The wrapper converts:
//   * square  : eng_sq ^ 56            (vertical flip; it is an involution)
//   * bitboard: byteswap64             (vertical flip of the whole board)
//   * side    : Fathom turn==true means White to move; engine side==white(0)
// A bug here silently throws games away, so every probe is gated and the root
// move is re-matched against generate_moves before it is ever played.
// ---------------------------------------------------------------------------

// Initialize tablebases from a Fathom-style search path (one or more
// directories separated by ';' on Windows). An empty or "<empty>" path
// disables probing. Returns true if at least one tablebase file was found.
// Safe to call repeatedly to switch path on/off.
bool syzygy_init(const char* path);

// Release tablebase memory. Safe to call even if never initialized.
void syzygy_free();

// Largest piece count (kings included) covered by the loaded tablebases.
// Returns 0 when probing is disabled / no files were found. Cheap (a global
// read) so callers can use it as the "enabled" check before counting pieces.
unsigned syzygy_max_pieces();

// In-search WDL probe for a ThreadData node. On a successful probe writes a
// side-to-move-relative score (engine mate scale) into `score` and returns
// true. The caller MUST have already ensured: ply > 0, td.castle == 0, and
// count_bits(td.occupancies[both]) <= syzygy_max_pieces().
bool syzygy_probe_wdl(const ThreadData& td, int ply, int& score);

// Root DTZ probe on the GLOBAL board. On success writes the DTZ-optimal engine
// move (already matched against generate_moves) into `best_move`, a
// side-to-move-relative score into `score`, and returns true. Returns false if
// probing is disabled, the position is out of range, the position still has
// castling rights, the DTZ tables are missing, or the move could not be
// matched (in which case the caller falls back to a normal search).
bool syzygy_probe_root(int& best_move, int& score);

#endif // SYZYGY_H

#pragma once
#ifndef EVALUATION_H
#define EVALUATION_H

// convert BBC piece code to Stockfish piece codes
extern int nnue_pieces[12];

// convert BBC square indices to Stockfish indices
extern int nnue_squares[64];

extern int evaluate();

#endif

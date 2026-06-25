#pragma once
#ifndef PERFT_H
#define PERFT_H

#include "defs.h"
#include "movegen.h"
#include "misc.h"

extern U64 nodes;

extern void perft_driver(int depth);
extern void perft_test(int depth);

#endif

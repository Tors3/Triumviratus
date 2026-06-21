/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2024 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef EVALUATE_H_INCLUDED
#define EVALUATE_H_INCLUDED

#include <string>
#include <unordered_map>

#include "types.h"

namespace Stockfish {

class Position;

namespace Eval {

namespace NNUE {
struct AccumulatorCaches;
// (Re)initialise a per-thread AccumulatorCaches to an empty board (biases only).
void clear_accumulator_caches(AccumulatorCaches& caches);
}

int   simple_eval(const Position& pos, Color c);
Value evaluate(const Position& pos, NNUE::AccumulatorCaches* caches = nullptr);

// 4.2 OWN-NET: default net names point to OUR nets (loaded from disk; embedding is
// disabled in evaluate.cpp so neither is compiled into the binary). The SF SHA-name
// convention does not apply since we no longer ship/embed a Stockfish net.
#define EvalFileDefaultNameBig "nn-rubicon-v1.nnue"
#define EvalFileDefaultNameSmall "mini-rubicon-v1.nnue"

struct EvalFile {
    // UCI option name
    std::string optionName;
    // Default net name, will use one of the macros above
    std::string defaultName;
    // Selected net name, either via uci option or default
    std::string current;
    // Net description extracted from the net file
    std::string netDescription;
};

namespace NNUE {

enum NetSize : int;

using EvalFiles = std::unordered_map<Eval::NNUE::NetSize, EvalFile>;

EvalFiles load_networks(const std::string&, EvalFiles);

}  // namespace NNUE

}  // namespace Eval

}  // namespace Stockfish

#endif  // #ifndef EVALUATE_H_INCLUDED

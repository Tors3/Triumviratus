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

// Class for difference calculation of NNUE evaluation function

#ifndef NNUE_ACCUMULATOR_H_INCLUDED
#define NNUE_ACCUMULATOR_H_INCLUDED

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../types.h"
#include "nnue_architecture.h"
#include "nnue_common.h"

namespace Stockfish::Eval::NNUE {

// DIAGNOSTIC counters (finny-tables analysis): how many accumulator
// perspective-updates take the full-refresh path vs the cheap incremental path.
// If refreshes are a small fraction, the finny cache cannot speed the engine up.
// Single-thread use (the smoke test runs Threads=1); not shipped behaviour.
extern unsigned long long g_dbg_refresh;
extern unsigned long long g_dbg_incremental;

// Class that holds the result of affine transformation of input features
template<IndexType Size>
struct alignas(CacheLineSize) Accumulator {
    std::int16_t accumulation[2][Size];
    std::int32_t psqtAccumulation[2][PSQTBuckets];
    bool         computed[2];
};

// AccumulatorCaches ("finny tables"): per-thread cache of computed
// feature-transformer accumulations, keyed by (king square, perspective). On a
// full refresh we look up the entry for the current king square, compute the
// piece diff vs the bitboards stored in the entry, apply only that diff, then
// copy the result into the position accumulator. This turns the costly full
// refresh (rebuild from biases over all active features, triggered on every
// king-bucket change) into a cheap incremental update. The cached value is
// always correct because byColorBB/byTypeBB record exactly which pieces produced
// the stored accumulation. One AccumulatorCaches lives per search thread (in the
// per-thread SfPos mirror), so concurrent searches never share cache state.
struct AccumulatorCaches {

    template<IndexType Size>
    struct alignas(CacheLineSize) Cache {

        struct alignas(CacheLineSize) Entry {
            std::int16_t accumulation[Size];
            std::int32_t psqtAccumulation[PSQTBuckets];
            Bitboard     byColorBB[COLOR_NB];
            Bitboard     byTypeBB[PIECE_TYPE_NB];

            // Reset to an empty board (no pieces) with just the biases, so the
            // first refresh against this entry equals a full refresh.
            void clear(const std::int16_t* biases) {
                std::memcpy(accumulation, biases, sizeof(accumulation));
                std::memset(reinterpret_cast<std::uint8_t*>(this)
                              + offsetof(Entry, psqtAccumulation),
                            0, sizeof(Entry) - offsetof(Entry, psqtAccumulation));
            }
        };

        void clear(const std::int16_t* biases) {
            for (auto& perColor : entries)
                for (auto& entry : perColor)
                    entry.clear(biases);
        }

        std::array<Entry, COLOR_NB>& operator[](Square sq) { return entries[sq]; }

        std::array<std::array<Entry, COLOR_NB>, SQUARE_NB> entries;
    };

    Cache<TransformedFeatureDimensionsBig>   big;
    Cache<TransformedFeatureDimensionsSmall> small;
};

}  // namespace Stockfish::Eval::NNUE

#endif  // NNUE_ACCUMULATOR_H_INCLUDED

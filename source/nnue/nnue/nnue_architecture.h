/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

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

// Input features and network structure used in NNUE evaluation function

#ifndef NNUE_ARCHITECTURE_H_INCLUDED
#define NNUE_ARCHITECTURE_H_INCLUDED

#include <cstdint>
#include <cstring>
#include <sstream>
#include <iosfwd>

#include "features/half_ka_v2_hm.h"
#include "features/full_threats.h"
#include "features/passed_pawns.h"
#include "features/pawn_pair.h"
#include "layers/affine_transform.h"
#include "layers/affine_transform_sparse_input.h"
#include "layers/clipped_relu.h"
#include "layers/sqr_clipped_relu.h"
#include "nnue_common.h"
#include "nnz_helper.h"

namespace Triumviratus::Eval::NNUE {

// Input features used in evaluation function.
// TRANN1 (Triumviratus Rubicon Alea NNUE 1): architettura derivata da
// Stockfish SFNNv13 (GPLv3, attribuzione in COPYING/README) + DUE blocchi di
// input assenti in SF: PawnPair (4560 feature) e PassedPawns (96 feature).
// Il nome proprio riflette la divergenza reale, non nasconde la derivazione:
// il formato del net NON e' piu' SFNNv13 (hash diverso) e un net SFNNv13 non
// e' caricabile.
using ThreatFeatureSet = Features::FullThreats;
using PSQFeatureSet    = Features::HalfKAv2_hm;
using PawnFeatureSet   = Features::PawnPair;
using PassedFeatureSet = Features::PassedPawns;  // v3 graft: 96 feature passed-pawn, folded dopo PawnPair

// Number of input feature dimensions after conversion
constexpr IndexType L1 = 1024;
constexpr int       L2 = 31;
constexpr int       L3 = 32;

constexpr IndexType PSQTBuckets = 8;
constexpr IndexType LayerStacks = 8;

// If vector instructions are enabled, we update and refresh the
// accumulator tile by tile such that each tile fits in the CPU's
// vector registers.
static_assert(PSQTBuckets % 8 == 0,
              "Per feature PSQT values cannot be processed at granularity lower than 8 at a time.");

struct NetworkArchitecture {
    static constexpr IndexType TransformedFeatureDimensions = L1;
    static constexpr int       FC_0_OUTPUTS                 = L2;
    static constexpr int       FC_1_OUTPUTS                 = L3;

    Layers::AffineTransformSparseInput<TransformedFeatureDimensions, FC_0_OUTPUTS + 1> fc_0;
    Layers::SqrClippedReLU<FC_0_OUTPUTS + 1, WeightScaleBits + 1>                      ac_sqr_0;
    Layers::ClippedReLU<FC_0_OUTPUTS + 1, WeightScaleBits + 1>                         ac_0;
    Layers::AffineTransform<FC_0_OUTPUTS * 2, FC_1_OUTPUTS>                            fc_1;
    Layers::ClippedReLU<FC_1_OUTPUTS, WeightScaleBits>                                 ac_1;
    Layers::AffineTransform<FC_1_OUTPUTS, 1>                                           fc_2;

    // Hash value embedded in the evaluation file
    static constexpr u32 get_hash_value() {
        // input slice hash
        u32 hashValue = 0xEC42E90Du;
        hashValue ^= TransformedFeatureDimensions * 2;

        hashValue = decltype(fc_0)::get_hash_value(hashValue);
        // TODO: considerincluding hash value of ac_sqr_0 in the overall hash value.
        // For now omitted on purpose because hash value is not written by trainer yet
        hashValue = decltype(ac_0)::get_hash_value(hashValue);
        hashValue = decltype(fc_1)::get_hash_value(hashValue);
        hashValue = decltype(ac_1)::get_hash_value(hashValue);
        hashValue = decltype(fc_2)::get_hash_value(hashValue);

        return hashValue;
    }

    // --- Rimappa colonne fc_1 (2026-07-19, port dell'idea SF eca43a97 alle NOSTRE dimensioni) ---
    // Il FILE (formato canonico, invariato) ha il concat [sqr 0..30 | lin 31..61 | pad 62..63]:
    // con FC_0_OUTPUTS=31 l'offset 31 e' disallineato e propagate() pagava memset+memcpy+buffer
    // extra A OGNI EVAL. In MEMORIA usiamo invece [sqr 0..31 | lin 32..63] dove le posizioni 31 e
    // 63 (attivazioni del neurone forwarded) sono COLONNE MORTE a peso zero: ac_sqr_0/ac_0
    // scrivono direttamente nel concat, allineati, senza copie. Semantica identica (colonna a
    // zero x input qualunque = 0); il costo di fc_1 e' invariato (processava gia' 64 input paddati).
    // La rimappa avviene QUI, al confine file<->memoria, in entrambe le direzioni: il formato su
    // disco non cambia mai (stessi hash, stesse reti, stesso trainer).
    static constexpr size_t FC1BiasBytes = FC_1_OUTPUTS * sizeof(i32);
    static constexpr size_t FC1RowBytes  = 64;   // PaddedInputDimensions di fc_1 (= anche il file)
    static constexpr size_t FC1BlockBytes = FC1BiasBytes + FC_1_OUTPUTS * FC1RowBytes;

    bool read_parameters(std::istream& stream) {
        if (!(fc_0.read_parameters(stream) && ac_0.read_parameters(stream)))
            return false;
        char buf[FC1BlockBytes];
        stream.read(buf, sizeof(buf));
        if (stream.fail())
            return false;
        for (int r = 0; r < FC_1_OUTPUTS; ++r)
        {
            char* row = buf + FC1BiasBytes + r * FC1RowBytes;
            char  tmp[FC1RowBytes];
            std::memcpy(tmp, row, FC1RowBytes);
            row[31] = 0;                                        // colonna morta: sqr(forwarded)
            for (int c = 31; c <= 61; ++c) row[c + 1] = tmp[c]; // lin k: colonna 31+k -> 32+k
            row[63] = 0;                                        // colonna morta: lin(forwarded)
        }
        std::stringstream remapped(std::string(buf, sizeof(buf)),
                                   std::ios::in | std::ios::binary);
        return fc_1.read_parameters(remapped) && ac_1.read_parameters(stream)
            && fc_2.read_parameters(stream);
    }

    // Write network parameters (rimappa INVERSA: la memoria torna al formato-file canonico)
    bool write_parameters(std::ostream& stream) const {
        if (!(fc_0.write_parameters(stream) && ac_0.write_parameters(stream)))
            return false;
        std::stringstream mem(std::ios::in | std::ios::out | std::ios::binary);
        if (!fc_1.write_parameters(mem))
            return false;
        std::string blk = mem.str();
        if (blk.size() != FC1BlockBytes)
            return false;
        for (int r = 0; r < FC_1_OUTPUTS; ++r)
        {
            char* row = &blk[0] + FC1BiasBytes + r * FC1RowBytes;
            char  tmp[FC1RowBytes];
            std::memcpy(tmp, row, FC1RowBytes);
            for (int c = 31; c <= 61; ++c) row[c] = tmp[c + 1]; // 32+k -> 31+k
            row[62] = 0;
            row[63] = 0;
        }
        stream.write(blk.data(), blk.size());
        return !stream.fail() && ac_1.write_parameters(stream) && fc_2.write_parameters(stream);
    }

    i32 propagate(const TransformedFeatureType* transformedFeatures,
                  const NNZInfo<L1>&            nnzInfo) const {
        struct alignas(CacheLineSize) Buffer {
            alignas(CacheLineSize) typename decltype(fc_0)::OutputBuffer fc_0_out;
            // Concat [sqr 0..31 | lin 32..63]: le posizioni 31 e 63 sono colonne morte a peso
            // zero (vedi read_parameters) -> le due attivazioni scrivono DIRETTAMENTE qui,
            // allineate. Niente memset, niente memcpy, niente ac_0_out (port idea SF eca43a97).
            alignas(CacheLineSize) typename decltype(ac_sqr_0)::OutputType concat[64];
            alignas(CacheLineSize) typename decltype(fc_1)::OutputBuffer fc_1_out;
            alignas(CacheLineSize) typename decltype(ac_1)::OutputBuffer ac_1_out;
            alignas(CacheLineSize) typename decltype(fc_2)::OutputBuffer fc_2_out;
        };

        Buffer buffer;

        fc_0.propagate(transformedFeatures, buffer.fc_0_out, nnzInfo);
        // ⛔ propagate_pair (attivazione fusa) MISURATA PIU' LENTA e non usata: vedi il commento
        // in layers/sqr_clipped_relu.h. Le due attivazioni separate restano la via veloce.
        ac_sqr_0.propagate(buffer.fc_0_out, buffer.concat);
        ac_0.propagate(buffer.fc_0_out, buffer.concat + 32);
        fc_1.propagate(buffer.concat, buffer.fc_1_out);
        ac_1.propagate(buffer.fc_1_out, buffer.ac_1_out);
        fc_2.propagate(buffer.ac_1_out, buffer.fc_2_out);

        // max value for fwdOut is (L1 + L3) * HiddenMaxVal * WeightMaxVal
        // for int8 activations and weights this is (L1 + L3) * 16129 making
        // fwdOut safe from overflow until (L1 + L3) > 133,144
        // first layer and last layer use WeightScaleBits + 1
        i32 fwdOut = buffer.fc_2_out[0] + buffer.fc_0_out[FC_0_OUTPUTS];
        // fwdOut is such that 1.0 is equal to HiddenOneVal*(1<<WeightScaleBits)*2 in
        // quantized form, but we want 1.0 to be equal to 600*OutputScale
        // to make overflow impossible we cast to i64
        constexpr i64 multiplier = 600 * OutputScale;
        constexpr i64 denominator =
          static_cast<i64>(HiddenOneVal) * static_cast<i64>(1U << WeightScaleBits) * 2;

        i32 outputValue = static_cast<i32>((static_cast<i64>(fwdOut) * multiplier) / denominator);
        return outputValue;
    }

    // Mens visualizer instrumentation: identical to propagate() but also exports the
    // L2 (ac_0) and L3 (ac_1) post-activation neuron values. Additive; the normal
    // propagate()/evaluate() path is untouched.
    i32 propagate_trace(const TransformedFeatureType* transformedFeatures,
                        const NNZInfo<L1>& nnzInfo, i32* l2out, i32* l3out) const {
        struct alignas(CacheLineSize) Buffer {
            alignas(CacheLineSize) typename decltype(fc_0)::OutputBuffer fc_0_out;
            alignas(CacheLineSize) typename decltype(ac_sqr_0)::OutputType concat[64];
            alignas(CacheLineSize) typename decltype(fc_1)::OutputBuffer fc_1_out;
            alignas(CacheLineSize) typename decltype(ac_1)::OutputBuffer ac_1_out;
            alignas(CacheLineSize) typename decltype(fc_2)::OutputBuffer fc_2_out;
        };
        Buffer buffer;
        fc_0.propagate(transformedFeatures, buffer.fc_0_out, nnzInfo);
        // ⛔ propagate_pair (attivazione fusa) MISURATA PIU' LENTA e non usata: vedi il commento
        // in layers/sqr_clipped_relu.h. Le due attivazioni separate restano la via veloce.
        ac_sqr_0.propagate(buffer.fc_0_out, buffer.concat);
        ac_0.propagate(buffer.fc_0_out, buffer.concat + 32);
        fc_1.propagate(buffer.concat, buffer.fc_1_out);
        ac_1.propagate(buffer.fc_1_out, buffer.ac_1_out);
        fc_2.propagate(buffer.ac_1_out, buffer.fc_2_out);
        // l2out: le attivazioni lineari ora vivono nel concat a partire da +32
        for (int i = 0; i < FC_0_OUTPUTS; ++i) l2out[i] = i32(buffer.concat[32 + i]);
        for (int i = 0; i < FC_1_OUTPUTS; ++i) l3out[i] = i32(buffer.ac_1_out[i]);
        i32 fwdOut = buffer.fc_2_out[0] + buffer.fc_0_out[FC_0_OUTPUTS];
        constexpr i64 multiplier = 600 * OutputScale;
        constexpr i64 denominator =
          static_cast<i64>(HiddenOneVal) * static_cast<i64>(1U << WeightScaleBits) * 2;
        return static_cast<i32>((static_cast<i64>(fwdOut) * multiplier) / denominator);
    }

    usize get_content_hash() const {
        usize h = 0;
        hash_combine(h, fc_0.get_content_hash());
        hash_combine(h, ac_sqr_0.get_content_hash());
        hash_combine(h, ac_0.get_content_hash());
        hash_combine(h, fc_1.get_content_hash());
        hash_combine(h, ac_1.get_content_hash());
        hash_combine(h, fc_2.get_content_hash());
        hash_combine(h, get_hash_value());
        return h;
    }
};

}  // namespace Triumviratus::Eval::NNUE

template<>
struct std::hash<Triumviratus::Eval::NNUE::NetworkArchitecture> {
    Triumviratus::usize
    operator()(const Triumviratus::Eval::NNUE::NetworkArchitecture& arch) const noexcept {
        return arch.get_content_hash();
    }
};

#endif  // #ifndef NNUE_ARCHITECTURE_H_INCLUDED

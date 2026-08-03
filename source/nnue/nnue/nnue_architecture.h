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

#include "../../profile.h"
#include "features/feat_perm.h"
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
// TRANN2 (Triumviratus NNUE 2, linea rete "legio-septima", 7.0): architettura derivata da
// Stockfish **SFNNv15** (GPLv3, attribuzione in COPYING/README) + UN blocco di input assente
// in SF: PassedPawns (96 feature).
// NB: il blocco PawnPair (4560) NON e' piu' un'aggiunta nostra — SF ha adottato la stessa
// feature come `PP_3Wide` (nnue-pytorch PR #502, merged 25/07/2026): coppie di pedoni su
// stessa colonna o adiacenti, stesse 4560 dimensioni. Il nostro filtro di banda in
// features/pawn_pair.cpp e' sempre stato quello; il "combinatorio pieno" non e' mai esistito.
// Il nome proprio riflette la divergenza reale, non nasconde la derivazione: il formato del net
// NON e' SFNNv15 puro (hash diverso: c'e' un blocco di input in piu') e i net TRANN1 della 6.0
// non sono caricabili.
using ThreatFeatureSet = Features::FullThreats;
using PSQFeatureSet    = Features::HalfKAv2_hm;
using PawnFeatureSet   = Features::PawnPair;
using PassedFeatureSet = Features::PassedPawns;  // v3 graft: 96 feature passed-pawn, folded dopo PawnPair

// Number of input feature dimensions after conversion
constexpr IndexType L1 = 1024;
// L2 = 32 (era 31 in TRANN1/v13): SFNNv15 non ha piu' il neurone forwarded in uno slot extra, quindi
// L2 e' un multiplo di 32 pulito e il concat e' allineato senza rimappe. Il conto dei parametri della
// testa cambia ⇒ i net v13 NON sono caricabili (l'hash li rifiuta: e' voluto).
constexpr int       L2 = 32;
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

    // --- SFNNv15 "Better Skip Architecture" (port di SF master 2384f27, 04/07/2026) ---
    // Due differenze rispetto al v13 che avevamo:
    //  1) L'output layer non legge piu' solo L3: legge il CONCAT di L2 e L3, ognuno nelle sue due
    //     attivazioni (quadratica + lineare) -> fc_2 ha 2*L2 + 2*L3 = 128 input invece di 32.
    //     E' lo "skip L2->output": la testa vede anche i neuroni di L2, non solo quelli filtrati da L3.
    //  2) Compare ac_sqr_1: anche L3 ha l'attivazione quadratica, prima ce l'aveva solo L2.
    // Inoltre L2 passa da 31 a 32 (vedi il commento su L2): sparisce il neurone-forwarded in slot
    // extra, e con esso TUTTA la rimappa delle colonne di fc_1 che serviva per riallinearlo.
    Layers::AffineTransformSparseInput<TransformedFeatureDimensions, FC_0_OUTPUTS> fc_0;
    Layers::SqrClippedReLU<FC_0_OUTPUTS, WeightScaleBits + 1>                      ac_sqr_0;
    Layers::ClippedReLU<FC_0_OUTPUTS, WeightScaleBits + 1>                         ac_0;
    Layers::AffineTransform<FC_0_OUTPUTS * 2, FC_1_OUTPUTS>                        fc_1;
    Layers::SqrClippedReLU<FC_1_OUTPUTS, WeightScaleBits>                          ac_sqr_1;
    Layers::ClippedReLU<FC_1_OUTPUTS, WeightScaleBits>                             ac_1;
    Layers::AffineTransform<FC_0_OUTPUTS * 2 + FC_1_OUTPUTS * 2, 1>                fc_2;

    // Dimensione del buffer di concat: [ac_sqr_0 | ac_0 | ac_sqr_1 | ac_1]
    static constexpr IndexType ConcatDims =
      ceil_to_multiple<IndexType>(FC_0_OUTPUTS * 2 + FC_1_OUTPUTS * 2, 32);

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

    // Con L2 = 32 il concat e' allineato per costruzione: nessuna rimappa delle colonne di fc_1.
    // (La rimappa del 19/07 esisteva SOLO per riallineare l'offset 31 quando L2 era 31; il v15
    // la rende inutile. Cancellata: era ~50 righe al confine file<->memoria in due direzioni.)
    bool read_parameters(std::istream& stream) {
        return fc_0.read_parameters(stream) && ac_0.read_parameters(stream)
            && fc_1.read_parameters(stream) && ac_1.read_parameters(stream)
            && fc_2.read_parameters(stream);
    }

    bool write_parameters(std::ostream& stream) const {
        return fc_0.write_parameters(stream) && ac_0.write_parameters(stream)
            && fc_1.write_parameters(stream) && ac_1.write_parameters(stream)
            && fc_2.write_parameters(stream);
    }

    i32 propagate(const TransformedFeatureType* transformedFeatures,
                  const NNZInfo<L1>&            nnzInfo) const {
        struct alignas(CacheLineSize) Buffer {
            alignas(CacheLineSize) typename decltype(fc_0)::OutputBuffer fc_0_out;
            // Concat SFNNv15: [ac_sqr_0 0..31 | ac_0 32..63 | ac_sqr_1 64..95 | ac_1 96..127].
            // fc_1 legge i primi 2*L2; fc_2 legge TUTTO il buffer (skip L2->output).
            alignas(CacheLineSize) typename decltype(ac_sqr_0)::OutputType concat[ConcatDims];
            alignas(CacheLineSize) typename decltype(fc_1)::OutputBuffer fc_1_out;
            alignas(CacheLineSize) typename decltype(fc_2)::OutputBuffer fc_2_out;
        };

        Buffer buffer;

        // PROF_GUARD e' un no-op senza TRIUMV_PROFILE: nessun costo nel binario spedito.
        { PROF_GUARD(prof_fc0);
          fc_0.propagate(transformedFeatures, buffer.fc_0_out, nnzInfo); }
        { PROF_GUARD(prof_layers);
        // ⛔ propagate_pair (attivazione fusa) MISURATA PIU' LENTA e non usata: vedi il commento
        // in layers/sqr_clipped_relu.h. Le due attivazioni separate restano la via veloce.
        ac_sqr_0.propagate(buffer.fc_0_out, buffer.concat);
        ac_0.propagate(buffer.fc_0_out, buffer.concat + FC_0_OUTPUTS);
        fc_1.propagate(buffer.concat, buffer.fc_1_out);
        ac_sqr_1.propagate(buffer.fc_1_out, buffer.concat + FC_0_OUTPUTS * 2);
        ac_1.propagate(buffer.fc_1_out, buffer.concat + FC_0_OUTPUTS * 2 + FC_1_OUTPUTS);
        fc_2.propagate(buffer.concat, buffer.fc_2_out); }

        // Skip L1->output (SFNNv15): non c'e' piu' un neurone forwarded in slot extra; si prende la
        // DIFFERENZA degli ultimi due output di fc_0, che sarebbero comunque zero-paddati per il SIMD
        // ("keep the 'free' information as we would otherwise zero pad anyway").
        static_assert(FC_0_OUTPUTS >= 2);
        i32 fwdOut = buffer.fc_2_out[0]
                   + (buffer.fc_0_out[FC_0_OUTPUTS - 2] - buffer.fc_0_out[FC_0_OUTPUTS - 1]);
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
            alignas(CacheLineSize) typename decltype(ac_sqr_0)::OutputType concat[ConcatDims];
            alignas(CacheLineSize) typename decltype(fc_1)::OutputBuffer fc_1_out;
            alignas(CacheLineSize) typename decltype(fc_2)::OutputBuffer fc_2_out;
        };
        Buffer buffer;
        fc_0.propagate(transformedFeatures, buffer.fc_0_out, nnzInfo);
        // ⛔ propagate_pair (attivazione fusa) MISURATA PIU' LENTA e non usata: vedi il commento
        // in layers/sqr_clipped_relu.h. Le due attivazioni separate restano la via veloce.
        ac_sqr_0.propagate(buffer.fc_0_out, buffer.concat);
        ac_0.propagate(buffer.fc_0_out, buffer.concat + FC_0_OUTPUTS);
        fc_1.propagate(buffer.concat, buffer.fc_1_out);
        ac_sqr_1.propagate(buffer.fc_1_out, buffer.concat + FC_0_OUTPUTS * 2);
        ac_1.propagate(buffer.fc_1_out, buffer.concat + FC_0_OUTPUTS * 2 + FC_1_OUTPUTS);
        fc_2.propagate(buffer.concat, buffer.fc_2_out);
        // attivazioni LINEARI: L2 dal concat a +FC_0_OUTPUTS, L3 a +2*FC_0_OUTPUTS+FC_1_OUTPUTS
        for (int i = 0; i < FC_0_OUTPUTS; ++i) l2out[i] = i32(buffer.concat[FC_0_OUTPUTS + i]);
        for (int i = 0; i < FC_1_OUTPUTS; ++i)
            l3out[i] = i32(buffer.concat[FC_0_OUTPUTS * 2 + FC_1_OUTPUTS + i]);
        static_assert(FC_0_OUTPUTS >= 2);
        i32 fwdOut = buffer.fc_2_out[0]
                   + (buffer.fc_0_out[FC_0_OUTPUTS - 2] - buffer.fc_0_out[FC_0_OUTPUTS - 1]);
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

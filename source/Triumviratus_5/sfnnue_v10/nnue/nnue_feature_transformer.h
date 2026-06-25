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

// A class that converts the input features of the NNUE evaluation function

#ifndef NNUE_FEATURE_TRANSFORMER_H_INCLUDED
#define NNUE_FEATURE_TRANSFORMER_H_INCLUDED

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <utility>

#include "../bitboard.h"
#include "../position.h"
#include "../types.h"
#include "nnue_accumulator.h"
#include "nnue_architecture.h"
#include "nnue_common.h"

namespace Stockfish::Eval::NNUE {

using BiasType       = std::int16_t;
using WeightType     = std::int16_t;
using PSQTWeightType = std::int32_t;

// If vector instructions are enabled, we update and refresh the
// accumulator tile by tile such that each tile fits in the CPU's
// vector registers.
#define VECTOR

static_assert(PSQTBuckets % 8 == 0,
              "Per feature PSQT values cannot be processed at granularity lower than 8 at a time.");

#ifdef USE_AVX512
using vec_t      = __m512i;
using vec_i8_t   = __m256i;
using psqt_vec_t = __m256i;
    #define vec_load(a) _mm512_load_si512(a)
    #define vec_store(a, b) _mm512_store_si512(a, b)
    #define vec_add_16(a, b) _mm512_add_epi16(a, b)
    #define vec_sub_16(a, b) _mm512_sub_epi16(a, b)
    #define vec_mul_16(a, b) _mm512_mullo_epi16(a, b)
    #define vec_zero() _mm512_setzero_epi32()
    #define vec_set_16(a) _mm512_set1_epi16(a)
    #define vec_max_16(a, b) _mm512_max_epi16(a, b)
    #define vec_min_16(a, b) _mm512_min_epi16(a, b)
    #define vec_convert_8_16(a) _mm512_cvtepi8_epi16(a)   // 32 int8 -> 32 int16 (sign-extend)
    #define vec_load_i8(a) _mm256_load_si256(a)
inline vec_t vec_msb_pack_16(vec_t a, vec_t b) {
    vec_t compacted = _mm512_packs_epi16(_mm512_srli_epi16(a, 7), _mm512_srli_epi16(b, 7));
    return _mm512_permutexvar_epi64(_mm512_setr_epi64(0, 2, 4, 6, 1, 3, 5, 7), compacted);
}
    #define vec_load_psqt(a) _mm256_load_si256(a)
    #define vec_store_psqt(a, b) _mm256_store_si256(a, b)
    #define vec_add_psqt_32(a, b) _mm256_add_epi32(a, b)
    #define vec_sub_psqt_32(a, b) _mm256_sub_epi32(a, b)
    #define vec_zero_psqt() _mm256_setzero_si256()
    #define NumRegistersSIMD 16
    #define MaxChunkSize 64

#elif USE_AVX2
using vec_t      = __m256i;
using vec_i8_t   = __m128i;
using psqt_vec_t = __m256i;
    #define vec_load(a) _mm256_load_si256(a)
    #define vec_store(a, b) _mm256_store_si256(a, b)
    #define vec_add_16(a, b) _mm256_add_epi16(a, b)
    #define vec_sub_16(a, b) _mm256_sub_epi16(a, b)
    #define vec_mul_16(a, b) _mm256_mullo_epi16(a, b)
    #define vec_zero() _mm256_setzero_si256()
    #define vec_set_16(a) _mm256_set1_epi16(a)
    #define vec_max_16(a, b) _mm256_max_epi16(a, b)
    #define vec_min_16(a, b) _mm256_min_epi16(a, b)
    #define vec_convert_8_16(a) _mm256_cvtepi8_epi16(a)   // 16 int8 -> 16 int16 (sign-extend)
    #define vec_load_i8(a) _mm_load_si128(a)
inline vec_t vec_msb_pack_16(vec_t a, vec_t b) {
    vec_t compacted = _mm256_packs_epi16(_mm256_srli_epi16(a, 7), _mm256_srli_epi16(b, 7));
    return _mm256_permute4x64_epi64(compacted, 0b11011000);
}
    #define vec_load_psqt(a) _mm256_load_si256(a)
    #define vec_store_psqt(a, b) _mm256_store_si256(a, b)
    #define vec_add_psqt_32(a, b) _mm256_add_epi32(a, b)
    #define vec_sub_psqt_32(a, b) _mm256_sub_epi32(a, b)
    #define vec_zero_psqt() _mm256_setzero_si256()
    #define NumRegistersSIMD 16
    #define MaxChunkSize 32

#elif USE_SSE2
using vec_t      = __m128i;
using psqt_vec_t = __m128i;
    #define vec_load(a) (*(a))
    #define vec_store(a, b) *(a) = (b)
    #define vec_add_16(a, b) _mm_add_epi16(a, b)
    #define vec_sub_16(a, b) _mm_sub_epi16(a, b)
    #define vec_mul_16(a, b) _mm_mullo_epi16(a, b)
    #define vec_zero() _mm_setzero_si128()
    #define vec_set_16(a) _mm_set1_epi16(a)
    #define vec_max_16(a, b) _mm_max_epi16(a, b)
    #define vec_min_16(a, b) _mm_min_epi16(a, b)
    #define vec_msb_pack_16(a, b) _mm_packs_epi16(_mm_srli_epi16(a, 7), _mm_srli_epi16(b, 7))
    #define vec_load_psqt(a) (*(a))
    #define vec_store_psqt(a, b) *(a) = (b)
    #define vec_add_psqt_32(a, b) _mm_add_epi32(a, b)
    #define vec_sub_psqt_32(a, b) _mm_sub_epi32(a, b)
    #define vec_zero_psqt() _mm_setzero_si128()
    #define NumRegistersSIMD (Is64Bit ? 16 : 8)
    #define MaxChunkSize 16

#elif USE_NEON
using vec_t      = int16x8_t;
using psqt_vec_t = int32x4_t;
    #define vec_load(a) (*(a))
    #define vec_store(a, b) *(a) = (b)
    #define vec_add_16(a, b) vaddq_s16(a, b)
    #define vec_sub_16(a, b) vsubq_s16(a, b)
    #define vec_mul_16(a, b) vmulq_s16(a, b)
    #define vec_zero() \
        vec_t { 0 }
    #define vec_set_16(a) vdupq_n_s16(a)
    #define vec_max_16(a, b) vmaxq_s16(a, b)
    #define vec_min_16(a, b) vminq_s16(a, b)
inline vec_t vec_msb_pack_16(vec_t a, vec_t b) {
    const int8x8_t  shifta    = vshrn_n_s16(a, 7);
    const int8x8_t  shiftb    = vshrn_n_s16(b, 7);
    const int8x16_t compacted = vcombine_s8(shifta, shiftb);
    return *reinterpret_cast<const vec_t*>(&compacted);
}
    #define vec_load_psqt(a) (*(a))
    #define vec_store_psqt(a, b) *(a) = (b)
    #define vec_add_psqt_32(a, b) vaddq_s32(a, b)
    #define vec_sub_psqt_32(a, b) vsubq_s32(a, b)
    #define vec_zero_psqt() \
        psqt_vec_t { 0 }
    #define NumRegistersSIMD 16
    #define MaxChunkSize 16

#else
    #undef VECTOR

#endif


#ifdef VECTOR

    // Compute optimal SIMD register count for feature transformer accumulation.

    // We use __m* types as template arguments, which causes GCC to emit warnings
    // about losing some attribute information. This is irrelevant to us as we
    // only take their size, so the following pragma are harmless.
    #if defined(__GNUC__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wignored-attributes"
    #endif

template<typename SIMDRegisterType, typename LaneType, int NumLanes, int MaxRegisters>
static constexpr int BestRegisterCount() {
    #define RegisterSize sizeof(SIMDRegisterType)
    #define LaneSize sizeof(LaneType)

    static_assert(RegisterSize >= LaneSize);
    static_assert(MaxRegisters <= NumRegistersSIMD);
    static_assert(MaxRegisters > 0);
    static_assert(NumRegistersSIMD > 0);
    static_assert(RegisterSize % LaneSize == 0);
    static_assert((NumLanes * LaneSize) % RegisterSize == 0);

    const int ideal = (NumLanes * LaneSize) / RegisterSize;
    if (ideal <= MaxRegisters)
        return ideal;

    // Look for the largest divisor of the ideal register count that is smaller than MaxRegisters
    for (int divisor = MaxRegisters; divisor > 1; --divisor)
        if (ideal % divisor == 0)
            return divisor;

    return 1;
}
    #if defined(__GNUC__)
        #pragma GCC diagnostic pop
    #endif
#endif


// Input feature converter
template<IndexType                                 TransformedFeatureDimensions,
         Accumulator<TransformedFeatureDimensions> StateInfo::*accPtr>
class FeatureTransformer {

   private:
    // Number of output dimensions for one side
    static constexpr IndexType HalfDimensions = TransformedFeatureDimensions;

#ifdef VECTOR
    static constexpr int NumRegs =
      BestRegisterCount<vec_t, WeightType, TransformedFeatureDimensions, NumRegistersSIMD>();
    static constexpr int NumPsqtRegs =
      BestRegisterCount<psqt_vec_t, PSQTWeightType, PSQTBuckets, NumRegistersSIMD>();

    static constexpr IndexType TileHeight     = NumRegs * sizeof(vec_t) / 2;
    static constexpr IndexType PsqtTileHeight = NumPsqtRegs * sizeof(psqt_vec_t) / 4;
    static_assert(HalfDimensions % TileHeight == 0, "TileHeight must divide HalfDimensions");
    static_assert(PSQTBuckets % PsqtTileHeight == 0, "PsqtTileHeight must divide PSQTBuckets");
#endif

   public:
    // SFNNv10: the Big net (1024) adds a second feature set (Full_Threats) on top
    // of HalfKAv2_hm; the Small net (128) is HalfKA-only (UseThreats=false).
    static constexpr bool UseThreats =
      (TransformedFeatureDimensions == TransformedFeatureDimensionsBig);

    // Threats feature set selector: V1 (SF18 stable / nn-49c, 79.856 features) by
    // default, V2 (master nnue-pytorch 89d5725 / our trained net, 60.720) under
    // TRIUMV_FT_V2. Same API surface so the rest of the FT is untouched.
#ifdef TRIUMV_FT_V2
    using ThreatsFeatureSet = Features::FullThreatsV2;
#else
    using ThreatsFeatureSet = Features::FullThreats;
#endif

    // Output type
    using OutputType = TransformedFeatureType;

    // Number of input/output dimensions
    static constexpr IndexType InputDimensions       = FeatureSet::Dimensions;
    static constexpr IndexType ThreatInputDimensions = ThreatsFeatureSet::Dimensions;
    static constexpr IndexType OutputDimensions      = HalfDimensions;

    // Size of forward propagation buffer
    static constexpr std::size_t BufferSize = OutputDimensions * sizeof(OutputType);

    // Hash value embedded in the evaluation file
    static constexpr std::uint32_t get_hash_value() {
        return (UseThreats ? ThreatsFeatureSet::HashValue : FeatureSet::HashValue)
             ^ (OutputDimensions * 2);
    }

    // Read network parameters.
    //
    // V1 (SF18 stable / nn-49c) layout:
    //   leb block 1 = biases (int16)
    //   leb block 2 = threatWeights(int8) ++ HalfKA weights(int16)   [COMBINED]
    //   leb block 3 = threatPsqtWeights(int32) ++ psqtWeights(int32) [COMBINED]
    //
    // V2 (master nnue-pytorch 89d5725) layout (from NNUEWriter, per-feature
    // interleaved, with int8 written RAW since "compression == none for int8"):
    //   leb block = biases (int16)
    //   RAW       = threatWeights (int8, 60720*1024 bytes)
    //   leb block = threatPsqtWeights (int32)
    //   leb block = HalfKA weights (int16)
    //   leb block = HalfKA psqtWeights (int32)
    bool read_parameters(std::istream& stream) {

        read_leb_128<BiasType>(stream, biases, HalfDimensions);

        if constexpr (UseThreats)
        {
#ifdef TRIUMV_FT_V2
            // RAW int8 threatWeights (no leb magic, just plain bytes).
            stream.read(reinterpret_cast<char*>(threatWeights),
                        std::streamsize(sizeof(ThreatWeightType) * std::size_t(HalfDimensions) * ThreatInputDimensions));
            // psqt threats (int32) leb.
            read_leb_128<PSQTWeightType>(stream, threatPsqtWeights,
                                         std::size_t(ThreatInputDimensions) * PSQTBuckets);
            // HalfKA weights (int16) leb.
            read_leb_128<WeightType>(stream, weights,
                                     std::size_t(HalfDimensions) * InputDimensions);
            // HalfKA psqt (int32) leb.
            read_leb_128<PSQTWeightType>(stream, psqtWeights,
                                         std::size_t(InputDimensions) * PSQTBuckets);
#else
            read_leb_128_combined(stream, threatWeights, std::size_t(HalfDimensions) * ThreatInputDimensions,
                                  weights, std::size_t(HalfDimensions) * InputDimensions);
            read_leb_128_combined(stream, threatPsqtWeights, std::size_t(ThreatInputDimensions) * PSQTBuckets,
                                  psqtWeights, std::size_t(InputDimensions) * PSQTBuckets);
#endif
        }
        else
        {
            read_leb_128<WeightType>(stream, weights, HalfDimensions * InputDimensions);
            read_leb_128<PSQTWeightType>(stream, psqtWeights, PSQTBuckets * InputDimensions);
        }

        return !stream.fail();
    }

    // Write network parameters
    bool write_parameters(std::ostream& stream) const {

        write_leb_128<BiasType>(stream, biases, HalfDimensions);
        write_leb_128<WeightType>(stream, weights, HalfDimensions * InputDimensions);
        write_leb_128<PSQTWeightType>(stream, psqtWeights, PSQTBuckets * InputDimensions);

        return !stream.fail();
    }

    // Accessor to the bias table, used to (re)initialise an AccumulatorCaches
    // entry to "empty board" (biases only).
    const BiasType* bias_table() const { return biases; }

    // Convert input features. When cache != nullptr the full-refresh path uses
    // the per-thread AccumulatorCaches ("finny tables"); when nullptr it falls
    // back to the plain refresh (byte-identical to the pre-finny behaviour).
    std::int32_t transform(const Position&                          pos,
                           OutputType*                              output,
                           int                                      bucket,
                           AccumulatorCaches::Cache<HalfDimensions>* cache = nullptr) const {
        update_accumulator<WHITE>(pos, cache);
        update_accumulator<BLACK>(pos, cache);
        if constexpr (UseThreats)
        {
            // Threats accumulator: incremental (DEFAULT; TRIUMV5_THREATS_INCR is
            // defined by the release build) vs SIMD full-refresh. Both validated
            // bit-exact (0/33 eval + node-count mismatch). Incremental is ~+1-2% NPS
            // in an alternating A/B (the transform combine, already SIMD'd, is the
            // real bottleneck -- the accumulator is near parity either way). Undefine
            // the macro (here AND in sf_bridge.cpp's mirror calls) for full-refresh.
#ifdef TRIUMV5_THREATS_INCR
            update_threats<WHITE>(pos);
            update_threats<BLACK>(pos);
#else
            refresh_threats<WHITE>(pos);
            refresh_threats<BLACK>(pos);
#endif
        }

        const Color perspectives[2]  = {pos.side_to_move(), ~pos.side_to_move()};
        const auto& accumulation     = (pos.state()->*accPtr).accumulation;
        const auto& psqtAccumulation = (pos.state()->*accPtr).psqtAccumulation;

        // Big net (SFNNv10): combine the HalfKA and Full_Threats accumulations.
        //   psqt = (halfkaPsqt + threatPsqt) / 2
        //   positional out = clamp(halfka+threat, 0, 255) pairwise product / 512
        // Scalar combine (Phase A = correctness; SIMD-combine is a later NPS step).
        if constexpr (UseThreats)
        {
            const auto& tacc  = pos.state()->accumulatorBigThreat.accumulation;
            const auto& tpsqt = pos.state()->accumulatorBigThreat.psqtAccumulation;
            const std::int32_t psqt =
              (psqtAccumulation[perspectives[0]][bucket] - psqtAccumulation[perspectives[1]][bucket]
               + tpsqt[perspectives[0]][bucket] - tpsqt[perspectives[1]][bucket])
              / 2;
            for (IndexType p = 0; p < 2; ++p)
            {
                const IndexType offset = (HalfDimensions / 2) * p;
                const int       persp  = static_cast<int>(perspectives[p]);

#if defined(USE_AVX512)
                // SIMD combine (natural order, no permute): s = clamp(halfka+threat,
                // 0..255); out = (s0*s1)/512 via mulhi(s0<<7, s1) (== >>9); narrow
                // int16->uint8 in-lane with cvtepi16_epi8 (no packus => no permute).
                const __m512i lo255 = _mm512_set1_epi16(255);
                const __m512i zero  = _mm512_setzero_si512();
                const auto*   ha    = reinterpret_cast<const __m512i*>(&accumulation[persp][0]);
                const auto*   hb    =
                  reinterpret_cast<const __m512i*>(&accumulation[persp][HalfDimensions / 2]);
                const auto* ta = reinterpret_cast<const __m512i*>(&tacc[persp][0]);
                const auto* tb =
                  reinterpret_cast<const __m512i*>(&tacc[persp][HalfDimensions / 2]);
                constexpr IndexType N = (HalfDimensions / 2) / 32;  // 32 int16 per __m512i
                for (IndexType j = 0; j < N; ++j)
                {
                    __m512i s0 = _mm512_add_epi16(_mm512_load_si512(ha + j), _mm512_load_si512(ta + j));
                    __m512i s1 = _mm512_add_epi16(_mm512_load_si512(hb + j), _mm512_load_si512(tb + j));
                    s0         = _mm512_max_epi16(_mm512_min_epi16(s0, lo255), zero);
                    s1         = _mm512_max_epi16(_mm512_min_epi16(s1, lo255), zero);
                    __m512i pr = _mm512_mulhi_epu16(_mm512_slli_epi16(s0, 7), s1);  // (s0*s1)>>9
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + offset + j * 32),
                                        _mm512_cvtepi16_epi8(pr));
                }
#else
                for (IndexType j = 0; j < HalfDimensions / 2; ++j)
                {
                    int s0 = accumulation[persp][j] + tacc[persp][j];
                    int s1 = accumulation[persp][j + HalfDimensions / 2]
                           + tacc[persp][j + HalfDimensions / 2];
                    s0                 = s0 < 0 ? 0 : (s0 > 255 ? 255 : s0);
                    s1                 = s1 < 0 ? 0 : (s1 > 255 ? 255 : s1);
                    output[offset + j] = static_cast<OutputType>(unsigned(s0 * s1) / 512);
                }
#endif
            }
            return psqt;
        }

        const auto psqt =
          (psqtAccumulation[perspectives[0]][bucket] - psqtAccumulation[perspectives[1]][bucket])
          / 2;


        for (IndexType p = 0; p < 2; ++p)
        {
            const IndexType offset = (HalfDimensions / 2) * p;

#if defined(VECTOR)

            constexpr IndexType OutputChunkSize = MaxChunkSize;
            static_assert((HalfDimensions / 2) % OutputChunkSize == 0);
            constexpr IndexType NumOutputChunks = HalfDimensions / 2 / OutputChunkSize;

            vec_t Zero = vec_zero();
            vec_t One  = vec_set_16(127);

            const vec_t* in0 = reinterpret_cast<const vec_t*>(&(accumulation[perspectives[p]][0]));
            const vec_t* in1 =
              reinterpret_cast<const vec_t*>(&(accumulation[perspectives[p]][HalfDimensions / 2]));
            vec_t* out = reinterpret_cast<vec_t*>(output + offset);

            for (IndexType j = 0; j < NumOutputChunks; ++j)
            {
                const vec_t sum0a = vec_max_16(vec_min_16(in0[j * 2 + 0], One), Zero);
                const vec_t sum0b = vec_max_16(vec_min_16(in0[j * 2 + 1], One), Zero);
                const vec_t sum1a = vec_max_16(vec_min_16(in1[j * 2 + 0], One), Zero);
                const vec_t sum1b = vec_max_16(vec_min_16(in1[j * 2 + 1], One), Zero);

                const vec_t pa = vec_mul_16(sum0a, sum1a);
                const vec_t pb = vec_mul_16(sum0b, sum1b);

                out[j] = vec_msb_pack_16(pa, pb);
            }

#else

            for (IndexType j = 0; j < HalfDimensions / 2; ++j)
            {
                BiasType sum0 = accumulation[static_cast<int>(perspectives[p])][j + 0];
                BiasType sum1 =
                  accumulation[static_cast<int>(perspectives[p])][j + HalfDimensions / 2];
                sum0               = std::clamp<BiasType>(sum0, 0, 127);
                sum1               = std::clamp<BiasType>(sum1, 0, 127);
                output[offset + j] = static_cast<OutputType>(unsigned(sum0 * sum1) / 128);
            }

#endif
        }

        return psqt;
    }  // end of function transform()

    // Full refresh of the Full_Threats accumulator for one perspective: rebuild
    // from the int8 threatWeights over all active threat features (NO bias -- the
    // bias lives in the HalfKA accumulator; transform() sums the two). Scalar
    // (Phase A: correctness); incremental + SIMD is the later NPS step.
    template<Color Perspective>
    void refresh_threats(const Position& pos) const {
        if constexpr (UseThreats)
        {
            auto& acc = pos.state()->accumulatorBigThreat;

            ThreatsFeatureSet::IndexList active;
            ThreatsFeatureSet::append_active_indices(Perspective, pos, active);

#ifdef VECTOR
            // Tile loop: rebuild from zero, adding int8 threatWeight columns
            // (sign-extended int8 -> int16). Natural order (no permute), so the
            // scalar transform combine reads the same order.
            constexpr IndexType I8PerReg = sizeof(vec_t) / 2;
            vec_t               accv[NumRegs];
            for (IndexType j = 0; j < HalfDimensions / TileHeight; ++j)
            {
                for (IndexType k = 0; k < NumRegs; ++k)
                    accv[k] = vec_zero();

                for (const auto index : active)
                {
                    const std::size_t       off = std::size_t(HalfDimensions) * index + j * TileHeight;
                    const ThreatWeightType* col = &threatWeights[off];
                    for (IndexType k = 0; k < NumRegs; ++k)
                    {
                        const vec_i8_t c8 =
                          vec_load_i8(reinterpret_cast<const vec_i8_t*>(&col[k * I8PerReg]));
                        accv[k] = vec_add_16(accv[k], vec_convert_8_16(c8));
                    }
                }

                auto out = reinterpret_cast<vec_t*>(&acc.accumulation[Perspective][j * TileHeight]);
                for (IndexType k = 0; k < NumRegs; ++k)
                    vec_store(&out[k], accv[k]);
            }
#else
            for (IndexType j = 0; j < HalfDimensions; ++j)
                acc.accumulation[Perspective][j] = 0;
            for (const auto index : active)
            {
                const std::size_t offset = std::size_t(HalfDimensions) * index;
                for (IndexType j = 0; j < HalfDimensions; ++j)
                    acc.accumulation[Perspective][j] += threatWeights[offset + j];
            }
#endif
            // PSQT (int32, 8 buckets) stays scalar -- negligible cost.
            for (std::size_t k = 0; k < PSQTBuckets; ++k)
                acc.psqtAccumulation[Perspective][k] = 0;
            for (const auto index : active)
                for (std::size_t k = 0; k < PSQTBuckets; ++k)
                    acc.psqtAccumulation[Perspective][k] +=
                      threatPsqtWeights[std::size_t(index) * PSQTBuckets + k];

            acc.computed[Perspective] = true;
        }
    }

    // Apply (add) or retract (sub) ONE Full_Threats feature column (int8 weights,
    // sign-extended to int16) for one perspective, plus its int32 PSQT column.
    template<Color Perspective, typename Acc>
    void apply_threat_col(Acc& acc, IndexType index, bool add) const {
        const ThreatWeightType* col = &threatWeights[std::size_t(HalfDimensions) * index];
        std::int16_t*           a   = acc.accumulation[Perspective];
#ifdef VECTOR
        constexpr IndexType I8PerReg = sizeof(vec_t) / 2;
        auto*               av       = reinterpret_cast<vec_t*>(a);
        for (IndexType k = 0; k < HalfDimensions / I8PerReg; ++k)
        {
            const vec_t w =
              vec_convert_8_16(vec_load_i8(reinterpret_cast<const vec_i8_t*>(&col[k * I8PerReg])));
            const vec_t cur = vec_load(&av[k]);
            vec_store(&av[k], add ? vec_add_16(cur, w) : vec_sub_16(cur, w));
        }
#else
        for (IndexType j = 0; j < HalfDimensions; ++j)
            a[j] = std::int16_t(add ? a[j] + col[j] : a[j] - col[j]);
#endif
        std::int32_t*         ps   = acc.psqtAccumulation[Perspective];
        const PSQTWeightType* pcol = &threatPsqtWeights[std::size_t(index) * PSQTBuckets];
        for (std::size_t k = 0; k < PSQTBuckets; ++k)
            ps[k] = add ? ps[k] + pcol[k] : ps[k] - pcol[k];
    }

    // Incremental Full_Threats update for one perspective: derive pos.state()'s
    // threats accumulator from the nearest computed ancestor by applying each
    // ply's dirtyThreats (FullThreats::append_changed_indices). A king mirror-cross
    // (requires_refresh) for this perspective falls back to a SIMD full refresh.
    template<Color Perspective>
    void update_threats(const Position& pos) const {
        if constexpr (UseThreats)
        {
            if (pos.state()->accumulatorBigThreat.computed[Perspective])
                return;

            StateInfo* states[64];
            int        n         = 0;
            bool       doRefresh = false;
            StateInfo* st        = pos.state();
            while (st->previous && !st->accumulatorBigThreat.computed[Perspective])
            {
                if (ThreatsFeatureSet::requires_refresh(st->dirtyThreats, Perspective))
                {
                    doRefresh = true;
                    break;
                }
                states[n++] = st;
                st          = st->previous;
                if (n >= 63)
                {
                    doRefresh = true;
                    break;
                }
            }
            if (doRefresh || !st->accumulatorBigThreat.computed[Perspective])
            {
                refresh_threats<Perspective>(pos);
                return;
            }

            const Square ksq  = pos.square<KING>(Perspective);
            auto*        prev = &st->accumulatorBigThreat;
            for (int i = n - 1; i >= 0; --i)
            {
                auto& cur = states[i]->accumulatorBigThreat;
                std::memcpy(cur.accumulation[Perspective], prev->accumulation[Perspective],
                            HalfDimensions * sizeof(std::int16_t));
                for (std::size_t k = 0; k < PSQTBuckets; ++k)
                    cur.psqtAccumulation[Perspective][k] = prev->psqtAccumulation[Perspective][k];

                ThreatsFeatureSet::IndexList removed, added;
                ThreatsFeatureSet::append_changed_indices(Perspective, ksq,
                                                              states[i]->dirtyThreats, removed, added);
                for (const auto idx : removed)
                    apply_threat_col<Perspective>(cur, idx, false);
                for (const auto idx : added)
                    apply_threat_col<Perspective>(cur, idx, true);

                cur.computed[Perspective] = true;
                prev                      = &cur;
            }
        }
    }

    void hint_common_access(const Position&                          pos,
                            AccumulatorCaches::Cache<HalfDimensions>* cache = nullptr) const {
        hint_common_access_for_perspective<WHITE>(pos, cache);
        hint_common_access_for_perspective<BLACK>(pos, cache);
    }

   private:
    template<Color Perspective>
    [[nodiscard]] std::pair<StateInfo*, StateInfo*>
    try_find_computed_accumulator(const Position& pos) const {
        // Look for a usable accumulator of an earlier position. We keep track
        // of the estimated gain in terms of features to be added/subtracted.
        StateInfo *st = pos.state(), *next = nullptr;
        int        gain = FeatureSet::refresh_cost(pos);
        while (st->previous && !(st->*accPtr).computed[Perspective])
        {
            // This governs when a full feature refresh is needed and how many
            // updates are better than just one full refresh.
            if (FeatureSet::requires_refresh(st, Perspective)
                || (gain -= FeatureSet::update_cost(st) + 1) < 0)
                break;
            next = st;
            st   = st->previous;
        }
        return {st, next};
    }

    // NOTE: The parameter states_to_update is an array of position states, ending with nullptr.
    //       All states must be sequential, that is states_to_update[i] must either be reachable
    //       by repeatedly applying ->previous from states_to_update[i+1] or
    //       states_to_update[i] == nullptr.
    //       computed_st must be reachable by repeatedly applying ->previous on
    //       states_to_update[0], if not nullptr.
    template<Color Perspective, size_t N>
    void update_accumulator_incremental(const Position& pos,
                                        StateInfo*      computed_st,
                                        StateInfo*      states_to_update[N]) const {
        static_assert(N > 0);
        assert(states_to_update[N - 1] == nullptr);

#ifdef VECTOR
        // Gcc-10.2 unnecessarily spills AVX2 registers if this array
        // is defined in the VECTOR code below, once in each branch
        vec_t      acc[NumRegs];
        psqt_vec_t psqt[NumPsqtRegs];
#endif

        if (states_to_update[0] == nullptr)
            return;

        // Update incrementally going back through states_to_update.

        // Gather all features to be updated.
        const Square ksq = pos.square<KING>(Perspective);

        // The size must be enough to contain the largest possible update.
        // That might depend on the feature set and generally relies on the
        // feature set's update cost calculation to be correct and never allow
        // updates with more added/removed features than MaxActiveDimensions.
        FeatureSet::IndexList removed[N - 1], added[N - 1];

        {
            int i =
              N
              - 2;  // Last potential state to update. Skip last element because it must be nullptr.
            while (states_to_update[i] == nullptr)
                --i;

            StateInfo* st2 = states_to_update[i];

            for (; i >= 0; --i)
            {
                (states_to_update[i]->*accPtr).computed[Perspective] = true;

                const StateInfo* end_state = i == 0 ? computed_st : states_to_update[i - 1];

                for (; st2 != end_state; st2 = st2->previous)
                    FeatureSet::append_changed_indices<Perspective>(ksq, st2->dirtyPiece,
                                                                    removed[i], added[i]);
            }
        }

        StateInfo* st = computed_st;

        // Now update the accumulators listed in states_to_update[], where the last element is a sentinel.
#ifdef VECTOR

        if (states_to_update[1] == nullptr && (removed[0].size() == 1 || removed[0].size() == 2)
            && added[0].size() == 1)
        {
            assert(states_to_update[0]);

            auto accIn =
              reinterpret_cast<const vec_t*>(&(st->*accPtr).accumulation[Perspective][0]);
            auto accOut = reinterpret_cast<vec_t*>(
              &(states_to_update[0]->*accPtr).accumulation[Perspective][0]);

            const IndexType offsetR0 = HalfDimensions * removed[0][0];
            auto            columnR0 = reinterpret_cast<const vec_t*>(&weights[offsetR0]);
            const IndexType offsetA  = HalfDimensions * added[0][0];
            auto            columnA  = reinterpret_cast<const vec_t*>(&weights[offsetA]);

            if (removed[0].size() == 1)
            {
                for (IndexType k = 0; k < HalfDimensions * sizeof(std::int16_t) / sizeof(vec_t);
                     ++k)
                    accOut[k] = vec_add_16(vec_sub_16(accIn[k], columnR0[k]), columnA[k]);
            }
            else
            {
                const IndexType offsetR1 = HalfDimensions * removed[0][1];
                auto            columnR1 = reinterpret_cast<const vec_t*>(&weights[offsetR1]);

                for (IndexType k = 0; k < HalfDimensions * sizeof(std::int16_t) / sizeof(vec_t);
                     ++k)
                    accOut[k] = vec_sub_16(vec_add_16(accIn[k], columnA[k]),
                                           vec_add_16(columnR0[k], columnR1[k]));
            }

            auto accPsqtIn =
              reinterpret_cast<const psqt_vec_t*>(&(st->*accPtr).psqtAccumulation[Perspective][0]);
            auto accPsqtOut = reinterpret_cast<psqt_vec_t*>(
              &(states_to_update[0]->*accPtr).psqtAccumulation[Perspective][0]);

            const IndexType offsetPsqtR0 = PSQTBuckets * removed[0][0];
            auto columnPsqtR0 = reinterpret_cast<const psqt_vec_t*>(&psqtWeights[offsetPsqtR0]);
            const IndexType offsetPsqtA = PSQTBuckets * added[0][0];
            auto columnPsqtA = reinterpret_cast<const psqt_vec_t*>(&psqtWeights[offsetPsqtA]);

            if (removed[0].size() == 1)
            {
                for (std::size_t k = 0; k < PSQTBuckets * sizeof(std::int32_t) / sizeof(psqt_vec_t);
                     ++k)
                    accPsqtOut[k] = vec_add_psqt_32(vec_sub_psqt_32(accPsqtIn[k], columnPsqtR0[k]),
                                                    columnPsqtA[k]);
            }
            else
            {
                const IndexType offsetPsqtR1 = PSQTBuckets * removed[0][1];
                auto columnPsqtR1 = reinterpret_cast<const psqt_vec_t*>(&psqtWeights[offsetPsqtR1]);

                for (std::size_t k = 0; k < PSQTBuckets * sizeof(std::int32_t) / sizeof(psqt_vec_t);
                     ++k)
                    accPsqtOut[k] =
                      vec_sub_psqt_32(vec_add_psqt_32(accPsqtIn[k], columnPsqtA[k]),
                                      vec_add_psqt_32(columnPsqtR0[k], columnPsqtR1[k]));
            }
        }
        else
        {
            for (IndexType j = 0; j < HalfDimensions / TileHeight; ++j)
            {
                // Load accumulator
                auto accTileIn = reinterpret_cast<const vec_t*>(
                  &(st->*accPtr).accumulation[Perspective][j * TileHeight]);
                for (IndexType k = 0; k < NumRegs; ++k)
                    acc[k] = vec_load(&accTileIn[k]);

                for (IndexType i = 0; states_to_update[i]; ++i)
                {
                    // Difference calculation for the deactivated features
                    for (const auto index : removed[i])
                    {
                        const IndexType offset = HalfDimensions * index + j * TileHeight;
                        auto            column = reinterpret_cast<const vec_t*>(&weights[offset]);
                        for (IndexType k = 0; k < NumRegs; ++k)
                            acc[k] = vec_sub_16(acc[k], column[k]);
                    }

                    // Difference calculation for the activated features
                    for (const auto index : added[i])
                    {
                        const IndexType offset = HalfDimensions * index + j * TileHeight;
                        auto            column = reinterpret_cast<const vec_t*>(&weights[offset]);
                        for (IndexType k = 0; k < NumRegs; ++k)
                            acc[k] = vec_add_16(acc[k], column[k]);
                    }

                    // Store accumulator
                    auto accTileOut = reinterpret_cast<vec_t*>(
                      &(states_to_update[i]->*accPtr).accumulation[Perspective][j * TileHeight]);
                    for (IndexType k = 0; k < NumRegs; ++k)
                        vec_store(&accTileOut[k], acc[k]);
                }
            }

            for (IndexType j = 0; j < PSQTBuckets / PsqtTileHeight; ++j)
            {
                // Load accumulator
                auto accTilePsqtIn = reinterpret_cast<const psqt_vec_t*>(
                  &(st->*accPtr).psqtAccumulation[Perspective][j * PsqtTileHeight]);
                for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                    psqt[k] = vec_load_psqt(&accTilePsqtIn[k]);

                for (IndexType i = 0; states_to_update[i]; ++i)
                {
                    // Difference calculation for the deactivated features
                    for (const auto index : removed[i])
                    {
                        const IndexType offset = PSQTBuckets * index + j * PsqtTileHeight;
                        auto columnPsqt = reinterpret_cast<const psqt_vec_t*>(&psqtWeights[offset]);
                        for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                            psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
                    }

                    // Difference calculation for the activated features
                    for (const auto index : added[i])
                    {
                        const IndexType offset = PSQTBuckets * index + j * PsqtTileHeight;
                        auto columnPsqt = reinterpret_cast<const psqt_vec_t*>(&psqtWeights[offset]);
                        for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                            psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
                    }

                    // Store accumulator
                    auto accTilePsqtOut = reinterpret_cast<psqt_vec_t*>(
                      &(states_to_update[i]->*accPtr)
                         .psqtAccumulation[Perspective][j * PsqtTileHeight]);
                    for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                        vec_store_psqt(&accTilePsqtOut[k], psqt[k]);
                }
            }
        }
#else
        for (IndexType i = 0; states_to_update[i]; ++i)
        {
            std::memcpy((states_to_update[i]->*accPtr).accumulation[Perspective],
                        (st->*accPtr).accumulation[Perspective], HalfDimensions * sizeof(BiasType));

            for (std::size_t k = 0; k < PSQTBuckets; ++k)
                (states_to_update[i]->*accPtr).psqtAccumulation[Perspective][k] =
                  (st->*accPtr).psqtAccumulation[Perspective][k];

            st = states_to_update[i];

            // Difference calculation for the deactivated features
            for (const auto index : removed[i])
            {
                const IndexType offset = HalfDimensions * index;

                for (IndexType j = 0; j < HalfDimensions; ++j)
                    (st->*accPtr).accumulation[Perspective][j] -= weights[offset + j];

                for (std::size_t k = 0; k < PSQTBuckets; ++k)
                    (st->*accPtr).psqtAccumulation[Perspective][k] -=
                      psqtWeights[index * PSQTBuckets + k];
            }

            // Difference calculation for the activated features
            for (const auto index : added[i])
            {
                const IndexType offset = HalfDimensions * index;

                for (IndexType j = 0; j < HalfDimensions; ++j)
                    (st->*accPtr).accumulation[Perspective][j] += weights[offset + j];

                for (std::size_t k = 0; k < PSQTBuckets; ++k)
                    (st->*accPtr).psqtAccumulation[Perspective][k] +=
                      psqtWeights[index * PSQTBuckets + k];
            }
        }
#endif
    }

    template<Color Perspective>
    void update_accumulator_refresh(const Position& pos) const {
#ifdef VECTOR
        // Gcc-10.2 unnecessarily spills AVX2 registers if this array
        // is defined in the VECTOR code below, once in each branch
        vec_t      acc[NumRegs];
        psqt_vec_t psqt[NumPsqtRegs];
#endif

        // Refresh the accumulator
        // Could be extracted to a separate function because it's done in 2 places,
        // but it's unclear if compilers would correctly handle register allocation.
        auto& accumulator                 = pos.state()->*accPtr;
        accumulator.computed[Perspective] = true;
        FeatureSet::IndexList active;
        FeatureSet::append_active_indices<Perspective>(pos, active);

#ifdef VECTOR
        for (IndexType j = 0; j < HalfDimensions / TileHeight; ++j)
        {
            auto biasesTile = reinterpret_cast<const vec_t*>(&biases[j * TileHeight]);
            for (IndexType k = 0; k < NumRegs; ++k)
                acc[k] = biasesTile[k];

            for (const auto index : active)
            {
                const IndexType offset = HalfDimensions * index + j * TileHeight;
                auto            column = reinterpret_cast<const vec_t*>(&weights[offset]);

                for (unsigned k = 0; k < NumRegs; ++k)
                    acc[k] = vec_add_16(acc[k], column[k]);
            }

            auto accTile =
              reinterpret_cast<vec_t*>(&accumulator.accumulation[Perspective][j * TileHeight]);
            for (unsigned k = 0; k < NumRegs; k++)
                vec_store(&accTile[k], acc[k]);
        }

        for (IndexType j = 0; j < PSQTBuckets / PsqtTileHeight; ++j)
        {
            for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                psqt[k] = vec_zero_psqt();

            for (const auto index : active)
            {
                const IndexType offset = PSQTBuckets * index + j * PsqtTileHeight;
                auto columnPsqt        = reinterpret_cast<const psqt_vec_t*>(&psqtWeights[offset]);

                for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                    psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
            }

            auto accTilePsqt = reinterpret_cast<psqt_vec_t*>(
              &accumulator.psqtAccumulation[Perspective][j * PsqtTileHeight]);
            for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                vec_store_psqt(&accTilePsqt[k], psqt[k]);
        }

#else
        std::memcpy(accumulator.accumulation[Perspective], biases,
                    HalfDimensions * sizeof(BiasType));

        for (std::size_t k = 0; k < PSQTBuckets; ++k)
            accumulator.psqtAccumulation[Perspective][k] = 0;

        for (const auto index : active)
        {
            const IndexType offset = HalfDimensions * index;

            for (IndexType j = 0; j < HalfDimensions; ++j)
                accumulator.accumulation[Perspective][j] += weights[offset + j];

            for (std::size_t k = 0; k < PSQTBuckets; ++k)
                accumulator.psqtAccumulation[Perspective][k] +=
                  psqtWeights[index * PSQTBuckets + k];
        }
#endif
    }

    // Full refresh using the AccumulatorCaches ("finny tables"). Instead of
    // rebuilding from biases over all active features, we diff against the
    // cached accumulation for this (king square, perspective) and apply only the
    // pieces that changed, then store the result back into the cache and copy it
    // into the position accumulator. Produces bit-identical accumulator values
    // to update_accumulator_refresh(), only faster.
    template<Color Perspective>
    void update_accumulator_refresh_cache(const Position&                          pos,
                                          AccumulatorCaches::Cache<HalfDimensions>* cache) const {
        assert(cache != nullptr);

#ifdef VECTOR
        vec_t      acc[NumRegs];
        psqt_vec_t psqt[NumPsqtRegs];
#endif

        const Square ksq   = pos.square<KING>(Perspective);
        auto&        entry = (*cache)[ksq][Perspective];

        FeatureSet::IndexList removed, added;

        for (Color c : {WHITE, BLACK})
            for (PieceType pt = PAWN; pt <= KING; ++pt)
            {
                const Piece    piece    = make_piece(c, pt);
                const Bitboard oldBB     = entry.byColorBB[c] & entry.byTypeBB[pt];
                const Bitboard newBB     = pos.pieces(c, pt);
                Bitboard       toRemove  = oldBB & ~newBB;
                Bitboard       toAdd      = newBB & ~oldBB;

                while (toRemove)
                {
                    Square sq = pop_lsb(toRemove);
                    removed.push_back(FeatureSet::make_index<Perspective>(sq, piece, ksq));
                }
                while (toAdd)
                {
                    Square sq = pop_lsb(toAdd);
                    added.push_back(FeatureSet::make_index<Perspective>(sq, piece, ksq));
                }
            }

        auto& accumulator                 = pos.state()->*accPtr;
        accumulator.computed[Perspective] = true;

#ifdef VECTOR
        for (IndexType j = 0; j < HalfDimensions / TileHeight; ++j)
        {
            auto* entryTile = reinterpret_cast<vec_t*>(&entry.accumulation[j * TileHeight]);
            for (IndexType k = 0; k < NumRegs; ++k)
                acc[k] = vec_load(&entryTile[k]);

            for (std::size_t i = 0; i < removed.size(); ++i)
            {
                const IndexType index  = removed[i];
                const IndexType offset = HalfDimensions * index + j * TileHeight;
                auto            column = reinterpret_cast<const vec_t*>(&weights[offset]);
                for (IndexType k = 0; k < NumRegs; ++k)
                    acc[k] = vec_sub_16(acc[k], column[k]);
            }
            for (std::size_t i = 0; i < added.size(); ++i)
            {
                const IndexType index  = added[i];
                const IndexType offset = HalfDimensions * index + j * TileHeight;
                auto            column = reinterpret_cast<const vec_t*>(&weights[offset]);
                for (IndexType k = 0; k < NumRegs; ++k)
                    acc[k] = vec_add_16(acc[k], column[k]);
            }

            for (IndexType k = 0; k < NumRegs; ++k)
                vec_store(&entryTile[k], acc[k]);
            auto* accTile =
              reinterpret_cast<vec_t*>(&accumulator.accumulation[Perspective][j * TileHeight]);
            for (IndexType k = 0; k < NumRegs; ++k)
                vec_store(&accTile[k], acc[k]);
        }

        for (IndexType j = 0; j < PSQTBuckets / PsqtTileHeight; ++j)
        {
            auto* entryTilePsqt =
              reinterpret_cast<psqt_vec_t*>(&entry.psqtAccumulation[j * PsqtTileHeight]);
            for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                psqt[k] = vec_load_psqt(&entryTilePsqt[k]);

            for (std::size_t i = 0; i < removed.size(); ++i)
            {
                const IndexType index  = removed[i];
                const IndexType offset = PSQTBuckets * index + j * PsqtTileHeight;
                auto columnPsqt        = reinterpret_cast<const psqt_vec_t*>(&psqtWeights[offset]);
                for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                    psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
            }
            for (std::size_t i = 0; i < added.size(); ++i)
            {
                const IndexType index  = added[i];
                const IndexType offset = PSQTBuckets * index + j * PsqtTileHeight;
                auto columnPsqt        = reinterpret_cast<const psqt_vec_t*>(&psqtWeights[offset]);
                for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                    psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
            }

            for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                vec_store_psqt(&entryTilePsqt[k], psqt[k]);
            auto* accTilePsqt = reinterpret_cast<psqt_vec_t*>(
              &accumulator.psqtAccumulation[Perspective][j * PsqtTileHeight]);
            for (std::size_t k = 0; k < NumPsqtRegs; ++k)
                vec_store_psqt(&accTilePsqt[k], psqt[k]);
        }
#else
        for (const auto index : removed)
        {
            const IndexType offset = HalfDimensions * index;
            for (IndexType j = 0; j < HalfDimensions; ++j)
                entry.accumulation[j] -= weights[offset + j];
            for (std::size_t k = 0; k < PSQTBuckets; ++k)
                entry.psqtAccumulation[k] -= psqtWeights[index * PSQTBuckets + k];
        }
        for (const auto index : added)
        {
            const IndexType offset = HalfDimensions * index;
            for (IndexType j = 0; j < HalfDimensions; ++j)
                entry.accumulation[j] += weights[offset + j];
            for (std::size_t k = 0; k < PSQTBuckets; ++k)
                entry.psqtAccumulation[k] += psqtWeights[index * PSQTBuckets + k];
        }

        std::memcpy(accumulator.accumulation[Perspective], entry.accumulation,
                    HalfDimensions * sizeof(std::int16_t));
        std::memcpy(accumulator.psqtAccumulation[Perspective], entry.psqtAccumulation,
                    PSQTBuckets * sizeof(std::int32_t));
#endif

        // Record the current piece configuration so the next refresh against this
        // entry diffs from here.
        for (Color c : {WHITE, BLACK})
            entry.byColorBB[c] = pos.pieces(c);
        for (PieceType pt = PAWN; pt <= KING; ++pt)
            entry.byTypeBB[pt] = pos.pieces(pt);
    }

    template<Color Perspective>
    void hint_common_access_for_perspective(const Position&                          pos,
                                            AccumulatorCaches::Cache<HalfDimensions>* cache
                                            = nullptr) const {

        // Works like update_accumulator, but performs less work.
        // Updates ONLY the accumulator for pos.

        // Look for a usable accumulator of an earlier position. We keep track
        // of the estimated gain in terms of features to be added/subtracted.
        // Fast early exit.
        if ((pos.state()->*accPtr).computed[Perspective])
            return;

        auto [oldest_st, _] = try_find_computed_accumulator<Perspective>(pos);

        if ((oldest_st->*accPtr).computed[Perspective])
        {
            // Only update current position accumulator to minimize work.
            StateInfo* states_to_update[2] = {pos.state(), nullptr};
            update_accumulator_incremental<Perspective, 2>(pos, oldest_st, states_to_update);
        }
        else if (cache)
            update_accumulator_refresh_cache<Perspective>(pos, cache);
        else
            update_accumulator_refresh<Perspective>(pos);
    }

    template<Color Perspective>
    void update_accumulator(const Position&                          pos,
                            AccumulatorCaches::Cache<HalfDimensions>* cache = nullptr) const {

        auto [oldest_st, next] = try_find_computed_accumulator<Perspective>(pos);

        if ((oldest_st->*accPtr).computed[Perspective])
        {
            if (next == nullptr)
                return;

            // Now update the accumulators listed in states_to_update[], where the last element is a sentinel.
            // Currently we update 2 accumulators.
            //     1. for the current position
            //     2. the next accumulator after the computed one
            // The heuristic may change in the future.
            StateInfo* states_to_update[3] = {next, next == pos.state() ? nullptr : pos.state(),
                                              nullptr};

            ++g_dbg_incremental;
            update_accumulator_incremental<Perspective, 3>(pos, oldest_st, states_to_update);
        }
        else if (cache)
        {
            ++g_dbg_refresh;
            update_accumulator_refresh_cache<Perspective>(pos, cache);
        }
        else
        {
            ++g_dbg_refresh;
            update_accumulator_refresh<Perspective>(pos);
        }
    }

    alignas(CacheLineSize) BiasType biases[HalfDimensions];
    alignas(CacheLineSize) WeightType weights[HalfDimensions * InputDimensions];
    alignas(CacheLineSize) PSQTWeightType psqtWeights[InputDimensions * PSQTBuckets];
    // SFNNv10 Full_Threats tables (Big net only; size 1 placeholder otherwise).
    alignas(CacheLineSize) ThreatWeightType
      threatWeights[UseThreats ? std::size_t(HalfDimensions) * ThreatInputDimensions : 1];
    alignas(CacheLineSize) PSQTWeightType
      threatPsqtWeights[UseThreats ? std::size_t(ThreatInputDimensions) * PSQTBuckets : 1];
};

}  // namespace Stockfish::Eval::NNUE

#endif  // #ifndef NNUE_FEATURE_TRANSFORMER_H_INCLUDED

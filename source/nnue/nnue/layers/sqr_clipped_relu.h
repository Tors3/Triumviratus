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

// Definition of layer ClippedReLU of NNUE evaluation function

#ifndef NNUE_LAYERS_SQR_CLIPPED_RELU_H_INCLUDED
#define NNUE_LAYERS_SQR_CLIPPED_RELU_H_INCLUDED

#include <algorithm>
#include <cstdint>
#include <iosfwd>

#include "../nnue_common.h"
#include "../simd.h"

namespace Triumviratus::Eval::NNUE::Layers {

// Clipped ReLU
template<IndexType InDims, int WeightScaleBitsLocal = WeightScaleBits>
class SqrClippedReLU {
   public:
    // Input/output type
    using InputType  = i32;
    using OutputType = u8;

    // Number of input/output dimensions
    static constexpr IndexType InputDimensions  = InDims;
    static constexpr IndexType OutputDimensions = InputDimensions;
    static constexpr IndexType PaddedOutputDimensions =
      ceil_to_multiple<IndexType>(OutputDimensions, 32);

    using OutputBuffer = OutputType[PaddedOutputDimensions];

    // Hash value embedded in the evaluation file
    static constexpr u32 get_hash_value(u32 prevHash) {
        u32 hashValue = 0x538D24C7u;
        hashValue += prevHash;
        // TODO: consider including WeightScaleBitsLocal in the hash value.
        // For now omitted on purpose because not written by trainer (yet)
        return hashValue;
    }

    // Read network parameters
    bool read_parameters(std::istream&) { return true; }

    // Write network parameters
    bool write_parameters(std::ostream&) const { return true; }

    usize get_content_hash() const {
        usize h = 0;
        hash_combine(h, get_hash_value(0));
        return h;
    }

#if defined(USE_AVX2)
    // Attivazione FUSA (port SF 1c384d3a alle nostre dimensioni): calcola in UNA passata sia la
    // ReLU quadratica sia quella lineare dallo stesso input. Prima si leggeva e impacchettava
    // due volte lo stesso `fc_0_out` (32 i32 = 128 byte) -- ora il pack i32->i16 si fa una volta
    // sola e da quello escono entrambe le uscite.
    // ⚠️ SF la abilita solo su AVX2-senza-VNNI/AVX512; da noi conviene anche sul flagship perche'
    // col concat ristrutturato i due store sono allineati e si risparmia comunque l'intera
    // seconda passata. Equivalenza col percorso separato: per la lineare `packs_epi32 + max(0)`
    // da' gli stessi byte di `packus_epi32` (i negativi vanno a 0 in entrambi; sopra 32767 la
    // successiva saturazione a i8 collassa comunque a 127) -> bench invariato = prova.
    void propagate_pair(const InputType* input, OutputType* squared, OutputType* clipped) const {
        static_assert(WeightScaleBitsLocal >= 5 && WeightScaleBitsLocal <= 8);
        static_assert(InputDimensions % 32 == 0);
        constexpr int SimdShiftAmount = WeightScaleBitsLocal * 2 + 7 - 16;
        constexpr IndexType NumChunks = InputDimensions / 32;

        const auto    in   = reinterpret_cast<const __m256i*>(input);
        const auto    outS = reinterpret_cast<__m256i*>(squared);
        const auto    outC = reinterpret_cast<__m256i*>(clipped);
        const __m256i zero = _mm256_setzero_si256();

        for (IndexType i = 0; i < NumChunks; ++i)
        {
            // pack i32 -> i16 UNA VOLTA (permute4x64 ricuce l'interleaving di corsia di AVX2)
            const __m256i w0 = _mm256_permute4x64_epi64(
              _mm256_packs_epi32(_mm256_load_si256(&in[i * 4 + 0]),
                                 _mm256_load_si256(&in[i * 4 + 1])), 0xD8);
            const __m256i w1 = _mm256_permute4x64_epi64(
              _mm256_packs_epi32(_mm256_load_si256(&in[i * 4 + 2]),
                                 _mm256_load_si256(&in[i * 4 + 3])), 0xD8);

            const __m256i s0 = _mm256_srli_epi16(_mm256_mulhi_epi16(w0, w0), SimdShiftAmount);
            const __m256i s1 = _mm256_srli_epi16(_mm256_mulhi_epi16(w1, w1), SimdShiftAmount);
            _mm256_store_si256(&outS[i],
                               _mm256_permute4x64_epi64(_mm256_packs_epi16(s0, s1), 0xD8));

            const __m256i c0 =
              _mm256_srli_epi16(_mm256_max_epi16(w0, zero), WeightScaleBitsLocal);
            const __m256i c1 =
              _mm256_srli_epi16(_mm256_max_epi16(w1, zero), WeightScaleBitsLocal);
            _mm256_store_si256(&outC[i],
                               _mm256_permute4x64_epi64(_mm256_packs_epi16(c0, c1), 0xD8));
        }
    }
#endif

    // Forward propagation
    void propagate(const InputType* input, OutputType* output) const {
        static_assert(WeightScaleBitsLocal >= 5 && WeightScaleBitsLocal <= 8,
                      "SqrClippedReLU only support WeightScaleBitsLocal between 5 and 8");
        // After squaring we need to shift by WeightScaleBitsLocal * 2 + 7
        // MulHi strips the lower 16 bits (i.e. shift by 16) so we need to shift out the remaining.
        [[maybe_unused]] constexpr int SimdShiftAmount = WeightScaleBitsLocal * 2 + 7 - 16;

// ⛔ LEZIONE MISURATA (2026-07-19): sulle NOSTRE dimensioni i vettori LARGHI PERDONO.
// InputDimensions = 32 (L2+1) => un ramo a 256/512 bit fa UNA sola iterazione, quindi conta la
// LATENZA della catena, non la larghezza; il ramo SSE2 fa DUE iterazioni indipendenti che il core
// sovrappone, e a 128 bit non esistono corsie da ricucire.
//   * ramo AVX-512 (SF 4c78ba89, cvtsepi32_epi16 + inserti64x4): -2.84% (inconclusivo vs rumore)
//   * attivazione FUSA propagate_pair (SF 1c384d3a, 4x permute4x64 di ricucitura): -2.09% e -1.45%
//     su DUE giri a lati invertiti = segno coerente = REALE.
// SF stessa abilita propagate_pair solo su AVX2-senza-VNNI/AVX512: sulle build larghe non conviene
// nemmeno a loro. NON riprovare a queste dimensioni; riconsiderare solo se L2 crescesse molto.
#if defined(USE_SSE2)
        constexpr IndexType NumChunks = InputDimensions / 16;
        const auto          in        = reinterpret_cast<const __m128i*>(input);
        const auto          out       = reinterpret_cast<__m128i*>(output);
        for (IndexType i = 0; i < NumChunks; ++i)
        {
            __m128i words0 =
              _mm_packs_epi32(_mm_load_si128(&in[i * 4 + 0]), _mm_load_si128(&in[i * 4 + 1]));
            __m128i words1 =
              _mm_packs_epi32(_mm_load_si128(&in[i * 4 + 2]), _mm_load_si128(&in[i * 4 + 3]));

            words0 = _mm_srli_epi16(_mm_mulhi_epi16(words0, words0), SimdShiftAmount);
            words1 = _mm_srli_epi16(_mm_mulhi_epi16(words1, words1), SimdShiftAmount);

            _mm_store_si128(&out[i], _mm_packs_epi16(words0, words1));
        }
        constexpr IndexType Start = NumChunks * 16;

#elif defined(USE_LASX)
        constexpr IndexType NumChunks = InputDimensions / 32;
        const auto          in        = reinterpret_cast<const __m256i*>(input);
        const auto          out       = reinterpret_cast<__m256i*>(output);
        for (IndexType i = 0; i < NumChunks; ++i)
        {
            const __m256i words0 = __lasx_xvssrani_h_w(in[i * 4 + 1], in[i * 4 + 0], 0);
            const __m256i words1 = __lasx_xvssrani_h_w(in[i * 4 + 3], in[i * 4 + 2], 0);
            const __m256i sqr0   = __lasx_xvmuh_h(words0, words0);
            const __m256i sqr1   = __lasx_xvmuh_h(words1, words1);
            __m256i       packed;
            packed               = __lasx_xvssrlni_b_h(sqr1, sqr0, SimdShiftAmount);
            const __m256i permed = __lasx_xvpermi_d(packed, 0xD8);
            __lasx_xvst(__lasx_xvshuf4i_w(permed, 0xD8), out + i, 0);
        }
        constexpr IndexType Start = NumChunks * 32;

#elif defined(USE_LSX)
        constexpr IndexType NumChunks = InputDimensions / 16;
        const auto          in        = reinterpret_cast<const __m128i*>(input);
        const auto          out       = reinterpret_cast<__m128i*>(output);
        for (IndexType i = 0; i < NumChunks; ++i)
        {
            const __m128i words0 = __lsx_vssrani_h_w(in[i * 4 + 1], in[i * 4 + 0], 0);
            const __m128i words1 = __lsx_vssrani_h_w(in[i * 4 + 3], in[i * 4 + 2], 0);
            const __m128i sqr0   = __lsx_vmuh_h(words0, words0);
            const __m128i sqr1   = __lsx_vmuh_h(words1, words1);
            out[i]               = __lsx_vssrlni_b_h(sqr1, sqr0, SimdShiftAmount);
        }
        constexpr IndexType Start = NumChunks * 16;

#elif defined(USE_NEON)
        constexpr IndexType NumChunks = InputDimensions / 16;
        const auto          in        = reinterpret_cast<const int32x4_t*>(input);
        const auto          out       = reinterpret_cast<int8x16_t*>(output);
        for (IndexType i = 0; i < NumChunks; ++i)
        {
            const int16x8_t words0 =
              vcombine_s16(vqmovn_s32(in[i * 4 + 0]), vqmovn_s32(in[i * 4 + 1]));
            const int16x8_t words1 =
              vcombine_s16(vqmovn_s32(in[i * 4 + 2]), vqmovn_s32(in[i * 4 + 3]));
            // Neon needs to shift by one more since the used simd instruction does
            // `Saturating Doubling Multiply High` (doubling before shift by 16).
            const int16x8_t r0 = vshrq_n_s16(vqdmulhq_s16(words0, words0), SimdShiftAmount + 1);
            const int16x8_t r1 = vshrq_n_s16(vqdmulhq_s16(words1, words1), SimdShiftAmount + 1);

            out[i] = vcombine_s8(vqmovn_s16(r0), vqmovn_s16(r1));
        }
        constexpr IndexType Start = NumChunks * 16;

#else
        constexpr IndexType Start = 0;
#endif

        for (IndexType i = Start; i < InputDimensions; ++i)
        {
            output[i] = static_cast<OutputType>(
              // Really should be /127 but we need to make it fast so we right-shift
              // by an extra 7 bits instead. Needs to be accounted for in the trainer.
              std::min(127ll,
                       ((long long) (input[i]) * input[i]) >> (2 * WeightScaleBitsLocal + 7)));
        }
    }
};

}  // namespace Triumviratus::Eval::NNUE::Layers

#endif  // NNUE_LAYERS_SQR_CLIPPED_RELU_H_INCLUDED

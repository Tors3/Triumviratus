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

#include "nnue_accumulator.h"

#include <cassert>
#include <new>

#include "../../profile.h"
#include "../bitboard.h"
#include "../misc.h"
#include "../position.h"
#include "../types.h"
#include "nnue_architecture.h"
#include "nnue_common.h"
#include "nnue_feature_transformer.h"  // IWYU pragma: keep
#include "simd.h"

namespace Triumviratus::Eval::NNUE {

using namespace SIMD;

namespace {

template<bool Forward>
void update_accumulator_incremental(Color                     perspective,
                                    const FeatureTransformer& featureTransformer,
                                    const Square              ksq,
                                    AccumulatorState&         target_state,
                                    const AccumulatorState&   computed);

void update_accumulator_refresh_cache(Color                     perspective,
                                      const FeatureTransformer& featureTransformer,
                                      const Position&           pos,
                                      AccumulatorState&         accumulatorState,
                                      AccumulatorCaches&        cache);

void update_accumulator_hybrid(Color                     perspective,
                               const Position&           pos,
                               const FeatureTransformer& featureTransformer,
                               AccumulatorState&         target,
                               const AccumulatorState&   computed,
                               AccumulatorCaches&        cache);
}

const AccumulatorState& AccumulatorStack::latest() const noexcept { return accumulators[size - 1]; }

AccumulatorState& AccumulatorStack::mut_latest() noexcept { return accumulators[size - 1]; }

void AccumulatorStack::reset() noexcept {
    accumulators[0].dirtyPiece = {};
    new (&accumulators[0].dirtyThreats) DirtyThreats;
    accumulators[0].computed.fill(false);
    size = 1;
}

std::tuple<DirtyPiece&, DirtyThreats&, DirtyPawns&> AccumulatorStack::push() noexcept {
    assert(size < MaxSize);
    auto& st = accumulators[size];
    st.computed.fill(false);
    new (&st.dirtyThreats) DirtyThreats;
    st.dirtyPawns.any = false;  // TRANN1: apply_move la riempie se la mossa tocca pedoni
    size++;
    return {st.dirtyPiece, st.dirtyThreats, st.dirtyPawns};
}

void AccumulatorStack::pop() noexcept {
    assert(size > 1);
    size--;
}

void AccumulatorStack::evaluate(const Position&           pos,
                                const FeatureTransformer& featureTransformer,
                                // Silence spurious warning on GCC 10
                                [[maybe_unused]] AccumulatorCaches& cache) noexcept {
#ifdef TRIUMV_PERSP_TOGETHER
    // ⛔ MISURATO −0,70% IL 3/08/2026 — default OFF, tenuto solo come punto di
    // partenza per il porting COMPLETO. Questa e' meta' del commit di Stockfish:
    // riordina le prospettive ma NON condivide la decodifica delle tuple, che e'
    // da dove viene il loro guadagno. Cosi' si prende solo il lato negativo —
    // alternando le prospettive a ogni transizione il working set attivo passa da
    // un accumulatore (2 KB + colonne) a due, e su un percorso memory-bound la
    // localita' della struttura GRANDE conta piu' di quella della dirty list.
    // Per farlo rendere serve riscrivere `append_changed_indices` in modo che
    // decodifichi i bitfield una volta sola producendo le liste di ENTRAMBE le
    // prospettive (indici e push restano separati: l'orientamento e' diverso).
    //
    // Porting di Stockfish 7b550409 "Update NNUE perspectives together".
    // Quando ENTRAMBE le prospettive sono aggiornabili in modo incrementale, si
    // percorre il suffisso comune una volta sola, facendo per ogni transizione
    // prima una prospettiva e poi l'altra. Prima si completava tutta la catena
    // per il BIANCO e poi tutta per il NERO: la stessa `dirtyThreats` di ogni
    // stato veniva letta due volte a distanza di un intero aggiornamento di
    // accumulatore (~1024 int16 per prospettiva), quindi fuori dalla cache.
    // Ora le due letture sono adiacenti. Nessun cambio funzionale: indici,
    // ordine delle liste e aritmetica restano per prospettiva.
    {
        const auto lastW = find_last_usable_accumulator(WHITE);
        const auto lastB = find_last_usable_accumulator(BLACK);

        if (accumulators[lastW].computed[WHITE] && accumulators[lastB].computed[BLACK])
        {
#ifdef TRIUMV_PROFILE
            prof_n_inc += 2;
#endif
            PROF_GUARD(prof_acc_inc);
            const Square ksqW  = pos.square<KING>(WHITE);
            const Square ksqB  = pos.square<KING>(BLACK);
            const usize  start = lastW < lastB ? lastW : lastB;

            for (usize next = start + 1; next < size; next++)
            {
                // `next > lastX` garantisce che accumulators[next-1] sia gia'
                // calcolato per quella prospettiva: o e' l'ancora lastX, o e'
                // stato prodotto al giro precedente di questo stesso ciclo.
                if (next > lastW)
                    update_accumulator_incremental<true>(WHITE, featureTransformer, ksqW,
                                                         accumulators[next], accumulators[next - 1]);
                if (next > lastB)
                    update_accumulator_incremental<true>(BLACK, featureTransformer, ksqB,
                                                         accumulators[next], accumulators[next - 1]);
            }
            return;
        }
    }
#endif

    evaluate_side(WHITE, pos, featureTransformer, cache);
    evaluate_side(BLACK, pos, featureTransformer, cache);
}

void AccumulatorStack::evaluate_side(Color                     perspective,
                                     const Position&           pos,
                                     const FeatureTransformer& featureTransformer,
                                     AccumulatorCaches&        cache) noexcept {

    const auto last_usable_accum = find_last_usable_accumulator(perspective);

    if (accumulators[last_usable_accum].computed[perspective])
    {
#ifdef TRIUMV_PROFILE
        prof_n_inc++;
#endif
        PROF_GUARD(prof_acc_inc);
        forward_update_incremental(perspective, pos, featureTransformer, last_usable_accum);
    }

    else
    {
#ifndef TRIUMV_NO_HYBRID_ACC
        // Percorso HYBRID (SF db98633b): una mossa di re che NON attraversa la
        // colonna d/e lascia validi tutti gli indici di threat/PawnPair/PassedPawns,
        // perche' quelli dipendono da OrientTBL[ksq] che ha due soli valori. In quel
        // caso si riusa l'accumulatore precedente invece di ricostruire il 59,6%
        // delle colonne da zero.
        //   - `add_sq == SQ_NONE` esclude l'arrocco (muoverebbe anche la torre)
        //   - sotto i 15 pezzi le feature attive sono poche e ricostruirle costa
        //     meno che ricavare l'HalfKA precedente dalla cache
        constexpr int MIN_PC_COUNT_HYBRID = 15;
        const auto&   dp                  = latest().dirtyPiece;
        if (size >= 2 && dp.pc == make_piece(perspective, KING) && dp.to != SQ_NONE
            && accumulators[size - 2].computed[perspective]
            && pos.count<ALL_PIECES>() >= MIN_PC_COUNT_HYBRID
            && ((int(dp.from) & 0b100) == (int(dp.to) & 0b100)) && dp.add_sq == SQ_NONE)
        {
    #ifdef TRIUMV_PROFILE
            prof_refresh_same_orient++;
    #endif
            PROF_GUARD(prof_acc_refresh);
            update_accumulator_hybrid(perspective, pos, featureTransformer, mut_latest(),
                                      accumulators[size - 2], cache);
            return;
        }
    #ifdef TRIUMV_PROFILE
        prof_refresh_cross_orient++;
    #endif
#endif
#ifdef TRIUMV_PROFILE
        prof_n_refresh++;
#endif
        PROF_GUARD(prof_acc_refresh);
        update_accumulator_refresh_cache(perspective, featureTransformer, pos, mut_latest(), cache);
        backward_update_incremental(perspective, pos, featureTransformer, last_usable_accum);
    }
}

// Find the earliest usable accumulator, this can either be a computed accumulator or the accumulator
// state just before a change that requires full refresh.
usize AccumulatorStack::find_last_usable_accumulator(Color perspective) const noexcept {

    for (usize curr_idx = size - 1; curr_idx > 0; curr_idx--)
    {
        if (accumulators[curr_idx].computed[perspective])
            return curr_idx;

        // Threat feature set refreshes require a king move across the center, i.e.,
        // a subset of halfka refreshes
        if (PSQFeatureSet::requires_refresh(accumulators[curr_idx].dirtyPiece, perspective))
            return curr_idx;
    }

    return 0;
}

void AccumulatorStack::forward_update_incremental(Color                     perspective,
                                                  const Position&           pos,
                                                  const FeatureTransformer& featureTransformer,
                                                  const usize               begin) noexcept {

    assert(begin < accumulators.size());
    assert(accumulators[begin].computed[perspective]);

    const Square ksq = pos.square<KING>(perspective);

    for (usize next = begin + 1; next < size; next++)
        update_accumulator_incremental<true>(perspective, featureTransformer, ksq,
                                             accumulators[next], accumulators[next - 1]);

    assert(latest().computed[perspective]);
}

void AccumulatorStack::backward_update_incremental(Color                     perspective,
                                                   const Position&           pos,
                                                   const FeatureTransformer& featureTransformer,
                                                   const usize               end) noexcept {

    assert(end < accumulators.size());
    assert(end < size);
    assert(latest().computed[perspective]);

    const Square ksq = pos.square<KING>(perspective);

    for (i64 next = i64(size) - 2; next >= i64(end); next--)
        update_accumulator_incremental<false>(perspective, featureTransformer, ksq,
                                              accumulators[next], accumulators[next + 1]);

    assert(accumulators[end].computed[perspective]);
}

namespace {

void apply_combined(Color                              perspective,
                    const FeatureTransformer&          featureTransformer,
                    const AccumulatorState&            from,
                    AccumulatorState&                  to,
                    const PSQFeatureSet::IndexList&    psqAdded,
                    const PSQFeatureSet::IndexList&    psqRemoved,
                    const ThreatFeatureSet::IndexList& thrAdded,
                    const ThreatFeatureSet::IndexList& thrRemoved) {
    constexpr IndexType Dimensions = FeatureTransformer::OutputDimensions;

    const auto& fromAcc = from.accumulation[perspective];
    auto&       toAcc   = to.accumulation[perspective];

    const auto& fromPsqtAcc = from.psqtAccumulation[perspective];
    auto&       toPsqtAcc   = to.psqtAccumulation[perspective];

#ifdef VECTOR
    using Tiling = SIMDTiling<Dimensions, Dimensions, PSQTBuckets>;

    vec_t      acc[Tiling::NumRegs];
    psqt_vec_t psqt[Tiling::NumPsqtRegs];

    const auto* psqWeights    = &featureTransformer.weights[0];
    const auto* threatWeights = &featureTransformer.threatWeights[0];

    // ⛔ PROVATO E RIGETTATO il 3/08/2026: prefetchare le righe PSQT (una linea di
    // cache ciascuna, ~10,5 per update, consumate nel secondo ciclo qui sotto) misura
    // **-1,04% NPS** — 52/150 posizioni, z = 3,67, p = 0,0002, nodi identici. Negativo
    // e significativo, non rumore.
    // 🔑 Regola che ne esce, e che spiega anche perche' il prefetch delle righe HalfKA
    // vale +1,3%: **il prefetch paga solo dove la tabella non ci sta in cache.**
    //   pesi FT   : threatWeights ~61 MB + weights ~46 MB  -> miss garantiti, prefetch OK
    //   pesi PSQT : psqtWeights 0,7 MB + threatPsqt 1,9 MB -> 2,6 MB, stanno in L2/L3
    //                                                         ed erano gia' hit
    // Su una riga gia' in cache il prefetch e' solo un'istruzione in piu' nel percorso
    // piu' caldo del motore. Prima di prefetchare qualcosa: quanto e' grande la tabella?

    for (IndexType j = 0; j < Dimensions / Tiling::TileHeight; ++j)
    {
        const usize tileOff  = j * Tiling::TileHeight;
        auto*       fromTile = reinterpret_cast<const vec_t*>(&fromAcc[tileOff]);
        auto*       toTile   = reinterpret_cast<vec_t*>(&toAcc[tileOff]);

        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            acc[k] = fromTile[k];

        for (int i = 0; i < psqRemoved.ssize(); ++i)
        {
            auto* row =
              reinterpret_cast<const vec_t*>(&psqWeights[psqRemoved[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], row[k]);
        }

        for (int i = 0; i < psqAdded.ssize(); ++i)
        {
            auto* row =
              reinterpret_cast<const vec_t*>(&psqWeights[psqAdded[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], row[k]);
        }

        for (int i = 0; i < thrRemoved.ssize(); ++i)
        {
            auto* column = reinterpret_cast<const vec_i8_t*>(
              &threatWeights[thrRemoved[i] * Dimensions + tileOff]);

    #ifdef USE_NEON
            for (IndexType k = 0; k < Tiling::NumRegs; k += 2)
            {
                acc[k]     = vsubw_s8(acc[k], vget_low_s8(column[k / 2]));
                acc[k + 1] = vsubw_high_s8(acc[k + 1], column[k / 2]);
            }
    #else
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], vec_convert_8_16(column[k]));
    #endif
        }

        for (int i = 0; i < thrAdded.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_i8_t*>(&threatWeights[thrAdded[i] * Dimensions + tileOff]);

    #ifdef USE_NEON
            for (IndexType k = 0; k < Tiling::NumRegs; k += 2)
            {
                acc[k]     = vaddw_s8(acc[k], vget_low_s8(column[k / 2]));
                acc[k + 1] = vaddw_high_s8(acc[k + 1], column[k / 2]);
            }
    #else
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], vec_convert_8_16(column[k]));
    #endif
        }

        for (IndexType k = 0; k < Tiling::NumRegs; k++)
            vec_store(&toTile[k], acc[k]);
    }

    for (IndexType j = 0; j < PSQTBuckets / Tiling::PsqtTileHeight; ++j)
    {
        const usize psqtTileOff  = j * Tiling::PsqtTileHeight;
        auto*       fromTilePsqt = reinterpret_cast<const psqt_vec_t*>(&fromPsqtAcc[psqtTileOff]);
        auto*       toTilePsqt   = reinterpret_cast<psqt_vec_t*>(&toPsqtAcc[psqtTileOff]);

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            psqt[k] = fromTilePsqt[k];

        for (int i = 0; i < psqRemoved.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[psqRemoved[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (int i = 0; i < psqAdded.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[psqAdded[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (int i = 0; i < thrRemoved.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.threatPsqtWeights[thrRemoved[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (int i = 0; i < thrAdded.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.threatPsqtWeights[thrAdded[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            vec_store_psqt(&toTilePsqt[k], psqt[k]);
    }

#else

    toAcc     = fromAcc;
    toPsqtAcc = fromPsqtAcc;

    for (const auto index : psqRemoved)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            toAcc[j] -= featureTransformer.weights[offset + j];
        for (usize k = 0; k < PSQTBuckets; ++k)
            toPsqtAcc[k] -= featureTransformer.psqtWeights[index * PSQTBuckets + k];
    }

    for (const auto index : psqAdded)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            toAcc[j] += featureTransformer.weights[offset + j];
        for (usize k = 0; k < PSQTBuckets; ++k)
            toPsqtAcc[k] += featureTransformer.psqtWeights[index * PSQTBuckets + k];
    }

    for (const auto index : thrRemoved)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            toAcc[j] -= featureTransformer.threatWeights[offset + j];
        for (usize k = 0; k < PSQTBuckets; ++k)
            toPsqtAcc[k] -= featureTransformer.threatPsqtWeights[index * PSQTBuckets + k];
    }

    for (const auto index : thrAdded)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            toAcc[j] += featureTransformer.threatWeights[offset + j];
        for (usize k = 0; k < PSQTBuckets; ++k)
            toPsqtAcc[k] += featureTransformer.threatPsqtWeights[index * PSQTBuckets + k];
    }

#endif
}

#ifndef TRIUMV_NO_PF_PSQ
// ✅ BAKATO 3/08/2026: **+1,3% NPS** — 194/300 posizioni vinte su due campioni
// indipendenti (89/150 seed 42 con mediana +0,59%, 105/150 seed 7 con +1,98%),
// z = 5,02, p ~ 5e-7, nodi identici in entrambi (121.575.142 / 122.855.971),
// PGO avx2 interlacciato. Il primo campione da solo era p = 0,02: troppo debole,
// ed e' stato il secondo a decidere. La mediana oscilla (+0,6 / +2,0), la
// FRAZIONE no — e' la frazione il numero che conta.
//
// 🔑 Perche' questo fronte era chiuso per sbaglio: la nota del 15/07 diceva
// "prefetch su HalfKA = -12,9%, non farlo". Quella misura e' anteriore a
// `nps_ab_interleaved.py` ed e' stata fatta con lo strumento non interlacciato,
// lo stesso che ha dichiarato -0,34% la permutazione FT (vale +1,62%).
//
// Prefetch delle righe di pesi HalfKA. Sono le piu' GROSSE del transformer
// (i16 x OutputDimensions = 2048 byte, il doppio di una riga threat che e' i8) e fino
// al 3/08 erano le uniche a non essere prefetchate affatto — mentre `apply_combined`
// le consuma per PRIME, quindi la loro latenza era interamente scoperta.
// Perche' funziona solo insieme al riordino: la lista PSQ si costruisce PRIMA di
// quelle threat (le liste sono disgiunte, l'ordine non cambia nulla di funzionale),
// cosi' fra il prefetch e l'uso c'e' tutta la costruzione delle liste threat a
// coprire la latenza. Emessa in cima, senza riordino, non coprirebbe niente.
// 🔴 UNA linea per riga, non di piu': prefetchare i 4 tile della riga e' stato provato
// sulle threat lo stesso giorno e misura -5,92% (vedi full_threats.cpp). Le linee
// successive sono sequenziali e le prende lo streamer L2; i prefetch in piu' rubano
// solo slot di load. Qui si replica ESATTAMENTE la forma che funziona sulle threat,
// applicata alle righe che oggi non ne hanno nessuna.
inline void prefetch_psq_rows(const FeatureTransformer&       featureTransformer,
                              const PSQFeatureSet::IndexList& a,
                              const PSQFeatureSet::IndexList& b) {
    constexpr usize RowBytes = usize(FeatureTransformer::OutputDimensions) * sizeof(WeightType);
    const char*     base     = reinterpret_cast<const char*>(&featureTransformer.weights[0]);

    for (int i = 0; i < a.ssize(); ++i)
        prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(base + usize(a[i]) * RowBytes);
    for (int i = 0; i < b.ssize(); ++i)
        prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(base + usize(b[i]) * RowBytes);
}
#endif

template<bool Forward>
void update_accumulator_incremental(Color                     perspective,
                                    const FeatureTransformer& featureTransformer,
                                    const Square              ksq,
                                    AccumulatorState&         target_state,
                                    const AccumulatorState&   computed) {

    assert(computed.computed[perspective]);
    assert(!target_state.computed[perspective]);

    // The size must be enough to contain the largest possible update.
    // That might depend on the feature set and generally relies on the
    // feature set's update cost calculation to be correct and never allow
    // updates with more added/removed features than MaxActiveDimensions.
    PSQFeatureSet::IndexList    psqRemoved, psqAdded;
    ThreatFeatureSet::IndexList thrRemoved, thrAdded;

    const auto& dirtyPiece   = Forward ? target_state.dirtyPiece : computed.dirtyPiece;
    const auto& dirtyThreats = Forward ? target_state.dirtyThreats : computed.dirtyThreats;
    const auto& dirtyPawns   = Forward ? target_state.dirtyPawns : computed.dirtyPawns;

    const auto* pfBase   = &featureTransformer.threatWeights[0];
    IndexType   pfStride = FeatureTransformer::OutputDimensions;

    if constexpr (Forward)
    {
#ifndef TRIUMV_NO_PF_PSQ
        PSQFeatureSet::append_changed_indices(perspective, ksq, dirtyPiece, psqRemoved, psqAdded);
        prefetch_psq_rows(featureTransformer, psqRemoved, psqAdded);
#endif
        ThreatFeatureSet::append_changed_indices(perspective, ksq, dirtyThreats, thrRemoved,
                                                 thrAdded, pfBase, pfStride);
        // TRANN1: gli indici PawnPair/PassedPawns (folded, gia' offsettati)
        // entrano nelle STESSE liste threat -> nessun pass SIMD aggiuntivo a valle.
        PawnFeatureSet::append_changed_indices(perspective, ksq, dirtyPawns, thrRemoved, thrAdded);
        PassedFeatureSet::append_changed_indices(perspective, ksq, dirtyPawns, thrRemoved, thrAdded);
#ifdef TRIUMV_NO_PF_PSQ
        PSQFeatureSet::append_changed_indices(perspective, ksq, dirtyPiece, psqRemoved, psqAdded);
#endif
    }
    else
    {
#ifndef TRIUMV_NO_PF_PSQ
        PSQFeatureSet::append_changed_indices(perspective, ksq, dirtyPiece, psqAdded, psqRemoved);
        prefetch_psq_rows(featureTransformer, psqRemoved, psqAdded);
#endif
        ThreatFeatureSet::append_changed_indices(perspective, ksq, dirtyThreats, thrAdded,
                                                 thrRemoved, pfBase, pfStride);
        PawnFeatureSet::append_changed_indices(perspective, ksq, dirtyPawns, thrAdded, thrRemoved);
        PassedFeatureSet::append_changed_indices(perspective, ksq, dirtyPawns, thrAdded, thrRemoved);
#ifdef TRIUMV_NO_PF_PSQ
        PSQFeatureSet::append_changed_indices(perspective, ksq, dirtyPiece, psqAdded, psqRemoved);
#endif
    }
    // NB (2026-07-15): estendere il prefetch a HalfKA/PawnPair/refresh aveva
    // MISURATO -12.9% NPS su Zen4, e per due settimane quel numero ha tenuto
    // chiuso il fronte.
    // 🔴 RITIRATO il 3/08/2026: a quella data `nps_ab_interleaved.py` NON ESISTEVA
    // ancora. La misura fu fatta con `nps_ab_binaries.py`, che esegue TUTTE le
    // posizioni di A e poi tutte quelle di B — lo stesso strumento che ha
    // dichiarato -0,34% la permutazione FT, che interlacciata vale +1,62%. Su un
    // laptop la deriva termica fra le due meta' del run finisce dritta nella
    // differenza A-B. Quel -12,9% non e' un risultato: e' l'artefatto noto.
    // Il fronte va riaperto e rimisurato interlacciato (vedi prefetch_psq_rows).

#ifdef TRIUMV_PROFILE
    // Quante colonne da HalfDimensions elementi vengono sommate/sottratte per UN
    // aggiornamento. E' il numero che distingue "il codice e' lento" da "le feature
    // sono tante": il costo dell'incrementale e' (colonne) x (L1) e nient'altro.
    prof_n_cols += psqAdded.size() + psqRemoved.size() + thrAdded.size() + thrRemoved.size();
    prof_n_upd++;
    // Istogramma degli accessi per riga di `threatWeights` (threat+PawnPair+Passed
    // folded). E' la tabella candidata alla permutazione per localita'.
    for (int i = 0; i < thrAdded.ssize(); ++i)
        if (thrAdded[i] < PROF_FEAT_N)
            prof_feat_hist[thrAdded[i]]++;
    for (int i = 0; i < thrRemoved.ssize(); ++i)
        if (thrRemoved[i] < PROF_FEAT_N)
            prof_feat_hist[thrRemoved[i]]++;
    for (int i = 0; i < psqAdded.ssize(); ++i)
        if (psqAdded[i] < PROF_PSQ_N)
            prof_psq_hist[psqAdded[i]]++;
    for (int i = 0; i < psqRemoved.ssize(); ++i)
        if (psqRemoved[i] < PROF_PSQ_N)
            prof_psq_hist[psqRemoved[i]]++;
    if ((unsigned long long) thrAdded.size() > prof_max_inc)
        prof_max_inc = thrAdded.size();
    if ((unsigned long long) thrRemoved.size() > prof_max_inc)
        prof_max_inc = thrRemoved.size();
#endif

    apply_combined(perspective, featureTransformer, computed, target_state, psqAdded, psqRemoved,
                   thrAdded, thrRemoved);

    target_state.computed[perspective] = true;
}

Bitboard get_changed_pieces(const std::array<Piece, SQUARE_NB>& oldPieces,
                            const std::array<Piece, SQUARE_NB>& newPieces) {
#if defined(USE_AVX2)
    static_assert(sizeof(Piece) == 1);
    Bitboard sameBB = 0;

    for (int i = 0; i < 64; i += 32)
    {
        const __m256i old_v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&oldPieces[i]));
        const __m256i new_v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&newPieces[i]));
        const __m256i cmpEqual  = _mm256_cmpeq_epi8(old_v, new_v);
        const u32     equalMask = _mm256_movemask_epi8(cmpEqual);
        sameBB |= static_cast<Bitboard>(equalMask) << i;
    }
    return ~sameBB;
#elif defined(USE_LASX)
    static_assert(sizeof(Piece) == 1);

    Bitboard changed = 0;

    for (int i = 0; i < 64; i += 32)
    {
        const __m256i old_v = __lasx_xvld(reinterpret_cast<const void*>(&oldPieces[i]), 0);
        const __m256i new_v = __lasx_xvld(reinterpret_cast<const void*>(&newPieces[i]), 0);
        const __m256i diff  = __lasx_xvxor_v(old_v, new_v);
        const __m256i mask  = __lasx_xvmsknz_b(diff);
        const auto    lo    = __lasx_xvpickve2gr_d(mask, 0);
        const auto    hi    = __lasx_xvpickve2gr_d(mask, 2);

        changed |= (static_cast<Bitboard>(lo) | (static_cast<Bitboard>(hi) << 16)) << i;
    }

    return changed;
#elif defined(USE_LSX)
    static_assert(sizeof(Piece) == 1);

    Bitboard changed = 0;

    for (int i = 0; i < 64; i += 16)
    {
        const __m128i old_v = __lsx_vld(reinterpret_cast<const void*>(&oldPieces[i]), 0);
        const __m128i new_v = __lsx_vld(reinterpret_cast<const void*>(&newPieces[i]), 0);
        const __m128i diff  = __lsx_vxor_v(old_v, new_v);
        const __m128i mask  = __lsx_vmsknz_b(diff);

        changed |= static_cast<Bitboard>(__lsx_vpickve2gr_d(mask, 0)) << i;
    }

    return changed;
#elif defined(USE_NEON)
    uint8x16x4_t old_v = vld4q_u8(reinterpret_cast<const u8*>(oldPieces.data()));
    uint8x16x4_t new_v = vld4q_u8(reinterpret_cast<const u8*>(newPieces.data()));
    auto         cmp   = [=](const int i) { return vceqq_u8(old_v.val[i], new_v.val[i]); };

    uint8x16_t cmp0_1 = vsriq_n_u8(cmp(1), cmp(0), 1);
    uint8x16_t cmp2_3 = vsriq_n_u8(cmp(3), cmp(2), 1);
    uint8x16_t merged = vsriq_n_u8(cmp2_3, cmp0_1, 2);
    merged            = vsriq_n_u8(merged, merged, 4);
    uint8x8_t sameBB  = vshrn_n_u16(vreinterpretq_u16_u8(merged), 4);

    return ~vget_lane_u64(vreinterpret_u64_u8(sameBB), 0);
#elif defined(USE_SSE2)
    Bitboard sameBB = 0;

    for (int i = 0; i < 64; i += 16)
    {
        const __m128i old_v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&oldPieces[i]));
        const __m128i new_v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&newPieces[i]));
        const __m128i same  = _mm_cmpeq_epi8(old_v, new_v);

        sameBB |= static_cast<Bitboard>(_mm_movemask_epi8(same)) << i;
    }

    return ~sameBB;
#else
    Bitboard changed = 0;

    for (Square sq = SQUARE_ZERO; sq < SQUARE_NB; ++sq)
        changed |= static_cast<Bitboard>(oldPieces[sq] != newPieces[sq]) << sq;

    return changed;
#endif
}

// ============================================================================
//  update_accumulator_hybrid — porting di Stockfish db98633b (26/07/2026)
//
//  IL PROBLEMA. `HalfKAv2_hm::requires_refresh` e' vero per OGNI mossa del
//  proprio re, quindi ogni mossa di re costa un refresh completo. Ma gli indici
//  di threat / PawnPair / PassedPawns dipendono da `OrientTBL[ksq]`, che ha due
//  soli valori e cambia SOLO se il re attraversa la colonna d/e. Per tutte le
//  altre mosse di re quelle feature restano valide, e le stiamo ricostruendo da
//  zero: sono il 59,6% delle colonne del refresh (10,4 threat su 17,4 totali).
//
//  L'IDEA.  acc_nuovo = acc_precedente − halfKA_precedente + halfKA_nuovo + Δ(threat/pp)
//  Nessuno dei due accumulatori HalfKA va memorizzato: si ricostruiscono
//  entrambi dalla finny table, quello nuovo come fa gia' il refresh, quello
//  precedente dalla entry del vecchio ksq applicando i diff verso la posizione
//  PRIMA della mossa — ricostruita qui dal dirtyPiece.
//
//  Da SF: +0,60%. Da noi il refresh pesa l'8,0% del wall e la cache dei blocchi
//  pedoni (3/08) copre gia' il 40,4% delle colonne: questo copre il resto.
// ============================================================================
void update_accumulator_hybrid(Color                     perspective,
                               const Position&           pos,
                               const FeatureTransformer& featureTransformer,
                               AccumulatorState&         target,
                               const AccumulatorState&   computed,
                               AccumulatorCaches&        cache) {
    constexpr IndexType Dimensions = FeatureTransformer::OutputDimensions;
    using Tiling [[maybe_unused]]  = SIMDTiling<Dimensions, Dimensions, PSQTBuckets>;

    const auto& dirtyPiece = target.dirtyPiece;
    const Square oldKsq    = dirtyPiece.from;
    const Square newKsq    = dirtyPiece.to;

    // Ricostruzione della posizione PRECEDENTE: si rimette il re su oldKsq e si
    // ripristina l'eventuale pezzo catturato su newKsq. L'arrocco NON passa di
    // qui (escluso dal gate): muoverebbe anche la torre.
    const auto& currentPieces  = pos.piece_array();
    auto        previousPieces = currentPieces;
    Bitboard    previousPieceBB = pos.pieces();

    if (dirtyPiece.remove_sq != SQ_NONE)
        previousPieces[newKsq] = dirtyPiece.remove_pc;
    else
    {
        previousPieces[newKsq] = NO_PIECE;
        previousPieceBB &= ~square_bb(newKsq);
    }
    previousPieces[oldKsq] = dirtyPiece.pc;
    previousPieceBB |= square_bb(oldKsq);

    const auto& oldEntry = cache[oldKsq][perspective];
    auto&       newEntry = cache[newKsq][perspective];

    // "Remove"/"Add" = cosa togliere/aggiungere ALLA ENTRY per ottenere
    // l'accumulatore HalfKA voluto.
    PSQFeatureSet::IndexList oldRemove, oldAdd, newRemove, newAdd;

    Bitboard oldChangedBB = get_changed_pieces(oldEntry.pieces, previousPieces);
    Bitboard oldRemovedBB = oldChangedBB & oldEntry.pieceBB;
    Bitboard oldAddedBB   = oldChangedBB & previousPieceBB;

    Bitboard newChangedBB = get_changed_pieces(newEntry.pieces, currentPieces);
    Bitboard newRemovedBB = newChangedBB & newEntry.pieceBB;
    Bitboard newAddedBB   = newChangedBB & pos.pieces();

    while (oldRemovedBB)
    {
        Square sq = pop_lsb(oldRemovedBB);
        oldRemove.push_back(PSQFeatureSet::make_index(perspective, sq, oldEntry.pieces[sq], oldKsq));
    }
    while (oldAddedBB)
    {
        Square sq = pop_lsb(oldAddedBB);
        oldAdd.push_back(PSQFeatureSet::make_index(perspective, sq, previousPieces[sq], oldKsq));
    }
    while (newRemovedBB)
    {
        Square sq = pop_lsb(newRemovedBB);
        newRemove.push_back(PSQFeatureSet::make_index(perspective, sq, newEntry.pieces[sq], newKsq));
    }
    while (newAddedBB)
    {
        Square sq = pop_lsb(newAddedBB);
        newAdd.push_back(PSQFeatureSet::make_index(perspective, sq, currentPieces[sq], newKsq));
    }

    // Delta dei tre blocchi non-HalfKA. Gli indici di PawnPair/PassedPawns sono
    // "folded" nelle stesse liste (gia' offsettati), come nel percorso incrementale.
    ThreatFeatureSet::IndexList thrRemoved, thrAdded;
    const auto*                 pfBase   = &featureTransformer.threatWeights[0];
    IndexType                   pfStride = Dimensions;
    ThreatFeatureSet::append_changed_indices(perspective, newKsq, target.dirtyThreats, thrRemoved,
                                             thrAdded, pfBase, pfStride);
    PawnFeatureSet::append_changed_indices(perspective, newKsq, target.dirtyPawns, thrRemoved,
                                           thrAdded);
    PassedFeatureSet::append_changed_indices(perspective, newKsq, target.dirtyPawns, thrRemoved,
                                             thrAdded);

    const auto& fromAcc     = computed.accumulation[perspective];
    auto&       toAcc       = target.accumulation[perspective];
    const auto& fromPsqtAcc = computed.psqtAccumulation[perspective];
    auto&       toPsqtAcc   = target.psqtAccumulation[perspective];

    target.computed[perspective] = true;

#ifdef VECTOR
    vec_t      acc[Tiling::NumRegs];
    psqt_vec_t psqt[Tiling::NumPsqtRegs];

    const auto* weights       = &featureTransformer.weights[0];
    const auto* threatWeights = &featureTransformer.threatWeights[0];

    for (IndexType j = 0; j < Dimensions / Tiling::TileHeight; ++j)
    {
        const usize tileOff      = j * Tiling::TileHeight;
        auto*       fromTile     = reinterpret_cast<const vec_t*>(&fromAcc[tileOff]);
        auto*       oldEntryTile = reinterpret_cast<const vec_t*>(&oldEntry.accumulation[tileOff]);
        auto*       newEntryTile = reinterpret_cast<vec_t*>(&newEntry.accumulation[tileOff]);
        auto*       toTile       = reinterpret_cast<vec_t*>(&toAcc[tileOff]);

        // 1) HalfKA NUOVO, esatto, a partire dalla finny entry del nuovo ksq.
        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            acc[k] = newEntryTile[k];
        for (int i = 0; i < newRemove.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[newRemove[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], column[k]);
        }
        for (int i = 0; i < newAdd.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[newAdd[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], column[k]);
        }

        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
        {
            // La finny entry del NUOVO ksq e' ora aggiornata (HalfKA puro).
            vec_store(&newEntryTile[k], acc[k]);
            // 2) Sommando l'accumulatore precedente entrano threat e pp gia' pronte,
            //    ma anche l'HalfKA del VECCHIO king bucket, che va tolto.
            acc[k] = vec_add_16(acc[k], fromTile[k]);
            acc[k] = vec_sub_16(acc[k], oldEntryTile[k]);
        }
        // 3) ...e si corregge con i diff della entry vecchia, a segno INVERTITO:
        //    stiamo togliendo l'HalfKA precedente, non aggiungendolo.
        for (int i = 0; i < oldRemove.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[oldRemove[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], column[k]);
        }
        for (int i = 0; i < oldAdd.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[oldAdd[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], column[k]);
        }

        // 4) Delta di threat/PawnPair/PassedPawns (pesi int8 -> convert).
        for (int i = 0; i < thrRemoved.ssize(); ++i)
        {
            auto* column = reinterpret_cast<const vec_i8_t*>(
              &threatWeights[thrRemoved[i] * Dimensions + tileOff]);
    #ifdef USE_NEON
            for (IndexType k = 0; k < Tiling::NumRegs; k += 2)
            {
                acc[k]     = vsubw_s8(acc[k], vget_low_s8(column[k / 2]));
                acc[k + 1] = vsubw_high_s8(acc[k + 1], column[k / 2]);
            }
    #else
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], vec_convert_8_16(column[k]));
    #endif
        }
        for (int i = 0; i < thrAdded.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_i8_t*>(&threatWeights[thrAdded[i] * Dimensions + tileOff]);
    #ifdef USE_NEON
            for (IndexType k = 0; k < Tiling::NumRegs; k += 2)
            {
                acc[k]     = vaddw_s8(acc[k], vget_low_s8(column[k / 2]));
                acc[k + 1] = vaddw_high_s8(acc[k + 1], column[k / 2]);
            }
    #else
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], vec_convert_8_16(column[k]));
    #endif
        }

        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            vec_store(&toTile[k], acc[k]);
    }

    // PSQT: stesso schema. ⚠️ Le feature attive alimentano ANCHE threatPsqtWeights —
    // dimenticarlo qui darebbe un PSQT stantio in silenzio (lezione del 3/08: bench
    // 262736 invece di 207259).
    for (IndexType j = 0; j < PSQTBuckets / Tiling::PsqtTileHeight; ++j)
    {
        const usize psqtTileOff = j * Tiling::PsqtTileHeight;
        auto*       fromTilePsqt =
          reinterpret_cast<const psqt_vec_t*>(&fromPsqtAcc[psqtTileOff]);
        auto* oldEntryTilePsqt =
          reinterpret_cast<const psqt_vec_t*>(&oldEntry.psqtAccumulation[psqtTileOff]);
        auto* newEntryTilePsqt =
          reinterpret_cast<psqt_vec_t*>(&newEntry.psqtAccumulation[psqtTileOff]);
        auto* toTilePsqt = reinterpret_cast<psqt_vec_t*>(&toPsqtAcc[psqtTileOff]);

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            psqt[k] = newEntryTilePsqt[k];
        for (int i = 0; i < newRemove.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[newRemove[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }
        for (int i = 0; i < newAdd.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[newAdd[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
        {
            vec_store_psqt(&newEntryTilePsqt[k], psqt[k]);
            psqt[k] = vec_add_psqt_32(psqt[k], fromTilePsqt[k]);
            psqt[k] = vec_sub_psqt_32(psqt[k], oldEntryTilePsqt[k]);
        }
        for (int i = 0; i < oldRemove.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[oldRemove[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }
        for (int i = 0; i < oldAdd.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[oldAdd[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (int i = 0; i < thrRemoved.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.threatPsqtWeights[thrRemoved[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }
        for (int i = 0; i < thrAdded.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.threatPsqtWeights[thrAdded[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            vec_store_psqt(&toTilePsqt[k], psqt[k]);
    }

    // Le entry della finny ora riflettono le rispettive posizioni HalfKA.
    newEntry.pieceBB = pos.pieces();
    newEntry.pieces  = currentPieces;
#else
    (void) fromAcc, (void) toAcc, (void) fromPsqtAcc, (void) toPsqtAcc;
    (void) oldEntry, (void) newEntry;
    assert(false && "update_accumulator_hybrid richiede il percorso VECTOR");
#endif
}

// HalfKA data comes from the Finny table entry, while the threats are built
// from the active threat features
void update_accumulator_refresh_cache(Color                     perspective,
                                      const FeatureTransformer& featureTransformer,
                                      const Position&           pos,
                                      AccumulatorState&         accumulator,
                                      AccumulatorCaches&        cache) {
    constexpr auto Dimensions = FeatureTransformer::OutputDimensions;

    using Tiling [[maybe_unused]] = SIMDTiling<Dimensions, Dimensions, PSQTBuckets>;

    const Square             ksq   = pos.square<KING>(perspective);
    auto&                    entry = cache[ksq][perspective];
    PSQFeatureSet::IndexList removed, added;

    const Bitboard changedBB = get_changed_pieces(entry.pieces, pos.piece_array());
    Bitboard       removedBB = changedBB & entry.pieceBB;
    Bitboard       addedBB   = changedBB & pos.pieces();

#if defined(USE_AVX512ICL)
    PSQFeatureSet::write_indices(entry.pieces, pos.piece_array(), removedBB, addedBB, perspective,
                                 ksq, removed, added);
#else
    while (removedBB)
    {
        Square sq = pop_lsb(removedBB);
        removed.push_back(PSQFeatureSet::make_index(perspective, sq, entry.pieces[sq], ksq));
    }
    while (addedBB)
    {
        Square sq = pop_lsb(addedBB);
        added.push_back(PSQFeatureSet::make_index(perspective, sq, pos.piece_on(sq), ksq));
    }
#endif

    entry.pieceBB = pos.pieces();
    entry.pieces  = pos.piece_array();

    // --- cache del refresh per i blocchi PEDONI (PawnPair + PassedPawns) ----------------
    // La finny table copre solo HalfKAv2_hm: gli altri blocchi si ricostruivano da zero a
    // OGNI refresh. Threats no (dipendono dalla posizione intera), ma PawnPair e PassedPawns
    // dipendono ESATTAMENTE da (pedoni bianchi, pedoni neri, orientation) — verificato nelle
    // rispettive make_index. E i refresh sono scatenati da mosse di RE, che i pedoni non li
    // toccano: fra due refresh consecutivi la chiave e' quasi sempre la stessa.
    // La chiave e' i due bitboard PER INTERO, non un hash: nessuna collisione possibile.
    // `orientation` (non ksq) perche' e' l'unico modo in cui il re entra negli indici, e ha
    // due soli valori per prospettiva (OrientTBL dipende dalla meta' di scacchiera del re).
    const Bitboard wpBB   = pos.pieces(WHITE, PAWN);
    const Bitboard bpBB   = pos.pieces(BLACK, PAWN);
    const int      orient = int(Features::FullThreats::OrientTBL[ksq]) ^ (56 * int(perspective));

    struct PawnRefreshEntry {
        Bitboard wp = ~Bitboard(0), bp = ~Bitboard(0);  // stato iniziale impossibile => miss
        int      orient = -1;
        alignas(64) std::int16_t acc[FeatureTransformer::OutputDimensions];
        alignas(64) std::int32_t psqt[PSQTBuckets];
    };
    // 8 entry = 16 KB per thread: resta in L1/L2. Piu' grande peggiorerebbe cio' che
    // stiamo ottimizzando, che e' traffico di memoria, non conto di istruzioni.
    static constexpr int  PawnCacheMask = 7;
    static thread_local PawnRefreshEntry pawnCache[PawnCacheMask + 1];

    PawnRefreshEntry& pe = pawnCache[(unsigned(wpBB ^ bpBB) ^ unsigned((wpBB ^ bpBB) >> 29)
                                      ^ unsigned(orient)) & PawnCacheMask];
#if defined(VECTOR) && !defined(TRIUMV_NO_PAWN_CACHE) && !defined(USE_AVX512)
    const bool pawnHit = (pe.wp == wpBB) & (pe.bp == bpBB) & (pe.orient == orient);
#else
    // Misurato 3/08/2026, interleaved, 60 posizioni depth 19, nodi identici:
    //   AVX2    +1,37%  (40/60 posizioni, test del segno p≈0,009)  -> ATTIVA
    //   AVX-512 -0,11%  (18/60 posizioni, stessa significativita' a rovescio) -> SPENTA
    // Su AVX-512 i tile sono piu' larghi e il `pv[NumRegs]` in piu' preme sui registri nel
    // percorso di miss, mentre il vantaggio della lettura contigua e' minore perche' il
    // percorso sparso era gia' piu' efficiente. E' l'AVX2 a darci il rating (CCRL compila
    // AVX2), ma non c'e' motivo di tenersi una regressione misurata dove non serve.
    // TRIUMV_NO_PAWN_CACHE = baseline per la misura A/B; il percorso scalare non ha cache.
    constexpr bool pawnHit = false;
#endif

    ThreatFeatureSet::IndexList active;
    ThreatFeatureSet::append_active_indices(perspective, pos, active);
    const int nThreat = active.ssize();
    if (!pawnHit)
    {
        // Miss: si enumera come prima. Il hit salta anche QUESTO, non solo le somme.
        PawnFeatureSet::append_active_indices(perspective, pos, active);    // TRANN1 folded
        PassedFeatureSet::append_active_indices(perspective, pos, active);  // v3 folded
        pe.wp = wpBB, pe.bp = bpBB, pe.orient = orient;
    }
#ifdef TRIUMV_PROFILE
    prof_cols_thr += nThreat;
    prof_cols_pawn += active.size() - nThreat;
    ++prof_n_refresh_calls;
#endif
#ifdef TRIUMV_PROFILE
    // Quanto si avvicina la lista al suo MaxActiveDimensions (288)? `push_back_if_lt` scrive
    // PRIMA di controllare e in Release l'assert sparisce: arrivarci = overflow silenzioso.
    if ((unsigned long long) active.size() > prof_max_active)
        prof_max_active = active.size();
#endif

    accumulator.computed[perspective] = true;

#ifdef VECTOR
    vec_t      acc[Tiling::NumRegs];
    psqt_vec_t psqt[Tiling::NumPsqtRegs];

    const auto* weights       = &featureTransformer.weights[0];
    const auto* threatWeights = &featureTransformer.threatWeights[0];

    for (IndexType j = 0; j < Dimensions / Tiling::TileHeight; ++j)
    {
        const usize tileOff = j * Tiling::TileHeight;
        auto* accTile   = reinterpret_cast<vec_t*>(&accumulator.accumulation[perspective][tileOff]);
        auto* entryTile = reinterpret_cast<vec_t*>(&entry.accumulation[tileOff]);

        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            acc[k] = entryTile[k];

        for (int i = 0; i < removed.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[removed[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], column[k]);
        }
        for (int i = 0; i < added.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[added[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], column[k]);
        }

        for (IndexType k = 0; k < Tiling::NumRegs; k++)
            vec_store(&entryTile[k], acc[k]);

        for (int i = 0; i < nThreat; ++i)
        {
            auto* column =
              reinterpret_cast<const vec_i8_t*>(&threatWeights[active[i] * Dimensions + tileOff]);

    #ifdef USE_NEON
            for (IndexType k = 0; k < Tiling::NumRegs; k += 2)
            {
                acc[k]     = vaddw_s8(acc[k], vget_low_s8(column[k / 2]));
                acc[k + 1] = vaddw_high_s8(acc[k + 1], column[k / 2]);
            }
    #else
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], vec_convert_8_16(column[k]));
    #endif
        }

        // Blocchi pedoni. Hit = UNA lettura contigua di 2 KB al posto di N colonne sparse
        // da 2 KB l'una: e' il traffico di memoria che si taglia, non le istruzioni.
        auto* pawnTile = reinterpret_cast<vec_t*>(&pe.acc[tileOff]);
        if (pawnHit)
        {
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], pawnTile[k]);
        }
        else
        {
            // Miss: si somma in un vettore SEPARATO (non su acc) perche' quel vettore va
            // memorizzato da solo — sommarlo su acc non lo renderebbe riusabile.
            vec_t pv[Tiling::NumRegs];
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                pv[k] = vec_zero();

            for (int i = nThreat; i < active.ssize(); ++i)
            {
                auto* column = reinterpret_cast<const vec_i8_t*>(
                  &threatWeights[active[i] * Dimensions + tileOff]);

    #ifdef USE_NEON
                for (IndexType k = 0; k < Tiling::NumRegs; k += 2)
                {
                    pv[k]     = vaddw_s8(pv[k], vget_low_s8(column[k / 2]));
                    pv[k + 1] = vaddw_high_s8(pv[k + 1], column[k / 2]);
                }
    #else
                for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                    pv[k] = vec_add_16(pv[k], vec_convert_8_16(column[k]));
    #endif
            }

            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            {
                vec_store(&pawnTile[k], pv[k]);
                acc[k] = vec_add_16(acc[k], pv[k]);
            }
        }

        for (IndexType k = 0; k < Tiling::NumRegs; k++)
            vec_store(&accTile[k], acc[k]);
    }

    for (IndexType j = 0; j < PSQTBuckets / Tiling::PsqtTileHeight; ++j)
    {
        const usize psqtTileOff = j * Tiling::PsqtTileHeight;
        auto*       accTilePsqt =
          reinterpret_cast<psqt_vec_t*>(&accumulator.psqtAccumulation[perspective][psqtTileOff]);
        auto* entryTilePsqt = reinterpret_cast<psqt_vec_t*>(&entry.psqtAccumulation[psqtTileOff]);

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            psqt[k] = entryTilePsqt[k];

        for (int i = 0; i < removed.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[removed[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }
        for (int i = 0; i < added.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[added[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            vec_store_psqt(&entryTilePsqt[k], psqt[k]);

        for (int i = 0; i < nThreat; ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.threatPsqtWeights[active[i] * PSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        // I blocchi pedoni contribuiscono ANCHE al PSQT (threatPsqtWeights): la cache deve
        // coprire tutti e due gli accumulatori, o al hit il PSQT resta indietro in silenzio.
        auto* pawnTilePsqt = reinterpret_cast<psqt_vec_t*>(&pe.psqt[psqtTileOff]);
        if (pawnHit)
        {
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], pawnTilePsqt[k]);
        }
        else
        {
            psqt_vec_t pq[Tiling::NumPsqtRegs];
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                pq[k] = vec_zero_psqt();

            for (int i = nThreat; i < active.ssize(); ++i)
            {
                auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
                  &featureTransformer.threatPsqtWeights[active[i] * PSQTBuckets + psqtTileOff]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    pq[k] = vec_add_psqt_32(pq[k], columnPsqt[k]);
            }

            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
            {
                vec_store_psqt(&pawnTilePsqt[k], pq[k]);
                psqt[k] = vec_add_psqt_32(psqt[k], pq[k]);
            }
        }

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            vec_store_psqt(&accTilePsqt[k], psqt[k]);
    }

#else

    for (const auto index : removed)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            entry.accumulation[j] -= featureTransformer.weights[offset + j];

        for (usize k = 0; k < PSQTBuckets; ++k)
            entry.psqtAccumulation[k] -= featureTransformer.psqtWeights[index * PSQTBuckets + k];
    }
    for (const auto index : added)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            entry.accumulation[j] += featureTransformer.weights[offset + j];

        for (usize k = 0; k < PSQTBuckets; ++k)
            entry.psqtAccumulation[k] += featureTransformer.psqtWeights[index * PSQTBuckets + k];
    }

    // The accumulator of the refresh entry has been updated.
    // Now copy its content to the actual accumulator we were refreshing.
    accumulator.accumulation[perspective]     = entry.accumulation;
    accumulator.psqtAccumulation[perspective] = entry.psqtAccumulation;

    for (const auto index : active)
    {
        const IndexType offset = Dimensions * index;

        for (IndexType j = 0; j < Dimensions; ++j)
            accumulator.accumulation[perspective][j] +=
              featureTransformer.threatWeights[offset + j];

        for (usize k = 0; k < PSQTBuckets; ++k)
            accumulator.psqtAccumulation[perspective][k] +=
              featureTransformer.threatPsqtWeights[index * PSQTBuckets + k];
    }

#endif
}

}

}

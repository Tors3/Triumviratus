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
#include <cstring>       // O1 (_wip pst_opt): lista di PassedState nella cache "pe" (memcmp, memcpy)
#include <type_traits>   // std::true_type / false_type: tile con o senza PSQT in apply_combined
// ADOTTATE 08/10/2026 sera (xperf 6 giri, docs/audit_8.0/X4_VELOCITA_PROFONDA.md): OLDWB + BIASBASE insieme -1,06%
// cicli/nodo in mediogioco (rumore A/A +-0,26), -0,96% nei finali (+-0,42). Accese di default; -DTRIUMV_NO_SPEED_X4
// torna al codice di prima (misure A/B). HYBALL resta spenta (peggiora OLDWB).
#if !defined(TRIUMV_NO_SPEED_X4)
    #ifndef TRIUMV_VG_OLDWB
        #define TRIUMV_VG_OLDWB
    #endif
    #ifndef TRIUMV_VG_BIASBASE
        #define TRIUMV_VG_BIASBASE
    #endif
#endif
// X2 (08/10/2026): patch di velocita' della rete ad albero identico, ognuna sotto la sua macro (rapporto
// docs/audit_8.0/X2_VELOCITA_RETE.md). TRIUMV_VG_OLDWB, TRIUMV_VG_BIASBASE, TRIUMV_VG_HYBALL; le rispettive
// TRIUMV_VERIFY_VG_* ricalcolano da zero le entry e l'accumulatore dopo ogni ibrido e ogni refresh.
#if defined(TRIUMV_VERIFY_VG_OLDWB) || defined(TRIUMV_VERIFY_VG_BIASBASE) || defined(TRIUMV_VERIFY_VG_HYBALL)
    #define TRIUMV_VG_VERIFY_ANY
#endif
#if defined(TRIUMV_VG_VERIFY_ANY)
    #include <cstdio>    // verifiche X2: messaggio e abort al primo disaccordo
    #include <cstdlib>
    #include <cstring>
#endif
#if defined(TRIUMV_VERIFY_PST_PE)
    #include <cstdio>    // O1 (_wip pst_opt): verifica della cache "pe" con PassedState, abort al primo disaccordo
    #include <cstdlib>
#endif

#include "../../profile.h"
#include "../../nstats.h"   // contatori del lavoro della rete, solo con -DTRIUMV_NSTATS (08/10/2026)
#include "../bitboard.h"
#include "../misc.h"
#include "../nn_board.h"
#include "../types.h"
#include "nnue_architecture.h"
#include "nnue_common.h"
#include "nnue_feature_transformer.h"  // IWYU pragma: keep
#include "simd.h"

namespace Triumviratus::Eval::NNUE {

// R4 (10/10/2026): passati di PassedPawns v1 prima e dopo la mossa dello stato st, gia' calcolati dal recupero
// (NnStack::v1PassW/B degli stati st.idx - 1 e st.idx, search/06_nndirty.inc). st ha una mossa (pawns.any): idx >= 1.
static inline void v1_pass_of(const NnState& st, Bitboard* pb, Bitboard* pa) {
    const NnStack& S = nn_stack_of(st);
    const int      i = st.idx;
    pb[WHITE] = S.v1PassW[i - 1], pb[BLACK] = S.v1PassB[i - 1];
    pa[WHITE] = S.v1PassW[i], pa[BLACK] = S.v1PassB[i];
}
#ifdef TRIUMV_VERIFY_V1PASS
    #define V1PASS_VERIFY(d, pb, pa) Features::PassedPawns::verify_pass(d, pb, pa)
#else
    #define V1PASS_VERIFY(d, pb, pa) ((void) 0)
#endif

using namespace SIMD;

namespace {

// 07/10/2026 (scacchiera unica v2): una transizione della catena e' una coppia (accumulatore, stato) per parte; lo
// stato (NnState, ../../nn_dirty.h) porta le dirty della mossa e i flag "calcolato".
template<bool Forward>
void update_accumulator_incremental(Color                     perspective,
                                    const FeatureTransformer& featureTransformer,
                                    const int                 ksq,
                                    Accumulator&              target,
                                    NnState&                  target_state,
                                    const Accumulator&        computed,
                                    const NnState&            computed_state);

// NPS 25/09/2026 — macro di RIAPERTURA per rimisurare su Intel (Skylake-SP, AVX-512 nativo a
// 512 bit) le tre vie spente su AVX-512 dopo le misure a tempo di agosto su Zen4 (AVX-512 a
// 256 bit). Tutte a nodi identici. Default: nessuna definita = comportamento di sempre.
#if defined(TRIUMV_PERSP_BOTH_AVX512) && defined(TRIUMV_NO_PERSP_BOTH)
    #undef TRIUMV_NO_PERSP_BOTH
#endif

#ifndef TRIUMV_NO_PERSP_BOTH
template<bool Forward>
void update_accumulator_incremental_both(const FeatureTransformer&    featureTransformer,
                                         const int                    ksqW,
                                         const int                    ksqB,
                                         Accumulator&                 target,
                                         NnState&                     target_state,
                                         const Accumulator&           computed,
                                         const NnState&               computed_state,
                                         AccumulatorStack::BothLists& lists);
#endif

void update_accumulator_refresh_cache(Color                     perspective,
                                      const FeatureTransformer& featureTransformer,
                                      const NnBoard&            pos,
                                      Accumulator&              accumulator,
                                      NnState&                  state,
                                      AccumulatorCaches&        cache);

void update_accumulator_hybrid(Color                     perspective,
                               const NnBoard&            pos,
                               const FeatureTransformer& featureTransformer,
                               Accumulator&              target,
                               NnState&                  target_state,
                               const Accumulator&        computed,
                               AccumulatorCaches&        cache);
}

const Accumulator& AccumulatorStack::latest() const noexcept { return accumulators[size() - 1]; }

Accumulator& AccumulatorStack::mut_latest() noexcept { return accumulators[size() - 1]; }

void AccumulatorStack::reset() noexcept {
    ds.size  = 1;
    ds.ready = 1;
    // La radice non ha una mossa: dirty vuote (nessuno le legge, ma restano definite).
    ds.st[0].dp        = {};
    ds.st[0].dp.to     = ds.st[0].dp.remove_sq = ds.st[0].dp.add_sq = NN_SQ_NONE;
    ds.st[0].pawns.any = 0;
    ds.st[0].threats.n = 0;
    ds.st[0].computed[WHITE] = ds.st[0].computed[BLACK] = 0;
    ds.st[0].idx       = 0;
    ds.st[0].graftRem  = ds.st[0].graftAdd = 0;  // _wip graftfix: diff dei blocchi da innesto in NnState
    ds.graftSrc[0]     = 255;  // voci dei blocchi da innesto della radice: da calcolare al primo bisogno
    ds.v1PassW[0]      = ~0ULL;  // R4: passati della radice da calcolare al primo recupero
}

void AccumulatorStack::evaluate(const NnBoard&            pos,
                                const FeatureTransformer& featureTransformer,
                                // Silence spurious warning on GCC 10
                                [[maybe_unused]] AccumulatorCaches& cache) noexcept {
    NSTAT(EVAL);

#ifndef TRIUMV_NO_PERSP_BOTH
    // Porting COMPLETO di SF 7b550409 (vedi update_accumulator_incremental_both).
    // Si entra solo se ENTRAMBE le prospettive hanno un'ancora calcolata: allora la
    // catena si percorre una volta sola e la dirty list si legge una volta per
    // transizione invece di due.
    {
        const auto lastW = find_last_usable_accumulator(WHITE);
        const auto lastB = find_last_usable_accumulator(BLACK);

        if (ds.st[lastW].computed[WHITE] && ds.st[lastB].computed[BLACK])
        {
#ifdef TRIUMV_PROFILE
            prof_n_inc += 2;
#endif
            PROF_GUARD(prof_acc_inc);
            const int   ksqW  = pos.king(WHITE);
            const int   ksqB  = pos.king(BLACK);
            const usize start = lastW < lastB ? lastW : lastB;

            for (usize next = start + 1; next < size(); next++)
            {
                // Le due ancore possono stare a profondita' DIVERSE: la passata
                // condivisa vale solo dove entrambe le prospettive devono ancora
                // essere aggiornate. Sulle transizioni disallineate si ricade sul
                // percorso a prospettiva singola, che e' esattamente il codice di
                // sempre.
                if (next > lastW && next > lastB)
                    update_accumulator_incremental_both<true>(featureTransformer, ksqW, ksqB, accumulators[next],
                                                              ds.st[next], accumulators[next - 1],
                                                              ds.st[next - 1], both_lists);
                else if (next > lastW)
                    update_accumulator_incremental<true>(WHITE, featureTransformer, ksqW, accumulators[next],
                                                         ds.st[next], accumulators[next - 1], ds.st[next - 1]);
                else if (next > lastB)
                    update_accumulator_incremental<true>(BLACK, featureTransformer, ksqB, accumulators[next],
                                                         ds.st[next], accumulators[next - 1], ds.st[next - 1]);
            }
            return;
        }
    }
#endif

    evaluate_side(WHITE, pos, featureTransformer, cache);
    evaluate_side(BLACK, pos, featureTransformer, cache);
}

void AccumulatorStack::evaluate_side(Color                     perspective,
                                     const NnBoard&            pos,
                                     const FeatureTransformer& featureTransformer,
                                     AccumulatorCaches&        cache) noexcept {

    const auto last_usable_accum = find_last_usable_accumulator(perspective);

    if (ds.st[last_usable_accum].computed[perspective])
    {
#ifdef TRIUMV_PROFILE
        prof_n_inc++;
#endif
        PROF_GUARD(prof_acc_inc);
        forward_update_incremental(perspective, pos, featureTransformer, last_usable_accum);
    }

    else
    {
// 🔴 6/08/2026 — SPENTA SU AVX-512, come la pawn cache poco sotto (:1214) e per la
// stessa ragione. Misurato con banco interlacciato, build PGO appaiate, node-identical
// a bench 205355, laptop scarico:
//     AVX2    : +3,4%  (300 pos, depth 19)
//     AVX-512 : -1,8%  (300 pos, depth 20, stabile su sei letture consecutive)
// Su AVX-512 i tile sono piu' larghi: ricostruire l'HalfKA precedente dalla finny table
// costa piu' di quanto si risparmi non ricostruendo le colonne di threat, e il percorso
// sparso che si evita era gia' piu' efficiente in partenza. E' il terzo caso con questa
// firma — pawn cache (+1,37% AVX2 / -0,11% AVX-512) e persp (+2,3% / -1,3%) — quindi
// non e' un'anomalia: e' la regola su questa ISA.
// ⚠️ Una prima lettura dava -8,34%, poi una seconda +1,3% e poi 0,00%: quest'ultima era
//    presa con una compilazione PGO in corso sulla stessa macchina. Il numero buono e' il
//    -1,8% a macchina scarica. Le misure NPS su laptop non valgono niente sotto carico.
// ✅ 25/09/2026 — RIACCESO anche su AVX-512. Il -1,8% del 6/08 era una misura a tempo su
// Zen4, che esegue l'AVX-512 a 256 bit. Rimisurato su Xeon Gold 6138 (Skylake-SP, AVX-512
// nativo) con i contatori hardware, nodi identici (bench 240500), 2 giri alternati da 12M
// nodi: -1,58% istruzioni/nodo, -1,8% branch miss, miss L3/nodo invariati (20,9).
// -DTRIUMV_NO_HYBRID_AVX512 riporta lo spegnimento su AVX-512 (es. per una build AMD).
#if !defined(TRIUMV_NO_HYBRID_ACC) && (!defined(USE_AVX512) || !defined(TRIUMV_NO_HYBRID_AVX512))
        // Percorso HYBRID (SF db98633b): una mossa di re che NON attraversa la
        // colonna d/e lascia validi tutti gli indici di threat/PawnPair/PassedPawns,
        // perche' quelli dipendono da OrientTBL[ksq] che ha due soli valori. In quel
        // caso si riusa l'accumulatore precedente invece di ricostruire il 59,6%
        // delle colonne da zero.
        //   - `add_sq == SQ_NONE` esclude l'arrocco (muoverebbe anche la torre)
        //   - sotto i 15 pezzi le feature attive sono poche e ricostruirle costa
        //     meno che ricavare l'HalfKA precedente dalla cache
        // 04/10/2026 — anche il CAMBIO DI FASCIA (TRIUMV_PSQ_PHASES > 1) passa di qui. Una cattura che attraversa
        // una soglia cambia tutte le righe HalfKA ma nessun indice di threat/pedoni: prima si ricostruiva tutto da
        // zero, e sul bench delle 30 posizioni SF era un refresh su tre (111.771 su 324.433, gli altri sono mosse di
        // re). Qui arrivano solo mosse che richiedono il refresh (vedi find_last_usable_accumulator): se non sono
        // del nostro re, sono un cambio di fascia, e il re resta fermo.
        // Misura (xperf, 30 posizioni SF x 400k nodi, 2 giri, nodi identici, bench 141196 invariato): -1,05%
        // istruzioni, -0,55% cicli per nodo. Poco: i refresh pieni scendono da 294k a 238k, ma il costo vero del
        // cambio di fascia sono le righe HalfKA della fascia nuova (entry della finny vecchia di molte mosse, 4 x 46
        // MB di pesi), che l'ibrido deve applicare lo stesso. Le threat erano la parte piccola.
        //   - `add_sq == SQ_NONE` esclude anche le promozioni, che restano sul refresh
        // 05/10/2026 sera — SOGLIA DEI PEZZI TOLTA (15 -> 0). Il 15 veniva dalla rete di SF e non era mai stato misurato
        // sulla nostra. Conteggio per causa (build di profilo, 30 posizioni a prof. 14 e 64 finali): il 21% dei refresh
        // pieni era sotto i 15 pezzi, quasi tutti mosse del nostro re; con l'ibrido costano l'8-10% in meno. xperf 6
        // giri a macchina quieta, build PGO, nodi identici: cicli per nodo -0,55% sulle 30 posizioni, -0,54% su 30 finali.
        // Provato e SCARTATO nello stesso giro: col padre non calcolato (55-58% dei refresh pieni) portarlo al passo con
        // update incrementali e poi l'ibrido costa ~2,5% IN PIU' sul tempo dei refresh: l'ibrido costa quasi quanto un
        // refresh pieno (due ricostruzioni HalfKA da entry della finny vecchie, 4 fasce), l'update in piu' non si ripaga.
        constexpr int MIN_PC_COUNT_HYBRID = 0;
        const usize   n                   = size();
        const auto&   dp                  = ds.st[n - 1].dp;
        const bool    ownKing             = dp.pc == NN_KING + NN_BLACK * int(perspective);
    #ifdef TRIUMV_VG_HYBALL
        // X2 (08/10/2026, VG_HYBALL): anche l'ARROCCO che non porta il re oltre la colonna d/e (O-O, e in 960 ogni
        // arrocco che resta nella stessa meta') e la PROMOZIONE con cattura che cambia fascia passano dall'ibrido. In
        // entrambi i casi gli indici di minaccia/pedoni restano validi (orientamento invariato) e le dirty delle
        // minacce coprono anche la torre e il pezzo promosso; la posizione precedente si ricostruisce togliendo prima
        // i pezzi arrivati e poi rimettendo quelli partiti, che regge anche le case condivise dell'arrocco 960. Escluso
        // solo l'arrocco 960 col re fermo: entry vecchia e nuova sarebbero la stessa.
        if (n >= 2 && ds.st[n - 2].computed[perspective] && pos.count() >= MIN_PC_COUNT_HYBRID
            && (!ownKing || ((int(dp.from) & 0b100) == (int(dp.to) & 0b100) && dp.from != dp.to)))
        {
    #else
        if (n >= 2 && dp.to != NN_SQ_NONE
            && ds.st[n - 2].computed[perspective]
            && pos.count() >= MIN_PC_COUNT_HYBRID
            && (!ownKing || (int(dp.from) & 0b100) == (int(dp.to) & 0b100)) && dp.add_sq == NN_SQ_NONE)
        {
    #endif
    #ifdef TRIUMV_PROFILE
            prof_refresh_same_orient++;
    #endif
            PROF_GUARD(prof_acc_refresh);
            update_accumulator_hybrid(perspective, pos, featureTransformer, mut_latest(), ds.st[n - 1],
                                      accumulators[n - 2], cache);
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
        update_accumulator_refresh_cache(perspective, featureTransformer, pos, mut_latest(), ds.st[size() - 1],
                                         cache);
        backward_update_incremental(perspective, pos, featureTransformer, last_usable_accum);
    }
}

// Find the earliest usable accumulator, this can either be a computed accumulator or the accumulator
// state just before a change that requires full refresh.
usize AccumulatorStack::find_last_usable_accumulator(Color perspective) const noexcept {

    for (usize curr_idx = size() - 1; curr_idx > 0; curr_idx--)
    {
        if (ds.st[curr_idx].computed[perspective])
            return curr_idx;

        // Threat feature set refreshes require a king move across the center, i.e.,
        // a subset of halfka refreshes
        if (PSQFeatureSet::requires_refresh(ds.st[curr_idx].dp, perspective))
            return curr_idx;
    }

    return 0;
}

void AccumulatorStack::forward_update_incremental(Color                     perspective,
                                                  const NnBoard&            pos,
                                                  const FeatureTransformer& featureTransformer,
                                                  const usize               begin) noexcept {

    assert(begin < accumulators.size());
    assert(ds.st[begin].computed[perspective]);

    const int ksq = pos.king(perspective);

    for (usize next = begin + 1; next < size(); next++)
        update_accumulator_incremental<true>(perspective, featureTransformer, ksq, accumulators[next], ds.st[next],
                                             accumulators[next - 1], ds.st[next - 1]);

    assert(ds.st[size() - 1].computed[perspective]);
}

void AccumulatorStack::backward_update_incremental(Color                     perspective,
                                                   const NnBoard&            pos,
                                                   const FeatureTransformer& featureTransformer,
                                                   const usize               end) noexcept {

    assert(end < accumulators.size());
    assert(end < size());
    assert(ds.st[size() - 1].computed[perspective]);

    const int ksq = pos.king(perspective);

    for (i64 next = i64(size()) - 2; next >= i64(end); next--)
        update_accumulator_incremental<false>(perspective, featureTransformer, ksq, accumulators[next],
                                              ds.st[next], accumulators[next + 1], ds.st[next + 1]);

    assert(ds.st[end].computed[perspective]);
}

namespace {

void apply_combined(Color                              perspective,
                    const FeatureTransformer&          featureTransformer,
                    const Accumulator&                 from,
                    Accumulator&                       to,
                    const PSQFeatureSet::IndexList&    psqAdded,
                    const PSQFeatureSet::IndexList&    psqRemoved,
                    const ThreatFeatureSet::IndexList& thrAdded,
                    const ThreatFeatureSet::IndexList& thrRemoved) {
    NSTAT(COMBINED);
    NSTATV(PSQ_ROWS, psqAdded.size() + psqRemoved.size());
    NSTATV(THR_ROWS, thrAdded.size() + thrRemoved.size());
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

    // 04/10/2026 notte — i quattro cicli PSQT (una riga da 32 byte per feature) non girano piu' da soli DOPO i tile
    // dell'accumulatore: li fa il PRIMO tile insieme alle sue righe (WithPsqt), gli altri tile no. Ogni ciclo "per
    // feature" ha un numero di giri che cambia a ogni chiamata e la sua uscita e' un salto mal predetto (xperf: ~4
    // per nodo in questa funzione): cosi' i cicli sono 8 invece di 12. Un solo tile PSQT (static_assert), stesse
    // somme nello stesso ordine: risultato identico.
    static_assert(PSQTBuckets / Tiling::PsqtTileHeight == 1, "apply_combined: serve un solo tile PSQT");
    const auto* psqtWeights    = &featureTransformer.psqtWeights[0];
    const auto* thrPsqtWeights = &featureTransformer.threatPsqtWeights[0];
    auto*       fromTilePsqt   = reinterpret_cast<const psqt_vec_t*>(&fromPsqtAcc[0]);
    auto*       toTilePsqt     = reinterpret_cast<psqt_vec_t*>(&toPsqtAcc[0]);

    const auto tile = [&](const IndexType j, auto withPsqtTag) {
        constexpr bool WithPsqt = decltype(withPsqtTag)::value;
        const usize    tileOff  = j * Tiling::TileHeight;
        auto*          fromTile = reinterpret_cast<const vec_t*>(&fromAcc[tileOff]);
        auto*          toTile   = reinterpret_cast<vec_t*>(&toAcc[tileOff]);

        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            acc[k] = fromTile[k];
        if constexpr (WithPsqt)
            for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = fromTilePsqt[k];

        for (int i = 0; i < psqRemoved.ssize(); ++i)
        {
            auto* row =
              reinterpret_cast<const vec_t*>(&psqWeights[psqRemoved[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], row[k]);
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&psqtWeights[psqRemoved[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
            }
        }

        for (int i = 0; i < psqAdded.ssize(); ++i)
        {
            auto* row =
              reinterpret_cast<const vec_t*>(&psqWeights[psqAdded[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], row[k]);
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&psqtWeights[psqAdded[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
            }
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
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&thrPsqtWeights[thrRemoved[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
            }
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
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&thrPsqtWeights[thrAdded[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
            }
        }

        for (IndexType k = 0; k < Tiling::NumRegs; k++)
            vec_store(&toTile[k], acc[k]);
        if constexpr (WithPsqt)
            for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
                vec_store_psqt(&toTilePsqt[k], psqt[k]);
    };

    tile(0, std::true_type{});
    for (IndexType j = 1; j < Dimensions / Tiling::TileHeight; ++j)
        tile(j, std::false_type{});

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

    // ⛔ 05/10/2026 — PROVATO E TOLTO (H): due prefetch fissi per lista (prima e ultima voce: le liste incrementali
    // hanno 1 o 2 voci) al posto dei due cicli a conteggio variabile. xperf 6 giri, nodi identici: istruzioni +0,27%,
    // cicli +0,07%, salti mal predetti -0,19%. Qui le uscite dei cicli erano gia' ben predette.
    for (int i = 0; i < a.ssize(); ++i)
        prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(base + usize(a[i]) * RowBytes);
    for (int i = 0; i < b.ssize(); ++i)
        prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(base + usize(b[i]) * RowBytes);
}

#ifdef TRIUMV_GRAFT_PF
// _wip graft_passedrel2 (Q6, DA MISURARE, spento di default): prefetch della prima linea delle righe dei blocchi da
// innesto appena aggiunte a una lista (dalla voce `from`), come fa FullThreats per le sue righe. Le 768 righe di
// PassedRel (768 KB) stanno fuori dalla permutazione per localita' e non le tocca nessun altro blocco: con la rete da
// 170 MB che passa per la L2, e' plausibile che arrivino dalla L3. La regola del progetto (prefetch solo su tabelle che
// non stanno in cache, qui sopra e in apply_combined) dice di aspettarsi zero o peggio: misurare sotto carico.
inline void prefetch_graft_rows(const FeatureTransformer& featureTransformer, const ThreatFeatureSet::IndexList& l,
                                int from) {
    constexpr usize RowBytes = usize(FeatureTransformer::OutputDimensions) * sizeof(ThreatWeightType);
    const char*     base     = reinterpret_cast<const char*>(&featureTransformer.threatWeights[0]);
    for (int i = from; i < l.ssize(); ++i)
        prefetch<PrefetchRw::READ, PrefetchLoc::LOW>(base + usize(l[i]) * RowBytes);
}
#endif


template<bool Forward>
void update_accumulator_incremental(Color                     perspective,
                                    const FeatureTransformer& featureTransformer,
                                    const int                 ksq,
                                    Accumulator&              target,
                                    NnState&                  target_state,
                                    const Accumulator&        computed,
                                    const NnState&            computed_state) {
    NSTAT(INC);

    assert(computed_state.computed[perspective]);
    assert(!target_state.computed[perspective]);

    // The size must be enough to contain the largest possible update.
    // That might depend on the feature set and generally relies on the
    // feature set's update cost calculation to be correct and never allow
    // updates with more added/removed features than MaxActiveDimensions.
    PSQFeatureSet::IndexList    psqRemoved, psqAdded;
    ThreatFeatureSet::IndexList thrRemoved, thrAdded;

    const auto& dirtyPiece   = Forward ? target_state.dp : computed_state.dp;
    const auto& dirtyThreats = Forward ? target_state.threats : computed_state.threats;
    const auto& dirtyPawns   = Forward ? target_state.pawns : computed_state.pawns;
    const NnState& graftState = Forward ? target_state : computed_state;  // blocchi da innesto: nn_graft_of

    const auto* pfBase   = &featureTransformer.threatWeights[0];
    IndexType   pfStride = FeatureTransformer::OutputDimensions;

    // Fascia (HalfKA a esperti): costante lungo la catena incrementale, perche' un cambio di fascia forza il refresh.
    const int psqPhase = PSQFeatureSet::phase_of(dirtyPiece);

    if constexpr (Forward)
    {
        PSQFeatureSet::append_changed_indices(perspective, ksq, dirtyPiece, psqRemoved, psqAdded,
                                              psqPhase);
        prefetch_psq_rows(featureTransformer, psqRemoved, psqAdded);
        { PROF_GUARD(prof_idx_thr);
        ThreatFeatureSet::append_changed_indices(perspective, ksq, dirtyThreats, thrRemoved,
                                                 thrAdded, pfBase, pfStride); }
#ifdef TRIUMV_PROFILE
        prof_cols_thr_inc += thrRemoved.size() + thrAdded.size();
        const usize profThrBeforePawn = thrRemoved.size() + thrAdded.size();
#endif
        // TRANN1: gli indici PawnPair/PassedPawns (folded, gia' offsettati)
        // entrano nelle STESSE liste threat -> nessun pass SIMD aggiuntivo a valle.
// ⛔ TRIUMV_PF_SMALL — MISURATO E RIGETTATO il 06/09/2026 (prima non aveva misura). Il codice
// e' stato tolto nella pulizia del 25/09/2026 (copia in _backup/Triumviratus_8.0_src_2026-09-25_pre_cleanup).
// Prefetch delle righe PawnPair+PassedPawns, cioe' l'equivalente dei parametri
// prefetchBase/prefetchStride che SF passa a PP_3Wide::append_changed_indices.
//   Xeon Gold 6138 (Skylake-SP), build PGO clang node-identical (bench 252074 su
//   entrambe), nps_ab_interleaved 150 pos depth 18 hash 256, macchina scarica:
//     B/A = -0,63%   (mediana dei rapporti per-posizione)
//     B piu' veloce in 59/150 posizioni (39,3%), z = 2,23, p = 0,025
//     ⚠️ divergenza d'ordine 0,65%, grande quanto il segnale -> lettura DEBOLE
// 🔑 Il segno lo prevedeva gia' la regola a :350: le righe PawnPair+PassedPawns
//    sono ~4,8 MB e in questa misura stanno TUTTE nei 55 MB di L3 del socket, perche'
//    gira un motore solo. Tabella che sta in cache => il prefetch e' solo un'istruzione
//    in piu' nel percorso piu' caldo. La regola ha predetto il risultato.
// ⚠️ RESTA APERTO il regime di partita: con 72 partite concorrenti l'L3 effettiva
//    per processo scende a ~2,75 MB, le righe NON ci stanno piu' e il segno potrebbe
//    invertirsi. nps_ab_interleaved non puo' misurarlo (vuole la macchina scarica).
//    Stessa forma della lezione TTTwoLevel: +4,55 a hash 64, zero a hash 256.
        { PROF_GUARD(prof_idx_pawn);
        PawnFeatureSet::append_changed_indices(perspective, ksq, dirtyPawns, thrRemoved, thrAdded);
        // R1 (_wip graft_passedrel3, 10/10/2026): con la sola PassedRel nelle liste le righe v1 escono dalla diff del
        // blocco (PawnGraftSet::append_changed_indices, piu' sotto). Il test su `any` evita anche la chiamata fuori linea
        // che per una mossa senza pedoni non faceva nulla (v1 esce subito se !any).
        if (dirtyPawns.any && !PawnGraftSet::v1_skip())
        {
            Bitboard pb[COLOR_NB], pa[COLOR_NB];  // R4: passati dal recupero, niente passers() qui
            v1_pass_of(graftState, pb, pa);
            V1PASS_VERIFY(dirtyPawns, pb, pa);
            PassedFeatureSet::append_changed_indices_pass(perspective, ksq, pb, pa, thrRemoved, thrAdded);
        }
        NSTATV(PREL_V1_SKIP, dirtyPawns.any && PawnGraftSet::v1_fused());
        // _wip graftfix (P2): conteggi in NnState, gia' in L1; zero con una rete senza blocchi o con diff vuota.
#ifdef TRIUMV_VERIFY_GRAFT
        const int vr0 = thrRemoved.ssize(), va0 = thrAdded.ssize();  // R1: righe scritte dalla diff dei blocchi
#endif
        if (graftState.graftRem | graftState.graftAdd)
        {
            NSTATV(GRAFT_ROWS, graftState.graftRem + graftState.graftAdd);
#ifdef TRIUMV_GRAFT_PF
            const int r0 = thrRemoved.ssize(), a0 = thrAdded.ssize();
#endif
            PawnGraftSet::append_changed_indices(perspective, ksq, graftState, thrRemoved, thrAdded);
#ifdef TRIUMV_GRAFT_PF
            prefetch_graft_rows(featureTransformer, thrRemoved, r0);
            prefetch_graft_rows(featureTransformer, thrAdded, a0);
#endif
        }
#ifdef TRIUMV_VERIFY_GRAFT
        if (dirtyPawns.any && PawnGraftSet::v1_fused())
            PawnGraftSet::verify_v1_fused(perspective, ksq, dirtyPawns, thrRemoved.begin() + vr0, thrRemoved.ssize() - vr0,
                                          thrAdded.begin() + va0, thrAdded.ssize() - va0);
#endif
        }
#ifdef TRIUMV_PROFILE
        prof_cols_pawn_inc += thrRemoved.size() + thrAdded.size() - profThrBeforePawn;
        prof_cols_psq_inc  += psqRemoved.size() + psqAdded.size();
#endif
    }
    else
    {
        PSQFeatureSet::append_changed_indices(perspective, ksq, dirtyPiece, psqAdded, psqRemoved,
                                              psqPhase);
        prefetch_psq_rows(featureTransformer, psqRemoved, psqAdded);
        ThreatFeatureSet::append_changed_indices(perspective, ksq, dirtyThreats, thrAdded,
                                                 thrRemoved, pfBase, pfStride);
        PawnFeatureSet::append_changed_indices(perspective, ksq, dirtyPawns, thrAdded, thrRemoved);
        if (dirtyPawns.any && !PawnGraftSet::v1_skip())  // R1 (_wip graft_passedrel3), come nel ramo in avanti
        {
            Bitboard pb[COLOR_NB], pa[COLOR_NB];  // R4
            v1_pass_of(graftState, pb, pa);
            V1PASS_VERIFY(dirtyPawns, pb, pa);
            PassedFeatureSet::append_changed_indices_pass(perspective, ksq, pb, pa, thrAdded, thrRemoved);
        }
#ifdef TRIUMV_VERIFY_GRAFT
        const int vr0 = thrRemoved.ssize(), va0 = thrAdded.ssize();
#endif
        if (graftState.graftRem | graftState.graftAdd)
        {
            NSTATV(GRAFT_ROWS, graftState.graftRem + graftState.graftAdd);
            PawnGraftSet::append_changed_indices(perspective, ksq, graftState, thrAdded, thrRemoved);
        }
#ifdef TRIUMV_VERIFY_GRAFT
        // liste scambiate: cio' che v1 chiamerebbe "removed" e' thrAdded
        if (dirtyPawns.any && PawnGraftSet::v1_fused())
            PawnGraftSet::verify_v1_fused(perspective, ksq, dirtyPawns, thrAdded.begin() + va0, thrAdded.ssize() - va0,
                                          thrRemoved.begin() + vr0, thrRemoved.ssize() - vr0);
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
    // 🔴 TRIUMV_PROFILE_LIGHT: SOLO contatori e rdtsc. Gli istogrammi per riga (700 KB) e la
    // matrice di co-occorrenza (134 MB) toccati QUI, dentro l'update, inquinano le cache e
    // gonfiano i cicli per update: il 09/09 misuravano 3.271 cicli contro i 571 di SF, e
    // meta' di quel divario era l'instrumentazione stessa. Con la luce si misura il motore.
#ifndef TRIUMV_PROFILE_LIGHT
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
    // Co-occorrenza: quali righe calde compaiono nello STESSO update. Si guardano
    // insieme added e removed, perche' l'update le legge tutte nello stesso ciclo
    // e quindi tocca le stesse pagine.
    {
        if (!prof_cooc)
            prof_cooc = new unsigned short[usize(PROF_COOC_N) * PROF_COOC_N]();
        IndexType hot[64];
        int       nh = 0;
        for (int i = 0; i < thrAdded.ssize() && nh < 64; ++i)
            if (thrAdded[i] < PROF_COOC_N)
                hot[nh++] = thrAdded[i];
        for (int i = 0; i < thrRemoved.ssize() && nh < 64; ++i)
            if (thrRemoved[i] < PROF_COOC_N)
                hot[nh++] = thrRemoved[i];
        for (int a = 0; a < nh; ++a)
            for (int b = a + 1; b < nh; ++b)
            {
                // saturazione a 16 bit: al clustering serve l'ordine, non il valore
                auto& x = prof_cooc[usize(hot[a]) * PROF_COOC_N + hot[b]];
                auto& y = prof_cooc[usize(hot[b]) * PROF_COOC_N + hot[a]];
                if (x < 65535) x++;
                if (y < 65535) y++;
            }
    }
#endif
    if ((unsigned long long) thrAdded.size() > prof_max_inc)
        prof_max_inc = thrAdded.size();
    if ((unsigned long long) thrRemoved.size() > prof_max_inc)
        prof_max_inc = thrRemoved.size();
#endif

    // 05/10/2026 (prova G): da quando le righe PSQT si sommano nel PRIMO tile (B1), il prefetch delle righe PSQT
    // delle minacce qui non ha piu' anticipo e costava ~1,3 salti mal predetti per nodo (xperf): tolto (08/10/2026).
    apply_combined(perspective, featureTransformer, computed, target, psqAdded, psqRemoved, thrAdded,
                   thrRemoved);

    target_state.computed[perspective] = 1;
}

#ifndef TRIUMV_NO_PERSP_BOTH
// Porting COMPLETO di SF 7b550409, nella nostra variante: una sola passata sulla
// dirty list dei threat produce le liste di ENTRAMBE le prospettive (i bitfield si
// decodificano una volta sola), ma le APPLICAZIONI restano sequenziali — tutto il
// bianco, poi tutto il nero. La forma di Stockfish alterna le prospettive a ogni
// transizione e da noi era costata -0,70% il 3/08: alternare tiene vivi due
// accumulatori da 2 KB mentre si streammano ~21 KB di colonne.
//
// 🔴 EQUIVALENZA FUNZIONALE: le liste prodotte qui sono le stesse, nello stesso
// ordine, di due chiamate separate a update_accumulator_incremental. L'unica cosa
// che cambia e' QUANTE volte si legge `dirty`. Il bench DEVE restare 207259.
template<bool Forward>
void update_accumulator_incremental_both(const FeatureTransformer&    featureTransformer,
                                         const int                    ksqW,
                                         const int                    ksqB,
                                         Accumulator&                 target,
                                         NnState&                     target_state,
                                         const Accumulator&           computed,
                                         const NnState&               computed_state,
                                         AccumulatorStack::BothLists& lists) {
    NSTAT(INC_BOTH);

    assert(computed_state.computed[WHITE] && computed_state.computed[BLACK]);
    assert(!target_state.computed[WHITE] && !target_state.computed[BLACK]);

    // Le liste vengono da AccumulatorStack (05/10/2026: fuori dallo stack, vedi BothLists) e partono vuote.
    auto& psqRemW = lists.psqRemW; auto& psqAddW = lists.psqAddW;
    auto& psqRemB = lists.psqRemB; auto& psqAddB = lists.psqAddB;
    auto& thrRemW = lists.thrRemW; auto& thrAddW = lists.thrAddW;
    auto& thrRemB = lists.thrRemB; auto& thrAddB = lists.thrAddB;
    psqRemW.clear(); psqAddW.clear(); psqRemB.clear(); psqAddB.clear();
    thrRemW.clear(); thrAddW.clear(); thrRemB.clear(); thrAddB.clear();

    const auto& dirtyPiece   = Forward ? target_state.dp : computed_state.dp;
    const auto& dirtyThreats = Forward ? target_state.threats : computed_state.threats;
    const auto& dirtyPawns   = Forward ? target_state.pawns : computed_state.pawns;
    const NnState& graftState = Forward ? target_state : computed_state;  // blocchi da innesto: nn_graft_of

    const auto* pfBase   = &featureTransformer.threatWeights[0];
    IndexType   pfStride = FeatureTransformer::OutputDimensions;

    // Nel ramo all'indietro added/removed si scambiano, esattamente come nel
    // percorso a prospettiva singola.
    auto& remW = Forward ? thrRemW : thrAddW;
    auto& addW = Forward ? thrAddW : thrRemW;
    auto& remB = Forward ? thrRemB : thrAddB;
    auto& addB = Forward ? thrAddB : thrRemB;

    const int psqPhase = PSQFeatureSet::phase_of(dirtyPiece);  // costante lungo la catena (vedi sopra)
    if constexpr (Forward)
    {
        PSQFeatureSet::append_changed_indices(WHITE, ksqW, dirtyPiece, psqRemW, psqAddW, psqPhase);
        PSQFeatureSet::append_changed_indices(BLACK, ksqB, dirtyPiece, psqRemB, psqAddB, psqPhase);
    }
    else
    {
        PSQFeatureSet::append_changed_indices(WHITE, ksqW, dirtyPiece, psqAddW, psqRemW, psqPhase);
        PSQFeatureSet::append_changed_indices(BLACK, ksqB, dirtyPiece, psqAddB, psqRemB, psqPhase);
    }
    prefetch_psq_rows(featureTransformer, psqRemW, psqAddW);
    prefetch_psq_rows(featureTransformer, psqRemB, psqAddB);

    // LA PASSATA CONDIVISA: un giro solo su diff.list per tutte e quattro le liste.
    ThreatFeatureSet::append_changed_indices_both(ksqW, ksqB, dirtyThreats, remW, addW, remB, addB,
                                                  pfBase, pfStride);

    // PawnPair/PassedPawns: indici folded, entrano nelle stesse liste threat.
    PawnFeatureSet::append_changed_indices(WHITE, ksqW, dirtyPawns, remW, addW);
    PawnFeatureSet::append_changed_indices(BLACK, ksqB, dirtyPawns, remB, addB);
    // R1 (_wip graft_passedrel3, 10/10/2026): con la sola PassedRel nelle liste le righe v1 delle due prospettive escono
    // dalla diff del blocco (append_changed_indices_both, piu' sotto): niente otto passers() fuori linea per evento di
    // pedone. Senza pedoni mossi v1 non fa nulla: anche le due chiamate si saltano.
    if (dirtyPawns.any && !PawnGraftSet::v1_skip())
    {
        Bitboard pb[COLOR_NB], pa[COLOR_NB];  // R4: una lettura per le due prospettive
        v1_pass_of(graftState, pb, pa);
        V1PASS_VERIFY(dirtyPawns, pb, pa);
        PassedFeatureSet::append_changed_indices_pass(WHITE, ksqW, pb, pa, remW, addW);
        PassedFeatureSet::append_changed_indices_pass(BLACK, ksqB, pb, pa, remB, addB);
    }
    NSTATV(PREL_V1_SKIP, dirtyPawns.any && PawnGraftSet::v1_fused());
    // _wip graftfix (P2): una sola decodifica per le due prospettive, solo se la diff non e' vuota.
#ifdef TRIUMV_VERIFY_GRAFT
    const int vrW = remW.ssize(), vaW = addW.ssize(), vrB = remB.ssize(), vaB = addB.ssize();  // R1
#endif
    if (graftState.graftRem | graftState.graftAdd)
    {
        NSTATV(GRAFT_ROWS, 2 * (graftState.graftRem + graftState.graftAdd));
#ifdef TRIUMV_GRAFT_PF
        const int rW = remW.ssize(), aW = addW.ssize(), rB = remB.ssize(), aB = addB.ssize();
#endif
        PawnGraftSet::append_changed_indices_both(ksqW, ksqB, graftState, remW, addW, remB, addB);
#ifdef TRIUMV_GRAFT_PF
        prefetch_graft_rows(featureTransformer, remW, rW);
        prefetch_graft_rows(featureTransformer, addW, aW);
        prefetch_graft_rows(featureTransformer, remB, rB);
        prefetch_graft_rows(featureTransformer, addB, aB);
#endif
    }
#ifdef TRIUMV_VERIFY_GRAFT
    if (dirtyPawns.any && PawnGraftSet::v1_fused())
    {
        PawnGraftSet::verify_v1_fused(WHITE, ksqW, dirtyPawns, remW.begin() + vrW, remW.ssize() - vrW,
                                      addW.begin() + vaW, addW.ssize() - vaW);
        PawnGraftSet::verify_v1_fused(BLACK, ksqB, dirtyPawns, remB.begin() + vrB, remB.ssize() - vrB,
                                      addB.begin() + vaB, addB.ssize() - vaB);
    }
#endif

#ifdef TRIUMV_PROFILE
    prof_n_cols += psqAddW.size() + psqRemW.size() + thrAddW.size() + thrRemW.size()
                 + psqAddB.size() + psqRemB.size() + thrAddB.size() + thrRemB.size();
    prof_n_upd += 2;
#endif

    // Applicazioni SEQUENZIALI: e' la differenza voluta da Stockfish.
    apply_combined(WHITE, featureTransformer, computed, target, psqAddW, psqRemW, thrAddW, thrRemW);
    apply_combined(BLACK, featureTransformer, computed, target, psqAddB, psqRemB, thrAddB, thrRemB);

    target_state.computed[WHITE] = 1;
    target_state.computed[BLACK] = 1;
}
#endif  // !TRIUMV_NO_PERSP_BOTH

// Codice del pezzo (0..11) su una casa, da dodici bitboard per pezzo, senza salti: tre piani di bit per il tipo
// (bit 0 = N R K, bit 1 = B R, bit 2 = Q K) e il nero (+6).
struct PiecePlanes {
    Bitboard p0, p1, p2, blk;
    template<typename BB>
    explicit PiecePlanes(const BB* b) :
        p0(b[1] | b[3] | b[5] | b[7] | b[9] | b[11]),
        p1(b[2] | b[3] | b[8] | b[9]),
        p2(b[4] | b[5] | b[10] | b[11]),
        blk(b[6] | b[7] | b[8] | b[9] | b[10] | b[11]) {}
    int operator()(int s) const {
        return (int((p0 >> s) & 1) | int((p1 >> s) & 1) << 1 | int((p2 >> s) & 1) << 2) + int((blk >> s) & 1) * 6;
    }
};

// Differenza fra la posizione di una entry della finny table e una posizione voluta, entrambe come dodici bitboard per
// pezzo (07/10/2026, scacchiera unica v2). Prima le case cambiate, in blocco e senza salti; poi due soli cicli (case da
// togliere dalla entry, case da aggiungere) col pezzo letto dai piani di bit. La versione con due cicli per ognuno dei
// dodici pezzi faceva +6% di salti mal previsti per nodo. L'ordine degli indici non conta: l'accumulatore e' una
// somma di interi (con avvolgimento a 16 e 32 bit), quindi il risultato e' identico in ogni bit.
template<typename List, typename BB>
inline void diff_entry(Color                           perspective,
                       const std::array<Bitboard, 12>& have,
                       const BB*                       want,
                       int                             ksq,
                       int                             phase,
                       List&                           removed,
                       List&                           added) {
    Bitboard changed = 0, haveOcc = 0, wantOcc = 0;
    for (int pc = 0; pc < 12; ++pc)
    {
        changed |= have[pc] ^ want[pc];
        haveOcc |= have[pc];
        wantOcc |= want[pc];
    }
    Bitboard rem = changed & haveOcc;
    Bitboard add = changed & wantOcc;
#ifdef TRIUMV_X4_VLREG
    // X4 (08/10/2026, VLREG): contatori delle due liste in registri (ValueList::Tail, misc.h).
    typename List::Tail remOut(removed), addOut(added);
#else
    List &remOut = removed, &addOut = added;
#endif
    if (rem)
    {
        const PiecePlanes look(have.data());
        while (rem)
        {
            const Square s = pop_lsb(rem);
            remOut.push_back(PSQFeatureSet::make_index(perspective, s, look(s), ksq, phase));
        }
    }
    if (add)
    {
        const PiecePlanes look(want);
        while (add)
        {
            const Square s = pop_lsb(add);
            addOut.push_back(PSQFeatureSet::make_index(perspective, s, look(s), ksq, phase));
        }
    }
}

#ifdef TRIUMV_VG_BIASBASE
// X2 (08/10/2026, VG_BIASBASE): scelta della base contando le righe. Una entry della finny table puo' essere vecchia
// di molte mosse (quattro tabelle per fascia, 64 case del re, due lati: le entry poco usate restano indietro). Dalla
// entry servono rem + add righe; dai soli bias (scacchiera vuota) ne serve una per pezzo. Se la entry costa di piu' si
// parte dai bias: le liste diventano "nessuna da togliere, tutti i pezzi da aggiungere" e la funzione rende true (il
// chiamante carica i bias e un PSQT nullo al posto della entry). Somme di interi a 16 e 32 bit con avvolgimento:
// risultato identico in ogni bit, cambia solo quante righe si leggono.
template<typename List, typename BB>
inline bool diff_entry_base(Color                           perspective,
                            const std::array<Bitboard, 12>& have,
                            const BB*                       want,
                            int                             ksq,
                            int                             phase,
                            List&                           removed,
                            List&                           added) {
    Bitboard changed = 0, haveOcc = 0, wantOcc = 0;
    for (int pc = 0; pc < 12; ++pc)
    {
        changed |= have[pc] ^ want[pc];
        haveOcc |= have[pc];
        wantOcc |= want[pc];
    }
    Bitboard rem = changed & haveOcc;
    Bitboard add = changed & wantOcc;
    if (popcount(rem) + popcount(add) > popcount(wantOcc))
    {
        const PiecePlanes look(want);
        Bitboard          all = wantOcc;
        while (all)
        {
            const Square s = pop_lsb(all);
            added.push_back(PSQFeatureSet::make_index(perspective, s, look(s), ksq, phase));
        }
        return true;
    }
    if (rem)
    {
        const PiecePlanes look(have.data());
        while (rem)
        {
            const Square s = pop_lsb(rem);
            removed.push_back(PSQFeatureSet::make_index(perspective, s, look(s), ksq, phase));
        }
    }
    if (add)
    {
        const PiecePlanes look(want);
        while (add)
        {
            const Square s = pop_lsb(add);
            added.push_back(PSQFeatureSet::make_index(perspective, s, look(s), ksq, phase));
        }
    }
    return false;
}

// PSQT di partenza quando la base sono i bias (una entry vuota ha PSQT nullo, vedi Entry::clear).
alignas(64) const PSQTWeightType kZeroPsqt[PSQTBuckets] = {};
#endif

#if defined(TRIUMV_VG_VERIFY_ANY)
// Verifica delle patch X2: ricalcolo da zero, scalare, di un accumulatore HalfKA (bias + una riga per pezzo) e di un
// accumulatore completo (HalfKA + minacce + PawnPair + PassedPawns), confrontati bit per bit con quelli prodotti dalle
// scorciatoie. Al primo disaccordo: messaggio e abort. Solo per le build di verifica: costa centinaia di righe a chiamata.
template<typename BB>
void vg_scratch_halfka(Color                     perspective,
                       const FeatureTransformer& ft,
                       const BB*                 pieces,
                       int                       ksq,
                       int                       phase,
                       i16*                      acc,
                       i32*                      psqt) {
    constexpr IndexType Dimensions = FeatureTransformer::OutputDimensions;
    for (IndexType j = 0; j < Dimensions; ++j)
        acc[j] = ft.biases[j];
    for (usize k = 0; k < PSQTBuckets; ++k)
        psqt[k] = 0;
    for (int pc = 0; pc < 12; ++pc)
    {
        Bitboard b = Bitboard(pieces[pc]);
        while (b)
        {
            const Square    s   = pop_lsb(b);
            const IndexType idx = PSQFeatureSet::make_index(perspective, s, pc, ksq, phase);
            for (IndexType j = 0; j < Dimensions; ++j)
                acc[j] = i16(u16(acc[j]) + u16(ft.weights[usize(idx) * Dimensions + j]));
            for (usize k = 0; k < PSQTBuckets; ++k)
                psqt[k] = i32(u32(psqt[k]) + u32(ft.psqtWeights[usize(idx) * PSQTBuckets + k]));
        }
    }
}

[[noreturn]] inline void vg_fail(const char* where, const char* what, Color perspective, int ksq, int phase) {
    std::fprintf(stderr, "VERIFY_VG %s: %s diverso (lato %d, re %d, fascia %d)\n", where, what, int(perspective),
                 ksq, phase);
    std::fflush(stderr);
    std::abort();
}

void vg_check_entry(const char*                      where,
                    Color                            perspective,
                    const FeatureTransformer&        ft,
                    const AccumulatorCaches::Entry&  e,
                    int                              ksq,
                    int                              phase) {
    constexpr IndexType Dimensions = FeatureTransformer::OutputDimensions;
    alignas(64) i16     acc[Dimensions];
    alignas(64) i32     psqt[PSQTBuckets];
    vg_scratch_halfka(perspective, ft, e.pieces.data(), ksq, phase, acc, psqt);
    if (std::memcmp(acc, e.accumulation.data(), sizeof(acc)) != 0)
        vg_fail(where, "entry HalfKA", perspective, ksq, phase);
    if (std::memcmp(psqt, e.psqtAccumulation.data(), sizeof(psqt)) != 0)
        vg_fail(where, "entry PSQT", perspective, ksq, phase);
}

void vg_check_acc(const char*               where,
                  Color                     perspective,
                  const FeatureTransformer& ft,
                  const NnBoard&            pos,
                  const Accumulator&        a) {
    constexpr IndexType Dimensions = FeatureTransformer::OutputDimensions;
    alignas(64) i16     acc[Dimensions];
    alignas(64) i32     psqt[PSQTBuckets];
    const int           ksq   = pos.king(perspective);
    const int           phase = PSQFeatureSet::phase_of_count(pos.count());
    vg_scratch_halfka(perspective, ft, pos.bbs(), ksq, phase, acc, psqt);
    ThreatFeatureSet::IndexList active;
    ThreatFeatureSet::append_active_indices(perspective, pos, active);
    PawnFeatureSet::append_active_indices(perspective, pos, active);
    PassedFeatureSet::append_active_indices(perspective, pos, active);
    if (nn_graft_mask)
        PawnGraftSet::append_active_indices(perspective, pos, active);
    for (int i = 0; i < active.ssize(); ++i)
    {
        const usize idx = usize(active[i]);
        for (IndexType j = 0; j < Dimensions; ++j)
            acc[j] = i16(u16(acc[j]) + u16(i16(ft.threatWeights[idx * Dimensions + j])));
        for (usize k = 0; k < PSQTBuckets; ++k)
            psqt[k] = i32(u32(psqt[k]) + u32(ft.threatPsqtWeights[idx * PSQTBuckets + k]));
    }
    if (std::memcmp(acc, a.accumulation[perspective].data(), sizeof(acc)) != 0)
        vg_fail(where, "accumulatore", perspective, ksq, phase);
    if (std::memcmp(psqt, a.psqtAccumulation[perspective].data(), sizeof(psqt)) != 0)
        vg_fail(where, "PSQT dell'accumulatore", perspective, ksq, phase);
}
#endif

#ifdef TRIUMV_VERIFY_PST_PE
// O1 (_wip pst_opt, 10/10/2026): la somma della entry "pe" (hit o miss) ricalcolata da zero, scalare, dai riferimenti
// della posizione: PawnPair, PassedPawns v1 (se accesa) e PassedState (PawnGraftSet::append_active_indices, voci da
// capo con entries_of, non quelle del recupero). Un hit con una chiave che non basta, o righe sbagliate nel miss,
// danno una somma diversa: messaggio e abort. Serve perche' nnperft confronta solo l'accumulatore finale, e il suo
// refresh di confronto passa dalla stessa cache "pe" del thread: un hit sbagliato potrebbe darlo a tutti e due.
[[noreturn]] void pe_fail(const char* what, Color perspective, int ksq, bool hit) {
    std::fprintf(stderr, "[PST_PE] %s della entry pe diverso dal riferimento: lato %d re %d %s\n", what,
                 int(perspective), ksq, hit ? "HIT" : "miss");
    std::fflush(stderr);
    std::abort();
}
void pe_verify_pst(Color                     perspective,
                   const FeatureTransformer& ft,
                   const NnBoard&            pos,
                   const std::int16_t*       peAcc,
                   const std::int32_t*       pePsqt,
                   bool                      hit) {
    constexpr IndexType              Dimensions = FeatureTransformer::OutputDimensions;
    static thread_local std::int16_t acc[Dimensions];
    std::int32_t                     psqt[PSQTBuckets] = {};
    std::memset(acc, 0, sizeof(acc));
    ThreatFeatureSet::IndexList ref;
    PawnFeatureSet::append_active_indices(perspective, pos, ref);
    if (!PawnGraftSet::v1_off())
        PassedFeatureSet::append_active_indices(perspective, pos, ref);
    PawnGraftSet::append_active_indices(perspective, pos, ref);
    for (int i = 0; i < ref.ssize(); ++i)
    {
        const usize idx = usize(ref[i]);
        for (IndexType j = 0; j < Dimensions; ++j)
            acc[j] = std::int16_t(std::uint16_t(acc[j])
                                  + std::uint16_t(std::int16_t(ft.threatWeights[idx * Dimensions + j])));
        for (usize k = 0; k < PSQTBuckets; ++k)
            psqt[k] = std::int32_t(std::uint32_t(psqt[k]) + std::uint32_t(ft.threatPsqtWeights[idx * PSQTBuckets + k]));
    }
    if (std::memcmp(acc, peAcc, sizeof(acc)) != 0)
        pe_fail("accumulatore", perspective, pos.king(perspective), hit);
    if (std::memcmp(psqt, pePsqt, sizeof(psqt)) != 0)
        pe_fail("PSQT", perspective, pos.king(perspective), hit);
    if (hit)
        NSTAT(REF_PST_VERIFIED_HIT);
}
#endif

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
                               const NnBoard&            pos,
                               const FeatureTransformer& featureTransformer,
                               Accumulator&              target,
                               NnState&                  target_state,
                               const Accumulator&        computed,
                               AccumulatorCaches&        cache) {
    NSTAT(HYBRID);
    constexpr IndexType Dimensions = FeatureTransformer::OutputDimensions;
    using Tiling [[maybe_unused]]  = SIMDTiling<Dimensions, Dimensions, PSQTBuckets>;

    const auto& dirtyPiece = target_state.dp;
    const int   newKsq     = pos.king(perspective);
    // Mossa del nostro re, oppure (04/10/2026) cattura che cambia fascia a re fermo.
    const int   oldKsq     = dirtyPiece.pc == NN_KING + NN_BLACK * int(perspective) ? int(dirtyPiece.from) : newKsq;

    // Ricostruzione della posizione PRECEDENTE (dodici bitboard per pezzo): il pezzo mosso torna dalla casa d'arrivo
    // a quella di partenza e l'eventuale catturato (anche en passant, fuori dalla casa d'arrivo) ricompare. Arrocco e
    // promozioni NON passano di qui (esclusi dal gate): muovono o cambiano un secondo pezzo.
    const auto*     currentPieces = pos.bbs();
    Bitboard        previousPieces[12];
    for (int pc = 0; pc < 12; ++pc)
        previousPieces[pc] = currentPieces[pc];
#ifdef TRIUMV_VG_HYBALL
    // Arrocco e promozioni ammessi (vedi il cancello in evaluate_side): prima si tolgono i pezzi ARRIVATI (pezzo mosso,
    // torre dell'arrocco o pezzo promosso), poi si rimettono quelli PARTITI (pezzo mosso, catturato o torre). Nell'arrocco
    // remove_sq e' la casa di partenza della torre, non una cattura: la fascia non cambia.
    const bool castling = dirtyPiece.to != NN_SQ_NONE && dirtyPiece.add_sq != NN_SQ_NONE;
    if (dirtyPiece.to != NN_SQ_NONE)
        previousPieces[dirtyPiece.pc] &= ~(1ULL << dirtyPiece.to);
    if (dirtyPiece.add_sq != NN_SQ_NONE)
        previousPieces[dirtyPiece.add_pc] &= ~(1ULL << dirtyPiece.add_sq);
    previousPieces[dirtyPiece.pc] |= 1ULL << dirtyPiece.from;
    if (dirtyPiece.remove_sq != NN_SQ_NONE)
        previousPieces[dirtyPiece.remove_pc] |= 1ULL << dirtyPiece.remove_sq;
    const bool captured = dirtyPiece.remove_sq != NN_SQ_NONE && !castling;
#else
    previousPieces[dirtyPiece.pc] ^= (1ULL << dirtyPiece.to) | (1ULL << dirtyPiece.from);
    const bool captured = dirtyPiece.remove_sq != NN_SQ_NONE;
    if (captured)
        previousPieces[dirtyPiece.remove_pc] |= 1ULL << dirtyPiece.remove_sq;
#endif

    // Fascia (HalfKA a esperti): la entry vecchia sta nella fascia della posizione PRIMA della mossa, che con una
    // cattura a cavallo di soglia e' diversa da quella di adesso.
    const int   psqPhase = PSQFeatureSet::phase_of(dirtyPiece);
    const int   oldPhase = PSQFeatureSet::phase_of_count(pos.count() + int(captured));
#ifdef TRIUMV_VG_OLDWB
    // X2 (08/10/2026, VG_OLDWB): la entry VECCHIA si riscrive con l'HalfKA della posizione precedente, che l'ibrido
    // ricostruisce comunque. Il caso che conta e' il cambio di fascia: con 24, 16 o 10 pezzi ogni cattura cambia
    // esperto, e tutte le catture sorelle partono dalla STESSA posizione precedente. Prima ognuna ricalcolava gli
    // stessi diff da una entry vecchia di molte mosse (la meta' circa delle ~15 righe HalfKA di un ibrido); con la
    // riscrittura le sorelle dopo la prima trovano la entry gia' allineata e non leggono nessuna riga vecchia. Nel
    // sottoalbero di una cattura la fascia vecchia non torna (i pezzi non aumentano), quindi la entry resta valida
    // per tutte le sorelle. Si scrive solo se la entry era davvero diversa.
    auto&       oldEntry = cache.at(oldPhase, Square(oldKsq))[perspective];
#else
    const auto& oldEntry = cache.at(oldPhase, Square(oldKsq))[perspective];
#endif
    auto&       newEntry = cache.at(psqPhase, Square(newKsq))[perspective];
    // La entry nuova si riscrive prima di leggere la vecchia: non devono essere la stessa.
    assert(&oldEntry != &newEntry);

    // "Remove"/"Add" = cosa togliere/aggiungere ALLA ENTRY per ottenere
    // l'accumulatore HalfKA voluto.
    PSQFeatureSet::IndexList oldRemove, oldAdd, newRemove, newAdd;
#ifdef TRIUMV_VG_BIASBASE
    // Base scelta contando le righe (vedi diff_entry_base): entry o bias, per ognuna delle due ricostruzioni.
    const bool oldFromBias =
      diff_entry_base(perspective, oldEntry.pieces, previousPieces, oldKsq, oldPhase, oldRemove, oldAdd);
    const bool newFromBias =
      diff_entry_base(perspective, newEntry.pieces, currentPieces, newKsq, psqPhase, newRemove, newAdd);
#else
    diff_entry(perspective, oldEntry.pieces, previousPieces, oldKsq, oldPhase, oldRemove, oldAdd);
    diff_entry(perspective, newEntry.pieces, currentPieces, newKsq, psqPhase, newRemove, newAdd);
#endif
    NSTATV(HYB_OLD_ROWS, oldRemove.size() + oldAdd.size());
    NSTATV(HYB_NEW_ROWS, newRemove.size() + newAdd.size());
#ifdef TRIUMV_VG_OLDWB
    // Entry vecchia da riallineare? (con la base dai bias la lista oldAdd contiene tutti i pezzi: e' sempre si')
    const bool oldDirty = oldRemove.ssize() + oldAdd.ssize() > 0;
#endif

    // Delta dei tre blocchi non-HalfKA. Gli indici di PawnPair/PassedPawns sono
    // "folded" nelle stesse liste (gia' offsettati), come nel percorso incrementale.
    ThreatFeatureSet::IndexList thrRemoved, thrAdded;
    const auto*                 pfBase   = &featureTransformer.threatWeights[0];
    IndexType                   pfStride = Dimensions;
    ThreatFeatureSet::append_changed_indices(perspective, newKsq, target_state.threats, thrRemoved,
                                             thrAdded, pfBase, pfStride);
    PawnFeatureSet::append_changed_indices(perspective, newKsq, target_state.pawns, thrRemoved,
                                           thrAdded);
    // R1 (_wip graft_passedrel3): un ibrido con pedoni e' una cattura di pedone che cambia fascia; con la sola PassedRel
    // le righe v1 escono dalla diff del blocco, come negli incrementali.
    if (target_state.pawns.any && !PawnGraftSet::v1_skip())
    {
        Bitboard pb[COLOR_NB], pa[COLOR_NB];  // R4
        v1_pass_of(target_state, pb, pa);
        V1PASS_VERIFY(target_state.pawns, pb, pa);
        PassedFeatureSet::append_changed_indices_pass(perspective, newKsq, pb, pa, thrRemoved, thrAdded);
    }
    NSTATV(PREL_V1_SKIP, target_state.pawns.any && PawnGraftSet::v1_fused());
#ifdef TRIUMV_VERIFY_GRAFT
    const int vr0 = thrRemoved.ssize(), va0 = thrAdded.ssize();  // R1
#endif
    if (target_state.graftRem | target_state.graftAdd)
    {
        NSTATV(GRAFT_ROWS, target_state.graftRem + target_state.graftAdd);
        PawnGraftSet::append_changed_indices(perspective, newKsq, target_state, thrRemoved, thrAdded);
    }
#ifdef TRIUMV_VERIFY_GRAFT
    if (target_state.pawns.any && PawnGraftSet::v1_fused())
        PawnGraftSet::verify_v1_fused(perspective, newKsq, target_state.pawns, thrRemoved.begin() + vr0,
                                      thrRemoved.ssize() - vr0, thrAdded.begin() + va0, thrAdded.ssize() - va0);
#endif

    const auto& fromAcc     = computed.accumulation[perspective];
    auto&       toAcc       = target.accumulation[perspective];
    const auto& fromPsqtAcc = computed.psqtAccumulation[perspective];
    auto&       toPsqtAcc   = target.psqtAccumulation[perspective];

    target_state.computed[perspective] = 1;

#ifdef VECTOR
    vec_t      acc[Tiling::NumRegs];
    psqt_vec_t psqt[Tiling::NumPsqtRegs];

    const auto* weights       = &featureTransformer.weights[0];
    const auto* threatWeights = &featureTransformer.threatWeights[0];

    // PSQT nel primo tile (04/10/2026 notte), come in apply_combined: sei cicli per feature in meno, stesse somme.
    static_assert(PSQTBuckets / Tiling::PsqtTileHeight == 1, "update_accumulator_hybrid: serve un solo tile PSQT");
    const auto* psqtWeights      = &featureTransformer.psqtWeights[0];
    const auto* thrPsqtWeights   = &featureTransformer.threatPsqtWeights[0];
    auto*       fromTilePsqt     = reinterpret_cast<const psqt_vec_t*>(&fromPsqtAcc[0]);
#ifdef TRIUMV_VG_OLDWB
    auto*       oldEntryTilePsqt = reinterpret_cast<psqt_vec_t*>(&oldEntry.psqtAccumulation[0]);
#else
    auto*       oldEntryTilePsqt = reinterpret_cast<const psqt_vec_t*>(&oldEntry.psqtAccumulation[0]);
#endif
    auto*       newEntryTilePsqt = reinterpret_cast<psqt_vec_t*>(&newEntry.psqtAccumulation[0]);
    auto*       toTilePsqt       = reinterpret_cast<psqt_vec_t*>(&toPsqtAcc[0]);
#ifdef TRIUMV_VG_BIASBASE
    // Basi delle due ricostruzioni HalfKA: la entry oppure i bias con PSQT nullo.
    auto* oldBasePsqt = oldFromBias ? reinterpret_cast<const psqt_vec_t*>(kZeroPsqt)
                                    : static_cast<const psqt_vec_t*>(oldEntryTilePsqt);
    auto* newBasePsqt = newFromBias ? reinterpret_cast<const psqt_vec_t*>(kZeroPsqt)
                                    : static_cast<const psqt_vec_t*>(newEntryTilePsqt);
#else
    auto* oldBasePsqt = static_cast<const psqt_vec_t*>(oldEntryTilePsqt);
    auto* newBasePsqt = static_cast<const psqt_vec_t*>(newEntryTilePsqt);
#endif

    const auto tile = [&](const IndexType j, auto withPsqtTag) {
        constexpr bool WithPsqt     = decltype(withPsqtTag)::value;
        const usize    tileOff      = j * Tiling::TileHeight;
        auto*          fromTile     = reinterpret_cast<const vec_t*>(&fromAcc[tileOff]);
#ifdef TRIUMV_VG_OLDWB
        auto*          oldEntryTile = reinterpret_cast<vec_t*>(&oldEntry.accumulation[tileOff]);
#else
        auto*          oldEntryTile = reinterpret_cast<const vec_t*>(&oldEntry.accumulation[tileOff]);
#endif
        auto*          newEntryTile = reinterpret_cast<vec_t*>(&newEntry.accumulation[tileOff]);
        auto*          toTile       = reinterpret_cast<vec_t*>(&toAcc[tileOff]);
#ifdef TRIUMV_VG_BIASBASE
        auto* biasTile    = reinterpret_cast<const vec_t*>(&featureTransformer.biases[tileOff]);
        auto* oldBaseTile = oldFromBias ? biasTile : static_cast<const vec_t*>(oldEntryTile);
        auto* newBaseTile = newFromBias ? biasTile : static_cast<const vec_t*>(newEntryTile);
#else
        auto* oldBaseTile = static_cast<const vec_t*>(oldEntryTile);
        auto* newBaseTile = static_cast<const vec_t*>(newEntryTile);
#endif

#ifdef TRIUMV_VG_OLDWB
        // 0) HalfKA VECCHIO, esatto, riscritto nella sua entry: base meno oldRemove piu' oldAdd. Dopo questo passo
        //    la entry vecchia vale esattamente l'HalfKA della posizione precedente e il passo 3 non serve piu'.
        if (oldDirty)
        {
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = oldBaseTile[k];
            if constexpr (WithPsqt)
                for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = oldBasePsqt[k];
            for (int i = 0; i < oldRemove.ssize(); ++i)
            {
                auto* column =
                  reinterpret_cast<const vec_t*>(&weights[oldRemove[i] * Dimensions + tileOff]);
                for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                    acc[k] = vec_sub_16(acc[k], column[k]);
                if constexpr (WithPsqt)
                {
                    auto* columnPsqt =
                      reinterpret_cast<const psqt_vec_t*>(&psqtWeights[oldRemove[i] * PSQTBuckets]);
                    for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                        psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
                }
            }
            for (int i = 0; i < oldAdd.ssize(); ++i)
            {
                auto* column =
                  reinterpret_cast<const vec_t*>(&weights[oldAdd[i] * Dimensions + tileOff]);
                for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                    acc[k] = vec_add_16(acc[k], column[k]);
                if constexpr (WithPsqt)
                {
                    auto* columnPsqt =
                      reinterpret_cast<const psqt_vec_t*>(&psqtWeights[oldAdd[i] * PSQTBuckets]);
                    for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                        psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
                }
            }
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                vec_store(&oldEntryTile[k], acc[k]);
            if constexpr (WithPsqt)
                for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
                    vec_store_psqt(&oldEntryTilePsqt[k], psqt[k]);
        }
#endif

        // 1) HalfKA NUOVO, esatto, a partire dalla finny entry del nuovo ksq.
        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            acc[k] = newBaseTile[k];
        if constexpr (WithPsqt)
            for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = newBasePsqt[k];
        for (int i = 0; i < newRemove.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[newRemove[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], column[k]);
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&psqtWeights[newRemove[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
            }
        }
        for (int i = 0; i < newAdd.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[newAdd[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], column[k]);
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&psqtWeights[newAdd[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
            }
        }

#ifdef TRIUMV_VG_OLDWB
        // Con la riscrittura (passo 0) la entry vecchia e' gia' l'HalfKA esatto: si toglie lei e basta.
        auto* oldSubTile = static_cast<const vec_t*>(oldEntryTile);
        auto* oldSubPsqt = static_cast<const psqt_vec_t*>(oldEntryTilePsqt);
#else
        auto* oldSubTile = oldBaseTile;
        auto* oldSubPsqt = oldBasePsqt;
#endif
        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
        {
            // La finny entry del NUOVO ksq e' ora aggiornata (HalfKA puro).
            vec_store(&newEntryTile[k], acc[k]);
            // 2) Sommando l'accumulatore precedente entrano threat e pp gia' pronte,
            //    ma anche l'HalfKA del VECCHIO king bucket, che va tolto.
            acc[k] = vec_add_16(acc[k], fromTile[k]);
            acc[k] = vec_sub_16(acc[k], oldSubTile[k]);
        }
        if constexpr (WithPsqt)
            for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            {
                vec_store_psqt(&newEntryTilePsqt[k], psqt[k]);
                psqt[k] = vec_add_psqt_32(psqt[k], fromTilePsqt[k]);
                psqt[k] = vec_sub_psqt_32(psqt[k], oldSubPsqt[k]);
            }
#ifndef TRIUMV_VG_OLDWB
        // 3) ...e si corregge con i diff della entry vecchia, a segno INVERTITO:
        //    stiamo togliendo l'HalfKA precedente, non aggiungendolo.
        for (int i = 0; i < oldRemove.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[oldRemove[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], column[k]);
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&psqtWeights[oldRemove[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
            }
        }
        for (int i = 0; i < oldAdd.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[oldAdd[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], column[k]);
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&psqtWeights[oldAdd[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
            }
        }
#endif

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
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&thrPsqtWeights[thrRemoved[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
            }
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
            if constexpr (WithPsqt)
            {
                // ⚠️ Le feature attive alimentano ANCHE threatPsqtWeights: dimenticarlo darebbe un PSQT stantio in
                // silenzio (lezione del 3/08: bench 262736 invece di 207259).
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&thrPsqtWeights[thrAdded[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
            }
        }

        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            vec_store(&toTile[k], acc[k]);
        if constexpr (WithPsqt)
            for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
                vec_store_psqt(&toTilePsqt[k], psqt[k]);
    };

    tile(0, std::true_type{});
    for (IndexType j = 1; j < Dimensions / Tiling::TileHeight; ++j)
        tile(j, std::false_type{});

    // Le entry della finny ora riflettono le rispettive posizioni HalfKA.
    for (int pc = 0; pc < 12; ++pc)
        newEntry.pieces[pc] = currentPieces[pc];
    #ifdef TRIUMV_VG_OLDWB
    if (oldDirty)
        for (int pc = 0; pc < 12; ++pc)
            oldEntry.pieces[pc] = previousPieces[pc];
    #endif
    #if defined(TRIUMV_VG_VERIFY_ANY)
    vg_check_entry("ibrido, entry nuova", perspective, featureTransformer, newEntry, newKsq, psqPhase);
    vg_check_acc("ibrido", perspective, featureTransformer, pos, target);
    #endif
    #ifdef TRIUMV_VERIFY_VG_OLDWB
    vg_check_entry("ibrido, entry vecchia", perspective, featureTransformer, oldEntry, oldKsq, oldPhase);
    #endif
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
                                      const NnBoard&            pos,
                                      Accumulator&              accumulator,
                                      NnState&                  state,
                                      AccumulatorCaches&        cache) {
    NSTAT(REFRESH);
    constexpr auto Dimensions = FeatureTransformer::OutputDimensions;

    using Tiling [[maybe_unused]] = SIMDTiling<Dimensions, Dimensions, PSQTBuckets>;

    const int ksq = pos.king(perspective);
    // Fascia (HalfKA a esperti) della posizione da ricostruire: si lavora sulla finny table di quella fascia.
    const int                psqPhase = PSQFeatureSet::phase_of_count(pos.count());
    auto&                    entry    = cache.at(psqPhase, Square(ksq))[perspective];
    PSQFeatureSet::IndexList removed, added;

#ifdef TRIUMV_VG_BIASBASE
    // Base scelta contando le righe (vedi diff_entry_base): se la entry e' piu' lontana della scacchiera vuota si
    // riparte dai bias. La entry riceve comunque l'HalfKA esatto della posizione.
    const bool fromBias = diff_entry_base(perspective, entry.pieces, pos.bbs(), ksq, psqPhase, removed, added);
#else
    diff_entry(perspective, entry.pieces, pos.bbs(), ksq, psqPhase, removed, added);
#endif
    NSTATV(REF_ROWS, removed.size() + added.size());
    for (int pc = 0; pc < 12; ++pc)
        entry.pieces[pc] = pos.bb(pc);
    // Prefetch delle righe HalfKA qui: provato (01/10/2026, NPS 0,00% a vuoto, +0,03% sotto carico) e tolto (08/10/2026).

    // --- cache del refresh per i blocchi PEDONI (PawnPair + PassedPawns) ----------------
    // La finny table copre solo HalfKAv2_hm: gli altri blocchi si ricostruivano da zero a
    // OGNI refresh. Threats no (dipendono dalla posizione intera), ma PawnPair e PassedPawns
    // dipendono ESATTAMENTE da (pedoni bianchi, pedoni neri, orientation) — verificato nelle
    // rispettive make_index. E i refresh sono scatenati da mosse di RE, che i pedoni non li
    // toccano: fra due refresh consecutivi la chiave e' quasi sempre la stessa.
    // La chiave e' i due bitboard PER INTERO, non un hash: nessuna collisione possibile.
    // `orientation` (non ksq) perche' e' l'unico modo in cui il re entra negli indici, e ha
    // due soli valori per prospettiva (OrientTBL dipende dalla meta' di scacchiera del re).
    const Bitboard wpBB   = pos.pawns(WHITE);
    const Bitboard bpBB   = pos.pawns(BLACK);
    const int      orient = int(Features::FullThreats::OrientTBL[ksq]) ^ (56 * (1 - int(perspective)));

    struct PawnRefreshEntry {
        Bitboard wp = ~Bitboard(0), bp = ~Bitboard(0);  // stato iniziale impossibile => miss
        int      orient = -1;
        unsigned epoch  = ~0u;  // nn_net_epoch della rete con cui la somma e' stata fatta (_wip graft_space_locked)
        // O1 (_wip pst_opt, 10/10/2026): con PassedState la somma contiene anche le sue righe, e la chiave anche la
        // lista delle sue voci (scritta solo con PassedState; con le altre reti non si legge). Sta nella prima linea.
        std::uint8_t  pstN = 0;
        std::uint16_t pst[NN_GRAFT_MAX];
        alignas(64) std::int16_t acc[FeatureTransformer::OutputDimensions];
        alignas(64) std::int32_t psqt[PSQTBuckets];
    };
    // 8 entry = 16 KB per thread: resta in L1/L2. Piu' grande peggiorerebbe cio' che
    // stiamo ottimizzando, che e' traffico di memoria, non conto di istruzioni.
    static constexpr int  PawnCacheMask = 7;
    static thread_local PawnRefreshEntry pawnCache[PawnCacheMask + 1];

    PawnRefreshEntry& pe = pawnCache[(unsigned(wpBB ^ bpBB) ^ unsigned((wpBB ^ bpBB) >> 29)
                                      ^ unsigned(orient)) & PawnCacheMask];
// TRIUMV_PAWN_CACHE_AVX512 (09/09/2026): riapre la cache anche su AVX-512 per rimisurarla su
// Skylake-SP; il -0,11% che l'ha spenta era su Zen4 (60 posizioni, lettura instabile).
// ✅ 25/09/2026 — RIACCESA anche su AVX-512 (stessa misura dell'ibrido, sopra): -1,41%
// istruzioni/nodo, branch miss e miss L3 invariati. -DTRIUMV_NO_PAWN_CACHE_AVX512 la rispegne.
#if defined(VECTOR) && !defined(TRIUMV_NO_PAWN_CACHE) && (!defined(USE_AVX512) || !defined(TRIUMV_NO_PAWN_CACHE_AVX512))
    #define TRIUMV_PE_CACHE_ON
    bool pawnHit = (pe.wp == wpBB) & (pe.bp == bpBB) & (pe.orient == orient) & (pe.epoch == nn_net_epoch);
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
    // O1 (_wip pst_opt, 10/10/2026; docs/audit_8.0/PASSEDSTATE_COSTO.md §2, R-pe): le righe di PassedState dentro la
    // cache "pe". Dipendono dalla prospettiva e dal re solo attraverso l'orientazione (PassedState::make_index ->
    // PassedPawns::make_index), gia' nella chiave; il resto e' la lista delle voci (passati, stop, prot, conn, mu), che
    // entra nella chiave PER INTERO: hit = stessi pedoni, orientazione ed epoca della rete E stessa lista (numero e
    // voci, confronto esatto, nessun hash). Le voci sono ordinate e uniche per (colore, casa): stessa lista = stesse
    // righe. Nel miss le righe vanno dopo nThreat, nella somma che si scrive nella entry; nell'hit non si enumerano ne'
    // si sommano. Somme intere modulo 2^16 / 2^32: l'ordine non conta, valutazione identica. Le voci sono quelle del
    // recupero (NnStack::graftList) o, senza (radice di uno stato di appoggio), quelle da capo.
    // -DTRIUMV_VERIFY_PST_PE ricalcola la somma della entry a ogni refresh con PassedState (pe_verify_pst).
    const bool           pstOn = (nn_graft_mask & PawnGraftSet::PASSED_STATE) != 0;
    std::uint16_t        pstBuf[NN_GRAFT_MAX];
    const std::uint16_t* pstL = pstBuf;
    int                  pstN = 0;
    if (pstOn)
    {
        pstN = PawnGraftSet::pst_entries(pos, state, pstBuf, pstL);
#ifdef TRIUMV_PE_CACHE_ON
        const bool pstSame =
          pe.pstN == pstN && std::memcmp(pe.pst, pstL, usize(pstN) * sizeof(std::uint16_t)) == 0;
    #ifdef TRIUMV_NSTATS
        if (pawnHit && !pstSame)
            NSTAT(REF_PST_LISTMISS);
    #endif
        pawnHit = pawnHit && pstSame;
#endif
    }
    // Blocco da innesto (09/10/2026): PassedRel con le minacce, fuori dalla cache dei blocchi pedoni (dipende anche dai
    // re e dall'occupazione). Le voci sono quelle gia' calcolate dal recupero per questo stato. (Space e LockedPawns,
    // che stavano nella cache dei pedoni, tolti il 10/10/2026 con KingFiles e KingFilesQ.)
    else if (nn_graft_mask & PawnGraftSet::LISTS)
    {
#ifdef TRIUMV_NSTATS
        const int g0 = active.ssize();
#endif
        // R2 (_wip graft_passedrel3): sola PassedRel in linea (pawn_grafts.h), il resto fuori linea come prima.
        PawnGraftSet::append_active_indices(perspective, ksq, pos, state, active);
        NSTATV(GRAFT_REF_ROWS, active.ssize() - g0);
    }
    const int nThreat = active.ssize();
    if (!pawnHit)
    {
        // Miss: si enumera come prima. Il hit salta anche QUESTO, non solo le somme.
        PawnFeatureSet::append_active_indices(perspective, pos, active);    // TRANN1 folded
        if (!PawnGraftSet::v1_off())  // PassedState con la v1 a zero: righe nulle, non si sommano
            PassedFeatureSet::append_active_indices(perspective, pos, active);  // v3 folded
        if (pstOn)  // O1: righe di PassedState nella somma della entry, lista nella chiave
        {
            constexpr IndexType base = PawnGraftSet::FoldOffset + PawnGraftSet::Offset[6];
            for (int i = 0; i < pstN; ++i)
                active.push_back(base + Features::PassedState::make_index(perspective, ksq, pstL[i]));
            pe.pstN = std::uint8_t(pstN);
            std::memcpy(pe.pst, pstL, usize(pstN) * sizeof(std::uint16_t));
            NSTATV(GRAFT_REF_ROWS, pstN);
        }
        pe.wp = wpBB, pe.bp = bpBB, pe.orient = orient, pe.epoch = nn_net_epoch;
        NSTATV(REF_PAWN_ROWS, active.ssize() - nThreat);
    }
    else
        NSTAT(REF_PAWN_HIT);
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

    state.computed[perspective] = 1;

#ifdef VECTOR
    vec_t      acc[Tiling::NumRegs];
    psqt_vec_t psqt[Tiling::NumPsqtRegs];

    const auto* weights       = &featureTransformer.weights[0];
    const auto* threatWeights = &featureTransformer.threatWeights[0];

    // PSQT nel primo tile (04/10/2026 notte), come in apply_combined: i cicli per feature del PSQT (removed, added,
    // threat attive, blocchi pedoni) non girano piu' da soli, li fa il primo tile insieme alle sue righe. Stesse
    // somme nello stesso ordine; le due cache (finny entry, blocchi pedoni) ricevono gli stessi valori di prima.
    static_assert(PSQTBuckets / Tiling::PsqtTileHeight == 1, "update_accumulator_refresh_cache: un solo tile PSQT");
    const auto* psqtWeights    = &featureTransformer.psqtWeights[0];
    const auto* thrPsqtWeights = &featureTransformer.threatPsqtWeights[0];
    auto* accTilePsqt   = reinterpret_cast<psqt_vec_t*>(&accumulator.psqtAccumulation[perspective][0]);
    auto* entryTilePsqt = reinterpret_cast<psqt_vec_t*>(&entry.psqtAccumulation[0]);
    auto* pawnTilePsqt  = reinterpret_cast<psqt_vec_t*>(&pe.psqt[0]);

    const auto tile = [&](const IndexType j, auto withPsqtTag) {
        constexpr bool WithPsqt = decltype(withPsqtTag)::value;
        const usize    tileOff  = j * Tiling::TileHeight;
        auto* accTile   = reinterpret_cast<vec_t*>(&accumulator.accumulation[perspective][tileOff]);
        auto* entryTile = reinterpret_cast<vec_t*>(&entry.accumulation[tileOff]);

#ifdef TRIUMV_VG_BIASBASE
        auto* baseTile = fromBias ? reinterpret_cast<const vec_t*>(&featureTransformer.biases[tileOff])
                                  : static_cast<const vec_t*>(entryTile);
        auto* basePsqt = fromBias ? reinterpret_cast<const psqt_vec_t*>(kZeroPsqt)
                                  : static_cast<const psqt_vec_t*>(entryTilePsqt);
#else
        auto* baseTile = entryTile;
        auto* basePsqt = entryTilePsqt;
#endif
        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            acc[k] = baseTile[k];
        if constexpr (WithPsqt)
            for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = basePsqt[k];

        for (int i = 0; i < removed.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[removed[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], column[k]);
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&psqtWeights[removed[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
            }
        }
        for (int i = 0; i < added.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[added[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], column[k]);
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&psqtWeights[added[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
            }
        }

        for (IndexType k = 0; k < Tiling::NumRegs; k++)
            vec_store(&entryTile[k], acc[k]);
        if constexpr (WithPsqt)
            for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
                vec_store_psqt(&entryTilePsqt[k], psqt[k]);

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
            if constexpr (WithPsqt)
            {
                auto* columnPsqt =
                  reinterpret_cast<const psqt_vec_t*>(&thrPsqtWeights[active[i] * PSQTBuckets]);
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
            }
        }

        // Blocchi pedoni. Hit = UNA lettura contigua di 2 KB al posto di N colonne sparse
        // da 2 KB l'una: e' il traffico di memoria che si taglia, non le istruzioni.
        // I blocchi pedoni contribuiscono ANCHE al PSQT (threatPsqtWeights): la cache deve
        // coprire tutti e due gli accumulatori, o al hit il PSQT resta indietro in silenzio.
        auto* pawnTile = reinterpret_cast<vec_t*>(&pe.acc[tileOff]);
        if (pawnHit)
        {
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], pawnTile[k]);
            if constexpr (WithPsqt)
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    psqt[k] = vec_add_psqt_32(psqt[k], pawnTilePsqt[k]);
        }
        else
        {
            // Miss: si somma in un vettore SEPARATO (non su acc) perche' quel vettore va
            // memorizzato da solo — sommarlo su acc non lo renderebbe riusabile.
            vec_t pv[Tiling::NumRegs];
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                pv[k] = vec_zero();
            psqt_vec_t pq[Tiling::NumPsqtRegs];
            if constexpr (WithPsqt)
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                    pq[k] = vec_zero_psqt();

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
                if constexpr (WithPsqt)
                {
                    auto* columnPsqt =
                      reinterpret_cast<const psqt_vec_t*>(&thrPsqtWeights[active[i] * PSQTBuckets]);
                    for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                        pq[k] = vec_add_psqt_32(pq[k], columnPsqt[k]);
                }
            }

            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            {
                vec_store(&pawnTile[k], pv[k]);
                acc[k] = vec_add_16(acc[k], pv[k]);
            }
            if constexpr (WithPsqt)
                for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                {
                    vec_store_psqt(&pawnTilePsqt[k], pq[k]);
                    psqt[k] = vec_add_psqt_32(psqt[k], pq[k]);
                }
        }

        for (IndexType k = 0; k < Tiling::NumRegs; k++)
            vec_store(&accTile[k], acc[k]);
        if constexpr (WithPsqt)
            for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
                vec_store_psqt(&accTilePsqt[k], psqt[k]);
    };

    tile(0, std::true_type{});
    for (IndexType j = 1; j < Dimensions / Tiling::TileHeight; ++j)
        tile(j, std::false_type{});

    #if defined(TRIUMV_VG_VERIFY_ANY)
    vg_check_entry("refresh, entry", perspective, featureTransformer, entry, ksq, psqPhase);
    vg_check_acc("refresh", perspective, featureTransformer, pos, accumulator);
    #endif
    #ifdef TRIUMV_VERIFY_PST_PE
    if (pstOn)
        pe_verify_pst(perspective, featureTransformer, pos, pe.acc, pe.psqt, pawnHit);
    #endif

#else

    #ifdef TRIUMV_VG_BIASBASE
    if (fromBias)
    {
        entry.accumulation = featureTransformer.biases;
        entry.psqtAccumulation.fill(0);
    }
    #endif
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

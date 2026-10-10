// nstats.h -- contatori del lavoro della rete (08/10/2026), SOLO con -DTRIUMV_NSTATS.
// Servono a confrontare quanto lavoro di rete fa ogni motore PER NODO: valutazioni, aggiornamenti incrementali degli
// accumulatori, refresh dalla finny table, righe di pesi lette. Una copia identica di questo file va nel sorgente di
// Stockfish 19 usato per il confronto (scratchpad sf19n), con le macro negli stessi punti di nnue_accumulator.cpp.
// A fine processo, se la variabile d'ambiente NSTATS_FILE e' impostata, aggiunge al file una riga "chiave valore" per
// contatore. I nodi si prendono dal carico (uci_workload.py), che li legge dal motore.
// Senza TRIUMV_NSTATS le macro non fanno nulla: codice generato identico.
#pragma once

#ifdef TRIUMV_NSTATS
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace NStats {
enum Key {
    EVAL,        // AccumulatorStack::evaluate: valutazioni della rete che passano dagli accumulatori
    INC,         // update_accumulator_incremental: un passo incrementale, un lato
    INC_BOTH,    // update_accumulator_incremental_both: un passo incrementale, entrambi i lati
    REFRESH,     // update_accumulator_refresh_cache: un refresh dalla finny table, un lato
    HYBRID,      // update_accumulator_hybrid: un aggiornamento ibrido, un lato
    COMBINED,    // apply_combined: chiamate (un lato)
    PSQ_ROWS,    // righe posizionali (aggiunte + tolte) lette da apply_combined
    THR_ROWS,    // righe di minaccia (aggiunte + tolte) lette da apply_combined
    // X2 (08/10/2026): righe HalfKA lette dalle ricostruzioni dalla finny table. Solo nel nostro motore: chiavi in
    // coda, la copia nel sorgente di confronto non le ha e non ne ha bisogno.
    HYB_OLD_ROWS,  // ibrido, entry vecchia (posizione precedente)
    HYB_NEW_ROWS,  // ibrido, entry nuova
    REF_ROWS,      // refresh, entry
    // (Contatori dei blocchi da innesto GRAFT_*, PREL_*, REF_PST_*: tolti il 10/10/2026 con i blocchi.)
    REF_PAWN_HIT,   // refresh con hit della cache "pe" (PawnPair, PassedPawns gratis)
    REF_PAWN_ROWS,  // refresh con miss: righe dei blocchi pedoni sommate (le quattro famiglie)
    NKEYS
};
inline const char* const kName[NKEYS] = {"EVAL", "INC", "INC_BOTH", "REFRESH", "HYBRID", "COMBINED", "PSQ_ROWS",
                                         "THR_ROWS", "HYB_OLD_ROWS", "HYB_NEW_ROWS", "REF_ROWS",
                                         "REF_PAWN_HIT", "REF_PAWN_ROWS"};
inline std::uint64_t c[NKEYS];
inline void dump() {
    const char* f = std::getenv("NSTATS_FILE");
    if (!f) return;
    if (std::FILE* o = std::fopen(f, "a")) {
        for (int k = 0; k < NKEYS; k++)
            std::fprintf(o, "%s %llu\n", kName[k], (unsigned long long) c[k]);
        std::fprintf(o, "END\n");
        std::fclose(o);
    }
}
inline const int reg = (std::atexit(dump), 0);
}  // namespace NStats
#define NSTAT(k) (++NStats::c[NStats::k])
#define NSTATV(k, v) (NStats::c[NStats::k] += std::uint64_t(v))
#else
#define NSTAT(k) ((void) 0)
#define NSTATV(k, v) ((void) 0)
#endif

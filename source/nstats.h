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
    // Blocchi da innesto (10/10/2026): contatori di GRAFT_PASSEDREL_COSTO2.md §6 e di GRAFT_KINGFILES_OTTIMIZZAZIONE.md.
    GRAFT_ST,       // stati del recupero con una rete a blocchi (nn_dirty_catch_up, gm != 0)
    GRAFT_PASS,     // ... di cui con passati prima o dopo la mossa (superano il primo filtro di nn_prel_step)
    GRAFT_REBUILD,  // ... ricalcolo da capo con merge (cambia l'insieme dei passati)
    GRAFT_RESTATE,  // ... ricalcolo dei bit 0 e 2 (re nemico che attraversa un quadrato, casa davanti toccata)
    GRAFT_CHG,      // ... diff non vuota
    GRAFT_ROWS,     // righe dei blocchi da innesto nelle liste incrementali e ibride (somma sulle prospettive)
    GRAFT_REF_ROWS, // righe dei blocchi da innesto nei refresh pieni
    // (GRAFT_STEP e GRAFT_KF_*: tolti il 10/10/2026 con KingFiles e il recupero generico nn_graft_step)
    GRAFT_ROWS_INC,   // righe dei blocchi lette dagli aggiornamenti incrementali (per lato)
    GRAFT_ROWS_HYB,   // righe dei blocchi lette dagli ibridi
    GRAFT_ROWS_REF,   // righe dei blocchi lette dai refresh pieni
    REF_PAWN_HIT,   // refresh con hit della cache "pe" (PawnPair, PassedPawns gratis)
    REF_PAWN_ROWS,  // refresh con miss: righe dei blocchi pedoni sommate (le quattro famiglie)
    // _wip graft_passedrel3 (10/10/2026, GRAFT_PASSEDREL_COSTO3.md)
    PREL_V1_SKIP,   // R1: aggiornamenti (incrementali e ibridi, una chiamata per percorso) con evento di pedone in cui
                    // la v1 non ricalcola i passati (righe v1 dalla diff di PassedRel)
    PREL_DELTA,     // R3: righe delta usate (ognuna sostituisce due righe di PassedRel), per prospettiva
    // _wip pst_opt (10/10/2026, O1): con PassedState REF_PAWN_HIT conta gli hit con la lista uguale (righe del blocco
    // gratis); questi i refresh con pedoni, orientazione ed epoca uguali ma lista diversa (miss per la sola lista).
    REF_PST_LISTMISS,
    REF_PST_VERIFIED_HIT,  // -DTRIUMV_VERIFY_PST_PE: hit della cache "pe" con PassedState controllati da zero
    NKEYS
};
inline const char* const kName[NKEYS] = {"EVAL", "INC", "INC_BOTH", "REFRESH", "HYBRID", "COMBINED", "PSQ_ROWS",
                                         "THR_ROWS", "HYB_OLD_ROWS", "HYB_NEW_ROWS", "REF_ROWS", "GRAFT_ST",
                                         "GRAFT_PASS", "GRAFT_REBUILD", "GRAFT_RESTATE", "GRAFT_CHG", "GRAFT_ROWS",
                                         "GRAFT_REF_ROWS", "GRAFT_ROWS_INC", "GRAFT_ROWS_HYB", "GRAFT_ROWS_REF",
                                         "REF_PAWN_HIT", "REF_PAWN_ROWS", "PREL_V1_SKIP", "PREL_DELTA",
                                         "REF_PST_LISTMISS", "REF_PST_VERIFIED_HIT"};
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

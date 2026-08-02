#ifndef TRIUMV_PROFILE_H
#define TRIUMV_PROFILE_H

// Compile-time profiling instrumentation. Define TRIUMV_PROFILE (e.g. /D TRIUMV_PROFILE
// in the build, or `make profile`) to enable per-phase cycle counters
// (eval / movegen / make / tt / score). Default builds (no define) compile every
// PROF_GUARD to a no-op => zero overhead, byte-identical to the release engine.
#ifdef TRIUMV_PROFILE
  #include <intrin.h>
  extern unsigned long long prof_eval, prof_mg, prof_make, prof_tt, prof_score;
  // Scomposizione del forward NNUE (prof_eval). Serve a rispondere a UNA domanda che
  // blocca il lavoro sulla sparsita' di L1: quanto pesa davvero fc_0? L'audit archivio'
  // `ft_optimize` con la condizione esplicita "se fc_0 e' sotto il 5% del tempo, chiuso",
  // e quella misura non e' mai stata fatta.
  //   prof_ft     = feature transformer (accumulatore + transform)
  //   prof_fc0    = il solo AffineTransformSparseInput 1024->16
  //   prof_layers = tutto il resto (attivazioni + fc_1 + fc_2)
  extern unsigned long long prof_ft, prof_fc0, prof_layers;
  // Scomposizione del feature transformer (prof_ft), che il primo giro di misure ha
  // rivelato essere il 39,3% del wall e il 91,2% del forward: il collo di bottiglia
  // numero uno del motore. Qui si separa DOVE va quel tempo.
  //   prof_acc_inc     = update incrementale dell'accumulatore (add/sub di colonne)
  //   prof_acc_refresh = refresh completo dalla cache (quando l'incrementale non basta)
  //   prof_ft_out      = clipped ReLU + moltiplicazione pairwise che produce l'input di fc_0
  // Contatori di EVENTI, non di cicli: quante volte si prende ciascuna via.
  extern unsigned long long prof_acc_inc, prof_acc_refresh, prof_ft_out;
  extern unsigned long long prof_n_inc, prof_n_refresh, prof_n_eval;
  // prof_n_cols / prof_n_upd = colonne di peso sommate o sottratte per aggiornamento.
  extern unsigned long long prof_n_cols, prof_n_upd;
  // Tuple threat generate vs BUTTATE dal filtro (map<0 => pedone->pedone, escluse dal set).
  extern unsigned long long prof_n_thr_seen, prof_n_thr_dead;
  // Massimo riempimento delle IndexList, contro MaxActiveDimensions = 288.
  extern unsigned long long prof_max_active, prof_max_inc;
  // Al REFRESH: colonne attive per blocco. I due blocchi pedoni dipendono solo dalla
  // struttura di pedoni => sono gli unici cachabili fra refresh consecutivi (che sono
  // scatenati da mosse di RE). Il rapporto pawn/(pawn+threat) e' il tetto del guadagno.
  extern unsigned long long prof_cols_thr, prof_cols_pawn, prof_n_refresh_calls;
  extern unsigned long long prof_dead_pair[8][8];
  inline unsigned long long prof_now() { return __rdtsc(); }
  struct ProfGuard {
      unsigned long long& c; unsigned long long t;
      ProfGuard(unsigned long long& cc) : c(cc), t(__rdtsc()) {}
      ~ProfGuard() { c += __rdtsc() - t; }
  };
  #define PROF_GUARD(counter) ProfGuard _pg(counter)
#else
  #define PROF_GUARD(counter) ((void)0)
#endif

#endif // TRIUMV_PROFILE_H

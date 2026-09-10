#ifndef TRIUMV_FROZEN_H
#define TRIUMV_FROZEN_H

// Decide UNA volta sola se questa build congela i parametri di tuning.
//
// 🔴 Perche' un header a se'. La guardia stava dentro defs.h, e nnue_bridge.cpp
// NON include defs.h: in una build di spedizione, dove si passa il solo
// -DTRIUMV_RELEASE, TRIUMV_FROZEN risultava definito in threads.cpp e NON in
// nnue_bridge.cpp. Le dieci costanti della miscela di valutazione sarebbero
// rimaste variabili, con le loro sette divisioni a divisore ignoto, e nessuno se
// ne sarebbe accorto: il bench non cambia, perche' congelare o no da' lo stesso
// albero. Un guadagno che sparisce senza lasciare traccia e' il modo peggiore di
// perderlo. Verificato con #pragma message su entrambe le unita' prima e dopo.
//
// TRIUMV_RELEASE gia' significa "niente tuning", quindi congelare e' la stessa
// decisione presa una volta. -DTRIUMV_FROZEN forza il congelamento anche in una
// build di test (serve per misurarlo isolato); -DTRIUMV_NO_FROZEN lo spegne in
// una build di spedizione.
#if defined(TRIUMV_RELEASE) && !defined(TRIUMV_NO_FROZEN) && !defined(TRIUMV_FROZEN)
    #define TRIUMV_FROZEN 1
#endif

#endif  // TRIUMV_FROZEN_H

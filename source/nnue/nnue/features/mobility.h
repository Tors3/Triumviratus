/*
  Mobility input features (Triumviratus 8.0 study) — "threat su case vuote".
  GPLv3, derived from Stockfish NNUE plumbing (see COPYING).

  PERCHE' ESISTE. FullThreats codifica per costruzione SOLO attacchi su PEZZI
  (numValidTargets): gli attacchi su case vuote — mobilita', controllo, pressione
  sulla zona del re — non sono un input della rete e vanno dedotti indirettamente
  dalle case dei pezzi in HalfKA. Questo blocco li fornisce nella forma
  BUCKETIZZATA, l'unica sostenibile: una feature per pezzo (cavallo/alfiere/torre/
  donna, proprio o avversario) = (casa orientata, bucket di mobilita' 0-3).
  Una forma alla FullThreats (attaccante, da, a-casa-vuota) sarebbe decine di
  migliaia di input con ~100 cambi per mossa: NON e' questa.

  COSTO, ed e' il motivo per cui il blocco esiste PRIMA della rete che lo usa:
  la mobilita' di un pezzo cambia con QUALUNQUE mossa (gli slider vedono tutta la
  scacchiera), quindi il blocco non e' incrementale per eventi come PawnPair.
  Si ricalcola per nodo da uno SNAPSHOT dei bitboard prima/dopo la mossa
  (DirtyMobility, riempito dal bridge come DirtyPawns) e si emette solo la
  differenza: tipicamente 2-6 righe per mossa, piu' ~28 attacks_bb per nodo.
  Pesi ZERO finche' non esiste una rete che li allena: eval BYTE-IDENTICA, il
  bench non si muove, e la differenza di NPS con `MobilityBlock` on/off e' il
  costo puro dell'inferenza. E' la misura che decide se allenarlo.

  INDICE (deve coincidere ESATTAMENTE con il trainer, vedi
  Training_NNUE/Training80/README.md):
    FoldOffset + (own?0:4 + (pt-KNIGHT)) * 256 + (sq ^ orientation) * 4 + bucket
    own          = colore del pezzo == prospettiva
    orientation  = FullThreats::OrientTBL[ksq] ^ (56*perspective)  (come PassedPawns)
    mobilita'    = popcount(attacks(pt, sq, occ) & ~pezziPropri & ~attacchiPedoniNemici)
    bucket       = numero di soglie superate, soglie per tipo in Thresholds
  Le righe vivono in CODA a threatWeights dopo PassedPawns (folded, int8), NON
  passano da FeatPerm (identita') e sono zero-fillate in lettura dei net v3.
*/

#ifndef NNUE_FEATURES_MOBILITY_INCLUDED
#define NNUE_FEATURES_MOBILITY_INCLUDED

#include "../../misc.h"
#include "../../types.h"
#include "../nnue_common.h"
#include "full_threats.h"
#include "passed_pawns.h"
#include "pawn_pair.h"

namespace Triumviratus {
class Position;
}

namespace Triumviratus::Eval::NNUE::Features {

// Interruttore di runtime (UCI `MobilityBlock`, default OFF = byte-identico e costo
// zero). Va impostato prima di `ucinewgame`: gli accumulatori gia' calcolati non
// vengono ricostruiti al cambio.
extern bool g_mobility_on;

class Mobility {
   public:
    // Hash value embedded in the evaluation file — MUST match the trainer ("MOB1").
    static constexpr u32 HashValue = 0x4d4f4231u;

    // {own, enemy} x {N, B, R, Q} x 64 oriented squares x 4 buckets
    static constexpr IndexType Dimensions = 2048;

    // Folded: righe in coda a threatWeights, dopo PassedPawns (== FeatRows).
    static constexpr IndexType FoldOffset =
      FullThreats::Dimensions + PawnPair::Dimensions + PassedPawns::Dimensions;

    // 14 pezzi minori/maggiori in una posizione normale; con le promozioni di piu'.
    // Oltre 32 non si conta (collect() si ferma): e' un limite di capacita', non
    // di correttezza, e non si raggiunge in partite vere.
    static constexpr IndexType MaxActiveDimensions = 32;
    using IndexList                                = FullThreats::IndexList;
    using DiffType                                 = DirtyMobility;

    // Soglie di bucket per tipo (indice = PieceType): bucket = #soglie superate.
    // Massimi teorici: N 8, B 13, R 14, Q 27.
    static constexpr u8 Thresholds[QUEEN + 1][3] = {
      {0, 0, 0}, {0, 0, 0}, {2, 4, 6}, {3, 6, 9}, {4, 7, 10}, {7, 13, 19}};

    static inline int bucket(PieceType pt, int mob) {
        return (mob >= Thresholds[pt][0]) + (mob >= Thresholds[pt][1])
             + (mob >= Thresholds[pt][2]);
    }

    static inline IndexType
    make_index(Color perspective, i8 orientation, Color pc, PieceType pt, Square sq, int b) {
        return FoldOffset + (IndexType(pc != perspective) * 4 + IndexType(pt - KNIGHT)) * 256
             + (IndexType(u8(sq) ^ orientation) << 2) + IndexType(b);
    }

    // Snapshot dei bitboard che bastano a ricalcolare il blocco (bridge, prima e
    // dopo apply_move).
    static void snapshot(const Position& pos, MobSnapshot& s);

    // Tutte le feature attive di UNA prospettiva da uno snapshot. Ritorna il numero
    // scritto in `out` (capacita' MaxActiveDimensions).
    static int collect(Color perspective, i8 orientation, const MobSnapshot& s, IndexType* out);

    // Full refresh
    static void append_active_indices(Color perspective, const Position& pos, IndexList& active);

    // Incrementale: differenza (multiset) fra before e after dello snapshot.
    static void append_changed_indices(Color           perspective,
                                       Square          ksq,
                                       const DiffType& diff,
                                       IndexList&      removed,
                                       IndexList&      added);
};

}  // namespace Triumviratus::Eval::NNUE::Features

#endif  // NNUE_FEATURES_MOBILITY_INCLUDED

/*
  Blocchi da innesto opzionali (Triumviratus, 09/10/2026): un meccanismo unico per i graft che dipendono da pedoni,
  re e donne, scelti dall'analisi del residuo (STATO_PROGETTO §4.1, P1). Nell'ordine canonico (lo stesso del trainer,
  graft_blocks/graft_block.py, e del formato .nnue):
    0 PassedRel   768  passati con le relazioni (imprendibile, collegato, strada libera), features/passed_rel.h
    1 KingFiles    TOLTO dal motore il 10/10/2026 (bit e hash "KFL1" riservati nel formato)
    2 Space        TOLTO dal motore il 10/10/2026 ("SPC1")
    3 LockedPawns  TOLTO dal motore il 10/10/2026 ("LKP1")
    4 Space24      TOLTO dal motore il 10/10/2026 ("SPC2")
    5 KingFilesQ   TOLTO dal motore il 10/10/2026 ("KFQ1")
  Blocchi tolti (10/10/2026) perche' non davano Elo, con le misure: Space24 rete finale -12,2 +- 15,6 su 514 a 12+0.12
  UHO e -7,7 +- 11,5 su 1.088 a 40k nodi fissi, costo PGO +3,2% di cicli/nodo; KingFilesQ rete finale -7,1 +- 10,7 su
  1.128 a 12+0.12 UHO, costo PGO +2,2%; LockedPawns rete finale -19,4 +- 14,5 su 628 a 12+0.12 UHO con la stessa
  profondita' (costo PGO +0,6%); Space e KingFiles costavano +5% e +7% e le loro varianti economiche non rendevano. Il codice del motore e' salvato in _backup/graft_engine_removed_2026-10-10/ (fuori dal
  sorgente, non pubblicato); il trainer li conserva. Una rete con questi blocchi non si carica
  piu' (valid_mask). STATO_PROGETTO, TEST APERTI.
  La definizione esatta e' il riferimento del trainer: PASSED_REL_SPEC in passed_rel.cpp. Il trainer numera le case da a1 = 0: qui i bitboard dei pedoni si girano
  (byteswap) e si usano le stesse formule.

  Voci: (blocco << 12) | dati, uguali per le due prospettive e generate in ordine crescente, cosi' la differenza fra
  prima e dopo una mossa e' un merge lineare (nn_dirty_catch_up) e l'indice per prospettiva lo fa make_index.
    PassedRel    colore << 9 | casa del motore << 3 | stato

  Pesi int8 in coda all'array threatWeights dopo PassedPawns, FUORI dalla permutazione per localita': riga =
  FeatRows + Offset[blocco] + indice. Un blocco assente dalla rete resta a zero e spento (nn_graft_mask).

  Le voci di PassedRel stanno nelle liste della pila (NnStack::graftList / graft, NN_GRAFT_LIST_MASK), aggiornate dal
  recupero (search/06_nndirty.inc, nn_prel_step). Le voci (blocco << 12 | dati) e entries_of restano il RIFERIMENTO:
  vg_check_acc e -DTRIUMV_VERIFY_GRAFT confrontano con esse.

  GPLv3, derived from Stockfish NNUE plumbing (see COPYING).
*/

#ifndef NNUE_FEATURES_PAWN_GRAFTS_INCLUDED
#define NNUE_FEATURES_PAWN_GRAFTS_INCLUDED

#include "../../../nstats.h"  // _wip graft_passedrel3: contatore PREL_DELTA (solo con -DTRIUMV_NSTATS)
#include "../../bitboard.h"
#include "../../misc.h"
#include "../../types.h"
#include "../nnue_common.h"
#include "feat_perm.h"
#include "full_threats.h"
#include "passed_pawns.h"  // R1 (_wip graft_passedrel3): righe v1 emesse dalla diff di PassedRel
#include "passed_rel.h"
#include "passed_state.h"  // PassedState (PassedPawns v3, bit 6, 10/10/2026)

namespace Triumviratus {
class NnBoard;
}

namespace Triumviratus::Eval::NNUE::Features {

class PawnGrafts {
   public:
    enum : unsigned {
        PASSED_REL   = 1,
        KING_FILES   = 2,   // tolti (10/10/2026): bit riservati, rifiutati da valid_mask (resta solo PassedRel)
        SPACE        = 4,
        LOCKED_PAWNS = 8,
        SPACE24      = 16,
        KING_FILES_Q = 32,
        PASSED_STATE = 64,  // PassedPawns v3 (10/10/2026): sostituisce la v1 (V1Off), esclude PassedRel
        ALL          = 127
    };
    // Blocchi dei soli pedoni (strada di PawnPair/PassedPawns) e blocchi delle liste della pila.
    enum : unsigned { LISTS = NN_GRAFT_LIST_MASK, REMOVED = KING_FILES | SPACE | LOCKED_PAWNS | SPACE24 | KING_FILES_Q };
    static_assert(LISTS == (PASSED_REL | PASSED_STATE) && (LISTS | REMOVED) == ALL, "PassedRel e PassedState nelle liste");
    static_assert(NN_GRAFT_PST == PASSED_STATE, "bit di PassedState uguale in nn_dirty.h");
    static constexpr int Blocks = 7;
    // I blocchi tolti non si caricano; PassedRel e PassedState sono due forme dello stesso blocco (voci diverse nelle
    // stesse liste della pila): al piu' uno.
    static constexpr bool valid_mask(unsigned m) {
        return m <= ALL && !(m & REMOVED) && !((m & PASSED_REL) && (m & PASSED_STATE));
    }

    // Hash dei blocchi, uguali al trainer ("PRV2", "KFL1", "SPC1", "LKP1", "SPC2", "KFQ1", "PST1"; usati PRV2 e PST1)
    static constexpr u32 Hash[Blocks] = {0x50525632u, 0x4B464C31u, 0x53504331u, 0x4C4B5031u,
                                         0x53504332u, 0x4B465131u, PassedState::HashValue};
    // PassedRel nel formato a base ("PRB1", passed_rel.h e GRAFT_PASSEDREL_COSTO2 §8): alternativa a Hash[0].
    static constexpr u32 HashPrelBased = PassedRel::HashValueBased;
    // I blocchi tolti non hanno righe (Dim 0).
    static constexpr IndexType Dim[Blocks]    = {768, 0, 0, 0, 0, 0, PassedState::Dimensions};
    static constexpr IndexType Offset[Blocks] = {0, 768, 768, 768, 768, 768, 768};
    static constexpr IndexType Dimensions     = 768 + PassedState::Dimensions;  // 10368
    static_assert(Offset[Blocks - 1] + Dim[Blocks - 1] == Dimensions, "blocchi contigui");

    // Righe dopo FullThreats + PawnPair + PassedPawns (FeatRows), fuori dalla permutazione per localita'.
    static constexpr IndexType FoldOffset = FeatRows;

    // 16 (PassedRel o PassedState: un passato, una voce)
    static constexpr IndexType MaxActiveDimensions = 16;
    static_assert(MaxActiveDimensions <= IndexType(NN_GRAFT_REF_MAX), "buffer del riferimento (tutti i blocchi)");
    static_assert(16 <= NN_GRAFT_MAX, "liste della pila: PassedRel (16)");
    using IndexList = FullThreats::IndexList;

    // Le voci della posizione per i blocchi in mask, in ordine crescente. bb12 = bitboard per pezzo del motore
    // (P N B R Q K p n b r q k; PassedRel usa pedoni e re, PassedState tutti), occ = tutti i pezzi. Restituisce il
    // numero (<= 16).
    static int entries_of(unsigned mask, const unsigned long long* bb12, unsigned long long occ, std::uint16_t* out);

    // Riga (senza FoldOffset) della voce e per la prospettiva con il re in ksq (casa del motore).
    // _wip graftfix (10/10/2026, P2): in linea qui (prima in pawn_grafts.cpp, chiamata fuori linea per ogni voce).
    // Le voci di PassedState (bit 6) hanno un'altra codifica; la maschera dice quale (i due blocchi si escludono).
    static inline IndexType make_index(Color perspective, int ksq, std::uint16_t e) {
        if (nn_graft_mask & PASSED_STATE)
            return Offset[6] + PassedState::make_index(perspective, ksq, e);
        const int p = e & 4095;  // blocco 0: PassedRel
        return Offset[0] + PassedRel::make_index(perspective, ksq, p >> 9, p & 7, (p >> 3) & 63);
    }
    // PassedState con la v1 della rete a zero (V1Off): la v1 non si calcola piu' (incrementali, ibrido, cache "pe").
    static inline bool v1_off() { return (nn_graft_mask & PASSED_STATE) && V1Off; }

    // ---------------------------------------------------------------------------------------------------------------
    // _wip graft_passedrel3 (10/10/2026; docs/audit_8.0/GRAFT_PASSEDREL_COSTO3.md). Righe di PassedRel per prospettiva.
    // g = gruppo = indice della feature PassedPawns v1 del passato ((colore != prospettiva ? 48 : 0) + casa orientata
    // - 8), o = OrientTBL[ksq] ^ (56 * (1 - prospettiva)), come PassedRel::make_index e PassedPawns::make_index.
    //   riga PassedRel = (rel ? 384 : 0) + stato * 48 + casa orientata - 8 = stato * 48 + g + (g >= 48 ? 336 : 0)
    //   riga v1        = feat_row(PassedPawns::FoldOffset + g)   (v1 sta dentro la permutazione per localita')
    // Formato a base (PRB1): la riga dello stato PrelBase[g] e' zero e non si somma. INVARIANTE (R1): senza formato a
    // base PrelBase vale 8 in ogni gruppo (FeatureTransformer::read_parameters, e TRIUMV_PREL_NOFILTER in network.cpp),
    // quindi il filtro "stato != PrelBase[g]" vale per ogni rete e non serve leggere PrelBased nel percorso caldo.
    // ---------------------------------------------------------------------------------------------------------------
#ifdef TRIUMV_PREL_DIAG_NOROWS
    // DIAGNOSI (R0, solo misura): le righe di PassedRel non si sommano (le v1 si'). Valutazione ESATTA solo con una
    // rete a pesi zero nel blocco (consilium_prel0.nnue): stesso albero, e xperf misura il costo del blocco SENZA le
    // sue righe. Con una rete vera la valutazione e' sbagliata: mai per partite.
    static constexpr bool PrelRowsOn = false;
#else
    static constexpr bool PrelRowsOn = true;
#endif
    static inline int prel_group(int perspective, int o, std::uint16_t v) {
        return (int(v >> 9) != perspective ? 48 : 0) + ((int(v >> 3) & 63) ^ o) - 8;
    }
    static inline IndexType prel_row(int g, unsigned st) {
        return FoldOffset + IndexType(int(st) * 48 + g + (g >= 48 ? 336 : 0));
    }
    static inline IndexType v1_row(int g) { return feat_row(PassedPawns::FoldOffset + IndexType(g)); }

    // R1 (_wip graft_passedrel3): con la sola PassedRel nelle liste della pila le righe di PassedPawns v1 escono dalla
    // diff del blocco, e l'accumulatore NON chiama PassedPawns::append_changed_indices. Le due feature vedono gli
    // stessi passati (PASSED_REL_SPEC: passato = PassedPawns v1): una voce (colore, casa) tolta senza una voce
    // (colore, casa) aggiunta e' un passato che sparisce (riga v1 tolta), e viceversa; la stessa (colore, casa) da
    // tutte e due le parti e' un passato che cambia solo stato (nessuna riga v1). Il recupero scrive la diff a ogni
    // cambio dell'insieme dei passati per colore (nn_prel_step: insieme diverso o casa di un passato toccata ->
    // nn_prel_rebuild, merge non vuoto), quindi diff vuota = v1 invariata. Prima, a OGNI evento di pedone, v1 rifaceva
    // quattro passers() per prospettiva (otto nel percorso a due prospettive, fuori linea) che il recupero aveva gia'
    // fatto due volte. Stesse righe, solo in un altro ordine nella lista: somme intere, valutazione identica.
    // -DTRIUMV_PREL_NO_V1FUSE torna al percorso di prima (A/B per xperf).
    static inline bool v1_fused() {
#ifndef TRIUMV_PREL_NO_V1FUSE
        return (nn_graft_mask & LISTS) == PASSED_REL;
#else
        return false;
#endif
    }
    // La v1 non si calcola: righe gia' nella diff di PassedRel (R1) o v1 spenta da PassedState (V1Off).
    static inline bool v1_skip() { return v1_fused() || v1_off(); }
    // Voce dall'altra parte della diff con la stessa (colore, casa): 1 se c'e', e il suo stato in `st`. Le liste sono
    // di poche voci (di solito 1-2): ciclo corto senza salti nel corpo.
    static inline unsigned prel_match(const std::uint16_t* e, int j0, int j1, std::uint16_t v, unsigned& st) {
        unsigned same = 0, s = 0;
        for (int j = j0; j < j1; j++)
        {
            const unsigned eq = unsigned((e[j] ^ v) < 8u);
            same |= eq;
            s |= (0u - eq) & (e[j] & 7u);
        }
        st = s;
        return same;
    }

#ifdef TRIUMV_PREL_DELTA
    // R3 (_wip graft_passedrel3, SPENTO di default: aggiunge 1152 righe alle tabelle dei pesi). Righe "delta" per i
    // cambi di UN bit di stato fra due stati entrambi fuori base: D[g][spigolo] = W[g][basso | bit] - W[g][basso], con
    // 12 spigoli per gruppo (3 bit x 4 valori degli altri due), costruite al caricamento della rete
    // (FeatureTransformer::build_prel_delta) e usate solo se ogni elemento entra nell'int8 (PrelDeltaOk). Un passato
    // che cambia stato X -> Y costa una riga invece di due: acc - W[X] + W[Y] = acc + D (somme intere, identico).
    static constexpr IndexType PrelDeltaBase = FoldOffset + Dimensions;
    static inline unsigned prel_edge(unsigned lo, unsigned k) {
        return k * 4 + ((lo & ((1u << k) - 1)) | ((lo >> (k + 1)) << k));
    }
    // X -> Y del gruppo g: true se si usa la riga delta (in row; toAdd = va fra le aggiunte, altrimenti fra le tolte).
    static inline bool prel_delta(int g, unsigned x, unsigned y, IndexType& row, bool& toAdd) {
        const unsigned d = x ^ y;
        if (!d || (d & (d - 1)) || x == PrelBase[g] || y == PrelBase[g])
            return false;
        const unsigned edge = prel_edge(x & y, d == 1u ? 0u : d == 2u ? 1u : 2u);
        if (!((PrelDeltaOk[g] >> edge) & 1u))
            return false;
        row   = PrelDeltaBase + IndexType(g * 12 + int(edge));
        toAdd = (y & d) != 0;
        return true;
    }
#endif

    // R1: diff di PassedRel per una prospettiva, con le righe v1 e il filtro del formato a base senza salti (scrittura
    // sempre, contatore che avanza di 0 o 1). Le liste dei threat hanno MaxActiveDimensions posti, ben oltre le voci di
    // un aggiornamento incrementale: la scrittura oltre il conteggio cade in un posto libero.
    static inline void prel_changed_fused(int perspective, int o, const std::uint16_t* e, int nr, int n,
                                          IndexList& removed, IndexList& added) {
        IndexType* pr = removed.data();
        IndexType* pa = added.data();
        int        kr = removed.ssize(), ka = added.ssize();
        for (int i = 0; i < nr; i++)
        {
            const std::uint16_t v = e[i];
            unsigned            y;
            const unsigned      same = prel_match(e, nr, n, v, y);
            const int           g    = prel_group(perspective, o, v);
            const unsigned      x    = v & 7u;
            pr[kr] = v1_row(g);
            kr += int(!same);  // il passato sparisce
            bool on = PrelRowsOn && x != PrelBase[g];
#ifdef TRIUMV_PREL_DELTA
            IndexType dr;
            bool      da;
            if (same && prel_delta(g, x, y, dr, da))
                on = false;  // la riga delta la scrive il ciclo delle aggiunte
#endif
            pr[kr] = prel_row(g, x);
            kr += int(on);
            if (!PrelBased)
                PREL_HIST(g, x);
        }
        for (int i = nr; i < n; i++)
        {
            const std::uint16_t v = e[i];
            unsigned            x;
            const unsigned      same = prel_match(e, 0, nr, v, x);
            const int           g    = prel_group(perspective, o, v);
            const unsigned      y    = v & 7u;
            pa[ka] = v1_row(g);
            ka += int(!same);  // il passato compare
            bool on = PrelRowsOn && y != PrelBase[g];
#ifdef TRIUMV_PREL_DELTA
            IndexType dr;
            bool      da;
            if (same && prel_delta(g, x, y, dr, da))
            {
                on = false;
                if (PrelRowsOn)
                {
                    NSTAT(PREL_DELTA);
                    if (da)
                        pa[ka++] = dr;
                    else
                        pr[kr++] = dr;
                }
            }
#endif
            pa[ka] = prel_row(g, y);
            ka += int(on);
            if (!PrelBased)
                PREL_HIST(g, y);
        }
        removed.set_size(usize(kr));
        added.set_size(usize(ka));
    }

    // R1: le due prospettive in una passata (oW, oB = orientazioni del bianco e del nero).
    static inline void prel_changed_fused_both(int oW, int oB, const std::uint16_t* e, int nr, int n, IndexList& remW,
                                               IndexList& addW, IndexList& remB, IndexList& addB) {
        IndexType* prW = remW.data();
        IndexType* paW = addW.data();
        IndexType* prB = remB.data();
        IndexType* paB = addB.data();
        int        krW = remW.ssize(), kaW = addW.ssize(), krB = remB.ssize(), kaB = addB.ssize();
        for (int i = 0; i < nr; i++)
        {
            const std::uint16_t v = e[i];
            unsigned            y;
            const unsigned      same = prel_match(e, nr, n, v, y);
            const int           gW = prel_group(0, oW, v), gB = prel_group(1, oB, v);
            const unsigned      x  = v & 7u;
            prW[krW] = v1_row(gW);
            prB[krB] = v1_row(gB);
            krW += int(!same);
            krB += int(!same);
            bool onW = PrelRowsOn && x != PrelBase[gW], onB = PrelRowsOn && x != PrelBase[gB];
#ifdef TRIUMV_PREL_DELTA
            IndexType dr;
            bool      da;
            if (same && prel_delta(gW, x, y, dr, da))
                onW = false;
            if (same && prel_delta(gB, x, y, dr, da))
                onB = false;
#endif
            prW[krW] = prel_row(gW, x);
            krW += int(onW);
            prB[krB] = prel_row(gB, x);
            krB += int(onB);
            if (!PrelBased)
            {
                PREL_HIST(gW, x);
                PREL_HIST(gB, x);
            }
        }
        for (int i = nr; i < n; i++)
        {
            const std::uint16_t v = e[i];
            unsigned            x;
            const unsigned      same = prel_match(e, 0, nr, v, x);
            const int           gW = prel_group(0, oW, v), gB = prel_group(1, oB, v);
            const unsigned      y  = v & 7u;
            paW[kaW] = v1_row(gW);
            paB[kaB] = v1_row(gB);
            kaW += int(!same);
            kaB += int(!same);
            bool onW = PrelRowsOn && y != PrelBase[gW], onB = PrelRowsOn && y != PrelBase[gB];
#ifdef TRIUMV_PREL_DELTA
            IndexType dr;
            bool      da;
            if (same && prel_delta(gW, x, y, dr, da))
            {
                onW = false;
                if (PrelRowsOn)
                {
                    NSTAT(PREL_DELTA);
                    if (da)
                        paW[kaW++] = dr;
                    else
                        prW[krW++] = dr;
                }
            }
            if (same && prel_delta(gB, x, y, dr, da))
            {
                onB = false;
                if (PrelRowsOn)
                {
                    NSTAT(PREL_DELTA);
                    if (da)
                        paB[kaB++] = dr;
                    else
                        prB[krB++] = dr;
                }
            }
#endif
            paW[kaW] = prel_row(gW, y);
            kaW += int(onW);
            paB[kaB] = prel_row(gB, y);
            kaB += int(onB);
            if (!PrelBased)
            {
                PREL_HIST(gW, y);
                PREL_HIST(gB, y);
            }
        }
        remW.set_size(usize(krW));
        addW.set_size(usize(kaW));
        remB.set_size(usize(krB));
        addB.set_size(usize(kaB));
    }

#ifdef TRIUMV_VERIFY_GRAFT
    // R1: le righe v1 (indici < FeatRows) scritte dalla diff di PassedRel in rem/add confrontate, come insiemi, con
    // quelle di PassedPawns::append_changed_indices sulla stessa snapshot dei pedoni (pawn_grafts.cpp). Abort al primo
    // disaccordo. rem/add = le righe che l'aggiornamento ha messo nelle liste che v1 avrebbe chiamato removed/added.
    static void verify_v1_fused(Color             perspective,
                                int               ksq,
                                const DirtyPawns& d,
                                const IndexType*  rem,
                                int               nr,
                                const IndexType*  add,
                                int               na);
#endif

    // RIFERIMENTO: tutti i blocchi di nn_graft_mask dalla posizione (entries_of + make_index). Lo usa vg_check_acc.
    static void append_active_indices(Color perspective, const NnBoard& pos, IndexList& active);
    // Refresh dello stato st, blocco delle liste (PassedRel): le voci gia' calcolate dal recupero
    // (NnStack::graftList) quando ci sono.
    static void append_active_indices(Color perspective, const NnBoard& pos, const NnState& st, IndexList& active);
    // R2 (_wip graft_passedrel3): la stessa cosa in linea per la sola PassedRel nelle liste, con la lista gia' nella
    // pila (graftSrc != 255): niente chiamata fuori linea, filtro del formato a base senza salti, nessun lavoro se la
    // lista e' vuota (mediogioco: nessun passato nel 57% delle posizioni del bench SF). Altrimenti la versione qui
    // sopra. ksq = casa del re della prospettiva (il chiamante la ha gia'). -DTRIUMV_PREL_NO_REFINLINE: come prima.
    static inline void
    append_active_indices(Color perspective, int ksq, const NnBoard& pos, const NnState& st, IndexList& active) {
#ifndef TRIUMV_PREL_NO_REFINLINE
        const NnStack& S   = nn_stack_of(st);
        const int      src = S.graftSrc[st.idx];
        if (src != 255 && (nn_graft_mask & LISTS) == PASSED_REL)
        {
            const int            n = S.graftN[src];
            const std::uint16_t* l = S.graftList[src];
            const int            o = FullThreats::OrientTBL[ksq] ^ (56 * (1 - int(perspective)));
            IndexType*           p = active.data();
            int                  k = active.ssize();
            for (int i = 0; i < n; i++)
            {
                const int      g  = prel_group(int(perspective), o, l[i]);
                const unsigned st7 = l[i] & 7u;
                p[k] = prel_row(g, st7);
                k += int(PrelRowsOn && st7 != PrelBase[g]);
                if (!PrelBased)
                    PREL_HIST(g, st7);
            }
            active.set_size(usize(k));
            return;
        }
#else
        (void) ksq;
#endif
        append_active_indices(perspective, pos, st, active);
    }

    // O1 (_wip pst_opt, 10/10/2026): le voci di PassedState dello stato st per il refresh (cache "pe" estesa,
    // update_accumulator_refresh_cache): quelle del recupero (NnStack::graftList) se ci sono, in l senza copia;
    // altrimenti da capo nella posizione, in buf (l = buf). Restituisce il numero (<= 16).
    static inline int
    pst_entries(const NnBoard& pos, const NnState& st, std::uint16_t* buf, const std::uint16_t*& l) {
        const NnStack& S   = nn_stack_of(st);
        const int      src = S.graftSrc[st.idx];
        if (src != 255)
        {
            l = S.graftList[src];
            return S.graftN[src];
        }
        l = buf;
        return pst_entries_scratch(pos, buf);
    }
    static int pst_entries_scratch(const NnBoard& pos, std::uint16_t* buf);

    // (LockedPawns, l'ultimo blocco sulla strada dei pedoni con Space e Space24, tolto il 10/10/2026: niente piu'
    // append_pawn_*, la cache "pe" del refresh torna a tenere solo PawnPair e PassedPawns.)

    // Diff dello stato st (_wip graftfix, P2): conteggi in NnState (graftRem, graftAdd), voci in NnStack::graft (tolte,
    // poi aggiunte). Il chiamante entra solo se graftRem | graftAdd != 0. Nelle liste c'e' solo PassedRel: l'indice si
    // calcola senza lo switch sul blocco (stessa formula di PassedRel::make_index).
    static inline void append_changed_indices(Color          perspective,
                                              int            ksq,
                                              const NnState& st,
                                              IndexList&     removed,
                                              IndexList&     added) {
        const std::uint16_t* e  = nn_graft_of(st).e;
        const int            nr = st.graftRem, n = nr + st.graftAdd;
        if (nn_graft_mask & PASSED_STATE)  // PassedState (10/10/2026): una riga per voce, nessuna riga v1
        {
            constexpr IndexType base = FoldOffset + Offset[6];
            for (int i = 0; i < nr; i++)
                removed.push_back(base + PassedState::make_index(perspective, ksq, e[i]));
            for (int i = nr; i < n; i++)
                added.push_back(base + PassedState::make_index(perspective, ksq, e[i]));
            return;
        }
        {  // PassedRel (KingFiles e KingFilesQ tolti il 10/10/2026)
            const int o = FullThreats::OrientTBL[ksq] ^ (56 * (1 - int(perspective)));
#ifndef TRIUMV_PREL_NO_V1FUSE
            // R1 (_wip graft_passedrel3): righe v1 dalla stessa diff, filtro del formato a base senza salti.
            prel_changed_fused(int(perspective), o, e, nr, n, removed, added);
            return;
#endif
            if (PrelBased)
            {
                // Formato a base (§8 di GRAFT_PASSEDREL_COSTO2): la riga dello stato base del gruppo e' zero, si salta.
                const auto put = [&](std::uint16_t v, IndexList& l) {
                    const bool rel = (v >> 9) != int(perspective);
                    const int  g   = (rel ? 48 : 0) + (((v >> 3) & 63) ^ o) - 8;  // indice della feature v1
                    if (int(v & 7) != PrelBase[g])
                        l.push_back(FoldOffset + (v & 7) * 48 + g + (rel ? 336 : 0));
                };
                for (int i = 0; i < nr; i++)
                    put(e[i], removed);
                for (int i = nr; i < n; i++)
                    put(e[i], added);
                return;
            }
            const auto row = [&](std::uint16_t v) -> IndexType {
                PREL_HIST(((v >> 9) != int(perspective) ? 48 : 0) + (((v >> 3) & 63) ^ o) - 8, v & 7);
                return FoldOffset + ((v >> 9) != int(perspective) ? 384 : 0) + (v & 7) * 48 + (((v >> 3) & 63) ^ o) - 8;
            };
            for (int i = 0; i < nr; i++)
                removed.push_back(row(e[i]));
            for (int i = nr; i < n; i++)
                added.push_back(row(e[i]));
            return;
        }
    }

    // Le due prospettive in una passata (update_accumulator_incremental_both): ogni voce si legge una volta.
    static inline void append_changed_indices_both(int            ksqW,
                                                   int            ksqB,
                                                   const NnState& st,
                                                   IndexList&     remW,
                                                   IndexList&     addW,
                                                   IndexList&     remB,
                                                   IndexList&     addB) {
        const std::uint16_t* e  = nn_graft_of(st).e;
        const int            nr = st.graftRem, n = nr + st.graftAdd;
        if (nn_graft_mask & PASSED_STATE)  // PassedState (10/10/2026): una riga per voce e prospettiva, nessuna v1
        {
            constexpr IndexType base = FoldOffset + Offset[6];
            for (int i = 0; i < nr; i++)
            {
                remW.push_back(base + PassedState::make_index(WHITE, ksqW, e[i]));
                remB.push_back(base + PassedState::make_index(BLACK, ksqB, e[i]));
            }
            for (int i = nr; i < n; i++)
            {
                addW.push_back(base + PassedState::make_index(WHITE, ksqW, e[i]));
                addB.push_back(base + PassedState::make_index(BLACK, ksqB, e[i]));
            }
            return;
        }
        {  // PassedRel (KingFiles e KingFilesQ tolti il 10/10/2026)
            // PassedRel::make_index: orientazione = OrientTBL[ksq] ^ (56 * (1 - prospettiva)); bianco 0, nero 1.
            const int  oW  = FullThreats::OrientTBL[ksqW] ^ 56, oB = FullThreats::OrientTBL[ksqB];
#ifndef TRIUMV_PREL_NO_V1FUSE
            // R1 (_wip graft_passedrel3): righe v1 dalla stessa diff, una decodifica per le due prospettive.
            prel_changed_fused_both(oW, oB, e, nr, n, remW, addW, remB, addB);
            return;
#endif
            if (PrelBased)
            {
                // Formato a base: per ogni prospettiva il gruppo (colore relativo, casa orientata) ha il suo stato base.
                const auto two = [&](std::uint16_t v, IndexList& w, IndexList& b) {
                    const int       c = v >> 9, s = (v >> 3) & 63, st = v & 7;
                    const int       gW = (c != int(WHITE) ? 48 : 0) + (s ^ oW) - 8;
                    const int       gB = (c != int(BLACK) ? 48 : 0) + (s ^ oB) - 8;
                    // riga = (rel ? 384 : 0) + st * 48 + casa orientata - 8 = st * 48 + g + (rel ? 336 : 0)
                    const IndexType base = FoldOffset + st * 48;
                    if (st != PrelBase[gW])
                        w.push_back(base + gW + (c != int(WHITE) ? 336 : 0));
                    if (st != PrelBase[gB])
                        b.push_back(base + gB + (c != int(BLACK) ? 336 : 0));
                };
                for (int i = 0; i < nr; i++)
                    two(e[i], remW, remB);
                for (int i = nr; i < n; i++)
                    two(e[i], addW, addB);
                return;
            }
            const auto two = [&](std::uint16_t v, IndexList& w, IndexList& b) {
                const int       c = v >> 9, s = (v >> 3) & 63;
                const IndexType base = FoldOffset + (v & 7) * 48 - 8;
                PREL_HIST((c != int(WHITE) ? 48 : 0) + (s ^ oW) - 8, v & 7);
                PREL_HIST((c != int(BLACK) ? 48 : 0) + (s ^ oB) - 8, v & 7);
                w.push_back(base + (c != int(WHITE) ? 384 : 0) + (s ^ oW));
                b.push_back(base + (c != int(BLACK) ? 384 : 0) + (s ^ oB));
            };
            for (int i = 0; i < nr; i++)
                two(e[i], remW, remB);
            for (int i = nr; i < n; i++)
                two(e[i], addW, addB);
            return;
        }
    }
};

}  // namespace Triumviratus::Eval::NNUE::Features

#endif  // #ifndef NNUE_FEATURES_PAWN_GRAFTS_INCLUDED

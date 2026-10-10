#pragma once
#ifndef SF_BRIDGE_H
#define SF_BRIDGE_H

// Thin bridge to the TRANN1 NNUE (in nnue/) — Triumviratus Rubicon Alea NNUE 1:
// FOUR input blocks (FullThreats + HalfKAv2_hm + PawnPair + PassedPawns), L1=1024,
// L2/L3, 8 LayerStacks. The evaluation machinery is derived from Stockfish-master's
// SFNNv13 (GPLv3, attribution in COPYING/README); the last two blocks are ours, so
// the net format is NOT SFNNv13 and an SFNNv13 net cannot be loaded (the reader
// accepts only the TRANN1 4-block hash and the 3-block v2 hash, which zero-fills
// the PassedPawns segment). Keeps all Stockfish headers/types/macros out of the
// rest of the engine so there is no clash with Triumviratus's own globals.

#include "nn_dirty.h"   // le dirty di ogni mossa (tipi semplici, condivisi con la rete)

// Initialize the shared substrate tables: bitboards + slider-magic attacks.
// (Attacks::init() is SEPARATE from Bitboards::init() in the master.)
void nn_init_tables(void);

// Nome del net di default (macro EvalFileDefaultName, definita in nnue/evaluate.h).
// Esposto qui perche' il resto del motore NON include gli header Stockfish: un solo
// punto di verita' per il nome, niente stringhe duplicate da tenere in sync.
const char* nn_default_net_name(void);
// Rete condivisa fra processi (-DTRIUMV_SHARED_NET): esito della attach, "" nelle altre build.
const char* nn_net_memory_status(void);

// Load the TRANN1 network from a file path. Returns 1 on success (file opened +
// arch-hash verified), 0 otherwise. Must be called once at startup before any
// nn_pos_create() (the per-handle accumulator caches are built from the net).
int nn_load_net(const char* net_path);

// Reload the network at runtime (UCI option "EvalFile"). Safe to call when no
// search is running. Returns 1 on success, 0 if the path could not be loaded.
int nn_reload_big(const char* net_path);

// Toggle the per-thread accumulator refresh cache ("finny tables"). No-op in the
// M2 full-refresh path (the master always uses the cache to accelerate refresh;
// the eval value is identical regardless). Kept for API stability.
void nn_set_finny(int on);

// DIAGNOSTIC ("accstats" UCI command). No-op stub in M2 (the v13 incremental
// refresh counters arrive with M3).
void nn_acc_stats(void);

// M3 incremental eval. on != 0 uses the master AccumulatorStack chain (DirtyPiece +
// DirtyThreats per ply) instead of a full refresh per node. Default OFF (the
// validated M2 full-refresh) until the verify oracle is clean. The handle MUST be
// re-set (nn_pos_set) after toggling so its board/accumulator chain is consistent.
void nn_set_incremental(int on);

// DEBUG: cross-check incremental == full-refresh at every leaf eval (prints the
// first mismatches). Halves NPS; single-thread recommended. Default OFF.
void nn_set_verify(int on);

// Eval output scale in PERCENT (default 100 = x1.0). The SFNNv13 cp formula lands on
// a different scale than the engine's SPSA-tuned (for SFNNv10) search margins expect;
// this re-aligns the two. UCI option "EvalScale". Diagnostic sweep at fixed depth.
void nn_set_eval_scale(int pct);          // scrive TUTTI gli 8 bucket (comportamento storico)
// EvalScale per bucket di output (15/08/2026). bucket = (pezzi - 1) / 4, 0..7, come
// network.cpp:170. Dal BAKE MOE1 (30/09/2026) i default sono per fascia (rete MoE-1024), non piu' tutti 60.
void nn_set_eval_scale_bucket(int bucket, int pct);
int  nn_get_eval_scale_bucket(int bucket);   // valore corrente (all'avvio = default), per la riga UCI

// --- Costanti del blend dell'eval (15/08/2026) -------------------------------
// Le sette costanti che trasformano le due uscite della rete (psqt, positional) in
// una valutazione: pesi del blend, smorzamento per disaccordo, scala col materiale,
// termini dell'optimism. Sono di Stockfish, ereditate col wrapper, e MAI tarate su
// questa rete. ⚠️ NON scalano l'eval — decidono COSA DICE, che e' la differenza con
// EvalScale/EvalScaleB (scala per bucket, chiusa a -9,82 il 15/08).
// L'enumerazione serve a uci_mt.cpp per dichiarare le opzioni senza duplicare i nomi.
int         nn_eval_const_count(void);
const char* nn_eval_const_name(int i);
int         nn_eval_const_get(int i);
int         nn_eval_const_lo(int i);
int         nn_eval_const_hi(int i);
int         nn_set_eval_const(const char* name, int value);   // 1 se il nome esiste
#ifdef TRIUMV_FROZEN
void        nn_frozen_check(void);   // vedi COSTANTI DELLA MISCELA CONGELATE
#endif
int  nn_get_eval_scale(void);   // current EvalScale %% (per normalizzare 'score cp' in stampa)
int  nn_last_opt_base(void* handle);    // EvalCacheOptSplit: eval con optimism=0 (pre-rule50, post-scale)
int  nn_last_opt_coeff(void* handle);   // EvalCacheOptSplit: coefficiente in MILLESIMI dell'optimism
int  nn_last_unadjusted(void* handle);  // unadjusted (pre-rule50/scale) dell'ultima nn_scale (thread-local)
// Ricostruisce l'eval finale dall'unadjusted. `bucket` = (pezzi - 1) / 4 della posizione
// CORRENTE: serve a scegliere la scala per bucket, e qui non c'e' la Position.
int  nn_finalize(int unadjusted, int rule50, int bucket);

// Valutazione da zero (refresh completo) della scacchiera globale, per il comando "eval": bb[12] nell'ordine
// P,N,B,R,Q,K,p,n,b,r,q,k, occ[3] = bianco/nero/tutti, case a8 = 0 (la numerazione del motore, l'unica).
// Returns the evaluation (stm-relative, internal units == the engine's eval scale).
int nn_eval(int side_white, const unsigned long long* bb, const unsigned long long* occ, int rule50,
            int* raw_out = nullptr);

// ---------------------------------------------------------------------------
// Handle per thread (07/10/2026, scacchiera unica v2): catena degli accumulatori e finny table. La rete non tiene una
// scacchiera sua: legge quella del motore in nn_pos_eval. Le dirty di ogni mossa le scrive la make del motore sulla
// pila dell'handle (nn_pos_stack, tipi in nn_dirty.h): uno stato per mossa fatta, tolto alla unmake; le mosse nulle
// non aggiungono stati.
// ---------------------------------------------------------------------------

// Create / destroy a per-thread incremental position handle (opaque).
void* nn_pos_create(void);
void  nn_pos_destroy(void* handle);
void  nn_pos_set_optimism(void* handle, int w, int b);   // OptPerThread

// Riparte dalla radice: la pila torna a un solo stato e l'accumulatore della radice si calcola alla prima valutazione.
void  nn_pos_set(void* handle);

// La pila delle dirty dell'handle, che la make e la unmake del motore scrivono direttamente.
NnStack* nn_pos_stack(void* handle);

// Evaluate the current position (centipawns / internal units, stm-relative).
// La rete legge la NOSTRA scacchiera: bb[12] nell'ordine P,N,B,R,Q,K,p,n,b,r,q,k, occ[3] = bianco/nero/tutti,
// mailbox[64] con -1 = vuota, tutto con a8 = 0. side_white e rule50 sono quelli della ricerca. Le dirty delle mosse
// fino a questa posizione devono essere complete (nn_dirty_catch_up del motore, prima).
int   nn_pos_eval(void* handle, const unsigned long long* bb, const unsigned long long* occ, const int* mailbox,
                  int side_white, int rule50);

// Controllo nnueverify: valutazioni confrontate e valutazioni diverse dall'avvio.
unsigned long long nn_verify_count(void);
unsigned long long nn_verify_bad(void);

#endif // SF_BRIDGE_H

# Training 7.0 — SFNNv15 + PassedPawns

Workspace del **primo full-training** del progetto (la 7.0). Non e' un graft: si allena tutta la rete,
non solo un blocco. Contesto e decisioni: `PROSPETTIVE_RETE_7.0.md` §10-18 nella radice del repo.

## Cos'e' cambiato rispetto alla 6.0

| | 6.0 (`rubicon-alea`) | 7.0 (questo) |
|---|---|---|
| architettura | SFNNv13 | **SFNNv15** ("Better Skip Architecture", merged in SF il 04/07/2026) |
| feature set | Full_Threats + HalfKAv2_hm + PawnPair + PassedPawns | **le stesse quattro** |
| pawn-pair | nostro `PawnPair` (4560) | **`PP_3Wide` upstream — e' la STESSA feature**, vedi sotto |
| training | **graft** (`--lr 0`, solo il blocco nuovo impara) | **full-training** 600+800 epoche |
| trainer | fork Lightning-based (`TrainingAleaV2_Grafting/`) | upstream **senza Lightning** (`SimpleTrainer`) |

## Trainer: upstream pinnato

```
nnue-pytorch/   = github.com/official-stockfish/nnue-pytorch
                  PIN: 9f72946529c4187d3679014036cd22c3be419716  (2026-07-26)
                  "Fix global step index calculation in DDP training mode"
```

Perche' upstream e non il nostro fork: **~+28% di throughput** (PR #508: kernel FT fuso, FT-backward
tilato, RangerLite fuso +14,7%, DDP tuning) piu' SFNNv15 piu' il codice che SF manterra'. Su un
preventivo di 14-19 giorni di GPU il 28% sono ~4-5 giorni e ~$250-300.

⚠️ **Upstream si muove veloce** (3 commit nell'ultima settimana di luglio): il pin sopra e' la
base di riferimento. Un `git pull` va fatto con intenzione, non per abitudine, e ri-verificando
`cross_check_eval.py`.

## Cosa dobbiamo aggiungere NOI

**Solo `PassedPawns` (96 input).** Il resto c'e' gia': `DEFAULT_FEATURES` upstream e'
`Full_Threats+PP_3Wide+HalfKAv2_hm^`.

🟢 **Il nostro `PawnPair` e il `PP_3Wide` di SF sono la stessa feature** — verificato il 27/07:
coppie di pedoni su stessa colonna o adiacenti, `NUM_INPUTS = 4560` in entrambi (con le stesse 3024
righe mai attivabili), `EXPORT_WEIGHT_DTYPE = torch.int8`. Il filtro di banda sta in
`nnue/nnue/features/pawn_pair.cpp:56-58` nel motore e in `training_data_loader.cpp:368` nel loader.
**Non c'e' niente da portare per il pawn-pair.**

### 1. Lato Python
`model/modules/features/passed_pawns.py`, sottoclasse di `InputFeature`
(`model/modules/features/input_feature.py`). Da implementare: `merged_weight`, `coalesce`,
`zero_virtual_weights`, `init_weights`, `get_export_weights`, `load_export_weights`.
Costanti: `NUM_INPUTS = 96`, `NUM_REAL_FEATURES = 96`, `MAX_ACTIVE_FEATURES` (8 o 16, da tarare),
`EXPORT_WEIGHT_DTYPE = torch.int8`, `HASH` e `NAME` da concordare col motore.
Poi una riga in `_FEATURE_COMPONENTS` di `model/modules/features/__init__.py` e il set
`Full_Threats+PP_3Wide+HalfKAv2_hm^+PassedPawns`.

### 2. Lato loader C++
`data_loader/cpp/training_data_loader.cpp` — l'indicizzazione PassedPawns **esiste gia'** nel nostro
fork (`Training_NNUE/TrainingAleaV2_Grafting/nnue-pytorch/data_loader/cpp/training_data_loader.cpp`,
vedi il commento a riga ~398 "enemy pawn on the same or adjacent files ahead AND no own pawn
directly..."). E' un trapianto, non un progetto.

### 3. Lato motore (`Triumviratus_7/`)
- **Skip SFNNv15** in `nnue/nnue/nnue_architecture.h` (oggi implementa v13): skip L2→output realizzato
  concatenando L2 con L3 per il layer finale; skip su L1 che prende gli ultimi due valori del buffer,
  uno x1.0 e uno x2.0 (sfrutta il padding SIMD che si paga comunque ⇒ costo ~nullo).
- PawnPair e PassedPawns nel motore **ci sono gia'**.

## 🔴 Il gate che rende sicura tutta l'operazione

`cross_check_eval.py` (upstream, nella radice del clone) prende `--engine`, `--net`, `--data` e
confronta la valutazione **lato trainer** con quella **lato motore** sulle stesse posizioni.

Va fatto girare **prima di lanciare la fase 1**, anche su una rete random. Chiude in un colpo tutta la
classe di disallineamenti: ordine degli indici, layout dei pesi, quantizzazione. Se il motore e il
trainer non concordano sull'ordine degli indici del pawn-pair, **la rete esce spazzatura e non se ne
accorge nessuno** finche' non si guarda l'eval — questo e' lo strumento che lo previene.

## Da installare (non incluso: sono GB)

```
pip install -r nnue-pytorch/requirements.txt
# + torch con CUDA >= 12.8 se si vuole girare su Blackwell (sm_120).
# ATTENZIONE: il torch locale e' 2.6.0+cu124 -> NON supporta sm_120.
# Sulla VM: immagine pytorch-2-9-cu129 (CUDA 12.9), va bene.
# data loader C++: nnue-pytorch/compile_data_loader.bat (Windows) / .sh (Linux)
```

## Dataset: NON qui

Restano fuori da questa cartella (sono centinaia di GB). Elenco verificato, dimensioni reali e comandi
di download: `PROSPETTIVE_RETE_7.0.md` §10.1, §12.2, §13.4.
- fase 1: `vondele/master-binpacks_relabel` (129 GB, BT4-relabeled, DFRC incluso)
- fase 2: `vondele/linrock_relabel_1` + `from_kaggle_1/2_relabel` + `jshriver/t91-binpacks` (solo 2026)

## Nome della rete

`rubicon-alea` era la linea del graft. La 7.0 cambia architettura ⇒ **linea nuova, nome nuovo**.
Candidati sul filo romano: **`legio-septima`** (la Settima Legione di Cesare, e codifica il 7),
`pharsalus`, `cursus-honorum`. **Da decidere** — poi rinominare questa cartella di conseguenza.

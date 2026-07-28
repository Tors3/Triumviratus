# Training NNUE ad alta velocità — cosa muove davvero l'ago

> Scoperte del 28/07/2026, tutte **misurate** su 4× RTX A4000 durante la fase 1 della rete
> `legio-septima`. Risultato complessivo: **130k → 529k pos/s, 4,1×**, che sulla fase 1 sono
> **107 ore → ~23 ore**.
>
> Nessuna delle due vittorie grosse era nel codice del modello: erano una libreria rotta in
> silenzio e un parametro dimensionato male.

---

## 1. Il riepilogo, in ordine di resa

| intervento | effetto | rischio |
|---|---|---|
| **CuPy riparato** (pin `<14`) | 130k → 177k (**+36%**) | nessuno |
| **batch 32768 → 131072** | 177k → 529k (**+199%**) | serve riscalare l'LR |
| worker loader 3 → 6 (`--threads 1`) | da verificare, atteso pochi % | nessuno |
| `BACKWARD_TILE_SIZE` | **zero** | — |
| ridurre gli 8 LayerStack | **zero** | — |

---

## 2. 🔴 CuPy rotto spegne i kernel veloci — in silenzio

**Il problema.** `cupy-cuda12x` 14.x è incompatibile con i pacchetti NVIDIA che torch si
porta dietro:

```
ImportError: cannot import name find_nvidia_binary_utility
```

**Perché conta.** Da quell'import dipendono **due** cose, e nessuna delle due fallisce
rumorosamente:

- i **kernel fusi del feature transformer** (`fused_ft_functions.py`, `_HAS_CUPY_KERNELS`).
  Senza, si ricade su `aten::_embedding_bag` di PyTorch — e il FT è il **78% del tempo GPU**.
- l'**optimizer fuso** RangerLite, che ricade sul percorso di aggiornamento in Python puro.

Il backend si sceglie da solo (`backend="auto"` → `"fused"` se CuPy c'è), quindi **non serve
alcun flag**: basta che la libreria funzioni. L'unico segnale del degrado è una `UserWarning`
persa in un log da centinaia di epoche.

**La cura:**
```bash
pip install "cupy-cuda12x<14"     # 13.6.0 verificata funzionante
python -c "import cupy; cupy.arange(10).sum()"   # deve girare senza errori
```

**Verifica che i kernel siano davvero attivi:**
```python
from model.modules.feature_transformer.fused_ft_functions import _HAS_CUPY_KERNELS
print(_HAS_CUPY_KERNELS)   # deve essere True
```

---

## 3. 🔴 Il batch ammortizza l'all-reduce, che è un costo FISSO

**Il meccanismo.** In DDP ogni step scambia l'intero gradiente fra le schede:
`90 M parametri × 4 byte = 360 MB`. Su GPU senza NVLink (A4000, 5090, tutte le consumer)
passa da PCIe e costa **~78 ms per step, indipendentemente dal batch**.

Il modello che ne esce combacia con le misure:

```
tempo per step = calcolo(batch) + all-reduce(fisso ~78 ms)

8192/GPU:   107 ms + 78 ms = 185 ms  →  32768 pos / 0,185 s = 177k    ← misurato 177k ✓
```

Ecco perché **4 GPU rendevano 1,9×** invece di 4×: quasi metà di ogni step se ne andava a
scambiare gradienti. Alzare il batch non rende il calcolo più efficiente — rende l'all-reduce
**più raro**.

**Throughput per GPU al variare del batch** (A4000, banco isolato):

```
4096 → 57k     8192 → 77k     16384 → 92k     32768 → 103k
```

**Il prezzo.** Il batch va compensato sull'LR con lo scaling `√(batch/batch_riferimento)`,
altrimenti ogni campione pesa meno:

```
ricetta SF:   batch 16384    LR 8,75e-4
batch 131072 = 8×      →     LR = 8,75e-4 × √8 = 2,47e-3
```

⚠️ **E si paga in passi di ottimizzazione.** `epoch-size` conta **posizioni** (100M), non
batch, quindi le posizioni viste non cambiano — cambia quanti aggiornamenti fai:

```
batch  16384 → 6103 passi/epoca   (ricetta)
batch 131072 →  763 passi/epoca   (un ottavo)
```

Con lo scaling sqrt la "distanza percorsa" nello spazio dei pesi (`passi × LR`) **scende di
2,8×**. Con lo scaling lineare resterebbe uguale ma il rumore per campione crescerebbe di √8.
Non c'è consenso su quale sia giusto per ottimizzatori Adam-like; sqrt è il più conservativo
sulla stabilità, e il rischio che introduce è **sotto-allenamento**, non divergenza.

**Come accorgersene**: confrontare la pendenza della `val_loss` per epoca contro quella del
regime precedente, ripartendo dallo **stesso checkpoint**. Se scende più lentamente, si sta
sotto-allenando e l'LR va spinto verso lo scaling lineare.

---

## 4. `--gpus` NON fa DDP

`train.py` legge `world_size` dall'ambiente distribuito. Senza `torchrun` resta 1 e gira su
**una sola scheda** — `--gpus 0,1,2,3` viene usato solo per dividere il batch, il che rende il
sintomo subdolo: i log parlano di 4 device e `nvidia-smi` mostra una scheda al 96% e tre a 0%.

```bash
torchrun --standalone --nproc_per_node=$N ddp_launcher.py train.py [args...]
```

In DDP `--gpus` è inutile: ogni rank usa la sua `LOCAL_RANK`.

**Controllo obbligatorio dopo ogni lancio multi-GPU:**
```bash
nvidia-smi --query-gpu=index,utilization.gpu,memory.used --format=csv,noheader
```
Devono avere memoria occupata **tutte**.

---

## 5. `ddp_launcher` affama il loader se non glielo impedisci

I core di ogni rank vengono divisi fra thread di torch e worker del loader **in proporzione a
quanto ne chiedi**, e `--threads` vale `-1` di default = "tutti i core disponibili":

```
req_t = 7 (tutti)  req_w = 8   →  scale = 7/15 = 0,47  →  threads 4, workers 3
req_t = 1          req_w = 12  →  scale = 7/13 = 0,54  →  threads 1, workers 6
```

Su training GPU-bound i thread di torch servono a poco (il lavoro è sui kernel), il loader
invece scala. Passare **`--threads 1`** raddoppia i worker.

---

## 6. ⛔ Cose che NON servono (misurate, non supposte)

**L'utilizzo GPU % non misura il margine.** Siamo passati da 130k a 529k con `nvidia-smi` che
segnava ~100% per tutto il tempo. Quel numero dice "c'è un kernel residente", non "l'hardware
è saturo". Non usarlo mai per concludere che non c'è più nulla da guadagnare.

**Gli 8 LayerStack non costano nulla.** `StackedLinear` fa `nn.Linear(in, out*count)` e poi
`select_output` ne tiene uno: **7/8 del lavoro è buttato**. Sembrava enorme sulla carta
(279.552 MAC per posizione contro 34.944 necessari) ed è risultato **piatto alla misura**:

```
LayerStack 8 → 49k     4 → 49k     2 → 48k     1 → 48k
```

🔑 **Lezione**: contare i FLOP senza distinguere il *tipo* di operazione porta fuori strada.
Una GEMM densa è ciò che una GPU fa meglio; la gather/scatter sparsa del FT è limitata dalla
memoria ed è dove sta il tempo, pur avendo **meno** FLOP. (Bullet, per inciso, fa la stessa
identica cosa: il suo `Select` riceve anch'esso l'uscita completa. Non è una sua ottimizzazione.)

**`BACKWARD_TILE_SIZE`**: 4, 8, 16, 32 → 92k in tutti i casi. Manopola morta.

---

## 7. Dove va davvero il tempo

Profilo con i kernel fusi attivi (`torch.profiler`, self CUDA time):

```
fused_double_ft_backward   53,8%
fused_double_ft_forward    24,3%
                           ─────  78,1%  il feature transformer
Optimizer RangerLite        9,2%
elementwise (fake-quant…)   5,4%
testa, cat, resto          ~7%
```

Il backward costa **2,2× il forward** perché il forward legge e il backward scrive in modo
contenzioso:

```cuda
atomicAdd(&grad_weight[w_idx * output_size + i], g_w0[s]);
```

per **ogni** feature attiva × **ogni** slice di uscita — miliardi di atomicAdd per batch su una
tabella 87k × 1024. Entrambi i kernel escono correttamente al primo indice `-1`, quindi il
padding a 304 slot **non** viene percorso: lì non c'è nulla da recuperare.

La prossima leva vera sarebbe riscrivere quel backward (ordinamento degli indici + riduzione
segmentata per accumulare una riga in shared memory invece di N atomicAdd globali), oppure
introdurre la precisione mista sul FT. **Il trainer non ha alcuna opzione di precisione**
(niente bf16, niente autocast): andrebbero toccati i kernel CuPy a mano.

---

## 7-bis. 🔴 Dove vivono le correzioni (non solo la documentazione)

Le scoperte sopra sono **applicate negli script**, non solo descritte. Se riparti da zero le
ottieni senza rifare niente:

| correzione | dove | verifica |
|---|---|---|
| CuPy pinnata `<14` | `setup_vm.sh:82` | lo script stampa `✅ CuPy presente` |
| `NNUE_WORKER_THREAD_RATIO` + diagnostica `[loader]` | **dentro `triumviratus_passedpawns.patch`** | riga `[loader] concurrency=N reader=… worker(feature)=…` all'avvio |
| `NNUE_BACKWARD_TILE` | idem (inerte al default, serve per lo sweep) | — |
| lancio DDP via `torchrun` | `train_phase1.sh` | riga `[DDP] N GPU via torchrun` |
| `--threads` esplicito | `train_phase1.sh`, env `TORCH_THREADS` | riga `[Launcher] Applied constraints` |
| batch 131072 + lr 2.47e-3 | `train_phase1.sh` | riga `batch_size(global)=131072` |
| `numpy<2.5` dopo requirements | `setup_vm.sh` | import di numba non fallisce |
| controllo wheel-vs-driver | `setup_vm.sh` | fallisce subito con il comando di rimedio |
| `hf` al posto di `huggingface-cli` | `download_phase1.sh`, `download_phase2.sh` | — |
| macro `USE_AVX512` per Windows | `build_pgo_clang_7_trann2.ps1` | — |

⚠️ La patch è stata **rigenerata** il 28/07 per includere le modifiche al loader, e verificata
applicandola su un clone pulito al pin `9f72946`. La versione precedente è in
`triumviratus_passedpawns.patch.bak`.

---

## 8. Strumenti lasciati in eredità

| file | cosa fa |
|---|---|
| `gpu_ceiling.py` | tetto GPU senza data loader. `--batch`, `--active`, `--ls-buckets`, `--device cpu` |
| `loader_ceiling.py` | throughput del solo loader. `--workers`, `--skip` |
| `profile_step.py` | profilo di uno step: dove va il tempo GPU |

Variabili d'ambiente aggiunte da noi:

```
NNUE_WORKER_THREAD_RATIO   ripartizione reader/extractor nel loader C++ (default 0.14)
NNUE_BACKWARD_TILE         BACKWARD_TILE_SIZE del kernel FT (default 4, risultato ininfluente)
TORCH_THREADS              passato a --threads di ddp_launcher (default nostro: 1)
```

⚠️ **`gpu_ceiling.py` sottostima.** Genera indici uniformemente casuali su 87k righe, mentre le
posizioni vere hanno una distribuzione molto sbilanciata (poche feature comunissime) con
località di cache assai migliore. Prevedeva 412k, la realtà è 529k: usalo per **confronti
relativi**, mai come previsione assoluta.

---

## 9. Prima di affittare una macchina — i gate da 30 secondi

Tre host bocciati in una notte, per tre motivi diversi. In ordine, prima di scaricare un byte:

**Banda verso HuggingFace** (non fidarsi di quella dichiarata da vast.ai: una macchina
annunciava 830 Mbps e ne dava 170):
```bash
curl -sL -o /dev/null -w "HF: %{http_code}  %{speed_download} B/s\n" --max-time 25 \
  "https://huggingface.co/datasets/vondele/master-binpacks_relabel/resolve/main/fishpack32.relabel-BT4-tf13tune.binpack"
```
Serve `200` e **≥ 20 MB/s**. Un host dava `000`: internet funzionava, HuggingFace no.

Verifica additiva se il numero è basso — scarica da Cloudflare **mentre** HF scarica: se il
totale non sale, il collo è il link dell'host e non la CDN.

**Disco**: fase 1 = 121 GB dati + ~15 GB checkpoint (1,86 GB l'uno) + ~10 di ambiente ⇒
**160 GB minimi**. Fase 2 ne vuole 380+. Due macchine scartate perché ne avevano 32.

**Driver contro wheel** — non basta che la GPU sia supportata:
```bash
python -c "import torch; torch.zeros(8, device='cuda')"
```
Una wheel `cu130` su driver CUDA 12.8 **importa senza errori** e fallisce solo al primo uso
della GPU, cioè dopo che hai scaricato i dati. Rimedio:
`uv pip install --reinstall torch --index-url https://download.pytorch.org/whl/cu128`.

**`numpy<2.5`** dopo `requirements.txt`: la risoluzione di torch tira su numpy 2.5, numba
vuole ≤ 2.4 e fallisce all'import di `serialize.py`.

**`huggingface-cli` è dismesso**: la CLI nuova è `hf`. La vecchia esiste ancora come
eseguibile ed **esce con errore** invece di scaricare.

---

## 10. Trappole di metodo pagate in giornata

🔑 **Una misura vale solo nel regime in cui è fatta.** Il `random-fen-skipping` sembrava da
togliere sull'H100 (CPU e GPU **entrambe** al 25-32%, regime latency-bound) ed è risultato
**gratis** sulle A4000 (GPU sature, CPU al 15%): 3,58 it/s con, 3,57 senza. La stessa modifica,
segno opposto, su due macchine.

🔑 **Misurare il controllo prima di accusare il codice nuovo.** L'ipotesi "gli 8 bucket sono lo
spreco" era aritmeticamente solida e falsa. Il confronto con bullet l'ha smentita come
*differenza*, la misura l'ha smentita come *costo*.

⚠️ **`pkill -f <pattern>` fa match su se stesso**: la riga di comando che contiene il pattern
viene uccisa insieme al bersaglio, e lo script muore prima di completare. Usare
`pgrep -f "[t]rain.py"` o `pkill -f pattern --older 5`.

⚠️ **Gli heredoc dentro `ssh '...'` si rompono** sulle virgolette e sui backslash annidati:
scrivere lo script in locale e copiarlo con `scp`.

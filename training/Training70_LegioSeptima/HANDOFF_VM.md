# HANDOFF — rete 7.0 "legio-septima"

> Aggiornato il **28/07/2026**. **Leggere questo file per primo.** Stato reale, comandi in
> ordine, e le trappole già pagate.
>
> 📄 Compagno obbligatorio: **`HANDOFF_PERFORMANCE.md`** — come far girare il training 5×
> più veloce e i gate da 30 secondi prima di affittare una macchina.

---

## 1. Stato

**Fase 1 IN CORSO** su vast.ai (4× RTX A4000, $15/giorno). Epoca **~114 su 500**.
Ritmo: **671k pos/s**, ~148 s per epoca ⇒ **~16 ore** al completamento.

Il motore 7.0 è portato a SFNNv16 e verificato contro il trainer (R² 0,999999). La catena
completa trainer → serialize → motore → partite è **chiusa e provata**.

## 2. Dove sta cosa

| | |
|---|---|
| **Motore 7.0** | `Triumviratus_7/` — build PGO: `build_pgo_clang_7_trann2.ps1` |
| **Trainer** | `Training70_LegioSeptima/nnue-pytorch/` — upstream `9f72946` + patch |
| **Reti prodotte** | `Networks_Triumviratus_7/legio-ep{66,92,111,114}.nnue` |
| **Script** | `setup_vm.sh` · `download_phase1.sh` · `train_phase1.sh` · `download_phase2.sh` · `train_phase2.sh` |
| **Strumenti perf** | `gpu_ceiling.py` · `loader_ceiling.py` · `profile_step.py` |
| **Gate locale** | `match_70_vs_60.ps1` (7.0 vs 6.0, entrambi PGO+AVX512) |
| **Architettura** | `Triumviratus_7/NETWORKS_7.0.md` + `docs/architecture.html` |

## 3. Architettura congelata

`Full_Threats 59.808 + HalfKAv2_hm 22.528 + PawnPair 4.560 + PassedPawns 96 = **86.992**`
L1 1024, L2 32, L3 32, 8 LayerStack. Base **SFNNv16**. L'unico blocco nostro è **PassedPawns**.

## 4. Ricetta della fase 1 — valori VIVI

```
--max-epochs 500          --epoch-size 100000000     (posizioni, non batch)
--batch-size 131072       --lr 2.47e-3               (= 8.75e-4 x sqrt(8))
--gamma 0.990             --start-lambda 1.0 --end-lambda 0.75
--random-fen-skipping 3   --validation-size 1000000  --check-val-every-n-epoch 5
--network-save-period 20  --save-top-k 5
GPUS=0,1,2,3  workers 8  NNUE_WORKER_THREAD_RATIO=0.05  TORCH_THREADS=4
```

⚠️ **`--batch-size` e `--lr` sono legati**: cambiando l'uno va riscalato l'altro con
`√(batch/16384)`. E `--max-epochs` con `--gamma`: `gamma^epoche` deve restare ~0,008.

## 5. Sequenza su una VM nuova

```bash
# 0) 🔴 PRIMA DI TUTTO: i gate di HANDOFF_PERFORMANCE.md §9 (banda HF, disco, driver)
./setup_vm.sh ~/legio-septima                # patch + CuPy pinnata + verifica driver
./download_phase1.sh /data/phase1 6          # 121 GB, ~3 min a banda piena
# ripristina il checkpoint in <ROOT>/lightning_logs/version_0/checkpoints/last.ckpt
./train_phase1.sh /data/phase1 8
```

**Ripresa**: `last.ckpt` a ogni epoca, lo script lo trova da solo.
⚠️ **Cercalo con `find`, non con un percorso fisso**: ogni riavvio crea una
`lightning_logs/version_N` nuova, e `version_0/` resta indietro. Ci sono cascato.

```bash
find /data/run_phase1 -name last.ckpt -exec ls -1t {} + | head -1
```

## 6. Cosa è verificato, e come

- **V1** `static_assert` lega `Dimensions` alla tabella di offset ⇒ 59.808 esatti.
- **V2** rete random dal trainer caricata dal motore. Trovò **tre** disallineamenti (§8).
- **V3** `cross_check_eval.py`: **R² 0,999999**, errore medio 0,57 = arrotondamento.
- **V4 ✅ CHIUSA (28/07)** rete allenata → motore: `eval +28 cp` sulla startpos (SF dà ~+20/+30),
  bench 319.740 nodi, PV sensate. La catena completa funziona.
- **V5** gate Elo: in corso, vedi §7.

## 7. Misure di forza — tutte a 15+0.15, UHO, 1 thread, 64 MB

| confronto | risultato | note |
|---|---|---|
| 7.0 ep54 vs 6.0 v3 | **−64,97 ± 13,39** (898 gare) | ⚠️ binari NON appaiati |
| 7.0 ep111 vs 6.0 v3 | **−21,95 ± 30,38** (206 gare) | entrambi PGO+AVX512 |
| ep92 vs ep60 | **+16,12 ± 12,50**, LOS 99,4% (798 gare) | il cambio di regime non ha danneggiato |
| EvalScale 75 vs 60 | −16,96 ± 32,99 (82 gare) | non concludente, manopola sbagliata |

🔴 **I primi due non sono confrontabili fra loro**: cambiano *sia* la build *sia* 57 epoche di
training. Non attribuire il salto a una delle due.

**Scala della rete**: su 400 posizioni del libro, `eval_7.0 = 0,802 × eval_6.0` (r = 0,755).
La 7.0 è compressa del 20%, e i margini additivi della ricerca (futility, razor, singular, LMR)
sono tarati in centipawn sulla v3. `EvalScale` da solo **non** è la cura: due parametri
(`CapFutVicScale`, `QFutVicScale`) lo seguono per costruzione, ma le decine di margini additivi
no. Serve un **co-tune SPSA del blocco accoppiato alla eval, sulla rete definitiva**.

## 8. 🔴 Trappole già pagate

**Trainer / rete**
1. **Ordine dei blocchi**: `Full_Threats+HalfKAv2_hm^+PP_3Wide+PassedPawns`. Upstream mette
   PP_3Wide prima; l'ordine decide layout dei pesi **e** hash. Sbagliato ⇒ rete rifiutata.
2. **`ft_compression="leb128"`** nel serialize: con `"none"` la rete non si carica.
3. **Hash pawn-pair** = `0x86F2B1DD` (upstream), non il nostro vecchio `0x50414952`.
4. **Python ≥ 3.12**, **`numpy<2.5`** (numba), **`cupy-cuda12x<14`** (vedi PERFORMANCE §2).
5. 🔑 **Il cross-check va fatto sull'uscita GREZZA**: il comando `eval` passa per `nn_scale`
   (blend psqt/positional, complessità, materiale, rule50) che il trainer non ha. Confrontando
   quella sbagliata si ottiene R² 0,73 **anche su una coppia nota-buona**.

**Motore**
6. 🔴 **Il `.vcxproj` della 7.0 NON definisce `USE_AVX512`.** Il codice gatta su
   `#if defined(USE_AVX512)`: i flag `-mavx512*` ci sono, ma i percorsi NNUE restano AVX2.
   `build_pgo_clang_7_trann2.ps1` aggiunge `-DUSE_AVX512 -DUSE_VNNI -DUSE_AVX512ICL`.
   Tutti i match fino al 28/07 pomeriggio giravano su un 7.0 di fatto AVX2.

**Effetti collaterali non ovvi**
7. **Cambiare `--batch-size` a metà run sposta l'annealing della lambda.**
   `max_steps = max_epoch × batch_per_epoca`, e `ratio = current_step / max_steps`. Riprendendo
   da un checkpoint con `batch_per_epoca` diverso, il rapporto salta:
   ```
   ratio 0,120 → lambda 0,970       (prima, batch 32768)
   ratio 0,480 → lambda 0,880       (dopo,  batch 131072)
   ```
   Il **valore finale resta 0,75** — corretto — ma ci arriva all'epoca ~320 invece che 500 e poi
   ci resta. Nel mezzo il risultato di partita pesa ~3× più della ricetta.
8. 🔑 **La `val_loss` NON è confrontabile fra regimi diversi**: cambia col lambda (bersaglio
   diverso) e col livello di LR (una rete "calda" ha loss più alta a parità di qualità).
   Il 28/07 la val_loss è peggiorata mentre il gioco dava **+16 Elo con LOS 99,4%**.
   **Il gioco è l'unico giudice.**

## 9. Aperto

- **Fase 2**: 380+ GB di disco, la macchina attuale ne ha 316 ⇒ serve un'altra VM.
  Con 1 TB si può tenere anche la fase 1 e alzare il budget a ~800 GB invece di cancellarla.
- **`.q` e i 68 GB classici**: un solo download (`fishpack32` nelle tre varianti, 19,3 GB)
  chiude entrambe le domande — `PROSPETTIVE_RETE_7.0.md` §17.2.
- **Occupanza dei bucket sul corpus vero**: `bucket_occupancy.py`, un minuto, decide 8 vs 16
  LayerStack. Senza, si resta a 8.
- **Ricalibrazione SPSA** del blocco eval-accoppiato, sulla rete definitiva (§7).
- **Il commento di `--lr` in `train_phase1.sh`** documenta ancora batch 32768: cosmetico.

## 10. Chiusura della fase 1

Non fermarsi a 500 per forza: guardare la **val_loss** *e* i gate. Se a fine corsa fosse ancora
in discesa, continuare costa poco — a 148 s/epoca, **300 epoche in più sono 12 ore**, meno di
quanto costavano 100 epoche prima delle ottimizzazioni. È anche il modo di recuperare il conto
degli aggiornamenti: 500 × 763 = 381k passi contro i 3,05 M della ricetta SF.

Chiusura con **`over_last`** (media degli ultimi 3 checkpoint), ricetta già validata su v1 e v2.
Poi build Linux, nuovo canary (`bench`, **un bench per processo**), e gate net-isolated vs la v3
a TC ≥ 20+0.2. Pavimento di risoluzione: ±Elo ≈ 344/√N ⇒ 5 Elo richiedono ~4.700 partite.

# Triumviratus — Stato & Roadmap

Documento unico di **stato attuale**, **storico migliorie misurate** e **roadmap**.
Filosofia: ogni modifica è uno step isolato dietro un toggle, validato matematicamente
(SPRT per i cambi di ricerca, A/B NPS per i cambi di velocità). Niente si committa
"a sensazione".

Ultimo aggiornamento: 2026-06-01

---

## 1. Stato attuale

- **Versione:** Triumviratus 3.4.2 (MSBuild Release|x64, MSVC v143, AVX2).
- **Forza stimata:** ~3572–3583 Elo (gauntlet 4CPU: 3561 ±22 vs pool CCRL; +~31 SPSA fine-tuning 3.4.1+3.4.2).
- **Eval:** NNUE ibrida (feature transformer + affine), incrementale + dual-net lazy.
- **Ricerca:** alpha-beta PVS, ABDADA SMP (shared TT + busy-bit + depth-staggering),
  improving heuristic, singular ext avanzate (double/negative), ProbCut, node-based TM,
  margini SPSA-tuned (RFP/razor/futility/singular), Syzygy.
- **Toggle UCI diagnostici/A-B:** `EvalCache` (on), `Improving` (on), `NodeTM` (on),
  `SingularExt` (on), `ProbCut` (on), `CorrHist` (off), `UsePolicy` (off), `EvalOff` (profiling).
- **Spin SPSA-tunabili (bakati):** LMRBase=47, LMRDiv=270, LMRTTDepth=2, FutilityBase=111 (3.4.1); RFPMargin=21, RazorMult=139, FutilityImproving=93, SingularDoubleMargin=43, HistReductionDiv=1041, AspInitDelta=31, AspGrow=31, ContHistDiv=6595, SmallNetThreshold=782 (3.4.2).

### Note di profiling (riferimento)
- Eval NNUE = ~60% del tempo/nodo. eval-OFF ≈ 2.3M nps, eval-ON ≈ 0.9M nps (1 thread).
- Collo di bottiglia eval = **feature transformer/accumulatore**, non i layer affine.
- Gap NPS ~3-4x vs Stockfish = **architetturale (rete)**, NON compilatore né movegen.
- Scaling ABDADA (8845HS, 8c/16t): 2t 93% · 4t 91% · 8t 67% · 16t 49% (SMT).

---

## 2. Storico migliorie (misurate)

| Data | Modifica | Tipo | Esito |
|------|----------|------|-------|
| 2026-05 | **Static-eval cache** per-thread (memoizzazione eval, key=hash^fifty) | velocità | **+3% NPS**, sicuro (gioco identico). Tenuto, def on (`EvalCache`). |
| 2026-05 | **Improving heuristic** (RFP/futility/LMR modulati dal trend eval) | ricerca | **Positiva confermata** (LOS 98%) @3+0.1, 508 partite. Stima centrale +17.8 ma con IC95% ampio ≈ [+1, +35] → la *grandezza* è ancora rumorosa (~+10/+18 realistico). Da rifinire con più partite + conferma a 8+0.08. Toggle `Improving`. |
| 2026-05 | **Node-Based Time Management** (`NodeTM`) | tempo | **Neutra** (~+3 ±14, LOS 64%). Non dannosa, reduce-only. Tenuta on. NB: prima versione aveva un bug grave (scalava il timestamp assoluto invece della durata → perdeva a tempo / depth-6); risolto scalando `soft - starttime`. |
| 2026-05 (v3.3.2) | **Singular Extensions avanzate** (Double +2 / Negative −1) (`SingularExt`) | ricerca | **+34.1 ±23.9 Elo, LOS 99.7%** @3+0.1, 235 partite (sopra node-TM). Lower bound +10 → successo netto. Il lever più grosso finora. |
| 2026-05 | **Correction history** (pawn-key bucket, `CorrHist`) | ricerca | **Neutra** → **default OFF**. Versione aggressiva (cap 48) = −12 Elo; versione gentile (cap 16, learn /512) ≈ 0 (±17). Codice tenuto dormiente per un retry con SPSA. |
| 2026-05 (v3.3.3) | **SPSA coarse dei margini** (RFP 80→30, RazorMult→102, FutilityBase→82, FutilityMult→66, SingularDoubleMargin→63) | tuning | **+18.8 ±15.5 Elo, LOS 99.2%** @3+0.2. Cotto nei default. Fine-pass successivo (HistRedDiv/AspGrow) = neutro → scartato. |
| 2026-05 (v3.3.4) | **ProbCut** (`ProbCut`, margine `ProbCutMargin` def 180) | ricerca | **+6.6 ±13.2, LOS ~84%** @8+0.08 (SPRT non chiuso ma trend + stabile su tecnica standard). Accettata default-on. Margine in taratura SPSA. |
| 2026-05 | Syzygy tablebases (Fathom, WDL+DTZ) | correttezza | Finali corretti. |
| 2026-05 | **CorrHist + ContHistPrune** (default on, provvisorie) | ricerca | ~+5.5 LOS ~81% @2+0.02 (NON validato, da confermare @8+0.08). Toggle `CorrHist`/`ContHistPrune`, tunabili esposti. |
| 2026-05 | **Anti-forfeit `bestmove (none)`** | correttezza | Se la ricerca viene abortita prima di produrre una mossa (estrema pressione di tempo), ripiega sulla 1ª mossa legale invece di emettere `(none)` (= sconfitta). Mitiga il crash ~1/800 in GUI/torneo. |
| 2026-05 | **Lazy eval + TimeMgmt** (default on) | velocità/tempo | LazyEval: salta l'eval NNUE in scacco (non usato lì) = NPS gratis. TimeMgmt: estende il tempo se lo score cala vs iterazione precedente. A/B combinato @1+0.01: **+9.1 ±10.2, LOS 96%** (stima tenuta su ~+9 mentre l'IC si stringeva → effetto reale). Toggle `LazyEval`/`TimeMgmt`. Caveat: TC con qualche perdita a bandiera → magnitudine ~+5..9 incerta; il SEGNO è solido. Split LazyEval/TimeMgmt non isolato. |
| 2026-05 | **Lazy SMP** (toggle `LazySMP`, default on) | architettura | Rimpiazza la coordinazione ABDADA (busy-table) con thread indipendenti + TT condivisa + depth-skipping per-thread. A/B diretto **+102 Elo LOS 99.99%** @2+0.02 4-thread; ancora **4CPU 3503→3558 (~+55)**. ABDADA preservato (toggle off). |
| 2026-05 (v3.4.1) | **SPSA fine LMR/futility** (LMRBase 75→47, LMRDiv 225→270, FutilityBase 82→111, LMRTTDepth 0→2) | tuning | **+19.95 ±13.18 Elo, LOS 99.85%** @8+0.08, 1360 partite. 4 parametri core LMR e futility; prima SPSA al TC di torneo. |
| 2026-06 (v3.4.2) | **SPSA fine search/history/aspiration** (18 parametri: AspGrow 100→31, HistReductionDiv 3500→1041, RazorMult 102→139, FutilityImproving 60→93, ContHistDiv 5000→6595, SingularDoubleMargin 63→43, AspInitDelta 25→31, RFPMargin 30→21, SmallNetThreshold 1050→782) | tuning | **+11.41 ±8.67 Elo, LOS 99.51%** @3+0.03, 3595 partite. SPRT H1 accepted. 9 parametri mossi su 18 tunati; 9 confermati all'init. |

### In sospeso — provate, non hanno reso (classificate)
Tutte dietro toggle, **default OFF**: non toccano il motore. Diviso per causa.

**A) Implementazione NAIVE → potenzialmente rifacibili (ma guadagno atteso sotto il pavimento di misura):**
- **CorrHist multi-tabella** (`CorrHistMulti`): minor (N/B) + major (R/Q) oltre alla pawn.
  Misurato **−7.5 Elo** @2+0.02 (isolato, 550 partite). **Causa = mia implementazione naive**:
  le 3 tabelle imparano lo stesso `diff` e vengono **sommate** → saturano il clamp ±32 troppo
  presto → sovra-correzione. **Fix**: pesi per tabella (o media, non somma) + cap più alto.
- **Continuation history 2/4 ply** (`ContHistMulti`): mossa a 2 e 4 ply indietro in ordering+update.
  Misurato **−19 Elo** @2+0.02 (isolato, 450 partite). **Causa = mia implementazione naive**:
  aggiunte a **peso pieno** come la 1-ply → il segnale lontano (più rumoroso) inquina
  l'ordinamento. **Fix**: pesare MENO i ply lontani.

**B) GENUINAMENTE piatte/negative → abbandono corretto (non era un errore):**
- **TT 4-way + aging** (`TT4Way`): **−20 Elo** a 4 thread (la sua arena). Possibile victim/bucketing
  subottimale, ma il valore atteso delle migliorie TT è comunque basso (+0..10). Bassa priorità.
- **SmallNetThreshold** (soglia Big/Small net): SPSA @4+0.04 = **piatto**, ciondola attorno a 1050
  (default Stockfish). Nessun ottimo diverso → **tenuto 1050**. Confermato: non c'è Elo da qui.
- **Fine-pass SPSA** (HistRedDiv/AspGrow), **Aspiration window**, **NodeTM**: misurati neutri. Corretto scartarle.

> Nota di metodo: anche le (A) fixate renderebbero ~+2..8 Elo, **sotto la soglia confermabile**
> a questa forza (3558). Riprenderle = molto lavoro per Elo non misurabili. Parcheggiate apposta.

### Vicoli ciechi (NON riprovare)
- **VNNI / AVX-512** (`/arch:AVX512`): −7%. Zen4 double-pumpa il 512-bit; VNNI aiuta
  solo i layer affine (la parte minore).
- **clang-cl 19** (AVX2 e znver4): pareggio/leggera regressione vs MSVC. MSVC già ottimale.
- **Policy CNN in ricerca** (root ordering / interior / root-LMR): da neutro a −85 Elo.
  Disabilitata (`UsePolicy` off). Per riprovare servirebbe int8/rete più piccola.

---

## 3. PIANO FORWARD (post-3.4.0) — ordinato per ratio Elo / (rischio × complessità)

Principio: prima i +Elo facili e misurabili e gli **abilitatori** (incl. la pipeline
dati), così quando arriveremo alla **rete custom** il self-play sarà generato dal
motore **più forte possibile** → dataset di qualità massima. Per questo la rete è in
fondo: non perché valga poco (vale di più di tutto il resto), ma perché **dipende**
dal lavoro sopra.

### Tier A — Quick win (basso rischio, misurabili, SUBITO)
| Voce | Elo | Rischio/Compl. | Note |
|---|---|---|---|
| **AggrLMR** | +0..8 | basso/basso | Ai default (2048/3) = **NEUTRO** (0.1 ±10, LOS 51% @2650 partite). Non parcheggiato: i param `AggrLMRDiv`/`AggrLMRClamp` entrano nella **SPSA unificata** (frozen on) per vedere se un'aggressività diversa è +. Se restano fermi → neutro confermato. |
| ~~**LMRBase / LMRDiv**~~ (formula core LMR) | +? | basso/basso | **FATTO (v3.4.1)**: LMRBase 75→47, LMRDiv 225→270, +19.95 Elo. |
| ~~**Re-SPSA margini + search/history/aspiration**~~ | +3..10 | basso/basso | **FATTO (v3.4.2)**: +11.41 Elo LOS 99.51% @3+0.03. 18 parametri tunati, 9 bakati. |
| **LMR enrichment + retune razor/NMP** | +2..8 | basso/medio | Condizioni LMR staccate (TT-move, tt-depth, quiet-count) + margini razor/NMP al TC giusto. |

### Tier B — Medie (più sforzo, Elo moderato)
| Voce | Elo | Rischio/Compl. | Note |
|---|---|---|---|
| **Singular ext avanzate** (triple ext, multi-cut) | +2..6 | medio/medio | |
| **CorrHistMulti / ContHist 2-4ply RIFATTE con pesi** | +0..8 | medio/medio | Le parcheggiate, fatte giuste (pesi per tabella/ply). Sotto la soglia di misura → solo se Tier A esaurito. |
| **Lazy MovePicker** (movegen stadiato) | +5..12 | **alto/alto** | +NPS reale ma rewrite invasivo del move loop (interagisce con singular/ProbCut/Lazy SMP/qsearch + validatore TT-move). Progetto, non test. |

### Tier C — Abilitatori / robustezza (no Elo diretto, ma necessari)
| Voce | Elo | Rischio/Compl. | Note |
|---|---|---|---|
| **ASAN + fix crash ~1/800** | 0 | basso/medio | Niente Elo ma alto valore torneo/release. Fallo prima di tornei seri. |
| **🔑 Self-play data gen** (FEN+score+eval+depth) | 0 | medio | **PREREQUISITO della rete.** Logging già abbozzato (`DataLog`). Da consolidare. |
| **Pulizia codice** (policy morta, ABDADA, toggle parcheggiati) | 0 | basso | Manutenibilità. |

### Tier D — Ricerca (il focus originale del progetto)
| Voce | Elo | Rischio/Compl. | Note |
|---|---|---|---|
| **Rivivere policy net int8/più piccola** | +? | alto/medio | Distinta dalla NNUE-eval. Ora −85 in ricerca; int8/rete piccola può ribaltarla. |

### Tier E — Il salto (in fondo PER SCELTA, dipende da tutto il resto)
| Voce | Elo | Rischio/Compl. | Note |
|---|---|---|---|
| **🏆 Rete NNUE custom** | +50..150 | alto/alto | Il lever più grosso. Richiede self-play **alla massima qualità** → serve l'engine più forte possibile (Tier A-B) + la pipeline dati (Tier C). Progetto lungo: calo iniziale fisiologico poi indipendenza totale da Stockfish. |

---

## 4. Roadmap storica (item originali, alcuni superati)
### Roadmap — ordinata per priorità (alto impatto + bassa difficoltà in cima)

Legenda:
- **Elo**: guadagno atteso (stima grossolana, da confermare con SPRT).
- **Diff**: difficoltà implementazione+test. ★ = facile, ★★ = media, ★★★ = alta.
- **F**: fase originale (1 ricerca · 2 architettura · 3 rete).

| # | Voce | Elo | Diff | F | Note |
|---|------|-----|------|---|------|
| 1 | **Improving su LMP + null-move** | +2..8 | ★ | 1 | RIMANDATA: implementata poi tolta (troppo piccola da isolare). Da riprovare singola. |
| 2 | ~~Node-Based Time Management~~ | +5..15 | ★ | 1 | **FATTO** (v3.3.2), neutra ~+3. Toggle `NodeTM`. |
| 3 | ~~Aspiration window tuning~~ | +2..8 | ★ | 1 | **FATTO** (esposto AspInitDelta/AspGrow, tarato in fine-pass): **neutro** (~0), AspInitDelta piatto. Resta ai default. |
| 4 | ~~SPSA tuning dei margini~~ (RFP/futility/razor/LMR) | +5..15 | ★★ | 1 | **FATTO** (v3.3.3): coarse +18.8 Elo cotto. Infra SPSA pronta (spsa_tune*.py) per ogni param futuro. |
| 5 | ~~Singular Extensions avanzate~~ (Double + Negative) | +10..20 | ★★ | 1 | **FATTO** (v3.3.2): +34 Elo, LOS 99.7%. Toggle `SingularExt`. |
| 6 | ~~**Correction History**~~ | +10..20 | ★★ | 1 | **ADOTTATA PROVVISORIA** (default on, toggle `CorrHist`): cap 32 / lr-div 512, tunabili `CorrCap`/`CorrLearnDiv`. SPRT congiunto con #8 @2+0.02: **~+5.5, LOS ~81% @1750 (IC tocca lo 0, NON validato)**. L'ultrabullet sotto-vende una feature depth-dipendente → adottata in via provvisoria, **da confermare a 8+0.08**. |
| 7 | ~~ProbCut~~ | +5..12 | ★★ | 1 | **FATTO** (v3.3.4): +6.6 Elo, LOS ~84% @8+0.08. Default on (`ProbCut`), margine 180 in taratura SPSA (`ProbCutMargin`). |
| 8 | ~~**History gravity/aging + continuation nel pruning**~~ | +5..15 | ★★ | 1 | **ADOTTATA PROVVISORIA** (default on, toggle `ContHistPrune`): continuation-history nella riduzione LMR + potatura dei quiet tardivi con storia combinata molto negativa a bassa depth. Tunabili `ContHistDiv` (def 5000), `HistPruneMargin` (def 1000). SPRT congiunto con #6 (vedi sopra). Da confermare a TC lungo. |
| 9 | **Address Sanitizer (`/fsanitize=address`)** | ~0 | ★ | 2 | Non dà Elo ma stana il crash ~1/400 della GUI. Alto valore per release/tornei. Fallo presto. |
| 10 | ~~**TT bucketizzata (4-way) + aging migliore**~~ | +3..10 | ★★ | 2 | **PROVATA = NEGATIVA**, parcheggiata. A/B diretto a **4 thread** (la sua arena): **~−20 Elo** (0.47, stabile @~100 partite), LLR fermo. Il bucket-scan + victim age-aware non compensa a questi core/dimensioni TT. Codice dietro toggle `TT4Way` (default OFF = direct-mapped originale). Da rifare semmai con entry più larga / replacement diverso. |
| 11 | **Static eval nella TT vera** | +0..3 | ★★ | 1 | Estende eval/improving cross-thread. Poco Elo (cache per-thread già prende il grosso). |
| 12 | **Pulizia codice morto** (policy, unused) | 0 | ★ | 2 | Manutenibilità, non Elo. |
| 13 | **Ottimizzazione movegen** | +NPS | ★★★ | 2 | Limare cicli. Guadagno piccolo (movegen non è il collo di bottiglia). |
| 14 | ~~**Lazy SMP** (rimuovere ABDADA)~~ | +? alto core | ★★★ | 2 | **FATTO/ADOTTATO** (default on, toggle `LazySMP`): A/B diretto @2+0.02 4-thread **+102 Elo LOS 99.99%**, ancora gauntlet **4CPU 3503→3558 (~+55)**. La busy-table ABDADA sprecava lavoro. Da provare anche a 8 thread. |
| 15 | **Self-play data gen** | 0 (enabler) | ★★ | 3 | Logging FEN+score. Prerequisito per la rete propria. |
| 16 | **Quantizzazione int8 / rete più piccola** | +NPS | ★★★ | 3 | Abbatte il vero collo di bottiglia (eval ~60%). Può far rivivere la policy. |
| 17 | **Training rete custom** (bullet/PyTorch) | −poi+ | ★★★ | 3 | Calo iniziale fisiologico, poi indipendenza totale. Progetto lungo. |
| 18 | **Sperimentare architettura rete** (bucket/dimensione) | ±? | ★★★ | 3 | Forza vs velocità. Dopo aver consolidato il pipeline di training. |

**Suggerimento di esecuzione:** punti 1→3 sono "quick win" da fare subito (facili, infra
pronta). Poi 4 (SPSA) perché rende automatici i tuning di tutti gli altri. Poi i due grossi
lever di forza, 5 (singular avanzate) e 6 (correction history). Il 9 (ASAN) infilalo appena
hai un'oretta: non è Elo ma toglie il crash che imbarazza in torneo.

---

## 4. Metodo di test (promemoria)

- **Cambi di ricerca → SPRT** in cutechess, stesso exe con toggle UCI:
  `option.Improving=true` vs `false`. Parametri tipici `elo0=0 elo1=5 alpha=0.05 beta=0.05`.
- **Cambi di velocità → A/B NPS** interlacciato (`eval_ab.ps1`): mediana cache/feature
  OFF vs ON nello stesso run (immune al rumore ~10% run-to-run dei singoli go).
- **TC:** 3+0.1 per volume veloce; **riconfermare a 8+0.08** prima di committare i
  cambi di pruning (a TC corto possono ingannare).
- **Anchor Elo:** ancorare i tornei a ≥3 avversari con rating noto affidabile; diffidare
  degli outlier (es. prune sotto-performa il suo anchor).

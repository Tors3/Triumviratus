# Triumviratus — Stato & Roadmap

Documento unico di **stato attuale**, **storico migliorie misurate** e **roadmap**.
Filosofia: ogni modifica è uno step isolato dietro un toggle, validato matematicamente
(SPRT per i cambi di ricerca, A/B NPS per i cambi di velocità). Niente si committa
"a sensazione".

Ultimo aggiornamento: 2026-06-03

---

## 1. Stato attuale

- **Versione:** Triumviratus 3.5 (MSBuild Release|x64, MSVC v143, AVX2). *(3.5 = MovePicker stadiato
  bakato + pipeline NNUE validata; NON 4.0: il 4.0 è riservato alla prima rete proprietaria competitiva.)*
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
| 2026-06 (v3.5) | **MovePicker stadiato** (movegen lazy per stadio: TT→catture buone→killer→counter→quiet→catture cattive; Phase 2: skip_quiets su LMP/futility, skip_bad_caps su SEE; margini SEE esposti `SEECaptureMargin`/`SEEQuietMargin`) | ricerca | **+16.6 ±? Elo, LOS 96.6%** @5+0.1, 900 partite. **Default ON.** Conferma chiave: il segnale emerge SOLO a TC reale (5+0.1+), ~0 a iperbullet (eval-bound). Primo +Elo di ricerca solido dopo il fine-tuning. |
| 2026-06 | **Pipeline NNUE training** (PGN→plain→binpack→nnue-pytorch→.nnue) | abilitatore | **VALIDATA end-to-end.** 12,8M pos (self-play d12, label SF), commit 2db3787 (L1=2560 SFNNv8). val_loss in caduta pulita, zero overfit. Rete "rubicon" = giro di validazione, NON competitiva (tetto distillazione). Ricetta in `memory/project_nnue_training.md` + `SCALETTA.md`. |
| 2026-06 | **Multi-cut singolare** (`MultiCut`, gate conservativo `singular_beta >= beta` → potatura nodo) | ricerca | **+10.4 ±11.2 Elo, LOS 96.6%** @8+0.08, **1100 partite**, 1 thread (rientrato da +13.4@700, stabile). **Default ON.** Unico sopravvissuto del "bundle #3": TripleExt −75 (tree-blowup), LMREnrich −25 (over-reduction), MultiCutAggr (gate SF `s>=beta`) −13 (fail-high a metà depth troppo debole) → tutti scartati. Lezione: il gate SF non trasferisce, il nostro singular vuole il gate conservativo sul bound. Dead-end tenuti come toggle dormienti (TripleExt/LMREnrich), MultiCutAggr rimosso dal codice. |

### In corso / in validazione (2026-06-03)
- **DiverseSMP wider-only** (`DiverseSMP`, bias≤0 = solo riduzioni, niente hijack) — **BAKATO default ON
  (2026-06-04, bake-on-trust).** Storia @8t: +9.7 LOS94%@860 (picco) → +4.8@1080 → **+3.42 LOS74%@1220** finale.
  SEMPRE positivo (+3..+10, MAI negativo) su ~1440 partite 4t+8t. Feature **SMP**: inerte a 1t, agisce solo
  multi-thread (= condizione gauntlet/torneo reale). Il [0,5] SPRT NON può confermare un ~+3 (valore<H1=5 → LLR
  a zonzo) → deciso **bake-on-trust** col segno robusto. Toggle conservato A/B (off=baseline), amount=1.
  (NB: forma simmetrica ±bias = MORTA, −79 @8t per hijack.)
- **DeeperShallower** (`DeeperShallower`, default OFF) — **IMPLEMENTATO 2026-06-03, SPRT IN CORSO.** Porting
  fedele dello Step-17 SF: dopo che la re-search ridotta (LMR) batte alpha, la re-search a piena profondità
  va **+1 ply** se lo score supera `best+52` (mossa chiaramente migliore) o **−1 ply** se appena sopra alpha
  (`<best+9`). OFF = re-search fissa a depth-1 (byte-identico → A/B pulito). Smoke test @d18 startpos: stesso
  eval (67cp) ma **~½ dei nodi** (548k vs 1119k) → riduzione nodi sostanziale. SPRT @8+0.08 Threads=1 Hash=128
  con adjudication (resign mc=4 s=650 twosided + draw mn=40 mc=8 s=10). **ON/OFF @8+0.08: +24@58 → +17@140 →
  −3.2@324 (decadimento monotòno) = NEUTRO ai default SF, NON un win.** NON un NO-GO secco (−3.2±19 = indistinguibile
  da 0) e dimezza i nodi a pari depth → c'è efficienza che non converte, sospetto **doShallower (margine 9) over-prune**.
  **Margini ESPOSTI come spin** `DeeperMargin`(52)/`ShallowerMargin`(9). **LINEA CHIUSA = NO-GO (tutte le varianti ≤0):**
  doDeeper-only −19 LOS4%@198, doShallower-only −10.5 LOS7.8%@598 (NEGATIVO confermato), full −3@324, stack(+ttPv) neutro +2..5±17@540. Il "+16 per
  sottrazione" era illusione (±29). Tutto dietro toggle default OFF, dormiente.
- **ttPv** (`TTPv`, default OFF) — **IN IMPLEMENTAZIONE 2026-06-03.** #2 dei delta SF (dopo DeeperShallower).
  Bit della TT (bit 63 del `data`, **era libero**) che ricorda se un nodo è/è stato PV; i nodi non-PV che la TT
  marca come ex-PV vengono **ridotti meno** in LMR (`reduction--`). Flag propagato negli store. OFF = bit sempre
  0 + LMR invariata (byte-identico). **NEUTRO, ARCHIVIATO** (+2.4±31@144 solo; +2..5@540 nello stack). Toggle default OFF.

- **Candidati "notte" (2026-06-04)** — 5 toggle default OFF (byte-identico verificato: tutti OFF = 1.118.617 nodi @d18
  = baseline), exe `Triumviratus_night_test.exe`, test automatici in `night_tests/` (`test_one.bat <OPT>` = SPRT pieno;
  `screen_all.bat` = screening ~800 partite/cad sequenziale): **NMPEvalScale** (R null-move += min((eval−beta)/div,3)),
  **QFutility** (futility per-mossa in qsearch, margine QFutMargin), **RFPDepth8** (reverse-futility depth≤8 vs 6),
  **RazorDepth4** (razor depth≤4 vs 3), **HistBonusSF** (bonus history lineare-clampato vs depth·depth). Spin tunabili
  esposti (NMPEvalDiv/QFutMargin/HistBonusMult/Sub/Max). Aspettativa ≤0 (search spremuto) ma **zero-rischio** → screening
  low-cost; i promettenti (LOS>90% @800) → `test_one.bat` SPRT pieno → bake.
  **ESITO SCREENING (α=β=0.20):** 🟢 **HistBonusSF +24.06 Elo LOS 99.99% LLR PASS @810 → BAKATO default ON (2026-06-04)** —
  primo +Elo di ricerca dopo una settimana (toggle conservato per A/B; spin HistBonusMult/Sub/Max=155/90/1600 da SPSA-are);
  🔴 QFutility −71@316, RazorDepth4 −23@768, RFPDepth8 −13@1282, NMPEvalScale −6@2402 = negativi (toggle OFF, dormienti);
  ⚪ ttPv +3.5@398 neutro. Deploy bake: rebuild exe reale + commit repo sacro.

> 🔬 **DA INDAGARE (perché i 4 negativi non hanno funzionato):** sono TUTTI tecniche che **aggiungono potatura/riduzione**
> (QFutility=futility qsearch, RazorDepth4/RFPDepth8=potatura più profonda, NMPEvalScale=null-move più aggressivo) → tutti
> ≤0. L'UNICO che ha funzionato (HistBonusSF) **non pota: cambia l'ordinamento/magnitudine history.** Insight forte: a ~3570
> il motore è **alla frontiera della potatura** (più pruning = sovra-pota, perde tattica), mentre c'è ancora margine
> sull'**ordinamento delle mosse**. Ipotesi specifiche: QFutility −71 = margine/base futility sbagliato per la nostra scala
> eval o conflitto con SEE<0 (rivedere `g_qfut_margin`/futility-base); Razor/RFP depth = margini `g_razor_*`/`g_rfp_margin*d`
> non tarati per le depth estese. **Direzione futura: cercare +Elo in ordering/history (capture-history, conthist, killer/counter
> tuning), NON in altra potatura.**

> ⛔ **SEARCH SPREMUTO (verdetto 2026-06):** una settimana di tweak — wider, DeeperShallower (×3 varianti), ttPv, stack —
> TUTTI ≤0. A ~3570 Elo la ricerca è near-ottima; le tecniche SF residue (+3..8) stanno SOTTO il pavimento di rumore SPRT
> del budget-macchina. **La leva Elo non è più il search.** Pivot → rete NNUE (training cloud) + NPS (static-eval-in-TT,
> validato con A/B NPS interlacciato che SFUGGE al pavimento) + enabler (ASAN, bake toggle validati, commit MultiCut).

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
| **🎯 DeeperShallower** (SF Step-17 doDeeper/doShallower sulla re-search LMR) | +0..8 | basso/basso | **IMPLEMENTATO (2026-06-03), SPRT IN CORSO** @8+0.08 Threads=1 (`DeeperShallower` off vs on, con adjudication). Re-search ±1 ply su forza del fail-high (`best+52`/`best+9`). OFF byte-identico. Smoke @d18: ~½ nodi a pari eval. |
| **🎯 ttPv** (flag PV nella TT, bit 63; LMR ridotta meno su nodi ex-PV) | +3..6 | basso/medio | **IN IMPLEMENTAZIONE (2026-06-03).** #2 dei delta SF. OFF byte-identico (bit 0 + LMR invariata). **PROSSIMO SPRT dopo DeeperShallower.** |
| ~~**SPSA Phase-2 MovePicker**~~ (SEECaptureMargin/SEEQuietMargin + futility + HistPruneMargin) | +2..10 | basso/basso | **FATTO = NO-GO.** Vettore tunato +0.8±12.9 @8+0.08 (= zero), −17 @5+0.05. Tenuti i DEFAULT SEE/Futility/HistPrune. Lezione: validare SEMPRE l'output SPSA come blocco vs default. |
| **AggrLMR** | +0..8 | basso/basso | Ai default (2048/3) = **NEUTRO** (0.1 ±10, LOS 51% @2650 partite). Non parcheggiato: i param `AggrLMRDiv`/`AggrLMRClamp` entrano nella **SPSA unificata** (frozen on) per vedere se un'aggressività diversa è +. Se restano fermi → neutro confermato. |
| ~~**LMRBase / LMRDiv**~~ (formula core LMR) | +? | basso/basso | **FATTO (v3.4.1)**: LMRBase 75→47, LMRDiv 225→270, +19.95 Elo. |
| ~~**Re-SPSA margini + search/history/aspiration**~~ | +3..10 | basso/basso | **FATTO (v3.4.2)**: +11.41 Elo LOS 99.51% @3+0.03. 18 parametri tunati, 9 bakati. |
| **LMR enrichment + retune razor/NMP** | +2..8 | basso/medio | Condizioni LMR staccate (TT-move, tt-depth, quiet-count) + margini razor/NMP al TC giusto. |

### Tier B — Medie (più sforzo, Elo moderato)
| Voce | Elo | Rischio/Compl. | Note |
|---|---|---|---|
| **Singular ext avanzate** (triple ext, multi-cut) | +2..6 | medio/medio | |
| **CorrHistMulti / ContHist 2-4ply RIFATTE con pesi** | +0..8 | medio/medio | Le parcheggiate, fatte giuste (pesi per tabella/ply). Sotto la soglia di misura → solo se Tier A esaurito. |
| ~~**Lazy MovePicker** (movegen stadiato)~~ | +5..12 | **alto/alto** | **FATTO (v3.5): +16.6 Elo, LOS 96.6% @5+0.1.** Rewrite del move loop + validatore TT-move (`td_is_pseudo_legal`) completato; Phase 2 (skip_quiets/skip_bad_caps) attiva. Resta da spremere via SPSA Phase-2 (Tier A). |

### Tier C — Abilitatori / robustezza (no Elo diretto, ma necessari)
| Voce | Elo | Rischio/Compl. | Note |
|---|---|---|---|
| **ASAN + fix crash ~1/800** | 0 | basso/medio | Niente Elo ma alto valore torneo/release. Fallo prima di tornei seri. |
| ~~**🔑 Self-play data gen + pipeline training**~~ (PGN→plain→binpack→nnue-pytorch→.nnue) | 0 | medio | **VALIDATA (2026-06).** Catena end-to-end provata: 12,8M pos d12, training L1=2560 (commit 2db3787), val_loss pulito, rete che carica nel motore. Ricetta in `memory/project_nnue_training.md`. Resta da SCALARE i dati (vedi Tier E). |
| **Pulizia codice** (policy morta, ABDADA, toggle parcheggiati) | 0 | basso | Manutenibilità. **Bake-in candidati** (default-true validati, l'SPSA NON li tocca → rimuovere l'opzione UCI, hardcodare ON, comportamento identico = solo rebuild, no SPRT): `EvalCache`, `Improving`, `SingularExt` (tieni spin SingularDoubleMargin), `ProbCut` (tieni spin ProbCutMargin), `LazyEval`. **NON bakare**: `MovePicker`/`AggrLMR` (l'SPSA li setta), `NodeTM`/`TimeMgmt` (assicurazione anti-bandiera), `CorrHist`/`ContHistPrune` (provvisori non validati), i default-false diagnostici. `LazySMP` bake = cancellare ABDADA (job separato). |

### Tier D — Ricerca (il focus originale del progetto)
| Voce | Elo | Rischio/Compl. | Note |
|---|---|---|---|
| **Rivivere policy net int8/più piccola** | +? | alto/medio | Distinta dalla NNUE-eval. Ora −85 in ricerca; int8/rete piccola può ribaltarla. |

### Tier E — Rete proprietaria (in fondo PER SCELTA, dipende da tutto il resto)
| Voce | Elo | Rischio/Compl. | Note |
|---|---|---|---|
| **🏆 Rete NNUE proprietaria** | **~0 sul net, valore = ownership** | alto/alto | **REINQUADRATA (2026-06).** La nostra eval **è già near-SOTA: è una rete SF di alto livello.** Riallenare la stessa arch L1-2560, anche su miliardi di posizioni, al meglio la **eguaglia, non la supera** → aspettarsi Elo da qui è un errore. Vale per **indipendenza/identità/licenza**, non per forza. Calo iniziale fisiologico. **RUN CLOUD T80 IN CORSO (2026-06-03):** rete "rubicon" su GCP L4 (g2-standard-4), **16 mesi T80 v6 ~138GB**, 600 epoche, ~$104; val_loss in discesa pulita (~ep9, zero overfit). Dettagli/comandi in `DataGen_Local/NNUE_Training_Pipeline/03_cloud_ubuntu/STATO_TRAINING_CLOUD.md`. A fine: serialize→scp→A/B (atteso ≈ pari rete SF). |

> **Nota strategica sulla rete (decisiva, da non dimenticare):**
> - **La leva Elo è il SEARCH, non la rete.** Prova: MovePicker +16.6, singular +34, SPSA +19.95/+11.41.
>   Finché l'eval è un net SF top, l'Elo si prende in ricerca (Tier A-B), non riallenando l'eval.
> - **Tetto di distillazione**: le label vengono da depth-12 *con la rete SF* → la rete allenata copia,
>   non batte. Per superare servono label MIGLIORI (search più profondo, o evaluatore più forte).
> - **Via realistica al volume (piano "T80")**: scaricare **binpack pubblici Leela/SF (T80)** = miliardi
>   di posizioni con label forti, già nel formato del nostro nnue-pytorch. È l'unico modo di saturare
>   la L1-2560 senza Fishtest.
> - **Il "data mixing" NON dà identità**: i nostri 12,8M in 2-3 miliardi = ~0,5% → statisticamente
>   invisibili. Per specializzare sul nostro motore serve **fine-tuning FINALE** solo sui nostri dati
>   (ad alta profondità), non mescolare e basta.
> - **Ordine corretto**: prima spremere il search (Tier A-B) così il self-play futuro è generato dal
>   motore più forte → label migliori. La rete proprietaria resta in fondo: vale tanto come *progetto*,
>   ma è ownership, non un +Elo a breve.

### Tier F — Speculativo, a costo-ZERO runtime (ultima voce, idea aperta)
| Voce | Elo | Rischio/Compl. | Note |
|---|---|---|---|
| **Policy distillata OFFLINE nelle euristiche di ordering** | +0..? | medio/medio | La policy in alpha-beta a RUNTIME è morta (−85: l'ordinamento è già quasi ottimo + costo della rete in un motore eval-bound → tenaglia accuratezza/leggerezza). Angolo che SCHIVA la tenaglia: allenare una policy (i ~14M bastano — è ranking, non centipawn) e usarla **offline** per **seminare/bias-are i priori delle tabelle history/butterfly/counter-move** del motore. Niente inferenza a runtime → **costo zero in ricerca**. Marginale e speculativo, ma è l'unico modo di sfruttare la policy *dentro* alpha-beta senza pagare velocità. Distinto dalla policy-in-search (Tier D, morta). |

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

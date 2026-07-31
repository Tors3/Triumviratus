# Triumviratus — Network lineage & training

Triumviratus ships its **own** NNUE network — weights trained by the project, **not** a Stockfish network.

To be precise about provenance (and GPL-honest): the NNUE **evaluation code** and the base network **architecture**
are Stockfish's (GPLv3 — see [`README`](README.md) and [`COPYING`](COPYING)); the official Stockfish **trainer**
([`nnue-pytorch`](https://github.com/official-stockfish/nnue-pytorch)) is used to train. The **training data is
mostly public** — Leela Chess Zero `test79`/`test80` binpacks, plus Stockfish-generated packs in one early stage,
plus a small slice of our own self-play. What is **ours** is the **network weights**: trained from scratch, with no
Stockfish network used as a seed or teacher, on a data mix and schedule we chose — and the extra input feature
blocks the architecture carries. On those two: the pawn-pair block was **invented by Jonathan Hallström for
Pawnocchio** and has since been adopted by Stockfish itself as `PP_3Wide` (SFNNv16) — our implementation and
weights are ours, the idea is his. **`PassedPawns` is original to this project.** This document records how
each shipped network was trained, for transparency and reproducibility.

---

## `rubicon-v1` — the Triumviratus 4.2 network

The project's **first own-lineage network**: 4.2 was the first build to ship a net trained entirely by us, with
**no Stockfish network shipped**.

| | |
|---|---|
| **Architecture** | `HalfKAv2_hm^`, **L1 = 2560** (the pre-threats Stockfish NNUE generation) |
| **Data** | Leela Chess Zero **T80** self-play data (Leela `test80`), ~16 months' worth |
| **Method** | trained **from scratch** with `nnue-pytorch` — batch 16384, ~400 epochs, **λ = 1.0** (pure score-distillation), `HalfKAv2_hm^` features, lr ≈ 8.75e-4 |
| **Finishing** | the shipped net was a refinement (a short **λ squeeze 1.0 → 0.75**) of the peak T80 checkpoint |
| **Hardware** | GPU on GCP |
| **Result** | ≈ **−39 Elo** vs the reference Stockfish net at game time control — the accepted "cost of going own-lineage" |

The point of `rubicon-v1` was **independence**, not beating the Stockfish net: a network whose *weights* are the
project's own, even at a strength cost. It made 4.2 the first own-network Triumviratus release.

---

## `rubicon-alea-v1` — the Triumviratus 5.0 network (SFNNv13, threats-aware)

> **Naming** (net file: `nn-rubicon-alea-v1.nnue`). `rubicon` is the own-lineage **family** — the project's
> self-trained networks. **`alea`** marks the new SFNNv13 *threats* generation: Caesar crossed the Rubicon (→
> `rubicon`) and said "*alea iacta est*", the die is cast (→ `alea`) — two halves of the same moment. **`-v1`** is
> the first training of this net; a later **Stage-3 refining** would ship as `rubicon-alea-v2`.

The project's **second own-lineage network**, on Stockfish's current-generation **SFNNv13** architecture, which adds
explicit **threat features** (which piece attacks which) to the evaluation.

| | |
|---|---|
| **Architecture** | `Full_Threats + HalfKAv2_hm^`, **L1 = 1024, L2 = 31, L3 = 32**, 8 LayerStacks / PSQT output buckets (SFNNv13) |
| **Recipe** | the official Stockfish `vondele/nettest/threats.yaml`; trainer `nnue-pytorch` (master), optimizer **rangerlite**, one-cycle LR, factorized weight-decay, early/random fen-skipping |
| **Stage 1** | Stockfish-generated 5000-node data (public `vondele` packs, ~136 GB) — lr **1.5e-3**, λ **1.0 → 0.75**, ~250 epochs, batch 65536 |
| **Stage 2** | Leela self-play binpacks (`test79` / `test80`, 2023–2024) — lr **1.3e-3**, λ **0.74**, resumed from Stage 1 |
| **Hardware** | GCP **g4** (RTX PRO 6000 Blackwell) |
| **Goal** | close the gap to the strongest Stockfish net while keeping the weights own-lineage; the staged SF-data → Leela-data schedule is exactly how Stockfish trains its own nets |
| **Result** | ≈ **−40 Elo** vs the strongest Stockfish net at long TC (gap halved from −84 at end of Stage 1 — own nets plateau below Stockfish). In the engine, Triumviratus 5.0 with this net measures **≈ +50 Elo over 4.2** (internal self-play, 20+0.2) |

Trained-by-us **weights** on Stockfish's **architecture** and **trainer** — same honest position as `rubicon-v1`,
one NNUE generation newer.

---

## `rubicon-alea-v2` — the Triumviratus 6.0 network (Stage-3 refining + PawnPair graft)

> **Naming** (net file: `nn-rubicon-alea-v2.nnue`). The **Stage-3 refining** foretold by `-v1`: same own-lineage
> `alea` family, refined on the project's **own** self-play data and extended with a new input-feature block.

The project's **third own-lineage network**. It is a **fine-tune of `rubicon-alea-v1`** (not a from-scratch train —
exactly how Stockfish refines its nets between architecture changes), with two changes over v1: it is trained
partly on **Triumviratus's own self-play data**, and the input is extended with a grafted **PawnPair** feature block.

| | |
|---|---|
| **Architecture** | `Full_Threats + HalfKAv2_hm^ + **PawnPair**`, **L1 = 1024, L2 = 31, L3 = 32**, 8 LayerStacks (SFNNv13 + own PawnPair block) |
| **Graft** | the PawnPair feature block is **zero-initialized and grafted** onto the trained v1 weights — the graft is bit-identical to v1 at init (verified: same node signature, zero eval mismatch), so training starts from v1's strength and *adds* the new signal rather than relearning |
| **Method** | **fine-tune from `rubicon-alea-v1`**; `nnue-pytorch`, batch 65536, epoch-size 100M, **λ = 0.75** constant, one-cycle gamma 0.997 |
| **Two-group LR** | base weights **1e-4** (gentle refine, no forgetting); the zero-init PawnPair block **1e-3** (10× — it starts from nothing and must learn fast) |
| **Data** | **Leela T80** (full 2022, unseen by v1, + part of 2023 — ~327 GB) **plus the project's own self-play** (~67 M positions from 5.1 / v1 self-play), mixed at ≈ 6.5 % own (`OWN_REPEAT=10`) — SF-style "add own data to the refine", not replace |
| **Run** | planned 800 epochs; **validation loss flat from ~ep80** (train ≈ val ≈ 0.0036: converged to the label-noise floor, no overfit) — checkpoints ep299–ep459 statistically equal in play (round-robin within noise) → the run was **stopped at ~ep470** instead of burning ~340 flat epochs |
| **Finishing** | **LR-anneal fine-tune** from ep459 with a fresh, steeper decay (base 2.5e-5 / PawnPair 2.5e-4 — the LR the main run had reached — γ = 0.95, ~50 epochs), then **over_last**: the shipped weights are the **average of the last 3 anneal checkpoints** — the same tail-averaging recipe that produced v1's best net. Validated net-isolated vs the plain ep459 checkpoint: **+3.8 ± 7.7 Elo** over 2004 games (15+0.15) — parity with a positive lean, so the variance-reduced average is what ships |
| **Serialization** | shipped **FT-permuted** (feature-transformer neuron reordering for sparsity) — eval **bit-identical**, ≈ +1.6 % NPS for free |
| **Hardware** | GCP **g4** (RTX PRO 6000 Blackwell), spot instance |
| **Result (net-isolated)** | v2 vs v1 on the **same** engine — during training, 10+0.1: **+13.3 Elo** (ep199) → **+16.1 Elo** (ep219). **Final shipped net vs v1, 20+0.2: +18.27 ± 9.94 Elo (LOS 99.98 %, 1104 games)** — measured against v1 with both new blocks zero-grafted (architecturally v3-format, evaluation bit-identical to v1), so a single binary plays both sides and the comparison isolates the network |
| **Result (release)** | **Triumviratus 6.0 + v2 vs 5.1 + v1**, long TC (20+0.2): **+30.9 ± 9.6 Elo** (LLR passed H1, 1208 games) — net refine + 6.0 search gains combined |

Same honest position as the rest of the `rubicon` family: **our weights**, our data, on Stockfish's architecture
and trainer. The PawnPair block is our own extension to the SFNNv13 feature set.

---

## `rubicon-alea-v3` — the PassedPawns network

> **Naming** (net file: `nn-rubicon-alea-v3.nnue`). Same own-lineage `alea` family, extended with a
> second grafted input block: **passed pawns**.

The project's **fourth own-lineage network**, and the second extension of the SFNNv13 feature set with
a Triumviratus-specific block. Where `PawnPair` gave the network pawn-*structure* geometry, `PassedPawns`
gives it the single most documented NNUE blind spot: **which pawns are passed** — a relational property
that requires combining the enemy pawn configuration across three files, and that `HalfKAv2_hm`
(king-relative, per-piece) cannot express directly.

| | |
|---|---|
| **Architecture** | `Full_Threats + HalfKAv2_hm^ + PawnPair + **PassedPawns**`, **L1 = 1024, L2 = 31, L3 = 32**, 8 LayerStacks (SFNNv13 + two own blocks) |
| **Feature space** | **96 features** — one per passed pawn: 48 oriented squares × {own, enemy}. "Passed" = no enemy pawn on the same or adjacent files ahead, **and** no own pawn directly ahead on the same file. Deliberately **square-only**: blocked / king-supported / connected flags were considered and rejected — the first two are already learnable through the SFNNv13 pairwise-multiplied L1 against the `HalfKA` and `PawnPair` blocks, and "blocked" would have broken the pawn-event-only incremental update |
| **Graft** | zero-initialised and grafted onto the finished v2 weights — bit-identical to v2 at init (verified end-to-end: same bench signature through graft → serialize → engine reader), so training starts from v2's strength and *adds* signal |
| **Method** | **frozen-base screening**: the pre-trained network is frozen (`lr = 0`) and **only the new block trains** (`lr = 1e-3`). Intended as a cheap go/no-go probe before a full fine-tune — it turned out to be the whole training |
| **Training length** | **~4 epochs.** The block is only ~99 k parameters (50× smaller than `PawnPair`) and saturates almost immediately: checkpoints at ep9 and ep14 measured **no better** than ep4 (ep9 vs ep4: −0.32 ± 10.29, parity), so ep4 is what ships. A full two-group fine-tune was never needed |
| **Serialization** | shipped **FT-permuted** (`ftperm`, which reorders the L1 neurons so ReLU zeros cluster and the sparse-input affine path pays off). Verified in the 6.0 pre-release audit: the permutation covers **all four** input blocks (`Full_Threats` 60720, `HalfKAv2_hm` 22528, `PawnPair` 4560, `PassedPawns` 96) — the point that matters, because permuting only the SFNNv13 columns and not the grafted ones would make the net *wrong*, not merely slow — and the evaluation is **bit-identical** to the plain net (identical bench signature, with a valid negative control). Speed benefit: **≈ +1.6 % NPS** — median of per-position ratios, **55 of 60 positions faster**, quartiles +1.0 % to +2.3 %, measured A/B-interleaved on an idle machine at identical node counts. A prior reading of −0.34 % that had led to retracting this figure was itself the artefact of a **non-interleaved** harness, which runs every position on A and only then on B and so charges any drift in machine state to the engine |
| **Engine** | the block folds into the existing threat accumulator (no new SIMD path), reuses the `PawnPair` pawn-event incremental trigger, and the reader is **dual-format** — the same binary loads v2 and v3 nets |
| **Result (net-isolated)** | v3 vs v2 on the **same** engine, 15+0.15: **+6.96 ± 6.56 Elo (LOS 98.1 %, 2596 games)** — three independent reads, all positive, LOS rising (94 % → 91 % → 98 %). Cumulative vs `rubicon-alea-v1`, 20+0.2: **+15.14 ± 7.69 Elo (LOS 99.99 %, 1998 games)** — measured against v1 with both new blocks zero-grafted, so one binary plays both sides |

The economics are the point: **~4 epochs of training on a 99 k-parameter block bought ≈ +7 Elo**, on a
network that had otherwise hit its data ceiling (see the v2 entry — 400 epochs of flat validation loss).
Adding *information the network cannot infer* beat adding *more training on information it already had*.

Also worth recording as a **negative result**: a data-enrichment experiment (a dataloader filter that
streams only positions containing passed pawns, so the block trains on a distribution where it is
actually active instead of diluted) did **not** improve on the plain ep4 checkpoint. The filter works
and costs almost nothing (~3 % throughput), but the block had already converged.

---

## `legio-septima` — the Triumviratus 7.0 network

> **Status: training complete, network chosen — no release yet.** Stage 1 (479 epochs) and stage 2
> (800 epochs) are done; a stage-3 annealing tail was run and closed at zero. The network is the
> stage-2 final, **epoch 799**. **Triumviratus 6.0 with `rubicon-alea-v3` remains the current release.**

The `rubicon-alea` lineage ends with 6.0. `legio-septima` changes both the architecture and the
training method — hence a new lineage and a new name.

| | 6.0 — `rubicon-alea-v3` | 7.0 — `legio-septima` |
|---|---|---|
| Base architecture | SFNNv13 | **SFNNv16** |
| L2 | 31 | 32 |
| `Full_Threats` | 60,720 | **59,808** |
| Pawn-pair block | ours, 4,560 | **identical** — it is Stockfish's `PP_3Wide` |
| `PassedPawns` | 96 | 96 (still ours; Stockfish has no equivalent) |
| **Total inputs** | 87,904 | **86,992** |
| Training | **graft** — the base net is frozen (`--lr 0`) and only the new feature block learns | **full training**, 500 + 800 epochs |

**Why the input count goes down.** Stockfish's SFNNv16 removed the pawn→pawn threat features and the
pawn-pusher inputs, because the pawn-pair block already covers every pawn–pawn interaction — keeping both
means paying twice for the same information. We had both; now we don't.

**The change that actually matters is not in that table — it is how the weights are made.** Grafting
worked: v2 and v3 together are worth **+15.14 ± 7.69 Elo** over v1, for a few epochs of training on
one small block. Its limit is structural — a frozen base can only *add* what the new block expresses,
never re-learn what the rest already believes. `legio-septima` trains base and blocks together, from
scratch.

### Stage 1 data

All five packs come from **[`vondele/master-binpacks_relabel`](https://huggingface.co/datasets/vondele/master-binpacks_relabel)**
— public Stockfish self-play, **re-labelled with Leela's BT4 network** so the whole stage shares one label
scale. Mixing BT4 labels with search-Stockfish labels would put two different target distributions in the
same training run.

| binpack | size | what it is |
|---|---|---|
| `nodes5000pv2_UHO.relabel-BT4-tf13tune.binpack` | 41.1 GiB | self-play, 5000 nodes, UHO openings |
| `dfrc_n5000.relabel-BT4-tf13tune.binpack` | 38.2 GiB | **DFRC / Fischer random** |
| `multinet_pv-2_diff-100_nodes-5000.relabel-BT4-tf13tune.binpack` | 28.4 GiB | multi-net labelling |
| `wrongIsRight_nodes5000pv2.relabel-BT4-tf13tune.binpack` | 7.3 GiB | positions where the eval is wrong |
| `fishpack32.relabel-BT4-tf13tune.binpack` | 5.5 GiB | misc |
| **total** | **121 GiB** | ≈ 50 G positions |

The trainer interleaves them at chunk level — the datasets are positional, so no pre-merge step is needed.

**Recipe.** `batch 131,072` · `lr 2.47e-3` · `gamma 0.990` · lambda **1.0 → 0.75** across the run ·
`random-fen-skipping 3` · `epoch-size 100 M`.

- Ran to **epoch 479** of a planned 500 — **47.9 G positions**, ≈ one pass over the corpus.
- Batch and lr are 8× and `√8`× Stockfish's published `16,384 / 8.75e-4`. Raising the batch amortises
  the DDP all-reduce, which is a **fixed** per-step cost; the lr is scaled to match.
- Stopping at 479 costs nothing measurable: lr was already at 1.2 % of its initial value, and no
  change was detectable between epochs 249 and 416.

### Stage 2 data

Leela-derived, **21 binpacks, 423 GB**. Everything from
[`vondele/*_relabel`](https://huggingface.co/vondele) is **re-labelled with BT4**, so it shares one
label scale with stage 1.

| binpack | files | size | source |
|---|---|---|---|
| `T60T70wIsRightFarseerT60T74T75T76.split_0…4.relabel-BT4-tf13tune` | 5 | ~105 GB | `vondele/from_kaggle_2_relabel` |
| `leela96-filt-v2.min.split_0…4.relabel-BT4-tf13tune` | 5 | ~95 GB | `vondele/from_kaggle_1_relabel` |
| `test80-2022-{jun,jul,aug,sep,oct,nov}-16tb7p.v6-dd[.min].relabel-BT4-tf13tune` | 6 | ~126 GB | `vondele/linrock_relabel_1` |
| `test78-2022-{01-to-05-jantomay,06-to-09-juntosep}-16tb7p.v6-dd.min.relabel-BT4-tf13tune` | 2 | ~31 GB | `vondele/linrock_relabel_1` |
| `test77-2021-12-dec-16tb7p.v6-dd.min.relabel-BT4-tf13tune` | 1 | ~18 GB | `vondele/linrock_relabel_1` |
| `T91-2026-{May,June}-6p-bp` | 2 | ~31 GB | `jshriver/t91-binpacks` |

**T91 is the one exception, and deliberately so.** It is *not* re-labelled: T91 is the Leela run that
*produces* the BT4 nets, so its labels come from that run's own search — at **800 visits** against the
10,000 of the re-labelled T80. Only the two most recent months are used (quality rises month by month),
which keeps it to 2 files of 21.

**Excluded on purpose:** `test60-2021-{nov,dec}` and `test79-2022-{apr,may}`. The loader picks a file
**uniformly at random**, not proportionally to size, so a 3 GB binpack would be traversed ~13× and its
positions repeated — exactly the correlation `random-fen-skipping` exists to avoid. Below ~9 GB a file
costs more in repetition than it adds in coverage.

**Recipe.** `batch 131,072` · `lr 1.237e-3` · `gamma 0.995` · `random-fen-skipping 3` ·
`epoch-size 100 M` · **800 epochs** · seeded from the stage-1 weights with a fresh schedule.

- **Lambda 0.79 → 0.75 over the first 100 epochs, then fixed.** Stage 1 annealed lambda across its
  whole run, so the target was still moving at the end — when lr was 3.8e-5 and the model could no
  longer follow it. Stage 2 resumes at 0.79, finishes the shift while lr is still ≥ 61 % of initial,
  and spends the remaining 700 epochs on a target that does not move.
- ⚠️ **`val_loss` is not comparable across epochs while lambda anneals.** The loss is computed against
  the *current* lambda: as weight moves onto the game result, the achievable floor rises by
  construction. It climbed 0.00331 → 0.00352 here while the network was measurably improving.
  Corollary: **`--save-top-k` by `val_loss` is unusable** in an annealed run.

### Stage 3 — annealing tail (run and closed)

Stage 2 ended with lr at 2.24e-5, so a natural question was whether the schedule had simply
been cut short. A third stage extended the annealing tail — 350 epochs planned, lr 3.36e-5 →
2.0e-6, lambda fixed, corpus weighted per position instead of per file.

**It produced nothing.** Stopped at epoch 169 and measured against its own starting point:

> **−0.28 ± 5.21 Elo** over 5024 games, 15+0.15, same binary, only the `.nnue` swapped.

Zero with the tightest interval of the whole project. Three earlier checkpoints agreed
(−3.37, −3.54, −1.09, all at ±9–11). The shipped network is therefore the **stage-2 final**,
epoch 799.

<sub>Worth recording because it was not obvious in advance: at that point the network showed
no overfitting at all — `val_loss` sat at or below `train_loss` for the entire run — and less
than half the corpus had ever been delivered. Neither data nor overfitting was the limit.</sub>

### Where it stands

Stage 1 and stage 2 complete; stage 3 run and closed at zero. The shipped network:

> **+23.41 ± 9.22 Elo** vs Triumviratus 6.0 with `rubicon-alea-v3` — 1442 games, 15+0.15, LOS
> 100 %, nElo +45.65, 1 thread, 64 MB, UHO_4060_v4, both engines PGO + AVX-512, two-sided resign
> 650 / draw 10 cp.

Measured on the 7.0 binary **frozen before** any search change, so the figure isolates the network.

| stage-2 epoch | date | TC | games | Elo | LOS |
|---|---|---|---|---|---|
| 189 | 2026-07-29 | 12+0.12 | 802 | +13.00 ± 12.72 | 97.75% |
| 229 | 2026-07-29 | 15+0.15 | 942 | +8.85 ± 11.82 | 92.91% |
| 263 | 2026-07-29 | 15+0.15 | 1678 | +13.88 ± 8.58 | 99.92% |
| 370 | 2026-07-30 | 15+0.15 | 1180 | +17.09 ± 10.55 | 99.93% |
| 659 | 2026-07-30 | 15+0.15 | 378 | +17.48 ± 18.71 | 96.68% |
| 696 | 2026-07-30 | 15+0.15 | 2138 | +28.01 ± 7.72 | 100 % |
| **799** (final) | 2026-07-30 | 15+0.15 | 1442 | **+23.41 ± 9.22** | 100 % |

<sub>Rows 189–659 are mutually indistinguishable at their error bars. The last two overlap heavily
(+28.01 and +23.41, intervals `[20.3, 35.7]` and `[14.2, 32.6]`) and the direct paired match between
those two networks — a much tighter measurement — says 799 is the *stronger* of the pair by
+5.18 ± 6.03. Reading a trend from this column is not supported: at ±9 Elo a 1000-game match cannot
separate +20 from +30.</sub>

The last 103 epochs of stage 2, measured directly (same binary, only the `.nnue` swapped, 128 threads):

| | TC | games | Elo | LOS |
|---|---|---|---|---|
| epoch 799 vs epoch 696 | 15+0.15 | 3892 | **+5.18 ± 6.03** | 95.4 % |

<sub>Not significant on its own, but it is the point estimate the stage-3 projection rests on:
+5.18 over 103 epochs is 0.050 Elo/epoch, the steepest window of the run.</sub>

Progression against the end of stage 1 — same binary, only the `.nnue` swapped:

| stage-2 epoch | TC | games | Elo |
|---|---|---|---|
| 19 | depth 15 | 494 | −27.49 ± 16.24 |
| 39 | depth 15 | 1000 | −6.25 ± 11.06 |
| 119 | 15+0.15 | 500 | +1.39 ± 15.17 |
| 176 | 15+0.15 | 1200 | +0.87 ± 10.33 |

<sub>The first 40 epochs are **recovery, not progress**: stage 2 restarts at an lr 32× higher than
where stage 1 ended, which moves the model off its converged minimum before it re-converges on better
data. The +21 Elo between epochs 19 and 39 is significant (z = 2.16).</sub>

<sub>Earlier stage-1 figures of −64.97 (epoch 54) and −21.95 (epoch 111) are **not** comparable to each
other: between them fell both 57 epochs of training and the discovery that the 7.0 Windows project was
not defining `USE_AVX512`, so the earlier build was effectively AVX2 against a PGO opponent.</sub>

---

## What "own-lineage" means (and does not)

- **Means:** the network *weights* shipped with Triumviratus are trained by the project, **from scratch**, with
  **no Stockfish network used as a seed or teacher**. Every training run in the table above starts either from
  random initialisation or from one of our own earlier networks. The evaluation values are ours.
- **Does not mean: our own data.** The training data is overwhelmingly **public**: Leela Chess Zero `test79`/`test80`
  binpacks make up the bulk of every run, and the first stage of `rubicon-alea-v1` used **Stockfish-generated**
  5000-node data packs (the public `vondele` sets, ~136 GB). The project's own self-play contributes a small
  minority — roughly **6.5 %** of the `rubicon-alea-v2` mix (~67 M positions, repeated ×10 against ~327 GB of
  Leela data). Calling the data "ours" would be wrong; what is ours is the training, the mix, and the resulting
  weights.
- **Does not mean:** independence from Stockfish *code*. The NNUE inference and the base network *architecture* are
  Stockfish's (GPLv3) — extended with our own input blocks — and the trainer is Stockfish's `nnue-pytorch`. The whole
  project is GPLv3 and credits Stockfish accordingly (see `README` / `COPYING`). Renaming code does not change this;
  the attribution is kept.
- **Why it still matters:** an own-lineage network plateaus measurably below the strongest Stockfish network
  (≈ −40 Elo for `rubicon-alea-v1`, less for later ones) — that gap is the accepted cost. What the project gets in
  return is a network whose weights, mix and feature extensions are its own rather than a redistribution of
  someone else's.

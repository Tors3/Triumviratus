# Triumviratus — Networks

Triumviratus ships its **own** NNUE network — weights trained by the project, **not** a Stockfish network.

To be precise about provenance (and GPL-honest):
- The NNUE **evaluation code** and the base network **architecture** are Stockfish's (GPLv3 — see
  [`README`](../../README.md) and [`COPYING`](../../COPYING)).
- The official Stockfish **trainer** ([`nnue-pytorch`](https://github.com/official-stockfish/nnue-pytorch)) is used
  to train, with our own feature blocks added.
- The **training data is public** — Leela Chess Zero and Stockfish self-play binpacks, re-labelled by the community.
- What is **ours** is the **network weights**: trained from scratch, with no Stockfish network used as a seed or
  teacher, on a data mix and schedule we chose.

The pawn-pair block was invented by Jonathan Hallström for Pawnocchio and is now Stockfish's `PP_3Wide`; our
implementation and weights are ours, the idea is his. **`PassedPawns` is original to this project.**

**Architecture names.** From 6.0 each network architecture of the project has a name, **TRANN** (*TRiumviratus
Architecture Neural Network*), with a number that grows at every change of architecture; a new training on the same
architecture keeps the number:

| name | network | release | architecture |
|---|---|---|---|
| `TRANN1` | `rubicon-alea-v3` | 6.0 | SFNNv13 (threats) + `PawnPair` + `PassedPawns` |
| `TRANN2` | `legio-septima` | 7.0 | SFNNv16 + `PassedPawns` |
| `TRANN3` | Consilium | 8.0 | TRANN2 with the HalfKA block split into four experts by total piece count |
| `TRANN4` | Consilium Plebis | 8.1 | int8 HalfKA split into eight experts per side, chosen by the opponent's non-pawn material, plus a pawn-king subnetwork |
| `TRANN5` | Lex Hortensia, the next network (designed, not in training) | after 8.1 | six experts per side with the queen as a boundary, 1024-byte rows with the PSQT in a small table, a gated pawn-king block, 32 layer stacks |

---

**Every network file, with its complete recipe** (binpacks, trainer patches, commands, scripts), is in the companion
repository **[Triumviratus-Networks](https://github.com/Tors3/Triumviratus-Networks)**.

## The next network — future training direction (not in training now)

No network is being trained now. This is the design the next training will follow, architecture `TRANN5`, named **Lex Hortensia**, and it
comes from the lessons of Consilium Plebis. The measures behind it are of two kinds:
- **offline studies on the released network,** judged on the game result over 179,008 quiet positions of 8.1 games
  (concordance: the share of position pairs with different results that the evaluation orders correctly);
- **xperf counts of the engine's cost per node** on a Xeon (large pages) and a Zen 4 laptop (4 KB pages).

<p align="center"><img src="next_network_design.svg" alt="Design of the next network" width="100%"></p>

| part | Consilium Plebis | next network | why |
|---|---|---|---|
| king-relative experts | 8, by the opponent's non-pawn material | **6**, the queen as a boundary: bare or one minor, one rook, at most one rook with up to two minors, two rooks, lone queen, queen with more | the endgame experts are the distinct ones and stay apart; the middlegame experts were 94-99% alike; with the queen as a boundary a queen middlegame never changes expert, so the expert changes (each a refresh) fall by about half |
| PSQT | 8 values at the end of every feature row (rows of 1088 bytes) | **a 24 KB piece-square table** per output bucket, outside the rows; rows of exactly 1024 bytes | the PSQT is half of today's evaluation, mostly material and piece-square values, which the table keeps; 1024-byte rows never cross a 4 KB page |
| pawn-king subnetwork | 256 hidden, in every evaluation | **128 hidden**, active only with pawns and at most one minor on the board, its mean output elsewhere | only the pure pawn endgames use it; a larger block shows no gain |
| layer stacks | 8, by piece count | **32**: piece count × which side has a queen (none, the opponent only, the side to move only, both) | free at run time; the experts carry the opponent's queen into each accumulator, the stacks see which side has one (32 rather than 16 decided on 08/11 as the safer choice for strength); about 2.1 MB in all |
| threats, pawn pairs, passed pawns, L1 / L2 / L3 | | unchanged (1024 / 32 / 32) | pawn pairs and passed pawns carry the pawn knowledge; L2 and L3 as in Stockfish |
| training target | nnue-pytorch's score-to-probability map | **unchanged** (studied on 08/11) | with offset and scale both free, the best map on our data is within about 0.2% of the current one in cross-entropy: not worth a change of the network's scale |

Measured on the released network, with routing-table copies of it (same engine, xperf, cycles per node): the six
experts of this design together with the pawn-king gate cost **3.7% less in the middlegame and 3.2% less in the
endgames on the Xeon**, and **5.3% / 3.0% less on the Zen 4**. The 1024-byte rows come on top. The size goes from
256 MB to about 204 MB.

## Earlier networks, in brief

The full record of each one — data, recipe, epoch-by-epoch measurements — is in the
[Triumviratus-Networks](https://github.com/Tors3/Triumviratus-Networks) repository, one recipe folder per network.

| Network | Release | Architecture | How it was made | Result |
|---|---|---|---|---|
| `rubicon-v1` | 4.2 | `HalfKAv2_hm^`, L1 2560 | from scratch on Leela T80 | first own net, ≈ −39 Elo vs the Stockfish net of the time |
| `rubicon-alea-v1` | 5.0 | SFNNv13 (threats), L1 1024 | two stages: Stockfish data, then Leela data | ≈ +50 Elo for 5.0 over 4.2 |
| `rubicon-alea-v2` | 6.0 | + `PawnPair` block | fine-tune of v1, `PawnPair` grafted at zero | +18 Elo over v1 (net isolated) |
| `rubicon-alea-v3` | 6.0 | + `PassedPawns` block | frozen base, only the new block trained (~4 epochs) | +7 Elo over v2 (net isolated) |
| `legio-septima` | 7.0 | SFNNv16, L1 1024 | full training from scratch, 479 + 800 epochs | +23 Elo over v3 (net isolated) |

---

## Consilium Plebis — the network of Triumviratus 8.1

> **File:** [`nn-consilium-plebis.nnue`](https://github.com/Tors3/Triumviratus-Networks/releases/download/consilium-plebis/nn-consilium-plebis.nnue)
> (255.7 MB, SHA-256 `b069fc08…`), the default network of 8.1, attached to the release
> [`consilium-plebis`](https://github.com/Tors3/Triumviratus-Networks/releases/tag/consilium-plebis) with its float
> weights; recipe, data, scripts and measurements in
> [`recipes/07_consilium-plebis`](https://github.com/Tors3/Triumviratus-Networks/tree/main/recipes/07_consilium-plebis).
> Trained 21-28 October 2026. **+10.3 ± 4.3 Elo over Consilium**, same search on both sides, 30k nodes per move.

The name comes from the *Concilium Plebis* of the Roman Republic, the assembly of the plebs. Here the plebs are the
pawns: in this network they get a subnetwork of their own, together with the two kings.

<p align="center"><img src="consilium_plebis_architecture.svg" alt="Consilium Plebis architecture" width="100%"></p>

### Why a new network

Consilium splits its king-relative block (HalfKA) into four input buckets, the "experts", chosen by the **total piece
count**, pawns included, and the same bucket serves both perspectives. Two things follow. A pawn more or less can
move a position to another bucket, and a position with a material imbalance (an exchange for two minor pieces, a
queen against two rooks) is read with the same bucket by both sides, although each side faces very different enemy
material. Consilium Plebis keys the buckets on what each side is facing.

### Architecture

Consilium Plebis is the architecture `TRANN4` (Consilium is `TRANN3`).

| part | Consilium (8.0) | **Consilium Plebis (8.1)** |
|---|---|---|
| feature set (trainer name) | `Full_Threats+HalfKAv2_hm_P4^+PP_3Wide+PassedPawns` | `Full_Threats+HalfKAv2_hm_X8^+PP_3Wide+PassedPawns` |
| king-relative block | 4 buckets by total piece count, shared by both perspectives | **8 buckets per perspective**, chosen by a **routing table** (learnable) over the non-pawn material |
| HalfKA weights | int16 | **int8** (merged weights clamped to the int8 grid during training) |
| threats, pawn pairs, passed pawns | int8 | int8 (unchanged) |
| L1 / L2 / L3, layer stacks | 1024 / 32 / 32, 8 stacks by (pieces - 1) / 4 | unchanged |
| pawn-king subnetwork | no | **yes**: 256 hidden units, its 32 outputs added to the first-layer outputs of the stack |

**Buckets and routing.** For each side a signature counts its knights, bishops and rooks (each capped at 2) and its
queen (capped at 1): 54 values. A perspective's cell is the pair (own signature, opponent's signature), 2,916 cells,
and a table maps every cell to one of 8 buckets. Pawn moves and pawn captures never change a cell, so the bucket
changes only on a non-pawn capture or a promotion, and only where the table says so. The table starts from the
opponent's non-pawn material (N = B = 3, R = 5, Q = 9) in eight classes: 0-3, 4-5, 6-8, 9, 10-13, 14-18, 19-25, 26-31.
It can be **learned during training** with a hard EM step (on a sample of positions, the loss of each perspective moved
to every bucket, summed per cell, decides the reassignment of cells, under a budget on how often a capture changes
bucket, since each change costs the engine an accumulator refresh), but in this run it moved under 1% of its cells:
in practice each side's bucket is set by the material it faces. The table is stored in the network file. Training
weights: a base per king bucket shared by all buckets, plus a delta per bucket, so a rarely used bucket falls back to
the base.

**Pawn-king subnetwork.** Inputs from the side to move's view: own pawns, enemy pawns, own king, enemy king (64
squares each, at most 18 active). h = clamp(b1 + sum of W1 rows, 0, 127/128); out = b2 + W2 h, added to the 32
first-layer outputs of the stack before its activations. The integer form is exact (the trainer keeps the weights on
the integer grid). The engine keeps a per-thread cache keyed by pawns, kings and side to move.

**File format.** As Consilium (header, feature transformer, 8 stacks), then a routing section (`RTB1`: 2,916 cells,
8 buckets, one byte per cell) and a pawn-king section (`PKN1`). As for Consilium, the first-layer neurons are permuted
at export so that the sparse first layer skips more zero blocks (+2.5% nodes per second, identical evaluations).

### Training

The BT4-relabelled corpus of Consilium (about 700 GB of binpacks); lambda 0.75 as the reference (reached early and
then held); piece-count sampling shifted towards the endgames so that the endgame buckets get more data. Before the
launch, a Python integer evaluator had to
match the trainer on exported networks, and the loader indices (cells, buckets, pawn-king inputs) had to match an
independent python-chess computation; after it, the engine had to match the Python evaluator move by move across
bucket changes.

The run, on two RTX 4090: a pretraining P in epochs of one billion positions, stopped after epoch 496 of the 550
planned while the loss was still falling; a finetune F of 60 epochs from those weights (a rise to 25% of P's peak
learning rate, a slow descent with a flat stretch at 15%, then down to P's final rate); and a continuation of 3 epochs
from F's final rate down to zero. The final network is the average of the weights of those 3 epochs, with its
first-layer neurons permuted on 2 million positions.

### Measurements

- **The final network against Consilium**, the same search source on both sides (built for each network), 30k nodes
  per move: **+10.3 ± 4.3 Elo** over 8,332 games (SPRT [0, 3] passed). Against the end of its own pretraining:
  +0.7 ± 2.8 over 18,332 games.
- **Loss on held-out data** (the same positions for every network): at epoch 435 Consilium Plebis was better than the
  final Consilium in every material bucket, by 2.9% at lambda 0.75 overall, most of all in endgames.
- **During the pretraining, against Consilium at 100k nodes per move:** +4.7 ± 3.5 Elo over 10,418 games pooling the
  exports from epoch 358 to 435, then +10.8 ± 5.5 at epoch 463 and +10.3 ± 5.6 at epoch 486.

---

## Consilium — the Triumviratus 8.0 network

> **Status: training finished on 29 September 2026.** The final network is the **average of the last five epochs of
> the second fine-tune (F4), with the feature transformer permuted**. With its network-dependent parameters re-tuned
> by SPSA, the 8.0 release build beats the official 7.0 by **+27.3 ± 8.3 Elo** (15+0.15, 2,000 games):
> see [the result](#the-result). **Consilium**, the council: four experts, one per phase of the game, that decide together. The sections below record the design, the choices and every
> measurement in the order they happened.

### Why a new network

`legio-septima` ended its run **saturated**:
- no gain between stage-1 epochs 249 and 416;
- a stage-3 annealing tail that measured zero (−0.28 ± 5.21 Elo over 5,024 games);
- validation loss never above training loss.

The conclusion was that the lineage needed more **capacity**, not more epochs. The obvious way to add it — a wider
L1 — is expensive at game time. Measured on our engine with random nets of each shape:

| option | NPS vs L1 = 1024 |
|---|---|
| L1 = 2048 | −20.0 % |
| L1 = 1536 | −11.2 % |
| **L1 = 1024, HalfKA as 4 experts (this net)** | **−7.5 %** |

<sub>L1 must be a multiple of 256 for the sparse affine path, so between 1024 and 1536 the only width available is
1280. It was considered and not measured: the choice was the cheapest option in NPS, with a MoE-1280 kept as the next
step if the Consilium gains little.</sub>

### The idea: a mixture of experts on the king-relative block

![TRANN3 architecture: Consilium](TRANN3_architecture_moe.svg)

The `HalfKAv2_hm` block gets **four weight sets**, one per material phase, chosen by the number of pieces on the
board. Only one set is active per position, so evaluation costs almost the same as a single block. The network gets
four times the parameters where a single set has to compromise most: how the value of a piece on a square changes
between the opening and the endgame.

What it is **not**: a network four times wider. The accumulator is still 1024 values and the layers after it are
unchanged (they already have 8 buckets by piece count). The experts specialise how those 1024 values are computed;
they do not enlarge them.

**Where the idea comes from, and what is new.** The idea is the author's own. It came from language models, where a
mixture of experts gives a model many parameters but only a small active part for each token, and it was designed
and built here without taking it from any chess engine. Consilium puts the experts where the parameters are, on the
king-relative input block, trains them as a shared base plus per-phase deltas on about 700 GB of public data (mostly
Leela games, re-labelled), and turns them into strength: with the parameters that depend on the network re-tuned,
the 8.0 build beat the 7.0 release by **+27.3 ± 8.3 Elo** at 15+0.15. To our knowledge it is **the first
mixture-of-experts network released in a top engine, and the first shown to gain strength**.

Related work, independent of this one: the layers after the accumulator come in eight buckets by piece count in
Stockfish's architecture and in most engines derived from it, a simple form of phase-dependent weights on the
smallest part of the network. And **Mark Tang**, as we learned after Consilium was released, had tried a network of
phase experts with fellow students in early 2026; it ran faster but did not play stronger, and it was not published.

| | 7.0 — `legio-septima` | 8.0 — Consilium |
|---|---|---|
| Base architecture | SFNNv16 | SFNNv16 |
| L1 / L2 / L3 | 1024 / 32 / 32 | 1024 / 32 / 32 |
| `Full_Threats` | 59,808 | 59,808 |
| King-relative block | `HalfKAv2_hm`, 22,528 | **`HalfKAv2_hm_P4`, 4 × 22,528 = 90,112** |
| `PP_3Wide` (pawn pair) | 4,560 | 4,560 |
| `PassedPawns` | 96 | 96 |
| **Total inputs** | **86,992** | **154,576** |

- **Phases** by piece count, kings and pawns included: **≤ 9, 10–15, 16–23, ≥ 24**. The cut points are
  equal-frequency bins measured on the corpus, so each expert sees roughly a quarter of the positions.
- **Trainer** (`HalfKAv2_hm_P4^`, our addition to `nnue-pytorch`): each expert's weight is a shared base plus a
  per-phase delta, plus the usual virtual (factorised) features — 162,768 training inputs.
  - The deltas start at zero, so training starts from an ordinary HalfKA network. The base learns from every
    position; the experts separate only where the data asks them to.
  - Export folds the three terms into the four plain weight sets the engine reads.
- **Engine** (build option `TRIUMV_PSQ_PHASES=4`):
  - a capture that crosses a phase boundary forces an accumulator refresh;
  - the Finny-table cache is kept per phase;
  - the network hash changes, so a MoE net cannot be loaded by a plain build or the other way round;
  - the default build is unchanged: 1 phase, bench identical.

### Verified before training

- **Engine against trainer** on the same random network, with the per-phase deltas set to random values. With zero
  deltas a wrong phase index would be invisible, so the random net is built with `--perturb-phases`. Over 4,096
  positions of `test80-2023-11-nov`:

  | | R² | mean error | 3σ |
  |---|---|---|---|
  | float | 0.999999 | 1.70 | 5.09 |
  | quantized | 1.000000 | 0.64 | 1.84 |

- **Incremental against full-refresh** evaluation in the engine: **0 mismatches** on 51 positions.
- **Smoke test** of the whole pipeline on the training machine: 2 short pretraining epochs, conversion, fine-tune
  resumed from the model, export to `.nnue`. Checkpoints are 3.9 GB, exported nets 161 MB (leb128).

### Data: one mix, every label BT4

A single mix from the start, as Stockfish's current recipe does, instead of the two stages of `legio-septima`.
Everything is **re-labelled with Leela's BT4**, so the whole run shares one label scale.

| source | files | size |
|---|---|---|
| [`vondele/master-binpacks_relabel`](https://huggingface.co/datasets/vondele/master-binpacks_relabel) (Stockfish self-play + DFRC) | 5 | 129.4 GB |
| [`vondele/linrock_relabel_1`](https://huggingface.co/datasets/vondele/linrock_relabel_1) (Leela `test80`, `test78`, `test77`, `test60`) | 13 | 203.9 GB |
| [`vondele/linrock_relabel_2`](https://huggingface.co/datasets/vondele/linrock_relabel_2) (Leela `test80`, 2023) | 12 | 141.8 GB |
| [`vondele/from_kaggle_2_relabel`](https://huggingface.co/datasets/vondele/from_kaggle_2_relabel) (T60/T70 wrongIsRight) | 5 | 110.6 GB |
| [`vondele/from_kaggle_1_relabel`](https://huggingface.co/datasets/vondele/from_kaggle_1_relabel) (`leela96`) | 5 | 97.6 GB |
| [`xushawn/test80-bt4-relabel`](https://huggingface.co/datasets/xushawn/test80-bt4-relabel) (Leela `test80`, early 2024) | 2 | 17.7 GB |
| **downloaded** | **42** | **701 GB** |

**40 files are used.** The two `test60-2021` files of 3 GB are excluded: the loader picks files uniformly, not by
size, so a small file would be read many times over. Anything under 5 GB is left out. Also excluded: T91
([`jshriver/t91-binpacks`](https://huggingface.co/datasets/jshriver/t91-binpacks), not re-labelled) and our own
self-play. The download script, which resolves every file through the Hugging Face API, is
[`recipes/06_consilium/download_bt4.sh`](https://github.com/Tors3/Triumviratus-Networks/blob/main/recipes/06_consilium/download_bt4.sh)
in Triumviratus-Networks.

<sub>The planned 790 GB turned out to be 701 GB, checked file by file against the Hugging Face API: one `test80`
month exists both whole and split in two parts and is downloaded once, and `xushawn/test80-bt4-relabel` is 17.7 GB,
not ~33. No data is missing.</sub>

### Recipe

Stockfish's SFNNv16 recipe (`vondele/nettest`, `threats.yaml`), scaled to the batch and the budget.

As run (the plan changed twice during training, see the notes below the table):

| | pretraining (P) | fine-tune F3 | fine-tune F4 |
|---|---|---|---|
| Length | **374 epochs × 1 G positions** (planned 450, stopped after epoch 373) | **80 epochs**, from P's weights | **60 epochs**, from F3's final weights |
| Batch | **524,288** | 524,288 | 524,288 |
| lr | **8e-4** (SFNNv16's 4e-4 at batch 131,072, × √4) | **1.6e-4** peak | **6e-5** peak |
| Schedule | one-cycle, 5 % warmup, final divisor 1000 | one-cycle, 1 % warmup | one-cycle, 1 % warmup, new seed |
| Lambda | see below | 0.75 fixed | 0.75 fixed |

**Final network:** the arithmetic mean of the model weights of F4 epochs 55–59, serialised, then the feature
transformer **permuted** (`serialize.py --ft-optimize`, 1 M positions): L1 neurons reordered so that zero activations
fall in contiguous blocks the sparse layer skips. The evaluation is bit-identical (same `bench` before and after).

<sub>The fine-tune was first planned as SFNNv16's (22 epochs re-warmed to 5.66e-4, lambda cycle restarting from 1.0),
then as a gentle 30 epochs at 6.5e-5 (F2, never run). On 29 September the tail of P was stopped at epoch 373, where
Elo and loss had gone flat with the rate still at ≈ 20 % of peak, and replaced by F3: a short second cycle at a fifth
of P's peak, the way a restart can move a network out of a plateau. F3's loss flattened after epoch ≈ 70 and F4, a
lower cycle from its end, took the validation loss below F3's.</sub>

- **Lambda, as launched:** **1.0 with a cycle** that dips by 0.3 (25 % warmup), plus jitter (0.0035 per sample,
  0.0070 per batch, decay 0.999), as in SFNNv16. That cycle dips to 0.7 at epoch ≈ 112 and **climbs back to 1.0**
  by the end of P, and restarts in F.
- **Lambda, changed at epoch 292** (see [the plateau](#the-plateau-and-the-lambda-fix)): a linear descent from 0.865
  to **0.75** over 40 epochs (reached at epoch 332), then **0.75 fixed** to the end of P and through all of F — the
  target every earlier own network finished on.
- **Piece-count sampling:** `pc-y` −0.20 / 0.45 / 1.0 / 0.95 / 0.75.
- **Skipping:** `random-fen-skipping 2`. Openings are **soft**-skipped (`soft-early 20`) and never hard-skipped: the
  ≥ 24-piece expert needs opening positions, but book lines repeated across millions of games are down-weighted.
- **Total:** **≈ 472 G positions** seen, about 3.7× `legio-septima` (≈ 128 G over both stages) and about 80 % of
  SFNNv16's run (≈ 600 G). The corpus holds roughly 270 G usable positions, so each is seen about 1.7 times on
  average.
- **Exports:** a `.nnue` every 45 epochs (10 % of P), plus on-demand exports of the live checkpoint, serialised on the
  training machine's CPU without touching the GPUs.

### Hardware, throughput and cost

**4× RTX 5090** (32 GB, PCIe 5.0, one NUMA node), 192 CPU threads, 251 GB RAM, rented on vast.ai at about **3 $/h**.

| step | positions/s |
|---|---|
| first DDP reading, loader tuned as for `legio-septima` | 1.4 M |
| loader feature-thread share 0.05 → **0.1**, 46 workers per GPU | 4.79 M (skip 3) |
| **final configuration**, skip 2, batch 524,288 | **4.97 M** |

- **The limit was the data loader, not the GPUs.** The feature-thread share that was optimal for `legio-septima`
  (0.05) leaves a single thread per GPU to build the input features, and threats plus experts cost more to build:
  about 350 k positions/s per thread. One 5090 alone went from 381 k positions/s (GPU at 28 %) to 1.9 M (GPU at
  95 %) with a larger share.
- **A profile misled first.** A synthetic benchmark (120 random active features) put 71 % of the step in the
  feature-transformer backward and suggested rewriting it. On real batches — 52 active features per side,
  concentrated — the upstream kernel takes 14.7 ms per 65,536 positions and the rewrite (verified correct, relative
  error 1.4e-6) takes 18.0 ms. It is not used.
- **DDP costs almost nothing here:** four GPUs give 4× one GPU, so the all-reduce over PCIe 5.0 is negligible.
- **Batch:** 131,072 → 262,144 → 524,288 gains about 7 % per doubling. At 262,144 with the full recipe the run
  measured 3.28 M positions/s — too slow for the budget, so the run was restarted at 524,288.
- **Cost:** about **0.17 $ per billion positions**, the same as the `legio-septima` machine (4× RTX 5060 Ti) but about
  six times faster. The whole run, P, F3 and F4 included, cost **105 $**, shared with **Maurizio Platino**. An 8× RTX
  3090 offer at 1.93 $/h was rejected: its 80-thread CPU cannot feed the loader.

Steady state: 201–212 s per epoch, GPUs at 97–99 %, 63–68 °C, well below their power limit. The last 3 % of speed
comes and goes with the GPU boost clock.

### Training so far

Checkpoints are numbered from 0, as the trainer does: "epoch 44" is the end of the 45th epoch.

| epoch | training loss |
|---|---|
| 1 | 0.0122 |
| 7 | 0.0081 |
| 20 | 0.0047 |
| 27 | 0.0041 |
| 42–69 | 0.0038–0.0041 |
| 80–124 | 0.0042–0.0045 (the lambda cycle is raising the floor, see below) |

<sub>The loss is not comparable across the whole run: lambda moves with its cycle, and the achievable floor moves
with it (see the `legio-septima` note in the archive).</sub>

### Strength during training

Each checkpoint plays `legio-septima` with **the same 8.0 search** on both sides: two builds from the same source and
compiler (MinGW, AVX-512, no PGO), the MoE one with `TRIUMV_PSQ_PHASES=4`. The only difference is the network. One
thread, 64 MB hash, UHO 2024 (+0.85/+0.94) openings, 75 games at a time.

| epoch | TC | games | W / D / L | Elo vs `legio-septima` |
|---|---|---|---|---|
| ≈ 16 | 20+0.2 | 131 | 5 / 44 / 82 | ≈ −230 |
| 20 | 20+0.2 | 143 | 11 / 58 / 74 | ≈ −164 |
| 26 | 20+0.2 | 355 | 26 / 164 / 165 | −144 |
| 30 | 20+0.2 | 589 | 59 / 274 / 256 | −121 ± 21 |
| 35 | 20+0.2 | 2,000 | 190 / 991 / 819 | −113 ± 11 |
| 44 | 20+0.2 | 2,000 | 264 / 989 / 747 | −86 ± 11 |
| 53 | **30+0.3** | 1,774 | 214 / 963 / 597 | −76 ± 11 |
| 64 | 30+0.3 | 1,001 | 139 / 562 / 300 | −56 ± 14 |
| 71 | 30+0.3 | 1,012 | 121 / 569 / 322 | −70 ± 14 |
| 80 | 30+0.3 | 2,000 | 315 / 1,052 / 633 | −56 ± 11 |
| 99 | 30+0.3 | 1,159 | 191 / 631 / 337 | −44 ± 14 |
| 110 | 30+0.3 | 2,000 | 311 / 1,048 / 641 | −58 ± 11 |
| 124 | 30+0.3 | 2,000 | 357 / 1,059 / 584 | −40 ± 10 |
| 134 | 30+0.3 | 1,370 | 233 / 742 / 395 | −41 ± 12 |
| 145 | 30+0.3 | 995 | 187 / 519 / 289 | −36 ± 15 |
| 157 | 30+0.3, **scale** | 1,220 | 210 / 690 / 320 | −31 ± 13 |
| 165 | 30+0.3, scale | 898 | 177 / 483 / 238 | −24 ± 16 |
| 171 | 30+0.3, scale | 2,000 | 356 / 1,125 / 519 | −28 ± 10 |
| 200 | 30+0.3, scale | 2,000 | 405 / 1,085 / 510 | −18 ± 10 |
| 254 | 30+0.3, scale | 1,187 | 257 / 654 / 276 | −6 ± 13 |
| 262 | 30+0.3, scale | 1,154 | 254 / 661 / 239 | **+5 ± 13** |
| 272 | 30+0.3, scale | 604 | 130 / 323 / 151 | −12 ± 19 |
| 278 | 30+0.3, scale | 701 | 151 / 385 / 165 | −7 ± 17 |
| 293 | 30+0.3, scale, **new lambda** | 909 | 211 / 480 / 218 | −3 ± 16 |
| 299 | 30+0.3, scale, new lambda | 886 | 201 / 460 / 225 | −9 ± 16 |
| 311 | 30+0.3, scale, new lambda | 1,491 | 340 / 794 / 357 | −4 ± 12 |
| 321 | 30+0.3, scale, new lambda | 1,949 | 438 / 1,106 / 405 | +6 ± 10 |
| 333 | 30+0.3, scale, **lambda 0.75 fixed** | 1,425 | 343 / 788 / 294 | **+12 ± 12** |
| 343 | 30+0.3, scale, lambda 0.75 fixed | 578 | 126 / 325 / 127 | −1 ± 15 |

- **The gap closed fast up to epoch ≈ 60, then the curve went flat** at −45 to −70. The flat stretch coincides with
  the learning rate near its peak. The schedule is a cosine one-cycle: warmup to 8e-4 by epoch ≈ 22, still above
  90 % of the peak until epoch ≈ 110, 84 % at 135, 54 % at 225, 23 % at 315. The previous own networks gained most of
  their strength late, while the rate decayed.
- **From epoch 53 the time control is 30+0.3.** The MoE build is 7.5 % slower, and a longer time control weighs that
  less, so the comparison reads the evaluation more than the speed. The two series are not strictly comparable.
- A self-check early on: epoch ≈ 16 against epoch ≈ 5, 74 / 31 / 4 in 109 games (≈ +265 Elo).
- **A prediction that failed.** A logarithmic fit of the early 20+0.2 points (Elo ≈ 111·ln(epoch) − 508) put parity
  near epoch 96. Registered before the match, it predicted −32 for epoch 71; the match gave −70 ± 14. Early points
  on a steep curve say little about the plateau that follows.
- **Checkpoints fixed in advance:** at epoch 225 (rate at 54 %) the network should be at −30 or better, otherwise
  the run needs a closer look; at epoch 315 it should be near parity. **Both were met** (−18 at 200, parity from 254).
- **From epoch 124 to 254 the curve rose on a straight line**, about +3.8 Elo every 10 epochs. A weighted linear fit
  of the uncalibrated points 53–145 predicted −17 at epoch 200 and parity near 240; the later points, not used in the
  fit, landed on it. **Then it went flat at parity**, from epoch ≈ 250 to at least 311.
- Two direct MoE-against-MoE checks (10+0.1, 2,000 games, same scale on both sides) confirm the plateau is in the
  network and not in the match: epoch 285 vs 278 **−2.1 ± 10.1**, epoch 305 vs 291 **+3.5 ± 10.7**.

#### Eval scale per phase ("scale" in the table)

The engine compresses the raw network output with a per-bucket factor, `EvalScaleB0..B7` (default 60, the value tuned
for `legio-septima`), before search thresholds use it. The MoE has its own scale, so it was measured: the raw static
eval of both nets on 40,000 positions from these matches, compared bucket by bucket (orthogonal regression).

- At epoch 145 the MoE matched `legio-septima` everywhere **except 5–8 pieces**, where it read about **13 % higher**.
  The scale depends on the piece count only: no jump at the expert boundaries 9|10, 15|16, 23|24.
- Corrected set for the MoE side: **64 / 53 / 62 / 60 / 60 / 59 / 59 / 60**. Against the default on the same
  network at 10+0.1: +6.1 ± 10.1 over 2,225 games — small, stopped before a verdict, adopted for all later tests.
- At epoch 254 the network had drifted: the opening buckets now read 6–9 % *lower* than `legio-septima`. A set that
  matched the ratios again (60 / 54 / 60 / 61 / 60 / 62 / 64 / 66) was tried on epoch 262 and did **worse**:
  −27 ± 24 over 327 games against +5 ± 13 over 1,154 with the old set. It was dropped. Matching the raw ratio is not
  what the search wants in the opening; the final values will be tuned by SPSA on the finished network.

#### The plateau and the lambda fix

The training and validation losses could not explain the plateau: both kept falling. But they are computed at the
current lambda, and **lambda was moving**: the SFNNv16 cycle dips to 0.7 at epoch ≈ 112 and then climbs back towards
1.0 — pure evaluation, no game result — by the end of the run. Every earlier own network (`rubicon-alea`,
`legio-septima`) finished on **lambda 0.75 fixed**; `legio-septima` spent 700 of its 800 stage-2 epochs there.

To separate learning from the moving target, five checkpoints were scored on the **same 600,000 positions** of a
binpack the MoE never saw (a sample of Leela T91 from
[`jshriver/t91-binpacks`](https://huggingface.co/datasets/jshriver/t91-binpacks), excluded from training), with
lambda held fixed (loss × 10⁻³):

| epoch | λ = 1.0 (predict the eval label) | λ = 0.0 (predict the game result) |
|---|---|---|
| 110 | 4.78 | 18.65 |
| 145 | 4.68 | 18.46 |
| 200 | 4.53 | **18.36** |
| 254 | 4.34 | 18.47 |
| 285 | 4.23 | 18.40 |

- Against the eval labels the network **kept improving at a constant rate**: no saturation, no over-fitting.
- Against game results it **stopped improving at epoch ≈ 200**. Games are won on results, and the Elo plateau starts
  right after.
- The cycle was pushing the network towards imitating the labels, and it did exactly that.

So at epoch 292 the run was stopped at an epoch boundary and resumed from the checkpoint (weights, optimiser,
learning-rate schedule and step count restored) with the lambda change described under [Recipe](#recipe). Two
details of the resume, both handled:

- the trainer re-runs the epoch stored in the checkpoint; left alone, that extra epoch would have pushed the step count
  past the one-cycle schedule and crashed the last epoch of P, so the stored epoch was advanced by one;
- the fine-tune F would have restarted the lambda cycle from 1.0; it now runs at 0.75 throughout.

The loss rises while lambda descends (a noisier target), as it did between epochs 39 and 119 while the network gained
70 Elo. The first test of the fix is at epoch ≈ 335, with lambda settled.

### Fine-tunes F3 and F4

Validation loss (× 10⁻³, λ 0.75, same validation set):

| | F3 ep. 49 | F3 ep. 59 | F3 ep. 69 | F3 ep. 79 | F4 ep. 9 | F4 ep. 39 | F4 ep. 49 | F4 ep. 59 |
|---|---|---|---|---|---|---|---|---|
| val loss | 3.42 | 3.40 | 3.37 | 3.37 | 3.39 | **3.35** | 3.38 | 3.44 |

<sub>The validation set is a single batch, so single points move by about ±0.05; F4's training loss per epoch fell
steadily from 3.43–3.44 to 3.39–3.40.</sub>

A second measurement lesson came with F3. For weeks the matches ran **75 games at a time on 40 physical cores**, so
most engines shared a core (and its caches) with another, and the MoE, whose first layer is almost twice the size of
`legio-septima`'s, lost more from it. The same F3 network with the same settings against `legio-septima`:

| F3 + first SPSA vector, against `legio-septima` 8.0 | concurrency | games | Elo |
|---|---:|---:|---:|
| epoch 42, 90+0.9 | 75 | 720 | +14.5 ± 12.6 |
| epoch 77, **12+0.12** | **38** | 922 | **+23.4 ± 12.0** |

A third came the next night: the two Xeons are two NUMA nodes, one of them had no memory of its own, and engines
moved between sockets. With memory on both CPUs and every fastchess pinned to one socket, 76 games at a time give the
same result as 38. Details in [`DEVELOPMENT_8.0.md`](../development/archive/DEVELOPMENT_8.0.md) §9.

**Choosing the final network**, MoE against MoE on the same engine, same scale on both sides: F4's
average-and-permute against the end of F3 (epoch 79), 8+0.08, **+19.3 ± 13.9** over 686 games. The eval scale
of the two is the same within 1 % in every piece-count bucket (correlation ≥ 0.999), so F4 refined F3 without
changing its character.

### The result

The per-phase eval scale, the eval blend and the pruning margins that depend on the network's scale (25 parameters)
were re-tuned by SPSA on the final network (20+0.2, 5,410 iterations; the vector at 4,363 iterations beat its
starting point by +8.6 ± 11.5 over 928 games) and baked into the code. Against the **official 7.0 binary**, the 8.0
**release build** (PGO, tuning frozen, no options set):

| TC | games | pentanomial | Elo |
|---|---:|---|---:|
| **15+0.15** | 2,000 | [10, 180, 471, 321, 18] | **+27.3 ± 8.3** |

1 thread, 64 MB hash, UHO 2024 (+0.85/+0.94), 76 games at a time pinned to the two sockets, LOS 100%. This number
includes 8.0's speed work and ablations (+11.7 ± 4.6 at 60+0.6 on the old network, see
[`DEVELOPMENT_8.0.md`](../development/archive/DEVELOPMENT_8.0.md)), so the network with its SPSA accounts for roughly +15 of it.

---

## What "own-lineage" means (and does not)

- **Means:** the network *weights* shipped with Triumviratus are trained by the project, **from scratch**, with
  **no Stockfish network used as a seed or teacher**. Every training run starts either from random initialisation or
  from one of our own earlier networks.
- **Does not mean: our own data.** The training data is overwhelmingly **public** — Leela Chess Zero and Stockfish
  self-play binpacks. The project's own self-play contributed a small minority to one network (`rubicon-alea-v2`,
  ≈ 6.5 %) and none to the 8.0 run. What is ours is the training, the mix, and the resulting weights.
- **Does not mean:** independence from Stockfish *code*. The NNUE inference started from Stockfish's code (GPLv3) and
  the base input set is Stockfish's SFNNv16 (threats and HalfKA); the inference has since been reworked for our mixture-of-experts network (four phase experts,
  our own input blocks), and the trainer is Stockfish's `nnue-pytorch`. The whole project is
  GPLv3 and credits Stockfish accordingly (see `README` / `COPYING`).
- **Why it still matters:** what the project gets is a network whose weights, mix and feature extensions are its own
  rather than a redistribution of someone else's.

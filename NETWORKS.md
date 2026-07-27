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
| **Serialization** | shipped **FT-permuted** (feature-transformer neuron reordering for sparsity) — eval **bit-identical**, ≈ +2 % NPS for free |
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
| **Serialization** | shipped **FT-permuted** (`ftperm`, which reorders the L1 neurons so ReLU zeros cluster and the sparse-input affine path pays off). Verified in the 6.0 pre-release audit: the permutation covers **all four** input blocks (`Full_Threats` 60720, `HalfKAv2_hm` 22528, `PawnPair` 4560, `PassedPawns` 96) — the point that matters, because permuting only the SFNNv13 columns and not the grafted ones would make the net *wrong*, not merely slow — and the evaluation is **bit-identical** to the plain net (identical bench signature, with a valid negative control). Its **speed benefit is not measurable**: −0.34 %, 95 % CI [−2.66 %, +1.98 %] over 100 benches in fresh processes at identical node counts, so an earlier "≈ +2 % NPS" claim is **retracted**. The permutation is kept because it is correct and free, not because it is faster |
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

## `legio-septima` — the Triumviratus 7.0 network (**in development, not released**)

> **Status: in development.** No network shipped yet, no strength measured yet. **Triumviratus 6.0 with
> `rubicon-alea-v3` remains the official release.** This section is here so the direction is public while
> the work happens, not because there is a result to report.

The `rubicon-alea` lineage ends with 6.0. The 7.0 network changes both the architecture and, more
importantly, **the training method** — which is why it starts a new lineage and a new name.

| | 6.0 — `rubicon-alea-v3` | 7.0 — `legio-septima` |
|---|---|---|
| Base architecture | SFNNv13 | **SFNNv16** |
| L2 | 31 | 32 |
| `Full_Threats` | 60,720 | **59,808** |
| Pawn-pair block | ours, 4,560 | **identical** — it is Stockfish's `PP_3Wide` |
| `PassedPawns` | 96 | 96 (still ours; Stockfish has no equivalent) |
| **Total inputs** | 87,904 | **86,992** |
| Training | **graft** — the base net is frozen (`--lr 0`) and only the new feature block learns | **full training**, 600 + 800 epochs |

**Why the input count goes down.** Stockfish's SFNNv16 removed the pawn→pawn threat features and the
pawn-pusher inputs, because the pawn-pair block already covers every pawn–pawn interaction — keeping both
means paying twice for the same information. We had both; now we don't.

**The change that actually matters is not in that table.** Every network this project has shipped so far
was a **graft**: an existing net with its dense layers frozen, learning only a newly added feature block.
That is why they saturate in about four epochs — most of the network never learns anything. `legio-septima`
is the project's **first real full training**: base and feature blocks together, from scratch, over a corpus
of public Stockfish self-play and DFRC data re-labelled with Leela's BT4 network, followed by a second stage
on Leela-derived data. Whether that closes the gap to the strongest networks is exactly the open question.

Architecture diagram, gates and the full recipe live in the development tree, not here — this file records
what shipped.

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

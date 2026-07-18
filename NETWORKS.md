# Triumviratus — Network lineage & training

Triumviratus ships its **own** NNUE network — weights trained by the project, **not** a Stockfish network.

To be precise about provenance (and GPL-honest): the NNUE **evaluation code** and the base network **architecture**
are Stockfish's (GPLv3 — see [`README`](README.md) and [`COPYING`](COPYING)); the official Stockfish **trainer**
([`nnue-pytorch`](https://github.com/official-stockfish/nnue-pytorch)) is used to train. The **training data is
mostly public** — Leela Chess Zero `test79`/`test80` binpacks, plus Stockfish-generated packs in one early stage,
plus a small slice of our own self-play. What is **ours** is the **network weights**: trained from scratch, with no
Stockfish network used as a seed or teacher, on a data mix and schedule we chose — and the extra input feature
blocks (`PawnPair`, `PassedPawns`) that the architecture carries. This document records how each shipped network
was trained, for transparency and reproducibility.

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
| **Serialization** | shipped **FT-permuted** — verified eval bit-identical to the plain net (same bench signature), ≈ +2 % NPS for free |
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

## `Outposts` — a third input block, tested and **rejected** (negative result, kept for the record)

> **Status: implemented, verified, trained, and measured at parity — not shipped.** The block was
> mechanically sound (a network with random Outposts weights collapses by −614 Elo, so the features
> demonstrably drive the evaluation) but added no strength: frozen-base training showed no signal by
> epoch 3-5, where `PassedPawns` had already shown its gain, and whole-network co-adaptation first
> *lost* ~18 Elo, then merely recovered to parity. The information is evidently already inferable
> from the threat features and `PawnPair` through the L1 pairwise product. Two lessons worth the GPU
> hours: square-only features that are meaningless without a piece standing on them get a diluted
> gradient (an outpost square matters only when something occupies it — a passed pawn matters by
> itself), and training loss is useless as a guide at the label-noise floor (it sat flat at ~0.0036
> while playing strength swung by tens of Elo). The third block slot went on to carry one more
> candidate (`CandidatePassers`, below) with the same outcome, after which the slot and the
> tri-format reader were **removed from the engine** — the shipped format is the four-block v3.

The engine's feature set can now carry a third project-specific block, on the same pattern as
`PawnPair` and `PassedPawns`:

| | |
|---|---|
| **Feature space** | **96 features** — 48 oriented squares × {own outpost, enemy outpost}. A square is an *outpost* for a side when it is **defended by one of that side's pawns** and **no enemy pawn can ever attack it** (no enemy pawn on an adjacent file still able to advance onto it). |
| **Why square-only** | No flags for the piece standing there, its colour complex, or whether it is defended twice. Those are learnable through the pairwise-multiplied L1 against `HalfKAv2_hm` (which already knows which piece is on which square), and adding them would break the pawn-event-only incremental update. Only the part the network *cannot* infer — the cross-file pawn configuration — is encoded. Same reasoning that shaped `PassedPawns`. |
| **Cost** | Folds into the existing threat accumulator and reuses the `PassedPawns` pawn-event trigger, so it adds **no new SIMD path** and no measurable inference cost. |
| **Reader** | During the experiment the reader was **tri-format**: one binary loaded a v4 net (with Outposts), a v3 net (zero-filling Outposts) and a v2 net (zero-filling both PassedPawns and Outposts), with automatic format detection from the network hash. This is what made net-isolated A/B gating possible with a single executable. Removed together with the slot once the experiments closed. |
| **Verification** | The zero-initialised graft is **bit-identical** to its parent (same bench signature through graft → serialize → reader), the incremental update matches a full refresh with **zero mismatches**, and the C++ loader agrees with an independent Python reference on every test position. |

One methodological observation worth recording, independent of whether this network ships: grafting a
new block and then **co-adapting the whole network** (a two-group fine-tune — low learning rate on the
base, high on the new block) behaves very differently from the frozen-base screening used for
`PassedPawns`. Early in such a run the network is measurably *weaker* than its parent, because the
base is being pulled around by a block that is still changing quickly; it recovers only after a few
epochs. Training loss is close to useless as a guide here — it sat flat at the label-noise floor
(~0.0036) throughout, while playing strength moved by tens of Elo in both directions. Only game
results tracked what was actually happening.

---

## `CandidatePassers` — the second slot candidate, also **rejected** (negative result, kept for the record)

> **Status: implemented, verified, trained frozen-base, and measured negative — not shipped.** After
> Outposts, the same third-block slot carried a **candidate-passer / pawn-majority** feature: 96
> features (48 oriented squares × own/enemy), where a pawn is a *candidate* when it is not passed,
> sits on a semi-open file, and its potential helpers on adjacent files are at least as many as the
> enemy sentries ahead of it. Same pawn-event incremental trigger, same zero-init graft discipline
> as `PassedPawns`.

Wiring was proven the same way (random block weights collapse the net by **−240 Elo**, so the
features drive the eval), and the block demonstrably *learned* — its mean absolute weight after 7
epochs of frozen-base training exceeded that of the shipped `PassedPawns` block. It still measured
**negative at every gate**: −1.9 / −3.1 / −6.8 at 12 s across epochs 3-7, and −5.0 at 18 s with
adjudication for the final check. Learned-but-useless is the interesting part: the information is
real but evidently already inferable from the threat features and `PawnPair` through the L1 pairwise
product. Together with Outposts (and a re-test of the best co-adapted Outposts checkpoint, which
came back neutral), this closed the slot: **the slot-5 code and the tri-format reader were removed
from the engine**, and the experimental nets are archived off-tree
(`nn-rubicon-alea-v4-coadapt-ep3-UNCONFIRMED.nnue`, `nn-rubicon-alea-v5-candidates-ep7-NEGATIVE.nnue`).

The refined lesson after two rejections, added to the feature-selection criteria: a block earns its
place only when the encoded information is (a) **not inferable** from existing inputs through the
pairwise L1, (b) carries **large evaluation magnitude** in classical terms, (c) **correlates with
the label** strongly enough to survive the label-noise floor, and (d) **means something by itself**,
without a piece having to stand on the square (`PassedPawns` passes all four; Outposts fails (d),
CandidatePassers fails (a)).

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

# Triumviratus — Network lineage & training

Triumviratus ships its **own** NNUE network — weights trained by the project, **not** a Stockfish network.

To be precise about provenance (and GPL-honest): the NNUE **evaluation code** and the network **architecture**
are Stockfish's (GPLv3 — see [`README`](README.md) and [`COPYING`](COPYING)); the official Stockfish **trainer**
([`nnue-pytorch`](https://github.com/official-stockfish/nnue-pytorch)) is used to train. What is **ours** is the
**network weights** — trained from our own data pipeline, from scratch (no Stockfish net used as a seed). This
document records how each shipped network was trained, for transparency and reproducibility.

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
| **Finishing** | **LR-anneal fine-tune** from ep459 with a fresh, steeper decay (base 2.5e-5 / PawnPair 2.5e-4 — the LR the main run had reached — γ = 0.95, ~50 epochs), then **over_last**: the shipped weights are the **average of the last 3 anneal checkpoints** — the same tail-averaging recipe that produced v1's best net |
| **Serialization** | shipped **FT-permuted** (feature-transformer neuron reordering for sparsity) — eval **bit-identical**, ≈ +2 % NPS for free |
| **Hardware** | GCP **g4** (RTX PRO 6000 Blackwell), spot instance |
| **Result (net-isolated)** | v2 vs v1 on the **same** engine, 10+0.1: **+13.3 Elo** (ep199) → **+16.1 Elo** (ep219) — the refine adds real strength over v1, still climbing when gated |
| **Result (release)** | **Triumviratus 6.0 + v2 vs 5.1 + v1**, long TC (20+0.2): **+30.9 ± 9.6 Elo** (LLR passed H1, 1208 games) — net refine + 6.0 search gains combined |

Same honest position as the rest of the `rubicon` family: **our weights**, our data, on Stockfish's architecture
and trainer. The PawnPair block is our own extension to the SFNNv13 feature set.

---

## What "own-lineage" means (and does not)

- **Means:** the network *weights* shipped with Triumviratus are trained by the project, from our own data, with no
  Stockfish network used as a seed. The evaluation values are ours.
- **Does not mean:** independence from Stockfish *code*. The NNUE inference and the network *architecture* are
  Stockfish's (GPLv3), and the trainer is Stockfish's `nnue-pytorch`. The whole project is GPLv3 and credits
  Stockfish accordingly (see `README` / `COPYING`). Renaming code does not change this; the attribution is kept.

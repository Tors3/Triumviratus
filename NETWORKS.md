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

Trained-by-us **weights** on Stockfish's **architecture** and **trainer** — same honest position as `rubicon-v1`,
one NNUE generation newer.

---

## What "own-lineage" means (and does not)

- **Means:** the network *weights* shipped with Triumviratus are trained by the project, from our own data, with no
  Stockfish network used as a seed. The evaluation values are ours.
- **Does not mean:** independence from Stockfish *code*. The NNUE inference and the network *architecture* are
  Stockfish's (GPLv3), and the trainer is Stockfish's `nnue-pytorch`. The whole project is GPLv3 and credits
  Stockfish accordingly (see `README` / `COPYING`). Renaming code does not change this; the attribution is kept.

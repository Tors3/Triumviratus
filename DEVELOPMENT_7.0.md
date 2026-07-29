<div align="center">

<img src="logo.png" alt="Triumviratus" width="200">

# Triumviratus 7.0 — development log

**Network project.** First full training from scratch · `legio-septima` · SFNNv16 + PassedPawns

**by Francesco Torsello**

<sub>in collaboration with Maurizio Platino</sub>

</div>

---

<div align="center">

[Why](#why-70-is-a-network-project) · [Architecture](#architecture--trann2) · [Training](#training) ·
[Results](#results-so-far) · [Search](#search-work) ·
[6.0 log](DEVELOPMENT_6.0.md) · [Networks](NETWORKS.md)

</div>

---

> [!IMPORTANT]
> **6.0 is and remains the official release.** 7.0 is in training. The numbers below are
> mid-training checkpoints, not release claims, and each carries its error bar.

---

## Why 7.0 is a network project

A full audit of the search in July 2026 — 21 parallel review agents, 122 findings, 113 verified
against the source — reached a blunt conclusion: **the remaining gap to the strongest engines is
≈ 25–40 Elo of *network*, not of search.** Of ~70 verified divergences from Stockfish, Reckless,
Stormphrax and Integral, two had a structural motive with an unguaranteed sign, one had a positive
prior worth +1..+5 Elo, and the rest were below the resolution floor or already falsified.

So 7.0 does not try to out-search anyone. It changes how the network is *made*.

**Every network this project has shipped so far was a graft**: an existing net frozen, a newly added
input block allowed to learn, everything else at `lr 0`. That is why they converged in about four
epochs — and why they could only ever add what the new block could express. **7.0 is the first full
training from scratch.**

---

## Architecture — `TRANN2`

| | 6.0 (`TRANN1`) | 7.0 (`TRANN2`) |
|---|---|---|
| base | SFNNv13 | **SFNNv16** |
| total inputs | 87,904 | **86,992** |
| L1 · L2 · L3 | 1024 · 31 · 32 | 1024 · **32** · 32 |
| output buckets | 8 | 8 |

Input blocks: `Full_Threats` 59,808 · `HalfKAv2_hm` 22,528 · `PawnPair` 4,560 · **`PassedPawns` 96**.

Two changes from `TRANN1`:

- **SFNNv16 skip structure.** The output layer no longer reads only L3: it reads the concatenation
  of L2 and L3, each in both activations — `fc_2` takes 128 inputs instead of 32. L2 goes 31 → 32,
  which removes the forwarded-neuron-in-an-extra-slot construction and with it the entire column
  remap that used to realign `fc_1`.
- **Pawn→pawn threat inputs removed** (60,720 → 59,808). They duplicated information the
  `PawnPair` block already carries — a conclusion Stockfish reached independently around the same
  time (nnue-pytorch PR #502).

`PassedPawns` (96 features, one per passed pawn) remains **the only block original to this
project** — a relational property `HalfKAv2_hm` cannot express directly.

> [!NOTE]
> The net format is therefore **not** SFNNv16: there is one extra input block, so the hash differs
> and `TRANN1` nets are not loadable. That is deliberate, and the name reflects the real
> divergence rather than hiding the derivation.

#### Verification before training

A mis-specified architecture fails silently: a mis-ordered block shifts every folded index and the
network trains on a mapping nobody checked. Three gates were run before a single epoch:

| | check | result |
|---|---|---|
| V1 | `static_assert` binds `Dimensions` to the generated offset table | 59,808 exact — the constant and the table can no longer diverge unnoticed |
| V2 | random network from the trainer, loaded by the engine | found **three** misalignments, none of them guessable: block order, `ft_compression`, pawn-pair hash |
| V3 | `cross_check_eval.py` — engine vs trainer on the **raw** network output | **R² 0.999999**, mean error 0.57 (rounding) |

<sub>V3 compares the raw network output deliberately. Comparing the engine's `eval` output instead
measures something else: that path applies the psqt/positional blend, a complexity term, material
scaling and the fifty-move damping, none of which the trainer has.</sub>

---

## Training

Two stages, ~380 GB of public data, all re-labelled with Leela's **BT4** network.

| | stage 1 | stage 2 |
|---|---|---|
| corpus | 129 GB — Stockfish self-play + DFRC, BT4-relabelled | 422 GB — Leela T80/T78/T77/leela96/Farseer BT4-relabelled + T91-2026 |
| epochs | 500 (stopped at 479) | 800 |
| batch | 131,072 | 131,072 |
| lr | 2.47e-3 | 1.237e-3 |
| gamma | 0.990 | 0.995 |
| lambda | 1.0 → 0.75 over the run | **0.79 → 0.75 over the first 100 epochs, then fixed** |
| positions delivered | 47.9 G | 80 G |

#### Why the lambda schedule differs between stages

`lambda` weights the training target between the search score and the **game result**. Stage 1
annealed it across the whole run, so it was still moving at the last epochs — when the learning rate
had fallen to 3.8e-5 and the model could no longer follow it.

Stage 2 resumes from 0.79, where stage 1 stopped, and finishes the shift in **100 epochs** while the
learning rate is still between 100% and 61% of its initial value — 33× higher than where it ends.
The target moves while the model is plastic; the remaining 700 epochs consolidate on a target that
no longer moves.

> [!WARNING]
> **`val_loss` is not comparable across epochs while lambda is annealing.** The loss is computed
> against the *current* lambda, so the target itself moves: as weight shifts onto the game result —
> a much noisier signal — the achievable loss floor rises **by construction**. In this run
> `val_loss` climbed from 0.00331 to 0.00352 while the network was measurably improving.
> Corollary: **`--save-top-k` by `val_loss` is compromised** in any annealed run — it selects the
> minimum of a metric that climbs, so the "best" checkpoints are all from early training.

The quality metric that *is* comparable across epochs is a **paired match at fixed depth**: same
binary, only the `.nnue` swapped, so both tree size and the moving metric drop out.

---

## Results so far

All measurements: 1 thread, 64 MB hash, UHO_4060_v4 book, two-sided resign 650 / draw 10 cp.

#### vs 6.0 — first checkpoint ahead

| | date | TC | games | Elo | LOS |
|---|---|---|---|---|---|
| stage-2 epoch 189 vs `6.0` + `rubicon-alea-v3` | 2026-07-29 | 12+0.12 | 802 | **+13.00 ± 12.72** | 97.75% |

Measured on the 7.0 binary **frozen before** any search change, so the figure isolates the network.
Zero is excluded, but only just — the interval is `[+0.28, +25.72]` — and roughly 600 training
epochs remain.

#### Stage 2 progress — vs the end of stage 1

Same binary on both sides, only the network changes.

| stage-2 epoch | TC | games | Elo |
|---|---|---|---|
| 19 | depth 15 | 494 | **−27.49 ± 16.24** |
| 39 | depth 15 | 1000 | **−6.25 ± 11.06** |
| 119 | 15+0.15 | 500 | +1.39 ± 15.17 |
| 176 | 15+0.15 | 1200 | +0.87 ± 10.33 |

<sub>The first 40 epochs are recovery, not progress: stage 2 restarts at a learning rate 32× higher
than where stage 1 ended, which moves the model off its converged minimum before it re-converges on
better data. The +21 Elo between epochs 19 and 39 is significant (z = 2.16). From epoch 119 the
measurements sit on parity — but at ±10 Elo they cannot resolve a gain of 2–3 Elo per 50 epochs,
which is a normal rate for the tail of a training run.</sub>

#### Stage 1 milestones

| | TC | games | Elo |
|---|---|---|---|
| ep152 vs ep111 — same binary, fixed depth | depth 15 | 2000 | **+19.65 ± 7.69** (LOS 100%) |
| ep416 vs `6.0` | 15+0.15 | 598 | −8.14 ± 14.92 |

---

## Search work

Three changes to move generation, all **node-identical** — the search tree is bit-for-bit the same,
so they cannot change playing strength at a fixed node count, only the rate at which nodes are
produced.

| | change |
|---|---|
| 1 | **`quiets_only` generation.** The move picker generated *all* pseudo-legal moves in the quiet stage — including every capture already produced by the tactical stage — then marked them consumed and never yielded them. |
| 2 | **`known_in_check`.** The evasion gate recomputed "am I in check?" on every generation, and outside check that test has no early exit. It ran three times per node: once in the search, once per move-picker stage. |
| 3 | **`pop_lsb_bb`.** `bb &= bb - 1` instead of clearing a specific bit, at the 14 sites where the bit being cleared *is* the least significant one. |

Node-identity verified at three levels: bench unchanged, **139,146,249 nodes identical** over 900
searches at depth 15, and identical again across a different compiler and instruction set
(clang/Linux/AVX2 against clang-cl/Windows/AVX512).

**Measured effect: +0.43% for change 1, nothing for 2 and 3** — paired interleaved A/B, 60 positions
at depth 18, three PGO binaries built from the same recipe. About +0.4 Elo.

<sub>Worth recording, since it sets expectations for further work of this kind: generating one extra
capture is a `pop_lsb`, a test, an encode and a store. On an out-of-order core with the data in L1
that hides in the shadow of the memory-bound network evaluation, which is ~58% of node time. The
move generator was not the bottleneck, and micro-optimising it does not become one.</sub>

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
> **6.0 is and remains the official release.** The 7.0 network is finished — stage 2 closed at epoch
> 799 — but there is no 7.0 release yet. Every figure below carries its error bar.

---

## Why 7.0 is a network project

A full audit of the search in July 2026 — 21 parallel review agents, 122 findings, 113 verified
against the source — reached a blunt conclusion: **the remaining gap to the strongest engines is
≈ 25–40 Elo of *network*, not of search.** Of ~70 verified divergences from Stockfish, Reckless,
Stormphrax and Integral, two had a structural motive with an unguaranteed sign, one had a positive
prior worth +1..+5 Elo, and the rest were below the resolution floor or already falsified.

So 7.0 does not try to out-search anyone. It changes how the network is *made*.

The first own-lineage network, `rubicon-alea-v1`, was trained from scratch — but the two that
followed grew by **grafting** a new input block onto a frozen predecessor: v2 fine-tuned v1 with a
zero-initialised `PawnPair` block, and v3 froze v2 entirely at `lr = 0` so that only `PassedPawns`
trained, which converged in about four epochs.

Grafting is cheap and it worked: v2 and v3 are worth **+15.14 ± 7.69 Elo** together over v1. But a
frozen base can only ever *add* what the new block can express — it cannot re-learn what the rest of
the network already believes. **7.0 trains base and feature blocks together, from scratch**, on a
corpus an order of magnitude larger than v1's.

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

Two stages. Full corpus, per-file, in **[`NETWORKS.md`](NETWORKS.md)**.

| | stage 1 | stage 2 |
|---|---|---|
| corpus | **121 GiB**, 5 binpacks — Stockfish self-play + DFRC, BT4-relabelled | **423 GB**, 21 binpacks — Leela T80/T78/T77/leela96/Farseer BT4-relabelled + T91-2026 |
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

All against `6.0` + `rubicon-alea-v3`, on the 7.0 binary **frozen before** any search change, so
each figure isolates the network.

| stage-2 epoch | date | TC | games | Elo | LOS |
|---|---|---|---|---|---|
| 189 | 2026-07-29 | 12+0.12 | 802 | +13.00 ± 12.72 | 97.75% |
| 229 | 2026-07-29 | 15+0.15 | 942 | +8.85 ± 11.82 | 92.91% |
| 263 | 2026-07-29 | 15+0.15 | 1678 | +13.88 ± 8.58 | 99.92% |
| 370 | 2026-07-30 | 15+0.15 | 1180 | +17.09 ± 10.55 | 99.93% |
| 659 | 2026-07-30 | 15+0.15 | 378 | +17.48 ± 18.71 | 96.68% |
| 696 | 2026-07-30 | 15+0.15 | 2138 | +28.01 ± 7.72 | 100 % |
| **799** (stage-2 final) | 2026-07-30 | 15+0.15 | 1442 | **+23.41 ± 9.22** | 100 % |

Ahead of 6.0 from epoch 189 on. The rows are mutually indistinguishable at their error bars — at
±9 Elo a 1000-game match cannot separate +20 from +30, so this column shows the level, not a trend.
The direct paired match between the last two networks is the tighter measurement: epoch 799 beats
epoch 696 by **+5.18 ± 6.03** over 3892 games.

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

### Where the time actually goes

That last note was a guess. It has now been measured, with per-phase cycle counters compiled into
a profiling build. On the AVX2 binary:

| | share of search wall time |
|---|---|
| **NNUE forward** | **52%** |
| — feature transformer | 38.0% |
| —— incremental accumulator update | 26.9% |
| —— full refresh | 7.3% (8.5% of calls) |
| —— final transform | 2.1% |
| — `fc_0` (sparse affine) | 4.6% |
| — remaining layers | 0.9% |
| move scoring | 6.5% |
| make + unmake | 4.9% |
| move generation | 2.9% |
| transposition table | 2.0% |
| rest of search | 31% |

Two things follow. First, `fc_0` is 4.6%, which closes a question left open in the 6.0 audit: it had
archived the feature-transformer permutation work with the explicit condition *"if `fc_0` is under
5% of the time, this is closed"*, and the measurement was never taken.

Second, and more useful: the accumulator update is **memory-bound, not compute-bound**. It touches
10.5 weight columns per update, 2 KB each, drawn at effectively random offsets from a **170 MB**
table — 1330 cycles measured against ~337 if the data were in cache. Vector instructions are not
the constraint; they are waiting.

### Dead threat tuples — +3.54% NPS

When the threat feature set dropped pawn→pawn relations and pushes (60,720 → 59,808 inputs), the
refresh path was updated to match but the incremental path was not. It kept generating those
tuples: each one took a slot in the dirty list, went through index computation and a prefetch, and
was then discarded by the bounds filter. **12.6% of every threat tuple the engine produced was
thrown away** — 178,975 of 1,416,458 in a bench run. Now 3.8%.

> **+3.54% NPS**, measured paired-interleaved over 40 book positions at depth 19, faster in 28 of
> them, with node counts identical on both sides (31,135,764). About +2.8 Elo.

<sub>Two notes on method. The gain is **+1.68% on AVX-512 and +3.54% on AVX2** — the AVX2 path
tiles the accumulator in 8 passes rather than 2, so removing work upstream is worth more there.
Since rating lists compile AVX2, that is the number that counts, and profiling on AVX-512 would
have understated it by half. And a companion change — rewriting the `PawnPair` refresh from an
O(n²) double loop to the precomputed file band its own incremental path already used — measured
**−0.10%: nothing**. Both results point the same way: on a memory-bound path, removing instructions
does not pay, removing memory traffic does.</sub>

### Pawn-block refresh cache — +1.37% NPS

The finny table covers only `HalfKAv2_hm`: 22,528 of 86,992 inputs. The other three blocks — the
other 74% of the feature space — were rebuilt from scratch on every full refresh, which is 8% of
search wall time.

`FullThreats` cannot be cached, it depends on the whole position. But `PawnPair` and `PassedPawns`
depend on exactly `(white pawns, black pawns, orientation)` — verified in their `make_index` — and
refreshes are triggered by **king** moves, which leave pawns untouched. Between consecutive
refreshes the key is almost always unchanged. The cache is direct-mapped, 8 entries per thread,
keyed on the full pawn bitboards rather than a hash, so a collision is impossible by construction.
A hit skips both the enumeration and the sparse column reads, replacing them with one contiguous
2 KB load.

> **+1.37% NPS on AVX2**, paired-interleaved over 60 positions at depth 19, faster in 40 of them
> (sign test p ≈ 0.009), node counts identical. About +1.1 Elo.
> On AVX-512 the same patch measured **−0.11%** and is compiled out there: the wider tiles leave
> less to gain and the extra vector array costs more. Rating lists build AVX2.

<sub>The bug worth recording is what it took to get there. Caching the feature-transformer vector
alone left the bench at 262,736 instead of 207,259, and three plausible theories about the key were
all wrong. The cause was that active features also feed the **PSQT accumulator** through
`threatPsqtWeights`, in a second loop further down the same function — so a hit produced a correct
feature-transformer vector and a silently stale PSQT. Bisection found it in three builds where
reasoning had failed in three attempts: force a miss (bench correct → the miss path is fine), then
recompute on hit and compare against the cached vector (no mismatch → the key is fine), which left
only the code the hit path skipped.</sub>

### What is *not* worth doing, measured

Two candidates were killed by measurement before any of them cost a day of work.

**Lazy accumulator updates.** Stockfish defers accumulator work and walks the chain when an
evaluation is finally needed; we update eagerly, so any update made for a node that never evaluates
is pure waste — and the incremental update is ~31% of wall time. Instrumenting it gives **0.91
updates per evaluation** (185,992 against 203,286). There are *fewer* updates than evaluations: the
eval cache and the refresh path already absorb the difference, and there is nothing to reclaim.

**Micro-optimising the rest.** With per-phase counters on the current binary: move generation 2.3%,
transposition table 2.3%, make+unmake 4.3%, `fc_0` 3.7%. Zeroing any one of them entirely would
return less than the refresh cache above.

### Multithreading

Rating lists run 4 CPU in the upper part of the 40/15 list and 8 CPU in Blitz, so this is not
academic. Two SMP options had never been tested: `ThreadVoting`, a Stockfish-style weighted vote
that had never been switched **on**, and `DiverseSMP`, which had been switched on *without* a test.

> Gauntlet at 4 threads, 5,598 games at 12+0.12: `ThreadVoting=true` **−4.10 ± 6.17**,
> `DiverseSMP=false` **−4.09 ± 6.18**. Both defaults hold.

The vote implementation was then checked line by line against Stockfish and is faithful — same
`(score − minScore + 14) × depth` weighting, and `depth` is assigned only on a *completed*
iteration, which is the easy thing to get wrong. It simply does not pay here.

<sub>A note on measuring SMP at all, since we got it wrong first. Time-to-depth is the metric for
split-point schemes, where threads divide the tree; under lazy SMP threads search the *same* tree
with different orderings and trade work through the shared table, so nominal depth grows little
while its quality grows a lot. Our time-to-depth speedup at 4 threads is 1.1–1.6x, which looks
alarming and is not: rating lists show ~40 Elo from 1 to 4 CPU, in line with comparable engines.
NPS is wrong in the other direction, counting duplicated nodes as progress. For multithreading the
only honest metric is Elo in games.</sub>

### Mailbox — +2.13% NPS

`ThreadData` carried only bitboards, so finding *which* piece stands on a square meant scanning up
to six of them with as many unpredictable branches. That loop existed twice: in `td_score_move`,
for the victim of every capture scored, and in `td_make_move`, for the captured piece. A
`piece_on[64]` array answers both in one load. It is maintained at the three sites where
`td_occ_update` is called — make-forward, illegal-move rollback, unmake — which is what makes the
coverage structural rather than a matter of remembering every branch.

> **+2.13% NPS**, paired-interleaved over 150 positions at depth 19, faster in 102 of them
> (sign test p ≈ 7e-6), node counts identical. About +1.7 Elo.

<sub>Two method notes, both from getting it wrong first. The engine's `perft` uses the **global**
`make_move` and never reaches `td_make_move`, so it could not have validated this: a desynchronised
mailbox gives no crash and no changed bench signature, only move ordering that is occasionally
wrong. A `-DTRIUMV_MAILBOX_VERIFY` build compares mailbox against bitboards square by square at all
three sites and aborts on the first mismatch; a full bench passes clean, castling and en passant
and promotions included. And the first two measurements both said "noise" — +0.64% at 32/60 on a
non-PGO build, +1.19% at 35/60 on PGO. Only 150 positions resolved it. Sixty positions were enough
for the +3.54% of the dead threat tuples and are not enough for +2%: the sample has to be sized to
the effect being looked for, or a real gain gets discarded as noise.</sub>

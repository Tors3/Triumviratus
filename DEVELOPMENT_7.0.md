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

Effect measured at the time with the old harness, and withdrawn: change 1 was reported as a small
gain, changes 2 and 3 as nothing. All three are strictly less work and remain in the engine.

<sub>Worth recording, since it sets expectations for further work of this kind: generating one extra
capture is a `pop_lsb`, a test, an encode and a store. On an out-of-order core with the data in L1
that hides in the shadow of the memory-bound network evaluation, which is ~58% of node time. The
move generator was not the bottleneck, and micro-optimising it does not become one.</sub>

### Quiet promotions in qsearch — +8.38 Elo

The first change in 7.0 that moves playing strength rather than speed, and the only one that
survived a two-day campaign in which five other candidates measured zero.

**The defect.** Quiet promotions were generated only inside `if (!captures_only)` — that is, in the
quiet buffer. Qsearch passes `captures_only = true`, so it **never saw them**. Meanwhile the engine
declares those same moves tactical in two separate places: they score 750,000 in `td_score_move`,
and they are exempt from LMP, futility, history and SEE pruning in the move loop. A move the search
treats as tactical everywhere else was being generated as a quiet one, and therefore dropped
exactly where tactics are resolved. Hobbes (#9 CCRL) generates them as `Noisies`: excluding them is
not a universal choice.

**Six variants, separated by the bench.** The first implementation lost 11.77 Elo. The node counts
say why, and they cost zero games — two minutes per variant:

| mode | nodes | vs off | what it does |
|---|---|---|---|
| 0 | 207,259 | — | off |
| 1 | 242,764 | +17.1% | all four promotions — with `QSMoveCap=3` they **evict** every capture |
| 2 | 241,596 | +16.6% | queen only (under-promotions were worth 1,168 nodes, 0.5%) |
| 4 | 207,259 | 0.0% | move-picker half only — a measured **no-op** |
| 5 | 246,329 | +18.9% | queen exempt from the `QSMoveCap` counter |
| **6** | **225,898** | **+9.0%** | 5, and only if the queen is not given away (SEE ≥ 0) |

Mode 1 is arithmetic, not bad luck: `55·log₂(1.171)` = −12.2 Elo of nodes, measured −11.77. It paid
exactly for the nodes it added and bought nothing, because with a cap of three the new promotions
displaced the captures instead of joining them.

Mode 6 fixes both halves. Exempting the promotion from the counter stops the eviction; the SEE
filter stops it opening a whole new subtree — a new queen generates all her captures and all the
recaptures — for a move that loses material. That filter alone halves the cost, from +18.9% to
+9.0%.

> **+8.38 ± 6.67** over 2,572 games at 25+0.25, LOS 99.31%, LLR 1.53 — 1 thread, 128 MB hash,
> UHO_4060_v4, dev build with all 383 options announced.

> [!WARNING]
> **The engine signature changes with this bake: `bench` goes from 207,259 to 225,898.** Any
> procedure that checks 207,259 as proof of identity is now checking against the wrong constant.

<sub>Two diagnoses were falsified by the bench bisect within minutes, and both were mine. "The
move-picker half is the dominant cost" — it is a no-op: `skip_quiets` is set *inside* the move loop,
by which point the quiet stage has already passed, so it never cancels a promotion. "The
under-promotions are expensive" — they are 0.5%. The real cost was the new queen in qsearch, which
opens a subtree with no depth limit. Node counts do not predict Elo, but they do decompose a cost,
and they do it without playing a single game.</sub>

<sub>The other lesson is about giving up too early. Mode 1 measured −11.77 at 12+0.12 and the idea
looked dead. Between mode 1 and mode 6 there are **20 Elo** at the same underlying idea, and the
regime mattered as much as the variant: the node cost is proportional and does not change with the
time control, while the value of the information grows with depth. The same code reads −11.77 at
12+0.12 and +8.38 at 25+0.25.</sub>

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

### The per-change NPS figures below are withdrawn

Every NPS figure in the sections that follow was produced by a paired-interleaved harness that ran
the two binaries in a fixed A-then-B order on each position. The design assumed that drift between
two adjacent searches, under a second apart, was negligible. It is not. Whichever binary occupies
the second slot inherits the machine's thermal drift, in whichever direction that drift happens to
be going.

The harness was eventually run against itself — same binary, same options, both sides, no
difference of any kind — and reported **135 of 150 positions won, z = 9.72** on a host still
cooling from a build, and a comparable effect with the opposite sign on a cold host warming up. A
tool that manufactures a nine-sigma result from nothing cannot certify a one-percent one.

It now alternates the order within each pair, excludes exact ties from the sign test, refuses to
run when a requested option is not announced by the engine, and reports the A-first and B-first
sub-samples separately as a built-in drift check. Against itself it now returns z = 0.06 with the
two sub-samples agreeing to a hundredth of a percent, on two different machines.

Re-measured with the repaired harness, on two independent PGO builds per arm and with a null
control run alongside: several of the changes below hold up but at a fraction of their published
value, at least one is a loss and has been removed from the binary, and their sum is not additive —
they compete for the same memory bottleneck, so gains that add on paper cancel in practice.

The individual figures are therefore withheld rather than restated. What will be published is a
single end-to-end measurement of the finished engine, taken with the repaired harness and its
control.

### Dead threat tuples

When the threat feature set dropped pawn→pawn relations and pushes (60,720 → 59,808 inputs), the
refresh path was updated to match but the incremental path was not. It kept generating those
tuples: each one took a slot in the dirty list, went through index computation and a prefetch, and
was then discarded by the bounds filter. **12.6% of every threat tuple the engine produced was
thrown away** — 178,975 of 1,416,458 in a bench run. Now 3.8%.

> Figure withdrawn — see the note above. The change itself is unchanged and still in the engine.

<sub>One note on method that survives the withdrawal: the gain was consistently larger on AVX2
than on AVX-512, because the AVX2 path tiles the accumulator in 8 passes rather than 2, so removing
work upstream is worth more there. Rating lists compile AVX2, so profiling on AVX-512 understates
this class of change. A companion rewrite of the `PawnPair` refresh — from an O(n²) double loop to
the precomputed file band its own incremental path already used — measured as nothing, which points
the same way: on a memory-bound path, removing instructions does not pay, removing memory traffic
does.</sub>

### Pawn-block refresh cache

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

> Figure withdrawn — see the note above. The cache is kept on AVX2 and compiled out on AVX-512,
> where it measured slightly negative: the wider tiles leave less to gain and the extra vector array
> costs more. Rating lists build AVX2.

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

### Mailbox

`ThreadData` carried only bitboards, so finding *which* piece stands on a square meant scanning up
to six of them with as many unpredictable branches. That loop existed twice: in `td_score_move`,
for the victim of every capture scored, and in `td_make_move`, for the captured piece. A
`piece_on[64]` array answers both in one load. It is maintained at the three sites where
`td_occ_update` is called — make-forward, illegal-move rollback, unmake — which is what makes the
coverage structural rather than a matter of remembering every branch.

> Figure withdrawn — see the note above. The change itself is unchanged and still in the engine.

<sub>Two method notes, both from getting it wrong first. The engine's `perft` uses the **global**
`make_move` and never reaches `td_make_move`, so it could not have validated this: a desynchronised
mailbox gives no crash and no changed bench signature, only move ordering that is occasionally
wrong. A `-DTRIUMV_MAILBOX_VERIFY` build compares mailbox against bitboards square by square at all
three sites and aborts on the first mismatch; a full bench passes clean, castling and en passant
and promotions included. And the first two measurements both said "noise" — on a
non-PGO build and again on PGO. Only a larger sample separated it from zero: the sample has to be
sized to the effect being looked for, or a real gain gets discarded as noise. That lesson stands
even though the figures that taught it do not.</sub>

### Hybrid accumulator update

`HalfKAv2_hm::requires_refresh` returns true for **every** move of one's own king, so every king
move costs a full refresh. For HalfKA that is cheap — the finny table covers it — but threats,
pawn pairs and passed pawns were rebuilt from scratch each time. They did not need to be: their
indices depend on `OrientTBL[ksq]`, which takes **two values only** and changes solely when the
king crosses the d/e file. Every other king move was discarding still-valid work.

Ported from Stockfish `db98633b`. When the king stays on its half of the board, the previous
accumulator is reused instead:

```
new_acc = HalfKA_new + prev_acc − HalfKA_prev + Δ(threat/pp)
```

Neither HalfKA accumulator has to be stored: both are reconstructed from the finny table — the new
one as the refresh already does, the previous one from the old king square's entry, applying the
diffs against the pre-move position rebuilt from the dirty-piece record. Gated on at least 15
pieces (below that, rebuilding the few active features is cheaper than recovering the previous
HalfKA) and on non-castling moves.

> Figure withdrawn — see the note above. The change itself is unchanged and still in the engine.

<sub>Worth noting against the source: the patch is reported as a small gain in Stockfish and was
measured larger here, because our refresh is a bigger share of the wall (8.0%) and threats are
59.6% of its columns. It also composes with the pawn-block refresh cache above rather than
replacing it — that one covers the remaining 40.4%, and still applies when the hybrid path cannot
(king crossing the centre, castling, few pieces, previous accumulator not computed).</sub>

### Prefetch the HalfKA weight rows — removed

The incremental accumulator update reads about 10.5 weight rows per call, at random offsets in a
table of roughly 110 MB: it is latency-bound, not arithmetic-bound. Threat rows were already
prefetched; the **HalfKA rows were not** — and they are the largest (int16 × 1024 = 2048 bytes,
twice a threat row) and the first ones `apply_combined` consumes, so their latency was fully
exposed.

The prefetch only pays if something covers the latency, so the PSQ index list is now built
**before** the threat lists — the lists are disjoint, so the order is functionally irrelevant —
and the whole threat-list construction sits between the prefetch and its use.

> Figure withdrawn — see the note above. Re-measured with the repaired harness this change reads
> **negative on two different CPUs**, so the published gain is almost certainly the wrong sign. It is
> left enabled for now: the re-measurements are not yet internally consistent (removing it and the
> mailbox together does not recover what removing either alone appears to gain), and that has to be
> resolved before anything is taken out of the engine.

<sub>The original reading is a good illustration of the failure mode: the median wandered between
samples while the fraction of positions won stayed put, which was read as the fraction being robust
— when in fact both were tracking the same drift. The re-measurement is not finished. Both this
change and the mailbox read as losses when tested one at a time, but a build with both disabled is
indistinguishable from the current one, which cannot all be true at once; the likeliest explanation
is that PGO build-to-build variation is larger than the effects being chased, and that has to be
measured before either is removed.</sub>

### Where prefetching stops paying — two rejected variants

Two more prefetch changes on the same code path were measured and rejected, and together with the
one above they give a rule rather than three anecdotes.

| change | result |
|---|---|
| prefetch all four SIMD tiles of a threat row instead of only the first line | clear loss |
| prefetch the PSQT rows (one cache line each, ~10.5 per update) | clear loss |

The first fails because the remaining lines of a row are **sequential**: the L2 streamer already
had them, so the extra prefetches bought no coverage and cost load slots and fill-buffer entries.
One line per row is the optimum, not a compromise.

The second fails on size. The FT weight tables are ~61 MB (threats) and ~46 MB (HalfKA) — misses
are guaranteed. The PSQT tables are **2.6 MB in total**: they live in L2/L3 and were already hits,
so the prefetch was a pure instruction cost in the hottest loop of the engine.

> Before writing a prefetch: *does the table fit in cache?* and *is this line reachable by
> sequentiality from one already touched?* Either answer being yes means the prefetch is dead
> weight.

<sub>This front had been closed since 15 July by a large negative figure for prefetching HalfKA
rows, taken with an even older sequential harness that ran all of A and then all of B — the same
tool that had called the FT permutation a loss when it is a gain. The note in the source attributed
that result to "codegen pessimisation of the hot template", an explanation invented after the fact
for a number the instrument could not produce reliably. The front was reopened on that basis; with
the repaired harness the HalfKA prefetch is a loss after all, for reasons that have nothing to do
with codegen.</sub>

### Permuting the weight rows for locality

The incremental accumulator update is memory-latency bound: 1423 cycles per update against
337 of actual arithmetic. The question nobody had asked was *how the accesses are
distributed*. Instrumented, over 150 book positions at depth 16 — 404 million row accesses:

```
rows 64,464 total, 46,023 ever touched
  top    644 rows (1.0%) = 49.0% of accesses   [0.63 MB]
  top  3,223 rows (5.0%) = 83.0%               [3.15 MB]
  90% of accesses -> 5,108 rows                [4.99 MB]
```

**Five megabytes carry 90% of the traffic, and they are scattered over 63 MB.** That is the
whole story: with 4 KB pages the weights span 16,116 pages, no TLB maps that, and every
access is a DRAM miss on a region that would fit in L3 if it were packed. Large pages would
be the hardware answer, but they need a Windows privilege no tester enables — a permutation
is the software substitute, and it ships inside the binary.

So the rows of `threatWeights` are reordered by access frequency at load time, hot ones
first and contiguous, and the indices are remapped through a 130 KB lookup table that stays
in L2. **The evaluation is unchanged** — this only moves rows around.

> Figure withdrawn — see the note above. Re-measured with the repaired harness the permutation is
> **not distinguishable from zero** on an EPYC Zen 2; it is kept, since the layout argument below is
> hardware-dependent and the change cannot cost anything by construction.

<sub>One lesson here survives and one has to be inverted. **Surviving: measure in the regime you
are judged in.** These runs use Hash 256, like CCRL, not the Hash 64 the harness defaulted to; with
a small transposition table there is far less pressure on L3, and a locality optimisation measured
there is measured where it matters least. **Inverted: pooling samples to reach significance.** Two
of the three samples here read "not significant" and the third was an outlier; pooling them was
presented as sample size scaling with the inverse of the effect. With the order defect understood,
a heterogeneous set of samples pooled until it crosses a threshold is exactly the shape a drifting
instrument produces, and the pooled p-value was not evidence of anything.</sub>

<sub>Two design questions were settled by measurement rather than intuition, each for the
price of one build. Permuting at *block* granularity would have cost nothing at runtime —
the index is a sum of three constant tables, so reassigning them needs no lookup — but the
heat is spread *within* blocks (90% of accesses touch 108 of 252 blocks), so block reordering
would still span 27 MB. And the whole thing was first built with the **identity** permutation
as a safety net: bench had to stay 207259 before the real table was generated, so that a
change in the bench could only mean the data was wrong, never the wiring.</sub>

<sub>The trap, worth recording because it does not announce itself: for an excluded feature
`FullThreats::make_index` does not return a constant — it returns `base + offsets + lut2`, a
*variable* value above the threshold. The marker was `Dimensions` (59808), which is exactly
the first valid row of the folded PawnPair segment; under a permutation those dead indices
would have landed on real rows. Relocated to `FeatRows` with a sentinel tail in the table, so
the filter stays a single branchless read.</sub>

### The same permutation on HalfKA — rejected, and it explains the first one

HalfKA looked like the better candidate. It is the largest block of the transformer (22,528
rows of 2048 bytes = 46 MB) and, instrumented, it is *more* concentrated than the threat
table: **90% of accesses fall in 910 rows = 1.78 MB**, with the top 1% (225 rows, 0.44 MB)
carrying 59%.

Permuting it measured a clear loss, consistently across two samples and on the same side both
times. The magnitude is withdrawn with the rest; the sign is not in doubt, and the reason is the
useful part.

The reason is the useful part:

```
make_index = (s ^ OrientTBL[ksq] ^ flip) + PieceSquareIndex[pc] + KingBuckets[ksq]
```

For a fixed piece and king, the 64 squares are **64 consecutive rows** — 128 KB contiguous.
A refresh walks the pieces and touches contiguous blocks: sequential access, which the
hardware prefetcher already serves perfectly. **HalfKA is laid out optimally by
construction**, and permuting by frequency destroys that structure, replacing sequential
accesses with scattered ones.

The threat table has no such property: its index is `base(attacker, attacked) + offsets(from)
+ lut2(to)`, and consecutive rows have nothing to do with each other. There was no structure
to break, which is why compacting pays there and only there.

> Before permuting anything, the question is not only *how concentrated* the accesses are but
> *how they are already laid out*. If the hot indices are contiguous by construction, a
> frequency permutation can only make things worse.

<sub>Kept behind an explicit opt-in as a documented baseline. If it is ever revisited: it is
not compatible with the ICL target, where HalfKA indices come from the vectorised
`write_indices` and never pass through `make_index` — permuting the weights without permuting
those indices would produce wrong evaluations silently, with no crash and no reliably
different bench.</sub>

### Tuning the clustering: what the co-location proxy is not

Three follow-ups, each one build and one 150-position measurement at Hash 256, node counts
identical throughout:

| change | co-located pairs | result |
|---|---|---|
| group rows 16 at a time instead of 4 | 6.79% → 18.02% | rejected |
| extend clustering from 4096 to 8192 hot rows | 6.79% → 5.18% | kept |
| cache the minor/major correction keys per position | — | rejected |

The first two settle the mechanism: 4 rows is 4 KB is one TLB page, and grouping wider optimises
a metric the hardware does not use. The second row also shows what the co-location fraction is
worth as a predictor — it said the wider clustering was *worse* and it was the best of the three.

> Every prediction made from that proxy was wrong, in both directions: a gain where "zero to
> +0.5%" was expected, a loss where it promised the most, and the best result where it promised the
> worst. It measures that the clustering is doing something, not how much that something is worth.
> A build costs two minutes and a measurement twenty-five; reasoning about the outcome costs more
> and gets it wrong.

<sub>Same verdict on caching the minor/major correction keys, which looked like a clear defect:
each key costs a scan of four bitboards and, with the current defaults, they were recomputed up
to six times per node. But those four bitboards are 32 bytes that never leave L1, and the key
comparison plus two extra fields in an already-hot thread struct cost more than the scan they
save. The same shape as the rejected PSQT prefetch — where the data is already close, adding
machinery to reach it faster takes away rather than gives.</sub>

### NPS work, cumulative

Four independent changes were bundled here — refresh cache, mailbox, hybrid update, HalfKA
prefetch — and an end-to-end figure was published for the whole engine against the 31 July release
binary, both AVX2, both PGO, both `bench 207259`.

> Figure withdrawn — see the note above. It was taken with the same fixed-order harness as the
> individual ones, so it inherits the same defect; and two of the four changes in the bundle have
> since been re-measured as losses and removed from the binary. A replacement end-to-end figure will
> be published once the engine is final, taken with the repaired harness and a null control run
> beside it.

<sub>Two method notes that outlived their numbers. Measurements must be taken on **PGO** builds:
the same patch reads differently on a plain -O3 binary, because the profile decides layout and
inlining in the hot path. And they must be taken on **two independent PGO builds per arm** rather
than by flipping a runtime switch inside one binary: the profile is trained on the default side of
that switch, so the other side runs code the compiler treated as cold, and the comparison is biased
before it starts.</sub>

### Updating both perspectives in one pass

A threat diff entry packs five bitfields: attacker, attacked piece, from-square, to-square, and the
add/remove flag. The accumulator used to walk that list **twice**, once per perspective, with a full
accumulator update in between — so every entry was decoded twice and, worse, was long evicted by the
time the second pass read it.

`append_changed_indices_both` walks it once, decodes each entry once, and feeds both perspectives'
index lists. `make_index` still runs per perspective and cannot be shared: orientation, piece swap
and the king square all differ between white and black.

<sub>This is a deliberate departure from Stockfish's `7b550409`, which alternates the two
perspectives at every transition of the chain. That form was ported first and measured **−0.70%**
here: alternating keeps two 2 KB accumulators live while ~21 KB of weight columns are streamed, and
on a memory-bound path the locality of the large structure outweighs the locality of the dirty list.
Sharing only the dirty-list pass, and keeping the wide writes sequential — all of white, then all of
black — takes the gain without the cost.</sub>

> Figure withdrawn — see the note above. This one was the weakest of the series even on its own
> terms, and it is the one that prompted the audit of the harness. Re-measured with the repaired
> tool the change is a real gain, but a smaller one; the number will be folded into the end-to-end
> measurement rather than published on its own.

<sub>This section originally argued that the three medians agreeing closely, despite the host's
clock swinging 12% between runs, showed the paired-interleaved design absorbing a noisy host. The
argument was wrong in an instructive way. Alternating A and B position by position does cancel
*slow* drift, but the two searches in a pair are not simultaneous, and whichever one runs second
absorbs whatever the machine does in between. Agreement across three samples taken on the same
drifting host is not independent confirmation — it is the same bias reproduced three times.</sub>

### Build targets: `avx512` no longer requires VBMI2

"AVX-512" is a family, not a switch. Skylake-X, Cascade Lake and Cooper Lake have F/BW/DQ/VL
(and VNNI from Cascade Lake on) but **not** VBMI/VBMI2/BITALG; Ice Lake and later, and AMD Zen 4/5,
have them.

The 7.0 build script defined `USE_AVX512ICL` inside the `avx512` target, together with
`-mavx512vbmi -mavx512vbmi2 -mavx512bitalg`. It had been set up for the GCP machines used for
testing — Sapphire Rapids and Zen 4, both of which have VBMI2 — but that is the binary shipped as
"avx512", and on a Skylake-X or Cascade Lake it dies on an illegal instruction at startup.

Verified on the binaries rather than the scripts, counting ICL instructions with `llvm-objdump`:

| binary | ICL instructions |
|---|---|
| `Triumviratus_6.0_avx512.exe` (shipped) | **0** — unaffected |
| `Triumviratus_7.0_avx512.exe` (pre-fix) | 44 |
| `avx512` target after the fix | **0** |
| `avx512icl` target | 44 |

The 6.0 release is clean: its project file never defined the macro, so the flags were inert. The
defect appeared with 7.0, where the script sets the macros explicitly, and was caught before
release.

The targets are now separate, as in Stockfish: `avx512` (F/BW/DQ/VL/VNNI, scalar threat emission)
and `avx512icl` (+ VBMI/VBMI2/BITALG, vectorised `write_multiple_dirties`).

> And the ICL path measured as **exactly nothing** — 75 of 150 positions, z ≈ 0, on Zen 4: a
> result that needs no scale to interpret, and the one figure here the harness defect cannot have
> manufactured. So `avx512` ships **without** ICL: broader compatibility at no measurable cost.

<sub>That zero also says where the mirror-position catch-up cost is *not*. Replaying moves onto the
mirror `Position` and generating the threat diff is 6.5% of the wall — more than `fc_0`, more than
movegen and TT combined. Vectorising the emission loop buys nothing, so the cost is in the magic
lookups and bitboard work of `update_piece_threats`, not in writing the tuples out.</sub>

<sub>Profiling note: `prof_n_eval` was incremented twice per `transform()` call, so the recorded
"0.91 accumulator updates per evaluation" was really **1.83**. The conclusion is unchanged —
intermediate updates are mandatory steps towards the current node, not wasted work — but the
figure now reflects reality.</sub>

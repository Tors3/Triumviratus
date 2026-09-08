<div align="center">

<img src="logo.png" alt="Triumviratus" width="200">

# Triumviratus 7.0 — development log

**Network project.** First full training from scratch · `legio-septima` · SFNNv16 + PassedPawns

**by Francesco Torsello**

<sub>in collaboration with Maurizio Platino</sub>

</div>

---

<div align="center">

[The network](#1-the-network) · [Measured Elo](#2-measured-elo-incremental) ·
[Speed work](#3-speed-work-nps) · [Search structure](#4-search-structure-measured-not-assumed) ·
[6.0 log](archive/DEVELOPMENT_6.0.md) · [Networks](NETWORKS.md)

</div>

---

> [!IMPORTANT]
> **6.0 is and remains the official release.** The 7.0 network is finished — stage 2 closed at
> epoch 799 — but there is no 7.0 release yet. Every figure below carries its error bar.

---

## 1. The network

A full audit of the search in July 2026 reached a blunt conclusion: the remaining gap to the
strongest engines is **≈ 25–40 Elo of network, not of search**. So 7.0 does not try to
out-search anyone — it changes how the network is *made*.

The first own-lineage net was trained from scratch, but the two that followed grew by
**grafting** a new input block onto a frozen predecessor. Grafting is cheap and it worked, but a
frozen base can only ever *add* what the new block can express — it cannot re-learn what the rest
of the network already believes. **7.0 trains base and feature blocks together, from scratch**, on
a corpus an order of magnitude larger.

`TRANN2` is Stockfish's SFNNv16 feature set plus a `PassedPawns` block of our own:

| block | inputs |
|---|---|
| `FullThreats` | 59,808 |
| `HalfKAv2_hm` | 22,528 |
| `PawnPair` | 4,560 |
| `PassedPawns` | 96 |
| **total** | **86,992** |

L1 1024, L2 32, eight LayerStacks. Two stages: a long stage 1 on the bulk corpus, then a stage 2
with the value/policy blend annealed. The shipped net is the final stage-2 checkpoint,
`legio-septima`.

**Corpus, recipe, hyper-parameters, the epoch-by-epoch history and the reasoning behind the
feature set are in [NETWORKS.md](NETWORKS.md).** Nothing about training is repeated here.

---

## 2. Measured Elo, incremental

Each stage is measured **against the one above it**, not against 6.0. The stages therefore
compose, but the total is *not* their sum — the whole engine against 6.0 is measured separately
and reported under the table.

| # | stage | what it is | TC | games | Elo |
|---|---|---|---|---|---|
| 1 | `6.0` → **network** | full training from scratch, `TRANN2`. Measured on the 7.0 binary **frozen before any search change**, so the figure isolates the net | 15+0.15 | 1,442 | **+23.41 ± 9.22** (LOS 100%) |
| 2 | → **quiet promotions in qsearch** | `PromoQS=6`: promotions generated in qsearch, exempt from the capture cap, filtered by SEE ≥ 0 | 25+0.25 | 2,572 | **+8.38 ± 6.67** (LOS 99.31%) |
| 3 | → **corrections block, retuned** | material correction table switched back on and the block rebalanced around it: material weight 67, continuation weight 85, cap 48 | 30+0.3 | **30,530** | **+2.65 ± 2.14** (LOS 99.25%, LLR 2.96) |
| 4 | → **qsearch delta pruning restored** | `QSDeltaMargin` 3000 → 1525, co-tuned with `QSCaptHistScale` 86 → 72. 3000 was the **maximum of the parameter's own range**, i.e. delta pruning was effectively off | 30+0.3 | 5,994 | **+5.57 ± 4.84** (LLR 1.89, stopped before the bound) |
| 5 | → **material correction table removed** | `CorrMaterial` off again. Stage 3 changed three things at once, so it measured the block; isolated — continuation weight and cap identical on both sides — the table loses. Figure is for the engine *without* it | 20+0.2 | 1,298 | **+18.49 ± 11.43** (LOS 99.93%) |
| 6 | → **TT eval decay fixed** | `TTEvalNoDecay=1`: the static eval written to the transposition table went through a round trip that truncates toward zero and **compounds on every revisit**, so the error was systematic and one-directional. The original value is stored instead | 20+0.2 | 2,618 | **+18.06 ± 8.07** (LOS 100%, LLR 2.95, bound crossed) |
| 7 | → **rule50 formula aligned** | `Rule50Formula=1`: the pair that de-damps and re-damps the eval stored in the table inverted `v*(200-fifty)/214`, a formula from an older wrapper, while the damping actually applied is `v*(199-rule50)/199`. Taken **on correctness, not on Elo** — see below | 15+0.15 | 6,002 | **+1.04 ± 4.90** (neutral) |
| 8 | → **negative extension on alpha** | `NegExtAlpha` 1 → 2: when the TT move does not even reach alpha the node is neither singular nor promising, so the extension shrinks further. One parameter, nothing else touched | 30+0.3 | 3,958 | **+6.50 ± 5.77** (LOS 98.64%) |
| 9 | → **eval-stability window made honest** | `TMv2EvalPrevAvg=1` with `TMv2EvalWindow` 10 → 20. The counter compared the score against a moving average that had **already absorbed that same score**, so the measured difference was exactly half the real one and the parameter meant double what it said. The pair keeps the effective threshold identical — taken **on readability, not on Elo** | — | — | **no measurable change by construction** |
| 10 | → **four Stockfish-19 structural differences, bundled** | probcut-from-TT freed from its in-check and capture-only gates; null-move verification only from depth 16 as upstream does, instead of at every depth; no internal iterative reduction at ALL nodes; correction history also learning on fail-low nodes | 10+0.1 | **11,182** | **+5.31 ± 3.44** (LOS 99.88%, LLR 1.66) |
| 11 | → **history pruning widened** | `ContHistPruneDepth` 2 → 6 with `HistPruneMargin` 2097 → 1200. Not a port: it comes from measuring our own tree shape against Stockfish's — see §4 | 10+0.1 | **19,228** | **+4.48 ± 2.65** (LOS 99.95%, LLR 2.33) |
| 12 | → **effort cut after alpha rises** | `AlphaDepthDecAmt` 1 → 3. Once a move has raised alpha the node holds a *real* best; the moves after it only have to beat that one, so upstream drops three plies where we dropped one | 10+0.1 | 12,022 | **+8.64 ± 3.39** (LOS 100%, **LLR 2.95 — bound crossed**) |

<sub>Stage 8 is worth recording for how it was found, because the obvious reading is the wrong one.
The audit that led to it started from a genuine defect: the third arm of the negative-extension
chain, `NegExtCut`, is **unreachable**. On a non-PV node the window is null, so every score is
either at or above beta or at or below alpha, and the two arms above it cover both cases; the
bench confirms it, with the parameter at 0, 1, 2, 3 and 4 all returning exactly the same node
count. Its shipped value was 3, the top of its own range, which means a tuning run had once
optimised noise. Reordering the chain the way Stockfish does makes the branch live — and **it does
not pay**: in a six-way gauntlet every configuration involving the reorder finished below the
unmodified engine. What paid was a parameter three lines away that had always worked and had never
been questioned. The defect did not contain the Elo; looking for it is what put the whole family
under review.</sub>

<sub>Stage 7 is the one entry here that was **not** taken for its Elo. `+1.04 ± 4.90` establishes only
that it does no harm; the interval is far too wide to call it a gain, and the SPRT was stopped
without reaching a bound because with a true effect near +1 it would never reach one. It was baked
because the alternative was keeping two different formulas for the same rule50 damping inside one
engine, so that anyone later reasoning about what the table holds starts from a false premise.
That is exactly how stage 6 — worth +18 — came about: a comment that described the defect
correctly sitting above a constant that did something else.</sub>

<sub>Stage 9 has no Elo column because there is nothing to measure: `avg_new = (avg_prev + score)/2`
means the difference the counter saw was `(score − avg_prev)/2`, so doubling the window restores
the same threshold. Two caveats stated plainly. It is **not** byte-identical: the division truncates
toward zero, and which way it truncates depends on the *sign* of the sum, so at exactly
`|score − avg_prev| = 21` with an odd sum the old and new answers differ by one counter tick —
a band one centipawn wide. And `bench` cannot see any of this, since the counter feeds only time
allocation and the bench runs at fixed depth; 252,074 is unchanged, but that confirms nothing about
this change. It was taken because the parameter sits in the SPSA space, and a parameter that lies by
a factor of two makes every tuning run start from the wrong coordinates. That is not hypothetical:
the campaign built on this fix was launched with the window initialised to 5 — the arithmetic was
inverted, the invariant point is 20 — and a thousand iterations were spent in a regime four times
tighter than the shipped one, where the high indices of `TMv2Eval[]` almost never fire. The tuning
found nothing (largest per-parameter drift 1.6 σ over 1,000 iterations, and the resulting vector read
−6.18 ± 8.29 at 60+0.6). The block is left where it was; only its units were fixed.</sub>

**The whole engine, against 6.0.** Not a stage: the shipped 6.0 binary against the current 7.0
build, each loading its own network, AVX-512, one thread, UHO_4060_v4. Measured at three time
controls to see whether the advantage survives longer thinking:

| TC | hash | s/side | **depth** | games | Elo |
|---|---:|---:|---:|---:|---:|
| 5+0.05 | 64 MB | 7 | 12.4 | 1,926 | +30.02 ± 8.81 |
| 25+0.25 | 256 MB | 35 | 17.3 | 3,170 | **+28.34 ± 6.19** |
| 40+0.4 | 256 MB | 56 | **19.3** | 3,000 | **+33.22 ± 6.14** |

LOS is 100% at all three. The longest point is also the strongest: PairsRatio 2.23, better than two
won pairs for every lost one.

<sub>**The advantage does not decay with depth**, which was the open question. Over seven plies —
12.4 to 19.3 — the figure stays between 28 and 33, and the weighted slope is **+0.42 ± 1.56 Elo per
ply**, indistinguishable from flat. Had this been a gain that only lives at shallow depth, the slope
would have come out clearly negative. The depths are **measured from the PGNs**, not inferred from
the time control, because two machines at the same TC were found to differ by 3 plies.</sub>

<sub>⚠️ CCRL Blitz runs at roughly 160 s per side, some three times beyond the longest point here, so
the curve still has to be extrapolated. Extrapolating a flat line is a much smaller act of faith
than extrapolating a falling one, but it is an extrapolation.</sub>

<sub>**This supersedes an earlier +37.93 ± 12.25**, taken on 6 August over 800 games at 30+0.3 with
128 MB — and it is *not* a regression from it. The two are compatible at 1.5σ; the earlier figure
carries twice the interval; and it predates the `NegExtAlpha` and `Rule50Formula` bakes, so it is
not even the same binary. The pattern is the ordinary one: a positive estimate taken on a small
sample runs high, because it gets looked at precisely when it is high. The most reliable figure
available before this gate was the +22.17 ± 9.70 over 1,287 games at the same time control, and the
new measurement sits **above** it, not below.</sub>

<sub>Hash is **256 MB**, not the 64–128 MB of the earlier runs, and that is deliberate: it is what
the rating lists use, and a number measured where the transposition table saturates need not survive
where it does not — see `TTTwoLevel`, whose +4.55 at 64 MB did not reproduce at 256. So this figure
is not directly comparable with the older ones, in the direction that matters: it measures the
engine where the engine will be measured.</sub>

The network alone was worth +23.41. The remainder is the search work, which cannot be read off the
table above: the stages are each measured against a different baseline and at three different time
controls, so they do not add up to anything.

⚠️ The 25+0.25 run was stopped by hand at 2,996 of a planned 6,000 because the figure came in
*below* expectation — the opposite of the direction in which an early stop biases an estimate. It
later accumulated to 3,170. The 40+0.4 run ran its full 3,000.

**Against another engine: Obsidian 16.0.** The only measurements of 7.0 outside its own family.
One thread, 128 MB, three conditions:

| date | TC | book | games | draws | Elo |
|---|---|---|---:|---:|---:|
| 12 Aug | 20+1 | Pollock | 700 | **90.3%** | +3.00 ± 8.00 |
| 15 Aug | 8+0.1 | UHO_4060_v4 | 240 | 52.5% | −10.14 ± 22.50 |
| 15 Aug | 20+0.2 | UHO_4060_v4 | 1,278 | 47.7% | −6.25 ± 10.20 |
| **combined** | | | **2,218** | | **−1.22 ± 6.06** |

The three are mutually consistent (Cochran's *Q* = 2.61 on 2 degrees of freedom), so combining them
is legitimate as a meta-estimate across those conditions — though not as a single measurement, since
the time control and the book both change between rows. Every individual row spans zero, and so does
the combination: **[−7.28, +4.84]**.

A dead heat, and now on 2,218 games rather than 700. What the result establishes is the order of
magnitude — 7.0 is *at* Obsidian 16.0's level, not a class below or above it. What it does **not**
establish is a sign: after three tries the best estimate is −1.2 Elo with a six-point interval, and
distinguishing that from zero would take far more games than the question is worth.

The number worth staring at is the draw count: **90.3%**. That is not a property of the engines
alone, it is what balanced opening positions plus a fast time control produce, and it is why the
interval is as tight as ±8 on only 700 games — the decisive games are few, but the paired
structure makes the ones that decide count for a lot. It also means this pairing needs far more
games than usual to separate anything: at a 90% draw rate, resolving 5 Elo would take tens of
thousands of games. It is a calibration point, not a gate.

> ⚠️ **The engine signature changes at every stage: `bench` goes 207,259 → 225,898 (stage 2) →
> 251,855 (stage 3) → 261,287 (stage 4) → 249,466 (stage 5) → 205,355 (stage 6).** The current
> signature is **205,355**; any script or procedure still checking an earlier value is verifying
> the wrong constant, and each of those is valid only for binaries built before the corresponding
> bake.

<sub>Stage 6 shrinks the tree by 17.7%, which is most of where its Elo comes from. Once the eval
read back from the table stops contracting toward zero on every revisit, the margins that compare
against it — reverse futility, null-move, razoring — are applied to the real value instead of a
degraded one, and they fire when they should. The gain is not only better moves; it is also a
smaller tree in the same time.</sub>

<sub>Stage 5 is a correction to stage 3, not a reversal of it. Stage 3 turned the material table on
*and* retuned continuation weight 100 → 85 and cap 50 → 48 in the same comparison, so its +2.65 over
30,530 games belongs to the block; the table itself was never isolated. Measured on its own it costs
11.5% of the tree and does not pay for it. The retuned continuation weight and cap stay: they were
co-tuned with the table on and have not been re-checked against 100 / 50, which is open work.</sub>

<sub>Stage 2 is worth recording because of what it cost. Quiet promotions were generated only
inside the quiet buffer, and qsearch never asks for it — so the engine declared those moves
tactical in two separate places and then never searched them. The first implementation *lost*
about 12 Elo: it paid for the nodes it added and bought nothing, because with a capture cap of
three the new promotions evicted every capture. Six variants, separated by their bench node
counts, were needed to find the one that pays; between the first and the last there are roughly
20 Elo at the same underlying idea. Node counts do not predict Elo, but they decompose a cost,
and that is what made the difference visible. The same code reads −11.77 at 12+0.12 and +8.38 at
25+0.25: a change that inflates the tree has to be judged at the time control it will be played
at.</sub>

<sub>Stage 3 is the clearest lesson of the project so far, and it is about **time control, not
about corrections**. The material correction table had been switched off in July on the strength of
450 games, and re-measured crudely it looked terrible: −17.68 over 708 games. Tuned by SPSA and
gated again it was flat. What changed the answer was tuning it **at the time control it would be
played at**: an SPSA run at 15+0.15 drove the continuation weight *up* to 162 and produced a
package that lost 11.38 Elo over 2,016 games; the same SPSA at 30+0.3 drove the same weight *down*
to 85 and turned the material weight toward zero — the opposite direction — and the resulting
vector passed its gate. Switching the table on costs 11.5% more nodes, roughly 8.8 Elo of debt at
55 Elo per doubling, and that debt is repayable at long time control and not at short. ⇒ **Every
lever rejected at short TC has to be looked at again.** The SPSA itself was stopped at 152
iterations and is not converged; the vector will be refined at long TC.</sub>

<sub>Stage 4 is not a tuning result and should not be read as one. `QSDeltaMargin` was sitting at
3000, which is the **maximum of its own range** — delta pruning in quiescence was effectively
switched off in the shipped binary, and it had been pushed there by two earlier SPSA runs. So the
gain is the repair of a mechanism that was off by mistake, not a better setting found on a smooth
landscape; the order of magnitude says the same thing, since the corrections block needed 30,530
games to show 2.65 Elo and this showed twice that on a fifth of the sample. The generalisation is
worth more than the patch: **a default sitting at a bound of its own range is the signature of a
disabled mechanism**, and it can be found mechanically rather than by experiment. Two caveats are
recorded honestly — the SPRT was stopped at LLR 1.89 without reaching either bound, with the
estimate declining as the sample grew (+13.05 at 1,918 games, +5.57 at 5,994), and the vector moves
two parameters at once, so which of the two pays is not known.</sub>

<sub>Tested and rejected: `PromoQS=7`, which adds the **knight** under-promotion in qsearch on top
of the queen — −1.82 ± 6.49 over 3,818 games at 25+0.25. It grows the tree by a further 11% and
did not repay it.</sub>

<sub>**Tested and rejected: the negative branch of late-move reductions, with its six fine
coefficients retuned around it** — **+1.70 ± 3.35 over 10,194 games**, 95% CI [−1.64, +5.05].
Stockfish and Obsidian let the sum of the fine reduction terms fall below zero, where a reduction
becomes an extension; this engine clamps it at zero. Enabling the negative branch on its own
measures **−9.11 ± 10.92**, and that is the interesting part: underneath a clamp the six
coefficients are mutually indistinguishable, so whatever values tuning had left down there were
arbitrary. Removing the floor does not import a technique — it **uncovers untuned territory**. The
six were therefore retuned above it by SPSA (3,055 iterations, 8 games each). The retune recovered
the 9 Elo in full and found nothing beneath: the floor at zero was already the optimum. Two
by-products are worth more than the null itself. At 550 games this same test read **+20.24 ± 14.72,
LOS 99.66%** — 2.7σ — against +1.70 at 10,194, a reminder that an SPRT read at a twentieth of its
distance is an anecdote *even when it carries three sigma*. And the mechanistic prediction that
something which extends ought to pay more at depth was testable and proved false: 40+0.4 measured
−0.71σ against 20+0.2, with the depths taken from the PGNs rather than assumed — 16.2 at 20+0.2,
18.0 at 40+0.4, and 13.1 at the same time control on an AVX2 machine 2.6× slower.</sub>

<sub>**Tested and rejected: per-bucket evaluation scaling** — **−9.82 ± 11.44 over 920 games** at
10+0.1, LOS 4.61%. The network has eight output buckets selected by piece count, but the single
recalibration that maps its output onto the search margins is **global**. The motive was that the
net minimises *prediction loss* uniformly across phases, not Elo, so one scale is forced to serve
both a 32-piece opening and a 4-piece endgame. Eight per-bucket scales were exposed and tuned by
SPSA (1,124 iterations, 12 games each, perturbation annealing ±5 → ±2.4); the tail was settled — the
drift between its two halves was 0.29 RMS against displacements of 1.79 — and the vector came out
`60 · 58 · 63 · 59 · 60 · 58 · 59 · 63`. Zero is still inside the interval, so this is not proof of
harm; but **+5 is excluded at 2.3σ**, and for a bake decision "not better" and "worse" lead to the
same place. Defaults stay at 60 and the shipped binary is byte-identical (bench 252074).</sub>

<sub>The reason it could not work is worth more than the result. **A multiplicative scale can only
stretch the evaluation uniformly.** If the net misjudges *which* positions are good, multiplying
everything by 1.05 corrects nothing. What the Stockfish community calls "NNUE SPSA" tunes the
**weights**, which changes *what the evaluation says about a position* — a different kind of
intervention, not a larger version of the same one. One useful by-product: `B0`, the 1–4 piece
bucket, did not move by 0.001 in 1,100 iterations. That is the bucket where the eval scale cannot
change the result, and its staying put while `B7` moved three points is the internal control saying
the tuner was following a real gradient rather than diffusing. The gradient was simply worth little.</sub>

<sub>**Stage 10 was bundled deliberately, and the arithmetic of that choice is worth stating.** Nine
switches were written, each isolating one structural difference from Stockfish 19, each defaulting
to the historical behaviour so the bench signature stayed byte-identical at 279,691. Seven were
screened at 10+0.1; four came out with a positive lean and were combined. The sum of their point
estimates was **+10.6**, and that is *not* what a bundle of them was expected to be worth: selecting
the positive subset of a noisy group biases the estimate upward by construction, so the prediction
written down **before** the run was +2 to +6. It measured **+5.31 ± 3.44**. The reason to bundle at
all is volume — under `[0, 2]` a patch worth +2 needs on the order of 66,000 games to close, so four
of them separately is about 44 hours of rig time, while four that jointly carry ~5 close in two.
The price is attribution: the bundle does not say which of the four carries it, and one of them
(`ProbCutTTAll`) returns *before* the null-move step the second one modifies, so they are not
strictly independent.</sub>

<sub>The screening also produced one clean negative, which is more useful than the borderline
positives. Our transposition table stores, on a fail-low node, the best of the moves that failed —
something Stockfish does not do. Switching that off measured **−3.90 ± 6.28** with a 47% smaller
tree: the entry is doing real work, the current behaviour is right, and that question is now closed
rather than open. Two more were flat and were dropped: gating null move to cut-nodes as upstream
does (−1.64), and refusing reverse futility when the TT move is quiet (−0.65).</sub>

<sub>**Stage 11 came from a measurement rather than from upstream, and its two parameters are one
hypothesis.** Taken separately they are inert, and this is measured, not argued: with the depth cap
at 2, lowering the margin moves the bench by 0.2%; with the margin at 2097, raising the cap changes
**nothing at all** — the node count is identical to the byte. The reason is arithmetic. At a reduced
depth of 3 to 6 the threshold `−2097 × depth` demands a history more negative than the two tables
summed can physically reach, since each is bounded by ±7000. So the cap admits nodes at which the
condition can never fire. Moved together — cap 6, margin 1200 — the tree changes by 3.9% and the
pair measures **+4.48 ± 2.65** over 19,228 games. The methodological residue is the part worth
keeping: **a switch that does not move the bench is not "neutral", it is not connected**, and the
first formulation of this test was already queued for a full night that would have measured zero by
construction. It was caught by re-running the bench-bite check after the previous bake — which is
why that check now runs after *every* bake, not once.</sub>

<sub>**Stage 12 is the only test of this campaign that crossed its bound** rather than being stopped
by hand — LLR 2.95 against a threshold of 2.94 — and it is also the largest. It removes 44.7% of the
bench tree, the most violent cut of the session, and it is the third result in a row pointing the
same way: this engine was pruning too little, in exactly the region where the measurement in §4 says
its tree is too wide.</sub>

<sub>Two candidates then **died as a consequence of the bakes**, and taking them out was worth more
than measuring them. `NMPEvalScale` moved the bench by 1.9% before stage 12 and by 0.04% after —
63 nodes out of 149,224; the widened futility depth went from −1.2% to −0.01%. The accepted changes
had already pruned the ground those two would have covered. A patch that does not change the tree
cannot carry measurable Elo, so both left the queue instead of consuming two hours each. This is the
second time in two days that re-running the bench-bite check after a bake changed what was worth
testing — the first time it rescued stage 11 from a formulation that would have measured zero by
construction.</sub>

<sub>Three ports were **rejected on 15,000 games each**, and at a ±3 band those are verdicts rather
than triage: the SEE gate moved onto the reduced depth (**−2.66 ± 3.00**), the fail-low node
inheriting its parent's ttPv flag (**−2.11 ± 2.98**), and accepting a TT cutoff incoherent with the
node type above depth 4 (**−0.16 ± 2.97**, flat). All three are upstream behaviour that does not
transfer here.</sub>

### Stages 10–12, checked together

The three stages above were each decided at 10+0.1 with a 64 MB hash, and this engine has twice
seen a sign change between that and the shipping regime — `TTTwoLevel` was worth +4.55 at the
small hash and exactly zero at the large one. So the seven defaults they consist of were reverted
in a single run and measured **at 25+0.25 with a 256 MB hash**, four times the hash and two and a
half times the clock, at a mean depth of **16.8 plies** against the 13.3–13.9 the stages were
decided at.

> **−20.24 ± 8.62** over 1,598 games, LOS 0.00%, pentanomial `[5, 238, 403, 151, 2]`
> — the engine *without* the three stages, against the engine with them.

The sum of the three point estimates at 10+0.1 was +18.4. Deeper and with four times the memory
they measure **−20.2 in ablation**, so the gains do not merely survive the regime change, they are
marginally larger there. The run was stopped at 1,598 games because at this magnitude there is
nothing left to resolve: the band is ±8.6 and zero sits more than two bands away.

<sub>Two things this does and does not establish. It fixes the **magnitude** of the set, not the
value of any one stage — the ablation moves all seven parameters together, so it says nothing about
which of them carries the total, and stage 12 alone accounted for +8.64 of it at short time control.
And it is not the 60+0.6 gate: 25+0.25 is closer to shipping than 10+0.1 but not equal to it, and
the full ablation at 60+0.6 remains queued.</sub>

<sub>One check worth recording because it costs nothing and proves something a diff cannot. With all
seven parameters returned to their original values the bench signature is **279,691 exactly** — the
canary from before any of this work began. The entire search change of this campaign is therefore
those seven numbers and nothing else; the raised ply ceiling, hash limit and thread cap that landed
in the same commits are confirmed, not merely assumed, to leave the search untouched.</sub>

---

## 3. Speed work (NPS)

Every change below is **node-identical**: the search tree is bit-for-bit the same, so none of them
can alter playing strength at a fixed node count — only the rate at which nodes are produced. Each
was gated on an unchanged `bench` signature before being measured at all.

The first set of figures published here was **withdrawn**. The harness that produced them ran the
two binaries in a fixed A-then-B order on each position, so whichever binary occupied the second
slot absorbed the machine's thermal drift; run against itself — same binary, same options, both
sides — it reported a nine-sigma difference out of nothing. It now alternates the order within each
pair, drops exact ties from the sign test, refuses to run when a requested option is not announced
by the engine, and reports the A-first and B-first sub-samples separately as a built-in drift check.

**End to end, against the 31 July build**, both at their own defaults, PGO, 300 positions
interleaved at depth 20 on an idle Zen4 laptop:

> **AVX2 +3.3% NPS · AVX-512 +1.9%**

AVX2 is the figure that matters for the rating lists, which compile it. AVX-512 gains less
because three of the five changes below are inert or harmful on that instruction set.

Everything below was then re-measured on the fixed harness, each change **on its own** against a
baseline built from the same source with all of them off. Six PGO builds, all at `bench` 249,466.

| change | in isolation |
|---|---|
| hybrid update on king moves | **+3.4%** |
| pawn-block refresh cache | **+2.8%** |
| mailbox `piece_on[64]` | **+2.5%** |
| both perspectives in one dirty-list pass (AVX2) | **+1.5%** |
| weight-row permutation for locality | **≈ 0%** |
| all five together | **+1.8%** |

Two things in that table matter more than the individual numbers.

**The five do not add up.** Together they are worth +1.8%, against roughly +10% if they composed.
They all attack the same bottleneck — memory traffic in the accumulator — so each one finds in
cache what the previous one already brought there. The remaining +1.5% of the end-to-end figure
comes from the HalfKA row prefetch, which has no compile-time switch and could not be isolated.

**The permutation is worth nothing.** Three commits claimed +1.5%, +2.4% and +3.1% for reordering
weight rows by access frequency and then by co-occurrence — nearly half the campaign. Measured
properly it is inside the noise. The machinery it needs (a generated permutation, its inverse on
save, an 8192² co-occurrence profiler) remains in the tree and buys nothing; a planned second
version, estimated at a further +0.3-1% *on top of* this one, was dropped.

**Feature generation**

- Dead threat tuples are no longer produced. When pawn→pawn relations left the feature set the
  refresh path was updated and the incremental path was not, so a large share of every tuple the
  engine generated was built, indexed, prefetched — and then dropped by the bounds filter.
- `PawnPair` refresh enumerates through a precomputed file band instead of an O(n²) double loop.

**Accumulator**

- **Pawn-block refresh cache.** The finny table covers only `HalfKAv2_hm`; the other blocks were
  rebuilt from scratch on every refresh. `PawnPair` and `PassedPawns` depend on exactly
  *(white pawns, black pawns, orientation)*, and refreshes are triggered by king moves, which
  leave pawns untouched — so between consecutive refreshes the key is almost always unchanged.
  Direct-mapped and keyed on the full pawn bitboards, so a collision is impossible by
  construction. AVX2 only.
- **Hybrid update on king moves**, ported from Stockfish `db98633b`. `requires_refresh` is true for
  every move of one's own king, but threats, pawn pairs and passed pawns depend on the king only
  through an orientation that takes two values and changes solely when the king crosses the d/e
  file. Every other king move was discarding still-valid work; now the previous accumulator is
  reused, with both HalfKA sides reconstructed from the finny table.
- **Both perspectives in one dirty-list pass**, ported from Stockfish `7b550409`. The list was
  walked twice, once per perspective, with a full accumulator update in between — so every entry
  was decoded twice and was long evicted by the time the second pass read it. Ours departs from
  the source by keeping the wide writes sequential, all of white then all of black, rather than
  alternating them at every transition.

**Memory layout**

- **Weight rows permuted for locality.** A small fraction of the threat table carries the large
  majority of the accesses, scattered across the whole of it — more pages than any TLB maps, so in
  practice every access was a DRAM miss on a working set that would fit in cache if it were packed.
  The rows are reordered at load time and the indices remapped through a lookup table small enough
  to stay in L2. The evaluation is unchanged; only rows move. Ordering is by co-occurrence
  clustering over the hot rows rather than by raw frequency.

**Search-side**

- **Capture-victim lookup through the mailbox.** Finding which piece stands on the target square
  scanned six bitboards, on a path taken for every capture in move ordering and again throughout
  quiescence. The `piece_on[64]` array is already maintained by make/unmake, so the answer is a
  single load; the fallback for an empty square or a friendly piece reproduces the old return value
  exactly. Node-identical by construction, with the unchanged `bench` signature as the gate.
  **+0.7 to +1.0%**, measured on one binary with the path chosen by a UCI option at runtime — the
  only way to resolve an effect this small, since between two separately built PGO binaries the
  noise floor is about 0.5% and its sign flips between sessions.
- The quiet stage no longer regenerates the captures already produced by the tactical stage.
- The in-check state is passed in by the caller instead of being recomputed on every generation.
- Least-significant-bit clearing uses `bb &= bb - 1` where the bit being cleared is provably the
  LSB, removing a dependency on the preceding bit scan.

**Build targets**

- `avx512` no longer requires VBMI2. The target had been set up on machines that all happened to
  have it — but that is the binary shipped as "avx512", and on Skylake-X or Cascade Lake it dies on
  an illegal instruction at startup. The targets are now separate, as in Stockfish: `avx512`
  (F/BW/DQ/VL/VNNI) and `avx512icl` (+ VBMI/VBMI2/BITALG). The ICL path measured as exactly nothing,
  so `avx512` ships without it: broader compatibility at no cost.
- New **`avx2-nopext`** target, Stockfish's `x86-64-avx2`. PEXT is three cycles on Intel and Zen 3+
  and microcoded on Zen 1/Zen 2, where a tester running the PEXT build loses a double-digit
  percentage of NPS. Verified on an EPYC Rome: zero `pext`/`pdep` in the binary and the same bench
  as the PEXT build, so the fancy-magic fallback is equivalent.

<sub>Two changes are **under re-measurement** and may yet be removed: the prefetch of the HalfKA
weight rows, and the `piece_on[64]` mailbox that replaces bitboard scans when finding which piece
stands on a square. Both read negative on the repaired harness on two different CPUs, but removing
them together does not recover what removing either alone appears to gain — an inconsistency that
has to be resolved before either is taken out.</sub>

<sub>Several candidates were measured and rejected rather than shipped, and the rejections were as
useful as the acceptances: prefetching all four SIMD tiles of a row instead of only the first line,
prefetching the PSQT rows, permuting the HalfKA rows the way the threat rows are permuted, widening
the clustering granularity, and caching the minor/major correction keys. Two rules came out of
them. A prefetch pays only when the table cannot fit in cache, and only one line per row — the rest
of the row is sequential and the hardware streamer already has it. And a frequency permutation
helps only where the layout has no structure to begin with: `HalfKA` maps 64 consecutive squares to
64 consecutive rows, so permuting it replaces sequential access with scattered access and loses.</sub>

---

## 4. Search structure: measured, not assumed

A single number had been sitting in this project's notes since June and steering work: our
transposition table was said to supply a move at **28%** of nodes against Stockfish's **46.5%**,
and that gap had motivated several changes in a row. It is not reproducible.

The check was to compile the *same* counter into both engines — twenty lines, incrementing per
depth bucket at the point where each search reads its TT entry — and run them over identical
positions: 32 of them, 3 million nodes each, one thread, 256 MB.

| depth | our nodes | our TT move | Stockfish nodes | its TT move |
|---|---:|---:|---:|---:|
| 1–3 | **73.5%** | 17.0% | **58.4%** | 15.5% |
| 4–6 | 17.8% | 31.4% | 20.9% | 24.2% |
| 7–9 | 6.4% | 44.2% | 11.1% | 29.1% |
| 10–13 | 2.0% | 63.0% | 6.8% | 38.1% |
| 14+ | 0.3% | 82.6% | 2.8% | 54.6% |
| **all** | | **22.4%** | | **21.4%** |

**Move availability is the same — 22.4% against 21.4%, marginally in our favour.** The deficit that
several months of work had been aimed at does not exist.

What the same measurement *does* show is a different defect, and it is in the shape of the tree
rather than in the table: **73.5% of our nodes live at depth 1–3 against Stockfish's 58.4%, and
0.3% of them get past depth 14 against 2.8%.** The tree is wide at the base and short at the top.
That is the same thing visible at fixed time — 21 plies against 27 in four seconds — seen from
the inside. Stage 11 above is the first change taken directly from this reading, and the direction
it points is to prune harder at low depth, not to chase entries into the table.

<sub>The entry hit rate does differ sharply — 53.9% for Stockfish against our 23.4% — but that is a
consequence of the shape rather than a cause: an engine that explores fewer, deeper nodes revisits
the same positions more often. It is the wrong number to optimise directly.</sub>

### Limits raised, and one guard that had to come with them

Four constants, none of which changes the search: the bench signature is unaffected by all of them.

- **`max_ply` 64 → 128.** Past that ply the search silently stops searching — both the main routine
  and quiescence return the static evaluation instead. At one thread and short time control the
  selective depth stays under 30, but at 24 threads and fifteen seconds it was measured at 49, and
  the regime this engine is aimed at is deeper still. Stockfish uses 246. Every derived constant was
  already written in terms of `max_ply` and rescales correctly on its own — the tablebase value band
  is `mate_score − max_ply`, and the guard that keeps tablebase verdicts out of the correction
  history is `mate_score − 2·max_ply`, which is exactly the lowest score the band can produce. The
  only quadratic cost is the principal-variation table, 20 KB → 72 KB against a 21.25 MB per-thread
  structure.
- **`Hash` ceiling 1024 → 65536 MB**, and **`MAX_THREADS` 64 → 512**. The thread cap was below the
  80 hardware threads of the machine the engine is tested on, so it could not use its own rig; the
  per-thread structure is allocated on demand, so the constant costs nothing at rest.
- **A guard on transposition-table allocation failure**, and it is the reason the hash ceiling is
  not merely a number. Under the old 1024 MB cap a failed allocation was unreachable; at 65536 it is
  an ordinary outcome, and the code went straight from a null pointer into `memset`. It now falls
  back from large pages to the heap and then to 64 MB, and the reported line says what was actually
  allocated rather than what was asked for — previously, after a fallback, it printed
  "4096 MB, large pages ON" while holding a 64 MB heap table, so the only diagnostic a user sees was
  wrong precisely in the case where it matters.

<sub>One build-system defect found in passing, and worth fixing before it costs someone a
measurement: the Makefile carries **no header dependencies**. Editing `tt.h`, `threads.h` or
`search.h` recompiles nothing, so a change to any of them followed by a measurement silently
measures the previous binary. It happened once during this session and was caught only because the
diagnostic string it should have changed did not change.</sub>

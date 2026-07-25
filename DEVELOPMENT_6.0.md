<div align="center">

<img src="logo.png" alt="Triumviratus" width="200">

# Triumviratus

**A strong UCI chess engine in C++** — NNUE evaluation · SPSA-tuned alpha-beta search · Syzygy tablebases

[![License: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](COPYING)
[![C++](https://img.shields.io/badge/language-C%2B%2B-00599C.svg)](source/)
![UCI](https://img.shields.io/badge/protocol-UCI-brightgreen.svg)
![NNUE](https://img.shields.io/badge/evaluation-NNUE-orange.svg)

**by Francesco Torsello**

<sub>in collaboration with Maurizio Platino</sub>

</div>

---

<div align="center">

[6.0 (prerelease)](#triumviratus-60-prerelease) · [Results](#results) · [History](HISTORY.md) · [License](#license) · [Credits](#credits)

</div>

---

## Triumviratus 6.0 (prerelease)

> [!NOTE]
> **Prerelease available.** Version-bump gate against 5.1 **passed**: **+33.18 ± 9.86 Elo** at 20+0.2
> (LOS 100%, SPRT `[0,5]` passed, 1166 games) — see [Results](#results).

Three main architectural changes over 5.1 — plus a longer tail of smaller incremental
refinements, tracked in the table below:

- **New network architecture — `TRANN1` (Triumviratus Rubicon Alea NNUE 1).** Extends the SFNNv13
  feature set with two extra input blocks: **pawn-pair features** (4560 — phalanxes, chains,
  doubled/isolated pawns: pawn-structure geometry the threat features alone don't capture; a
  pairwise pawn-square co-occurrence feature **shared with Stormphrax, Viridithas and Pawnocchio** —
  not our idea, credited in [Credits](#credits)) and **passed-pawn features** (96 — one per passed
  pawn: a relational property `HalfKAv2_hm` cannot express directly; **an original feature of this
  project**). Both are grafted with zero-initialised columns onto the previous net —
  bit-identical at graft time — then fine-tuned; both fold into the existing threat accumulator, so
  no new SIMD path is added. Shipped in **`nn-rubicon-alea-v3`**, whose net format is consequently
  **not** SFNNv13 (the reader still loads v2-format nets, zero-filling the passed-pawn segment, so a
  single binary can gate v2 against v3). See [`NETWORKS.md`](NETWORKS.md) for training details.
- **SPSA mega co-tune** of the search parameters, co-tuned as a block and baked into the compiled
  defaults.
- **TMv2 time management** — a multiplicative-stateless time manager (stability, eval-trend, node and
  predicted-move factors), SPSA-tuned. Measured **+23.8 Elo at 20+0.2**. It shipped **time-control
  gated** — a 2026-07-13 measurement found −22.9 Elo at 10+0.1, so it fell back to the original time
  manager below a 15 s base-time threshold (`TMv2MinBaseMs`). A 2026-07-24 re-measurement (see
  [Corrections](#corrections)) found that cliff no longer reproduces at any tested TC, and the gate
  was removed: TMv2 now runs unconditionally whenever the game has an increment.

#### Network gains (net-isolated)

Same binary on both sides — **only the network changes** — so these isolate the network from the
search. Measured against `v1` with both new blocks zero-grafted (a v3-format net whose evaluation is
bit-identical to v1), so one binary plays both sides.

| Comparison | Date | TC | Games | Elo | LOS |
|---|---|---|---|---|---|
| `v2` vs `v1` — PawnPair block + own self-play data | 2026-07-17 | 20+0.2 | 1104 | **+18.27 ± 9.94** | 99.98% |
| `v3` vs `v2` — PassedPawns block | 2026-07-17 | 15+0.15 | 2596 | **+6.96 ± 6.56** | 98.1% |
| **`v3` vs `v1` — cumulative** | 2026-07-17 | 20+0.2 | 1998 | **+15.14 ± 7.69** | 99.99% |

<sub>Net gains shrink at longer time controls (a deeper search compensates for part of what the
evaluation already knows), which is why the 15+0.15 row is not additive with the 20+0.2 rows.</sub>

#### Incremental gains (vs 5.1 baseline)

Each row is measured against the state *before* that change (1 thread, 64 MB, UHO 2024 book
`UHO_2024_8mvs_big_+080_+099.epd`, no score-based adjudication).

| # | Change | Date | TC | Games | Elo | LOS |
|---|---|---|---|---|---|---|
| 1 | SPSA mega co-tune (50 params) | 2026-07-13 | 10+0.1 | 1058 | **+15.8 ± 10.9** | 99.8% |
| 2 | TMv2 time management (gated ≥ 15 s) | 2026-07-13 | 20+0.2 | 380 | **+23.8 ± 18.2** | 99.5% |
| 3 | v2 network + search re-tune + large-pages NPS ¹ | 2026-07-16 | 20+0.2 | 394 | **+38.1 ± 17.1** | 100% |
| 4 | Unconditional check extension removed ² | 2026-07-19 | 30+0.3 | 1654 | **+8.0 ± 8.1** | 97.3% |
| 5 | Fine-grained SMP tree diversification ³ | 2026-07-20 | 16+0.16 | 1700 | **+4.91 ± 7.97** | 88.6% |
| 6 | EPKeyFix (En Passant hash correction) | 2026-07-21 | 16+0.16 | - | **~ +4.5** | lean |
| 7 | Continuation History (CorrHistCont + MathFix) ⁴ | 2026-07-22 | 16+0.16 | 2862 | **+13.97 ± 6.43** | 100% |
| 8 | KillerReset (anti-stale killer moves) ⁵ | 2026-07-22 | 16+0.16 | 3290 | **+12.15 ± 5.97** | 100% |
| 9 | Correction-history + move-ordering bundle ⁶ | 2026-07-24 | 10+0.1 | 4130 | **+10.43 ± 5.57** | 99.99% |
| 10 | King-shield-pawn ordering malus ⁷ | 2026-07-24 | 12+0.12 | 10k+ | **+3.53 ± 3.42** | 97.85% |
| 11 | Seven latent-defect fixes from the final audit ⁸ | 2026-07-25 | 8+0.08 | 3190 | **+2.18 ± 6.31** | 75.1% |
| 12 | QSMoveCap 1 → 3 (quiescence searched one move per node) ⁹ | 2026-07-25 | 15+0.15 | 2868 | **+5.21 ± 6.47** | 94.3% |
| 13 | Quiet checks removed from quiescence (`QSChecks=false`) ¹⁰ | 2026-07-25 | 20+0.2 | 3294 | **+9.71 ± 5.99** | 99.93% |

<sub>¹ Row 3 is a cumulative snapshot: mid-training `nn-rubicon-alea-v2` (checkpoint ep439 of ~800,
so it understates the final net) plus an SPSA re-tune and large-pages/NPS work, measured together
against rows 1–2. Superseded by the final v2/v3 networks above and by the version-bump gate below.</sub>

<sub>² The search used to extend by one ply on every check, at any depth — a technique Stockfish
dropped years ago (only singular extensions remain). Removing it cuts the benchmark tree 55.8%
(592074 → 261932), though most of that comes from one pathological endgame position; on the opening
book used for gating, the tree shrinks a more modest 11.5% at depth 18. Partial settings (e.g. a
depth cap of 4) are worse than either extreme, so the choice is binary — the old behaviour is still
reachable via `CheckExtDepth=30`.</sub>

<sub>⁵ Clears the Killer Moves for the child ply (`ply + 1`) at every node. Prevents stale killer
moves inherited from sibling subtrees from polluting the search.</sub>

<sub>³ Each helper thread biases its own LMR reduction to diversify the tree it searches. The old
bias only moved in whole plies — a step 2–4× coarser than what other engines converge to; this
reworks it into fine-grained units. Measured at 8 threads (the effect is invisible below 2); the
lean stayed positive throughout even though the LLR never closed — an 8-thread SPRT can't
realistically resolve on a single machine, so this is baked on trend rather than a closed bound.
Thread count 1 is untouched: bench stays exactly **261932**.</sub>

<sub>⁴ The continuation history table tracks the path-dependent static evaluation error based on the last two moves.
To make it work, the underlying math was completely replaced (from a buggy EMA that caused chronic under-correction
to a Stockfish-style pure gravity update), the table's weight was lowered to 100, and a 2x learning rate
multiplier was injected to combat its extreme sparsity.</sub>

<sub>⁶ Three features — a material-key correction-history table (endgame/fortress eval bias), a
softer good/bad capture-ordering threshold, and a quiescence-search stalemate probe (qsearch only
generates captures, so it could never detect stalemate; this catches the specific case where a
capture removes the board's last rook or queen) — each tested alone landed **below the
measurement floor** (flat-to-small leans, none individually conclusive). Bundled into one SPRT they
passed cleanly: LLR reached the upper bound at 4130 games. A same-settings confirmation at 20+0.2
is running; the first 582 games are positive and consistent (+3.58 ± 15.44, not yet conclusive).</sub>

<sub>⁷ Part of the same Reckless move-ordering port as row 9's stalemate probe, but split out: a
malus for moving a pawn that shields your own king, tested independently from the port's other new
term (a bonus for quiets landing on a square that attacks a vulnerable enemy piece). That other
term measured **cleanly negative in isolation** (`−9.70 ± 10.41`, LOS 3.38% @1290 games) and ships
disabled (`OffenseBonus=0`). The shield-pawn malus needed its magnitude found empirically — the
value converted by ratio from Reckless's own scale (5600) gave a barely-there lean (`+3.24 nElo`,
LOS 85%, never resolving even past ~12,700 games); tripling it (16800) produced the qualitatively
stronger, still-running result in the table — the point estimate has moved around checkpoint to
checkpoint (as expected with a match still in progress) but has stayed positive with LOS in the
high-90s throughout, not decaying toward zero the way the B1 SPSA bake did. Baked on that trend
rather than a closed SPRT bound; the exact magnitude is a candidate for the final pre-release mega
co-tune.</sub>

<sub>⁸ Not a feature: seven latent defects found by a systematic audit of the whole engine run before
closing 6.0 (search compared mechanism by mechanism against Stockfish master, Reckless, Stormphrax
and Integral; SPSA tooling; NPS; network permutation). The one with real consequences is that the
ProbCut child node initialised only one of five per-ply stacks, so `seen`, `de`, `hbucket` and
`captured` kept whatever the *previous sibling* left at that depth — and the child reads them: a
stale `captured` of −1 made the static-eval-difference ordering write quiet-history bonuses for a
capture, the history bucket credited the wrong victim in capture history, and a dirty double-extension
counter let one subtree enable double and triple extensions inherited from another. The other six are
a stale advertised UCI default (8 while the live value is 14 — a tuner echoing the advertised defaults
back would switch off 15.4% of the search tree), missing FEN validation (out-of-range writes on
illegal positions, plus a phantom en-passant square that gave one position two different Zobrist
keys), `go movetime` being scaled by the time-management factors, a hard timeout that could fire
before any root move existed (so the anti-forfeit fallback emitted the first legal move), a
correction-history table never cleared between games, and a UCI maximum that overflowed the history
gravity ceiling. Only the ProbCut fix changes the tree (bench 275063 → 283729, +3.15%); disabling
that one alone returns the benchmark to exactly 275063, which is how the other six were confirmed
byte-identical. The gate above was run as a **non-regression** check (bound `[−5, 0]`), so the
positive point estimate is not a significant gain — it is evidence the fixes cost nothing, which was
the requirement. Full audit report kept out of tree.</sub>

<sub>⁹ The same audit's largest structural finding, and it was a default, not a bug: `QSMoveCap` was 1
with the break at the top of the move loop, so outside of check the quiescence search examined
**exactly one move per node** — a chain rather than a tree. Nothing was wrong with the code; the
value had simply never been revisited after the surrounding qsearch pruning was tuned, and it
quietly capped what several of those mechanisms could ever do. Raised to 3, Obsidian's value.
Baked on LOS (94.3%) rather than a closed SPRT bound, on a 15+0.15 measurement rather than the
short time controls that have misled this project before. Bench 283729 → 306473 (+8.0%);
`QSMoveCap=1` reproduces 283729 exactly, so the old behaviour is one option away. One consequence
worth re-testing: the standing note that quiet checks in quiescence looked nearly inert was measured
under the one-move cap, where they competed with captures for the only slot available — that
constraint is now gone.</sub>

<sub>¹⁰ The consequence anticipated in note ⁹, and the clearest single result of the audit: with the
one-move cap gone, **removing quiet checks from the quiescence search gains 9.71 Elo** — an SPRT that
closed its bound (LLR 2.96) over 3294 games at 20+0.2, the longest time control used for any decision
in this release. Stockfish removed the same mechanism in July 2024 (PR #5498) as a non-regressive
simplification; here it is worth considerably more than that. The measurement matters as much as the
result: a standing audit note had concluded the *opposite* — that quiet checks were doing useful
pruning, because switching them off made the tree on the gating book grow 19.2%. That measurement was
taken while `QSMoveCap` was 1, when the quiescence search examined a single move per node and a quiet
check could evict the best capture from the only slot available. Lifting the cap reversed the sign. A
measurement is only valid in the regime it was taken in, and this release contains two cases of it
(see also the retracted bundle under Corrections). Bench 262238 → 205566; the old behaviour returns
with `QSChecks=true`.</sub>

<sub>**Free NPS, same release.** With `OffenseBonus` at 0 — the offense-square ordering term measured
negative in isolation and ships disabled, see note ⁷ — the ordering code still computed the full set
of offense masks on every node: roughly ten sliding-attack loops, plus a threat computation that only
they needed, all multiplied by zero. Skipping that work when the bonus is zero is byte-identical:
node counts match to the node over 48 million nodes, and the benchmark is unchanged at 205566.
Measured **+2.6% NPS**, paired, at identical node counts, and confirmed in both run orders (+3.0% with
the patched build first, +2.2% with it second, so the sign survives thermal drift on the measurement
machine). The king-shield-pawn term, which is the part that actually pays, is still computed.</sub>

#### Corrections

Bugs and stale design decisions found during and after prerelease prep. All are fixed in the
current source and in the published binaries; they are recorded because the first one changes what
*earlier* prerelease builds were, and the others are the kind of thing worth being honest about.

- **The Windows binaries embedded the wrong network.** The single source of truth for the default
  net name was updated in the code but not in the Windows resource file, so the embedded blob was
  still `rubicon-alea-v2`. Binaries with `nn-rubicon-alea-v3.nnue` next to them were unaffected
  (a file on disk always wins), but running the `.exe` on its own meant playing the previous
  network, ≈7 Elo weaker. Linux and Android were never affected. **Replace any earlier 6.0
  prerelease `.exe`.** The build now benches the binary standalone and refuses to package a
  mismatch.
- **Capture futility pruned good captures.** The victim's value entered the pruning margin in
  classic centipawns while the surrounding terms were in the network's own (compressed) evaluation
  units, under-weighting the victim ≈2.4×. The effect was the opposite of the intent: captures
  winning material got pruned. The twin site in quiescence search had been bridged correctly a month
  earlier; this one had not. The bridge now tracks `EvalScale` rather than being a hardcoded
  constant, so it cannot silently drift out of calibration again.
  <br><sub>Measured **indistinguishable from zero**: `−0.99 ± 8.88` over 1406 games at 20+0.2
  (LOS 41%). The surrounding margins had been SPSA-tuned around the broken scale and had absorbed
  most of the error, which is the likeliest reason correcting it changes so little. It ships as a
  correctness fix — a per-victim scale error is not defensible whatever the scoreboard says — and it
  lets those margins be re-tuned in a coherent space.</sub>
- **Three Syzygy UCI options were advertised but never wired to a handler.** `SyzygyProbeDepth`,
  `SyzygyProbeLimit`, and `Syzygy50MoveRule` showed up in `uci` output and could be set from any
  GUI, but nothing read the values back — a silent no-op for anyone who tuned them expecting an
  effect. Wired into the in-search WDL probe gate and the score conversion; defaults reproduce the
  exact prior behavior (bench unchanged).
- **The TMv2 time-control gate (`TMv2MinBaseMs`) no longer reflects reality and was removed.** It
  shipped because a 2026-07-13 measurement found TMv2 lost −22.9 Elo at 10+0.1 despite winning
  +23.8 at 20+0.2 — a genuine sign flip at the time. A 2026-07-24 sweep (8+0.08 / 12+0.12 / 15+0.15,
  then a dedicated full-concurrency confirmation at 8+0.08: `+31.10 ± 18.37` nElo, LOS 99.95%) found
  **no regression at any tested time control** — the short-TC cliff does not reproduce on current
  code, most likely superseded by later search changes (continuation history, the En Passant hash
  fix) that shifted the same time budget. TMv2 now runs unconditionally whenever the game has an
  increment; the 15000ms threshold and its supporting state were deleted rather than left at zero,
  since a dead UCI option is its own bug (see the Syzygy item above).</sub>

## Results

Results below are **6.0** vs **5.1** (the current stable release) and **6.0** vs other engines. 5.1's
own feature set, its own SPRT-confirmed gains, and its historical match results are archived in
[`HISTORY.md`](HISTORY.md) alongside 5.0 and 4.2; source is in [`source/`](source/) and build
instructions are in [`source/BUILD_NOTES.md`](source/BUILD_NOTES.md).

#### 6.0 vs 5.1 — version-bump gate (2026-07-16)

> [!NOTE]
> Release binary vs release binary (AVX-512), each loading **its own** network (6.0 → `rubicon-alea-v3`,
> 5.1 → `rubicon-alea-v1`), 1 thread, 64 MB, UHO 2024 book.

| Date | Time control | Games | Score (6.0) | Elo (6.0) | LOS | SPRT |
|---|---|---|---|---|---|---|
| 2026-07-16 | 20+0.2 | 1166 | 54.8% | **+33.18 ± 9.86** | 100.00% | `[0,5]` **passed** (LLR 2.95) |
| 2026-07-20 | 16+0.16 | 518 | 57.1% | **+49.29 ± 15.33** | 100.00% | — |

#### State 6+7+8 vs State 5

Cumulative test of EPKeyFix, Continuation History, and KillerReset against the previous state (Fine-grained SMP).

| Date | Time control | Games | Score | Elo | LOS | SPRT |
|---|---|---|---|---|---|---|
| 2026-07-23 | 16+0.16 | 782 | 54.1% | **+28.50 ± 12.07** | 100.00% | `[0,5]` unclosed (LLR 1.76) |

<sub>W 210 · L 146 · D 426. Pentanomial [0–2]: [0, 64, 203, 120, 4].</sub>

<sub>6.0: W 301 · L 190 · D 675. Pentanomial [0–2]: [3, 81, 306, 188, 5].</sub>

<sub>The 16+0.16 row is a separate, still-running confirmation match on the current development
build (post-`DiverseSMPFine`), stable around +48/+49 Elo since the start of the run.</sub>

<sub>**Measured before the capture-futility scale fix** (see [Corrections](#corrections)). That fix
measured **indistinguishable from zero** on its own — `−0.99 ± 8.88` over 1406 games at 20+0.2 — so
this figure carries over to the shipped binary unchanged. The fix is kept because the formula was
objectively wrong, not because it gains strength.</sub>

<sub>`[0,5]` are the SPRT bounds in **nElo** (fastchess's default model), ≈ `[0, 2.3]` in the
logistic Elo the `+33.18` is quoted in. The point estimate is logistic Elo and is comparable to
other engines' figures; only the pass/fail threshold is nElo.</sub>

<sub>**Why this is smaller than the sum of the incremental rows above:** the incremental gains are
**not additive**, and this was measured, not assumed — back on 2026-07-13 the first two rows (+15.8 and
+23.8) already gave only **+14.6 ± 15.5** in a direct 6.0-vs-5.1 gate. Gains overlap (a re-tuned search
partly compensates for what the network already knows), row 1 was measured at a shorter time control,
and row 3's ±17.1 over 394 games was too noisy to carry a point estimate. The consistency check works
out: the direct gate moved from **+14.6** (before the network work) to **+33.2** (with `v3`) — a
**+18.6** contribution from the network, matching the independent net-isolated measurement of
**+15.14 ± 7.69** for `v3` vs `v1` within noise.</sub>

#### Pawnocchio 1.9.1

**6.0** vs Pawnocchio 1.9.1, 1 thread, 128 MB, UHO 2024 book, resign-adjudicated:

| Date | Time control | Games | Score (6.0) | Elo (6.0) | LOS |
|---|---|---|---|---|---|
| 2026-07-18 | 45+0.45 | 1000 | 56.0% | **+41.89 ± 11.08** | 100.00% |

<sub>6.0: W 285 · L 165 · D 550. Pentanomial [0–2]: [3, 68, 241, 182, 6]. Where 5.1 was essentially
even with Pawnocchio 1.9.1, 6.0 clears it by a confirmed margin — consistent with the +33 gate over
5.1. 5.1's own results against Pawnocchio and other external engines: [`HISTORY.md`](HISTORY.md).</sub>

## License

[![License: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](COPYING)

> [!IMPORTANT]
> **GPLv3** — see [`COPYING`](COPYING). Only the **NNUE inference code** is derived from **Stockfish** (the SFNNv13 evaluation machinery in `nnue/`, GPLv3); the search and the rest of the engine are the project's own. Of the two extra NNUE input blocks: **`PassedPawns` is an original feature of this project**, whereas **`PawnPair` implements a pawn-pair input feature shared across several open-source engines** (Stormphrax, Viridithas, Pawnocchio — see [Credits](#credits)); its C++ implementation and trained weights are the project's own, but the feature *design* is not. The shipped network was trained by the project (see [`NETWORKS.md`](NETWORKS.md)). Because the engine incorporates Stockfish's GPL code, **the whole project is distributed under GPLv3**, with Stockfish's copyright notices preserved.

## Credits

See the [Credits section in the README](README.md#credits) — including the NNUE input-feature
attribution (`PawnPair` shared with Stormphrax / Viridithas / Pawnocchio; `PassedPawns` original to
this project).
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

[6.0 (prerelease)](#triumviratus-60-prerelease) · [Triumviratus 5.1](#triumviratus-51) · [Results](#results) · [History](HISTORY.md) · [License](#license) · [Credits](#credits)

</div>

---

## Triumviratus 6.0 (prerelease)

> [!NOTE]
> **Prerelease available.** Version-bump gate against 5.1 **passed**: **+33.18 ± 9.86 Elo** at 20+0.2
> (LOS 100%, SPRT `[0,5]` passed, 1166 games) — see [Results](#results).

Three changes over 5.1:

- **New network architecture — `TRANN1` (Triumviratus Rubicon Alea NNUE 1).** Extends the SFNNv13
  feature set with two input blocks of our own: **pawn-pair features** (4560 — phalanxes, chains,
  doubled/isolated pawns: pawn-structure geometry the threat features alone don't capture) and
  **passed-pawn features** (96 — one per passed pawn: a relational property `HalfKAv2_hm` cannot
  express directly). Both are grafted with zero-initialised columns onto the previous net —
  bit-identical at graft time — then fine-tuned; both fold into the existing threat accumulator, so
  no new SIMD path is added. Shipped in **`nn-rubicon-alea-v3`**, whose net format is consequently
  **not** SFNNv13 (the reader still loads v2-format nets, zero-filling the passed-pawn segment, so a
  single binary can gate v2 against v3). See [`NETWORKS.md`](NETWORKS.md) for training details.
- **SPSA mega co-tune** of the search parameters, co-tuned as a block and baked into the compiled
  defaults.
- **TMv2 time management** — a multiplicative-stateless time manager (stability, eval-trend, node and
  predicted-move factors), SPSA-tuned. It is **time-control gated**: measured **+23.8 Elo at 20+0.2** but
  **−22.9 at 10+0.1**, so it activates only when the game's base time ≥ 15 s and there's an increment
  (`TMv2MinBaseMs`); below that it falls back to the original time manager. Captures the long-TC gain
  without the short-TC regression.

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

<sub>TMv2 is TC-gated: the −22.9 Elo it costs at 10+0.1 is why it falls back to the original time
manager below 15 s.</sub>

<sub>**A short-time-control mirage, recorded because the lesson cost a day.** A fourth row briefly
lived here: halving the singular-extension margin on exact-bound transposition entries, measured at
**+8.6 ± 7.6 Elo over 2304 games at 10+0.1**. At the release time control the same patch measured
**−8.6 ± 10.5 at 20+0.2**. The two results were not in conflict — they were the same patch on either
side of a crossover. Benching at several depths showed the tree shrinking by 7.4 % at depth 13 and
11.1 % at depth 17, then **growing by 42.7 % at depth 20**. The margin is `depth`-proportional, so
halving it subtracts more the deeper the search goes; worse, the double- and triple-extension
thresholds are measured *from* that margin, so raising it made those cascade too. Neither anchoring
the double/triple thresholds to the full margin nor capping the halving above depth 18 (which
restored the tree to baseline exactly) recovered the Elo. The patch is reverted; the bench signature
is back to **592074**. The takeaway now applies to every extension or reduction patch: bench at
multiple depths before baking, because a single depth is blind to a crossover.</sub>

<sub>¹ Row 3 is a **cumulative development snapshot** measured against the state after rows 1–2 (mega
co-tune + TMv2 on the graft-time, v1-identical network). It bundles four changes at once: the
mid-training **`nn-rubicon-alea-v2`** network (checkpoint ep439 of ~800, so this *understates* the final
net), an SPSA re-tune of the search on the new network, and the **large-pages / NPS** engine changes. (The
**DoDeeper** LMR re-search extension was enabled here too but is TC-dependent — +12.95 at 15+0.15,
neutral +0.9 at this 20+0.2 — so it contributes little to this row and is not baked.) It predates the
final v2 network and the v3 network above; the definitive version-bump gate of the 6.0 prerelease
against 5.1 at 20+0.2 is running.</sub>

#### Corrections

Two bugs found while preparing the prerelease. Both are fixed in the current source and in the
published binaries; they are recorded because the first changes what *earlier* prerelease builds were.

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

## Triumviratus 5.1

Current release. Keeps 5.0's own-lineage network **`nn-rubicon-alea-v1`** (SFNNv13, threats-trained from
scratch). Adds a recalibrated eval scale, two-level TT, hindsight extensions, faster SEE/AVX-512
accumulators, and re-tuned time management. Two SPRT-confirmed gains (see [Results](#results)): a
**second-audit patch** (**+26 Elo**: threat-indexed quiet history, refined TT-cutoff, aspiration/fail-high
tweaks, an SPSA-tuned singular/extension vector) and an **NPS-optimization patch** (**+26 to +42 Elo** by
time control): a lazy NNUE-mirror apply — the board mirror + threat computation is deferred until an eval
actually needs it, not paid on every legal move — plus a `-mtune=native` PGO build.

| | |
|---|---|
| **Evaluation** | NNUE, SFNNv13 — own-lineage `nn-rubicon-alea-v1`, loaded at runtime via `EvalFile` (not embedded) |
| **Search** | PVS · LMR (incl. captures) / NMP / futility / razoring / SEE pruning · singular & multi-cut extensions · ProbCut · correction & continuation history · threat-aware ordering |
| **Time management** | Re-tuned soft/hard budget, score-drop and node-based extensions |
| **Parallel** | Lazy SMP (`Threads`) |
| **Endgames** | Syzygy via Fathom (`SyzygyPath`) |

Source in [`source/`](source/). Build: see [`source/BUILD_NOTES.md`](source/BUILD_NOTES.md) (Linux `make`,
MSVC `Triumviratus_5.0.vcxproj`, or clang-PGO).

<sub>Earlier releases (**5.0**, **4.2**) and their match results: [`HISTORY.md`](HISTORY.md).</sub>

## Results

#### 6.0 vs 5.1 — version-bump gate (2026-07-17)

> [!NOTE]
> Release binary vs release binary (AVX-512), each loading **its own** network (6.0 → `rubicon-alea-v3`,
> 5.1 → `rubicon-alea-v1`), 1 thread, 64 MB, UHO 2024 book.

| Time control | Games | Score (6.0) | Elo (6.0) | LOS | SPRT |
|---|---|---|---|---|---|
| 20+0.2 | 1166 | 54.8% | **+33.18 ± 9.86** | 100.00% | `[0,5]` **passed** (LLR 2.95) |

<sub>6.0: W 301 · L 190 · D 675. Pentanomial [0–2]: [3, 81, 306, 188, 5].</sub>

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
even with Pawnocchio 1.9.1 (below), 6.0 clears it by a confirmed margin — consistent with the
+33 gate over 5.1.</sub>

Earlier, **5.1** vs Pawnocchio 1.9.1 (znver5 build), AVX512, 1 thread, 64 MB hash, no score-based
adjudication, UHO 2024 book:

| Date | Time control | Opening | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|
| 2026-07-08 | 60+0.6 | UHO_2024_8mvs_big_+080_+099.epd | 2000 | 50.4% | **+2.61 ± 7.48** | 75.26% |
| 2026-07-08 | 20+0.2 | UHO_2024_8mvs_big_+080_+099.epd | 800 | 51.4% | **+9.56 ± 12.89** | 92.71% |
| 2026-07-06 | 10+0.15 | UHO_2024_8mvs_big_+095_+114.epd | 612 | 51.1% | **+7.95 ± 12.35** | 85.78% |

<sub>At long TC (60+0.6, 2000 games) Triumviratus 5.1 and Pawnocchio 1.9.1 were essentially even —
the small edge (LOS 75%) within noise. 6.0 turns that into a clear +42.</sub>

#### vs. external engines (2026-07-05)
`v5.1` (1 thread), no score-based adjudication, UHO 2024 book.

| Date | Opponent | Time control | Hash | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|---|
| 2026-07-05 | Pawnocchio 1.9.1 | 20+0.2 | 512 MB | 558 | 48.9% | **-7.5 ± 14.8** | 15.9% |
| 2026-07-05 | Berserk 14 | 25+0.25 | 1024 MB | 322 | 46.3% | **-25.9 ± 18.1** | 0.24% |

<sub>Balanced-book matches draw far more than the unbalanced UHO set — compare sign/LOS across rows, not
the raw Elo number. Older gates (5.1 vs 5.0, 5.0 vs 4.2): [`HISTORY.md`](HISTORY.md).</sub>

## License

[![License: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](COPYING)

> [!IMPORTANT]
> **GPLv3** — see [`COPYING`](COPYING). Only the **NNUE inference code** is derived from **Stockfish** (the SFNNv13 evaluation machinery in `nnue/`, GPLv3); the search, the rest of the engine, the two extra feature blocks (`PawnPair`, `PassedPawns`) and the shipped network are the project's own (see [`NETWORKS.md`](NETWORKS.md)). Because the engine incorporates that GPL code, **the whole project is distributed under GPLv3**, with Stockfish's copyright notices preserved.

## Credits

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — SFNNv13 NNUE evaluation.
- **[BBC](https://github.com/maksimKorzh/chess_programming)** by Maksim Korzh ("Code Monkey King") — original bitboard/magic-number move generator; the project's earliest (2024) foundation for `attacks.cpp`/`magic.cpp`/`movegen.cpp` and the original search, both since substantially rewritten and extended.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing.
- **[Berserk](https://github.com/jhonnold/berserk)** and **[Pawnocchio](https://github.com/JonathanHallstrom/pawnocchio)** — studied for search/move-ordering ideas informing some of the engine's own implementations.
- Thanks to **Maurizio Platino** for the SPSA search-tuning and for extensive testing throughout the project,
  generously contributing his hardware.

<sub>Developed openly and with significant AI assistance.</sub>
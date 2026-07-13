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

[6.0 (dev)](#triumviratus-60-in-development) · [Triumviratus 5.1](#triumviratus-51) · [Triumviratus 5.0](#triumviratus-50) · [Triumviratus 4.2](#triumviratus-42) · [Results](#results) · [License](#license) · [Credits](#credits)

</div>

---

## Triumviratus 6.0 (in development)

> [!NOTE]
> Work in progress — not yet released or gated. 5.1 remains the current stable version.

Three changes over 5.1:

- **New network architecture — `TRANN1` (Triumviratus Rubicon Alea NNUE 1).** Extends the SFNNv13
  feature set with a third input block of **pawn-pair features**, giving the network explicit awareness
  of pawn structure (phalanxes, chains, doubled/isolated pawns) that the threat features alone don't
  capture. The block is grafted onto `nn-rubicon-alea-v1` with zero-initialised columns — bit-identical
  to v1 at graft time — then fine-tuned into **`nn-rubicon-alea-v2`** (training in progress). Engine-side
  the block folds into the existing threat accumulator, so no new SIMD path is added.
- **SPSA mega co-tune** of ~50 search parameters, baked into the compiled defaults.
- **TMv2 time management** — a multiplicative-stateless time manager (stability, eval-trend, node and
  predicted-move factors), SPSA-tuned. It is **time-control gated**: measured **+23.8 Elo at 20+0.2** but
  **−22.9 at 10+0.1**, so it activates only when the game's base time ≥ 15 s and there's an increment
  (`TMv2MinBaseMs`); below that it falls back to the original time manager. Captures the long-TC gain
  without the short-TC regression.

#### Incremental gains (vs 5.1 baseline)

Each row is measured against the state *before* that change (1 thread, 64 MB, UHO 2024 book
`UHO_2024_8mvs_big_+080_+099.epd`, no score-based adjudication).

| # | Change | TC | Games | Elo | LOS |
|---|---|---|---|---|---|
| 1 | SPSA mega co-tune (50 params) | 10+0.1 | 1058 | **+15.8 ± 10.9** | 99.8% |
| 2 | TMv2 time management (gated ≥ 15 s) | 20+0.2 | 380 | **+23.8 ± 18.2** | 99.5% |

<sub>TMv2 is TC-gated: the −22.9 Elo it costs at 10+0.1 is why it falls back to the original time
manager below 15 s. Cumulative total will be gated against 5.1 at 20+0.2 once the v2 network lands.</sub>

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

## Triumviratus 5.0

**SFNNv13** NNUE (`Full_Threats + HalfKAv2_hm`) with an SPSA-co-tuned alpha-beta search and Lazy SMP. Ships
the own-lineage network **`nn-rubicon-alea-v1`**. See [`NETWORKS.md`](NETWORKS.md) for training details.

## Triumviratus 4.2

First release with a **NNUE network trained by the author**. *(CCRL rating: to be added.)*

## Results

#### 5.1 vs 5.0 — official release gate (2026-07-07)

> [!NOTE]
> `v5.0` vs `v5.1`, **AVX2 build** (the CCRL binary), same network (`nn-rubicon-alea-v1`),
> no score-based adjudication (games decided by mate / 50-move / repetition only).
> The definitive 2000-game version-bump gate.

Book: **UHO 2024** (`UHO_2024_8mvs_big_+080_+099.epd`).

| Time control | Threads | Hash | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|
| 20+0.2 | 1 | 64 MB | 2000 | 59.2% | **+64.7 ± 7.6** | 100.00% |

<sub>v5.1: W 621 · L 253 · D 1126. Pentanomial (v5.1) [0–2]: [1, 87, 483, 401, 28].</sub>

#### Pawnocchio 1.9.1

`v5.1` vs Pawnocchio 1.9.1 (znver5 build), AVX512, 1 thread, 64 MB hash, no score-based adjudication,
UHO 2024 book.

| Date | Time control | Opening | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|
| 2026-07-08 | 60+0.6 | UHO_2024_8mvs_big_+080_+099.epd | 2000 | 50.4% | **+2.61 ± 7.48** | 75.26% |
| 2026-07-08 | 20+0.2 | UHO_2024_8mvs_big_+080_+099.epd | 800 | 51.4% | **+9.56 ± 12.89** | 92.71% |
| 2026-07-06 | 10+0.15 | UHO_2024_8mvs_big_+095_+114.epd | 612 | 51.1% | **+7.95 ± 12.35** | 85.78% |

<sub>At long TC (60+0.6, 2000 games) Triumviratus 5.1 and Pawnocchio 1.9.1 are essentially even —
the small edge (LOS 75%) is within noise, not a confirmed gap.</sub>

#### vs. external engines (2026-07-05)
`v5.1` (1 thread), no score-based adjudication, UHO 2024 book.

| Date | Opponent | Time control | Hash | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|---|
| 2026-07-05 | Pawnocchio 1.9.1 | 20+0.2 | 512 MB | 558 | 48.9% | **-7.5 ± 14.8** | 15.9% |
| 2026-07-05 | Berserk 14 | 25+0.25 | 1024 MB | 322 | 46.3% | **-25.9 ± 18.1** | 0.24% |

<details>
<summary><b>Historical</b></summary>
<br>

| Date | Match | Time control | Book | Games | Score | Elo | LOS |
|---|---|---|---|---|---|---|---|
| 2026-07-03 | 5.1 vs 5.0 | 10+0.2 | UHO | 250 | 60.8% | **+76.25 ± 29.82** | 100.00% |
| 2026-07-04 | 5.1-patched vs 5.1 (all patch improvements) | 12+0.12 | UHO | 500 | 53.8% | **+26.5 ± 15.4** | 99.96% |
| 2026-07-03 | 5.1 vs 5.0 | 30+0.2 | UHO | 600 | 58.7% draws | **+31.5 ± 17.6** | 99.98% |
| — | 5.1 vs 5.0 | 10+0.1 | UHO | 300 | — | **+27** | 99% |
| — | 5.1 vs 5.0 | 3min+1s | UHO | 100 | 54% | **+36** | — |
| — | 5.0 vs 4.2 | 20+0.2 | self-play | — | — | **+50** | — |
| — | 5.0 vs 4.2 | 3min+1s | UHO | 100 | 61.5% | **+81** | — |

</details>

Gap widens at longer TC (deeper search rewards the stronger network). Balanced-book matches draw far more
than the unbalanced UHO set — compare sign/LOS across rows, not the raw Elo number.

## License

[![License: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](COPYING)

> [!IMPORTANT]
> **GPLv3** — see [`COPYING`](COPYING). Only the **NNUE inference code** is derived from **Stockfish** (the SFNNv13 evaluation in `nnue/`, GPLv3); the search, the rest of the engine, and the shipped network are the project's own (see [`NETWORKS.md`](NETWORKS.md)). Because the engine incorporates that GPL code, **the whole project is distributed under GPLv3**, with Stockfish's copyright notices preserved.

## Credits

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — SFNNv13 NNUE evaluation.
- **[BBC](https://github.com/maksimKorzh/chess_programming)** by Maksim Korzh ("Code Monkey King") — original bitboard/magic-number move generator; the project's earliest (2024) foundation for `attacks.cpp`/`magic.cpp`/`movegen.cpp` and the original search, both since substantially rewritten and extended.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing.
- **[Berserk](https://github.com/jhonnold/berserk)** and **[Pawnocchio](https://github.com/JonathanHallstrom/pawnocchio)** — studied for search/move-ordering ideas informing some of the engine's own implementations.
- Thanks to **Maurizio Platino** for the SPSA search-tuning and for extensive testing throughout the project,
  generously contributing his hardware.

<sub>Developed openly and with significant AI assistance.</sub>
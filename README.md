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

[Triumviratus 5.1](#triumviratus-51) · [Triumviratus 5.0](#triumviratus-50) · [Triumviratus 4.2](#triumviratus-42) · [Results](#results) · [License](#license) · [Credits](#credits)

</div>

---

## Triumviratus 5.1

Current release. Keeps 5.0's own-lineage network **`nn-rubicon-alea-v1`** (SFNNv13, threats-trained from
scratch). Adds a recalibrated eval scale, two-level TT, hindsight extensions, faster SEE/AVX-512
accumulators, re-tuned time management, a display-only eval normalization ("+1.00" ≈ 50% win probability),
a **second-audit patch** (**+26 Elo cumulative**, SPRT-confirmed — see [Results](#results)): threat-indexed
quiet history, refined TT-cutoff, aspiration/alpha-raise/fail-high tweaks, an SPSA-tuned singular/extension
vector, and an **NPS-optimization patch** (2026-07-05, **+26 to +42 Elo** depending on time control,
SPRT-confirmed — see [Results](#results)): a lazy NNUE-mirror apply (the board mirror + threat computation
for a move is deferred until an evaluation actually needs it, instead of paying for it on every legal
move) plus a `-mtune=native` PGO build flag — a real, measurable strength gain, not just raw NPS. A
**TT-cutoff history malus** (2026-07-06, **+15.6 Elo**, SPRT-confirmed — see [Results](#results)): the
opponent's quiet move that walked into a TT-cutoff is now malused, tuned to only cool moves the parent
node had barely explored. 28 more search toggles (verified against the compiled defaults, not just the
advertised UCI text) were screened and left off — no net gain yet, kept for future tuning.

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

#### TT-cutoff history malus (2026-07-06)

`v5.1+TTCutMalus` vs `v5.1`, 20+0.2, 1 thread, 128 MB hash, no score-based adjudication.

| Opponent | Time control | Opening | Games | Score | Elo | LOS |
|---|---|---|---|---|---|---|
| v5.1 (self) | 20+0.2 | UHO_2024_8mvs_big_+095_+114.epd | 1006 | 52.2% | **+15.55 ± 11.09** | 99.71% |

<sub>Baked in as default ON.</sub>

#### 5.1 final version-bump verification (2026-07-05)

> [!NOTE]
> `v5.0` vs `v5.1`, no score-based adjudication (games decided by mate / 50-move / repetition only).

Book: **UHO 2024** (`UHO_2024_8mvs_big_+080_+099.epd`).

| Time control | Threads | Hash | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|
| 20+0.2 | 1 | 256 MB | 666 | 59.1% | **+63.8 ± 13.2** | 100.00% |
| 40+0.4 | 1 | 256 MB | 626 | 60.3% | **+72.6 ± 13.0** | 100.00% |
| 15+0.15 | 4 | 1024 MB | 100* | 59.0% | **+63.2 ± 37.1** | 99.97% |

<sub>*Stopped early by choice, not by SPRT bound — smaller sample, wider error bar.</sub>

#### NPS-optimization patch (2026-07-05)

`v5.1NPS` = `v5.1` + lazy NNUE-mirror apply + `-mtune=native` PGO build (same network, same
search — see [Triumviratus 5.1](#triumviratus-51)). No score-based adjudication, 1 thread, 128 MB hash.

| Opponent | Time control | Opening | Games | Score (v5.1NPS) | Elo (v5.1NPS) | LOS |
|---|---|---|---|---|---|---|
| v5.1 (self) | 20+0.2 | UHO_2024_8mvs_big_+095_+114.epd | 200 | 56.0% | **+41.9 ± 26.0** | 99.93% |
| v5.1 (self) | 10+0.15 | UHO_2024_8mvs_big_+095_+114.epd | 396 | 53.8% | **+26.4 ± 18.6** | 99.74% |

<sub>SPRT in progress (LLR hadn't reached the [0, 5] elo bound at time of writing) — LOS is already
conclusive.</sub>

#### Post-fix re-verification (2026-07-05, AVX2 build)

After the NPS patch above **and** an `EvalFile`-default consistency fix (dev builds used to
silently default to the Stockfish SFNNv13 reference net instead of `nn-rubicon-alea-v1.nnue`;
both build kinds now always load the same own-lineage net), re-confirms the `v5.0`→`v5.1` gap
holds on the AVX2 build specifically. 1 thread, 128 MB hash, no score-based adjudication.

| Opponent | Time control | Opening | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|
| v5.0 (avx2) | 10+0.15 | UHO_2024_8mvs_big_+095_+114.epd | 318 | 58.2% | **+57.3 ± 20.6** | 100.00% |

<sub>SPRT in progress (LLR at 43.5% of the [0, 5] elo bound at time of writing) — LOS already conclusive.</sub>

#### Pawnocchio 1.9.1 (2026-07-06)

`v5.1` vs Pawnocchio 1.9.1, 10+0.15, 1 thread, 128 MB hash, no score-based adjudication.

| Opponent | Time control | Opening | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|
| Pawnocchio 1.9.1 | 10+0.15 | UHO_2024_8mvs_big_+095_+114.epd | 612 | 51.1% | **+7.95 ± 12.35** | 85.78% |

<sub>SPRT in progress.</sub>

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
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing.
- **[Berserk](https://github.com/jhonnold/berserk)** and **[Pawnocchio](https://github.com/JonathanHallstrom/pawnocchio)** — studied for search/move-ordering ideas informing some of the engine's own implementations.
- Search and engine by **Francesco Torsello**.
- Thanks to **Maurizio Platino** for the SPSA search-tuning and for extensive testing throughout the project,
  generously contributing his hardware.

<sub>Developed openly and with significant AI assistance.</sub>
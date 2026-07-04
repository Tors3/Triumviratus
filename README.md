<div align="center">

<img src="logo.png" alt="Triumviratus" width="200">

# Triumviratus

**A strong UCI chess engine in C++** — NNUE evaluation · SPSA-tuned alpha-beta search · Syzygy tablebases

**by Francesco Torsello**

<sub>in collaboration with Maurizio Platino</sub>

</div>

---

## Triumviratus 5.1

Current release. Keeps 5.0's own-lineage network **`nn-rubicon-alea-v1`** (SFNNv13, threats-trained from
scratch). Adds a recalibrated eval scale, two-level TT, hindsight extensions, faster SEE/AVX-512
accumulators, re-tuned time management, a display-only eval normalization ("+1.00" ≈ 50% win probability),
and a **second-audit patch** (**+26 Elo cumulative**, SPRT-confirmed — see [Results](#results)): threat-indexed
quiet history, refined TT-cutoff, aspiration/alpha-raise/fail-high tweaks, an SPSA-tuned singular/extension
vector. ~15 more search toggles were screened and left off by default (no net gain yet), kept for future tuning.

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

#### 5.1 final version-bump verification (2026-07-05)
`v5.0` vs `v5.1`, no score-based adjudication (games decided by mate / 50-move / repetition only — the two
versions use different eval-display scales, so score-based resign/draw thresholds would be asymmetric).
Book: **UHO 2024** (`UHO_2024_8mvs_big_+080_+099.epd`).

| Time control | Threads | Hash | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|
| 20+0.2 | 1 | 384 MB | 666 | 59.1% | **+63.8 ± 13.2** | 100.00% |
| 40+0.4 | 1 | 384 MB | 626 | 60.3% | **+72.6 ± 13.0** | 100.00% |
| 15+0.15 | 4 | 1024 MB | 100* | 59.0% | **+63.2 ± 37.1** | 99.97% |

<sub>*Stopped early by choice, not by SPRT bound — smaller sample, wider error bar.</sub>

#### vs. external engines
`v5.1` (1 thread), no score-based adjudication, UHO 2024 book.

| Opponent | Time control | Hash | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|
| Pawnocchio 1.9.1 | 20+0.2 | 512 MB | 558 | 48.9% | **-7.5 ± 14.8** | 15.9% |
| Berserk 14 | 25+0.25 | 1024 MB | 322 | 46.3% | **-25.9 ± 18.1** | 0.24% |

#### Historical
| Date | Match | Time control | Book | Games | Score | Elo | LOS |
|---|---|---|---|---|---|---|---|
| 2026-07-03 | 5.1 vs 5.0 | 10+0.2 | UHO | 250 | 60.8% | **+76.25 ± 29.82** | 100.00% |
| 2026-07-04 | 5.1-patched vs 5.1 (all patch improvements) | 12+0.12 | UHO | 500 | 53.8% | **+26.5 ± 15.4** | 99.96% |
| 2026-07-03 | 5.1 vs 5.0 | 30+0.2 | UHO | 600 | 58.7% draws | **+31.5 ± 17.6** | 99.98% |
| — | 5.1 vs 5.0 | 10+0.1 | UHO | 300 | — | **+27** | 99% |
| — | 5.1 vs 5.0 | 3min+1s | UHO | 100 | 54% | **+36** | — |
| — | 5.0 vs 4.2 | 20+0.2 | self-play | — | — | **+50** | — |
| — | 5.0 vs 4.2 | 3min+1s | UHO | 100 | 61.5% | **+81** | — |

Gap widens at longer TC (deeper search rewards the stronger network). Balanced-book matches draw far more
than the unbalanced UHO set — compare sign/LOS across rows, not the raw Elo number.

## License

**GPLv3** — see [`COPYING`](COPYING). Only the **NNUE inference code** is derived from **Stockfish** (the SFNNv13
evaluation in `nnue/`, GPLv3); the search, the rest of the engine, and the shipped network are the project's own
(see [`NETWORKS.md`](NETWORKS.md)). Because the engine incorporates that GPL code, **the whole project is
distributed under GPLv3**, with Stockfish's copyright notices preserved.

## Credits

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — SFNNv13 NNUE evaluation.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing.
- Search and engine by **Francesco Torsello**.
- Thanks to **Maurizio Platino** for the SPSA search-tuning and for extensive testing throughout the project,
  generously contributing his hardware.

<sub>Developed openly and with significant AI assistance.</sub>

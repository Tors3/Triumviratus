<div align="center">

<img src="logo.png" alt="Triumviratus" width="200">

# Triumviratus

**A strong UCI chess engine in C++** — NNUE evaluation · SPSA-tuned alpha-beta search · Syzygy tablebases

**by Francesco Torsello**

<sub>in collaboration with Maurizio Platino</sub>

</div>

---

## Triumviratus 5.1

The current release. It keeps 5.0's own-lineage network **`nn-rubicon-alea-v1`** (SFNNv13, threats-trained
from scratch) and matures the engine around it: a **recalibrated evaluation scale**, a **two-level
transposition table**, **hindsight search extensions**, faster **SEE** (threshold early-exit) and **AVX-512**
accumulator updates, a **re-tuned time management** (including a time-forfeit fix), a **display-only eval
normalization** (empirically fitted so "+1.00" means "50% win probability"), plus assorted **GUI-compatibility**
and LMR fixes.

**Latest patch** — Fixed: mate-vs-50-move-rule bug, `score mate` off-by-one, repetition-table overflow,
benign data race. Added, **ON by default** (**+26 Elo cumulative** vs the pre-patch release, SPRT-confirmed,
see [Results](#results)): threat-indexed quiet history (weight-tuned), refined TT-cutoff, aspiration
fail-high depth reduction, alpha-raise depth decrement, fail-high score smoothing, plus an SPSA-tuned
singular/extension vector. Added, **OFF by default** (screened, not currently a net gain): ~15 more search
toggles (post-LMR history update, NMP static margin / easy-capture gate, and others), kept for future tuning.

| | |
|---|---|
| **Evaluation** | NNUE, SFNNv13 — own-lineage `nn-rubicon-alea-v1`, loaded at runtime via `EvalFile` (not embedded) |
| **Search** | PVS · LMR (incl. on captures) / NMP / futility / razoring / SEE pruning · singular & multi-cut extensions · ProbCut · correction & continuation history · threat-aware ordering |
| **Time management** | Re-tuned soft/hard budget, score-drop and node-based extensions |
| **Parallel** | Lazy SMP (`Threads`) |
| **Endgames** | Syzygy via Fathom (`SyzygyPath`) |

Source code is in [`source/`](source/). **Build** — see [`source/BUILD_NOTES.md`](source/BUILD_NOTES.md):
Linux `make`, MSVC `Triumviratus_5.0.vcxproj`, or clang-PGO.

## Triumviratus 5.0

The **SFNNv13** NNUE evaluation (`Full_Threats + HalfKAv2_hm`) paired with an original, heavily
**SPSA-co-tuned** alpha-beta search and multi-threaded **Lazy SMP**. Ships the project's own-lineage network
**`nn-rubicon-alea-v1`** (SFNNv13, threats-trained from scratch). See [`NETWORKS.md`](NETWORKS.md) for how each
network was trained.

## Triumviratus 4.2

The first release to ship a **NNUE network trained by the author**.
*(CCRL rating: to be added.)*

## Results

| Date | Match | Time control | Book | Games | Score | Elo | LOS |
|---|---|---|---|---|---|---|---|
| 2026-07-03 | 5.1 vs 5.0 | 10+0.2 | UHO | 250 | 60.8% | **+76.25 ± 29.82** | 100.00% |
| 2026-07-04 | 5.1-patched vs 5.1 (all patch improvements) | 12+0.12 | UHO | 500 | 53.8% | **+26.5 ± 15.4** | 99.96% |
| 2026-07-03 | 5.1 vs 5.0 | 30+0.2 | UHO | 600 | 58.7% draws | **+31.5 ± 17.6** | 99.98% |
| — | 5.1 vs 5.0 | 10+0.1 | UHO | 300 | — | **+27** | 99% |
| — | 5.1 vs 5.0 | 3min+1s | UHO | 100 | 54% | **+36** | — |
| — | 5.0 vs 4.2 | 20+0.2 | self-play | — | — | **+50** | — |
| — | 5.0 vs 4.2 | 3min+1s | UHO | 100 | 61.5% | **+81** | — |

The gap tends to widen at longer time control, as the stronger network pays off with depth. Balanced-book
matches naturally run a much higher draw rate than the unbalanced UHO set — treat the sign and LOS as
comparable across rows, not the raw Elo number.

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

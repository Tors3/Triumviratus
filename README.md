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
accumulator updates, a **re-tuned time-management** (fixed a dead sudden-death fallback, then re-tuned the
whole block around it), a **display-only eval normalization** (empirically fitted so "+1.00" means "50% win
probability", matching engine convention), plus assorted **GUI-compatibility** and LMR fixes. Measures **≈ +27
Elo over 5.0** at blitz **10+0.1** (LOS 99%, 300 games) and **+36 Elo at 3min+1s** (54%, 100 games, tested by
Maurizio Platino — before the time-management re-tune below); the margin is expected to keep widening with
depth, as it did from 5.0 over 4.2. The time-management re-tune itself measured **+20 Elo at 15+0.15s**, but the
gain shrinks to statistical noise at longer time control (+3.9 Elo, LOS 61%, inconclusive at 60+0.6s over 280+
games) — kept as the new default (never negative at any tested time control), but treat the Elo gain as
demonstrated only at blitz-and-faster, not proven at slower time controls.

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
**`nn-rubicon-alea-v1`** (SFNNv13, threats-trained from scratch). Measured **≈ +50 Elo over 4.2** in internal
self-play (20+0.2), and **≈ +81 Elo over 4.2** at blitz **3min+1s** (61.5%, 100 games, tested by Maurizio Platino) —
the gap widens at longer time control, as the stronger threats-net pays off with depth. See
[`NETWORKS.md`](NETWORKS.md) for how each network was trained.

## Triumviratus 4.2

The first release to ship a **NNUE network trained by the author**.
*(CCRL rating: to be added.)*

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

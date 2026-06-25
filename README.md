<div align="center">

<img src="logo.png" alt="Triumviratus" width="200">

# Triumviratus

**A strong UCI chess engine in C++** — NNUE evaluation · Stockfish-faithful search · Syzygy tablebases

</div>

---

## Triumviratus 5.0 · *in development*

The current line: the **SFNNv13** NNUE evaluation (`Full_Threats + HalfKAv2_hm`) paired with a
Stockfish-faithful, SPSA-co-tuned search and multi-threaded **Lazy SMP**. An own-lineage network is in training.

| | |
|---|---|
| **Evaluation** | NNUE, SFNNv13 — loaded at runtime via `EvalFile` (not embedded) |
| **Search** | PVS · LMR / NMP / futility / razoring / SEE pruning · singular & multi-cut extensions · ProbCut · correction & continuation history · threat-aware ordering |
| **Parallel** | Lazy SMP (`Threads`) |
| **Endgames** | Syzygy via Fathom (`SyzygyPath`) |

Source code is in [`source/`](source/). **Build** — see [`source/BUILD_NOTES.md`](source/BUILD_NOTES.md):
Linux `make`, MSVC `Triumviratus_5.0.vcxproj`, or clang-PGO.

## Triumviratus 4.2

The previous release, and the first to ship a **NNUE network trained by the author**.
*(CCRL rating: to be added.)*

## License

**GPLv3** — see [`COPYING`](COPYING). Triumviratus is a derivative work of **Stockfish** (the SFNNv13 NNUE
code in `sfnnue_v13/` and the `nn-*.nnue` networks, GPLv3), so the whole project is distributed under GPLv3.

## Credits

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — SFNNv13 NNUE evaluation.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing.
- Search and engine by **Francesco Torsello**.
- Thanks to **Maurizio Platino** for the SPSA search-tuning and for generously contributing his hardware.

<sub>Developed openly and with significant AI assistance.</sub>

<p align="center">
  <img src="logo.png" alt="Triumviratus" width="220">
</p>

# Triumviratus 5.0 Chess Engine

Triumviratus is a strong, UCI-compliant chess engine written in C++. **Version 5.0** introduces the
**SFNNv13** NNUE architecture (a faithful port of Stockfish-master's evaluation) combined with a
Stockfish-faithful, heavily SPSA-co-tuned alpha-beta search and Syzygy tablebase support.

> Triumviratus 5.0 is a **development version**. Its search is co-tuned and validated against the
> 4.x line; an own-lineage NNUE network is in training. Default builds load the reference SFNNv13
> network (see *Setup & Usage*).

## What's New in 5.0

* **SFNNv13 NNUE architecture:** full port of Stockfish-master's evaluation — `Full_Threats + HalfKAv2_hm`
  feature set, L1 = 1024, 8 output buckets — with incremental accumulator updates (dirty-piece *and*
  dirty-threats) for high NPS.
* **Stockfish-faithful search, broadly co-tuned:** PVS, iterative deepening, aspiration windows; NMP,
  LMR, reverse-futility / futility, razoring (quadratic), LMP, SEE pruning, capture-futility, singular /
  double / triple extensions, negative extensions, multi-cut, ProbCut, correction history (pawn / minor /
  major / continuation), multi-ply continuation history, and threat-aware move ordering. Search constants
  are baked from a large SPSA co-tune (validated vs. the previous generation).
* **Parallel search:** Lazy SMP (shared TT, per-thread diversification).
* **Endgames:** Syzygy tablebases via [Fathom](https://github.com/jdart1/Fathom) (WDL in search, DTZ at root).

## Features

* **Protocol:** Fully UCI compliant.
* **Board representation:** 64-bit bitboards.
* **Evaluation:** NNUE, SFNNv13 (`Full_Threats + HalfKAv2_hm`), loaded at runtime (not embedded).
* **Search:** PVS + iterative deepening + aspiration windows.
* **Pruning & reductions:** NMP, LMR, RFP / futility, razoring, LMP, SEE pruning, capture futility,
  history & continuation-history heuristics, correction history.
* **Parallel search:** Lazy SMP.
* **Endgames:** Syzygy probing (WDL in search, DTZ at the root).

## Setup & Usage

To use Triumviratus in any standard chess GUI (Cute Chess, Arena, BanksiaGUI, Lichess-bot, …):

1. Build the engine (see *Compiling from source*) or download a release executable.
2. The engine loads its NNUE network at runtime — it is **not embedded**. Provide the SFNNv13 network
   `nn-71d6d32cb962.nnue` either next to the executable or via the `EvalFile` UCI option.
3. *(Optional)* For perfect endgames, point `SyzygyPath` at your Syzygy `.rtbw`/`.rtbz` folder.
4. Add the engine to your GUI with the standard UCI installation procedure.

### Recommended UCI settings
* **EvalFile:** path to the SFNNv13 network (required if not placed next to the exe).
* **Hash:** 64 MB default; 1024 MB+ recommended for long time controls.
* **Threads:** match your hardware's optimal concurrent thread count.
* **SyzygyPath:** absolute path to the Syzygy folder (empty = disabled).

## Compiling from source

Three build paths are documented in [`BUILD_NOTES.md`](BUILD_NOTES.md):

* **Linux / GCC or clang** — `make` (AVX2) or `make avx512`.
* **Windows / MSVC** — `Triumviratus_5.0.vcxproj`, `Release | x64`.
* **Windows / clang-cl + ThinLTO + PGO** — `build/build_pgo_clang_5_v13.ps1` (fastest binary).

The NNUE network and the PGO opening book are external data (not in the repository); see `BUILD_NOTES.md`.

## License

Triumviratus is **free software licensed under the GNU General Public License v3 (GPLv3)** — see the
[`COPYING`](COPYING) file for the full text.

Triumviratus incorporates and is a derivative work of **Stockfish** — the NNUE evaluation code in
`sfnnue_v13/` (and `sfnnue_v10/`) and the official `nn-*.nnue` networks — which is itself GPLv3. In
accordance with the GPL, the **entire Triumviratus project is therefore distributed under the GPLv3**,
the original Stockfish copyright notices are preserved in all derived files, and the complete
corresponding source code is published in this repository.

It is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

## Credits & acknowledgements

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — the SFNNv13 NNUE evaluation
  (`Full_Threats + HalfKAv2_hm`, `sfnnue_v13/`) and the `nn-*.nnue` networks are derived from Stockfish.
  Copyright (C) 2004–2024 The Stockfish developers.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing (`fathom/`).
  Copyright (C) Ronald de Man, basil00, and Jon Dart.
- **[Syzygy tablebases](https://github.com/syzygy1/tb)** — endgame tablebase format by Ronald de Man.

The classical search (PVS, pruning, reductions, Lazy SMP) and the engine glue are original work by
Francesco Torsello.

## Development note

Triumviratus is developed openly and with **significant AI assistance** (coding, refactoring, and
documentation). The NNUE evaluation is **derived from Stockfish** (GPLv3, credited above); the current
development focus of 5.0 is the SFNNv13 port, the search co-tune, and the training of an own-lineage
network. This is stated transparently so the nature and provenance of the code are clear.

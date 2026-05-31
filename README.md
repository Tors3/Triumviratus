# Triumviratus Chess Engine

Triumviratus is a strong, UCI-compliant chess engine written in C++.
Version 3.4.1 builds on the stable hybrid architecture, combining classical alpha-beta search enhancements with NNUE evaluation and an experimental policy network, adds **Lazy SMP** parallel search, and supports **Syzygy endgame tablebases** for perfect endgame play.

Estimated strength (CCRL 40/15 scale, measured via gauntlets against established CCRL-rated engines — Devre 6.0, Eleanor 4.1, Prune 3.2.1, pawn 4.0, Willow 4.0):
* **~3560 Elo (4-CPU)** — with Lazy SMP (the 4-CPU anchor rose from ~3500 to ~3560 on adopting it).
* **~3420 Elo (1-CPU)**.

## What's New

Each change below was validated in isolation with an SPRT match (search changes), an interleaved A/B NPS test (speed changes), or an anchored gauntlet (Elo); nothing is merged on feel.

* **SPSA-tuned LMR / futility (3.4.1):** Joint 17-parameter SPSA over the core late-move-reduction formula (never previously tuned) and futility/razoring margins. Four parameters converged and were baked: `LMRBase` 75→47 and `LMRDiv` 225→270 (less reduction overall), `FutilityBase` 82→111 (wider futility margin), `LMRTTDepth` 0→2 (reduce less on a deep TT hit). **+19.95 Elo** (LOS 99.85%, 1360 games, TC 8+0.08).
* **Lazy SMP (3.4.0):** Replaces the previous ABDADA busy-node coordination with independent threads that share only the transposition table, diversified by per-thread depth skipping. **~+55 Elo (4-CPU anchor 3503→3558)**; a direct A/B at 4 threads measured +102 Elo (LOS 99.99%). Toggle `LazySMP` (default on); the legacy ABDADA path is preserved behind the toggle.
* **Robustness (3.4.0):** Anti-forfeit — if a search is aborted under extreme time pressure before producing a move, the engine falls back to the first legal move instead of emitting `bestmove (none)`.
* **ProbCut (3.3.4):** Capture-gated forward pruning — when a reduced-depth verification search above `beta + ProbCutMargin` fails high, the node is pruned. ~+6 Elo. Toggle `ProbCut` (default on), margin `ProbCutMargin`.
* **SPSA-tuned margins (3.3.3):** RFP / razoring / futility / singular-double margins tuned with an in-house SPSA driver over fastchess. **+18.8 Elo** (LOS 99.2%).
* **Advanced Singular Extensions (3.3.2):** Double (+2) and negative (−1) extensions on top of the base singular extension. **+34 Elo** (LOS 99.7%) — the single biggest search gain. Toggle `SingularExt`.
* **Improving heuristic (3.3.1):** Static-eval trend modulates RFP / futility / LMR. ~+18 Elo. Toggle `Improving`.
* **Static-eval cache & node-based time management:** per-thread eval memoization (+3% NPS, bit-identical search; `EvalCache`) and node-share time scaling (`NodeTM`).
* **Syzygy Tablebases:** In-search WDL probing plus a root DTZ probe, up to 5 men via [Fathom](https://github.com/jdart1/Fathom).
* **Robustness:** Guard against searching illegal/king-capture positions parsed from FEN.

## Features

* **Protocol:** Fully UCI compliant.
* **Board Representation:** 64-bit Bitboards.
* **Search:** Principal Variation Search (PVS), Iterative Deepening, Aspiration Windows, advanced Singular Extensions (double/negative), Improving heuristic.
* **Pruning & Reductions:** Null Move Pruning (NMP), Late Move Reductions (LMR), Reverse Futility / Futility Pruning, Razoring, **ProbCut**, Late Move Pruning (LMP), SEE pruning, History & Continuation-History Heuristics. Key margins are SPSA-tuned and exposed as UCI spin options.
* **Parallel Search:** **Lazy SMP** — independent threads sharing the transposition table, with per-thread depth skipping for diversity (the legacy ABDADA scheme remains available via the `LazySMP` toggle).
* **Evaluation:** NNUE (HalfKAv2_hm architecture) with dual nets (big/small) for highly accurate static evaluation.
* **Endgames:** Syzygy tablebase probing (WDL in search, DTZ at the root).
* **Policy Network:** Experimental move-ordering policy network (can be toggled via UCI options).

## Setup & Usage

To use Triumviratus in any standard chess GUI (e.g., Cute Chess, Arena, BanksiaGUI, Lichess-bot):

1. Download the latest executable from the [Releases](../../releases) tab.
2. The engine requires specific network files to run correctly. Ensure the following files are placed in the **same directory** as the executable:
   * `nn-b1a57edbea57.nnue` (Big Net)
   * `nn-baff1ede1f90.nnue` (Small Net)
   * `triumviratus_policy.bin` (Policy Network weights)
3. *(Optional)* For perfect endgame play, place your Syzygy tablebase files in a folder and point the engine at it (see `SyzygyPath` below). A `Syzygy/` folder next to the executable is auto-detected.
4. Add the engine to your GUI using the standard UCI installation procedure.

### Recommended UCI Settings
* **Hash:** Allocate according to your available RAM (default is 64 MB, 1024 MB or more recommended for long time controls).
* **Threads:** Set to match your hardware's optimal concurrent thread count.
* **SyzygyPath:** Absolute path to the folder containing your Syzygy `.rtbw`/`.rtbz` files (leave empty to disable). Setting this explicitly is recommended when the GUI launches the engine from a different working directory.
* **UsePolicy:** `false` (default for standard testing), can be set to `true` to enable the experimental policy network.

## Compiling from Source

For maximum performance, Triumviratus should be compiled with AVX2 optimizations enabled. The project is configured for MSBuild (Visual Studio 2022, toolset v143) and the Release|x64 configuration already enables `/O2`, AVX2, intrinsics and whole-program optimization (LTCG). The output is named `Triumviratus_3.4.exe`.

### Windows (MSVC)
Using PowerShell and the MSBuild tools, navigate to the source directory and run:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "Triumviratus_3.0.vcxproj" `
  /t:Rebuild `
  /p:Configuration=Release `
  /p:Platform=x64
```

> Note: the Visual Studio project file is still named `Triumviratus_3.0.vcxproj`; only the build output is named `Triumviratus_3.4.exe` (via `<TargetName>` in the Release|x64 configuration).

## License

Triumviratus is **free software licensed under the GNU General Public License v3 (GPLv3)** — see the [`COPYING`](COPYING) file for the full text.

Triumviratus incorporates and is a derivative work of **Stockfish** (the NNUE evaluation code in `sfnnue/` and the official `nn-*.nnue` networks), which is itself licensed under the GPLv3. In accordance with the GPL, the **entire Triumviratus project is therefore distributed under the GPLv3**, the original Stockfish copyright notices are preserved in all derived files, and the complete corresponding source code is published in this repository.

You may redistribute and/or modify Triumviratus under the terms of the GPLv3. It is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

## Credits & Acknowledgements

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — the NNUE evaluation (HalfKAv2_hm architecture, `sfnnue/`) and the `nn-b1a57edbea57.nnue` / `nn-baff1ede1f90.nnue` networks are derived from Stockfish. Copyright (C) 2004-2024 The Stockfish developers.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing (`fathom/`). Copyright (C) Ronald de Man, basil00, and Jon Dart.
- **[Syzygy tablebases](https://github.com/syzygy1/tb)** — endgame tablebase format by Ronald de Man.

The classical search (PVS, pruning, reductions, ABDADA SMP), the policy network, and the engine glue are original work by Francesco Torsello.

## Development Note

Triumviratus is developed openly and with **significant AI assistance** (coding, refactoring, and documentation). The NNUE evaluation is **derived from Stockfish** (GPLv3, credited above); the original research focus of this project is the **policy network** and its integration into the search. This is stated transparently so the nature and provenance of the code are clear.

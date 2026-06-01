# Triumviratus Chess Engine

Triumviratus is a strong, UCI-compliant chess engine written in C++.
Version 3.4.2 builds on the stable hybrid architecture with extensively SPSA-tuned search parameters, combining classical alpha-beta search enhancements with NNUE evaluation and Syzygy tablebase support.

Currently, Triumviratus 3.4.2 plays at an estimated strength of **~3572–3583 Elo** (CCRL 4CPU scale), validated against a pool of CCRL-rated engines (gauntlet 3561 ±22).

## What's New in 3.4.2

* **SPSA fine-tuning round 2 (v3.4.2):** 18 search parameters tuned at TC 3+0.03; 9 converged to new optima (AspGrow 100→31, HistReductionDiv 3500→1041, RazorMult 102→139, FutilityImproving 60→93, ContHistDiv 5000→6595, SingularDoubleMargin 63→43, AspInitDelta 25→31, RFPMargin 30→21, SmallNetThreshold 1050→782). Validated: **+11.41 ±8.67 Elo, LOS 99.51%, SPRT H1 accepted** @3+0.03, 3595 games.
* **SPSA fine-tuning round 1 (v3.4.1):** Core LMR formula and futility margins tuned (LMRBase 75→47, LMRDiv 225→270, FutilityBase 82→111, LMRTTDepth 0→2). Validated: **+19.95 ±13.18 Elo, LOS 99.85%** @8+0.08, 1360 games.
* **Syzygy Tablebases:** In-search WDL probing plus root DTZ probe via [Fathom](https://github.com/jdart1/Fathom).
* **Robustness:** Fixed access violation on illegal/king-capture positions.

## Features

* **Protocol:** Fully UCI compliant.
* **Board Representation:** 64-bit Bitboards.
* **Search:** Principal Variation Search (PVS), Iterative Deepening, Aspiration Windows.
* **Pruning & Reductions:** Null Move Pruning (NMP), Late Move Reductions (LMR), Reverse Futility / Futility Pruning, Razoring, Late Move Pruning (LMP), SEE pruning, History & Continuation-History Heuristics.
* **Parallel Search:** ABDADA-style multi-threaded SMP.
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

> Note: the Visual Studio project file is still named `Triumviratus_3.0.vcxproj`; only the build output is named `Triumviratus_3.3.exe` (via `<TargetName>` in the Release|x64 configuration).

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

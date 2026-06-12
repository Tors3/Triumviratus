# Triumviratus Chess Engine

Triumviratus is a strong, UCI-compliant chess engine written in C++.
Version 4.0 builds on a hybrid architecture: a heavily co-tuned classical
alpha-beta search with NNUE evaluation — fed directly from the engine's native
bitboards via a **single-board bridge** — plus **Lazy SMP** parallel search and
**Syzygy endgame tablebases**.

Every change is validated empirically: search changes by SPRT, speed changes by
an interleaved A/B NPS test, Elo by anchored gauntlets. Nothing is merged on feel.

## What's New

### 4.0 — TT redesign + a large co-tuned search vector
* **Mega-SPSA search co-tune (+29.5 Elo):** 55 search / move-ordering / time-management
  parameters were re-tuned **together** (along with several previously dormant
  heuristics enabled: quiet-check qsearch, NMP verification, SF-style LMP,
  prior-move history bonus, low-ply history) in a single large SPSA run
  (~9,200 iterations). Validated by SPRT against the previous defaults — same
  network on both sides — at **+29.5 ± 21 Elo (LOS 99.7%, 20+0.08)**. The vector
  is co-adapted: it is shipped as a block.
* **24-byte transposition table with 16-bit static eval:** the TT entry stores the
  static evaluation at full 16-bit precision (with a key fragment as an anti-race
  guard), so NNUE forward passes are skipped on TT hits; plus windowed repetition
  scanning and masked evasion generation.
* **Cumulative gain since the last release (3.7):** a build-neutral SPRT (same
  compiler, same network on both sides) measured **+27.6 Elo (3.7 → 4.0)** before
  the co-tune above stacks on top, from correctness fixes (3.8), search micro-fixes
  (3.9), and the TT redesign.

### Earlier highlights (3.x)
* **SPSA search re-tune (3.7):** Google-Cloud SPSA over the core search vector, **+27.7 Elo** over 3.6.
* **Search co-tune + single-board NNUE (3.6):** **+42 Elo** over 3.5 — full history/ordering co-tune, threat-/check-aware ordering, correction history, the single-board NNUE bridge (~+3% NPS), incremental occupancy updates (~+3% NPS).
* **Staged MovePicker (3.5):** lazy staged move generation, **+16.6 Elo**.
* **Advanced Singular Extensions (3.3.2):** double/negative extensions, **+34 Elo** — the single biggest search gain.
* **Lazy SMP (3.4):** independent threads sharing the TT, diversified by per-thread depth skipping.

## Features

* **Protocol:** Fully UCI compliant.
* **Board Representation:** 64-bit Bitboards.
* **Search:** Principal Variation Search (PVS), Iterative Deepening, Aspiration Windows, advanced Singular Extensions (double/negative), Multi-Cut, Improving heuristic.
* **Move Ordering:** staged MovePicker, capture history, butterfly + multi-ply continuation history (1/2/3/4/6-ply), pawn-structure history, prior-move and low-ply history, counter-move and killer heuristics, threat- / check-aware ordering. All ordering weights are co-tuned and exposed as UCI spin options.
* **Pruning & Reductions:** Null Move Pruning (with verification), Late Move Reductions (continuous butterfly/continuation-history scaling), Reverse Futility / Futility Pruning, Razoring, ProbCut (TT-stored), Late Move Pruning (improving-scaled), SEE pruning, quiet checks in qsearch. Margins are SPSA-tuned and exposed as UCI spin options.
* **Parallel Search:** **Lazy SMP** — independent threads sharing the transposition table, with per-thread depth skipping and reduction-bias diversity.
* **Evaluation:** NNUE (Stockfish HalfKAv2_hm architecture) with dual nets (big/small), a **single-board bridge** feeding the network from the engine's native bitboards, a finny-table accumulator-refresh cache, a per-thread static-eval cache, a 16-bit static eval cached in the transposition table, and learned **correction history** (pawn + minor + major material keys).
* **Endgames:** Syzygy tablebase probing (WDL in search, DTZ at the root), up to 5 men via [Fathom](https://github.com/jdart1/Fathom).

## Setup & Usage

To use Triumviratus in any standard chess GUI (e.g., Cute Chess, Banksia GUI, Arena, lichess-bot):

1. Download the latest executable from the [Releases](../../releases) tab. Two
   variants are provided: `*_avx2` (Intel Haswell 2013+ / AMD Zen+) and `*_avx512`
   (Intel Ice Lake+ / AMD Zen4+). **BMI2 is required** (the engine always uses PEXT).
2. The engine loads its NNUE networks at runtime — place these files in the **same
   directory** as the executable:
   * `nn-b1a57edbea57.nnue` — **big net (default)**, the Stockfish HalfKAv2_hm network.
   * `nn-baff1ede1f90.nnue` — small net (always required).
   * `nn-rubicon-v1.nnue` — *(optional)* an in-house network; used as a fallback if
     the default big net is absent, or selectable at runtime via `EvalFile`.
3. *(Optional)* Place Syzygy tablebase files in a folder and set `SyzygyPath`
   (a `Syzygy/` folder next to the executable is auto-detected).
4. Add the engine to your GUI using the standard UCI procedure.

### Recommended UCI Settings
* **Hash:** according to available RAM (default 64 MB; 1024 MB+ for long time controls).
* **Threads:** match your hardware's optimal concurrent thread count.
* **EvalFile:** path to the big NNUE net to load (defaults to `nn-b1a57edbea57.nnue`).
* **SyzygyPath:** absolute path to your Syzygy `.rtbw`/`.rtbz` folder (empty to disable).
* **Move Overhead:** ms reserved per move for GUI/lag (default 50).

## Compiling from Source

The project is configured for MSBuild (Visual Studio 2022, toolset v143), Release|x64,
with `/O2`, AVX2, intrinsics and whole-program optimization. A Linux `Makefile` is also
provided (`make` for AVX2+BMI2, `make avx512` for AVX-512+VNNI).

For the fastest release builds, `build_pgo_clang.ps1` (clang-cl + ThinLTO + IR-PGO)
produces both distributable variants (`*_avx512` and `*_avx2`); `build_pgo.ps1` is the
MSVC-PGO path. All paths are relative, so the scripts run from a fresh clone.

### Windows (MSVC)
```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "Triumviratus_3.0.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64
```
> Note: the Visual Studio project file is still named `Triumviratus_3.0.vcxproj` for
> historical reasons; the build output name follows the engine version.

## License

Triumviratus is **free software licensed under the GNU General Public License v3 (GPLv3)** — see the [`COPYING`](COPYING) file for the full text.

Triumviratus incorporates and is a derivative work of **Stockfish** (the NNUE evaluation code in `sfnnue/` and the official `nn-*.nnue` networks), which is itself licensed under the GPLv3. In accordance with the GPL, the **entire Triumviratus project is therefore distributed under the GPLv3**, the original Stockfish copyright notices are preserved in all derived files, and the complete corresponding source code is published in this repository. Any binary release is accompanied by a link to this source repository.

You may redistribute and/or modify Triumviratus under the terms of the GPLv3. It is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

## Credits & Acknowledgements

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — the NNUE evaluation (HalfKAv2_hm architecture, `sfnnue/`) and the `nn-b1a57edbea57.nnue` / `nn-baff1ede1f90.nnue` networks are derived from Stockfish. Copyright (C) 2004-2024 The Stockfish developers.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing (`fathom/`). Copyright (C) Ronald de Man, basil00, and Jon Dart.
- **[Syzygy tablebases](https://github.com/syzygy1/tb)** — endgame tablebase format by Ronald de Man.

The classical search (PVS, pruning, reductions, move ordering, Lazy SMP), the bitboard
move generator, and the engine architecture are original work by Francesco Torsello.

## Development Note

Triumviratus is developed openly and with **significant AI assistance** (coding,
refactoring, tuning infrastructure, and documentation). The NNUE evaluation is
**derived from Stockfish** (GPLv3, credited above); the original work is the classical
search, the move generator, the engine architecture, and the empirical tuning pipeline.
This is stated transparently so the nature and provenance of the code are clear.

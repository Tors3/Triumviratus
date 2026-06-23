# Triumviratus Chess Engine

Triumviratus is a strong, UCI-compliant chess engine written in C++.
The current release (**4.2**) is the first **own-network** build: it evaluates with
our own **"rubicon"** NNUE — trained from scratch on T80 data, no Stockfish network
shipped — on top of the same heavily co-tuned classical alpha-beta search, **Lazy SMP**
parallel search, and **Syzygy endgame tablebases**.

Every change is validated empirically: search changes by SPRT, speed changes by
an interleaved A/B NPS test, Elo by anchored gauntlets. Nothing is merged on feel.

## What's New

### 5.0 — In development
* **Port of Stockfish's newest NNUE architecture (SFNNv13, threats-based):** Triumviratus 5
  faithfully ports Stockfish-master's current NNUE — the richer **threats** feature set
  (`FullThreats + HalfKAv2_hm`) that encodes board threats — with a **bit-exact**,
  incrementally-updated evaluation and a faster back end (clang PGO + AVX-512).
* **Own-lineage SFNNv13 network in training:** an in-house threats network is being trained
  following Stockfish's official **staged** training recipe (nnue-pytorch), continuing the
  own-lineage philosophy of 4.2 on the new architecture.
* **Search re-audit vs Stockfish-master:** the tree-pruning has been re-audited against
  current Stockfish; the missing aggressive-cutting features (capture futility, triple /
  full negative singular extensions, opponent-worsening margins, quadratic razoring) are
  being ported and will be **co-tuned together** for the stronger network.
* *Unreleased — work in progress. 4.2 remains the current release.*

### 4.2 — First own-lineage network
* **Own network by default:** ships `nn-rubicon-v1.nnue`, an in-house NNUE trained
  **from scratch on T80 data** (own lineage). **No Stockfish network is distributed.**
  The NNUE *inference code* (`sfnnue/`, HalfKAv2_hm) remains Stockfish-derived — see
  License — but the *weights* are our own.
* **`UseSmallNet` off by default:** an own small net (`mini-rubicon-v1.nnue`) exists but
  is a net loss in-game, so 4.2 runs big-net-only. The toggle is kept for the future.
* **Eval-wrapper retune for the new net:** the NNUE post-processing constants
  (`EvalOptimism`, `EvalComplexityDiv`, `EvalBlendDelta`) were re-tuned by SPSA for the
  rubicon net, since a different network wants a different eval-search coupling.
* **Cost of going own:** vs 4.1 (Stockfish net) at 12+0.12, the own-network 4.2 measures
  **−38.9 ± 19.9 Elo** — the deliberate, accepted price of a 100% own-lineage build. The
  search is unchanged from 4.1.

> **Independent community test — thanks to Maurizio Platino.** In a 100-game match run
> independently by Maurizio Platino (Fritz 18, Intel i7-8700, 6 threads/engine, 1024 MB
> hash, 3 min + 1 s, UHO 2022 6-move +120/+129 book by Stefan Pohl, no tablebases), the
> **own-lineage 4.2** network held its own against the established **Stockfish network**
> shipped by 4.1, **trailing by a margin of only 3 games** out of 100 at a long time
> control. An encouraging external confirmation that the from-scratch *rubicon* network is
> genuinely competitive with the Stockfish network. Thank you, Maurizio, for the rigorous
> test and the annotated games.

### 4.1 — Search co-tune (+35 Elo over 4.0)
* **Large SPSA co-tune (~10k iterations, 18 parameters):** pruning margins, history
  weights, LMR + SF-style depth-pruning block — re-tuned **together** as a block.
* **SPRT-verified vs 4.0** (same network, `nn-b1a57edbea57.nnue`, both sides):
  **+35.0 ± 17.9 Elo at 20+0.2 (LOS ~100%, fastchess, UHO_2024 +080/+099)** and
  **+17.8 ± 8.8 Elo at 12+0.12**.
* No new features vs 4.0 — same architecture. Just better-tuned constants.

### 4.0 — TT redesign + a large co-tuned search vector
* **Mega-SPSA search co-tune (+29.5 Elo):** 55 parameters re-tuned together with
  several dormant heuristics enabled (quiet-check qsearch, NMP verification,
  SF-style LMP, prior-move/low-ply history). SPRT-verified at
  **+29.5 ± 21 Elo (LOS 99.7%, 20+0.08)**.
* **24-byte transposition table with 16-bit static eval:** NNUE forward passes
  skipped on TT hits; windowed repetition scanning; masked evasion generation.
* **Cumulative gain 3.7 → 4.0 (pre co-tune): +27.6 Elo** (correctness fixes,
  search micro-fixes, TT redesign).

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
* **Evaluation:** NNUE (Stockfish HalfKAv2_hm inference architecture) with our **own `rubicon` network** trained from scratch on T80 data, an optional small net (`UseSmallNet`, off by default), a **single-board bridge** feeding the network from the engine's native bitboards, a finny-table accumulator-refresh cache, a per-thread static-eval cache, a 16-bit static eval cached in the transposition table, and learned **correction history** (pawn + minor + major material keys).
* **Endgames:** Syzygy tablebase probing (WDL in search, DTZ at the root), up to 5 men via [Fathom](https://github.com/jdart1/Fathom).

## Setup & Usage

To use Triumviratus in any standard chess GUI (e.g., Cute Chess, Banksia GUI, Arena, lichess-bot):

1. Download the latest executable from the [Releases](../../releases) tab. Two
   variants are provided: `*_avx2` (Intel Haswell 2013+ / AMD Zen+) and `*_avx512`
   (Intel Ice Lake+ / AMD Zen4+). **BMI2 is required** (the engine always uses PEXT).
2. The engine loads its NNUE network at runtime — place this file in the **same
   directory** as the executable:
   * `nn-rubicon-v1.nnue` — **the network** (our own, trained from scratch on T80).
     Shipped inside the release archive.
3. *(Optional)* Place Syzygy tablebase files in a folder and set `SyzygyPath`
   (a `Syzygy/` folder next to the executable is auto-detected).
4. Add the engine to your GUI using the standard UCI procedure.

### Recommended UCI Settings
* **Hash:** according to available RAM (default 64 MB; 1024 MB+ for long time controls).
* **Threads:** match your hardware's optimal concurrent thread count.
* **EvalFile:** path to the NNUE net to load (defaults to `nn-rubicon-v1.nnue`).
* **UseSmallNet:** dual-net mode (default `false`; big-net-only is strongest).
* **SyzygyPath:** absolute path to your Syzygy `.rtbw`/`.rtbz` folder (empty to disable).
* **Move Overhead:** ms reserved per move for GUI/lag (default 50).

## Compiling from Source

The project is configured for MSBuild (Visual Studio 2022, toolset v143), Release|x64,
with `/O2`, AVX2, intrinsics and whole-program optimization. A Linux `Makefile` is also
provided (`make` for AVX2+BMI2, `make avx512` for AVX-512+VNNI).

For the fastest release builds, `build/build_pgo_clang_42.ps1` (clang-cl + ThinLTO + IR-PGO)
produces both distributable variants (`*_avx512` and `*_avx2`); `build/build_pgo.ps1` is the
MSVC-PGO path. All paths are relative, so the scripts run from a fresh clone.

### Windows (MSVC)
```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "source\Triumviratus_4.2.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64
```
> Build output: `Triumviratus_4.2_avx512.exe` / `_avx2.exe` from `build/build_pgo_clang_42.ps1`.

## License

Triumviratus is **free software licensed under the GNU General Public License v3 (GPLv3)** — see the [`COPYING`](COPYING) file for the full text.

Triumviratus incorporates and is a derivative work of **Stockfish**: the NNUE evaluation
**inference code** in `sfnnue/` (the HalfKAv2_hm architecture) is taken from Stockfish,
which is itself licensed under the GPLv3. In accordance with the GPL, the **entire
Triumviratus project is therefore distributed under the GPLv3**, the original Stockfish
copyright notices are preserved in all derived files, and the complete corresponding source
code is published in this repository. Any binary release is accompanied by a link to this
source repository.

> **Network provenance (4.2):** the shipped network `nn-rubicon-v1.nnue` is **our own**,
> trained from scratch on T80 data — it is *not* a Stockfish network. Only the NNUE
> inference *code* is Stockfish-derived.

You may redistribute and/or modify Triumviratus under the terms of the GPLv3. It is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

## Credits & Acknowledgements

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — the NNUE evaluation *inference code* (HalfKAv2_hm architecture, `sfnnue/`) is derived from Stockfish. Copyright (C) 2004-2024 The Stockfish developers. The networks shipped with Triumviratus 4.2 are our own (`nn-rubicon-v1.nnue`), not Stockfish networks.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing (`fathom/`). Copyright (C) Ronald de Man, basil00, and Jon Dart.
- **[Syzygy tablebases](https://github.com/syzygy1/tb)** — endgame tablebase format by Ronald de Man.
- **Maurizio Platino** — independent testing, gauntlet validation, and community feedback.

The classical search (PVS, pruning, reductions, move ordering, Lazy SMP), the bitboard
move generator, the engine architecture, the own NNUE training pipeline (rubicon), and the
empirical tuning infrastructure are original work by Francesco Torsello.

## Development Note

Triumviratus is developed openly and with **significant AI assistance** (coding,
refactoring, tuning infrastructure, and documentation). The NNUE *inference code* is
**derived from Stockfish** (GPLv3, credited above); the original work is the classical
search, the move generator, the engine architecture, the own-lineage NNUE training
(rubicon), and the empirical tuning pipeline. This is stated transparently so the nature
and provenance of the code are clear.

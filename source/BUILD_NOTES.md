# Triumviratus 5.0 — build notes

SFNNv13 NNUE port of Stockfish-master + SF-faithful, SPSA-co-tuned search.
**Source only** — NNUE nets and generated artifacts are not in the repo (see "External data" below).

## Build paths

### 1. Linux / GCC (or clang) — `make`
```
make            # AVX2  + BMI2/PEXT
make avx512     # + AVX512 + VNNI + ICL (Intel n2/c3, AMD Zen4)
```
Produces `triumviratus`. The NNUE net is loaded at runtime via the `EvalFile` UCI option
(the binary is built with `-DNNUE_EMBEDDING_OFF`, so the net is NOT embedded).

### 2. Windows / MSVC — `Triumviratus_5.0.vcxproj`
Open in Visual Studio 2022 and build `Release | x64` (toolset v143, AVX512 by default).
Output: `x64\Release\Triumviratus_5.0.exe`. Put the net next to the exe (or set `EvalFile`).

### 3. Windows / clang-cl + ThinLTO + PGO — `build/build_pgo_clang_5_v13.ps1`
4-phase IR-based PGO (instrument → train on real positions → merge → optimize). Fastest binary.
```
.\build\build_pgo_clang_5_v13.ps1            # avx512
.\build\build_pgo_clang_5_v13.ps1 -Arch avx2
```
Needs VS "C++ Clang tools for Windows" (`clang-cl`, `llvm-profdata`).

> ⚠️ **Path caveat:** this script was written for the original dev-repo layout, where it sat at a
> root containing `Triumviratus_5\<vcxproj>`, `pgo_train.py`, and `OpeningBooks\...`. In this backup the
> files are reorganized (script + trainer under `build/`, vcxproj at the project root). Before running,
> adjust the paths near the top of the script (`$proj`, `$trainer`, `$book`) — or recreate the expected
> layout. `pgo_train.py` is included alongside the script in `build/`.

## External data (NOT in the repo)
- **NNUE net** — `nn-71d6d32cb962.nnue` (SFNNv13, Stockfish-master). Load via `EvalFile`,
  or place next to the exe. The MSVC/PGO builds expect it at `sfnnue_v13/nn-71d6d32cb962.nnue` as source.
  *(This is the SF-master net used as the strong reference; the project's own-lineage net is trained separately.)*
- **Opening book** (PGO training only) — `UHO_2024_8mvs_big_+080_+099.epd`.

## Notes
- Search defaults are baked from an SPSA co-tune (validated +18 Elo vs the prior baked defaults).
- The `5.0-B` search-refinement toggles (`CorrValExt`, `MalusScaled`, `BadNoisy`, `LMREnrich`, `DoDeeper`)
  are present but **gated OFF by default** (zero effect unless enabled via UCI) — still under co-tune.

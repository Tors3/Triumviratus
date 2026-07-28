# Training infrastructure

> Branch `training-7.0`. **Not part of the release** — `main` and `source/` are what ships.
> This is how the networks get trained and how the binaries get built.

## Where each thing runs

| | runs on | needs |
|---|---|---|
| `Training70_LegioSeptima/*.sh` | **rented Linux GPU box** | Python ≥ 3.12, CUDA, ~160 GB disk |
| `Training70_LegioSeptima/*.py` | wherever the trainer is | measurement tools, no data needed for `gpu_ceiling` |
| `build/build_pgo_clang_*.ps1` | **Windows, this PC only** | Visual Studio 2022 + *C++ Clang tools for Windows* |
| `build/match_70_vs_60.ps1` | Windows, this PC | fastchess + an opening book |
| Linux PGO builds | any Linux box | `make pgo-avx512` — it lives in **`source/Makefile`** on `main`, not here |

**On the PGO scripts specifically.** The two `.ps1` are Windows-only and machine-specific by
design: PGO profiles the binary *on the machine that will run it*, so the profile is not portable
and there is no point shipping one. They need `MSBuild` and `llvm-profdata` from a Visual Studio
install, and they drive the same four phases the Linux `make pgo-avx512` target does
(instrument → train on real positions → merge profiles → rebuild optimised).

If you need a PGO build on Linux — which is how the shipped Linux binaries are made — you do not
use these: you use `make pgo-avx512 CXX=clang++ BOOK=<book.epd>` from `source/`.

## Start here

- **`Training70_LegioSeptima/HANDOFF_VM.md`** — state of the 7.0 network, the recipe, the traps.
- **`Training70_LegioSeptima/HANDOFF_PERFORMANCE.md`** — how the training got 5× faster, and the
  30-second checks to run *before* renting a machine (three hosts were rejected in one night).

## Layout

```
Training70_LegioSeptima/
  setup_vm.sh              clone upstream at the pin, apply our patch, build the loader, verify
  download_phase1.sh       the five BT4-relabelled binpacks (121 GiB)
  download_phase2.sh       Leela-derived corpus (frees phase 1 first — refuses without a model)
  train_phase1.sh          500 epochs, DDP via torchrun
  train_phase2.sh          800 epochs from the phase-1 model
  triumviratus_passedpawns.patch    our 4-file delta on nnue-pytorch @ 9f72946
  gpu_ceiling.py           GPU throughput with the data loader excluded
  loader_ceiling.py        data loader throughput on its own
  profile_step.py          where the time actually goes in one training step
build/
  build_pgo_clang_7_trann2.ps1   Windows PGO for 7.0 (adds -DUSE_AVX512, which the .vcxproj omits)
  build_pgo_clang_6_trann1.ps1   same for the 6.0 release
  match_70_vs_60.ps1             paired gate, both engines PGO + AVX-512
  pgo_train.py                   drives the engine over real positions to collect profiles
```

The trainer itself is **not** vendored: `setup_vm.sh` clones upstream `nnue-pytorch` at commit
`9f72946` and applies the patch. That way it is verifiable that the box runs exactly the code
that was validated, and there is no 23 MB copy here going stale.

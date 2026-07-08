# Triumviratus 5.1 — Release Notes (2026-07-08)

## Headline

**+64.7 ± 7.6 Elo vs 5.0** (2000 games, 20+0.2, AVX2 build, same network, no adjudication, LOS 100%).
This is the official version-bump gate — see [README → Results](README.md#results) for the full table.

## What's new since 5.0

- **Second-audit patch** (+26 Elo cumulative, SPRT-confirmed): threat-indexed quiet history,
  refined TT-cutoff, aspiration/alpha-raise/fail-high tweaks, an SPSA-tuned singular/extension
  vector.
- **NPS-optimization patch** (+26 to +42 Elo depending on time control, SPRT-confirmed): a lazy
  NNUE-mirror apply (board mirror + threat computation for a move deferred until an evaluation
  actually needs it) plus a `-mtune=native` PGO build flag — a real, measurable strength gain,
  not just raw NPS.
- Recalibrated eval scale, two-level TT, hindsight extensions, faster SEE/AVX-512 accumulators,
  re-tuned time management.
- A **display-only** eval normalization ("+1.00" ≈ 50% win probability) — search, TT, and time
  management are untouched; only the printed `score cp` is rescaled.
- **Quieter startup**: the ASCII banner on launch is now suppressed (engine stays silent until it
  answers `uci`, matching strict UCI etiquette and making log/GUI parsing cleaner).

## Screened and left off (no net gain)

This cycle, ~12 additional search toggles were implemented, bench-swept for a sensible parameter
range, and SPRT-tested — all resulted in a neutral or negative signal and remain off by default:
`GoodCapHistDiv`, `QSMoveCap`, `RFPHistThresh`, `NMPStaticMargin`/`NMPTTNoisy`/`EasyCapGate`,
`TTCutMalus` (retested), `PostLMRHist`, `KillerReset`, `HistTrivGuard`, `MalusPct`,
`SingularPlyGuard`, `AspAvg`, `FHBoostMargin`, `QSDrawCheck`.

A methodological note for anyone reading the SPRT logs: three of those (`AspAvg`,
`FHBoostMargin`, `QSDrawCheck`) initially looked like ~9-11 Elo gains and were briefly baked in,
but a clean re-test (isolated, no early stopping) showed the effect was noise — a combination of
winner's-curse (picking the best of many near-zero-signal tests) and stopping the SPRT early on a
favorable-looking LOS. They were reverted before this release; the lesson is now written up in
`source/PIPELINE_5.1.md` for future tuning sessions.

A first Time-Management toggle (`TMStability`, soft-time reduction on a stable root best-move)
was implemented and is ready, but flat at short time controls — it needs re-testing at long TC
(40+/60+) before a verdict, since it's a reduction feature that needs long searches to matter.

## vs. external engines

- **Pawnocchio 1.9.1**: essentially even at long TC (60+0.6, 2000 games: +2.6 ± 7.5, within
  noise), a small edge at faster TC (20+0.2, 800 games: +9.6 ± 12.9).
- **Berserk 14**: −25.9 ± 18.1 (25+0.25, 322 games).

## Build

- Network: `nn-rubicon-alea-v1.nnue` (SFNNv13, own-lineage), unchanged from 5.0.
- Shipping binaries: clang-cl + ThinLTO + PGO, both AVX2 (CCRL-compatible) and AVX-512 targets.
  See [`source/BUILD_NOTES.md`](source/BUILD_NOTES.md).

## Downloads — two variants per architecture

- **Standard** (`Triumviratus_5.1_avx2.zip` / `..._avx512.zip`): the release build
  (`-DTRIUMV_RELEASE`) — only the standard UCI options are exposed (`Hash`, `Threads`,
  `Move Overhead`, `EvalFile`, `SyzygyPath`). Use this for play/tournaments.
- **`..._DEVELOPMENT_VERSION.zip`**: same search and network, built *without* `-DTRIUMV_RELEASE`,
  so every internal tuning parameter (SPSA spins, A/B toggles) is exposed as a UCI option. Use
  this only if you want to experiment with the search constants yourself — the extra options do
  not change default behavior (all default to the same values as the standard build).

## Tooling (not shipped, internal)

A standalone self-play datagen suite for a possible future rubicon-alea refinement pass
(T80 + a self-generated data fraction) now targets 5.1 (previously built for the experimental
6.0 bullet-net line). Not part of this release; mentioned here for provenance.

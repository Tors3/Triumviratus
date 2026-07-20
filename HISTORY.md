# Triumviratus — release history

Archive of the releases before 5.1 and of the match results that documented them.
The current releases live in the [`README`](README.md); how each network was trained is in
[`NETWORKS.md`](NETWORKS.md).

| Release | Network | Headline |
|---|---|---|
| **5.1** (current) | `nn-rubicon-alea-v1` | +64.7 Elo over 5.0 (official gate) |
| **5.0** | `nn-rubicon-alea-v1` | first SFNNv13 (threats) net · ≈ +50 Elo over 4.2 |
| **4.2** | `rubicon-v1` | first network trained by the author |

---

## Triumviratus 5.1

Current stable release. Keeps 5.0's own-lineage network **`nn-rubicon-alea-v1`** (SFNNv13,
threats-trained from scratch). Adds a recalibrated eval scale, two-level TT, hindsight extensions,
faster SEE/AVX-512 accumulators, and re-tuned time management. Two SPRT-confirmed gains: a
**second-audit patch** (**+26 Elo**: threat-indexed quiet history, refined TT-cutoff,
aspiration/fail-high tweaks, an SPSA-tuned singular/extension vector) and an **NPS-optimization
patch** (**+26 to +42 Elo** by time control): a lazy NNUE-mirror apply — the board mirror + threat
computation is deferred until an eval actually needs it, not paid on every legal move — plus a
`-mtune=native` PGO build.

| | |
|---|---|
| **Evaluation** | NNUE, SFNNv13 — own-lineage `nn-rubicon-alea-v1`, loaded at runtime via `EvalFile` (not embedded) |
| **Search** | PVS · LMR (incl. captures) / NMP / futility / razoring / SEE pruning · singular & multi-cut extensions · ProbCut · correction & continuation history · threat-aware ordering |
| **Time management** | Re-tuned soft/hard budget, score-drop and node-based extensions |
| **Parallel** | Lazy SMP (`Threads`) |
| **Endgames** | Syzygy via Fathom (`SyzygyPath`) |

## Triumviratus 5.0

**SFNNv13** NNUE (`Full_Threats + HalfKAv2_hm`) with an SPSA-co-tuned alpha-beta search and Lazy SMP.
First release to ship the own-lineage network **`nn-rubicon-alea-v1`** — the project's second
own-lineage net and the first on Stockfish's threats-aware architecture.

Measured **≈ +50 Elo over 4.2** (internal self-play, 20+0.2).

## Triumviratus 4.2

First release with a **NNUE network trained by the author** — **`rubicon-v1`**
(`HalfKAv2_hm^`, L1 = 2560, trained from scratch on Leela T80 data). No Stockfish network shipped.

The point of 4.2 was **independence**, not strength: the net measured ≈ **−39 Elo** against the
reference Stockfish net, the accepted cost of going own-lineage.

---

## Results

#### 5.1 vs 5.0 — official release gate (2026-07-07)

> [!NOTE]
> `v5.0` vs `v5.1`, **AVX2 build** (the CCRL binary), same network (`nn-rubicon-alea-v1`),
> no score-based adjudication (games decided by mate / 50-move / repetition only).
> The definitive 2000-game version-bump gate.

Book: **UHO 2024** (`UHO_2024_8mvs_big_+080_+099.epd`).

| Time control | Threads | Hash | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|
| 20+0.2 | 1 | 64 MB | 2000 | 59.2% | **+64.7 ± 7.6** | 100.00% |

<sub>v5.1: W 621 · L 253 · D 1126. Pentanomial (v5.1) [0–2]: [1, 87, 483, 401, 28].</sub>

#### 5.1 vs Pawnocchio 1.9.1

`v5.1` (znver5 build), AVX512, 1 thread, 64 MB hash, no score-based adjudication, UHO 2024 book:

| Date | Time control | Opening | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|
| 2026-07-08 | 60+0.6 | UHO_2024_8mvs_big_+080_+099.epd | 2000 | 50.4% | **+2.61 ± 7.48** | 75.26% |
| 2026-07-08 | 20+0.2 | UHO_2024_8mvs_big_+080_+099.epd | 800 | 51.4% | **+9.56 ± 12.89** | 92.71% |
| 2026-07-06 | 10+0.15 | UHO_2024_8mvs_big_+095_+114.epd | 612 | 51.1% | **+7.95 ± 12.35** | 85.78% |

<sub>At long TC (60+0.6, 2000 games) Triumviratus 5.1 and Pawnocchio 1.9.1 were essentially even —
the small edge (LOS 75%) within noise. **6.0** turns that into a clear +42 (see [`README`](README.md)).</sub>

#### 5.1 vs external engines (2026-07-05)

`v5.1` (1 thread), no score-based adjudication, UHO 2024 book.

| Date | Opponent | Time control | Hash | Games | Score (v5.1) | Elo (v5.1) | LOS |
|---|---|---|---|---|---|---|---|
| 2026-07-05 | Pawnocchio 1.9.1 | 20+0.2 | 512 MB | 558 | 48.9% | **-7.5 ± 14.8** | 15.9% |
| 2026-07-05 | Berserk 14 | 25+0.25 | 1024 MB | 322 | 46.3% | **-25.9 ± 18.1** | 0.24% |

<sub>Balanced-book matches draw far more than the unbalanced UHO set — compare sign/LOS across rows,
not the raw Elo number.</sub>

#### Development snapshots

Small-sample matches from development — compare sign and LOS across rows, not the raw Elo number.

| Date | Match | Time control | Book | Games | Score | Elo | LOS |
|---|---|---|---|---|---|---|---|
| 2026-07-04 | 5.1-patched vs 5.1 (all patch improvements) | 12+0.12 | UHO | 500 | 53.8% | **+26.5 ± 15.4** | 99.96% |
| 2026-07-03 | 5.1 vs 5.0 | 10+0.2 | UHO | 250 | 60.8% | **+76.25 ± 29.82** | 100.00% |
| 2026-07-03 | 5.1 vs 5.0 | 30+0.2 | UHO | 600 | 58.7% draws | **+31.5 ± 17.6** | 99.98% |
| — | 5.1 vs 5.0 | 10+0.1 | UHO | 300 | — | **+27** | 99% |
| — | 5.1 vs 5.0 | 3min+1s | UHO | 100 | 54% | **+36** | — |
| — | 5.0 vs 4.2 | 20+0.2 | self-play | — | — | **+50** | — |
| — | 5.0 vs 4.2 | 3min+1s | UHO | 100 | 61.5% | **+81** | — |

Gap widens at longer TC (deeper search rewards the stronger network). Balanced-book matches draw far
more than the unbalanced UHO set.

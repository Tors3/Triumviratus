# Triumviratus — release history

Archive of the releases and of the match results that documented them. The current and previous
release are described in the [`README`](README.md); how each network was trained is in
[`NETWORKS.md`](NETWORKS.md).

| Release | Network | Headline |
|---|---|---|
| **7.0** (current) | `legio-septima` | +21.34 Elo over 6.0 (25+0.25, release binaries) |
| **6.0** | `nn-rubicon-alea-v3` | +52.98 Elo over 5.1 (40+0.4, SPRT passed) |
| **5.1** | `nn-rubicon-alea-v1` | +64.7 Elo over 5.0 (official gate) |
| **5.0** | `nn-rubicon-alea-v1` | first SFNNv13 (threats) net · ≈ +50 Elo over 4.2 |
| **4.2** | `rubicon-v1` | first network trained by the author |

---

## Triumviratus 7.0

Released September 2026. A **network project**: **`legio-septima`** is the first network the project
trains from scratch with base and feature blocks together, instead of grafting a new block onto a
frozen predecessor, on `TRANN2` — Stockfish's SFNNv16 feature set plus the project's own
`PassedPawns` block, 86,992 inputs. Alongside it: the evaluation blend constants retuned on this
network, the correction-history block retuned at the time control the engine is played at,
node-identical speed work with the tuning parameters compiled as constants in release builds (+5%
NPS), and the displayed score recalibrated so that +1.00 means a 50% chance of winning. Full log:
[`DEVELOPMENT_7.0.md`](DEVELOPMENT_7.0.md).

## Triumviratus 6.0

**+52.98 ± 12.25 Elo over 5.1** (40+0.4, 760 games, SPRT `[0,5]` passed). The `TRANN1` network
architecture, the `nn-rubicon-alea-v3` network trained for it, and TMv2 time management. Full log:
[`archive/DEVELOPMENT_6.0.md`](archive/DEVELOPMENT_6.0.md).

## Triumviratus 5.1

Keeps 5.0's own-lineage network **`nn-rubicon-alea-v1`** (SFNNv13,
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

#### 7.0 vs 6.0 — release binaries (2026-09-10)

Release binaries, **AVX2** on both sides, each with its own network, 1 thread, resign/draw
adjudication on. Book: **UHO 2024** (`UHO_2024_8mvs_+085_+094.epd`).

| Time control | Threads | Hash | Games | Score (7.0) | Elo (7.0) | LOS |
|---|---|---|---|---|---|---|
| 25+0.25 | 1 | 256 MB | 3000 | 53.07% | **+21.34 ± 6.66** | 100.00% |
| 5+0.05 | 1 | 64 MB | 9000 | 53.62% | **+25.18 ± 4.28** | 100.00% |

<sub>Pentanomial (7.0) [0–2]: 25+0.25 [14, 284, 726, 456, 20]; 5+0.05 [97, 857, 1997, 1396, 153],
two runs of 3000 and 6000 games pooled.</sub>

#### 7.0 vs external engines (2026-09-10)

Release binaries, 1 thread, 128 MB, the same instruction set on both sides (AVX-512 against Hobbes,
AVX2 against Stormphrax and Cinder), UHO 2024 (`UHO_2024_8mvs_+085_+094.epd`).

| Opponent | Time control | Games | Score (7.0) | Elo (7.0) | LOS |
|---|---|---|---|---|---|
| Stormphrax 8.0.0 | 25+0.25 | 1000 | 57.15% | **+50.03 ± 11.93** | 100.00% |
| Hobbes 3.0 | 25+0.25 | 1000 | 56.75% | **+47.19 ± 12.19** | 100.00% |
| Hobbes 3.0 | 15+0.15 | 1972 | 57.28% | **+50.93 ± 8.99** | 100.00% |
| Cinder 0.6.1 | 25+0.25 | 1000 | 48.60% | **−9.73 ± 11.42** | 4.73% |

<sub>One thread and an unbalanced book widen the gaps: on CCRL 40/15 these engines and 6.0 are within
a few Elo of each other.</sub>

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

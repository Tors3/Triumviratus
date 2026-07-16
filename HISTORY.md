# Triumviratus — release history

Archive of the releases before 5.1 and of the match results that documented them.
The current releases live in the [`README`](README.md); how each network was trained is in
[`NETWORKS.md`](NETWORKS.md).

| Release | Network | Headline |
|---|---|---|
| **5.1** (current) | `nn-rubicon-alea-v1` | see [`README`](README.md) |
| **5.0** | `nn-rubicon-alea-v1` | first SFNNv13 (threats) net · ≈ +50 Elo over 4.2 |
| **4.2** | `rubicon-v1` | first network trained by the author |

---

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

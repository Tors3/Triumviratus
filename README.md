<div align="center">

<img src="logo.png" alt="Triumviratus" width="200">

# Triumviratus

**A strong UCI chess engine in C++** — NNUE evaluation · SPSA-tuned alpha-beta search · Syzygy tablebases

[![License: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](COPYING)
[![C++](https://img.shields.io/badge/language-C%2B%2B-00599C.svg)](source/)
![UCI](https://img.shields.io/badge/protocol-UCI-brightgreen.svg)
![NNUE](https://img.shields.io/badge/evaluation-NNUE-orange.svg)

**by Francesco Torsello**

<sub>in collaboration with Maurizio Platino</sub>

</div>

---

<div align="center">

[Rating](#rating) · [7.1 (in development)](#triumviratus-71--in-development) · [7.0 (current release)](#triumviratus-70--current-release) · [6.0 (previous release)](#triumviratus-60--previous-release) · [Dev log 7.1](DEVELOPMENT_7.1.md) · [Dev log 7.0](DEVELOPMENT_7.0.md) · [Dev log 6.0](archive/DEVELOPMENT_6.0.md) · [Networks](NETWORKS.md) · [Tests](tests/) · [History](HISTORY.md) · [License](#license) · [Credits](#credits)

</div>

---

## Rating

**CCRL Blitz** (2 min + 1 s):

| Version | Rating | Rank | List |
|---|---|---|---|
| **Triumviratus 6.0 64-bit (1 CPU)** | **3749** ±13 | **12–14** | 2026-08-16 |
| Triumviratus 4.2 64-bit (1 CPU) | 3670 ±13 | 51–52 | 2026-08-08 |

**CCRL 40/15** (40 moves in 15 minutes + increment):

| Version | Rating | Rank | Games | List |
|---|---|---|---:|---|
| **Triumviratus 6.0 64-bit (4 CPU)** | **3632** ±17 | **13–14** | 678 | 2026-09-23 |
| Triumviratus 5.1 64-bit (1 CPU) | 3605 | — | — | 2026-07-23 |
| Triumviratus 5.0 64-bit (4 CPU) | 3603 | — | — | 2026-07-16 |
| Triumviratus 5.0 64-bit (1 CPU) | 3570 | — | — | 2026-07-16 |

<sub>Ranks are tie bands shared by the engines inside the interval: read the rating, not the rank. On
40/15, 6.0 sits 18 Elo behind the first entry (Stockfish 19 4 CPU, 3650 ±19). The two lists are not comparable with each other. Both tables are 6.0: 7.0
has not appeared on either list yet.</sub>

---

## Triumviratus 7.1 — in development

7.1 **started from speed**: same network as 7.0, and a first round of changes that make the code
around it faster while leaving the search tree node-for-node identical. Where 7.1 ends up is still
open. Measured against Stockfish 19 with the same compiler and the same network size, the engine
now executes **fewer instructions per node (5,647 vs 6,087)** and has fewer branch mispredictions.
Against the **official 7.0 binary**, measured with paired simultaneous runs on the same CPU cores:
**+8.7% NPS with an identical search tree**, and **+11.5%** with the new transposition table,
which is now being tested in games.

`source/` holds the 7.1 development code; the 7.0 release is the tag `v7.0`. Details, method and
numbers: **[`DEVELOPMENT_7.1.md`](DEVELOPMENT_7.1.md)**.

---

## Triumviratus 7.0 — current release

7.0 is a **network project**: a July 2026 audit put the remaining gap to the strongest engines at
**≈ 25–40 Elo of network**, not of search. Its network, **`legio-septima`**, is the first the project
trains **from scratch with base and feature blocks together**, instead of grafting a new block onto a
frozen predecessor, on a much larger corpus re-labelled with Leela's BT4 network. The architecture
moves to **`TRANN2`**: Stockfish's SFNNv16 feature set plus the **`PassedPawns`** block that no other
engine has.

#### Ahead of 6.0

| TC | hash | depth 7.0 / 6.0 | games | Elo |
|---|---:|---:|---:|---:|
| **25+0.25** | **256 MB** | **15.7 / 15.0** | **3,000** | **+21.34 ± 6.66** |
| 5+0.05 | 64 MB | 11.7 / 11.2 | 9,000 | +25.18 ± 4.28 |

<sub>The release binaries against each other, AVX2 on both sides, each with its own network; 1 thread,
UHO 2024 (+0.85/+0.94), LOS 100% at both points, depths measured from the PGNs. Stage-by-stage
measurements, speed work and training: **[`DEVELOPMENT_7.0.md`](DEVELOPMENT_7.0.md)** ·
**[`NETWORKS.md`](NETWORKS.md)**.</sub>

#### Against other engines

| Opponent | Elo (7.0) | Games | TC · threads |
|---|---:|---:|---|
| Stormphrax 8.0.0 | +50 ± 12 | 1,000 | 25+0.25 · 1 |
| Hobbes 3.0 | +51 ± 9 | 1,972 | 15+0.15 · 1 |
| Caissa 1.26 | +20 | 300 | 1+1 · 4 |
| Cinder 0.6.1 | −10 ± 11 | 1,000 | 25+0.25 · 1 |
| Caissa 2.0 | −27 ± 15 | 300 | 1+1 · 4 |
| Coda 0.9.4 | −38 ± 16 | 300 | 1+1 · 4 |
| pawnocchio 3.0-dev | −38 ± 15 | 300 | 1+1 · 4 |
| PlentyChess 8.0.0 | −63 ± 16 | 300 | 1+1 · 4 |

<sub>1 thread: our runs, release binaries, 128 MB, UHO 2024 (+0.85/+0.94). 4 threads: Maurizio
Platino, i7-8700, Fritz 18, 1024 MB, ponder on, UHO 2024 (+1.10/+1.29); pawnocchio and PlentyChess
met a 7.0 build from a month before the release. Games and details: **[`tests/`](tests/)**. Fast
time controls and unbalanced books widen the gaps compared with a rating list.</sub>

#### Playing style

On Stefan Pohl's **[EAS ratinglist](https://www.sp-cc.de/eas-ratinglist.htm)**, which scores style
rather than strength (computed from sacrifices, short wins and draws in the 120,000 games of the
UHO-Top15 list), **Triumviratus 7.0 is the fourth most aggressive of 16 engines**, behind only Torch and
two Stockfish builds, with the second-highest sacrifice rate after Torch.

| Rank | Engine | EAS-Score | sacs | early sacs | short wins | bad draws |
|---:|---|---:|---:|---:|---:|---:|
| 1 | Torch 4d | 249,549 | 19.09% | 29.50% | 27.54% | 15.15% |
| 2 | Stockfish 19 | 247,367 | 16.84% | 32.16% | 26.91% | 12.95% |
| 3 | Stockfish 260913 | 231,239 | 16.66% | 29.60% | 25.84% | 14.51% |
| **4** | **Triumviratus 7.0** | **175,162** | **17.63%** | **32.57%** | **16.99%** | **19.75%** |
| 5 | PlentyChess 8.0.0 | 172,527 | 12.99% | 29.64% | 21.88% | 20.46% |

<sub>Update of 2026-09-24. Further down: Cinder 6.0, Reckless, Obsidian, Caissa 2.0, Alexandria 9.0,
Stormphrax 8, Integral 8, Quanticade, Coda 0.9.4, Pawnocchio 2.0, Viridithas 20.</sub>

---

## Triumviratus 6.0 — previous release

**+52.98 ± 12.25 Elo over 5.1** (40+0.4, 760 games, LOS 100%, SPRT `[0,5]` passed), and the version
still carrying the project's ratings on both CCRL lists. Three changes carry most of it: the `TRANN1`
network architecture, the `nn-rubicon-alea-v3` network trained for it, and TMv2 time management.
Full log: **[`archive/DEVELOPMENT_6.0.md`](archive/DEVELOPMENT_6.0.md)**.

---

## License

[![License: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](COPYING)

> [!IMPORTANT]
> **GPLv3** — see [`COPYING`](COPYING). Only the **NNUE inference code** is derived from **Stockfish** (the SFNNv16 evaluation machinery in `nnue/`, GPLv3); the search and the rest of the engine are the project's own. Of the two extra NNUE input blocks: **`PassedPawns` is an original feature of this project**, whereas **`PawnPair` implements a pawn-pair input feature that is shared across several open-source engines** (Stormphrax, Viridithas, Pawnocchio — see [Credits](#credits)); its C++ implementation and its trained weights are the project's own, but the feature *design* is not. The shipped network was trained by the project (see [`NETWORKS.md`](NETWORKS.md)). Because the engine incorporates Stockfish's GPL code, **the whole project is distributed under GPLv3**, with Stockfish's copyright notices preserved.

## Credits

### Testing & tuning

**Maurizio Platino** is the project's tester and search-tuner throughout its development. Beyond the
SPSA search-parameter tuning, he probes the engine's real playing strength by running it against
curated **hard positions at long time controls** — the kind of qualitative strength testing that fast
automated match-play cannot reach, and the project's only systematic testing of that sort — and has
generously contributed his hardware for the long tuning and testing runs. Triumviratus would be
materially weaker without his work.

### Derived code

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — SFNNv16 NNUE evaluation (the `nnue/` inference machinery).
- **[BBC](https://github.com/maksimKorzh/chess_programming)** by Maksim Korzh ("Code Monkey King") — the original bitboard/magic-number move generator; the project's earliest (2024) foundation for `attacks.cpp`/`magic.cpp`/`movegen.cpp` and the first search, both since substantially rewritten and extended.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing.

### Open-source engines studied

Ideas for search, move-ordering, time management and pruning were studied from — and in several cases
ported and then **re-tuned against the project's own data and network** — a number of open-source
engines. Credit and thanks to all of them:

- **[Reckless](https://github.com/codedeliveryservice/Reckless)** — quiet move-ordering (offense-square and king-shield-pawn terms), TT prefetch, capture-ordering ideas.
- **[Caissa](https://github.com/Witek902/Caissa)** — node-count move cache, quiescence capture history, moves-left time curve.
- **[Alexandria](https://github.com/PGG106/Alexandria)** — the multiplicative, stateless time-management factors.
- **[Pawnocchio](https://github.com/JonathanHallstrom/pawnocchio)**, **[Viridithas](https://github.com/cosmobobak/viridithas)** — the **`PawnPair` NNUE input feature** (see below).
- **[Berserk](https://github.com/jhonnold/berserk)**, **[Obsidian](https://github.com/gab8192/Obsidian)**, **[Ethereal](https://github.com/AndyGrant/Ethereal)** and **[Stormphrax](https://github.com/Ciekce/Stormphrax)** — assorted search, pruning and ordering refinements.

**NNUE input features — attribution.** The **`PawnPair`** block (pairs of pawns on the same or adjacent files, `4560` inputs) is **not an original idea of this project**. It was **invented by Jonathan Hallström for [Pawnocchio](https://github.com/JonathanHallstrom/pawnocchio)**, from his observation that in a network trained on *all* pawn pairs the ones that mattered were those at most one file apart. It was also used by **Stormphrax** and **Viridithas**, and in July 2026 **Stockfish adopted it too, as `PP_3Wide`**. Our C++ implementation (`nnue/features/pawn_pair.*`) and the trained weights are our own; the idea is his.

The **`PassedPawns`** block (one input per passed pawn, 96 slots) is, by contrast, **an original feature of this project** — designed, implemented and trained here, and present in no other engine we know of.

The open-source computer-chess community is what makes a project like this possible.

<sub>Developed openly and with significant AI assistance.</sub>
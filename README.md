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

[Rating](#rating) · [7.0 (current release)](#triumviratus-70--current-release) · [6.0 (previous release)](#triumviratus-60--previous-release) · [Dev log 6.0](archive/DEVELOPMENT_6.0.md) · [Dev log 7.0](DEVELOPMENT_7.0.md) · [Networks](NETWORKS.md) · [History](HISTORY.md) · [License](#license) · [Credits](#credits)

</div>

---

## Rating

**CCRL Blitz** (2 min + 1 s):

| Version | Rating | Rank | List |
|---|---|---|---|
| **Triumviratus 6.0 64-bit (1 CPU)** | **3749** ±13 | **12–14** | 2026-08-16 |
| Triumviratus 4.2 64-bit (1 CPU) | 3670 ±13 | 51–52 | 2026-08-08 |

<sub>6.0 was 10th at 3754 ±16 on 8 August. The rating moved by 5 points and the interval
tightened as games accumulated, but the rank moved by four — because at this density of the list a
handful of engines gaining a single Elo is enough to reorder it. The 12–14 is a tie band: three
engines share the rating. Read the rating, not the rank.</sub>

**CCRL 40/15** (40 moves in 15 minutes + increment):

| Version | Rating | Rank | Games | List |
|---|---|---|---:|---|
| **Triumviratus 6.0 64-bit (4 CPU)** | **3633** ±23 | **10–12** | 343 | 2026-09-04 |
| Triumviratus 5.1 64-bit (1 CPU) | 3605 | — | — | 2026-07-23 |
| Triumviratus 5.0 64-bit (4 CPU) | 3603 | — | — | 2026-07-16 |
| Triumviratus 5.0 64-bit (1 CPU) | 3570 | — | — | 2026-07-16 |

<sub>6.0 entered this list on 4 September 2026, 16 Elo behind the first entry (Stockfish 18, 3649 ±12)
— a gap **narrower than 6.0's own interval**. Read that interval before the rank: at 343 games it is
±23, the widest near the top of the list and about twice that of the engines around it, which have
700–2,000 games each. The 10–12 is a tie band shared with two other engines. The rating will move as
games accumulate, and it can move either way.</sub>

<sub>The two lists are not comparable: different time control, different pool, different draw rate.</sub>

<sub>Both tables are **6.0**. 7.0 is the current release but has not appeared on either list yet;
its measured advantage over 6.0 is below.</sub>

---

## Triumviratus 7.0 — current release

7.0 is a **network project**, not a search project: a July 2026 audit put the remaining gap to the
strongest engines at **≈ 25–40 Elo of network**, not of search.

Two things change, and the second is the real one:

- **Architecture** moves from **`TRANN1` to `TRANN2`** — the project's own line, now built on the
  SFNNv16 skip structure instead of SFNNv13, still carrying the **`PassedPawns`** block that no other
  engine has. Pawn→pawn threat inputs are dropped because the pawn-pair block already covers them:
  total inputs 87,904 → 86,992.
- **Training method.** The two networks before it grew by grafting a new block onto a frozen
  predecessor, which is cheap but can only *add* what the new block expresses. The new line,
  **`legio-septima`**, trains base and feature blocks **together, from scratch**, in two stages on a
  much larger corpus of public data re-labelled with Leela's BT4 network.

#### Ahead of 6.0

| | date | TC | hash | depth | games | Elo |
|---|---|---|---|---:|---:|---:|
| **release gate — whole engine** | **2026-08-16** | **40+0.4** | **256 MB** | **19.3** | **3,000** | **+33.22 ± 6.14** |
| **release gate — whole engine** | **2026-08-15** | **25+0.25** | **256 MB** | **17.3** | **3,170** | **+28.34 ± 6.19** |
| release gate — whole engine | 2026-08-15 | 5+0.05 | 64 MB | 12.4 | 1,926 | +30.02 ± 8.81 |
| whole engine — network + re-tuned search | 2026-07-31 | 25+0.25 | 64 MB | — | 1,287 | +22.17 ± 9.70 |
| network alone — epoch 799, search frozen | 2026-07-30 | 15+0.15 | 64 MB | — | 1,442 | +23.41 ± 9.22 |

<sub>All against 6.0 with `rubicon-alea-v3`, 1 thread, UHO_4060_v4, AVX-512 PGO builds, LOS 100% on
the gate rows. The rows answer different questions and are **not** additive: the last freezes the
search to isolate the network. The first three are one measurement at three time controls, taken at
the **256 MB** the rating lists use rather than the 64 MB of development runs; **the advantage is
flat across seven plies** (slope +0.42 ± 1.56 Elo per ply), with depths measured from the PGNs rather
than inferred. It supersedes an earlier +37.93 ± 12.25 on 800 games — compatible, but that interval
was twice as wide and predated two changes to the binary. This is one opponent on one book, which is
a different measurement from a rating list. Ahead of 6.0 since epoch 189; stage 2 ended at epoch 799
and a stage-3 annealing tail was run and closed at zero. Per-checkpoint table and recipe:
**[`NETWORKS.md`](NETWORKS.md)** · development log: **[`DEVELOPMENT_7.0.md`](DEVELOPMENT_7.0.md)**.</sub>

#### Against other engines

| Opponent | Result | Games | TC | Depth — ours / theirs |
|---|---|---:|---|---|
| **Hobbes 3.0** (AVX-512) | **+47.24 ± 16.14** (56.76%), LOS 100% | 518 | 40+0.4 | **17.97 / 18.57 ply** |

<sub>1 thread, 256 MB, UHO_4060_v4, adjudication at 40/8/10cp and resignation at 600cp. Depths are
measured from the PGN over 530 games and roughly 30,000 annotated moves per side.</sub>

<sub>🔑 Note the last column. **Hobbes searches about half a ply deeper and still loses by 47 Elo** —
the advantage is in evaluation, not in depth, which is the claim this release is built on. Our depth
is also the more tightly held of the two (per-game quartiles 16.6–18.7 against 16.4–19.8), which is a
property of the time management rather than of the search.</sub>

<sub>The run was checked for a NUMA artefact before being reported — the test machine is dual-socket
with all memory on one node, so half the cores read across the interconnect. Per-game mean depth is
unimodal for both engines, symmetric between colours (0.08 ply for us, 0.27 for Hobbes) and flat
between the first and second halves of the match (drift +0.00 and +0.06 ply). The slower socket is
real but weighs equally on both sides.</sub>

---

## Triumviratus 6.0 — previous release

**+52.98 ± 12.25 Elo over 5.1** (40+0.4, 760 games, LOS 100%, SPRT `[0,5]` passed), and the version
still carrying the project's ratings on both CCRL lists. Three changes carry most of it: the `TRANN1`
network architecture, the `nn-rubicon-alea-v3` network trained for it, and TMv2 time management —
followed by a long tail of search work and a pre-release audit that found seven latent defects.

<sub>Full incremental table, networks, SPSA tunes, corrections and retractions:
**[`archive/DEVELOPMENT_6.0.md`](archive/DEVELOPMENT_6.0.md)** · how each network was trained:
**[`NETWORKS.md`](NETWORKS.md)**. The incremental gains listed there are **not additive** — each is
measured against the state immediately before it, and they overlap.</sub>

---

## License

[![License: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](COPYING)

> [!IMPORTANT]
> **GPLv3** — see [`COPYING`](COPYING). Only the **NNUE inference code** is derived from **Stockfish** (the SFNNv13 evaluation machinery in `nnue/`, GPLv3); the search and the rest of the engine are the project's own. Of the two extra NNUE input blocks: **`PassedPawns` is an original feature of this project**, whereas **`PawnPair` implements a pawn-pair input feature that is shared across several open-source engines** (Stormphrax, Viridithas, Pawnocchio — see [Credits](#credits)); its C++ implementation and its trained weights are the project's own, but the feature *design* is not. The shipped network was trained by the project (see [`NETWORKS.md`](NETWORKS.md)). Because the engine incorporates Stockfish's GPL code, **the whole project is distributed under GPLv3**, with Stockfish's copyright notices preserved.

## Credits

### Testing & tuning

**Maurizio Platino** is the project's tester and search-tuner throughout its development. Beyond the
SPSA search-parameter tuning, he probes the engine's real playing strength by running it against
curated **hard positions at long time controls** — the kind of qualitative strength testing that fast
automated match-play cannot reach, and the project's only systematic testing of that sort — and has
generously contributed his hardware for the long tuning and testing runs. Triumviratus would be
materially weaker without his work.

### Derived code

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — SFNNv13 NNUE evaluation (the `nnue/` inference machinery).
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
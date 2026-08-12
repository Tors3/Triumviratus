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

[Rating](#rating) · [6.0 (current release)](#triumviratus-60--current-release) · [7.0 (in development)](#triumviratus-70--in-development) · [Dev log 6.0](archive/DEVELOPMENT_6.0.md) · [Dev log 7.0](DEVELOPMENT_7.0.md) · [Networks](NETWORKS.md) · [History](HISTORY.md) · [License](#license) · [Credits](#credits)

</div>

---

## Rating

**CCRL Blitz** (2 min + 1 s):

| Version | Rating | Rank | List |
|---|---|---|---|
| **Triumviratus 6.0 64-bit (1 CPU)** | **3754** ±16 | **10** | 2026-08-08 |
| Triumviratus 4.2 64-bit (1 CPU) | 3670 ±13 | 51–52 | 2026-08-08 |

<sub>6.0 is **34 Elo from the top of the list**, and six of the nine engines above it are rated on
eight cores. 996 games, so ±16 is still wide.</sub>

**CCRL 40/15** (40 moves in 15 minutes + increment):

| Version | Rating | List |
|---|---|---|
| **Triumviratus 5.1 64-bit (1 CPU)** | **3605** | 2026-07-23 |
| Triumviratus 5.0 64-bit (4 CPU) | 3603 | 2026-07-16 |
| Triumviratus 5.0 64-bit (1 CPU) | 3570 | 2026-07-16 |

<sub>On this list 6.0 is **not yet rated** and 5.1 is the most recent entry, provisional on few games.
The two lists are not comparable: different time control, different pool, different draw rate.</sub>

<sub>**7.0 is not rated anywhere** — it is not released. On the internal gate it measures
**+37.93 ± 12.25** over 6.0, but that is 800 games against one opponent, which is a different
measurement from a rating list and carries the caveat above about draw rates.</sub>

---

## Triumviratus 6.0 — current release

> [!IMPORTANT]
> **+52.98 ± 12.25 Elo over 5.1** — 40+0.4, 760 games, LOS 100%, nElo +108.57, SPRT `[0,5]` passed
> (LLR 2.95). Release binary vs release binary (AVX-512), each loading its own network
> (6.0 → `rubicon-alea-v3`, 5.1 → `rubicon-alea-v1`), 1 thread, 256 MB, UHO 2024 book.

Three architectural changes carry most of it — a new network **architecture** (`TRANN1`: the SFNNv13
feature set plus two extra input blocks, pawn-pair and passed-pawn), the own-lineage network
**`nn-rubicon-alea-v3`** trained for it, and **TMv2** time management — followed by a long tail of
search work: an unconditional check extension removed, a quiescence search that turned out to be
examining a single move per node, quiet checks removed from quiescence, continuation-history and
killer-move fixes, and a pre-release audit that found seven latent defects.

<sub>The incremental gains listed in [`archive/DEVELOPMENT_6.0.md`](archive/DEVELOPMENT_6.0.md) are **not additive** —
each is measured against the state immediately before it, and they overlap.</sub>

#### Against other engines

| Opponent | Result | Games | TC | Book |
|---|---|---|---|---|
| **Berserk 14** | **+8.7 Elo** (51.25%), 95% CI [+2.2, +15.2] | 600 | 60+1 | Perfect2023 |
| Pawnocchio 1.9.1 | **+41.89 ± 11.08**, LOS 100% | — | 20+0.2 | UHO |

> [!NOTE]
> Networks, SPSA tunes, time management, the full incremental table, the corrections and the
> retractions are all in **[`archive/DEVELOPMENT_6.0.md`](archive/DEVELOPMENT_6.0.md)**, to keep this page short.
> How each network was trained: **[`NETWORKS.md`](NETWORKS.md)**.

---

## Triumviratus 7.0 — in development

> [!IMPORTANT]
> **6.0 is and remains the official release.** The 7.0 network is finished and chosen; there is no
> 7.0 release yet.

7.0 is a **network project**, not a search project. The search is measurably close to exhausted for
this project's effort budget: a full audit in July 2026 found the remaining gap to the strongest
engines is **≈ 25–40 Elo of network**, not of search.

Two things change, and the second one is the real one:

- **Architecture** moves from **`TRANN1` to `TRANN2`** — the project's own network line, now built on
  the SFNNv16 skip structure instead of SFNNv13, and still carrying the **`PassedPawns`** block that
  no other engine has. Pawn→pawn threat inputs are dropped because the pawn-pair block already covers
  them: total inputs 87,904 → 86,992.
- **Training method.** The two networks before it grew by grafting a new input block onto a frozen
  predecessor, which is cheap but can only *add* what the new block expresses. The new line —
  **`legio-septima`** — trains **base and feature blocks together, from scratch**, in two stages on a
  much larger corpus of public data re-labelled with Leela's BT4 network.

#### Ahead of 6.0

| | date | TC | games | Elo |
|---|---|---|---|---|
| whole engine — network + re-tuned search | 2026-07-31 | **25+0.25** | 1287 | **+22.17 ± 9.70** |
| network alone — epoch 799, search frozen | 2026-07-30 | 15+0.15 | 1442 | +23.41 ± 9.22 |

<sub>Both against 6.0 with `rubicon-alea-v3`, 1 thread, UHO_4060_v4, AVX-512 PGO builds. The rows
answer different questions and are **not** additive: the second freezes the search to isolate the
network, and the two agree within their error bars. Ahead of 6.0 since epoch 189. Training is
complete — stage 2 ended at epoch 799, and a stage-3 annealing tail was run and closed at zero.
Per-checkpoint table and recipe: **[`NETWORKS.md`](NETWORKS.md)** · development log:
**[`DEVELOPMENT_7.0.md`](DEVELOPMENT_7.0.md)**.</sub>


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
- **[Berserk](https://github.com/jhonnold/berserk)**, **[Obsidian](https://github.com/gab8192/Obsidian)**, **[Ethereal](https://github.com/AndyGrant/Ethereal)** and **[Stormphrax](https://github.com/Ciekce/Stormphrax)** — assorted search, pruning and ordering refinements (Stormphrax also originated the `PawnPair` feature design).

**NNUE input features — attribution.** The **`PawnPair`** block (pairs of pawns on the same or adjacent files, `4560` inputs) is **not an original idea of this project**. It was **invented by Jonathan Hallström for [Pawnocchio](https://github.com/JonathanHallstrom/pawnocchio)**, from his observation that in a network trained on *all* pawn pairs the ones that mattered were those at most one file apart. It was also used by **Stormphrax** and **Viridithas**, and in July 2026 **Stockfish adopted it too, as `PP_3Wide`**. Our C++ implementation (`nnue/features/pawn_pair.*`) and the trained weights are our own; the idea is his.

The **`PassedPawns`** block (one input per passed pawn, 96 slots) is, by contrast, **an original feature of this project** — designed, implemented and trained here, and present in no other engine we know of.

The open-source computer-chess community is what makes a project like this possible.

<sub>Developed openly and with significant AI assistance.</sub>
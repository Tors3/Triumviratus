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

[Rating](#rating) · [6.0 (current release)](#triumviratus-60--current-release) · [Development](DEVELOPMENT_6.0.md) · [Networks](NETWORKS.md) · [History](HISTORY.md) · [License](#license) · [Credits](#credits)

</div>

---

## Rating

**CCRL 40/15** (40 moves in 15 minutes + increment):

| Version | Rating | List |
|---|---|---|
| **Triumviratus 5.1 64-bit (1 CPU)** | **3605** | 2026-07-23 |
| Triumviratus 5.0 64-bit (4 CPU) | 3603 | 2026-07-16 |
| Triumviratus 5.0 64-bit (1 CPU) | 3570 | 2026-07-16 |

<sub>5.1 on a single CPU has just entered the list at **3605**, on few games so far — provisional, and
placed just below the 24th–25th positions of the full ranking. **6.0 is not yet rated**; on the
internal gate it is a further **+53 Elo** over 5.1, but a rating list is a different measurement and
this section will be updated when CCRL publishes one.</sub>

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

<sub>An earlier gate measured **+33.18 ± 9.86** at 20+0.2 on 2026-07-16. It predates eight further
changes and is superseded by the figure above, not contradicted by it — the two are separate
measurements at different time controls and hash sizes, and the incremental gains listed in
[`DEVELOPMENT_6.0.md`](DEVELOPMENT_6.0.md) are **not additive** (each is measured against the state
immediately before it, and they overlap).</sub>

#### Against other engines

| Opponent | Result | Games | TC | Book |
|---|---|---|---|---|
| **Berserk 14** | **+8.7 Elo** (51.25%), 95% CI [+2.2, +15.2] | 600 | 60+1 | Perfect2023 |
| Pawnocchio 1.9.1 | **+41.89 ± 11.08**, LOS 100% | — | 20+0.2 | UHO |

<sub>The Berserk match was played from **Perfect2023**, a deep balanced book, which is why **94.5% of
the games were drawn** (24 wins, 567 draws, 9 losses) and why the margin is small even though the
confidence interval excludes zero. A book like that compresses differences between strong engines by
design; the same engines on an unbalanced book would show a wider gap in either direction. Recorded
as measured rather than re-run on a friendlier book.</sub>

> [!NOTE]
> Networks, SPSA tunes, time management, the full incremental table, the corrections and the
> retractions are all in **[`DEVELOPMENT_6.0.md`](DEVELOPMENT_6.0.md)**, to keep this page short.
> How each network was trained: **[`NETWORKS.md`](NETWORKS.md)**.


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

**NNUE input features — attribution.** The **`PawnPair`** block (a pairwise pawn-square co-occurrence feature, `96·95/2 = 4560` inputs) is **not an original idea of this project**: it is a design shared by **Stormphrax**, **Viridithas** and **Pawnocchio**. Our C++ implementation (`nnue/features/pawn_pair.*`) and the trained network weights are our own, but the feature itself belongs to those engines and is credited to them explicitly. The **`PassedPawns`** block (one input per passed pawn, 96 slots) is, by contrast, **an original feature of this project** — designed, implemented and trained here, and not drawn from another engine.

The open-source computer-chess community is what makes a project like this possible.

<sub>Developed openly and with significant AI assistance.</sub>
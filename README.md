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

[Rating](#rating) · [6.0 (prerelease)](#triumviratus-60-prerelease) · [Results](#results) · [History](HISTORY.md) · [License](#license) · [Credits](#credits)

</div>

---

## Rating

**CCRL 40/15** (40 moves in 15 minutes + increment), list of 2026-07-16:

| Rank | Version | Rating |
|---|---|---|
| #29 | Triumviratus 5.0 64-bit (4 CPU) | 3603 |
| #47 | Triumviratus 5.0 64-bit (1 CPU) | 3570 |

<sub>5.1 and 6.0 are not yet CCRL-rated; this section will be updated when they are.</sub>

---

## Triumviratus 6.0 (prerelease)

> [!NOTE]
> All details regarding the 6.0 prerelease (including new networks, SPSA tunes, time management, incremental gains, and SPRT results) have been moved to **[`DEVELOPMENT_6.0.md`](DEVELOPMENT_6.0.md)** to keep this page clean.


## License

[![License: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](COPYING)

> [!IMPORTANT]
> **GPLv3** — see [`COPYING`](COPYING). Only the **NNUE inference code** is derived from **Stockfish** (the SFNNv13 evaluation machinery in `nnue/`, GPLv3); the search, the rest of the engine, the two extra feature blocks (`PawnPair`, `PassedPawns`) and the shipped network are the project's own (see [`NETWORKS.md`](NETWORKS.md)). Because the engine incorporates that GPL code, **the whole project is distributed under GPLv3**, with Stockfish's copyright notices preserved.

## Credits

- **[Stockfish](https://github.com/official-stockfish/Stockfish)** (GPLv3) — SFNNv13 NNUE evaluation.
- **[BBC](https://github.com/maksimKorzh/chess_programming)** by Maksim Korzh ("Code Monkey King") — original bitboard/magic-number move generator; the project's earliest (2024) foundation for `attacks.cpp`/`magic.cpp`/`movegen.cpp` and the original search, both since substantially rewritten and extended.
- **[Fathom](https://github.com/jdart1/Fathom)** (MIT) — Syzygy tablebase probing.
- **[Berserk](https://github.com/jhonnold/berserk)** and **[Pawnocchio](https://github.com/JonathanHallstrom/pawnocchio)** — studied for search/move-ordering ideas informing some of the engine's own implementations. The *PawnPair* network features introduced in Triumviratus 6.0 were also inspired by Pawnocchio.
- Thanks to **Maurizio Platino** for the SPSA search-tuning and for extensive testing throughout the project,
  generously contributing his hardware.

<sub>Developed openly and with significant AI assistance.</sub>
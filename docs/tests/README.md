# Tests

Results from outside the project's own SPRT pipeline: the project's own matches against other engines, and matches
and test suites run by other testers.
For Maurizio Platino's matches, the complete game records (PGN) and the GUI crosstables are in
**[Tors3/Triumviratus-Testing](https://github.com/Tors3/Triumviratus-Testing)**.

## Maurizio Platino — 8.1 prerelease against ShashChess 42

| Date | Opponent | Triumviratus build | Games | +W =D −L | Score | Elo (Triumviratus) |
|---|---|---|---:|---|---:|---:|
| 2026-11-07 | **ShashChess 42** | 8.1 prerelease (2026-11-04) | 400 | +5 =391 −4 | **50.1%** | **+1 ± 5** |

<sub>TorsGUI · 1 min + 1 s · 1 thread and 256 MB hash per engine · both engines with default settings and their
universal executables · openings `AVT2026d.pgn`, each played with both colours · 296 of the 400 games ended by
adjudication. ± is the 95% interval as reported by TorsGUI. The summary of the match (no games) is in
Triumviratus-Testing.</sub>

The two engines are level: 391 of the 400 games were drawn, and the nine decisive games split five to four.

## 8.0 release: the README summary (archived)

The summary of the 8.0 release as it stood in the main README until the 8.1 release. 1 thread per engine; ± is the
95% interval on complete opening pairs (pentanomial).

| opponent | games | conditions | +W =D −L | Elo (Triumviratus) |
|---|---:|---|---|---:|
| **Triumviratus 7.0** | 4,000 | 5+0.05, UHO 2024 | +1773 =1864 −363 | **+128.0 ± 5.6** |
| **Triumviratus 7.0** | 400 (stopped) | 20+0.2, UHO 2024 | +177 =189 −34 | **+129.9 ± 15.7** |
| **Stockfish 18** | 600 | 133+1 (CCRL Blitz), TopGM openings | +20 =568 −12 | **+4.6 ± 5.0** |
| **Stockfish 18** | 2,000 | 40+0.4, UHO 2024 | +530 =1082 −388 | **+24.7 ± 7.2** |

Games of the UHO match in [`games/`](games/).

| suite | tester | result |
|---|---|---|
| IQ4 (183 positions, 1 s, 1 thread) | Maurizio Platino | **155 / 183**, second of six |
| IQ4 (183 positions, 1 s) | Mark Tang | 145 / 183, highest among the engines tested |
| ENET 2026 (110 positions) | Maurizio Platino | 89 / 110 |

Other engines: Andres (EveAnn)'s gauntlet of the 11 October pre-release scored **62.8%, +91.2 ± 9.4** against fifteen
engines (1,500 games); Maurizio Platino measured **+94 ± 16** against Stormphrax 8.0.0 and **+25.5 ± 14.6** against
Reckless 0.10.0-dev. The details of every row are in the sections below.

## The project's own matches — 8.0 against Stockfish 18

The release 8.0 against Stockfish 18 with an unbalanced book, played on the project's test machine (2× Xeon Gold
6138) in TorsGUI. All 2,000 games are in [`games/Triumviratus_8.0_vs_Stockfish_18_UHO_40+0.4.zip`](games/Triumviratus_8.0_vs_Stockfish_18_UHO_40+0.4.zip).

| Date | Opponent | Games | +W =D −L (Triumviratus) | Score | Pairs (0, ½, 1, 1½, 2) | Elo (Triumviratus) |
|---|---|---:|---|---:|---|---:|
| 2026-10-14/15 | **Stockfish 18** | 2,000 | +530 =1,082 −388 | **53.6%** | 1, 152, 554, 290, 3 | **+24.7 ± 7.2** |

<sub>40 s + 0.4 s per game · 1 thread and 256 MB hash per engine · ponder off · Syzygy tablebases · openings
`UHO_2024_8mvs_+085_+094` (Stefan Pohl, SPCC), 1,000 openings each played with both colours · draw adjudication
from move 35 (8 moves within 10 cp), resignation at 600 cp for 4 moves on both sides. Three games were lost on
time, two by Triumviratus and one by Stockfish. ± is the 95% interval on the opening pairs (pentanomial).</sub>

On balanced openings at CCRL Blitz time control the same two engines are level (+4.6 ± 5.0 over 600 games,
[README](../../README.md#strength)); the unbalanced book, where each side has to convert an advantage or hold a worse
position, separates them.

## Maurizio Platino — 7.0 against recent engines

Bullet matches against engines released in the last months, played by **Maurizio Platino** on his
own machine.

| Date | Opponent | Triumviratus build | Games | Score | Elo (Triumviratus) |
|---|---|---|---:|---:|---:|
| 2026-08-14 | pawnocchio 3.0-dev | 7.0 dev (2026-08-14) | 300 | 44.5% | −38 ± 15 |
| 2026-08-16 | PlentyChess 8.0.0 | 7.0 dev (2026-08-14) | 300 | 41.0% | −63 ± 16 |
| 2026-09-11 | Coda 0.9.4 | 7.0 (2026-09-10) | 300 | 44.5% | −38 ± 16 |
| 2026-09-14 | Caissa 1.26 | 7.0 (2026-09-10) | 300 | 52.8% | +20 |
| 2026-09-22 | Caissa 2.0 | 7.0 (2026-09-10) | 300 | 46.2% | −27 ± 15 |

<sub>Intel Core i7-8700 @ 3.20 GHz · Fritz 18 · 4 threads and 1024 MB hash per engine · ponder on ·
1 min + 1 s · openings `UHO_2024_8mvs_big_+110_+129.pgn` (Stefan Pohl, SPCC), 150 per match, each
played with both colours. ± is the 95% interval on the opening pairs (pentanomial). The Caissa 1.26
match has the crosstable only, without the games, hence no interval.</sub>

**What it says.** 7.0 is ahead of Caissa 1.26 and 25–65 Elo behind the newest releases: Caissa 2.0,
Coda 0.9.4, pawnocchio 3.0 and PlentyChess 8. These are the engines the 8.0 audit compares against
([`DEVELOPMENT_8.0.md`](../development/archive/DEVELOPMENT_8.0.md)). Bullet with an unbalanced book widens the gaps
compared with a rating list, and the two August matches used a development build from a month
before the release.

## Maurizio Platino — 8.0 prerelease

| Date | Opponent | Triumviratus build | Games | +W =D −L | Score | Elo (Triumviratus) |
|---|---|---|---:|---|---:|---:|
| 2026-10-05 | Stormphrax 8.0.0 | 8.0 prerelease (2026-10-04) | 300 | +149 =81 −70 | **63.2%** | **+94 ± 16** |
| 2026-10-07 | Reckless 0.10.0-dev | 8.0 prerelease (2026-10-04) | 300 | +132 =58 −110 | **53.7%** | **+25.5 ± 14.6** |
| 2026-10-09 | **Stockfish 19** | 8.0 prerelease (2026-10-08), 3 threads | 300 | +137 =20 −143 | **49.0%** | **−6.9 ± 9.6** |

<sub>Same machine, GUI, hash, time control and openings as the 7.0 matches above, but 1 thread per engine (3 against
Stockfish 19) and ponder off. ± is the 95% interval on the 150 opening pairs (pentanomial). The 4 October prerelease is the first
build with the restructured search; the earlier 8.0 development matches against Caissa 2.0 predate it and are
kept, with their games, in Triumviratus-Testing.</sub>

## Maurizio Platino — ENET 2026 test suite

**ENET 2026** by Eduard Nemeth: 110 hard positions. Intel Core i7-8700 @ 3.20 GHz, Fritz 18, 8 GB hash.

| Triumviratus build | Solved |
|---|---:|
| 4.1 | 37 / 110 |
| 4.2 | 37 / 110 |
| 6.0 | 77 / 110 |
| 6.0, later build | 82 / 110 |
| 6.0 dev (2026-07-24) | 85 / 110 |
| 6.0 dev (2026-07-25) | 81 / 110 |
| 7.0 dev (2026-07-31) | 79 / 110 |
| 8.0 dev (2026-10-01) | 81 / 110 |
| **8.0 dev (2026-10-04)** | **89 / 110** |

The same suite on the same machine, other engines:

| Engine | Solved |
|---|---:|
| Stockfish 17.1 | 94 / 110 |
| Stockfish 19 | 91 / 110 |
| Stockfish dev (2026-09-13) | 91 / 110 |
| ShashChess 41 | 91 / 110 |
| Theoria 0.2 | 91 / 110 |
| Berserk 14 | 90 / 110 |
| **Triumviratus 8.0 dev (2026-10-04)** | **89 / 110** |
| Stockfish 18 | 82 / 110 |

From 6.0 to the 1 October build every version solved about three quarters of the suite. The build
of 4 October, the first with the restructured search, solves **89**: the best Triumviratus so far,
eight more than three days earlier, and within two positions of Stockfish 19.

## Maurizio Platino — IQ4 test suite

**IQ4**: 183 hard positions. 1 second per position, 1 thread, 64 MB hash, on Maurizio Platino's machine.

| Engine | Solved |
|---|---:|
| Leptir TI MPV | 165 / 183 |
| **Triumviratus 8.0 (pre-release of 11 October 2026)** | **155 / 183** |
| Cinder 0.6.1 | 149 / 183 |
| Alexandria 9.1.0 | 147 / 183 |
| Corchess 5 (2026-10-01) | 145 / 183 |
| Stockfish 19 | 142 / 183 |

Triumviratus is second of the six engines and solves 13 positions more than Stockfish 19 under the same conditions.

## Mark Tang — 8.0 prerelease

Tests run by **Mark Tang** on the 8.0 prerelease builds of 5 and 8 October 2026.

### Tactics: the IQ4 suite

**IQ4** is a set of 183 hard tactical positions from the IQ test series (Arasan's collection).

| Engine | Solved | |
|---|---:|---:|
| **Triumviratus 8.0 (prerelease)** | **145 / 183** | **79.2%** |
| Quanticade | 137 / 183 | 74.9% |
| Stockfish | 134 / 183 | 73.2% |

<sub>1 second per position · 1 thread · 64 MB hash · one engine process at a time. Triumviratus
had the highest score among the engines tested.</sub>

A second run on the build of 8 October solved **143 / 183** (78.1%).

### Match against Stockfish dev

| Date | Opponent | Games | Result | Score | TC |
|---|---|---:|---:|---:|---|
| 2026-10-05 | Stockfish (development version) | 30 | +3 =17 −10 | 38.3% | 2 min + 1 s |
| 2026-10-08 | Stockfish (development version) | 30 | +5 =15 −10 | 41.7% | 103 s + 1 s |

<sub>About −83 Elo for the first match (95% interval about −170 to −5) and −58 for the second, on the build of
8 October (about −152 to +28). With 30 games each, both intervals are wide.</sub>

## Andres (EveAnn) — 8.0 pre-release gauntlet

A gauntlet run by **Andres (EveAnn)** on the 8.0 pre-release of 11 October 2026 against fifteen engines, 100 games
each.

| Opponent | Games | +W =D −L (Triumviratus) | Score | Elo (Triumviratus) |
|---|---:|---|---:|---:|
| **Stockfish 19** | 100 | +8 =84 −8 | 50.0% | **+0.0 ± 27.3** |
| Stockfish 18 | 100 | +9 =85 −6 | 51.5% | +10.4 ± 26.4 |
| Reckless 0.9.0 | 100 | +13 =82 −5 | 54.0% | +27.9 ± 28.6 |
| Torch v4d | 100 | +17 =78 −5 | 56.0% | +41.9 ± 31.4 |
| Plentychess 8.0.0 | 100 | +23 =71 −6 | 58.5% | +59.6 ± 36.0 |
| Obsidian 16.0 | 100 | +24 =72 −4 | 60.0% | +70.4 ± 34.9 |
| pawnocchio 2.0 | 100 | +29 =69 −2 | 63.5% | +96.2 ± 35.9 |
| Viridithas 20.0.0 | 100 | +29 =70 −1 | 64.0% | +100.0 ± 34.9 |
| Alexandria 9.0 | 100 | +32 =67 −1 | 65.5% | +111.4 ± 36.6 |
| Hobbes 3.0 | 100 | +33 =66 −1 | 66.0% | +115.2 ± 37.2 |
| Caissa 1.25 | 100 | +34 =66 −0 | 67.0% | +123.0 ± 36.7 |
| Viridithas 19.0.1 | 100 | +39 =57 −4 | 67.5% | +127.0 ± 43.3 |
| Stormphrax 8.0.0 | 100 | +40 =60 −0 | 70.0% | +147.2 ± 40.0 |
| Raphael 4.2.0 | 100 | +45 =53 −2 | 71.5% | +159.8 ± 45.0 |
| Heimdall 1.5.0 | 100 | +55 =45 −0 | 77.5% | +214.8 ± 49.2 |
| **All fifteen opponents** | **1,500** | **+430 =1,025 −45** | **62.8%** | **+91.2 ± 9.4** |

<sub>2 min + 0.4 s per game, book avt-fringe-2026, 68% draws overall. One of Hobbes' losses was on time. Elo and
intervals as reported by the tester's tool.</sub>

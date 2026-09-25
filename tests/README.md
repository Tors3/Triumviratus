# Tests

Summaries of match results from outside the project's own SPRT pipeline. The complete game
records (PGN) and the GUI crosstables are in
**[Tors3/Triumviratus-Testing](https://github.com/Tors3/Triumviratus-Testing)**.

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
Coda 0.9.4, pawnocchio 3.0 and PlentyChess 8. These are the engines the 7.1 audit compares against
([`DEVELOPMENT_7.1.md`](../DEVELOPMENT_7.1.md)). Bullet with an unbalanced book widens the gaps
compared with a rating list, and the two August matches used a development build from a month
before the release.

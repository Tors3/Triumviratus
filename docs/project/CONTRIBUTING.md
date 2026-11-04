# Contributing to Triumviratus

Two branches: **`main`** holds the development of 8.1, merged from **`dev`**, the working branch. Released versions
are on the [Releases](https://github.com/Tors3/Triumviratus/releases) page (8.0: tag `v8.0`). Pull requests go to
`dev`; every pull request is reviewed and merged by hand.

## Code contributions

1. Fork the repository and branch from `dev`. Changes that keep the search identical can go together in one pull
   request, listed in its description; each change to the search goes in its own pull request.
2. Build and run the bench (`source/Makefile`, then `bench` in the engine). Put the result in the last commit
   message as `Bench: <nodes>`. The check on the pull request builds the engine and compares the two numbers.
3. Say which kind of change it is:
   - **Same search** (speed, refactoring, cleanup): the bench must not change. Give the speed gain you measured and
     on which CPU. We repeat the measurement on our machines and run a short non-regression test.
   - **Different search** (pruning, reductions, time management, evaluation): the bench changes. We test it with an
     SPRT on our machines, and it is merged only if it passes.
4. Say whether the code is your own, ported from another engine (which one, and under which licence), or generated
   with an AI tool.

Contributions are accepted under the project's licence, GPLv3. Every merged contribution is credited in the README
under the name the contributor chooses.

## Testing

Matches against other engines and test-suite runs are welcome. Open an issue with the **Test report** template, or
post in the project's Discord. A useful report gives:

- the Triumviratus build (version, file name or SHA-256) and the opponents' versions;
- the hardware, threads and hash per engine, and the time control or time per position;
- the opening book and the number of games, or the test suite;
- the result, and the PGN file or the list of failed positions when possible.

Results from testers are published in [`tests/`](../tests/README.md) with their names, only with their agreement.

## Contributions

Merged contributions, newest first.

- **Houze Guo**, branch `perf/avx512-eval-speedup` (5 November 2026), speed at an identical search tree: in the AVX-512
  builds the activations of the last two layers of the network stay in registers (no store-to-load stall), the
  second layer runs two independent accumulator chains, the one-time pawn-cache initialisation moves out of the
  evaluation (no register spills), empty threat masks return at once; in every build the refresh skips the write-back
  of unchanged king-cache entries, the passed pawns are found with bitboard shifts, and pieces that cannot be threat
  targets are filtered out early. Bench unchanged, `nnperft` clean. Measured with hardware counters: -0.23% / -0.20%
  cycles per node (middlegame / endgames) on Skylake-SP in the AVX-512 build, level in AVX2; on Zen 4 -2.3% in the
  AVX-512 VNNI build and level in AVX-512 with VBMI2 (middlegame; the laptop's endgame runs were too noisy).
- **Houze Guo**, AVX2 / AVX-VNNI evaluation speedups (3 November 2026), at an identical search tree: the pawn cache in
  the AVX2 refresh of the accumulators, the sums of the first layer written to memory only when the C2 terms read them,
  and the second perspective's rows gathered after the first one's update. 1.7% to 3.2% fewer instructions per node
  in the AVX2 and AVX-VNNI builds, measured on four processors (Skylake-SP, Zen 4, Raptor Lake-U, and Raptor Cove by
  A Tin Man); the AVX-512 paths unchanged.

- **A Tin Man**, bug report (28 October 2026): in a CCRL Blitz gauntlet game against Halogen 16.0.0, Triumviratus 8.0
  played 37.Re4 at depth 26 with +1.27, and 37...Nxe4 wins the rook. The investigation traced it to a false hash hit:
  the Zobrist keys, inherited from the BBC base, were built from 16-bit pieces of a 32-bit xorshift generator, so every
  64-bit key carried only 32 bits of entropy (2,300-3,600 false hash hits per search at 1.5M nodes with a 256 MB hash).
  With a constructed key collision the 8.0 binary repeats the game's move, score and principal variation. The keys of
  the 8.1 development line now come from a 64-bit generator (no false hits in the same test), with the position as a
  regression test; non-regression SPRT at 5+0.05, Hash 64: +4.57 ± 3.56 over 9,660 games.
- **Bastiii**, code review of the 8.1 development branch (17 October 2026): a race between threads in the
  transposition table store (the entry is checked again before its move and evaluation are reused), a dead write of
  the network-scale cache in the evaluation, the `hashfull` count limited to the current search, the network name in
  the `Makefile` and an optional set of linker flags, the default of `gen_cooc_perm.py`, and a static exchange
  evaluation that counts the gain of a promotion (adopted after an SPRT on endgames, +1.78 ± 1.76 over 14,874 games). All at an identical search tree except the
  promotion gain; a second suggestion on the exchange evaluation was measured with hardware counters and showed no gain.
- **A Tin Man**, pull request 6 (15 October 2026), speed at an identical search tree: a legal move generator ported
  from his own engine, [Zoomies](https://github.com/CodeAdopter/Zoomies), with the move order of ours; an AVX-VNNI variant of the universal executable for Intel
  processors without AVX-512; the removal of the per-thread evaluation cache; a prefetch of the next move's hash entry
  on Intel processors; and two smaller vector changes (rank counting in AVX2, threat rows without `vpmovsxbw`).


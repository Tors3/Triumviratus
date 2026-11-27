<div align="center">

<picture><source media="(prefers-color-scheme: dark)" srcset="../images/logo_mark_dark.svg"><img src="../images/logo_mark.svg" alt="Triumviratus" width="130"></picture>

# Triumviratus 9.0: development log

**In progress.** Speed of 9.0 against 8.1 first; the rest of the 9.0 work follows here as it is measured

**by Francesco Torsello**

<sub>in collaboration with Maurizio Platino</sub>

</div>

---

<div align="center">

[Speed against 8.1](#1-speed-against-81) · [How it is measured](#11-how-it-is-measured) ·
[Results](#12-results) · [Speed changes of 22 November](#13-speed-changes-of-22-november-2026)

</div>

---

## 1. Speed against 8.1

### 1.1 How it is measured

**Hardware counters.** Windows `xperf` reads the performance counters of the CPU for the search thread of each
engine: instructions retired, cycles, branch mispredictions. Each engine searches the same positions to the same
fixed node count, so the result is a cost **per node**. Instructions per node do not depend on the load of the
machine; cycles per node do, a little, and that part is controlled by an **A/A pair**: two identical copies of one
executable in the same capture. Their difference is the noise floor of that capture.

**Workload.** 30 middlegame positions and 30 endgame positions, 400,000 nodes each, one search thread, Hash 16 MB,
the engine pinned to one core. Every round runs all engines once, in alternating order.

**Machines.**

| machine | CPU | variant | core | large pages | rounds |
|---|---|---|---|---|---:|
| Xeon | Intel Xeon Gold 6138 (Skylake-SP), 2 sockets | `avx512` | socket 0, cpu 2 (memory on node 0) | on | 3 |
| Zen 4 | AMD Ryzen 7 8845HS, laptop on AC power, Performance mode | `avx512icl` | cpu 2 | as the account allows | 8 |

**Engines.** Each with its own embedded network.

| engine | network | what it is |
|---|---|---|
| 8.1 | Consilium Plebis | the official `Triumviratus_8.1_windows_universal.exe` (sha256 `62d5604c…`), which picks the variant of the CPU |
| 9.0 | Lex Hortensia | branch `dev`, commit `c3874c9`, with the Lex Hortensia network embedded |
| 9.0, copy | Lex Hortensia | the A/A control |

The 9.0 build uses the release chain (`source/universal/build_variants.ps1`): deterministic PGO on the CPU that runs
the build, LLVM 23.1.4, MSVC 14.44.35207, Windows SDK 10.0.26100.0. It is checked with `bench` before it is measured:
309144 nodes.

### 1.2 Results

Measured on 22 November 2026.

| cycles per node | Xeon middlegame | Xeon endgame | Zen 4 middlegame | Zen 4 endgame |
|---|---:|---:|---:|---:|
| 8.1 | 3,694 | 3,505 | 3,466 | 3,321 |
| **9.0** | **3,591 (−2.8%)** | **3,394 (−3.2%)** | **3,329 (−4.0%)** | **3,209 (−3.4%)** |
| 9.0, copy (A/A) | 3,592 (−2.8%) | 3,399 (−3.0%) | 3,326 (−4.1%) | 3,185 (−4.1%) |

| per node | Xeon middlegame | Xeon endgame | Zen 4 middlegame | Zen 4 endgame |
|---|---:|---:|---:|---:|
| instructions, 8.1 | 5,492 | 5,154 | 5,121 | 4,799 |
| instructions, 9.0 | 5,729 (+4.3%) | 5,335 (+3.5%) | 5,256 (+2.6%) | 4,880 (+1.7%) |
| branch mispredictions, 8.1 | 28.4 | 29.1 | 20.8 | 21.2 |
| branch mispredictions, 9.0 | 26.9 (−5.2%) | 28.0 (−3.8%) | 19.4 (−6.5%) | 20.4 (−3.6%) |

**9.0 needs 3 to 4% fewer cycles per node than 8.1 on both machines**, although it executes 2 to 4% more
instructions: it mispredicts fewer branches and retires more instructions per cycle (Xeon middlegame: IPC 1.49 for
8.1, 1.60 for 9.0). The A/A pair agrees within 0.2% on the Xeon and within 0.8% on the Zen 4.

The extra instructions come in part from the network. The first layer skips groups of four inputs that are all zero;
counted over `bench` with a diagnostic build (`-DCN_PROF`), Lex Hortensia leaves **124.0 of 256 groups non-zero per
evaluation**, against 96.9 for its distilled starting point (the network the dev branch embedded before the training
ended). The forward pass costs 11% more per evaluation with the trained network, about 0.8% of the cost of a node.

### 1.3 Speed changes of 22 November 2026

Three changes were kept; none alters the search tree (same `bench`). Measured with the same method, each against
the build without it:

| change | Xeon middlegame | Xeon endgame | Zen 4 middlegame | Zen 4 endgame |
|---|---:|---:|---:|---:|
| tile loops of the accumulator update fully unrolled, tile functions always inlined | −0.18% | +0.03% | −0.12% | −0.10% |
| correction history read before the hash probe in the main search | −0.60% | −0.76% | −0.54% | −1.19% |
| the two together | −0.57% | −0.54% | −0.30% (1) | −1.00% |
| the same order in quiescence | +0.14% | +0.21% | −1.32% | −1.25% |

(1) Against the A/A copy: in that capture the first copy of the reference build ran 1.0% slower than the second.

Two more changes help one machine and cost the other, so each is on only in the variant where it was measured to
help. The table gives both machines; the cell of the variant that keeps the change is in bold.

| change | variant where on | Xeon middlegame | Xeon endgame | Zen 4 middlegame | Zen 4 endgame |
|---|---|---:|---:|---:|---:|
| the make saves the board before the move, the accumulator catch-up starts from it | `avx512` | **−0.27%** | **−0.47%** | +0.34% | +1.03% |
| prefetch of the hash entry of the next move | `avx512icl` on AMD | +0.9% | +0.7% | **−1.2%** | **−1.6%** |

The Xeon figures come from 6 rounds, the Zen 4 figures from 12; the A/A pairs agree within 0.3%. These two changes
came after the measurement of section 1.2, so they are not in its 9.0 numbers. The Intel processors that run the
`avx512icl` variant (Ice Lake, Sapphire Rapids) are not measured and keep the prefetch off.

Two more were measured and dropped: accumulator tiles of 8 registers instead of 16 (slower on the Xeon, and on the
Zen 4 once the changes above were in), and a bitset of the non-zero groups in the AVX2 forward pass (+2.7% cycles).

## 2. Search changes

### 2.1 Two search levers of 24 November 2026

Two changes to the search went in together, each tested alone first, none negative.

- **In check every legal reply is searched.** The move gates no longer skip evasions. The capture gate read there a
  static evaluation copied from two plies back.
- **Reverse futility goes up to 4 plies past its depth limit.** There its cut must be confirmed by a null-window
  quiescence search at beta. Not in mate hunts.

Bundle SPRT against the tree before: +1.17 ± 1.89 Elo over 32,052 games (8+0.08, UHO openings, [−1, 2]). Bench
309144 → 271761.

**Still to check at a long time control.** The second change only acts at remaining depths 19 to 22. At 8+0.08 such
nodes are rare (the bench is the same with and without it), so the short SPRT measured mostly the first change. An
SPRT at 40+0.4 of the second change alone, with the first on both sides, decides whether it stays.

### 2.2 The correction from the opponent's plan (`CorrContOppW`), 24 November 2026

The correction of the static evaluation reads continuation tables keyed by pairs of moves. The ones in use pair one of
our moves with the opponent's answer. `CorrContOppW`, the author's idea of 6 October 2026, adds the pair of the
opponent's two last moves (three plies back and one ply back), a fragment of the opponent's plan. On 8.0 it lost
(−5.2 ± 12.7 over 742 games at weight 4011, −4.8 ± 8.7 over 1,806 at 8000) and stayed off. With Lex Hortensia it holds.

SPRT at weight 8000 against off: +2.70 ± 2.15 Elo over 24,698 games (8+0.08, UHO openings, [−1, 2], H1). Bench
271761 → 302771. The weight stays a parameter, for the SPSA of the correction block that follows the single tests of
its two extensions (the opponent's plan over three moves, our own two last moves) and of the long-term plan in the
reduction, all written and off.

### 2.3 Nine late-move-reduction terms, 25 November 2026

Nine terms of the late-move reduction, written on 18 November 2026 (9.0 plan, groups S3, S7 and S8) and tuned
together by one SPSA (up to iteration ~1328 of 1500; the vector is the mean of iterations 1261-1309), in 1/1024 ply,
positive meaning less reduction:

| term | where it acts | value |
|---|---|---:|
| `LmrImpSlope` | the improvement of the static evaluation (times improvement / 64) | 74 |
| `LmrTtNone` | no hash value at the node | 197 |
| `LmrTtDeep` | a hash entry at least as deep as the node | 200 |
| `LmrFewMoves` | quiet moves when the node has few moves (times max(0, 30 − moves) / 16) | 32 |
| `LmrCheckLess` | checking moves | −68 |
| `LmrCutEarly` | cut nodes, move number up to 5 | 296 |
| `LmrCutCuts` | cut nodes whose child had three or more cutoffs | 80 |
| `LmrCutEnd` | cut nodes in the endgame band | −47 |
| `RootAltLmr` | root moves 2 and 3 of the previous iteration's order | −481 |

SPRT of the vector against all terms at zero, in four parts pooled (10+0.1, UHO openings, [−1, 2]): +1.40 ± 1.93 Elo
over 30,268 games, LLR +1.76 of 2.94. The author accepted it at 30,000 games without the LLR closing. Bench 302771 →
168444 (with the nine terms at zero the bench is 302771 again). The terms stay parameters for later tuning.

### 2.4 HistGrad removed, 27 November 2026

`HistGrad` (8.1, adopted at +0.90 ± 3.41 Elo) scaled the quiet history updates by the gradient of a softmax over the
ordering scores of the moves involved. A comparison of the learning code with the current reference engines showed it
as the main extra cost of the updates, and the instruction counters confirmed it: with `HistGradMix` at 0 the Xeon
(PGO avx512, 6 rounds, identical copy of the base at −0.04% / −0.06%) spent 1.05% fewer cycles per node in the
middlegame and 1.14% fewer in endgames (instructions −0.41% / −0.87%, branch misses −1.28% / −1.74%). The author
removed it as a simplification: the updates are those it gave at 0, and the two parameters, the softmax and the
ordering scores the node kept for it are gone. Bench 333951 → 356977.

## 3. Correctness fixes of the 25 November 2026 audit

A code audit of dev 223220b (search, quiescence, histories, move ordering, hash table, the network's incremental state,
time management) found the defects below. Every fix in 3.1 keeps the search identical where the defect does not occur:
bench 168444 in dev and release builds, the same node counts on 200 positions at a fixed depth, perft, perft960,
tdperft and nnperft. The changes that alter the tree are in 3.2, behind options that are off by default.

### 3.1 Fixes (always on)

| defect | effect | evidence | fix |
|---|---|---|---|
| ProbCut stored the result of a verification search that the stop flag had cut short | after a stop every child returns the draw score; ProbCut wrote it as a lower bound (0 or ±1) into the hash table, which is kept between moves, so the next search could cut on a false value | 11 such entries in 480 fixed-node searches, at verified depths up to 10 | no store and no return after the stop (`capture_probe_search`) |
| The singular (exclusion) search also learned after a stop | the draw score of a stopped search could extend, cut, or teach the correction tables through `on_multi_cut` | by reading (same mechanism) | return at once after the stop (`singular_probe`) |
| Torn hash entries with several threads | two threads storing the same position with data words that differ only in their low 16 bits (the low bits of the move) could leave an entry that still passed the tag check, with a meaningless static evaluation | by the bit layout | the low 16 bits of the data word also enter the tag check (`tt_bind`, all 14 sites including the vector probes); xperf cost +0.25% / −0.18% cycles per node (avx512, middlegame / endgames) and +0.16% / +0.39% (avx2) |
| Chess960 castling where the king jumps over its own rook (e.g. Kd1–g1 with Re1–f1) | the fast incremental update of castling skipped slider lines that the jumped rook still blocked: the opponent's accumulator kept or lost a threat feature | nnperft 3 on `6k1/8/8/8/8/8/8/R2KR2Q w KQ - 0 1`: 72 wrong evaluations, now 0 | the fast path only when the rook is not between the king's squares |
| Mate-hunt mode switched on by a sentinel score | under MultiPV a secondary line that failed low keeps −infinity as its score, and its absolute value passed `HuntScore` | node counts at MultiPV 2 depended on `HuntDepth` (536,402 against 688,758); now the same | an unknown root score counts as 0 |
| En passant square from a FEN without a pawn to capture | `4k3/8/8/4P3/8/8/8/4K3 w - d6 0 1` was played e5d6, removing a pawn that was not there | reproduced | the square needs the opponent's pawn on the square it reached and the passed and start squares empty |
| Time: `go movetime` ignored the Move Overhead | the answer came after the full movetime plus stopping and printing | 299 ms for movetime 300 with overhead 30, now 270 ms | the overhead is taken off movetime |
| Time: only the opponent's clock given | the search had no limit | no answer in 30 s; now 971 ms with a 1 s increment | our increment, or 1 ms |
| Time: clocks above 2^31 ms, negative movestogo | the value wrapped to a negative number: an instant move | reproduced | values read as 64-bit and clamped |
| Time: `go` sent while a search still ran | the limits were rewritten under the running search | by reading | the running search is stopped and joined first |
| Time: a search stopped before its first iteration | its −infinity root score became the "previous score" and cut the next move's time by about a third | by reading | the previous score is kept only when known |
| Root evaluations before the network sync | `C2ConvRoot` and the analysis gold facts evaluated the root with the previous search's accumulators | by reading (both off in games) | `nn_root_sync` at the start of `deepen` |
| A repetition at the maximum ply | scored as the static evaluation instead of a draw | by reading | the draw comes first |
| Defects reachable only with option values inside their ranges (they matter for SPSA runs) | the 8-bit depth field of the hash table wrapped to ~255 for a negative ProbCut depth; an int overflow in the prior-move reward; depth 0 and an out-of-bounds read with `PriorRedMinDepth` 1; inexact low-ply reciprocal above `HistLimQuiet` 10,000; 0/0 in the time interpolation when two ends are equal | by reading and arithmetic | depth clamped, 64-bit products, ranges narrowed, the division guarded |
| `TRIUMV_VERIFY_LEGAL` (verification builds) | reported every Chess960 castling as a false legality error | 276 of 400 positions | Chess960 castling excluded, as in the normal path |

### 3.2 Changes of the tree, off by default (`Audit*` options, each needs its own SPRT)

| option | defect | bench when on |
|---|---|---:|
| `AuditEvalNone` | a node in check takes the static evaluation of two plies back, and at plies 0–1 that frame lies before the root with VAL_NONE (32001): in its children the hindsight depth steps fired always, and `improving` and the `LmrGap` term saturated (every check given by a root move) | 194598 |
| `AuditTtNone` | the `LmrTtPvAlpha` reduction term read "hash value above alpha" also for entries without a value | 245917 |
| `AuditRootCuts` (baked 26/11) | ProbCut and the hash-bound cut also ran at the root, where they ignore the root move list (searchmoves, MultiPV) | 168444 |
| `AuditPromoIdx` (baked 26/11) | after a promotion some history writers indexed the promoted piece, the readers the pawn | 333951 |

The SPRT of all fixes with the four options on against 223220b was stopped at −0.26 ± 4.60 over 5,420 games (8+0.08
and 6+0.06): the options are tested one at a time, each against the same build with the option off (3+0.03, 20+20,
Hash 64, [−1, 2]):

| option | result | decision |
|---|---|---|
| `AuditEvalNone` | −1.16 ± 3.23 over 11,418 games | stopped; to retry at a longer time control |
| `AuditRootCuts` | +1.38 ± 1.95 over 30,000 games | baked, option retired |
| `AuditTtNone` | no lean at 3+0.03 nor at 8+0.08 | kept off; retried later with a tuning of `LmrTtPvAlpha` |
| `AuditPromoIdx` | a correction (writers and readers of the histories now agree); it does not lose | baked, option retired; bench 333951 |

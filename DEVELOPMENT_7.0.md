<div align="center">

<img src="logo.png" alt="Triumviratus" width="200">

# Triumviratus 7.0 — development log

**Network project.** First full training from scratch · `legio-septima` · SFNNv16 + PassedPawns

**by Francesco Torsello**

<sub>in collaboration with Maurizio Platino</sub>

</div>

---

<div align="center">

[The network](#1-the-network) · [Measured Elo](#2-measured-elo-incremental) ·
[Speed work](#3-speed-work-nps) · [Limits and robustness](#4-limits-and-robustness) ·
[6.0 log](archive/DEVELOPMENT_6.0.md) · [Networks](NETWORKS.md)

</div>

---

> [!NOTE]
> Every figure below carries its error bar. The whole-engine results come from the release binaries,
> measured after the release freeze of 10 September 2026; earlier whole-engine runs are not used.

---

## 1. The network

A full audit of the search in July 2026 reached a blunt conclusion: the remaining gap to the
strongest engines is **≈ 25–40 Elo of network, not of search**. So 7.0 does not try to
out-search anyone — it changes how the network is *made*.

The first own-lineage net was trained from scratch, but the two that followed grew by
**grafting** a new input block onto a frozen predecessor. Grafting is cheap and it worked, but a
frozen base can only ever *add* what the new block can express — it cannot re-learn what the rest
of the network already believes. **7.0 trains base and feature blocks together, from scratch**, on
a corpus an order of magnitude larger.

`TRANN2` is Stockfish's SFNNv16 feature set plus a `PassedPawns` block of our own:

| block | inputs |
|---|---|
| `FullThreats` | 59,808 |
| `HalfKAv2_hm` | 22,528 |
| `PawnPair` | 4,560 |
| `PassedPawns` | 96 |
| **total** | **86,992** |

L1 1024, L2 32, eight LayerStacks. Two stages: a long stage 1 on the bulk corpus, then a stage 2
with the value/policy blend annealed. The shipped net is the final stage-2 checkpoint,
`legio-septima`.

**Corpus, recipe, hyper-parameters, the epoch-by-epoch history and the reasoning behind the
feature set are in [NETWORKS.md](NETWORKS.md).** Nothing about training is repeated here.

---

## 2. Measured Elo, incremental

Each stage is measured **against the one above it**, not against 6.0. The whole engine against 6.0
is measured separately and reported under the table.

**Every search stage was re-measured in September 2026**, one at a time, each in the regime it was
first measured in and with a fixed number of games rather than a stopped SPRT. Only the stages whose
gain survived are listed. Several of the original figures had been read on 1,300–2,600 games and
ran high — the ordinary effect of reading a result when it looks good — so this table replaces them.

| # | stage | what it is | TC | games | Elo |
|---|---|---|---|---|---|
| 1 | `6.0` → **network** | full training from scratch, `TRANN2`. Measured on the 7.0 binary **frozen before any search change**, so the figure isolates the net | 15+0.15 | 1,442 | **+23.41 ± 9.22** |
| 2 | → **corrections block, retuned** | the correction-history block rebalanced by SPSA at the time control the engine is played at: continuation weight 100 → 85, cap 50 → 48 | 30+0.3 | 30,530 | **+2.65 ± 2.14** |
| 3 | → **evaluation blend constants, tuned on this network** | the eight numbers that turn the network's two outputs into a score were **Stockfish's**, inherited with the wrapper and never tuned for a network carrying three input blocks theirs does not have. SPSA co-tune, confirmed in the shipping regime | 20+0.2, hash 256 | 10,000 | **+3.75 ± 3.49** |

<sub>Stage 2 is about **time control, not about corrections**. The same SPSA at 15+0.15 drove the
continuation weight *up* to 162 and produced a package that lost 11.38 Elo; at 30+0.3 it drove it
*down* to 85 and the vector passed its gate. A lever rejected at short time control has to be looked
at again at the time control it will be played at. The figure was measured with the material
correction table switched on as well; the table was later taken back out, because on its own it
costs 11.5% of the tree and does not repay it. The retuned continuation weight and cap stay.</sub>

<sub>Stage 3 touches the **evaluation** rather than the search. `nn_scale` takes the network's two
raw outputs and produces the number every search margin is compared against, and its constants were
chosen for Stockfish's network. SPSA (mirror, 8+0.08, 1,989 iterations) produced the vector; it read
+2.71 ± 3.35 over 11,654 games in the regime the tuning ran in, and **+3.75 ± 3.49 over 10,000 games
at 20+0.2 with 256 MB** — the effect grows where the engine ships rather than decaying. One of the
eight, `EvalPsqtW`, was left alone on purpose: with `EvalPosW` it only spans "scale everything", which
`EvalScale` already covers, so only the ratio was allowed to move.</sub>

**The whole engine, against 6.0.** The two release binaries against each other — AVX2 PGO on both
sides, each loading its own network — one thread, UHO 2024 (+0.85/+0.94), 75 games in parallel on
the 80-thread test machine:

| TC | hash | depth 7.0 / 6.0 | games | score | Elo |
|---|---:|---:|---:|---:|---:|
| 5+0.05 | 64 MB | 11.7 / 11.2 | 9,000 | 53.62% | **+25.18 ± 4.28** |
| 25+0.25 | 256 MB | 15.7 / 15.0 | 3,000 | 53.07% | **+21.34 ± 6.66** |

LOS is 100% at both points. The two differ by 3.8 ± 7.9 Elo: over the four plies between them there
is no significant change with depth. Depths are measured from the PGNs, not inferred from the time
control. The 5+0.05 row pools two runs of the same binary, 3,000 and 6,000 games (+30.19 ± 7.35 and
+22.67 ± 5.25, compatible); each run draws its openings at random, so the two are independent.

> ⚠️ The current engine signature is **`bench` 240,503**. It moved from 242,956 when continuation
> history at plies 2 and 4 was switched off: an SPRT over 36,620 games found it worth nothing
> (+0.76 ± 1.89 for removing it), and removing it drops two tables and two random reads per scored
> move. Any script still checking an earlier value is verifying the wrong constant.

---

## 3. Speed work (NPS)

Every change below is **node-identical**: the search tree is bit-for-bit the same, so none of them
can alter playing strength at a fixed node count — only the rate at which nodes are produced. Each
was gated on an unchanged `bench` signature before being measured at all.

**Tuning parameters compiled as constants.** The search read **303 tuning parameters** as global
variables inside its hottest code — 26 of them in move scoring alone, reloaded and tested for every
move scored — and the evaluation blend divided by ten more whose divisors were therefore unknown to
the compiler: seven hardware divisions per evaluation. Release builds now compile all of them as
constants. The list is generated from an analysis of every assignment site across the source rather
than written by hand, and it correctly excludes the one variable that looked like a parameter but is
refilled on every `go`. The tree is unchanged — identical node counts on 256 book positions at
depth 14 — and a release build that receives a `setoption` for a frozen parameter says so on the
channel the GUI reads, instead of ignoring it silently.

> **+5.0% NPS** on the shipping binary.

**End to end, against the 31 July build**, both at their own defaults, PGO, 300 positions
interleaved at depth 20 on an idle Zen4 laptop:

> **AVX2 +3.3% NPS · AVX-512 +1.9%**

AVX2 is the figure that matters for the rating lists, which compile it. Each change on its own,
against a baseline built from the same source with all of them off:

| change | in isolation |
|---|---|
| hybrid update on king moves | **+3.4%** |
| pawn-block refresh cache | **+2.8%** |
| mailbox `piece_on[64]` | **+2.5%** |
| both perspectives in one dirty-list pass (AVX2) | **+1.5%** |

They do not add up — together they are worth less than their sum — because they all attack the same
bottleneck, memory traffic in the accumulator, so each one finds in cache what the previous one
already brought there.

<sub>⚠️ The mailbox figure is disputed: later runs on two different CPUs read it slightly negative,
and the question is still open.</sub>

**Feature generation**

- Dead threat tuples are no longer produced. When pawn→pawn relations left the feature set the
  refresh path was updated and the incremental path was not, so a large share of every tuple the
  engine generated was built, indexed, prefetched — and then dropped by the bounds filter.
- `PawnPair` refresh enumerates through a precomputed file band instead of an O(n²) double loop.

**Accumulator**

- **Pawn-block refresh cache.** The finny table covers only `HalfKAv2_hm`; the other blocks were
  rebuilt from scratch on every refresh. `PawnPair` and `PassedPawns` depend on exactly
  *(white pawns, black pawns, orientation)*, and refreshes are triggered by king moves, which
  leave pawns untouched — so between consecutive refreshes the key is almost always unchanged.
  Keyed on the full pawn bitboards, so a collision is impossible by construction. AVX2 only.
- **Hybrid update on king moves**, ported from Stockfish `db98633b`. Threats, pawn pairs and passed
  pawns depend on the king only through an orientation that changes solely when the king crosses
  the d/e file. Every other king move was discarding still-valid work; now the previous accumulator
  is reused, with both HalfKA sides reconstructed from the finny table.
- **Both perspectives in one dirty-list pass**, ported from Stockfish `7b550409`. The list was walked
  twice, once per perspective, so every entry was decoded twice and was long evicted by the time the
  second pass read it.

**Search-side**

- **Capture-victim lookup through the mailbox.** Finding which piece stands on the target square
  scanned six bitboards, on a path taken for every capture in move ordering and throughout
  quiescence. The `piece_on[64]` array is already maintained by make/unmake, so the answer is a
  single load. **+0.7 to +1.0%**, measured on one binary with the path chosen at runtime.
- The quiet stage no longer regenerates the captures already produced by the tactical stage.
- The in-check state is passed in by the caller instead of being recomputed on every generation.
- Least-significant-bit clearing uses `bb &= bb - 1` where the bit being cleared is provably the
  LSB, removing a dependency on the preceding bit scan.

**Build targets**

- **`avx512` no longer carries VNNI**, so it runs on Skylake-X; VNNI lives in **`vnni512`**, and
  **`avx512icl`** adds VBMI/VBMI2/BITALG. Every release binary is checked statically for
  instructions outside its target, because a build machine that has a feature cannot tell you a
  binary depends on it.
- New **`avx2-nopext`** target, Stockfish's `x86-64-avx2`. PEXT is microcoded on Zen 1/Zen 2, where a
  tester running the PEXT build loses a double-digit percentage of NPS. Verified on an EPYC Rome:
  zero `pext`/`pdep` in the binary and the same bench as the PEXT build.

---

## 4. Limits and robustness

Four constants, none of which changes the search: the bench signature is unaffected by all of them.

- **`max_ply` 64 → 128.** Past that ply the search silently stops searching — both the main routine
  and quiescence return the static evaluation instead. At 24 threads and fifteen seconds the
  selective depth was measured at 49, and the regime this engine is aimed at is deeper still.
  Stockfish uses 246. Every derived constant was already written in terms of `max_ply` and rescales
  on its own; the only quadratic cost is the principal-variation table, 20 KB → 72 KB.
- **`Hash` ceiling 1024 → 65536 MB**, and **`MAX_THREADS` 64 → 512**. The thread cap was below the
  80 hardware threads of the machine the engine is tested on; the per-thread structure is allocated
  on demand, so the constant costs nothing at rest.
- **A guard on transposition-table allocation failure**, and it is the reason the hash ceiling is
  not merely a number. At 65536 MB a failed allocation is an ordinary outcome, and the code went
  straight from a null pointer into `memset`. It now falls back from large pages to the heap and
  then to 64 MB, and the reported line says what was actually allocated rather than what was asked
  for.
- **Displayed score recalibrated for this network.** The constant that maps the internal score to
  `score cp` had been fitted on the 5.1 network, and on 7.0 it overstated every score by about 15%:
  a displayed +1.15 was the point of a 50% win probability, where the convention Stockfish follows
  puts it at +1.00. Refitted on 84,660 games of 7.0 against itself — 10.7 million positions — it
  goes from 392 to **449**, and the value holds across time controls (452 at 10+0.1, 447 at 20+0.2,
  444 at 25+0.25). Display only: the search, the tree and the bench signature are unchanged. Drawn
  positions already read zero.

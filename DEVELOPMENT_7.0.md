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
[Speed work](#3-speed-work-nps) ·
[6.0 log](DEVELOPMENT_6.0.md) · [Networks](NETWORKS.md)

</div>

---

> [!IMPORTANT]
> **6.0 is and remains the official release.** The 7.0 network is finished — stage 2 closed at
> epoch 799 — but there is no 7.0 release yet. Every figure below carries its error bar.

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

Each stage is measured **against the one above it**, not against 6.0. The stages therefore
compose, but the total is *not* their sum — a whole-engine gate against 6.0 will be measured
separately and published when the work is closed.

| # | stage | what it is | TC | games | Elo |
|---|---|---|---|---|---|
| 1 | `6.0` → **network** | full training from scratch, `TRANN2`. Measured on the 7.0 binary **frozen before any search change**, so the figure isolates the net | 15+0.15 | 1,442 | **+23.41 ± 9.22** (LOS 100%) |
| 2 | → **quiet promotions in qsearch** | `PromoQS=6`: promotions generated in qsearch, exempt from the capture cap, filtered by SEE ≥ 0 | 25+0.25 | 2,572 | **+8.38 ± 6.67** (LOS 99.31%) |
| 3 | → **corrections block, retuned** | material correction table switched back on and the block rebalanced around it: material weight 67, continuation weight 85, cap 48 | 30+0.3 | **30,530** | **+2.65 ± 2.14** (LOS 99.25%, LLR 2.96) |
| 4 | → **qsearch delta pruning restored** | `QSDeltaMargin` 3000 → 1525, co-tuned with `QSCaptHistScale` 86 → 72. 3000 was the **maximum of the parameter's own range**, i.e. delta pruning was effectively off | 30+0.3 | 5,994 | **+5.57 ± 4.84** (LLR 1.89, stopped before the bound) |

> ⚠️ **The engine signature changes at every stage: `bench` goes 207,259 → 225,898 (stage 2) →
> 251,855 (stage 3) → 261,287 (stage 4).** The current signature is **261,287**; any script or
> procedure still checking an earlier value is verifying the wrong constant, and each of those is
> valid only for binaries built before the corresponding bake.

<sub>Stage 2 is worth recording because of what it cost. Quiet promotions were generated only
inside the quiet buffer, and qsearch never asks for it — so the engine declared those moves
tactical in two separate places and then never searched them. The first implementation *lost*
about 12 Elo: it paid for the nodes it added and bought nothing, because with a capture cap of
three the new promotions evicted every capture. Six variants, separated by their bench node
counts, were needed to find the one that pays; between the first and the last there are roughly
20 Elo at the same underlying idea. Node counts do not predict Elo, but they decompose a cost,
and that is what made the difference visible. The same code reads −11.77 at 12+0.12 and +8.38 at
25+0.25: a change that inflates the tree has to be judged at the time control it will be played
at.</sub>

<sub>Stage 3 is the clearest lesson of the project so far, and it is about **time control, not
about corrections**. The material correction table had been switched off in July on the strength of
450 games, and re-measured crudely it looked terrible: −17.68 over 708 games. Tuned by SPSA and
gated again it was flat. What changed the answer was tuning it **at the time control it would be
played at**: an SPSA run at 15+0.15 drove the continuation weight *up* to 162 and produced a
package that lost 11.38 Elo over 2,016 games; the same SPSA at 30+0.3 drove the same weight *down*
to 85 and turned the material weight toward zero — the opposite direction — and the resulting
vector passed its gate. Switching the table on costs 11.5% more nodes, roughly 8.8 Elo of debt at
55 Elo per doubling, and that debt is repayable at long time control and not at short. ⇒ **Every
lever rejected at short TC has to be looked at again.** The SPSA itself was stopped at 152
iterations and is not converged; the vector will be refined at long TC.</sub>

<sub>Stage 4 is not a tuning result and should not be read as one. `QSDeltaMargin` was sitting at
3000, which is the **maximum of its own range** — delta pruning in quiescence was effectively
switched off in the shipped binary, and it had been pushed there by two earlier SPSA runs. So the
gain is the repair of a mechanism that was off by mistake, not a better setting found on a smooth
landscape; the order of magnitude says the same thing, since the corrections block needed 30,530
games to show 2.65 Elo and this showed twice that on a fifth of the sample. The generalisation is
worth more than the patch: **a default sitting at a bound of its own range is the signature of a
disabled mechanism**, and it can be found mechanically rather than by experiment. Two caveats are
recorded honestly — the SPRT was stopped at LLR 1.89 without reaching either bound, with the
estimate declining as the sample grew (+13.05 at 1,918 games, +5.57 at 5,994), and the vector moves
two parameters at once, so which of the two pays is not known.</sub>

<sub>Tested and rejected: `PromoQS=7`, which adds the **knight** under-promotion in qsearch on top
of the queen — −1.82 ± 6.49 over 3,818 games at 25+0.25. It grows the tree by a further 11% and
did not repay it.</sub>

---

## 3. Speed work (NPS)

Every change below is **node-identical**: the search tree is bit-for-bit the same, so none of them
can alter playing strength at a fixed node count — only the rate at which nodes are produced. Each
was gated on an unchanged `bench` signature before being measured at all.

**No per-change figures are given.** The harness that produced them ran the two binaries in a
fixed A-then-B order on each position, so whichever binary occupied the second slot absorbed the
machine's thermal drift; run against itself — same binary, same options, both sides — it reported
a nine-sigma difference out of nothing. It now alternates the order within each pair, drops exact
ties from the sign test, refuses to run when a requested option is not announced by the engine,
and reports the A-first and B-first sub-samples separately as a built-in drift check. A single
end-to-end measurement of the finished engine will replace the withdrawn figures.

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
  Direct-mapped and keyed on the full pawn bitboards, so a collision is impossible by
  construction. AVX2 only.
- **Hybrid update on king moves**, ported from Stockfish `db98633b`. `requires_refresh` is true for
  every move of one's own king, but threats, pawn pairs and passed pawns depend on the king only
  through an orientation that takes two values and changes solely when the king crosses the d/e
  file. Every other king move was discarding still-valid work; now the previous accumulator is
  reused, with both HalfKA sides reconstructed from the finny table.
- **Both perspectives in one dirty-list pass**, ported from Stockfish `7b550409`. The list was
  walked twice, once per perspective, with a full accumulator update in between — so every entry
  was decoded twice and was long evicted by the time the second pass read it. Ours departs from
  the source by keeping the wide writes sequential, all of white then all of black, rather than
  alternating them at every transition.

**Memory layout**

- **Weight rows permuted for locality.** A small fraction of the threat table carries the large
  majority of the accesses, scattered across the whole of it — more pages than any TLB maps, so in
  practice every access was a DRAM miss on a working set that would fit in cache if it were packed.
  The rows are reordered at load time and the indices remapped through a lookup table small enough
  to stay in L2. The evaluation is unchanged; only rows move. Ordering is by co-occurrence
  clustering over the hot rows rather than by raw frequency.

**Search-side**

- The quiet stage no longer regenerates the captures already produced by the tactical stage.
- The in-check state is passed in by the caller instead of being recomputed on every generation.
- Least-significant-bit clearing uses `bb &= bb - 1` where the bit being cleared is provably the
  LSB, removing a dependency on the preceding bit scan.

**Build targets**

- `avx512` no longer requires VBMI2. The target had been set up on machines that all happened to
  have it — but that is the binary shipped as "avx512", and on Skylake-X or Cascade Lake it dies on
  an illegal instruction at startup. The targets are now separate, as in Stockfish: `avx512`
  (F/BW/DQ/VL/VNNI) and `avx512icl` (+ VBMI/VBMI2/BITALG). The ICL path measured as exactly nothing,
  so `avx512` ships without it: broader compatibility at no cost.
- New **`avx2-nopext`** target, Stockfish's `x86-64-avx2`. PEXT is three cycles on Intel and Zen 3+
  and microcoded on Zen 1/Zen 2, where a tester running the PEXT build loses a double-digit
  percentage of NPS. Verified on an EPYC Rome: zero `pext`/`pdep` in the binary and the same bench
  as the PEXT build, so the fancy-magic fallback is equivalent.

<sub>Two changes are **under re-measurement** and may yet be removed: the prefetch of the HalfKA
weight rows, and the `piece_on[64]` mailbox that replaces bitboard scans when finding which piece
stands on a square. Both read negative on the repaired harness on two different CPUs, but removing
them together does not recover what removing either alone appears to gain — an inconsistency that
has to be resolved before either is taken out.</sub>

<sub>Several candidates were measured and rejected rather than shipped, and the rejections were as
useful as the acceptances: prefetching all four SIMD tiles of a row instead of only the first line,
prefetching the PSQT rows, permuting the HalfKA rows the way the threat rows are permuted, widening
the clustering granularity, and caching the minor/major correction keys. Two rules came out of
them. A prefetch pays only when the table cannot fit in cache, and only one line per row — the rest
of the row is sequential and the hardware streamer already has it. And a frequency permutation
helps only where the layout has no structure to begin with: `HalfKA` maps 64 consecutive squares to
64 consecutive rows, so permuting it replaces sequential access with scattered access and loses.</sub>

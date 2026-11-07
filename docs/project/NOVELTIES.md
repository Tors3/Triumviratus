<div align="center">

<picture><source media="(prefers-color-scheme: dark)" srcset="../images/logo_mark_dark.svg"><img src="../images/logo_mark.svg" alt="Triumviratus" width="130"></picture>

# Triumviratus: what is new

**The techniques of Triumviratus 8.1 for which no earlier example was found in the published work of other engines**

**by Francesco Torsello**

<sub>in collaboration with Maurizio Platino</sub>

</div>

---

<div align="center">

[Method](#method) · [Summary](#summary) · [Consilium](#1-consilium-experts-by-material-on-the-first-layer) ·
[PassedPawns](#2-the-passedpawns-input-block) · [CorrPhase](#3-correction-history-per-material-band-corrphase) ·
[HistGrad](#4-the-quiet-history-as-a-listwise-ranker-histgrad) ·
[Bishop-pair capture](#5-capture-of-a-bishop-of-the-pair-in-move-ordering) ·
[Slider attacks](#6-slider-attacks-on-avx-512) · [Analysis build](#7-analysis-build-position-solver) ·
[In development](#8-in-development) · [Open checks](#9-open-checks)

</div>

---

## Method

First written on 9 October 2026, during the development of 8.0; revised on 7 November 2026 for the 8.1 release. The
register lists the techniques that the 8.1 release uses and for which no earlier example was found, and, in a
separate section ([§8](#8-in-development)), the new ideas that are written but not used in 8.1 and will be tested
again for the next version. Known ideas that the project uses in its own form, and ideas that were tried and closed,
are not listed.

Every technique was compared with other engines in two ways: by reading the source code of nine engines (Stockfish 19,
Reckless, PlentyChess 8.0, Viridithas 20, Berserk 14, Caissa 2.0, Coda, Integral 8, Stormphrax 8.0) and by searching
chessprogramming.org, TalkChess, GitHub and the literature. A search that finds nothing does not prove that nobody
has done it: the developer channels on Discord are not indexed, and closed-source engines cannot be read. Where no
precedent was found, the text says "no earlier example found in the sources checked", and every claim of priority
below is meant **to our knowledge**.

Each entry is in one of two categories:

- **New**: no earlier example found in the sources checked.
- **New in its form**: the general idea is known, but not in this form; the text says what is known and what is ours.

Measurements are Elo differences from SPRT or fixed-games runs on the project's test machines, with the 95% error
margin and the number of games; the full context of each is in [`DEVELOPMENT_8.0.md`](../development/archive/DEVELOPMENT_8.0.md),
[`NETWORKS.md`](../networks/NETWORKS.md) and the [`archive`](../development/archive) of earlier versions.

## Summary

| entry | category | version | measured effect | in 8.1 |
|---|---|---|---|---|
| Experts by material on the king-relative input block: Consilium (8.0, four experts by piece count) and Consilium Plebis (8.1, eight experts per side, chosen by the opponent's non-pawn material) | New | 8.0, 8.1 | 8.0 against 7.0 **+27.3 ± 8.3** (15+0.15, 2,000 games), of which about +15 from the network with its tuning; Consilium Plebis over Consilium **+10.3 ± 4.3** (same search, 30k nodes per move, 8,332 games) | the 8.1 network |
| `PassedPawns` input block (96 inputs) | New | 6.0 | **+6.96 Elo** (15+0.15, 2,596 games), network alone | in every network since 6.0 |
| Correction history per material band (`CorrPhase`) | New in its form | 8.0 | **+3.2 ± 3.8** (12+0.12, 8,634) | in use |
| History update as a listwise ranker (`HistGrad`) | New | 8.1 | non-regression test passed (5+0.05, both sockets and a Zen 4 laptop, 11,166 games); a stronger blend (512) did not hold | in use at 256 |
| Capture of a bishop of the pair in move ordering (`BishopPairCapt`) | New | 8.0 | not measured alone; adopted in 8.0 with the tuned vector of the ordering terms (+3.4 ± 3.9, about 8,100 games) | in use |
| Slider attacks by obstruction difference on the four lines of a square in one AVX-512 register | New in its form | 8.1 | **−0.98%** cycles per node in the middlegame, **−1.20%** in endgames, whole engine (hardware counters, PGO build, Xeon) | default of the AVX-512 builds |

The analysis build has its own section ([§7](#7-analysis-build-position-solver)), and the ideas in development
another ([§8](#8-in-development)), both outside this table.

---

## 1. Consilium: experts by material on the first layer

**What it is.** The king-relative input block (HalfKA) of the network is divided into sets of weights, the "experts",
chosen by the material on the board. One expert is active per position, so an evaluation costs almost the same as with
a single block. In the trainer each expert is a shared base plus a delta that starts at zero: the network begins as an
ordinary HalfKA network and the experts separate only where the data asks for it; this is the same scheme as the
factorizer that nnue-pytorch uses for king buckets, applied to material. The idea is the author's, taken from
mixture-of-experts language models (many parameters, few active per token).

- **Consilium (8.0)** has four experts, chosen by the total piece count (up to 9, 10-15, 16-23, 24 or more), the same
  for both perspectives. A capture that crosses a threshold forces a refresh of the accumulator, and the refresh cache
  is kept per expert.
- **Consilium Plebis (8.1)** has eight experts **per perspective**: each side's expert is chosen by the non-pawn
  material that side is facing, through a routing table learned in training. Positions with a material imbalance are
  therefore read differently from the two points of view, and a pawn more or less no longer moves a position to
  another expert. The king-relative weights are int8, so the eight experts take the memory of four.

**What "mixture of experts" means here.** The experts are chosen by material, not by a learned gate over the whole
input. In that sense every routing scheme of an NNUE network is a mixture of experts: king buckets on the input layer,
output buckets and layer stacks after the accumulator. The name describes where the idea came from; what is new is the
routing key on the input layer, as the next paragraphs explain.

**Measured.** Consilium, trained on 28-29 September 2026 (about 472 billion positions): version 8.0 against the 7.0
release **+27.3 ± 8.3 Elo** at 15+0.15 on 2,000 games, of which about +15 come from the network with its re-tuned
parameters. Consilium Plebis, trained from 21 to 28 October 2026: **+10.3 ± 4.3 Elo** over Consilium with the same
search, at 30,000 nodes per move, on 8,332 games
([`NETWORKS.md`](../networks/NETWORKS.md#consilium-plebis--the-network-of-triumviratus-81)).

**Compared with others.** The idea was reached independently by the author, and no network of this kind was found in
another engine. Choosing weights by piece count is common, but after the accumulator: Stockfish's layer stacks and the
output buckets of many engines select the small output layers by piece count. In the engines we checked, the input
buckets of the first layer are chosen by the king square only, not by material; an external engine developer confirmed
on 14 October 2026 that, as far as she knew, no other engine buckets the input layer by piece count. Stockfish 16.1
switched between two whole networks by material imbalance, which routes between separate networks rather than between
experts inside one layer. The academic work on phase-specific models uses separate full networks with AlphaZero-style
MCTS ([arXiv 2401.16852](https://arxiv.org/abs/2401.16852);
[Helfenstein et al.](https://ml-research.github.io/papers/helfenstein2024game.pdf)), not experts inside an
efficiently updatable first layer of an alpha-beta engine. Mark Tang had tried an unpublished network of phase experts
in early 2026 that ran faster but did not play stronger (independent work, see [`NETWORKS.md`](../networks/NETWORKS.md)).

**Category.** New: experts by material on the king-relative block of the first layer, on top of the king buckets, one
active per position, in a top engine and with a measured gain; in 8.1, chosen separately for each perspective by the
material that side is facing. To our knowledge Triumviratus is the first engine to do this. Not new: hard routing by
piece count as such (output buckets, layer stacks) and the shared-base training scheme (the nnue-pytorch factorizer).

## 2. The PassedPawns input block

**What it is.** 96 inputs, one per passed pawn: 48 oriented squares for each side. A pawn is passed when no enemy pawn
stands ahead of it on its file or the adjacent ones and no own pawn stands directly ahead. This is a relation across
three files that the king-relative block does not express directly. The block deliberately encodes only the square:
blocked, supported and connected passers can be learned by the pairwise product of the first layer. In the engine it
is added to the threat accumulator and reuses the incremental update of pawn events.

**Measured.** Grafted at zero on `rubicon-alea-v2` and trained alone with the base frozen, about 4 epochs:
**+6.96 Elo** on 2,596 games at 15+0.15, network alone (17 July 2026). It has been in every network since:
`legio-septima` (7.0), Consilium (8.0) and Consilium Plebis (8.1).

**Compared with others.** Stockfish's `PP_3Wide` encodes pairs of pawns on nearby files, not the passed state. None of
the nine engines read has passed-pawn inputs. The only precedent found is an unimplemented note in Viridithas's ideas
file ("passed pawns by file?").

**Category.** New: to our knowledge the first passed-pawn input block implemented and measured.

## 3. Correction history per material band (CorrPhase)

**What it is.** The keys of the pawn and minor-piece correction tables are mixed with the material band (pieces on
the board: up to 9, 10-15, 16-23, 24 or more), so that each pawn structure and each minor-piece configuration is
corrected separately in each phase of the game. In 8.0 the four bands were the four experts of the network, so each
expert learned its own error; 8.1 keeps the same four bands with the new network.

**Measured.** **+3.2 ± 3.8** on 8,634 games at 12+0.12, adopted on 6 October 2026. A diagnostic build shows that the
corrections remove 58.8% of the evaluation error with `CorrPhase` against 57.3% without, the gain coming from
endgames with nine pieces or fewer.

**Compared with others.** Correction history began in Caissa (October 2023) and is indexed today by pawn structure,
minor pieces, non-pawn material and previous moves. Caissa and Stockfish also keep a table keyed by the material
configuration ([Stockfish PR #5556](https://github.com/official-stockfish/Stockfish/pull/5556)), a correction for each
material signature on its own. No engine was found that splits the pawn and minor-piece corrections by material band.

**Category.** New in its form; corrections conditioned on material alone are known.

## 4. The quiet history as a listwise ranker (HistGrad)

**What it is.** When a quiet move causes a beta cutoff, the history rewards it and punishes the quiet moves tried
before it. With `HistGrad` these updates are scaled by the gradient of a softmax over the ordering scores of the moves
involved, as in a listwise ranking loss: a best move the ordering already expected learns little, a surprising one
learns a lot, and a tried move that the ordering already rated low is barely punished. The scaled update is blended
with the ordinary one (`HistGradMix`, 256 of 1024) at a fixed temperature (`HistGradT`).

**Measured.** Non-regression test passed at 5+0.05 on both sockets of the test machine and on a Zen 4 laptop (11,166
games); a stronger blend (512) did not hold. Adopted at 256 on 22 October 2026.

**Compared with others.** History updates scaled by depth, by the size of the cutoff or by the current value of the
entry (gravity) are standard. No engine was found that scales the reward and the penalties by the ordering scores of
the moves of the node, as the gradient of a ranking loss.

**Category.** New.

## 5. Capture of a bishop of the pair in move ordering

**What it is.** A bonus in the tactical score for capturing a bishop when the opponent holds the bishop pair
(`BishopPairCapt`).

**Measured.** Not measured alone. Adopted in 8.0 with the tuned vector of the ordering terms (+3.4 ± 3.9 over about
8,100 games against the defaults) and kept in 8.1.

**Compared with others.** The value of the bishop pair is a classical evaluation term; no example of it in the move
pickers of the engines read was found.

**Category.** New.

## 6. Slider attacks on AVX-512

**What it is.** In 8.1 the vectorised slider attacks of Stockfish origin were replaced by code of our own. The
obstruction difference itself is published (chessprogramming wiki): on one line, the attacked squares come from the
occupied squares above the slider and the highest occupied square below it. That needs the highest set bit of a word,
which vector instructions could not give before AVX-512 CD added a lane-wise leading-zero count (`vplzcntq`). Our
kernel puts the four lines of a square (two diagonals, file, rank) in the four 64-bit lanes of one register and
computes them together, with no table and no byte reversal.

**Measured.** **−0.98%** cycles per node in the middlegame and **−1.20%** in endgames, whole engine, at an identical
search tree (hardware counters, PGO build, Xeon). The measurement method, with the engine pinned to one processor and
only the search threads counted, is described in [`DEVELOPMENT_8.0.md`](../development/archive/DEVELOPMENT_8.0.md) §36.

**Compared with others.** We have not found this combination described elsewhere.

**Category.** New in its form.

## 7. Analysis build (Position Solver)

Techniques of the Position Solver, the build for hard positions (October 2026). They act only in analysis, so they
are kept apart from the game entries. The measure here is the number of positions solved, on IQ4 (183 positions) and
on the Arasan 2026 collection (200), at fixed nodes with one thread; details in
[`DEVELOPMENT_8.0.md` §49](../development/archive/DEVELOPMENT_8.0.md). A search of the forums and of the source of
Crystal and ShashChess found no option like these two; as above, this is to our knowledge.

| entry | category | measured effect | status |
|---|---|---|---|
| Preference for the more direct win (`AnDirect`): when both exact lines win clearly (above +3), the direct one (check, capture, promotion, or a sacrifice: a quiet move that loses material by static exchange) is shown and played if the higher one is not direct and the gap is at most 4 pawns; the search tree is unchanged | New | same tree: IQ4 163 → 166 at 4M nodes (three solutions added, none lost), Arasan 178 unchanged; without the sacrifice in the definition, 164 (a quiet sacrifice was lost) | default in the Position Solver |
| The second line paused in quiet positions (`AnShQuiet`): from root depth 10, when the two best moves are quiet and the score has stayed within 15 cp for three iterations, the search goes on with one line | New | eight collections at 1M nodes +20 (SBD, quiet key moves, 98 → 110); higher budgets +7 | default in the Position Solver |

## 8. In development

New ideas of the project that 8.1 does not use. Their code is written and off by default, or, for the network, the
blocks are designed and trained as grafts; each will be tested again for the next version, on the 8.1 search and
network. They are listed here so that the record of priority is public; no claim of strength is made for them until
they pass a test in their final form, and a partial measurement, where one is given, is not a result.

| entry | category | where it acts | state |
|---|---|---|---|
| Knight outposts in move ordering (`OutpostOrder`, `OutpostSafe`, the author's idea): a bonus for a knight move to a square on ranks 4-6 defended by an own pawn and out of reach of every enemy pawn, now and later; with `OutpostSafe` also without an enemy bishop of the square's colour. Outposts are a classical evaluation term; none of the nine engines read uses them in move ordering, and no example was found on the web | New | move ordering | in the code, off in 8.1; to be tuned again with the 8.1 ordering |
| Helper threads specialised by material regime (`SmpAff`, the author's direction): each helper of the parallel search takes a range of the material regimes read from the experts of the network and reduces less in the nodes of its range, so the threads share one network but divide the tree by the kind of position. Helpers with different search parameters are known; the division of the work by the experts of the network needs a network with experts by material | New in its form | parallel search | in the code, off in 8.1; to be measured at many threads |
| Correction from the opponent's plan (`CorrContOppW`, the author's idea): a continuation-correction term whose pair of moves is the opponent's two last moves (three plies back and one ply back), a fragment of the opponent's plan, instead of one of our moves and its answer. The engines read use only pairs that contain one of our moves | New | correction of the static evaluation | in the code, off in 8.1; on the 8.1 search and network, weight 8000 against off at 8+0.08 (UHO): +1.5 ± 4.8 over 4,882 games, test stopped before a decision, to be continued |
| Pawn break in a closed structure reduced less (`RdPawnBreak`, the author's idea): when at least three pawns are blocked, a quiet pawn push that attacks an enemy pawn, offering the exchange that opens a file or a diagonal, has its late-move reduction lowered. The engines read do not condition any reduction on the pawn structure | New | search reductions | in the code, off in 8.1 |
| Passed pawns with their relations (`PassedRel`, `PassedState`): an extension of the `PassedPawns` block with the relations of each passed pawn (outside the enemy king's square, connected, free path; or one of 100 states of the passer) | New | network inputs | trained as grafts on the 8.0 network; a candidate for the next network |

## 9. Open checks

1. **Experts by material and recent engines.** No engine with input buckets by material was found, but the developer
   channels are not indexed. Every statement of priority here is meant to our knowledge; corrections are welcome.
2. **Move ordering and `HistGrad`.** For the bishop-pair capture, the knight outposts and `HistGrad`, nine engines and
   the web were checked; Obsidian, Koivisto, Ethereal and recent Berserk were not read line by line.

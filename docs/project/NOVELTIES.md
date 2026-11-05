<div align="center">

<picture><source media="(prefers-color-scheme: dark)" srcset="../images/logo_mark_dark.svg"><img src="../images/logo_mark.svg" alt="Triumviratus" width="130"></picture>

# Triumviratus: what is new

**A register of the techniques of Triumviratus, from version 1.0 to 8.0, compared with the published work of other
engines**

**by Francesco Torsello**

<sub>in collaboration with Maurizio Platino</sub>

</div>

---

<div align="center">

[Method](#method) · [Summary](#summary) · [Consilium](#1-consilium-experts-by-material-on-the-first-layer) ·
[PassedPawns](#2-the-passedpawns-input-block) · [CorrPhase](#3-correction-history-per-expert-corrphase) ·
[Move ordering](#4-move-ordering-with-chess-knowledge) · [Neurons](#5-the-first-layer-neuron-by-neuron-per-expert) ·
[Other new ideas](#6-other-new-ideas-not-adopted) · [Revisited](#7-known-ideas-revisited-in-a-different-form) ·
[Speed](#8-speed-work) · [Done elsewhere first](#9-done-elsewhere-first) · [Analysis build](#10-analysis-build-position-solver) ·
[Open checks](#11-open-checks)

</div>

---

## Method

Written on 9 October 2026, during the development of 8.0. Every technique of the project that looked new, or new in
its form, was compared with other engines in two ways: by reading the source code of nine engines (Stockfish 19,
Reckless, PlentyChess 8.0, Viridithas 20, Berserk 14, Caissa 2.0, Coda, Integral 8, Stormphrax 8.0) and by searching
chessprogramming.org, TalkChess, GitHub and the literature. A search that finds nothing does not prove that nobody
has done it: the developer channels on Discord are not indexed, and closed-source engines cannot be read. Where no
precedent was found, the text says "no earlier example found in the sources checked", and every claim of priority
below is meant **to our knowledge**.

Each entry is in one of three categories:

- **New**: no earlier example found in the sources checked.
- **Revisited**: a known idea, taken up again in a different form or at a different scale.
- **Done elsewhere first**: the idea was published by another engine before us, even when we reached it on our own;
  the source is named. The category describes public priority, not where our idea came from.

Measurements are Elo differences from SPRT or fixed-games runs on the project's test machine, with the 95% error
margin and the number of games; the full context of each is in [`DEVELOPMENT_8.0.md`](../development/archive/DEVELOPMENT_8.0.md),
[`NETWORKS.md`](../networks/NETWORKS.md) and the [`archive`](../development/archive) of earlier versions.

## Summary

Of 61 entries, **16 are new**, **29 are known ideas revisited in a different form**, and **16 were done elsewhere
first**. Three of the new ones are adopted and measured as gains, and together they form the core of 8.0: a network
divided by game phase, an input block that the king-relative block cannot express, and corrections that follow the
experts of the network. The analysis build has its own section ([§10](#10-analysis-build-position-solver)), outside these counts.

| entry | category | version | measured effect | status |
|---|---|---|---|---|
| Consilium: four experts by material on the king-relative input block | New | 8.0 | 8.0 against 7.0 **+27.3 ± 8.3** (15+0.15, 2,000 games), of which about +15 from the network with its tuning | adopted, the 8.0 network |
| `PassedPawns` input block (96 inputs) | New | 6.0 | **+6.96 ± 6.56** (15+0.15, 2,596), network alone | in every network since 6.0 |
| Passed-pawn blocks with relations, grafted on Consilium: `PassedRel` (rows added to `PassedPawns`) and `PassedState` (replaces it, one row per passer in one of 100 states) | New | 8.0 dev | `PassedRel` +1.8 ± 4.1 (20+0.2 endgames, 2,484); `PassedState` −2.1 ± 3.7 trained alone, +3.3 ± 5.0 with the layers after the accumulator (60k nodes, endgames); on ordinary games against a build without grafts `PassedRel` −2.9 ± 7.5 (2,034), `PassedState` −4.8 to −16.5 | closed, not in 8.0; engine cost 2 to 3% of cycles per node |
| Correction history per expert (`CorrPhase`) | New in its form | 8.0 | **+3.2 ± 3.8** (12+0.12, 8,634) | adopted |
| Search margins per expert (`RfpExp`, `FutExp`, `LmrExp`) | New in its form | 8.0 | +1.6 ± 4.3 (6,562, three tests); tree −2 to −6% at fixed depth | adopted, tuning continues |
| Reduction slope per expert (`LmrScaleExp`) | New in its form | 8.0 | +1.1 ± 3.7 (9,118, 8+0.08); expert 1 alone 0.0 ± 6.0 (3,160) | off, being tested in the defence package |
| Knight outposts in move ordering (`OutpostOrder`) | New | 8.0 | +0.21 ± 3.93 (10+0.1, 8,244) at a hand-set value | adopted with the SPSA PR4 vector (+3.4 ± 3.9, about 8,100 games) |
| Capture of a bishop of the pair in move ordering (`BishopPairCapt`) | New | 8.0 | not measured alone | adopted with the SPSA PR4 vector (+3.4 ± 3.9, about 8,100 games) |
| Expert-boundary ideas (PhaseEdge) | New | 8.0 | from +1.3 ± 2.3 (24,292) to −14.1 | closed |
| Blending two experts at a boundary (`EvalExpertBlend*`) | New | 8.0 dev | +5.27 ± 7.86 at fixed nodes (1,978); −24.23 ± 11.12 at 8+0.08 (1,034), +15 to 18% cycles; depth ≥ 3 variant −3.06 ± 9.59 (1,248) | removed |
| Correction from the opponent's two last moves (`CorrContOppW`) | New | 8.0 | −4.8 ± 8.7 (1,806); real signal, correlation +0.24; on the build without the I/O defect +2.4 ± 4.7 at 8+0.08 (5,460), −8.0 ± 7.7 at 5+0.05; all runs at 8 s or more pooled (8+0.08, 10+0.1 without hyperthreading, 12+0.12): **+0.6 ± 3.0 (about 13,300), neutral**; SPSA CORR1p2 kept it on (about 4,550); on the release-candidate build at 8+0.08 (14 October, two runs with the same tree, before and after the prefetch below): **+0.5 ± 3.0 over 14,426 games, neutral** | off |
| `Mobility` input block | New as an idea | 8.0 dev | cost only: −16.2% NPS | archived, never trained |
| Slider attacks by obstruction difference on the four lines of a square in one AVX-512 register, with the lane-wise leading-zero count (`vplzcntq`) | New in its form | 8.1 dev | **−0.98%** cycles per node in the middlegame, **−1.20%** in endgames, whole engine (xperf, PGO, Xeon) | adopted, default of the AVX-512 builds |
| History update as a listwise ranker (`HistGrad`): the reward of the best quiet move and the penalties of the quiet moves tried before it are scaled by the gradient of a softmax over their ordering scores, so a best move the ordering expected learns little, a surprising one a lot, and a tried move the ordering rated low is barely punished | New | 8.1 dev | **+0.90 ± 3.41** (5+0.05, both sockets and a Zen 4 laptop, 11,166); without socket 1 +3.1 ± 4.2; the stronger blend (512) −4.0 ± 6.3 (3,066) | adopted at 256 (22 October 2026); blend and temperature to the history tuning |
| Helper threads specialised by material regime (`SmpAff`): each helper of the parallel search takes a range of the material regimes read from the experts of the network (0-2, 3-5, 6-7 in turn) and searches wider in the nodes of its range, so the threads share one network but divide the tree by the kind of position | New in its form (diversified helpers exist elsewhere, see `DiverseSMP`) | 8.1 dev | +0.54 ± 3.42 at 4 and 8 threads (8,940 games, 5 to 10 s, both machines); +1.9 ± 13.1 at 20 and 32 threads (544) | measurement still open, neutral so far (off by default) |
| Discovered check in move ordering (`ChkDisc`): a quiet move whose piece stands between one of our sliders and the enemy king and lands off that line gets a bonus, on top of the bonus for direct checks | Revisited (Coda scores discovered attacks on any enemy piece) | 8.1 dev | +0.8 ± 3.7 over 9,178 (8+0.08 and 5+0.05 summed, both sockets; test paused, to be continued); first test −0.2 ± 5.3 (4,174) | being tested (off by default) |
| Pawn break in a closed structure (three or more blocked pawns) reduced less (`RdPawnBreak`) | New | 8.1 dev | 2048: +0.19 ± 5.74 (8+0.08, 3,724), +0.6 ± 4.8 pooled with 7+0.07 (5,142); 3072 −3.7 ± 9.2 | closed, neutral |
| Hyperbola quintessence for AVX2 in our form (`pair_hqs`: the two diagonals in a vector register, the rook in general registers) | Revisited | 8.1 dev | −0.23% / −0.05% cycles per node | adopted, default of the AVX2 builds |
| Surprise rule (`TmSurpriseScale`) | Revisited | 8.0 | **+6.23 ± 6.75** at 40+0.4 (2,174); neutral at 16+0.16 | adopted |
| Guard on the low-depth hash-move extension (`LdseMax`) | Revisited | 8.0 | **+6.25 ± 7.89** (15+0.15, 2,112) | adopted; inactive while LDSE is off |
| Cap of one extension from a negative reduction per line (`NegExtMax`) | Revisited | 8.0 | **+3.88 ± 3.26** (12+0.12, 11,384) | adopted |
| More time in a defence that is not lost, less with 24 or more pieces (`TmDefendMul`, `TmEarlySave`) | Revisited | 8.0 | **+7.27 ± 7.08** at 40+0.4 (2,150), the two together | adopted |
| Guard on sudden score jumps (`TmJumpGuard`) | Revisited | 8.0 | −2.52 ± 7.33 at 8+0.08 (2,210); triggers on 0.25% of moves | on, as a guard |
| Passed-pawn pushes in endgames reduced less, never pruned (`PassedPushRed`) | Revisited | 8.0 | +4.4 ± 6.8 on an endgame book | adopted |
| Passed-pawn push bonus in move ordering (`PassedPush`) | Revisited | 8.0 | not measured alone | adopted with the SPSA PR4 vector (+3.4 ± 3.9, about 8,100 games) |
| Passed-pawn push to the seventh searched in quiescence in endgames (`QsPasserPush`), with its own levers | Revisited | 8.0 dev | −0.77 ± 7.48 (2,264); +1.1 ± 3.8 (3,370); endgames +0.23 ± 2.45 (7,516) | closed, off |
| Neuron study of the first layer, per expert | Revisited | 8.0 | no dead or duplicate neurons; experts specialise by intensity | study |
| Universal executable from one unit per source file | Revisited | 8.0 | −0.28% / −0.17% cycles per node against the separate build | adopted |
| Refresh cache of the pawn input blocks keyed by the pawn bitboards | Revisited | 7.0 | +2.8% NPS (AVX2), identical tree | adopted |
| `tdperft` and `nnperft` | Revisited | 8.0 | 0 errors on 482 M nodes and on the Chess960 suite | permanent tools |
| Zero-initialised graft of a new block on a finished network, base frozen | Revisited | 6.0 | `PassedPawns` +7 Elo in about 4 epochs | in use |
| Residual concepts used in the engine without retraining: constant terms, corrections learned with keys on the concept (`CorrKingPawnW`, `CorrShelterW`, `CorrPassKingW`), less pruning with an open king (`KingOpenRfp`), less reduction for an unstoppable passer (`PasserFreeRed`) | Revisited | 8.0 dev | from −16.0 ± 16.0 (564) to −0.37 ± 3.68 (about 9,300); SPSA without a gradient | removed |
| Graft blocks chosen from the residual between static evaluation and deep search (`KingFiles`, `Space`, `LockedPawns` and two reduced forms) | Revisited | 8.0 dev | −7 to −19 Elo (500 to 1,100 games each) | removed |
| Analysis mode compiled as a second copy of the search | Revisited | 8.0 | more test positions solved; nothing changes in games | adopted |
| Fine-tuning one expert at a time | Revisited | 8.0 | +0.7 ± 6.1 (15+0.15, 3,302) | not kept |
| Material-key correction table | Elsewhere first (Caissa, Stockfish) | 6.0 | +10.43 ± 5.57 at 10+0.1, −6.89 ± 9.01 at 20+0.2 | removed in 7.0 |
| `CorrUncert`, disagreement between correction tables as uncertainty | Revisited | 6.0 dev | −0.74 ± 7.29 (1,878) | closed |
| `TroubleMaking`, the hardest move for the opponent in a lost position | Revisited | 6.0 dev | −2.67 ± 10.66 (910) | suspended |
| `BrilliantSac`, extension of losing captures with high capture history | Revisited | 6.0 dev | +2.70 ± 7.25 (2,190); −17.83 ± 15.66 (390) | closed |
| SPLE, lazy evaluation with the parent's positional part | Revisited | 6.0 dev | offline error of the order of the margins | closed |
| `TMEvalDisagree`, more time when search and static evaluation diverge | Revisited | 7.0 dev | neutral, ±1.8 on about 40,000 games | closed |
| Policy network as a position signal (`PolicySeed`, `EntropyTM`, `PolicyEasyMove`, `PolicyRootLmr`) | Revisited | 3.6-3.8, August 2026 | −35.17, −16.7, −14.35, +1.36 ± 2.71 | closed |
| `TalStyle`, root switch to verified sacrifices | Revisited | 8.0 dev | −40 ± 18 (388) | removed |
| Move-ordering rank by mask and popcount, 16 moves at a time | Revisited | 8.0 | −0.70% cycles per node (AVX-512) | adopted |
| Causal reduction (`CausalRed`): more reduction for quiet moves away from the squares of a sibling's refutation | Revisited | 8.0 | +4.27 ± 4.13 (6,594) | adopted (512) |
| Threat extension from a failed null move (`NullThreatExt`, Botvinnik-Markoff): our form is a margin on how far the null move falls below beta, once per node, up to twice the root depth | Revisited | 7.x, 8.0 | +2.75 ± 5.5 (7,070, 7.x search); −0.42 ± 4.52 (about 5,900, 8.0 search) | closed |
| Policy network inside the alpha-beta search | Elsewhere first | 3.1 | −85, −30.56 ± 21.9, −46.89, −22.83 | closed |
| `DiverseSMP`, LMR bias per helper thread | Elsewhere first (Reckless) | 3.5, 6.0 | +4.91 ± 7.97 at 8 threads (1,700) | in 6.0 and 7.0 |
| TMv2 multiplicative time manager | Elsewhere first (Alexandria, Caissa) | 6.0 | +23.8 ± 18.2 at 20+0.2 (380) | in 6.0 and 7.0 |
| `PawnPair` input block | Elsewhere first (Pawnocchio) | 6.0 | +18.27 ± 9.94 (1,104), with self-play data | in every network since 6.0 |
| Less time when behind on the clock (`TmBehindMul`) | Elsewhere first (Stockfish after 19) | 8.0 | **+5.23 ± 4.72** at 6+0.06 (6,378) | adopted |
| Low-depth hash-move extension (LDSE) | Elsewhere first (Reckless, Stormphrax) | 8.0 | **+4.5 ± 3.3** (10+0.1, 11,604); −9.4 ± 10.9 at 40+0.4 (890) | off in the release, to be measured alone at 30+0.3 |
| King-shield malus in move ordering (`KingShield`) | Elsewhere first (inspired by Reckless) | 6.0, 8.0 | +3.53 ± 3.42 (6.0); −1.57 ± 4.32 (8.0) | adopted with the SPSA PR4 vector (+3.4 ± 3.9, about 8,100 games) |
| Attack from a safe square in move ordering (`AttackOrder`; `AttackOrderQ`, the same term for the queen) | Elsewhere first (Reckless), our target rule | 8.0 | +0.39 ± 5.42 with the outposts (4,428) | adopted with the SPSA PR4 vector (+3.4 ± 3.9, about 8,100 games) |
| Queen promotion in move ordering (`PromoOrder`) | Elsewhere first (Reckless) | 8.0 | not measured alone | adopted with the SPSA PR4 vector (+3.4 ± 3.9, about 8,100 games) |
| Advanced pawn pushes exempt from pruning (`PasserGuard`, `PasserLmr`) | Elsewhere first (Stockfish 8) | 7.0 dev | −9.04 ± 7.49; +0.30 ± 2.56 (22,000) | closed |
| Fifty-move band in the hash key and in the corrections | Elsewhere first (Reckless) | 8.0 | `CorrFiftyStep` +1.25 ± 2.87 | closed |
| Deterministic PGO training at fixed nodes | Elsewhere first (Stockfish) | 8.0 | repeatable speed measurements | adopted |
| Printed scale, +1.00 = 50% win | Elsewhere first (Stockfish 15.1) | 5.1 | display only | adopted |
| First own network, `rubicon-v1` | Elsewhere first | 4.2 | −38.91 ± 19.94 against 4.1 with a Stockfish network | replaced in 5.0 |
| Chess960 | Elsewhere first | 8.0 | no speed cost | adopted |

---

## 1. Consilium: experts by material on the first layer

**What it is.** The king-relative input block `HalfKAv2_hm` of the SFNNv16 network is replaced by four sets of
weights, one per material band (pieces on the board, kings and pawns included: up to 9, 10-15, 16-23, 24 or more).
The thresholds are quartiles of the training corpus, so each expert sees about a quarter of the positions. One expert
is active per position, so an evaluation costs almost the same as with a single block: −7.5% NPS, against −11.2% for
a first layer of 1,536 and −20% for 2,048. In the trainer each expert is a shared base plus a per-band delta that
starts at zero: the network begins as an ordinary HalfKA network and the experts separate only where the data asks
for it; this is the same scheme as the factorizer that nnue-pytorch uses for king buckets (the `^` of `HalfKAv2_hm^`),
applied to material bands. In the engine, a capture that crosses a threshold forces a refresh, and the refresh cache
is kept per band. The idea is the author's, taken from mixture-of-experts language models (many parameters, few
active per token).

**What "mixture of experts" means here.** The experts are chosen by a fixed rule, the number of pieces, not by a
learned gate. In that sense every routing scheme of an NNUE network is a mixture of experts: king buckets on the
input layer, output buckets and layer stacks after the accumulator. The name describes where the idea came from;
what is new is the routing key on the input layer, as the next paragraphs explain.

**Measured.** Trained on 28-29 September 2026 (374 epochs of pre-training and two fine-tunes, about 472 billion
positions). Version 8.0 against the 7.0 release: **+27.3 ± 8.3 Elo** at 15+0.15 on 2,000 games, of which about +15
come from the network with its re-tuned parameters ([`NETWORKS.md`](../networks/NETWORKS.md)).

**Compared with others.** The idea was reached independently by the author, and no network of this kind was found
in another engine. What exists elsewhere is of a different nature. Choosing weights by piece count is common, but
after the accumulator: Stockfish's layer stacks and the output buckets of many engines select the small output layers
by piece count. In the engines we checked, the input buckets of the first layer are chosen by the king square only,
not by material; an external engine developer confirmed on 14 October 2026 that, as far as she knew, no other engine
buckets the input layer by piece count. Stockfish 16.1
switched between two whole networks by material imbalance, which routes between separate networks rather than between
experts inside one layer. The academic work on phase-specific models uses separate full networks with AlphaZero-style
MCTS ([arXiv 2401.16852](https://arxiv.org/abs/2401.16852);
[Helfenstein et al.](https://ml-research.github.io/papers/helfenstein2024game.pdf)), not experts inside an
efficiently updatable first layer of an alpha-beta engine. Mark Tang had tried an unpublished network of phase experts
in early 2026 that ran faster but did not play stronger (independent work, see [`NETWORKS.md`](../networks/NETWORKS.md)).

**Category.** New: piece-count buckets on the input layer, that is experts by material band on the king-relative
block of the first layer, on top of the king buckets, one active per position, in a top engine and with a measured
gain. To our knowledge Triumviratus is the first engine to do this. Not new: hard routing by piece count as such
(output buckets, layer stacks) and the shared-base training scheme (the nnue-pytorch factorizer).

## 2. The PassedPawns input block

**What it is.** 96 inputs, one per passed pawn: 48 oriented squares for each side. A pawn is passed when no enemy pawn
stands ahead of it on its file or the adjacent ones and no own pawn stands directly ahead. This is a relation across
three files that the king-relative block does not express directly. The block deliberately encodes only the square:
blocked, supported and connected passers can be learned by the pairwise product of the first layer. In the engine it
is added to the threat accumulator and reuses the incremental update of pawn events.

**Measured.** Grafted at zero on `rubicon-alea-v2` and trained alone with the base frozen, about 4 epochs:
**+6.96 ± 6.56** on 2,596 games at 15+0.15, network alone (17 July 2026). It has been in every network since:
`legio-septima` (7.0) and Consilium (8.0).

**Compared with others.** Stockfish's `PP_3Wide` encodes pairs of pawns on nearby files, not the passed state. None of
the nine engines read has passed-pawn inputs. The only precedent found is an unimplemented note in Viridithas's ideas
file ("passed pawns by file?").

**Category.** New: to our knowledge the first passed-pawn input block implemented and measured.

**Follow-up in 8.0: passed pawns with their relations.** The block above encodes only where each passed pawn stands,
on the assumption that the first layer can combine it with the other inputs. On 9 October a regression of the
difference between Consilium's static evaluation and the engine's deep search, on 3,888 positions, found that the
network still undervalues some passed pawns in endgames: those outside the enemy king's square and connected ones.
Two blocks were written for this and grafted on the finished Consilium with the rest of the network frozen
([`DEVELOPMENT_8.0.md`](../development/archive/DEVELOPMENT_8.0.md)
§37–38):

- **`PassedRel`** adds, for each passed pawn, a second row with three relations: outside the enemy king's square,
  connected to another passed pawn, free path to promotion. It is cheap to train and showed a small positive signal in
  endgames (+1.8 ± 4.1 at 20+0.2 on endgame openings, 2,484 games), but each passed pawn now costs two rows, and the
  relation that follows the enemy king changes on many king moves: +1.5% cycles per node in the middlegame and +2.9%
  in endgames.
- **`PassedState`** replaces the `PassedPawns` rows instead of adding to them: one row per passed pawn, in one of 100
  states (what stands on the square in front of it, protection by a pawn, a connected passed pawn, the opponent's
  material class, and in pawn endgames whether the enemy king can catch it). At the start every state equals the old
  row, so the network evaluates exactly as before; training moves the states apart. Trained alone it did not gain
  (−2.1 ± 3.7 at 60,000 nodes per move on endgame openings, 4,320 games). When the layers after the accumulator were
  also trained, with the first layer still frozen, it gained +3.3 ± 5.0 (2,190 games) at fixed nodes and +2.9 ± 3.7 at
  25+0.25 on endgame openings, which pays its speed cost of about 2.0% / 2.5% cycles per node.

Neither block entered 8.0. On ordinary games (UHO book), against the same engine built without graft code, PassedRel
scored −2.9 ± 7.5 over 2,034 games and PassedState lost between −4.8 and −16.5: the retrained layers hurt the positions
without passed pawns, and the extra rows cost more depth on a machine with less memory bandwidth.

Two lessons follow. The relations of passed pawns are mostly already available to the network through the square
encoding and the pairwise product of the first layer, as the 6.0 design assumed: what remains is worth a few Elo in
endgames, close to the cost of computing it. And when a new block carries information the network did not have, the
frozen layers after the accumulator may be unable to use it: the gain of `PassedState` appeared only when those layers
were allowed to adapt, while the block itself hardly changed.

**Category.** New, as an extension of the `PassedPawns` block (no engine with passed-pawn inputs was found).

## 3. Correction history per expert (CorrPhase)

**What it is.** The keys of the pawn and minor-piece correction tables are mixed with the material band, that is with
the expert of the network: each expert learns its own error. A first test (−5.1 ± 8.1) was distorted by a prefetch
that used the unmixed keys; an analysis made before closing the idea found it.

**Measured.** **+3.2 ± 3.8** on 8,634 games at 12+0.12, adopted on 6 October 2026. A diagnostic build shows that the
corrections remove 58.8% of the evaluation error with `CorrPhase` against 57.3% without, the gain coming from
endgames with nine pieces or fewer.

**Compared with others.** Correction history began in Caissa (October 2023) and is indexed today by pawn structure,
minor pieces, non-pawn material and previous moves. Caissa and Stockfish also keep a table keyed by the material
configuration ([Stockfish PR #5556](https://github.com/official-stockfish/Stockfish/pull/5556)), a correction for each
material signature on its own. Reckless splits its tables by the fifty-move counter. No engine was found that splits
the pawn and minor-piece corrections by material band, so that each structure is corrected separately for each
expert of the network.

**Category.** New in its form, and tied to Consilium; corrections conditioned on material alone are known.

**The search margins per expert.** The same idea, applied to the search: the reverse futility multiplier, the quiet
futility margin and the late-move reduction each get an offset per expert of the network (`RfpExp0-3`, `FutExp0-3`,
`LmrExp0-3`, the four material bands of Consilium), so that each phase of the game is pruned with its own margins.
An SPSA (EXP1) moved mainly the reductions of two experts, by about a tenth of a ply; the other offsets stayed near
zero. Three SPRTs of the tuned values against zero offsets give +1.6 ± 4.3 over 6,562 games, and the tree at fixed
depth shrinks by 2 to 6%. The measure does not exclude zero. The values were adopted on 12 October 2026 because the
offsets per expert are part of the design, a structure we want to keep and to tune further: the SPSA ran with
hyperthreading and with the old clock (DEVELOPMENT_8.0 §45), and a new SPSA will continue from these values.
Search parameters conditioned on game phase or material exist in many engines; tying them to the expert of the
network that evaluates the position was not found elsewhere. **New in its form**, like `CorrPhase`.

**The reduction slope per expert.** The offsets above shift a level: `LmrExp` adds the same fraction of a ply to
every late move, at depth 3 as at depth 20. On 13 October 2026 we asked whether EXP1 had been looking for a slope
rather than a level, and added a scale per expert of the base reduction (`LmrScaleExp0-3`, in 1024ths, applied to the
logarithmic table before the other adjustments), together with a slope per expert of the quiet futility margin
(`FutDepthExp0-3`). The first test follows the direction of EXP1: experts 1 and 3 (10 to 15 pieces, and 24 or more)
reduce about 9% more, expert 0 (9 pieces or fewer) about 6% less. At 8+0.08 it measured +1.1 ± 3.7 Elo over 9,118
games: a small effect at best, which a single test cannot resolve. In an SPSA with the other margins per expert (EXP2,
43 parameters) neither slope found a clear gradient; the futility slope was removed. A measure of the defence against
Stockfish 19 then pointed at expert 1 (more errors with 10 to 15 pieces), and the reduction scale of experts 1 and 2
is being tested again in that direction, against less reduction (`LmrScaleExp1=-128` alone: 0.0 ± 6.0 over 3,160).
**New in its form**, as an extension of the margins per expert.

## 4. Move ordering with chess knowledge

Written on 8 October 2026, off by default, then tuned in an SPSA of 46 search parameters at 20+0.2. At hand-set
values no term moves the result, because an ordering bonus acts together with the histories and the thresholds that
separate good quiet moves from bad ones ([`DEVELOPMENT_8.0.md`](../development/archive/DEVELOPMENT_8.0.md) §34). The SPSA vector, with all
these terms on, was checked against the defaults (+3.4 ± 3.9 over about 8,100 games) and baked on 9–10 October
(§38); the terms are now active in games.

- **Knight outposts (`OutpostOrder`, `OutpostSafe`), the author's idea.** A bonus for a knight move to a square on
  ranks 4-6 defended by an own pawn and out of reach of every enemy pawn, now and later; with `OutpostSafe` also
  without an enemy bishop of the square's colour. +0.21 ± 3.93 on 8,244 games at a hand-set value. Outposts are a
  classical evaluation term, but none of the nine engines read uses them in move ordering, and no example was found on
  the web. **New.** The bishop variant (`OutpostOrderB`) uses the same mask and is not presented as new.
- **Capture of a bishop of the pair (`BishopPairCapt`).** A bonus in the tactical score for capturing a bishop when
  the opponent holds the pair. No example in the move pickers read. **New.**
- **Passed-pawn push (`PassedPush`).** A bonus for pushing a passed pawn from a given rank to the seventh, onto a
  square where it does not lose material. Special treatment of passer pushes is classical (extensions, seventh-rank
  pushes in quiescence); as an ordering bonus no example was found. **Revisited.**

The king-shield, safe-attack and promotion terms of the same batch come from Reckless and are listed in
[section 9](#9-done-elsewhere-first).

## 5. The first layer neuron by neuron, per expert

**What it is.** A build with `-DTRIUMV_NEURONS` records, for each evaluation of the search, the outputs of the feature
transformer (clipped ReLU and pairwise product, 512 pairs per perspective) separately for each material band, that
is for each expert, together with a sample of whole output vectors annotated with the piece count and the material
difference. Two questions: are there dead, saturated or duplicate neurons (wasted work, so speed at equal strength
with a narrower layer), and does the network build its own material counters (which bears on the division by
material). The starting point was a neuron-by-neuron analysis of a Stockfish network shared in the Stockfish
community; a separate analysis per expert needs a network with experts.

**Measured (9 October 2026).** 12,000 positions, 3,000 per band, from about 7,000 games against many different engines
and from varied openings, 500,000 nodes each: **2.58 billion evaluations**. A first run on 300 positions per band had
given the same picture.

| band (expert) | dead (< 0.1%) | rare (< 1%) | saturated | mean activity | duplicates (r > 0.95) |
|---|---:|---:|---:|---:|---:|
| up to 9 pieces | 0 | 0 | 0 | 15.5% | 0 |
| 10-15 | 0 | 0 | 0 | 14.5% | 0 |
| 16-23 | 0 | 0 | 0 | 14.0% | 0 |
| 24 or more | 0 | 14 | 0 | 12.9% | 0 |

- **Nothing to prune.** No pair is dead in every band, none is saturated, none duplicates another. Consilium uses all
  512 pairs, so removing neurons would not make it faster.
- **The experts specialise by intensity, not by selection.** Every expert switches on nearly every pair (overlap 0.97
  to 1.00 between bands), but how often differs: 306 pairs change their activation frequency by at least a factor of
  2 between the most and the least active band, 102 by a factor of 4, 33 by a factor of 10 (for example a pair active
  in 26% of endgame evaluations and almost never in the opening). The endgame expert has 85 "preferred" pairs
  (activity at least twice the mean of the other bands), the opening expert 29. Neighbouring bands have similar
  activity profiles (correlation 0.85-0.92), the extremes do not (0.43).
- **Material counters in fixed places.** A few pairs follow the material difference in every expert (|r| up to 0.83),
  in the same positions of the layer: consistent with training as a shared base plus deltas.

The specialisation by intensity is the measured fact that could justify a different width per expert in the next
network.

**Category.** Revisited: a known analysis technique, applied for the first time, to our knowledge, to a network
divided by phase.

## 6. Other new ideas, not adopted

- **Helper threads specialised by material regime (`SmpAff`), 8.1 development, the author's direction.** In Lazy SMP
  every thread searches the same root with the same rules and the threads help each other only through the shared
  hash table. Here the main thread is unchanged and each helper takes a range of material regimes, read from the two
  experts of the Consilium network along the line (their mean, in the ranges 0-2, 3-5 and 6-7, assigned in turn); in
  the nodes of its range the helper reduces half a ply less (`SmpAffLmr`; offsets for the other pruning rules exist
  and are at zero). The network is the same on every thread; the shape of the tree differs, so each helper goes
  deeper in its kind of position and the main thread finds those results in the table. Helpers with different search
  parameters are known (`DiverseSMP`, after Reckless, in §9); the division of the work by the experts of the network
  needs a network with experts by material. From 8 threads the helpers split the eight regimes into up to eight
  groups. Measured: +0.54 ± 3.42 over 8,940 games at 4 and 8 threads, +1.9 ± 13.1 over 544 games at 20 and 32
  threads. **New in its form**; the measurement is still open (neutral so far, the runs at many threads are short).
- **Expert-boundary ideas (PhaseEdge), 8.0.** A capture that changes the active expert must clear an extra margin
  (−2.5 ± 10.0, removed); "improving" is not computed between evaluations of different experts (−14.1 ± 19.4,
  removed); a capture that crosses a boundary is neither pruned nor reduced more (+1.3 ± 2.3 on 24,292, neutral, off).
  These ideas cannot exist without a network with experts. **New**, none adopted.
- **Blending two experts at a boundary, 8.0 development.** Near a boundary between two experts (9|10, 15|16, 23|24
  pieces), the evaluation of the neighbouring expert is computed as well and the two are averaged, to smooth the jump
  of the evaluation when a capture changes the expert. At fixed nodes +5.27 ± 7.86 (1,978), but the second
  evaluation costs 15 to 18% of the cycles per node: −24.23 ± 11.12 at 8+0.08 (1,034); a variant only at depth 3 or
  more (+3.9% of cycles) −3.06 ± 9.59 at 15+0.15 (1,248). It needs a network with experts. **New**, removed; the full
  record is in [`archive/EXPERT_BLEND_8.0.md`](../development/archive/EXPERT_BLEND_8.0.md).
- **Correction from the opponent's plan (`CorrContOppW`), 8.0, the author's idea.** The continuation correction
  corrects the static evaluation with a table indexed by pairs of moves: the row is chosen by one of our earlier moves
  (two and four plies back), the cell by the opponent's last move, so each cell learns how far the evaluation is off
  after "our move, then this reply". `CorrContOppW` adds a term from the same table whose row is chosen by the
  opponent's previous move (three plies back) and whose cell is the opponent's last move: the pair is two consecutive
  moves of the same side, a fragment of the opponent's plan, instead of our move and its answer. It is updated with the
  other continuation terms (step `CorrContOppUpd`) and weighted by `CorrContOppW`. The version with the opponent's last
  move alone, without the earlier one, was closed (−7.6 ± 7.8 at 20+0.2). The cells carry information (correlation
  +0.24 with the residual error, as much as the existing pair term), at a cost of about 1% of the speed. Measured at the
  existing weight −5.2 ± 12.7 (742) and at the regression weight −4.8 ± 8.7 (1,806). Stockfish and Reckless use only
  pairs that contain one of our moves. After the input/output fix of 12 October 2026 it was measured again at the
  existing weight: at 5+0.05 the table stays too sparse (−8.0 ± 7.7), and all runs at 8 seconds or more pooled give
  +0.6 ± 3.0 over about 13,300 games. In the correction SPSA of 13 October (CORR1p2) the weight stayed on, near 4,550.
  On 14 October it was measured again on the release-candidate build at 8+0.08: +0.5 ± 3.0 over 14,426 games, neutral, and it stays off. During that test the two processor sockets of the test machine disagreed: the build with the term lost about three times more depth per move on the socket with half the memory bandwidth (−0.04 against −0.01 plies), because the extra table read missed the cache under load. A prefetch of the continuation-correction cells, issued when the move is made, removed the difference; Stockfish has had the same prefetch since August 2026 (commit 4150d22b86), which our rewrite of the search had left out. On an idle machine the term costs nothing in the middlegame and about 0.5% of the cycles in the endgame. **New**, neutral.
- **`Mobility` input block, 8.0 development.** One feature per knight, bishop, rook and queen (oriented square and four
  mobility buckets, 2,048 inputs). Only its cost was measured, with random weights: −16.2% NPS, because mobility
  changes with every move. Archived on 29 September 2026 without training. No network of the nine engines has
  mobility inputs. **New as an idea**, with no measurement of strength.
- **Pawn break in a closed structure reduced less (`RdPawnBreak`), 8.1 development, the author's idea.** In a closed
  position the plan of both sides turns around the one or two pawn pushes that can open the structure, and a reduced
  search tends to miss them because the gain comes several moves later. The structure counts as closed when at least
  three pawns face an opposing pawn on the square directly in front of them (same file, blocked). In such a position a
  quiet pawn push that lands on a square attacking an enemy pawn, that is a move offering a pawn exchange that opens a
  file or a diagonal, has its late-move reduction lowered by `RdPawnBreak` (in 1/1024 of a ply), never below zero; the
  lever is signed, so a negative value tests the opposite direction. The nine engines read do not condition any
  reduction on the pawn structure. The closest examples are a move-ordering bonus in Reckless for a pawn push to an
  advanced square attacked by an enemy pawn, which applies in every structure and only to ordering, and the general
  advice to exempt pawn moves from late-move reductions (chessprogramming.org). First measurements: 2048 at 7+0.07
  without hyperthreading (UHO book) +1.3 ± 9.0 (1,418 games); 3072 on the 8-move book −3.7 ± 9.2, with the search
  clearly deeper, a value judged too strong; 2048 at 8+0.08 with hyperthreading (UHO) **+0.19 ± 5.74** (3,724). The
  two runs at 2048 pooled: +0.6 ± 4.8 over 5,142 games. In the games of the test, 6% of the positions and 30% of the
  games have a closed structure with a break available, but the breaks actually played hardly change (597 with the
  lever, 569 without, in about 10,700 moves each): the search already finds them, so the lever adds no information.
  **New**, neutral, off.

## 7. Known ideas revisited in a different form

These entries take a known idea and change its form, its scale or its use. Those in use:

- **Surprise rule, 8.0, the author's idea.** If the opponent does not play the reply our principal variation
  predicted, the time for the move is multiplied by 1.2, so the time that the stability rule saves on predicted
  replies goes to uncertain moves. The starting point was a remark by Mark Tang: Stoofvlees answers very quickly
  between two moves, even at long time controls. **+6.23 ± 6.75 at 40+0.4** (2,174), neutral at 16+0.16; after a
  surprise the engine spends 12% more time and searches 0.35-0.4 plies deeper. A factor on the predicted reply
  exists before: Caissa publishes it in both directions (0.915 on the predicted reply, 1.132 otherwise), and our own
  6.0 and 7.0 used it as a declared port from Caissa ([`archive/DEVELOPMENT_6.0.md`](../development/archive/DEVELOPMENT_6.0.md),
  "predicted-move factors"). The 8.0 form keeps only the surprise direction, applied on top of the time manager
  rewritten on 4 October, and the measured dependence on the time control is ours.
- **Guard on LDSE (`LdseMax`).** At most one low-depth hash-move extension per line, from a measurement of the tree
  (rook endgames ×5 to ×16): +6.25 ± 7.89 at 15+0.15. Neither Reckless nor Stormphrax caps it per line; limiting
  extensions along a line is known in general.
- **Cap on extensions from a negative reduction (`NegExtMax`), 8.0.** When the reduction of a move is negative, the
  reduced search goes up to two plies deeper than the nominal depth, and along one line these extensions add up as the
  LDSE extensions did before their guard. The same remedy: at most one such extension per line, counted in the node
  frame. At fixed depth the cap of 1 takes 6.6% of the nodes and 0.55 plies of selective depth; in games **+3.88 ±
  3.26 at 12+0.12** (11,384), adopted on 11 October 2026. Stockfish addressed the same growth after version 19 with a
  different rule, a limit on the ply of the extended search relative to the root depth ("Fix deep recursion"); ours
  counts the extensions along the line.
- **Time in defence and early saving (`TmDefendMul`, `TmEarlySave`), 8.0.** Two time rules taken from our own loss
  analysis against Stockfish 19, which placed most of the loss in defence with a short clock: the time for the move
  ×1.5 while the root score lies between about −0.45 and −3.6 pawns (defending, not lost), and the optimal share of
  the clock −20% while 24 or more pieces are on the board, where the two engines play at the same level. Together
  **+7.27 ± 7.08 at 40+0.4** (2,150), adopted on 11 October 2026; which of the two carries the gain is still to be
  measured. Spending time according to the score is known (Stockfish's rule for a falling score reacts to a drop
  between moves, and many engines spend less time early in the game); the absolute defensive window and the phase
  threshold come from our measurements.
- **Guard on sudden score jumps (`TmJumpGuard`), 8.0, the author's idea.** When the root score rises by two printed
  pawns or more over the previous move, the time manager may not shorten the think on that jump: the rule for a
  falling score of Stockfish and of our time manager lends time on a drop, and a sudden rise was treated as good news
  even when it came from an unconfirmed iteration. Born from a lost tournament game (+0.37 to +3.68 at depth 19,
  played after 1.7 s with 48 s on the clock). It triggers on 0.25% of the moves; −2.52 ± 7.33 at 8+0.08 (2,210), flat.
  Kept on as a guard, not as a measured gain.
- **Passed-pawn pushes in endgames (`PassedPushRed`).** With little material, a push to the sixth or seventh rank is
  reduced less and never pruned, because the network sees passers through `PassedPawns`: +4.4 ± 6.8 on an endgame
  book. Stockfish 8 exempted advanced pawn pushes from shallow pruning.
- **Passed-pawn push in quiescence (`QsPasserPush`), 8.0 dev.** In endgames, below a non-pawn material threshold, a
  pawn push from the sixth to the seventh rank is searched in quiescence as a tactical move, one push per node, so
  that the horizon does not cut the move that decides a pawn race; the network judges a pawn on the seventh well
  through `PassedPawns`, but only if the search reaches it. Searching seventh-rank pushes in quiescence is a classical
  technique; Stockfish 19's quiescence searches only captures and promotions. Proposed in the search study of
  5 October; −0.77 ± 7.48 over 2,264 games at 15+0.15 and +1.1 ± 3.8 over 3,370 at 10+0.1, so on 10 October it
  received three levers of its own for an SPSA: the material threshold (`QsPushNpm`, until then shared with
  `PassedPushRed`), the exchange bound the push must meet (`QsPushSee`, until then the general one of quiescence) and
  the number of quiescence plies in which it is tried (`QsPushPly`). Tested the same day: only in the first
  quiescence ply −1.8 ± 7.2 over about 2,070 games (UHO, 15+0.15); only below 2,600 of non-pawn material +0.23 ±
  2.45 over 7,516 games on the endgame book at 12+0.12. It does not gain even in endgames: closed, off.
- **Refresh cache of the pawn blocks, 7.0.** `PawnPair` and `PassedPawns` depend only on the pawn bitboards and the
  orientation, and refreshes come from king moves, which leave the pawns alone; their contribution is kept with the
  full pawn bitboards as key (no collision possible). +2.8% NPS on AVX2 with an identical tree. The principle of the
  pawn hash table of classical evaluations, applied to an accumulator; Stockfish 19 has no such cache.
- **Universal executable, 8.0.** Five variants (AVX2 without PEXT, AVX2, AVX-512, VNNI-512, AVX-512 ICL), chosen at
  start-up. Each source file is compiled once per variant, the variants are joined in the final link with ThinLTO, and
  the five profile-guided profiles are merged because ThinLTO refuses modules with conflicting profiles. −0.28% and
  −0.17% cycles per node against the separate build, where one unit per variant had cost about 1%. Stockfish has
  had a universal binary since June 2026, built differently, with per-architecture LTO and renamed sections, a route
  not available with clang-cl and lld-link on Windows.
- **`tdperft` and `nnperft`.** A perft that uses the search's own move generation, make and unmake, and at every node
  recomputes from scratch every hash key, the occupancy and the piece table, and checks the pseudo-legality test used
  for hash and killer moves; and a perft that compares the incremental accumulator with a full refresh at every node.
  They validated the correctness audit (482 million nodes), Chess960 and the single board.
- **Zero-initialised graft with the base frozen, 6.0.** A new block is added at zero to a finished network (identical
  output at the start, checked with the bench) and trained alone: enough for `PassedPawns` (+7 Elo in about 4 epochs)
  and a cheap way to screen input features. Function-preserving initialisation is known in machine learning; as a
  screening method for NNUE features no description was found. In 8.0 it was used to screen six blocks chosen from the
  residual between static evaluation and deep search (passed-pawn relations, open files next to the king, space,
  blocked pawns): only the passed-pawn blocks showed a signal, and the others, which describe what the king-relative
  inputs already contain, lost 7 to 19 Elo and were removed (§2). Two limits of the method appeared: a block that
  starts from existing rows instead of zero needs a lower learning rate (at 1e-2 the training loss first doubled), and
  a frozen network can only use new information in the way its later layers already read the accumulator.
- **Residual concepts used without retraining, 8.0 development.** The same residual study pointed at concepts that the
  network underestimates (an unstoppable passed pawn, open files next to the king, space, connected passers). Without
  a new network they were tried in three engine-side forms: constant terms added to the evaluation (−6.48 ± 6.17 on
  endgames, −16.0 ± 16.0 on UHO), corrections learned during the game with keys built on the concept (pawns and king
  zones −4.68 ± 7.19; king shelter −5.63 ± 5.53; passed pawns and the king's square −5.24 ± 8.49), and search rules
  (less pruning with an open king, −0.37 ± 3.68 over about 9,300 games; less reduction for an unstoppable passer
  push, −2.07 ± 3.42 over about 4,300 endgame games; an SPSA of the two found no gradient). Constant terms and
  correction histories are known; keying a correction on a concept chosen from the network's residual is our form.
  All removed: what the network misses here is not recovered by the search (`DEVELOPMENT_8.0.md` §44).
- **Analysis mode, 8.0.** With `UCI_AnalyseMode` or `go infinite` the search prunes and reduces less at depth; the
  search is compiled twice, a game copy with every analysis term a compile-time constant and an analysis copy, so
  games are unaffected.
- **Move-ordering rank by mask and popcount, 8.0.** The rank of each move is found by comparing it with 16 others at a
  time and counting with a popcount (−0.70% cycles per node on AVX-512). Stockfish uses a vectorised insertion sort.

- **Causal reduction (`CausalRed`), 8.0, the author's idea of June 2026, rewritten on the current search.** When one of
  our quiet moves fails low, the opponent's move that refutes it (the beta cutoff in the child) marks the squares where
  the problem lies: where the refuting piece comes from, where it lands and what it attacks from there. Later quiet
  siblings that neither start from nor land on those squares usually change nothing against that refutation, and are
  reduced by an extra fraction of a ply; they are never pruned, and a reduced search that beats alpha is repeated at
  full depth as usual. The closest precedent is Stockfish 1.x-2.x, which pruned moves not connected to the threat
  found by the null move; here the threat is the actual refutation of a sibling and the moves are reduced, not
  pruned. Adopted at 512 (half a ply) on 10/10/2026: SPRT at 25+0.25 on UHO, two pooled runs, +4.27 ± 4.13 Elo over
  6,594 games (LLR 1.17), with the candidate 0.25 ply deeper on average on both sockets. The test was stopped before
  the usual 20,000 games because zero was already excluded; the value stays a parameter of the closing SPSA runs,
  which can only refine it.

Those tried and closed, each a variant of a known idea: `CorrUncert` (the
disagreement between correction tables as uncertainty; Stockfish uses the size of the total correction),
`TroubleMaking` (in a lost position, the move that cost the search almost as many nodes as the best, hence hard to
refute; self-play cannot measure it, because a copy of the engine finds the refutation as easily as it does),
`BrilliantSac`, SPLE, `TMEvalDisagree` (the nearest precedent is Stockfish 15.1's `complexPosition`), fine-tuning one
expert at a time, `TalStyle` (ShashChess has style personalities acting on thresholds and evaluation; the root switch
to verified sacrifices was not found), and the uses of the policy network as a signal on the position
(`PolicySeed`, `EntropyTM`, `PolicyEasyMove`, `PolicyRootLmr`).

## 8. Speed work

Most of the speed of 8.0 comes from engineering on techniques that other engines also use, and we say so. Checked
against the sources: the early prefetch of the child's hash entry from an estimated key, the prefetch of the
correction entries, the AVX-512 compaction of the move list, the vectorised slider attacks and the large pages for the
search state are all in Stockfish; keeping the minor and major piece keys incrementally follows Stockfish; finding the next attacker of the
static exchange evaluation from a bitboard is what Reckless does. These are **done elsewhere first**, written in our
code and measured one by one.

What is ours in this area is listed in [section 7](#7-known-ideas-revisited-in-a-different-form): the refresh cache of
the pawn blocks, the universal build from one unit per source file, the move-ordering rank by popcount, and the
analysis mode compiled as a second copy.

**8.1: the slider attacks of our own.** In 8.1 the vectorised slider attacks of Stockfish origin were replaced by code
of our own.
The AVX-512 kernel is new in its form. The obstruction difference itself is published (chessprogramming wiki): on one
line, the attacked squares come from the occupied squares above the slider and the highest occupied square below it.
That needs the highest set bit of a word, which vector instructions could not give before AVX-512 CD added a lane-wise
leading-zero count. Our kernel puts the four lines of a square (two diagonals, file, rank) in the four 64-bit lanes of
one register and computes them together, with no table and no byte reversal; we have not found this combination
described elsewhere. On AVX2 the kernel is our variant of the hyperbola quintessence. Block 6 rewrote the network
inference from a specification of our own, at identical evaluations (with block 7, 5.5% fewer cycles per node on
AVX-512 and 2% on AVX2): that is engineering on a known
technique, not a new idea, and it is not counted above. Some further patches (the compaction of the threat features, the passed-pawn fill, the prefetch of the threat rows of the network) have not yet been checked line
by line against every engine and are listed among the [open checks](#11-open-checks). The measurement method, with
the engine pinned to one processor and only the search threads counted, is described in
[`DEVELOPMENT_8.0.md`](../development/archive/DEVELOPMENT_8.0.md) §36.

## 9. Done elsewhere first

Ideas we use or tried that other engines published first. Several we reached on our own; the category records public
priority.

- **Low-depth hash-move extension (LDSE).** +4.5 ± 3.3 on 11,604 games at 10+0.1, adopted on 5 October. From Reckless
  and Stormphrax, with our form and values. Turned off for the release on 14 October: in a 40+0.4 non-regression test
  of the release settings it read −9.4 ± 10.9 over 890 games, and the side with it searched 0.1 to 0.25 plies
  shallower at the same time per move. It will be measured alone at 30+0.3.
- **Less time when behind on the clock (`TmBehindMul`).** The author's idea, +5.23 ± 4.72 on 6,378 games at 6+0.06.
  Stockfish has an almost identical rule in a development commit after version 19 (`5ca4bfe7`, "Decrease time usage
  if engine has time disadvantage").
- **King shield in move ordering.** Inspired by the malus of Reckless ("Resist moving king wall pawns"), ported in 6.0
  (+3.53 ± 3.42) and written again for 8.0 with a condition on the opponent's material.
- **Attack from a safe square, queen promotion in move ordering.** From Reckless; the target rule (an enemy piece,
  neither pawn nor king, of a higher class or undefended) is ours.
- **Policy network inside alpha-beta (3.1 "Hybrid", and again in August 2026).** A move-predicting network used for
  ordering and reductions. Every form lost: −85, −30.56 ± 21.9, −46.89 ± 33.99, −22.83 ± 8.20. Neural move ordering in
  alpha-beta goes back to Kocsis, Uiterwijk and van den Herik (2001) and to the Neural MoveMap heuristic. The lesson
  recorded: ordering already reaches 79.2% first-move cutoffs without a hash move, and a static prior computed at the
  parent loses against histories that adapt to the subtree.
- **`DiverseSMP`.** A small LMR bias per helper thread of Lazy SMP; the value comes from Reckless, which published it
  six weeks before our first form.
- **TMv2**, the 6.0 time manager, composed of declared rules from Alexandria and Caissa.
- **`PawnPair`**, the idea of Jonathan Hallström for Pawnocchio, later adopted by Stockfish as `PP_3Wide`.
- **`PasserGuard` and `PasserLmr`**, in Stockfish 8 as the exemption of advanced pawn pushes.
- **Fifty-move bands** in the hash key and in the corrections, from Reckless.
- **Material-key correction table** (6.0, removed in 7.0), from Caissa, also in Stockfish since 2024.
- **Deterministic profile training**, as Stockfish trains its PGO with the fixed-depth bench.
- **The printed scale** (+1.00 = 50% win), as Stockfish since 15.1.
- **`rubicon-v1`**, the first network trained by the project (4.2), on the official recipes and trainer.
- **Chess960** support.

Versions 1.x and 2.x were learning engines, built on the models of VICE and of BBC (the video series of Code Monkey
King), and have no new entries.

## 10. Analysis build (Position Solver)

Techniques of [Triumviratus 8.0 Position Solver](https://github.com/Tors3/Triumviratus/releases/tag/v8.0-possolver), the
build for hard positions (October 2026). They act only in analysis, so they are kept apart from the game entries and
from the counts of the summary. The measure here is the number of positions solved, on IQ4 (183 positions) and on the
Arasan 2026 collection (200), at fixed nodes with one thread; details in
[`DEVELOPMENT_8.0.md` §49](../development/archive/DEVELOPMENT_8.0.md). The same three categories apply. A search of the forums and of the
source of Crystal and ShashChess found no option like the first and the third entry; as above, this is to our
knowledge.

| entry | category | measured effect | status |
|---|---|---|---|
| Two-line search in analysis (`AnShadow`): the second best root move is always searched with a full window, as with MultiPV 2, while the interface sees only the lines it asks for | Revisited (MultiPV is standard; our form is a solving mode that does not depend on the interface's setting) | Arasan 157 → 170 at 1M nodes, 166 → 178 at 2M, 151 → 161 at 1 s; IQ4 level (158 → 157 at 1M, 161 → 163 at 4M) | default in the Position Solver |
| Separate analysis settings chosen at compile time, with the per-expert pruning margins of the game build switched off | Revisited | IQ4 at 1M nodes: release 148, the same build with the game margins off 156 | the Position Solver |
| Preference for the more direct win (`AnDirect`): when both exact lines win clearly (above +3), the direct one (check, capture, promotion, or a sacrifice: a quiet move that loses material by static exchange) is shown and played if the higher one is not direct and the gap is at most 4 pawns; the search tree is unchanged | New | same tree: IQ4 163 → 166 at 4M nodes (three solutions added, none lost), Arasan 178 unchanged; without the sacrifice in the definition, 164 (a quiet sacrifice was lost) | default in the Position Solver (v3) |
| The second line paused in quiet positions (`AnShQuiet`): from root depth 10, when the two best moves are quiet and the score has stayed within 15 cp for three iterations, the search goes on with one line | New | eight collections at 1M nodes +20 (SBD, quiet key moves, 98 → 110); higher budgets +7 | default in the Position Solver (v4) |
| Depth instead of pruning in analysis (`AnDeep` presets: wider futility margins and one ply less reduction from root depth 5) | Revisited, inspired by ShashChess's GoldDigger | +3 to +7 positions over three collections at 8M nodes (9 October); level with the alternatives on top of the two-line search | default in analysis mode |
| GoldDigger on our search (`AnGold`) | Done elsewhere first (ShashChess) | +11 on IQ4 over the release tree, nothing over the Pos Solver tree | option, off |
| No pruning along the previous principal line near the root (`AnCrystal`) | Done elsewhere first (Crystal) | within the noise | option, off |
| King in danger and coming repetitions (`AnKingDanger`, `AnCycle`) | Done elsewhere first (Crystal) | IQ4 +1 / −7, Arasan −4 / +2 | options, off |
| No move-count pruning near the root, captures without reduction, a cautious null move near the solution (`AnLmpNear`, `AnCaptLmr`, `AnNmpZug`) | Done elsewhere first (Crystal) | within ±3, or a loss on one collection | options, off; the null-move guard waits for a game test |
| Our other attempts: the two runners-up at the root without reduction (`AnRootAlt`), no depth cut after a new best move near the root (`AnKeepDepth`), cheaper two-line searches (`ShadowFrom`, `ShadowRed`, `ShadowMargin`), three or four lines | New or revisited, not adopted | the cheaper forms gain up to 4 on IQ4 at low budgets and lose 5 to 7 on Arasan at higher ones; the others lose or are within the noise | closed |

A diagnosis of the positions still unsolved (`tools/diag_fail.py`: a search of 64M nodes, the solution forced, and
Stockfish 19 under the same conditions) found no error of the search. On Arasan most of them are solved with more
nodes; on IQ4 most are positions won with several moves, where the key is the most direct one and Stockfish 19 chooses
the same move as Triumviratus.

## 11. Open checks

1. **Consilium and recent engines.** No engine with input buckets by material was found, but the developer channels
   are not indexed. Every statement of priority here is meant to our knowledge; corrections are welcome.
2. **Knight outposts and the bishop-pair capture in move ordering.** Nine engines and the web were checked; Obsidian,
   Koivisto, Ethereal and recent Berserk were not read line by line.
3. **Speed patches.** Threat-feature compaction, the passed-pawn fill and the threat-row prefetch, to be compared with
   every engine before any claim.
4. **Surprise rule in closed engines.** It may exist in commercial or closed engines that cannot be read.
5. **Zero-initialised graft.** Other developers probably use it without describing it.

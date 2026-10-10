# Experimental grafts on Consilium (9–10 October 2026)

A sub-chapter of the [8.0 development log](../DEVELOPMENT_8.0.md#37-experimental-grafts-on-consilium-910-october-2026).
It records two nights of work on small input blocks added to the finished network: where they came from, how they
were trained, how they were put into the engine and made cheap, and what they did in games. **Outcome (10 October):**
only PassedRel showed a positive signal and stays in the engine; KingFiles, Space, LockedPawns and the reduced forms
KingFilesQ and Space24 did not gain and were removed from the engine (§6). The engine sections below describe the
mechanism as it was built for all six blocks. The training material
(scripts, trainer patch, reference implementations, label tools) is in the training repository under
`04_consilium/graft_passedrel`, `04_consilium/graft_blocks` and `04_consilium/residual_labels`.

## 1. Where the idea came from

The residual analysis of 9 October ([FUTURE_DIRECTIONS.md](../FUTURE_DIRECTIONS.md), proposal P1) compared the static
evaluation of Consilium with the score of a deep search of the engine on 3,888 positions, and regressed the
difference on about thirty chess concepts at equal score, material and phase. The joint estimate pointed at groups of
positions where the network is systematically wrong in the same direction:

| concept | phase | residual | t |
|---|---|---|---|
| passed pawn, per rank | endgame | +8 cp | 5.6 |
| passed pawn outside the enemy king's square | endgame | +18 cp | 2.2 |
| connected passed pawns | middlegame | +18 cp | 3.3 |
| open files next to the own king | middlegame | −14 cp | −4.5 |
| space (safe squares behind the pawn chain) | both | +6 / +8 cp | 5.9 / 4.0 |
| blocked pawns, times the score | middlegame | +11 | 8 |

A positive residual means that the search values the feature more than the network does.

## 2. The blocks

Each block is a set of extra inputs that depend only on pawns, kings and the presence of queens (and, for one bit,
on the squares in front of a passed pawn). Such inputs change on few moves, so the incremental update of the first
layer can be cheap.

| block | inputs | active at most | content |
|---|---|---|---|
| PassedRel (PassedPawns v2) | 768 | 16 | every passed pawn, by square, with three bits: unstoppable (enemy king outside the square), connected (another passer on an adjacent file), clear path (no piece in front up to promotion) |
| KingFiles | 768 | 6 | for each king, the three files around it: least advanced own pawn (none, second rank, third rank, further) × enemy pawn on the file × enemy queen on the board |
| KingFilesQ | 96 | 3 | KingFiles reduced to the perspective's own king, and only while the opponent has a queen |
| Space | 48 | 24 | the classical definition: squares on files c to f, ranks two to four, not occupied by an own pawn and not attacked by an enemy pawn, with a bit for "behind the own chain" |
| Space24 | 24 | 24 | Space without the "behind" bit |
| LockedPawns | 48 | 8 | pairs of pawns blocked head on, by the square of the perspective's pawn |

KingFilesQ and Space24 were derived from the cost analysis of section 4: they keep the part of the information that
matters most and change far fewer inputs per move. A quarter of the KingFiles rows can never be active (the mirror
puts the perspective's own king on files a to d), which KingFilesQ also removes.

The first block of this kind, PassedPawns (96 inputs, a passer by square), was grafted onto the 7.0 lineage and is
part of Consilium. The new blocks follow the same method: the block is appended after the existing inputs with zero
weights, so the network starts with exactly the old evaluation (same `bench`), and then only the block is trained,
with every other parameter frozen. The blocks are kept in a fixed order in the network file; a network carries any
subset of them, and the engine switches on the ones the file contains.

## 3. Training

One RTX PRO 5000 (48 GB) on a rented machine, relabelled Leela data (six files, 126 GB), λ 0.75 as in every network
of the lineage, batch 262,144, epochs of 100 million positions.

**Throughput.** The first launch reached 0.5 to 0.9 million positions per second with the GPU at 56%. More loader
workers changed nothing, and neither did a doubled batch. The limit was the copy of every batch into pinned memory
before the transfer to the GPU, which ran on a single thread because the trainer was started with one torch thread (a
setting that was right when the loader was the bottleneck, two months earlier). With eight torch threads the rate
went to 2.3 million positions per second and the GPU to 97–99%: one epoch in 43 seconds, a graft of 30 epochs in
about 25 minutes.

**Learning rate.** The first PassedRel graft (block learning rate 1e-3, 60 epochs) learned weights seven times smaller
than those of the original PassedPawns block. With a learning rate of 1e-2 (30 epochs) the weights grew three times
larger, peaked around epoch ten and then settled as the one-cycle schedule decayed; the last checkpoints differ by 4%
and 1% (cosine 0.999 and 1.000), so the block had converged. KingFiles at 1e-2 followed the same pattern (cosine
between consecutive checkpoints 0.81, 0.87, 0.89, 0.91 over epochs 4 to 10). All later grafts use 1e-2 and 30 epochs.

| block | mean absolute weight | largest weight | weights rounded to zero in int8 |
|---|---|---|---|
| PassedPawns (part of Consilium) | 0.044 | 0.50 | 6.1% |
| PassedRel, learning rate 1e-3 | 0.0066 | 0.13 | 26.3% |
| PassedRel, learning rate 1e-2 | 0.0173 | 0.50 | 12.1% |
| KingFiles, learning rate 1e-3 (40 epochs) | 0.0048 | 0.15 | |
| KingFiles, learning rate 1e-2 (10 epochs) | 0.0195 | 0.50 | |

Int8 weights are stored as round(256 w), clipped at ±127; no weight of the blocks reaches the clip.

## 4. The engine side

### 4.1 Format

The blocks are optional segments after PassedPawns in the first-layer weight table, outside the locality permutation
of the other rows. The network file announces its blocks through its hash: one hash for every subset, in the fixed
order, so the engine accepts Consilium (no blocks) and any graft network with the same binary. A block absent from the
file keeps zero rows and costs nothing. An `exportgraft <mask> <file>` command writes the loaded network with the
requested blocks grafted at zero, which is how the zero-weight test networks below were made.

### 4.2 Incremental update

The first engine version of PassedRel recomputed the block's entries at every move and compared the old and new
lists once per perspective. It cost 0.9% of cycles per node even with the block switched off, because its data made
every state of the move stack larger, and with the block on (a network with the block at zero weights, so the tree
is identical) +3.7% cycles per node in the middlegame and +5.9% in endgames.

It was rewritten as one mechanism for all blocks, in four steps measured one after the other:

1. **Entries outside the move state.** Every block produces perspective-independent entries (block number in the high
   bits), generated in ascending order. The difference between the entries before and after a move is computed once
   per move by a linear merge and kept in a separate array of the move stack, so the state itself has its old size;
   the network finds a state's data by the state's position in the stack (a byte that fits in existing padding).
2. **Entry lists kept per position.** Each position keeps its list, rewritten only when a move can change it, so the
   lists are never rebuilt from scratch before an evaluation. A first version still rebuilt the starting list at
   every evaluation; that alone cost +5% in the middlegame with PassedRel and +26% with all four blocks.
3. **Only what can change.** PassedRel recomputes from scratch only when the set of passed pawns changes. A move of
   the enemy king touches only the "unstoppable" bit, and only of the passers whose square the king enters or
   leaves (an inverse table of the king-to-square relation); a piece entering or leaving a square in front of a
   passer touches only the "clear path" bit. With PassedRel alone the decision "nothing to do" is taken on two
   bitboards (passed pawns and the squares in front of them) held in registers. KingFiles keeps a 13-bit signature
   per king (file, enemy queen, three pawn states per file); entries and differences are written from the signature
   only when it changes. Space and LockedPawns depend only on pawns: they follow the path of PawnPair and PassedPawns,
   with differences taken from the pawn bitboards before and after the move (exclusive or, no sorting) and their rows
   inside the pawn-structure cache of the refresh, where a cache hit costs nothing.
4. **The base state folded into PassedPawns (PRB1).** For each passer square one of the eight states is chosen as
   base, its row is added to the PassedPawns row of the same square (both blocks see exactly the same passers), and
   subtracted from the eight PassedRel rows of that square. The evaluation is identical bit for bit; the base row is
   zero and never added. The converter rewrites a trained network; the choice of the base uses a histogram of the
   states met in search. On the PassedRel network of section 5 the int8 range is the limit: the sum of the PassedPawns
   row and the base row must stay within ±127, and since the PassedPawns weights are large, many squares had to take a
   less frequent base or none (13 of 96). 18% of the PassedRel rows disappear.

### 4.3 Verification

* With every block at zero weights the `bench` is 269775 for every combination of blocks, the same as Consilium.
  Networks trained with the first engine version give the same `bench` on every later version (182369, 326120,
  278773), and the converted PRB1 network gives the same `bench` as the original (278773).
* With random weights in the blocks, `nnperft` (incremental update against full refresh at every node) gave 0
  differences over 112.8 million evaluations from 115 middlegame and endgame positions, with all blocks on and with
  PassedRel alone, using a build that also checks every entry list, every signature and every difference against a
  computation from scratch and stops at the first disagreement.
* That check found one error in the fast path of step 3 before any game was played: a king capturing a pawn is a pawn
  event, but it also moves the enemy king of the passers of the other colour, and the "unstoppable" bit was not
  rewritten. The fix is one condition; the network evaluations of the trained graft networks then matched the
  earlier versions again.

### 4.4 Cost

Hardware counters, cycles per node against Consilium on the same binary, engine pinned to one core, network with the
block at zero weights (same tree), three or four rounds, MinGW build without profile-guided optimisation, so the
comparison is between code versions:

| version | PassedRel, middlegame | PassedRel, endgame | KingFiles, middlegame | KingFiles, endgame |
|---|---|---|---|---|
| first version | +3.66% | +5.94% | | |
| step 1 and 2 | +3.78% | +5.01% | +7.94% | +8.49% |
| step 3, first pass | +2.57% | +3.23% | +7.32% | +8.01% |
| all steps (merged build) | +1.99% | to be measured | | |

On the merged build the trained network (learning rate 1e-2) costs +1.83% in the middlegame, and the same network in
the PRB1 form +1.77%; the previous version, measured in the same session, +3.44%. In a game at 8+0.08 on endgame
openings the side with the graft searched 0.08 plies less on average than Consilium, on both sockets. The remaining
cost is mostly rows of weights: a passer that changes state, and the block's rows in every full refresh. The endgame
measurement was interrupted to start the game test and is still to be taken.

The final figure must be taken on the release build with a profile trained on a network that has the block: the
present profile is trained on Consilium, so the code of the blocks is cold.

**Deterministic PGO builds (10 October).** Eight single-variant AVX-512 builds (`build_universal.ps1 -Only 3`), each
profiled by the deterministic trainer with its own network, three rounds, cycles per node against Consilium:

| configuration | middlegame | endgames |
|---|---|---|
| PassedRel, PRB1 form, with the third round of patches (v1 rows taken from the PassedRel difference, inline refresh) | +1.51% | +2.88% |
| the same without those two patches | +1.92% | +3.56% |
| the same with delta rows (one row for a one-bit state change) | +1.50% | +2.70% |
| PassedRel without adding its rows (diagnostic, zero network) | +0.76% | +1.20% |
| PassedRel, zero network, with its rows | +1.84% | +3.02% |
| Space24 | +3.19% | +3.37% |
| KingFilesQ | +2.18% | +1.77% |
| LockedPawns | +0.56% | +0.77% |

The baseline varies by about 0.9% between rounds, so differences below 0.3 points are not resolved. In PassedRel the
rows are the larger part in endgames (about 1.7 of the 2.9 points) and the fixed work of the incremental update the
rest; a smaller cost therefore requires fewer rows, not faster bookkeeping. The MinGW figures above are kept for the
comparison between code versions only.

## 5. In play

| test | conditions | games | Elo |
|---|---|---|---|
| PassedRel, learning rate 1e-3, 27 epochs | 10+0.1, first engine version | 1,422 | −16.1 ± 9.5 |
| PassedRel, learning rate 1e-3, 60 epochs | fixed depth 16 | 356 | −12.7 ± 17.2 (stopped) |
| PassedRel, learning rate 1e-2, 30 epochs | 40,000 nodes per move, UHO openings | 2,806 | −1.2 ± 7.0 (stopped) |
| PassedRel, learning rate 1e-2, 30 epochs | 60,000 nodes per move, endgame openings | 2,366 | +1.2 ± 4.8 (stopped for the speed work) |
| PassedRel, learning rate 1e-2, PRB1 form, merged engine | 8+0.08, endgame openings | 1,052 | −2.0 ± 6.8 (stopped: too sensitive to speed) |
| PassedRel, learning rate 1e-2, PRB1 form, merged engine | 20+0.2, endgame openings | 2,484 | **+1.8 ± 4.1** (stopped for the cost work; to be completed) |
| Space24, network at epoch 9 (mid-training) | 12+0.12, UHO openings | 204 | −81.5 ± 22.7 (stopped) |
| Space24, final network | 12+0.12, UHO openings | 514 | −12.2 ± 15.6 (stopped) |
| Space24, final network | 40,000 nodes per move, UHO openings | 1,088 | −7.7 ± 11.5 (stopped) |
| KingFilesQ, final network | 12+0.12, UHO openings, PGO build | 1,128 | −7.1 ± 10.7 (excluded) |
| LockedPawns, final network | 12+0.12, UHO openings, PGO build | 628 | −19.4 ± 14.5 (removed) |

Fixed nodes remove the speed of the block from the comparison and measure the evaluation alone. The endgame openings
(12 to 18 pieces, 68% with at least one passed pawn, no draw adjudication) are where the block can matter; there the
two sides were even after the first 2,366 games, with 140 favourable and 132 unfavourable game pairs.

With the first learning rate the block learned little and that little cost Elo: it moved the evaluation by about
2 cp on average, with no consistent direction. With the higher learning rate the loss in games disappeared; whether
a small gain remains is not settled. KingFiles was retrained at 1e-2: its weights peaked at epoch 8 (mean 0.0199) and
then fell by half while the learning rate decayed, ending at 0.0098 with 40% of the int8 weights at zero, so the data
do not support large weights for it; it will be tested only in its reduced form, KingFilesQ. Space at 1e-2 learns the
largest weights of all blocks (mean 0.024 at epoch 10, 7% at zero in int8). Space, LockedPawns, Space24 and KingFilesQ
are queued on the training machine.

## 6. Outcome

* **PassedRel stays.** At 20+0.2 on endgame openings it searched to the same depth as Consilium on both sockets
  (23.06 against 23.05 and 23.03 against 23.03 plies) and scored +1.8 ± 4.1 over 2,484 games; combined with the
  fixed-node result the estimate is about +1 to +2.5 Elo in endgames, positive with a probability of about 90%. The
  game test is to be completed once the cost is lower.
* **The other blocks were removed** from the engine on 10 October; the trainer keeps their definitions. The blocks that
  are active in most positions shared one pattern in training: the weights rose until epoch 5 to 10 and then shrank
  while the learning rate decayed, and the final networks did not improve on the base. Our reading is that what they
  describe (pawns near the king, space, blocked pawns) is already available to the base network through the
  king-relative piece-square inputs and the pawn-pair block, while the relations of a passed pawn (unstoppable,
  connected, free path to promotion) are not. The residual signal that motivated KingFiles probably reflects concrete
  attacks that the deep search finds, which depend on where the pieces stand and cannot be captured by a linear input
  on the pawn structure added to a frozen network.
* **Mid-training networks are not representative.** Space24 at epoch 9, with a training loss nearly twice that of the
  base, lost 80 Elo and searched one ply less at equal time; the final network of the same block lost about 8 with
  nearly the same depth.

## 7. Next

* A study of how to train a better passed-pawn block, or a different and possibly larger one, within an engine cost of
  1 to 1.5% of cycles per node: the cost depends on the number of active rows per passer and on how often they change,
  not on the size of the table, so richer states with a single active row are affordable if they change rarely. Other
  directions: data weighted towards positions with passers, the layers after the accumulator unfrozen, a block that
  feeds only the PSQT output, and PassedPawns v1 folded into the PassedRel rows in a 16-bit table, which would remove
  the v1 work when PassedRel is present.
* **Labels that contain the residual**, if the blocks stay small: positions with passers sampled from the same data,
  each labelled with the original score plus the difference between the engine's deep search and its depth-one
  score, converted to the scale of the data (the frozen base stays consistent with its labels, λ stays 0.75). The tools
  are written (`04_consilium/residual_labels`).
* The engine mechanism and its verification tools stay: any future pawn-structure block can use them.

## 8. Follow-up: PassedState (10 October)

The study of section 7 produced PassedState (PassedPawns v3), a block that replaces the PassedPawns rows with one row
per passed pawn in one of 100 states. Its engine implementation, the index check against the trainer, the three
training runs and their results are in [DEVELOPMENT_8.0.md, section 38](../DEVELOPMENT_8.0.md); the training material
is in `04_consilium/graft_passedstate` of the training repository. In short: −2.1 ± 3.7 Elo at fixed nodes with the
block trained alone, +3.3 ± 5.0 once the layers after the accumulator were unfrozen, an engine cost of about 2.0% /
2.5% of cycles per node (middlegame / endgames), and a game test at 25+0.25 on endgame openings running. The engine
can now be built without any graft code (`-DTRIUMV_NO_GRAFTS`); the release is built that way unless a block passes.

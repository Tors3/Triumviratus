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

Two more were measured and dropped: accumulator tiles of 8 registers instead of 16 (slower on the Xeon, and on the
Zen 4 once the changes above were in), and a bitset of the non-zero groups in the AVX2 forward pass (+2.7% cycles).

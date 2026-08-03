#!/usr/bin/env python3
"""Permutazione per CO-OCCORRENZA: raggruppa nella stessa pagina da 4 KB le righe
che compaiono nello STESSO update.

PERCHE' NON BASTA LA FREQUENZA. L'ordinamento per frequenza (gia' bakato, +1,5-2%)
mette davanti le righe piu' usate e comprime il working set: agisce sulla LATENZA,
che pero' il prefetch gia' nasconde in buona parte. Cio' che il prefetch NON puo'
dare e' ridurre il numero di PAGINE che un singolo update tocca — e quello dipende
da quali righe stanno insieme, non da quanto sono calde.

Una riga threat e' 1024 byte => 4 righe per pagina da 4 KB. Un update tocca ~10,5
righe: se cadono su 10 pagine diverse sono 10 miss di TLB, se cadono su 3 sono 3.

INPUT : <base>       istogramma frequenze (indice conteggio) — spazio ORIGINALE
        <base>.cooc  coppie (i j conteggio) — spazio GIA' PERMUTATO per frequenza
OUTPUT: feat_perm.cpp con la permutazione COMPOSTA (frequenza -> clustering).

⚠️ La composizione e' obbligatoria: la matrice di co-occorrenza e' stata raccolta su
un binario che aveva gia' la permutazione per frequenza attiva, quindi i suoi indici
sono in quello spazio, non in quello originale.
"""
import argparse, sys
from collections import defaultdict

FEAT_ROWS = 64464
PERM_SIZE = 66560
PSQ_ROWS  = 22528
COOC_N    = 4096
ROWS_PER_PAGE = 4          # 4096 byte / 1024 byte per riga

ap = argparse.ArgumentParser()
ap.add_argument("--hist", required=True, help="istogramma in spazio ORIGINALE (featdump.txt)")
ap.add_argument("--cooc", required=True, help="coppie in spazio PERMUTATO (*.cooc)")
ap.add_argument("--out", default=r"C:\Users\franc\source\repos\Triumviratus_3.0\Triumviratus_7\nnue\nnue\features\feat_perm.cpp")
args = ap.parse_args()

# --- 1. permutazione per frequenza (quella gia' in produzione) -----------------
counts = [0] * FEAT_ROWS
for line in open(args.hist):
    a = line.split()
    if len(a) == 2 and 0 <= int(a[0]) < FEAT_ROWS:
        counts[int(a[0])] = int(a[1])
freq_order = sorted(range(FEAT_ROWS), key=lambda i: (-counts[i], i))
freq_perm = [0] * FEAT_ROWS                 # riga originale -> riga "per frequenza"
for new, old in enumerate(freq_order):
    freq_perm[old] = new

# --- 2. clustering nello spazio permutato -------------------------------------
adj = defaultdict(dict)
for line in open(args.cooc):
    a = line.split()
    if len(a) == 3:
        i, j, c = int(a[0]), int(a[1]), int(a[2])
        adj[i][j] = c
        adj[j][i] = c

# Peso di una riga = quanto e' calda: nello spazio permutato la riga r ha la
# frequenza della r-esima piu' calta, cioe' counts[freq_order[r]].
hot = [counts[freq_order[r]] if r < FEAT_ROWS else 0 for r in range(COOC_N)]

placed = [False] * COOC_N
pages = []
# Greedy: si parte dalla riga piu' calda non piazzata e si riempie la sua pagina con
# le righe che le stanno piu' spesso accanto. Semplice e sufficiente: l'alternativa
# (partizionamento di grafo vero) e' sproporzionata per un'ipotesi da verificare.
order_by_hot = sorted(range(COOC_N), key=lambda r: -hot[r])
for seed in order_by_hot:
    if placed[seed]:
        continue
    page = [seed]
    placed[seed] = True
    while len(page) < ROWS_PER_PAGE:
        best, bestw = -1, -1
        cand = {}
        for m in page:
            for n, c in adj.get(m, {}).items():
                if n < COOC_N and not placed[n]:
                    cand[n] = cand.get(n, 0) + c
        for n, w in cand.items():
            if w > bestw:
                best, bestw = n, w
        if best < 0:                      # nessun vicino: si riempie con la piu' calda
            for r in order_by_hot:
                if not placed[r]:
                    best = r
                    break
            if best < 0:
                break
        page.append(best)
        placed[best] = True
    pages.append(page)

clustered = [r for pg in pages for r in pg]
assert len(clustered) == COOC_N and sorted(clustered) == list(range(COOC_N))

# cluster_perm: riga "per frequenza" -> riga finale. Le righe >= COOC_N restano dove sono.
cluster_perm = list(range(FEAT_ROWS))
for new, old in enumerate(clustered):
    cluster_perm[old] = new

# --- 3. composizione ----------------------------------------------------------
perm = [cluster_perm[freq_perm[r]] for r in range(FEAT_ROWS)]
assert sorted(perm) == list(range(FEAT_ROWS)), "la composizione non e' una permutazione!"

# Quante pagine tocca in media un update? Stima sulle coppie: frazione di coppie
# co-occorrenti che finiscono nella STESSA pagina, prima e dopo.
def same_page_frac(mapping):
    num = den = 0
    for i, d in adj.items():
        for j, c in d.items():
            if i < j and i < COOC_N and j < COOC_N:
                den += c
                if mapping[i] // ROWS_PER_PAGE == mapping[j] // ROWS_PER_PAGE:
                    num += c
    return 100.0 * num / den if den else 0.0

before = same_page_frac(list(range(COOC_N)))
after  = same_page_frac(cluster_perm)
note = (f"clustering co-occorrenza su {COOC_N} righe calde, pagine da {ROWS_PER_PAGE} righe: "
        f"coppie nella stessa pagina {before:.2f}% -> {after:.2f}%")
print(note)

# --- 4. HalfKA: identita' (permutarlo PEGGIORA, vedi feat_perm.h) --------------
psq = list(range(PSQ_ROWS))
psq_note = "IDENTITA' — permutare HalfKA misura -1,15% (righe gia' contigue per costruzione)"

full = perm + [FEAT_ROWS] * (PERM_SIZE - FEAT_ROWS)
with open(args.out, "w", encoding="utf-8") as f:
    f.write("// GENERATO DA gen_cooc_perm.py — NON MODIFICARE A MANO.\n")
    f.write(f"// {note}\n")
    f.write("// Composizione: frequenza -> clustering per co-occorrenza.\n\n")
    f.write('#include "feat_perm.h"\n\n')
    f.write('#include "full_threats.h"\n#include "pawn_pair.h"\n#include "passed_pawns.h"\n'
            '#include "half_ka_v2_hm.h"\n\n')
    f.write("namespace Triumviratus::Eval::NNUE::Features {\n\n")
    f.write("static_assert(FullThreats::Dimensions + PawnPair::Dimensions\n"
            "                + PassedPawns::Dimensions == FeatRows,\n"
            '              "FeatRows non combacia con le tre feature set");\n\n')
    f.write("#ifndef TRIUMV_NO_FEAT_PERM\n")
    f.write("const unsigned short FeatPerm[FeatPermSize] = {\n")
    for i in range(0, PERM_SIZE, 16):
        f.write("    " + ",".join(str(v) for v in full[i:i + 16]) + ",\n")
    f.write("};\n#endif\n\n")
    f.write('static_assert(HalfKAv2_hm::Dimensions == PsqRows,\n'
            '              "PsqRows non combacia con HalfKAv2_hm");\n\n')
    f.write("#ifdef TRIUMV_PSQ_PERM_ON\n")
    f.write(f"// {psq_note}\n")
    f.write("const unsigned short PsqPerm[PsqRows] = {\n")
    for i in range(0, PSQ_ROWS, 16):
        f.write("    " + ",".join(str(v) for v in psq[i:i + 16]) + ",\n")
    f.write("};\n#endif\n\n}  // namespace\n")
print(f"scritto {args.out}")

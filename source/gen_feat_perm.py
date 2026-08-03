#!/usr/bin/env python3
"""Genera `feat_perm.cpp`: la permutazione per localita' delle righe di threatWeights.

INPUT  : un dump dell'istogramma degli accessi (una riga "<indice> <conteggio>"),
         prodotto dal comando `featdump <file>` di una build TRIUMV_PROFILE.
         Senza input genera l'IDENTITA', che serve come RETE DI SICUREZZA: con
         l'identita' il motore deve benchare 207259: se lo fa, il cablaggio
         (permutazione dei pesi + rimappatura degli indici) e' corretto e l'unica
         cosa che puo' cambiare dopo e' la velocita'.

OUTPUT : FeatPerm[raw] = riga nuova. Le righe calde finiscono per prime e contigue;
         tutta la coda >= FeatRows vale FeatRows (sentinella delle feature morte).

USO:
  python gen_feat_perm.py --identity                 # rete di sicurezza
  python gen_feat_perm.py --hist featdump.txt        # permutazione vera
"""
import argparse, sys

FEAT_ROWS = 64464
PERM_SIZE = 66560
OUT = r"C:\Users\franc\source\repos\Triumviratus_3.0\Triumviratus_7\nnue\nnue\features\feat_perm.cpp"

ap = argparse.ArgumentParser()
ap.add_argument("--hist", default=None)
ap.add_argument("--identity", action="store_true")
ap.add_argument("--out", default=OUT)
args = ap.parse_args()

if args.identity or not args.hist:
    perm = list(range(FEAT_ROWS))
    note = "IDENTITA' (rete di sicurezza: il bench deve restare 207259)"
else:
    counts = [0] * FEAT_ROWS
    with open(args.hist) as f:
        for line in f:
            p = line.split()
            if len(p) == 2:
                i, c = int(p[0]), int(p[1])
                if 0 <= i < FEAT_ROWS:
                    counts[i] = c
    # Righe ordinate per frequenza decrescente: le calde davanti, contigue.
    # A parita' di conteggio si tiene l'ordine originale (stabile) — le righe mai
    # toccate restano nel loro ordine relativo, e' irrilevante ma rende il file
    # riproducibile.
    order = sorted(range(FEAT_ROWS), key=lambda i: (-counts[i], i))
    perm = [0] * FEAT_ROWS
    for new_row, old_row in enumerate(order):
        perm[old_row] = new_row
    tot = sum(counts) or 1
    hot = sum(counts[i] for i in order[:3358])
    note = (f"da istogramma: {sum(1 for c in counts if c)} righe toccate, "
            f"il 90% degli accessi entra nelle prime {3358} righe "
            f"({100.0*hot/tot:.1f}% verificato)")

full = perm + [FEAT_ROWS] * (PERM_SIZE - FEAT_ROWS)
assert len(full) == PERM_SIZE
assert sorted(perm) == list(range(FEAT_ROWS)), "non e' una permutazione!"

with open(args.out, "w", encoding="utf-8") as f:
    f.write("// GENERATO DA gen_feat_perm.py — NON MODIFICARE A MANO.\n")
    f.write(f"// {note}\n")
    f.write("// Vedi feat_perm.h per il perche' e per la trappola degli indici morti.\n\n")
    f.write('#include "feat_perm.h"\n\n')
    f.write('#include "full_threats.h"\n#include "pawn_pair.h"\n#include "passed_pawns.h"\n\n')
    f.write("namespace Triumviratus::Eval::NNUE::Features {\n\n")
    f.write("// Se questo assert salta, la tabella dei pesi ha cambiato taglia e la\n"
            "// permutazione va rigenerata: usarla com'e' darebbe righe sbagliate IN SILENZIO.\n")
    f.write("static_assert(FullThreats::Dimensions + PawnPair::Dimensions\n"
            "                + PassedPawns::Dimensions == FeatRows,\n"
            '              "FeatRows non combacia con le tre feature set");\n\n')
    f.write("#ifndef TRIUMV_NO_FEAT_PERM\n")
    f.write("const unsigned short FeatPerm[FeatPermSize] = {\n")
    for i in range(0, PERM_SIZE, 16):
        f.write("    " + ",".join(str(v) for v in full[i:i + 16]) + ",\n")
    f.write("};\n#endif\n\n}  // namespace\n")

print(f"scritto {args.out}\n  {note}")

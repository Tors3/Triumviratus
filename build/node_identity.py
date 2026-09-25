"""Verifica di nodi identici fra due build: stesse posizioni, go depth fissa, confronta
i nodi riportati da ogni ricerca. Serve per le ottimizzazioni NPS, che non devono
cambiare l'albero.

Uso: python node_identity.py <exeA> <exeB> <fens.txt> [--depth 14] [--first 20]
"""
import argparse
import os
import subprocess


def nodes_per_position(exe, fens, depth):
    p = subprocess.Popen([os.path.abspath(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def send(c):
        p.stdin.write(c + "\n")
        p.stdin.flush()

    def wait_for(tok):
        while True:
            line = p.stdout.readline()
            if not line:
                raise RuntimeError("motore uscito")
            if line.startswith(tok):
                return line

    send("uci"); wait_for("uciok")
    send("setoption name Hash value 16"); send("setoption name Threads value 1")
    out = []
    for fen in fens:
        send("ucinewgame"); send("isready"); wait_for("readyok")
        send(f"position fen {fen}")
        send(f"go depth {depth}")
        last = 0
        while True:
            line = p.stdout.readline()
            if line.startswith("info") and " nodes " in line:
                t = line.split()
                last = int(t[t.index("nodes") + 1])
            if line.startswith("bestmove"):
                out.append((last, line.split()[1]))
                break
    send("quit")
    p.wait(timeout=30)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b"); ap.add_argument("fens")
    ap.add_argument("--depth", type=int, default=14)
    ap.add_argument("--first", type=int, default=20)
    a = ap.parse_args()
    fens = [l.strip() for l in open(a.fens) if l.strip()][:a.first]
    ra = nodes_per_position(a.a, fens, a.depth)
    rb = nodes_per_position(a.b, fens, a.depth)
    diff = [(i, x, y) for i, (x, y) in enumerate(zip(ra, rb)) if x != y]
    print(f"posizioni {len(fens)}  nodi A {sum(x[0] for x in ra)}  nodi B {sum(x[0] for x in rb)}"
          f"  diverse {len(diff)}")
    for i, x, y in diff[:10]:
        print(f"  pos {i}: A {x}  B {y}")


if __name__ == "__main__":
    main()

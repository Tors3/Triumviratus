"""Carico di lavoro UCI comune per confronti NPS / istruzioni fra motori.

Per ogni FEN: ucinewgame, position fen, go nodes N. Stesso carico per qualunque
motore UCI (Triumviratus, Stockfish, ...). Stampa nodi totali, tempo e nps.

Uso:
  python uci_workload.py <exe> <fens.txt> [--nodes 500000] [--hash 16] [--reps 1]
                         [--first K] [--tag nome]
Con --reps > 1 ripete l'intero giro e riporta la mediana dell'nps.
"""
import argparse
import os
import statistics
import subprocess
import time


def run_once(exe, fens, nodes, hash_mb):
    exe = os.path.abspath(exe)  # CreateProcess non risolve i percorsi relativi con '/'
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def send(cmd):
        p.stdin.write(cmd + "\n")
        p.stdin.flush()

    def wait_for(token):
        while True:
            line = p.stdout.readline()
            if not line:
                raise RuntimeError("il motore e' uscito")
            if line.startswith(token):
                return line

    send("uci")
    wait_for("uciok")
    send(f"setoption name Hash value {hash_mb}")
    send("setoption name Threads value 1")
    send("isready")
    wait_for("readyok")

    tot_nodes, tot_time = 0, 0.0
    for fen in fens:
        send("ucinewgame")
        send("isready")
        wait_for("readyok")
        send(f"position fen {fen}")
        t0 = time.perf_counter()
        send(f"go nodes {nodes}")
        last_nodes = 0
        while True:
            line = p.stdout.readline()
            if not line:
                raise RuntimeError("il motore e' uscito")
            if line.startswith("info") and " nodes " in line:
                tok = line.split()
                last_nodes = int(tok[tok.index("nodes") + 1])
            if line.startswith("bestmove"):
                break
        tot_time += time.perf_counter() - t0
        tot_nodes += last_nodes
    send("quit")
    p.wait(timeout=30)
    return tot_nodes, tot_time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("fens")
    ap.add_argument("--nodes", type=int, default=500000)
    ap.add_argument("--hash", type=int, default=16)
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    fens = [l.strip() for l in open(a.fens) if l.strip()]
    if a.first:
        fens = fens[:a.first]
    rates = []
    for _ in range(a.reps):
        n, t = run_once(a.exe, fens, a.nodes, a.hash)
        rates.append(n / t)
        print(f"{a.tag} nodes {n} time {t:.2f}s nps {n / t:.0f}", flush=True)
    if a.reps > 1:
        print(f"{a.tag} mediana nps {statistics.median(rates):.0f}  "
              f"min {min(rates):.0f} max {max(rates):.0f}")


if __name__ == "__main__":
    main()

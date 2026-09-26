"""Profondita' raggiunta per nodi spesi: quanto e' "largo" l'albero di un motore.

Uso: python depth_per_nodes.py <fens.txt> <righe es. 3,20,21 | all> <nodi> tag=exe[|Opzione=valore...] ...
Per ogni posizione e motore: `go nodes N` a 1 thread, Hash 64. Dalle righe `info` si ricava, per
ogni profondita' completata, quanti nodi sono serviti. Il conteggio dei nodi non dipende dal carico
della macchina ne' dal compilatore, quindi il confronto regge anche con la CPU occupata.
Variabile DPN_JOBS = posizioni in parallelo (default 1). Stampa: profondita' finale per motore e nodi per arrivare a profondita' 16/20/24/28.
Le righe del file possono avere "fen ... moves ..." (formato del bench di SF).
"""
import json
import os
import subprocess
import sys
import threading


def run(spec, fen, nodes):
    exe, *opts = spec.split("|")
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         text=True, bufsize=1)
    def send(c):
        p.stdin.write(c + "\n"); p.stdin.flush()
    send("uci"); send("setoption name Hash value 64"); send("setoption name Threads value 1")
    for o in opts:
        k, v = o.split("=", 1)
        send(f"setoption name {k} value {v}")
    send("isready")
    while not p.stdout.readline().startswith("readyok"):
        pass
    send("ucinewgame")
    if " moves " in fen:
        f, mv = fen.split(" moves ", 1)
        send(f"position fen {f} moves {mv}")
    else:
        send(f"position fen {fen}")
    send(f"go nodes {nodes}")
    per_depth = {}
    while True:
        line = p.stdout.readline()
        if not line or line.startswith("bestmove"):
            break
        t = line.split()
        if t and t[0] == "info" and "depth" in t and "nodes" in t and "score" in t and "pv" in t:
            if "lowerbound" in t or "upperbound" in t:
                continue
            d = int(t[t.index("depth") + 1]); n = int(t[t.index("nodes") + 1])
            per_depth[d] = max(n, per_depth.get(d, 0))
    send("quit")
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()
    return per_depth


def main():
    fens_file, nodes = sys.argv[1], int(sys.argv[3])
    engines = [a.split("=", 1) for a in sys.argv[4:]]
    lines = [l.strip() for l in open(fens_file) if l.strip()]
    rows = list(range(1, len(lines) + 1)) if sys.argv[2] == "all" else [int(x) for x in sys.argv[2].split(",")]
    marks = (16, 20, 24, 28)
    tot = {tag: [] for tag, _ in engines}
    dump = {}
    print(f"{'riga':>4} " + " | ".join(f"{tag:>22}" for tag, _ in engines))
    print(f"{'':>4} " + " | ".join(f"{'dmax  n@16/20/24/28 (k)':>22}" for _ in engines))
    jobs = int(os.environ.get("DPN_JOBS", "1"))  # posizioni in parallelo (ognuna lancia un processo per motore)
    from concurrent.futures import ThreadPoolExecutor

    def one(r):
        fen = lines[r - 1]
        res = {}
        ths = [threading.Thread(target=lambda tag=tag, exe=exe: res.__setitem__(tag, run(exe, fen, nodes)))
               for tag, exe in engines]
        for t in ths: t.start()
        for t in ths: t.join()
        return r, res

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        results = list(ex.map(one, rows))
    for r, res in results:
        dump[r] = res
        cells = []
        for tag, _ in engines:
            pd = res[tag]
            dmax = max(pd) if pd else 0
            tot[tag].append(dmax)
            nk = "/".join(str(pd[m] // 1000) if m in pd else "-" for m in marks)
            cells.append(f"{dmax:>3}  {nk:>17}")
        print(f"{r:>4} " + " | ".join(cells), flush=True)
    if os.environ.get("DPN_JSON"):
        json.dump(dump, open(os.environ["DPN_JSON"], "w"))
    print("media dmax: " + "  ".join(f"{tag} {sum(v)/len(v):.1f}" for tag, v in tot.items()))


if __name__ == "__main__":
    main()

"""Confronta i contatori di ricerca (nodi non-PV a profondita' >= 12) fra Triumviratus e Stockfish.

Uso: python search_counters.py <fens.txt> <righe> <nodi> <triumv_egstat.exe> <sf_dbg.exe>
Richiede le due build strumentate costruite nella cartella di lavoro (vedi docs/audit_7.1/H_FINALI.md):
  Triumviratus: `info string EGS <nome> n=<conteggio> v=<valore>` prima del bestmove;
  Stockfish:    dbg_print() su stderr (Hit #i / Mean #i), slot allineati ai nostri.
Per ogni posizione un processo nuovo (i contatori sono cumulativi nel processo); si sommano i totali.
"""
import re
import subprocess
import sys
import threading

OUR = {"lmr_red": "riduzione LMR media (ply)", "lmr_fh": "LMR ridotta che batte alpha",
       "ttcut": "cutoff TT", "nmp_ok": "null move riuscita", "moves": "mosse fatte per nodo"}
# slot Stockfish -> nome nostro
SF_MEAN = {0: "lmr_red", 3: "moves", 1: "moves_considered", 2: "ext"}
SF_HIT = {0: "lmr_fh", 1: "ttcut", 2: "nmp_ok", 3: "tt_hit", 4: "tt_depthok", 5: "tt_boundok", 6: "tt_refineok"}


def run(exe, fen, nodes, opts, err_to_out):
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT if err_to_out else subprocess.DEVNULL, text=True, bufsize=1)
    w = lambda c: (p.stdin.write(c + "\n"), p.stdin.flush())
    w("uci"); w("setoption name Hash value 64"); w("setoption name Threads value 1")
    for o in opts:
        k, v = o.split("="); w(f"setoption name {k} value {v}")
    if " moves " in fen:
        f, mv = fen.split(" moves ", 1); w(f"position fen {f} moves {mv}")
    else:
        w(f"position fen {fen}")
    w(f"go nodes {nodes}")
    out = []
    while True:
        line = p.stdout.readline()
        if not line:
            break
        out.append(line)
        if line.startswith("bestmove"):
            break
    # stderr di SF arriva prima del bestmove (dbg_print prima di onBestmove)
    w("quit")
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()
    return out


def main():
    fens, rows, nodes, our, sf = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
    lines = [l.strip() for l in open(fens) if l.strip()]
    rows = [int(x) for x in rows.split(",")]
    agg = {"T": {}, "SF": {}}  # nome -> [n, somma]

    def add(side, name, n, total):
        a = agg[side].setdefault(name, [0, 0.0]); a[0] += n; a[1] += total

    for r in rows:
        fen = lines[r - 1]
        res = {}
        t1 = threading.Thread(target=lambda: res.__setitem__("T", run(our, fen, nodes, ["QuietOffense=false"], False)))
        t2 = threading.Thread(target=lambda: res.__setitem__("SF", run(sf, fen, nodes, [], True)))
        t1.start(); t2.start(); t1.join(); t2.join()
        for l in res["T"]:
            m = re.match(r"info string EGS (\w+) n=(\d+) v=([\d.]+)", l)
            if m:
                n, v = int(m.group(2)), float(m.group(3)); add("T", m.group(1), n, n * v)
        last = {}
        for l in res["SF"]:  # tiene l'ultima stampa (cumulativa)
            m = re.match(r"(Hit|Mean) #(\d+): Total (\d+) (?:Hits (\d+).*|Mean ([-\d.e+]+))", l)
            if m:
                last[(m.group(1), int(m.group(2)))] = m
        for (kind, slot), m in last.items():
            n = int(m.group(3))
            if kind == "Hit" and slot in SF_HIT:
                add("SF", SF_HIT[slot], n, int(m.group(4)))
            elif kind == "Mean" and slot in SF_MEAN:
                v = float(m.group(5)) / 1000.0
                add("SF", SF_MEAN[slot], n, n * v)
        print(f"riga {r} fatta", flush=True)
    print(f"\n{'contatore (non-PV, depth>=12)':34} {'Triumviratus':>14} {'Stockfish':>14}")
    for name in ["moves", "moves_considered", "lmr_red", "lmr_fh", "ttcut", "nmp_ok", "ext", "tt_hit", "tt_depthok", "tt_boundok", "tt_refineok"]:
        def cell(side):
            a = agg[side].get(name)
            if not a or not a[0]:
                return "-"
            v = a[1] / a[0]
            return f"{100 * v:.1f}%" if name in ("lmr_fh", "ttcut", "nmp_ok", "tt_hit", "tt_depthok", "tt_boundok", "tt_refineok") else f"{v:.2f}"
        def cnt(side):
            a = agg[side].get(name); return a[0] if a else 0
        print(f"{name:34} {cell('T'):>14} {cell('SF'):>14}   (n {cnt('T')} / {cnt('SF')})")


if __name__ == "__main__":
    main()

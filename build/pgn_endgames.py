"""Estrae posizioni di finale da un PGN di fastchess scritto con `notation=uci`.

Uso: python pgn_endgames.py <games.pgn> <sf.exe> <out.txt> [--max-pieces 10] [--max-eval 300]
Per ogni partita prende la PRIMA posizione con al piu' max-pieces pezzi (re compresi) e la tiene se
la valutazione del motore in quella mossa e' entro +-max-eval centipedoni (le posizioni gia' decise
non dicono nulla sulla ricerca). Le FEN le ricostruisce Stockfish (`position ... moves ...` + `d`),
cosi' non serve una libreria di scacchi. Il numero di pezzi cala in modo monotono lungo la partita,
quindi basta una ricerca binaria sulla ply.
"""
import argparse
import re
import subprocess


class SF:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def fen(self, start, moves):
        cmd = f"position fen {start}" + (" moves " + " ".join(moves) if moves else "")
        self.p.stdin.write(cmd + "\nd\n"); self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if line.startswith("Fen:"):
                fen = line[4:].strip()
            if line.startswith("Checkers:"):
                return fen


def pieces(fen):
    return sum(c.isalpha() for c in fen.split()[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pgn"); ap.add_argument("sf"); ap.add_argument("out")
    ap.add_argument("--max-pieces", type=int, default=10)
    ap.add_argument("--max-eval", type=int, default=300)
    a = ap.parse_args()
    txt = open(a.pgn, encoding="utf-8", errors="replace").read()
    sf = SF(a.sf)
    out, seen = [], set()
    for g in re.split(r"\n(?=\[Event )", txt):
        m = re.search(r'\[FEN "([^"]+)"\]', g)
        if not m or "\n\n" not in g:
            continue
        start = m.group(1)
        body = g.split("\n\n", 1)[1]
        toks = re.findall(r"\{([^}]*)\}|([a-h][1-8][a-h][1-8][qrbn]?)", body)
        moves, evals = [], []
        for comment, mv in toks:
            if mv:
                moves.append(mv); evals.append(None)
            elif comment and evals:
                e = re.match(r"\s*([+-]?)(M?)(\d+(?:\.\d+)?)", comment)
                if e and not e.group(2):
                    evals[-1] = (-1 if e.group(1) == "-" else 1) * float(e.group(3)) * 100
        if pieces(sf.fen(start, moves)) > a.max_pieces:
            continue
        lo, hi = 0, len(moves)  # prima ply con pieces <= max
        while lo < hi:
            mid = (lo + hi) // 2
            if pieces(sf.fen(start, moves[:mid])) <= a.max_pieces:
                hi = mid
            else:
                lo = mid + 1
        ply = lo
        # la valutazione "della posizione" e' quella della mossa giocata DA quella posizione
        ev = evals[ply] if ply < len(evals) else None
        if ev is None or abs(ev) > a.max_eval:
            continue
        fen = sf.fen(start, moves[:ply])
        key = " ".join(fen.split()[:4])
        if key in seen:
            continue
        seen.add(key)
        out.append(fen)
    with open(a.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"{len(out)} posizioni scritte in {a.out}")


if __name__ == "__main__":
    main()

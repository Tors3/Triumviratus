#!/usr/bin/env python3
# =============================================================================
#  Workload di TRAINING per il PGO  –  versione parallelizzata.
#
#  IMPORTANTE (lezione imparata): allenare il PGO su poche posizioni a depth
#  FISSO produce un profilo che somiglia alla bench ma NON al gioco reale ->
#  la bench vola (+30%) ma in partita il guadagno e' ~0 (overfit del profilo).
#
#  Qui il profilo e' reso rappresentativo del gioco vero:
#    - posizioni VARIE prese dal libro di apertura usato nei tornei (UHO),
#    - ricerca a MOVETIME (come in partita a tempo), non a depth fisso,
#    - piu' livelli di movetime per coprire lo spettro di profondita' reali,
#    - Hash/Threads uguali a quelli del match (256MB / 1 thread per istanza).
#  Cosi' i percorsi caldi profilati = quelli che contano davvero in partita.
#
#  PARALLELISMO:
#    --workers N  lancia N processi del motore in parallelo su chunk disgiunti
#    di posizioni. Ogni processo strumentato scrive Triumviratus_pgo!K.pgc con
#    un indice univoco assegnato da pgort a runtime -> nessuna collisione.
#    pgomgr /merge (chiamato da build_pgo.ps1) consolida tutti i .pgc nel .pgd.
#
#  USO:   python pgo_train.py <exe> [movetime_ms] [book.epd] [n_pos] [--workers N]
#  Lo lancia build_pgo.ps1.
# =============================================================================
import subprocess, sys, time, os, math
from concurrent.futures import ThreadPoolExecutor, as_completed

# Path RELATIVI alla posizione dello script (= repo-root) -> niente path assoluti.
# Sono solo fallback: gli script build_pgo*.ps1 passano exe+book come argomenti.
_REPO = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BOOK = os.path.join(_REPO, "OpeningBooks", "uho_2024", "UHO_2024_+085_+094",
                            "UHO_2024_8mvs_big_+080_+099.epd")

# Spettro di movetime (ms): copre le profondita' reali del bullet 0.5+0.1.
MOVETIMES = [40, 100, 250, 600, 1000]

# Fallback se il libro non si trova: set vario (apertura/medio/tattica/finale).
FALLBACK_POS = [
    "startpos",
    "fen rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
    "fen r1bqk2r/pp2bppp/2n1pn2/2pp4/3P4/2N1PN2/PPP1BPPP/R1BQK2R w KQkq - 0 8",
    "fen r2q1rk1/1b1nbppp/p2ppn2/1p4B1/3NPP2/2N2Q2/PPP3PP/2KR1B1R w - - 0 11",
    "fen r1b1k2r/2q1bppp/p1nppn2/1p6/3NP3/1BN1B3/PPP1QPPP/R4RK1 w kq - 0 11",
    "fen 2rq1rk1/pp1bppbp/3p1np1/8/2PNP3/2N1BP2/PP1Q2PP/R3KB1R w KQ - 0 11",
    "fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "fen 8/8/4k3/8/2p5/2P1K3/4P3/8 w - - 0 1",
    "fen r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
]


def load_positions(book, n):
    """Estrae fino a n posizioni dall'EPD (campionate uniformemente)."""
    if not book or not os.path.exists(book):
        return FALLBACK_POS, "fallback (libro non trovato)"
    lines = []
    with open(book, "r", encoding="utf-8", errors="ignore") as f:
        for ln in f:
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            toks = ln.split()
            if len(toks) < 4:
                continue
            # EPD = 4 campi (no halfmove/fullmove): aggiungo "0 1" per FEN valida
            fen = " ".join(toks[:4]) + " 0 1"
            lines.append("fen " + fen)
    if not lines:
        return FALLBACK_POS, "fallback (EPD vuoto)"
    if len(lines) > n:
        step = len(lines) / n
        lines = [lines[int(i * step)] for i in range(n)]
    return lines, os.path.basename(book)


def run_worker(worker_id, exe, positions, times, total_searches, t0):
    """
    Lancia un singolo processo del motore e lo fa girare su `positions`.
    Ogni processo strumentato riceve un ID univoco da pgort -> scrive
    Triumviratus_pgo!<N>.pgc senza collisioni con gli altri worker.
    Restituisce (worker_id, n_searches_done, elapsed).

    Tutti i worker girano la config di SHIPPING (default UCI): policy OFF
    (morta), finny ON. Cosi' il profilo caldo = esattamente il path di release.
    """
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True, bufsize=1)

    def send(c): p.stdin.write(c + "\n"); p.stdin.flush()
    def wait(tok):
        while True:
            l = p.stdout.readline()
            if l == "":
                raise RuntimeError(f"[worker {worker_id}] motore terminato inaspettatamente")
            if l.strip() == tok or l.startswith(tok):
                return

    send("uci"); wait("uciok")
    send("setoption name Hash value 256")
    send("setoption name Threads value 1")
    send("isready"); wait("readyok")

    done = 0
    for pos in positions:
        send("position " + pos)
        send("isready"); wait("readyok")
        for mt in times:
            send(f"go movetime {mt}")
            wait("bestmove")
            done += 1

    send("quit")
    try:
        p.wait(timeout=10)
    except Exception:
        p.kill()

    elapsed = time.time() - t0
    return worker_id, done, elapsed


def split_chunks(lst, n):
    """Divide lst in n chunk il piu' possibile uguali."""
    k, r = divmod(len(lst), n)
    chunks, start = [], 0
    for i in range(n):
        end = start + k + (1 if i < r else 0)
        chunks.append(lst[start:end])
        start = end
    return chunks


def main():
    # Parsing argomenti posizionali + --workers
    args = sys.argv[1:]
    workers = 8  # default
    if "--workers" in args:
        idx = args.index("--workers")
        workers = int(args[idx + 1])
        args = args[:idx] + args[idx + 2:]

    exe      = args[0] if len(args) > 0 else \
        os.path.join(_REPO, "x64", "Release", "Triumviratus_pgo.exe")
    movetime = int(args[1]) if len(args) > 1 else 0   # 0 = usa lo spettro
    book     = args[2] if len(args) > 2 else DEFAULT_BOOK
    n_pos    = int(args[3]) if len(args) > 3 else 60

    times = [movetime] if movetime > 0 else MOVETIMES
    positions, src = load_positions(book, n_pos)

    # Limita workers al numero di posizioni disponibili
    workers = min(workers, len(positions))
    chunks  = split_chunks(positions, workers)
    total   = len(positions) * len(times)

    print(f"PGO training: {exe}")
    print(f"  sorgente posizioni : {src}  ({len(positions)} pos)")
    print(f"  movetime (ms)      : {times}")
    print(f"  workers paralleli  : {workers}  ({[len(c) for c in chunks]} pos/worker)")
    print(f"  ricerche totali    : {total}  | Hash 256MB / 1 thread per worker")

    t0 = time.time()
    completed_total = 0

    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {
            pool.submit(run_worker, wid, exe, chunk, times, total, t0): wid
            for wid, chunk in enumerate(chunks)
        }
        for fut in as_completed(futures):
            wid = futures[fut]
            try:
                _, done, elapsed = fut.result()
                completed_total += done
                pct = 100.0 * completed_total / total
                print(f"  [worker {wid} done] {done} ricerche  |  "
                      f"cumulative {completed_total}/{total} ({pct:.0f}%)  "
                      f"({elapsed:.0f}s)")
            except Exception as exc:
                print(f"  [worker {wid} ERRORE] {exc}", file=sys.stderr)
                raise

    print(f"Training finito in {time.time()-t0:.0f}s.")


if __name__ == "__main__":
    main()

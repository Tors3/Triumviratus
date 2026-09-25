"""Misura NPS a COPPIE SIMULTANEE, robusta anche a macchina carica.

Due motori girano NELLO STESSO MOMENTO sui due processori logici fratelli di uno stesso core
fisico, sulla stessa posizione, con lo stesso `go nodes`: subiscono lo stesso identico
disturbo (altri processi, L3, memoria, frequenza), quindi il RAPPORTO dei loro nps e' stabile
anche quando i valori assoluti oscillano del 20%. A ogni ripetizione i due motori si
scambiano il processore logico (elimina l'asimmetria fra i due fratelli). Piu' core in
parallelo moltiplicano i campioni.

Uso:
  python nps_pair.py <exeA> <exeB> <fens.txt> [--nodes 300000] [--reps 4] [--first 30]
                     [--cores 0,2,4,6] [--group 0] [--hash 16] [--tag nome]
Stampa il rapporto B/A (media geometrica), l'intervallo al 95% e quante volte B e' piu' veloce.
Il tempo si misura DALL'ESTERNO (perf_counter, dal `go` al `bestmove`) con nodi fissi: non
usare le righe `info` del motore, che arrivano solo a fine iterazione (finestre diverse fra
i due motori: e' il difetto che faceva fallire il test nullo con `go movetime`).
Chi finisce prima continua con `go infinite` finche' l'altro non ha finito, cosi' il
processore fratello resta sempre occupato. Il test nullo (stesso binario due volte) deve
dare ~0%: va rifatto quando si cambia macchina o strumento.
"""
import argparse
import ctypes
import math
import os
import statistics
import subprocess
import threading
import time
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class GROUP_AFFINITY(ctypes.Structure):
    _fields_ = [("Mask", ctypes.c_uint64), ("Group", wintypes.WORD), ("Reserved", wintypes.WORD * 3)]


def pin_process(proc, group, cpu):
    """Fissa TUTTI i thread del processo a (gruppo, cpu). Il motore crea i thread di ricerca
    dopo, e questi ereditano l'affinita' del processo; per i thread gia' esistenti (UCI) si
    usa SetThreadGroupAffinity via la toolhelp snapshot."""
    h = int(proc._handle)
    ok = k32.SetProcessAffinityMask(wintypes.HANDLE(h), ctypes.c_size_t(1 << cpu))
    if ok:
        return True
    # Processo in un altro gruppo: sposto ogni thread con SetThreadGroupAffinity.
    TH32CS_SNAPTHREAD = 0x4
    THREAD_SET_INFORMATION, THREAD_QUERY_INFORMATION = 0x20, 0x40

    class THREADENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ThreadID", wintypes.DWORD), ("th32OwnerProcessID", wintypes.DWORD),
                    ("tpBasePri", wintypes.LONG), ("tpDeltaPri", wintypes.LONG), ("dwFlags", wintypes.DWORD)]
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    ga = GROUP_AFFINITY(Mask=1 << cpu, Group=group)
    moved = 0
    if k32.Thread32First(snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == proc.pid:
                th = k32.OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, False, te.th32ThreadID)
                if th:
                    moved += bool(k32.SetThreadGroupAffinity(th, ctypes.byref(ga), None))
                    k32.CloseHandle(th)
            if not k32.Thread32Next(snap, ctypes.byref(te)):
                break
    k32.CloseHandle(snap)
    return moved > 0


CREATE_SUSPENDED = 0x4
ntdll = ctypes.WinDLL('ntdll')


class Engine:
    def __init__(self, exe, hash_mb, group=0, cpus=None):
        # Creato SOSPESO e fissato al core PRIMA di eseguire una sola istruzione: la rete
        # (~100 MB) e la hash vengono toccate per la prima volta sul nodo NUMA di quel core.
        # Senza, il processo carica la rete dove capita e su una macchina a due socket uno
        # dei due motori puo' ritrovarsi con i pesi sulla memoria REMOTA: il test nullo
        # (stesso binario contro se stesso) dava +3,6% di falso scarto.
        self.p = subprocess.Popen([os.path.abspath(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                  creationflags=CREATE_SUSPENDED)
        if cpus is not None:
            mask = 0
            for c in cpus:
                mask |= 1 << c
            h = wintypes.HANDLE(int(self.p._handle))
            if not k32.SetProcessAffinityMask(h, ctypes.c_size_t(mask)):
                ga = GROUP_AFFINITY(Mask=mask, Group=group)
                # processo creato in un altro gruppo: fallback sul primo thread non serve,
                # il processo sospeso ha un solo thread; lo spostiamo di gruppo
                pin_process(self.p, group, cpus[0])
        ntdll.NtResumeProcess(wintypes.HANDLE(int(self.p._handle)))
        self.send("uci"); self.wait("uciok")
        self.send(f"setoption name Hash value {hash_mb}")
        self.send("setoption name Threads value 1")
        self.send("isready"); self.wait("readyok")

    def send(self, c):
        self.p.stdin.write(c + "\n")
        self.p.stdin.flush()

    def wait(self, tok):
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("motore uscito")
            if line.startswith(tok):
                return line

    def result(self):
        nodes = t = 0
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("motore uscito")
            if line.startswith("info") and " nodes " in line and " time " in line:
                tok = line.split()
                nodes = int(tok[tok.index("nodes") + 1])
                t = int(tok[tok.index("time") + 1])
            if line.startswith("bestmove"):
                return nodes, t

    def quit(self):
        try:
            self.send("quit")
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def _run_one(e, nodes, fen, done_self, done_other, res, key):
    """Cerca `nodes` nodi e cronometra dall'esterno. Poi, finche' l'altro motore non ha
    finito, continua a cercare (go infinite) per tenere occupato il processore fratello:
    nessuno dei due deve mai avere il core tutto per se' nel finale."""
    t0 = time.perf_counter()
    e.send(f"go nodes {nodes}")
    e.wait("bestmove")
    res[key] = time.perf_counter() - t0
    done_self.set()
    if not done_other.is_set():
        e.send("go infinite")
        done_other.wait()
        e.send("stop")
        e.wait("bestmove")


def worker(core, group, exe_a, exe_b, fens, a, out, lock):
    ea = Engine(exe_a, a.hash, group, [core, core + 1])
    eb = Engine(exe_b, a.hash, group, [core, core + 1])
    try:
        for rep in range(a.reps):
            ca, cb = (core, core + 1) if rep % 2 == 0 else (core + 1, core)
            pin_process(ea.p, group, ca)
            pin_process(eb.p, group, cb)
            for fen in fens:
                for e in (ea, eb):
                    e.send("ucinewgame"); e.send("isready")
                for e in (ea, eb):
                    e.wait("readyok")
                for e in (ea, eb):
                    e.send(f"position fen {fen}")
                res = {}
                da, db = threading.Event(), threading.Event()
                ta = threading.Thread(target=_run_one, args=(ea, a.nodes, fen, da, db, res, 'a'))
                tb = threading.Thread(target=_run_one, args=(eb, a.nodes, fen, db, da, res, 'b'))
                # ordine di partenza alternato: chi riceve il `go` per primo, e chi viene
                # svegliato per primo al `bestmove`, non deve essere sempre lo stesso motore
                first, second = (ta, tb) if (rep + fens.index(fen)) % 2 == 0 else (tb, ta)
                first.start(); second.start(); ta.join(); tb.join()
                if res.get('a') and res.get('b'):
                    with lock:
                        # stessi nodi per costruzione: il rapporto nps e' il rapporto dei tempi
                        out.append((core, rep, res['a'] / res['b'], a.nodes / res['a'], a.nodes / res['b']))
    finally:
        ea.quit(); eb.quit()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b"); ap.add_argument("fens")
    ap.add_argument("--nodes", type=int, default=300000)
    ap.add_argument("--reps", type=int, default=4)
    ap.add_argument("--first", type=int, default=30)
    ap.add_argument("--cores", default="0,2,4,6")
    ap.add_argument("--group", type=int, default=0)
    ap.add_argument("--hash", type=int, default=16)
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    fens = [l.strip() for l in open(a.fens) if l.strip()][:a.first]
    cores = [int(x) for x in a.cores.split(',')]
    out, lock = [], threading.Lock()
    t0 = time.time()
    ths = [threading.Thread(target=worker, args=(c, a.group, a.a, a.b, fens, a, out, lock)) for c in cores]
    for t in ths: t.start()
    for t in ths: t.join()
    logs = [math.log(r[2]) for r in out]
    n = len(logs)
    m = statistics.mean(logs)
    se = statistics.stdev(logs) / math.sqrt(n) if n > 1 else float('nan')
    wins = sum(1 for x in logs if x > 0)
    print(f"{a.tag} campioni {n} ({time.time() - t0:.0f}s)  B/A = {100 * (math.exp(m) - 1):+.2f}%  "
          f"IC95 [{100 * (math.exp(m - 1.96 * se) - 1):+.2f}%, {100 * (math.exp(m + 1.96 * se) - 1):+.2f}%]  "
          f"B piu' veloce in {wins}/{n}")
    # stabilita' per core e per ripetizione
    for c in cores:
        lc = [math.log(r[2]) for r in out if r[0] == c]
        if lc:
            print(f"   core {c}: {100 * (math.exp(statistics.mean(lc)) - 1):+.2f}% su {len(lc)}")


if __name__ == "__main__":
    main()

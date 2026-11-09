#!/usr/bin/env python3
"""Regression test for the 37. Re4?? blunder (CCRL Blitz gauntlet of Triumviratus 8.0, game 149, 2026).

Cause found on 28/10/2026: the Zobrist keys (random.cpp/zobrist.cpp, inherited from BBC) had GF(2) rank 32, so the
64-bit position key carried only 32 bits of entropy and the transposition table returned entries of other positions
(2,700-3,600 false hits per search at 1.5M nodes, Hash 256). A colliding entry on the position after 37. Re4 Nxe4
makes the engine refute 37...Nxe4 and play 37. Re4 with the game's PV (Re4 Rc6 Re5 Rd6 Rxg5 Kf7 Kf1, about +1.3).

The test, against any engine binary (net beside it), with three checks:
  1. KEY RANK: reads the piece keys back from the engine (debug command "d" prints the hash key; one extra piece
     changes the key by exactly that piece's Zobrist key) and computes their GF(2) rank. Must be >= 60 (64 expected;
     the inherited keys give 32).
  2. COLLISION: the engine keys of the position after 37. Re4 Nxe4 and of a legal position that shared its key under
     the inherited keys must now differ.
  3. BEHAVIOUR: (a) from the game FEN with a clean hash, go depth 26 must not play d4e4; (b) the collision scenario:
     search the formerly colliding position to depth 24, then the game position to depth 26 without clearing the
     hash: must not play d4e4 (with the inherited keys the official 8.0 binary plays d4e4 here, +1.54, PV d4e4 c7c6
     e4e5 c6d6 e5g5 e7f7 e1f1).
Usage: python regress_re4.py <engine.exe> [--skip-search]
Exit code 0 = all checks passed.
"""
import argparse, os, re, subprocess, sys

GAME_FEN = '8/2r1k2p/1p2pn2/pP4p1/P1BR2p1/1P2P3/5P1P/4K3 w - - 2 37'
AFTER_RE4_NXE4 = '8/2r1k2p/1p2p3/pP4p1/P1B1n1p1/1P2P3/5P1P/4K3 w - - 0 38'
# Legal position that shared the 64-bit key of AFTER_RE4_NXE4 with the inherited keys (e374564b34e3139f): the same
# position plus six piece toggles (+Qa2 +Rf1 +Pc3 +Nf4 +bf5 +ba7), found by meet in the middle on the key subspace.
COLLIDER = '8/b1r1k2p/1p2p3/pP3bp1/P1B1nNp1/1PP1P3/Q4P1P/4KR2 w - - 0 38'
PIECES = 'PNBRQKpnbrqk'


class Engine:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, bufsize=1, cwd=os.path.dirname(os.path.abspath(path)),
                                  creationflags=0x00004000 if os.name == 'nt' else 0)   # BelowNormal on Windows

    def send(self, s):
        self.p.stdin.write(s + '\n')
        self.p.stdin.flush()

    def until(self, prefix):
        out = []
        for line in self.p.stdout:
            line = line.rstrip('\n')
            out.append(line)
            if line.startswith(prefix) or (prefix in line and prefix.startswith('Hash key')):
                return out
        raise RuntimeError('engine exited')

    def key(self, fen):
        self.send('position fen ' + fen)
        self.send('d')
        for line in self.until('     Hash key'):
            m = re.search(r'Hash key:\s+([0-9a-f]+)', line)
            if m:
                return int(m.group(1), 16)
        raise RuntimeError('no key printed')

    def search(self, go):
        self.send(go)
        lines = self.until('bestmove')
        info = [l for l in lines if l.startswith('info depth') and ' pv ' in l]
        return lines[-1].split()[1], (info[-1] if info else '')

    def quit(self):
        try:
            self.send('quit')
            self.p.wait(timeout=20)
        except Exception:
            self.p.kill()


def gf2_rank(vectors):
    basis, r = {}, 0
    for v in vectors:
        while v:
            h = v.bit_length() - 1
            if h not in basis:
                basis[h] = v
                r += 1
                break
            v ^= basis[h]
    return r


def piece_keys(e):
    """Piece keys read back from the engine: key(base + piece on sq) XOR key(base)."""
    keys = []
    # Base: kings on a1/h8 is not possible for every square; use two bases and skip the squares they occupy.
    bases = [('7k/8/8/8/8/8/8/K7', {0, 63}), ('K7/8/8/8/8/8/8/7k', {7, 56})]   # square index a1 = 0 (python-chess)
    for board, occupied in bases:
        k0 = e.key(board + ' w - - 0 1')
        for sq in range(64):
            if sq in occupied:
                continue
            rank, file = sq // 8, sq % 8
            rows = [list(r) for r in expand(board)]
            for pc in PIECES:
                if pc in 'Kk':
                    continue
                if pc in 'Pp' and rank in (0, 7):
                    continue
                rows2 = [r[:] for r in rows]
                rows2[7 - rank][file] = pc
                keys.append(e.key(compress(rows2) + ' w - - 0 1') ^ k0)
        if len(keys) > 600:
            break
    return keys


def expand(board):
    out = []
    for r in board.split('/'):
        row = []
        for ch in r:
            row += ['.'] * int(ch) if ch.isdigit() else [ch]
        out.append(row)
    return out


def compress(rows):
    s = []
    for row in rows:
        n, t = 0, ''
        for ch in row:
            if ch == '.':
                n += 1
            else:
                if n:
                    t += str(n)
                    n = 0
                t += ch
        if n:
            t += str(n)
        s.append(t)
    return '/'.join(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('engine')
    ap.add_argument('--skip-search', action='store_true')
    a = ap.parse_args()
    ok = True
    e = Engine(a.engine)
    e.send('uci')
    e.until('uciok')
    e.send('setoption name Threads value 1')
    e.send('setoption name Hash value 256')

    keys = piece_keys(e)
    r = gf2_rank(keys)
    print('1. key rank: %d over %d piece keys read back (inherited keys: 32)  %s' % (r, len(keys), 'OK' if r >= 60 else 'FAIL'))
    ok &= r >= 60

    k1, k2 = e.key(AFTER_RE4_NXE4), e.key(COLLIDER)
    print('2. collision: %016x vs %016x  %s' % (k1, k2, 'OK' if k1 != k2 else 'FAIL (same key)'))
    ok &= k1 != k2

    if not a.skip_search:
        e.send('ucinewgame')
        e.send('isready')
        e.until('readyok')
        e.send('position fen ' + GAME_FEN)
        bm, info = e.search('go depth 26')
        print('3a. clean hash, depth 26: bestmove %s  %s' % (bm, 'OK' if bm != 'd4e4' else 'FAIL'))
        ok &= bm != 'd4e4'
        e.send('ucinewgame')
        e.send('isready')
        e.until('readyok')
        e.send('position fen ' + COLLIDER)
        e.search('go depth 24')
        e.send('position fen ' + GAME_FEN)
        bm, info = e.search('go depth 26')
        sc = re.search(r'score (cp|mate) (-?\d+)', info)
        print('3b. after searching the old collider: bestmove %s (%s)  %s' %
              (bm, ' '.join(sc.groups()) if sc else '?', 'OK' if bm != 'd4e4' else 'FAIL'))
        ok &= bm != 'd4e4'
    e.quit()
    print('RESULT:', 'PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

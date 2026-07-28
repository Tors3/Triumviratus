"""Misura il throughput del DATA LOADER da solo, senza GPU.

L'altra meta' di gpu_ceiling.py: quanto veloce il loader C++ riesce a produrre
batch, in funzione del numero di worker. Il minimo fra questo e il tetto GPU e'
la velocita' vera del training.

Serve a scegliere quanti vCPU affittare (che e' la voce che decide il costo) e
a verificare se scala linearmente coi worker.

Uso:
    python loader_ceiling.py <file.binpack>
    python loader_ceiling.py <file.binpack> --workers 1,2,4,8,16

Da lanciare dentro nnue-pytorch/ (o con quella dir nel PYTHONPATH).
"""

import argparse
import os
import sys
import time

from data_loader.config import DataloaderSkipConfig
from data_loader.dataset import SparseBatchProvider
from model.modules.features import DEFAULT_FEATURES


def measure(path, feature_set, batch_size, num_workers, batches, warmup, skip=3):
    # random_fen_skipping=N => il reader deve DECODIFICARE ~N+1 posizioni per consegnarne 1.
    # Moltiplica il lavoro di lettura senza toccare quello di estrazione feature, quindi
    # confrontare skip=0 con skip=3 dice se il collo sta nei reader o negli extractor.
    config = DataloaderSkipConfig(
        filtered=True,
        random_fen_skipping=skip,
        wld_filtered=True,
    )
    # Il parser C++ (make_single_extractor) confronta i nomi ESATTI e non conosce
    # il '^' della fattorizzazione, che e' una nozione solo lato Python.
    provider = SparseBatchProvider(
        feature_set.replace("^", ""),
        [path],
        batch_size,
        cyclic=True,
        num_workers=num_workers,
        config=config,
        device="cpu",
    )

    it = iter(provider)
    for _ in range(warmup):
        next(it)

    t0 = time.perf_counter()
    for _ in range(batches):
        next(it)
    elapsed = time.perf_counter() - t0

    del provider
    return batches * batch_size / elapsed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binpack")
    ap.add_argument("--workers", default="1,2,4,8,16", help="lista separata da virgole")
    ap.add_argument("--batch", type=int, default=16384)
    ap.add_argument("--batches", type=int, default=40, help="batch cronometrati")
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--skip", default="3", help="valori di random_fen_skipping da confrontare, separati da virgola")
    args = ap.parse_args()

    if not os.path.exists(args.binpack):
        sys.exit(f"non trovo {args.binpack}")

    print(f"binpack: {args.binpack} ({os.path.getsize(args.binpack) / 2**30:.1f} GB)")
    print(f"core logici: {os.cpu_count()}")
    print(f"feature set: {DEFAULT_FEATURES}")
    print(f"batch {args.batch}, random_fen_skipping={args.skip}")
    print()
    print(f"{'worker':>7}  {'skip':>5}  {'pos/s':>10}  {'scaling':>8}")

    base = None
    for skip in [int(x) for x in args.skip.split(",")]:
        for w in [int(x) for x in args.workers.split(",")]:
            pos_s = measure(args.binpack, DEFAULT_FEATURES, args.batch, w,
                            args.batches, args.warmup, skip=skip)
            if base is None:
                base = pos_s
            print(f"{w:>7}  {skip:>5}  {pos_s / 1000:>9.0f}k  {pos_s / base:>7.2f}x")

    print()
    print("Se lo scaling e' ~lineare fino a N worker, su una VM con piu' core si estrapola;")
    print("se si appiattisce, il collo e' altrove (I/O del disco, lock nel loader).")


if __name__ == "__main__":
    main()

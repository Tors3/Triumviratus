"""Misura il TETTO GPU del training, saltando il data loader.

Costruisce un batch sintetico con la stessa forma di quello che produce il
loader C++, poi ripete il train step in loop. Nessun I/O, nessun worker: quello
che esce e' quanto la GPU riesce a macinare se il loader fosse infinitamente
veloce.

Serve a decidere quale scheda affittare: se il tetto e' molto sopra i ~100k
pos/s che il loader puo' produrre, la scheda costosa e' sprecata.

Uso:
    python gpu_ceiling.py                    # default: batch 16384, 120 feature attive
    python gpu_ceiling.py --batch 8192       # se la VRAM non basta
    python gpu_ceiling.py --active 90        # sensibilita' al conteggio feature

Da lanciare dentro nnue-pytorch/ (o con quella dir nel PYTHONPATH).
"""

import argparse
import sys
import time

import torch

from model.config import NNUELightningConfig
from model.nnue import NNUE

# Somma dei MAX_ACTIVE_FEATURES in data_loader/cpp/training_data_loader.cpp:
# HalfKAv2_hm 32 + FullThreats 128 + PP_3Wide 128 + PassedPawns 16
MAX_ACTIVE = 304


def make_batch(batch_size, num_active, num_inputs, device, generator, ls_buckets=8):
    """Un batch nella forma di SparseBatch.get_tensors().

    Le feature non attive sono paddate a -1, come fa il loader C++.
    """
    us = (torch.rand(batch_size, 1, device=device, generator=generator) < 0.5).float()
    them = 1.0 - us

    def indices():
        idx = torch.full((batch_size, MAX_ACTIVE), -1, dtype=torch.int32, device=device)
        idx[:, :num_active] = torch.randint(
            0, num_inputs, (batch_size, num_active),
            dtype=torch.int32, device=device, generator=generator,
        )
        return idx

    outcome = torch.rand(batch_size, 1, device=device, generator=generator)
    score = (torch.rand(batch_size, 1, device=device, generator=generator) - 0.5) * 2000.0
    # Il modello ricava l'indice del bucket da (piece_count - 1) // 4, quindi per N
    # LayerStack i conteggi devono stare in 1..4N: con N < 8 e pezzi fino a 32 l'indice
    # uscirebbe fuori range e il gather fallirebbe.
    piece_count = torch.randint(
        1, 4 * ls_buckets + 1, (batch_size,), dtype=torch.int64, device=device,
        generator=generator
    )

    return (us, them, indices(), indices(), outcome, score, piece_count)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--batch", type=int, default=16384, help="batch size (default: quello del training vero)")
    ap.add_argument("--active", type=int, default=120, help="feature attive per posizione per prospettiva")
    ap.add_argument("--iters", type=int, default=60, help="iterazioni cronometrate")
    ap.add_argument("--warmup", type=int, default=15, help="iterazioni di riscaldamento, non contate")
    ap.add_argument("--device", default="cuda")
    # StackedLinear calcola TUTTI i bucket con una nn.Linear(in, out*count) e poi ne
    # seleziona uno: il costo della testa scala linearmente con questo numero, e 7/8
    # del lavoro viene buttato. Serve per l'ablazione che misura quanto pesa davvero.
    ap.add_argument("--ls-buckets", type=int, default=8,
                    help="numero di LayerStack (ablazione del costo della testa)")
    args = ap.parse_args()

    if args.active > MAX_ACTIVE:
        sys.exit(f"--active {args.active} supera il buffer paddato ({MAX_ACTIVE})")

    device = torch.device(args.device)
    if device.type == "cuda":
        if not torch.cuda.is_available():
            sys.exit("CUDA non disponibile")
        name = torch.cuda.get_device_name(device)
        vram = torch.cuda.get_device_properties(device).total_memory / 2**30
        print(f"GPU: {name}  ({vram:.1f} GB VRAM)")
    print(f"batch {args.batch}, {args.active} feature attive/prospettiva (buffer {MAX_ACTIVE})")
    print(f"LayerStack: {args.ls_buckets}")

    config = NNUELightningConfig()
    print(f"feature set: {config.features}")

    model = NNUE(config=config, max_epoch=600, num_batches_per_epoch=6103,
                 num_ls_buckets=args.ls_buckets).to(device)
    model.train()

    opt_spec = model.configure_optimizers()
    # configure_optimizers puo' restituire un optimizer, una lista, o un dict:
    # qui serve solo l'optimizer, gli scheduler non incidono sul tempo per step.
    if isinstance(opt_spec, dict):
        optimizer = opt_spec["optimizer"]
    elif isinstance(opt_spec, (list, tuple)):
        first = opt_spec[0]
        optimizer = first[0] if isinstance(first, (list, tuple)) else first
    else:
        optimizer = opt_spec

    num_inputs = model.model.input.num_inputs if hasattr(model.model.input, "num_inputs") else 86992
    print(f"input della rete: {num_inputs}")

    generator = torch.Generator(device=device).manual_seed(0)
    # Il riuso e' totale comunque: 16384x120x2 accessi su ~87k righe toccano
    # l'intera matrice a ogni batch, quindi un batch fisso non falsa la misura.
    batch = make_batch(args.batch, args.active, num_inputs, device, generator,
                       ls_buckets=args.ls_buckets)

    def train_step(step):
        outputs = model.train_step(batch, 0, step)
        loss = outputs["loss"]
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        optimizer.step()
        return loss

    try:
        for i in range(args.warmup):
            train_step(i)
    except torch.cuda.OutOfMemoryError:
        sys.exit(f"VRAM insufficiente a batch {args.batch}: riprova con --batch {args.batch // 2}")

    if device.type == "cuda":
        torch.cuda.synchronize()
        peak = torch.cuda.max_memory_allocated() / 2**30
        print(f"picco VRAM: {peak:.2f} GB")

    t0 = time.perf_counter()
    for i in range(args.iters):
        train_step(args.warmup + i)
    if device.type == "cuda":
        torch.cuda.synchronize()
    elapsed = time.perf_counter() - t0

    its = args.iters / elapsed
    pos_s = its * args.batch
    print()
    print(f"{its:.2f} it/s   ->   {pos_s / 1000:.0f}k pos/s   (tetto GPU, loader escluso)")
    print(f"fase 1 (60 G posizioni) se il loader tenesse il passo: {60e9 / pos_s / 3600:.1f} ore")
    print()
    print("Confronta con i ~100k pos/s che ci aspettiamo dal loader su 24 worker:")
    print("  tetto >> 100k  -> loader-bound, prendi la scheda economica")
    print("  tetto ~  100k  -> le due risorse si contendono, serve la scheda media")
    print("  tetto <  100k  -> GPU-bound, la scheda grossa si ripaga")


if __name__ == "__main__":
    main()

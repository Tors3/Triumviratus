"""Profila UN train step e stampa dove va il tempo sulla GPU.

Nessuna ipotesi: elenca i kernel per tempo cumulativo, cosi' si vede se domina
la gather sparsa del feature transformer, le GEMM della testa, o le passate
elementwise della fake-quantization.
"""
import sys
import torch
from torch.profiler import profile, ProfilerActivity

sys.path.insert(0, ".")
sys.argv = [sys.argv[0]]
import importlib.util
spec = importlib.util.spec_from_file_location("gc_mod", "/root/gpu_ceiling.py")
gc = importlib.util.module_from_spec(spec)
spec.loader.exec_module.__self__ if False else None

from model.config import NNUELightningConfig
from model.nnue import NNUE

MAX_ACTIVE = 304
BATCH, ACTIVE = 16384, 120
dev = torch.device("cuda")

def make_batch(n, active, num_inputs, g):
    us = (torch.rand(n, 1, device=dev, generator=g) < 0.5).float()
    def idx():
        t = torch.full((n, MAX_ACTIVE), -1, dtype=torch.int32, device=dev)
        t[:, :active] = torch.randint(0, num_inputs, (n, active), dtype=torch.int32,
                                      device=dev, generator=g)
        return t
    return (us, 1.0 - us, idx(), idx(),
            torch.rand(n, 1, device=dev, generator=g),
            (torch.rand(n, 1, device=dev, generator=g) - 0.5) * 2000.0,
            torch.randint(1, 33, (n,), dtype=torch.int64, device=dev, generator=g))

cfg = NNUELightningConfig()
model = NNUE(config=cfg, max_epoch=600, num_batches_per_epoch=6103).to(dev)
model.train()
opt = model.configure_optimizers()
if isinstance(opt, dict): opt = opt["optimizer"]
elif isinstance(opt, (list, tuple)):
    opt = opt[0][0] if isinstance(opt[0], (list, tuple)) else opt[0]

g = torch.Generator(device=dev).manual_seed(0)
batch = make_batch(BATCH, ACTIVE, 86992, g)

def step(i):
    out = model.train_step(batch, 0, i)
    opt.zero_grad(set_to_none=True)
    out["loss"].backward()
    opt.step()

for i in range(12):
    step(i)
torch.cuda.synchronize()

with profile(activities=[ProfilerActivity.CPU, ProfilerActivity.CUDA]) as prof:
    for i in range(6):
        step(100 + i)
    torch.cuda.synchronize()

print(prof.key_averages().table(sort_by="self_cuda_time_total", row_limit=22))

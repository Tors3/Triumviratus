"""Coppie di processori logici fratelli (stesso core fisico) nel processor group 0.

Usa GetLogicalProcessorInformationEx(RelationProcessorCore). Stampa le coppie come
"gruppo:cpuA,cpuB". Serve a nps_pair.py per mettere due motori sullo stesso core.
"""
import ctypes
from ctypes import wintypes

RelationProcessorCore = 0


class GROUP_AFFINITY(ctypes.Structure):
    _fields_ = [("Mask", ctypes.c_uint64), ("Group", wintypes.WORD), ("Reserved", wintypes.WORD * 3)]


def core_pairs():
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    size = wintypes.DWORD(0)
    k32.GetLogicalProcessorInformationEx(RelationProcessorCore, None, ctypes.byref(size))
    buf = ctypes.create_string_buffer(size.value)
    if not k32.GetLogicalProcessorInformationEx(RelationProcessorCore, buf, ctypes.byref(size)):
        raise OSError(ctypes.get_last_error())
    out, off = [], 0
    while off < size.value:
        rel = ctypes.c_uint32.from_buffer_copy(buf, off).value
        sz = ctypes.c_uint32.from_buffer_copy(buf, off + 4).value
        if rel == RelationProcessorCore:
            # PROCESSOR_RELATIONSHIP: Flags(1) EfficiencyClass(1) Reserved(20) GroupCount(2) GroupMask[...]
            gcount = ctypes.c_uint16.from_buffer_copy(buf, off + 8 + 22).value
            ga = GROUP_AFFINITY.from_buffer_copy(buf, off + 8 + 24)
            cpus = [i for i in range(64) if ga.Mask >> i & 1]
            out.append((ga.Group, cpus))
        off += sz
    return out


if __name__ == '__main__':
    pairs = core_pairs()
    print(f"core fisici: {len(pairs)}")
    for g, c in pairs:
        print(f"{g}:{','.join(map(str, c))}")

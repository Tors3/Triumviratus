"""Order of the threat groups for CN_THRGROUP_TABLE (02/11/2026, speed).

  python make_thrgroups.py <cn_groupstats.bin> [<more .bin> ...] <out cn_thrgroup.inc>

Reads the reads per group (100 x 64 little-endian uint64, written by a -DCN_ROWSTATS build at exit: id = kind * 64 +
oriented from-square, kind = attacker kind * 10 + target slot) and writes the groups that were read at least once,
most read first. The statistics must come from positions other than those of the speed measurements (fens.txt,
eg64.txt), or the layout would be fitted to the measurement.
"""
import struct, sys, zlib


def main():
    *ins, out = sys.argv[1:]
    tot = [0] * (100 * 64)
    for p in ins:
        data = open(p, 'rb').read()
        vals = struct.unpack(f'<{100 * 64}Q', data)
        for i, v in enumerate(vals):
            tot[i] += v
    order = sorted((i for i in range(100 * 64) if tot[i] > 0), key=lambda i: -tot[i])
    s = sum(tot)
    acc, cut = 0, {}
    for n, i in enumerate(order):
        acc += tot[i]
        for q in (0.5, 0.9, 0.99):
            if q not in cut and acc >= q * s:
                cut[q] = n + 1
    gid = zlib.crc32(struct.pack(f'<{len(order)}H', *order)) & 0xFFFFFFFF
    with open(out, 'w', encoding='utf-8') as f:
        f.write('// Threat groups by reads, most read first (tools/make_thrgroups.py, 02/11/2026). Generated: do not edit.\n')
        f.write(f'// {len(order)} groups read; 50% / 90% / 99% of the reads in the first {cut.get(0.5)} / {cut.get(0.9)} / '
                f'{cut.get(0.99)} groups.\n')
        f.write(f'constexpr std::uint64_t THR_GROUP_ID = 0x{gid:08X}ULL << 16;\n')
        f.write('constexpr std::int16_t THR_GROUP_ORDER[] = {\n')
        for k in range(0, len(order), 20):
            f.write('    ' + ', '.join(str(i) for i in order[k:k + 20]) + ',\n')
        f.write('};\n')
    print(f'{len(order)} groups read of {100 * 64}; 50/90/99% of reads in {cut.get(0.5)}/{cut.get(0.9)}/{cut.get(0.99)} groups')


if __name__ == '__main__':
    main()

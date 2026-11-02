"""Order of the pawn-pair rows for CN_PPORDER_TABLE (02/11/2026, speed).

  python make_pporder.py <cn_ppstats.bin> <out cn_pporder.inc>

Reads the reads per pawn-pair row (4560 little-endian uint64, written by a -DCN_ROWSTATS -DCN_NO_PPORDER_TABLE build
at exit, so that the stored rows are the specification rows) and writes the rows read at least once, most read first.
The statistics must come from positions other than those of the speed measurements.
"""
import struct, sys, zlib

ROWS_PP = 4560


def main():
    src, out = sys.argv[1], sys.argv[2]
    tot = struct.unpack(f'<{ROWS_PP}Q', open(src, 'rb').read())
    order = sorted((i for i in range(ROWS_PP) if tot[i] > 0), key=lambda i: -tot[i])
    s = sum(tot)
    acc, cut = 0, {}
    for n, i in enumerate(order):
        acc += tot[i]
        for q in (0.5, 0.9, 0.99):
            if q not in cut and acc >= q * s:
                cut[q] = n + 1
    oid = zlib.crc32(struct.pack(f'<{len(order)}H', *order)) & 0xFFFFFFFF
    with open(out, 'w', encoding='utf-8') as f:
        f.write('// Pawn-pair rows by reads, most read first (tools/make_pporder.py, 02/11/2026). Generated: do not edit.\n')
        f.write(f'// {len(order)} rows read of {ROWS_PP}; 50% / 90% / 99% of the reads in the first {cut.get(0.5)} / '
                f'{cut.get(0.9)} / {cut.get(0.99)} rows.\n')
        f.write(f'constexpr std::uint64_t PP_ORDER_ID = 0x{oid:08X}ULL << 8;\n')
        f.write('constexpr std::int16_t PP_ORDER[] = {\n')
        for k in range(0, len(order), 20):
            f.write('    ' + ', '.join(str(i) for i in order[k:k + 20]) + ',\n')
        f.write('};\n')
    print(f'{len(order)} rows read of {ROWS_PP}; 50/90/99% of reads in {cut.get(0.5)}/{cut.get(0.9)}/{cut.get(0.99)} rows')


if __name__ == '__main__':
    main()

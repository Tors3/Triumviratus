// Origin: this file descends from BBC (Bit Board Chess) by Maksim Korzh, https://github.com/maksimKorzh/bbc,
// released under the GNU General Public License v3; the project started from BBC in 2024 and has since rewritten
// and extended much of it. Triumviratus is distributed under the GNU GPL v3 (see COPYING).
//
// Empty since 28/10/2026. It held BBC's 32-bit xorshift generator, which built the Zobrist keys from four 16-bit
// pieces (GF(2) rank 32: false hash hits, behind the 37.Re4 blunder of 8.0) and fed the magic-number search,
// which the engine never ran. The Zobrist keys now come from zobrist.cpp (64-bit generator); the magic numbers are
// constants. The file stays because the Makefile, the Visual Studio project and universal/variant.cpp name it.

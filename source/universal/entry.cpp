// Universal binary (08/10/2026): entry point. Compiled WITHOUT instructions beyond base x86-64, it chooses with cpuid the
// best variant the CPU and the operating system support, runs ONLY its global constructors (section
// .tv<n>) and calls its main. Variants (TRIUMV_VID in variant.cpp):
//   5 avx512icl   AVX-512 F/BW/DQ/VL + VNNI + VBMI/VBMI2/BITALG   Ice Lake, Tiger Lake, Sapphire Rapids, Zen 4/5
//   4 vnni512     AVX-512 F/BW/DQ/VL + VNNI                       Cascade Lake, Cooper Lake
//   3 avx512      AVX-512 F/BW/DQ/VL                              Skylake-X, Skylake-SP
//   7 avxvnni     AVX2 + BMI2 + AVX-VNNI                          Intel from Alder Lake without AVX-512 (Raptor, Meteor,
//                                                                 Arrow, Lunar Lake)
//   2 avx2        AVX2 + BMI2 (fast PEXT)                         Intel from Haswell, AMD from Zen 3
//   1 avx2-nopext AVX2 without PEXT                               AMD Excavator, Zen 1/Zen+/Zen 2 (microcoded PEXT)
//   6 sse41       SSE4.1 + POPCNT                                 CPUs without AVX2 (before Haswell, 2013)
// The environment variable TRIUMV_ISA (avx2-nopext, avx2, avx512, vnni512, avx512icl, sse41, avxvnni) forces a
// variant, but only if the CPU supports it: it serves the measurements (same binary, different variants) and the trials.
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void(__cdecl *TvInit)(void);

#define TV_DECLARE(n)                                                                                               \
  namespace Triumv_v##n { int main(); }                                                                             \
  __pragma(section(".tv" #n "$a", read)) __pragma(section(".tv" #n "$z", read))                                    \
  __declspec(allocate(".tv" #n "$a")) const TvInit tv##n##_a = nullptr;                                             \
  __declspec(allocate(".tv" #n "$z")) const TvInit tv##n##_z = nullptr;                                             \
  static int run_v##n() {                                                                                           \
    for (const TvInit *p = &tv##n##_a + 1; p < &tv##n##_z; ++p)                                                    \
      if (*p)                                                                                                       \
        (*p)();                                                                                                     \
    return Triumv_v##n::main();                                                                                     \
  }

TV_DECLARE(1)
TV_DECLARE(2)
TV_DECLARE(3)
TV_DECLARE(4)
TV_DECLARE(5)
TV_DECLARE(6)
TV_DECLARE(7)

// 6 sse41 (15/10/2026): SSE4.1 + POPCNT, for CPUs without AVX2 (before Haswell, 2013); slower.
// 7 avxvnni (15/10/2026): avx2 plus AVX-VNNI, the 256-bit VEX form of vpdpbusd (Intel from Alder Lake without AVX-512):
// the dot product of the first sparse layer and of the dense layers in one instruction instead of three, same values
// (+4.55% NPS on i9-14900KF, identical nodes).
static const char *const kNames[8] = {"", "avx2-nopext", "avx2", "avx512", "vnni512", "avx512icl", "sse41",
                                      "avxvnni"};

// Variants this CPU and the operating system support (best_variant writes them): TRIUMV_ISA can force only these.
// "A lower variant" is not enough: avxvnni (7) does not run on Zen 4 (AVX-512 without AVX-VNNI).
static bool g_can[8];

// Name of the chosen variant, read by the engine for the "id name" line (extern "C": the same symbol for all).
extern "C" const char *g_triumv_isa = "";

static int best_variant() {
  int r[4];
  __cpuid(r, 0);
  const int max_leaf = r[0];
  char vendor[13];
  memcpy(vendor, &r[1], 4);
  memcpy(vendor + 4, &r[3], 4);
  memcpy(vendor + 8, &r[2], 4);
  vendor[12] = 0;
  __cpuid(r, 1);
  const unsigned ecx1 = (unsigned)r[2];
  const unsigned eax1 = (unsigned)r[0];
  const int family = ((eax1 >> 8) & 0xF) + (((eax1 >> 8) & 0xF) == 0xF ? ((eax1 >> 20) & 0xFF) : 0);
  const bool popcnt = ecx1 & (1u << 23), osxsave = ecx1 & (1u << 27), avx = ecx1 & (1u << 28),
             fma = ecx1 & (1u << 12), sse41 = ecx1 & (1u << 19), movbe = ecx1 & (1u << 22), f16c = ecx1 & (1u << 29);
  // LZCNT (extended leaf 0x80000001, ECX bit 5). Audit 26/11/2026: /arch:AVX2 also allows LZCNT, MOVBE and F16C in
  // the AVX2 and AVX-512 variants; every real AVX2 CPU has them, but a virtual CPU may hide one, so a CPU without all
  // three now gets the sse41 variant instead of an illegal-instruction crash.
  __cpuid(r, 0x80000000);
  bool lzcnt = false;
  if ((unsigned)r[0] >= 0x80000001u) {
    __cpuid(r, 0x80000001);
    lzcnt = (unsigned)r[2] & (1u << 5);
  }
  const int fallback = sse41 && popcnt ? 6 : 0;   // without AVX2: the sse41 variant, if the CPU has SSE4.1 and POPCNT
  g_can[6] = fallback != 0;
  if (!(popcnt && osxsave && avx && fma) || max_leaf < 7)
    return fallback;
  const unsigned long long xcr0 = _xgetbv(0);
  if ((xcr0 & 0x6) != 0x6)   // SSE and AVX state saved by the operating system
    return fallback;
  __cpuidex(r, 7, 0);
  const unsigned ebx7 = (unsigned)r[1], ecx7 = (unsigned)r[2], max_sub7 = (unsigned)r[0];
  const bool avx2 = ebx7 & (1u << 5), bmi1 = ebx7 & (1u << 3), bmi2 = ebx7 & (1u << 8);
  if (!(avx2 && bmi1 && lzcnt && movbe && f16c))
    return fallback;
  bool avxvnni = false;   // AVX-VNNI: foglia 7, sottofoglia 1, EAX bit 4
  if (max_sub7 >= 1) {
    __cpuidex(r, 7, 1);
    avxvnni = (unsigned)r[0] & (1u << 4);
  }
  g_can[1] = true;
  g_can[2] = bmi2;
  g_can[7] = bmi2 && avxvnni;
  const bool os_zmm = (xcr0 & 0xE0) == 0xE0;   // opmask and ZMM registers saved by the operating system
  const bool f = ebx7 & (1u << 16), dq = ebx7 & (1u << 17), bw = ebx7 & (1u << 30), vl = ebx7 & (1u << 31);
  const bool vbmi = ecx7 & (1u << 1), vbmi2 = ecx7 & (1u << 6), vnni = ecx7 & (1u << 11), bitalg = ecx7 & (1u << 12);
  if (os_zmm && f && dq && bw && vl && bmi2) {
    g_can[3] = true;
    g_can[4] = vnni;
    g_can[5] = vnni && vbmi && vbmi2 && bitalg;
    return g_can[5] ? 5 : g_can[4] ? 4 : 3;
  }
  // PEXT/PDEP microcodificati: AMD famiglia 15h (Excavator), 17h (Zen 1/Zen+/Zen 2) e Hygon 18h.
  const bool amd_like = !strcmp(vendor, "AuthenticAMD") || !strcmp(vendor, "HygonGenuine");
  const bool slow_pext = amd_like && (family == 0x15 || family == 0x17 || family == 0x18);
  if (!bmi2 || slow_pext)
    return 1;
  return avxvnni ? 7 : 2;
}

int main() {
  const int best = best_variant();
  if (!best) {
    printf("Triumviratus 8.1 needs a CPU with SSE4.1 and POPCNT.\n");
    return 1;
  }
  int v = best;
  if (const char *force = getenv("TRIUMV_ISA")) {
    for (int i = 1; i <= 7; i++)
      if (!strcmp(force, kNames[i])) {
        if (g_can[i])
          v = i;
        else
          fprintf(stderr, "TRIUMV_ISA=%s non supportata da questa CPU: uso %s\n", force, kNames[best]);
      }
  }
  g_triumv_isa = kNames[v];
  switch (v) {
  case 1: return run_v1();
  case 2: return run_v2();
  case 3: return run_v3();
  case 4: return run_v4();
  case 6: return run_v6();
  case 7: return run_v7();
  default: return run_v5();
  }
}

#ifndef TRIUMV_PROFILE_H
#define TRIUMV_PROFILE_H

// Compile-time profiling instrumentation. Define TRIUMV_PROFILE (e.g. /D TRIUMV_PROFILE
// in the build, or `make profile`) to enable per-phase cycle counters
// (eval / movegen / make / tt / score). Default builds (no define) compile every
// PROF_GUARD to a no-op => zero overhead, byte-identical to the release engine.
#ifdef TRIUMV_PROFILE
  #include <intrin.h>
  extern unsigned long long prof_eval, prof_mg, prof_make, prof_tt, prof_score;
  inline unsigned long long prof_now() { return __rdtsc(); }
  struct ProfGuard {
      unsigned long long& c; unsigned long long t;
      ProfGuard(unsigned long long& cc) : c(cc), t(__rdtsc()) {}
      ~ProfGuard() { c += __rdtsc() - t; }
  };
  #define PROF_GUARD(counter) ProfGuard _pg(counter)
#else
  #define PROF_GUARD(counter) ((void)0)
#endif

#endif // TRIUMV_PROFILE_H

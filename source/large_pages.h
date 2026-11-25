// Triumviratus, a UCI chess engine.
// Copyright (C) 2026 Francesco Torsello
// SPDX-License-Identifier: GPL-3.0-or-later (see COPYING)

// large_pages.h -- memory on large pages for the big tables (block 7 of 8.1, 18/10/2026).
//
// The network, the transposition table, the shared search tables and the per-thread data are read at random on
// every node: on 4 KB pages they need thousands of TLB entries, on 2 MB pages a handful.
//
//  * Windows: large pages need the "Lock pages in memory" right (SeLockMemoryPrivilege) in the account and enabled
//    in the process token. large_pages_alloc enables it, asks VirtualAlloc for MEM_LARGE_PAGES (size rounded up to
//    GetLargePageMinimum), puts the token back as it was, and falls back silently to normal pages (VirtualAlloc,
//    zeroed as well) when anything is missing. advapi32 is loaded at run time, so no build needs to link it.
//  * Linux (and Android): memory aligned to 2 MB with madvise(MADV_HUGEPAGE), so that transparent huge pages back it
//    when the kernel allows them. Not zeroed.
//  * Elsewhere: memory aligned to 4 KB.
//
// large_pages_free releases what large_pages_alloc returned (nullptr is allowed). large_pages_available says whether
// a real large page can be obtained (Windows: a 2 MB test allocation; Linux: transparent huge pages enabled).

#ifndef LARGE_PAGES_H_INCLUDED
#define LARGE_PAGES_H_INCLUDED

#include <cstddef>
#include <mutex>

void* large_pages_alloc(std::size_t size);
void  large_pages_free(void* mem);
bool  large_pages_available();

#ifdef _WIN32
// Runs fn(large page size) with the lock-memory privilege enabled and returns its result, then restores the token;
// returns fallback() when the privilege or large-page support is missing. Used for the shared network (shared_net.h).
// The lambdas run in the caller: the template only needs the two plain functions below.
bool   lock_memory_privilege_enable(void** saved);   // true when enabled; *saved = state to restore (or nullptr)
void   lock_memory_privilege_restore(void* saved);
size_t large_page_minimum();
// (audit 26/11/2026) One caller at a time: the privilege is a property of the process token, and two threads moving
// their network handles at once (NumaSpread) enabled and restored it under each other, so some allocations fell back
// to normal pages.
inline std::recursive_mutex& lock_memory_privilege_mutex() {
    static std::recursive_mutex m;
    return m;
}
template <class F, class G>
auto with_lock_memory_privilege(F&& fn, G&& fallback) -> decltype(fallback()) {
    std::lock_guard<std::recursive_mutex> lock(lock_memory_privilege_mutex());
    const size_t lp = large_page_minimum();
    void*        saved = nullptr;
    if (!lp || !lock_memory_privilege_enable(&saved)) return fallback();
    auto result = fn(lp);
    lock_memory_privilege_restore(saved);
    return result;
}
#endif

#endif  // LARGE_PAGES_H_INCLUDED

/*
 * Triumviratus Chess Engine - Main Entry Point
 * UCI compliant - no debug output on startup
 */

#include "defs.h"
#include "io.h"
#include "tt.h"
#include "uci.h"
#include "threads.h"
#include "sf_bridge.h"

//Added for policy network

// Syzygy tablebase probing (Fathom)
#include "syzygy.h"

// Banner di avvio
#include "presentation.h"

#include <string>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>        // readlink (/proc/self/exe) per risolvere la dir dell'eseguibile
#include <sys/resource.h>  // setrlimit(RLIMIT_STACK): stack grande per i thread di ricerca
#endif

// Resolve an NNUE net filename to a path that exists, INDEPENDENT of the current
// working directory. Match runners / GUIs (e.g. cutechess) often launch the
// engine from a different cwd than the project root; the net is loaded by
// relative path, so a wrong cwd used to make the net silently fail to load ->
// eval returns 0 -> the engine plays junk (a2a3...). We search, in order:
//   1) <exe dir>\<name>          (net shipped next to the binary)
//   2) <exe dir>\..\..\<name>    (project root, when exe sits in x64\Release)
//   3) <name>                    (current working directory; legacy fallback)
// Returns the first existing path, or an empty string if none is found.
// NON-static: also used by uci_mt.cpp to resolve a UCI "EvalFile" path at runtime.
std::string resolve_net_path(const std::string& name)
{
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::string exePath(buf, n);
        size_t slash = exePath.find_last_of("\\/");
        if (slash != std::string::npos)
        {
            std::string exeDir = exePath.substr(0, slash);
            const std::string cands[] = { exeDir + "\\" + name,
                                          exeDir + "\\..\\..\\" + name };
            for (const std::string& c : cands)
            {
                std::ifstream f(c, std::ios::binary);
                if (f.good()) return c;
            }
        }
    }
#else
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0)
        {
            std::string exePath(buf, (size_t)n);
            size_t slash = exePath.find_last_of('/');
            if (slash != std::string::npos)
            {
                std::string exeDir = exePath.substr(0, slash);
                const std::string cands[] = { exeDir + "/" + name,
                                              exeDir + "/../../" + name };
                for (const std::string& c : cands)
                {
                    std::ifstream f(c, std::ios::binary);
                    if (f.good()) return c;
                }
            }
        }
    }
#endif
    std::ifstream f(name, std::ios::binary);   // current working directory
    if (f.good()) return name;
    return std::string();
}

// Default Syzygy directory: a "Syzygy" folder next to the executable. When the
// exe runs from x64\Release this resolves to x64\Release\Syzygy. Returned even
// if it does not exist - tb_init() just finds 0 files and stays disabled.
static std::string default_syzygy_dir()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::string exePath(buf, n);
        size_t slash = exePath.find_last_of("\\/");
        if (slash != std::string::npos)
            return exePath.substr(0, slash) + "\\Syzygy";
    }
#else
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0)
        {
            std::string exePath(buf, (size_t)n);
            size_t slash = exePath.find_last_of('/');
            if (slash != std::string::npos)
                return exePath.substr(0, slash) + "/Syzygy";
        }
    }
#endif
    return std::string("Syzygy");
}

int main()
{
#ifndef _WIN32
    // Linux: i thread di ricerca (std::thread = pthread) nascono con stack pari a
    // RLIMIT_STACK (default ~8 MB). La ricorsione di td_negamax (depth + qsearch +
    // estensioni, fino a ply 64) con frame grandi lo sfora -> stack-overflow/SEGV.
    // MSVC linka l'exe con uno stack grande, per questo su Windows non si vede.
    // Alziamo il soft limit a 256 MB PRIMA di creare qualunque thread (i pthread
    // creati dopo ereditano lo stack grande).
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_STACK, &rl) == 0) {
            const rlim_t want = (rlim_t)256 * 1024 * 1024;
            if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < want) {
                rl.rlim_cur = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= want)
                              ? want : rl.rlim_max;
                setrlimit(RLIMIT_STACK, &rl);
            }
        }
    }
#endif
    // Banner di presentazione (appena si apre il motore)
    ascii_art();

    // Initialize all bitboards and attack tables (silent)
    init_bitboards();

    // Initialize hash table with default 64 MB (silent)
    init_hash_table(64);

    // Resolve the NNUE nets relative to the executable so the engine works from
    // ANY working directory. A missing net used to cause a SILENT eval=0 (junk
    // moves under cutechess); now we fail loudly instead of playing garbage.
    // 4.2 = FULLY OWN-NET build: big = our "rubicon" net, small = our "mini-rubicon"
    // net (both own-lineage, no Stockfish nets shipped). Overridable at runtime via
    // UCI "EvalFile" (big). Both nets ship next to the executable.
    const char* bigName   = "nn-rubicon-v1.nnue";
    const char* smallName = "mini-rubicon-v1.nnue";
    std::string bigNet   = resolve_net_path(bigName);
    const std::string smallNet = resolve_net_path(smallName);
    if (bigNet.empty())
    {
        std::cerr << "FATAL: NNUE big net not found. Place " << bigName
                  << " next to the executable (or in the project root) and restart." << std::endl;
        return 1;
    }

    // Initialize the Stockfish HalfKAv2_hm NNUE probe. smallNet.c_str() is "" when
    // absent -> load_networks skips an empty name (NOT nullptr: probe.cpp builds a
    // std::string from it, and nullptr would crash). The small net is OFF by
    // default in 4.2 (g_use_small_net = false): our own small net is a net loss
    // in-game (big-only is stronger); toggle UseSmallNet=true to enable it.
    sf_init(bigNet.c_str(), smallNet.c_str());
    printf("info string Big net: %s   Small net: %s%s\n", bigName, smallName,
           smallNet.empty() ? " (MISSING -> big-net-only)" : "");
    fflush(stdout);

    // (Policy-net: rimossa 2026-06-11 — capitolo chiuso, vedi notes/ §P5.)

    // Auto-load Syzygy tablebases from a "Syzygy" folder next to the exe
    // (x64\Release\Syzygy). The "SyzygyPath" UCI option overrides this at runtime.
    {
        std::string tbDir = default_syzygy_dir();
        if (syzygy_init(tbDir.c_str()))
            printf("info string Syzygy: %u-men tablebases loaded from %s\n",
                   syzygy_max_pieces(), tbDir.c_str());
    }

    // Run UCI loop
    uci_loop();

    // Release Syzygy tablebase memory (no-op if SyzygyPath was never set).
    syzygy_free();

    return 0;
}

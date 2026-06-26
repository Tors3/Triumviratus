/*
 * Triumviratus Chess Engine - Main Entry Point
 * UCI compliant - no debug output on startup
 */

#include "defs.h"
#include "io.h"
#include "tt.h"
#include "uci.h"
#include "threads.h"
#include "nnue_bridge.h"

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

    // Triumviratus 5.0 evaluation = the vendored Stockfish-master SFNNv13 NNUE
    // (FullThreats + HalfKAv2_hm, L1=1024; see nnue/ + nnue_bridge). We init the
    // shared substrate (bitboard + slider-magic attack tables) and load the network.
    // Resolve relative to the exe so the engine works from ANY working directory; a
    // missing net fails loudly. Override at runtime via the UCI "EvalFile" option.
    nn_init_tables();   // bitboards + Attacks::init (substrate for the NNUE feature extraction)
#ifdef TRIUMV_RELEASE
    const char* netName = "nn-rubicon-alea-v1.nnue";   // shipped own-lineage net (5.0 release)
#else
    const char* netName = "nn-71d6d32cb962.nnue";      // dev: SF reference net (sits next to the dev exe)
#endif
    std::string netPath = resolve_net_path(netName);
    if (netPath.empty() || !nn_load_net(netPath.c_str()))
    {
        std::cerr << "FATAL: NNUE net not found/invalid. Place " << netName
                  << " next to the executable (or in the project root) and restart."
                  << std::endl;
        return 1;
    }
    printf("info string Net: %s (Stockfish SFNNv13)\n", netName);
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

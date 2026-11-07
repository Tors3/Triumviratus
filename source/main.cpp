/*
 * Triumviratus Chess Engine - Main Entry Point
 * UCI compliant - no debug output on startup
 */

#include "defs.h"
#include "io.h"
#include "tt.h"
#include "uci.h"
#include "threads.h"
#include "consilium_bridge.h"

// Syzygy tablebase probing (Fathom)
#include "syzygy.h"

// Startup line in a terminal (print_banner)
#include "presentation.h"

#include <string>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>            // _isatty/_fileno: detects whether stdin is a terminal (no GUI)
#define STDIN_IS_TTY (_isatty(_fileno(stdin)) != 0)
#else
#include <unistd.h>        // readlink (/proc/self/exe) to resolve the executable's directory
#include <sys/resource.h>  // setrlimit(RLIMIT_STACK): big stack for the search threads
#define STDIN_IS_TTY (isatty(fileno(stdin)) != 0)
#endif

// SILENT startup when the engine is launched by a GUI (stdin is not a
// TTY). A UCI engine must be SILENT until it answers "uci" with
// id/option/uciok: a banner or info string BEFORE "id name" can corrupt the
// way a strict GUI (ChessBase/Fritz) registers the engine's options
// (e.g. it then never sends SyzygyPath -> tablebases never used). Humans in a
// terminal still see banner + info. Read by main and by consilium_bridge.
bool g_startup_quiet = false;
#ifdef TRIUMV_VID
extern "C" const char *g_triumv_isa;   // universal binary: name of the chosen variant (universal/entry.cpp)
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
    // Linux: the search threads (std::thread = pthread) are born with a stack equal to
    // RLIMIT_STACK (default ~8 MB). The recursion of td_negamax (depth + qsearch +
    // extensions, up to ply 64) with big frames exceeds it -> stack overflow/SEGV.
    // On Windows the thread stack is the reserve written in the exe header. Until 26/09/2026 it was
    // 1 MB (the comment here said the opposite): with max_ply 128 the estimated worst case was ~0.78 MB.
    // Since then the vcxproj sets StackReserveSize = 8 MB (AUDIT D).
    // We raise the soft limit to 256 MB BEFORE creating any thread (the pthreads
    // created afterwards inherit the big stack).
    // Since 05/11/2026 (issue #9) the search threads set their stack explicitly (8 MB,
    // start_search_thread in search/15_threads.inc), because macOS (512 KB) and musl
    // (128 KB) ignore RLIMIT_STACK for new threads. This limit now only covers the main thread.
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
    // Detects whether we were launched by a GUI (stdin non-TTY) -> silent startup.
    g_startup_quiet = !STDIN_IS_TTY;

    // Startup line "Triumviratus - 8.1 by Francesco Torsello" (06/11/2026, issue #10): only in an interactive
    // terminal, never towards a GUI, and before the initialisation, so a hand-started engine shows at once that it runs.
    if (!g_startup_quiet)
        print_banner();

    // Initialize all bitboards and attack tables (silent)
    init_bitboards();

    // Initialize hash table with default 64 MB (silent)
    init_hash_table(64);

    // Evaluation = the Consilium network (threats + piece-square with four phase
    // experts + pawn pairs + passed pawns, L1=1024; module consilium/, link consilium_bridge).
    // We init the shared substrate (bitboard + slider-magic attack tables) and load the
    // network. Resolve relative to the exe so the engine works from ANY working directory;
    // a missing net fails loudly. Override at runtime via the UCI "EvalFile" option.
    nn_init_tables();   // feature index tables of the network
    // Own-lineage net, dev and release builds alike (2026-07-05: dev builds used to default to
    // a reference net here, which silently made a dev-build bench/SPRT run on
    // different weights than a release build unless EvalFile was set explicitly -> never
    // again; both build kinds load our own net by default now).
    // TRANN3 (4 blocks: threats+halfka+pawnpair+passedpawns). ONE single source of truth
    // for the name (CN_DEFAULT_NET_FILE in consilium_bridge.cpp), exposed via the bridge: a string
    // duplicated here already caused a near-incident (a release that shipped the zerograft net).
    const char* netName = nn_default_net_name();
    std::string netPath = resolve_net_path(netName);
    // A file next to the exe/project (if present) ALWAYS WINS - a handy override in
    // development. Without a file, the bare name routes to the net EMBEDDED in the exe
    // (RCDATA resource on Windows, incbin on Linux/Android). "EvalFile" at runtime
    // keeps loading any explicit path from disk.
    const char* loadArg = netPath.empty() ? netName : netPath.c_str();
    if (!nn_load_net(loadArg))
    {
        std::cerr << "FATAL: NNUE net not found/invalid (no " << netName
                  << " next to the executable/project root, and no embedded net in "
                  << "this build). Place the file and restart." << std::endl;
        return 1;
    }
    if (!g_startup_quiet) {
        printf("info string Net: %s (%s; Consilium = TRANN3, mixture of 8 phase experts, threat + HalfKA + PawnPair + PassedPawns inputs)\n",
               netName, netPath.empty() ? "embedded" : netPath.c_str());
#ifdef TRIUMV_VID
        // Universal binary: the variant chosen for this CPU (terminal only; the id name does not change).
        printf("info string Build: universal, variant %s\n", g_triumv_isa);
#endif
        fflush(stdout);
    }



    // Auto-load Syzygy tablebases from a "Syzygy" folder next to the exe
    // (x64\Release\Syzygy). The "SyzygyPath" UCI option overrides this at runtime.
    // Audit D (27/09/2026, kept on purpose): it fires only if that folder exists, i.e. if someone put it there;
    // a GUI that sends an empty SyzygyPath (or "<empty>") unloads them (syzygy_init), so a
    // tester's conditions stay those the tester sets. The info string below makes the loading visible.
    {
        std::string tbDir = default_syzygy_dir();
        if (syzygy_init(tbDir.c_str()) && !g_startup_quiet)
            printf("info string Syzygy: %u-men tablebases loaded from %s\n",
                   syzygy_max_pieces(), tbDir.c_str());
    }

    // Run UCI loop
    uci_loop();

    // Release Syzygy tablebase memory (no-op if SyzygyPath was never set).
    syzygy_free();

    return 0;
}

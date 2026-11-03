// Origin: this file descends from BBC (Bit Board Chess) by Maksim Korzh, https://github.com/maksimKorzh/bbc,
// released under the GNU General Public License v3; the project started from BBC in 2024 and has since rewritten
// and extended much of it. Triumviratus is distributed under the GNU GPL v3 (see COPYING).
/*
 * UCI Protocol Implementation for Triumviratus Chess Engine
 * Fully compliant with UCI specification
 * https://www.shredderchess.com/chess-features/uci-universal-chess-interface.html
 */

#include "defs.h"
#include "uci.h"
#include <fstream>
#include <string>
#include <vector>
#include "movegen.h"
#include "search.h"
#include "tt.h"
#include "misc.h"
#include "io.h"
#include "threads.h"
#include "syzygy.h"
#include "perft.h"
#include "chess960.h"
#include "consilium_bridge.h"   // nn_acc_stats (diagnostic "accstats" command)
#include <algorithm>       // std::sort (access histogram, TRIUMV_PROFILE only)
#include <thread>

#ifdef CLANG_PGO_GEN
// clang-PGO instrument build (-fprofile-generate): the engine's exit path does not
// trigger LLVM's atexit -> the quit handler calls this to write the profile.
// extern "C" must be at file scope (NOT inside a block). Absent in normal/optimize builds.
extern "C" int __llvm_profile_write_file(void);
#endif
#include <string.h>
#include <string>
#ifndef _WIN32
#include <unistd.h>   // getpid() for the per-process suffix of TMLog (on Windows
                      // GetCurrentProcessId comes from windows.h via defs.h)
#endif

// Defined in main.cpp: resolve an NNUE filename/path to an existing path
// (tries the path as given, then next to the exe, then cwd). Used by the UCI
// "EvalFile" handler to load a big net specified at runtime.
std::string resolve_net_path(const std::string& name);

// "Move Overhead" (UCI): ms reserved per move for I/O and GUI lag. 10, as in the time management we use.
int g_move_overhead_ms = 10;

// parse user/GUI move string input (e.g. "e7e8q")
int parse_move(char* move_string)
{
    moves move_list[1];
    generate_moves(move_list);

    int source_square = (move_string[0] - 'a') + (8 - (move_string[1] - '0')) * 8;
    int target_square = (move_string[2] - 'a') + (8 - (move_string[3] - '0')) * 8;

    for (int move_count = 0; move_count < move_list->count; move_count++)
    {
        int move = move_list->moves[move_count];
        // UCI_Chess960: the GUI sends castling as "king captures its own rook" (e1h1).
        const int move_target = (g_chess960 && get_move_castling(move))
            ? castle_rook_sq[castle_index(get_move_target(move))] : get_move_target(move);

        if (source_square == get_move_source(move) && target_square == move_target)
        {
            int promoted_piece = get_move_promoted(move);

            if (promoted_piece)
            {
                if ((promoted_piece == Q || promoted_piece == q) && move_string[4] == 'q')
                    return move;
                else if ((promoted_piece == R || promoted_piece == r) && move_string[4] == 'r')
                    return move;
                else if ((promoted_piece == B || promoted_piece == b) && move_string[4] == 'b')
                    return move;
                else if ((promoted_piece == N || promoted_piece == n) && move_string[4] == 'n')
                    return move;
                continue;
            }
            return move;
        }
    }
    return 0;
}

// parse UCI "position" command

void parse_position(char* command)
{
    command += 9;
    char* current_char = command;

    if (strncmp(command, "startpos", 8) == 0)
        parse_fen(start_position);
    else
    {
        current_char = strstr(command, "fen");
        if (current_char == NULL)
            parse_fen(start_position);
        else
        {
            current_char += 4;
            parse_fen(current_char);
        }
    }

    current_char = strstr(command, "moves");

    if (current_char != NULL)
    {
        current_char += 6;

        while (*current_char)
        {
            int move = parse_move(current_char);

            if (move == 0)
                break;

            // F-016 (2026-07-03): overflow guard for repetition_table[2048] (ultra-long games
            // in datagen/ultrabullet): drops the OLDEST half of the history - inert,
            // it is beyond any fifty-move window (<=100 plies) used by the repetition check.
            if (repetition_index >= 2048 - 300) {
                memmove(repetition_table, repetition_table + 1024,
                        (size_t)(repetition_index + 1 - 1024) * sizeof(repetition_table[0]));
                repetition_index -= 1024;
            }
            repetition_index++;
            repetition_table[repetition_index] = hash_key;

            make_move(move, all_moves);

            while (*current_char && *current_char != ' ') current_char++;
            // AUDIT D (T6): several spaces between moves are allowed by the protocol; before, a double space
            // stopped the parsing and the following moves were silently lost.
            while (*current_char == ' ' || *current_char == '\t') current_char++;
        }
    }
}

// UCI option "Depth": 0 = off (use time / explicit "go depth"); >0 = force a
// fixed search depth and ignore the clock (handy for testing / fixed strength).
// An explicit "go depth N" in the command still overrides this.
int g_uci_depth = 0;

// Clears the search limits before every "go" (and the bench).
void reset_time_control()
{
    quit = 0;
    stopped = 0;
    starttime = 0;
    g_limits = SearchLimits{};
}

// Integer value after a keyword of the "go" command (0 if the word is missing).
static long long go_value(const char* command, const char* key)
{
    const size_t klen = strlen(key);
    for (const char* p = strstr(command, key); p; p = strstr(p + 1, key))
    {
        const bool starts = p == command || p[-1] == ' ';
        const bool ends   = p[klen] == ' ' || p[klen] == '\0';
        if (starts && ends)
            return strtoll(p + klen, nullptr, 10);
    }
    return 0;
}

static bool go_flag(const char* command, const char* key)
{
    const size_t klen = strlen(key);
    for (const char* p = strstr(command, key); p; p = strstr(p + 1, key))
        if ((p == command || p[-1] == ' ') && (p[klen] == ' ' || p[klen] == '\0'))
            return true;
    return false;
}

// parse UCI command "go"
void parse_go(char* command)
{
    reset_time_control();
    SearchLimits& L = g_limits;
    // Negative clock (GUI with a margin): counts as minimum time, not as "no clock".
    L.time[white] = (int)go_value(command, "wtime");
    L.time[black] = (int)go_value(command, "btime");
    if (go_flag(command, "wtime") && L.time[white] <= 0) L.time[white] = 1;
    if (go_flag(command, "btime") && L.time[black] <= 0) L.time[black] = 1;
    L.inc[white]  = (int)go_value(command, "winc");
    L.inc[black]  = (int)go_value(command, "binc");
    L.movestogo   = (int)go_value(command, "movestogo");
    L.movetime    = (int)go_value(command, "movetime");
    L.mate        = (int)go_value(command, "mate");
    L.nodes       = (U64)go_value(command, "nodes");
    L.infinite    = go_flag(command, "infinite");
    L.ponder      = go_flag(command, "ponder");
    int depth     = (int)go_value(command, "depth");

    // "go searchmoves m1 m2 ...": the root searches only these. The list ends at the first token that is not a move.
    g_searchmoves_count = 0;
    if (const char* sm = strstr(command, "searchmoves"))
    {
        const char* p = sm + 11;
        while (*p == ' ') p++;
        while (*p && g_searchmoves_count < 256)
        {
            char tok[8];
            int n = 0;
            while (p[n] && p[n] != ' ' && n < 7) { tok[n] = p[n]; n++; }
            tok[n] = '\0';
            const int mv = parse_move(tok);
            if (!mv) break;
            g_searchmoves[g_searchmoves_count++] = mv;
            p += n;
            while (*p == ' ') p++;
        }
    }

    // Option "Depth" > 0: fixed depth and clock ignored, unless an explicit "go depth N" is given.
    if (!depth && g_uci_depth > 0)
    {
        depth = g_uci_depth;
        L.time[white] = L.time[black] = 0;
        L.movetime = 0;
    }

    starttime = get_time_ms();
    launch_search(depth);
}

// ---------------------------------------------------------------------------
// setoption with free spacing (block 7 of 8.1, 18/10/2026).
// The line is read as words separated by blanks (space, tab, CR, LF): "setoption", "name", the words of the option
// name up to the word "value", then the words of the value. Name and value are each rebuilt with one space between
// words, so a GUI that sends extra or missing spaces around a name with spaces or a path (SyzygyPath) is understood
// the same way. Returns true when the line sets option `want`; the value (possibly empty, cut to outsz - 1 chars)
// goes to out. Names longer than 255 characters are cut before the comparison.

// Next word of a line: start and length, advancing p past it; false at the end of the line.
static bool next_word(const char*& p, const char*& word, size_t& len)
{
    auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (*p && blank(*p)) ++p;
    if (!*p) return false;
    word = p;
    while (*p && !blank(*p)) ++p;
    len = size_t(p - word);
    return true;
}

// Appends a word to a bounded string, with one space before it when the string is not empty.
static void append_word(char* dst, size_t cap, size_t& used, const char* word, size_t len)
{
    if (used && used + 1 < cap) dst[used++] = ' ';
    const size_t n = used + len < cap ? len : (cap > used + 1 ? cap - used - 1 : 0);
    memcpy(dst + used, word, n);
    used += n;
    dst[used] = '\0';
}

static bool parse_setoption(const char* input, const char* want,
                            char* out, size_t outsz)
{
    const char* p = input;
    const char* w;
    size_t      n;
    auto is = [&](const char* s) { return strlen(s) == n && memcmp(w, s, n) == 0; };
    if (!next_word(p, w, n) || !is("setoption")) return false;
    if (!next_word(p, w, n) || !is("name")) return false;

    char   name[256];
    size_t nameLen = 0;
    name[0] = '\0';
    bool more;
    while ((more = next_word(p, w, n)) && !is("value"))
        append_word(name, sizeof(name), nameLen, w, n);
    if (strcmp(name, want) != 0) return false;

    size_t outLen = 0;
    out[0] = '\0';
    while (more && next_word(p, w, n))
        append_word(out, outsz, outLen, w, n);
    return true;
}

// main UCI loop - fully compliant with UCI protocol
void uci_loop()
{
    // Input buffer
    static char input[65536];   // AUDIT D: was 10000 (~1,990 plies); beyond that, fgets split the line
    
    // Engine settings
    // 🔴 2026-09-07: was 1024 MB. Too little for those who test us at long TC (CCRL
    // uses 256, CEGT more, TCEC runs at 64 GB). All the TT arithmetic is
    // already U64 (hash_entries, tt_base_index, the indexing), so the cap
    // was only a constant. At 65536 MB the table has 2.86e9 entries: above the
    // int32 range, which is why it is raised only after checking
    // that no index is an int (checked: none).
    int max_hash = 65536;
    int mb = 64;
    
    // Detect available threads
    int max_threads = std::thread::hardware_concurrency();
    if (max_threads < 1) max_threads = 1;
    if (max_threads > MAX_THREADS) max_threads = MAX_THREADS;
    
    // Initialize with 1 thread. NOTE: init_threads() also builds the LMR
    // reduction table (init_lmr_table). Without this call the table stays
    // all-zero and Late Move Reductions are effectively disabled.
    init_threads(1);

    // Start position on launch (24/10/2026, GitHub issue #7): a "go" (or "d", "eval") sent before any "position"
    // searched an empty board without kings and crashed. Now the engine starts as if it had received
    // "position startpos".
    parse_fen(start_position);

    // Buffered I/O (12/10/2026). Before, unbuffered: every printf of an "info" line (one per move of the pv)
    // became a write on the pipe, ~2.5-5 ms lost per move (go nodes 1000: 4.1 ms wall against 1.4).
    // Output stays prompt: fflush after every info, currmove and bestmove line, and the loop below does fflush
    // before every read (uciok, readyok, info string). Buffered stdin: nobody reads stdin behind our back any more
    // during the search (read_input/communicate of misc.cpp are not called).
    setvbuf(stdin, NULL, _IOFBF, 1 << 16);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    // Main UCI loop
    while (1)
    {
        memset(input, 0, sizeof(input));
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
        {
            // EOF / closed stdin: behave like "quit" instead of busy-looping.
            stop_search_threads();
            wait_for_search_done();
            break;
        }

        if (input[0] == '\n')
            continue;

        // Remove newline (AUDIT D, T6: also '\r' and trailing spaces; "uci\r" from a GUI did not get uciok)
        size_t len = strlen(input);
        while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r' ||
                           input[len-1] == ' ' || input[len-1] == '\t'))
            input[--len] = '\0';

        char szpath[4096];   // scratch for the SyzygyPath value (parse_setoption)

        // UCI command: "uci" (EXACT match: strncmp len 3 would also swallow
        // "ucinewgame" -> its handler below would be dead code).
        if (strcmp(input, "uci") == 0)
        {
            // NB: VERSION already contains the separator (" - 7.0") -> %s%s, without a space
            // in between. With "%s %s" it printed "Triumviratus  - 7.0" (double space): the id
            // name is the engine's identity for GUIs and rating lists, it must be clean.
            // The build date at the end tells the daily builds apart:
            // "Triumviratus - 7.0 2026-08-06".
            // In the universal binary too the id name stays this: it is the name GUIs and lists (CCRL) record. The
            // chosen variant is read in the startup line (main.cpp), in a terminal only.
            // Official release (14/10/2026, -DTRIUMV_OFFICIAL from build_universal.ps1 -Official): name without date.
#ifdef TRIUMV_OFFICIAL
            printf("id name %s%s\n", NAME, VERSION);
#else
            printf("id name %s%s %s\n", NAME, VERSION, build_date());
#endif
            printf("id author %s\n", AUTHOR);
            printf("option name Hash type spin default 64 min 1 max %d\n", max_hash);
            printf("option name Threads type spin default 1 min 1 max %d\n", max_threads);
            printf("option name NumaSpread type check default %s\n", g_numa_spread ? "true" : "false");
            // MultiPV (multi-line analysis): OFFICIAL option, always advertised
            // (also in TRIUMV_RELEASE). The setoption arrives via the generic handler
            // -> set_search_param("MultiPV") in threads.cpp.
            printf("option name MultiPV type spin default 1 min 1 max 64\n");
            // Official analysis/usage options (always visible, also in release):
            printf("option name UCI_ShowWDL type check default false\n");  // W/D/L in the info lines (via generic handler)
            printf("option name UCI_Chess960 type check default false\n"); // Fischer Random (chess960.h), since 8.0
            // Analysis mode (09/10/2026, search/01_params.inc): selectivity for analysis, off in games.
            // The setoption goes through the generic handler (set_search_param), also in the release.
            print_analysis_options();
            printf("option name Clear Hash type button\n");                // clears the TT on request
            // Ponder: the GUI decides whether to ponder. Since 04/10/2026 the option on gives 25% more optimum time
            // (search/14_deepen.inc, clock_plan). Without declaring it many GUIs never send `go ponder`.
            printf("option name Ponder type check default false\n");
#ifdef TRIUMV_OFFICIAL
            printf("option name UCI_EngineAbout type string default %s%s by %s\n", NAME, VERSION, AUTHOR);
#else
            printf("option name UCI_EngineAbout type string default %s%s build %s by %s\n", NAME, VERSION, build_date(), AUTHOR);
#endif
            printf("option name Move Overhead type spin default 10 min 0 max 5000\n"); // ms reserved per move for lag/GUI
            printf("option name EvalFile type string default %s\n", nn_default_net_name());
            // Standard Syzygy group: the ChessBase/Fritz GUIs recognise an engine with tablebases from the full set.
            printf("option name SyzygyPath type string default <empty>\n");
            printf("option name SyzygyProbeDepth type spin default 1 min 1 max 100\n");
            printf("option name Syzygy50MoveRule type check default true\n");
            printf("option name SyzygyProbeLimit type spin default 7 min 0 max 7\n");
#ifndef TRIUMV_RELEASE
            // --- Development and tuning options (hidden in the release) ---
            printf("option name Depth type spin default 0 min 0 max 64\n");
            printf("option name DataLog type check default false\n");
            printf("option name DataFile type string default triumviratus_dataset.txt\n");
            printf("option name EvalOff type check default false\n");
            printf("option name FinnyTables type check default true\n");
            printf("option name LargePages type spin default 1 min 0 max 1\n");
            printf("option name EvalScale type spin default 60 min 10 max 2000\n");
            printf("option name PassedScale type spin default 100 min 50 max 200\n");
            for (int b = 0; b < 8; ++b)
                printf("option name EvalScaleB%d type spin default %d min 20 max 150\n", b, nn_get_eval_scale_bucket(b));
            for (int i = 0, n = nn_eval_const_count(); i < n; ++i)
                printf("option name %s type spin default %d min %d max %d\n",
                       nn_eval_const_name(i), nn_eval_const_get(i),
                       nn_eval_const_lo(i), nn_eval_const_hi(i));
            print_search_options();   // search parameters (search/01_params.inc)
#elif defined(TRIUMV_TUNE_TIME)
            print_search_options();   // tuning build of the time management: only the tuned levers (01_params.inc)
#endif
            // (SyzygyPath moved to the top of the list, right after EvalFile - see above:
            //  it avoids the truncation of long lists in the ChessBase/Fritz GUIs.)
            printf("uciok\n");
            fflush(stdout);
        }

        // UCI command: "isready"
        else if (strncmp(input, "isready", 7) == 0)
        {
            // AUDIT D (T3/S4): same mutex as info and bestmove of the search thread (stdout unbuffered),
            // otherwise an isready during the search could produce "bestmove readyok".
            extern std::mutex output_mutex;
            std::lock_guard<std::mutex> out_lock(output_mutex);
            // Shared network (01/10/2026): the state is told once, at the first isready (when started by a GUI the engine
            // is silent until the handshake). In builds without TRIUMV_SHARED_NET the string is empty and nothing is printed.
            static bool net_mem_said = false;
            if (!net_mem_said && *nn_net_memory_status()) {
                printf("info string NNUE weights: %s\n", nn_net_memory_status());
                net_mem_said = true;
            }
            printf("readyok\n");
            fflush(stdout);
        }

        // DIAGNOSTIC: "accstats" -> print accumulator refresh vs incremental
        // counts (and refresh %) since the last call, then reset. Used to gauge
        // the finny-tables ceiling (refresh fraction).
        else if (strncmp(input, "accstats", 8) == 0)
        {
            nn_acc_stats();
        }

#ifdef TRIUMV_PROFILE
        // "featdump <file>" -> writes the access histogram per row of
        // threatWeights, which feeds gen_feat_perm.py (locality permutation).
        // ⚠️ The counts are in the CURRENT index space: collecting them on a
        // binary that already has a non-identity permutation would give a table to
        // COMPOSE with that one, not to use in its place.
        else if (strncmp(input, "featdump ", 9) == 0)
        {
            const char* path = input + 9;
            FILE*       f    = fopen(path, "w");
            if (!f)
                printf("info string featdump: impossibile aprire %s\n", path);
            else
            {
                unsigned long long tot = 0;
                for (int i = 0; i < PROF_FEAT_N; i++)
                    if (prof_feat_hist[i])
                    {
                        fprintf(f, "%d %llu\n", i, (unsigned long long) prof_feat_hist[i]);
                        tot += prof_feat_hist[i];
                    }
                fclose(f);
                // HalfKA histogram in the same pass, on <path>.psq
                char p2[1024];
                snprintf(p2, sizeof(p2), "%s.psq", path);
                FILE* g = fopen(p2, "w");
                if (g)
                {
                    for (int i = 0; i < PROF_PSQ_N; i++)
                        if (prof_psq_hist[i])
                            fprintf(g, "%d %llu\n", i, (unsigned long long) prof_psq_hist[i]);
                    fclose(g);
                }
                // Sparse co-occurrence on <path>.cooc: only the pairs above a threshold,
                // otherwise the file would be gigabytes.
                if (prof_cooc)
                {
                    char p3[1024];
                    snprintf(p3, sizeof(p3), "%s.cooc", path);
                    FILE* h = fopen(p3, "w");
                    if (h)
                    {
                        long long np = 0;
                        for (int i = 0; i < PROF_COOC_N; i++)
                            for (int j = i + 1; j < PROF_COOC_N; j++)
                            {
                                unsigned c = prof_cooc[(size_t) i * PROF_COOC_N + j];
                                if (c >= 16)
                                {
                                    fprintf(h, "%d %d %u\n", i, j, c);
                                    np++;
                                }
                            }
                        fclose(h);
                        printf("info string coocdump: %s, %lld coppie\n", p3, np);
                    }
                }
                printf("info string featdump: %s scritto, %llu accessi (+ %s)\n", path, tot, p2);
            }
            fflush(stdout);
        }
#endif

        // M3: incremental NNUE eval toggle (default OFF = M2 full-refresh). The
        // search threads must be idle + re-set; safe to send before a search/bench.
        else if (strncmp(input, "incremental ", 12) == 0)
        {
            int on = strncmp(input + 12, "on", 2) == 0;
            nn_set_incremental(on);
            printf("incremental %s\n", on ? "on" : "off");
            fflush(stdout);
        }
        // M3 DEBUG: cross-check incremental == full-refresh at every leaf eval.
        else if (strncmp(input, "nnueverify ", 11) == 0)
        {
            int on = strncmp(input + 11, "on", 2) == 0;
            nn_set_verify(on);
            printf("nnueverify %s\n", on ? "on" : "off");
            fflush(stdout);
        }
        // (07/10/2026: the `lazymirror` command removed. The network's dirty entries are written by the make, there is
        // no lazy catch-up to turn on or off any more.)

        // "bench [depth]" - fixed suite of 8 positions at fixed depth
        // (default 13): CANONICAL node count (node identity in one command) +
        // NPS. Deterministic at Threads=1 (the default). State cleared as by
        // ucinewgame before every position.
        else if (strncmp(input, "bench", 5) == 0)
        {
            // "bench [depth] [file.epd]": the file replaces the 8 fixed positions, one
            // FEN per line (max 64). Without a file the command is IDENTICAL to before: the canary
            // signature (plain bench) does not change. It serves the PER-POSITION profile, because
            // two engines distribute the nodes among the 8 positions differently and the
            // aggregate average mixes 2-file endgames with 12-file middlegames.
            int  bdepth     = 0;
            char bfile[512] = {0};
            sscanf(input + 5, "%d %511s", &bdepth, bfile);
            if (bdepth <= 0) bdepth = 13;
            static const char* bench_fens[8] = {
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
                "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
                "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
                "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
                "8/2k5/3p4/p2P1p2/P2P1P2/8/2K5/8 w - - 0 1",
                "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N w - - 0 1",
            };
            stop_search_threads();
            wait_for_search_done();
            U64 bench_nodes = 0;
            int bench_t0 = get_time_ms();
#ifdef TRIUMV_PROFILE
            prof_eval = prof_mg = prof_make = prof_tt = prof_score = 0;
            prof_ft = prof_fc0 = prof_layers = prof_catchup = 0;
            prof_acc_inc = prof_acc_refresh = prof_ft_out = 0;
            prof_n_inc = prof_n_refresh = prof_n_eval = 0;
            prof_n_cols = prof_n_upd = 0;
            prof_n_thr_seen = prof_n_thr_dead = 0;
            prof_max_active = prof_max_inc = 0;
            prof_cols_thr = prof_cols_pawn = prof_n_refresh_calls = 0;
            prof_cols_psq_inc = prof_cols_thr_inc = prof_cols_pawn_inc = 0;
            prof_mp = prof_hist = prof_corr = prof_gc = prof_n_mg = 0;
            prof_thr = prof_see = prof_isatk = prof_rep = 0;
            prof_idx_thr = prof_idx_pawn = 0;
            prof_n_thr_calls = prof_n_see = prof_n_isatk = 0;
            unsigned long long prof_wall = 0;
#endif
            static std::vector<std::string> bench_file_fens;
            const char* bench_list[64];
            int         bench_n = 8;
            for (int bi = 0; bi < 8; bi++) bench_list[bi] = bench_fens[bi];
            if (bfile[0]) {
                bench_file_fens.clear();
                std::ifstream bf(bfile);
                std::string   line;
                while (std::getline(bf, line))
                    if (line.size() > 10 && bench_file_fens.size() < 64) bench_file_fens.push_back(line);
                bench_n = 0;
                for (const auto& l : bench_file_fens) bench_list[bench_n++] = l.c_str();
                if (bench_n == 0) { printf("bench: nessuna FEN in %s\n", bfile); fflush(stdout); continue; }
            }
            for (int bi = 0; bi < bench_n; bi++) {
                parse_fen((char*)bench_list[bi]);
                clear_hash_table();
                search_clear();   // every bench position starts from clean statistics
                reset_time_control();
#ifdef TRIUMV_PROFILE
                unsigned long long _w = prof_now();
#endif
                launch_search(bdepth);
                wait_for_search_done();
#ifdef TRIUMV_PROFILE
                prof_wall += prof_now() - _w;
#endif
                U64 bn = 0;
                for (int i = 0; i < num_threads; i++) bn += thread_data[i].nodes;
                bench_nodes += bn;
                printf("info string bench pos %d/8 nodes %llu\n", bi + 1, (unsigned long long)bn);
                fflush(stdout);
            }
            int bench_el = get_time_ms() - bench_t0;
            if (bench_el <= 0) bench_el = 1;
            printf("===========================\n");
            printf("Nodes searched  : %llu\n", (unsigned long long)bench_nodes);
            printf("Time (ms)       : %d\n", bench_el);
            printf("Nodes/second    : %llu\n", (unsigned long long)((bench_nodes * 1000) / bench_el));
#ifdef TRIUMV_PROFILE
            { unsigned long long pw = prof_wall ? prof_wall : 1;
              unsigned long long acc = prof_eval + prof_mg + prof_make + prof_tt + prof_score;
              printf("--- PROFILE (%% of search wall) ---\n");
              printf("  eval (NNUE fwd) : %5.1f%%\n", 100.0 * (double)prof_eval  / (double)pw);
              // Sub-buckets of the search: they are INSIDE "other" (their guards are in
              // functions called outside the five main guards), so they must be read
              // as a breakdown of that 26-28%, not added to the others.
              // Entries of the rewritten search (04/10/2026): ordering = the partial ordering only; history =
              // update of the statistics at the end of the node; gives-check = legality and check before the move.
              printf("    movepicker    : %5.1f%%   (dentro 'other')\n", 100.0 * (double)prof_mp   / (double)pw);
              printf("    corr history  : %5.1f%%   (dentro 'other')\n", 100.0 * (double)prof_corr / (double)pw);
              printf("    cont history  : %5.1f%%   (dentro 'other')\n", 100.0 * (double)prof_hist / (double)pw);
              printf("    gives-check   : %5.1f%%   (dentro 'other')\n", 100.0 * (double)prof_gc   / (double)pw);
              printf("    ripetizioni   : %5.1f%%   (dentro 'other')\n", 100.0 * (double)prof_rep  / (double)pw);
              printf("    movegen       : %llu chiamate (%.2f/nodo)\n", (unsigned long long)prof_n_mg,
                     (double)prof_n_mg / (double)(bench_nodes ? bench_nodes : 1));
              printf("    threat masks  : %5.1f%%   (%.2f/nodo)\n", 100.0 * (double)prof_thr / (double)pw,
                     (double)prof_n_thr_calls / (double)(bench_nodes ? bench_nodes : 1));
              printf("    SEE           : %5.1f%%   (%.2f/nodo)\n", 100.0 * (double)prof_see / (double)pw,
                     (double)prof_n_see / (double)(bench_nodes ? bench_nodes : 1));
              printf("    is_sq_attacked: %5.1f%%   (%.2f/nodo)\n", 100.0 * (double)prof_isatk / (double)pw,
                     (double)prof_n_isatk / (double)(bench_nodes ? bench_nodes : 1));
              printf("  indici update: threat %4.1f%%   pedoni(PawnPair+Passed) %4.1f%%   [SF non ha PassedPawns]\n",
                     100.0 * (double)prof_idx_thr / (double)pw, 100.0 * (double)prof_idx_pawn / (double)pw);
              printf("  movegen         : %5.1f%%\n", 100.0 * (double)prof_mg    / (double)pw);
              printf("  make+unmake     : %5.1f%%\n", 100.0 * (double)prof_make  / (double)pw);
              printf("  tt probe+store  : %5.1f%%\n", 100.0 * (double)prof_tt    / (double)pw);
              printf("  move scoring    : %5.1f%%\n", 100.0 * (double)prof_score / (double)pw);
              printf("  other (search)  : %5.1f%%\n", 100.0 * (double)(pw - acc) / (double)pw);
              // Breakdown of the NNUE forward. The question that blocks the work on the
              // sparsity of L1: `ft_optimize` was archived with the condition "if fc_0
              // is below 5% of the time, closed", and that measurement was never made.
              unsigned long long nn = prof_ft + prof_fc0 + prof_layers;
              if (nn) {
                printf("--- NNUE forward (%% del wall / %% del forward) ---\n");
                printf("  feature transf. : %5.1f%% / %5.1f%%\n",
                       100.0 * (double)prof_ft     / (double)pw, 100.0 * (double)prof_ft     / (double)nn);
                printf("  fc_0 (sparse)   : %5.1f%% / %5.1f%%\n",
                       100.0 * (double)prof_fc0    / (double)pw, 100.0 * (double)prof_fc0    / (double)nn);
                printf("  altri layer     : %5.1f%% / %5.1f%%\n",
                       100.0 * (double)prof_layers / (double)pw, 100.0 * (double)prof_layers / (double)nn);
                printf("  (fc_0 sotto il 5%% del wall => ft_optimize e' chiuso)\n");
                // The gap between `eval` and the forward: late threats and pawns + rest of the bridge.
                // --- Concentration of the accesses to the rows of threatWeights ---
                // Decides whether the LOCALITY PERMUTATION makes sense. A row is 1024 bytes
                // (int8 x OutputDimensions) => 4 rows per 4 KB page.
                {
                    static unsigned long long h[PROF_FEAT_N];
                    unsigned long long tot = 0;
                    int used = 0;
                    for (int i = 0; i < PROF_FEAT_N; i++) {
                        h[i] = prof_feat_hist[i];
                        tot += h[i];
                        if (h[i]) used++;
                    }
                    if (tot) {
                        std::sort(h, h + PROF_FEAT_N,
                                  [](unsigned long long a, unsigned long long b) { return a > b; });
                        printf("--- accessi alle righe threatWeights (candidato permutazione) ---\n");
                        printf("  righe totali %d, TOCCATE %d (%.1f%%), accessi %llu\n",
                               PROF_FEAT_N, used, 100.0 * used / PROF_FEAT_N, tot);
                        const double fr[] = {0.01, 0.05, 0.10, 0.25, 0.50};
                        for (double f : fr) {
                            int k = (int)(PROF_FEAT_N * f);
                            unsigned long long c = 0;
                            for (int i = 0; i < k; i++) c += h[i];
                            printf("    top %4.0f%% delle righe (%5d) = %5.1f%% degli accessi\n",
                                   f * 100, k, 100.0 * (double)c / (double)tot);
                        }
                        // How many rows cover 90% of the accesses, and how much space
                        // they would take if they were CONTIGUOUS (today they are spread over 63 MB).
                        unsigned long long acc = 0;
                        int k90 = 0;
                        while (k90 < PROF_FEAT_N && acc < tot * 9 / 10) acc += h[k90++];
                        printf("  il 90%% degli accessi sta in %d righe = %.1f MB se contigue\n",
                               k90, k90 * 1024.0 / (1024 * 1024));
                        // Same analysis for the HalfKA rows (2048 bytes each, 46 MB):
                        // the biggest block of the transformer, never looked at.
                        {
                            static unsigned long long q[PROF_PSQ_N];
                            unsigned long long qt = 0;
                            int qu = 0;
                            for (int i = 0; i < PROF_PSQ_N; i++) {
                                q[i] = prof_psq_hist[i];
                                qt += q[i];
                                if (q[i]) qu++;
                            }
                            if (qt) {
                                std::sort(q, q + PROF_PSQ_N,
                                          [](unsigned long long a, unsigned long long b) { return a > b; });
                                unsigned long long a2 = 0;
                                int k2 = 0;
                                while (k2 < PROF_PSQ_N && a2 < qt * 9 / 10) a2 += q[k2++];
                                printf("--- accessi alle righe HalfKA (2048 B l'una, 46 MB) ---\n");
                                printf("  righe %d, TOCCATE %d (%.1f%%), accessi %llu\n",
                                       PROF_PSQ_N, qu, 100.0 * qu / PROF_PSQ_N, qt);
                                printf("  il 90%% degli accessi sta in %d righe = %.1f MB se contigue\n",
                                       k2, k2 * 2048.0 / (1024 * 1024));
                            }
                        }
                        // Is the heat concentrated in FEW CONTIGUOUS ZONES or spread out?
                        // If it is in zones, the permutation is done by reassigning the constant
                        // tables of make_index => ZERO runtime cost, no LUT.
                        // Trial granularity: blocks of 256 rows (256 KB, 64 pages).
                        {
                            const int BLK = 256, NB = PROF_FEAT_N / BLK + 1;
                            static unsigned long long b[512];
                            for (int i = 0; i < NB && i < 512; i++) b[i] = 0;
                            for (int i = 0; i < PROF_FEAT_N; i++)
                                if (i / BLK < 512) b[i / BLK] += prof_feat_hist[i];
                            int nb = NB < 512 ? NB : 512;
                            std::sort(b, b + nb,
                                      [](unsigned long long x, unsigned long long y) { return x > y; });
                            unsigned long long c = 0;
                            int kb = 0;
                            while (kb < nb && c < tot * 9 / 10) c += b[kb++];
                            printf("  a blocchi da %d righe: il 90%% degli accessi sta in %d/%d blocchi\n",
                                   BLK, kb, nb);
                            printf("    => %s\n", kb <= nb / 8
                                   ? "CONCENTRATO a zone: permutazione a BLOCCHI, costo runtime ZERO"
                                   : "SPARSO dentro i blocchi: serve la permutazione per riga (LUT)");
                        }
                        printf("  (oggi quelle righe sono sparse su %.1f MB = %d pagine da 4 KB)\n",
                               PROF_FEAT_N * 1024.0 / (1024 * 1024), PROF_FEAT_N / 4);
                    }
                }
                printf("  dirty in ritardo : %5.1f%% del wall  (minacce e pedoni prima della valutazione)\n",
                       100.0 * (double)prof_catchup / (double)pw);
                printf("  bridge/cache/scal: %5.1f%% del wall  (eval - forward - catch-up)\n",
                       100.0 * (double)(prof_eval - nn - prof_catchup) / (double)pw);
                // Inside the FT: where the time really goes.
                unsigned long long ftsum = prof_acc_inc + prof_acc_refresh + prof_ft_out;
                if (ftsum) {
                  printf("--- dentro il feature transformer ---\n");
                  printf("  acc. incrementale: %5.1f%% del wall  (%llu chiamate)\n",
                         100.0 * (double)prof_acc_inc / (double)pw, (unsigned long long)prof_n_inc);
                  printf("  acc. refresh     : %5.1f%% del wall  (%llu chiamate, %4.1f%% del totale)\n",
                         100.0 * (double)prof_acc_refresh / (double)pw, (unsigned long long)prof_n_refresh,
                         (prof_n_inc + prof_n_refresh) ? 100.0 * (double)prof_n_refresh / (double)(prof_n_inc + prof_n_refresh) : 0.0);
                  printf("  transform finale : %5.1f%% del wall\n",
                         100.0 * (double)prof_ft_out / (double)pw);
                  printf("  eval NNUE totali : %llu\n", (unsigned long long)prof_n_eval);
                  if (prof_n_inc)
                    printf("  cicli/update inc.: %llu\n", (unsigned long long)(prof_acc_inc / prof_n_inc));
                  if (prof_n_refresh)
                    printf("  cicli/refresh    : %llu\n", (unsigned long long)(prof_acc_refresh / prof_n_refresh));
                  if (prof_n_upd) {
                    printf("  colonne/update per blocco (path singolo): psq %.2f  threat %.2f  pedoni %.2f\n",
                           (double)prof_cols_psq_inc / (double)prof_n_upd, (double)prof_cols_thr_inc / (double)prof_n_upd,
                           (double)prof_cols_pawn_inc / (double)prof_n_upd);
                    printf("  update effettivi : %llu  (colonne/update: %.1f)\n",
                           (unsigned long long)prof_n_upd, (double)prof_n_cols / (double)prof_n_upd);
                    printf("  costo teorico    : %.0f cicli/update a 1 col = %d int16 = %d vettori AVX512\n",
                           (double)prof_n_cols / (double)prof_n_upd * (1024.0 / 32.0),
                           1024, 1024 / 32);
                  }
                  if (prof_n_thr_seen)
                    printf("  tuple threat     : %llu generate, %llu BUTTATE (%.1f%%)\n",
                           (unsigned long long)prof_n_thr_seen, (unsigned long long)prof_n_thr_dead,
                           100.0 * (double)prof_n_thr_dead / (double)prof_n_thr_seen);
                  { static const char* PT = "?PNBRQK";
                    for (int a = 1; a <= 6; a++) for (int b = 1; b <= 6; b++)
                      if (prof_dead_pair[a][b])
                        printf("      %c->%c : %llu\n", PT[a], PT[b], (unsigned long long)prof_dead_pair[a][b]); }
                  printf("  MAX riempimento  : refresh %llu/288   incrementale %llu/288  %s\n",
                         (unsigned long long)prof_max_active, (unsigned long long)prof_max_inc,
                         (prof_max_active >= 260 || prof_max_inc >= 260) ? "<<< VICINO AL BOUND" : "");
                  if (prof_n_eval) {
                      // 🔴 prof_n_inc counts ONCE PER PERSPECTIVE: evaluate() calls
                      // evaluate_side(WHITE) and evaluate_side(BLACK), each adds 1
                      // (the TRIUMV_PERSP_TOGETHER branch, removed on 25/09, did +=2). So the
                      // SANE maximum is 2.00 per eval = one per perspective, not 1.00.
                      // The alarm threshold was at 1.2 and lit up on any sane
                      // engine: on 4/08/2026 it printed "update WASTED" with 1.83, which instead
                      // means that 91.5% of the perspective-eval slots went the
                      // incremental way and the rest the refresh. Second false alarm of
                      // this same counter (the first: prof_n_eval incremented twice).
                      printf("  UPDATE vs EVAL   : %llu update incrementali, %llu valutazioni  "
                             "=> %.2f update per eval (max sano 2,00 = uno per prospettiva)  %s\n",
                             (unsigned long long) prof_n_inc, (unsigned long long) prof_n_eval,
                             (double) prof_n_inc / (double) prof_n_eval,
                             prof_n_inc > prof_n_eval * 24 / 10
                               ? "<<< SOPRA 2 PER PROSPETTIVA: update davvero sprecati"
                               : "");
                  }
                  if (prof_n_refresh_calls) {
                      const double tot = (double)(prof_cols_thr + prof_cols_pawn);
                      printf("  COLONNE @refresh : threat %.1f/chiamata   pedoni %.1f/chiamata   "
                             "pedoni = %.1f%% del totale  (tetto della cache di refresh)\n",
                             (double)prof_cols_thr / (double)prof_n_refresh_calls,
                             (double)prof_cols_pawn / (double)prof_n_refresh_calls,
                             tot > 0 ? 100.0 * (double)prof_cols_pawn / tot : 0.0);
                  }
                }
              } }
#endif
            fflush(stdout);
            parse_fen(start_position);
        }

        // (Commands of the graft blocks exportprel, exportpst, exportgraft and pstidx: removed on 10/10/2026 with the
        // blocks; code in _backup/Triumviratus_8.0_pre_rimozione_graft_2026-10-10.)

        // DIAGNOSTIC: "eval" -> static NNUE eval of the current position (cp,
        // side-to-move relative), no search => byte-identical for cross-checks.
        // ORACOLO B6: "evalbatch <in.fen> <out.txt>": per riga FEN -> "psqt positional final".
        // DIAGNOSTIC (28/10/2026, piece values per expert, match_moe/piece_values): "expertbatch <in.fen> <out.txt>":
        // per FEN line the Consilium Plebis expert of the White and of the Black perspective (router table over the
        // non-pawn material signatures, as c2_pair_now in see.cpp). No search, no change to the engine's state beyond
        // the board.
        else if (strncmp(input, "expertbatch", 11) == 0)
        {
            char fin[1024], fout[1024];
            if (sscanf(input + 11, "%1023s %1023s", fin, fout) == 2) {
                FILE* fi = fopen(fin, "r");
                FILE* fo = fopen(fout, "w");
                char line[512];
                long n = 0;
                const unsigned char* rt = nn_router_table();
                while (fi && fo && fgets(line, sizeof line, fi)) {
                    size_t L = strlen(line);
                    while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
                    if (!L) continue;
                    parse_fen(line);
                    int sig[2];
                    for (int c = 0; c < 2; c++)
                        sig[c] = cn_sig(count_bits(bitboards[N + 6 * c]), count_bits(bitboards[B + 6 * c]),
                                        count_bits(bitboards[R + 6 * c]), count_bits(bitboards[Q + 6 * c]));
                    fprintf(fo, "%d %d\n", rt[sig[0] * 54 + sig[1]], rt[sig[1] * 54 + sig[0]]);
                    n++;
                }
                if (fi) fclose(fi);
                if (fo) fclose(fo);
                printf("expertbatch %ld\n", n);
                fflush(stdout);
            }
        }
        else if (strncmp(input, "evalbatch", 9) == 0)
        {
            extern int nn_eval_parts(int, const unsigned long long*, const unsigned long long*, int, int*, int*);
            char fin[1024], fout[1024];
            if (sscanf(input + 9, "%1023s %1023s", fin, fout) == 2) {
                FILE* fi = fopen(fin, "r");
                FILE* fo = fopen(fout, "w");
                char line[512];
                long n = 0;
                while (fi && fo && fgets(line, sizeof line, fi)) {
                    size_t L = strlen(line);
                    while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
                    if (!L) continue;
                    parse_fen(line);
                    int ps = 0, po = 0;
                    int fv = nn_eval_parts(side == white, bitboards, occupancies, fifty, &ps, &po);
                    fprintf(fo, "%d %d %d\n", ps, po, fv);
                    n++;
                }
                if (fi) fclose(fi);
                if (fo) fclose(fo);
                printf("evalbatch %ld\n", n);
                fflush(stdout);
            }
        }
        else if (strncmp(input, "eval", 4) == 0)
        {
            const int e = debug_eval_position();
            printf("eval %d\n", e);
            // Line required by the standard tools (nnue-pytorch
            // cross_check_eval.py looks for exactly this form). "internal units" is
            // literal: this value does NOT go through the NORM_CP=392 normalisation
            // applied to the `score cp` of the search info (print_search_info, search/13_iterdeep.inc), so it is
            // directly comparable with the trainer's output. (27/07/2026)
            // 🔴 RAW, not `e`: `e` goes through nn_scale (psqt/positional blend, complexity,
            // material, rule50), which the trainer does NOT have. Comparing `e` with the trainer compares
            // two different quantities - measured R^2 0.73 even on a known-good pair.
            printf("NNUE evaluation  %d (side to move, internal units)\n",
                   debug_eval_position_raw());
            fflush(stdout);
        }

       // UCI command: "ucinewgame"
        else if (strncmp(input, "ucinewgame", 10) == 0)
        {
            // Makes sure no thread is searching, then resets
            stop_search_threads();
            wait_for_search_done();
            parse_fen(start_position);
            clear_hash_table();
            search_clear();   // statistics, corrections and time-management memory: new game
            { extern std::atomic<int> g_optimism[2]; g_optimism[0] = g_optimism[1] = 0; }  // BUG FIX 2026-07-16: dynamic contempt must not persist between games
        }

        // UCI command: "position"
        else if (strncmp(input, "position", 8) == 0)
        {
            // The board is global state shared with the search threads, so a
            // running search must be stopped and joined before we change it.
            stop_search_threads();
            wait_for_search_done();
            parse_position(input);
            // NOTE: do NOT clear the TT here. The table is keyed by Zobrist
            // hash and ages itself every search (new_search()), so keeping it
            // across moves lets the engine reuse work between moves. It is only
            // fully cleared on "ucinewgame".
        }

        // UCI command: "go"
        else if (strncmp(input, "go", 2) == 0)
        {
            parse_go(input);
        }

        // UCI command: "stop"
        else if (strncmp(input, "stop", 4) == 0)
        {
            stop_search_threads();
            stopped = 1;
        }

        // UCI command: "ponderhit" (01/10/2026): the expected move was played, the ponder becomes a normal search.
        else if (strcmp(input, "ponderhit") == 0)
        {
            ponder_hit();
        }

        // UCI command: "quit"
        else if (strncmp(input, "quit", 4) == 0)
        {
            stop_search_threads();
            wait_for_search_done();   // joins the master search (which joins helpers)
#ifdef CLANG_PGO_GEN
            __llvm_profile_write_file();   // writes the profile (LLVM's atexit does not fire on our exit)
#endif
            break;
        }

        // UCI button: "setoption name Clear Hash" (no value) -> clears the TT.
        // Stops the threads first (like Hash/EvalFile): clearing under a search would
        // corrupt it. Prefix distinct from "...name Hash value" -> no collision.
        else if (strncmp(input, "setoption name Clear Hash", 25) == 0)
        {
            stop_search_threads();
            wait_for_search_done();
            clear_hash_table();
        }

        // UCI command: "setoption name Hash value X"
        else if (strncmp(input, "setoption name Hash value ", 26) == 0)
        {
            // Reallocating the TT under a running search would crash it.
            stop_search_threads();
            wait_for_search_done();
            int newMb = atoi(input + 26);
            if (newMb < 1) newMb = 1;
            if (newMb > max_hash) newMb = max_hash;
            // 08/10/2026: same size = no reallocation. fastchess resends Hash after EVERY ucinewgame: the
            // TT was freed and reallocated at every game, and on a node with fragmented memory the large pages
            // could be missing (fallback to normal pages at random, engine by engine). The TT clearing stays at
            // ucinewgame.
            if (newMb != mb || !hash_table) {
                mb = newMb;
                init_hash_table(mb);
            }
        }

        // UCI command: "setoption name LargePages value 0|1" - reallocates the TT
        // at once with the chosen allocator (serves the NPS A/B on the same binary).
        else if (strncmp(input, "setoption name LargePages value ", 32) == 0)
        {
            stop_search_threads();
            wait_for_search_done();
            g_large_pages = atoi(input + 32) != 0;
            init_hash_table(mb);
        }

        // PassedScale (26/10/2026): percent on the PassedPawns rows of the network, applied when it is read
        // (consilium/cn.cpp). A change reloads the current network and clears the TT, as EvalFile does (the TT
        // keeps static evaluations of the old weights). 100 = the file as it is.
        else if (strncmp(input, "setoption name PassedScale value ", 33) == 0)
        {
            stop_search_threads();
            wait_for_search_done();
            if (nn_set_passed_scale(atoi(input + 33)))
            {
                clear_hash_table();
                printf("info string PassedScale: network reloaded at %d%%\n", atoi(input + 33));
            }
            fflush(stdout);
        }

        // UCI command: "setoption name EvalFile value <path>" -> reload the TRANN1
        // network at runtime. Default at startup = CN_DEFAULT_NET_FILE (own-lineage).
        else if (strncmp(input, "setoption name EvalFile value ", 30) == 0)
        {
            // Swapping the net under a running search would read half-loaded
            // weights; stop first (mirrors the Hash/Threads handlers). The next
            // search root full-refreshes from the new net, so it's clean.
            stop_search_threads();
            wait_for_search_done();
            char val[1024];
            strncpy(val, input + 30, sizeof(val) - 1);
            val[sizeof(val) - 1] = '\0';
            char* e = val + strlen(val);              // trim stray CR/space/newline
            while (e > val && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\n')) *--e = '\0';
            std::string resolved = resolve_net_path(val);
            // 08/10/2026: the same network already loaded is not reloaded. fastchess resends EvalFile after EVERY
            // ucinewgame: the side with the external network reloaded it at every game (~0.6 s, 245 MB of large pages
            // allocated and freed, shared area detached and reattached), the other side never. In the SPRT ft2avg of
            // 08/10 (6+0.06) that side on socket 1 searched 0.22 ply less than the other (on socket 0 0.02) and
            // socket 1 gave -20.8 Elo against -0.1 for socket 0 with the same network. A file changed under the
            // same path needs a different path or a restart.
            static std::string s_loaded_net;
            if (!resolved.empty() && resolved == s_loaded_net)
                printf("info string EvalFile: %s already loaded\n", resolved.c_str());
            else if (resolved.empty())
                printf("info string EvalFile: '%s' not found (kept current net)\n", val);
            else if (nn_reload_big(resolved.c_str()))
            {
                s_loaded_net = resolved;
                // Net swapped: EVERY cached eval derived from the old net is stale
                // (same family as the finny g_net_gen bug, 2026-07-14). The TT
                // carries old-net static evals (ext eval16, consumed by
                // g_tt_static_eval) plus scores searched under the old eval ->
                // clear it. Rare, search-stopped
                // operation; gates load the net once at startup (TT empty) so match
                // runs pay nothing. NOT cleared: corr_hist (it corrects
                // itself). NB: "setoption EvalScale" mid-session has the same
                // staleness family (diagnostic-only option, documented here).
                clear_hash_table();
                printf("info string EvalFile: loaded %s\n", resolved.c_str());
            }
            else
                printf("info string EvalFile: failed to load %s (kept current net)\n", resolved.c_str());
            fflush(stdout);
        }

        // UCI command: "setoption name EvalScale value N" -> % scale of the final eval
        // (re-calibrate the TRANN1 cp scale to the search margins). Diagnostic sweep.
        else if (strncmp(input, "setoption name EvalScale value ", 31) == 0)
        {
            nn_set_eval_scale(atoi(input + 31));
            fflush(stdout);
        }

        // EvalScaleB0..B7 (15/08/2026): the same recalibration, but PER output BUCKET
        // of the network (bucket = (pieces - 1) / 4, as network.cpp:170). Default all 60 =
        // byte-identical to the old single scalar.
        // ⚠️ It goes AFTER the EvalScale branch: "EvalScale" is a prefix of "EvalScaleB0", so
        //    with the order reversed the strncmp at 31 characters would not tell them apart... in
        //    fact they do not collide (the 31st character is 'v' against 'B'), but the explicit
        //    order removes the doubt for the reader.
        else if (strncmp(input, "setoption name EvalScaleB", 25) == 0
                 && input[25] >= '0' && input[25] <= '7'
                 && strncmp(input + 26, " value ", 7) == 0)
        {
            nn_set_eval_scale_bucket(input[25] - '0', atoi(input + 33));
            fflush(stdout);
        }

        // Blend constants (EvalPsqtW, EvalPosW, EvalComplexDiv, ...): a single branch,
        // the names are in the table of consilium_bridge.cpp. It goes BEFORE the generic handler,
        // otherwise they would be swallowed by it without effect.
        else if (strncmp(input, "setoption name Eval", 19) == 0 && strstr(input, " value ")
                 && [&] {
                        char nm[64] = {0};
                        const char* p = input + 15;                   // "setoption name " = 15 caratteri
                        const char* v = strstr(input, " value ");
                        size_t len = size_t(v - p);
                        if (len == 0 || len >= sizeof(nm)) return false;
                        memcpy(nm, p, len);
                        return nn_set_eval_const(nm, atoi(v + 7)) != 0;
                    }())
        {
            fflush(stdout);
        }

        // UCI command: "setoption name Threads value X"
        else if (strncmp(input, "setoption name Threads value ", 29) == 0)
        {
            // Resizing thread_data under a running search would invalidate the
            // ThreadData& references held by the helper threads.
            stop_search_threads();
            wait_for_search_done();
            int threads = atoi(input + 29);
            if (threads < 1) threads = 1;
            if (threads > max_threads) threads = max_threads;

            init_threads(threads);
        }

        // UCI command: "setoption name NumaSpread value true|false" (03/11/2026, search/03_tables.inc): the threads
        // spread over the NUMA nodes of a multi-socket machine. The tables are rebuilt for the new split, as by Threads.
        else if (strncmp(input, "setoption name NumaSpread value ", 32) == 0)
        {
            stop_search_threads();
            wait_for_search_done();
            const bool on = strncmp(input + 32, "true", 4) == 0;
            if (on != g_numa_spread)
            {
                g_numa_spread = on;
                init_threads(num_threads);
            }
        }

        // UCI command: "setoption name Depth value X" (0 = off; >0 = fixed depth)
        else if (strncmp(input, "setoption name Depth value ", 27) == 0)
        {
            int d = atoi(input + 27);
            if (d < 0) d = 0;
            if (d > 64) d = 64;
            g_uci_depth = d;
        }

        // UCI command: "setoption name DataLog value <true|false>" (self-play)
        else if (strncmp(input, "setoption name DataLog value ", 29) == 0)
        {
            const char* v = input + 29;
            bool on = (strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
            set_data_log_enabled(on);
        }

        // UCI command: "setoption name DataFile value <path>"
        else if (strncmp(input, "setoption name DataFile value ", 30) == 0)
        {
            char path[512];
            strncpy(path, input + 30, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            size_t n = strlen(path);
            while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r' || path[n - 1] == ' '))
                path[--n] = '\0';
            set_data_log_file(path);
        }

        // Chess960 (27/09/2026, chess960.h): plays 960 castling, verified with the official perft suite
        // (tools/perft960.py, global perft and tdperft). In release since the evening of 27/09: NPS measurement PGO release
        // pre/post 960 = +0.57% [+0.53, +0.62] (no cost), null test +0.00%.
        else if (strncmp(input, "setoption name UCI_Chess960 value ", 34) == 0)
        {
            const char* v = input + 34;
            g_chess960 = strncmp(v, "true", 4) == 0 || v[0] == '1';
        }
        // "Move Overhead" (ms reserved per move for lag/GUI; formerly hard-coded 50)
        else if (strncmp(input, "setoption name Move Overhead value ", 35) == 0)
        {
            int v = atoi(input + 35);
            g_move_overhead_ms = v < 0 ? 0 : (v > 5000 ? 5000 : v);
        }
        // DIAGNOSTIC: "setoption name EvalOff value <true|false>" (NPS profiling)
        else if (strncmp(input, "setoption name EvalOff value ", 29) == 0)
        {
            const char* v = input + 29;
            set_search_param("EvalOff", strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name FinnyTables value <true|false>" (A/B accumulator refresh cache)
        else if (strncmp(input, "setoption name FinnyTables value ", 33) == 0)
        {
            const char* v = input + 33;
            nn_set_finny(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }


        // UCI: "setoption name SyzygyPath value <dir[;dir...]>" — read by words
        // (parse_setoption: value trimmed, blanks collapsed) so any GUI spacing
        // loads identically. Loading touches global tablebase state;
        // stop any running search first. Always logs the path received so the
        // exact string the GUI sent is visible in the engine output (diagnostic).
        else if (parse_setoption(input, "SyzygyPath", szpath, sizeof(szpath)))
        {
            stop_search_threads();
            wait_for_search_done();
            if (syzygy_init(szpath))
                printf("info string Syzygy: tablebases loaded (max %u-men) from \"%s\"\n",
                       syzygy_max_pieces(), szpath);
            else
                printf("info string Syzygy: probing disabled (path=\"%s\")\n", szpath);
            fflush(stdout);
        }

        // SPSA-tunable spin options: generic "setoption name <Param> value <N>".
        // Placed AFTER all specific setoption handlers, so it only catches the
        // search-parameter spins; set_search_param ignores unknown names.
        else if (strncmp(input, "setoption name ", 15) == 0)
        {
            const char* p = input + 15;
            const char* vp = strstr(p, " value ");
            if (vp)
            {
                char nm[64];
                size_t nlen = (size_t)(vp - p);
                if (nlen < sizeof(nm))
                {
                    memcpy(nm, p, nlen);
                    nm[nlen] = '\0';
                    // FIX F-001 (2026-07-02): GUIs send checks as "true"/"false"
                    // (atoi -> 0 for BOTH: a default-ON was silently turned off).
                    // Explicit parse before the atoi (identical to the fix already validated on 6.0).
                    const char* vs = vp + 7;
                    int v;
                    if      (strncmp(vs, "true",  4) == 0 || strncmp(vs, "on",  2) == 0) v = 1;
                    else if (strncmp(vs, "false", 5) == 0 || strncmp(vs, "off", 3) == 0) v = 0;
                    else v = atoi(vs);
                    set_search_param(nm, v);
                }
            }
        }

        // DIAGNOSTIC: "perft N" - movegen + make/unmake speed on the current
        // position (no eval, no NNUE). Prints Nodes + Time(ms).
#ifndef TRIUMV_FROZEN
        // DIAGNOSTIC: "tdperft N" - perft on the per-thread board with a check of keys, mailbox,
        // occupancies and pseudo-legality at every node (search/15_tdperft.inc). Development builds only.
        else if (strncmp(input, "tdperft ", 8) == 0)
        {
            extern void td_perft_driver(int depth);
            td_perft_driver(atoi(input + 8));
        }
        // DIAGNOSTIC: "nnperft N" - perft that evaluates with the network and compares the incremental chain with a full
        // refresh (search/16_tdperft.inc). Development builds only.
        // DIAGNOSTIC: "nnspeed N <fen file>" - the same tree without the checks, from every FEN of the file: time per
        // evaluation on work that does not depend on the network (21/10/2026, cost of Consilium Plebis).
        else if (strncmp(input, "nnspeed ", 8) == 0)
        {
            extern void td_nnspeed_driver(int depth, const char *path);
            int d = 0;
            char path[1024] = {0};
            if (sscanf(input + 8, "%d %1023s", &d, path) == 2) td_nnspeed_driver(d, path);
        }
        else if (strncmp(input, "nnperft ", 8) == 0)
        {
            extern void td_nnperft_driver(int depth);
            td_nnperft_driver(atoi(input + 8));
        }
#endif
        else if (strncmp(input, "perft", 5) == 0)
        {
            int d = atoi(input + 5);
            if (d < 1) d = 1;
            nodes = 0;
            perft_test(d);
            fflush(stdout);
        }

        // Debug command: "d" - print board (non-UCI, but useful)
        else if (strncmp(input, "d", 1) == 0 && strlen(input) == 1)
        {
            print_board();
        }
    }
}

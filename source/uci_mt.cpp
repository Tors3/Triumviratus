/*
 * UCI Protocol Implementation for Triumviratus Chess Engine
 * Fully compliant with UCI specification
 * https://www.shredderchess.com/chess-features/uci-universal-chess-interface.html
 */

#include "defs.h"
#include "uci.h"
#include "movegen.h"
#include "search.h"
#include "tt.h"
#include "misc.h"
#include "io.h"
#include "threads.h"
#include "syzygy.h"
#include "perft.h"
#include <thread>
#include <string.h>

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

        if (source_square == get_move_source(move) && target_square == get_move_target(move))
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

            repetition_index++;
            repetition_table[repetition_index] = hash_key;

            make_move(move, all_moves);

            while (*current_char && *current_char != ' ') current_char++;
            current_char++;
        }
    }
}

// reset time control variables
// UCI option "Depth": 0 = off (use time / explicit "go depth"); >0 = force a
// fixed search depth and ignore the clock (handy for testing / fixed strength).
// An explicit "go depth N" in the command still overrides this.
int g_uci_depth = 0;

void reset_time_control()
{
    quit = 0;
    movestogo = 30;
    movetime = -1;
    time_uci = -1;
    inc = 0;
    starttime = 0;
    stoptime = 0;
    timeset = 0;
    stopped = 0;
}

// parse UCI command "go"
void parse_go(char* command)
{
    reset_time_control();

    int depth = -1;
    char* argument = NULL;

    if ((argument = strstr(command, "infinite"))) {}

    if ((argument = strstr(command, "binc")) && side == black)
        inc = atoi(argument + 5);

    if ((argument = strstr(command, "winc")) && side == white)
        inc = atoi(argument + 5);

    if ((argument = strstr(command, "wtime")) && side == white)
        time_uci = atoi(argument + 6);

    if ((argument = strstr(command, "btime")) && side == black)
        time_uci = atoi(argument + 6);

    if ((argument = strstr(command, "movestogo")))
        movestogo = atoi(argument + 10);

    if ((argument = strstr(command, "movetime")))
        movetime = atoi(argument + 9);

    if ((argument = strstr(command, "depth")))
        depth = atoi(argument + 6);

    // UCI option "Depth" > 0 forces a fixed-depth search (clock ignored), unless
    // the GUI sent an explicit "go depth N" (which takes precedence).
    if (depth == -1 && g_uci_depth > 0)
    {
        depth = g_uci_depth;
        movetime = -1;
        time_uci = -1;
    }

    if (movetime != -1)
    {
        time_uci = movetime;
        movestogo = 1;
    }

    starttime = get_time_ms();

    if (time_uci != -1)
    {
        timeset = 1;
        const int overhead = 50;               // ms reserved for lag / output
        int remaining = time_uci - overhead;
        if (remaining < 0) remaining = 0;

        int optimum, maximum;
        if (movetime != -1)
        {
            // Fixed move time: use (almost) all of it.
            optimum = maximum = remaining;
        }
        else
        {
            int mtg = (movestogo > 0) ? movestogo : 30;   // assume ~30 moves if unknown
            optimum = remaining / mtg + inc * 3 / 4;       // base share + most of the increment
            maximum = optimum * 4;                          // allow bursts on hard positions
            int cap = remaining * 4 / 5;                    // never risk more than ~80% of the clock
            if (maximum > cap) maximum = cap;
            if (maximum < optimum) maximum = optimum;
        }
        if (optimum < 1) optimum = 1;
        if (maximum < 1) maximum = 1;

        soft_time_limit = starttime + optimum;   // stop starting new iterations past this
        stoptime        = starttime + maximum;    // hard cap checked inside the search
    }

    if (depth == -1)
        depth = 64;

    // Start the search on a background thread so the UCI loop stays free to
    // handle "stop" / "isready" / "quit" while we are thinking.
    launch_search(depth);
}

// main UCI loop - fully compliant with UCI protocol
void uci_loop()
{
    // Input buffer
    static char input[10000];
    
    // Engine settings
    int max_hash = 1024;
    int mb = 64;
    
    // Detect available threads
    int max_threads = std::thread::hardware_concurrency();
    if (max_threads < 1) max_threads = 1;
    if (max_threads > MAX_THREADS) max_threads = MAX_THREADS;
    
    // Initialize with 1 thread. NOTE: init_threads() also builds the LMR
    // reduction table (init_lmr_table). Without this call the table stays
    // all-zero and Late Move Reductions are effectively disabled.
    init_threads(1);

    // Disable I/O buffering for UCI compliance
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

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

        // Remove newline
        size_t len = strlen(input);
        if (len > 0 && input[len-1] == '\n')
            input[len-1] = '\0';

        // UCI command: "uci"
        if (strncmp(input, "uci", 3) == 0)
        {
            printf("id name %s %s\n", NAME, VERSION);
            printf("id author %s\n", AUTHOR);
            printf("option name Hash type spin default 64 min 1 max %d\n", max_hash);
            printf("option name Threads type spin default 1 min 1 max %d\n", max_threads);
            printf("option name Depth type spin default 0 min 0 max 64\n");
            printf("option name DataLog type check default false\n");
            printf("option name DataFile type string default triumviratus_dataset.txt\n");
            printf("option name UsePolicy type check default false\n");
            printf("option name EvalOff type check default false\n");
            printf("option name EvalCache type check default true\n");
            printf("option name Improving type check default true\n");
            printf("option name NodeTM type check default true\n");
            printf("option name SingularExt type check default true\n");
            printf("option name CorrHist type check default true\n");
            printf("option name ProbCut type check default true\n");
            printf("option name ContHistPrune type check default true\n");
            printf("option name TT4Way type check default false\n");
            printf("option name CorrHistMulti type check default false\n");
            printf("option name ContHistMulti type check default false\n");
            printf("option name MovePicker type check default true\n");
            printf("option name DiverseSMP type check default true\n");   // BAKED ON (bake-on-trust): wider-only SMP diversity
            printf("option name DiverseSMPAmount type spin default 1 min 0 max 4\n");
            printf("option name TripleExt type check default false\n");
            printf("option name MultiCut type check default true\n");
            printf("option name LMREnrich type check default false\n");
            printf("option name DeeperShallower type check default false\n");
            printf("option name TTPv type check default false\n");
            printf("option name NMPEvalScale type check default false\n");
            printf("option name RFPDepth8 type check default false\n");
            printf("option name RazorDepth4 type check default false\n");
            printf("option name QFutility type check default false\n");
            printf("option name HistBonusSF type check default true\n");   // BAKED ON: +24 LOS99.99% @810
            printf("option name NMPEvalDiv type spin default 200 min 50 max 1000\n");
            printf("option name QFutMargin type spin default 150 min 0 max 500\n");
            printf("option name HistBonusMult type spin default 155 min 1 max 600\n");
            printf("option name HistBonusSub type spin default 90 min 0 max 400\n");
            printf("option name HistBonusMax type spin default 1600 min 200 max 4000\n");
            printf("option name LazyEval type check default true\n");
            printf("option name TimeMgmt type check default true\n");
            printf("option name AggrLMR type check default false\n");
            printf("option name AggrLMRDiv type spin default 2048 min 512 max 6000\n");
            printf("option name AggrLMRClamp type spin default 3 min 1 max 6\n");
            printf("option name NMPBase type spin default 3 min 1 max 6\n");
            printf("option name NMPDiv type spin default 4 min 2 max 8\n");
            printf("option name LMREvalMargin type spin default 100 min 0 max 400\n");
            printf("option name LMRTTDepth type spin default 2 min 0 max 3\n");
            printf("option name LMRBase type spin default 47 min 0 max 200\n");
            printf("option name LMRDiv type spin default 270 min 100 max 500\n");
            printf("option name RFPMargin type spin default 21 min 20 max 200\n");        // bakato: 30->21
            printf("option name RazorBase type spin default 300 min 100 max 600\n");
            printf("option name RazorMult type spin default 139 min 20 max 250\n");       // bakato: 102->139
            printf("option name FutilityBase type spin default 111 min 20 max 300\n");
            printf("option name FutilityMult type spin default 66 min 20 max 200\n");
            printf("option name FutilityImproving type spin default 93 min 0 max 200\n"); // bakato: 60->93
            printf("option name SingularDoubleMargin type spin default 43 min 0 max 200\n"); // bakato: 63->43
            printf("option name HistReductionDiv type spin default 1041 min 500 max 8000\n"); // bakato: 3500->1041
            printf("option name AspInitDelta type spin default 31 min 8 max 60\n");       // bakato: 25->31
            printf("option name AspGrow type spin default 31 min 30 max 200\n");          // bakato: 100->31
            printf("option name ProbCutMargin type spin default 180 min 60 max 400\n");
            printf("option name CorrCap type spin default 32 min 8 max 128\n");
            printf("option name CorrLearnDiv type spin default 512 min 64 max 2048\n");
            printf("option name ContHistDiv type spin default 6595 min 1000 max 12000\n"); // bakato: 5000->6595
            printf("option name HistPruneMargin type spin default 1000 min 200 max 4000\n");
            printf("option name SEECaptureMargin type spin default 90 min 20 max 300\n");
            printf("option name SEEQuietMargin type spin default 50 min 10 max 200\n");
            printf("option name DeeperMargin type spin default 52 min 0 max 30000\n");  // max alto: 30000 = doDeeper OFF (mai > mate)
            printf("option name ShallowerMargin type spin default 9 min -1000 max 200\n");  // -1000 = doShallower OFF
            printf("option name SmallNetThreshold type spin default 1050 min 300 max 2000\n"); // 782->1050 (+13 Elo, higher=more Elo)
            printf("option name SyzygyPath type string default <empty>\n");
            printf("uciok\n");
            fflush(stdout);
        }

        // UCI command: "isready"
        else if (strncmp(input, "isready", 7) == 0)
        {
            printf("readyok\n");
            fflush(stdout);
        }

       // UCI command: "ucinewgame"
        else if (strncmp(input, "ucinewgame", 10) == 0)
        {
            // Assicura che nessun thread stia cercando, poi resetta
            stop_search_threads();
            wait_for_search_done();
            parse_fen(start_position);
            clear_hash_table();
            
            // FIX DEFINITIVO: Cancella il passato tra partite diverse
            for (int i = 0; i < num_threads; i++) {
                memset(thread_data[i].history_moves, 0, sizeof(thread_data[i].history_moves));
                memset(thread_data[i].capture_history, 0, sizeof(thread_data[i].capture_history));
                memset(thread_data[i].counter_moves, 0, sizeof(thread_data[i].counter_moves));
                memset(thread_data[i].continuation_history, 0, sizeof(thread_data[i].continuation_history)); // <--- AGGIUNTO!
            }
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
            // across moves lets the engine reuse work between moves (as
            // Stockfish does). It is only fully cleared on "ucinewgame".
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

        // UCI command: "quit"
        else if (strncmp(input, "quit", 4) == 0)
        {
            stop_search_threads();
            wait_for_search_done();   // joins the master search (which joins helpers)
            break;
        }

        // UCI command: "setoption name Hash value X"
        else if (strncmp(input, "setoption name Hash value ", 26) == 0)
        {
            // Reallocating the TT under a running search would crash it.
            stop_search_threads();
            wait_for_search_done();
            mb = atoi(input + 26);
            if (mb < 1) mb = 1;
            if (mb > max_hash) mb = max_hash;
            init_hash_table(mb);
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

        // UCI command: "setoption name UsePolicy value <true|false>" (A/B test)
        else if (strncmp(input, "setoption name UsePolicy value ", 31) == 0)
        {
            const char* v = input + 31;
            set_use_policy(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // DIAGNOSTIC: "setoption name EvalOff value <true|false>" (NPS profiling)
        else if (strncmp(input, "setoption name EvalOff value ", 29) == 0)
        {
            const char* v = input + 29;
            set_eval_off(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // DIAGNOSTIC: "setoption name EvalCache value <true|false>" (A/B eval cache)
        else if (strncmp(input, "setoption name EvalCache value ", 31) == 0)
        {
            const char* v = input + 31;
            set_eval_cache(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name Improving value <true|false>" (A/B the improving heuristic)
        else if (strncmp(input, "setoption name Improving value ", 31) == 0)
        {
            const char* v = input + 31;
            set_improving(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name NodeTM value <true|false>" (A/B node-based time mgmt)
        else if (strncmp(input, "setoption name NodeTM value ", 28) == 0)
        {
            const char* v = input + 28;
            set_node_tm(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name SingularExt value <true|false>" (A/B double/negative ext)
        else if (strncmp(input, "setoption name SingularExt value ", 33) == 0)
        {
            const char* v = input + 33;
            set_singular_ext(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name CorrHist value <true|false>" (A/B correction history)
        else if (strncmp(input, "setoption name CorrHist value ", 30) == 0)
        {
            const char* v = input + 30;
            set_corr_hist(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name ProbCut value <true|false>" (A/B ProbCut pruning)
        else if (strncmp(input, "setoption name ProbCut value ", 29) == 0)
        {
            const char* v = input + 29;
            set_probcut(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name MovePicker value <true|false>" (A/B staged move picker)
        else if (strncmp(input, "setoption name MovePicker value ", 32) == 0)
        {
            const char* v = input + 32;
            set_move_picker(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name DiverseSMP value <true|false>" (A/B per-thread LMR-bias SMP).
        // NB: must precede the generic spin handler; "DiverseSMPAmount" is NOT caught
        // here (its 26th char is 'A', not the space before "value") -> falls through to
        // the generic spin handler -> set_search_param.
        else if (strncmp(input, "setoption name DiverseSMP value ", 32) == 0)
        {
            const char* v = input + 32;
            set_diverse_smp(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // Search bundle #3 toggles (A/B; default off = behaviour-preserving).
        else if (strncmp(input, "setoption name TripleExt value ", 31) == 0)
        {
            const char* v = input + 31;
            set_triple_ext(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name MultiCut value ", 30) == 0)
        {
            const char* v = input + 30;
            set_multicut(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name LMREnrich value ", 31) == 0)
        {
            const char* v = input + 31;
            set_lmr_enrich(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name DeeperShallower value ", 37) == 0)
        {
            const char* v = input + 37;
            set_deeper_shallower(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name TTPv value ", 26) == 0)
        {
            const char* v = input + 26;
            set_ttpv(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name NMPEvalScale value ", 34) == 0)
        {
            const char* v = input + 34;
            set_nmp_eval_scale(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name RFPDepth8 value ", 31) == 0)
        {
            const char* v = input + 31;
            set_rfp_depth8(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name RazorDepth4 value ", 33) == 0)
        {
            const char* v = input + 33;
            set_razor_depth4(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name QFutility value ", 31) == 0)
        {
            const char* v = input + 31;
            set_qfutility(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name HistBonusSF value ", 33) == 0)
        {
            const char* v = input + 33;
            set_hist_bonus_sf(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name ContHistPrune value <true|false>" (A/B continuation-history
        // pruning + reduction). NB: must precede the generic spin handler; "ContHist"
        // would otherwise be parsed as an (unknown) spin name.
        else if (strncmp(input, "setoption name ContHistPrune value ", 35) == 0)
        {
            const char* v = input + 35;
            set_cont_hist_prune(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name TT4Way value <true|false>" (A/B 4-way bucketed TT)
        else if (strncmp(input, "setoption name TT4Way value ", 28) == 0)
        {
            const char* v = input + 28;
            set_tt_4way(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name CorrHistMulti value <true|false>" (A/B multi-table corrhist)
        else if (strncmp(input, "setoption name CorrHistMulti value ", 35) == 0)
        {
            const char* v = input + 35;
            set_corr_multi(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name ContHistMulti value <true|false>" (A/B 2/4-ply cont history)
        else if (strncmp(input, "setoption name ContHistMulti value ", 35) == 0)
        {
            const char* v = input + 35;
            set_conthist_multi(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name LazyEval value <true|false>" (A/B skip-eval-in-check)
        else if (strncmp(input, "setoption name LazyEval value ", 30) == 0)
        {
            const char* v = input + 30;
            set_lazy_eval(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name TimeMgmt value <true|false>" (A/B score-drop time extension)
        else if (strncmp(input, "setoption name TimeMgmt value ", 30) == 0)
        {
            const char* v = input + 30;
            set_time_mgmt(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name AggrLMR value <true|false>" (A/B aggressive multi-ply LMR)
        else if (strncmp(input, "setoption name AggrLMR value ", 29) == 0)
        {
            const char* v = input + 29;
            set_aggr_lmr(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // UCI command: "setoption name SyzygyPath value <dir[;dir...]>"
        // Loading touches global tablebase state; stop any running search first.
        else if (strncmp(input, "setoption name SyzygyPath value ", 32) == 0)
        {
            stop_search_threads();
            wait_for_search_done();
            char path[1024];
            strncpy(path, input + 32, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            size_t n = strlen(path);
            while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r' || path[n - 1] == ' '))
                path[--n] = '\0';
            if (syzygy_init(path))
                printf("info string Syzygy: tablebases loaded (max %u-men) from %s\n",
                       syzygy_max_pieces(), path);
            else
                printf("info string Syzygy: probing disabled (no tablebases found)\n");
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
                    set_search_param(nm, atoi(vp + 7));
                }
            }
        }

        // DIAGNOSTIC: "perft N" - movegen + make/unmake speed on the current
        // position (no eval, no NNUE mirror). Prints Nodes + Time(ms).
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

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
#include "nnue_bridge.h"   // nn_acc_stats (diagnostic "accstats" command)
#include <algorithm>       // std::sort (istogramma accessi, solo TRIUMV_PROFILE)
#include <thread>

#ifdef CLANG_PGO_GEN
// clang-PGO instrument build (-fprofile-generate): il path di exit del motore non fa
// scattare l'atexit di LLVM -> il quit-handler chiama questa per scrivere il profilo.
// extern "C" deve stare a file scope (NON dentro un blocco). Assente nei build normali/optimize.
extern "C" int __llvm_profile_write_file(void);
#endif
#include <string.h>
#include <string>
#ifndef _WIN32
#include <unistd.h>   // getpid() per il suffisso per-processo di TMLog (su Windows
                      // GetCurrentProcessId arriva da windows.h via defs.h)
#endif

// Defined in main.cpp: resolve an NNUE filename/path to an existing path
// (tries the path as given, then next to the exe, then cwd). Used by the UCI
// "EvalFile" handler to load a big net specified at runtime.
std::string resolve_net_path(const std::string& name);

// "Move Overhead" (UCI): ms riservati per mossa a lag di I/O e GUI. 10 come la gestione del tempo che usiamo.
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
        // UCI_Chess960: la GUI manda l'arrocco come "re cattura la propria torre" (e1h1).
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

            // F-016 (2026-07-03): guard overflow repetition_table[2048] (partite ultra-lunghe
            // in datagen/ultrabullet): scarta la meta' piu' VECCHIA della storia — inerte,
            // e' oltre qualsiasi finestra-fifty (<=100 semimosse) usata dal rep-check.
            if (repetition_index >= 2048 - 300) {
                memmove(repetition_table, repetition_table + 1024,
                        (size_t)(repetition_index + 1 - 1024) * sizeof(repetition_table[0]));
                repetition_index -= 1024;
            }
            repetition_index++;
            repetition_table[repetition_index] = hash_key;

            make_move(move, all_moves);

            while (*current_char && *current_char != ' ') current_char++;
            // AUDIT D (T6): piu' spazi fra le mosse sono ammessi dal protocollo; prima uno spazio doppio
            // fermava il parsing e le mosse successive venivano perse in silenzio.
            while (*current_char == ' ' || *current_char == '\t') current_char++;
        }
    }
}

// UCI option "Depth": 0 = off (use time / explicit "go depth"); >0 = force a
// fixed search depth and ignore the clock (handy for testing / fixed strength).
// An explicit "go depth N" in the command still overrides this.
int g_uci_depth = 0;

// Azzera i limiti della ricerca prima di ogni "go" (e del bench).
void reset_time_control()
{
    quit = 0;
    stopped = 0;
    starttime = 0;
    g_limits = SearchLimits{};
}

// Valore intero dopo una parola chiave del comando "go" (0 se la parola manca).
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
    // Orologio negativo (GUI con margine): vale come tempo minimo, non come "nessun orologio".
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

    // "go searchmoves m1 m2 ...": la radice cerca solo queste. La lista finisce al primo token che non e' una mossa.
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

    // Opzione "Depth" > 0: profondita' fissa e orologio ignorato, salvo "go depth N" esplicito.
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
// SF-faithful setoption parser (mirrors Stockfish ucioption.cpp:42-59).
// Tokenizes on whitespace: the option name may contain spaces (joined with a
// single space) and the VALUE is the remaining tokens re-joined with single
// spaces -> leading/trailing whitespace is trimmed and internal runs collapse,
// EXACTLY as Stockfish reads SyzygyPath. So a GUI (Fritz/ChessBase) that sends a
// slightly non-standard spacing is handled identically to SF. Returns true iff
// 'input' is "setoption name <want> value ..."; writes the normalized value
// (possibly empty) into out[0..outsz-1].
static bool parse_setoption(const char* input, const char* want,
                            char* out, size_t outsz)
{
    char buf[10000];
    strncpy(buf, input, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const char* SEP = " \t\r\n";
    char* tok = strtok(buf, SEP);
    if (!tok || strcmp(tok, "setoption") != 0) return false;
    tok = strtok(nullptr, SEP);
    if (!tok || strcmp(tok, "name") != 0) return false;

    // option name: tokens until "value" (name may contain spaces, e.g. SF style)
    char name[256]; name[0] = '\0';
    while ((tok = strtok(nullptr, SEP)) != nullptr && strcmp(tok, "value") != 0) {
        if (name[0]) strncat(name, " ", sizeof(name) - strlen(name) - 1);
        strncat(name, tok, sizeof(name) - strlen(name) - 1);
    }
    if (strcmp(name, want) != 0) return false;

    // value: remaining tokens joined by single spaces (trimmed + normalized)
    out[0] = '\0';
    while ((tok = strtok(nullptr, SEP)) != nullptr) {
        if (out[0]) strncat(out, " ", outsz - strlen(out) - 1);
        strncat(out, tok, outsz - strlen(out) - 1);
    }
    return true;
}

// main UCI loop - fully compliant with UCI protocol
void uci_loop()
{
    // Input buffer
    static char input[65536];   // AUDIT D: era 10000 (~1.990 semimosse); oltre, fgets spezzava la riga
    
    // Engine settings
    // 🔴 2026-09-07: era 1024 MB. Troppo poco per chi ci testa a TC lungo (CCRL
    // usa 256, CEGT di piu', TCEC gira a 64 GB). Tutta l'aritmetica della TT e'
    // gia' U64 (hash_entries, tt_base_index, l'indicizzazione), quindi il tetto
    // era solo una costante. A 65536 MB la tabella ha 2,86e9 entry: sopra il
    // range int32, ed e' il motivo per cui va alzato solo dopo aver verificato
    // che nessun indice sia un int (verificato: nessuno).
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

        // Remove newline (AUDIT D, T6: anche '\r' e spazi finali; "uci\r" da un GUI non riceveva uciok)
        size_t len = strlen(input);
        while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r' ||
                           input[len-1] == ' ' || input[len-1] == '\t'))
            input[--len] = '\0';

        char szpath[4096];   // scratch for SF-style SyzygyPath value parsing

        // UCI command: "uci" (EXACT match: strncmp len 3 would also swallow
        // "ucinewgame" -> its handler below would be dead code).
        if (strcmp(input, "uci") == 0)
        {
            // NB: VERSION contiene gia' il separatore (" - 7.0") -> %s%s, senza spazio
            // in mezzo. Con "%s %s" usciva "Triumviratus  - 7.0" (doppio spazio): l'id
            // name e' l'identita' del motore per GUI e liste rating, deve essere pulito.
            // La data di build in coda distingue le build giornaliere fra loro:
            // "Triumviratus - 7.0 2026-08-06".
            // Anche nel binario universale l'id name resta questo: e' il nome che GUI e liste (CCRL) registrano. La
            // variante scelta si legge nella riga d'avvio (main.cpp), solo in terminale.
            printf("id name %s%s %s\n", NAME, VERSION, build_date());
            printf("id author %s\n", AUTHOR);
            printf("option name Hash type spin default 64 min 1 max %d\n", max_hash);
            printf("option name Threads type spin default 1 min 1 max %d\n", max_threads);
            // MultiPV (analisi stile Stockfish): opzione UFFICIALE, sempre advertised
            // (anche in TRIUMV_RELEASE). Il setoption arriva via il generic handler
            // -> set_search_param("MultiPV") in threads.cpp.
            printf("option name MultiPV type spin default 1 min 1 max 64\n");
            // Opzioni ufficiali di analisi/utilizzo (sempre visibili, anche in release):
            printf("option name UCI_ShowWDL type check default false\n");  // W/D/L nelle info-line (via generic handler)
            printf("option name UCI_Chess960 type check default false\n"); // Fischer Random (chess960.h), dalla 8.0
            // Modalita' analisi (09/10/2026, search/01_params.inc): selettivita' per l'analisi, spenta in partita.
            // Il setoption passa dal gestore generico (set_search_param), anche nella release.
            print_analysis_options();
            printf("option name Clear Hash type button\n");                // svuota la TT su richiesta
            // Ponder: la GUI decide se pondera. Dal 04/10/2026 l'opzione accesa da' il 25% di tempo ottimale in piu'
            // (search/14_deepen.inc, clock_plan). Senza dichiararla molte GUI non mandano mai `go ponder`.
            printf("option name Ponder type check default false\n");
            printf("option name UCI_EngineAbout type string default %s%s build %s by %s\n", NAME, VERSION, build_date(), AUTHOR);
            printf("option name Move Overhead type spin default 10 min 0 max 5000\n"); // ms riservati a lag/GUI per mossa
            printf("option name EvalFile type string default %s\n", nn_default_net_name());
            // Gruppo Syzygy standard: le GUI ChessBase/Fritz riconoscono un motore con tablebase dal set completo.
            printf("option name SyzygyPath type string default <empty>\n");
            printf("option name SyzygyProbeDepth type spin default 1 min 1 max 100\n");
            printf("option name Syzygy50MoveRule type check default true\n");
            printf("option name SyzygyProbeLimit type spin default 7 min 0 max 7\n");
#ifndef TRIUMV_RELEASE
            // --- Opzioni di sviluppo e di taratura (nascoste nella release) ---
            printf("option name Depth type spin default 0 min 0 max 64\n");
            printf("option name DataLog type check default false\n");
            printf("option name DataFile type string default triumviratus_dataset.txt\n");
            printf("option name EvalOff type check default false\n");
            printf("option name FinnyTables type check default true\n");
            printf("option name LargePages type spin default 1 min 0 max 1\n");
            printf("option name EvalScale type spin default 60 min 10 max 2000\n");
            for (int b = 0; b < 8; ++b)
                printf("option name EvalScaleB%d type spin default %d min 20 max 150\n", b, nn_get_eval_scale_bucket(b));
            for (int i = 0, n = nn_eval_const_count(); i < n; ++i)
                printf("option name %s type spin default %d min %d max %d\n",
                       nn_eval_const_name(i), nn_eval_const_get(i),
                       nn_eval_const_lo(i), nn_eval_const_hi(i));
            print_search_options();   // parametri della ricerca (search/01_params.inc)
#endif
            // (SyzygyPath spostata in cima alla lista, subito dopo EvalFile — vedi sopra:
            //  evita il troncamento delle liste lunghe nelle GUI ChessBase/Fritz.)
            printf("uciok\n");
            fflush(stdout);
        }

        // UCI command: "isready"
        else if (strncmp(input, "isready", 7) == 0)
        {
            // AUDIT D (T3/S4): stesso mutex di info e bestmove del thread di ricerca (stdout non bufferizzato),
            // altrimenti un isready durante la ricerca poteva produrre "bestmove readyok".
            extern std::mutex output_mutex;
            std::lock_guard<std::mutex> out_lock(output_mutex);
            // Rete condivisa (01/10/2026): lo stato si dice una volta, al primo isready (all'avvio da GUI il motore
            // tace fino all'handshake). Nelle build senza TRIUMV_SHARED_NET la stringa e' vuota e non esce nulla.
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
        // "featdump <file>" -> scrive l'istogramma degli accessi per riga di
        // threatWeights, che alimenta gen_feat_perm.py (permutazione per localita').
        // ⚠️ I conteggi sono nello spazio degli indici CORRENTE: raccoglierli su un
        // binario che ha gia' una permutazione non-identita' darebbe una tabella da
        // COMPORRE con quella, non da usare al suo posto.
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
                // Istogramma HalfKA nello stesso giro, su <path>.psq
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
                // Co-occorrenza sparsa su <path>.cooc: solo le coppie sopra soglia,
                // altrimenti il file sarebbe da gigabyte.
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
        // (07/10/2026: tolto il comando `lazymirror`. Le dirty della rete le scrive la make, non c'e' piu' un
        // recupero pigro da accendere o spegnere.)

        // "bench [depth]" — suite fissa di 8 posizioni a profondita' fissa
        // (default 13): node-count CANONICO (la node-identity in un comando) +
        // NPS. Deterministico a Threads=1 (il default). Stato azzerato come
        // ucinewgame prima di ogni posizione.
        else if (strncmp(input, "bench", 5) == 0)
        {
            // "bench [depth] [file.epd]": il file sostituisce le 8 posizioni fisse, una
            // FEN per riga (max 64). Senza file il comando e' IDENTICO a prima: la firma
            // canary (bench liscio) non cambia. Serve al profilo PER POSIZIONE, perche'
            // due motori distribuiscono i nodi fra le 8 posizioni in modo diverso e la
            // media aggregata mescola finali a 2 colonne con mediogiochi a 12.
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
                search_clear();   // ogni posizione del bench parte da statistiche pulite
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
              // Sotto-bucket della ricerca: sono DENTRO "other" (i loro guard stanno in
              // funzioni chiamate fuori dai cinque guard principali), quindi vanno letti
              // come scomposizione di quel 26-28%, non sommati agli altri.
              // Voci della ricerca riscritta (04/10/2026): ordinamento = solo l'ordinamento parziale; history =
              // aggiornamento delle statistiche a fine nodo; gives-check = legalita' e scacco prima della mossa.
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
              // Scomposizione del forward NNUE. La domanda che blocca il lavoro sulla
              // sparsita' di L1: `ft_optimize` fu archiviato con la condizione "se fc_0
              // e' sotto il 5% del tempo, chiuso", e quella misura non fu mai fatta.
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
                // Il divario fra `eval` e il forward: minacce e pedoni in ritardo + resto del bridge.
                // --- Concentrazione degli accessi alle righe di threatWeights ---
                // Decide se la PERMUTAZIONE PER LOCALITA' ha senso. Una riga e' 1024 byte
                // (int8 x OutputDimensions) => 4 righe per pagina da 4 KB.
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
                        // Quante righe coprono il 90% degli accessi, e quanto spazio
                        // occuperebbero se fossero CONTIGUE (oggi sono sparse su 63 MB).
                        unsigned long long acc = 0;
                        int k90 = 0;
                        while (k90 < PROF_FEAT_N && acc < tot * 9 / 10) acc += h[k90++];
                        printf("  il 90%% degli accessi sta in %d righe = %.1f MB se contigue\n",
                               k90, k90 * 1024.0 / (1024 * 1024));
                        // Stessa analisi per le righe HalfKA (2048 byte l'una, 46 MB):
                        // il blocco piu' grosso del transformer, mai guardato.
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
                        // Il calore e' concentrato in POCHE ZONE CONTIGUE o sparso?
                        // Se e' a zone, la permutazione si fa riassegnando le tabelle
                        // costanti di make_index => ZERO costo a runtime, niente LUT.
                        // Granularita' di prova: blocchi da 256 righe (256 KB, 64 pagine).
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
                // Dentro il FT: dove va davvero il tempo.
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
                      // 🔴 prof_n_inc conta UNA VOLTA PER PROSPETTIVA: evaluate() chiama
                      // evaluate_side(WHITE) e evaluate_side(BLACK), ognuna incrementa di 1
                      // (il ramo TRIUMV_PERSP_TOGETHER, tolto il 25/09, faceva +=2). Quindi il
                      // massimo SANO e' 2,00 per eval = uno per prospettiva, non 1,00.
                      // La soglia di allarme era a 1,2 e si accendeva su qualunque motore
                      // sano: il 4/08/2026 stampava "update SPRECATI" con 1,83, che invece
                      // vuol dire che il 91,5% degli slot prospettiva-eval e' andato per la
                      // via incrementale e il resto per il refresh. Secondo falso allarme di
                      // questo stesso contatore (il primo: prof_n_eval incrementato due volte).
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

        // "exportprel <file>" (09/10/2026): la rete caricata salvata nel formato con PassedPawns v2 (innesto a zero).
        else if (strncmp(input, "exportprel ", 11) == 0)
        {
            char path[1024] = {0};
            sscanf(input + 11, "%1023s", path);
            printf("info string exportprel %s\n", nn_export_graft(1, path) ? "ok" : "fallito");
            fflush(stdout);
        }

        // "exportpst <file>" (10/10/2026): la rete caricata con PassedState di partenza (ogni stato = la riga v1, v1 a
        // zero): stessa valutazione, il bench deve restare quello della rete.
        else if (strncmp(input, "exportpst ", 10) == 0)
        {
            char path[1024] = {0};
            sscanf(input + 10, "%1023s", path);
            printf("info string exportpst %s\n", nn_export_pst(path) ? "ok" : "fallito");
            fflush(stdout);
        }

        // "exportgraft <mask> <file>" (09/10/2026): la rete caricata con i blocchi da innesto di mask (1 PassedRel,
        // innesto a zero se la rete non lo ha; 64 PassedState a zero, con la v1 della rete ancora accesa). Gli altri
        // blocchi (2, 4, 8, 16, 32) sono stati tolti il 10/10/2026.
        else if (strncmp(input, "exportgraft ", 12) == 0)
        {
            unsigned mask = 0;
            char     path[1024] = {0};
            if (sscanf(input + 12, "%u %1023s", &mask, path) == 2 && (mask == 1 || mask == 64))
                printf("info string exportgraft %u %s\n", mask, nn_export_graft(mask, path) ? "ok" : "fallito");
            else
                printf("info string uso: exportgraft 1|64 <file>\n");
            fflush(stdout);
        }

        // DIAGNOSI (10/10/2026): "pstidx" -> righe di PassedState della posizione per le due prospettive, crescenti,
        // nel formato del riferimento del trainer (_wip/passer_study/kit/verify_passedstate.py, confronto degli indici).
        else if (strncmp(input, "pstidx", 6) == 0)
        {
            unsigned long long bb[12];
            for (int i = 0; i < 12; i++)
                bb[i] = bitboards[i];
            for (int p = 0; p < 2; p++)
            {
                unsigned  idx[16];
                const int n = nn_pst_indices(bb, occupancies[both], p, idx);
                printf("pstidx %d", p);
                for (int i = 0; i < n; i++)
                    printf(" %u", idx[i]);
                printf("\n");
            }
            fflush(stdout);
        }

        // DIAGNOSTIC: "eval" -> static NNUE eval of the current position (cp,
        // side-to-move relative), no search => byte-identical for cross-checks.
        else if (strncmp(input, "eval", 4) == 0)
        {
            const int e = debug_eval_position();
            printf("eval %d\n", e);
            // Formato Stockfish, richiesto dagli strumenti standard (nnue-pytorch
            // cross_check_eval.py cerca esattamente questa riga). "internal units" e'
            // letterale: questo valore NON passa per la normalizzazione NORM_CP=392
            // applicata al `score cp` della search-info (print_search_info, search/13_iterdeep.inc), quindi e'
            // direttamente confrontabile con l'uscita del trainer. (27/07/2026)
            // 🔴 GREZZO, non `e`: `e` passa per nn_scale (blend psqt/positional, complessita',
            // materiale, rule50), che il trainer NON ha. Confrontare `e` col trainer paragona
            // due grandezze diverse — misurato R^2 0.73 anche su una coppia nota-buona.
            printf("NNUE evaluation  %d (side to move, internal units)\n",
                   debug_eval_position_raw());
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
            search_clear();   // statistiche, correzioni e memoria della gestione del tempo: nuova partita
            { extern std::atomic<int> g_optimism[2]; g_optimism[0] = g_optimism[1] = 0; }  // BUG FIX 2026-07-16: contempt dinamico non deve perdurare tra partite
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

        // UCI command: "ponderhit" (01/10/2026): la mossa prevista e' stata giocata, il ponder diventa ricerca normale.
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
            __llvm_profile_write_file();   // scrive il profilo (atexit LLVM non scatta sul ns exit)
#endif
            break;
        }

        // UCI button: "setoption name Clear Hash" (nessun value) -> svuota la TT.
        // Ferma i thread prima (come Hash/EvalFile): azzerare sotto una ricerca la
        // corromperebbe. Prefisso distinto da "...name Hash value" -> nessuna collisione.
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
            // 08/10/2026: stessa dimensione = nessuna riallocazione. fastchess rimanda Hash dopo OGNI ucinewgame: la
            // TT veniva liberata e riallocata a ogni partita, e su un nodo con la memoria frammentata le large pages
            // potevano mancare (ripiego su pagine normali a caso, motore per motore). La pulizia della TT resta a
            // ucinewgame.
            if (newMb != mb || !hash_table) {
                mb = newMb;
                init_hash_table(mb);
            }
        }

        // UCI command: "setoption name LargePages value 0|1" — ri-alloca subito
        // la TT con l'allocatore scelto (serve per l'A/B NPS sullo stesso binario).
        else if (strncmp(input, "setoption name LargePages value ", 32) == 0)
        {
            stop_search_threads();
            wait_for_search_done();
            g_large_pages = atoi(input + 32) != 0;
            init_hash_table(mb);
        }

        // UCI command: "setoption name EvalFile value <path>" -> reload the TRANN1
        // network at runtime. Default at startup = EvalFileDefaultName (own-lineage).
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
            // 08/10/2026: la stessa rete gia' caricata non si ricarica. fastchess rimanda EvalFile dopo OGNI
            // ucinewgame: il lato con la rete esterna la ricaricava a ogni partita (~0,6 s, 245 MB di large pages
            // allocati e liberati, area condivisa staccata e riattaccata), l'altro lato mai. Nell'SPRT ft2avg del
            // 08/10 (6+0.06) quel lato sul socket 1 cercava 0,22 ply meno dell'altro (sul socket 0 0,02) e il
            // socket 1 ha dato -20,8 Elo contro -0,1 del socket 0 con la stessa rete. Un file cambiato sotto lo
            // stesso percorso richiede un percorso diverso o un riavvio.
            static std::string s_loaded_net;
            if (!resolved.empty() && resolved == s_loaded_net)
                printf("info string EvalFile: %s already loaded\n", resolved.c_str());
            else if (resolved.empty())
                printf("info string EvalFile: '%s' not found (kept current net)\n", val);
            else if (nn_reload_big(resolved.c_str()))
            {
                s_loaded_net = resolved;
                // Net swapped: EVERY cached eval derived from the old net is stale
                // (same family as the finny g_net_gen bug, 2026-07-14). The eval
                // cache has no generation tag, and the TT carries old-net static
                // evals (ext eval16, consumed by g_tt_static_eval) plus scores
                // searched under the old eval -> clear both. Rare, search-stopped
                // operation; gates load the net once at startup (TT empty) so match
                // runs pay nothing. NOT cleared: corr_hist (self-corrects, SF keeps
                // it too). NB: "setoption EvalScale" mid-session has the same
                // staleness family (diagnostic-only option, documented here).
                for (size_t i = 0; i < thread_data.size(); ++i)
                    memset(thread_data[i].eval_cache, 0, sizeof(thread_data[i].eval_cache));
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

        // EvalScaleB0..B7 (15/08/2026): la stessa ricalibrazione, ma PER BUCKET di output
        // della rete (bucket = (pezzi - 1) / 4, come network.cpp:170). Default tutti 60 =
        // byte-identico al vecchio scalare unico.
        // ⚠️ Va DOPO il ramo di EvalScale: "EvalScale" e' prefisso di "EvalScaleB0", quindi
        //    con l'ordine invertito lo strncmp a 31 caratteri non li distinguerebbe... in
        //    realta' non collide (il 31-esimo carattere e' 'v' contro 'B'), ma l'ordine
        //    esplicito toglie il dubbio a chi legge.
        else if (strncmp(input, "setoption name EvalScaleB", 25) == 0
                 && input[25] >= '0' && input[25] <= '7'
                 && strncmp(input + 26, " value ", 7) == 0)
        {
            nn_set_eval_scale_bucket(input[25] - '0', atoi(input + 33));
            fflush(stdout);
        }

        // Costanti del blend (EvalPsqtW, EvalPosW, EvalComplexDiv, ...): un solo ramo,
        // i nomi stanno nella tabella di nnue_bridge.cpp. Va PRIMA del generic handler,
        // altrimenti verrebbero ingoiate da quello senza avere effetto.
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

        // Chess960 (27/09/2026, chess960.h): gioca gli arrocchi 960, verificato con la suite perft ufficiale
        // (tools/perft960.py, perft globale e tdperft). In release dal 27/09 sera: misura NPS PGO release
        // pre/post 960 = +0,57% [+0,53, +0,62] (nessun costo), test nullo +0,00%.
        else if (strncmp(input, "setoption name UCI_Chess960 value ", 34) == 0)
        {
            const char* v = input + 34;
            g_chess960 = strncmp(v, "true", 4) == 0 || v[0] == '1';
        }
        // "Move Overhead" (ms riservati per mossa a lag/GUI; prima hardcoded 50)
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


        // UCI: "setoption name SyzygyPath value <dir[;dir...]>" — parsed EXACTLY
        // like Stockfish (whitespace-tokenized, value trimmed/normalized) so any
        // GUI spacing loads identically. Loading touches global tablebase state;
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
                    // FIX F-001 (2026-07-02): le GUI mandano i check come "true"/"false"
                    // (atoi -> 0 per ENTRAMBI: un default-ON veniva spento in silenzio).
                    // Parse esplicito prima dell'atoi (identico al fix gia' validato su 6.0).
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
        // DIAGNOSTIC: "tdperft N" - perft sulla scacchiera per thread con verifica di chiavi, mailbox,
        // occupazioni e pseudo-legalita' a ogni nodo (search/15_tdperft.inc). Solo build di sviluppo.
        else if (strncmp(input, "tdperft ", 8) == 0)
        {
            extern void td_perft_driver(int depth);
            td_perft_driver(atoi(input + 8));
        }
        // DIAGNOSTIC: "nnperft N" - perft che valuta con la rete e confronta catena incrementale e refresh completo
        // (search/16_tdperft.inc). Solo build di sviluppo.
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

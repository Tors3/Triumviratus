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
#include "nnue_bridge.h"   // nn_acc_stats (diagnostic "accstats" command)
#include <thread>

#ifdef CLANG_PGO_GEN
// clang-PGO instrument build (-fprofile-generate): il path di exit del motore non fa
// scattare l'atexit di LLVM -> il quit-handler chiama questa per scrivere il profilo.
// extern "C" deve stare a file scope (NON dentro un blocco). Assente nei build normali/optimize.
extern "C" int __llvm_profile_write_file(void);
#endif
#include <string.h>
#include <string>

// Defined in main.cpp: resolve an NNUE filename/path to an existing path
// (tries the path as given, then next to the exe, then cwd). Used by the UCI
// "EvalFile" handler to load a big net specified at runtime.
std::string resolve_net_path(const std::string& name);

// "Move Overhead" (UCI): ms riservati per mossa a lag di I/O e GUI (prima 50 fisso).
static int g_move_overhead = 50;

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
    // FIX F-003 (2026-07-02): 0 = "la GUI non ha mandato movestogo". Il vecchio 30
    // rendeva IRRAGGIUNGIBILE il fallback g_tm_movestogo (default 24, tunable) in
    // parse_go: in sudden-death il TM allocava sempre remaining/30 (~20% piu' avaro
    // dell'intento) e TMMovesToGo era una tunable morta.
    movestogo = 0;
    movetime = -1;
    time_uci = -1;
    inc = 0;
    starttime = 0;
    stoptime = 0;
    timeset = 0;
    stopped = 0;
    g_node_limit = 0;
    g_mate_in = 0;
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

    // "go nodes N": hard node budget (datagen). Stops the search at ~N nodes
    // (per-node check in (q)search); independent of depth/time.
    if ((argument = strstr(command, "nodes")))
        g_node_limit = (U64) strtoull(argument + 6, nullptr, 10);

    // "go mate N": cerca un matto forzato in <= N mosse e fermati quando e' confermato.
    // " mate " con gli spazi: nessuna collisione con altri token del comando go.
    if ((argument = strstr(command, " mate ")))
        g_mate_in = atoi(argument + 6);

    // "go searchmoves m1 m2 ...": restringe la root alle sole mosse elencate (analisi).
    // I token-mossa seguono fino al primo token che NON e' una mossa legale (= la
    // prossima keyword UCI, es. "depth"/"infinite"). parse_move ritorna 0 su token
    // non-mossa -> stop pulito. Azzerato a ogni 'go' (nessuna keyword = cerca tutte).
    g_searchmoves_count = 0;
    if ((argument = strstr(command, "searchmoves")))
    {
        char* p = argument + 11;   // oltre "searchmoves"
        while (*p == ' ') p++;
        while (*p && g_searchmoves_count < 256)
        {
            char tok[8];
            int n = 0;
            while (p[n] && p[n] != ' ' && n < 7) { tok[n] = p[n]; n++; }
            tok[n] = '\0';
            int mv = parse_move(tok);
            if (!mv) break;         // token non-mossa -> fine lista searchmoves
            g_searchmoves[g_searchmoves_count++] = mv;
            p += n;
            while (*p == ' ') p++;
        }
    }

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

    // Gate TMv2: soglia base-time ELIMINATA 2026-07-24 (sweep VM 8s/12s/15s, nessuna
    // regressione a nessun TC -> il vecchio cliff -22.94 Elo @10+0.1 non si riproduce
    // col codice attuale). Resta solo il gate sull'incremento (mai testato, prudenza
    // invariata: TMv2 usa formule pool-increment che presuppongono inc>0).
    g_tmv2_tc_ok = (inc > 0);

    if (time_uci != -1)
    {
        timeset = 1;
        const int overhead = g_move_overhead;  // ms reserved for lag / output (UCI "Move Overhead")
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
            int mtg;
            if (movestogo > 0) {
                mtg = movestogo;
            } else if (g_tmv2_alloc && g_tmv2_tc_ok) {
                // TM v2 Q-05a (Caissa): moves-left stimate CALANO col progredire della
                // partita (~35 a mossa 1 -> min a mossa ~40): meno overspend in apertura,
                // piu' tempo nel mediogioco avanzato. repetition_index conta le semimosse
                // giocate dalla posizione iniziale -> fullmove approssimato.
                int fullmove = repetition_index / 2 + 1;
                mtg = g_tmv2_mtg_base - g_tmv2_mtg_slope * fullmove / 100;
                if (mtg < g_tmv2_mtg_min) mtg = g_tmv2_mtg_min;
            } else {
                mtg = g_tm_movestogo;                       // assunzione moves-to-go (tunable)
            }

            if (g_tmv2_alloc && g_tmv2_tc_ok) {
                // TM v2 Q-05b (Alexandria, pooled increments): l'incremento entra nel
                // MONTANTE diviso per mtg invece che come addendo fisso per-mossa ->
                // elimina alla radice la classe di bug F-012 (il termine 0.94*inc fisso
                // che a orologio basso sfora), e l'overhead e' riservato per TUTTE le
                // mosse restanti, non solo per questa.
                long long pool = (long long)time_uci + (long long)inc * (mtg - 1)
                               - (long long)overhead * (2 + mtg);
                if (pool < 1) pool = 1;
                optimum = (int)(pool / mtg * g_tmv2_opt_pct / 100);
                maximum = optimum * g_tm_max_mult / 100;
            } else {
                optimum = remaining / mtg + inc * g_tm_inc_frac / 100;    // quota base + % incremento (tunable)
                maximum = optimum * g_tm_max_mult / 100;                   // burst su posizioni difficili (tunable)
            }

            // TM v2 Q-06 (Caissa): l'avversario ha giocato la risposta PREDETTA dalla
            // nostra PV -> TT calda -> serve meno tempo (hit); mossa a sorpresa ->
            // TT fredda -> un po' di piu' (miss). Applicato PRIMA del cap anti-forfeit.
            if (g_tmv2_pred && g_tmv2_tc_ok && g_tm_pred_hash) {
                int pf = (hash_key == g_tm_pred_hash) ? g_tmv2_pred_hit : g_tmv2_pred_miss;
                optimum = (int)((long long)optimum * pf / 1000);
            }

            int cap = remaining * 4 / 5;                    // never risk more than ~80% of the clock
            // FIX time-forfeit (2026-07-03): a orologio basso con incremento alto il termine
            // inc*IncFrac rende OPTIMUM > cap (es. 30s+0.5: sotto ~0.6s residui optimum~485ms
            // sfora l'orologio) e il vecchio "if (maximum < optimum) maximum = optimum"
            // SCAVALCAVA il cap -> stoptime oltre il tempo disponibile -> perdita per tempo.
            // Bug latente pre-esistente (anche 5.0); clampare ANCHE optimum lo chiude.
            if (cap < 1) cap = 1;
            if (optimum > cap) optimum = cap;
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

        char szpath[4096];   // scratch for SF-style SyzygyPath value parsing

        // UCI command: "uci" (EXACT match: strncmp len 3 would also swallow
        // "ucinewgame" -> its handler below would be dead code).
        if (strcmp(input, "uci") == 0)
        {
            // NB: VERSION contiene gia' il separatore (" - 6.0") -> %s%s, senza spazio
            // in mezzo. Con "%s %s" usciva "Triumviratus  - 6.0" (doppio spazio): l'id
            // name e' l'identita' del motore per GUI e liste rating, deve essere pulito.
            printf("id name %s%s\n", NAME, VERSION);
            printf("id author %s\n", AUTHOR);
            printf("option name Hash type spin default 64 min 1 max %d\n", max_hash);
            printf("option name Threads type spin default 1 min 1 max %d\n", max_threads);
            // MultiPV (analisi stile Stockfish): opzione UFFICIALE, sempre advertised
            // (anche in TRIUMV_RELEASE). Il setoption arriva via il generic handler
            // -> set_search_param("MultiPV") in threads.cpp.
            printf("option name MultiPV type spin default 1 min 1 max 64\n");
            // Opzioni ufficiali di analisi/utilizzo (sempre visibili, anche in release):
            printf("option name UCI_ShowWDL type check default false\n");  // W/D/L nelle info-line (via generic handler)
            printf("option name Clear Hash type button\n");                // svuota la TT su richiesta
            printf("option name UCI_EngineAbout type string default %s%s by %s\n", NAME, VERSION, AUTHOR);
            // --- Tuning / experimental / diagnostic options: hidden in the release build
            //     (define TRIUMV_RELEASE). Dev/tuning builds expose them for SPSA. ---
#ifndef TRIUMV_RELEASE
            printf("option name Depth type spin default 0 min 0 max 64\n");
            printf("option name DataLog type check default false\n");
            printf("option name DataFile type string default triumviratus_dataset.txt\n");
            // Bundle 3.9 (micro-fix dietro toggle, ablazione stile 3.8)
            printf("option name MateDistPruning type check default true\n");   // P1.4
            printf("option name DrawDither type check default true\n");        // P1.11 patte = ±1cp
            printf("option name TTCutBonus type check default true\n");        // P1.13 history bonus al ttMove su TT-cutoff
            printf("option name TTCutBonusScale type spin default 111 min 0 max 400\n");  // /100 del stat_bonus; SPSA
            printf("option name TTAgeRefresh type check default true\n");      // P1.10a probe-hit rinfresca l'age
            printf("option name PawnKeyIncr type check default true\n");       // P2.1 pawn key incrementale (node-identical)
            printf("option name NPKeyIncr type check default true\n");         // CorrNonPawn: np_key incrementale (node-identical; false=rescan oracle)
            printf("option name TTStaticEval type check default true\n");      // P1.1 static eval in TT (salva la forward NNUE)
            printf("option name FastRepScan type check default true\n");       // P2.2 repetition scan a finestra
            printf("option name EvasionGen type check default true\n");        // P2.3 evasioni mascherate (node-identical)
            printf("option name ThreadVoting type check default false\n");     // P1.12 selezione SMP per voto pesato (SF-style)
            // Toggle da CO-TUNE (default OFF = byte-identico; si accendono nel mega-SPSA 4.0)
            printf("option name QSChecks type check default true\n");          // P1.3 quiet check alla prima ply di qsearch
            printf("option name NMPVerif type check default true\n");          // P1.6 NMP verification + no doppia null
            printf("option name NMPVerifDepth type spin default 1 min 1 max 64\n");
            printf("option name LMPImproving type check default true\n");      // P1.7 LMP SF-style senza cap d8
            printf("option name LMPBase type spin default 16 min 0 max 20\n");
            printf("option name LMPQuad type spin default 138 min 20 max 300\n"); // /100
            printf("option name CheckExtDepth type spin default 0 min 0 max 128\n");  // BAKATO a 0 (+7.98 Elo, LOS 97.3% @30+0.3): niente check-extension, come SF
#endif
            printf("option name Move Overhead type spin default 50 min 0 max 5000\n"); // ms riservati a lag/GUI per mossa
#ifndef TRIUMV_RELEASE
            printf("option name EvalCacheUndamp type check default true\n");  // N1: eval-cache senza fifty in chiave (valore undamped)
            printf("option name ProbCutTT type check default true\n");        // N2: fail-high di probcut salvato in TT (SF)
            printf("option name EvalOff type check default false\n");
#endif
            printf("option name EvalFile type string default %s\n", nn_default_net_name());  // own-lineage net (dev and release alike)
            // Gruppo Syzygy STANDARD (come Stockfish & co.): SyzygyPath + le 3 compagne.
            // Le GUI ChessBase/Fritz riconoscono un motore come "tablebase-capable" dal SET
            // completo: con la sola SyzygyPath il campo poteva non comparire. Stampate QUI (in
            // cima, prima del blocco-tuning) -> sempre visibili (cutechess le vede ovunque) e
            // fuori dal guard TRIUMV_RELEASE (sempre on). SyzygyPath e' wired (carica le TB via
            // Fathom); le 3 compagne sono advertised-only per ora (il generic-handler le accetta
            // senza errore) -> default = comportamento reale del motore (50-move on, fino a 7 uomini).
            printf("option name SyzygyPath type string default <empty>\n");
            printf("option name SyzygyProbeDepth type spin default 1 min 1 max 100\n");
            printf("option name Syzygy50MoveRule type check default true\n");
            printf("option name SyzygyProbeLimit type spin default 7 min 0 max 7\n");
#ifndef TRIUMV_RELEASE
            printf("option name EvalScale type spin default 60 min 10 max 2000\n");  // % scala eval -> ricalibra ai margini search (TRANN1)
            printf("option name EvalCache type check default true\n");
            printf("option name FinnyTables type check default true\n");   // BAKED ON: +6.9% NPS, eval bit-identica
            // SingleBoard + OccIncr consolidati nel codice 2026-06-07 (sempre ON, niente toggle)
            printf("option name Improving type check default true\n");
            printf("option name NodeTM type check default true\n");
            printf("option name SingularExt type check default true\n");
            printf("option name CorrHist type check default true\n");
            printf("option name ProbCut type check default true\n");
            printf("option name ContHistPrune type check default true\n");
            printf("option name TT4Way type check default false\n");
            printf("option name TTEvalImprove type check default true\n");   // P1.1: tt_score come eval migliorata nelle decisioni di pruning
            printf("option name UpcomingRep type check default true\n");     // P1.2: ripetizione imminente (cuckoo) -> alpha >= 0
            // Toggle di ablazione dei fix 3.8 (OFF = comportamento 3.7) — night tournament
            printf("option name TTMove24 type check default true\n");        // P0.1 TT move 24 bit
            printf("option name SeeFix type check default true\n");          // P0.2 SEE quiet + e.p.
            printf("option name KillerLMRFix type check default true\n");    // P0.3 sconto LMR killer
            printf("option name QsearchCorr type check default true\n");     // P0.4 corr in qsearch
            printf("option name ImprovingFix type check default true\n");    // P0.6 sentinel improving
            printf("option name CorrHistMulti type check default true\n");   // BAKED ON: HM +6.2 LOS87.6% @1338
            printf("option name CorrHistCont type check default true\n");    // continuation correction history (SF): corregge la static eval per le ultime 2 mosse nel cammino
            printf("option name CorrContWeight type spin default 100 min 0 max 400\n");  // /100 contributo cont alla somma corr; co-tunabile
            printf("option name CorrNonPawn type check default false\n");     // corrhist non-pedoni PER-LATO (port Pawnocchio/SF, 2026-07-03): chiave = nonpawn-Zobrist di UN colore
            printf("option name CorrNonPawnWeight type spin default 100 min 0 max 400\n");  // /100 contributo delle 2 tabelle non-pawn; co-tunabile
            printf("option name CorrMaterial type check default true\n");      // BAKED 2026-07-24 (bundle lean SPRT +10.43); SF #5556 corrhist per MATERIAL-KEY -> eval finali/fortezze
            printf("option name CorrMaterialWeight type spin default 100 min 0 max 400\n");  // /100 contributo della tabella material; co-tunabile
            printf("option name PawnHistory type check default true\n");    // ordering quiet per struttura pedonale (SF-style, peso 2x)
            printf("option name PawnHistoryWeight type spin default 187 min 0 max 800\n");  // [4.1 BAKE 126->139]
            printf("option name ThreatOrdering type check default true\n");  // ordering quiet per minacce (SF #2): salva pezzo minacciato da inferiore
            printf("option name ThreatScale type spin default 4212 min 0 max 8000\n");  // contributo = scale/100 * pieceValue * (from-to minacciato); co-tunabile
            printf("option name ThreatHist type check default true\n");                   // [BAKE 2026-07-03] history quiet condizionata dalle minacce (from/to attaccata)
            printf("option name ThreatHistWeight type spin default 130 min 0 max 400\n");  // /100 scala extra threat-history in ordering [BAKE 2026-07-04 100->75->60, ultimo step LOS82.13% @296g + SPSA concorde]
            printf("option name CheckOrdering type check default true\n");   // bonus quiet che danno scacco diretto (SF #3), filtro SEE>=-75
            printf("option name CheckBonus type spin default 13357 min 0 max 30000\n");  // bonus scacco diretto; co-tunabile (fix 2026-06-10: printf diceva 8000 ma g_=4201)
            printf("option name QuietOffense type check default true\n");    // BAKED 2026-07-24 (solo wall-pawn, vedi WallPawnPenalty); port Reckless move-ordering
            printf("option name OffenseBonus type spin default 0 min 0 max 40000\n");        // spento: isolato NEGATIVO (-9.70 LOS 3.38% @1290g) -> in attesa di ri-test a peso grosso
            printf("option name WallPawnPenalty type spin default 16800 min 0 max 40000\n"); // BAKED 2026-07-24 (3x: +8.43 nElo LOS 99.14% @9652g, 12+0.12); era 5600
            printf("option name ContHist36 type check default true\n");      // conthist 3-ply+6-ply nell'ordering quiet (SF #4)
            printf("option name ContHist36Weight type spin default 37 min 0 max 400\n");  // /100 peso 3/6-ply; co-tunabile
            printf("option name PriorBonus type check default true\n");       // V2: su fail-low bonus alla mossa precedente (conthist/main + capture-hist)
            printf("option name PriorBonusScale type spin default 189 min 0 max 400\n");  // /100 del td_stat_bonus; co-tunabile
            printf("option name LowPlyHistory type check default true\n");    // #5: history per-ply near-root nell'ordering quiet
            printf("option name LowPlyWeight type spin default 179 min 0 max 200\n");  // contributo lowply; co-tunabile
            printf("option name StatEvalDiffMult type spin default 8 min 0 max 60\n");  // [4.1 BAKE 14->6] SF static-eval-diff ordering (neutro a ogni valore)
            printf("option name CutoffCntPenalty type spin default 2 min 0 max 3\n");        // SF cutoffCnt-LMR: 0=off, 1=SF (riduzione +1 se figlio cutoffCnt>3)
            printf("option name ProbCutInCheckMargin type spin default 335 min 0 max 800\n");  // [4.1 BAKE 0->523] SF probcut-sotto-scacco
            printf("option name MainHistWeight type spin default 93 min 50 max 400\n");    // [4.1 BAKE 122->168]
            printf("option name ContHistWeight type spin default 135 min 50 max 400\n");    // [4.1 BAKE 80->96]
            printf("option name LMPScale type spin default 35 min 30 max 250\n");     // [3.7] scala % soglia LMP
            printf("option name ContHistMulti type check default true\n");   // BAKED ON: HM +6.2 LOS87.6% @1338
            printf("option name MovePicker type check default true\n");
            printf("option name DiverseSMP type check default true\n");   // BAKED ON (bake-on-trust): wider-only SMP diversity
            printf("option name DiverseSMPAmount type spin default 1 min 0 max 4\n");
            printf("option name DiverseSMPFine type spin default 96 min 0 max 512\n");  // BAKATO: +4.91 Elo LOS 88.6% @16+0.16 Threads=8, 1700g. 0=legacy a ply interi
            printf("option name MulticutTTMalus type check default false\n");           // port SF a47a1c1804: malus alla ttMove smascherata dal multicut
            printf("option name MulticutTTMalusScale type spin default 100 min 0 max 300\n");
            printf("option name MultiCut type check default true\n");
            printf("option name TTPvAmount type spin default 1 min 0 max 2\n");   // ex-PV LMR reduction in ply (0=off); co-tunable
            printf("option name NMPEvalScale type check default false\n");
            printf("option name RFPDepth8 type check default false\n");
            printf("option name RazorDepth4 type check default false\n");
            printf("option name QFutility type check default false\n");
            printf("option name HistBonusSF type check default true\n");   // BAKED ON: +24 LOS99.99% @810
            printf("option name CaptureHist type check default true\n");   // baked ON, div16+malus-fix (era -21 a div1)
            printf("option name CaptureHistDiv type spin default 21 min 1 max 64\n");   // REVERT 2026-07-23 (SPSA B1 evaporato @4452g)
            printf("option name NMPEvalDiv type spin default 100 min 50 max 1000\n");
            printf("option name QFutMargin type spin default 199 min 0 max 500\n");
            printf("option name HistBonusMult type spin default 490 min 1 max 600\n");   // [4.1 BAKE 282->326]
            printf("option name HistBonusSub type spin default 299 min 0 max 400\n");      // [4.1 BAKE 59->35]
            printf("option name HistBonusMax type spin default 2946 min 200 max 8000\n"); // [4.1 BAKE 1247->2439; max 4000->8000 il 2026-07-10: il default era INCOLLATO al max dichiarato -> SPSA poteva solo scendere. Nessun clamp compilato, allargare e' sicuro]
            printf("option name LazyEval type check default true\n");
            printf("option name TimeMgmt type check default true\n");
            printf("option name AggrLMR type check default false\n");
            printf("option name AggrLMRDiv type spin default 903 min 512 max 6000\n");
            printf("option name AggrLMRClamp type spin default 4 min 1 max 6\n");
            printf("option name StatScoreLMR type check default true\n");                          // LMR butterfly continua (fix sotto-riduzione vs SF15.1)
            printf("option name ContHistLMR type check default true\n");                           // conthist 1/2/4 ply nella LMR
            printf("option name CutNodeLMR type check default true\n");                            // riduzione extra sui cut-node
            printf("option name LMRStatScoreDiv type spin default 5430 min 1000 max 30000\n");       // [4.1: tenuto 4.0 - il BAKE 13790 gonfiava l'albero 2.5x]
            printf("option name LMRStatScoreOffset type spin default -2953 min -4000 max 12000\n");     // [4.1: tenuto 4.0 - parte del bloat LMR revertito]
            printf("option name LMRContHistDiv type spin default 10942 min 1000 max 40000\n");       // [3.7] ContHistLMR: divisore conthist
            printf("option name CutNodeLMRExtra type spin default 1 min 0 max 3\n");                 // CutNodeLMR: ply extra
            printf("option name NMPBase type spin default 5 min 1 max 10\n");   // max alzato 6->10: SF usa base 7 (co-tune toward SF)
            printf("option name NMPDiv type spin default 3 min 2 max 8\n");
            printf("option name LMREvalMargin type spin default 43 min 0 max 400\n");
            printf("option name LMRTTDepth type spin default 1 min 0 max 3\n");
            printf("option name LMRBase type spin default 22 min 0 max 200\n");   // [3.7]
            printf("option name LMRDiv type spin default 447 min 100 max 500\n");   // [3.7]
            printf("option name RFPMargin type spin default 62 min 20 max 200\n");        // bakato: 30->21
            printf("option name RazorBase type spin default 293 min 100 max 600\n");
            printf("option name RazorMult type spin default 77 min 20 max 250\n");       // bakato: 102->139
            printf("option name FutilityBase type spin default 202 min 20 max 300\n");
            printf("option name FutilityMult type spin default 61 min 20 max 200\n");   // [3.7]
            printf("option name FutilityImproving type spin default 186 min 0 max 200\n"); // bakato: 60->93
            printf("option name SingularDoubleMargin type spin default 59 min 0 max 200\n"); // bakato: 63->43
            // F-002/F-004/F-005 (audit 2026-07-02): ex-hardcoded promossi a tunable + LMR-catture.
            // Default = comportamento storico bit-identico; leve per SPSA (preset 5.1 in SPSA Lab).
            printf("option name QSDeltaMargin type spin default 1488 min 200 max 4000\n");   // delta-pruning qsearch (unita'-eval)
            printf("option name SingularMarginPD type spin default 1 min 1 max 10\n");        // margine singular per-depth
            printf("option name TMDropThresh type spin default 6 min 1 max 100\n");           // soglia score-drop TM; BAKE 2026-07-03 TM post-F-003 8->6
            printf("option name TMDropCap type spin default 160 min 50 max 1000\n");          // cap score-drop TM; BAKE 2026-07-03 TM post-F-003 200->160
            printf("option name NodeTMBase type spin default 133 min 100 max 200\n");         // NodeTM: scale=(base-frac*100)/100; BAKE 2026-07-03 TM post-F-003 140->133
            printf("option name NodeTMMin type spin default 58 min 30 max 100\n");            // NodeTM: clamp inferiore %%; BAKE 2026-07-03 TM post-F-003 60->58
            printf("option name LMRMinDepth type spin default 3 min 2 max 6\n");              // gate depth LMR (SF=2)
            printf("option name LMRMinMoves type spin default 1 min 1 max 8\n");              // gate move-count LMR (SF=2)
            printf("option name LMRCaptures type check default true\n");                     // F-004: LMR sulle catture (OFF=byte-identico)
            printf("option name SeeGE type check default true\n");                            // F-008: SEE a soglia early-exit, +1.81%% NPS node-identico (ON 2026-07-02)
            printf("option name SeeGEVerify type check default false\n");                     // F-008: oracle cross-check dei due path SEE (diagnostica)
            printf("option name LMRCapScale type spin default 52 min 10 max 150\n");          // REVERT 2026-07-23 (SPSA B1 evaporato); %% lmr_table sulle catture
            // ==== F-018 (secondo audit 2026-07-03): micro-tecniche Obsidian/Berserk, default OFF/neutro ====
            printf("option name PostLMRHist type check default false\n");                     // conthist update post re-search LMR (SF mainline); test pulito neutro -> OFF, candidato tuning
            printf("option name PostLMRHistScale type spin default 106 min 0 max 400\n");
            printf("option name TTCutRefine type check default true\n");                      // [BAKE 2026-07-03] cutoff TT: depth+1 sui fail-high, coerenza cutnode, fifty gate
            printf("option name TTResearch type check default false\n");                      // Q-14 (Ethereal): fail-low anticipato a depth-1 su entry UPPER
            printf("option name TTResearchMargin type spin default 74 min 0 max 400\n");      // Q-14: margine (scala-56) sotto alpha per il fail-low a depth-1
            printf("option name TTCutFifty type spin default 89 min 50 max 100\n");
            printf("option name TTCutMalus type check default false\n");                     // #3d malus quiet avversaria su TT-cut (duale TTCutBonus). Bake revertito 2026-07-06, vedi threads.cpp
            printf("option name TTCutMalusSeen type spin default 3 min 0 max 16\n");
            printf("option name GoodCapHistDiv type spin default 32 min 0 max 256\n");         // BAKED 2026-07-24 (bundle lean SPRT +10.43; era 18); #4 split good/bad a soglia -(mvv+caphist)/div
            printf("option name AspAvg type check default false\n");                          // #5a aspiration centrata su averageScore. Bake revertito 2026-07-07 (rumore)
            printf("option name AspFHRed type check default true\n");                         // [BAKE 2026-07-03] depth-1 per fail-high consecutivi alla root
            printf("option name SingularMinDepth type spin default 6 min 4 max 12\n");        // #6a gate depth singular (Obsidian 5, Berserk 6)
            printf("option name SingularDECap type spin default 1 min 0 max 16\n");           // #6b cap catena double-ext (0=off, Berserk 6)
            printf("option name SingularPlyGuard type check default false\n");                // #6c niente singular oltre ply >= 2*rootDepth
            printf("option name NegExtAlpha type spin default 1 min 0 max 3\n");              // #6d negext se ttScore<=alpha (Berserk 1)
            printf("option name FailHighSmooth type check default true\n");                   // [BAKE 2026-07-03] return (score+beta)/2 su RFP/qsearch
            printf("option name NMPStaticMargin type check default false\n");                 // #8a NMP: static_eval >= beta - mult*depth + bias (SF)
            printf("option name NMPStaticMult type spin default 23 min 0 max 100\n");
            printf("option name NMPStaticBias type spin default 414 min 0 max 1200\n");
            printf("option name NMPTTNoisy type check default false\n");                      // #8b R+1 se TT move cattura
            printf("option name AlphaDepthDec type check default true\n");                    // [BAKE 2026-07-03] depth-- per le mosse restanti quando alpha sale
            printf("option name FHBoostMargin type spin default 2 min 0 max 500\n");          // #10a bonus a depth+1 se best > beta+margine. Bake revertito 2026-07-07 (rumore)
            printf("option name HistTrivGuard type check default false\n");                   // #10b niente bonus su cutoff 'gratis'
            printf("option name MalusPct type spin default 69 min 10 max 300\n");            // #10c malus = bonus*pct/100
            printf("option name EasyCapGate type check default false\n");                     // #11 niente NMP con pezzo in presa facile
            printf("option name RFPHistThresh type spin default 64 min 0 max 7000\n");         // #12 RFP gated su history della hash move quiet (0=off)
            printf("option name KillerReset type check default true\n");                     // #13 azzera killer del ply figlio a ogni nodo (+12.15 Elo)
            printf("option name CounterMove type check default true\n");                      // OFF = via la countermove heuristic (SF l'ha rimossa: PR #5441, 978k partite)
            printf("option name QSMoveCap type spin default 1 min 0 max 16\n");               // #14 cap mosse qsearch non-in-check (0=off, Obsidian 3)
            printf("option name QSDrawCheck type check default false\n");                     // F-015 draw-detection in qsearch. Bake revertito 2026-07-07 (rumore)
            printf("option name EPKeyFix type check default true\n");                         // F-017 niente phantom-ep nella hash key (fix correttezza, SPRT +4.31 Elo)
            printf("option name HistReductionDiv type spin default 3190 min 500 max 8000\n"); // bakato: 3500->1041
            printf("option name AspInitDelta type spin default 19 min 8 max 60\n");       // bakato: 25->31
            printf("option name AspGrow type spin default 34 min 30 max 200\n");          // bakato: 100->31
            printf("option name AspScoreMult type spin default 38 min 0 max 2000\n");       // Q-28 (Stormphrax): finestra asp += |avgSq(score)|*mult>>20. 0=OFF
            printf("option name CMHCScale type spin default 6 min 0 max 200\n");           // Q-12 (SF #6653): bonus conthist *= (100 + scale*consistenza)/100. 0=OFF
            printf("option name FollowPV type check default false\n");                     // Q-15 (SF #6656): niente IIR/futility/LMP sui nodi che riseguono la PV precedente
            printf("option name RFPBadNode type spin default 4 min 0 max 200\n");          // Q-20a (Alexandria): rfp_margin -= questo se !tt_move. 0=OFF
            printf("option name FutSpareQuiet type check default false\n");                // Q-20c (Caissa): la futility non pota mai la prima quiet del nodo
            printf("option name QSDeltaBestCase type check default false\n");              // Q-20d (Ethereal): delta-pruning qsearch col best-case reale (vittima max + promo)
            printf("option name QSBCMargin type spin default 183 min 0 max 800\n");        // Q-20d: cuscino sopra il best-case
            printf("option name ProbCutMargin type spin default 271 min 60 max 400\n");
            printf("option name ProbCutImprove type spin default 4 min 0 max 200\n");         // Q-20b (Alexandria): probcut_beta -= questo se improving. 0=OFF
            printf("option name CorrCap type spin default 50 min 8 max 128\n");
            printf("option name CorrLearnDiv type spin default 303 min 64 max 2048\n");
            printf("option name CorrAsym type spin default 137 min 64 max 192\n");   // Tier-1: correzioni positive piu' lente (/128); 128=simmetrico
            printf("option name ContHistDiv type spin default 3684 min 1000 max 12000\n");
            printf("option name LmrDepthPrune type spin default 1 min 0 max 1\n");  // SF: gating futility+conthist sulla depth ridotta-LMR (chiude gap-midgame). 0=off, 1=on
            printf("option name LmrDepthHistDiv type spin default 4509 min 500 max 30000\n");  // PASSO1 SF: prune_depth += history/div (protezione-history). Solo con LmrDepthPrune ON
            printf("option name ContHistPruneDepth type spin default 2 min 1 max 12\n");  // PASSO2 SF: gate conthist-prune (SF lmrDepth<6). Col blocco si alza
            printf("option name CutoffStats type spin default 0 min 0 max 1\n");    // diagnostica move-ordering: 1=stampa 'info string FMC ...' (first-move-cutoff rate) a fine ricerca
            printf("option name TTMoveKeep type spin default 1 min 0 max 1\n");      // SF: conserva la TT move sui fail-low senza mossa -> +ttrate ai cut-node. 0=off (byte-identico), 1=on
            printf("option name TTTwoLevel type spin default 1 min 0 max 1\n");       // 5.1 BAKE ON: TT a 2 livelli (depth-preferred + always-replace), ~-4%% nodi. 0=off (1-via), 1=on
            printf("option name LargePages type spin default 1 min 0 max 1\n");        // TT su large pages 2MB (come i pesi NNUE). 0=off (new[], baseline), 1=on. Richiede privilegio "Lock pages in memory"
            printf("option name EvalTTWrite type spin default 0 min 0 max 1\n");       // cache static eval su MISS (SF :830). PROVATO 1-via=albero x1.87 (roundtrip eval). Re-test con two-level. 0=off, 1=on
            printf("option name HistPruneMargin type spin default 2097 min 200 max 4000\n");   // [3.7]
            printf("option name SEECaptureMargin type spin default 81 min 20 max 300\n");   // REVERT 2026-07-23 (SPSA B1 evaporato)
            printf("option name SEEQuietMargin type spin default 116 min 10 max 400\n");   // [3.7] max alzato per SPSA-cut
            // Capture futility pruning (SF Step 14, default OFF). Toggle = spin 0/1; cp margins are the SPSA targets, depth gate fixed.
            printf("option name CaptureFutility type spin default 1 min 0 max 1\n");
            printf("option name CapFutBase type spin default 155 min 0 max 500\n");   // REVERT 2026-07-23 (SPSA B1 evaporato)
            printf("option name CapFutMult type spin default 140 min 0 max 400\n");    // REVERT 2026-07-23 (SPSA B1 evaporato)
            printf("option name CapFutChist type spin default 205 min 0 max 400\n");   // REVERT 2026-07-23 (SPSA B1 evaporato); [F-002 audit 2026-07-02] 125->123
            printf("option name CapFutDepth type spin default 8 min 1 max 12\n");    // REVERT 2026-07-23 (SPSA B1 evaporato)
            // Ponte cp->unita'-eval del termine vittima, /10000 insieme a EvalScale (v. threads.cpp).
            // 392 = NORM_CP -> fattore 2.35 a EvalScale=60. ~167 riproduce il vecchio 1x (il bug).
            printf("option name CapFutVicScale type spin default 392 min 0 max 1000\n");
            // Other missing SF cut features (default OFF/legacy). Margins = SPSA targets; toggles/gates fixed.
            printf("option name OppWorsening type spin default 1 min 0 max 1\n");
            printf("option name OppWorseMargin type spin default 23 min 0 max 100\n");
            printf("option name TripleExt type spin default 1 min 0 max 1\n");
            printf("option name SingularTripleMargin type spin default 319 min 0 max 400\n");
            printf("option name NegExtTT type spin default 2 min 0 max 4\n");     // -ext on ttMove>=beta (0=off,1=legacy,3=SF)
            printf("option name NegExtCut type spin default 3 min 0 max 3\n");    // -ext on cutNode (0=off/legacy,2=SF)
            printf("option name CorrValMargin type spin default 1 min 0 max 1\n");
            printf("option name CorrValRFP type spin default 41 min 0 max 256\n");
            printf("option name CorrValExt type spin default 1 min 0 max 1\n");      // 5.0-B: folda |corr| in futility/SEE/LMR
            printf("option name CorrValFut type spin default 38 min 0 max 400\n");   // peso fold futility
            printf("option name CorrValSee type spin default 27 min 0 max 400\n");   // peso fold SEE
            printf("option name CorrValLmr type spin default 83 min 0 max 400\n");   // peso fold LMR
            // Gate strutturali (interi delle decisioni di profondita', mai tunati: SPSA non li vede)
            printf("option name LMPCheckGuard type check default false\n");   // guard: la LMP non pota le quiet che danno scacco
            printf("option name IIRMinDepth type spin default 4 min 1 max 12\n");
            printf("option name IIRAmount type spin default 1 min 1 max 3\n");
            printf("option name IIRNotInCheck type check default false\n");           // fix: IIR non spara in scacco
            printf("option name ProbCutMinDepth type spin default 5 min 3 max 10\n");
            printf("option name ProbCutDepthOff type spin default 4 min 2 max 6\n");
            printf("option name SingularTTMargin type spin default 3 min 1 max 6\n");
            printf("option name SingularDepthDiv type spin default 2 min 1 max 4\n");
            printf("option name MalusScaled type spin default 1 min 0 max 1\n");     // 5.0-B: malus history scalato per move-order
            printf("option name MalusScaleCoef type spin default 59 min 0 max 200\n");
            printf("option name MalusQuad type check default false\n");                  // decay malus quadratico invece che lineare
            printf("option name MalusQuadCoef type spin default 45 min 0 max 200\n");
            printf("option name AspStableShrink type check default false\n");        // finestra aspiration piu' stretta se la ricerca e' stabile
            printf("option name AspStableCap type spin default 4 min 0 max 15\n");
            printf("option name RazorTTQuiet type check default false\n");           // razora solo con TT move rumorosa
            printf("option name RazorTTLower type check default false\n");           // niente razoring se bound TT = LOWER
            printf("option name SingularExactMargin type check default false\n");    // SBAKATA 19/07: +8.60 a 10+0.1 ma -8.60 a 20+0.2 (crossover di profondita' ~18)
            printf("option name SingularExactDecouple type check default false\n");  // doppia/tripla estensione ancorate al margine PIENO (anti-cascata a TC lungo)
            printf("option name SingularExactMaxDepth type spin default 0 min 0 max 40\n");  // 0=nessun cap; sopra ~18 il dimezzamento esplode l'albero
            printf("option name DoDeeper type spin default 0 min 0 max 1\n");        // 5.0-B: doDeeper/doShallower nella re-search LMR
            printf("option name DoDeeperBase type spin default 43 min 0 max 400\n");
            printf("option name HindsightExt type spin default 1 min 0 max 1\n");    // 5.1 BAKE ON (Pawnocchio): ri-estendi nodi ridotti se l'eval e' girata male
            printf("option name HindsightMargin type spin default 3 min 1 max 12\n");   // [F-006 audit 2026-07-02] 3->4
            printf("option name HindsightRed type check default false\n");     // 5.1 (Stormphrax #300): riduci nodi ridotti se l'eval e' girata bene
            printf("option name HindsightRedMargin type spin default 2 min 1 max 12\n");
            printf("option name HindsightRedThresh type spin default 113 min 0 max 600\n");
            printf("option name DoShallowerMargin type spin default 7 min 0 max 200\n");
            printf("option name BadNoisy type spin default 1 min 0 max 1\n");        // 5.0-B: qsearch move-count pruning catture tardive
            printf("option name BadNoisyCount type spin default 7 min 1 max 32\n");   // REVERT 2026-07-23 (SPSA B1 evaporato)
            printf("option name LMREnrich type spin default 1 min 0 max 1\n");        // 5.0-B (archivio 4.2): +riduzione LMR se TT-move noisy
            printf("option name LMREnrichAmount type spin default 2 min 0 max 4\n");
            printf("option name RazorQuadCoef type spin default 92 min 0 max 100\n");  // quad razor term cp*d^2 (0=linear)
            printf("option name RFPDepth type spin default 7 min 0 max 17\n");      // 0=legacy cap; widen toward SF=17
            printf("option name RazorDepth type spin default 6 min 0 max 18\n");    // 0=legacy cap; widen toward SF (uncapped)
            printf("option name IID type spin default 0 min 0 max 1\n");            // Internal Iterative Deepening (5.1): mini-ricerca per OTTENERE una hash move (ordinamento ~SF); 0=off,1=on (spin per il generic handler atoi)
            printf("option name IIDDepth type spin default 4 min 2 max 12\n");       // profondita' minima per attivare l'IID
            printf("option name IIDReduction type spin default 2 min 1 max 6\n");    // ply tolti alla mini-ricerca
            // ⭐ 5.1 riduzione LMR FINE ×1024 stile-SF (default OFF = byte-identico). Coeff in 1/1024 ply.
            printf("option name LMRFine type spin default 1 min 0 max 1\n");
            printf("option name LMRFCut type spin default 3687 min 0 max 8000\n");      // cut-node forte
            printf("option name LMRFCutNoTT type spin default 2397 min 0 max 4000\n");
            printf("option name LMRFTTCap type spin default 200 min 0 max 4000\n");
            printf("option name LMRFTTPv type spin default 617 min 0 max 8000\n");      // protezione ex-PV
            printf("option name LMRFPv type spin default 437 min 0 max 4000\n");
            printf("option name LMRFSS type spin default 664 min 0 max 2000\n");         // history continua
            printf("option name LMRFCorr type spin default 866 min 0 max 2000\n");       // eval incerta
            printf("option name LMRFAll type spin default 618 min 0 max 1200\n");        // scaling ALL-node
            printf("option name LMRFImprov type spin default 489 min 0 max 3000\n");
            printf("option name LMRFEvalCut type spin default 979 min 0 max 3000\n");
            printf("option name LMRFCutoff type spin default 1520 min 0 max 4000\n");
            printf("option name LMRFCont4 type spin default 0 min 0 max 2000\n");        // conthist 4-ply nella LMR fine: segnale ORFANO perso nel port di LMRFine (0=off, byte-identico)
            printf("option name LMRExpect type spin default 0 min 0 max 2000\n");    // bonus riduzione ad ALL-node con cutoffCnt alto (0=off)
            // ⭐ 5.1 EVAL optimism (SF), default OFF = byte-identico
            printf("option name EvalOptimism type spin default 1 min 0 max 1\n");
            printf("option name EvalOptStrength type spin default 170 min 0 max 600\n");
            printf("option name EvalOptDiv type spin default 272 min 1 max 600\n");
            printf("option name FutilityDepth type spin default 7 min 2 max 16\n");   // gate profondita' futility (cut-SPSA): alzare = pota piu' in profondita'
            printf("option name SEEPruneDepth type spin default 3 min 1 max 18\n");   // gate profondita' SEE (cut-SPSA): alzare = pota piu' in profondita'. min 3->1 il 2026-07-10: il default era INCOLLATO al min dichiarato -> SPSA poteva solo salire (il clamp compilato e' gia' <1->1)
            printf("option name SEELmrDepth type check default false\n");            // S-05: gate/margine SEE su prune_depth (LMR-ridotta) invece che su depth piena
            printf("option name SEELmrQuietMargin type spin default 193 min 0 max 600\n"); // margine quadratico dedicato al percorso SEELmrDepth (disaccoppiato da SEEQuietMargin), SPSA-tunabile
            printf("option name SEELmrPruneCap type spin default 27 min 2 max 40\n");      // cap su prune_depth per il percorso SEELmrDepth (32=praticamente illimitato); abbassare = meno reach ma meno chiamate SEE
            printf("option name SEELmrLinear type check default false\n");                  // S-05 forma lineare: margine -coef*prune_depth invece di -coef*prune_depth^2 (attiva il path da solo)
            printf("option name SEELmrLinearCoef type spin default 368 min 1 max 1200\n");  // coefficiente della forma lineare; alto=pota meno (albero piu' grande)
            printf("option name TTPrefetch type check default true\n");        // Reckless #1085: prefetch bucket TT figlio in make (node-identical, +1.88% NPS confermato su VM N=100)
            printf("option name GoodCapTTQuiet type check default false\n");   // Reckless #1107: TT-move quiet -> catture "good" solo se SEE>=+1
            printf("option name QsStalemateCheck type check default true\n");  // BAKED 2026-07-24 (bundle lean SPRT +10.43); SF d2d046c-style: probe stallo a movegen-completo quando una cattura toglie l'ultima Torre/Donna
            printf("option name CorrUncert type check default true\n");       // P1a: disaccordo tavole corr = incertezza eval -> RFP/futility piu' larghi
            printf("option name CorrUncertRFP type spin default 60 min 0 max 512\n");
            printf("option name CorrUncertFut type spin default 71 min 0 max 512\n");
            printf("option name CorrUncertCap type spin default 70 min 1 max 256\n");
            printf("option name TroubleMaking type check default false\n");    // P4: in pos. persa gioca la mossa piu' "fastidiosa" (verificata)
            printf("option name TroubleScore type spin default 150 min 0 max 1000\n");
            printf("option name TroubleEffort type spin default 60 min 1 max 200\n");
            printf("option name TroubleMargin type spin default 40 min 0 max 400\n");
            printf("option name BrilliantSac type check default false\n");         // REVERT 2026-07-23 (SPSA B1 evaporato @4452g); v1: estende catture SEE<0 con caphist alta
            printf("option name BrilliantSacMargin type spin default 5000 min 0 max 7000\n");  // REVERT 2026-07-23 (SPSA B1 evaporato); caphist soglia; max = HISTORY_MAX
            printf("option name BrilliantSacLMR type check default false\n");       // REVERT 2026-07-23 (SPSA B1 evaporato); B: reduce-less LMR sullo stesso sac
            printf("option name BrilliantSacLMRAmt type spin default 2 min 0 max 4\n");         // plies di riduzione risparmiati
            printf("option name TMMovesToGo type spin default 27 min 12 max 60\n");        // time mgmt: quota base = remaining/questo; BAKE 2026-07-03 TM post-F-003 24->27
            printf("option name TMIncFrac type spin default 94 min 0 max 100\n");           // % incremento; BAKE 2026-07-03 TM post-F-003: invariato
            printf("option name TMMaxMult type spin default 614 min 150 max 800\n");        // burst maximum = optimum*questo/100; BAKE 2026-07-03 TM post-F-003 592->614
            printf("option name TMInstab type spin default 59 min 0 max 100\n");            // % headroom su cambio best-move; BAKE 2026-07-03 TM post-F-003 64->59
            printf("option name TMDropDiv type spin default 508 min 200 max 3000\n");       // estensione su eval che cala; BAKE 2026-07-03 TM post-F-003 570->508
            printf("option name TMStability type check default false\n");                    // T-01: riduci il soft-time se la best move e' stabile da piu' iterazioni (SF)
            printf("option name TMStabThresh type spin default 4 min 0 max 20\n");           // T-01: iterazioni stabili prima che la riduzione inizi
            printf("option name TMStabStep type spin default 40 min 0 max 200\n");           // T-01: riduzione per-mille per ogni iterazione stabile oltre thresh
            printf("option name TMStabFloor type spin default 55 min 10 max 100\n");         // T-01: % minima della durata (mai ridurre sotto)
            printf("option name RootMateRestore type check default true\n");                  // Q-29 (Reckless): non regredire a root su un matto gia' provato  [BAKATA 2026-07-10]
            printf("option name LMRDecisiveBeta type check default true\n");                  // Q-30 (Reckless): r+1 quando beta e' gia' decisivo (matefinding)  [BAKATA]
            printf("option name TTDecisiveClamp type check default true\n");                  // Q-32 (Stormphrax): declassa score decisivi fuori orizzonte-50mr alla probe TT  [BAKATA]
            printf("option name TTSecondaryAge type check default false\n");                  // R-01 (SF 94beadff): decisive non-EXACT depth>=5 invecchiano piu' in fretta nel ranking di rimpiazzo
            printf("option name SEEStalemateGuard type check default true\n");                // S-11 (SF): non SEE-potare il sacrificio dell'ultimo pezzo con alpha<0  [BAKATA]
            printf("option name TBScoreTT type check default true\n");                        // S-10 (SF): scrivi il risultato TB in TT a depth+6 (niente ri-probe)  [BAKATA]
            printf("option name ProbCutImproving type check default true\n");                // Q-10 (Reckless+SF convergenti): verifica ProbCut a depth-1 quando improving
            printf("option name QSCaptHist type check default true\n");                      // Q-17 (Caissa): capture-history aggiornata ai beta-cutoff di qsearch
            printf("option name QSCaptHistScale type spin default 86 min 0 max 400\n");      // Q-17: % del bonus/malus (leva SPSA dedicata)
            printf("option name NodeCache type check default true\n");                       // Q-11 (Caissa): ordering quiet near-root pesato sui nodi spesi (cross-iterazione E cross-move)
            printf("option name NodeCacheBonus type spin default 2080 min 0 max 16384\n");    // Q-11: bonus max (Caissa: 4096, range tune [1000,8000])
            printf("option name NodeCacheMinSum type spin default 1066 min 0 max 8192\n");     // Q-11: nodes_sum minimo prima di applicare il bonus (Caissa: 256)
            printf("option name HistBonusMoves type spin default 32 min 0 max 512\n");         // Q-18 (SF #6871): bonus best-move scalato sulle mosse cercate prima (0=piatto)
            printf("option name HistAge type spin default 940 min 512 max 1024\n");           // Q-25 (Stormphrax 750/1024, Pawnocchio 3/4, Caissa 7/8): decay history tra le mosse (1024=off)
            printf("option name HistInitQuiet type spin default 31 min 0 max 2000\n");          // Q-26 (Caissa 802): prior positivo history quiet (0=zero-init)
            printf("option name HistInitCapt type spin default 249 min 0 max 2000\n");           // Q-26 (Caissa 346): prior positivo capture-history
            printf("option name LMRFPly type spin default 524 min 0 max 2048\n");                // Q-19a (Caissa 1024): LMRFine, riduci meno vicino alla radice (0=off)
            printf("option name LMRFKiller type spin default 797 min 0 max 4000\n");             // Q-19b (Caissa): LMRFine, killer/counter ridotte molto meno (0=off)
            printf("option name RecaptureExt type check default false\n");                    // Q-16 (Caissa): +1 ply alla ttMove ricattura ai nodi PV
            printf("option name RecaptureTension type spin default 70 min 0 max 800\n");      // filtro tensione: estende solo se |SEE(ricattura)| <= questo (cp)
            // ---- TM v2 (quarto audit Q-01..Q-08), tutto default-OFF ----
            printf("option name TMv2 type check default true\n");                            // Q-01 master: composizione moltiplicativa stateless (sostituisce instab+drop+NodeTM quando ON)
            printf("option name TMv2Stab0 type spin default 232 min 100 max 400\n");          // Q-02 (Alexandria 2.38): fattore con best APPENA cambiata
            printf("option name TMv2Stab1 type spin default 172 min 50 max 300\n");           // Q-02: 1 iterazione stabile
            printf("option name TMv2Stab2 type spin default 82 min 50 max 200\n");           // Q-02: 2 iterazioni stabili
            printf("option name TMv2Stab3 type spin default 112 min 30 max 200\n");            // Q-02: 3 iterazioni stabili
            printf("option name TMv2Stab4 type spin default 56 min 20 max 150\n");            // Q-02 (0.71): 4+ stabili -> si RISPARMIA (il ramo che ci mancava)
            printf("option name TMv2Eval0 type spin default 139 min 80 max 250\n");           // Q-03 (Alexandria 1.25): eval instabile -> piu' tempo
            printf("option name TMv2Eval1 type spin default 119 min 60 max 200\n");           // Q-03
            printf("option name TMv2Eval2 type spin default 101 min 50 max 200\n");           // Q-03
            printf("option name TMv2Eval3 type spin default 93 min 40 max 150\n");            // Q-03
            printf("option name TMv2Eval4 type spin default 88 min 30 max 150\n");            // Q-03 (0.87): eval ferma da 4+ iterazioni -> risparmia
            printf("option name TMv2EvalWindow type spin default 10 min 1 max 100\n");        // Q-03: |score-avg| entro questo = "ferma"
            printf("option name TMv2NodeBase type spin default 155 min 100 max 250\n");       // Q-04 (Alexandria 1.52): nodeFactor = (base/100 - frac)*mult/100
            printf("option name TMv2NodeMult type spin default 130 min 50 max 400\n");        // Q-04 (Alexandria 1.74)
            printf("option name TMv2NodeMin type spin default 36 min 10 max 100\n");          // Q-04: clamp inferiore %
            printf("option name TMv2NodeMax type spin default 263 min 100 max 400\n");        // Q-04: clamp superiore % (ramo di ESTENSIONE)
            printf("option name TMv2Alloc type check default true\n");                       // Q-05: allocazione pool-increments (Alexandria) + curva moves-left (Caissa)
            printf("option name TMv2MtgBase type spin default 35 min 10 max 80\n");           // Q-05a: moves-left stimate a inizio partita
            printf("option name TMv2MtgSlope type spin default 67 min 0 max 200\n");          // Q-05a: mtg -= slope*fullmove/100
            printf("option name TMv2MtgMin type spin default 14 min 2 max 40\n");             // Q-05a: pavimento moves-left
            printf("option name TMv2OptPct type spin default 124 min 30 max 300\n");          // Q-05b: optimum = pool/mtg * questo/100
            printf("option name TMv2Predicted type check default true\n");                   // Q-06 (Caissa): fattore cross-move sulla risposta predetta (pv[1])
            printf("option name TMv2PredHit type spin default 945 min 500 max 1000\n");       // Q-06: per-mille su hit (TT calda)
            printf("option name TMv2PredMiss type spin default 1157 min 1000 max 1500\n");    // Q-06: per-mille su miss (sorpresa)
            printf("option name TMv2RootSingular type check default true\n");                // Q-07 (Caissa): stop se la best a root e' "singolare" (verifica excluded a depth/2)
            printf("option name TMv2RSMarginBase type spin default 405 min 100 max 800\n");   // Q-07: margine base (cala con la depth)
            printf("option name TMv2RSMarginMin type spin default 225 min 50 max 500\n");     // Q-07: margine minimo
            printf("option name TMv2RSMarginSlope type spin default 27 min 0 max 100\n");     // Q-07: riduzione margine per ply oltre depth 9
            printf("option name TMv2RSTimeFrac type spin default 10 min 0 max 100\n");        // Q-07: % dell'optimum prima che la verifica si attivi
            printf("option name TMv2RSScoreCap type spin default 2627 min 100 max 20000\n");  // Q-07: |score| sotto cui la verifica ha senso
            printf("option name TMv2SingleReply type check default true\n");                 // Q-08a (Caissa): unica legale a root -> stop alla prima iterazione completata
            printf("option name TMv2MateStop type check default true\n");                    // Q-08b (Caissa): N iterazioni consecutive di score-matto -> stop
            printf("option name TMv2MateIters type spin default 7 min 1 max 30\n");           // Q-08b: quante conferme del matto servono
#endif
            // (SyzygyPath spostata in cima alla lista, subito dopo EvalFile — vedi sopra:
            //  evita il troncamento delle liste lunghe nelle GUI ChessBase/Fritz.)
            printf("uciok\n");
            fflush(stdout);
        }

        // UCI command: "isready"
        else if (strncmp(input, "isready", 7) == 0)
        {
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
        // N-1: lazy mirror apply toggle (default ON). OFF = pre-N1 eager apply, for
        // bisection. Search threads must be idle; safe to send before a search/bench.
        else if (strncmp(input, "lazymirror ", 11) == 0)
        {
            int on = strncmp(input + 11, "on", 2) == 0;
            nn_set_lazy_mirror(on);
            printf("lazymirror %s\n", on ? "on" : "off");
            fflush(stdout);
        }

        // "bench [depth]" — suite fissa di 8 posizioni a profondita' fissa
        // (default 13): node-count CANONICO (la node-identity in un comando) +
        // NPS. Deterministico a Threads=1 (il default). Stato azzerato come
        // ucinewgame prima di ogni posizione.
        else if (strncmp(input, "bench", 5) == 0)
        {
            int bdepth = atoi(input + 5);
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
            unsigned long long prof_wall = 0;
#endif
            for (int bi = 0; bi < 8; bi++) {
                parse_fen((char*)bench_fens[bi]);
                clear_hash_table();
                for (int i = 0; i < num_threads; i++) {
                    memset(thread_data[i].history_moves, 0, sizeof(thread_data[i].history_moves));
                    memset(thread_data[i].capture_history, 0, sizeof(thread_data[i].capture_history));
                    memset(thread_data[i].counter_moves, 0, sizeof(thread_data[i].counter_moves));
                    memset(thread_data[i].continuation_history, 0, sizeof(thread_data[i].continuation_history));
                    memset(thread_data[i].cont_corr_hist, 0, sizeof(thread_data[i].cont_corr_hist));
                    memset(thread_data[i].pawn_history, 0, sizeof(thread_data[i].pawn_history));
                    memset(thread_data[i].lowply_history, 0, sizeof(thread_data[i].lowply_history));
                    memset(thread_data[i].cont_hist_2, 0, sizeof(thread_data[i].cont_hist_2));
                    memset(thread_data[i].cont_hist_3, 0, sizeof(thread_data[i].cont_hist_3));
                    memset(thread_data[i].cont_hist_4, 0, sizeof(thread_data[i].cont_hist_4));
                    memset(thread_data[i].cont_hist_6, 0, sizeof(thread_data[i].cont_hist_6));
                    memset(thread_data[i].corr_hist, 0, sizeof(thread_data[i].corr_hist));
                    memset(thread_data[i].corr_hist_minor, 0, sizeof(thread_data[i].corr_hist_minor));
                    memset(thread_data[i].corr_hist_major, 0, sizeof(thread_data[i].corr_hist_major));
                    memset(thread_data[i].corr_hist_material, 0, sizeof(thread_data[i].corr_hist_material));
                    apply_history_priors(thread_data[i]);   // Q-26: il bench azzera a mano -> riapplica il prior
                }
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
              printf("  movegen         : %5.1f%%\n", 100.0 * (double)prof_mg    / (double)pw);
              printf("  make+unmake     : %5.1f%%\n", 100.0 * (double)prof_make  / (double)pw);
              printf("  tt probe+store  : %5.1f%%\n", 100.0 * (double)prof_tt    / (double)pw);
              printf("  move scoring    : %5.1f%%\n", 100.0 * (double)prof_score / (double)pw);
              printf("  other (search)  : %5.1f%%\n", 100.0 * (double)(pw - acc) / (double)pw); }
#endif
            fflush(stdout);
            parse_fen(start_position);
        }

        // DIAGNOSTIC: "eval" -> static NNUE eval of the current position (cp,
        // side-to-move relative), no search => byte-identical for cross-checks.
        else if (strncmp(input, "eval", 4) == 0)
        {
            printf("eval %d\n", debug_eval_position());
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
                memset(thread_data[i].continuation_history, 0, sizeof(thread_data[i].continuation_history));
                memset(thread_data[i].cont_corr_hist, 0, sizeof(thread_data[i].cont_corr_hist));   // move-context corr table: un pattern di un'altra partita = rumore (come continuation_history)
                memset(thread_data[i].pawn_history, 0, sizeof(thread_data[i].pawn_history));   // mancava: si trascinava tra partite (SF la azzera su ucinewgame)
                memset(thread_data[i].lowply_history, 0, sizeof(thread_data[i].lowply_history));
                // FIX P0.5 (2026-06-09): mancavano le conthist multi-ply e le TRE
                // correction history -> si trascinavano tra partite (stessa classe
                // di contaminazione dello sweep HistBonusMult). SF azzera tutto.
                memset(thread_data[i].cont_hist_2, 0, sizeof(thread_data[i].cont_hist_2));
                memset(thread_data[i].cont_hist_3, 0, sizeof(thread_data[i].cont_hist_3));
                memset(thread_data[i].cont_hist_4, 0, sizeof(thread_data[i].cont_hist_4));
                memset(thread_data[i].cont_hist_6, 0, sizeof(thread_data[i].cont_hist_6));
                memset(thread_data[i].corr_hist, 0, sizeof(thread_data[i].corr_hist));
                memset(thread_data[i].corr_hist_minor, 0, sizeof(thread_data[i].corr_hist_minor));
                memset(thread_data[i].corr_hist_major, 0, sizeof(thread_data[i].corr_hist_major));
                memset(thread_data[i].corr_hist_material, 0, sizeof(thread_data[i].corr_hist_material));
                // Q-11 NodeCache: le entry sopravvivono cross-MOVE (voluto) ma non
                // cross-PARTITA. La chiave piena renderebbe innocuo un residuo, ma i
                // conteggi di un'altra partita sono comunque rumore per l'ordering.
                memset(thread_data[i].node_cache, 0, sizeof(thread_data[i].node_cache));
                thread_data[i].nc_gen = 0;
                apply_history_priors(thread_data[i]);   // Q-26 (no-op se prior = 0)
            }
            g_tm_pred_hash = 0;   // TM v2 Q-06: la predizione non sopravvive alla partita
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
            mb = atoi(input + 26);
            if (mb < 1) mb = 1;
            if (mb > max_hash) mb = max_hash;
            init_hash_table(mb);
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
            if (resolved.empty())
                printf("info string EvalFile: '%s' not found (kept current net)\n", val);
            else if (nn_reload_big(resolved.c_str()))
            {
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

        // ---- Bundle 3.9: toggle A/B (gli spin cadono nel gestore generico) ----
        else if (strncmp(input, "setoption name MateDistPruning value ", 37) == 0)
        {
            const char* v = input + 37;
            set_mate_dist(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name DrawDither value ", 32) == 0)
        {
            const char* v = input + 32;
            set_draw_dither(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name TTCutBonus value ", 32) == 0)
        {
            const char* v = input + 32;
            set_ttcut_bonus(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name TTAgeRefresh value ", 34) == 0)
        {
            const char* v = input + 34;
            set_tt_age_refresh(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name PawnKeyIncr value ", 33) == 0)
        {
            const char* v = input + 33;
            set_pawn_key_incr(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name NPKeyIncr value ", 31) == 0)
        {
            const char* v = input + 31;
            set_np_key_incr(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name TTStaticEval value ", 34) == 0)
        {
            const char* v = input + 34;
            set_tt_static_eval(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name FastRepScan value ", 33) == 0)
        {
            const char* v = input + 33;
            set_fast_rep_scan(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name EvasionGen value ", 32) == 0)
        {
            const char* v = input + 32;
            set_evasion_gen(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name ThreadVoting value ", 34) == 0)
        {
            const char* v = input + 34;
            set_thread_voting(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name QSChecks value ", 30) == 0)
        {
            const char* v = input + 30;
            set_qs_checks(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name NMPVerif value ", 30) == 0)
        {
            const char* v = input + 30;
            set_nmp_verif(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name LMPImproving value ", 34) == 0)
        {
            const char* v = input + 34;
            set_lmp_improving(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        // "Move Overhead" (ms riservati per mossa a lag/GUI; prima hardcoded 50)
        else if (strncmp(input, "setoption name Move Overhead value ", 35) == 0)
        {
            int v = atoi(input + 35);
            g_move_overhead = v < 0 ? 0 : (v > 5000 ? 5000 : v);
        }
        else if (strncmp(input, "setoption name EvalCacheUndamp value ", 37) == 0)
        {
            const char* v = input + 37;
            set_evalcache_undamp(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name ProbCutTT value ", 31) == 0)
        {
            const char* v = input + 31;
            set_probcut_tt(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
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

        // "setoption name FinnyTables value <true|false>" (A/B accumulator refresh cache)
        else if (strncmp(input, "setoption name FinnyTables value ", 33) == 0)
        {
            const char* v = input + 33;
            set_finny(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
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

        else if (strncmp(input, "setoption name MultiCut value ", 30) == 0)
        {
            const char* v = input + 30;
            set_multicut(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        // "setoption name TTPvAmount value N" -> generic spin handler (set_search_param).
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
        // NB: "CaptureHist value " (offset 33) must precede the generic spin handler;
        // "CaptureHistDiv" is longer ('Div' before " value ") so it won't match here and
        // falls through to set_search_param as a spin.
        else if (strncmp(input, "setoption name CaptureHist value ", 33) == 0)
        {
            const char* v = input + 33;
            set_capture_hist(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
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
            // BUG FIX 2026-07-16: cambia tt_ways = larghezza indicizzazione TT; se
            // applicato sotto ricerca (il loop UCI legge stdin durante il search)
            // -> tt_base_index cambia sotto i thread = OOB. Fermare come fa Hash.
            stop_search_threads(); wait_for_search_done();
            const char* v = input + 28;
            set_tt_4way(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name TTTwoLevel value <0|1>" — come TT4Way cambia tt_ways()
        // (1->2) e la larghezza di tt_base_index: togglato sotto ricerca = tt indexing
        // cambia sotto i thread = OOB/torn bucket. Fermare come Hash/TT4Way.
        // (Intercettato QUI, prima del generic spin handler che chiamerebbe
        //  set_search_param senza stop.)
        else if (strncmp(input, "setoption name TTTwoLevel value ", 32) == 0)
        {
            stop_search_threads(); wait_for_search_done();
            const char* vs = input + 32;
            int v;
            if      (strncmp(vs, "true",  4) == 0 || strncmp(vs, "on",  2) == 0) v = 1;
            else if (strncmp(vs, "false", 5) == 0 || strncmp(vs, "off", 3) == 0) v = 0;
            else v = atoi(vs);
            set_search_param("TTTwoLevel", v);
        }

        // "setoption name TTEvalImprove value <true|false>" (P1.1: tt_score come eval)
        else if (strncmp(input, "setoption name TTEvalImprove value ", 35) == 0)
        {
            const char* v = input + 35;
            set_tt_eval_improve(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name UpcomingRep value <true|false>" (P1.2: cuckoo anti-shuffling)
        else if (strncmp(input, "setoption name UpcomingRep value ", 33) == 0)
        {
            const char* v = input + 33;
            set_upcoming_rep(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // Toggle di ablazione dei fix 3.8 (night tournament; OFF = comportamento 3.7)
        else if (strncmp(input, "setoption name TTMove24 value ", 30) == 0)
        {
            const char* v = input + 30;
            set_ttmove24(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name SeeFix value ", 28) == 0)
        {
            const char* v = input + 28;
            set_see_fix(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name KillerLMRFix value ", 34) == 0)
        {
            const char* v = input + 34;
            set_killer_lmr_fix(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name QsearchCorr value ", 33) == 0)
        {
            const char* v = input + 33;
            set_qsearch_corr(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name ImprovingFix value ", 34) == 0)
        {
            const char* v = input + 34;
            set_improving_fix(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name CorrHistMulti value <true|false>" (A/B multi-table corrhist)
        else if (strncmp(input, "setoption name CorrHistMulti value ", 35) == 0)
        {
            const char* v = input + 35;
            set_corr_multi(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // "setoption name PawnHistory value <true|false>" (A/B ordering pawn-structure)
        else if (strncmp(input, "setoption name PawnHistory value ", 33) == 0)
        {
            const char* v = input + 33;
            set_pawn_hist(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
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

        // StatScore-LMR levers (fix sotto-riduzione vs SF15.1), A/B uno alla volta.
        else if (strncmp(input, "setoption name StatScoreLMR value ", 34) == 0)
        {
            const char* v = input + 34;
            set_statscore_lmr(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name ContHistLMR value ", 33) == 0)
        {
            const char* v = input + 33;
            set_conthist_lmr(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name CutNodeLMR value ", 32) == 0)
        {
            const char* v = input + 32;
            set_cutnode_lmr(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }

        // ThreatOrdering (#2 SF): A/B toggle. Lo spin "ThreatScale" cade nel gestore
        // generico (set_search_param), non qui (26esimo char 'S' != ' ' di "value").
        else if (strncmp(input, "setoption name ThreatOrdering value ", 36) == 0)
        {
            const char* v = input + 36;
            set_threat_ordering(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        // ThreatHist (5.1): A/B toggle. Lo spin "ThreatHistWeight" cade nel gestore generico.
        else if (strncmp(input, "setoption name ThreatHist value ", 32) == 0)
        {
            const char* v = input + 32;
            set_threat_hist(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        // CheckOrdering (#3 SF): A/B toggle. Lo spin "CheckBonus" cade nel gestore generico.
        else if (strncmp(input, "setoption name CheckOrdering value ", 35) == 0)
        {
            const char* v = input + 35;
            set_check_ordering(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        // ContHist36 (#4 SF): A/B toggle. Lo spin "ContHist36Weight" cade nel gestore generico.
        else if (strncmp(input, "setoption name ContHist36 value ", 32) == 0)
        {
            const char* v = input + 32;
            set_conthist36(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        // CorrHistCont (continuation correction history, SF): A/B toggle. Lo spin
        // "CorrContWeight" cade nel gestore generico (nome diverso, nessuna collisione).
        else if (strncmp(input, "setoption name CorrHistCont value ", 34) == 0)
        {
            const char* v = input + 34;
            set_corr_cont(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        // CorrNonPawn (corrhist non-pedoni per-lato, port Pawnocchio/SF): A/B toggle.
        // Lo spin "CorrNonPawnWeight" cade nel gestore generico (nome diverso).
        else if (strncmp(input, "setoption name CorrNonPawn value ", 33) == 0)
        {
            const char* v = input + 33;
            set_corr_nonpawn(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        // V2 PriorBonus + #5 LowPlyHistory: A/B toggle. Gli spin (PriorBonusScale/LowPlyWeight)
        // cadono nel gestore generico (25esimo/22esimo char != ' ' di " value ").
        else if (strncmp(input, "setoption name PriorBonus value ", 32) == 0)
        {
            const char* v = input + 32;
            set_prior_bonus(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
        }
        else if (strncmp(input, "setoption name LowPlyHistory value ", 35) == 0)
        {
            const char* v = input + 35;
            set_lowply(strncmp(v, "true", 4) == 0 || strncmp(v, "on", 2) == 0 || v[0] == '1');
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

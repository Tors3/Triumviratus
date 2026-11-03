// search/02_state.inc -- Global search state: threads, stop, limits, output, game log.
// Part of threads.cpp: it is NOT a translation unit of its own.

std::vector<std::thread> search_threads;
ThreadDataVec            thread_data;   // su large pages (V1, threads.h)
std::atomic<bool>        stop_threads(false);
std::mutex               output_mutex;
int                      num_threads = 1;
bool                     g_numa_spread = false;
SearchLimits             g_limits;

// Search of draws/wins only: values and thresholds. Mates lie in (mate_score, mate_value], tablebase wins
// in [TB_WIN_FLOOR, TB_WIN_TOP] (syzygy.cpp: 30000 - max_ply - ply).
constexpr int VAL_NONE     = 32001;
constexpr int TB_WIN_TOP   = mate_score - max_ply;      // 29872
constexpr int TB_WIN_FLOOR = TB_WIN_TOP - max_ply;      // 29744: from here up the score is "decided"
constexpr int MATE_FLOOR   = mate_value - max_ply;      // 30872: from here up it is a mate
constexpr int NULL_MOVE    = -1;                         // null move in the frames (0 = no move)

static inline bool val_known(int v) { return v != VAL_NONE; }
static inline bool val_won(int v) { return v >= TB_WIN_FLOOR; }
static inline bool val_lost(int v) { return v <= -TB_WIN_FLOOR; }
static inline bool val_decided(int v) { return val_won(v) || val_lost(v); }
static inline bool val_mating(int v) { return v >= MATE_FLOOR || v <= -MATE_FLOOR; }
static inline int  mating_in(int ply) { return mate_value - ply; }
static inline int  mated_at(int ply) { return -mate_value + ply; }

// Stop requested from outside (stop / quit command), distinct from the search's internal stops.
static std::atomic<bool> g_uci_stop{false};
// Ponder: while it is true the time does not stop the search and the bestmove waits for ponderhit or stop.
static std::atomic<bool> g_pondering{false};
static std::atomic<bool> g_stop_at_ponderhit{false};
// "go searchmoves": definite in defs.h, riempite da parse_go.
int  g_searchmoves[256];
int  g_searchmoves_count = 0;
// TbDrawSearch (28/10/2026): the current search chooses among the moves that keep a tablebase draw at the root, with the
// in-search tablebase probe off (15_threads.inc launch_search). Set before the threads start, read-only during search.
static bool g_tb_root_draw_search = false;

// Anti-forfeit floor: the time cut waits until thread 0 has searched at least one root move.
static inline bool root_move_ready() {
  return !thread_data.empty() && (thread_data[0].completed_depth > 0 || thread_data[0].root[0].score != -infinity);
}

// ---------------------------------------------------------------------------
// Game log (DataLog): one line "FEN<TAB>move<TAB>score<TAB>depth" per search.
// One file per process (".<pid>"), so several instances in parallel do not overlap.
// ---------------------------------------------------------------------------
void set_data_log_file(const char *path) {
  if (path && *path)
    g_data_log_file = path;
}

static void log_search_record(int best_move, int score, int depth) {
#ifdef _WIN32
  unsigned long tri_pid = (unsigned long)GetCurrentProcessId();
#else
  unsigned long tri_pid = (unsigned long)getpid();
#endif
  std::string path = g_data_log_file + "." + std::to_string(tri_pid);
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out)
    return;
  out << board_to_fen() << '\t' << move_to_uci(best_move) << '\t' << score << '\t' << depth << '\n';
}

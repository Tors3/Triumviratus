// search/15_threads.inc -- Starting and stopping the search, root moves, choice of the thread, bestmove.
// Part of threads.cpp: NOT a translation unit of its own.

#include <condition_variable>
#include <functional>
#ifndef _WIN32
#include <pthread.h>
#endif

// Stack of the search threads (05/11/2026, issue #9). The search recurses once per ply up to max_ply = 128, plus the
// frames re-entered at the same ply (singular search, null-move verification). Measured with -fstack-usage (GCC 13,
// x86-64): explore 688 B + move_loop 4400 B = ~5.1 KB per ply of the main search, quiesce 4208 B per ply of the
// quiescence search, plus ~7 KB of leaf helpers (fill_quiets, rank_into_v). Worst case at max_ply with two frames per
// ply: ~1.3 MB, and other compilers or ABIs (Apple clang, arm64) can be larger. Default thread stacks are 512 KB for
// secondary threads on macOS and 128 KB on musl, and on Windows the reserve written in the exe header (1 MB unless
// the linker sets it), so a deep search on a helper thread crashed on macOS arm64 with Threads=2 (EXC_BAD_ACCESS in
// the stack guard page at depth 22, seldepth 53). The threads are now created with an explicit stack of 8 MB on every
// platform: only address space is reserved, the pages are committed when touched, so a shallow search costs nothing
// more. The search never runs on the main (UCI) thread: "go" and "bench" both go through the master worker below.
constexpr size_t SEARCH_STACK_BYTES = (size_t)8 * 1024 * 1024;

// Starts body(arg) on a new detached thread with a stack of SEARCH_STACK_BYTES. Windows: CreateThread with the size as
// a reservation (the default would commit it all). POSIX: pthread attributes; if the system refuses the size, the
// thread is created with the default stack rather than not at all. A thread that cannot be created stops the program,
// as std::thread did by throwing.
struct SearchThreadStart {
  void (*body)(void *);
  void *arg;
};
#ifdef _WIN32
static DWORD WINAPI search_thread_entry(LPVOID p) {
#else
static void *search_thread_entry(void *p) {
#endif
  const SearchThreadStart s = *static_cast<SearchThreadStart *>(p);
  delete static_cast<SearchThreadStart *>(p);
  s.body(s.arg);
  return 0;
}

static void start_search_thread(void (*body)(void *), void *arg) {
  SearchThreadStart *s = new SearchThreadStart{body, arg};
#ifdef _WIN32
  HANDLE h = CreateThread(nullptr, SEARCH_STACK_BYTES, search_thread_entry, s, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!h) {
    fprintf(stderr, "info string error: cannot create a search thread (CreateThread %lu)\n", (unsigned long)GetLastError());
    std::abort();
  }
  CloseHandle(h);   // detached: the handle is not needed
#else
  pthread_attr_t at;
  pthread_t t;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int rc = pthread_attr_setstacksize(&at, SEARCH_STACK_BYTES);
  if (rc == 0)
    rc = pthread_create(&t, &at, search_thread_entry, s);
  pthread_attr_destroy(&at);
  if (rc != 0) {   // size refused: default stack, as before this change
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&t, &at, search_thread_entry, s);
    pthread_attr_destroy(&at);
  }
  if (rc != 0) {
    fprintf(stderr, "info string error: cannot create a search thread (pthread_create %d)\n", rc);
    std::abort();
  }
#endif
}

// Persistent threads (12/10/2026). Before, every "go" created a thread for the main search and one per helper and
// joined them at the end: 1-2 ms per move with 4-8 threads (costi_infrastruttura.py). Now each thread is created once
// and waits for work on a condition variable. The objects are never destroyed (the thread is detached): at process exit
// the system closes the waiting threads.
struct SearchWorker {
  std::mutex m;
  std::condition_variable cv;
  std::function<void()> job;
  bool busy = false;
  // Started by start_search_thread (explicit stack size, issue #9) instead of std::thread.
  SearchWorker() { start_search_thread([](void *w) { static_cast<SearchWorker *>(w)->loop(); }, this); }
  void loop() {
    for (;;) {
      std::function<void()> j;
      {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [this] { return (bool)job; });
        j = std::move(job);
        job = nullptr;
      }
      j();
      {
        std::lock_guard<std::mutex> lk(m);
        busy = false;
      }
      cv.notify_all();
    }
  }
  void run(std::function<void()> f) {
    {
      std::lock_guard<std::mutex> lk(m);
      busy = true;
      job = std::move(f);
    }
    cv.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [this] { return !busy; });
  }
};
static SearchWorker *g_master_worker = nullptr;
static std::vector<SearchWorker *> g_helper_workers;   // helper i = g_helper_workers[i - 1]

// Waiting for stop or ponderhit (12/10/2026): before, a loop of 1 ms sleeps, which on Windows last up to 15.6 ms (the
// timer granularity), so the bestmove after a ponderhit could leave ~15 ms late. Now a condition variable, woken by
// stop_search_threads and ponder_hit. Whoever changes the flags takes the mutex before notifying, so the waiter has
// either not yet checked the predicate or is already asleep, and the wake-up is never lost.
static std::mutex g_release_m;
static std::condition_variable g_release_cv;
static void notify_release() {
  { std::lock_guard<std::mutex> lk(g_release_m); }
  g_release_cv.notify_all();
}
static void wait_release() {
  std::unique_lock<std::mutex> lk(g_release_m);
  g_release_cv.wait(lk, [] {
    return !((g_limits.infinite || g_pondering.load(std::memory_order_acquire)) &&
             !g_uci_stop.load(std::memory_order_relaxed));
  });
}

void stop_search_threads() {
  g_uci_stop.store(true, std::memory_order_relaxed);
  stop_threads.store(true, std::memory_order_relaxed);
  notify_release();
}

void wait_for_threads() {
  for (int i = 1; i < num_threads && i <= (int)g_helper_workers.size(); i++)
    g_helper_workers[i - 1]->wait();
}

std::thread search_master;   // no longer used (persistent threads, above); kept for threads.h

void wait_for_search_done() {
  if (g_master_worker)
    g_master_worker->wait();
}

// The search runs on a thread of its own, so the UCI loop stays free for stop, ponderhit and isready.
void launch_search(int depth) {
  stop_search_threads();
  wait_for_search_done();
  g_limits.depth = depth;

  // Root position covered by the tablebases (DTZ): the perfect move is played at once.
  // TbDrawSearch (28/10/2026, ours, OFF; evaluation-ideas agent, STATO §0.2 T8): when the root is a tablebase draw and
  // more than one move keeps it, the normal search runs among those moves only (as "go searchmoves") with the in-search
  // tablebase probe off for that search, so the network can choose the draw that leaves the opponent most chances to
  // go wrong, instead of the DTZ move (every draw has DTZ 0). 0: the DTZ move at once, as before.
  g_tb_root_draw_search = false;
  if (!g_limits.infinite && !g_limits.ponder) {
    int tb_move = 0, tb_score = 0, keep[256], n_keep = 0;
    const bool want_keep = TbDrawSearch && !g_searchmoves_count;
    const bool probed = syzygy_probe_root(tb_move, tb_score, want_keep ? keep : nullptr, want_keep ? &n_keep : nullptr);
    if (probed && want_keep && tb_score == 0 && n_keep > 1) {
      for (int i = 0; i < n_keep; i++)
        g_searchmoves[i] = keep[i];
      g_searchmoves_count = n_keep;
      g_tb_root_draw_search = true;
    } else if (probed) {
      new_search();
      std::lock_guard<std::mutex> lock(output_mutex);
      char sc[32];   // the internal tablebase value through print_score (it was printed raw: cp 29872)
      print_score(sc, sizeof sc, tb_score);
      printf("info depth 1 score %s tbhits 1 pv ", sc);
      print_move(tb_move);
      printf("\nbestmove ");
      print_move(tb_move);
      printf("\n");
      fflush(stdout);
      return;
    }
  }

  stop_threads.store(false, std::memory_order_relaxed);
  g_uci_stop.store(false, std::memory_order_relaxed);
  g_stop_at_ponderhit.store(false, std::memory_order_relaxed);
  g_pondering.store(g_limits.ponder, std::memory_order_relaxed);
  if (!g_master_worker)
    g_master_worker = new SearchWorker();
  g_master_worker->run([depth] { search_position_mt(depth); });
}

// The opponent played the expected move: the search becomes a normal one. If the time is already over, it moves.
void ponder_hit() {
  if (!g_pondering.load(std::memory_order_acquire))
    return;
  g_pondering.store(false, std::memory_order_release);
  if (g_stop_at_ponderhit.load(std::memory_order_relaxed))
    stop_threads.store(true, std::memory_order_relaxed);
  notify_release();
}

// Legal moves of the root (restricted to "go searchmoves" when given).
static int collect_root_moves(ThreadData &td, int out[256]) {
  moves ml[1];
  td_generate_moves(td, ml, false);
  int n = 0;
  for (int i = 0; i < ml->count; i++) {
    const int m = ml->moves[i];
    if (g_searchmoves_count) {
      bool listed = false;
      for (int j = 0; j < g_searchmoves_count; j++)
        if (g_searchmoves[j] == m) { listed = true; break; }
      if (!listed)
        continue;
    }
    UndoInfo u;
    td.repetition_table[++td.repetition_index] = td.hash_key;
    td.ply++;
    if (td_make_move(td, m, u)) {
      td_unmake_move(td, m, u);
      out[n++] = m;
    }
    td.ply--;
    td.repetition_index--;
  }
  return n;
}

static void reset_root_lines(ThreadData &td, const int *list, int n) {
  td.root_count = n;
  for (int i = 0; i < n; i++) {
    RootLine &rl = td.root[i];
    rl.pv[0] = list[i];
    rl.pv_len = 1;
    rl.prev_pv_len = 0;
    rl.score = rl.prev_score = rl.avg_score = rl.uci_score = -infinity;
    rl.mean_sq = -(long long)infinity * infinity;
    rl.lower = rl.upper = rl.prev_exact = false;
    rl.sel_depth = 0;
    rl.effort = 0;
  }
}

// NumaSpread (03/11/2026, 03_tables.inc): at the start of each search the thread of id `id` (the main thread too)
// runs on node id % nodes, its network handle is moved to that node once, and it reads the network copy of the node.
// The threads keep their node from one search to the next; nothing is done with one node.
static void numa_bind_thread(int id) {
  const int nodes = numa_search_nodes();
  if (nodes < 2) {
    nn_bind_node_net(-1);   // the copy of the load (the option was switched off)
    return;
  }
  static thread_local int bound = -1;
  ThreadData &td = thread_data[id];
  const int node = id % nodes;
  if (bound != node) {
    numa_bind_cpu(node);
    bound = node;
  }
  if (td.nn_home != node) {
    nn_pos_rehome(td.nnpos, node);
    td.nnstack = nn_pos_stack(td.nnpos);
    td.nn_home = node;
  }
  nn_bind_node_net(node);
}

static void thread_main(int id) {
  numa_bind_thread(id);
  deepen(thread_data[id]);
}

// Choice of the thread that gives the bestmove (block 5 of 8.1). Each thread gives its own move a share:
//   (score - lowest score among the threads + SmpVoteBase) x SmpVoteScore / 1024 + SmpVoteDepth x completed depth.
// The support of a move is the sum of the shares of the threads that chose it, computed once per move. A decided and
// complete result (mate or tablebase score, without a bound) wins; equal support goes to the longer line.
// Origin: a weighted vote among threads is a common lazy-SMP idea. Ours: the additive depth term and the parameters.
// Defaults (14, 1024, 0) give the old choice. Code map: docs/conceptual_maps/BLOCK_5_CODE_MAP.md.
static ThreadData *pick_thread() {
  ThreadData *best = &thread_data[0];
  if (num_threads == 1)
    return best;
  const int nt = (int)thread_data.size();
  // SmpRouteShift with SmpRouteVote 0: the odd helpers searched with shifted experts; they neither vote nor can be
  // chosen (always true without TRIUMV_C2 or with the shift off, so the choice is the one of before).
  auto voter = [](int i) { return !(SmpRouteShift && !SmpRouteVote && (i & 1)); };
  int floor_score = infinity;
  for (int i = 0; i < nt; i++)
    if (voter(i))
      floor_score = std::min(floor_score, thread_data[i].root[0].score);
  std::vector<long long> share(nt), support(nt, 0);
  for (int i = 0; i < nt; i++) {
    const ThreadData &t = thread_data[i];
    share[i] = voter(i) ? (long long)(t.root[0].score - floor_score + SmpVoteBase) * SmpVoteScore / 1024 +
                              (long long)SmpVoteDepth * t.completed_depth
                        : 0;
  }
  for (int i = 0; i < nt; i++)
    for (int j = 0; j < nt; j++)
      if (thread_data[j].root[0].pv[0] == thread_data[i].root[0].pv[0])
        support[i] += share[j];
  // Order of preference between the line of thread i and that of the current choice (rewritten 27/10/2026 as one
  // comparison, same choices): a complete decided result beats everything, and between two of them the larger one
  // (the shorter mate); a lost score never replaces anything; otherwise more support wins, then the longer line.
  auto decided = [](const RootLine &l) { return l.score != -infinity && val_decided(l.score) && !l.lower && !l.upper; };
  auto prefer = [&](int i, int cur) {
    const RootLine &cand = thread_data[i].root[0], &held = thread_data[cur].root[0];
    if (decided(held))
      return decided(cand) && std::abs(cand.score) > std::abs(held.score);
    if (decided(cand))
      return true;
    if (val_lost(cand.score))
      return false;
    if (support[i] != support[cur])
      return support[i] > support[cur];
    return cand.pv_len > held.pv_len;
  };
  int chosen = 0;
  for (int i = 0; i < nt; i++)
    if (voter(i) && prefer(i, chosen))
      chosen = i;
  best = &thread_data[chosen];
  return best;
}

// Expected reply (TmExpect*, 05/10/2026): key of the position after our move and the reply of the main line, on
// thread 0's board (at the root, after the search). 0 if either move is not legal.
static U64 expected_key(ThreadData &td, int first, int reply) {
  UndoInfo u, u2;
  U64 key = 0;
  td.repetition_table[++td.repetition_index] = td.hash_key;
  td.ply++;
  if (td_make_move(td, first, u)) {
    if (td_is_pseudo_legal(td, reply)) {
      td.repetition_table[++td.repetition_index] = td.hash_key;
      td.ply++;
      if (td_make_move(td, reply, u2)) {
        key = td.hash_key;
        td_unmake_move(td, reply, u2);
      }
      td.ply--;
      td.repetition_index--;
    }
    td_unmake_move(td, first, u);
  }
  td.ply--;
  td.repetition_index--;
  return key;
}

// Without a second move in the line, the expected reply is looked up in the hash.
static int reply_from_hash(ThreadData &td, int first) {
  UndoInfo u;
  int reply = 0;
  td.repetition_table[++td.repetition_index] = td.hash_key;
  td.ply++;
  if (td_make_move(td, first, u)) {
    int m = get_tt_move(tt_key(td));
    if (m && td_is_pseudo_legal(td, m)) {
      UndoInfo u2;
      td.repetition_table[++td.repetition_index] = td.hash_key;
      td.ply++;
      if (td_make_move(td, m, u2)) {
        td_unmake_move(td, m, u2);
        reply = m;
      }
      td.ply--;
      td.repetition_index--;
    }
    td_unmake_move(td, first, u);
  }
  td.ply--;
  td.repetition_index--;
  return reply;
}

void search_position_mt(int depth) {
  (void)depth;
  g_limits.start_ms = starttime;
  build_reduction_table();
  new_search();
  stopped = 0;
  g_poll_countdown = 0;
  g_clock.increase_depth = true;

  int list[256];
  copy_board_to_thread(thread_data[0]);
  // (26/10/2026) ExpertFix, the root's expert kept for a margin of pieces in the Consilium network, removed with that
  // network (32 = -27.7 +- 18.2 over 352 games at 10+0.1 HT); its Consilium Plebis form C2HoldRoot removed on 27/10 (H0).
  c2_eval_push();   // C2 evaluation terms (agent B, 21/10/2026; 13_search.inc): all 0 = evaluation unchanged
  if (SmpRouteShift)   // SmpRouteShift: the shifted routing table, before the helpers start (deepen chooses it)
    nn_router_alt_build(SmpRouteShift);
  // Expected reply (TmExpect*): are we in the position expected at the end of the previous search?
  g_clock.expected = g_clock.expect_key && thread_data[0].hash_key == g_clock.expect_key;
  g_clock.surprise = g_clock.expect_key && !g_clock.expected;   // TmSurpriseScale
  const int n = collect_root_moves(thread_data[0], list);
  {
    extern int g_move_overhead_ms;
    // TmFenPly (05/10/2026): game ply = move number of the FEN + moves played. At 0 only the moves played after the
    // given position count: from an .epd book the game "started" at ply 0.
    clock_plan(side, repetition_index + (TmFenPly ? g_fen_ply : 0), g_move_overhead_ms);
  }

  if (n == 0) {
    const bool mated = thread_data[0].frames[FRAME_OFFSET].in_check ||
                       td_is_square_attacked(thread_data[0],
                                             get_ls1b_index(thread_data[0].bitboards[side == white ? K : k]), side ^ 1);
    wait_release();
    std::lock_guard<std::mutex> lock(output_mutex);
    printf("info depth 0 score %s\nbestmove (none)\n", mated ? "mate 0" : "cp 0");
    fflush(stdout);
    return;
  }

  for (int i = 0; i < num_threads; i++) {
    ThreadData &td = thread_data[i];
    if (i)
      copy_board_to_thread(td);
    td.nodes = td.tb_hits = td.best_move_changes = 0;
    td.nmp_min_ply = 0;
    td.root_depth = td.completed_depth = 0;
    td.pv_idx = 0;
    td.sel_depth = 0;
    reset_root_lines(td, list, n);
  }

  while ((int)g_helper_workers.size() < num_threads - 1)
    g_helper_workers.push_back(new SearchWorker());
  for (int i = 1; i < num_threads; i++)
    g_helper_workers[i - 1]->run([i] { thread_main(i); });
  if (num_threads > 1)
    numa_bind_thread(0);
  deepen(thread_data[0]);

  // "go infinite" and ponder: the bestmove waits for stop or ponderhit.
  wait_release();
  g_pondering.store(false, std::memory_order_relaxed);
  stop_threads.store(true, std::memory_order_relaxed);
  wait_for_threads();

  ThreadData *bt = g_limits.depth ? &thread_data[0] : pick_thread();
  RootLine &line = bt->root[0];
  opp_judge(line.score);   // ContemptOpp: before the new score replaces the previous one
  g_clock.prev_best = line.score;
  g_clock.prev_avg = line.avg_score;
  g_clock.have_prev = true;
  if (bt != &thread_data[0])
    report_lines(*bt, std::max(1, bt->completed_depth));

  const int best_move = line.pv[0];
  int reply = line.pv_len > 1 ? line.pv[1] : 0;
  if (!reply)
    reply = reply_from_hash(*bt, best_move);

  // Expected reply (TmExpect*): it needs a line of at least three moves (ours, the reply, our next). Computed only with
  // the option on, so nothing changes with it off.
  g_clock.expect_key = 0;
  if ((TmExpectScale < 1000 || TmSurpriseScale > 1000) && line.pv_len >= 3) {
    g_clock.expect_key = expected_key(thread_data[0], best_move, line.pv[1]);
    g_clock.expect_move = line.pv[2];
    g_clock.expect_score = line.score;
    g_clock.expect_depth = bt->completed_depth;
  }
  thread_data[0].best_move = best_move;
  thread_data[0].best_score = line.score;
  thread_data[0].best_reply = reply;
  thread_data[0].depth = bt->completed_depth;

  if (g_data_log_enabled && best_move)
    log_search_record(best_move, line.score, bt->completed_depth);

  std::lock_guard<std::mutex> lock(output_mutex);
  printf("bestmove ");
  print_move(best_move);
  if (reply) {
    printf(" ponder ");
    print_move(reply);
  }
  printf("\n");
  fflush(stdout);
}

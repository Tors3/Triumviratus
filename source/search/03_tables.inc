// search/03_tables.inc -- Tabelle della ricerca: riduzioni, statistiche condivise fra i thread, preparazione dei
// thread e della loro scacchiera.
// Parte di threads.cpp: NON e' un'unita' di compilazione a se'.

// ---------------------------------------------------------------------------
// Riduzioni tardive: red_log[i] = LmrLogMul/128 * ln(i), ricalcolata a ogni ricerca (LmrLogMul e' un'opzione).
// ---------------------------------------------------------------------------
static int red_log[256];

static void build_reduction_table() {
  red_log[0] = 0;
  for (int i = 1; i < 256; i++)
    red_log[i] = int(LmrLogMul / 128.0 * std::log(double(i)));
}

// ---------------------------------------------------------------------------
// Chiave della tabella hash (nostra, 05/10/2026). Con HashFiftyStep > 0 la chiave porta anche la fascia del
// contatore delle 50 mosse: una fascia ogni HashFiftyStep semimosse sopra HashFiftyFrom (fino a 15 fasce). Una
// posizione raggiunta a 70 semimosse dall'ultima cattura non e' la stessa posizione raggiunta a 10: il limite
// salvato in hash e' stato cercato con un'altra distanza dalla patta e la rete la valuta smorzata. Sotto From la
// chiave e' quella di sempre: apertura e mediogioco non perdono trasposizioni. La chiave della scacchiera (td.hash_key)
// resta intatta per le ripetizioni e per la cache delle valutazioni (che salva l'eval senza smorzamento).
// A HashFiftyStep = 0 la funzione restituisce td.hash_key: albero identico, nella release il ramo sparisce.
// ---------------------------------------------------------------------------
static constexpr U64 fifty_hash_keys[16] = {
    0, 0x2B4AE6736F4FC7BEULL, 0xEAD209722A16E0CBULL, 0x89AE5F87DF9B4018ULL, 0x8B26E98280A48722ULL,
    0x08BD40C8D758D0F7ULL, 0xF64A5A01895149D9ULL, 0x445D6E85630EE7C1ULL, 0x91423FEEA7C93B58ULL,
    0xAD3FBFD5C23F9759ULL, 0xD8DD9840329B5C29ULL, 0x7CB97C9AE2AC9416ULL, 0x1934510935C788F3ULL,
    0xDC22B41127891257ULL, 0xA2FE2D5BA8CB11FBULL, 0x24A5B78779ED9899ULL};

static inline U64 tt_key(const ThreadData &td) {
  if (!HashFiftyStep || td.fifty <= HashFiftyFrom)
    return td.hash_key;
  return td.hash_key ^ fifty_hash_keys[std::min(1 + (td.fifty - HashFiftyFrom - 1) / HashFiftyStep, 15)];
}

// ---------------------------------------------------------------------------
// Statistiche condivise fra i thread. Le tabelle di seguito (continuation), di struttura pedonale e di correzione
// sono una sola per tutto il motore e crescono col numero di thread: ogni thread porta la sua fetta di spazio e
// tutti imparano nella stessa tabella. Le scritture concorrenti su int16 sono innocue per una statistica.
// ---------------------------------------------------------------------------
struct CorrSlot {           // le correzioni lette con chiavi diverse ma nello stesso array
  int16_t pawn, minor, np_white, np_black;
  // 06/10/2026: tolto il campo `major` di CorrMajorW (idea chiusa: -2,1 +- 20,7). Slot da 10 a 8 byte: riga da 16
  // byte, quattro righe per linea di cache, tabella da 1,31 a 1,05 MB per thread (rapporto N_CORREZIONI 4.6).
  // Misura xperf a macchina quieta (06/10/2026, PGO release, 6 giri): cicli/nodo mediogioco +0,00%, finali +0,26%:
  // neutro in velocita'. Tenuto perche' toglie un campo morto.
};

struct SharedStats {
  // follow[in_check][capture][pezzo][casa] -> riga pezzo x casa della mossa successiva. Pezzo 12 = sentinella.
  PieceToRow (*follow)[2][NO_PC + 1][64] = nullptr;
  PieceToRow* pawn_rows = nullptr;      // [indice struttura pedonale]
  CorrSlot  (*corr)[2] = nullptr;       // [indice chiave][lato al tratto]
  int16_t   (*corr_x)[2] = nullptr;     // CorrExpertW: pedoni per esperto, in aggiunta alla condivisa [indice][lato]
  U64 pawn_mask = 0;
  U64 corr_mask = 0;
  int slots = 0;                        // potenza di due >= numero di thread
};
static SharedStats g_stats;

constexpr int PAWN_ROWS_PER_SLOT = 8192;
constexpr int CORR_ROWS_PER_SLOT = 65536;

// V1 (08/10/2026, velocita', albero identico): le tabelle condivise su large pages, come TT e rete. Con un thread sono
// ~21 MB (follow 5,5 MB, pawn_rows 13,6 MB, corr 1 MB, corr_x 256 KB) letti a caso a ogni nodo (5 righe di
// continuazione per quieta, 4 correzioni): su pagine da 4 KB sono ~5.200 pagine contro ~1.500 voci dello STLB di
// questo Xeon, su pagine da 2 MB 11. Profilo del 05/10: td_prefetch_child 2,9% e corr_signal 2,0% dei cicli, entrambi
// costi di memoria. Senza il privilegio SeLockMemoryPrivilege aligned_large_pages_alloc ripiega su VirtualAlloc
// normale (Windows) o su allocazione allineata + madvise (Linux). La memoria torna azzerata, poi la riempie
// shared_stats_reset come prima.
template <typename T>
static T *lp_alloc(size_t bytes) {
  return static_cast<T *>(Triumviratus::aligned_large_pages_alloc(bytes));
}

static void shared_stats_alloc(int threads) {
  int slots = 1;
  while (slots < threads)
    slots <<= 1;
  if (slots == g_stats.slots)
    return;
  Triumviratus::aligned_large_pages_free(g_stats.follow);
  Triumviratus::aligned_large_pages_free(g_stats.pawn_rows);
  Triumviratus::aligned_large_pages_free(g_stats.corr);
  Triumviratus::aligned_large_pages_free(g_stats.corr_x);
  g_stats.follow    = lp_alloc<PieceToRow[2][NO_PC + 1][64]>(sizeof(PieceToRow[2][2][NO_PC + 1][64]));
  g_stats.pawn_rows = lp_alloc<PieceToRow>(sizeof(PieceToRow) * PAWN_ROWS_PER_SLOT * slots);
  g_stats.corr      = lp_alloc<CorrSlot[2]>(sizeof(CorrSlot[2]) * CORR_ROWS_PER_SLOT * slots);
  g_stats.corr_x    = lp_alloc<int16_t[2]>(sizeof(int16_t[2]) * CORR_ROWS_PER_SLOT * slots);   // 256 KB per slot
  g_stats.pawn_mask = (U64)PAWN_ROWS_PER_SLOT * slots - 1;
  g_stats.corr_mask = (U64)CORR_ROWS_PER_SLOT * slots - 1;
  g_stats.slots     = slots;
}

template <typename T>
static inline void fill_with(T *first, size_t count, int16_t value) {
  int16_t *p = reinterpret_cast<int16_t *>(first);
  std::fill(p, p + count * (sizeof(T) / sizeof(int16_t)), value);
}

static void shared_stats_reset() {
  fill_with(&g_stats.follow[0][0][0][0], (size_t)2 * 2 * (NO_PC + 1) * 64, -586);
  fill_with(g_stats.pawn_rows, (size_t)g_stats.pawn_mask + 1, -1338);
  fill_with(&g_stats.corr[0][0], (size_t)(g_stats.corr_mask + 1) * 2, -5);
  fill_with(&g_stats.corr_x[0][0], (size_t)(g_stats.corr_mask + 1) * 2, 0);
}

static void thread_stats_reset(ThreadData &td) {
  fill_with(&td.quiet_hist[0][0], (size_t)2 * QH_SIZE, -5);
  fill_with(&td.low_ply_hist[0][0], (size_t)LOW_PLY_SIZE * QH_SIZE, 0);
  fill_with(&td.capt_hist[0][0][0], (size_t)12 * 64 * 7, -742);
  fill_with(&td.cont_corr[0][0], (size_t)(NO_PC + 1) * 64, 5);
  td.tt_move_hist = 0;
}

// Stato della gestione del tempo che vive fra una mossa e l'altra della partita (13_deepen.inc).
static void clock_memory_reset();

void search_clear() {
  for (auto &td : thread_data) {
    thread_stats_reset(td);
    memset(td.eval_cache, 0, sizeof(td.eval_cache));
  }
  shared_stats_reset();
  clock_memory_reset();
}

// ---------------------------------------------------------------------------
// Thread
// ---------------------------------------------------------------------------
// AA3 (10/10/2026, velocita', stessa scacchiera): la make copia qui, prima di toccarli, i 64 byte contigui di
// ThreadData da hash_key a plies_from_null (chiave, sei chiavi parziali, contatore delle 50 mosse, ply dalla mossa nulla)
// e le tre occupazioni; la unmake li rimette con due copie invece di ricalcolarli (td_keys_update: sei XOR in memoria
// lette e riscritte, td_occ_update con il salto sulla cattura). Come lo StateInfo di SF, che do_move copia e undo_move
// lascia indietro.
struct UndoInfo {
  int captured_piece;
  int captured_square;
  int old_castle;
  int old_enpassant;
  U64 keys[8];   // hash_key, pawn_key, mm_key[2], np_key[2], key_sink, fifty | plies_from_null
  U64 occ[3];
};
static_assert(offsetof(ThreadData, plies_from_null) + sizeof(int) == offsetof(ThreadData, hash_key) + sizeof(UndoInfo::keys) &&
              offsetof(ThreadData, fifty) == offsetof(ThreadData, hash_key) + 56,
              "AA3: hash_key ... plies_from_null devono essere 64 byte contigui in ThreadData");

static void init_cuckoo();
static void nn_dirty_init();

void init_threads(int thread_count) {
  if (thread_count < 1)
    thread_count = 1;
  if (thread_count > MAX_THREADS)
    thread_count = MAX_THREADS;

  for (auto &td : thread_data)
    if (td.nnpos) {
      nn_pos_destroy(td.nnpos);
      td.nnpos = nullptr;
      td.nnstack = nullptr;
    }

  static bool cuckoo_ready = false;
  if (!cuckoo_ready) {
    init_cuckoo();
    nn_dirty_init();   // raggi per le minacce scoperte delle dirty della rete (06_nndirty.inc)
    cuckoo_ready = true;
  }

  num_threads = thread_count;
  thread_data.resize(num_threads);
  shared_stats_alloc(num_threads);
  for (int i = 0; i < num_threads; i++) {
    ThreadData &td = thread_data[i];
    td.thread_id = i;
    td.nnpos = nn_pos_create();
    td.nnstack = nn_pos_stack(td.nnpos);
    td.nodes = td.tb_hits = 0;
    td.best_move = td.best_reply = td.depth = 0;
    td.completed_depth = 0;
  }
  search_clear();
}

void copy_board_to_thread(ThreadData &td) {
  memcpy(td.bitboards, bitboards, sizeof(bitboards));
  memcpy(td.occupancies, occupancies, sizeof(occupancies));
  td.side = side;
  td.enpassant = enpassant;
  td.castle = castle;
  td.hash_key = hash_key;
  td.fifty = fifty;
  // Mailbox e chiavi parziali: calcolate qui per intero, poi mantenute da make/unmake.
  for (int sq = 0; sq < 64; sq++)
    td.piece_on[sq] = -1;
  td.pawn_key = 0;
  td.mm_key[0] = td.mm_key[1] = 0;
  td.np_key[white] = td.np_key[black] = 0;
  for (int pc = P; pc <= k; pc++) {
    U64 bb = td.bitboards[pc];
    while (bb) {
      const int sq = get_ls1b_index(bb);
      td.piece_on[sq] = pc;
      const int type = pc % 6;
      if (type == P)
        td.pawn_key ^= piece_keys[pc][sq];
      else
        td.np_key[pc < p ? white : black] ^= piece_keys[pc][sq];
      if (type == N || type == B)
        td.mm_key[0] ^= piece_keys[pc][sq];
      else if (type == R || type == Q)
        td.mm_key[1] ^= piece_keys[pc][sq];
      pop_bit(bb, sq);
    }
  }
  td.plies_from_null = 1024;
  td.seldepth = 0;
  memcpy(td.repetition_table, repetition_table, sizeof(repetition_table));
  td.repetition_index = repetition_index;
  td.ply = 0;
  td.see_pin_key = ~td.hash_key;   // SeePinned: inchiodature da ricalcolare alla prima SEE
}

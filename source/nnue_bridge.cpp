// Triumviratus NNUE bridge — routes the engine's eval through the TRANN1 network
// code (nnue/): ThreatFeatureSet=FullThreats + PSQFeatureSet=HalfKAv2_hm +
// PawnFeatureSet=PawnPair + PassedFeatureSet=PassedPawns, L1=1024, 8 LayerStacks.
// The machinery is Stockfish-master's SFNNv13 (GPLv3, attribution in COPYING/README)
// adapted into our isolated Triumviratus:: namespace; the last two input blocks are
// ours, so the net format diverges from SFNNv13 (see nnue/nnue/nnue_architecture.h).
//
// Modello (07/10/2026, scacchiera unica v2): ogni thread di ricerca ha un handle con la catena degli accumulatori
// (AccumulatorStack) e la finny table (AccumulatorCaches). La rete non ha una scacchiera sua e non traduce nulla:
// legge quella del motore attraverso NnBoard (nnue/nn_board.h), e le dirty di ogni mossa (NnDirtyPiece,
// NnDirtyThreats, NnDirtyPawns, ../nn_dirty.h) le scrive la make del motore sulla pila dell'handle (nn_pos_stack).
// Una sola numerazione delle case, quella del motore (a8 = 0): le tabelle degli indici delle feature sono costruite
// per lei e danno gli stessi indici di prima (stessa rete, stessa valutazione).
// Storia: fino al 06/10/2026 c'era una seconda scacchiera (Position), specchio di quella del motore; la v1 della
// scacchiera unica (07/10) l'aveva tolta ricalcolando le dirty in un recupero pigro avanti/indietro, con un byteswap
// per ogni bitboard letto. Entrambi sono spariti.

#include "frozen.h"   // 🔴 DEVE stare qui: senza, il congelamento della
                      // miscela non si attiva nelle build di spedizione.
#include "nnue_bridge.h"

#include "profile.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#if defined(_MSC_VER)
    #include <intrin.h>   // _byteswap_uint64, _BitScanForward64
#endif

#include "nnue/attacks.h"          // Attacks::init (prima arrivava con position.h)
#include "nnue/bitboard.h"
#include "nnue/memory.h"          // LargePagePtr / make_unique_large_page (pesi rete su large pages)
#include "nnue/nn_board.h"         // la scacchiera del motore vista dalla rete (niente piu' Position)
#include "nnue/nnue/features/passed_state.h"  // pstidx (diagnosi degli indici di PassedState)
#include "nnue/types.h"
#include "nnue/evaluate.h"         // EvalFileDefaultName (nome del net embeddato)
#include "nnue/nnue/network.h"
#include "nnue/nnue/nnue_accumulator.h"
#include "nnue/nnue/nnue_misc.h"   // Eval::NNUE::EvalFile

#ifdef TRIUMV_EMBED_RESOURCE
// Windows: la rete di default sta in una risorsa RCDATA dell'exe (incbin non
// funziona con _MSC_VER, clang-cl incluso). Qui si definiscono i puntatori che
// nnue/nnue/network.cpp dichiara extern e si risolvono al primo load.
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
const unsigned char* gEmbeddedNNUEData = nullptr;
unsigned int         gEmbeddedNNUESize = 0;
static void embed_init_from_resource() {
    static bool done = false;
    if (done) return;
    done = true;
    // MAKEINTRESOURCEA(10) = RT_RCDATA in versione ANSI (RT_RCDATA segue UNICODE)
    HRSRC r = FindResourceA(nullptr, "NNUE_DEFAULT", MAKEINTRESOURCEA(10));
    if (!r) return;
    HGLOBAL h = LoadResource(nullptr, r);
    if (!h) return;
    gEmbeddedNNUEData = static_cast<const unsigned char*>(LockResource(h));
    gEmbeddedNNUESize = static_cast<unsigned int>(SizeofResource(nullptr, r));
}
#endif

using namespace Triumviratus;
using Triumviratus::Eval::NNUE::Network;
using Triumviratus::Eval::NNUE::AccumulatorStack;
using Triumviratus::Eval::NNUE::AccumulatorCaches;

// ---------------------------------------------------------------------------
// The single immutable network, loaded once at startup (nn_load_net) and
// optionally swapped at runtime (nn_reload_big). ~90 MB of weights, walked on
// EVERY eval -> the TLB-hottest data in the engine, so it lives on large pages
// (aligned_large_pages_alloc: Windows VirtualAlloc MEM_LARGE_PAGES with silent
// fallback to regular pages on failure/no-privilege; Linux 2MB-aligned +
// madvise(MADV_HUGEPAGE) = THP). AccumulatorCaches are built per handle FROM
// this net, so it must be loaded before any nn_pos_create().
// Immortalized (leak-at-exit by design): the large-page deleter may exit() on
// VirtualFree failure and the Linux path locks a mutex whose cross-TU static
// destruction order is unspecified -- never run it during static destruction.
// Reload-time frees (search stopped) are unaffected.
// ---------------------------------------------------------------------------
static LargePagePtr<Network>& g_net = *new LargePagePtr<Network>();

// Rete condivisa fra processi (01/10/2026, solo -DTRIUMV_SHARED_NET; vedi shared_net.h). La ricerca legge la rete
// da g_net_view: la copia locale (g_net) o l'oggetto in memoria condivisa, e in quel caso g_net viene liberata.
// Senza il flag NET_REF e' *g_net: codice identico a prima.
// 03/10/2026: ACCESA DI DEFAULT su Windows (come SF, che la usa sempre). Misura del 01/10, due build PGO release dallo
// stesso sorgente, socket pieno: +15,2% NPS con 20 motori sul socket 0, +24,6% sul socket 1 (32 GB, 2 canali),
// +34,9% con 38 motori; un motore solo non cambia. -DTRIUMV_NO_SHARED_NET la spegne. shared_net.h esiste solo per
// Windows: altrove resta la copia privata.
#if defined(_WIN32) && !defined(TRIUMV_NO_SHARED_NET) && !defined(TRIUMV_SHARED_NET)
    #define TRIUMV_SHARED_NET
#endif
#ifdef TRIUMV_SHARED_NET
    #include "shared_net.h"
static const Network* g_net_view = nullptr;
static std::string    g_net_mem_status;   // esito dell'ultima attach, per nn_net_memory_status()
    #define NET_REF (*g_net_view)
#else
    #define NET_REF (*g_net)
#endif
// "" nelle build senza TRIUMV_SHARED_NET: uci_mt stampa la riga solo se c'e' qualcosa da dire.
const char* nn_net_memory_status(void) {
#ifdef TRIUMV_SHARED_NET
    return g_net_mem_status.c_str();
#else
    return "";
#endif
}

// Generation counter, bumped on every (re)load of g_net. AccumulatorCaches
// (finny) are seeded from the NET'S BIASES: caches built from an older net
// silently corrupt every refresh after an EvalFile reload (bug found
// 2026-07-14 — a semantically-identical permuted net benched differently via
// setoption but identically as startup default). Every cache holder compares
// its own generation and rebuilds when stale.
static std::atomic<int> g_net_gen{1};

// Eval output scale (percent, default 100 = x1.0). The SFNNv13 cp formula lands on
// a DIFFERENT scale than the SFNNv10 eval-wrapper the engine's search margins were
// SPSA-tuned for (pawn ~56 vs ~332) -> the pruning thresholds are mis-sized. This
// multiplier re-aligns the eval with the existing margins; sweep it at fixed depth.
static int g_eval_scale_pct = 60;   // BAKE 2026-07-16: vettore iter1800 (era 56)
// --- EvalScale PER BUCKET (15/08/2026) --------------------------------------
// La rete ha LayerStacks = 8 bucket di output, scelti per numero di pezzi
// (network.cpp:170: bucket = (count<ALL_PIECES>() - 1) / 4), ma la ricalibrazione
// sull'eval e' UNA SOLA e globale. La rete e' stata addestrata a minimizzare la
// loss di PREDIZIONE uniformemente su tutte le fasi, non gli Elo: una scala unica
// impone la stessa calibrazione a un'apertura con 32 pezzi e a un finale con 4,
// dove la struttura dell'errore di eval non e' la stessa.
// ⚠️ BYTE-IDENTICO coi default: tutti a 60 = il valore globale precedente, e
//    l'aritmetica applicata e' esattamente la stessa (v * pct / 100).
// `EvalScale` resta e scrive TUTTI gli otto, cosi' il vecchio comportamento e le
// vecchie ricette continuano a valere; i B0..B7 lo raffinano per fase.
// ⚠️ ORDINE: chi setta `EvalScale` DOPO i B0..B7 li sovrascrive tutti.
// BAKE MOE1 (30/09/2026): scala per fascia tarata sulla rete MoE-1024 (calibrazione 64/53/62/60/60/59/59/60,
// poi SPSA MOE1 a iterazione 5410). Tutti 60 era la scala della legio-septima.
static int g_eval_scale_b[8] = {60, 54, 60, 57, 59, 58, 59, 58};
// 5.1 EvalTTWrite: ultimo valore UNADJUSTED (pre-rule50, pre-EvalScale) calcolato da nn_scale
// su QUESTO thread = lo "unadjustedStaticEval" di SF, fifty-independent -> si cacha questo e si
// ri-finalizza col fifty corrente (hit su TUTTE le trasposizioni, sempre esatto).
// NPS 25/09/2026: non piu' thread_local. Con MinGW il TLS passa da emutls
// (pthread_getspecific + once + spinlock): il profilo xperf del 25/09 dava ~2,4% del
// tempo del motore g++ a quelle funzioni, per tre scritture per eval che servono solo a
// due opzioni spente (EvalTTWrite, EvalCacheOptSplit). I tre valori vivono ora nella
// struttura per thread del bridge (SfPos) e si leggono passando l'handle.
struct NnLast {
    int unadjusted = 0;
    // EvalCacheOptSplit: i due termini invarianti della decomposizione lineare
    // v = base + optimism*coeff/1000 (coeff in millesimi). Vedi nn_scale.
    int opt_base  = 0;
    int opt_coeff = 0;
};

// Stockfish's eval cp scaling (evaluate.cpp), inlined here with optimism=0 (the
// engine's static eval is unbiased; optimism is a search-only blend in SF). psqt
// and positional are the stm-relative raw NNUE outputs of Network::evaluate.
// material is computed from the piece counts of our board. Returns stm-relative internal-unit value.
// ⭐ 5.1 EVAL: optimism (SF evaluate.cpp:55,59), default OFF (g_optimism resta 0 -> termine nullo
// -> byte-identico). g_optimism[stm] e' aggiornato dalla root (search) in unita'-interne SF; il
// nostro static-eval lo ometteva (=0). Riacceso, ricalibra l'eval come fa SF (contempt dinamico).
int g_eval_optimism = 1;  // [5.1 BAKE] ON di default (spsa_struct lo ha tenuto a strength=89, non spento)
// F-019 (2026-07-03): atomic relaxed-di-fatto — il main scrive a fine iterazione, gli
// helper leggono nella eval; su int non-atomici era UB formale (TSan-visibile). Su x86
// load/store atomici su int = stessa istruzione: zero costo, stesso comportamento.
std::atomic<int> g_optimism[2] = {0, 0};
// OptPerThread (SMP, 26/09/2026): con 1 ogni thread usa l'optimism della PROPRIA posizione NNUE (SfPos::opt),
// calcolato dal proprio score di radice, come i worker di SF. 0 = g_optimism globale (storico). A 1 thread le due
// forme coincidono (il thread 0 scrive entrambi): bench identico.
// 04/10/2026: dalla riscrittura della ricerca e' sempre 1 (ogni thread imposta il suo optimism a ogni iterazione,
// search/14_deepen.inc).
int g_opt_per_thread = 1;

// --- COSTANTI DEL BLEND, ESPOSTE (15/08/2026) --------------------------------
// Sono tutte di Stockfish, ereditate col wrapper e MAI tarate su questa rete. E non
// scalano l'eval: decidono COSA DICE. `EvalPosW` e' quanto fidarsi della testa
// posizionale rispetto alla psqt; `EvalComplexDiv` quanto smorzare quando le due sono
// in disaccordo; `EvalMatBase`/`EvalPawnMat` come l'eval cresce col materiale.
// Calibrate per la rete di SF: la nostra e' TRANN2, con PawnPair, PassedPawns e
// FullThreats in ingresso, quindi l'affidabilita' relativa delle due teste non ha
// motivo di essere la stessa.
// ⚠️ E' la differenza con la scala per bucket chiusa a -9,82 il 15/08: quella poteva
//    solo STIRARE l'eval in modo uniforme, queste ne cambiano la forma.
// ⚠️ DEGENERAZIONE: `EvalPsqtW` ed `EvalPosW` insieme contengono anche la direzione
//    "scala tutto", che e' gia' coperta da EvalScale. Nel preset se ne tara UNO SOLO.
// 🔴 BAKE 09/09/2026 — co-tune SPSA di questi sei (+ EvalOptStrength/EvalOptDiv in
// threads.cpp), 1.989 iterazioni mirror a 8+0.08. Non sono piu' i numeri di Stockfish:
// sono i primi tarati su QUESTA rete. Due letture indipendenti, in regimi diversi:
//     10+0.1  hash  64   +2,71 ± 3,35   LOS 94,4%   11.654 partite  (s28)
//     20+0.2  hash 256   +3,75 ± 3,49   LOS 98,3%   10.000 partite  (s30)
// 🔑 La seconda e' quella che conta: e' il regime di SPEDIZIONE, e l'effetto CRESCE
//    invece di svanire. Era il rischio vero — TTTwoLevel valeva +4,55 a hash 64 e
//    zero a 256 — ed e' stato misurato, non assunto.
// 📌 `g_ev_psqt_w` resta 125: NON e' stato tarato di proposito. Insieme a `g_ev_pos_w`
//    contiene la direzione "scala tutto", gia' coperta da EvalScale; tararli entrambi
//    avrebbe sprecato un parametro su una degenerazione. Si e' mosso solo il RAPPORTO.
// Canary: 252074 -> 242956.
int g_ev_psqt_w    = 125;    // peso psqt        (/128)  — non tarato (degenere con pos_w)
// BAKE MOE1 (30/09/2026): SPSA MOE1 sulla rete MoE-1024 F4_avg5_perm, iterazione 5410, 20+0.2, 25 leve
// legate alla rete (Tuning_SPSA/spsa_lab/runs/20260930_004814_SPSA_MoE-1024_...). Vettore a 4363 contro
// vettore iniziale: +8,6 +- 11,5 su 928 partite a 10+0.1. Valori precedenti fra parentesi.
int g_ev_pos_w     = 131;    // peso positional  (/128)  [BAKE MOE1 126->131]
// EvalPosWEnd (01/10/2026): peso positional nei FINALI (<= 15 pezzi, i due esperti di finale della Consilium).
// Misura senza ricerca (65.947 posizioni CCRL, correlazione di rango con la valutazione profonda di un terzo):
// con 125/131 la miscela inverte il segno nei finali in cui psqt (+X) e positional (-X) si compensano, perche'
// resta solo (125-131)*X/128. Correlazione <= 9 pezzi: -0,067 con 131, 0,252 con 125 (rete grezza 0,257, SF19
// 0,301); 10-15 pezzi: 0,457 -> 0,515. Default = g_ev_pos_w: BYTE-IDENTICO.
// CHIUSA 01/10/2026, resta 131. SPRT EvalPosWEnd=125 vs 131 a 20+0.2: -6,6 +- 8,3 su 1.638 partite (fermato).
// SPSA congiunto con EvalScaleB0..B3 su libro di finali (run 20261001_203201_SPSA_finali, 1.517 iterazioni a 12-16 s):
// nessun gradiente, tutti i valori entro 0,2 c dai default (PosWEnd ~129). Le scale per fascia non possono cambiare
// l'ordinamento dentro una fascia, quindi non correggono la correlazione. Diagnosi contro SF19 a 20+0.2: dal libro di
// finali -55 (338 partite), dal libro UHO_4060 -127 (261): il distacco da SF non nasce nei finali.
int g_ev_pos_w_end = 131;    // peso positional con <= 15 pezzi (/128)
int g_ev_cplx_div  = 18198;  // smorzamento per disaccordo fra le due teste [BAKE MOE1 19139->18198]
int g_ev_pawn_mat  = 680;    // valore del pedone nel termine material      [BAKE MOE1 551->680]
int g_ev_mat_base  = 102051; // base materiale della scala nnue             [BAKE MOE1 84768->102051]
int g_ev_opt_cplx  = 432;    // blend optimism <-> complessita'             [BAKE MOE1 461->432]
int g_ev_opt_base  = 5831;   // base materiale del termine optimism         [BAKE MOE1 6456->5831]

// ⭐ EvalOptSimple — port di SF de948f0f "Simplify optimism scaling formula" (10/08/2026).
// SF ha tolto la dipendenza dal MATERIALE al termine optimism, rendendolo una costante,
// e ha alzato la base materiale:
//     -  v = (nnue * (77871 + material) + optimism * (7191 + material)) / 77871
//     +  v = (nnue * (91000 + material) + optimism *  7675           ) / 91000
// Passata STC non-reg <-1.75,0.25> su 110.624 partite e LTC su 153.366.
// ⚠️ NON e' un guadagno: sono soglie di NON-REGRESSIONE. SF l'ha fusa perche' semplifica,
//    non perche' porti Elo. Da noi ha pero' un interesse che da loro non ha: e' l'unico
//    port della finestra che tocca la EVAL, ed e' proprio dove la nostra rete diverge di
//    piu' dalla loro. Quelle costanti sono tarate sulla rete di SF; sulla nostra la forma
//    piu' semplice potrebbe stare meglio o peggio, e non c'e' modo di saperlo a tavolino.
// 0 = forma storica => BYTE-IDENTICO. 1 = forma SF.
int g_ev_opt_simple = 0;
int g_ev_mat_base2  = 91000;  // base materiale della forma nuova (SF: 91000)
int g_ev_opt_const  = 7675;   // coefficiente optimism, ora COSTANTE (SF: 7675)

// ===== COSTANTI DELLA MISCELA CONGELATE (TRIUMV_FROZEN) =================
// 🔴 Il motivo, misurato il 09/09/2026: nn_scale contiene SETTE divisioni, e con
// queste dieci lasciate variabili il divisore non e' noto a compilazione, quindi
// sono divisioni HARDWARE - una anche a 64 bit, `/((long long) g_ev_opt_cplx *
// g_ev_mat_base * 100)`. Rese costanti diventano moltiplica-e-scala, che il
// compilatore genera con risultato interi IDENTICO. Il divisore su questa CPU e'
// misurato a 57 cicli/nodo contro i 36,6 di Stockfish.
// Il congelamento di threads.cpp non le copriva: vivono in un'altra unita'.
//
// ⚠️ NON sono variabili morte: uci_mt le scrive tramite la TABELLA DI PUNTATORI
// g_eval_consts qui sotto (nn_set_eval_const), ed e' cosi' che il tuo SPSA le ha
// tarate. Un'analisi che cerca `g_x =` non lo vede: la scrittura e' `*c.var =`.
// Per questo la tabella resta com'e' - le #define vengono tolte attorno a lei e
// rimesse subito dopo - e nn_frozen_check() confronta i valori vivi con i
// letterali a ogni ricerca, dicendolo se qualcuno li cambia.
#ifdef TRIUMV_FROZEN
#define g_ev_psqt_w 125
#define g_ev_pos_w 131
#define g_ev_pos_w_end 131
#define g_ev_cplx_div 18198
#define g_ev_pawn_mat 680
#define g_ev_mat_base 102051
#define g_ev_opt_cplx 432
#define g_ev_opt_base 5831
#define g_ev_opt_simple 0
#define g_ev_mat_base2 91000
#define g_ev_opt_const 7675
#endif
// =======================================================================

static inline int nn_scale(const NnBoard& pos, Value psqt, Value positional, int rule50,
                           NnLast* last = nullptr, const int* opt_local = nullptr) {
    const int pieces   = pos.count();
    const int pos_w    = pieces <= 15 ? g_ev_pos_w_end : g_ev_pos_w;   // EvalPosWEnd (vedi la dichiarazione)
    int nnue           = (g_ev_psqt_w * int(psqt) + pos_w * int(positional)) / 128;
    int nnueComplexity = std::abs(int(psqt) - int(positional));
    nnue -= nnue * nnueComplexity / g_ev_cplx_div;

    int npm = int(KnightValue) * pos.count_type(NN_KNIGHT) + int(BishopValue) * pos.count_type(NN_BISHOP)
            + int(RookValue) * pos.count_type(NN_ROOK) + int(QueenValue) * pos.count_type(NN_QUEEN);
    int material = g_ev_pawn_mat * pos.count_type(NN_PAWN) + npm;

    int v;
    if (g_eval_optimism) {
        int optimism = opt_local ? opt_local[pos.side_to_move()] : g_optimism[pos.side_to_move()].load();
        optimism += optimism * nnueComplexity / g_ev_opt_cplx;   // SF: blend optimism con la complessita'
        if (g_ev_opt_simple)
            // SF de948f0f: optimism non scala piu' col materiale, e la base sale.
            v = int((std::int64_t(nnue) * (g_ev_mat_base2 + material)
                     + std::int64_t(optimism) * g_ev_opt_const) / g_ev_mat_base2);
        else
        v = int((std::int64_t(nnue) * (g_ev_mat_base + material)
                 + std::int64_t(optimism) * (g_ev_opt_base + material)) / g_ev_mat_base);
    } else
        v = int(std::int64_t(nnue) * (g_ev_mat_base + material) / g_ev_mat_base);

    // --- Scomposizione per la eval-cache (EvalCacheOptSplit) ------------------
    // L'optimism entra LINEARMENTE: v = base + optimism * coeff, dove base e coeff
    // dipendono solo dalla POSIZIONE (nnue, material, complessita') e optimism cambia
    // a ogni iterazione. Esportandoli, la cache puo' memorizzare i due termini
    // invarianti e ricomporre il valore con l'optimism CORRENTE alla lettura:
    // eval esatta e nessun hit perso. E' l'alternativa a invalidare la entry, che
    // costava −9,42 Elo perche' buttava via il 52% del tempo in forward NNUE.
    // Il coeff include GIA' EvalScale, cosi' il consumatore lo somma direttamente a un
    // valore gia' scalato. Resta fuori solo il rule50, che la cache smorza per conto suo.
    // Stesso bucket che network.cpp:170 usa per scegliere lo stack di output.
    const int scale_pct = g_eval_scale_b[(pieces - 1) / 4];

    if (last) {
        last->opt_base  = int(std::int64_t(nnue) * (g_ev_mat_base + material) / g_ev_mat_base);
        last->opt_coeff = int(std::int64_t(g_ev_opt_cplx + nnueComplexity) * (g_ev_opt_base + material) * 1000
                              * scale_pct / ((long long)g_ev_opt_cplx * g_ev_mat_base * 100LL));
        last->unadjusted = v;   // PRE rule50/EvalScale: SF unadjustedStaticEval (fifty-independent)
    }
    v -= v * rule50 / 199;
    if (scale_pct != 100)
        v = int(std::int64_t(v) * scale_pct / 100);  // re-calibrate to the search margins
    v = std::clamp(v, int(VALUE_TB_LOSS_IN_MAX_PLY) + 1, int(VALUE_TB_WIN_IN_MAX_PLY) - 1);
    return v;
}


// ---------------------------------------------------------------------------
// Net loading
// ---------------------------------------------------------------------------
static int load_net_impl(const char* path) {
    if (!path || !*path)
        return 0;
#ifdef TRIUMV_EMBED_RESOURCE
    embed_init_from_resource();
#endif
    // File presente sul disco? Se si', vince SEMPRE il file (path passato as-is:
    // un path completo non e' mai uguale al nome-default -> Network::load va sul
    // disco). Se NO: fallback sul net EMBEDDATO, ma solo se il nome richiesto e'
    // quello di default (EvalFile con un path esplicito sbagliato resta un errore).
    bool file_ok;
    {
        std::ifstream f(path, std::ios::binary);
        file_ok = f.good();
    }
    const char* load_name = path;
    if (!file_ok) {
        std::string p(path);
        size_t      sl   = p.find_last_of("/\\");
        std::string base = sl == std::string::npos ? p : p.substr(sl + 1);
        if (base != EvalFileDefaultName || !Eval::NNUE::embedded_net_available())
            return 0;
        load_name = EvalFileDefaultName;   // nome "nudo" -> Network::load instrada su <internal>
    }
    Eval::NNUE::EvalFile ef{};
    ef.defaultName = EvalFileDefaultName;  // serve al match nome-default -> embedded
    auto net = make_unique_large_page<Network>(ef);   // large pages (fallback automatico)
    net->load(".", load_name);  // dirs {"<internal>","",rootDir}: "" opens the path as given
    // verify() invokes the callback with a one-line SUCCESS info string when the net
    // loaded, or with a multi-line "ERROR: ..." block followed by exit(EXIT_FAILURE)
    // when it did not. So if verify() returns at all, the load succeeded; we just echo
    // the info line for visibility (do NOT treat the callback as a failure signal).
    // g_startup_quiet (main.cpp): when launched by a GUI, stay silent during the
    // net load so nothing prints before the "uci" handshake. verify() still runs
    // (it exits on a bad net); we just suppress the success info line.
    extern bool g_startup_quiet;
    net->verify(path, [](std::string_view s) {
        if (!g_startup_quiet) {
            std::printf("info string %.*s\n", int(s.size()), s.data());
            std::fflush(stdout);
        }
    });
    g_net = std::move(net);
#ifdef TRIUMV_SHARED_NET
    {
        g_net_view = g_net.get();
        std::string st;
        const void* sh = TriumvShm::attach(g_net.get(), sizeof(Network),
                                           std::uint64_t(g_net->get_content_hash()) ^ std::uint64_t(sizeof(Network)), st);
        if (sh) {
            g_net_view = static_cast<const Network*>(sh);
            g_net.reset();   // la copia locale non serve piu': ~245 MB restituiti
        }
        g_net_mem_status = st;
        if (!g_startup_quiet) {
            std::printf("info string NNUE weights: %s\n", st.c_str());
            std::fflush(stdout);
        }
    }
#endif
    ++g_net_gen;   // invalidate every AccumulatorCaches built from the old net
    return 1;
}

const char* nn_default_net_name(void) { return EvalFileDefaultName; }

int nn_load_net(const char* net_path) { return load_net_impl(net_path); }
int nn_reload_big(const char* net_path) { return load_net_impl(net_path); }

// Salva la rete caricata nel formato con i blocchi da innesto di mask (09/10/2026, "exportgraft <mask> <file>" e
// "exportprel <file>" = mask 1 in uci_mt.cpp): i blocchi che la rete non ha hanno le righe a zero, quindi il file e' la
// stessa rete con quei blocchi innestati a zero (stessa valutazione, da verificare col bench). Dopo il salvataggio il
// motore torna allo stato di prima.
int nn_export_graft(unsigned mask, const char* path) {
#ifdef TRIUMV_NO_GRAFTS
    (void) mask, (void) path;
    return 0;  // build senza blocchi da innesto
#else
    const unsigned was = nn_graft_mask;
    nn_graft_mask      = mask;
    const bool ok      = NET_REF.save(std::optional<std::string>(std::string(path)));
    nn_graft_mask      = was;
    return ok ? 1 : 0;
#endif
}

int nn_export_pst(const char* path) { return NET_REF.save_pst(std::string(path)) ? 1 : 0; }

int nn_pst_indices(const unsigned long long* bb12, unsigned long long occ, int persp, unsigned* out) {
    using Eval::NNUE::Features::PassedState;
    std::uint16_t e[16];
    const int     n   = PassedState::entries_of(bb12, occ, e);
    const int     ksq = int(lsb(Bitboard(bb12[5 + 6 * persp])));
    for (int i = 0; i < n; i++)
        out[i] = unsigned(PassedState::make_index(Color(persp), ksq, e[i]));
    std::sort(out, out + n);
    return n;
}

int nn_graft_entries(unsigned mask, const unsigned long long* bb12, unsigned long long occ, unsigned short* out) {
    return Eval::NNUE::Features::PawnGrafts::entries_of(mask, bb12, occ, reinterpret_cast<std::uint16_t*>(out));
}

void nn_init_tables(void) {
    Bitboards::init();
    Attacks::init();  // slider magics live in attacks.cpp (separate from Bitboards::init)
}

// No-ops kept for API stability (both paths always use the refresh cache).
void nn_set_finny(int) {}

void nn_acc_stats(void) {}

// M3 toggles. Incremental is now the DEFAULT: validated bit-exact vs full-refresh
// (zero verify mismatches + IDENTICAL bench node counts at depth 12/14 across all 8
// positions, exercising captures/promotions/e.p./castling) at +16% NPS. Toggle OFF
// ("incremental off") to fall back to the M2 full-refresh A/B base.
static bool g_incremental = true;
static bool g_verify      = false;
static unsigned long long g_verify_count = 0, g_verify_bad = 0;   // valutazioni confrontate / diverse

// NB (2026-07-15): "PsqtFastPath" (eval = solo psqt del net ai nodi |psqt|>soglia,
// saltando pairwise+propagate) PROVATO e UCCISO CON MISURA su build PGO:
// thr700 fire 75% -> albero +154%; thr2000 fire 16% -> +28%; thr4000 fire 0.6%
// -> +4%; time-to-depth SEMPRE peggiore. Terza falsificazione della famiglia
// "eval economica ai nodi decisi" (smallnet SF -17 Elo, SPLE): la search e'
// co-adattata all'eval piena, ogni surrogato grossolano gonfia l'albero piu'
// di quanto il forward risparmiato ripaghi. NON riprovare varianti.
void        nn_set_incremental(int on) { g_incremental = on != 0; }
void        nn_set_verify(int on) { g_verify = on != 0; }
void        nn_set_eval_scale(int pct) {
    g_eval_scale_pct = pct < 1 ? 1 : pct;
    for (int b = 0; b < 8; ++b) g_eval_scale_b[b] = g_eval_scale_pct;  // globale = tutti
}
void        nn_set_eval_scale_bucket(int b, int pct) {
    if (b >= 0 && b < 8) g_eval_scale_b[b] = pct < 1 ? 1 : pct;
}
int         nn_get_eval_scale_bucket(int b) { return (b >= 0 && b < 8) ? g_eval_scale_b[b] : 60; }

// Costanti del blend, in UNA tabella: il nome sta qui e non sparso in uci_mt.cpp, cosi'
// aggiungerne una non richiede di toccare due file e non si puo' dichiarare in UCI
// un'opzione che nessuno legge (il modo piu' silenzioso di far misurare zero a un tuner).
// I `min` non sono cosmetici: sono DIVISORI, e uno zero sarebbe una divisione per zero
// dentro la eval, cioe' un crash a meta' partita invece di un errore al setoption.
// Sonda EvalBucketOverride: definita in nnue/network.cpp, dove si sceglie il bucket.
namespace Triumviratus::Eval::NNUE { extern int g_eval_bucket_override; }

#ifdef TRIUMV_FROZEN
#undef g_ev_psqt_w
#undef g_ev_pos_w
#undef g_ev_pos_w_end
#undef g_ev_cplx_div
#undef g_ev_pawn_mat
#undef g_ev_mat_base
#undef g_ev_opt_cplx
#undef g_ev_opt_base
#undef g_ev_opt_simple
#undef g_ev_mat_base2
#undef g_ev_opt_const
#endif
namespace {
struct EvalConst { const char* name; int* var; int lo; int hi; };
const EvalConst g_eval_consts[] = {
    {"EvalPsqtW",         &g_ev_psqt_w,    40,   260},
    {"EvalPosW",          &g_ev_pos_w,     40,   260},
    {"EvalPosWEnd",       &g_ev_pos_w_end, 40,   260},   // peso positional con <= 15 pezzi (01/10/2026)
    {"EvalComplexDiv",    &g_ev_cplx_div, 4000, 60000},
    {"EvalPawnMat",       &g_ev_pawn_mat, 200,  1200},
    {"EvalMatBase",       &g_ev_mat_base, 20000, 200000},
    {"EvalOptComplexDiv", &g_ev_opt_cplx, 100,   2000},
    {"EvalOptMatBase",    &g_ev_opt_base, 1000,  30000},
    // Port SF de948f0f. EvalOptSimple e' un TOGGLE, ma vive qui come spin 0/1: la
    // tabella e' gia' enumerata da uci_mt per dichiarare le opzioni, quindi entrare
    // qui vuol dire essere dichiarata e instradata senza toccare altro. Ed e' anche
    // la forma giusta per il gestore generico, che fa atoi() e su una `check` non
    // accenderebbe mai (vedi uci_mt.cpp:514).
    {"EvalOptSimple",     &g_ev_opt_simple,   0,      1},
    // SONDA (17/08): -1 = normale. 0..7 forza la testa di output, per misurare il
    // salto di eval fra due bucket adiacenti sulla STESSA posizione. Non giocarci.
    {"EvalBucketOverride", &Triumviratus::Eval::NNUE::g_eval_bucket_override, -1, 7},
    {"EvalMatBase2",      &g_ev_mat_base2, 20000, 200000},
    {"EvalOptConst",      &g_ev_opt_const,  1000,  30000},
};
}
#ifdef TRIUMV_FROZEN
// Confronta i valori VIVI con i letterali compilati. Sta QUI, dentro l'isola in cui
// le #define sono tolte, perche' e' l'unico punto in cui `&g_ev_psqt_w` e' ancora
// l'indirizzo di una variabile e non `&125`. Chiamata una volta per ricerca.
void nn_frozen_check() {
    struct FzRef { const int* p; int val; const char* name; };
    static const FzRef fz[] = {
        {&g_ev_psqt_w, 125, "g_ev_psqt_w"},
        {&g_ev_pos_w, 131, "g_ev_pos_w"},
        {&g_ev_pos_w_end, 131, "g_ev_pos_w_end"},
        {&g_ev_cplx_div, 18198, "g_ev_cplx_div"},
        {&g_ev_pawn_mat, 680, "g_ev_pawn_mat"},
        {&g_ev_mat_base, 102051, "g_ev_mat_base"},
        {&g_ev_opt_cplx, 432, "g_ev_opt_cplx"},
        {&g_ev_opt_base, 5831, "g_ev_opt_base"},
        {&g_ev_opt_simple, 0, "g_ev_opt_simple"},
        {&g_ev_mat_base2, 91000, "g_ev_mat_base2"},
        {&g_ev_opt_const, 7675, "g_ev_opt_const"},
    };
    static bool reported[sizeof(fz) / sizeof(fz[0])] = {false};
    for (std::size_t j = 0; j < sizeof(fz) / sizeof(fz[0]); j++) {
        if (reported[j] || *fz[j].p == fz[j].val) continue;
        reported[j] = true;
        printf("info string ATTENZIONE: %s e' congelato a %d ma e' stato impostato a %d: "
               "questa build IGNORA il cambiamento\n", fz[j].name, fz[j].val, *fz[j].p);
        fflush(stdout);
    }
}
#endif
#ifdef TRIUMV_FROZEN
#define g_ev_psqt_w 125
#define g_ev_pos_w 131
#define g_ev_pos_w_end 131
#define g_ev_cplx_div 18198
#define g_ev_pawn_mat 680
#define g_ev_mat_base 102051
#define g_ev_opt_cplx 432
#define g_ev_opt_base 5831
#define g_ev_opt_simple 0
#define g_ev_mat_base2 91000
#define g_ev_opt_const 7675
#endif
int nn_eval_const_count(void) { return int(sizeof(g_eval_consts) / sizeof(g_eval_consts[0])); }
const char* nn_eval_const_name(int i) { return g_eval_consts[i].name; }
int nn_eval_const_get(int i)  { return *g_eval_consts[i].var; }
int nn_eval_const_lo(int i)   { return g_eval_consts[i].lo; }
int nn_eval_const_hi(int i)   { return g_eval_consts[i].hi; }
int nn_set_eval_const(const char* name, int value) {
    for (const EvalConst& c : g_eval_consts)
        if (!std::strcmp(c.name, name)) {
            *c.var = value < c.lo ? c.lo : (value > c.hi ? c.hi : value);
            return 1;
        }
    return 0;
}
int         nn_get_eval_scale(void) { return g_eval_scale_pct; }   // per normalizzare 'score cp' in stampa (undo EvalScale, SF-style)
// 5.1 EvalTTWrite (SF-style): l'unadjusted dell'ultima nn_scale su questo thread (fifty-indep).
// Ri-finalizza l'unadjusted col rule50 corrente: IDENTICO a un td_evaluate fresco (stesse op di
// nn_scale 111-114) per QUALSIASI fifty -> la cache eval e' esatta su ogni trasposizione.
// ⚠️ `bucket` va passato dal chiamante: qui non c'e' la Position. E' il bucket della
//    posizione CORRENTE (la entry TT e' di questa posizione), (pezzi - 1) / 4.
int         nn_finalize(int unadjusted, int rule50, int bucket) {
    int v = unadjusted;
    v -= v * rule50 / 199;
    const int scale_pct = g_eval_scale_b[bucket < 0 ? 0 : (bucket > 7 ? 7 : bucket)];
    if (scale_pct != 100)
        v = int(std::int64_t(v) * scale_pct / 100);
    return std::clamp(v, int(VALUE_TB_LOSS_IN_MAX_PLY) + 1, int(VALUE_TB_WIN_IN_MAX_PLY) - 1);
}

// ---------------------------------------------------------------------------
// Per-thread handle
// ---------------------------------------------------------------------------
namespace {

// Stato per thread della rete (07/10/2026, scacchiera unica v2): la catena degli accumulatori, con la pila delle
// dirty che scrive la make del motore, e la finny table. Nessuna scacchiera: la posizione e' quella del motore.
// V3 (08/10/2026, velocita', albero identico): pila degli accumulatori e finny table su large pages, come la TT e i
// pesi. Prima erano su std::make_unique, cioe' su pagine da 4 KB: la finny table (una riga per casa del re
// e per esperto) si legge a ogni refresh in punti sparsi, e il profilo del 05/10 dava 8,6% dei cicli a
// update_accumulator_refresh_cache. Senza il privilegio l'allocazione ripiega su pagine normali.
struct SfPos {
    LargePagePtr<AccumulatorStack>  accStack;
    LargePagePtr<AccumulatorCaches> caches;

    int    opt[2] = {0, 0};   // OptPerThread: optimism di QUESTO thread (per lato)

    int netGen;   // generation of g_net the caches were built from

    NnLast last;  // termini dell'ultima nn_scale di QUESTO thread (ex thread_local)

    SfPos() {
        accStack = make_unique_large_page<AccumulatorStack>();
        accStack->reset();
        caches   = make_unique_large_page<AccumulatorCaches>(NET_REF);
        netGen   = g_net_gen;
    }
};

// Rebuild the handle's finny caches if the network was reloaded since they were
// built. Called at root set (never mid-search: EvalFile reload stops search first).
inline void ensure_caches_fresh(SfPos* p) {
    if (p->netGen != g_net_gen) {
        p->caches = make_unique_large_page<AccumulatorCaches>(NET_REF);
        p->netGen = g_net_gen;
    }
}

// Valutazione da zero (refresh completo) della nostra scacchiera, su uno stato di appoggio.
inline int eval_full(const NnBoard& board, int rule50, AccumulatorStack& acc, AccumulatorCaches& cch,
                     int* raw_out = nullptr, const int* opt_local = nullptr) {
    acc.reset();
    auto [psqt, positional] = NET_REF.evaluate(board, acc, cch);
    if (raw_out)
        *raw_out = int(psqt) + int(positional);
    return nn_scale(board, psqt, positional, rule50, nullptr, opt_local);
}

}  // namespace

void* nn_pos_create(void) { return new SfPos(); }

int nn_last_unadjusted(void* handle) { return static_cast<SfPos*>(handle)->last.unadjusted; }
int nn_last_opt_base(void* handle)   { return static_cast<SfPos*>(handle)->last.opt_base; }
int nn_last_opt_coeff(void* handle)  { return static_cast<SfPos*>(handle)->last.opt_coeff; }
void  nn_pos_destroy(void* handle) { delete static_cast<SfPos*>(handle); }
void  nn_pos_set_optimism(void* handle, int w, int b) {
    SfPos* p = static_cast<SfPos*>(handle);
    p->opt[0] = w;
    p->opt[1] = b;
}

void nn_pos_set(void* handle) {
    SfPos* p = static_cast<SfPos*>(handle);
    ensure_caches_fresh(p);   // EvalFile reload -> caches seeded from old net's biases
    p->accStack->reset();     // l'accumulatore della radice si calcola alla prima valutazione
}

NnStack* nn_pos_stack(void* handle) { return &static_cast<SfPos*>(handle)->accStack->dirty_stack(); }

namespace {
// Stato di appoggio per thread per le valutazioni da zero (incremental off, nnueverify): non tocca la catena della
// ricerca, che la make del motore continua ad allungare e accorciare.
struct ScratchEval {
    std::unique_ptr<AccumulatorStack>  acc;
    std::unique_ptr<AccumulatorCaches> cch;
    int                                gen = 0;
};
ScratchEval& scratch_eval() {
    thread_local ScratchEval s;
    if (!s.acc || s.gen != g_net_gen) {
        if (!s.acc) s.acc = std::make_unique<AccumulatorStack>();
        s.cch = std::make_unique<AccumulatorCaches>(NET_REF);
        s.gen = g_net_gen;
    }
    return s;
}
}  // namespace

int nn_pos_eval(void* handle, const unsigned long long* bb, const unsigned long long* occ, const int* mailbox,
                int side_white, int rule50) {
    SfPos* p = static_cast<SfPos*>(handle);
    const NnBoard board(bb, occ, mailbox, side_white ? WHITE : BLACK);

    if (!g_incremental) {
        // Refresh completo a ogni valutazione (stessa finny table del thread), optimism globale come sempre.
        return eval_full(board, rule50, *scratch_eval().acc, *p->caches);
    }

    // La catena degli accumulatori e' quella della ricerca, con le dirty gia' scritte dalla make; la posizione si
    // legge dalla nostra scacchiera.
    auto [psqt, positional] = NET_REF.evaluate(board, *p->accStack, *p->caches);
#ifdef TRIUMV_X4_NOLAST
    // X4 (08/10/2026, NOLAST): i termini dell'ultima valutazione (NnLast: unadjusted, opt_base, opt_coeff) servivano
    // a EvalTTWrite ed EvalCacheOptSplit, ritirate: nn_last_* non ha piu' chiamanti. Senza `last` nn_scale salta due
    // moltiplicazioni a 64 bit con divisione per costante e tre scritture per valutazione. Valore restituito identico.
    int inc = nn_scale(board, psqt, positional, rule50, nullptr, g_opt_per_thread ? p->opt : nullptr);
#else
    int  inc                = nn_scale(board, psqt, positional, rule50, &p->last,
                                       g_opt_per_thread ? p->opt : nullptr);
#endif

    if (g_verify) {
        // Confronto con un refresh completo della stessa scacchiera, su uno stato di appoggio: oltre al valore
        // finale si confrontano i due accumulatori interi (entrambe le prospettive, PSQT compresa).
        ScratchEval& s = scratch_eval();
        // Stesso optimism del valore incrementale (OptPerThread), o il confronto dei valori finali non vale.
        int           full    = eval_full(board, rule50, *s.acc, *s.cch, nullptr, g_opt_per_thread ? p->opt : nullptr);
        const auto&   a       = p->accStack->latest();
        const auto&   b       = s.acc->latest();
        const bool    accSame = std::memcmp(&a.accumulation, &b.accumulation, sizeof(a.accumulation)) == 0
                             && std::memcmp(&a.psqtAccumulation, &b.psqtAccumulation, sizeof(a.psqtAccumulation)) == 0;
        if (inc != full || !accSame) {
            static int reported = 0;
            g_verify_bad++;
            if (reported++ < 64)
                std::printf("info string NNUE MISMATCH stati=%d inc=%d full=%d acc=%s\n",
                            p->accStack->dirty_stack().size, inc, full, accSame ? "uguale" : "DIVERSO");
            std::fflush(stdout);
        }
        g_verify_count++;
    }
    return inc;
}

// Quante valutazioni ha confrontato il controllo (nnueverify) e quante erano diverse, per i rapporti dei test.
unsigned long long nn_verify_count(void) { return g_verify_count; }
unsigned long long nn_verify_bad(void) { return g_verify_bad; }

// Stateless full-refresh eval ("eval" command): la scacchiera globale del motore (bitboard per pezzo e occupazioni,
// a8 = 0), senza conversioni; la mailbox si ricava qui dai bitboard. Single-threaded (UI/debug only).
// `raw_out` (opzionale): uscita GREZZA della rete, psqt + positional, PRIMA di nn_scale.
// Serve al cross-check contro il trainer: il valore di ritorno passa per nn_scale, che
// applica blend psqt/positional, smorzamento per complessita', scaling per materiale e
// smorzamento rule50 — nessuna delle quali esiste nel forward del trainer. Confrontare il
// ritorno col trainer paragona due grandezze diverse: misurato R^2 0.73 anche su una coppia
// motore+rete NOTA-BUONA (6.0 + v3), con gli errori massimi tutti su posizioni ad alto
// contatore 50-mosse. (27/07/2026)
int nn_eval(int side_white, const unsigned long long* bb, const unsigned long long* occ, int rule50,
            int* raw_out) {
    int mb[64];
    for (int s = 0; s < 64; ++s)
        mb[s] = -1;
    for (int pc = 0; pc < 12; ++pc)
        for (Bitboard b = bb[pc]; b;)
            mb[pop_lsb(b)] = pc;
    const NnBoard board(bb, occ, mb, side_white ? WHITE : BLACK);
    ScratchEval&  s = scratch_eval();
    return eval_full(board, rule50, *s.acc, *s.cch, raw_out);
}

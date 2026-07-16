// mens_dump: load the TRANN1 net, evaluate FEN positions with the master Network,
// and dump REAL data for the "Mens" visualizer as one JSON line per position:
//   - eval (psqt+positional, stm-relative internal units)
//   - 1024 first-layer accumulator activations (both perspectives)
//   - L2 (31) and L3 (32) layer activations + active output bucket
//   - 64-square per-piece contribution table via ablation (eval - eval_without_piece)
//
// Modes:
//   mens_dump <net.nnue> "<FEN>"   one-shot (prints one line, exits)
//   mens_dump <net.nnue>           REPL: loads net once, reads one FEN per stdin line,
//                                  prints one MENS_JSON line each (for the live server)
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "bitboard.h"
#include "position.h"
#include "types.h"

#include "nnue/network.h"
#include "nnue/nnue_accumulator.h"
#include "nnue/nnue_misc.h"

using namespace Triumviratus;

static Piece piece_from_char(char c) {
    switch (c) {
        case 'P': return W_PAWN;   case 'N': return W_KNIGHT; case 'B': return W_BISHOP;
        case 'R': return W_ROOK;   case 'Q': return W_QUEEN;  case 'K': return W_KING;
        case 'p': return B_PAWN;   case 'n': return B_KNIGHT; case 'b': return B_BISHOP;
        case 'r': return B_ROOK;   case 'q': return B_QUEEN;  case 'k': return B_KING;
        default:  return NO_PIECE;
    }
}
static bool is_king(Piece p) { return p == W_KING || p == B_KING; }

struct Ctx {
    Eval::NNUE::Network*          net;
    Eval::NNUE::AccumulatorStack* acc;
    Eval::NNUE::AccumulatorCaches* caches;
};

static int eval_list(Ctx& cx, const std::vector<Piece>& P, const std::vector<Square>& S, Color side) {
    StateInfo si;
    Position  pos;
    pos.set_pieces(P.data(), S.data(), int(P.size()), side, &si);
    cx.acc->reset();
    auto [psqt, positional] = cx.net->evaluate(pos, *cx.acc, *cx.caches);
    return int(psqt) + int(positional);
}

static void process_fen(Ctx& cx, const std::string& fen) {
    std::vector<Piece>  pcs;
    std::vector<Square> sqs;
    int    rank = 7, file = 0;
    size_t i = 0;
    for (; i < fen.size() && fen[i] != ' '; ++i) {
        char ch = fen[i];
        if (ch == '/') { rank--; file = 0; }
        else if (ch >= '1' && ch <= '8') file += ch - '0';
        else { Piece p = piece_from_char(ch); if (p != NO_PIECE) { pcs.push_back(p); sqs.push_back(Square(rank * 8 + file)); } ++file; }
    }
    Color stm = WHITE;
    while (i < fen.size() && fen[i] == ' ') ++i;
    if (i < fen.size() && fen[i] == 'b') stm = BLACK;
    if (pcs.empty()) { std::printf("MENS_JSON {\"error\":\"bad fen\"}\n"); std::fflush(stdout); return; }

    StateInfo si0;
    Position  pos0;
    pos0.set_pieces(pcs.data(), sqs.data(), int(pcs.size()), stm, &si0);
    cx.acc->reset();
    auto tr   = cx.net->mens_trace(pos0, *cx.acc, *cx.caches);
    int  full = tr.positional + tr.psqt;

    std::vector<int> accW, accB;
    {
        const auto& aw = cx.acc->latest().accumulation[WHITE];
        const auto& ab = cx.acc->latest().accumulation[BLACK];
        accW.resize(aw.size()); accB.resize(ab.size());
        for (size_t k = 0; k < aw.size(); ++k) accW[k] = int(aw[k]);
        for (size_t k = 0; k < ab.size(); ++k) accB[k] = int(ab[k]);
    }
    const int n2 = int(sizeof(tr.l2) / sizeof(tr.l2[0]));
    const int n3 = int(sizeof(tr.l3) / sizeof(tr.l3[0]));
    std::vector<int> l2(tr.l2, tr.l2 + n2), l3(tr.l3, tr.l3 + n3);

    int contrib[64] = {0};
    for (size_t k = 0; k < pcs.size(); ++k) {
        if (is_king(pcs[k])) continue;
        std::vector<Piece>  P; P.reserve(pcs.size() - 1);
        std::vector<Square> S; S.reserve(sqs.size() - 1);
        for (size_t j = 0; j < pcs.size(); ++j) if (j != k) { P.push_back(pcs[j]); S.push_back(sqs[j]); }
        contrib[int(sqs[k])] = full - eval_list(cx, P, S, stm);   // >0 helps side-to-move
    }

    auto emit_arr = [](std::string& o, const std::vector<int>& v) {
        for (size_t k = 0; k < v.size(); ++k) { o += std::to_string(v[k]); if (k + 1 < v.size()) o += ","; }
    };
    std::string out = "MENS_JSON {";
    out += "\"stm\":\"" + std::string(stm == WHITE ? "w" : "b") + "\",";
    out += "\"evalStm\":" + std::to_string(full) + ",";
    out += "\"psqt\":" + std::to_string(tr.psqt) + ",";
    out += "\"bucket\":" + std::to_string(tr.bucket) + ",";
    out += "\"l2\":["; emit_arr(out, l2);
    out += "],\"l3\":["; emit_arr(out, l3);
    out += "],\"accW\":["; emit_arr(out, accW);
    out += "],\"accB\":["; emit_arr(out, accB);
    out += "],\"contrib\":[";
    for (int k = 0; k < 64; ++k) { out += std::to_string(contrib[k]); if (k + 1 < 64) out += ","; }
    out += "]}";
    std::printf("%s\n", out.c_str());
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <net.nnue> [\"<FEN>\"]   (no FEN = REPL, one FEN per stdin line)\n", argv[0]);
        return 1;
    }
    Bitboards::init();
    Attacks::init();

    Eval::NNUE::EvalFile ef{};
    auto net = std::make_unique<Eval::NNUE::Network>(ef);
    net->load(".", argv[1]);
    net->verify(argv[1], [&](std::string_view s) { std::fprintf(stderr, "VERIFY: %.*s\n", int(s.size()), s.data()); });

    auto acc    = std::make_unique<Eval::NNUE::AccumulatorStack>();
    auto caches = std::make_unique<Eval::NNUE::AccumulatorCaches>(*net);
    Ctx cx{net.get(), acc.get(), caches.get()};

    if (argc >= 3) { process_fen(cx, argv[2]); return 0; }   // one-shot

    std::fprintf(stderr, "[mens_dump REPL ready]\n"); std::fflush(stderr);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") break;
        if (line.empty()) continue;
        process_fen(cx, line);
    }
    return 0;
}

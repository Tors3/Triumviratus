// M1 eval-match validator: load the TRANN1 net, build a Position from a FEN via
// the minimal set_pieces(), run the master Network::evaluate(), print the raw NNUE
// value (psqt+positional, stm-relative internal units). Compare vs SF master `eval`.
#include <cstdio>
#include <memory>
#include <string>
#include <tuple>
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

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <net.nnue> \"<FEN>\"\n", argv[0]);
        return 1;
    }
    Bitboards::init();
    Attacks::init();   // slider magics live in attacks.cpp (NOT Bitboards::init) — required for threats

    Eval::NNUE::EvalFile ef{};
    auto net = std::make_unique<Eval::NNUE::Network>(ef);  // ~88MB inline weights -> HEAP
    net->load(".", argv[1]);
    bool ok = true;
    net->verify(argv[1], [&](std::string_view s) { ok = false; std::printf("VERIFY: %.*s\n", int(s.size()), s.data()); });
    std::printf("[net loaded, verify_ok=%d]\n", ok ? 1 : 0); std::fflush(stdout);

    // --- parse FEN placement + side to move into an SF piece list ---
    const std::string fen = argv[2];
    std::vector<Piece>  pcs;
    std::vector<Square> sqs;
    int    rank = 7, file = 0;
    size_t i    = 0;
    for (; i < fen.size() && fen[i] != ' '; ++i) {
        char ch = fen[i];
        if (ch == '/') { rank--; file = 0; }
        else if (ch >= '1' && ch <= '8') file += ch - '0';
        else {
            Piece p = piece_from_char(ch);
            if (p != NO_PIECE) { pcs.push_back(p); sqs.push_back(Square(rank * 8 + file)); }
            ++file;
        }
    }
    Color stm = WHITE;
    while (i < fen.size() && fen[i] == ' ') ++i;
    if (i < fen.size() && fen[i] == 'b') stm = BLACK;

    StateInfo si;
    Position  pos;
    pos.set_pieces(pcs.data(), sqs.data(), int(pcs.size()), stm, &si);

    std::printf("[pos set, pieces=%d]\n", int(pcs.size())); std::fflush(stdout);
    auto acc    = std::make_unique<Eval::NNUE::AccumulatorStack>();
    acc->reset();
    auto caches = std::make_unique<Eval::NNUE::AccumulatorCaches>(*net);
    std::printf("[acc+caches ready, evaluating]\n"); std::fflush(stdout);
    auto [psqt, positional] = net->evaluate(pos, *acc, *caches);
    int v = int(psqt) + int(positional);  // stm-relative, internal units (== SF "NNUE evaluation")

    std::printf("psqt=%d positional=%d  NNUE_internal_stm=%d\n", int(psqt), int(positional), v);
    return 0;
}

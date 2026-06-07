#include "policy_bridge.h"
#include "defs.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm> // per std::max

// Dimensioni Architettura CNN (DEVE combaciare con PolicyNet in
// train_memmap_resume.py / estrai_bin.py):
//   Conv(12->64) -> Conv(64->128) -> Conv(128->128) -> Conv(128->128)
//   -> PolicyHead: Conv1x1(128->128)+ReLU -> Conv1x1(128->64)
// La head produce CH_HO canali "to" su una mappa spaziale 8x8 = casa "from",
// per un output di 64*64 = 4096 logit con indice (from)*64 + (to).
#define CH_IN   12
#define CH_C1   64
#define CH_C2   128
#define CH_C3   128
#define CH_C4   128
#define CH_HH   128   // head hidden channels
#define CH_HO   64    // head output channels (= caselle "to")
#define OUT_SIZE 4096

// Spazio in memoria per i pesi (globale)
static float conv1_w[CH_C1 * CH_IN * 3 * 3];
static float conv1_b[CH_C1];

static float conv2_w[CH_C2 * CH_C1 * 3 * 3];
static float conv2_b[CH_C2];

static float conv3_w[CH_C3 * CH_C2 * 3 * 3];
static float conv3_b[CH_C3];

static float conv4_w[CH_C4 * CH_C3 * 3 * 3];
static float conv4_b[CH_C4];

// Policy head a due strati 1x1 (pesi PyTorch [out,in,1,1] -> w[co*in + ci]).
static float hh_w[CH_HH * CH_C4];   // hidden 128->128
static float hh_b[CH_HH];
static float ho_w[CH_HO * CH_HH];   // out 128->64
static float ho_b[CH_HO];

// Set true at the end of a successful load_policy; consumed by policy_loaded().
static bool g_policy_loaded = false;
bool policy_loaded() { return g_policy_loaded; }

void init_policy() {
    g_policy_loaded = false;
    std::memset(conv1_w, 0, sizeof(conv1_w)); std::memset(conv1_b, 0, sizeof(conv1_b));
    std::memset(conv2_w, 0, sizeof(conv2_w)); std::memset(conv2_b, 0, sizeof(conv2_b));
    std::memset(conv3_w, 0, sizeof(conv3_w)); std::memset(conv3_b, 0, sizeof(conv3_b));
    std::memset(conv4_w, 0, sizeof(conv4_w)); std::memset(conv4_b, 0, sizeof(conv4_b));
    std::memset(hh_w,    0, sizeof(hh_w));    std::memset(hh_b,    0, sizeof(hh_b));
    std::memset(ho_w,    0, sizeof(ho_w));    std::memset(ho_b,    0, sizeof(ho_b));
}

bool load_policy(const char* filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) return false;

    // Validazione dimensione: il .bin DEVE corrispondere ESATTAMENTE
    // all'architettura compilata, altrimenti leggeremmo pesi spazzatura
    // (rete di architettura sbagliata) -> policy che peggiora il gioco in
    // silenzio. Se non combacia, rifiutiamo (init_policy ha gia' azzerato i
    // pesi -> policy disattivata, gioco corretto).
    const std::streamsize expected =
        (std::streamsize)(sizeof(conv1_w) + sizeof(conv1_b) +
                          sizeof(conv2_w) + sizeof(conv2_b) +
                          sizeof(conv3_w) + sizeof(conv3_b) +
                          sizeof(conv4_w) + sizeof(conv4_b) +
                          sizeof(hh_w) + sizeof(hh_b) +
                          sizeof(ho_w) + sizeof(ho_b));
    file.seekg(0, std::ios::end);
    std::streamsize actual = file.tellg();
    file.seekg(0, std::ios::beg);
    if (actual != expected) {
        printf("info string Policy net SCARTATA: dimensione %lld byte, attesi %lld "
               "(architettura non corrispondente).\n",
               (long long)actual, (long long)expected);
        return false;
    }

    file.read(reinterpret_cast<char*>(conv1_w), sizeof(conv1_w));
    file.read(reinterpret_cast<char*>(conv1_b), sizeof(conv1_b));

    file.read(reinterpret_cast<char*>(conv2_w), sizeof(conv2_w));
    file.read(reinterpret_cast<char*>(conv2_b), sizeof(conv2_b));

    file.read(reinterpret_cast<char*>(conv3_w), sizeof(conv3_w));
    file.read(reinterpret_cast<char*>(conv3_b), sizeof(conv3_b));

    file.read(reinterpret_cast<char*>(conv4_w), sizeof(conv4_w));
    file.read(reinterpret_cast<char*>(conv4_b), sizeof(conv4_b));

    file.read(reinterpret_cast<char*>(hh_w), sizeof(hh_w));
    file.read(reinterpret_cast<char*>(hh_b), sizeof(hh_b));

    file.read(reinterpret_cast<char*>(ho_w), sizeof(ho_w));
    file.read(reinterpret_cast<char*>(ho_b), sizeof(ho_b));

    file.close();
    g_policy_loaded = true;
    return true;
}

// Funzione helper per l'operazione di convoluzione 2D con Padding=1 e ReLU
static inline void conv2d_3x3_pad1_relu(const float* input, float* output, const float* weights, const float* bias, int in_channels, int out_channels) {
    for (int co = 0; co < out_channels; ++co) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                float sum = bias[co];
                for (int ci = 0; ci < in_channels; ++ci) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            int ny = y + dy;
                            int nx = x + dx;
                            // Controllo dei bordi per il padding 1 (zero-padding)
                            if (ny >= 0 && ny < 8 && nx >= 0 && nx < 8) {
                                int w_idx = co * (in_channels * 9) + ci * 9 + (dy + 1) * 3 + (dx + 1);
                                sum += input[ci * 64 + ny * 8 + nx] * weights[w_idx];
                            }
                        }
                    }
                }
                // Attiva ReLU usando (std::max) per evitare il conflitto con la macro di Windows
                output[co * 64 + y * 8 + x] = (std::max)(0.0f, sum);
            }
        }
    }
}

void evaluate_policy(ThreadData& td, float* output_scores) {
    // Array allocati nello stack locale del thread, thread-safe!
    alignas(32) float input_features[CH_IN * 64];
    alignas(32) float map1[CH_C1 * 64];
    alignas(32) float map2[CH_C2 * 64];
    alignas(32) float map3[CH_C3 * 64];
    alignas(32) float map4[CH_C4 * 64];
    alignas(32) float hid [CH_HH * 64];

    std::memset(input_features, 0, sizeof(input_features));

    // 1. Costruzione Tensore Input (12x8x8) in codifica CANONICA sul lato che
    //    muove (fonte unica: chess_encoding.py, ENCODING_VERSION = v2_canonical_stm):
    //      - piani 0..5  = pezzi di CHI MUOVE ("us"); 6..11 = avversario ("them").
    //      - Bianco al tratto: csq = cpp_sq ^ 56 ; plane = piece
    //      - Nero   al tratto: csq = cpp_sq      ; plane = (piece + 6) % 12
    //    (dim.: square_mirror(py) = py ^ 56 e py = cpp_sq ^ 56  =>  csq_nero = cpp_sq)
    //    Cosi' la rete vede SEMPRE "us" in basso: coerente tra Bianco e Nero.
    const bool white_to_move = (td.side == white);
    for (int piece = P; piece <= k; piece++) {
        U64 bb = td.bitboards[piece];
        int plane = white_to_move ? piece : ((piece + 6) % 12);
        while (bb) {
            int cpp_sq = get_ls1b_index(bb);
            int csq = white_to_move ? (cpp_sq ^ 56) : cpp_sq;
            input_features[plane * 64 + csq] = 1.0f;
            pop_bit(bb, cpp_sq);
        }
    }

    // 2. Trunk convoluzionale: 4 conv 3x3 (ognuno con ReLU) -> campo recettivo 9x9
    conv2d_3x3_pad1_relu(input_features, map1, conv1_w, conv1_b, CH_IN, CH_C1);
    conv2d_3x3_pad1_relu(map1, map2, conv2_w, conv2_b, CH_C1, CH_C2);
    conv2d_3x3_pad1_relu(map2, map3, conv3_w, conv3_b, CH_C2, CH_C3);
    conv2d_3x3_pad1_relu(map3, map4, conv4_w, conv4_b, CH_C3, CH_C4);

    // 3. Policy head, strato NASCOSTO: Conv1x1 (CH_C4 -> CH_HH) + ReLU.
    for (int sq = 0; sq < 64; ++sq) {
        for (int co = 0; co < CH_HH; ++co) {
            float sum = hh_b[co];
            for (int ci = 0; ci < CH_C4; ++ci) {
                sum += map4[ci * 64 + sq] * hh_w[co * CH_C4 + ci];
            }
            hid[co * 64 + sq] = (std::max)(0.0f, sum);
        }
    }

    // 4. Policy head, strato OUT: Conv1x1 (CH_HH -> CH_HO), NESSUNA ReLU (logits).
    //    output index (sq*64 + co) == (from_canon)*64 + (to_canon). La ricerca
    //    (td_sort_moves) calcola p_idx con la STESSA codifica canonica:
    //      Bianco: (sq ^ 56) ; Nero: sq.
    for (int sq = 0; sq < 64; ++sq) {
        for (int co = 0; co < CH_HO; ++co) {
            float sum = ho_b[co];
            for (int ci = 0; ci < CH_HH; ++ci) {
                sum += hid[ci * 64 + sq] * ho_w[co * CH_HH + ci];
            }
            output_scores[sq * CH_HO + co] = sum;
        }
    }
}
#include "defs.h"
#include "io.h"
#include "movegen.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>

// print bitboard
void print_bitboard(U64 bitboard)
{
    printf("\n");

    for (int rank = 0; rank < 8; rank++)
    {
        for (int file = 0; file < 8; file++)
        {
            int square = rank * 8 + file;
            if (!file)
                printf("  %d ", 8 - rank);
            printf(" %d", get_bit(bitboard, square) ? 1 : 0);
        }
        printf("\n");
    }

    printf("\n     a b c d e f g h\n\n");
    printf("     Bitboard: %llud\n\n", bitboard);
}

// print board
void print_board()
{
    printf("\n");

    for (int rank = 0; rank < 8; rank++)
    {
        for (int file = 0; file < 8; file++)
        {
            int square = rank * 8 + file;
            if (!file)
                printf("  %d ", 8 - rank);

            int piece = -1;

            for (int bb_piece = P; bb_piece <= k; bb_piece++)
            {
                if (get_bit(bitboards[bb_piece], square))
                    piece = bb_piece;
            }

            printf(" %c", (piece == -1) ? '.' : ascii_pieces[piece]);
        }
        printf("\n");
    }

    printf("\n     a b c d e f g h\n\n");
    printf("     Side:     %s\n", !side ? "white" : "black");
    printf("     Enpassant:   %s\n", (enpassant != no_sq) ? square_to_coordinates[enpassant] : "no");
    printf("     Castling:  %c%c%c%c\n\n", (castle & wk) ? 'K' : '-',
        (castle & wq) ? 'Q' : '-',
        (castle & bk) ? 'k' : '-',
        (castle & bq) ? 'q' : '-');
    printf("     Hash key:  %llx\n", hash_key);
    printf("     Fifty move: %d\n\n", fifty);
}

// Build a FEN string for the current global board state. Ranks are emitted
// 8->1 (square 0 = a8, matching print_board). The fullmove number is not
// tracked by the engine, so a placeholder "1" is used (irrelevant for the
// position itself / for training).
std::string board_to_fen()
{
    std::string fen;
    for (int rank = 0; rank < 8; rank++)
    {
        int empty = 0;
        for (int file = 0; file < 8; file++)
        {
            int square = rank * 8 + file;
            int piece = -1;
            for (int bb = P; bb <= k; bb++)
                if (get_bit(bitboards[bb], square)) { piece = bb; break; }

            if (piece == -1)
                empty++;
            else
            {
                if (empty) { fen += char('0' + empty); empty = 0; }
                fen += ascii_pieces[piece];
            }
        }
        if (empty) fen += char('0' + empty);
        if (rank < 7) fen += '/';
    }

    fen += ' ';
    fen += (side == white) ? 'w' : 'b';

    fen += ' ';
    std::string cr;
    if (castle & wk) cr += 'K';
    if (castle & wq) cr += 'Q';
    if (castle & bk) cr += 'k';
    if (castle & bq) cr += 'q';
    fen += cr.empty() ? "-" : cr.c_str();

    fen += ' ';
    fen += (enpassant != no_sq) ? square_to_coordinates[enpassant] : "-";

    fen += ' ';
    fen += std::to_string(fifty);
    fen += " 1";   // fullmove placeholder
    return fen;
}

// Serialize a move to UCI long-algebraic notation ("e2e4", "e7e8q").
std::string move_to_uci(int move)
{
    std::string s;
    s += square_to_coordinates[get_move_source(move)];
    s += square_to_coordinates[get_move_target(move)];
    int promo = get_move_promoted(move);
    if (promo) s += char(tolower(ascii_pieces[promo]));
    return s;
}

// reset board variables
void reset_board()
{
    memset(bitboards, 0ULL, sizeof(bitboards));
    memset(occupancies, 0ULL, sizeof(occupancies));
    side = 0;
    enpassant = no_sq;
    castle = 0;
    repetition_index = 0;
    fifty = 0;
    memset(repetition_table, 0ULL, sizeof(repetition_table));
}

// parse FEN string - IMPROVED to parse halfmove clock
void parse_fen(const char* fen)
{
    reset_board();

    for (int rank = 0; rank < 8; rank++)
    {
        for (int file = 0; file < 8; file++)
        {
            int square = rank * 8 + file;

            if ((*fen >= 'a' && *fen <= 'z') || (*fen >= 'A' && *fen <= 'Z'))
            {
                int piece = mapCharToPiece(*fen);
                set_bit(bitboards[piece], square);
                fen++;
            }

            if (*fen >= '0' && *fen <= '9')
            {
                int offset = *fen - '0';
                int piece = -1;

                for (int bb_piece = P; bb_piece <= k; bb_piece++)
                {
                    if (get_bit(bitboards[bb_piece], square))
                        piece = bb_piece;
                }

                if (piece == -1)
                    file--;

                file += offset;
                fen++;
            }

            if (*fen == '/')
                fen++;
        }
    }

    // Skip space and parse side to move
    fen++;
    (*fen == 'w') ? (side = white) : (side = black);
    fen += 2;

    // Parse castling rights
    while (*fen != ' ')
    {
        switch (*fen)
        {
        case 'K': castle |= wk; break;
        case 'Q': castle |= wq; break;
        case 'k': castle |= bk; break;
        case 'q': castle |= bq; break;
        case '-': break;
        }
        fen++;
    }

    // Skip space and parse en passant square
    fen++;

    if (*fen != '-')
    {
        int file = fen[0] - 'a';
        int rank = 8 - (fen[1] - '0');
        enpassant = rank * 8 + file;
        fen += 2;
    }
    else
    {
        enpassant = no_sq;
        fen++;
    }

    // Skip space and parse halfmove clock (fifty-move counter)
    if (*fen == ' ')
    {
        fen++;
        // Parse halfmove clock
        if (*fen >= '0' && *fen <= '9')
        {
            fifty = atoi(fen);
            // Skip past the number
            while (*fen >= '0' && *fen <= '9') fen++;
        }
    }

    // Skip space and parse fullmove number (we don't use this but parse it anyway)
    if (*fen == ' ')
    {
        fen++;
        // Skip fullmove number
        while (*fen >= '0' && *fen <= '9') fen++;
    }

    // Build occupancy bitboards
    for (int piece = P; piece <= K; piece++)
        occupancies[white] |= bitboards[piece];

    for (int piece = p; piece <= k; piece++)
        occupancies[black] |= bitboards[piece];

    occupancies[both] |= occupancies[white];
    occupancies[both] |= occupancies[black];

    // Generate hash key
    hash_key = generate_hash_key();
}

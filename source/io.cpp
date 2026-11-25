// Origin: this file descends from BBC (Bit Board Chess) by Maksim Korzh, https://github.com/maksimKorzh/bbc,
// released under the GNU General Public License v3; the project started from BBC in 2024 and has since rewritten
// and extended much of it. Triumviratus is distributed under the GNU GPL v3 (see COPYING).
#include "defs.h"
#include "chess960.h"
#include "attacks.h" // A3: pawn_attacks, for the phantom e.p. filter
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
    if (g_chess960)
    {
        // Squares read from the FEN (see chess960.h): white/black king, rooks for K Q k q.
        printf("     Chess960:  re %s/%s, torri", g_c960.king_from[0] != no_sq ? square_to_coordinates[g_c960.king_from[0]] : "-",
               g_c960.king_from[1] != no_sq ? square_to_coordinates[g_c960.king_from[1]] : "-");
        for (int i = 0; i < 4; i++)
            printf(" %s", g_c960.rook_from[i] != no_sq ? square_to_coordinates[g_c960.rook_from[i]] : "-");
        printf("\n\n");
    }
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
    if (g_chess960)
    {
        // Shredder-FEN: the rook's file (KQkq would be ambiguous with two rooks on the same side)
        const char base[4] = {'A', 'A', 'a', 'a'};
        for (int i = 0; i < 4; i++)
            if (castle & c960_bit(i)) cr += char(base[i] + (castle_rook_sq[i] & 7));
    }
    else
    {
        if (castle & wk) cr += 'K';
        if (castle & wq) cr += 'Q';
        if (castle & bk) cr += 'k';
        if (castle & bq) cr += 'q';
    }
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

// Game ply of the given position (from the FEN move number), for the time management (TmFenPly).
int g_fen_ply = 0;

// parse FEN string - IMPROVED to parse halfmove clock
void parse_fen(const char* fen)
{
    reset_board();
    g_fen_ply = 0;

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
    // Chess960 (chess960.h): X-FEN or Shredder-FEN, king and rook squares read from the board and carried into the
    // engine. In normal chess the usual squares come back (h1/a1/h8/a8, fixed mask).
    if (g_chess960)
    {
        const char* field = fen;
        while (*fen != ' ' && *fen) fen++;
        c960_parse_castling(field, fen);
        castle = g_c960.rights;
        c960_apply_to_engine();
    }
    else
    {
        c960_reset_standard();
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
    }

    // Skip space and parse en passant square
    fen++;

    if (*fen != '-')
    {
        int file = fen[0] - 'a';
        int rank = 8 - (fen[1] - '0');
        // AUDIT D (26/09/2026): an en passant square inconsistent with the side to move (e3 with White to move)
        // made a friendly pawn "capture en passant" and crashed the engine. It is ignored, as SF does.
        const bool ep_ok = (side == white && fen[1] == '6') || (side == black && fen[1] == '3');
        enpassant = ep_ok ? rank * 8 + file : no_sq;
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

    // Move number: the time management uses it as game ply (TmFenPly, 05/10/2026, core audit):
    // from .epd books (UHO "... 0 9") the game does not start at ply 0. Same rule as SF: 2*(fullmove-1) + black.
    if (*fen == ' ')
    {
        fen++;
        int fullmove = 0;
        while (*fen >= '0' && *fen <= '9') fullmove = fullmove * 10 + (*fen++ - '0');
        if (fullmove > 1)
            g_fen_ply = 2 * (fullmove - 1);
    }
    g_fen_ply += side == black;

    // Build occupancy bitboards
    for (int piece = P; piece <= K; piece++)
        occupancies[white] |= bitboards[piece];

    for (int piece = p; piece <= k; piece++)
        occupancies[black] |= bitboards[piece];

    occupancies[both] |= occupancies[white];
    occupancies[both] |= occupancies[black];

    // A3 FIX 2026-07-25: the FEN is a trust boundary (it comes from the GUI, from the
    // books, from the datagen scripts) and it was not validated at all. An
    // illegal FEN is not just "wrong", it corrupts memory:
    //   - >32 pieces -> nn_build_piece_list writes beyond pieces[33]/squares[33]
    //     and the same unbounded loop in debug_eval_position is reachable from the "eval" command;
    //   - missing king -> get_ls1b_index(0) = -1 used as a square index
    //     (e.g. in the search in search/*.inc and in misc.cpp).
    // Here it is rejected and we go back to the starting position: the recursion ends
    // at once because start_position is valid by construction.
    if (count_bits(bitboards[K]) != 1 || count_bits(bitboards[k]) != 1 ||
        count_bits(occupancies[both]) > 32)
    {
        printf("info string FEN rifiutata (re mancante/duplicato o piu' di 32 pezzi): uso la posizione iniziale\n");
        fflush(stdout);
        parse_fen(start_position);
        return;
    }

    // 21/10/2026: the side NOT to move must not be in check either. Otherwise the side to move can capture the king
    // (the generator produces the move and the make accepts it), and one ply later the board has a missing king, the
    // state rejected above: get_ls1b_index(0) = -1 as a king square in the generator and the make, keys and occupancies
    // out of step, the network reading rows outside its tables. Found with nnperft on 4k3/8/8/3qq3/8/8/8/QQ2K3 b (the
    // queen on e5 checks e1 with Black to move): accumulator mismatches, then an access violation in consilium/cn.cpp.
    if (is_square_attacked(get_ls1b_index(bitboards[side == white ? k : K]), side))
    {
        printf("info string FEN rifiutata (il lato che non ha il tratto e' sotto scacco): uso la posizione iniziale\n");
        fflush(stdout);
        parse_fen(start_position);
        return;
    }

    // Phantom e.p.: if no pawn of the side to move can really capture
    // on the declared square, that square changes the Zobrist key without changing
    // the position -> the same position gets two keys and the TT does not
    // recognise it (measured: 22566 vs 21406 nodes with a warm TT). SF filters in the same
    // way in position.cpp. pawn_attacks[side^1][ep] = where a pawn of the side
    // to move would capture on ep from.
    if (enpassant != no_sq &&
        !(pawn_attacks[side ^ 1][enpassant] & bitboards[side == white ? P : p]))
        enpassant = no_sq;
    // Audit 25/11/2026: the square must also be one a double push can have left behind: the opponent's pawn on the
    // square it reached, the passed square and the pawn's start square empty. Otherwise the en passant capture removed
    // a pawn that was not there, or put two pieces on one square ("4k3/8/8/4P3/8/8/8/4K3 w - d6 0 1" was played e5d6).
    if (enpassant != no_sq) {
        U64 all = 0;
        for (int pc = P; pc <= k; pc++)
            all |= bitboards[pc];
        const int reached = side == white ? enpassant + 8 : enpassant - 8;   // a8 = 0: White captures towards row 0
        const int start = side == white ? enpassant - 8 : enpassant + 8;
        if (!(bitboards[side == white ? p : P] >> reached & 1) || (all >> enpassant & 1) || (all >> start & 1))
            enpassant = no_sq;
    }

    // AUDIT D (27/09/2026): castling rights inconsistent with the board. The engine knows only classic
    // castling (king on e1/e8, rooks in the corners); a Chess960 or wrong FEN with "KQkq" and the king elsewhere
    // made castling be generated from the wrong squares. Only the right the position makes
    // possible is kept (the Shredder letters A-H were already ignored by the switch above). Normal positions: unchanged.
    // With 960 on the equivalent check is done by c960_parse_castling.
    const int castle_in = castle;
    if (!g_chess960)
    {
        if (!get_bit(bitboards[K], e1)) castle &= ~(wk | wq);
        if (!get_bit(bitboards[R], h1)) castle &= ~wk;
        if (!get_bit(bitboards[R], a1)) castle &= ~wq;
        if (!get_bit(bitboards[k], e8)) castle &= ~(bk | bq);
        if (!get_bit(bitboards[r], h8)) castle &= ~bk;
        if (!get_bit(bitboards[r], a8)) castle &= ~bq;
    }
    if (castle != castle_in)
    {
        printf("info string diritti d'arrocco incoerenti con la scacchiera (per il Chess960: UCI_Chess960): ignorati\n");
        fflush(stdout);
    }

    // Generate hash key
    hash_key = generate_hash_key();
}

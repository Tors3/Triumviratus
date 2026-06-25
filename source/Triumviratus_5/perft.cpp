#include "defs.h"
#include "misc.h"
#include "movegen.h"
#include "perft.h"

extern U64 nodes;

// perft driver
void perft_driver(int depth)
{
    if (depth == 0)
    {
        nodes++;
        return;
    }

    moves move_list[1];
    generate_moves(move_list);

    for (int move_count = 0; move_count < move_list->count; move_count++)
    {
        copy_board();

        if (!make_move(move_list->moves[move_count], all_moves))
            continue;

        perft_driver(depth - 1);
        take_back();
    }
}

// perft test
void perft_test(int depth)
{
    printf("\nPerformance test\n\n");

    moves move_list[1];
    generate_moves(move_list);

    int start = get_time_ms();

    for (int move_count = 0; move_count < move_list->count; move_count++)
    {
        copy_board();

        if (!make_move(move_list->moves[move_count], all_moves))
            continue;

        U64 cummulative_nodes = nodes;
        perft_driver(depth - 1);
        U64 old_nodes = nodes - cummulative_nodes;

        take_back();

        printf("move: %s%s%c  nodes: %llu\n", square_to_coordinates[get_move_source(move_list->moves[move_count])],
            square_to_coordinates[get_move_target(move_list->moves[move_count])],
            get_move_promoted(move_list->moves[move_count]) ? mapPieceToPromotion(get_move_promoted(move_list->moves[move_count])) : ' ',
            old_nodes);
    }

    printf("\n    Depth: %d\n", depth);
    printf("    Nodes: %llu\n", nodes);
    printf("     Time: %d\n\n", get_time_ms() - start);
}

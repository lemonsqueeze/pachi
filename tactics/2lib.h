#ifndef PACHI_TACTICS_2LIB_H
#define PACHI_TACTICS_2LIB_H

/* Two-liberty tactical checks (i.e. dealing with two-step capturing races,
 * preventing atari). */

#include "board.h"
#include "debug.h"

void can_atari_group(board_t *b, group_t group, enum stone owner, enum stone to_play, move_queue_t *q, int tag, bool use_def_no_hopeless);
void group_2lib_check(board_t *b, group_t group, enum stone to_play, move_queue_t *q, int tag, bool use_miaisafe, bool use_def_no_hopeless);

bool can_capture_2lib_group(board_t *b, group_t g, move_queue_t *q, int tag);
void group_2lib_capture_check(board_t *b, group_t group, enum stone to_play, move_queue_t *q, int tag, bool use_miaisafe, bool use_def_no_hopeless);

/* Returns 0 or ID of neighboring group with 2 libs. */
static group_t board_get_2lib_neighbor(board_t *b, coord_t c, enum stone color);


static inline group_t
board_get_2lib_neighbor(board_t *b, coord_t c, enum stone color)
{
	foreach_neighbor(b, c, {
		group_t g = group_at(b, c);
		if (board_at(b, c) == color && board_group_info(b, g).libs == 2)
			return g;
	});
	return 0;
}


#endif

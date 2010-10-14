#ifndef ZZGO_TACTICS_2LIB_H
#define ZZGO_TACTICS_2LIB_H

/* Two-liberty tactical checks (i.e. dealing with two-step capturing races,
 * preventing atari). */

#include "board.h"
#include "debug.h"

struct move_queue;

void group_2lib_check(struct board *b, group_t group, enum stone to_play, struct move_queue *q, int tag);

#endif

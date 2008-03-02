#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "engine.h"
#include "move.h"
#include "montecarlo/internal.h"
#include "montecarlo/hint.h"
#include "montecasino/montecasino.h"
#include "random.h"


/* This is a monte-carlo-based engine with a feedback mechanism.
 * It is based on montecarlo/, but the algorithm works a little differently.
 * It plays MC_GAMES "random" games from the current board and records
 * win/loss ratio for each first TWO moves. From the first moves with
 * the biggest number of winning games, the one with the least lucrative
 * opponent response gets chosen. In a sense, if Monte Carlo looks one
 * move ahead, this algorithm looks two moves ahead. (And UCT would look
 * many moves ahead.) */
/* Note that while the library is based on New Zealand rules, this engine
 * returns moves according to Chinese rules. Thus, it does not return suicide
 * moves. It of course respects positional superko too. */

/* The arguments accepted are same as montecarlo's. Please see
 * montecarlo/montecarlo.c for the documentation. */
/* Note that YOU MUST PLAY MANY SIMULATIONS for montecasino to work well!
 * 50000 is probably the lowest sensible bound. */

/* How many games must be played for a move in order to trust it. */
#define TRUST_THRESHOLD 10

/* Slice of played-out games to play out initially. */
#define GAMES_SLICE_BASIC 8
/* Number of candidates looked at in more detail. */
#define CANDIDATES 10
/* Slice of played-out games to play out per candidate. */
#define GAMES_SLICE_CANDIDATE 8


/* We reuse large part of the code from the montecarlo/ engine. The
 * struct montecarlo internal state is part of our internal state. */

struct montecasino {
	struct montecarlo *carlo;
	int debug_level; // shortcut for carlo->debug_level
};


/* FIXME: Cutoff rule for simulations. Currently we are so fast that this
 * simply does not matter; even 100000 simulations are fast enough to
 * play 5 minutes S.D. on 19x19 and anything more sounds too ridiculous
 * already. */
/* FIXME: We cannot handle seki. Any good ideas are welcome. A possibility is
 * to consider 'pass' among the moves, but this seems tricky. */


/* TODO: Use the standard play_random_game(). */
/* 1: m->color wins, 0: m->color loses
 * -1 superko at the game root
 * -2 superko inside the game tree (NOT at root, that's simply invalid move)
 * -3 first move is multi-stone suicide */
static int
play_random_game(struct montecasino *mc, struct board *b, struct move_stat *moves,
		 struct move *m, int i)
{
	if (b->superko_violation) {
		if (MCDEBUGL(0)) {
			fprintf(stderr, "\tILLEGAL: superko violation at root!\n");
			board_print(b, stderr);
		}
		return -1;
	}

	struct board b2;
	board_copy(&b2, b);

	board_play_random(&b2, m->color, &m->coord);
	if (!is_pass(m->coord) && !group_at(&b2, m->coord)) {
		if (MCDEBUGL(4)) {
			fprintf(stderr, "SUICIDE DETECTED at %d,%d:\n", coord_x(m->coord, b), coord_y(m->coord, b));
			board_print(&b2, stderr);
		}
		board_done_noalloc(&b2);
		return -3;
	}

	if (MCDEBUGL(3))
		fprintf(stderr, "[%d,%d] playing random game\n", coord_x(m->coord, b), coord_y(m->coord, b));

	int gamelen = mc->carlo->gamelen - b2.moves;
	if (gamelen < 10)
		gamelen = 10;

	enum stone color = stone_other(m->color);
	coord_t next_move = pass;
	coord_t urgent;

	int passes = is_pass(m->coord);

	/* Special check: We probably tenukied the last opponent's move. But
	 * check if the opponent has lucrative local continuation for her last
	 * move! */
	/* This check is ultra-important BTW. Without it domain checking does
	 * not bring that much of an advantage. It might even warrant it to by
	 * default do only this domain check. */
	urgent = pass;
	//domain_hint(mc->carlo, b, &urgent, m->color);
	if (!is_pass(urgent))
		goto play_urgent;

	while (gamelen-- && passes < 2) {
		urgent = pass;
		//domain_hint(mc->carlo, &b2, &urgent, m->color);

		coord_t coord;

		if (!is_pass(urgent)) {
			struct move m;
play_urgent:
			m.coord = urgent; m.color = color;
			if (board_play(&b2, &m) < 0) {
				if (MCDEBUGL(7)) {
					fprintf(stderr, "Urgent move %d,%d is ILLEGAL:\n", coord_x(urgent, b), coord_y(urgent, b));
					board_print(&b2, stderr);
				}
				goto play_random;
			}
			coord = urgent;
		} else {
play_random:
			board_play_random(&b2, color, &coord);
		}

		if (is_pass(next_move))
			next_move = coord;

		if (unlikely(b2.superko_violation)) {
			/* We ignore superko violations that are suicides. These
			 * are common only at the end of the game and are
			 * rather harmless. (They will not go through as a root
			 * move anyway.) */
			if (group_at(&b2, coord)) {
				if (MCDEBUGL(3)) {
					fprintf(stderr, "Superko fun at %d,%d in\n", coord_x(coord, b), coord_y(coord, b));
					if (MCDEBUGL(4))
						board_print(&b2, stderr);
				}
				board_done_noalloc(&b2);
				return -2;
			} else {
				if (MCDEBUGL(6)) {
					fprintf(stderr, "Ignoring superko at %d,%d in\n", coord_x(coord, b), coord_y(coord, b));
					board_print(&b2, stderr);
				}
				b2.superko_violation = false;
			}
		}

		if (MCDEBUGL(7)) {
			char *cs = coord2str(coord, b);
			fprintf(stderr, "%s %s\n", stone2str(color), cs);
			free(cs);
		}

		if (unlikely(is_pass(coord))) {
			passes++;
		} else {
			passes = 0;
		}

		color = stone_other(color);
	}

	if (MCDEBUGL(6 - !(i % (mc->carlo->games/2))))
		board_print(&b2, stderr);

	float score = board_fast_score(&b2);
	bool result = (m->color == S_WHITE ? (score > 0 ? 1 : 0) : (score < 0 ? 1 : 0));

	if (MCDEBUGL(3)) {
		fprintf(stderr, "\tresult %d (score %f)\n", result, score);
	}

	if (!is_pass(m->coord) && !is_pass(next_move) && moves) {
		int j = coord_raw(m->coord) * b->size2 + coord_raw(next_move);
		moves[j].games++;
		if (!result)
			moves[j].wins++;
	}

	board_done_noalloc(&b2);
	return result;
}

/* 1: Games played. 0: No games can be played from this position anymore. */
static int
play_many_random_games(struct montecasino *mc, struct board *b, int games, enum stone color,
			struct move_stat *moves, struct move_stat *second_moves)
{
	if (MCDEBUGL(3))
		fprintf(stderr, "Playing %d random games\n", games);

	struct move m;
	m.color = color;
	int losses = 0;
	int i, superko = 0, good_games = 0;
	for (i = 0; i < games; i++) {
		int result = play_random_game(mc, b, second_moves, &m, i);

		if (result == -1) {
			/* Uh. Oops? Er... */
			return 0;
		}
		if (result == -2) {
			/* Superko. We just ignore this playout.
			 * And play again. */
			if (unlikely(superko > 2 * mc->carlo->games)) {
				/* Uhh. Triple ko, or something? */
				if (MCDEBUGL(0))
					fprintf(stderr, "SUPERKO LOOP. I will pass. Did we hit triple ko?\n");
				return 0;
			}
			superko++;
			continue;
		}
		if (result == -3) {
			/* Multi-stone suicide. We play chinese rules,
			 * so we can't consider this. (Note that we
			 * unfortunately still consider this in playouts.) */
			continue;
		}

		if (b->moves < 3) {
			/* Simple heuristic: avoid opening too low. Do not
			 * play on second or first line as first white or
			 * first two black moves.*/
			if (coord_x(m.coord, b) < 3 || coord_x(m.coord, b) > b->size - 4
			    || coord_y(m.coord, b) < 3 || coord_y(m.coord, b) > b->size - 4)
				continue;
		}

		int pos = is_pass(m.coord) ? 0 : coord_raw(m.coord);

		good_games++;
		moves[pos].games++;

		losses += 1 - result;
		moves[pos].wins += result;

		if (unlikely(!losses && i == mc->carlo->loss_threshold)) {
			/* We played out many games and didn't lose once yet.
			 * This game is over. */
			break;
		}
	}

	if (!good_games) {
		/* No moves to try??? */
		if (MCDEBUGL(0)) {
			fprintf(stderr, "OUT OF MOVES! I will pass. But how did this happen?\n");
			board_print(b, stderr);
		}
		return 0;
	}

	return 1;
}


struct move_info {
	coord_t coord;
	float ratio;
};

static int
create_move_queue(struct montecasino *mc, struct board *b,
                  struct move_stat *moves, struct move_info *q)
{
	int qlen = 0;
	foreach_point(b) {
		float ratio = (float) moves[coord_raw(c)].wins / moves[coord_raw(c)].games;
		if (!isfinite(ratio))
			continue;
		struct move_info mi = { c, ratio };
		if (qlen == 0 || q[qlen - 1].ratio >= ratio) {
			q[qlen++] = mi;
		} else {
			int i;
			for (i = 0; i < qlen; i++) {
				if (q[i].ratio < ratio)
					break;
			}
			memmove(&q[i + 1], &q[i], sizeof(*q) * (qlen++ - i));
			q[i] = mi;
		}
	} foreach_point_end;
	return qlen;
}

static float
best_move_at_board(struct montecasino *mc, struct board *b, struct move_stat *moves)
{
	float top_ratio = 0;
	foreach_point(b) {
		if (moves[coord_raw(c)].games < TRUST_THRESHOLD)
			continue;
		float ratio = (float) moves[coord_raw(c)].wins / moves[coord_raw(c)].games;
		if (ratio > top_ratio)
			top_ratio = ratio;
	} foreach_point_end;
	return top_ratio;
}

static void
choose_best_move(struct montecasino *mc, struct board *b, enum stone color,
		struct move_stat *moves, struct move_stat *second_moves, struct move_stat *first_moves,
		float *top_ratio, coord_t *top_coord)
{
	struct move_info sorted_moves[b->size2];
	memset(sorted_moves, 0, b->size2 * sizeof(*sorted_moves));
	int qlen = create_move_queue(mc, b, moves, sorted_moves);
	/* Now, moves sorted descending by ratio are in sorted_moves. */

	/* We take the moves with ratio better than 0.55 (arbitrary value),
	 * but at least best nine (arbitrary value). From those, we choose
	 * the one where opponent's best counterattack has worst chance
	 * of working. */

	/* Before, we just tried to take _any_ move with the opponent's worst
	 * counterattack, but that didn't work very well in the practice;
	 * there have to be way too many game playouts to have reliable
	 * second_moves[], apparently. */

	int move = 0;
	while (move < CANDIDATES && move < qlen) {
		coord_t c = sorted_moves[move].coord;
		move++;
		if (!moves[coord_raw(c)].wins) { /* whatever */
			continue;
		}

		/* These moves could use further reading. */
		{
			struct board b2;
			board_copy(&b2, b);
			struct move m = { coord_raw(c) == 0 ? pass : c, color };
			if (board_play(&b2, &m) < 0) {
				if (MCDEBUGL(0)) {
					fprintf(stderr, "INTERNAL ERROR - Suggested impossible move %d,%d.\n", coord_x(c, b), coord_y(c, b));
				}
				board_done_noalloc(&b2);
				continue;
			}
			play_many_random_games(mc, &b2, mc->carlo->games / GAMES_SLICE_CANDIDATE, stone_other(color), (struct move_stat *) &second_moves[coord_raw(c) * b->size2], NULL);
			board_done_noalloc(&b2);
		}

		float coratio = 1 - best_move_at_board(mc, b, &second_moves[coord_raw(c) * b->size2]);
		float ratio = /* sorted_moves[move - 1].ratio * */ coratio;
		/* Since pass is [0,0], we will pass only when we have nothing
		 * better to do. */
		if (ratio >= *top_ratio) {
			*top_ratio = ratio;
			*top_coord = coord_raw(c) == 0 ? pass : c;
		}
		/* Evil cheat. */
		first_moves[coord_raw(c)].games = 100; first_moves[coord_raw(c)].wins = ratio * 100;
		if (MCDEBUGL(2)) {
			fprintf(stderr, "Winner candidate [%d,%d] has ratio %f counter ratio %f => final ratio %f\n", coord_x(c, b), coord_y(c, b), sorted_moves[move - 1].ratio, coratio, ratio);
			if (MCDEBUGL(3))
				board_stats_print(b, &second_moves[coord_raw(c) * b->size2], stderr);
		}
	}
}


static coord_t *
montecasino_genmove(struct engine *e, struct board *b, enum stone color)
{
	struct montecasino *mc = e->data;

	/* resign when the hope for win vanishes */
	coord_t top_coord = resign;
	float top_ratio = mc->carlo->resign_ratio;

	/* We use [0] for pass. Normally, this is an inaccessible corner
	 * of board margin. */
	struct move_stat moves[b->size2];
	struct move_stat second_moves[b->size2][b->size2];
	/* Then first moves again, final decision; only for debugging */
	struct move_stat first_moves[b->size2];
	memset(moves, 0, sizeof(moves));
	memset(second_moves, 0, sizeof(second_moves));
	memset(first_moves, 0, sizeof(first_moves));

	if (!play_many_random_games(mc, b, mc->carlo->games / GAMES_SLICE_BASIC, color, moves, (struct move_stat *) second_moves)) {
		/* No more moves. */
		top_coord = pass; top_ratio = 0.5;
		goto move_found;
	}

	if (MCDEBUGL(3))
		fprintf(stderr, "Played the random games\n");

	/* We take the best moves and choose the one with least lucrative
	 * opponent's counterattack. */
	choose_best_move(mc, b, color, moves, (struct move_stat *) second_moves, first_moves, &top_ratio, &top_coord);

	if (MCDEBUGL(2)) {
		fprintf(stderr, "Our board stats:\n");
		board_stats_print(b, moves, stderr);
		fprintf(stderr, "Opponents' counters stats:\n");
		board_stats_print(b, first_moves, stderr);
		if (!is_resign(top_coord) && !is_pass(top_coord)) {
			fprintf(stderr, "Opponent's reaction stats:\n");
			board_stats_print(b, second_moves[coord_raw(top_coord)], stderr);
		}
	}

move_found:
	if (MCDEBUGL(1))
		fprintf(stderr, "*** WINNER is %d,%d with score %1.4f\n", coord_x(top_coord, b), coord_y(top_coord, b), top_ratio);

	return coord_copy(top_coord);
}

struct engine *
engine_montecasino_init(char *arg)
{
	struct montecarlo *carlo = montecarlo_state_init(arg);
	struct montecasino *mc = calloc(1, sizeof(*mc));
	struct engine *e = calloc(1, sizeof(struct engine));
	e->name = "MonteCasino Engine";
	e->comment = "I'm playing in Monte Casino now! When we both pass, I will consider all the stones on the board alive. If you are reading this, write 'yes'. Please bear with me at the game end, I need to fill the whole board; if you help me, we will both be happier. Filling the board will not lose points (NZ rules).";
	e->genmove = montecasino_genmove;
	e->data = mc;

	mc->carlo = carlo;
	mc->debug_level = carlo->debug_level;

	return e;
}
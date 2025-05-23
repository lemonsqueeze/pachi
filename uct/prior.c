#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEBUG
#include "board.h"
#include "debug.h"
#include "move.h"
#include "random.h"
#include "engine.h"
#include "joseki/joseki.h"
#include "pattern/prob.h"
#include "dcnn/dcnn.h"
#include "uct/prior.h"
#include "uct/tree.h"
#include "uct/plugins.h"
#include "uct/internal.h"

#define PRIOR_BEST_N 20

void
get_node_prior_best_moves(tree_node_t *parent, coord_t *best_c, float *best_r, int nbest)
{
	for (int i = 0; i < nbest; i++) {
		best_c[i] = pass;  best_r[i] = 0;
	}
	
	float max = 0.0;
	for (tree_node_t *n = parent->children; n; n = n->sibling)
		max = MAX(max, n->prior.playouts);

	for (tree_node_t *n = parent->children; n; n = n->sibling)
		best_moves_add(node_coord(n), (float)n->prior.playouts / max, best_c, best_r, nbest);
}

/* Display node's priors best moves. */
void
print_node_prior_best_moves(board_t *b, tree_node_t *parent)
{
	float best_r[PRIOR_BEST_N];
	coord_t best_c[PRIOR_BEST_N];
	int nbest = PRIOR_BEST_N;
	get_node_prior_best_moves(parent, best_c, best_r, nbest);

	int cols = best_moves_print(b, "prior =    ", best_c, nbest);
	
	fprintf(stderr, "%*s[ ", cols, "");
	for (int i = 0; i < nbest; i++)
		fprintf(stderr, "%-3i ", (int)(best_r[i] * 100));
	fprintf(stderr, "]\n");	
}

static void
get_prior_best_moves(prior_map_t *map, coord_t *best_c, float *best_r, int nbest)
{
	for (int i = 0; i < nbest; i++) {
		best_c[i] = pass;  best_r[i] = 0;
	}
	
	float max = map->prior[pass].playouts;
	foreach_free_point(map->b) {
		max = MAX(max, map->prior[c].playouts);
	} foreach_free_point_end;

	best_moves_add(pass, (float)map->prior[pass].playouts / max, best_c, best_r, nbest);
	foreach_free_point(map->b) {
		best_moves_add(c, (float)map->prior[c].playouts / max, best_c, best_r, nbest);
	} foreach_free_point_end;
}

/* Display priors best moves. */
static void
print_prior_best_moves(board_t *b, prior_map_t *map)
{
	float best_r[PRIOR_BEST_N];
	coord_t best_c[PRIOR_BEST_N];
	int nbest = PRIOR_BEST_N;
	get_prior_best_moves(map, best_c, best_r, nbest);

	int cols = best_moves_print(b, "prior =    ", best_c, nbest);

	fprintf(stderr, "%*s[ ", cols, "");
	for (int i = 0; i < nbest; i++)
		fprintf(stderr, "%-3i ", (int)(best_r[i] * 100));
	fprintf(stderr, "]\n");	
}

static void
uct_prior_even(uct_t *u, tree_node_t *node, prior_map_t *map)
{
	/* Q_{even} */
	/* This may be dubious for normal UCB1 but is essential for
	 * reading stability of RAVE, it appears. */
	add_prior_value(map, pass, 0.5, u->prior->even_eqex);
	for (int i = 0; i < map->consider->moves; i++) {
		coord_t c = map->consider->move[i];
		add_prior_value(map, c, 0.5, u->prior->even_eqex);
	}
}

static void
uct_prior_dcnn(uct_t *u, tree_node_t *node, prior_map_t *map)
{
#ifdef DCNN
	board_t *b = map->b;
	float   r[19 * 19];
	bool    debugl = (UDEBUGL(2) && !node->parent);

	int high = u->prior->dcnn_eqex_high;
	int low  = u->prior->dcnn_eqex_low;
	int board_size2 = board_rsize(b) * board_rsize(b);
	int moves = 0.42 * board_size2;  // 150 on 19x19
	assert(high >= low);

	/* Progressively decrease dcnn prior until middle-game. */
	int dcnn_eqex;
	if (b->moves >= moves)  dcnn_eqex = low;
	else                    dcnn_eqex = (high - b->moves * (high - low) / moves) / 10 * 10;

	strbuf(buf, 128);
	strbuf_printf(buf, "(dcnn prior = %i)", dcnn_eqex);
	dcnn_evaluate(map->b, map->to_play, r, &u->ownermap, debugl, buf->str);
	
	for (int i = 0; i < map->consider->moves; i++) {
		coord_t c = map->consider->move[i];
		int k = coord2dcnn_idx(c);
		float val = r[k];
		if (isnan(val) || val < 0.001)
			continue;
		assert(val >= 0.0 && val <= 1.0);
		add_prior_value(map, c, 1, sqrt(val) * dcnn_eqex);
	}

	node->hints |= TREE_HINT_DCNN;
#endif
}

static void
uct_prior_joseki(uct_t *u, tree_node_t *node, prior_map_t *map)
{
	/* Q_{joseki} */
	int matches = 0;

	board_t *b = map->b;
	enum stone color = map->to_play;
	coord_t coords[BOARD_MAX_COORDS];
	float ratings[BOARD_MAX_COORDS];
	matches = joseki_list_moves(joseki_dict, b, color, coords, ratings);
	
	for (int i = 0; i < matches; i++)
		add_prior_value(map, coords[i], 1.0, ratings[i] * u->prior->joseki_eqex);

	if (DEBUGL(2) && !node->parent && matches) {
		float best_r[20];
		coord_t best_c[20];
		get_joseki_best_moves(b, coords, ratings, matches, best_c, best_r, 20);
		print_joseki_best_moves(b, best_c, best_r, 20);
	}
}

static void
uct_prior_pattern(uct_t *u, tree_node_t *node, prior_map_t *map)
{
	/* Q_{pattern} */

	board_t *b = map->b;
	floating_t probs[b->flen];
	pattern_context_t ct;
	pattern_context_init(&ct, &u->pc, &u->ownermap);
	pattern_rate_moves(b, map->to_play, probs, NULL, &ct, NULL);

	/* Show patterns best moves for root node if not using dcnn. */
	if (DEBUGL(2) && !node->parent && !using_dcnn(b)) {
		float best_r[20];
		coord_t best_c[20];
		get_pattern_best_moves(b, probs, best_c, best_r, 20);
		print_pattern_best_moves(map->b, best_c, best_r, 20);
	}		

	if (UDEBUGL(5)) {
		fprintf(stderr, "Pattern prior at node %s\n", coord2sstr(node->coord));
		board_print(b, stderr);
	}

	for (int f = 0; f < b->flen; f++) {
		if (isnan(probs[f]) || probs[f] < 0.001)
			continue;
		assert(!is_pass(b->f[f]));
		add_prior_value(map, b->f[f], 1.0, sqrt(probs[f]) * u->prior->pattern_eqex);
	}
}

void
uct_prior(uct_t *u, tree_node_t *node, prior_map_t *map)
{
	if (u->prior->boost_pass)  /* Endgame with japanese rules, pass can be hard to find. */
		add_prior_value(map, pass, 1.0, u->prior->pattern_eqex * 3 / 4);

	if (u->prior->even_eqex)			uct_prior_even(u, node, map);

	if (!u->tree_ready) {  /* Root node: use dcnn for priors, don't mix pattern priors */
		if      (u->prior->dcnn_eqex_high)	uct_prior_dcnn(u, node, map);
		else if (u->prior->pattern_eqex)	uct_prior_pattern(u, node, map);
	}
	else
		if (u->prior->pattern_eqex)		uct_prior_pattern(u, node, map);

	if (u->prior->joseki_eqex)			uct_prior_joseki(u, node, map);

#ifdef PACHI_PLUGINS
	if (u->prior->plugin_eqex)			plugin_prior(u->plugins, node, map, u->prior->plugin_eqex);
#endif

	/* Show final prior mix. */
	if (DEBUGL(3) && !node->parent)                 print_prior_best_moves(map->b, map);
}

uct_prior_t *
uct_prior_init(char *arg, board_t *b, uct_t *u)
{
	uct_prior_t *p = calloc2(1, uct_prior_t);

	p->even_eqex = p->plugin_eqex = -20;
	/* FIXME: Optimal pattern_eqex is about 300 with small playout counts
	 * but only half on a cluster. We need a better way to set the default
	 * here. For high playouts lowering it may be better. */
	p->pattern_eqex    = -300;

	/* joseki_eqex: Double pattern_eqex to override patterns.
	 * (remember to also change joseki prior when setting pattern prior
	 *  through command line). */
	p->joseki_eqex     = p->pattern_eqex * 2;

	/* Progressively decrease dcnn prior from dcnn_eqex_high to dcnn_eqex_low.
	 * For a fixed dcnn prior sweet spot is between 600 and 900 now (was 1300).
	 * However low prior isn't great early game: playouts are pretty clueless,
	 * leading to early game blunders. On the other hand it's good in middle/
	 * end game to escape dcnn blind spots. Progressive prior seems best. */
	p->dcnn_eqex_high  = -1300;
	p->dcnn_eqex_low   = -900;

	/* Even number! */
	p->eqex = (board_large(b) ? 20 : 14);

	p->prune_ladders = true;

	if (arg) {
		char *optspec, *next = arg;
		while (*next) {
			optspec = next;
			next += strcspn(next, ":");
			if (*next) { *next++ = 0; } else { *next = 0; }

			char *optname = optspec;
			char *optval = strchr(optspec, '=');
			if (optval) *optval++ = 0;

			if (!strcasecmp(optname, "eqex") && optval) {
				p->eqex = atoi(optval);

			/* In the following settings you can use negative numbers
			 * to scale values automatically based on default eqex.
			 * E.g. with default settings -100 means 100 on large
			 * boards, and 70 on small boards. */
			} else if (!strcasecmp(optname, "even") && optval) {
				p->even_eqex = atoi(optval);
			} else if (!strcasecmp(optname, "joseki") && optval) {
				p->joseki_eqex = atoi(optval);
			} else if (!strcasecmp(optname, "pattern") && optval) {
				/* MM Pattern-based prior eqex. */
				p->pattern_eqex = atoi(optval);
			} else if (!strcasecmp(optname, "plugin") && optval) {
				/* Unlike others, this is just a *recommendation*. */
				p->plugin_eqex = atoi(optval);
			} else if (!strcasecmp(optname, "prune_ladders")) {
				p->prune_ladders = !optval || atoi(optval);
#ifdef DCNN
			} else if (!strcasecmp(optname, "dcnn_high") && optval) {
				/* Progressive dcnn prior: fuseki dcnn prior. */
				p->dcnn_eqex_high = atoi(optval);
			} else if (!strcasecmp(optname, "dcnn_low") && optval) {
				/* Progressive dcnn prior: middle/end game dcnn prior. */
				p->dcnn_eqex_low = atoi(optval);
			} else if (!strcasecmp(optname, "dcnn") && optval) {
				/* Fixed dcnn prior. */
				p->dcnn_eqex_high = atoi(optval);
				p->dcnn_eqex_low = atoi(optval);
#endif
			} else
				die("uct: Invalid prior argument %s or missing value\n", optname);
		}
	}

	if (p->even_eqex < 0)      p->even_eqex      = -p->even_eqex * p->eqex / 20;
	if (p->joseki_eqex < 0)    p->joseki_eqex    = -p->joseki_eqex * p->eqex / 20;
	if (p->pattern_eqex < 0)   p->pattern_eqex   = -p->pattern_eqex * p->eqex / 20;
	if (p->plugin_eqex < 0)    p->plugin_eqex    = -p->plugin_eqex * p->eqex / 20;
	if (p->dcnn_eqex_high < 0) p->dcnn_eqex_high = -p->dcnn_eqex_high * p->eqex / 20;
	if (p->dcnn_eqex_low < 0)  p->dcnn_eqex_low  = -p->dcnn_eqex_low * p->eqex / 20;

	if (!using_joseki(b))   p->joseki_eqex = 0;
	if (!using_dcnn(b))     p->dcnn_eqex_high = 0;
	if (!using_patterns())  p->pattern_eqex = 0;
	
	return p;
}

void
uct_prior_done(uct_prior_t *p)
{
	free(p);
}

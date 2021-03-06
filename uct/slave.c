/* This is the slave specific part of the distributed engine.
 * See introduction at top of distributed/distributed.c.
 * The slave maintains a hash table of nodes received from the
 * master. When receiving stats the hash table gives a pointer to the
 * tree node to update. When sending stats we remember in the tree
 * what was previously sent so that only the incremental part has to
 * be sent.  The incremental part is smaller and can be compressed.
 * The compression is not yet done in this version. */

/* Similarly the master only sends stats increments.
 * They include only contributions from other slaves. */

/* The keys for the hash table are coordinate paths from
 * a root child to a given node. See distributed/distributed.h
 * for the encoding of a path to a 64 bit integer. */

/* To allow the master to select the best move, slaves also send
 * absolute playout counts for the best top level nodes (children
 * of the root node), including contributions from other slaves. */

/* Pass me arguments like a=b,c=d,...
 * Slave specific arguments (see uct.c for the other uct arguments
 * and distributed.c for the port arguments) :
 *  slave                   required to indicate slave mode
 *  max_nodes=MAX_NODES     default 80K
 *  stats_hbits=STATS_HBITS default 24. 2^stats_bits = hash table size
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VERBOSE_LOGS 1000
#define DEBUG

#include "debug.h"
#include "board.h"
#include "fbook.h"
#include "gtp.h"
#include "move.h"
#include "timeinfo.h"
#include "uct/internal.h"
#include "uct/search.h"
#include "uct/slave.h"
#include "uct/tree.h"

/* UCT infrastructure for a distributed engine slave. */

/* For debugging only. */
static hash_counts_t h_counts;
static long parent_not_found = 0;
static long parent_leaf = 0;
static long node_not_found = 0;

/* Hash table entry mapping path to node. */
typedef struct tree_hash {
	path_t coord_path;
	tree_node_t *node;
} tree_hash_t;

tree_hash_t *
uct_htable_alloc(int hbits)
{
	return calloc2(1 << hbits, tree_hash_t);
}

/* Clear the hash table. Used only when running as slave for the distributed engine. */
void uct_htable_reset(tree_t *t)
{
	if (!t->htable) return;
	double start = time_now();
	memset(t->htable, 0, (1 << t->hbits) * sizeof(t->htable[0]));
	if (DEBUGL(3))
		fprintf(stderr, "tree occupied %ld %.1f%% inserts %ld collisions %ld/%ld %.1f%% clear %.3fms\n"
			"parent_not_found %.1f%% parent_leaf %.1f%% node_not_found %.1f%%\n",
			h_counts.occupied, h_counts.occupied * 100.0 / (1 << t->hbits),
			h_counts.inserts, h_counts.collisions, h_counts.lookups,
			h_counts.collisions * 100.0 / (h_counts.lookups + 1),
			(time_now() - start)*1000,
			parent_not_found * 100.0 / (h_counts.lookups + 1),
			parent_leaf * 100.0 / (h_counts.lookups + 1),
			node_not_found * 100.0 / (h_counts.lookups + 1));
	if (DEBUG_MODE) h_counts.occupied = 0;
}

/* Find a node given its coord path from root. Insert it in the
 * hash table if it is not already there.
 * Return the tree node, or NULL if the node cannot be found.
 * The tree is modified in background while this function is running.
 * prev is only used to optimize the tree search, given that calls to
 * tree_find_node are made with sorted coordinates (increasing levels
 * and increasing coord within a level). */
static tree_node_t *
tree_find_node(tree_t *t, incr_stats_t *is, tree_node_t *prev)
{
	assert(t && t->htable);
	path_t path = is->coord_path;
	/* pass and resign must never be inserted in the hash table. */
	assert(path > 0);

	int hash, parent_hash;
	bool found;
	find_hash(hash, t->htable, t->hbits, path, found, h_counts);
	tree_hash_t *hnode = &t->htable[hash];

	if (DEBUGVV(7))
		fprintf(stderr,
			"find_node %" PRIpath " %s found %d hash %d playouts %d node %p\n", path,
			path2sstr(path), found, hash, is->incr.playouts, hnode->node);

	if (found) return hnode->node;

	/* The master sends parents before children so the parent should
	 * already be in the hash table. */
	path_t parent_p = parent_path(path);
	tree_node_t *parent;
	if (parent_p) {
		find_hash(parent_hash, t->htable, t->hbits,
			  parent_p, found, h_counts);
		parent = t->htable[parent_hash].node;
	} else {
		parent = t->root;
	}
	tree_node_t *node = NULL;
	if (parent) {
		/* Search for the node in parent's children. */
		coord_t leaf = leaf_coord(path);
		node = (prev && prev->parent == parent ? prev->sibling : parent->children);
		while (node && node_coord(node) != leaf) node = node->sibling;

		if (DEBUG_MODE) parent_leaf += !parent->is_expanded;
	} else {
		if (DEBUG_MODE) parent_not_found++;
		if (DEBUGVV(7))
			fprintf(stderr, "parent of %" PRIpath " %s not found\n",
				path, path2sstr(path));
	}

	/* Insert the node in the hash table. */
	hnode->node = node;
	if (DEBUG_MODE) h_counts.inserts++, h_counts.occupied++;
	if (DEBUGVV(7))
		fprintf(stderr, "insert path %" PRIpath " %s hash %d playouts %d node %p\n",
			path, path2sstr(path), hash, is->incr.playouts, node);

	if (DEBUG_MODE && !node) node_not_found++;

	hnode->coord_path = path;
	return node;
}


/* Read and discard any binary arguments. The number of
 * bytes to be skipped is given by @size in the command. */
static void
discard_bin_args(char *args)
{
	char *s = strchr(args, '@');
	int size = 0;
	if (s) size = atoi(s+1);
	while (size) {
		char buf[64*1024];
		int len = sizeof(buf);
		if (len > size) len = size;
		len = fread(buf, 1, len, stdin);
		if (len <= 0) break;
		size -= len;
	}
}

enum parse_code
uct_notify(engine_t *e, board_t *b, int id, char *cmd, char *args, gtp_t *gtp)
{
	uct_t *u = (uct_t*)e->data;

	static bool board_resized = true;
	if (is_gamestart(cmd)) {
		board_resized = true;
		uct_pondering_stop(u);
	}

	if (id == -1)  return P_OK;
	
	/* Force resending the whole command history if we are out of sync
	 * but do it only once, not if already getting the history. */
	if ((move_number(id) != b->moves || !board_resized)
	    && !reply_disabled(id) && !is_reset(cmd)) {
		discard_bin_args(args);
		
		if (UDEBUGL(0))  fprintf(stderr, "Out of sync, %d %s, move %d expected\n", id, cmd, b->moves);
		gtp_error_printf(gtp, "Out of sync, %d %s, move %d expected\n", id, cmd, b->moves);
		return P_OK;
	}
	return (reply_disabled(id) ? P_NOREPLY : P_OK);
}


/* Read the move stats sent by the master, as a binary array of
 * incr_stats structs. The stats come sorted by increasing coord path.
 * To simplify the code, we assume that master and slave have the same
 * architecture (store values identically).
 * Keep this code in sync with distributed/merge.c:output_stats()
 * Return true if ok, false if error. */
static bool
receive_stats(uct_t *u, int size)
{
	if (size % sizeof(incr_stats_t)) return false;
	int nodes = size / sizeof(incr_stats_t);
	if (nodes > (1 << u->stats_hbits)) return false;

	tree_t *t = u->t;
	assert(nodes && t->htable);
	tree_node_t *prev = NULL;
	double start_time = time_now();

	for (int n = 0; n < nodes; n++) {
		incr_stats_t is;
		if (fread(&is, sizeof(incr_stats_t), 1, stdin) != 1)
			return false;

		if (UDEBUGL(7))
			fprintf(stderr, "read %5d/%d %6d %.3f %" PRIpath " %s\n", n, nodes,
				is.incr.playouts, is.incr.value, is.coord_path,
				path2sstr(is.coord_path));

		tree_node_t *node = tree_find_node(t, &is, prev);
		if (!node) continue;

		/* node_total += others_incr */
		stats_add_result(&node->u, is.incr.value, is.incr.playouts);

		/* last_total += others_incr */
		stats_add_result(&node->pu, is.incr.value, is.incr.playouts);

		prev = node;
	}
	if (DEBUGVV(3))
		fprintf(stderr, "read args for %d nodes in %.4fms\n", nodes,
			(time_now() - start_time)*1000);
	return true;
}

/* A tree traversal fills this array, then the nodes with most increments are sent. */
typedef struct {
	path_t coord_path;
	int playout_incr;
	tree_node_t *node;
} stats_candidate_t;

/* We maintain counts per bucket to avoid sorting stats_queue.
 * All nodes with n updates since last send go to bucket n.
 * If we put all nodes above 1023 updates in the top bucket,
 * we get at most 27 nodes in this bucket. So we can select
 * exactly the best shared_nodes nodes if shared_nodes >= 27. */
#define MAX_BUCKETS 1024
static int bucket_count[MAX_BUCKETS];

/* Traverse the tree rooted at node, and append incremental stats
 * for children to stats_queue. start_path is the coordinate path
 * for the top node. Stats for a node are only appended if enough playouts
 * have been made since the last send, and the level is not too deep.
 * Return the updated stats count. */
static int
append_stats(stats_candidate_t *stats_queue, tree_node_t *node, int stats_count,
	     int max_count, path_t start_path, path_t max_path, int min_increment)
{
	/* The children field is set only after all children are created
	 * so we can traverse the the tree while it is updated. */
	for (tree_node_t *ni = node->children; ni; ni = ni->sibling) {

		if (is_pass(node_coord(ni))) continue;
		if (ni->hints & TREE_HINT_INVALID) continue;

		int incr = ni->u.playouts - ni->pu.playouts;
		if (incr < min_increment) continue;

		/* min_increment should be tuned to avoid overflow. */
		if (stats_count >= max_count) {
			if (DEBUGL(0))
				fprintf(stderr, "*** stats overflow %d nodes\n", stats_count);
			return stats_count;
		}
		path_t child_path = append_child(start_path, node_coord(ni));
		stats_queue[stats_count].playout_incr = incr;
		stats_queue[stats_count].coord_path = child_path;
		stats_queue[stats_count++].node = ni;

		if (incr >= MAX_BUCKETS) incr = MAX_BUCKETS - 1;
		bucket_count[incr]++;

		/* Do not recurse if level deep enough. */
		if (child_path >= max_path) continue;

		stats_count = append_stats(stats_queue, ni, stats_count, max_count,
					   child_path, max_path, min_increment);
	}
	return stats_count;
}

/* Used to sort by coord path the incremental stats to be sent. */
static int
coord_cmp(const void *p1, const void *p2)
{
	path_t diff = ((incr_stats_t *)p1)->coord_path
		    - ((incr_stats_t *)p2)->coord_path;
	return (int)(diff >> 32) | !!(int)diff;
}

/* Select from stats_queue at most shared_nodes candidates with
 * biggest increments. Return a binary array sorted by coord path. */
static incr_stats_t *
select_best_stats(stats_candidate_t *stats_queue, int stats_count,
		  int shared_nodes, int *byte_size)
{
	static incr_stats_t *out_stats = NULL;
	if (!out_stats)
		out_stats = calloc2(shared_nodes, incr_stats_t);

	/* Find the minimum increment to send. The bucket with minimum
         * increment may be sent only partially. */
	int out_count = 0;
	int min_incr = MAX_BUCKETS;
	do {
		out_count += bucket_count[--min_incr];
	} while (min_incr > 1 && out_count < shared_nodes);

	/* Send all all increments > min_incr plus whatever we can at min_incr. */
	int min_count = bucket_count[min_incr] - (out_count - shared_nodes);
	incr_stats_t *os = out_stats;
	out_count = 0;
	for (int count = 0; count < stats_count; count++) {
		int delta = stats_queue[count].playout_incr - min_incr;
		if (delta < 0 || (delta == 0 && --min_count < 0)) continue;

		tree_node_t *node = stats_queue[count].node;
		os->incr = node->u;
		stats_rm_result(&os->incr, node->pu.value, node->pu.playouts);

		/* With virtual loss os->incr.playouts might be <= 0; we only
		 * send positive increments to other slaves so a virtual loss
		 * can be propagated to other machines (good). The undo of the
		 * virtual loss will be propagated later when node->u gets
		 * above node->pu. */
		if (os->incr.playouts > 0) {
			node->pu = node->u;
			os->coord_path = stats_queue[count].coord_path;
			assert(os->coord_path > 0);
			os++;
			out_count++;
		}
		assert (out_count <= shared_nodes);
	}
	*byte_size = (char *)os - (char *)out_stats;

	/* Sort the increments by increasing coord path (required by master).
	 * Can be done in linear time with radix sort if qsort is too slow. */
	qsort(out_stats, out_count, sizeof(*os), coord_cmp);
	return out_stats;
}

/* Get incremental stats updates for the distributed engine.
 * Return a binary array of incr_stats structs in coordinate order
 * (increasing levels and increasing coordinates within a level).
 * This function is called only by the main thread, but may be
 * called while the tree is updated by the worker threads. Keep this
 * code in sync with distributed/merge.c:merge_new_stats(). */
static void *
report_incr_stats(uct_t *u, int *stats_size)
{
	double start_time = time_now();

	tree_node_t *root = u->t->root;

	/* The factor 3 below has experimentally been found to be
	 * sufficient. At worst if we fill stats_queue we will
	 * discard some stats updates but this is rare. */
	int max_nodes = 3 * u->shared_nodes;
	static stats_candidate_t *stats_queue = NULL;
	if (!stats_queue) stats_queue = calloc2(max_nodes, stats_candidate_t);

	memset(bucket_count, 0, sizeof(bucket_count));

	/* Try to fill the output buffer with the most important
         * nodes (highest increments), while still traversing
	 * as little of the tree as possible. If we set min_increment
	 * too low we waste time. If we set it too high we can't
	 * fill the output buffer with the desired number of nodes.
	 * The best min_increment results in stats_count just above 
	 * shared_nodes. However perfect tuning is not necessary:
	 * if we send too few nodes we just send shorter buffers
	 * more frequently. */
	static int min_increment = 1;
	static int stats_count = 0;
	if (stats_count > 2 * u->shared_nodes) {
		min_increment++;
	} else if (stats_count < u->shared_nodes / 2 && min_increment > 1) {
		min_increment--;
	}

	stats_count = append_stats(stats_queue, root, 0, max_nodes, 0,
				   max_parent_path(u), min_increment);

	void *buf = select_best_stats(stats_queue, stats_count, u->shared_nodes, stats_size);

	if (DEBUGVV(3))
		fprintf(stderr,
			"min_incr %d games %d stats_queue %d/%d sending %d/%d in %.3fms\n",
			min_increment, root->u.playouts - root->pu.playouts, stats_count,
			max_nodes, *stats_size / (int)sizeof(incr_stats_t), u->shared_nodes,
			(time_now() - start_time)*1000);
	root->pu = root->u;
	return buf;
}

/* Get stats for the distributed engine. Return a buffer with one
 * line "played_own root_playouts threads keep_looking @size", then
 * a list of lines "coord playouts value" with absolute counts for
 * children of the root node (including contributions from other
 * slaves). The last line must not end with \n.
 * If @force is non-zero, add this move with a large weight.
 * This function is called only by the main thread, but may be
 * called while the tree is updated by the worker threads. Keep this
 * code in sync with distributed/distributed.c:select_best_move(). */
static char *
report_stats(uct_t *u, board_t *b, coord_t force,
	     bool keep_looking, int bin_size)
{
	static char reply[10240];
	char *r = reply;
	char *end = reply + sizeof(reply);
	tree_node_t *root = u->t->root;
	r += snprintf(r, end - r, "%d %d %d %d @%d", u->played_own, root->u.playouts,
		      u->threads, keep_looking, bin_size);
	int min_playouts = root->u.playouts / 100;
	if (min_playouts < GJ_MINGAMES)
		min_playouts = GJ_MINGAMES;
	int max_playouts = 1;

	/* We rely on the fact that root->children is set only
	 * after all children are created. */
	for (tree_node_t *ni = root->children; ni; ni = ni->sibling) {

		if (is_pass(node_coord(ni))) continue;
		assert(node_coord(ni) > 0 && node_coord(ni) < board_max_coords(b));

		if (ni->u.playouts > max_playouts)
			max_playouts = ni->u.playouts;
		if (ni->u.playouts <= min_playouts || ni->hints & TREE_HINT_INVALID)
			continue;
		/* A book move is only added at the end: */
		if (node_coord(ni) == force) continue;

		char buf[4];
		/* We return the values as stored in the tree, so from black's view. */
		r += snprintf(r, end - r, "\n%s %d %.16f", coord2bstr(buf, node_coord(ni)),
			      ni->u.playouts, ni->u.value);
	}
	/* Give a large but not infinite weight to pass, resign or book move, to avoid
	 * forcing resign if other slaves don't like it. */
	if (force) {
		double resign_value = u->t->root_color == S_WHITE ? 0.0 : 1.0;
		double value = is_resign(force) ? resign_value : 1.0 - resign_value;
		r += snprintf(r, end - r, "\n%s %d %.1f", coord2sstr(force), 2 * max_playouts, value);
	}
	return reply;
}

void
uct_slave_init(uct_t *u, board_t *b)
{
	assert(u->slave);
	
	if (!u->stats_hbits) u->stats_hbits = DEFAULT_STATS_HBITS;
	if (!u->shared_nodes) u->shared_nodes = DEFAULT_SHARED_NODES;
	assert(u->shared_levels * board_bits2() <= 8 * (int)sizeof(path_t));

	static int showed = 0;
	if (!showed++) {  /* Display once */
		if (DEBUGL(2))  fprintf(stderr, "distributed: slave node\n");
		if (DEBUGL(2) && !DEBUGL(3))
			fprintf(stderr,
				"distributed: pachi-genmoves subcommands not logged\n"
				"distributed: run with -d4 to see everything.\n");
	}
}

/* Check the state of the Monte Carlo Tree Search. Since we're not using the
 * pondering infrastructure also need to check fullmem and print progress here. */
static bool
slave_check_progress(uct_t *u, board_t *b, enum stone color, time_info_t *ti,
		     uct_search_state_t *s, coord_t *force)
{
	/* Print progress */
	int played_games = uct_search_games(s);
	uct_search_progress(u, b, color, u->t, ti, s, played_games);
	u->played_own = played_games - s->base_playouts;

	if (s->fullmem) {
		/* Stop search, realloc tree and restart search */
		if (!u->auto_alloc || !uct_search_realloc_tree(u, b, color, ti, s))
			uct_search_stop();
	}

	bool keep_looking = !uct_search_check_stop(u, b, color, u->t, ti, s, played_games);

	DEBUG_QUIET();
	coord_t best;
	uct_search_result(u, b, color, u->pass_all_alive, played_games, s->base_playouts, &best);
	DEBUG_QUIET_END();

	/* Give heavy weight to pass and resign */
	if (best < 0)  *force = best;

	return keep_looking;
}

/* genmoves is issued by the distributed engine master to all slaves, to:
 * 1. Start a MCTS search if not running yet
 * 2. Report current move statistics of the on-going search.
 * The MCTS search is left running on the background when uct_genmoves()
 * returns. It is stopped by receiving a play GTP command, triggering
 * uct_pondering_stop(). */
/* genmoves gets in the args parameter
 * "played_games nodes main_time byoyomi_time byoyomi_periods byoyomi_stones @size"
 * and reads a binary array of coord, playouts, value to get stats of other slaves,
 * except possibly for the first call at a given move number.
 * See report_stats() for the description of the return value. */
char *
uct_genmoves(engine_t *e, board_t *b, time_info_t *ti, enum stone color,
	     char *args, bool pass_all_alive, void **stats_buf, int *stats_size)
{
	uct_t *u = (uct_t*)e->data;
	assert(u->slave);
	u->pass_all_alive |= pass_all_alive;

	/* Prepare the state if the search is not already running.
	 * We must do this first since we tweak the state below
	 * based on instructions from the master. */
	if (!thread_manager_running)
		uct_genmove_setup(u, b, color);

	/* Get playouts and time information from master. Keep this code
	 * in sync with distibuted/distributed.c:distributed_genmove(). */
	if ((ti->dim == TD_WALLTIME
	     && sscanf(args, "%d %lf %lf %d %d", &u->played_all,
		       &ti->main_time, &ti->byoyomi_time,
		       &ti->byoyomi_periods, &ti->byoyomi_stones) != 5)

	    || (ti->dim == TD_GAMES && sscanf(args, "%d", &u->played_all) != 1)) {
		return NULL;
	}

	static uct_search_state_t s;
	if (!thread_manager_running) {
		/* This is the first genmoves received, start the MCTS now and let it run.
		 * Can't use uct_pondering_start() here, we need time management.
		 * So we are pondering with foreground search infrastructure... */
		
		if (DEBUGL(2) && debug_boardprint)
			engine_board_print(e, b, stderr);
		
		if (ti->type == TT_NULL)
				*ti = ti_unlimited();
		uct_search_start(u, b, color, u->t, ti, &s, 0);
	}

	/* Read binary incremental stats if present, otherwise
	 * wait a bit to populate the statistics. */
	int size = 0;
	char *sizep = strchr(args, '@');
	if (sizep)  size = atoi(sizep+1);
	if (!size)  time_sleep(u->stats_delay);
	else if (!receive_stats(u, size))
		return NULL;

	*stats_size = 0;
	bool keep_looking = false;
	coord_t force = (b->fbook ? fbook_check(b) : 0);

	/* Book move takes precedence */
	if (!force) {
		keep_looking = slave_check_progress(u, b, color, ti, &s, &force);
		if (u->shared_levels)
			*stats_buf = report_incr_stats(u, stats_size);
	}

	char *reply = report_stats(u, b, force, keep_looking, *stats_size);
	return reply;
}

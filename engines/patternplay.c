#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "board.h"
#include "debug.h"
#include "engine.h"
#include "move.h"
#include "engines/patternplay.h"
#include "pattern.h"
#include "patternsp.h"
#include "patternprob.h"
#include "random.h"


/* Internal engine state. */
struct patternplay {
	int debug_level;
	struct pattern_config pc;
	bool mcowner_fast;
	int  matched_locally;
};

struct pattern_config*
patternplay_get_pc(struct engine *e)
{
	struct patternplay *pp = e->data;
	return &pp->pc;
}

bool
patternplay_matched_locally(struct engine *e)
{
	struct patternplay *pp = e->data;
	assert(pp->matched_locally != -1);
	return pp->matched_locally;
}

static void
debug_pattern_best_moves(struct patternplay *pp, struct board *b, enum stone color,
			 coord_t *best_c, int nbest)
{
	struct ownermap ownermap;
	if (pp->mcowner_fast)  mcowner_playouts_fast(b, color, &ownermap);
	else                   mcowner_playouts(b, color, &ownermap);
	bool locally = pattern_matching_locally(&pp->pc, b, color, &ownermap);
	
	fprintf(stderr, "\n");
	for (int i = 0; i < nbest; i++) {
		struct move m = { .coord = best_c[i], .color = color };
		struct pattern p;
		pattern_match(&pp->pc, &p, b, &m, &ownermap, locally);

		char buffer[512];  strbuf_t strbuf;
		strbuf_t *buf = strbuf_init(&strbuf, buffer, sizeof(buffer));
		dump_gammas(buf, &pp->pc, &p);
		fprintf(stderr, "%3s gamma %s\n", coord2sstr(m.coord, b), buf->str);
	}
	fprintf(stderr, "\n");
}


static coord_t
patternplay_genmove(struct engine *e, struct board *b, struct time_info *ti, enum stone color, bool pass_all_alive)
{
	struct patternplay *pp = e->data;

	struct pattern pats[b->flen];
	floating_t probs[b->flen];
	struct ownermap ownermap;
	if (pp->mcowner_fast)  mcowner_playouts_fast(b, color, &ownermap);
	else		       mcowner_playouts(b, color, &ownermap);
	pp->matched_locally = -1;  // Invalidate
	pattern_rate_moves(&pp->pc, b, color, pats, probs, &ownermap);

	float best_r[20];
	coord_t best_c[20];
	get_pattern_best_moves(b, probs, best_c, best_r, 20);
	print_pattern_best_moves(b, best_c, best_r, 20);
	if (pp->debug_level >= 4)
		debug_pattern_best_moves(pp, b, color, best_c, 20);

	int best = 0;
	for (int f = 0; f < b->flen; f++) {
		if (pp->debug_level >= 5 && probs[f] >= 0.001) {
			char s[256]; pattern2str(s, &pats[f]);
			fprintf(stderr, "\t%s: %.3f %s\n", coord2sstr(b->f[f], b), probs[f], s);
		}
		if (probs[f] > probs[best])
			best = f;
	}

	return b->f[best];
}

static void
patternplay_best_moves(struct engine *e, struct board *b, struct time_info *ti, enum stone color,
		       coord_t *best_c, float *best_r, int nbest)
{
	struct patternplay *pp = e->data;

	struct pattern pats[b->flen];
	floating_t probs[b->flen];
	struct ownermap ownermap;
	if (pp->mcowner_fast)  mcowner_playouts_fast(b, color, &ownermap);
	else		       mcowner_playouts(b, color, &ownermap);
	pp->matched_locally = pattern_matching_locally(&pp->pc, b, color, &ownermap);
	pattern_rate_moves(&pp->pc, b, color, pats, probs, &ownermap);

	get_pattern_best_moves(b, probs, best_c, best_r, nbest);
	print_pattern_best_moves(b, best_c, best_r, nbest);
}

void
patternplay_evaluate(struct engine *e, struct board *b, struct time_info *ti, floating_t *vals, enum stone color)
{
	struct patternplay *pp = e->data;

	struct pattern pats[b->flen];
	struct ownermap ownermap;
	if (pp->mcowner_fast)  mcowner_playouts_fast(b, color, &ownermap);
	else                   mcowner_playouts(b, color, &ownermap);
	pp->matched_locally = -1;  // Invalidate
	pattern_rate_moves(&pp->pc, b, color, pats, vals, &ownermap);

#if 0
	// unused variable 'total' in above call to pattern_rate_moves()
	floating_t total = pattern_rate_moves(&pp->pc, b, color, pats, vals);
	/* Rescale properly. */
	for (int f = 0; f < b->flen; f++) {
		probs[f] /= total;
	}
#endif

	if (pp->debug_level >= 4) {
		for (int f = 0; f < b->flen; f++) {
			if (vals[f] >= 0.001) {
				char s[256]; pattern2str(s, &pats[f]);
				fprintf(stderr, "\t%s: %.3f %s\n", coord2sstr(b->f[f], b), vals[f], s);
			}
		}
	}
}


struct patternplay *
patternplay_state_init(char *arg)
{
	struct patternplay *pp = calloc2(1, sizeof(struct patternplay));
	bool pat_setup = false;

	pp->debug_level = debug_level;
	pp->matched_locally = -1;  /* Invalid */
	pp->mcowner_fast = true;

	if (arg) {
		char *optspec, *next = arg;
		while (*next) {
			optspec = next;
			next += strcspn(next, ",");
			if (*next) { *next++ = 0; } else { *next = 0; }

			char *optname = optspec;
			char *optval = strchr(optspec, '=');
			if (optval) *optval++ = 0;

			if (!strcasecmp(optname, "debug")) {
				if (optval)
					pp->debug_level = atoi(optval);
				else
					pp->debug_level++;

			} else if (!strcasecmp(optname, "mcowner_fast") && optval) {
				/* Use mcowner_fast=0 for better ownermap accuracy,
				 * Will be much slower though. (Default: mcowner_fast=1) 
				 * See also MM_MINGAMES. */
				pp->mcowner_fast = atoi(optval);

			} else if (!strcasecmp(optname, "patterns") && optval) {
				patterns_init(&pp->pc, optval, false, true);
				pat_setup = true;

			} else
				die("patternplay: Invalid engine argument %s or missing value\n", optname);
		}
	}

	if (!pat_setup)
		patterns_init(&pp->pc, NULL, false, true);
	
	if (!using_patterns())
		die("Missing spatial dictionary / probtable, aborting.\n");
	return pp;
}

void
engine_patternplay_init(struct engine *e, char *arg, struct board *b)
{
	struct patternplay *pp = patternplay_state_init(arg);
	e->name = "PatternPlay Engine";
	e->comment = "I select moves blindly according to learned patterns. I won't pass as long as there is a place on the board where I can play. When we both pass, I will consider all the stones on the board alive.";
	e->genmove = patternplay_genmove;
	e->best_moves = patternplay_best_moves;
	e->evaluate = patternplay_evaluate;
	e->data = pp;
}

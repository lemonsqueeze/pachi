#ifndef PACHI_UCT_SLAVE_H
#define PACHI_UCT_SLAVE_H

#include "move.h"
#include "distributed/distributed.h"

#ifdef DISTRIBUTED

enum parse_code uct_notify(engine_t *e, board_t *b, int id, char *cmd, char *args, char **reply);
char *uct_genmoves(engine_t *e, board_t *b, time_info_t *ti, enum stone color,
		   char *args, bool pass_all_alive, void **stats_buf, int *stats_size);
struct tree_hash *uct_htable_alloc(int hbits);
void uct_htable_reset(tree_t *t);


#else
#define uct_htable_reset(t)  do { } while(0)
#define uct_htable_alloc(hbits)  NULL
#endif /* DISTRIBUTED */

#endif

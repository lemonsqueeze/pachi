#ifndef PACHI_MOVE_H
#define PACHI_MOVE_H

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "util.h"
#include "stone.h"

typedef int coord_t;

// XXX board_size() instead of board_statics.size
#define coord_xy(x, y) ((x) + (y) * the_board_size())
#define coord_x(c) (board_statics.coord[c][0])
#define coord_y(c) (board_statics.coord[c][1])
/* TODO: Smarter way to do this? */
#define coord_dx(c1, c2) (coord_x(c1) - coord_x(c2))
#define coord_dy(c1, c2) (coord_y(c1) - coord_y(c2))

#define pass   -1
#define resign -2
#define is_pass(c)   (c == pass)
#define is_resign(c) (c == resign)

#define coord_is_adjecent(c1, c2) (abs(c1 - c2) == 1 || abs(c1 - c2) == the_board_size())
#define coord_is_8adjecent(c1, c2) (abs(c1 - c2) == 1 || abs(abs(c1 - c2) - the_board_size()) < 2)

char *coord2bstr(char *buf, coord_t c);
/* Return coordinate string in a dynamically allocated buffer. Thread-safe. */
char *coord2str(coord_t c);
/* Return coordinate string in a static buffer; multiple buffers are shuffled
 * to enable use for multiple printf() parameters, but it is NOT safe for
 * anything but debugging - in particular, it is NOT thread-safe! */
char *coord2sstr(coord_t c);
coord_t str2coord(char *str);
coord_t str2coord_for(char *str, int size);
/* Rotate coordinate according to rot: [0-7] for 8 board symmetries. */
coord_t rotate_coord(coord_t c, int rot);

struct move {
	coord_t coord;
	enum stone color;
};

#define move(coord, color)  { coord, color }

static inline int 
move_cmp(struct move *m1, struct move *m2)
{
	if (m1->color != m2->color)
		return m1->color - m2->color;
	return m1->coord - m2->coord;
}

#endif

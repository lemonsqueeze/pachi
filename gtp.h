#ifndef PACHI_GTP_H
#define PACHI_GTP_H

#include "board.h"
#include "timeinfo.h"

struct engine;

enum parse_code {
	P_OK,
	P_NOREPLY,
	P_DONE_OK,
	P_DONE_ERROR,
	P_ENGINE_RESET,
	P_UNKNOWN_COMMAND,
};

typedef struct
{
	char *cmd;
	char *next;
	int   id;
	bool  quiet;
	int   replied;

	/* Global fields: */
	int     played_games;
	move_t  move[1500];     /* move history, for undo */
	int     moves;
	bool    undo_pending;
	bool    noundo;           /* undo only allowed for pass */
	bool    kgs;
	bool    analyze_running;
	char*   custom_name;
	char*   custom_version;
} gtp_t;

#define next_tok(to_) \
	to_ = gtp->next; \
	gtp->next = gtp->next + strcspn(gtp->next, " \t\r\n"); \
	if (*gtp->next) { \
		*gtp->next = 0; gtp->next++; \
		gtp->next += strspn(gtp->next, " \t\r\n"); \
	}

void   gtp_init(gtp_t *gtp);

enum parse_code gtp_parse(gtp_t *gtp, board_t *b, struct engine *e, char *e_arg, time_info_t *ti, char *buf);
bool gtp_is_valid(struct engine *e, const char *cmd);
void gtp_reply(gtp_t *gtp, ...);
void gtp_reply_printf(gtp_t *gtp, const char *format, ...);
void gtp_error_printf(gtp_t *gtp, const char *format, ...);
void gtp_error(gtp_t *gtp, ...);

#define is_gamestart(cmd) (!strcasecmp((cmd), "boardsize"))
#define is_reset(cmd) (is_gamestart(cmd) || !strcasecmp((cmd), "clear_board") || !strcasecmp((cmd), "kgs-rules"))
#define is_repeated(cmd) (strstr((cmd), "pachi-genmoves"))


#endif

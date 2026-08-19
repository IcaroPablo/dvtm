/*
 * internal.h - what term.c and ui.c both need to see.
 *
 * Not a public interface: vt.h is what the rest of dvtm uses. This exists only
 * because the two halves of one module -- the child and its terminal state on
 * one side, painting it with ncurses on the other -- work on the same struct.
 */
#ifndef DVTM_INTERNAL_H
#define DVTM_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <vterm.h>

#include "vt.h"

#define RGB(r, g, b) (((r) & 0xff) << 16 | ((g) & 0xff) << 8 | ((b) & 0xff))

/* One line that has scrolled off the top. libvterm gives us the cells and
 * forgets them; if we want scrollback we have to keep them ourselves. */
typedef struct {
	int cols;
	VTermScreenCell *cells;
} Line;

struct Vt {
	VTerm *vt;
	VTermScreen *screen;
	VTermState *state;

	int rows, cols;
	int pty;
	pid_t pid;
	void *data;

	vt_title_handler_t title_handler;
	vt_urgent_handler_t urgent_handler;
	char title[256];

	bool cursor_visible;
	VTermPos cursor;
	bool dirty;

	/* scrollback ring; oldest at `first`, `count` entries in use */
	Line *sb;
	int sb_size, sb_count, sb_first;
	int scroll;           /* lines scrolled back; 0 means live screen */

	attr_t defattrs;
	int32_t deffg, defbg;

	int srow, scol;       /* where this terminal was last painted */
};

/* Set once, in vt_init: what this terminal calls its own default colours. */
extern int32_t default_fg, default_bg;
extern bool has_default_colors;

/* term.c, used by ui.c when painting scrolled-back lines. */
Line *sb_at(Vt *t, int n);

/* ui.c */
void ui_init_colors(void);

/* A libvterm colour as an ncurses colour number; -1 means the default. Shared
 * because copy mode has to spell colours out as escape sequences too. */
int32_t color_of(const VTermColor *c, bool is_fg);

#endif /* DVTM_INTERNAL_H */

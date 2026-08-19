/*
 * Copyright © 2004 Bruno T. C. de Oliveira
 * Copyright © 2006 Pierre Habouzit
 * Copyright © 2008-2013 Marc André Tanner
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef TERM_H
#define TERM_H

#include <curses.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <vterm.h>

#define RGB(r, g, b) (((r) & 0xff) << 16 | ((g) & 0xff) << 8 | ((b) & 0xff))

typedef struct Term Term;

typedef void (*TermTitleHandler)(Term *, const char *title);
typedef void (*TermUrgentHandler)(Term *);

/* One line that has scrolled off the top. libvterm gives us the cells and
 * forgets them; if we want scrollback we have to keep them ourselves. */
typedef struct {
	int cols;
	VTermScreenCell *cells;
} Line;

/* One child: its pty, the libvterm instance parsing what it writes, and the
 * lines that have scrolled off the top.
 *
 * The fields are open on purpose. dvtm and this terminal are one program, not
 * a library and its user, and a dozen one-line accessors only hid that. Read
 * and write them directly; what is declared below is what actually does
 * something.
 */
struct Term {
	VTerm *vt;
	VTermScreen *screen;
	VTermState *state;

	int rows, cols;
	int pty;
	pid_t pid;
	void *data;           /* whatever the caller wants to find again */

	TermTitleHandler title_handler;
	TermUrgentHandler urgent_handler;
	char title[256];

	bool cursor_visible;
	VTermPos cursor;
	bool dirty;

	/* scrollback ring; oldest at `first`, `count` entries in use */
	Line *sb;
	int sb_size, sb_count, sb_first;
	int scroll;           /* lines scrolled back; 0 means live screen */

	attr_t defattrs;      /* what this window's colour rule asks for */
	int32_t deffg, defbg;

	int srow, scol;       /* where this terminal was last painted */
};

/* Once, after curses is up: colours, the character set, and the TERM children
 * are run with. `keytable` is the escape-sequence overlay from config.h. */
void term_init(char const * const keytable[], int count);

Term *term_create(int rows, int cols, int scroll_buf_sz);
void term_destroy(Term *);
void term_resize(Term *, int rows, int cols);
pid_t term_forkpty(Term *, const char *p, const char *argv[], const char *cwd,
                   const char *env[], int *to, int *from);

int term_process(Term *);
ssize_t term_write(Term *, const char *buf, size_t len);
void term_keypress(Term *, int keycode);
void term_mouse(Term *, int x, int y, mmask_t mask);

void term_scroll(Term *, int rows);
int term_content_start(Term *);
size_t term_content_get(Term *, char **s, bool colored);
bool term_cursor_visible(Term *);

void term_draw(Term *, WINDOW *win, int startrow, int startcol);
/* An ncurses colour pair for fg/bg. A NULL terminal means the program's own
 * defaults rather than one window's colour rule; -1 means "the default". */
int term_color_get(Term *, int32_t fg, int32_t bg);

/* ── shared by term.c and ui.c only ───────────────────────────────────────── */

/* What this terminal calls its own default colours; set by term_init. */
extern int32_t default_fg, default_bg;
extern bool has_default_colors;

Line *term_sb_at(Term *, int n);     /* nth scrollback line, oldest first */
void term_noscroll(Term *);
void ui_init_colors(void);

/* A libvterm colour as an ncurses colour number; -1 means the default. Shared
 * because copy mode has to spell colours out as escape sequences too. */
int32_t color_of(const VTermColor *c, bool is_fg);

#endif /* TERM_H */

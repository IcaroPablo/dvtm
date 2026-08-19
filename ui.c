/*
 * ui.c - painting a terminal's cells with ncurses, and owning the colour pairs.
 *
 * The other half of term.c: everything here is about getting cells onto the
 * screen. Nothing in this file talks to the child or to libvterm's parser.
 */
#include <langinfo.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "term.h"

int32_t default_fg = -1, default_bg = -1;
bool has_default_colors;

/* ── colour ───────────────────────────────────────────────────────────────── */

/* The 256-colour palette as rgb. Kept identical to what vt.c produced, so a
 * program's colours do not shift underneath the user when the engine changes. */
static int32_t color_index(int i)
{
	static const int32_t bright[8] = { 0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
	                                   0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff };
	static const int cube[6] = { 0, 95, 135, 175, 215, 255 };

	if (i < 8)
		return i;
	if (i < 16)
		return bright[i - 8];
	if (i < 232) {
		i -= 16;
		return RGB(cube[i / 36], cube[(i / 6) % 6], cube[i % 6]);
	}
	if (i < 256) {
		int v = 8 + (i - 232) * 10;
		return RGB(v, v, v);
	}
	return -1;
}

/* A libvterm colour as an ncurses colour number. With a direct-colour terminal
 * ncurses treats the number as packed rgb, which is what RGB() produces. */
int32_t color_of(const VTermColor *c, bool is_fg)
{
	if (VTERM_COLOR_IS_DEFAULT_FG(c) || VTERM_COLOR_IS_DEFAULT_BG(c))
		return -1;
	if (VTERM_COLOR_IS_INDEXED(c))
		return color_index(c->indexed.idx);
	if (VTERM_COLOR_IS_RGB(c))
		return RGB(c->rgb.red, c->rgb.green, c->rgb.blue);
	return is_fg ? default_fg : default_bg;
}

int term_color_get(Term *t, int32_t fg, int32_t bg)
{
	if (fg == -1)
		fg = (t ? t->deffg : default_fg);
	if (bg == -1)
		bg = (t ? t->defbg : default_bg);

	if (!has_default_colors) {
		if (fg == -1)
			fg = default_fg;
		if (bg == -1)
			bg = default_bg;
	}
	if (fg == -1 && bg == -1)
		return 0;

	int pair = alloc_pair(fg, bg);
	return pair > 0 ? pair : 0;
}

/* Ask ncurses what this terminal calls its own default colours -- what a -1 in
 * a colour pair means. Called from term_init. */
void ui_init_colors(void)
{
	short fg = -1, bg = -1;

	/* Deliberately short locals, initialised. pair_content writes shorts:
	 * pointing it at the int32_t globals would leave their upper half
	 * untouched, turning a -1 into 65535 and every default-coloured cell into
	 * an arbitrary colour. And if pair_content fails outright, the locals must
	 * already hold something sane rather than stack garbage. */
	pair_content(0, &fg, &bg);
	default_fg = fg == -1 ? COLOR_WHITE : fg;
	default_bg = bg == -1 ? COLOR_BLACK : bg;
	has_default_colors = (use_default_colors() == OK);
	term_color_get(NULL, COLOR_WHITE, COLOR_BLACK);
}

/* ── painting ─────────────────────────────────────────────────────────────── */

/* The cells for one visible row: either from the scrollback, when scrolled
 * back, or from the live screen. Returns false if there is nothing there. */
static bool row_cells(Term *t, int row, VTermScreenCell *out)
{
	int sb_row = t->sb_count - t->scroll + row;

	if (t->scroll && sb_row < t->sb_count) {
		Line *l = term_sb_at(t, sb_row);
		if (!l)
			return false;
		for (int c = 0; c < t->cols; c++) {
			if (c < l->cols)
				out[c] = l->cells[c];
			else
				memset(&out[c], 0, sizeof *out);
		}
		return true;
	}

	for (int c = 0; c < t->cols; c++) {
		VTermPos pos = { .row = row - t->scroll, .col = c };
		if (!vterm_screen_get_cell(t->screen, pos, &out[c]))
			memset(&out[c], 0, sizeof *out);
	}
	return true;
}

static attr_t attrs_of(const VTermScreenCell *cell)
{
	attr_t a = A_NORMAL;

	if (cell->attrs.bold)      a |= A_BOLD;
	if (cell->attrs.underline) a |= A_UNDERLINE;
	if (cell->attrs.blink)     a |= A_BLINK;
	if (cell->attrs.reverse)   a |= A_REVERSE;
	if (cell->attrs.italic)    a |= A_ITALIC;
	return a;
}

void term_draw(Term *t, WINDOW *win, int srow, int scol)
{
	VTermScreenCell *cells;

	if (srow != t->srow || scol != t->scol) {
		t->dirty = true;
		t->srow = srow;
		t->scol = scol;
	}

	if (!(cells = calloc((size_t)t->cols, sizeof *cells)))
		return;

	for (int row = 0; row < t->rows; row++) {
		int x, y;

		if (!row_cells(t, row, cells))
			continue;
		wmove(win, srow + row, scol);

		for (int col = 0; col < t->cols; col++) {
			VTermScreenCell *cell = &cells[col];
			int pair;

			wattrset(win, attrs_of(cell) | t->defattrs);

			/* An int, handed over as the extended colour pair. A short is too
			 * small for the numbers alloc_pair() returns once more than a few
			 * hundred colours are on screen, and that is exactly what carries
			 * 24-bit colour. Passing a short here reads two bytes of adjacent
			 * stack as the top half of the pair number and paints arbitrary
			 * colours. */
			pair = term_color_get(t, color_of(&cell->fg, true),
			                       color_of(&cell->bg, false));
			wcolor_set(win, 0, &pair);

			if (cell->chars[0] == 0) {
				waddch(win, ' ');
				continue;
			}

			{
				char buf[MB_LEN_MAX * VTERM_MAX_CHARS_PER_CELL + 1];
				size_t len = 0;
				mbstate_t ps;

				memset(&ps, 0, sizeof ps);
				for (int n = 0; n < VTERM_MAX_CHARS_PER_CELL && cell->chars[n]; n++) {
					size_t k = wcrtomb(buf + len, (wchar_t)cell->chars[n], &ps);
					if (k == (size_t)-1)
						break;
					len += k;
				}
				if (len)
					waddnstr(win, buf, (int)len);
				else
					waddch(win, ' ');
			}
			if (cell->width > 1)
				col += cell->width - 1;
		}

		/* Blank the rest of the row rather than leaving whatever was there. */
		getyx(win, y, x);
		(void)y;
		if (x && x < t->cols)
			whline(win, ' ', t->cols - x);
	}

	/* Leave the ncurses cursor where the child put its own, so the terminal
	 * draws it in the right place. Without this it stays wherever the last
	 * character was written. */
	wmove(win, srow + t->cursor.row, scol + t->cursor.col);

	t->dirty = false;
	free(cells);
}


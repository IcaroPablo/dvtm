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

/* Whether the host terminal takes a colour number as packed rgb. ncurses does
 * that only for a direct-colour terminfo (the RGB capability, COLORS of
 * 0x1000000); anywhere else a colour number is an index into a palette, and a
 * packed rgb value is far past the end of it. */
static bool direct_color;

/* ── colour ───────────────────────────────────────────────────────────────── */

static const int cube[6] = { 0, 95, 135, 175, 215, 255 };

/* rgb for the 16 ANSI entries as an unthemed terminal draws them. A themed one
 * differs, and that is fine: this only has to pick a plausible nearest, not
 * reproduce somebody's palette. */
static const int32_t ansi[16] = {
    0x000000,
    0xcd0000,
    0x00cd00,
    0xcdcd00,
    0x0000ee,
    0xcd00cd,
    0x00cdcd,
    0xe5e5e5,
    0x7f7f7f,
    0xff0000,
    0x00ff00,
    0xffff00,
    0x5c5cff,
    0xff00ff,
    0x00ffff,
    0xffffff,
};

static long rgb_dist(int r, int g, int b, int32_t c) {
    long dr = r - ((c >> 16) & 0xff), dg = g - ((c >> 8) & 0xff),
         db = b - (c & 0xff);
    return dr * dr + dg * dg + db * db;
}

/* The nearest of the colours this terminal actually has, when it has 16 or
 * fewer. Without this an approximation lands somewhere in 16-255, which is
 * past the end of the palette, and every cell falls back to the default. */
static int32_t nearest_ansi(int r, int g, int b) {
    int limit = COLORS < 16 ? COLORS : 16;
    int best = 0, i;

    if (limit <= 0)
        return -1;
    for (i = 1; i < limit; i++)
        if (rgb_dist(r, g, b, ansi[i]) < rgb_dist(r, g, b, ansi[best]))
            best = i;
    return best;
}

/* The nearest 256-palette entry to an rgb triple: the best of the 6x6x6 cube
 * and the 24-step grey ramp. Used when the terminal cannot be told an rgb
 * value directly -- an approximation is the whole point, and it is still far
 * better than the default colour, which is what an out-of-range number gets. */
static int32_t nearest_index(int r, int g, int b) {
    int ci[3], best_cube = 0, grey_step, i, c;
    const int v[3] = { r, g, b };
    long dist_cube = 0, dist_grey = 0;

    for (c = 0; c < 3; c++) {
        int best = 0;
        for (i = 1; i < 6; i++)
            if (abs(cube[i] - v[c]) < abs(cube[best] - v[c]))
                best = i;
        ci[c] = best;
        dist_cube += (long)(cube[best] - v[c]) * (cube[best] - v[c]);
    }
    best_cube = 16 + 36 * ci[0] + 6 * ci[1] + ci[2];

    grey_step = (r * 299 + g * 587 + b * 114) / 1000;
    grey_step = (grey_step - 8 + 5) / 10;
    if (grey_step < 0)
        grey_step = 0;
    if (grey_step > 23)
        grey_step = 23;
    for (c = 0; c < 3; c++) {
        int gv = 8 + grey_step * 10;
        dist_grey += (long)(gv - v[c]) * (gv - v[c]);
    }

    return dist_grey < dist_cube ? 232 + grey_step : best_cube;
}

/* An rgb triple as a colour number this terminal can be given. */
static int32_t nearest_color(int r, int g, int b) {
    return COLORS >= 256 ? nearest_index(r, g, b) : nearest_ansi(r, g, b);
}

/* The 256-colour palette as rgb. Kept identical to what vt.c produced, so a
 * program's colours do not shift underneath the user when the engine changes. */
static int32_t index_rgb(int i) {
    if (i < 16)
        return ansi[i];
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

/* A palette entry as a colour number for this terminal. A direct-colour
 * terminal wants the rgb; anything else wants an index, and one it actually
 * has -- asking a 16-colour terminal for entry 196 gets the default colour. */
static int32_t color_index(int i) {
    int32_t c;

    if (direct_color)
        return i < 8 ? i : index_rgb(i);
    if (i < COLORS)
        return i;
    if ((c = index_rgb(i)) < 0)
        return -1;
    return nearest_color((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}

/* A libvterm colour as an ncurses colour number. With a direct-colour terminal
 * ncurses treats the number as packed rgb, which is what RGB() produces. */
int32_t color_of(const VTermColor *c, bool is_fg) {
    if (VTERM_COLOR_IS_DEFAULT_FG(c) || VTERM_COLOR_IS_DEFAULT_BG(c))
        return -1;
    if (VTERM_COLOR_IS_INDEXED(c))
        return color_index(c->indexed.idx);
    if (VTERM_COLOR_IS_RGB(c))
        return direct_color
                   ? RGB(c->rgb.red, c->rgb.green, c->rgb.blue)
                   : nearest_color(c->rgb.red, c->rgb.green, c->rgb.blue);
    return is_fg ? default_fg : default_bg;
}

/* -1 means "whatever this terminal calls its default", and it is passed to
 * ncurses as -1 wherever ncurses can carry that -- which is what
 * use_default_colors() buys, and it emits ESC[39m / ESC[49m for it.
 *
 * Substituting a concrete colour for -1 here instead is what painted the unused
 * tags and the borders black on black: on a direct-colour terminal a colour
 * number is an rgb value, so the "default" read out of pair 0 is 0, which is
 * black, and black on a dark background is nothing at all. A concrete colour is
 * only correct when the terminal cannot express a default. */
int term_color_get(Term *t, int32_t fg, int32_t bg) {
    if (fg == -1 && t)
        fg = t->deffg;
    if (bg == -1 && t)
        bg = t->defbg;

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
void ui_init_colors(void) {
    short fg = -1, bg = -1;

    /* Deliberately short locals, initialised. pair_content writes shorts:
     * pointing it at the int32_t globals would leave their upper half
     * untouched, turning a -1 into 65535 and every default-coloured cell into
     * an arbitrary colour. And if pair_content fails outright, the locals must
     * already hold something sane rather than stack garbage. */
    direct_color = COLORS >= (1 << 24);

    /* use_default_colors() first: it is what decides whether -1 can be handed
     * to ncurses at all, and pair 0 only answers usefully afterwards. */
    has_default_colors = (use_default_colors() == OK);
    pair_content(0, &fg, &bg);
    default_fg = fg;
    default_bg = bg;

    /* Only where a default cannot be expressed does a concrete colour have to
     * be invented, and then white on black is the conventional guess. */
    if (!has_default_colors) {
        if (default_fg == -1)
            default_fg = COLOR_WHITE;
        if (default_bg == -1)
            default_bg = COLOR_BLACK;
    }
    term_color_get(NULL, COLOR_WHITE, COLOR_BLACK);
}

/* ── painting ─────────────────────────────────────────────────────────────── */

/* The cells for one visible row: either from the scrollback, when scrolled
 * back, or from the live screen. Returns false if there is nothing there. */
static bool row_cells(Term *t, int row, VTermScreenCell *out) {
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

static attr_t attrs_of(const VTermScreenCell *cell) {
    attr_t a = A_NORMAL;

    if (cell->attrs.bold)
        a |= A_BOLD;
    if (cell->attrs.underline)
        a |= A_UNDERLINE;
    if (cell->attrs.blink)
        a |= A_BLINK;
    if (cell->attrs.reverse)
        a |= A_REVERSE;
    if (cell->attrs.italic)
        a |= A_ITALIC;
    return a;
}

void term_draw(Term *t, WINDOW *win, int srow, int scol) {
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
            pair = term_color_get(
                t, color_of(&cell->fg, true), color_of(&cell->bg, false));
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
                for (int n = 0; n < VTERM_MAX_CHARS_PER_CELL && cell->chars[n];
                    n++) {
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

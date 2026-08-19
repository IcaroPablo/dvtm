#include "dvtm.h"

/* The master area along one edge, everything else side by side below or above
 * it. bstack puts the masters at the top, tstack at the bottom -- which was the
 * whole difference between two files of the same fifty lines.
 *
 * The nmaster intersections are drawn only with the masters on top, because
 * that is what tstack did: it never drew them. Kept as it was rather than
 * quietly changed. */
static void hstack(bool master_top) {
    unsigned int i, n, nx, ny, nw, nh, m, mw, mh, tw;
    Client *c;

    for (n = 0, c = nextvisible(clients); c; c = nextvisible(c->next))
        if (!c->minimized)
            n++;

    m = MAX(1, MIN(n, screen.nmaster));
    mh = n == m ? wah : screen.mfact * wah;
    mw = waw / m;
    tw = n == m ? 0 : waw / (n - m);
    nx = wax;
    ny = master_top ? way : way + wah - mh;

    for (i = 0, c = nextvisible(clients); c; c = nextvisible(c->next)) {
        if (c->minimized)
            continue;
        if (i < m) { /* master */
            if (i > 0) {
                mvvline(ny, nx, ACS_VLINE, nh);
                mvaddch(ny, nx, ACS_TTEE);
                nx++;
            }
            nh = mh;
            nw = (i < m - 1) ? mw : (wax + waw) - nx;
        } else { /* stacked window */
            if (i == m) {
                nx = wax;
                ny = master_top ? ny + mh : way;
                nh = wah - mh;
            }
            if (i > m) {
                mvvline(ny, nx, ACS_VLINE, nh);
                mvaddch(ny, nx, ACS_TTEE);
                nx++;
            }
            nw = (i < n - 1) ? tw : (wax + waw) - nx;
        }
        resize(c, nx, ny, nw, nh);
        nx += nw;
        i++;
    }

    if (master_top && n > m) {
        nx = wax;
        for (i = 0; i < m; i++) {
            if (i > 0) {
                mvaddch(ny, nx, ACS_PLUS);
                nx++;
            }
            nw = (i < m - 1) ? mw : (wax + waw) - nx;
            nx += nw;
        }
    }
}

/* static inline, not static: the two names live in one file now, so a config
 * that uses only one of them would otherwise warn about the other being
 * unused. An unused static inline is the ordinary way to put a function in a
 * header and no compiler complains about it. */
static inline void bstack(void) {
    hstack(true);
}

static inline void tstack(void) {
    hstack(false);
}

#include "dvtm.h"

static void tile(void) {
    unsigned int i, n, nx, ny, nw, nh, m, mw, mh, th;
    Client *c;

    for (n = 0, c = nextvisible(clients); c; c = nextvisible(c->next))
        if (!c->minimized)
            n++;

    m = MAX(1, MIN(n, screen.nmaster));
    mw = n == m ? waw : screen.mfact * waw;
    mh = wah / m;
    th = n == m ? 0 : wah / (n - m);
    nx = wax;
    ny = way;

    for (i = 0, c = nextvisible(clients); c; c = nextvisible(c->next)) {
        if (c->minimized)
            continue;
        if (i < m) { /* master */
            nw = mw;
            nh = (i < m - 1) ? mh : (way + wah) - ny;
        } else { /* tile window */
            if (i == m) {
                ny = way;
                nx += mw;
                mvvline(ny, nx, ACS_VLINE, wah);
                mvaddch(ny, nx, ACS_TTEE);
                nx++;
                nw = waw - mw - 1;
            }
            nh = (i < n - 1) ? th : (way + wah) - ny;
            if (i > m)
                mvaddch(ny, nx - 1, ACS_LTEE);
        }
        resize(c, nx, ny, nw, nh);
        ny += nh;
        i++;
    }

    /* Fill in nmaster intersections: a cross where a master boundary lines up
     * with a stack one, a tee where it does not.
     *
     * th is a row count and reaches zero once there are more stacked windows
     * than rows to give them -- a dozen windows on a six-row terminal. There is
     * then nothing for a boundary to line up with, so the cross is never right,
     * and `% th` was a division by zero. It is SIGFPE on x86-64; on arm64 the
     * instruction is defined to return the dividend, so this can be dismissed
     * as theoretical on the wrong machine. */
    if (n > m) {
        ny = way + mh;
        for (i = 1; i < m; i++) {
            mvaddch(ny, nx - 1,
                (th && (ny - 1) % th == 0) ? ACS_PLUS : ACS_RTEE);
            ny += mh;
        }
    }
}

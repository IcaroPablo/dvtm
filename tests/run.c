/* dvtm test suite.
 *
 * Runs the real dvtm binary on a pty and asserts on what it paints. Black-box
 * on purpose: these tests must survive vt.c being replaced by libvterm, so
 * they may not touch any internal API.
 *
 * Two assertion layers, because the claims are of two kinds:
 *   - bytes  — protocol claims ("DSR 5 is answered with ESC[0n"). Asserted on
 *              the raw stream, because that is literally what they are about.
 *   - cells  — picture claims ("the window is gone"). Asserted on a screen
 *              model built by feeding dvtm's output to libvterm, because
 *              matching raw bytes for these rots into false greens.
 *
 * Nothing sleeps. Every wait is on an observable with a deadline; a fixed
 * sleep is how the old testsuite.sh became timing-dependent.
 *
 * A green run here is not proof the program works. Three real drawing bugs --
 * the color pair passed as a short where ncurses wants an int, TERM never set
 * for children, the cursor never repositioned -- all survived a fully green
 * suite and were found by running a real shell inside dvtm and reading the
 * bytes it emitted. After touching drawing code, do that too. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <vterm.h>

#define ROWS 24
#define COLS 80
#define OBUF (1u << 20)

#define MOD 0x07 /* CTRL+g, the default modifier in config.def.h */

static int mfd = -1;   /* pty master */
static pid_t kid = -1; /* dvtm */
static VTerm *vt;
static VTermScreen *vts;
static char obuf[OBUF]; /* every byte dvtm has written */
static size_t olen;
static char tinfo[4096]; /* TERMINFO_DIRS for the spawned dvtm */

static int failures;
static int checks;
static int skipped;

/* ── plumbing ─────────────────────────────────────────────────────────────── */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* memmem is not POSIX; the suite has to build on any Unix. */
static const char *findmem(
    const char *hay, size_t hn, const char *nee, size_t nn) {
    if (nn == 0 || hn < nn)
        return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, nee, nn) == 0)
            return hay + i;
    return NULL;
}

/* The TERM dvtm itself is started with. Not a constant, because dvtm has to
 * work on a terminal that has no direct colour too. */
static const char *outer_term = "xterm-direct";

/* ── the pty ──────────────────────────────────────────────────────────────── */

/* POSIX only: forkpty(3) is not POSIX and is exactly why dvtm carries
 * per-platform files today. Phase 2 lifts this into dvtm itself. */
/* Kept so the window size can be changed after the fork: the slave is where
 * TIOCSWINSZ works on every system, and the parent closes its copy below. */
static char slave_name[256];

static void spawn_dvtm(char *const argv[]) {
    struct winsize ws;
    const char *slave;
    int sfd;

    if ((mfd = posix_openpt(O_RDWR | O_NOCTTY)) < 0)
        die("posix_openpt: %s", strerror(errno));
    if (grantpt(mfd) < 0)
        die("grantpt: %s", strerror(errno));
    if (unlockpt(mfd) < 0)
        die("unlockpt: %s", strerror(errno));
    if (!(slave = ptsname(mfd)))
        die("ptsname: %s", strerror(errno));
    snprintf(slave_name, sizeof slave_name, "%s", slave);

    if ((sfd = open(slave, O_RDWR)) < 0)
        die("open %s: %s", slave, strerror(errno));

    /* Before the fork, always. A pty left at 0x0 makes dvtm paint nothing at
     * all, which reads as a broken build rather than a missing ioctl.
     *
     * On the slave, not the master: Linux accepts TIOCSWINSZ on either end,
     * but macOS rejects it on the master with ENOTTY. The slave works
     * everywhere, so there is one call and no per-platform branch. */
    memset(&ws, 0, sizeof ws);
    ws.ws_row = ROWS;
    ws.ws_col = COLS;
    if (ioctl(sfd, TIOCSWINSZ, &ws) < 0)
        die("TIOCSWINSZ: %s", strerror(errno));

    if ((kid = fork()) < 0)
        die("fork: %s", strerror(errno));

    if (kid == 0) {
        setsid();
        if (ioctl(sfd, TIOCSCTTY, 0) < 0)
            _exit(126);
        dup2(sfd, 0);
        dup2(sfd, 1);
        dup2(sfd, 2);
        if (sfd > 2)
            close(sfd);
        close(mfd);
        /* Direct colour by default, so truecolour is actually exercised
         * rather than quietly downgraded to the 256 palette. One case
         * overrides it: see t_palette_terminal. */
        setenv("TERM", outer_term, 1);
        setenv("TERMINFO_DIRS", tinfo, 1);
        unsetenv("ESCDELAY");
        execv(argv[0], argv);
        _exit(127);
    }

    close(sfd);
}

static void reap(void) {
    if (kid > 0) {
        kill(kid, SIGKILL);
        waitpid(kid, NULL, 0);
        kid = -1;
    }
    if (mfd >= 0) {
        close(mfd);
        mfd = -1;
    }
    if (vt) {
        vterm_free(vt);
        vt = NULL;
    }
}

/* ── screen model ─────────────────────────────────────────────────────────── */

static void screen_init(void) {
    vt = vterm_new(ROWS, COLS);
    vterm_set_utf8(vt, 1);
    vts = vterm_obtain_screen(vt);
    vterm_screen_reset(vts, 1);
    olen = 0;
}

/* Drain whatever dvtm has written, into both assertion layers. */
static bool pump(int ms) {
    fd_set r;
    struct timeval tv;
    char b[4096];
    ssize_t n;

    FD_ZERO(&r);
    FD_SET(mfd, &r);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (select(mfd + 1, &r, NULL, NULL, &tv) <= 0)
        return false;
    if ((n = read(mfd, b, sizeof b)) <= 0)
        return false;
    if (olen + (size_t)n < sizeof obuf) {
        memcpy(obuf + olen, b, (size_t)n);
        olen += (size_t)n;
    }
    vterm_input_write(vt, b, (size_t)n);
    return true;
}

static void screen_row(int row, char *out, size_t n) {
    VTermPos p;
    size_t o = 0;
    p.row = row;
    for (p.col = 0; p.col < COLS && o + 8 < n; p.col++) {
        VTermScreenCell c;
        if (!vterm_screen_get_cell(vts, p, &c) || c.chars[0] == 0) {
            out[o++] = ' ';
            continue;
        }
        if (c.chars[0] < 0x80)
            out[o++] = (char)c.chars[0];
        else
            out[o++] = '?';
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = '\0';
}

/* How many rows carry the text. Enough to tell "still there once" from "written
 * to the window a second time", which is how a paste that should not have
 * happened shows up. */
static int screen_count(const char *text) {
    char line[COLS + 8];
    int n = 0;
    for (int r = 0; r < ROWS; r++) {
        screen_row(r, line, sizeof line);
        if (strstr(line, text))
            n++;
    }
    return n;
}

static int screen_occurrences(const char *text) {
    char line[COLS + 8];
    size_t n = strlen(text);
    int total = 0;

    for (int r = 0; r < ROWS; r++) {
        screen_row(r, line, sizeof line);
        for (char *p = line; (p = strstr(p, text)); p += n)
            total++;
    }
    return total;
}

static bool screen_has(const char *text) {
    char line[COLS + 8];
    for (int r = 0; r < ROWS; r++) {
        screen_row(r, line, sizeof line);
        if (strstr(line, text))
            return true;
    }
    return false;
}

static void screen_dump(void) {
    char line[COLS + 8];
    printf("    ---- screen ----\n");
    for (int r = 0; r < ROWS; r++) {
        screen_row(r, line, sizeof line);
        printf("    %2d|%s\n", r, line);
    }
}

/* Locate text on screen; returns false if it is not there. */
static bool screen_find(const char *text, VTermPos *at) {
    char line[COLS + 8];

    for (int r = 0; r < ROWS; r++) {
        char *hit;
        screen_row(r, line, sizeof line);
        if ((hit = strstr(line, text))) {
            at->row = r;
            at->col = (int)(hit - line);
            return true;
        }
    }
    return false;
}

/* Is this cell painted in the terminal's default foreground, or has something
 * chosen a colour for it? */
static bool cell_fg_is_default(int row, int col) {
    VTermPos p = { .row = row, .col = col };
    VTermScreenCell c;

    if (!vterm_screen_get_cell(vts, p, &c))
        return false;
    return VTERM_COLOR_IS_DEFAULT_FG(&c.fg);
}

/* Where the cursor ended up, as a real terminal would place it. */
static void cursor_pos(int *row, int *col) {
    VTermPos p;
    vterm_state_get_cursorpos(vterm_obtain_state(vt), &p);
    *row = p.row;
    *col = p.col;
}

/* ── waiting ──────────────────────────────────────────────────────────────── */

static bool wait_bytes(const char *needle, int ms) {
    long deadline = now_ms() + ms;
    do {
        if (findmem(obuf, olen, needle, strlen(needle)))
            return true;
        pump(50);
    } while (now_ms() < deadline);
    return findmem(obuf, olen, needle, strlen(needle)) != NULL;
}

static bool wait_screen(const char *text, int ms) {
    long deadline = now_ms() + ms;
    do {
        if (screen_has(text))
            return true;
        pump(50);
    } while (now_ms() < deadline);
    return screen_has(text);
}

/* The stand-in editor drops a file before it exits, so a check can tell "it ran
 * and decided to write nothing back" from "it never ran at all". */
static bool wait_screen_gone(const char *text, int ms) {
    long deadline = now_ms() + ms;
    do {
        if (!screen_has(text))
            return true;
        pump(50);
    } while (now_ms() < deadline);
    return !screen_has(text);
}

static bool wait_file(const char *path, int ms) {
    struct stat st;
    long deadline = now_ms() + ms;
    do {
        if (stat(path, &st) == 0)
            return true;
        pump(50);
    } while (now_ms() < deadline);
    return stat(path, &st) == 0;
}

/* The inverse of wait_file: waits for something to go away. Pumping while it
 * waits is not incidental — dvtm writes as it tears the screen down, and a
 * caller that stops reading the pty can block it before it reaches its own
 * cleanup, which looks exactly like the cleanup never running. */
static bool wait_gone(const char *path, int ms) {
    struct stat st;
    long deadline = now_ms() + ms;
    do {
        if (stat(path, &st) != 0)
            return true;
        pump(50);
    } while (now_ms() < deadline);
    return stat(path, &st) != 0;
}

static void settle(int ms) {
    long deadline = now_ms() + ms;
    while (now_ms() < deadline)
        pump(20);
}

/* ── driving dvtm ─────────────────────────────────────────────────────────── */

static void tty_write(const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(mfd, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            /* Never silent: a swallowed write here looks exactly like dvtm
             * ignoring a keystroke, and that is the bug this suite exists to
             * detect. */
            printf("    tty_write: %s (dropped %lu bytes)\n", strerror(errno),
                (unsigned long)n);
            return;
        }
        if (w == 0) {
            printf("    tty_write: wrote 0 of %lu bytes\n", (unsigned long)n);
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

/* ── reporting ────────────────────────────────────────────────────────────── */

static void ok(const char *name) {
    checks++;
    printf("OK   %s\n", name);
    fflush(stdout);
}

/* Escaped tail of everything dvtm wrote. A failing screen assertion rarely
 * says why on its own; the bytes do. */
static void bytes_dump(size_t want) {
    size_t start = olen > want ? olen - want : 0;
    printf("    ---- last %lu bytes dvtm wrote ----\n    ",
        (unsigned long)(olen - start));
    for (size_t i = start; i < olen; i++) {
        unsigned char c = (unsigned char)obuf[i];
        if (c == 0x1b)
            printf("\\e");
        else if (c == '\n')
            printf("\\n");
        else if (c < 0x20 || c >= 0x7f)
            printf("\\x%02x", c);
        else
            putchar(c);
    }
    putchar('\n');
}

/* A check we deliberately do not make, and the reason. Printed like a result
 * rather than hidden in a comment, so `make test` says out loud what is not
 * being covered -- a silently deleted assertion is indistinguishable from one
 * nobody thought of. */
static void skip(const char *name, const char *why) {
    skipped++;
    printf("SKIP %s\n", name);
    printf("     %s\n", why);
    fflush(stdout);
}

static void fail(const char *name, const char *why) {
    checks++;
    failures++;
    printf("FAIL %s\n", name);
    printf("    %s\n", why);
    screen_dump();
    bytes_dump(700);
    fflush(stdout);
}

static void check(const char *name, bool cond, const char *why) {
    if (cond)
        ok(name);
    else
        fail(name, why);
}

/* Locate text on screen; returns false if absent. */
/* Did dvtm put this rgb triple on the wire? Either separator form counts:
 * which one is used is the terminal description's business (xterm-direct's
 * setrgbf is colon-separated, xterm-direct-sc's is semicolon-separated), and
 * asserting one form would be asserting the terminfo rather than dvtm.
 *
 * Asserted on bytes rather than on cells on purpose. libvterm 0.3.3 misparses
 * the ITU-T T.416 form `38:2::r:g:b`, dropping the empty colourspace field and
 * reading (255,10,200) for (10,200,30) — verified in isolation. Since that is
 * the form xterm-direct emits, the harness's own screen model cannot be
 * trusted for this one claim. The bytes can. */
static bool wait_rgb(int r, int g, int b, int ms) {
    char colon[64], semi[64];
    snprintf(colon, sizeof colon, "38:2::%d:%d:%d", r, g, b);
    snprintf(semi, sizeof semi, "38;2;%d;%d;%d", r, g, b);
    return wait_bytes(colon, ms) || wait_bytes(semi, 10);
}

/* Change the terminal size behind dvtm's back. Setting it on the slave makes
 * the kernel deliver SIGWINCH to dvtm, which is the point: the redraw that
 * follows can only happen if the main loop woke up on the signal. */
static void resize_tty(int rows, int cols) {
    struct winsize ws;
    int fd;

    memset(&ws, 0, sizeof ws);
    ws.ws_row = rows;
    ws.ws_col = cols;
    if ((fd = open(slave_name, O_RDWR | O_NOCTTY)) < 0)
        die("open %s: %s", slave_name, strerror(errno));
    if (ioctl(fd, TIOCSWINSZ, &ws) < 0)
        die("TIOCSWINSZ: %s", strerror(errno));
    close(fd);
}

/* ── the cases ────────────────────────────────────────────────────────────── */

static char dvtm_path[1024];

/* start() with the whole argument list spelled out, for the flags that decide
 * which descriptors the main loop watches. */
static void start_argv(const char *args[]) {
    char *argv[10];
    int n = 0;

    argv[n++] = dvtm_path;
    for (int i = 0; args[i] && n < 9; i++)
        argv[n++] = (char *)args[i];
    argv[n] = NULL;
    screen_init();
    spawn_dvtm(argv);
}

static void start(const char *w1, const char *w2, const char *w3) {
    char *argv[8];
    int n = 0;
    argv[n++] = dvtm_path;
    if (w1)
        argv[n++] = (char *)w1;
    if (w2)
        argv[n++] = (char *)w2;
    if (w3)
        argv[n++] = (char *)w3;
    argv[n] = NULL;
    screen_init();
    spawn_dvtm(argv);
}

static void t_startup(void) {
    start("tests/probe mark HELLO", NULL, NULL);
    check("startup: dvtm paints a window", wait_screen("HELLO", 5000),
        "dvtm produced no window containing the marker within 5s");
    reap();
}

static void t_dsr(void) {
    start("tests/probe dsr", NULL, NULL);
    check("dsr 5: answered with ESC[0n", wait_screen("DSR5=ESC[0n", 5000),
        "the probe did not report ESC[0n; dvtm left DSR 5 unanswered");
    reap();
}

static void t_truecolor(void) {
    start("tests/probe truecolor", NULL, NULL);
    wait_screen("PAL256", 5000);
    settle(300);

    check("truecolor: ';' separator survives to the outer terminal",
        wait_rgb(10, 200, 30, 2000),
        "rgb(10,200,30) from the ';' form never reached the wire");

    /* Known limitation, not an oversight. libvterm 0.3.3 -- the newest release
     * that exists -- misparses `38:2::r:g:b`, dropping the empty colourspace
     * field ITU-T T.416 requires and reading (255,20,100) for (20,100,250).
     *
     * Nothing was lost here. vt.c did worse: it split parameters on `;` alone,
     * so a colon fell through its parser and the digits after it kept piling
     * into the parameter before, turning the whole colour into one long
     * number. libvterm at least reads the abbreviated `38:2:r:g:b` correctly.
     *
     * Accepted rather than worked around: dvtm.info tells children to use the
     * ';' form, so every program that asks terminfo is unaffected.
     *
     * The assertion is kept below, commented, so that re-enabling it is a
     * one-line change the day libvterm fixes this. Uncomment it and drop the
     * skip() call. */
    /*
    check("truecolor: ':' subparameter separator survives to the outer terminal",
          wait_rgb(20, 100, 250, 2000),
          "rgb(20,100,250) from the ':' form never reached the wire");
    */
    skip("truecolor: ':' subparameter separator survives to the outer "
         "terminal",
        "libvterm 0.3.3 misparses 38:2::r:g:b (reads 255,20,100); "
        "see README.md under Limitations.");

    check("256 palette resolves to rgb", wait_rgb(255, 0, 0, 2000),
        "palette entry 196 did not resolve to rgb(255,0,0)");

    check("all three colour forms are painted",
        screen_has("SEMI") && screen_has("COLON") && screen_has("PAL256"),
        "one of SEMI / COLON / PAL256 is missing from the screen");
    reap();
}

static void t_faint(void) {
    start("tests/probe faint", NULL, NULL);
    wait_screen("FAINT", 5000);
    settle(300);

    check("faint: the text itself reaches the terminal", screen_has("FAINT"),
        "the probe's line never appeared");

    /* Known limitation, not an oversight. libvterm 0.3.3 has no faint bit:
     * VTermScreenCellAttrs carries bold, underline, italic, blink, reverse,
     * conceal, strike, font, dwl, dhl, small and baseline, and nothing for
     * SGR 2. The cell it returns for faint text is byte for byte the cell it
     * returns for unstyled text, so there is nothing for dvtm to map onto
     * A_DIM.
     *
     * This is something the fork lost, not something dvtm never had. vt.c set
     * A_DIM on SGR 2, cleared it on 22, and wrote it back out as `;2` in copy
     * mode. Anyone whose faint text worked before the move to libvterm sees it
     * at full brightness now.
     *
     * The assertion is kept below, commented, so that re-enabling it is a
     * one-line change the day libvterm models faint. Uncomment it and drop
     * the skip() call. */
    /*
    check("faint: SGR 2 survives to the outer terminal",
          wait_bytes("\033[2m", 2000),
          "the faint text was painted at full brightness");
    */
    skip("faint: SGR 2 survives to the outer terminal",
        "libvterm 0.3.3 has no faint bit in VTermScreenCellAttrs, so dvtm "
        "never sees SGR 2; see README.md under Limitations.");
    reap();
}

/* Count distinct 24-bit colours dvtm put on the wire.
 *
 * Counted from the bytes, not from the cell grid, for the same reason the other
 * colour checks are: libvterm 0.3.3 misparses `38:2::r:g:b`, dropping the empty
 * colourspace field, and that is the form a direct-colour terminfo emits. The
 * harness's own screen model would therefore undercount what dvtm actually
 * sent. The bytes are not in doubt. */
static int emitted_distinct_colors(void) {
    static long seen[4096];
    int n = 0;

    for (size_t i = 0; i + 8 < olen; i++) {
        int r, g, b, consumed = 0;
        long key;

        if (obuf[i] != '3' || obuf[i + 1] != '8')
            continue;
        if (sscanf(obuf + i, "38:2::%d:%d:%d%n", &r, &g, &b, &consumed) != 3 &&
            sscanf(obuf + i, "38;2;%d;%d;%d%n", &r, &g, &b, &consumed) != 3)
            continue;
        key = ((long)(r & 0xff) << 16) | ((long)(g & 0xff) << 8) |
              (long)(b & 0xff);
        int k = 0;
        while (k < n && seen[k] != key)
            k++;
        if (k == n && n < (int)(sizeof seen / sizeof *seen))
            seen[n++] = key;
    }
    return n;
}

static void t_manycolors(void) {
    int n;
    start("tests/probe manycolors", NULL, NULL);
    wait_screen("MANYDONE", 8000);
    settle(600);
    n = emitted_distinct_colors();
    if (n > 255) {
        ok("more than 255 distinct colours reach the terminal");
    } else {
        char why[128];
        snprintf(why, sizeof why,
            "only %d distinct 24-bit colours on the wire; the "
            "extended pair path did not engage",
            n);
        fail("more than 255 distinct colours reach the terminal", why);
    }
    reap();
}

/* dvtm turns every colour into packed rgb and hands it to alloc_pair(). That
 * only works where ncurses reads a colour number as rgb -- a direct-colour
 * terminfo. On a 256-colour terminal the number is past the end of the
 * palette, allocation fails, and every cell paints in the default colour: an
 * editor inside dvtm loses all its syntax highlighting. Every other case here
 * runs on xterm-direct, which is why this one exists. */
static void t_palette_terminal(void) {
    bool got;

    outer_term = "xterm-256color";
    start("tests/probe truecolor", NULL, NULL);
    wait_screen("SEMI", 5000);
    settle(600);
    /* The probe asks for rgb(10,200,30). The nearest entry in the 6x6x6 cube
     * is (0,215,0), which is index 40; the grey ramp is much further away.
     * A specific number, so this fails if the approximation changes rather
     * than only if colour disappears. */
    got = wait_bytes("38;5;40", 10) || wait_bytes("38:5:40", 10);
    check("colour survives a terminal without direct colour", got,
        "rgb(10,200,30) did not come out as palette index 40: either "
        "the "
        "approximation moved, or dvtm painted in the default pair, "
        "which "
        "is what a failed alloc_pair() looks like");
    reap();

    /* And again with sixteen, and with eight. An approximation that lands in
     * 16-255 is past the end of both and fails the same way. The probe's
     * palette 196 is rgb(255,0,0), and the two sizes must answer it
     * differently: with sixteen entries the exact bright red is available
     * (ESC[91m), with eight only the dim one (ESC[31m). Asserting the two
     * apart is what makes this a test of the fold rather than of colour
     * merely being present. */
    outer_term = "xterm-16color";
    start("tests/probe truecolor", NULL, NULL);
    wait_screen("PAL256", 5000);
    settle(600);
    check("sixteen-colour terminal gets the bright ANSI entry",
        wait_bytes("\033[91m", 10),
        "palette 196 did not come out as bright red: the approximation "
        "landed outside the palette this terminal has");
    reap();

    outer_term = "xterm";
    start("tests/probe truecolor", NULL, NULL);
    wait_screen("PAL256", 5000);
    settle(600);
    check("eight-colour terminal gets the plain ANSI entry",
        wait_bytes("\033[31m", 10) && !wait_bytes("\033[91m", 10),
        "palette 196 did not come out as plain red: eight colours is "
        "the "
        "smallest palette dvtm has to fold into, and nothing below 16 "
        "is "
        "reachable if this fails");
    reap();

    outer_term = "xterm-direct";
}

static void t_backspace(void) {
    start("tests/probe backspace", NULL, NULL);
    check("terminfo canary: backspaces survive a redraw",
        wait_screen("ABCXYZ", 5000),
        "expected ABCXYZ; the backspaces were lost between child and "
        "outer terminal");
    reap();
}

/* Typing a non-ASCII character must reach the child as the bytes that were
 * typed.
 *
 * This is a keyboard test, not a drawing one, and the two failed independently:
 * dvtm painted 'á' correctly all along while delivering it to the child as
 * 'Ã¡'. The read side handed the main loop one byte at a time and the byte was
 * then passed on as if it were a code point, so every character above U+007F
 * was re-encoded and arrived at twice its length. Nothing on screen showed it;
 * only the child could tell. */
static void t_utf8_input(void) {
    struct {
        const char *name, *keys, *want;
    } cases[] = {
        /* U+00E1, two bytes: the shortest thing the old path got wrong. */
        { "a two-byte character", "\xc3\xa1", "ECHO=c3a1" },
        /* U+20AC, three bytes: proves the fix is not a two-byte special case. */
        { "a three-byte character", "\xe2\x82\xac", "ECHO=e282ac" },
        /* Still ASCII, and still one byte. A wide read that turned every key
         * into something longer would pass the two above and break every
         * program inside dvtm. */
        { "plain ASCII", "k", "ECHO=6b" },
    };
    char why[200];

    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        char name[80];
        snprintf(
            name, sizeof name, "typing reaches the child: %s", cases[i].name);

        start("tests/probe echo", NULL, NULL);
        if (!wait_screen("ECHOREADY", 5000)) {
            fail(name, "the probe never reported it was reading");
            reap();
            continue;
        }
        tty_write(cases[i].keys, strlen(cases[i].keys));

        snprintf(why, sizeof why,
            "expected %s on screen; dvtm delivered different bytes to the "
            "child than were typed",
            cases[i].want);
        check(name, wait_screen(cases[i].want, 5000), why);
        reap();
    }
}

/* Which windows dvtm currently shows, as a bitmask of its own window ids.
 *
 * The ids are the right observable for "is the window gone". Marker text is
 * not: dvtm titles a window with the command that created it, so a marker
 * passed on the command line appears in the title bar as well as the body, and
 * "the marker is absent" then conflates the window vanishing with the body
 * being repainted. The id is dvtm's own bookkeeping and appears exactly once
 * per live window. */
static unsigned visible_ids(void) {
    unsigned mask = 0;
    for (int i = 1; i <= 9; i++) {
        char pat[8];
        snprintf(pat, sizeof pat, "#%d]", i);
        if (screen_has(pat))
            mask |= 1u << i;
    }
    return mask;
}

static int popcount(unsigned v) {
    int n = 0;
    while (v) {
        n += (int)(v & 1u);
        v >>= 1;
    }
    return n;
}

static bool wait_ids(int want, int ms) {
    long deadline = now_ms() + ms;
    do {
        pump(50);
        if (popcount(visible_ids()) == want)
            return true;
    } while (now_ms() < deadline);
    return popcount(visible_ids()) == want;
}

static void t_kill_removes_window(void) {
    unsigned before, after;
    char why[160];

    /* Identical commands on purpose, so the windows differ only by the id dvtm
     * assigns them. Three, not two: dvtm draws no title bar when a single
     * window is left, so a survivor would report no id at all and the kill
     * would look like it removed everything. */
    start("tests/probe mark W", "tests/probe mark W", "tests/probe mark W");
    if (!wait_ids(3, 6000)) {
        fail("kill removes the window", "the three windows never all appeared");
        reap();
        return;
    }
    before = visible_ids();

    /* MOD x x kills the focused client. The assertion is that the *window*
     * goes, not that the process died — those are different failures, and the
     * difference is the whole point of the case. */
    tty_write("\x07"
              "xx",
        3);

    bool one_left = wait_ids(2, 6000);
    after = visible_ids();
    snprintf(why, sizeof why,
        "window ids before=0x%02X after=0x%02X; expected exactly one "
        "to disappear",
        before, after);
    check("kill removes exactly one window",
        one_left && (after & before) == after && after != before, why);
    reap();
}

/* A window whose program ends by itself has to go, the same as one that is
 * killed.
 *
 * dvtm learns this two ways -- waitpid in the main loop, and EOF on the pty --
 * and the check covers the pair, not either one alone. That is deliberate: what
 * matters is that the window goes, and pretending to isolate the reaping would
 * be a lie, since disabling it leaves the other route working.
 *
 * Three windows because dvtm draws no title bar when one is left, and the ids
 * are what is being counted. */
static void t_exit_removes_window(void) {
    static const char *const name = "a window whose program exits goes away";

    start("tests/probe mark W", "tests/probe mark W", "sh -c 'echo GOING'");
    if (!wait_ids(3, 6000)) {
        fail(name, "the three windows never all appeared");
        reap();
        return;
    }
    check(name, wait_ids(2, 8000),
        "the shell printed and exited, and its window is still on screen: "
        "nothing reaped the child and nothing noticed its pty hang up");
    reap();
}

/* MOD is a control byte, so it cannot be written inline: "\x07f" is one byte,
 * 0x7f, because a hex escape swallows every hex digit that follows it. */
static void send_chord(const char *keys) {
    char buf[8] = { MOD };
    size_t n = strlen(keys);

    if (n + 1 >= sizeof buf)
        die("chord too long: %s", keys);
    memcpy(buf + 1, keys, n);
    tty_write(buf, n + 1);
}

/* dvtm prepends new clients, so the last command on the command line is #1 and
 * the first is #3. */
#define W1 "CCC"
#define W2 "BBB"
#define W3 "AAA"

static bool start_three(const char *name) {
    start(
        "tests/probe mark " W3, "tests/probe mark " W2, "tests/probe mark " W1);
    if (wait_ids(3, 6000))
        return true;
    fail(name, "the three windows never all appeared");
    reap();
    return false;
}

static void t_window_ids(void) {
    char why[200];

    if (!start_three("windows are numbered in the title bar"))
        return;
    snprintf(why, sizeof why,
        "expected the title bars to read '%s | #1', '%s | #2' and '%s | #3'",
        W1, W2, W3);
    check("windows are numbered in the title bar",
        screen_has(W1 " | #1") && screen_has(W2 " | #2") &&
            screen_has(W3 " | #3"),
        why);
    reap();
}

/* Focus has no text of its own on screen, so each case reads it back by killing
 * the focused window and naming which marker went. */
static void t_focus(void) {
    struct {
        const char *name, *away, *keys, *dies;
    } cases[] = {
        { "Mod-1 focuses the first window", "j", "1", W1 },
        { "Mod-2 focuses the second window", NULL, "2", W2 },
        { "Mod-3 focuses the third window", NULL, "3", W3 },
    };

    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        char why[240];

        if (!start_three(cases[i].name))
            continue;
        if (cases[i].away)
            send_chord(cases[i].away);
        send_chord(cases[i].keys);
        settle(400);
        send_chord("xx");
        wait_ids(2, 6000);
        settle(400);

        snprintf(why, sizeof why,
            "expected %s to be the one killed; on screen now: %s=%d %s=%d "
            "%s=%d",
            cases[i].dies, W1, screen_has(W1), W2, screen_has(W2), W3,
            screen_has(W3));
        check(cases[i].name, !screen_has(cases[i].dies), why);
        reap();
    }
}

static void t_focus_moves(void) {
    char why[220];

    if (!start_three("Mod-j moves the focus"))
        return;
    send_chord("2");
    send_chord("j");
    settle(400);
    send_chord("xx");
    wait_ids(2, 6000);
    settle(400);
    snprintf(why, sizeof why,
        "Mod-2 then Mod-j should leave %s focused, and it was not the window "
        "killed",
        W3);
    check("Mod-j moves the focus", !screen_has(W3), why);
    reap();

    if (!start_three("Mod-k undoes Mod-j"))
        return;
    send_chord("2");
    send_chord("j");
    send_chord("k");
    settle(400);
    send_chord("xx");
    wait_ids(2, 6000);
    settle(400);
    snprintf(why, sizeof why,
        "Mod-j then Mod-k should return to %s, and it was not the window "
        "killed",
        W2);
    check("Mod-k undoes Mod-j", !screen_has(W2), why);
    reap();
}

/* The layout symbol dvtm prints in the bar, right after the tag list. Asserted
 * as well as the geometry: tile is the layout dvtm starts in, so a geometry
 * claim alone would pass for a Mod-f that did nothing at all. */
static void t_layouts(void) {
    VTermPos a, b;
    char why[260];

    if (!start_three("Mod-g selects the grid layout"))
        return;

    send_chord("g");
    settle(700);
    check("Mod-g selects the grid layout", screen_has("[5]+++"),
        "the bar never showed the grid symbol");

    send_chord("f");
    settle(700);
    if (screen_find(W1 " | #1", &a) && screen_find(W2 " | #2", &b)) {
        snprintf(why, sizeof why,
            "expected the vertical stack symbol in the bar and the master area "
            "on the left: symbol=%d, #1 col %d, #2 col %d",
            screen_has("[5][]="), a.col, b.col);
        check("Mod-f selects the vertical stack, master area on the left",
            screen_has("[5][]=") && a.col < b.col, why);
    } else {
        fail("Mod-f selects the vertical stack, master area on the left",
            "could not locate both title bars");
    }

    send_chord("b");
    settle(700);
    check("Mod-b selects the bottom stack layout", screen_has("[5]TTT"),
        "the bar never showed the bottom stack symbol");
    if (screen_find(W1 " | #1", &a) && screen_find(W2 " | #2", &b)) {
        snprintf(why, sizeof why,
            "the bottom stack puts the master area on top, so #1 must sit "
            "above #2: #1 row %d, #2 row %d",
            a.row, b.row);
        check(
            "the bottom stack puts the master area on top", a.row < b.row, why);
    } else {
        fail("the bottom stack puts the master area on top",
            "could not locate both title bars");
    }

    send_chord("m");
    settle(700);
    check("Mod-m selects the fullscreen layout", screen_has("[5][ ]"),
        "the bar never showed the fullscreen symbol");
    snprintf(why, sizeof why,
        "fullscreen shows the selected window only: %s=%d %s=%d %s=%d", W1,
        screen_has(W1), W2, screen_has(W2), W3, screen_has(W3));
    check("fullscreen shows one window",
        screen_has(W1) && !screen_has(W2) && !screen_has(W3), why);

    send_chord(" ");
    settle(700);
    check("Mod-Space moves to another layout", !screen_has("[5][ ]"),
        "the layout symbol did not change");
    reap();
}

static void t_tags(void) {
    char why[220];

    start("tests/probe mark " W3, "tests/probe mark " W2, NULL);
    if (!wait_ids(2, 6000)) {
        fail("Mod-t tags the focused window", "the two windows never appeared");
        reap();
        return;
    }

    send_chord("t2");
    send_chord("v2");
    settle(900);
    snprintf(why, sizeof why,
        "the focused window was tagged 2 and tag 2 selected, so only %s "
        "should show: %s=%d %s=%d",
        W2, W2, screen_has(W2), W3, screen_has(W3));
    check("Mod-t tags the focused window", screen_has(W2) && !screen_has(W3),
        why);

    send_chord("v1");
    settle(900);
    snprintf(why, sizeof why,
        "tag 1 should show the untagged window only: %s=%d %s=%d", W2,
        screen_has(W2), W3, screen_has(W3));
    check("Mod-v switches the view", screen_has(W3) && !screen_has(W2), why);
    reap();
}

/* Counted with screen_occurrences and not screen_count: two windows side by
 * side report on the same screen row. */
static void t_runinall(void) {
    struct {
        const char *name, *prefix;
        int want;
    } cases[] = {
        { "typing reaches the focused window only", NULL, 1 },
        { "Mod-a sends what is typed to every window", "a", 2 },
    };

    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        char why[220];
        int got;

        start("tests/probe echo", "tests/probe echo", NULL);
        if (!wait_ids(2, 6000)) {
            fail(cases[i].name, "the two windows never appeared");
            reap();
            continue;
        }
        settle(900);

        if (cases[i].prefix)
            send_chord(cases[i].prefix);
        tty_write("k", 1);
        settle(1500);

        got = screen_occurrences("ECHO=6b");
        snprintf(why, sizeof why,
            "expected %d window(s) to report ECHO=6b, %d did", cases[i].want,
            got);
        check(cases[i].name, got == cases[i].want, why);
        reap();
    }
}

/* These three cover bugs that were live in term.c while the whole suite was
 * green. Each was found by driving a real shell and reading the output, not by
 * the tests, which is the reason they exist now. */

/* dvtm sets TERM for the programs it runs. term.c declared the variable and
 * never filled it in, so every child ran with TERM= and no terminfo: wrong
 * colours, and line editors that cannot position the cursor. */
static void t_child_env(void) {
    start("tests/probe env", NULL, NULL);
    check("children are given a usable TERM", wait_screen("TERM=[dvtm", 6000),
        "the child's TERM is empty or not a dvtm entry; curses "
        "programs "
        "inside dvtm will misbehave");
    check("children are told the terminal does truecolor",
        screen_has("COLORTERM=[truecolor]"),
        "COLORTERM was not set for the child");
    reap();
}

/* Text a program never coloured must stay in the terminal's default colours.
 *
 * Honest note on what this does and does not cover: it was written after a bug
 * where the colour pair was passed to ncurses through a `short` where the
 * extended colour interface wants an `int`, so ncurses read two bytes of
 * adjacent stack as the top half of the pair number and painted everything
 * blue. Reintroducing that bug does *not* make this check fail -- the three
 * truecolor checks above catch it instead. Keep it anyway: it asserts
 * something true and cheap that nothing else asserts, but do not rely on it as
 * the guard for colour-pair handling. */
static void t_plain_text_default_color(void) {
    VTermPos at;
    bool found;

    start("tests/probe plain", NULL, NULL);
    if (!wait_screen("PLAINTEXT", 6000)) {
        fail("uncoloured text stays uncoloured", "the marker never appeared");
        reap();
        return;
    }
    settle(400);

    found = screen_find("PLAINTEXT", &at);
    check("uncoloured text stays uncoloured",
        found && cell_fg_is_default(at.row, at.col),
        "a cell the program never coloured came out with a colour of "
        "its own");
    reap();
}

/* term_draw must leave the cursor where the child put it. Without the final
 * wmove it stays wherever the last character was written, which is the bottom
 * right of the window, and the terminal draws the cursor in the wrong place. */
static void t_cursor_follows_child(void) {
    int row, col;
    char why[128];

    start("tests/probe mark CURSORTEST", NULL, NULL);
    if (!wait_screen("CURSORTEST", 6000)) {
        fail("the cursor follows the child", "the marker never appeared");
        reap();
        return;
    }
    settle(600);

    /* The probe printed one line and stopped, so the child's cursor sits at
     * the start of the next line -- near the top, and certainly not parked in
     * the last column of the screen. */
    cursor_pos(&row, &col);
    snprintf(why, sizeof why,
        "cursor at row %d col %d; the child left it near the top left, "
        "so it "
        "was never moved there",
        row, col);
    check("the cursor follows the child", row <= 2 && col < COLS - 1, why);
    reap();
}

/* Copy mode is the only user of term_forkpty()'s to/from pipes: dvtm hands the
 * window's contents to an editor over one pipe and reads the selection back
 * over the other. Nothing else exercises that path, so a change to how children
 * are forked can break it silently -- which is exactly what Phase 2 changed.
 *
 * The editor is tests/editor, which prints a marker of its own and then the
 * text it was given. The marker only appears if the editor really started; the
 * text only appears if the buffer reached it down the pipe. */
static void t_copymode(void) {
    char chord[2];

    start("tests/probe mark COPYTEXT", NULL, NULL);
    if (!wait_screen("COPYTEXT", 6000)) {
        fail("copy mode starts the editor", "the window never appeared");
        reap();
        return;
    }
    settle(800);

    chord[0] = MOD;
    chord[1] = 'e';
    tty_write(chord, 2);

    check("copy mode starts the editor over the pipe pair",
        wait_screen("EDITORSTARTED", 6000),
        "the stand-in editor never ran; the to/from pipes did not "
        "work");
    check("copy mode keeps the window contents", screen_has("COPYTEXT"),
        "the editor started but the window text did not reach it");
    reap();
}

/* dvtm-editor's contract, which nothing checked: what the editor leaves behind
 * is pasted, and two cases must paste nothing at all -- an editor that exits
 * without saving, and an editor that saves and then fails. The second is the
 * only one that separates the exit status from the content comparison, so it
 * saves the same text the successful case does.
 *
 * The window runs `cat` because the pasted bytes have to be visible: dvtm
 * writes them to the window's pty, and tests/probe puts its terminal in raw
 * mode with echo off, so nothing would show.
 */
/* `expect` is what Mod-p must produce: PASTEME for an editor that saved
 * something new, AGAIN for one that saved the text it was given back
 * unchanged, NOTHING for one that declined. */
static void copymode_paste(const char *mode, const char *expect) {
    char witness[512], cwd[256], chord[2];
    int seen;

    if (!getcwd(cwd, sizeof cwd))
        die("getcwd: %s", strerror(errno));
    snprintf(
        witness, sizeof witness, "%s/tests/witness.%d", cwd, (int)getpid());
    unlink(witness);
    setenv("EDITOR_MODE", mode, 1);
    setenv("EDITOR_WITNESS", witness, 1);

    start("sh -c cat", NULL, NULL);
    settle(1200);

    /* Put a known line in the window, so that the two declining cases can be
     * told apart from a broken check: if dvtm-editor wrongly hands back the
     * buffer it was given, this line is what comes back, and it lands on the
     * screen a second time. */
    tty_write("COPYSRC\n", 8);
    if (!wait_screen("COPYSRC", 4000)) {
        fail("copy mode paste", "the window never echoed its input");
        reap();
        return;
    }
    settle(400);
    seen = screen_count("COPYSRC");

    chord[0] = MOD;
    chord[1] = 'e';
    tty_write(chord, 2);

    if (!wait_file(witness, 8000)) {
        char why[128];
        snprintf(why, sizeof why, "the %s editor never ran", mode);
        fail("copy mode paste", why);
        reap();
        unsetenv("EDITOR_MODE");
        unsetenv("EDITOR_WITNESS");
        return;
    }
    /* The first witness only says the editor started; the modes that write
     * leave a second one when the writing is over. */
    {
        char done[540];
        snprintf(done, sizeof done, "%s.done", witness);
        wait_file(done, 10000);
        unlink(done);
    }
    settle(2000); /* the editor exits; dvtm gives the window back */

    /* Wipe the window before pasting. The register holds the window's own
     * text, so without this there is no telling a paste from dvtm merely
     * repainting what was already there -- which is what made this check
     * disagree with itself run to run. */
    if (!strcmp(expect, "AGAIN")) {
        tty_write("\033[2J\033[H\n", 8);
        if (!wait_screen_gone("COPYSRC", 8000)) {
            fail("copy mode paste", "the window would not clear");
            reap();
            unlink(witness);
            unsetenv("EDITOR_MODE");
            unsetenv("EDITOR_WITNESS");
            return;
        }
    }

    chord[0] = MOD;
    chord[1] = 'p';
    tty_write(chord, 2);
    settle(1500);

    {
        char name[96], why[220];
        int now = screen_count("COPYSRC");

        if (!strcmp(expect, "PASTEME")) {
            check("an edited buffer is pasted back", screen_has("PASTEME"),
                "the editor saved PASTEME and Mod-p produced nothing");
        } else if (!strcmp(expect, "AGAIN")) {
            /* Asserted on what dvtm wrote after Mod-p, not on the screen: the
             * register is the whole window, blank rows and all, so putting it
             * back scrolls the old copy off by exactly as much as it adds and
             * the picture ends up unchanged. */
            check("saving without editing pastes the text back",
                wait_screen("COPYSRC", 8000),
                "the editor saved the text it was handed, so Mod-p must put "
                "it back, and none of it was written to the window");
        } else {
            snprintf(name, sizeof name, "nothing is pasted when the editor %s",
                !strcmp(mode, "keep") ? "changes nothing" : "fails");
            snprintf(why, sizeof why,
                "the %s editor must leave the register alone; the window got "
                "PASTEME=%d and COPYSRC %d times, having had it %d times",
                mode, screen_has("PASTEME"), now, seen);
            check(name, !screen_has("PASTEME") && now == seen, why);
        }
    }

    reap();
    unlink(witness);
    unsetenv("EDITOR_MODE");
    unsetenv("EDITOR_WITNESS");
}

/* The register outlives the window it came from: the README's claim is that
 * what an editor hands back goes into *any* window, and the three cases above
 * only ever paste back into the one they copied from.
 *
 * Which window received it is read the same way focus is, by killing the
 * focused one and seeing whether the pasted text goes with it. */
static void t_paste_other_window(void) {
    static const char *const name =
        "Mod-p pastes into the focused window, not the one copied from";
    char witness[512], cwd[256], why[220];

    if (!getcwd(cwd, sizeof cwd))
        die("getcwd: %s", strerror(errno));
    snprintf(
        witness, sizeof witness, "%s/tests/witness.x.%d", cwd, (int)getpid());
    unlink(witness);
    setenv("EDITOR_MODE", "edit", 1);
    setenv("EDITOR_WITNESS", witness, 1);

    start("sh -c cat", "sh -c cat", NULL);
    if (!wait_ids(2, 6000)) {
        fail(name, "the two windows never appeared");
        goto out;
    }
    settle(1200);

    tty_write("COPYSRC\n", 8);
    if (!wait_screen("COPYSRC", 4000)) {
        fail(name, "the focused window never echoed its input");
        goto out;
    }
    settle(400);

    send_chord("e");
    if (!wait_file(witness, 8000)) {
        fail(name, "the editor never ran");
        goto out;
    }
    settle(1500);

    send_chord("2");
    settle(400);
    send_chord("p");
    settle(1500);

    if (!screen_has("PASTEME")) {
        fail(name, "Mod-p produced nothing in either window");
        goto out;
    }

    send_chord("xx");
    settle(1500);
    snprintf(why, sizeof why,
        "killing the window Mod-2 selected should take PASTEME with it and "
        "leave COPYSRC behind; both going means the paste landed in the "
        "window it was copied from. PASTEME=%d COPYSRC=%d",
        screen_has("PASTEME"), screen_has("COPYSRC"));
    check(name, !screen_has("PASTEME") && screen_has("COPYSRC"), why);

out:
    reap();
    unlink(witness);
    unsetenv("EDITOR_MODE");
    unsetenv("EDITOR_WITNESS");
}

/* A paste larger than the pty will take in one go must not wedge dvtm.
 *
 * The child echoes what it is given, so its output fills the master while dvtm
 * is still writing. A write that insists on finishing blocks there, dvtm stops
 * reading, the child blocks on its own output and stops reading too, and the
 * two wait on each other for good. Measured outside dvtm with the same write
 * loop: it stalls after 1086 bytes on this machine whatever the total is, so
 * any real scrollback is past it.
 *
 * The assertion is that dvtm still answers afterwards, not that the text
 * arrived: a paste that is slow, or even one the child drops, is not this
 * bug. */
static void t_paste_large(void) {
    static const char *const name =
        "a paste larger than the pty does not wedge dvtm";
    char witness[512], done[540], cwd[256];

    if (!getcwd(cwd, sizeof cwd))
        die("getcwd: %s", strerror(errno));
    snprintf(
        witness, sizeof witness, "%s/tests/witness.b.%d", cwd, (int)getpid());
    snprintf(done, sizeof done, "%s.done", witness);
    unlink(witness);
    unlink(done);
    setenv("EDITOR_MODE", "big", 1);
    setenv("EDITOR_BYTES", "32768", 1);
    setenv("EDITOR_WITNESS", witness, 1);

    start("sh -c cat", NULL, NULL);
    settle(1200);

    send_chord("e");
    if (!wait_file(witness, 8000)) {
        fail(name, "the editor never ran");
        goto out;
    }
    settle(1500);

    send_chord("p");
    settle(3000);

    send_chord("c");
    check(name, wait_ids(2, 8000),
        "after the paste dvtm never opened a second window: it is blocked "
        "inside its own write to the pty and no longer reads anything");

out:
    reap();
    unlink(witness);
    unlink(done);
    unsetenv("EDITOR_MODE");
    unsetenv("EDITOR_BYTES");
    unsetenv("EDITOR_WITNESS");
}

/* A paste has to arrive announced, and only when the child asked for that.
 *
 * This is the long-paste report. Without the brackets a line editor cannot tell
 * pasted bytes from typing, so it reads a multi-line paste as line after line
 * of typing and runs each one: 64 lines of scrollback became 64 commands and
 * none of it appeared as text. A one-line paste looked fine, which is what made
 * it read as a size limit.
 *
 * The brackets are libvterm's to send, and it sends none to a child that never
 * enabled the mode -- an editor reading them literally would put them on screen
 * as text. Both halves are checked, because sending them unconditionally is the
 * obvious wrong fix.
 *
 * Asserted from inside the window: dvtm's screen cannot show what the child was
 * handed, only the probe can say.
 */
static void paste_announced(
    const char *name, const char *how, const char *saved, const char *want) {
    char witness[512], cwd[256], win[64], why[256];

    if (!getcwd(cwd, sizeof cwd))
        die("getcwd: %s", strerror(errno));
    snprintf(
        witness, sizeof witness, "%s/tests/witness.k.%d", cwd, (int)getpid());
    unlink(witness);
    snprintf(win, sizeof win, "tests/probe paste%s%s", *how ? " " : "", how);
    setenv("EDITOR_MODE", saved, 1);
    setenv("EDITOR_WITNESS", witness, 1);

    start(win, NULL, NULL);
    if (!wait_screen("PASTEREADY", 6000)) {
        fail(name, "the probe never came up");
        goto out;
    }
    settle(600);

    /* Copy mode is the only way to put anything in the register: what the
     * editor saves is what gets pasted back a moment later. */
    send_chord("e");
    if (!wait_file(witness, 8000)) {
        fail(name, "the editor never ran");
        goto out;
    }
    settle(1500);

    send_chord("p");
    snprintf(why, sizeof why,
        "the probe prints the bytes it was handed, and %s was expected; "
        "anything else means the paste was announced when it should not have "
        "been, or not announced when it should",
        want);
    check(name, wait_screen(want, 8000), why);

out:
    reap();
    unlink(witness);
    unsetenv("EDITOR_MODE");
    unsetenv("EDITOR_WITNESS");
}

static void t_paste_bracketed(void) {
    /* The whole rendering, closing bracket included: brackets that open and
     * never close leave a line editor waiting for the rest of a paste that has
     * already arrived. */
    paste_announced("a paste reaches a line editor announced as a paste",
        "bracket", "edit", "PASTE=ESC[200~PASTEME<0a>ESC[201~");
}

/* The report itself: several lines at once. One pair of brackets around the
 * lot and not a pair per line -- a single line is the one case that behaved
 * the same either way, which is what made this look like a limit on length. */
static void t_paste_bracketed_lines(void) {
    paste_announced("a multi-line paste arrives as one paste", "bracket",
        "multi", "PASTE=ESC[200~L1<0a>L2<0a>L3<0a>ESC[201~");
}

static void t_paste_unannounced(void) {
    paste_announced("a child that never asked is sent no brackets", "", "edit",
        "PASTE=PASTEME<0a>");
}

static void t_paste_unasked(void) {
    paste_announced("a child that took the request back is sent none either",
        "unask", "edit", "PASTE=PASTEME<0a>");
}

/* An editor may hand back more than a pipe will hold.
 *
 * dvtm used to read the editor's answer only once the editor had exited, and
 * the pipe between them holds a few dozen kilobytes. Anything longer filled
 * it, the editor blocked writing, so it never exited, so dvtm never read --
 * copy mode simply never finished and nothing was ever pasted. A scrollback
 * is easily that long. */
static void t_copymode_big_answer(void) {
    static const char *const name = "an answer larger than the pipe is read";
    char witness[512], done[540], cwd[256];

    if (!getcwd(cwd, sizeof cwd))
        die("getcwd: %s", strerror(errno));
    snprintf(
        witness, sizeof witness, "%s/tests/witness.p.%d", cwd, (int)getpid());
    /* Both names before the first `goto out`, which unlinks both. */
    snprintf(done, sizeof done, "%s.done", witness);
    unlink(witness);
    unlink(done);
    setenv("EDITOR_MODE", "big", 1);
    setenv("EDITOR_BYTES", "131072", 1);
    setenv("EDITOR_WITNESS", witness, 1);

    start("sh -c cat", NULL, NULL);
    settle(1200);

    send_chord("e");
    if (!wait_file(witness, 8000)) {
        fail(name, "the editor never ran");
        goto out;
    }
    /* The editor finishing its writing is the thing under test: with nobody
     * draining the pipe it stops part way and this file never appears. The
     * witness above only says it started. */
    if (!wait_file(done, 20000)) {
        fail(name, "the editor never finished writing: it is blocked part "
                   "way, with nobody draining the pipe it writes into");
        goto out;
    }
    settle(1500); /* the editor exits; dvtm returns to the window */

    send_chord("p");
    check(name, wait_screen("BIGEND", 20000),
        "the editor's last line never reached the window: copy mode never "
        "finished, because the editor is still blocked writing into a full "
        "pipe");

out:
    reap();
    unlink(witness);
    unlink(done);
    unsetenv("EDITOR_MODE");
    unsetenv("EDITOR_BYTES");
    unsetenv("EDITOR_WITNESS");
}

static void t_copymode_paste(void) {
    copymode_paste("edit", "PASTEME");
}

/* Copy mode with the scrollback switched off.
 *
 * The buffer dvtm reads the editor's answer into took its first size from -h,
 * which counts lines and not bytes. At -h 0 that was zero bytes, so every read
 * asked for nothing, the answer was dropped, and Mod-p produced silence -- with
 * no error anywhere. The live screen is still worth copying when no history is
 * kept, so this has to work.
 *
 * Its own case rather than a fourth mode in copymode_paste(), because the flag
 * has to be on dvtm's command line and that helper starts dvtm without one. */
static void t_copymode_no_history(void) {
    static const char *const name = "copy mode works with the history at zero";
    char witness[512], cwd[256];
    const char *args[4];

    if (!getcwd(cwd, sizeof cwd))
        die("getcwd: %s", strerror(errno));
    snprintf(
        witness, sizeof witness, "%s/tests/witness.h.%d", cwd, (int)getpid());
    unlink(witness);
    setenv("EDITOR_MODE", "edit", 1);
    setenv("EDITOR_WITNESS", witness, 1);

    args[0] = "-h";
    args[1] = "0";
    args[2] = "sh -c cat";
    args[3] = NULL;
    start_argv(args);
    settle(1200);

    send_chord("e");
    if (!wait_file(witness, 8000)) {
        fail(name, "the editor never ran");
        goto out;
    }
    settle(1500);

    send_chord("p");
    check(name, wait_screen("PASTEME", 8000),
        "the editor saved PASTEME and Mod-p produced nothing: the answer "
        "buffer was sized from -h, so with no history there was nowhere to "
        "put it");

out:
    reap();
    unlink(witness);
    unsetenv("EDITOR_MODE");
    unsetenv("EDITOR_WITNESS");
}
static void t_copymode_unchanged(void) {
    copymode_paste("keep", "NOTHING");
}

static void t_copymode_resaved(void) {
    copymode_paste("resave", "AGAIN");
}
static void t_copymode_editor_fails(void) {
    copymode_paste("fail", "NOTHING");
}

/* An editor that saved and is still on screen must leave the previous copy
 * alone.
 *
 * This is the report none of the checks above covered: Mod-e, `:w`, switch
 * window, Mod-p -- and nothing arrives. Saving is not what hands the text
 * over; leaving is. dvtm-editor is a filter, and its stdout only closes once
 * the editor is gone, so while the editor is still up there is nothing new to
 * paste and there cannot be. That part is the design. What was not the design
 * is that dvtm emptied the register the moment copy mode started, so Mod-p
 * answered with silence instead of the last thing that was copied.
 *
 * Every other copy mode check quits the editor, which is why they all pass and
 * this one did not exist.
 */
static void t_copymode_editor_still_open(void) {
    static const char *const name =
        "an editor still on screen leaves the previous copy alone";
    char witness[512], one[540], two[540], seq[540], cwd[256], why[220];

    if (!getcwd(cwd, sizeof cwd))
        die("getcwd: %s", strerror(errno));
    snprintf(
        witness, sizeof witness, "%s/tests/witness.s.%d", cwd, (int)getpid());
    snprintf(seq, sizeof seq, "%s.seq", witness);
    snprintf(one, sizeof one, "%s.1", witness);
    snprintf(two, sizeof two, "%s.2", witness);
    unlink(witness);
    unlink(seq);
    unlink(one);
    unlink(two);
    setenv("EDITOR_MODE", "stay", 1);
    setenv("EDITOR_WITNESS", witness, 1);
    setenv("EDITOR_SEQ", seq, 1);

    start("sh -c cat", "sh -c cat", NULL);
    if (!wait_ids(2, 6000)) {
        fail(name, "the two windows never appeared");
        goto out;
    }
    settle(1200);

    /* First run: saves PASTEME and leaves, which is the ordinary path. The
     * register holds PASTEME from here on, and nothing has been pasted yet, so
     * the word is not on screen. */
    send_chord("e");
    if (!wait_file(one, 8000)) {
        fail(name, "the editor never ran");
        goto out;
    }
    settle(1500);

    /* Second run: saves SECONDTEXT and stays. This is the editor the report
     * left sitting there. */
    send_chord("e");
    if (!wait_file(two, 8000)) {
        fail(name, "the second editor never ran");
        goto out;
    }
    settle(1000);

    send_chord("2");
    settle(400);
    send_chord("p");
    settle(1500);

    snprintf(why, sizeof why,
        "with an editor still up, Mod-p must put back what was copied before "
        "it started, and never the buffer that editor is holding: "
        "PASTEME=%d SECONDTEXT=%d",
        screen_has("PASTEME"), screen_has("SECONDTEXT"));
    check(name, screen_has("PASTEME") && !screen_has("SECONDTEXT"), why);

out:
    reap();
    unlink(witness);
    unlink(seq);
    unlink(one);
    unlink(two);
    unsetenv("EDITOR_MODE");
    unsetenv("EDITOR_WITNESS");
    unsetenv("EDITOR_SEQ");
}

/* Everything dvtm paints itself -- the tag numbers, the window borders, the
 * titles of unfocused windows -- is COLOR(DEFAULT) in config.h, which is -1/-1:
 * whatever this terminal calls its default foreground and background.
 *
 * That has to reach the terminal as a default and not as a colour. dvtm used to
 * resolve it here instead, reading pair 0 and using whatever number came back.
 * On a direct-colour terminal a colour number is an rgb value, so pair 0 reads
 * as 0, which is black -- and the unused tags and every border were painted
 * black on black. Nothing was missing; it was all there in the terminal's
 * darkest colour.
 *
 * The check is on the bytes rather than the screen model, because a screen
 * model has no opinion about whether two colours can be told apart. */
static void t_default_color_is_not_black(void) {
    start("tests/probe mark DEFCOLOR", NULL, NULL);
    if (!wait_screen("DEFCOLOR", 6000)) {
        fail("dvtm's own colours stay the terminal's default",
            "the window never appeared");
        reap();
        return;
    }
    settle(400);

    check("unused tags are painted in the default colour, not in black",
        findmem(obuf, olen, "\033[39m", 5) != NULL &&
            findmem(obuf, olen, "\033[30m[2]", 8) == NULL,
        "the tags after the first are drawn with an explicit black "
        "foreground, which on a dark terminal is nothing at all");

    check("dvtm does not force a background on its own furniture",
        findmem(obuf, olen, "\033[40m[2]", 8) == NULL,
        "the bar is painted on a hard black background instead of the "
        "terminal's own");
    reap();
}

/* The main loop registers four kinds of descriptor with select(): the keyboard,
 * the pipe its own signal handlers poke, the command fifo and the status fifo.
 * Nothing here covered the two fifos, which is most of that bookkeeping.
 *
 * Both are checked from the outside, the way a user meets them: a line written
 * to the -s fifo has to reach the bar, and a line written to the -c fifo has to
 * open a window. Breaking either registration fails the matching check --
 * measured, not assumed.
 *
 * The resize at the end is weaker on purpose, and it is worth saying why. The
 * self-pipe exists for a signal that arrives while signals are blocked, which
 * is a race too narrow to provoke from here; a resize arriving at any other
 * moment interrupts select() by itself, so dvtm repaints even with the pipe
 * unwatched. So this checks that a resize is acted on at all, and does not
 * pretend to cover the pipe. */
static void t_fifos(void) {
    char cmdfifo[512], statusfifo[512], cwd[256];
    const char *args[6];
    size_t before;
    int fd;

    if (!getcwd(cwd, sizeof cwd))
        die("getcwd: %s", strerror(errno));
    snprintf(cmdfifo, sizeof cmdfifo, "%s/tests/cmd.%d", cwd, (int)getpid());
    snprintf(
        statusfifo, sizeof statusfifo, "%s/tests/bar.%d", cwd, (int)getpid());
    unlink(cmdfifo);
    unlink(statusfifo);

    args[0] = "-c";
    args[1] = cmdfifo;
    args[2] = "-s";
    args[3] = statusfifo;
    args[4] = NULL;
    start_argv(args);
    settle(800);

    if ((fd = open(statusfifo, O_WRONLY)) < 0) {
        fail("status fifo is read", strerror(errno));
    } else {
        if (write(fd, "BARTEXT\n", 8) < 0)
            fail("status fifo is read", strerror(errno));
        close(fd);
        check("status fifo reaches the bar", wait_screen("BARTEXT", 4000),
            "wrote to the -s fifo and the bar never showed it");
    }

    if ((fd = open(cmdfifo, O_WRONLY)) < 0) {
        fail("command fifo is read", strerror(errno));
    } else {
        const char *line = "create tests/probe mark FIFOWIN\n";
        if (write(fd, line, strlen(line)) < 0)
            fail("command fifo is read", strerror(errno));
        close(fd);
        check("command fifo opens a window", wait_screen("FIFOWIN", 6000),
            "wrote `create` to the -c fifo and no window appeared");
    }

    /* Last, because it leaves the screen model at the old size. */
    settle(300);
    before = olen;
    resize_tty(ROWS, COLS - 10);
    settle(1500);
    check("a resize is acted on", olen > before,
        "SIGWINCH arrived and dvtm wrote nothing back");

    reap();
    unlink(cmdfifo);
    unlink(statusfifo);
}

/* Being told to go away must not leave dvtm's fifos behind.
 *
 * The `-c` and `-s` pipes are dvtm's to remove, and cleanup() does remove them
 * — but only on the paths that reach it. A terminal going away sends SIGHUP,
 * whose default action is to terminate, so the process died before cleanup ran
 * and every session ended by closing its window left a pipe behind. Found by
 * counting nineteen of them in one temporary directory.
 *
 * SIGTERM is checked beside it. That path already worked; the pair is here so
 * that neither can regress without the other noticing. */
static void t_hangup(void) {
    static const int sigs[] = { SIGHUP, SIGTERM };
    static const char *const names[] = { "SIGHUP", "SIGTERM" };
    char cwd[256];

    if (!getcwd(cwd, sizeof cwd))
        die("getcwd: %s", strerror(errno));

    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) {
        char fifo[512], name[96], why[300];
        const char *args[3];

        snprintf(
            fifo, sizeof fifo, "%s/tests/hup.%d.%u", cwd, (int)getpid(), i);
        unlink(fifo);

        args[0] = "-c";
        args[1] = fifo;
        args[2] = NULL;
        start_argv(args);
        settle(800);

        snprintf(name, sizeof name, "%s removes the command fifo", names[i]);
        if (!wait_file(fifo, 3000)) {
            fail(name, "dvtm never created the fifo named with -c");
            reap();
            unlink(fifo);
            continue;
        }

        kill(kid, sigs[i]);
        snprintf(why, sizeof why,
            "dvtm was sent %s and left the fifo behind. cleanup() unlinks it, "
            "so the signal terminated the process before cleanup ran — the "
            "handler is registered for one signal and not the other.",
            names[i]);
        check(name, wait_gone(fifo, 5000), why);

        reap();
        unlink(fifo);
    }
}

/* The regression this exists for: keystrokes silently lost while signals were
 * blocked. Send the chords back to back in a single write, with no settling —
 * spacing them out is exactly what hides the bug.
 *
 * Counted with `create`, not `killclient`. killclient acts on the focused
 * client, and focus only advances once dvtm notices the dead child's pty hang
 * up; three kill chords delivered faster than that all target the same client,
 * so three firings produce one dead window. That measures dvtm's reaping, not
 * its key handling. `create` has no such lag: every chord that fires adds a
 * window with the next id, so the ids on screen count the chords exactly. */
static void t_no_dropped_keys(void) {
    const int N = 4;
    char burst[2 * 8];
    int i;
    char why[160];

    start("tests/probe mark FIRST", NULL, NULL);
    if (!wait_screen("FIRST", 5000)) {
        fail("keystrokes are never dropped", "the first window never appeared");
        reap();
        return;
    }

    for (i = 0; i < N; i++) {
        burst[i * 2 + 0] = MOD;
        burst[i * 2 + 1] = 'c';
    }
    tty_write(burst, (size_t)(N * 2));

    /* window #1 already existed, so the burst must produce #2 .. #(N+1) */
    settle(2500);
    {
        bool all = true;
        char id[8];
        for (i = 2; i <= N + 1; i++) {
            snprintf(id, sizeof id, "#%d", i);
            if (!wait_screen(id, 3000))
                all = false;
        }
        snprintf(why, sizeof why,
            "not every chord fired: expected windows #2..#%d from "
            "%d chords sent in one write",
            N + 1, N);
        check("keystrokes are never dropped", all, why);
    }
    reap();
}

/* ── main ─────────────────────────────────────────────────────────────────── */

static void build_terminfo(void) {
    char cmd[2048], cwd[1024];

    if (!getcwd(cwd, sizeof cwd))
        die("getcwd: %s", strerror(errno));

    /* Compile dvtm's own terminfo into the tree, so `make test` never depends
     * on `make install` having run, while still exercising the real entry.
     * -x matters: without it Tc/setrgbf/setrgbb are silently dropped and the
     * children see a plain 256-colour terminal. */
    snprintf(cmd, sizeof cmd,
        "tic -x -o '%s/tests/terminfo' dvtm.info 2>/dev/null", cwd);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: tic failed; children may lack the "
                        "dvtm terminfo\n");

    /* Trailing colon keeps ncurses' compiled-in default path in the search,
     * which is where xterm-direct lives. */
    snprintf(tinfo, sizeof tinfo, "%s/tests/terminfo:", cwd);
}

int main(int argc, char *argv[]) {
    const char *bin = argc > 1 ? argv[1] : "./dvtm";
    struct stat st;

    if (stat(bin, &st) != 0)
        die("%s: %s (build it first)", bin, strerror(errno));
    snprintf(dvtm_path, sizeof dvtm_path, "%s", bin);

    /* Deterministic child shell, whatever the developer's login shell is. */
    setenv("SHELL", "/bin/sh", 1);

    /* A stand-in editor, so the copy mode checks measure dvtm rather than
     * whichever editor this machine happens to have. See tests/editor. */
    {
        char ed[1024], cwd[1024];
        if (!getcwd(cwd, sizeof cwd))
            die("getcwd: %s", strerror(errno));
        snprintf(ed, sizeof ed, "%s/tests/editor", cwd);
        setenv("DVTM_EDITOR", ed, 1);
    }

    /* Put this tree first on PATH, so copy mode runs the dvtm-editor in it.
     * Without this the suite finds whichever one is installed -- testing the
     * last release instead of this tree -- and on a machine with none
     * installed the copy mode check fails with `execv() failed`, which reads
     * like a dvtm bug and is not one.
     *
     * scripts/ as well as the root: the scripts moved there and the root alone
     * silently went back to testing the installed copy. Verified by breaking
     * scripts/dvtm-editor and watching the suite stay green. */
    {
        char path[4096], cwd[1024];
        const char *old = getenv("PATH");
        if (!getcwd(cwd, sizeof cwd))
            die("getcwd: %s", strerror(errno));
        snprintf(path, sizeof path, "%s/scripts:%s%s%s", cwd, cwd,
            old ? ":" : "", old ? old : "");
        setenv("PATH", path, 1);
    }
    signal(SIGPIPE, SIG_IGN);

    build_terminfo();

    t_startup();
    t_dsr();
    t_truecolor();
    t_faint();
    t_manycolors();
    t_palette_terminal();
    t_backspace();
    t_utf8_input();
    t_child_env();
    t_plain_text_default_color();
    t_cursor_follows_child();
    t_copymode();
    t_copymode_paste();
    t_copymode_no_history();
    t_copymode_big_answer();
    t_copymode_unchanged();
    t_copymode_resaved();
    t_copymode_editor_fails();
    t_copymode_editor_still_open();
    t_paste_other_window();
    t_paste_large();
    t_paste_bracketed();
    t_paste_bracketed_lines();
    t_paste_unannounced();
    t_paste_unasked();
    t_kill_removes_window();
    t_exit_removes_window();
    t_window_ids();
    t_focus();
    t_focus_moves();
    t_layouts();
    t_tags();
    t_runinall();
    t_no_dropped_keys();
    t_default_color_is_not_black();
    t_fifos();
    t_hangup();

    printf("\n%d checks, %d failed, %d skipped\n", checks, failures, skipped);
    if (skipped)
        printf("(skipped checks are known limitations, listed "
               "above)\n");
    return failures ? 1 : 0;
}

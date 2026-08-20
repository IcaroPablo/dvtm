/*
 * The initial "port" of dwm to curses was done by
 *
 * © 2007-2016 Marc André Tanner <mat at brain-dump dot org>
 *
 * It is highly inspired by the original X11 dwm and
 * reuses some code of it which is mostly
 *
 * © 2006-2007 Anselm R. Garbe <garbeam at gmail dot com>
 *
 * See LICENSE for details.
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <wchar.h>
#include <limits.h>
#include <libgen.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <curses.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <pwd.h>
#include <termios.h>
#include "dvtm.h"
#include "config.h"

/* global variables */
static const char *dvtm_name = "dvtm";
Screen screen = {
    .mfact = MFACT, .nmaster = NMASTER, .history = SCROLL_HISTORY
};
static Client *stack = NULL;
static Client *sel = NULL;
static Client *lastsel = NULL;
static Client *msel = NULL;
static unsigned int seltags;
static unsigned int tagset[2] = { 1, 1 };
static bool mouse_events_enabled = ENABLE_MOUSE;
static Layout *layout = layouts;
static StatusBar bar = { .fd = -1,
    .lastpos = BAR_POS,
    .pos = BAR_POS,
    .autohide = BAR_AUTOHIDE,
    .h = 1 };
static CmdFifo cmdfifo = { .fd = -1 };
/* Self-pipe, so a handler can wake the main loop. pselect(2) on macOS ignores
 * its sigmask argument, so the "block the signal, unblock it only while
 * waiting" pattern never delivers SIGCHLD there and dead clients stay on
 * screen forever. A byte in the pipe wakes select() on any platform, with no
 * race: a signal arriving before the wait leaves the byte already queued. */
static int sigpipe[2] = { -1, -1 };
static const char *shell;
static Register copyreg;
static volatile sig_atomic_t running = true;
static bool runinall = false;

static void eprint(const char *errstr, ...) {
    va_list ap;
    va_start(ap, errstr);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
}

static void error(const char *errstr, ...) {
    va_list ap;
    va_start(ap, errstr);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static bool isarrange(void (*func)(void)) {
    return func == layout->arrange;
}

static bool isvisible(Client *c) {
    return c->tags & tagset[seltags];
}

static bool is_content_visible(Client *c) {
    if (!c)
        return false;
    if (isarrange(fullscreen))
        return sel == c;
    return isvisible(c) && !c->minimized;
}

static Client *nextvisible(Client *c) {
    for (; c && !isvisible(c); c = c->next)
        ;
    return c;
}

static void updatebarpos(void) {
    bar.y = 0;
    wax = 0;
    way = 0;
    wah = screen.h;
    waw = screen.w;
    if (bar.pos == BAR_TOP) {
        wah -= bar.h;
        way += bar.h;
    } else if (bar.pos == BAR_BOTTOM) {
        wah -= bar.h;
        bar.y = wah;
    }
}

static void hidebar(void) {
    if (bar.pos != BAR_OFF) {
        bar.lastpos = bar.pos;
        bar.pos = BAR_OFF;
    }
}

static void showbar(void) {
    if (bar.pos == BAR_OFF)
        bar.pos = bar.lastpos;
}

static void drawbar(void) {
    int sx, sy, x, y, width;
    unsigned int occupied = 0, urgent = 0;
    if (bar.pos == BAR_OFF)
        return;

    for (Client *c = clients; c; c = c->next) {
        occupied |= c->tags;
        if (c->urgent)
            urgent |= c->tags;
    }

    getyx(stdscr, sy, sx);
    attrset(BAR_ATTR);
    move(bar.y, 0);

    for (unsigned int i = 0; i < LENGTH(tags); i++) {
        if (tagset[seltags] & (1 << i))
            attrset(TAG_SEL);
        else if (urgent & (1 << i))
            attrset(TAG_URGENT);
        else if (occupied & (1 << i))
            attrset(TAG_OCCUPIED);
        else
            attrset(TAG_NORMAL);
        printw(TAG_SYMBOL, tags[i]);
    }

    attrset(runinall ? TAG_SEL : TAG_NORMAL);
    addstr(layout->symbol);
    attrset(TAG_NORMAL);

    for (unsigned int i = 0; i < MAX_KEYS && keys[i]; i++) {
        if (keys[i] < ' ')
            printw("^%c", 'A' - 1 + keys[i]);
        else
            printw("%c", keys[i]);
    }

    getyx(stdscr, y, x);
    (void)y;
    int maxwidth = screen.w - x - 2;

    addch(BAR_BEGIN);
    attrset(BAR_ATTR);

    wchar_t wbuf[sizeof bar.text];
    size_t numchars = mbstowcs(wbuf, bar.text, sizeof bar.text);

    if (numchars != (size_t)-1 && (width = wcswidth(wbuf, maxwidth)) != -1) {
        int pos;
        for (pos = 0; pos + width < maxwidth; pos++)
            addch(' ');

        for (size_t i = 0; i < numchars; i++) {
            pos += wcwidth(wbuf[i]);
            if (pos > maxwidth)
                break;
            addnwstr(wbuf + i, 1);
        }

        clrtoeol();
    }

    attrset(TAG_NORMAL);
    mvaddch(bar.y, screen.w - 1, BAR_END);
    attrset(NORMAL_ATTR);
    move(sy, sx);
    wnoutrefresh(stdscr);
}

static int show_border(void) {
    return (bar.pos != BAR_OFF) || (clients && clients->next);
}

static void draw_border(Client *c) {
    char t = '\0';
    int x, y, maxlen, attrs = NORMAL_ATTR;

    if (!show_border())
        return;
    if (sel != c && c->urgent)
        attrs = URGENT_ATTR;
    if (sel == c || (runinall && !c->minimized))
        attrs = SELECTED_ATTR;

    wattrset(c->window, attrs);
    getyx(c->window, y, x);
    mvwhline(c->window, 0, 0, ACS_HLINE, c->w);
    maxlen = c->w - 10;
    if (maxlen < 0)
        maxlen = 0;
    if ((size_t)maxlen < sizeof(c->title)) {
        t = c->title[maxlen];
        c->title[maxlen] = '\0';
    }

    mvwprintw(c->window, 0, 2, "[%s%s#%d]", *c->title ? c->title : "",
        *c->title ? " | " : "", c->order);
    if (t)
        c->title[maxlen] = t;
    wmove(c->window, y, x);
}

static void draw_content(Client *c) {
    term_draw(c->term, c->window, c->has_title_line, 0);
}

static void draw(Client *c) {
    if (is_content_visible(c)) {
        redrawwin(c->window);
        draw_content(c);
    }
    if (!isarrange(fullscreen) || sel == c)
        draw_border(c);
    wnoutrefresh(c->window);
}

static void draw_all(void) {
    if (!nextvisible(clients)) {
        sel = NULL;
        curs_set(0);
        erase();
        drawbar();
        doupdate();
        return;
    }

    if (!isarrange(fullscreen)) {
        for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
            if (c != sel)
                draw(c);
        }
    }
    /* as a last step the selected window is redrawn,
     * this has the effect that the cursor position is
     * accurate
     */
    if (sel)
        draw(sel);
}

static void arrange(void) {
    unsigned int m = 0, n = 0;
    for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
        c->order = ++n;
        if (c->minimized)
            m++;
    }
    erase();
    attrset(NORMAL_ATTR);
    if (bar.fd == -1 && bar.autohide) {
        if ((!clients || !clients->next) && n == 1)
            hidebar();
        else
            showbar();
        updatebarpos();
    }
    if (m && !isarrange(fullscreen))
        wah--;
    layout->arrange();
    if (m && !isarrange(fullscreen)) {
        unsigned int i = 0, nw = waw / m, nx = wax;
        for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
            if (c->minimized) {
                resize(c, nx, way + wah, ++i == m ? waw - nx : nw, 1);
                nx += nw;
            }
        }
        wah++;
    }
    focus(NULL);
    wnoutrefresh(stdscr);
    drawbar();
    draw_all();
}

static void attach(Client *c) {
    if (clients)
        clients->prev = c;
    c->next = clients;
    c->prev = NULL;
    clients = c;
    for (int o = 1; c; c = nextvisible(c->next), o++)
        c->order = o;
}

static void attachafter(Client *c, Client *a) { /* attach c after a */
    if (c == a)
        return;
    if (!a)
        for (a = clients; a && a->next; a = a->next)
            ;

    if (a) {
        if (a->next)
            a->next->prev = c;
        c->next = a->next;
        c->prev = a;
        a->next = c;
        for (int o = a->order; c; c = nextvisible(c->next))
            c->order = ++o;
    }
}

static void attachstack(Client *c) {
    c->snext = stack;
    stack = c;
}

static void detach(Client *c) {
    Client *d;
    if (c->prev)
        c->prev->next = c->next;
    if (c->next) {
        c->next->prev = c->prev;
        for (d = nextvisible(c->next); d; d = nextvisible(d->next))
            --d->order;
    }
    if (c == clients)
        clients = c->next;
    c->next = c->prev = NULL;
}

static void settitle(Client *c) {
    char *term, *t = title;
    if (!t && sel == c && *c->title)
        t = c->title;
    if (t && (term = getenv("TERM")) && !strstr(term, "linux")) {
        printf("\033]0;%s\007", t);
        fflush(stdout);
        wnoutrefresh(c->window);
    }
}

static void detachstack(Client *c) {
    Client **tc;
    for (tc = &stack; *tc && *tc != c; tc = &(*tc)->snext)
        ;
    *tc = c->snext;
}

static void focus(Client *c) {
    if (!c)
        for (c = stack; c && !isvisible(c); c = c->snext)
            ;
    if (sel == c)
        return;
    lastsel = sel;
    sel = c;
    if (lastsel) {
        lastsel->urgent = false;
        if (!isarrange(fullscreen)) {
            draw_border(lastsel);
            wnoutrefresh(lastsel->window);
        }
    }

    if (c) {
        detachstack(c);
        attachstack(c);
        settitle(c);
        c->urgent = false;
        if (isarrange(fullscreen)) {
            draw(c);
        } else {
            draw_border(c);
            wnoutrefresh(c->window);
        }
    }
    curs_set(c && !c->minimized && term_cursor_visible(c->term));
}

static void applycolorrules(Client *c) {
    const ColorRule *r = colorrules;
    short fg = r->color->fg, bg = r->color->bg;
    attr_t attrs = r->attrs;

    for (unsigned int i = 1; i < LENGTH(colorrules); i++) {
        r = &colorrules[i];
        if (strstr(c->title, r->title)) {
            attrs = r->attrs;
            fg = r->color->fg;
            bg = r->color->bg;
            break;
        }
    }

    c->term->defattrs = attrs;
    c->term->deffg = fg;
    c->term->defbg = bg;
}

static void term_title_handler(Term *term, const char *title) {
    Client *c = term->data;
    if (title)
        strncpy(c->title, title, sizeof(c->title) - 1);
    c->title[title ? sizeof(c->title) - 1 : 0] = '\0';
    settitle(c);
    if (!isarrange(fullscreen) || sel == c)
        draw_border(c);
    applycolorrules(c);
}

static void term_urgent_handler(Term *term) {
    Client *c = term->data;
    c->urgent = true;
    printf("\a");
    fflush(stdout);
    drawbar();
    if (!isarrange(fullscreen) && sel != c && isvisible(c))
        draw_border(c);
}

static void move_client(Client *c, int x, int y) {
    if (c->x == x && c->y == y)
        return;
    debug("moving, x: %d y: %d\n", x, y);
    if (mvwin(c->window, y, x) == ERR) {
        eprint("error moving, x: %d y: %d\n", x, y);
    } else {
        c->x = x;
        c->y = y;
    }
}

static void resize_client(Client *c, int w, int h) {
    bool has_title_line = show_border();
    bool resize_window = c->w != w || c->h != h;
    if (resize_window) {
        debug("resizing, w: %d h: %d\n", w, h);
        if (wresize(c->window, h, w) == ERR) {
            eprint("error resizing, w: %d h: %d\n", w, h);
        } else {
            c->w = w;
            c->h = h;
        }
    }
    if (resize_window || c->has_title_line != has_title_line) {
        c->has_title_line = has_title_line;
        term_resize(c->app, h - has_title_line, w);
        if (c->editor)
            term_resize(c->editor, h - has_title_line, w);
    }
}

static void resize(Client *c, int x, int y, int w, int h) {
    resize_client(c, w, h);
    move_client(c, x, y);
}

static Client *get_client_by_coord(unsigned int x, unsigned int y) {
    if (y < way || y >= way + wah)
        return NULL;
    if (isarrange(fullscreen))
        return sel;
    for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) {
            debug("mouse event, x: %d y: %d client: %d\n", x, y, c->order);
            return c;
        }
    }
    return NULL;
}

/* async-signal-safe: only write(2) */
static void wake_main_loop(void) {
    if (sigpipe[1] == -1)
        return;
    char b = 0;
    ssize_t unused = write(sigpipe[1], &b, 1);
    (void)unused;
}

/* Reap exited children and mark their clients dead.
 *
 * Called from the main loop rather than left to the signal handler alone.
 * SIGCHLD is blocked outside select() so that getch() is never interrupted,
 * and on macOS the signal was observed staying pending and blocked
 * indefinitely: the handler never ran, the client was never marked dead, and
 * its window stayed on screen for as long as you cared to wait. Polling costs
 * one waitpid per wakeup and cannot be lost. */
static void reap_children(void) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (Client *c = clients; c; c = c->next) {
            if (c->pid == pid) {
                c->died = true;
                break;
            }
            if (c->editor && c->editor->pid == pid) {
                c->editor_died = true;
                break;
            }
        }
    }
}

static void sigchld_handler(int sig) {
    int errsv = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) != 0) {
        if (pid == -1) {
            if (errno == ECHILD) {
                /* no more child processes */
                break;
            }
            eprint("waitpid: %s\n", strerror(errno));
            break;
        }

        debug("child with pid %d died\n", pid);

        wake_main_loop();

        for (Client *c = clients; c; c = c->next) {
            if (c->pid == pid) {
                c->died = true;
                break;
            }
            if (c->editor && c->editor->pid == pid) {
                c->editor_died = true;
                break;
            }
        }
    }

    errno = errsv;
}

static void sigwinch_handler(int sig) {
    screen.need_resize = true;
    wake_main_loop();
}

/* Both signals that mean "stop": leave the loop, so main() reaches cleanup()
 * and the fifos dvtm created are unlinked. */
static void sigexit_handler(int sig) {
    running = false;
}

static void resize_screen(void) {
    struct winsize ws;

    if (ioctl(0, TIOCGWINSZ, &ws) == -1) {
        getmaxyx(stdscr, screen.h, screen.w);
    } else {
        screen.w = ws.ws_col;
        screen.h = ws.ws_row;
    }

    debug("resize_screen(), w: %d h: %d\n", screen.w, screen.h);

    resizeterm(screen.h, screen.w);
    wresize(stdscr, screen.h, screen.w);
    updatebarpos();
    clear();
    arrange();
}

static KeyBinding *keybinding(KeyCombo keys, unsigned int keycount) {
    for (unsigned int b = 0; b < LENGTH(bindings); b++) {
        for (unsigned int k = 0; k < keycount; k++) {
            if (keys[k] != bindings[b].keys[k])
                break;
            if (k == keycount - 1)
                return &bindings[b];
        }
    }
    return NULL;
}

static unsigned int bitoftag(const char *tag) {
    unsigned int i;
    if (!tag)
        return ~0;
    for (i = 0; (i < LENGTH(tags)) && strcmp(tags[i], tag); i++)
        ;
    return (i < LENGTH(tags)) ? (1 << i) : 0;
}

static void tagschanged(void) {
    bool allminimized = true;
    for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
        if (!c->minimized) {
            allminimized = false;
            break;
        }
    }
    if (allminimized && nextvisible(clients)) {
        focus(NULL);
        toggleminimize(NULL);
    }
    arrange();
}

static void tag(const char *args[]) {
    if (!sel)
        return;
    sel->tags = bitoftag(args[0]) & TAGMASK;
    tagschanged();
}

static void tagid(const char *args[]) {
    if (!args[0] || !args[1])
        return;

    const int win_id = atoi(args[0]);
    for (Client *c = clients; c; c = c->next) {
        if (c->id == win_id) {
            unsigned int ntags = c->tags;
            for (unsigned int i = 1; i < MAX_ARGS && args[i]; i++) {
                if (args[i][0] == '+')
                    ntags |= bitoftag(args[i] + 1);
                else if (args[i][0] == '-')
                    ntags &= ~bitoftag(args[i] + 1);
                else
                    ntags = bitoftag(args[i]);
            }
            ntags &= TAGMASK;
            if (ntags) {
                c->tags = ntags;
                tagschanged();
            }
            return;
        }
    }
}

static void toggletag(const char *args[]) {
    if (!sel)
        return;
    unsigned int newtags = sel->tags ^ (bitoftag(args[0]) & TAGMASK);
    if (newtags) {
        sel->tags = newtags;
        tagschanged();
    }
}

static void toggleview(const char *args[]) {
    unsigned int newtagset = tagset[seltags] ^ (bitoftag(args[0]) & TAGMASK);
    if (newtagset) {
        tagset[seltags] = newtagset;
        tagschanged();
    }
}

static void view(const char *args[]) {
    unsigned int newtagset = bitoftag(args[0]) & TAGMASK;
    if (tagset[seltags] != newtagset && newtagset) {
        seltags ^= 1; /* toggle sel tagset */
        tagset[seltags] = newtagset;
        tagschanged();
    }
}

static void viewprevtag(const char *args[]) {
    seltags ^= 1;
    tagschanged();
}

/* One key on its way to the children. `is_key` says which of the two things
 * curses handed us: a key code such as KEY_UP, or a character. Nothing in the
 * value itself distinguishes them -- see the note above term_keychar(). */
static void keypress(int code, bool is_key) {
    bool escape = !is_key && code == '\033';
    int key = -1;
    uint32_t chr = 0;
    unsigned int len = 1;
    char buf[8] = { '\033' };

    if (escape) {
        /* pass characters following escape to the underlying app */
        nodelay(stdscr, TRUE);
        while (len < sizeof(buf)) {
            wint_t wc;
            int rc = get_wch(&wc);
            if (rc == ERR)
                break;
            if (rc == KEY_CODE_YES) {
                key = (int)wc;
                break;
            }
            /* The body of an escape sequence is ASCII. Anything else is the
             * next thing the user typed, and it has to be forwarded whole:
             * truncating it into buf is how a character gets cut in half. */
            if (wc >= 0x80) {
                chr = (uint32_t)wc;
                break;
            }
            buf[len++] = (char)wc;
        }
        nodelay(stdscr, FALSE);
    }

    for (Client *c = runinall ? nextvisible(clients) : sel; c;
        c = nextvisible(c->next)) {
        if (is_content_visible(c)) {
            c->urgent = false;
            if (escape)
                term_write(c->term, buf, len);
            else if (is_key)
                term_keypress(c->term, code);
            else
                term_keychar(c->term, (uint32_t)code);
            if (key != -1)
                term_keypress(c->term, key);
            if (chr)
                term_keychar(c->term, chr);
        }
        if (!runinall)
            break;
    }
}

static void mouse_setup(void) {
    mmask_t mask = 0;

    if (mouse_events_enabled) {
        mask = BUTTON1_CLICKED | BUTTON2_CLICKED;
        for (unsigned int i = 0; i < LENGTH(buttons); i++)
            mask |= buttons[i].mask;
    }
    mousemask(mask, NULL);
}

static bool checkshell(const char *shell) {
    if (shell == NULL || *shell == '\0' || *shell != '/')
        return false;
    if (!strcmp(strrchr(shell, '/') + 1, dvtm_name))
        return false;
    if (access(shell, X_OK))
        return false;
    return true;
}

static const char *getshell(void) {
    const char *shell = getenv("SHELL");
    struct passwd *pw;

    if (checkshell(shell))
        return shell;
    if ((pw = getpwuid(getuid())) && checkshell(pw->pw_shell))
        return pw->pw_shell;
    return "/bin/sh";
}

static void setup(void) {
    shell = getshell();
    setlocale(LC_CTYPE, "");
    initscr();
    start_color();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    mouse_setup();
    raw();
    term_init(keytable, LENGTH(keytable));
    for (unsigned int i = 0; i < LENGTH(colors); i++) {
        if (COLORS == 256) {
            if (colors[i].fg256)
                colors[i].fg = colors[i].fg256;
            if (colors[i].bg256)
                colors[i].bg = colors[i].bg256;
        }
        colors[i].pair = term_color_get(NULL, colors[i].fg, colors[i].bg);
    }
    resize_screen();
    if (pipe(sigpipe) == 0) {
        for (int i = 0; i < 2; i++) {
            fcntl(sigpipe[i], F_SETFD, FD_CLOEXEC);
            /* Both ends, not just the writer. The main loop drains this pipe
             * with `while (read(...) > 0)`, which on a blocking descriptor
             * does not stop when the pipe runs dry -- it waits there forever,
             * and dvtm stops responding to the keyboard and to every window
             * the moment any signal wakes it. */
            fcntl(sigpipe[i], F_SETFL, O_NONBLOCK);
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = sigexit_handler;
    sigaction(SIGTERM, &sa, NULL);
    /* SIGHUP as well, and not by default: its default action is to terminate,
     * so a terminal window closing killed dvtm before cleanup() ran and left
     * the fifos named with -c and -s behind, one per session. */
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

static void destroy(Client *c) {
    if (sel == c)
        focusnextnm(NULL);
    detach(c);
    detachstack(c);
    if (sel == c) {
        Client *next = nextvisible(clients);
        if (next) {
            focus(next);
            toggleminimize(NULL);
        } else {
            sel = NULL;
        }
    }
    if (lastsel == c)
        lastsel = NULL;
    werase(c->window);
    wnoutrefresh(c->window);
    term_destroy(c->term);
    delwin(c->window);
    free(c->editreg.data);
    if (!clients && LENGTH(actions)) {
        if (!strcmp(c->cmd, shell))
            quit(NULL);
        else
            create(NULL);
    }
    free(c);
    arrange();
}

static void cleanup(void) {
    while (clients)
        destroy(clients);
    endwin();
    free(copyreg.data);
    if (bar.fd > 0)
        close(bar.fd);
    if (bar.file)
        unlink(bar.file);
    if (cmdfifo.fd > 0)
        close(cmdfifo.fd);
    if (cmdfifo.file)
        unlink(cmdfifo.file);
}

static char *getcwd_by_pid(Client *c) {
    if (!c)
        return NULL;
    char buf[32];
    snprintf(buf, sizeof buf, "/proc/%d/cwd", c->pid);
    return realpath(buf, NULL);
}

static void create(const char *args[]) {
    const char *pargs[4] = { shell, NULL };
    char buf[8], *cwd = NULL;
    const char *env[] = { "DVTM_WINDOW_ID", buf, NULL };

    if (args && args[0]) {
        pargs[1] = "-c";
        pargs[2] = args[0];
        pargs[3] = NULL;
    }
    Client *c = calloc(1, sizeof(Client));
    if (!c)
        return;
    c->tags = tagset[seltags];
    c->id = ++cmdfifo.id;
    snprintf(buf, sizeof buf, "%d", c->id);

    if (!(c->window = newwin(wah, waw, way, wax))) {
        free(c);
        return;
    }

    c->term = c->app = term_create(screen.h, screen.w, screen.history);
    if (!c->term) {
        delwin(c->window);
        free(c);
        return;
    }

    if (args && args[0]) {
        c->cmd = args[0];
        char name[PATH_MAX];
        strncpy(name, args[0], sizeof(name));
        name[sizeof(name) - 1] = '\0';
        strncpy(c->title, basename(name), sizeof(c->title));
    } else {
        c->cmd = shell;
    }

    if (args && args[1])
        strncpy(c->title, args[1], sizeof(c->title));
    c->title[sizeof(c->title) - 1] = '\0';

    if (args && args[2])
        cwd = !strcmp(args[2], "$CWD") ? getcwd_by_pid(sel) : (char *)args[2];
    c->pid = term_forkpty(c->term, shell, pargs, cwd, env, NULL, NULL);
    if (args && args[2] && !strcmp(args[2], "$CWD"))
        free(cwd);
    c->term->data = c;
    c->term->title_handler = term_title_handler;
    c->term->urgent_handler = term_urgent_handler;
    applycolorrules(c);
    c->x = wax;
    c->y = way;
    debug("client with pid %d forked\n", c->pid);
    attach(c);
    focus(c);
    arrange();
}

static void copymode(const char *args[]) {
    if (!args || !args[0] || !sel || sel->editor)
        return;

    bool colored = strstr(args[0], "pager") != NULL;

    if (!(sel->editor = term_create(sel->h - sel->has_title_line, sel->w, 0)))
        return;

    int *to = &sel->editor_fds[0];
    int *from = strstr(args[0], "editor") ? &sel->editor_fds[1] : NULL;
    sel->editor_fds[0] = sel->editor_fds[1] = -1;

    const char *argv[3] = { args[0], NULL, NULL };
    char argline[32];
    int line = term_content_start(sel->app);
    snprintf(argline, sizeof(argline), "+%d", line);
    argv[1] = argline;

    char *cwd = getcwd_by_pid(sel);
    if (term_forkpty(sel->editor, args[0], argv, cwd, NULL, to, from) < 0) {
        term_destroy(sel->editor);
        sel->editor = NULL;
        return;
    }

    sel->term = sel->editor;
    /* The window's own answer buffer starts empty; the paste register does not
     * get touched. An editor that hands nothing back -- or that is still
     * sitting there with the text saved but not yet let go of, which is what
     * `:w` without `:q` looks like from here -- must leave the last copy
     * pastable rather than replace it with silence. */
    if (sel->editor_fds[1] != -1)
        sel->editreg.len = 0;

    if (sel->editor_fds[0] != -1) {
        char *buf = NULL;
        size_t len = term_content_get(sel->app, &buf, colored);
        char *cur = buf;
        while (len > 0) {
            ssize_t res = write(sel->editor_fds[0], cur, len);
            if (res < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                break;
            }
            cur += res;
            len -= res;
        }
        free(buf);
        close(sel->editor_fds[0]);
        sel->editor_fds[0] = -1;
    }

    if (args[1])
        term_write(sel->editor, args[1], strlen(args[1]));
}

static void focusn(const char *args[]) {
    for (Client *c = nextvisible(clients); c; c = nextvisible(c->next)) {
        if (c->order == atoi(args[0])) {
            focus(c);
            if (c->minimized)
                toggleminimize(NULL);
            return;
        }
    }
}

static void focusid(const char *args[]) {
    if (!args[0])
        return;

    const int win_id = atoi(args[0]);
    for (Client *c = clients; c; c = c->next) {
        if (c->id == win_id) {
            focus(c);
            if (c->minimized)
                toggleminimize(NULL);
            if (!isvisible(c)) {
                c->tags |= tagset[seltags];
                tagschanged();
            }
            return;
        }
    }
}

static void focusnext(const char *args[]) {
    Client *c;
    if (!sel)
        return;
    for (c = sel->next; c && !isvisible(c); c = c->next)
        ;
    if (!c)
        for (c = clients; c && !isvisible(c); c = c->next)
            ;
    if (c)
        focus(c);
}

static void focusnextnm(const char *args[]) {
    if (!sel)
        return;
    Client *c = sel;
    do {
        c = nextvisible(c->next);
        if (!c)
            c = nextvisible(clients);
    } while (c->minimized && c != sel);
    focus(c);
}

static void focusprev(const char *args[]) {
    Client *c;
    if (!sel)
        return;
    for (c = sel->prev; c && !isvisible(c); c = c->prev)
        ;
    if (!c) {
        for (c = clients; c && c->next; c = c->next)
            ;
        for (; c && !isvisible(c); c = c->prev)
            ;
    }
    if (c)
        focus(c);
}

static void focuslast(const char *args[]) {
    if (lastsel)
        focus(lastsel);
}

static void focusup(const char *args[]) {
    if (!sel)
        return;
    /* avoid vertical separator, hence +1 in x direction */
    Client *c = get_client_by_coord(sel->x + 1, sel->y - 1);
    if (c)
        focus(c);
    else
        focusprev(args);
}

static void focusdown(const char *args[]) {
    if (!sel)
        return;
    Client *c = get_client_by_coord(sel->x, sel->y + sel->h);
    if (c)
        focus(c);
    else
        focusnext(args);
}

static void focusleft(const char *args[]) {
    if (!sel)
        return;
    Client *c = get_client_by_coord(sel->x - 2, sel->y);
    if (c)
        focus(c);
    else
        focusprev(args);
}

static void focusright(const char *args[]) {
    if (!sel)
        return;
    Client *c = get_client_by_coord(sel->x + sel->w + 1, sel->y);
    if (c)
        focus(c);
    else
        focusnext(args);
}

static void killclient(const char *args[]) {
    if (!sel)
        return;
    debug("killing client with pid: %d\n", sel->pid);
    kill(-sel->pid, SIGKILL);
}

static void paste(const char *args[]) {
    if (sel && copyreg.data)
        term_write(sel->term, copyreg.data, copyreg.len);
}

static void quit(const char *args[]) {
    cleanup();
    exit(EXIT_SUCCESS);
}

static void redraw(const char *args[]) {
    for (Client *c = clients; c; c = c->next) {
        if (!c->minimized) {
            c->term->dirty = true;
            wclear(c->window);
            wnoutrefresh(c->window);
        }
    }
    resize_screen();
}

static void scrollback(const char *args[]) {
    if (!is_content_visible(sel))
        return;

    if (!args[0] || atoi(args[0]) < 0)
        term_scroll(sel->term, -sel->h / 2);
    else
        term_scroll(sel->term, sel->h / 2);

    draw(sel);
    curs_set(term_cursor_visible(sel->term));
}

static void sendkeys(const char *args[]) {
    if (sel && args && args[0])
        term_write(sel->term, args[0], strlen(args[0]));
}

static void setlayout(const char *args[]) {
    unsigned int i;

    if (!args || !args[0]) {
        if (++layout == &layouts[LENGTH(layouts)])
            layout = &layouts[0];
    } else {
        for (i = 0; i < LENGTH(layouts); i++)
            if (!strcmp(args[0], layouts[i].symbol))
                break;
        if (i == LENGTH(layouts))
            return;
        layout = &layouts[i];
    }
    arrange();
}

static void incnmaster(const char *args[]) {
    int delta;

    if (isarrange(fullscreen) || isarrange(grid))
        return;
    /* arg handling, manipulate nmaster */
    if (args[0] == NULL) {
        screen.nmaster = NMASTER;
    } else if (sscanf(args[0], "%d", &delta) == 1) {
        if (args[0][0] == '+' || args[0][0] == '-')
            screen.nmaster += delta;
        else
            screen.nmaster = delta;
        if (screen.nmaster < 1)
            screen.nmaster = 1;
    }
    arrange();
}

static void setmfact(const char *args[]) {
    float delta;

    if (isarrange(fullscreen) || isarrange(grid))
        return;
    /* arg handling, manipulate mfact */
    if (args[0] == NULL) {
        screen.mfact = MFACT;
    } else if (sscanf(args[0], "%f", &delta) == 1) {
        if (args[0][0] == '+' || args[0][0] == '-')
            screen.mfact += delta;
        else
            screen.mfact = delta;
        if (screen.mfact < 0.1)
            screen.mfact = 0.1;
        else if (screen.mfact > 0.9)
            screen.mfact = 0.9;
    }
    arrange();
}

static void startup(const char *args[]) {
    for (unsigned int i = 0; i < LENGTH(actions); i++)
        actions[i].cmd(actions[i].args);
}

static void togglebar(const char *args[]) {
    if (bar.pos == BAR_OFF)
        showbar();
    else
        hidebar();
    bar.autohide = false;
    updatebarpos();
    redraw(NULL);
}

static void togglebarpos(const char *args[]) {
    switch (bar.pos == BAR_OFF ? bar.lastpos : bar.pos) {
        case BAR_TOP:
            bar.pos = BAR_BOTTOM;
            break;
        case BAR_BOTTOM:
            bar.pos = BAR_TOP;
            break;
    }
    updatebarpos();
    redraw(NULL);
}

static void toggleminimize(const char *args[]) {
    Client *c, *m, *t;
    unsigned int n;
    if (!sel)
        return;
    /* the last window can't be minimized */
    if (!sel->minimized) {
        for (n = 0, c = nextvisible(clients); c; c = nextvisible(c->next))
            if (!c->minimized)
                n++;
        if (n == 1)
            return;
    }
    sel->minimized = !sel->minimized;
    m = sel;
    /* check whether the master client was minimized */
    if (sel == nextvisible(clients) && sel->minimized) {
        c = nextvisible(sel->next);
        detach(c);
        attach(c);
        focus(c);
        detach(m);
        for (; c && (t = nextvisible(c->next)) && !t->minimized; c = t)
            ;
        attachafter(m, c);
    } else if (m->minimized) {
        /* non master window got minimized move it above all other
         * minimized ones */
        focusnextnm(NULL);
        detach(m);
        for (c = nextvisible(clients);
            c && (t = nextvisible(c->next)) && !t->minimized; c = t)
            ;
        attachafter(m, c);
    } else { /* window is no longer minimized, move it to the master area */
        m->term->dirty = true;
        detach(m);
        attach(m);
    }
    arrange();
}

static void togglemouse(const char *args[]) {
    mouse_events_enabled = !mouse_events_enabled;
    mouse_setup();
}

static void togglerunall(const char *args[]) {
    runinall = !runinall;
    drawbar();
    draw_all();
}

static void zoom(const char *args[]) {
    Client *c;

    if (!sel)
        return;
    if (args && args[0])
        focusn(args);
    if ((c = sel) == nextvisible(clients))
        if (!(c = nextvisible(c->next)))
            return;
    detach(c);
    attach(c);
    focus(c);
    if (c->minimized)
        toggleminimize(NULL);
    arrange();
}

/* commands for use by mouse bindings */
static void mouse_focus(const char *args[]) {
    focus(msel);
    if (msel->minimized)
        toggleminimize(NULL);
}

static void mouse_fullscreen(const char *args[]) {
    mouse_focus(NULL);
    setlayout(isarrange(fullscreen) ? NULL : args);
}

static void mouse_minimize(const char *args[]) {
    focus(msel);
    toggleminimize(NULL);
}

static void mouse_zoom(const char *args[]) {
    focus(msel);
    zoom(NULL);
}

static Cmd *get_cmd_by_name(const char *name) {
    for (unsigned int i = 0; i < LENGTH(commands); i++) {
        if (!strcmp(name, commands[i].name))
            return &commands[i];
    }
    return NULL;
}

static void handle_cmdfifo(void) {
    int r;
    char *p, *s, cmdbuf[512], c;
    Cmd *cmd;

    r = read(cmdfifo.fd, cmdbuf, sizeof cmdbuf - 1);
    if (r <= 0) {
        cmdfifo.fd = -1;
        return;
    }

    cmdbuf[r] = '\0';
    p = cmdbuf;
    while (*p) {
        /* find the command name */
        for (; *p == ' ' || *p == '\n'; p++)
            ;
        for (s = p; *p && *p != ' ' && *p != '\n'; p++)
            ;
        if ((c = *p))
            *p++ = '\0';
        if (*s && (cmd = get_cmd_by_name(s)) != NULL) {
            bool quote = false;
            int argc = 0;
            const char *args[MAX_ARGS], *arg;
            memset(args, 0, sizeof(args));
            /* if arguments were specified in config.h ignore the one given via
             * the named pipe and thus skip everything until we find a new line
             */
            if (cmd->action.args[0] || c == '\n') {
                debug("execute %s", s);
                cmd->action.cmd(cmd->action.args);
                while (*p && *p != '\n')
                    p++;
                continue;
            }
            /* no arguments were given in config.h so we parse the command line */
            while (*p == ' ')
                p++;
            arg = p;
            for (; (c = *p); p++) {
                switch (*p) {
                    case '\\':
                        /* remove the escape character '\\' move every
                     * following character to the left by one position
                     */
                        switch (p[1]) {
                            case '\\':
                            case '\'':
                            case '\"': {
                                char *t = p + 1;
                                do {
                                    t[-1] = *t;
                                } while (*t++);
                            }
                        }
                        break;
                    case '\'':
                    case '\"':
                        quote = !quote;
                        break;
                    case ' ':
                        if (!quote) {
                            case '\n':
                                /* remove trailing quote if there is one */
                                if (*(p - 1) == '\'' || *(p - 1) == '\"')
                                    *(p - 1) = '\0';
                                *p++ = '\0';
                                /* remove leading quote if there is one */
                                if (*arg == '\'' || *arg == '\"')
                                    arg++;
                                if (argc < MAX_ARGS)
                                    args[argc++] = arg;

                                while (*p == ' ')
                                    ++p;
                                arg = p--;
                        }
                        break;
                }

                if (c == '\n' || *p == '\n') {
                    if (!*p)
                        p++;
                    debug("execute %s", s);
                    for (int i = 0; i < argc; i++)
                        debug(" %s", args[i]);
                    debug("\n");
                    cmd->action.cmd(args);
                    break;
                }
            }
        }
    }
}

static void handle_mouse(void) {
    MEVENT event;
    unsigned int i;
    if (getmouse(&event) != OK)
        return;
    msel = get_client_by_coord(event.x, event.y);

    if (!msel)
        return;

    debug("mouse x:%d y:%d cx:%d cy:%d mask:%d\n", event.x, event.y,
        event.x - msel->x, event.y - msel->y, event.bstate);

    term_mouse(msel->term, event.x - msel->x, event.y - msel->y, event.bstate);

    for (i = 0; i < LENGTH(buttons); i++) {
        if (event.bstate & buttons[i].mask)
            buttons[i].action.cmd(buttons[i].action.args);
    }

    msel = NULL;
}

static void handle_statusbar(void) {
    char *p;
    int r;
    switch (r = read(bar.fd, bar.text, sizeof bar.text - 1)) {
        case -1:
            strncpy(bar.text, strerror(errno), sizeof bar.text - 1);
            bar.text[sizeof bar.text - 1] = '\0';
            bar.fd = -1;
            break;
        case 0:
            bar.fd = -1;
            break;
        default:
            bar.text[r] = '\0';
            p = bar.text + r - 1;
            for (; p >= bar.text && *p == '\n'; *p-- = '\0')
                ;
            for (; p >= bar.text && *p != '\n'; --p)
                ;
            if (p >= bar.text)
                memmove(bar.text, p + 1, strlen(p));
            drawbar();
    }
}

/* Take whatever the editor has written so far.
 *
 * Called every time the pipe is readable, and not once at the end: the pipe
 * holds a few dozen kilobytes, so an editor handing back more than that used
 * to fill it and block. Blocked, it never exited; never having exited, dvtm
 * never read; and copy mode never finished. Any real scrollback is that long.
 */
static void read_editor(Client *c) {
    ssize_t len;

    if (c->editor_fds[1] == -1)
        return;
    if (c->editreg.len == c->editreg.size) {
        size_t want =
            c->editreg.size ? c->editreg.size * 2 : (size_t)screen.history;
        char *p = realloc(c->editreg.data, want);
        if (!p) {
            /* Nothing can be added and nothing can be read: leave the pipe
             * alone rather than spin on it. What is already there is still
             * handed over when the editor exits. */
            close(c->editor_fds[1]);
            c->editor_fds[1] = -1;
            return;
        }
        c->editreg.data = p;
        c->editreg.size = want;
    }
    len = read(c->editor_fds[1], c->editreg.data + c->editreg.len,
        c->editreg.size - c->editreg.len);
    if (len > 0) {
        c->editreg.len += (size_t)len;
        return;
    }
    if (len < 0 && errno == EINTR)
        return;
    /* Nothing more is coming: either the editor closed its end or the pipe
     * broke. Either way this side is done with it. */
    close(c->editor_fds[1]);
    c->editor_fds[1] = -1;
}

static void handle_editor(Client *c) {
    while (c->editor_fds[1] != -1)
        read_editor(c);
    /* The editor is gone, so whatever it wrote is all there is. An empty
     * answer means it declined -- it quit without saving, or it failed -- and
     * the register keeps what it had. The buffers are swapped rather than
     * copied, which also hands the old register's allocation back for the next
     * editor to grow into. */
    if (c->editreg.len) {
        Register t = copyreg;
        copyreg = c->editreg;
        c->editreg = t;
        c->editreg.len = 0;
    }
    c->editor_died = false;
    c->editor_fds[1] = -1;
    term_destroy(c->editor);
    c->editor = NULL;
    c->term = c->app;
    c->term->dirty = true;
    draw_content(c);
    wnoutrefresh(c->window);
}

static int open_or_create_fifo(const char *name, const char **name_created) {
    struct stat info;
    int fd;

    do {
        if ((fd = open(name, O_RDWR | O_NONBLOCK)) == -1) {
            if (errno == ENOENT && !mkfifo(name, S_IRUSR | S_IWUSR)) {
                *name_created = name;
                continue;
            }
            error("%s\n", strerror(errno));
        }
    } while (fd == -1);

    if (fstat(fd, &info) == -1)
        error("%s\n", strerror(errno));
    if (!S_ISFIFO(info.st_mode))
        error("%s is not a named pipe\n", name);
    return fd;
}

static void usage(void) {
    cleanup();
    eprint("usage: dvtm [-v] [-M] [-m mod] [-d delay] [-h lines] [-t "
           "title] "
           "[-s status-fifo] [-c cmd-fifo] [cmd...]\n");
    exit(EXIT_FAILURE);
}

static bool parse_args(int argc, char *argv[]) {
    bool init = false;
    const char *name = argv[0];

    if (name && (name = strrchr(name, '/')))
        dvtm_name = name + 1;
    if (!getenv("ESCDELAY"))
        set_escdelay(100);
    for (int arg = 1; arg < argc; arg++) {
        if (argv[arg][0] != '-') {
            const char *args[] = { argv[arg], NULL, NULL };
            if (!init) {
                setup();
                init = true;
            }
            create(args);
            continue;
        }
        if (argv[arg][1] != 'v' && argv[arg][1] != 'M' && (arg + 1) >= argc)
            usage();
        switch (argv[arg][1]) {
            case 'v':
                puts("dvtm-" VERSION " © 2007-2016 Marc André Tanner");
                exit(EXIT_SUCCESS);
            case 'M':
                mouse_events_enabled = !mouse_events_enabled;
                break;
            case 'm': {
                char *mod = argv[++arg];
                if (mod[0] == '^' && mod[1])
                    *mod = CTRL(mod[1]);
                for (unsigned int b = 0; b < LENGTH(bindings); b++)
                    if (bindings[b].keys[0] == MOD)
                        bindings[b].keys[0] = *mod;
                break;
            }
            case 'd':
                set_escdelay(atoi(argv[++arg]));
                if (ESCDELAY < 50)
                    set_escdelay(50);
                else if (ESCDELAY > 1000)
                    set_escdelay(1000);
                break;
            case 'h':
                screen.history = atoi(argv[++arg]);
                break;
            case 't':
                title = argv[++arg];
                break;
            case 's':
                bar.fd = open_or_create_fifo(argv[++arg], &bar.file);
                updatebarpos();
                break;
            case 'c': {
                char *fifo;
                cmdfifo.fd = open_or_create_fifo(argv[++arg], &cmdfifo.file);
                if (!(fifo = realpath(argv[arg], NULL)))
                    error("%s\n", strerror(errno));
                setenv("DVTM_CMD_FIFO", fifo, 1);
                free(fifo);
                break;
            }
            default:
                usage();
        }
    }
    return init;
}

/* Add one descriptor to the set select() is about to be handed, and keep nfds
 * at the highest of them.
 *
 * This was written out four times, and one of the four assigned nfds where the
 * others took the maximum. With a command fifo whose descriptor happened to be
 * lower than the self-pipe's -- which is what `dvtm -c fifo` with no window
 * gives you, since the fifo is opened while parsing arguments and the pipe is
 * made afterwards in setup() -- select() was told a smaller nfds than the set
 * contained, and the pipe that wakes this loop when a signal arrives was not
 * watched at all. */
static void watch(int fd, fd_set *rd, int *nfds) {
    if (fd < 0)
        return;
    FD_SET(fd, rd);
    if (fd > *nfds)
        *nfds = fd;
}

int main(int argc, char *argv[]) {
    unsigned int key_index = 0;
    sigset_t blockset;
    memset(keys, 0, sizeof(keys));

    setenv("DVTM", VERSION, 1);
    /* Signals stay blocked while we process input: a handler firing inside
     * getch(2) would interrupt it and drop the keystroke. They are unblocked
     * only around select(2) -- see the self-pipe comment above for why
     * pselect(2) cannot do this for us. */
    sigemptyset(&blockset);
    sigaddset(&blockset, SIGWINCH);
    sigaddset(&blockset, SIGCHLD);
    sigprocmask(SIG_BLOCK, &blockset, NULL);

    if (!parse_args(argc, argv)) {
        setup();
        startup(NULL);
    }

    while (running) {
        int r, nfds = 0;
        fd_set rd, wr;

        if (screen.need_resize) {
            resize_screen();
            screen.need_resize = false;
        }

        FD_ZERO(&rd);
        FD_ZERO(&wr);
        watch(STDIN_FILENO, &rd, &nfds);
        watch(sigpipe[0], &rd, &nfds);
        watch(cmdfifo.fd, &rd, &nfds);
        watch(bar.fd, &rd, &nfds);

        for (Client *c = clients; c;) {
            if (c->editor && c->editor_died)
                handle_editor(c);
            if (!c->editor && c->died) {
                Client *t = c->next;
                destroy(c);
                c = t;
                continue;
            }
            watch(c->editor ? c->editor->pty : c->app->pty, &rd, &nfds);
            if (c->editor_fds[1] != -1)
                watch(c->editor_fds[1], &rd, &nfds);
            /* Reading stays unconditional. That is the whole point: the child
             * only takes more once dvtm has drained what it wrote back. */
            if (term_pending(c->term))
                watch(c->term->pty, &wr, &nfds);
            c = c->next;
        }

        doupdate();
        sigprocmask(SIG_UNBLOCK, &blockset, NULL);
        r = select(nfds + 1, &rd, &wr, NULL, NULL);
        sigprocmask(SIG_BLOCK, &blockset, NULL);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("select()");
            exit(EXIT_FAILURE);
        }

        reap_children();

        if (sigpipe[0] != -1 && FD_ISSET(sigpipe[0], &rd)) {
            char discard[64];
            while (read(sigpipe[0], discard, sizeof discard) > 0)
                ;
        }

        if (FD_ISSET(STDIN_FILENO, &rd)) {
            /* Drain what ncurses already holds. select() reports the
             * descriptor, but get_wch() reads ahead into ncurses' own buffer:
             * after the first key the descriptor is quiet again, so reading
             * one key per wakeup strands the rest of a burst until unrelated
             * input happens to arrive. Typing quickly or pasting lost keys.
             * nodelay is re-armed each pass because keypress() clears it.
             *
             * get_wch() and not getch(): getch() reports a character one byte
             * at a time, and a byte of UTF-8 is not a character. Everything
             * above U+007F then reached the child re-encoded -- 'á' as 'Ã¡'.
             * get_wch() decodes in ncurses, where the locale is already
             * known, and says in its return value which of the two kinds of
             * thing it read. */
            for (;;) {
                wint_t wch;
                int rc;
                bool is_key;

                nodelay(stdscr, TRUE);
                rc = get_wch(&wch);
                if (rc == ERR) {
                    nodelay(stdscr, FALSE);
                    break;
                }
                is_key = rc == KEY_CODE_YES;

                if (is_key && wch == KEY_MOUSE) {
                    key_index = 0;
                    handle_mouse();
                } else if (is_key || wch < KEY_MIN) {
                    /* Only a key code, or a character below the range curses
                     * reserves for key codes, is allowed to match a binding.
                     * The two overlap by value, and without this a letter
                     * whose code point happens to equal KEY_F(4) would run a
                     * window command instead of being typed. */
                    KeyBinding *binding = NULL;
                    keys[key_index++] = (unsigned int)wch;
                    if ((binding = keybinding(keys, key_index))) {
                        unsigned int key_length = MAX_KEYS;
                        while (key_length > 1 && !binding->keys[key_length - 1])
                            key_length--;
                        if (key_index == key_length) {
                            binding->action.cmd(binding->action.args);
                            key_index = 0;
                            memset(keys, 0, sizeof(keys));
                        }
                    } else {
                        key_index = 0;
                        memset(keys, 0, sizeof(keys));
                        keypress((int)wch, is_key);
                    }
                } else {
                    key_index = 0;
                    memset(keys, 0, sizeof(keys));
                    keypress((int)wch, false);
                }
                drawbar();
                if (is_content_visible(sel))
                    wnoutrefresh(sel->window);
            }
            if (r == 1) /* no data available on pty's */
                continue;
        }

        if (cmdfifo.fd != -1 && FD_ISSET(cmdfifo.fd, &rd))
            handle_cmdfifo();

        if (bar.fd != -1 && FD_ISSET(bar.fd, &rd))
            handle_statusbar();

        for (Client *c = clients; c; c = c->next) {
            if (c->editor_fds[1] != -1 && FD_ISSET(c->editor_fds[1], &rd))
                read_editor(c);

            if (FD_ISSET(c->term->pty, &wr))
                term_flush(c->term);

            if (FD_ISSET(c->term->pty, &rd)) {
                if (term_process(c->term) < 0 && errno == EIO) {
                    if (c->editor)
                        c->editor_died = true;
                    else
                        c->died = true;
                    continue;
                }
            }

            if (c != sel && is_content_visible(c)) {
                draw_content(c);
                wnoutrefresh(c->window);
            }
        }

        if (is_content_visible(sel)) {
            draw_content(sel);
            curs_set(term_cursor_visible(sel->term));
            wnoutrefresh(sel->window);
        }
    }

    cleanup();
    return 0;
}

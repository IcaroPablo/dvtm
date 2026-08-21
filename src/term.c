/*
 * term.c - one child: its pty, its libvterm instance, and its screen state.
 *
 * libvterm owns the escape parsing and the cell grid. The scrollback is ours,
 * because libvterm hands lines off as they fall out of the top of the screen
 * and expects somebody else to keep them. Painting lives in ui.c.
 */
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <wchar.h>

#include "term.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static const char *const *keytable_overlay;
static int keytable_overlay_len;
static bool is_utf8;
static char child_term[32]; /* the TERM handed to children */

/* ── scrollback ───────────────────────────────────────────────────────────── */

static void line_free(Line *l) {
    free(l->cells);
    l->cells = NULL;
    l->cols = 0;
}

/* Index into the ring, oldest line first. */
Line *term_sb_at(Term *t, int n) {
    if (n < 0 || n >= t->sb_count)
        return NULL;
    return &t->sb[(t->sb_first + n) % t->sb_size];
}

static int sb_pushline(int cols, const VTermScreenCell *cells, void *user) {
    Term *t = user;
    Line *l;

    if (t->sb_size <= 0)
        return 0;

    if (t->sb_count == t->sb_size) {
        /* full: reuse the oldest slot */
        l = &t->sb[t->sb_first];
        t->sb_first = (t->sb_first + 1) % t->sb_size;
        t->sb_count--;
        line_free(l);
    }
    l = &t->sb[(t->sb_first + t->sb_count) % t->sb_size];
    l->cells = calloc((size_t)cols, sizeof(VTermScreenCell));
    if (!l->cells)
        return 0;
    memcpy(l->cells, cells, (size_t)cols * sizeof(VTermScreenCell));
    l->cols = cols;
    t->sb_count++;
    return 1;
}

/* libvterm asks for a line back when the screen scrolls down past the top. */
static int sb_popline(int cols, VTermScreenCell *cells, void *user) {
    Term *t = user;
    Line *l;

    if (t->sb_count == 0)
        return 0;
    l = &t->sb[(t->sb_first + t->sb_count - 1) % t->sb_size];
    for (int c = 0; c < cols; c++) {
        if (c < l->cols)
            cells[c] = l->cells[c];
        else
            memset(&cells[c], 0, sizeof(VTermScreenCell));
    }
    line_free(l);
    t->sb_count--;
    return 1;
}

static int sb_clear(void *user) {
    Term *t = user;
    for (int i = 0; i < t->sb_count; i++)
        line_free(term_sb_at(t, i));
    t->sb_count = t->sb_first = 0;
    return 1;
}

/* ── libvterm callbacks ───────────────────────────────────────────────────── */

static int cb_damage(VTermRect rect, void *user) {
    ((Term *)user)->dirty = true;
    return 1;
}

static int cb_moverect(VTermRect dest, VTermRect src, void *user) {
    ((Term *)user)->dirty = true;
    return 1;
}

static int cb_movecursor(
    VTermPos pos, VTermPos oldpos, int visible, void *user) {
    Term *t = user;
    t->cursor = pos;
    t->dirty = true;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user) {
    Term *t = user;

    switch (prop) {
        case VTERM_PROP_CURSORVISIBLE:
            t->cursor_visible = val->boolean;
            return 1;
        case VTERM_PROP_TITLE:
            /* libvterm 0.3 reports the title as a fragment with a length; older
         * ones used a plain string. Take the fragment form. */
            if (val->string.len < sizeof t->title) {
                memcpy(t->title, val->string.str, val->string.len);
                t->title[val->string.len] = '\0';
            }
            if (t->title_handler)
                t->title_handler(t, t->title);
            return 1;
        default:
            return 0;
    }
}

static int cb_bell(void *user) {
    Term *t = user;
    if (t->urgent_handler)
        t->urgent_handler(t);
    return 1;
}

static int cb_resize(int rows, int cols, void *user) {
    Term *t = user;
    t->rows = rows;
    t->cols = cols;
    t->dirty = true;
    return 1;
}

static const VTermScreenCallbacks screen_callbacks = {
    .damage = cb_damage,
    .moverect = cb_moverect,
    .movecursor = cb_movecursor,
    .settermprop = cb_settermprop,
    .bell = cb_bell,
    .resize = cb_resize,
    .sb_pushline = sb_pushline,
    .sb_popline = sb_popline,
    .sb_clear = sb_clear,
};

/* ── lifecycle ────────────────────────────────────────────────────────────── */

void term_init(char const *const keytable[], int count) {
    const char *cset, *term;

    keytable_overlay = keytable;
    keytable_overlay_len = count;

    ui_init_colors();

    cset = nl_langinfo(CODESET);
    is_utf8 = cset && !strcmp(cset, "UTF-8");

    /* The TERM children are run with. Without it they inherit whatever dvtm
     * was started with, or nothing, and every curses program inside dvtm
     * misbehaves -- wrong colours, and a line editor that cannot position the
     * cursor redraws what you typed instead of moving over it. */
    if (!(term = getenv("DVTM_TERM")))
        term = "dvtm";
    snprintf(child_term, sizeof child_term, "%s%s", term,
        COLORS >= 256 ? "-256color" : "");
}

Term *term_create(int rows, int cols, int scroll_buf_sz) {
    Term *t;

    if (rows <= 0 || cols <= 0)
        return NULL;
    if (!(t = calloc(1, sizeof *t)))
        return NULL;

    t->rows = rows;
    t->cols = cols;
    t->pty = -1;
    t->pid = -1;
    t->deffg = t->defbg = -1;
    t->cursor_visible = true;
    t->dirty = true;

    if (scroll_buf_sz > 0 &&
        (t->sb = calloc((size_t)scroll_buf_sz, sizeof(Line))))
        t->sb_size = scroll_buf_sz;

    if (!(t->vt = vterm_new(rows, cols))) {
        free(t->sb);
        free(t);
        return NULL;
    }
    vterm_set_utf8(t->vt, is_utf8 ? 1 : 0);
    t->state = vterm_obtain_state(t->vt);
    t->screen = vterm_obtain_screen(t->vt);
    vterm_screen_set_callbacks(t->screen, &screen_callbacks, t);
    vterm_screen_enable_altscreen(t->screen, 1);
    vterm_screen_reset(t->screen, 1);
    return t;
}

void term_resize(Term *t, int rows, int cols) {
    struct winsize ws = { .ws_row = rows, .ws_col = cols };

    if (rows <= 0 || cols <= 0 || (rows == t->rows && cols == t->cols))
        return;

    term_noscroll(t);
    vterm_set_size(t->vt, rows, cols);
    t->rows = rows;
    t->cols = cols;
    t->dirty = true;
    if (t->pty >= 0)
        ioctl(t->pty, TIOCSWINSZ, &ws);
}

void term_destroy(Term *t) {
    if (!t)
        return;
    for (int i = 0; i < t->sb_count; i++)
        line_free(term_sb_at(t, i));
    free(t->sb);
    free(t->out);
    if (t->vt)
        vterm_free(t->vt);
    if (t->pty >= 0)
        close(t->pty);
    free(t);
}

/* ── the pty ──────────────────────────────────────────────────────────────── */

/* forkpty(3) written out; see the note in the commit that introduced it. Not
 * POSIX, lives in a different header on every system, and absent on some. */
static pid_t pty_fork(int *master, const struct winsize *ws) {
    int mfd, sfd;
    const char *name;
    pid_t pid;

    if ((mfd = posix_openpt(O_RDWR | O_NOCTTY)) < 0)
        return -1;
    /* The master, and only the master: term_write() must never block, and the
     * child's own descriptors have to stay as any program expects them. */
    if (fcntl(mfd, F_SETFL, fcntl(mfd, F_GETFL) | O_NONBLOCK) < 0) {
        close(mfd);
        return -1;
    }
    if (grantpt(mfd) < 0 || unlockpt(mfd) < 0 || !(name = ptsname(mfd))) {
        close(mfd);
        return -1;
    }
    if ((sfd = open(name, O_RDWR | O_NOCTTY)) < 0) {
        close(mfd);
        return -1;
    }
    /* On the slave: macOS rejects TIOCSWINSZ on the master, and a pty left at
     * 0x0 makes the child draw nothing at all. */
    if (ws && ioctl(sfd, TIOCSWINSZ, ws) < 0) {
        close(sfd);
        close(mfd);
        return -1;
    }

    switch ((pid = fork())) {
        case -1:
            close(sfd);
            close(mfd);
            return -1;
        case 0:
            close(mfd);
            if (setsid() < 0)
                _exit(1);
            if (ioctl(sfd, TIOCSCTTY, 0) < 0)
                _exit(1);
            dup2(sfd, STDIN_FILENO);
            dup2(sfd, STDOUT_FILENO);
            dup2(sfd, STDERR_FILENO);
            if (sfd > STDERR_FILENO)
                close(sfd);
            return 0;
        default:
            close(sfd);
            *master = mfd;
            return pid;
    }
}

pid_t term_forkpty(Term *t, const char *p, const char *argv[], const char *cwd,
    const char *env[], int *to, int *from) {
    int vt2ed[2], ed2vt[2];
    struct winsize ws = { .ws_row = t->rows, .ws_col = t->cols };
    pid_t pid;

    if (to && pipe(vt2ed)) {
        *to = -1;
        to = NULL;
    }
    if (from && pipe(ed2vt)) {
        *from = -1;
        from = NULL;
    }

    if ((pid = pty_fork(&t->pty, &ws)) < 0) {
        if (to) {
            close(vt2ed[0]);
            close(vt2ed[1]);
            *to = -1;
        }
        if (from) {
            close(ed2vt[0]);
            close(ed2vt[1]);
            *from = -1;
        }
        return -1;
    }

    if (pid == 0) {
        sigset_t emptyset;
        sigemptyset(&emptyset);
        sigprocmask(SIG_SETMASK, &emptyset, NULL);

        if (to) {
            close(vt2ed[1]);
            dup2(vt2ed[0], STDIN_FILENO);
            close(vt2ed[0]);
        }
        if (from) {
            close(ed2vt[0]);
            dup2(ed2vt[1], STDOUT_FILENO);
            close(ed2vt[1]);
        }

        /* Close inherited descriptors. Walk the whole range rather than
         * stopping at the first gap, or the descriptors above a gap survive
         * and the child inherits other windows' ptys. */
        for (int fd = 3, maxfd = (int)sysconf(_SC_OPEN_MAX); fd < maxfd; fd++)
            close(fd);

        for (const char **envp = env; envp && envp[0]; envp += 2)
            setenv(envp[0], envp[1], 1);
        setenv("TERM", child_term, 1);
        setenv("COLORTERM", "truecolor", 1);

        if (cwd && chdir(cwd) != 0) {
            fprintf(stderr, "\nchdir() failed. ");
            perror(cwd);
            exit(1);
        }

        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = SIG_DFL;
        sigaction(SIGPIPE, &sa, NULL);

        execvp(p, (char *const *)argv);
        fprintf(stderr, "\nexecv() failed.\nCommand: '%s'\n", argv[0]);
        exit(1);
    }

    if (to) {
        close(vt2ed[0]);
        *to = vt2ed[1];
    }
    if (from) {
        close(ed2vt[1]);
        *from = ed2vt[0];
    }
    return t->pid = pid;
}

/* ── reading and writing ──────────────────────────────────────────────────── */

bool term_pending(Term *t) {
    return t && t->outpos < t->outlen;
}

void term_flush(Term *t) {
    while (t->outpos < t->outlen) {
        ssize_t res = write(t->pty, t->out + t->outpos, t->outlen - t->outpos);
        if (res < 0) {
            if (errno == EINTR)
                continue;
            break; /* EAGAIN: the pty is full; the next pass will try again */
        }
        t->outpos += (size_t)res;
    }
    if (t->outpos == t->outlen)
        t->outpos = t->outlen = 0;
}

static bool queue(Term *t, const char *buf, size_t len) {
    /* Compact before growing: a long paste drains from the front, so the room
     * already consumed is usually enough for what follows. */
    if (t->outpos > 0) {
        memmove(t->out, t->out + t->outpos, t->outlen - t->outpos);
        t->outlen -= t->outpos;
        t->outpos = 0;
    }
    if (t->outlen + len > t->outsize) {
        size_t want = t->outsize ? t->outsize : 4096;
        char *p;
        while (want < t->outlen + len)
            want *= 2;
        if (!(p = realloc(t->out, want)))
            return false;
        t->out = p;
        t->outsize = want;
    }
    memcpy(t->out + t->outlen, buf, len);
    t->outlen += len;
    return true;
}

ssize_t term_write(Term *t, const char *buf, size_t len) {
    if (!queue(t, buf, len))
        return -1;
    term_flush(t);
    return (ssize_t)len;
}

/* Send whatever libvterm has queued for the child: key presses and mouse
 * reports come back out this way, as the bytes a real terminal would send. */
static void flush_output(Term *t) {
    char buf[512];
    size_t len;

    while ((len = vterm_output_read(t->vt, buf, sizeof buf)) > 0)
        term_write(t, buf, len);
}

void term_paste(Term *t, const char *buf, size_t len) {
    /* All three append to the one queue, so the order on the wire is the order
     * here even when the pty takes none of it yet. */
    vterm_keyboard_start_paste(t->vt);
    flush_output(t);
    term_write(t, buf, len);
    vterm_keyboard_end_paste(t->vt);
    flush_output(t);
}

int term_process(Term *t) {
    char buf[8192];
    ssize_t res;

    if (t->pty < 0) {
        errno = EINVAL;
        return -1;
    }
    res = read(t->pty, buf, sizeof buf);
    if (res < 0) {
        /* The master is non-blocking, so an empty pty is an ordinary answer
         * and not the child going away. Reporting it as an error would tear
         * down a perfectly live window. */
        if (errno == EAGAIN || errno == EINTR)
            return 0;
        return -1;
    }
    /* End of file: the child is gone. Linux reports that as EIO, macOS and
     * the BSDs as a zero-byte read; report it the same way everywhere. */
    if (res == 0) {
        errno = EIO;
        return -1;
    }
    vterm_input_write(t->vt, buf, (size_t)res);
    flush_output(t);
    return 0;
}

/* ── keyboard ─────────────────────────────────────────────────────────────── */

/* ncurses hands back a keycode; libvterm wants to be told which key it was and
 * generates the right bytes itself, including the application-cursor variants
 * that vt.c had to special-case by hand. */
static VTermKey vterm_key_of(int keycode) {
    switch (keycode) {
        case KEY_ENTER:
            return VTERM_KEY_ENTER;
        case KEY_BACKSPACE:
            return VTERM_KEY_BACKSPACE;
        case KEY_UP:
            return VTERM_KEY_UP;
        case KEY_DOWN:
            return VTERM_KEY_DOWN;
        case KEY_LEFT:
            return VTERM_KEY_LEFT;
        case KEY_RIGHT:
            return VTERM_KEY_RIGHT;
        case KEY_IC:
            return VTERM_KEY_INS;
        case KEY_DC:
            return VTERM_KEY_DEL;
        case KEY_HOME:
            return VTERM_KEY_HOME;
        case KEY_END:
            return VTERM_KEY_END;
        case KEY_PPAGE:
            return VTERM_KEY_PAGEUP;
        case KEY_NPAGE:
            return VTERM_KEY_PAGEDOWN;
        case KEY_BTAB:
            return VTERM_KEY_TAB;
        default:
            if (keycode >= KEY_F(1) && keycode <= KEY_F(12))
                return (VTermKey)(VTERM_KEY_FUNCTION_0 + (keycode - KEY_F(0)));
            return VTERM_KEY_NONE;
    }
}

void term_keypress(Term *t, int keycode) {
    VTermKey key;

    term_noscroll(t);

    /* A sequence supplied through config.h wins: it exists precisely to say
     * something this mapping would otherwise get wrong. */
    if (keycode >= 0 && keycode < keytable_overlay_len && keytable_overlay &&
        keytable_overlay[keycode]) {
        term_write(
            t, keytable_overlay[keycode], strlen(keytable_overlay[keycode]));
        return;
    }

    if ((key = vterm_key_of(keycode)) == VTERM_KEY_NONE)
        return;
    vterm_keyboard_key(t->vt, key, VTERM_MOD_NONE);
    flush_output(t);
}

/* One character the user typed, as a code point rather than as a byte.
 *
 * Kept apart from term_keypress() because the two cannot be told apart by
 * value: curses key codes start at KEY_MIN, and so do plenty of perfectly
 * ordinary letters. Only the caller knows which of the two it read, so only
 * the caller can choose the entry point.
 *
 * The code point, and not the byte, is the whole point. libvterm encodes what
 * it is given; handing it a byte of UTF-8 makes it encode that byte as a
 * character in its own right, and 'á' arrives at the child as 'Ã¡'. */
void term_keychar(Term *t, uint32_t codepoint) {
    term_noscroll(t);
    vterm_keyboard_unichar(t->vt, codepoint, VTERM_MOD_NONE);
    flush_output(t);
}

void term_mouse(Term *t, int x, int y, mmask_t mask) {
    vterm_mouse_move(t->vt, y, x, VTERM_MOD_NONE);

    if (mask & BUTTON1_PRESSED)
        vterm_mouse_button(t->vt, 1, true, VTERM_MOD_NONE);
    else if (mask & BUTTON1_RELEASED)
        vterm_mouse_button(t->vt, 1, false, VTERM_MOD_NONE);
    else if (mask & BUTTON2_PRESSED)
        vterm_mouse_button(t->vt, 2, true, VTERM_MOD_NONE);
    else if (mask & BUTTON2_RELEASED)
        vterm_mouse_button(t->vt, 2, false, VTERM_MOD_NONE);
    else if (mask & BUTTON3_PRESSED)
        vterm_mouse_button(t->vt, 3, true, VTERM_MOD_NONE);
    else if (mask & BUTTON3_RELEASED)
        vterm_mouse_button(t->vt, 3, false, VTERM_MOD_NONE);

    flush_output(t);
}

/* ── scrollback navigation ────────────────────────────────────────────────── */

void term_scroll(Term *t, int rows) {
    int max = t->sb_count;

    if (rows < 0) { /* towards the past */
        t->scroll = MIN(t->scroll - rows, max);
    } else {
        t->scroll = MAX(t->scroll - rows, 0);
    }
    t->dirty = true;
}

void term_noscroll(Term *t) {
    if (t->scroll) {
        t->scroll = 0;
        t->dirty = true;
    }
}

int term_content_start(Term *t) {
    return t->sb_count - t->scroll;
}

bool term_cursor_visible(Term *t) {
    return t->scroll ? false : t->cursor_visible;
}

/* ── copy mode ────────────────────────────────────────────────────────────── */

size_t term_content_get(Term *t, char **buf, bool colored) {
    int lines = t->sb_count + t->rows;
    size_t size = (size_t)lines *
                  ((size_t)(t->cols + 1) * ((colored ? 64 : 0) + MB_CUR_MAX));
    VTermScreenCell *cells;
    char *s;

    if (!(*buf = malloc(size)))
        return 0;
    if (!(cells = calloc((size_t)t->cols, sizeof *cells))) {
        free(*buf);
        *buf = NULL;
        return 0;
    }
    s = *buf;

    for (int i = 0; i < lines; i++) {
        char *last_non_space = s;
        int32_t prev_fg = -2, prev_bg = -2;

        if (i < t->sb_count) {
            Line *l = term_sb_at(t, i);
            for (int c = 0; c < t->cols; c++) {
                if (l && c < l->cols)
                    cells[c] = l->cells[c];
                else
                    memset(&cells[c], 0, sizeof *cells);
            }
        } else {
            for (int c = 0; c < t->cols; c++) {
                VTermPos pos = { .row = i - t->sb_count, .col = c };
                if (!vterm_screen_get_cell(t->screen, pos, &cells[c]))
                    memset(&cells[c], 0, sizeof *cells);
            }
        }

        for (int c = 0; c < t->cols; c++) {
            VTermScreenCell *cell = &cells[c];

            if (colored) {
                int32_t fg = color_of(&cell->fg, true);
                int32_t bg = color_of(&cell->bg, false);
                if (fg != prev_fg) {
                    s += fg == -1 ? sprintf(s, "\033[39m")
                                  : sprintf(s,
                                        "\033[38;2;%d;%"
                                        "d;%dm",
                                        (int)(fg >> 16) & 0xff,
                                        (int)(fg >> 8) & 0xff, (int)fg & 0xff);
                    prev_fg = fg;
                }
                if (bg != prev_bg) {
                    s += bg == -1 ? sprintf(s, "\033[49m")
                                  : sprintf(s,
                                        "\033[48;2;%d;%"
                                        "d;%dm",
                                        (int)(bg >> 16) & 0xff,
                                        (int)(bg >> 8) & 0xff, (int)bg & 0xff);
                    prev_bg = bg;
                }
            }

            if (cell->chars[0] == 0) {
                *s++ = ' ';
            } else {
                char mb[MB_LEN_MAX];
                int n = wctomb(mb, (wchar_t)cell->chars[0]);
                if (n > 0) {
                    memcpy(s, mb, (size_t)n);
                    s += n;
                } else {
                    *s++ = ' ';
                }
                last_non_space = s;
            }
            if (cell->width > 1)
                c += cell->width - 1;
        }

        s = last_non_space;
        if (colored)
            s += sprintf(s, "\033[0m");
        *s++ = '\n';
    }

    free(cells);
    return (size_t)(s - *buf);
}

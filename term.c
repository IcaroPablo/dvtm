/*
 * term.c - one child: its pty, its libvterm instance, and its screen state.
 *
 * Implements the vt.h interface that dvtm.c already uses, so the engine can be
 * replaced without touching the rest of the program. libvterm owns the escape
 * parsing and the cell grid; the scrollback is ours, because libvterm hands
 * lines off as they fall out of the top of the screen and expects somebody
 * else to keep them.
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

#include <vterm.h>

#include "vt.h"

#define RGB(r, g, b) (((r) & 0xff) << 16 | ((g) & 0xff) << 8 | ((b) & 0xff))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

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

static int32_t default_fg = -1, default_bg = -1;
static bool has_default_colors;
static const char * const *keytable_overlay;
static int keytable_overlay_len;
static bool is_utf8;
static char vt_term[32];   /* the TERM handed to children */

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
static int32_t color_of(const VTermColor *c, bool is_fg)
{
	if (VTERM_COLOR_IS_DEFAULT_FG(c) || VTERM_COLOR_IS_DEFAULT_BG(c))
		return -1;
	if (VTERM_COLOR_IS_INDEXED(c))
		return color_index(c->indexed.idx);
	if (VTERM_COLOR_IS_RGB(c))
		return RGB(c->rgb.red, c->rgb.green, c->rgb.blue);
	return is_fg ? default_fg : default_bg;
}

int vt_color_get(Vt *t, int32_t fg, int32_t bg)
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

int vt_color_reserve(int32_t fg, int32_t bg)
{
	return vt_color_get(NULL, fg, bg);
}

void vt_default_colors_set(Vt *t, attr_t attrs, int32_t fg, int32_t bg)
{
	t->defattrs = attrs;
	t->deffg = fg;
	t->defbg = bg;
}

/* ── scrollback ───────────────────────────────────────────────────────────── */

static void line_free(Line *l)
{
	free(l->cells);
	l->cells = NULL;
	l->cols = 0;
}

/* Index into the ring, oldest line first. */
static Line *sb_at(Vt *t, int n)
{
	if (n < 0 || n >= t->sb_count)
		return NULL;
	return &t->sb[(t->sb_first + n) % t->sb_size];
}

static int sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
	Vt *t = user;
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
static int sb_popline(int cols, VTermScreenCell *cells, void *user)
{
	Vt *t = user;
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

static int sb_clear(void *user)
{
	Vt *t = user;
	for (int i = 0; i < t->sb_count; i++)
		line_free(sb_at(t, i));
	t->sb_count = t->sb_first = 0;
	return 1;
}

/* ── libvterm callbacks ───────────────────────────────────────────────────── */

static int cb_damage(VTermRect rect, void *user)
{
	((Vt *)user)->dirty = true;
	return 1;
}

static int cb_moverect(VTermRect dest, VTermRect src, void *user)
{
	((Vt *)user)->dirty = true;
	return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
	Vt *t = user;
	t->cursor = pos;
	t->dirty = true;
	return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
	Vt *t = user;

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

static int cb_bell(void *user)
{
	Vt *t = user;
	if (t->urgent_handler)
		t->urgent_handler(t);
	return 1;
}

static int cb_resize(int rows, int cols, void *user)
{
	Vt *t = user;
	t->rows = rows;
	t->cols = cols;
	t->dirty = true;
	return 1;
}

static const VTermScreenCallbacks screen_callbacks = {
	.damage      = cb_damage,
	.moverect    = cb_moverect,
	.movecursor  = cb_movecursor,
	.settermprop = cb_settermprop,
	.bell        = cb_bell,
	.resize      = cb_resize,
	.sb_pushline = sb_pushline,
	.sb_popline  = sb_popline,
	.sb_clear    = sb_clear,
};

/* ── lifecycle ────────────────────────────────────────────────────────────── */

void vt_init(void)
{
	short fg = -1, bg = -1;
	const char *cset, *term;

	/* Deliberately short locals, initialised. pair_content writes shorts:
	 * pointing it at the int32_t globals would leave their upper half
	 * untouched, turning a -1 into 65535 and every default-coloured cell into
	 * an arbitrary colour. And if pair_content fails outright, the locals must
	 * already hold something sane rather than stack garbage. */
	pair_content(0, &fg, &bg);
	default_fg = fg == -1 ? COLOR_WHITE : fg;
	default_bg = bg == -1 ? COLOR_BLACK : bg;
	has_default_colors = (use_default_colors() == OK);
	vt_color_reserve(COLOR_WHITE, COLOR_BLACK);

	cset = nl_langinfo(CODESET);
	is_utf8 = cset && !strcmp(cset, "UTF-8");

	/* The TERM children are run with. Without it they inherit whatever dvtm
	 * was started with, or nothing, and every curses program inside dvtm
	 * misbehaves -- wrong colours, and a line editor that cannot position the
	 * cursor redraws what you typed instead of moving over it. */
	if (!(term = getenv("DVTM_TERM")))
		term = "dvtm";
	snprintf(vt_term, sizeof vt_term, "%s%s", term,
	         COLORS >= 256 ? "-256color" : "");
}

void vt_shutdown(void)
{
}

void vt_keytable_set(char const * const keytable[], int count)
{
	keytable_overlay = keytable;
	keytable_overlay_len = count;
}

void vt_title_handler_set(Vt *t, vt_title_handler_t handler)
{
	t->title_handler = handler;
}

void vt_urgent_handler_set(Vt *t, vt_urgent_handler_t handler)
{
	t->urgent_handler = handler;
}

void vt_data_set(Vt *t, void *data)
{
	t->data = data;
}

void *vt_data_get(Vt *t)
{
	return t->data;
}

Vt *vt_create(int rows, int cols, int scroll_buf_sz)
{
	Vt *t;

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

	if (scroll_buf_sz > 0 && (t->sb = calloc((size_t)scroll_buf_sz, sizeof(Line))))
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

void vt_resize(Vt *t, int rows, int cols)
{
	struct winsize ws = { .ws_row = rows, .ws_col = cols };

	if (rows <= 0 || cols <= 0 || (rows == t->rows && cols == t->cols))
		return;

	vt_noscroll(t);
	vterm_set_size(t->vt, rows, cols);
	t->rows = rows;
	t->cols = cols;
	t->dirty = true;
	if (t->pty >= 0)
		ioctl(t->pty, TIOCSWINSZ, &ws);
}

void vt_destroy(Vt *t)
{
	if (!t)
		return;
	for (int i = 0; i < t->sb_count; i++)
		line_free(sb_at(t, i));
	free(t->sb);
	if (t->vt)
		vterm_free(t->vt);
	if (t->pty >= 0)
		close(t->pty);
	free(t);
}

/* ── the pty ──────────────────────────────────────────────────────────────── */

/* forkpty(3) written out; see the note in the commit that introduced it. Not
 * POSIX, lives in a different header on every system, and absent on some. */
static pid_t pty_fork(int *master, const struct winsize *ws)
{
	int mfd, sfd;
	const char *name;
	pid_t pid;

	if ((mfd = posix_openpt(O_RDWR | O_NOCTTY)) < 0)
		return -1;
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

pid_t vt_forkpty(Vt *t, const char *p, const char *argv[], const char *cwd,
                 const char *env[], int *to, int *from)
{
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

	if ((pid = pty_fork(&t->pty, &ws)) < 0)
		return -1;

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
		setenv("TERM", vt_term, 1);
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

int vt_pty_get(Vt *t)
{
	return t->pty;
}

pid_t vt_pid_get(Vt *t)
{
	return t->pid;
}

/* ── reading and writing ──────────────────────────────────────────────────── */

ssize_t vt_write(Vt *t, const char *buf, size_t len)
{
	ssize_t ret = (ssize_t)len;

	while (len > 0) {
		ssize_t res = write(t->pty, buf, len);
		if (res < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return -1;
		}
		buf += res;
		len -= (size_t)res;
	}
	return ret;
}

/* Send whatever libvterm has queued for the child: key presses and mouse
 * reports come back out this way, as the bytes a real terminal would send. */
static void vt_flush_output(Vt *t)
{
	char buf[512];
	size_t len;

	while ((len = vterm_output_read(t->vt, buf, sizeof buf)) > 0)
		vt_write(t, buf, len);
}

int vt_process(Vt *t)
{
	char buf[8192];
	ssize_t res;

	if (t->pty < 0) {
		errno = EINVAL;
		return -1;
	}
	res = read(t->pty, buf, sizeof buf);
	if (res < 0)
		return -1;
	/* End of file: the child is gone. Linux reports that as EIO, macOS and
	 * the BSDs as a zero-byte read; report it the same way everywhere. */
	if (res == 0) {
		errno = EIO;
		return -1;
	}
	vterm_input_write(t->vt, buf, (size_t)res);
	vt_flush_output(t);
	return 0;
}

/* ── keyboard ─────────────────────────────────────────────────────────────── */

/* ncurses hands back a keycode; libvterm wants to be told which key it was and
 * generates the right bytes itself, including the application-cursor variants
 * that vt.c had to special-case by hand. */
static VTermKey vterm_key_of(int keycode)
{
	switch (keycode) {
	case KEY_ENTER:     return VTERM_KEY_ENTER;
	case KEY_BACKSPACE: return VTERM_KEY_BACKSPACE;
	case KEY_UP:        return VTERM_KEY_UP;
	case KEY_DOWN:      return VTERM_KEY_DOWN;
	case KEY_LEFT:      return VTERM_KEY_LEFT;
	case KEY_RIGHT:     return VTERM_KEY_RIGHT;
	case KEY_IC:        return VTERM_KEY_INS;
	case KEY_DC:        return VTERM_KEY_DEL;
	case KEY_HOME:      return VTERM_KEY_HOME;
	case KEY_END:       return VTERM_KEY_END;
	case KEY_PPAGE:     return VTERM_KEY_PAGEUP;
	case KEY_NPAGE:     return VTERM_KEY_PAGEDOWN;
	case KEY_BTAB:      return VTERM_KEY_TAB;
	default:
		if (keycode >= KEY_F(1) && keycode <= KEY_F(12))
			return (VTermKey)(VTERM_KEY_FUNCTION_0 + (keycode - KEY_F(0)));
		return VTERM_KEY_NONE;
	}
}

void vt_keypress(Vt *t, int keycode)
{
	VTermKey key;

	vt_noscroll(t);

	/* A sequence supplied through config.h wins: it exists precisely to say
	 * something this mapping would otherwise get wrong. */
	if (keycode >= 0 && keycode < keytable_overlay_len &&
	    keytable_overlay && keytable_overlay[keycode]) {
		vt_write(t, keytable_overlay[keycode], strlen(keytable_overlay[keycode]));
		return;
	}

	if ((key = vterm_key_of(keycode)) != VTERM_KEY_NONE) {
		vterm_keyboard_key(t->vt, key, VTERM_MOD_NONE);
	} else if (keycode >= 0 && keycode <= UCHAR_MAX) {
		vterm_keyboard_unichar(t->vt, (uint32_t)keycode, VTERM_MOD_NONE);
	} else {
		return;
	}
	vt_flush_output(t);
}

void vt_mouse(Vt *t, int x, int y, mmask_t mask)
{
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

	vt_flush_output(t);
}

/* ── scrollback navigation ────────────────────────────────────────────────── */

void vt_scroll(Vt *t, int rows)
{
	int max = t->sb_count;

	if (rows < 0) {          /* towards the past */
		t->scroll = MIN(t->scroll - rows, max);
	} else {
		t->scroll = MAX(t->scroll - rows, 0);
	}
	t->dirty = true;
}

void vt_noscroll(Vt *t)
{
	if (t->scroll) {
		t->scroll = 0;
		t->dirty = true;
	}
}

int vt_content_start(Vt *t)
{
	return t->sb_count - t->scroll;
}

bool vt_cursor_visible(Vt *t)
{
	return t->scroll ? false : t->cursor_visible;
}

void vt_dirty(Vt *t)
{
	t->dirty = true;
}

/* ── painting ─────────────────────────────────────────────────────────────── */

/* The cells for one visible row: either from the scrollback, when scrolled
 * back, or from the live screen. Returns false if there is nothing there. */
static bool row_cells(Vt *t, int row, VTermScreenCell *out)
{
	int sb_row = t->sb_count - t->scroll + row;

	if (t->scroll && sb_row < t->sb_count) {
		Line *l = sb_at(t, sb_row);
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

void vt_draw(Vt *t, WINDOW *win, int srow, int scol)
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
			pair = vt_color_get(t, color_of(&cell->fg, true),
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

/* ── copy mode ────────────────────────────────────────────────────────────── */

size_t vt_content_get(Vt *t, char **buf, bool colored)
{
	int lines = t->sb_count + t->rows;
	size_t size = (size_t)lines * ((size_t)(t->cols + 1) * ((colored ? 64 : 0) + MB_CUR_MAX));
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
			Line *l = sb_at(t, i);
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
					              : sprintf(s, "\033[38;2;%d;%d;%dm",
					                        (int)(fg >> 16) & 0xff,
					                        (int)(fg >> 8) & 0xff,
					                        (int)fg & 0xff);
					prev_fg = fg;
				}
				if (bg != prev_bg) {
					s += bg == -1 ? sprintf(s, "\033[49m")
					              : sprintf(s, "\033[48;2;%d;%d;%dm",
					                        (int)(bg >> 16) & 0xff,
					                        (int)(bg >> 8) & 0xff,
					                        (int)bg & 0xff);
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

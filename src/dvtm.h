/*
 * dvtm.h - the part of dvtm.c that config.h and the layout files need.
 *
 * They are #included into dvtm.c and use its types, its globals and its
 * command functions. Without this header they parse only as fragments: opened
 * on their own, by a reader or a language server, every identifier in them is
 * undeclared.
 *
 * This is a description of what already exists, not an interface. It is
 * deliberately not a public one: the declarations below are `static`, and some
 * of them are definitions. Include it from dvtm.c, from config.h and from the
 * layout files -- nowhere else. A second translation unit including it would
 * quietly get its own copy of every global.
 */
#ifndef DVTM_H
#define DVTM_H

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>

#include <curses.h>

#include "term.h"

typedef struct {
    float mfact;
    unsigned int nmaster;
    int history;
    int w;
    int h;
    volatile sig_atomic_t need_resize;
} Screen;

typedef struct {
    const char *symbol;
    void (*arrange)(void);
} Layout;

typedef struct {
    char *data;
    size_t len;
    size_t size;
} Register;

typedef struct Client Client;
struct Client {
    WINDOW *window;
    Term *term;
    Term *editor, *app;
    int editor_fds[2];
    bool editor_died;
    /* What is still on its way to the editor, held between editoutpos and
     * editoutlen. Queued rather than written in one go: see flush_to_editor. */
    char *editout;
    size_t editoutpos, editoutlen;
    /* What this window's editor has handed back so far, kept apart from
     * copyreg until the editor is gone: one that hands back nothing leaves the
     * last copy where it was. Per window, because two windows can be in copy
     * mode at once and their answers must not interleave. */
    Register editreg;
    const char *cmd;
    char title[255];
    int order;
    pid_t pid;
    unsigned short int id;
    unsigned short int x;
    unsigned short int y;
    unsigned short int w;
    unsigned short int h;
    bool has_title_line;
    bool minimized;
    bool urgent;
    bool died;
    Client *next;
    Client *prev;
    Client *snext;
    unsigned int tags;
};

typedef struct {
    short fg;
    short bg;
    short fg256;
    short bg256;
    int pair;
} Color;

typedef struct {
    const char *title;
    attr_t attrs;
    Color *color;
} ColorRule;

#define ALT(k) ((k) + (161 - 'a'))
/* Undefined first, not guarded: some systems' <curses.h> pulls in a CTRL of
 * its own, and dvtm's key bindings must mean this one. */
#undef CTRL
#define CTRL(k) ((k) & 0x1F)
#define CTRL_ALT(k) ((k) + (129 - 'a'))

#define MAX_ARGS 8

typedef struct {
    void (*cmd)(const char *args[]);
    const char *args[3];
} Action;

#define MAX_KEYS 3

typedef unsigned int KeyCombo[MAX_KEYS];

typedef struct {
    KeyCombo keys;
    Action action;
} KeyBinding;

typedef struct {
    mmask_t mask;
    Action action;
} Button;

typedef struct {
    const char *name;
    Action action;
} Cmd;

enum { BAR_TOP, BAR_BOTTOM, BAR_OFF };

typedef struct {
    int fd;
    int pos, lastpos;
    bool autohide;
    unsigned short int h;
    unsigned short int y;
    char text[512];
    const char *file;
} StatusBar;

typedef struct {
    int fd;
    const char *file;
    unsigned short int id;
} CmdFifo;

typedef struct {
    char *name;
    const char *argv[4];
    bool filter;
    bool color;
} Editor;

#define LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define TAGMASK ((1 << LENGTH(tags)) - 1)

static void eprint(const char *errstr, ...);

/* The one build-time knob left, and a standard one: a debug build traces the
 * event loop on stderr. Redirect it -- the messages land on the terminal dvtm
 * is drawing on. `args...` would be a GCC extension; __VA_ARGS__ is C99. */
#ifdef NDEBUG
#define debug(...) ((void)0)
#else
#define debug(...) eprint(__VA_ARGS__)
#endif

/* commands for use by keybindings */
static void create(const char *args[]);
static void copymode(const char *args[]);
static void pagemode(const char *args[]);
static void focusn(const char *args[]);
static void focusid(const char *args[]);
static void focusnext(const char *args[]);
static void focusnextnm(const char *args[]);
static void focusprev(const char *args[]);
static void focuslast(const char *args[]);
static void focusup(const char *args[]);
static void focusdown(const char *args[]);
static void focusleft(const char *args[]);
static void focusright(const char *args[]);
static void killclient(const char *args[]);
static void paste(const char *args[]);
static void quit(const char *args[]);
static void redraw(const char *args[]);
static void scrollback(const char *args[]);
static void sendkeys(const char *args[]);
static void setlayout(const char *args[]);
static void incnmaster(const char *args[]);
static void setmfact(const char *args[]);
static void startup(const char *args[]);
static void tag(const char *args[]);
static void tagid(const char *args[]);
static void togglebar(const char *args[]);
static void togglebarpos(const char *args[]);
static void toggleminimize(const char *args[]);
static void togglemouse(const char *args[]);
static void togglerunall(const char *args[]);
static void toggletag(const char *args[]);
static void toggleview(const char *args[]);
static void viewprevtag(const char *args[]);
static void view(const char *args[]);
static void zoom(const char *args[]);

/* commands for use by mouse bindings */
static void mouse_focus(const char *args[]);
static void mouse_fullscreen(const char *args[]);
static void mouse_minimize(const char *args[]);
static void mouse_zoom(const char *args[]);

/* functions and variables available to layouts via config.h */
static Client *nextvisible(Client *c);
static void focus(Client *c);
static void resize(Client *c, int x, int y, int w, int h);
extern Screen screen;
static unsigned int waw, wah, wax, way;
static Client *clients = NULL;
static char *title;
static KeyCombo keys;

#endif /* DVTM_H */

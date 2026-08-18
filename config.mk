# Customize below to fit your system

PREFIX ?= /usr/local
MANPREFIX = ${PREFIX}/share/man
# specify your systems terminfo directory
# leave empty to install into your home folder
TERMINFO := ${DESTDIR}${PREFIX}/share/terminfo

# Truecolor needs ncursesw >= 6.1 built with --enable-ext-colors: alloc_pair()
# and the extended pair argument of wcolor_set(). Ask the library's own *-config
# first, since the ncurses shipped in macOS is 6.0 and lacks both.
NCURSES_CONFIG ?= $(shell command -v ncursesw6-config || command -v ncurses6-config || echo false)
NCURSES_CFLAGS ?= $(shell ${NCURSES_CONFIG} --cflags 2>/dev/null)
NCURSES_LIBS ?= $(shell ${NCURSES_CONFIG} --libs 2>/dev/null || echo -lncursesw)

INCS = -I. ${NCURSES_CFLAGS}
LIBS = -lc -lutil ${NCURSES_LIBS}
# _DARWIN_C_SOURCE brings back SIGWINCH, which _POSIX_C_SOURCE hides on macOS;
# every other system just ignores the macro.
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_XOPEN_SOURCE_EXTENDED -D_DARWIN_C_SOURCE
CFLAGS += -std=c99 ${INCS} -DNDEBUG ${CPPFLAGS}

CC ?= cc

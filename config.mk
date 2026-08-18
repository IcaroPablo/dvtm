# Customize below to fit your system

PREFIX ?= ${HOME}/.local
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

# libvterm ships no *-config script, only a pkg-config file, and pkg-config is
# not everywhere. Rather than take on that tool, look for the header in the
# usual prefixes and use whichever one actually has it. This asks the file
# system, not the operating system, so there is nothing here to keep in sync
# with a list of platforms: a prefix that does not exist simply does not match.
#
# Both variables are ?=, so the environment still wins:
#   make VTERM_CFLAGS=-I/somewhere/include VTERM_LIBS='-L/somewhere/lib -lvterm'
VTERM_PREFIX := $(patsubst %/include/vterm.h,%,$(firstword $(wildcard \
	/opt/homebrew/include/vterm.h \
	/opt/local/include/vterm.h \
	/usr/local/include/vterm.h \
	/usr/include/vterm.h)))
VTERM_CFLAGS ?= $(if ${VTERM_PREFIX},-I${VTERM_PREFIX}/include)
VTERM_LIBS ?= $(if ${VTERM_PREFIX},-L${VTERM_PREFIX}/lib )-lvterm

INCS = -I. ${NCURSES_CFLAGS} ${VTERM_CFLAGS}
# -lutil is gone with forkpty(3): it was the only thing here that needed it.
LIBS = -lc ${NCURSES_LIBS} ${VTERM_LIBS}
# _DARWIN_C_SOURCE brings back SIGWINCH, which _POSIX_C_SOURCE hides on macOS;
# every other system just ignores the macro.
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_XOPEN_SOURCE_EXTENDED -D_DARWIN_C_SOURCE
# Warn by default. Without this the normal build passes no -W flags at all and
# a new warning goes unseen until someone happens to run `make debug`.
# -Wno-unused-parameter because every key binding takes an args[] it ignores.
WARNINGS = -Wall -Wextra -Wno-unused-parameter

CFLAGS += -std=c99 ${INCS} -DNDEBUG ${WARNINGS} ${CPPFLAGS}

CC ?= cc

# GNU make. config.mk searches the file system for ncurses and libvterm, and
# that needs $(shell), $(wildcard) and $(firstword) -- GNU extensions. On a
# system whose `make` is the BSD one, build with `gmake`.
include config.mk

SRC = src/dvtm.c src/term.c src/ui.c
HDR = src/dvtm.h src/term.h $(wildcard src/layouts/*.h)
# dvtm is built here; the other three are scripts and are installed as they are.
BIN = dvtm scripts/dvtm-status scripts/dvtm-editor scripts/dvtm-pager
MANUALS = man/dvtm.1 man/dvtm-editor.1 man/dvtm-pager.1

VERSION = $(shell git describe --always --dirty 2>/dev/null || echo "0.15-git")
DVTM_CFLAGS += -DVERSION=\"${VERSION}\"
# -pedantic here and not in the normal build: the tree is meant to be plain C99
# so that it is not tied to one compiler, and this is what says so out loud.
# Keeping it out of the normal build means a strict-mode complaint from some
# system header can never stop a user from compiling dvtm.
DEBUG_CFLAGS = ${DVTM_CFLAGS} -UNDEBUG -O0 -g -pedantic

all: dvtm

config.h:
	cp config.def.h config.h

dvtm: config.h config.mk config.def.h ${SRC} ${HDR}
	${CC} ${DVTM_CFLAGS} ${SRC} ${LDFLAGS} ${LIBS} -o $@

# clang-format is not reliably on PATH: macOS keeps it inside the Command Line
# Tools and Homebrew keeps llvm off PATH entirely. Same search as config.mk does
# for ncurses -- ask the file system, do not name an operating system.
CLANG_FORMAT ?= $(firstword $(shell command -v clang-format) \
	$(wildcard /Library/Developer/CommandLineTools/usr/bin/clang-format \
		/opt/homebrew/opt/llvm/bin/clang-format \
		/usr/local/opt/llvm/bin/clang-format) \
	clang-format)

# config.def.h is in the list and opts out from inside, so that a copy of it
# living as somebody's config.h keeps opting out too. config.h itself is never
# touched: it belongs to whoever wrote it.
format:
	@echo "formatting with $$(${CLANG_FORMAT} --version)"
	@${CLANG_FORMAT} -i ${SRC} ${HDR} config.def.h tests/run.c tests/probe.c

man:
	@for m in ${MANUALS}; do \
		echo "Generating $$m"; \
		sed -e "s/VERSION/${VERSION}/" "$$m" | mandoc -W warning -T utf8 -T xhtml -O man=%N.%S.html -O style=mandoc.css 1> "$$m.html" || true; \
	done

debug: clean
	@$(MAKE) DVTM_CFLAGS='${DEBUG_CFLAGS}'

test: dvtm tests/run tests/probe
	@tests/run ./dvtm

tests/run: tests/run.c
	${CC} ${DVTM_CFLAGS} $< ${LDFLAGS} ${VTERM_LIBS} -o $@

tests/probe: tests/probe.c
	${CC} ${DVTM_CFLAGS} $< ${LDFLAGS} -o $@

clean:
	@echo cleaning
	@rm -f dvtm
	@rm -f tests/run tests/probe
	@rm -rf tests/terminfo
	@rm -rf *.dSYM

dist: clean
	@echo creating dist tarball
	@git archive --prefix=dvtm-${VERSION}/ -o dvtm-${VERSION}.tar.gz HEAD

# The rm before each cp is not tidiness. Overwriting an executable in place
# leaves macOS holding a code signature that no longer matches the bytes, and
# the kernel kills the process the moment it is run -- no output, no error,
# exit 137. Unlinking first means the copy lands on a fresh inode with a fresh
# signature. Unlinking is also safe for a copy that is currently running: the
# running process keeps the old inode until it exits.
install: all
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@for b in ${BIN}; do \
		n=$$(basename "$$b"); \
		echo "installing ${DESTDIR}${PREFIX}/bin/$$n"; \
		rm -f "${DESTDIR}${PREFIX}/bin/$$n" && \
		cp "$$b" "${DESTDIR}${PREFIX}/bin/$$n" && \
		chmod 755 "${DESTDIR}${PREFIX}/bin/$$n"; \
	done
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
	@for m in ${MANUALS}; do \
		n=$$(basename "$$m"); \
		sed -e "s/VERSION/${VERSION}/" < "$$m" >  "${DESTDIR}${MANPREFIX}/man1/$$n" && \
		chmod 644 "${DESTDIR}${MANPREFIX}/man1/$$n"; \
	done
	@echo installing terminfo description
# -x keeps the user-defined capabilities. Without it tic silently drops Tc,
# setrgbf and setrgbb, and every child sees a plain 256-color terminal.
	@TERMINFO=${TERMINFO} tic -x -s dvtm.info

uninstall:
	@for b in ${BIN}; do \
		n=$$(basename "$$b"); \
		echo "removing ${DESTDIR}${PREFIX}/bin/$$n"; \
		rm -f "${DESTDIR}${PREFIX}/bin/$$n"; \
	done
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/dvtm.1

.PHONY: all clean dist install uninstall debug test format

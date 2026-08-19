include config.mk

SRC = dvtm.c term.c ui.c
BIN = dvtm dvtm-status dvtm-editor dvtm-pager
MANUALS = dvtm.1 dvtm-editor.1 dvtm-pager.1

VERSION = $(shell git describe --always --dirty 2>/dev/null || echo "0.15-git")
CFLAGS += -DVERSION=\"${VERSION}\"
DEBUG_CFLAGS = ${CFLAGS} -UNDEBUG -O0 -g -ggdb -Wall -Wextra -Wno-unused-parameter

all: dvtm dvtm-editor

config.h:
	cp config.def.h config.h

dvtm: config.h config.mk *.c *.h
	${CC} ${CFLAGS} ${SRC} ${LDFLAGS} ${LIBS} -o $@

dvtm-editor: dvtm-editor.c
	${CC} ${CFLAGS} $^ ${LDFLAGS} -o $@

man:
	@for m in ${MANUALS}; do \
		echo "Generating $$m"; \
		sed -e "s/VERSION/${VERSION}/" "$$m" | mandoc -W warning -T utf8 -T xhtml -O man=%N.%S.html -O style=mandoc.css 1> "$$m.html" || true; \
	done

debug: clean
	@$(MAKE) CFLAGS='${DEBUG_CFLAGS}'

test: dvtm tests/run tests/probe
	@tests/run ./dvtm

tests/run: tests/run.c
	${CC} ${CFLAGS} ${VTERM_CFLAGS} $< ${LDFLAGS} ${VTERM_LIBS} -o $@

tests/probe: tests/probe.c
	${CC} ${CFLAGS} $< ${LDFLAGS} -o $@

clean:
	@echo cleaning
	@rm -f dvtm
	@rm -f dvtm-editor
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
		echo "installing ${DESTDIR}${PREFIX}/bin/$$b"; \
		rm -f "${DESTDIR}${PREFIX}/bin/$$b" && \
		cp "$$b" "${DESTDIR}${PREFIX}/bin/$$b" && \
		chmod 755 "${DESTDIR}${PREFIX}/bin/$$b"; \
	done
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
	@for m in ${MANUALS}; do \
		sed -e "s/VERSION/${VERSION}/" < "$$m" >  "${DESTDIR}${MANPREFIX}/man1/$$m" && \
		chmod 644 "${DESTDIR}${MANPREFIX}/man1/$$m"; \
	done
	@echo installing terminfo description
	@TERMINFO=${TERMINFO} tic -x -s dvtm.info

uninstall:
	@for b in ${BIN}; do \
		echo "removing ${DESTDIR}${PREFIX}/bin/$$b"; \
		rm -f "${DESTDIR}${PREFIX}/bin/$$b"; \
	done
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/dvtm.1

.PHONY: all clean dist install uninstall debug test

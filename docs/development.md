# Development

The README states the goals and non-goals. This is the reasoning behind them,
the shape of the tree, and the things you need to know before changing it.

## Design

dvtm follows the [Unix
philosophy](http://www.catb.org/esr/writings/taoup/html/ch01s06.html): do one
thing — *dynamic* window management on the console — and delegate the rest.

Session management is [abduco](https://www.brain-dump.org/projects/abduco/)'s
job, not dvtm's. Selecting text in copy mode is the editor's job. Emulating a
terminal is libvterm's job, which is what this fork changed. What is left is
about 3300 lines of C, small enough to read in an afternoon and hack on.

That delegation is the whole design argument. The host terminal is already an
emulator; a second one inside dvtm meant every byte was parsed twice, stored
twice and re-serialised in between, and every terminal feature had to be
written twice. Removing it removed half the program and most of the defects
with it — see [changes.md](changes.md).

## Where things are

    Makefile  config.mk       the build
    config.def.h              what you edit, copied to config.h on first build
    dvtm.info                 the terminfo description installed for the windows
    src/                      dvtm.c, term.c, ui.c and their headers
    src/layouts/              one file per way of arranging windows
    scripts/                  dvtm-editor, dvtm-pager, dvtm-status: installed,
                              never compiled
    man/                      the three manual pages
    tests/                    the suite `make test` runs

The three source files divide by what they talk to. `term.c` owns one child:
its pty, its libvterm instance and its scrollback. `ui.c` owns everything that
puts cells on a screen and the ncurses colour pairs. `dvtm.c` is the window
manager above both.

Sources are under `src/` and the compiler is told about it with `-I`, so
`#include "dvtm.h"` and `#include "tile.h"` still resolve — those exact lines
live in every custom `config.h` and moving the files was not a reason to break
them.

## Running the suite

    make test

It needs no `make install` first. The suite drives the real binary on a pty and
asserts on what it paints, on two layers: raw bytes for protocol claims ("DSR 5
is answered with `ESC[0n`"), and a screen model for picture claims ("the window
is gone"). Nothing sleeps — every wait is on an observable with a deadline.

It measures this tree and not what the machine happens to have installed. It
compiles `dvtm.info` into `tests/terminfo` itself, puts the build directory
first on `PATH` so copy mode runs the `dvtm-editor` just built, and supplies its
own stand-in editor.

Checks it deliberately does not make are printed as `SKIP` with the reason,
rather than hidden in a comment: a silently deleted assertion is
indistinguishable from one nobody thought of.

A green run is not proof the program works. Three real drawing bugs — the
colour pair passed as a `short` where ncurses wants an `int`, `TERM` never set
for children, the cursor never repositioned — all survived a fully green suite
and were found by running a real shell inside dvtm and reading the bytes it
emitted. After touching drawing code, do that too.

## Limitations

Everything below is either a declared dependency or something every Unix has had
for decades, with one exception that is a real limitation. Listing them beats
letting someone find out on a machine where one is missing.

  * **GNU make**, for the library search in `config.mk`. It finds ncurses and
    libvterm by searching the file system, which needs `$(shell)`,
    `$(wildcard)` and `$(firstword)`. Where the system `make` is the BSD one,
    build with `gmake`.
  * **curses**, all of it. POSIX does not standardise curses — that is X/Open
    Curses, a separate standard — and dvtm goes past even that: `alloc_pair`,
    `use_default_colors`, `A_ITALIC`, the extended-pair form of `wcolor_set`,
    `set_escdelay`, `resizeterm`, `wresize`, `mousemask` and `getmouse` are
    ncurses extensions. This is why ncursesw is required by version rather than
    "some curses".
  * **libvterm**, obviously.
  * **`ioctl` with `TIOCGWINSZ`, `TIOCSWINSZ` and `TIOCSCTTY`.** POSIX had no
    window-size call at all until its 2024 revision, and still has no way to
    claim a controlling terminal. Every Unix spells these the same; the standard
    simply never covered them.
  * **`SIGWINCH`**, which POSIX does not define either, and which every Unix has.
  * **`Tc`, `setrgbf` and `setrgbb` in `dvtm.info`** — user-defined terminfo
    capabilities, which is why `make install` runs `tic -x`. Without the `-x`
    they are silently dropped and the windows lose 24-bit colour.
  * **`mktemp(1)`**, which every system has and no standard describes. It is
    there so the file `dvtm-editor` hands the editor cannot be guessed by
    something else in the temporary directory first.
  * **`/proc/<pid>/cwd`**, which is Linux and only Linux. This is the exception:
    it is a limitation rather than a dependency, and it is under *Limitations*
    in the README.

What is *not* on that list is worth saying too, because it was checked rather
than assumed: the C is C99 and compiles silently under `-pedantic` on both clang
and gcc; no glibc or BSD libc extension is used anywhere — `memmem` appears once,
in a comment in `tests/run.c` explaining why the suite hand-rolls the search
instead; and `dvtm-status`, `dvtm-pager` and `tests/editor` are `/bin/sh` with no
bashisms in them.

There is deliberately no list of supported systems — such a list goes stale, and
keeping one true is what invites back the conditionals this fork spent its time
deleting. What there is instead is a record of what has actually been compiled:
macOS with clang, Linux with gcc against both glibc and musl, and OpenBSD. A
system that provides the dependencies and is not in that sentence has not
failed; it is one nobody has tried yet.

## House style

It is in `.clang-format`, and `make format` applies it. Braces on the same line,
four spaces to indent, no tab characters, eighty columns.

Two things opt out on purpose: comments are never rewrapped, because they are
hand-wrapped prose whose paragraphs a formatter would turn into unreadable
diffs, and `config.def.h` is skipped entirely — its tables are aligned into
columns by hand and that alignment is how they are read.

`clang-format` is found even when it is off `PATH`, as it is inside the macOS
Command Line Tools; override with `make format CLANG_FORMAT=/path/to/it`.

`make debug` turns on `-pedantic` and the build is expected to be silent.

## Upstream

This fork lives at [IcaroPablo/dvtm](https://github.com/IcaroPablo/dvtm).
Upstream is at [Github](https://github.com/martanne/dvtm) and
[Sourcehut](https://git.sr.ht/~martanne/dvtm), and takes patches through the
[suckless developer mailing list](https://suckless.org/community).

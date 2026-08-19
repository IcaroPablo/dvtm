# What this fork changes

A fork of [martanne/dvtm](https://github.com/martanne/dvtm), whose last release
was 0.15 in 2016. The README carries the short version; this is the whole of it.

**The big one: the terminal emulator is gone.** `vt.c` was half of dvtm — an
escape-code parser, a cell grid and a scrollback. The host terminal is already
an emulator, so every byte a program wrote was parsed here, stored,
re-serialised and parsed again upstairs: two parsers and two screen models for
the same byte, and every terminal feature written twice. That half is where the
defects lived. It now sits on
[libvterm](https://www.leonerd.org.uk/code/libvterm/), the same emulator neovim
uses.

    before   dvtm.c 1992 + vt.c 1972 + vt.h 67                     = 4031 lines
    after    dvtm.c 1827 + term.c 639 + ui.c 304 + headers 332     = 3102 lines

## Fixed

  * dvtm froze completely the first time any signal woke it. The pipe it uses
    to wake its own main loop was drained with a loop that never stops on an
    empty pipe, so it stopped responding to the keyboard and to every window.
  * Keystrokes were lost when they arrived together — typing quickly, pasting,
    or any key combination sent in one go. Only the first key of each batch was
    read.
  * Typing any character above `U+007F` sent the wrong bytes to the program in
    the window. The keyboard was read one byte at a time and each byte was
    passed on as if it were a code point, so `á` arrived as `Ã¡`. Painting was
    never affected, which is why it survived so long: only the child could tell.
  * A window whose program had exited stayed on screen forever on macOS and the
    BSDs. Neither of the two ways dvtm detects this worked there.
  * `make install` could leave a binary that was killed the instant it ran, with
    no output and no error, because it overwrote executables in place.
  * 24-bit colour was not supported.
  * All colour was lost on any terminal without direct colour — which is most
    of them. dvtm sent every colour as a 24-bit value whatever the terminal
    said it could take, the values were rejected as out of range, and each cell
    was painted in the default colour: an editor inside dvtm showed no syntax
    highlighting at all. Colours are now folded to what the terminal actually
    has, and the suite checks four palette sizes instead of one.
  * Everything dvtm painted itself — the tag numbers, the window borders, the
    titles of unfocused windows — came out black on black on a direct-colour
    terminal, and the bar and borders sat on a hard black background rather than
    the terminal's own. All of that is `COLOR(DEFAULT)`, meaning "whatever this
    terminal calls its default", and dvtm resolved it by reading colour pair 0
    instead of letting ncurses carry the default through. On a direct-colour
    terminal a colour number is an rgb value, so pair 0 reads as 0, which is
    black. Nothing was missing from the screen; it was painted in the darkest
    colour the terminal has.
  * `DSR 5`, a program asking whether the terminal is alive, went unanswered.
  * Programs inside dvtm were run with no `TERM` set at one point during the
    port; they now always get a working terminfo entry.

## Improved

  * Builds and runs on macOS, using ncursesw 6.1+ rather than the 6.0 the
    system ships. AIX, SunOS and Cygwin support was dropped — it was the source
    of most of the per-platform conditionals, of which there are now none.
  * `forkpty(3)`, which is not POSIX and lives in a different header on every
    system, is gone; the pty is opened with POSIX calls. Two hand-written
    replacements for platforms that lacked it were deleted with it.
  * The source is plain C99, checked under `-pedantic`, instead of quietly
    relying on gcc and clang extensions.
  * `make CFLAGS=...` no longer breaks the build. The build's own flags shared
    that variable, so setting it — which every ports tree and package build
    does — deleted the include paths with everything else, and the compile
    stopped at `'vterm.h' file not found`.
  * The build finds ncurses even when the package manager keeps it off `PATH`,
    as Homebrew does. Before, it quietly fell back to the ncurses shipped in
    macOS and stopped at `alloc_pair`.
  * Installs to `~/.local` by default, so no administrator rights are needed.
  * `make test` runs a suite that drives the real binary on a terminal and
    checks what it paints, including cases for every bug above. It measures
    this tree and not what the machine happens to have installed — it used to
    run whichever `dvtm-editor` was on `PATH` and to assert on a status line
    that only some editors print.
  * Warnings are on by default and the build is clean on every system it has
    been compiled on.
  * `dvtm-editor` is a shell script rather than 140 lines of C, which is what
    `dvtm-pager` beside it always was. One behaviour changed with it: it decides
    "you quit without saving" by comparing the content rather than the
    modification time, which is what the C compared — `st_mtime` is whole
    seconds, so a save made inside the same second as the write was missed.
  * `bstack` and `tstack` were the same fifty lines twice, differing in which
    edge the master area sits on. They are one layout with two names in
    `stack.h` now.
  * Every file parses on its own. `config.h` and the layouts used to be
    fragments that only made sense pasted into `dvtm.c`; they now include
    `dvtm.h`, which carries the declarations they need, and the layouts are
    named `.h` to match what they are.
  * The manual page matches the program again. `Mod-E` and `DVTM_PAGER` were
    undocumented, `Mod-/` was described as opening an editor when it opens a
    pager, and the entry for `DVTM_EDITOR` broke off mid-sentence.
  * ncursesw 6.1+ is simply required, so the conditionals that used to stand in
    for other curses libraries are gone. Mouse support is always compiled in
    and switched at runtime.

## Bringing a `config.h` over from upstream dvtm

Three things changed:

  * Add `#include "dvtm.h"` at the top, and change layout includes from
    `#include "tile.c"` to `#include "tile.h"`. The layouts were renamed to
    match what they are — files that are only ever `#include`d.
  * Change `#include "bstack.h"` to `#include "stack.h"`. `bstack` and `tstack`
    were the same fifty lines twice over, differing in which edge the masters
    sit on, and are now one layout with two names in one file. Both names still
    work in `layouts[]`; only the include moved.
  * Delete the `#ifdef CONFIG_MOUSE` around `buttons[]`. That compile-time
    switch no longer exists; mouse support is always built in, and is turned
    off at runtime with `ENABLE_MOUSE` in `config.h`, the `-M` flag, or
    `MOD+M`.

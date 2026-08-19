# dvtm — dynamic virtual terminal manager

dvtm brings tiling window management, popularised by X11 window managers like
[dwm](https://dwm.suckless.org), to the console. It runs several terminal
programs at once inside one terminal and arranges them for you.

![abduco+dvtm demo](https://raw.githubusercontent.com/martanne/dvtm/gh-pages/screencast.gif#center)

This is a fork of [martanne/dvtm](https://github.com/martanne/dvtm), whose last
release was 0.15 in 2016. The one-line difference: **the terminal emulator
inside dvtm is gone**, and libvterm does that work now.

## What this fork changes

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

**Fixed**

  * dvtm froze completely the first time any signal woke it. The pipe it uses
    to wake its own main loop was drained with a loop that never stops on an
    empty pipe, so it stopped responding to the keyboard and to every window.
  * Keystrokes were lost when they arrived together — typing quickly, pasting,
    or any key combination sent in one go. Only the first key of each batch was
    read.
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

**Improved**

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
    `stack.h` now; see *Configuring* if you have your own `config.h`.
  * Every file parses on its own. `config.h` and the layouts used to be
    fragments that only made sense pasted into `dvtm.c`; they now include
    `dvtm.h`, which carries the declarations they need, and the layouts are
    named `.h` to match what they are. See *Configuring* if you have your own
    `config.h`.
  * The manual page matches the program again. `Mod-E` and `DVTM_PAGER` were
    undocumented, `Mod-/` was described as opening an editor when it opens a
    pager, and the entry for `DVTM_EDITOR` broke off mid-sentence.
  * ncursesw 6.1+ is simply required, so the conditionals that used to stand in
    for other curses libraries are gone. Mouse support is always compiled in
    and switched at runtime.

**Known problems**

  * `OSC 11`, a program asking for the background colour, is unanswered.
    libvterm does not answer it either — it cannot know the real terminal's
    colours — so it has to be implemented here if it is wanted.
  * Colours written as `38:2::r:g:b` are read wrongly. That is the
    standards-conformant spelling, with an empty colourspace field, and
    libvterm 0.3.3 drops the empty field and shifts the values. It is that
    field alone: of the three spellings a program can choose, only the middle
    one breaks.

        38;2;10;200;30     -> rgb( 10,200, 30)
        38:2::10:200:30    -> rgb(255, 10,200)
        38:2:10:200:30     -> rgb( 10,200, 30)

    Programs that ask terminfo how to set a colour are unaffected, because the
    terminfo installed here uses `38;2;r;g;b`; only programs that hardcode the
    full colon spelling see wrong colours. `make test` reports this as a
    skipped check on every run. There is nothing to upgrade to: 0.3.3 is the
    newest release that exists, and Debian, Arch, Fedora and Guix all ship it.
  * **Faint text is painted at full brightness.** `SGR 2`, which programs use
    for text meant to sit back from the rest, is lost. libvterm 0.3.3 has no
    way to carry it: `VTermScreenCellAttrs` has bits for bold, underline,
    italic, blink, reverse, conceal, strike and several more, and none for
    faint. The cell libvterm hands back for `ESC[2m` is byte for byte the cell
    it hands back for unstyled text, so dvtm never learns the distinction and
    has nothing to pass on to `A_DIM`. It is visible wherever a program dims
    something rather than colouring it: secondary lines in a banner, the
    greyed-out suggestion under a prompt. `make test` reports this as a
    skipped check on every run. There is nowhere to move to: the attribute is
    absent from libvterm's own tree and from the fork neovim keeps, which is
    the most actively maintained one there is. Recovering it means either
    teaching some libvterm to model faint, or dvtm parsing SGR alongside
    libvterm and keeping its own grid — which is the emulator this fork
    deleted, back again.
  * **`Mod-C` only works on Linux, and says nothing anywhere else.** It is meant
    to open a window in the working directory of the focused one, and copy mode
    is meant to start the editor there too. Both ask `/proc/<pid>/cwd`, which
    exists on Linux and nowhere else; the lookup returns nothing, and both fall
    back to dvtm's own directory. So on macOS and the BSDs `Mod-C` is `Mod-c`.
    Reading another process's working directory has no portable answer:
    `libproc` on one system, `sysctl` on another, nothing at all on a third —
    per-operating-system code, which is what this fork spent its time removing.
    Left as it is, and written down here rather than discovered.

## Building and installing

Dependencies:

  * A C99 compiler. The source is plain C99 and compiles clean under
    `-pedantic`, which `make debug` turns on. The flags in `config.mk` are the
    gcc and clang spellings; a compiler that spells them differently is told so
    from the command line, not by editing the file:
    `make STD=-xc99 WARNINGS=`.
  * GNU make. `config.mk` finds ncurses and libvterm by searching the file
    system, which needs `$(shell)`, `$(wildcard)` and `$(firstword)`. Where the
    system `make` is the BSD one, build with `gmake`.
  * `ncursesw` >= 6.1, built with `--enable-ext-colors` — `alloc_pair()` and the
    extended pair argument of `wcolor_set()` are what 24-bit colour needs, and
    without it ncurses allows only 255 colour pairs at once.
  * `libvterm` >= 0.3. It parses everything the programs inside dvtm write.
  * `tic`, from that same ncurses, to compile `dvtm.info` at install time.
  * `mktemp(1)`, used by `dvtm-editor` for the buffer it hands the editor.

Nothing else. No `#ifdef` in the source names an operating system, and no `-l`
flag is there for one libc. Any Unix providing those five should build it.
There is deliberately no list of supported systems — such a list goes stale, and
keeping one true is what invites back the conditionals this fork spent its time
deleting.

What there is instead is a record of what has actually been compiled: macOS
with clang, Linux with gcc against both glibc and musl, and OpenBSD. A system
that provides the five and is not in that sentence has not failed; it is one
nobody has tried yet.

### What here is not POSIX

Everything below is either a declared dependency or something every Unix has had
for decades, with one exception that is a real limitation. Listing them beats
letting someone find out on a machine where one is missing.

  * **GNU make**, for the library search in `config.mk`, as above.
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
    it is a limitation rather than a dependency, and it is under *Known
    problems* above.

What is *not* on that list is worth saying too, because it was checked rather
than assumed: the C is C99 and compiles silently under `-pedantic` on both clang
and gcc; no glibc or BSD libc extension is used anywhere — `memmem` appears once,
in a comment in `tests/run.c` explaining why the suite hand-rolls the search
instead; and `dvtm-status`, `dvtm-pager` and `tests/editor` are `/bin/sh` with no
bashisms in them.

Then:

    make && make install

which installs to `~/.local`; pass `PREFIX=/usr/local` or similar for anywhere
else. To run the test suite, which needs no `make install` first:

    make test

`CFLAGS` is yours: what you pass is appended after the build's own flags, so it
wins on anything repeatable and cannot delete the include paths.

    make CFLAGS='-O2 -march=native'

Note that `make` prints `Nothing to be done for 'all'` when the binaries are
already newer than the sources. That is not a failure — touch a source, or run
`make clean`, if you want to force a rebuild.

### If the build stops at `'vterm.h' file not found`

That is not a missing install; it means the build could not work out **where**
libvterm is. `config.mk` looks for the header under `/opt/homebrew`,
`/opt/local`, `/usr/local` and `/usr`, and uses whichever prefix actually has
it. If yours is somewhere else, say so:

    make VTERM_CFLAGS=-I/your/prefix/include \
         VTERM_LIBS='-L/your/prefix/lib -lvterm'

or export the same two variables. Both override the search, so nothing in
`config.mk` needs editing. To see what it decided, ask make to show the command
without running it:

    make -n dvtm | tr ' ' '\n' | grep vterm

## Using dvtm

Every dvtm key binding starts with a modifier, written `MOD` here. By default
it is `CTRL+g`; `dvtm -m ^b` changes it at startup.

### Windows

New windows are created with `MOD+c` and closed with `MOD+x x`. Switch between
them with `MOD+j` and `MOD+k`, or jump straight to one with `MOD+[1..9]` — the
digit is the window number shown in the title bar. `MOD+.` minimises and
restores a window, and `MOD+a` sends what you type to every visible window at
once until you press it again. `MOD+q q` quits dvtm.

`Shift+PageUp` and `Shift+PageDown` scroll back through a window's history. How
much history is kept is set with `dvtm -h lines`.

### Layouts

Visible windows are arranged by a layout, which divides the screen into a
master area and a stack. The master area is the large one, for whatever you are
working on. `MOD+h` and `MOD+l` shrink and grow it, `MOD+Enter` moves a window
into it, and `MOD+i` and `MOD+d` change how many windows it holds.

`MOD+Space` cycles through the four layouts enabled by default, each of which
also has a key of its own:

 * `MOD+f` vertical stack: master area on the left half, the rest on the right
 * `MOD+g` grid: every window gets an equally sized portion of the screen
 * `MOD+b` bottom stack: master area on the top half, the rest stacked below
 * `MOD+m` fullscreen: only the selected window is shown

More layouts ship with the source and are switched on in `config.h`.

### Tagging

Each window carries a non-empty set of tags `[1..n]`, and a view is a set of
tags: the current view shows every window carrying one of the active tags.

- `MOD+0` view all windows with any tag
- `MOD+v Tab` toggle back to the previously selected tags
- `MOD+v [1..n]` view all windows with the nth tag
- `MOD+V [1..n]` add/remove all windows with the nth tag to/from the view
- `MOD+t [1..n]` apply the nth tag to the focused window
- `MOD+T [1..n]` add/remove the nth tag to/from the focused window

### Status bar

Started with `dvtm -s fifo`, dvtm reads status messages from a named pipe and
displays them. The
`scripts/dvtm-status` is provided as an example; it shows the current time. `MOD+s` hides and
shows the bar, `MOD+S` moves it between the top and the bottom.

### Copy mode

`MOD+e` hands the window's whole scrollback to an editor. Whatever the editor
writes back is remembered by dvtm and can be pasted into any window with
`MOD+p` — so the selecting is done in the editor, with whatever search and
regex it has, instead of in dvtm.

`MOD+E` sends the same text to a pager instead, which only displays it, and
`MOD+/` opens the pager already searching forward.

The editor is `$DVTM_EDITOR`, then `$VISUAL`, then `$EDITOR`, then `vi`. The
pager is `$DVTM_PAGER`, then `$PAGER`, then `less`. Any full-screen editor
works: the text is handed over in a temporary file and the editor gets the
terminal to itself.

`dvtm(1)` lists every binding, including the ones not mentioned here.

## Configuring

dvtm is configured by editing `config.h` and recompiling — `config.def.h` is
copied to `config.h` on the first build, and is the example to work from. It
defines the layouts, the key bindings and the colour rules, with macros for the
common cases.

If you are bringing a `config.h` over from upstream dvtm, two things changed:

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

## FAQ

### Detach and reattach

dvtm has no session support. Use
[abduco](https://www.brain-dump.org/projects/abduco/):

    $ abduco -c dvtm-session

Detach with `CTRL+\` and reattach later with

    $ abduco -a dvtm-session

### Copy and paste does not work under X

dvtm grabs mouse events by default, which takes them away from the terminal's
own selection. Hold `Shift` while selecting and pasting, or give up the mouse
bindings: start with `dvtm -M`, toggle with `MOD+M`, or set `ENABLE_MOUSE` to
false in `config.h`.

### WARNING: terminal is not fully functional

The `dvtm.info` terminfo description is not installed. `make install` does it;
by hand it is `tic -x -s dvtm.info`, and the `-x` is not optional — without it
`tic` silently drops the capabilities that carry 24-bit colour.

If you cannot install terminfo descriptions at all, name a terminal the system
already knows and dvtm will use it for its windows:

    $ DVTM_TERM=rxvt dvtm

### How do I set the window title?

With the [xterm escape
sequence](https://tldp.org/HOWTO/Xterm-Title-3.html#ss3.2):

    $ printf '\033]0;Your title here\007'

In `bash`, to keep the working directory in the title:

    # If this is an xterm set the title to user@host:dir
    case "$TERM" in
    dvtm*|xterm*|rxvt*)
        PROMPT_COMMAND='printf "\033]0;${USER}@${HOSTNAME}: ${PWD/$HOME/~}\007"'
        ;;
    esac

Other shells have an equivalent; zsh has
[precmd](http://zsh.sourceforge.net/Doc/Release/Functions.html#Hook-Functions).

### Colours look close, but not right

Programs inside dvtm look brighter or more saturated than they should, or an
editor pane sits on a background a shade off from the shell beside it. Nothing
is broken: dvtm is approximating, because your terminal told it to.

ncurses sends 24-bit colour only when the terminal description carries the
**`RGB`** capability. That is the flag that makes a colour number mean packed
rgb instead of a palette slot. `Tc` is not a substitute: it is a tmux
convention that ncurses ignores on purpose, and several terminals that really
do 24-bit colour advertise `Tc` while keeping `colors#256`. On those, dvtm
maps every colour to the nearest entry the palette has, which is close and
measurably not the same:

    #1d2021 -> palette 234 = #1c1c1c

dvtm cannot decide this for you. It paints through curses windows, and curses
will not carry a colour the description says the terminal cannot take.

Ask your terminal what it claims:

    $ infocmp -x $TERM | tr ',' '\n' | grep -E 'RGB|colors#'

Ask the ncurses dvtm is linked against, not whichever binary is first on
`PATH` — a system can have two, and they can disagree about both the database
and the format.

If you see `RGB` and a large `colors#`, there is nothing to do. If you see
`colors#256`, look for a ready-made direct-colour description first:

    $ infocmp xterm-direct >/dev/null 2>&1 && echo present

`xterm-direct` ships with ncurses 6.1+ and works on its own, at the cost of
whatever your terminal describes that xterm does not. To keep both, write a
description that takes the colour machinery from one and everything else from
the other. **The first `use=` wins**, so the direct one goes first:

    myterm-direct|myterm with ncurses direct color,
        use=xterm-direct,
        use=myterm,

Compile it into your own terminfo tree and point `TERM` at it:

    $ tic -x -o ~/.terminfo myterm-direct.info
    $ TERM=myterm-direct dvtm

Two things go wrong quietly here:

  * **`tic` must be ncurses 6.1 or newer.** Older ones cannot store
    `colors#0x1000000`; they clamp it to 32767 and say nothing, and the result
    paints nonsense like `ESC[38;5;8154980m`. The `tic` first on `PATH` is not
    always the newest one installed — check with `tic -V`.
  * **`-o` is not optional if `TERMINFO` is set.** Without it `tic` installs
    wherever that variable points, which for some terminals is inside the
    application bundle, where the next update deletes it.

Keep the `xterm` prefix if your terminal's own name has one. It is not
decoration: vim reads it to decide whether to enable `modifyOtherKeys`.

**Point only the programs that need it at the new description.** A large
`colors#` fits only the extended terminfo format, which arrived in ncurses 6.1,
and an older ncurses does not read it partially — it reports the terminal as
unknown, and the program degrades to nothing. That matters wherever one system
carries two ncurses: on macOS everything in `/usr/bin` links the 6.0 in the
base system, so with a direct-colour `TERM` set globally, `less` greets you
with *terminal is not fully functional*. An alias is enough:

    alias dvtm='TERM=myterm-direct dvtm'

Set it globally only where a single modern ncurses serves the whole system.
Either way a name only your machine knows will not resolve over `ssh`.

If you carry one set of dotfiles between machines, an alias gets awkward: the
name is right on one machine and wrong on the next. A small wrapper keeps the
two apart. In the file every machine shares:

    dvtm() {
        if [ -n "$DVTM_OUTER_TERM" ]; then
            env TERM="$DVTM_OUTER_TERM" dvtm "$@"
        else
            command dvtm "$@"
        fi
    }

and in the rc file of the machine that actually has the description:

    DVTM_OUTER_TERM=myterm-direct

A machine that sets nothing behaves exactly as before, so there is nothing to
edit when you move. Three details in there are deliberate:

  * **`env`, and not `TERM=myterm-direct dvtm`.** The second form makes your
    *shell* load the description as well, and a shell linked against an older
    ncurses cannot read a direct-colour one — you get `can't find terminal
    definition` printed on every launch. `env` hands it to dvtm alone.
  * **A variable, and not an alias.** The function reads it when it runs, so it
    does not matter whether the shared file is sourced before or after the
    machine's own rc. An alias named `dvtm` has the opposite problem: it
    expands into the `dvtm() { … }` that comes later, that definition fails to
    parse, and the wrapper quietly ceases to exist.
  * **`DVTM_OUTER_TERM`, and not `DVTM_TERM`.** dvtm already reads `DVTM_TERM`,
    and that one is the `TERM` given to the windows *inside* dvtm. This is the
    terminal dvtm paints *out* to. Two different things, one word apart.

It is plain POSIX shell — sh, dash, bash, ksh and zsh all take it.

The wrapper is also a convenient place for `-c`, though that has nothing to do
with colour:

    env TERM="$DVTM_OUTER_TERM" dvtm -c "${TMPDIR:-/tmp}/dvtm.$$.cmd" "$@"

`-c` creates the command fifo and names it in `DVTM_CMD_FIFO`, which lets a
program running inside dvtm ask dvtm to do something — open a window, say — by
writing to that pipe. See `dvtm(1)`.

### Some characters are displayed like garbage

Check that your locale settings say UTF-8. If they do and it still happens with
a terminal that draws lines through the alternate character set, try
`NCURSES_NO_UTF8_ACS=1`.

### Putty

Under `Terminal => Features`, tick *Disable application keypad mode* to get the
numeric keypad working. Under `Window => Translation`, set the character set to
UTF-8, and use `TERM=putty` or `putty-256color`.

## Design

dvtm follows the [Unix
philosophy](http://www.catb.org/esr/writings/taoup/html/ch01s06.html): do one
thing — *dynamic* window management on the console — and delegate the rest.

Session management is [abduco](https://www.brain-dump.org/projects/abduco/)'s
job, not dvtm's. Selecting text in copy mode is the editor's job. Emulating a
terminal is libvterm's job, which is what this fork changed. What is left is
about 3100 lines of C, small enough to read in an afternoon and hack on.

## Development

Where things are:

    Makefile  config.mk       the build
    config.def.h              what you edit, copied to config.h on first build
    dvtm.info                 the terminfo description installed for the windows
    src/                      dvtm.c, term.c, ui.c and their headers
    src/layouts/              one file per way of arranging windows
    scripts/                  dvtm-editor, dvtm-pager, dvtm-status: installed,
                              never compiled
    man/                      the three manual pages
    tests/                    the suite `make test` runs

Sources are under `src/` and the compiler is told about it with `-I`, so
`#include "dvtm.h"` and `#include "tile.h"` still resolve — those exact lines
live in every custom `config.h` and moving the files was not a reason to break
them.

The house style is in `.clang-format`, and `make format` applies it. Braces on
the same line, four spaces to indent, no tab characters, eighty columns. Two things opt out on purpose: comments are never
rewrapped, because they are hand-wrapped prose whose paragraphs a formatter
would turn into unreadable diffs, and `config.def.h` is skipped entirely — its
tables are aligned into columns by hand and that alignment is how they are read.
`clang-format` is found even when it is off `PATH`, as it is inside the macOS
Command Line Tools; override with `make format CLANG_FORMAT=/path/to/it`.

This fork lives at [IcaroPablo/dvtm](https://github.com/IcaroPablo/dvtm).
Upstream is at [Github](https://github.com/martanne/dvtm) and
[Sourcehut](https://git.sr.ht/~martanne/dvtm), and takes patches through the
[suckless developer mailing list](https://suckless.org/community).

## License

dvtm reuses some code of dwm and is released under the same
[MIT/X11 license](https://raw.githubusercontent.com/martanne/dvtm/master/LICENSE).
`src/term.h` and `scripts/dvtm-editor` carry ISC notices from the code they
came from.

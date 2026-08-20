# dvtm — dynamic virtual terminal manager

dvtm brings tiling window management, popularised by X11 window managers like
[dwm](https://dwm.suckless.org), to the console. It runs several terminal
programs at once inside one terminal and arranges them for you.

![abduco+dvtm demo](https://raw.githubusercontent.com/martanne/dvtm/gh-pages/screencast.gif#center)

This is a fork of [martanne/dvtm](https://github.com/martanne/dvtm), whose last
release was 0.15 in 2016. The one-line difference: **the terminal emulator
inside dvtm is gone**, and libvterm does that work now.

## What changed

  * **The terminal emulator is gone.** `vt.c` was half the program, and the
    half the defects lived in. That work is [libvterm]'s now — the same
    emulator neovim uses. 4031 lines became 3102.
  * dvtm froze completely the first time any signal woke it.
  * Keystrokes typed quickly or pasted together were dropped; only the first
    of each batch was read.
  * Typing any character above `U+007F` reached programs mangled — `á` arrived
    as `Ã¡`.
  * Windows whose program had exited never disappeared on macOS and the BSDs.
  * 24-bit colour was not supported, and on the terminals without it — most of
    them — colour was lost entirely.
  * dvtm's own bar, borders and tag numbers were painted black on black on a
    direct-colour terminal.
  * Builds on macOS, and no `#ifdef` in the source names an operating system
    any more.
  * `make test` drives the real binary and asserts on what it paints.

The whole list, with the reasoning: [docs/changes.md](docs/changes.md).

[libvterm]: https://www.leonerd.org.uk/code/libvterm/

### Limitations

  * **Faint text is painted at full brightness.** libvterm has no bit for
    `SGR 2`, so dvtm never learns a cell was dim. This one the fork lost: the
    old emulator handled it, and text that was dim before is not now.
  * **Colours written `38:2::r:g:b` are read wrongly** — the standards form
    with an empty colourspace field. `38;2;r;g;b` and `38:2:r:g:b` are fine,
    and `dvtm.info` tells programs to use the first, so only programs that
    hardcode the full colon form are affected.
  * **`OSC 11`**, a program asking for the background colour, is unanswered.
  * **`Mod-C` only works on Linux**, and says nothing anywhere else. It opens a
    window in the working directory of the focused one, which needs
    `/proc/<pid>/cwd`. Elsewhere `Mod-C` is `Mod-c`.

The first two are libvterm's, not dvtm's, and `make test` reports each as a
skipped check on every run so that neither goes quiet. There is nothing to
upgrade to: 0.3.3 is the newest release that exists.

## Installing

You need:

  * a C99 compiler
  * **GNU make** — where the system `make` is the BSD one, use `gmake`
  * **`ncursesw` >= 6.1**, built with `--enable-ext-colors`
  * **`libvterm` >= 0.3**
  * **`tic`**, from that same ncurses
  * **`mktemp(1)`**

Nothing else, and no list of supported systems on purpose — any Unix providing
those should build it. What has actually been compiled, and why each dependency
is there, is in [docs/development.md](docs/development.md).

    make && make install

which installs to `~/.local`; pass `PREFIX=/usr/local` or similar for anywhere
else. `CFLAGS` is yours — what you pass is appended after the build's own flags,
so it wins on anything repeatable and cannot delete the include paths:

    make CFLAGS='-O2 -march=native'

`make` printing `Nothing to be done for 'all'` is not a failure; it means the
binaries are newer than the sources.

## Usage and configuring

Every dvtm key binding starts with a modifier, written `MOD` here. By default
it is `CTRL+g`; `dvtm -m ^b` changes it at startup. `dvtm(1)` lists every
binding, including the ones not mentioned below.

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
displays them; `scripts/dvtm-status` is an example that shows the current time.
`MOD+s` hides and shows the bar, `MOD+S` moves it between the top and the
bottom.

`dvtm -c fifo` opens the other direction: a program running inside dvtm can ask
dvtm to do something — open a window, say — by writing to that pipe, whose path
it finds in `DVTM_CMD_FIFO`.

### Copy mode

`MOD+e` hands the window's whole scrollback to an editor. Save and quit, and
what you saved is remembered by dvtm and can be pasted into any window with
`MOD+p` — so the selecting is done in the editor, with whatever search and
regex it has, instead of in dvtm. Cut the buffer down to what you want and
save, or save it whole; either way you get back what you saved. Quit without
saving and nothing is taken, so whatever you copied before is still there.

Quitting is what hands the text over, and not saving: the editor runs as a
filter, and dvtm only reads what it left behind once it is gone. So `MOD+p`
while the editor is still on screen pastes the copy from before it — saving and
switching windows without quitting is not enough.

`MOD+E` sends the same text to a pager instead, which only displays it, and
`MOD+/` opens the pager already searching forward.

The editor is `$DVTM_EDITOR`, then `$VISUAL`, then `$EDITOR`, then `vi`. The
pager is `$DVTM_PAGER`, then `$PAGER`, then `less`. Any full-screen editor
works: the text is handed over in a temporary file and the editor gets the
terminal to itself.

### Configuring

dvtm is configured by editing `config.h` and recompiling — `config.def.h` is
copied to `config.h` on the first build, and is the example to work from. It
defines the layouts, the key bindings and the colour rules, with macros for the
common cases.

A `config.h` brought over from upstream dvtm needs three small edits; they are
listed in [docs/changes.md](docs/changes.md).

## Development

Goals

  * Dynamic tiling window management on the console, and nothing else.
  * Small enough to read in an afternoon: about 3100 lines of C.
  * Plain C99, checked under `-pedantic`, with no `#ifdef` in the source
    naming an operating system.
  * A suite that drives the real binary and asserts on what it paints, and
    that says out loud what it does not cover.

Non-goals

  * Session management — that is [abduco]'s job.
  * Selecting text in copy mode — that is the editor's job.
  * Emulating a terminal — that is libvterm's job, and the reason this fork
    exists.
  * A list of supported systems. Such a list goes stale, and keeping one true
    is what invites back the conditionals this fork spent its time deleting.

Design notes, the source tree, the portability limits, the house style and how
to run the suite: [docs/development.md](docs/development.md).

[abduco]: https://www.brain-dump.org/projects/abduco/

## FAQ

### Detach and reattach

dvtm has no session support. Use [abduco]:

    $ abduco -c dvtm-session

Detach with `CTRL+\` and reattach later with `abduco -a dvtm-session`.

### The build stops at `'vterm.h' file not found`

Not a missing install: the build could not work out **where** libvterm is.
`config.mk` looks under `/opt/homebrew`, `/opt/local`, `/usr/local` and `/usr`.
If yours is elsewhere, say so — either flag overrides the search, so nothing in
`config.mk` needs editing:

    make VTERM_CFLAGS=-I/your/prefix/include \
         VTERM_LIBS='-L/your/prefix/lib -lvterm'

To see what it decided: `make -n dvtm | tr ' ' '\n' | grep vterm`.

### WARNING: terminal is not fully functional

The `dvtm.info` terminfo description is not installed. `make install` does it;
by hand it is `tic -x -s dvtm.info`, and the `-x` is not optional — without it
`tic` silently drops the capabilities that carry 24-bit colour.

If you cannot install terminfo descriptions at all, name a terminal the system
already knows and dvtm will use it for its windows:

    $ DVTM_TERM=rxvt dvtm

### Colours look close, but not right

Programs inside dvtm look brighter or more saturated than they should, or a
pane sits on a background a shade off from the shell beside it. Nothing is
broken: dvtm is approximating, because your terminal told it to.

ncurses sends 24-bit colour only when the terminal description carries the
**`RGB`** capability — the flag that makes a colour number mean packed rgb
instead of a palette slot. `Tc` is not a substitute: it is a tmux convention
ncurses ignores on purpose, and several terminals that really do 24-bit colour
advertise `Tc` while keeping `colors#256`. On those, dvtm maps every colour to
the nearest entry the palette has, which is close and measurably not the same:
`#1d2021` becomes palette 234, `#1c1c1c`. dvtm cannot decide this for you: it
paints through curses, and curses will not carry a colour the description says
the terminal cannot take.

Ask your terminal what it claims — using the ncurses dvtm is linked against,
not whichever binary is first on `PATH`:

    $ infocmp -x $TERM | tr ',' '\n' | grep -E 'RGB|colors#'

If you see `RGB` and a large `colors#`, there is nothing to do. If you see
`colors#256`, `xterm-direct` ships with ncurses 6.1+ and works on its own, at
the cost of whatever your terminal describes that xterm does not. To keep both,
compose a description — **the first `use=` wins**, so the direct one goes
first, and keep the `xterm` prefix if your terminal's name has one, because vim
reads it to decide whether to enable `modifyOtherKeys`:

    myterm-direct|myterm with ncurses direct color,
        use=xterm-direct,
        use=myterm,

    $ tic -x -o ~/.terminfo myterm-direct.info

`tic` must be ncurses 6.1 or newer — older ones clamp `colors#0x1000000` to
32767 without saying so, and paint nonsense like `ESC[38;5;8154980m`; check
with `tic -V`. And `-o` is not optional if `TERMINFO` is set, or `tic` installs
wherever that points, which for some terminals is inside the application
bundle where the next update deletes it.

**Point only dvtm at the new description.** A large `colors#` fits only the
extended terminfo format, and an older ncurses does not read it partially — it
reports the terminal as unknown and the program degrades to nothing. On macOS
everything in `/usr/bin` links the 6.0 in the base system, so with a
direct-colour `TERM` set globally, `less` greets you with *terminal is not
fully functional*. If you carry one set of dotfiles between machines, a wrapper
beats an alias:

    dvtm() {
        if [ -n "$DVTM_OUTER_TERM" ]; then
            env TERM="$DVTM_OUTER_TERM" dvtm "$@"
        else
            command dvtm "$@"
        fi
    }

with `DVTM_OUTER_TERM=myterm-direct` in the rc file of the machine that has the
description. A machine that sets nothing behaves exactly as before. Three
details are deliberate: **`env`**, because `TERM=... dvtm` makes your *shell*
load the description too and an older ncurses then prints `can't find terminal
definition` on every launch; **a variable rather than an alias**, because an
alias named `dvtm` expands inside the function that follows and the wrapper
quietly ceases to exist; and **`DVTM_OUTER_TERM`, not `DVTM_TERM`** — dvtm
already reads the latter, and that one is the `TERM` given to the windows
*inside* dvtm. Either way, a name only your machine knows will not resolve over
`ssh`.

### Copy and paste does not work under X

dvtm grabs mouse events by default, which takes them away from the terminal's
own selection. Hold `Shift` while selecting and pasting, or give up the mouse
bindings: start with `dvtm -M`, toggle with `MOD+M`, or set `ENABLE_MOUSE` to
false in `config.h`.

### How do I set the window title?

With the [xterm escape
sequence](https://tldp.org/HOWTO/Xterm-Title-3.html#ss3.2):

    $ printf '\033]0;Your title here\007'

In `bash`, to keep the working directory in the title:

    case "$TERM" in
    dvtm*|xterm*|rxvt*)
        PROMPT_COMMAND='printf "\033]0;${USER}@${HOSTNAME}: ${PWD/$HOME/~}\007"'
        ;;
    esac

Other shells have an equivalent; zsh has
[precmd](http://zsh.sourceforge.net/Doc/Release/Functions.html#Hook-Functions).

### Some characters are displayed like garbage

Check that your locale settings say UTF-8. If they do and it still happens with
a terminal that draws lines through the alternate character set, try
`NCURSES_NO_UTF8_ACS=1`.

### Putty

Under `Terminal => Features`, tick *Disable application keypad mode* to get the
numeric keypad working. Under `Window => Translation`, set the character set to
UTF-8, and use `TERM=putty` or `putty-256color`.

## License

dvtm reuses some code of dwm and is released under the same
[MIT/X11 license](https://raw.githubusercontent.com/martanne/dvtm/master/LICENSE).
`src/term.h` and `scripts/dvtm-editor` carry ISC notices from the code they
came from.

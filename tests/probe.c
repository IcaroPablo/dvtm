/* Fixture that runs *inside* a dvtm window.
 *
 * Several things the suite has to check can only be driven from a child of
 * dvtm: a device status report is answered to the program that asked, and
 * colour handling is only exercised by a program emitting SGR. This probe
 * performs one such interaction per invocation and prints the result as plain
 * text, which tests/run.c then reads off dvtm's screen.
 *
 * Printing results as text is what keeps the assertions honest: the harness
 * never has to guess whether an escape sequence was swallowed by dvtm or never
 * sent, because the probe reports what it actually received. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

/* Read whatever the terminal replies, within a deadline. Returns byte count. */
static int reply(char *buf, size_t n, int ms) {
    size_t got = 0;
    while (got < n - 1) {
        fd_set r;
        struct timeval tv;
        FD_ZERO(&r);
        FD_SET(STDIN_FILENO, &r);
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        if (select(STDIN_FILENO + 1, &r, NULL, NULL, &tv) <= 0)
            break;
        ssize_t k = read(STDIN_FILENO, buf + got, n - 1 - got);
        if (k <= 0)
            break;
        got += (size_t)k;
        ms = 100; /* first byte may be slow; the rest arrive together */
    }
    buf[got] = '\0';
    return (int)got;
}

/* Render control bytes readable so they can be asserted on as screen text. */
static void printable(const char *s, int n, char *out, size_t outn) {
    size_t o = 0;
    for (int i = 0; i < n && o + 8 < outn; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b)
            o += (size_t)snprintf(out + o, outn - o, "ESC");
        else if (c < 0x20 || c == 0x7f)
            o += (size_t)snprintf(out + o, outn - o, "<%02x>", c);
        else
            out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* Ask the terminal something and report what came back. */
static void ask(const char *label, const char *query) {
    struct termios old, raw;
    char buf[128], pretty[512];
    int n;

    if (tcgetattr(STDIN_FILENO, &old) == 0) {
        raw = old;
        raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    fputs(query, stdout);
    fflush(stdout);
    n = reply(buf, sizeof buf, 2000);
    tcsetattr(STDIN_FILENO, TCSANOW, &old);

    printable(buf, n, pretty, sizeof pretty);
    printf("%s=%s\n", label, pretty);
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    const char *what = argc > 1 ? argv[1] : "";

    if (!strcmp(what, "dsr")) {
        ask("DSR5", "\033[5n"); /* expect ESC[0n */
    } else if (!strcmp(what, "osc11")) {
        /* Ask the terminal for its background colour. vt.c never answered
         * this; libvterm does, so a reply here is proof the engine changed. */
        ask("OSC11", "\033]11;?\033\\");
    } else if (!strcmp(what, "cursor")) {
        ask("CPR", "\033[6n"); /* expect ESC[<row>;<col>R */
    } else if (!strcmp(what, "truecolor")) {
        /* Deliberately different colours per form. dvtm normalises both to
         * whatever its terminfo prescribes, so identical colours would make the
         * two cases indistinguishable downstream. */
        printf("\033[38;2;10;200;30mSEMI\033[0m\n");    /* rgb(10,200,30)  */
        printf("\033[38:2::20:100:250mCOLON\033[0m\n"); /* rgb(20,100,250) */
        printf("\033[38;5;196mPAL256\033[0m\n");        /* palette 196     */
        fflush(stdout);
    } else if (!strcmp(what, "manycolors")) {
        /* More than 255 distinct foreground colours at once, to exercise the
         * extended colour-pair path rather than the 256 palette slots.
         *
         * Red is kept at 1 or 2 so every colour is far above the 0-7 range.
         * A colour whose packed value lands there is the same number as an
         * ANSI colour, and a direct-colour terminfo emits it in the short
         * form -- which is correct, but indistinguishable from a palette
         * colour when counting 24-bit colours on the wire. */
        for (int i = 0; i < 300; i++) {
            int r = 1 + (i / 256), g = i & 0xff, b = 100;
            printf("\033[38;2;%d;%d;%dm#\033[0m", r, g, b);
            if ((i % 60) == 59)
                printf("\n");
        }
        printf("\nMANYDONE\n");
        fflush(stdout);
    } else if (!strcmp(what, "faint")) {
        /* SGR 2. Not a colour: a program asking for the same colour, dimmer.
         * Emitted on its own line so the check can tell "painted without the
         * attribute" from "not painted at all". */
        printf("\033[2mFAINT\033[0m\n");
        fflush(stdout);
    } else if (!strcmp(what, "backspace")) {
        /* Canary for terminfo regressions. The colors#0x1000000 breakage showed
         * up first as a prompt losing its backspaces, because the oversized
         * value broke arithmetic in the child's shell. Assert the erasure
         * itself, which is deterministic, rather than a shell prompt, which is
         * not. */
        printf("ABCDEF\b\b\bXYZ\n");
        fflush(stdout);
    } else if (!strcmp(what, "env")) {
        /* What the child was actually handed. dvtm sets TERM for its children;
         * if it is empty or wrong, every curses program inside dvtm misbehaves
         * and nothing else in the suite would notice. */
        printf("TERM=[%s]\n", getenv("TERM") ? getenv("TERM") : "");
        printf(
            "COLORTERM=[%s]\n", getenv("COLORTERM") ? getenv("COLORTERM") : "");
        fflush(stdout);
    } else if (!strcmp(what, "echo")) {
        /* The exact bytes dvtm's keyboard path delivered, as hex.
         *
         * Hex rather than the characters themselves, because the thing under
         * test is the encoding. A window showing 'á' proves only that dvtm
         * painted something plausible; c3a1 proves the child was handed the
         * two bytes that were typed and not four others. */
        struct termios old, raw;
        char buf[64], hex[3 * sizeof buf];
        size_t o = 0;
        int n;

        if (tcgetattr(STDIN_FILENO, &old) == 0) {
            raw = old;
            raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        /* Announce readiness before blocking: the harness has no other way to
         * know the raw mode is in place, and keys sent before it are echoed by
         * the line discipline instead of reaching this read. */
        printf("ECHOREADY\n");
        fflush(stdout);

        n = reply(buf, sizeof buf, 10000);
        for (int i = 0; i < n && o + 3 < sizeof hex; i++)
            o += (size_t)snprintf(
                hex + o, sizeof hex - o, "%02x", (unsigned char)buf[i]);
        hex[o] = '\0';
        printf("ECHO=%s\n", hex);
        fflush(stdout);
    } else if (!strcmp(what, "paste")) {
        /* What a paste looks like from inside the window.
         *
         * A line editor cannot tell pasted bytes from typing unless the paste
         * arrives bracketed, and reads a multi-line one as line after line of
         * typing. dvtm's screen cannot show that, so the probe prints the bytes
         * it was handed and the check reads them off the screen.
         *
         * The argument says what the child asks for; without one it asks for
         * nothing, and then nothing must arrive, since a program that reads
         * the brackets literally would show them as text. */
        struct termios old, raw;
        char buf[256], pretty[3 * sizeof buf];
        int n;

        if (tcgetattr(STDIN_FILENO, &old) == 0) {
            raw = old;
            raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        /* `bracket` asks and keeps it; `unask` asks and takes it back, which
         * has to leave the child in the same position as never having asked.
         * A mode is a mode and not a latch. */
        if (argc > 2 && !strcmp(argv[2], "bracket")) {
            fputs("\033[?2004h", stdout);
            fflush(stdout);
        } else if (argc > 2 && !strcmp(argv[2], "unask")) {
            fputs("\033[?2004h\033[?2004l", stdout);
            fflush(stdout);
        }
        /* Nothing is pasted until the register has something in it, which
         * takes a trip through copy mode, so the wait here is long. */
        printf("PASTEREADY\n");
        fflush(stdout);

        n = reply(buf, sizeof buf, 30000);
        printable(buf, n, pretty, sizeof pretty);
        printf("PASTE=%s\n", pretty);
        fflush(stdout);
    } else if (!strcmp(what, "plain")) {
        /* Deliberately no SGR at all: these cells must come out in the
         * terminal's default colours, not in some colour of dvtm's choosing. */
        printf("PLAINTEXT\n");
        fflush(stdout);
    } else if (!strcmp(what, "mark")) {
        printf("%s\n", argc > 2 ? argv[2] : "MARK");
        fflush(stdout);
    } else {
        printf("PROBEREADY\n");
        fflush(stdout);
    }

    /* Stay alive so the window persists, and go when the window does.
     *
     * This used to be `for (;;) pause();`, which outlived dvtm. Killing dvtm
     * closes the pty master, and on macOS that gives the slave an end of file
     * rather than a SIGHUP -- so a probe that never reads never learns its
     * terminal is gone. Every run left one behind per window, and several
     * hundred of them exhaust the ptys, at which point windows stop opening
     * and half the suite fails for reasons that are not in dvtm. */
    for (;;) {
        char discard[64];
        fd_set r;
        ssize_t n;

        FD_ZERO(&r);
        FD_SET(STDIN_FILENO, &r);
        /* select first: the modes above leave the terminal at VMIN 0, where a
         * read returns 0 straight away rather than waiting, and the probe
         * would exit the moment it had printed its answer. */
        if (select(STDIN_FILENO + 1, &r, NULL, NULL, NULL) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        n = read(STDIN_FILENO, discard, sizeof discard);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
    }
    return 0;
}

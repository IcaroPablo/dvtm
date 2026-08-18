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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

/* Read whatever the terminal replies, within a deadline. Returns byte count. */
static int reply(char *buf, size_t n, int ms)
{
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
static void printable(const char *s, int n, char *out, size_t outn)
{
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
static void ask(const char *label, const char *query)
{
	struct termios old, raw;
	char buf[128], pretty[512];
	int n;

	if (tcgetattr(STDIN_FILENO, &old) == 0) {
		raw = old;
		raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
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

int main(int argc, char *argv[])
{
	const char *what = argc > 1 ? argv[1] : "";

	if (!strcmp(what, "dsr")) {
		ask("DSR5", "\033[5n");           /* expect ESC[0n */
	} else if (!strcmp(what, "cursor")) {
		ask("CPR", "\033[6n");            /* expect ESC[<row>;<col>R */
	} else if (!strcmp(what, "truecolor")) {
		/* Deliberately different colours per form. dvtm normalises both to
		 * whatever its terminfo prescribes, so identical colours would make the
		 * two cases indistinguishable downstream. */
		printf("\033[38;2;10;200;30mSEMI\033[0m\n");     /* rgb(10,200,30)  */
		printf("\033[38:2::20:100:250mCOLON\033[0m\n");  /* rgb(20,100,250) */
		printf("\033[38;5;196mPAL256\033[0m\n");         /* palette 196     */
		fflush(stdout);
	} else if (!strcmp(what, "manycolors")) {
		/* more than 255 distinct foreground colours on screen at once, to
		 * exercise the extended colour-pair path rather than the 256 slots */
		for (int i = 0; i < 300; i++) {
			int r = (i * 7) & 0xff, g = (i * 13) & 0xff, b = (i * 29) & 0xff;
			printf("\033[38;2;%d;%d;%dm#\033[0m", r, g, b);
			if ((i % 60) == 59)
				printf("\n");
		}
		printf("\nMANYDONE\n");
		fflush(stdout);
	} else if (!strcmp(what, "backspace")) {
		/* Canary for terminfo regressions. The colors#0x1000000 breakage showed
		 * up first as a prompt losing its backspaces, because the oversized
		 * value broke arithmetic in the child's shell. Assert the erasure
		 * itself, which is deterministic, rather than a shell prompt, which is
		 * not. */
		printf("ABCDEF\b\b\bXYZ\n");
		fflush(stdout);
	} else if (!strcmp(what, "mark")) {
		printf("%s\n", argc > 2 ? argv[2] : "MARK");
		fflush(stdout);
	} else {
		printf("PROBEREADY\n");
		fflush(stdout);
	}

	/* Stay alive so the window persists until the test kills it. */
	for (;;)
		pause();
	return 0;
}

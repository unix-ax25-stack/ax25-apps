/*
 * Copyright 1996, 1997 Heikki Hannikainen OH7LZB <oh7lzb@sral.fi>
 *
 * Portions and ideas (like the ibm character mapping) from
 *	Tomi Manninen OH2BNS <oh2bns@sral.fi>
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <curses.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>

#include "listen.h"

int color = 0;			/* Colorized? */
int sevenbit = 1;		/* Are we on a 7-bit terminal? */
int ibmhack = 0;		/* IBM mapping? */

/*
 * Whether we have already said that the output cannot be written.  One flag
 * and not a counter, because the thing that goes wrong is one thing - there is
 * no output to be had - and it says itself once per outage, not once per
 * frame.  See lprintf() below.
 */
static int output_failed = 0;

/* mapping of IBM codepage 437 chars 128-159 to ISO latin1 equivalents
 * (158 and 159 are mapped to space)
 */

static unsigned char ibm_map[32] = {
	199, 252, 233, 226, 228, 224, 229, 231,
	234, 235, 232, 239, 238, 236, 196, 197,
	201, 230, 198, 244, 246, 242, 251, 249,
	255, 214, 220, 162, 163, 165, 32, 32
};

/*
 *	Printf in Technicolor (TM) (available in selected theatres only)
 */

void lprintf(int dtype, char *fmt, ...)
{
	va_list args;
	char str[1024];
	chtype ch;
	char *p;

	va_start(args, fmt);
	vsnprintf(str, 1024, fmt, args);
	va_end(args);

	if (color) {
		for (p = str; *p != '\0'; p++) {
			ch = *p;

			if (sevenbit && ch > 127)
				ch = '.';

			if ((ch > 127 && ch < 160) && ibmhack)
				ch = ibm_map[ch - 128] | A_BOLD;
			else if ((ch < 32) && (ch != '\n'))
				ch = (ch + 64) | A_REVERSE;

			if ((dtype == T_ADDR) || (dtype == T_PROTOCOL)
			    || (dtype == T_AXHDR) || (dtype == T_IPHDR)
			    || (dtype == T_ROSEHDR) || (dtype == T_PORT)
			    || (dtype == T_TIMESTAMP))
				ch |= A_BOLD;

			ch |= COLOR_PAIR(dtype);

			addch(ch);
		}
	} else {
		int last, err, busy = 0;

		for (p = str; *p != '\0'; p++)
			if ((*p < 32 && *p != '\n')
			    || (*p > 126 && (unsigned char) *p < 160 && sevenbit))
				*p = '.';
		last = (p > str && p[-1] == '\n');

		/*
		 * Flush at the end of a line and nowhere else.
		 *
		 * Every line this program shows is built from fragments -
		 * "fm ", the call, " to ", the other call, the control
		 * field, the newline - and flushing each of them turned one
		 * line into a dozen writes.  When the terminal was full, the
		 * write that failed was one in the middle of a line, so the
		 * operator got half a frame header, the reason for it after
		 * it, and the rest of the header somewhere else: output that
		 * cannot be told apart from output that arrived.
		 *
		 * Left alone, the fragments stay in the buffer until the
		 * newline arrives, and the line goes out in one write or not
		 * at all.  What is missing when the output is blocked is
		 * then a whole line, which is what the message below is
		 * about, and it is announced at a line boundary instead of
		 * in the middle of one.
		 *
		 * The flush itself stays where it is.  A buffered fputs()
		 * can take the bytes happily and only the flush finds out
		 * that the descriptor will not have them, and nothing else
		 * in this function would ever see that failure - the next
		 * fragment does not end a line either.
		 */
		err = 0;
		if (fputs(str, stdout) == EOF)
			err = errno;
		else if (last && fflush(stdout) == EOF)
			err = errno;

		if (err == EINTR || err == EAGAIN)
			busy = 1;
		else if (err == EWOULDBLOCK)	/* the same value as EAGAIN
						 * where the two are one */
			busy = 1;

		if (busy) {
			/*
			 * Output that is full is not the monitor failing.  A
			 * terminal whose buffer is full, a pty nobody is
			 * draining - none of that says anything about the
			 * frames, and a listener that exits for it has
			 * stopped watching the band over a reason that has
			 * nothing to do with the band.  This program writes
			 * one line per frame to a terminal that whatever
			 * else is running on it is filling up at the same
			 * time, so a terminal with no room left is an
			 * ordinary Tuesday, not a fault in the monitor.
			 *
			 * So: say once that the frames are going nowhere,
			 * keep listening, and keep trying - the terminal may
			 * drain, and when it does the output carries on
			 * where it left off.  Say that too, because a
			 * monitor that goes quiet and then starts up again
			 * is otherwise indistinguishable from one that lost
			 * the band and found it.
			 *
			 * The outage is only remembered if the words got
			 * out.  stderr goes to the same terminal that has
			 * no room, so that write can be the one that is
			 * dropped - and a monitor that then announced the
			 * recovery of an outage nobody read ends up with a
			 * "works again" for which there was nothing before
			 * it.  Left unremembered, the next frame says it
			 * again.
			 *
			 * And clear the stream's error indicator, because
			 * stdio does not do that for us: on a stream that
			 * has failed, fputs() and fflush() keep answering
			 * EOF without putting anything on the descriptor
			 * any more - measured on macOS, where a raw write()
			 * to the very same descriptor succeeded right next
			 * to a flush() that still returned EAGAIN.  Left
			 * set, the busy output never becomes unbusy: the
			 * monitor would drop every line after the first
			 * one it could not write, for as long as it runs.
			 * For an output that is full, the indicator is
			 * only a note about a line that is still owed, and
			 * the next line has to be allowed to try.
			 */
			clearerr(stdout);
			if (!output_failed &&
			    fprintf(stderr, "listen: cannot write to "
				    "standard output (%s); frames are "
				    "dropped until it works again\n",
				    strerror(err)) >= 0)
				output_failed = 1;
			return;
		}

		if (err != 0) {
			/*
			 * Everything else says the descriptor itself is not
			 * there any more: EBADF for one closed for good, and
			 * on Linux EIO for a pty whose master side went away
			 * - which is what a login shell leaving its terminal
			 * does.  Nothing is going to make those writable
			 * again, and a listener that cannot write to the
			 * terminal it was started on and goes on listening
			 * anyway is a program with no reader, no purpose and
			 * no end: it survives the logout and waits for
			 * frames it will show to nobody.
			 *
			 * So this one ends the program, with the reason.
			 * That is where the exit(1) went that this used to
			 * have for every error, and it belongs here rather
			 * than on a terminal that is merely busy.
			 */
			fprintf(stderr, "listen: cannot write to standard "
				"output (%s): the output is gone, stopping\n",
				strerror(err));
			exit(1);
		}

		if (output_failed) {
			output_failed = 0;
			fprintf(stderr, "listen: standard output works "
				"again\n");
		}
	}
}

int initcolor(void)
{
	initscr();		/* Start ncurses */
	if (!has_colors()) {
		endwin();
		fprintf(stderr, "Your terminal does not support color\n");
		exit(1);
	}
	start_color();		/* Initialize color support */
	refresh();		/* Clear screen */
	noecho();		/* Don't echo */
	wattrset(stdscr, 0);	/* Clear attributes */
	scrollok(stdscr, TRUE);	/* Like a scrolling Stone... */
	leaveok(stdscr, TRUE);	/* Cursor position doesn't really matter */
	idlok(stdscr, TRUE);	/* Use hardware ins/del of the terminal */
	nodelay(stdscr, TRUE);	/* Make getch() nonblocking */

	/* Pick colors for each type */
	init_pair(T_PORT, COLOR_GREEN, COLOR_BLACK);
	init_pair(T_DATA, COLOR_WHITE, COLOR_BLACK);
	init_pair(T_ERROR, COLOR_RED, COLOR_BLACK);
	init_pair(T_PROTOCOL, COLOR_CYAN, COLOR_BLACK);
	init_pair(T_AXHDR, COLOR_WHITE, COLOR_BLACK);
	init_pair(T_IPHDR, COLOR_WHITE, COLOR_BLACK);
	init_pair(T_ADDR, COLOR_GREEN, COLOR_BLACK);
	init_pair(T_ROSEHDR, COLOR_WHITE, COLOR_BLACK);
	init_pair(T_TIMESTAMP, COLOR_YELLOW, COLOR_BLACK);
	init_pair(T_KISS, COLOR_MAGENTA, COLOR_BLACK);
	init_pair(T_BPQ, COLOR_MAGENTA, COLOR_BLACK);
	init_pair(T_TCPHDR, COLOR_BLUE, COLOR_BLACK);
	init_pair(T_FLEXNET, COLOR_BLUE, COLOR_BLACK);
	init_pair(T_OPENTRAC, COLOR_YELLOW, COLOR_BLACK);

	return 1;
}

char *servname(int port, char *proto)
{
	struct servent *serv;
	static char str[6];

	if ((serv = getservbyport(htons(port), proto)))
		return serv->s_name;
	else
		snprintf(str, sizeof(str), "%i", port);

	return str;
}

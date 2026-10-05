#include <sys/types.h>
#include <sys/ioctl.h>
#include <netdb.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <curses.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <net/if.h>
#include <netax25/ax25.h>
#include <netax25/axconfig.h>
#include <netax25/axmon.h>

#include <config.h>
#include "listen.h"

/* No packet socket on macOS/BSD: the shim in libax25 intercepts
 * socket(PF_PACKET, SOCK_PACKET, ...) and feeds raw AX.25 frames from
 * the AGWPE server into it.  */
#ifndef PF_PACKET
#define	PF_PACKET	17
#endif
#ifndef AF_PACKET
#define	AF_PACKET	PF_PACKET
#endif
#ifndef SOCK_PACKET
#define	SOCK_PACKET	10
#endif

/* The values Linux uses in <linux/if_ether.h>, so that a frame captured
 * here means the same thing on either kind of system.  */
#ifndef ETH_P_AX25
#define	ETH_P_AX25	0x0002
#endif
#ifndef ETH_P_ALL
#define	ETH_P_ALL	0x0003
#endif

#ifndef SIOCGIFHWADDR
#define	SIOCGIFHWADDR	0x8927
#endif

/* The hardware address member of struct ifreq is ifr_hwaddr on Linux
 * and ifr_addr on the BSDs and macOS.  */
#if defined(__linux__)
#define	LISTEN_IFR_HWADDR	ifr_hwaddr
#else
#define	LISTEN_IFR_HWADDR	ifr_addr
#endif

static struct timeval t_recv;
static int tflag = 0;
static int32_t thiszone;	/* seconds offset from gmt to local time */
static int sigint;

static void display_port(char *dev)
{
	char *port;

	port = ax25_config_get_name(dev);
	if (port == NULL)
		port = dev;
	/* Nothing came back, and dev is empty as well: the frame arrived
	 * without a name for the port it came on.  Say so.  Falling through
	 * prints an empty label, so every such frame looks as if it had come
	 * in on no port at all, and the line gives the operator nothing to go
	 * on.  A question mark is not a name either, but it is visibly not
	 * one, and the callsign on the frame still says where it went.  */
	if (port == NULL || *port == '\0')
		port = "?";

	lprintf(T_PORT, "%s: ", port);
}

/* from tcpdump util.c */

/*
 * Format the timestamp
 */
static char * ts_format(unsigned int sec, unsigned int usec)
{
	/* "%06u" can print up to ten digits for an "unsigned int"; GCC does
	 * not always narrow the usec<1000000 clamp below into the format
	 * range check, so size the buffer for the worst case.  */
	static char buf[sizeof("00:00:00.0000000000")];
	unsigned int hours, minutes, seconds;

	seconds  = sec % 60;
	sec	 = sec / 60;
	minutes  = sec % 60;
	sec	 = sec / 60;
	hours	 = sec % 24;

	/*
	 * The real purpose of these checks is to let GCC figure out the
	 * value range of all variables thus avoid bogus warnings.  For any
	 * halfway modern GCC the checks will be optimized away.
	 */
	if (hours >= 24)
		unreachable();
	if (minutes >= 60)
		unreachable();
	if (seconds >= 60)
		unreachable();
	if (usec >= 1000000)
		unreachable();

	snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06u",
		 hours, minutes, seconds, usec);

	return buf;
}

/*
 * Print the timestamp
 */
static void ts_print(const struct timeval *tvp)
{
        int s;
        struct tm *tm;
        time_t Time;
        static unsigned b_sec;
        static unsigned b_usec;
        int d_usec;
        int d_sec;

        switch (tflag) {

        case 0: /* Default */
                s = (tvp->tv_sec + thiszone) % 86400;
                (void)lprintf(T_TIMESTAMP, "%s ", ts_format(s, tvp->tv_usec));
                break;

        case 1: /* No time stamp */
                break;

        case 2: /* Unix timeval style */
                (void)lprintf(T_TIMESTAMP, "%u.%06u ",
                             (unsigned)tvp->tv_sec,
                             (unsigned)tvp->tv_usec);
                break;

        case 3: /* Microseconds since previous packet */
        case 5: /* Microseconds since first packet */
                if (b_sec == 0) {
                        /* init timestamp for first packet */
                        b_usec = tvp->tv_usec;
                        b_sec = tvp->tv_sec;
                }

                d_usec = tvp->tv_usec - b_usec;
                d_sec = tvp->tv_sec - b_sec;

                while (d_usec < 0) {
                    d_usec += 1000000;
                    d_sec--;
                }

                (void)lprintf(T_TIMESTAMP, "%s ", ts_format(d_sec, d_usec));

                if (tflag == 3) { /* set timestamp for last packet */
                    b_sec = tvp->tv_sec;
                    b_usec = tvp->tv_usec;
                }
                break;

        case 4: /* Default + Date*/
                s = (tvp->tv_sec + thiszone) % 86400;
                Time = (tvp->tv_sec + thiszone) - s;
                tm = gmtime (&Time);
                if (!tm)
                        lprintf(T_TIMESTAMP, "Date fail  ");
                else
                        lprintf(T_TIMESTAMP, "%04d-%02d-%02d %s ",
                               tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
                               ts_format(s, tvp->tv_usec));
                break;
        }
}

void display_timestamp(void)
{
	ts_print(&t_recv);
}

/* from tcpdump gmtlocal.c */

static int32_t gmt2local(time_t t)
{
        int dt, dir;
        struct tm *gmt, *loc;
        struct tm sgmt;

        if (t == 0)
                t = time(NULL);
        gmt = &sgmt;
        *gmt = *gmtime(&t);
        loc = localtime(&t);
        dt = (loc->tm_hour - gmt->tm_hour) * 60 * 60 +
            (loc->tm_min - gmt->tm_min) * 60;

        /*
         * If the year or julian day is different, we span 00:00 GMT
         * and must add or subtract a day. Check the year first to
         * avoid problems when the julian day wraps.
         */
        dir = loc->tm_year - gmt->tm_year;
        if (dir == 0)
                dir = loc->tm_yday - gmt->tm_yday;
        dt += dir * 24 * 60 * 60;

        return dt;
}

static void handle_sigint(int signal)
{
	/*
	 * Just the flag.  poll() is interrupted by a signal whether or not it
	 * is restarted, so the wait comes back with EINTR and the loop below
	 * sees sigint - there is no blocking recvfrom to disturb any more,
	 * and there are several descriptors now, so there is no single one
	 * to close.
	 */
	sigint++;
}

#define ASCII		0
#define HEX 		1
#define READABLE	2

#define BUFSIZE		1500

/*
 * Read one monitor frame.  On the monitor side a frame arrives as one
 * length-prefixed unit and on a kernel packet socket each recvfrom returns
 * exactly one frame; axmon_read() hands over the payload either way.  Returns
 * the payload length, 0 at end of file, or -1.
 */
static int recv_frame(int fd, unsigned char *buf, size_t buflen,
		      struct sockaddr *sa, socklen_t *asize, int framed)
{
	return (int)axmon_read(fd, framed, buf, buflen, sa, asize);
}

int main(int argc, char **argv)
{
	unsigned char buffer[BUFSIZE];
	int dumpstyle = ASCII;
	int size;
	int s;
	char *port = NULL;
	struct sockaddr sa;
	socklen_t asize;
	struct ifreq ifr;
	struct axmon mon;
	int proto = ETH_P_AX25;
	int exit_code = EXIT_SUCCESS;

	while ((s = getopt(argc, argv, "8achip:rtv")) != -1) {
		switch (s) {
		case '8':
			sevenbit = 0;
			break;
		case 'a':
			proto = ETH_P_ALL;
			break;
		case 'c':
			color = 1;
			break;
		case 'h':
			dumpstyle = HEX;
			break;
		case 'i':
			ibmhack = 1;
			break;
		case 'p':
			port = optarg;
			break;
		case 'r':
			dumpstyle = READABLE;
			break;
		case 't':
			tflag++;
			break;
		case 'v':
			printf("listen: %s\n", VERSION);
			return 0;
		case ':':
			fprintf(stderr,
				"listen: option -p needs a port name\n");
			return 1;
		case '?':
			fprintf(stderr,
				"Usage: listen [-8] [-a] [-c] [-h] [-i] [-p port] [-r] [-t..] [-v]\n");
			return 1;
		}
	}

	switch (tflag) {
	case 0: /* Default */
	case 4: /* Default + Date*/
		thiszone = gmt2local(0);
		break;
	case 1: /* No time stamp */
	case 2: /* Unix timeval style */
	case 3: /* Microseconds since previous packet */
	case 5: /* Microseconds since first packet */
		break;
	default: /* Not supported */
		fprintf(stderr, "listen: only -t, -tt, -ttt, -tttt and -ttttt are supported\n");
		return 1;
        }

	if (ax25_config_load_ports() == 0)
		fprintf(stderr, "listen: no AX.25 port data configured\n");

	/*
	 * Every source of raw frames this machine has, not one of them.
	 * On a host that has a kernel AX.25 stack as well as an ax25netd
	 * that is a packet socket and a monitor stream side by side, and
	 * both carry frames the operator asked to see.  -p restricts both:
	 * the kernel socket by device where the port has one, the monitor
	 * by name where it does not - and a name in no axports entry is
	 * still the refusal it always was, so a typo is not silently
	 * turned into "monitor everything".
	 */
	if (axmon_open(htons(proto), port, &mon) < 0) {
		if (errno == EINVAL)
			fprintf(stderr, "listen: invalid port name - %s\n",
				port);
		else
			perror("listen: cannot watch for AX.25 frames");
		return 1;
	}

	for (s = 0; s < mon.nfd; s++)
		if (mon.framed[s] && getenv("AXSOCK_DEBUG") != NULL)
			fprintf(stderr, "listen: raw monitor via libax25 "
				"ax25netd (framed)\n");

	/* -p is answered by the two binds inside axmon_open() now, so
	 * there is no name left to compare each frame against here: the
	 * frames that arrive have been filtered already, by the interface
	 * on one side and by the port name on the other.  That is why the
	 * kernel side needs no comparison - a packet socket bound to an
	 * interface only ever sees that interface - and why the monitor
	 * side needs none either, having been handed the same name in the
	 * same call.  */

	if (color) {
		color = initcolor();	/* Initialize color support */
		if (!color)
			printf("Could not initialize color support.\n");
	}

	setservent(1);

	/* The source count is part of the condition and not only something
	 * checked inside: retiring the last source sets exit_code to the
	 * reason it went, and polling again would answer ENOTCONN and
	 * overwrite that with a question the program already knows the
	 * answer to.  A program with nothing to listen to has finished,
	 * and says so for the reason the last source gave.  */
	while (!sigint && axmon_alive(&mon) > 0) {
		unsigned ready = 0;
		int n, f;

		signal(SIGINT, handle_sigint);
		signal(SIGTERM, handle_sigint);

		n = axmon_poll(&mon, -1, &ready);
		if (n < 0) {
			if (errno == EINTR) {
				refresh();
				continue;
			}
			perror("poll");
			exit_code = errno;
			break;
		}
		if (n == 0)
			continue;	/* a signal, not an answer */

		for (f = 0; f < mon.nfd && !sigint; f++) {
			if ((ready & (1u << f)) == 0)
				continue;

			asize = sizeof(sa);
			size = recv_frame(mon.fd[f], buffer, sizeof(buffer),
					  &sa, &asize, mon.framed[f]);
			if (size <= 0) {
				const char *which = axmon_source_name(&mon, f);
				int err = (size < 0) ? errno : 0;
				int left;

				/*
				 * Two ways a source can stop, and neither of
				 * them is the monitor stopping.
				 *
				 * A read that returned nothing is the end of
				 * a stream: a frame is never empty, so there
				 * is nothing here to decode.  A read that
				 * failed is a socket that will not come
				 * back - the kernel AX.25 module unloaded
				 * under this process, an interface taken
				 * away, a peer gone mid frame.  Neither is
				 * worth a program that still has another
				 * source: on a host with a kernel stack the
				 * kernel's ports stay on the air, and the
				 * ax25netd beside them has nothing to do
				 * with a packet socket that failed.
				 *
				 * Both end the same way and both are said
				 * out loud, with the reason and with what is
				 * left, because a listener that goes quiet
				 * without a word looks exactly like one
				 * that is hearing nothing.
				 */
				if (err == EINTR) {
					/*
					 * Signals are cared for by the
					 * handler, and we don't want to abort
					 * on SIGWINCH.
					 */
					refresh();
					continue;
				}
				ready &= ~(1u << f);
				left = axmon_retire(&mon, f);
				if (err == 0)
					fprintf(stderr, "listen: the %s closed%s\n",
						which,
						left > 0 ? ", watching the other "
							  "source" : "");
				else
					fprintf(stderr,
						"listen: the %s failed: %s%s\n",
						which, strerror(err),
						left > 0 ? ", watching the other "
							  "source" : "");
				if (left == 0) {
					exit_code = err ? err : ENOTCONN;
					break;
				}
				continue;
			}
			gettimeofday(&t_recv, NULL);
			signal(SIGINT, SIG_DFL);
			signal(SIGTERM, SIG_DFL);
			if (sigint)
				break;

			if (proto == ETH_P_ALL && mon.kind[f] == AXMON_KERNEL) {
				strcpy(ifr.ifr_name, sa.sa_data);
				signal(SIGINT, handle_sigint);
				signal(SIGTERM, handle_sigint);
				if (ioctl(mon.fd[f], SIOCGIFHWADDR, &ifr) == -1) {
					/*
					 * -a asks for every protocol, so a
					 * frame arrives here with the name
					 * of the interface it came from and
					 * nothing else to say what it was.
					 * An interface that is gone, or that
					 * has no hardware address to give,
					 * means this frame is not an AX.25
					 * one from a port we can name - the
					 * same question the line below asks
					 * and answers for a live interface.
					 * It is a question about the frame,
					 * not about the socket: the next one
					 * may be an AX.25 frame again, so
					 * the frame is dropped and the
					 * socket stays.
					 */
					refresh();
					continue;
				}
				signal(SIGINT, SIG_DFL);
				signal(SIGTERM, SIG_DFL);
				if (sigint)
					break;
				if (ifr.LISTEN_IFR_HWADDR.sa_family != AF_AX25)
					continue;
				if (size > 2 && *buffer == 0xcc) {
					/* IP packets from the ax25 de-segmenter
					 * are seen on socket "PF_PACKET,
					 * SOCK_PACKET, ETH_P_ALL" without
					 * AX.25 header (just the IP-frame),
					 * prefixed by 0xcc (AX25_P_IP).
					 * It's unclear why in the kernel code
					 * this happens (unsegmentet AX25 PID
					 * AX25_P_IP have not this behavior).
					 * We have already displayed all the
					 * segments and like to ignore this
					 * data.
					 * AX.25 packets start with a kiss
					 * byte (buffer[0]); ax25_dump()
					 * looks for it.
					 * There's no kiss command 0xcc
					 * defined; kiss bytes are checked
					 * against & 0xf (= 0x0c), which is
					 * also not defined.
					 * Kiss commands may have one argument.
					 * => We can make safely make the
					 * assumption for first byte == 0xcc
					 * and length > 2, that we safeley can
					 * detect those IP frames, and then
					 * ignore it.
					 */
					continue;
				}
			}
			display_port(sa.sa_data);
			ki_dump(buffer, size, dumpstyle);
/*			lprintf(T_DATA, "\n");  */
			if (color)
				refresh();
		}
		if (sigint)
			break;
	}
	axmon_close(&mon);
	if (color)
		endwin();

	return exit_code;
}

static void ascii_dump(unsigned char *data, int length)
{
	char c;
	int i, j;
	char buf[100];

	for (i = 0; length > 0; i += 64) {
		sprintf(buf, "%04X  ", i);

		for (j = 0; j < 64 && length > 0; j++) {
			c = *data++;
			length--;

			if ((c != '\0') && (c != '\n'))
				strncat(buf, &c, 1);
			else
				strcat(buf, ".");
		}

		lprintf(T_DATA, "%s\n", buf);
	}
}

static void readable_dump(unsigned char *data, int length)
{
	unsigned char c;
	int i;
	int cr = 1;
	char buf[BUFSIZE];

	for (i = 0; length > 0; i++) {

		c = *data++;
		length--;

		switch (c) {
		case 0x00:
			buf[i] = ' ';
		case 0x0A:	/* hum... */
		case 0x0D:
			if (cr)
				buf[i] = '\n';
			else
				i--;
			break;
		default:
			buf[i] = c;
		}
		cr = (buf[i] != '\n');
	}
	if (cr)
		buf[i++] = '\n';
	buf[i] = '\0';
	lprintf(T_DATA, "%s", buf);
}

static void hex_dump(unsigned char *data, int length)
{
	unsigned char *data2;
	int i, j, length2;
	unsigned char c;

	char buf[4], hexd[49], ascd[17];

	length2 = length;
	data2 = data;

	for (i = 0; length > 0; i += 16) {

		hexd[0] = '\0';
		for (j = 0; j < 16; j++) {
			c = *data2++;
			length2--;

			if (length2 >= 0)
				sprintf(buf, "%2.2X ", c);
			else
				strcpy(buf, "   ");
			strcat(hexd, buf);
		}

		ascd[0] = '\0';
		for (j = 0; j < 16 && length > 0; j++) {
			c = *data++;
			length--;

			sprintf(buf, "%c",
				((c != '\0') && (c != '\n')) ? c : '.');
			strcat(ascd, buf);
		}

		lprintf(T_DATA, "%04X  %s | %s\n", i, hexd, ascd);
	}
}

void data_dump(void *data, int length, int dumpstyle)
{
	switch (dumpstyle) {

	case READABLE:
		readable_dump(data, length);
		break;
	case HEX:
		hex_dump(data, length);
		break;
	default:
		ascii_dump(data, length);
	}
}

int get16(unsigned char *cp)
{
	int x;

	x = *cp++;
	x <<= 8;
	x |= *cp++;

	return x;
}

int get32(unsigned char *cp)
{
	int x;

	x = *cp++;
	x <<= 8;
	x |= *cp++;
	x <<= 8;
	x |= *cp++;
	x <<= 8;
	x |= *cp;

	return x;
}


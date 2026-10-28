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
#include <fcntl.h>

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
static FILE *pcap_fp = NULL;
static char *listen_port_arg = NULL;

static void display_port(char *dev);

static void display_frame_port(struct axmon *mon, int idx, struct sockaddr *sa)
{
	const char *src = axmon_source_name(mon, idx);

	/*
	 * Prefer the logical port/interface name from axmon if it is a real
	 * name rather than a generic source description.  axmon_open() may
	 * store names for WAMPES/AGWPE/ax25netd monitor sources here, and
	 * using it avoids truncation when that name is copied into a
	 * struct sockaddr with sa_data[14] on some paths.  If axmon ever
	 * carries the full name per source in a larger buffer, this
	 * sa_data fallback and the ax25_config_get_name() lookup below can
	 * be removed or simplified.
	 */
	if (src && strcmp(src, "AX.25 packet socket") &&
	    strcmp(src, "WAMPES monitor") &&
	    strcmp(src, "AX.25 monitor") &&
	    strcmp(src, "AX.25 source")) {
		lprintf(T_PORT, "%s: ", src);
		return;
	}

	if (sa)
		display_port(sa->sa_data);
	else
		lprintf(T_PORT, "?: ");
}

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

/* PCAP (libpcap) format for writing to file or pipe */
struct pcap_file_header {
	uint32_t magic;
	uint16_t version_major;
	uint16_t version_minor;
	int32_t thiszone;
	uint32_t sigfigs;
	uint32_t snaplen;
	uint32_t linktype;
};

struct pcap_pkthdr {
	uint32_t ts_sec;
	uint32_t ts_usec;
	uint32_t incl_len;
	uint32_t orig_len;
};

#ifndef DLT_AX25
#define DLT_AX25 3
#endif

static void pcap_write_header(void)
{
	struct pcap_file_header hdr;

	hdr.magic = 0xa1b2c3d4;
	hdr.version_major = 2;
	hdr.version_minor = 4;
	hdr.thiszone = 0;
	hdr.sigfigs = 0;
	hdr.snaplen = 65535;
	hdr.linktype = DLT_AX25;

	if (pcap_fp)
		fwrite(&hdr, sizeof(hdr), 1, pcap_fp);
}

static void pcap_write_packet(const unsigned char *data, int len)
{
	struct pcap_pkthdr phdr;
	struct timeval tv;
	const unsigned char *p = data;
	int l = len;

	if (!pcap_fp || l <= 0)
		return;

	/*
	 * DLT_AX25 expects a raw AX.25 frame (no KISS framing). Frames coming
	 * via ax25netd/WAMPES/AGWPE monitor are often KISS-wrapped (command
	 * byte in high nibble, type in low nibble; for data frames low nibble
	 * is 0x0). Strip a leading KISS command byte when it looks like one.
	 * This heuristic must not touch raw AX.25 frames from a kernel packet
	 * socket (they do not start with a KISS command byte in normal cases).
	 * Multi-port KISS uses the upper nibble for port number; CRC variants
	 * use different encodings in the upper nibbles - be conservative.
	 */
//fprintf(stderr, "l = %d; p0 %2.2x p1 %2.2x p3 %2.2x p4 %2.2x\n", l, p[0], p[1], p[2], p[3]);
// AI counted wrong. Not 18 but >= 16 is korrect: DL9SAU>DB0CCC SABM has no PID field
//                                                -> 7+7+1 = 15. Plus leading kiss byte = 16
// Still a bug - in libpcap? Both, src and dst calls are shown in wireshark with SSID striped;
// and if SSID is >= 10, it shows 5 bytes of the call instead of 6.
	//if (l >= 18 && (p[0] & 0x0f) == 0x00) {
	if (l >= 16 && (p[0] & 0x0f) == 0x00) {
		/* Looks like KISS data frame (port in high nibble, port 0 is 0x00) */
		p++;
		l--;
	//} else if (l >= 18 && p[0] == 0x01) {
	} else if (l >= 16 && p[0] == 0x01) {
		/*
		 * Some implementations may use command 0x01 for TX Data; for a
		 * pure monitor stream we almost never see TX, but be defensive.
		 */
		p++;
		l--;
	}

	if (gettimeofday(&tv, NULL) < 0) {
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}

	phdr.ts_sec = (uint32_t)tv.tv_sec;
	phdr.ts_usec = (uint32_t)tv.tv_usec;
	if (l > 65535)
		l = 65535;
	phdr.incl_len = (uint32_t)l;
	phdr.orig_len = (uint32_t)l;

	fwrite(&phdr, sizeof(phdr), 1, pcap_fp);
	fwrite(p, 1, l, pcap_fp);
	fflush(pcap_fp);
}

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

	while ((s = getopt(argc, argv, "8achip:rtvw:")) != -1) {
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
			listen_port_arg = optarg;
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
		case 'w':
			if (strcmp(optarg, "-") == 0)
				pcap_fp = stdout;
			else {
				pcap_fp = fopen(optarg, "wb");
				if (pcap_fp == NULL) {
					perror("listen: fopen");
					return 1;
				}
			}
			break;
		case ':':
			fprintf(stderr,
				"listen: option -p needs a port name\n");
			return 1;
		case '?':
			fprintf(stderr,
				"Usage: listen [-8] [-a] [-c] [-h] [-i] [-p port] [-r] [-t..] [-v] [-w file|-]\n");
			return 1;
		}
	}

	if (pcap_fp) {
		pcap_write_header();
		if (pcap_fp == stdout) {
			/* Make sure stdout is not line-buffered in a bad way */
			setvbuf(stdout, NULL, _IOFBF, 0);
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
	 * Every source of raw frames this machine has, not one of them, and
	 * -p narrows that to the one source that owns the named port instead
	 * of opening all of them and filtering by name: the kernel for an
	 * entry that names one of its interfaces, ax25netd for the rest, a
	 * WAMPES node's own monitor for a WAMPES entry.  A name in no
	 * axports entry is still the refusal it always was, so a typo is not
	 * silently turned into "monitor everything", and a port whose own
	 * source will not open is that same refusal with its reason - which
	 * is why the error below is no longer only about EINVAL: an EACCES
	 * on a node's monitor socket would otherwise be a run that watches a
	 * different world and says nothing about it.
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
			fprintf(stderr, "listen: raw monitor via libax25 (%s, "
				"framed)\n", axmon_source_name(&mon, s));

	/* -p is answered by the source chosen inside axmon_open() now, so
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
			if (pcap_fp) {
				pcap_write_packet(buffer, size);
				if (pcap_fp == stdout) {
					/* Pipe mode: write only pcap to stdout for tools like
					 * wireshark/tcpdump. */
					continue;
				}
			}
			/*
			 * Quick-and-dirty fix: if -p was given, use it directly
			 * for the label.  axmon_source_name() currently only
			 * returns generic kind strings and sa_data is truncated
			 * to 14 bytes for some monitor sources (WAMPES/AGWPE),
			 * so long logical port names like wampes:kisstcp get
			 * cut off.  Proper fix: extend struct axmon to carry
			 * the full per-source port/name and have
			 * axmon_source_name() return it (Option 1).  This
			 * direct use of listen_port_arg can then go away.
			 */
			if (listen_port_arg)
				lprintf(T_PORT, "%s: ", listen_port_arg);
			else
				display_frame_port(&mon, f, &sa);
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
			/* pad the last line without reading past the data */
			if (length2 > 0) {
				c = *data2++;
				length2--;
				sprintf(buf, "%2.2X ", c);
			} else
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
	unsigned int x;		/* shifting a signed int into bit 31 is undefined */

	x = *cp++;
	x <<= 8;
	x |= *cp++;
	x <<= 8;
	x |= *cp++;
	x <<= 8;
	x |= *cp;

	return x;
}


/* AX25TPCD - interactive frontend to an AGWPE multiplexer
 *
 * ax25tcpd exposes an ax25netd instance as a small TCP and unix domain
 * service.  Each incoming connection is an interactive session: the
 * first line is a command, later bytes are packet data.
 *
 * Front side: one listener per listen line, and a connection in one of
 * two modes that the client chooses:
 *   ascii    line oriented.  Each input line is one packet with its end
 *            of line normalised to CR; messages end with LF.  The
 *            default, because it is what a person at a terminal wants
 *            and what a telnet client speaks.
 *   binary   8 bit clean byte stream.  Remote data is passed through
 *            unchanged.  A datagram is one packet, the whole stream,
 *            split into mtu sized chunks and delivered on close.
 *
 * The mode is a line of its own before the command - "binary" - the
 * way wampes asks for it, so that a script which sends a binary blob
 * has said so before the first byte of it.  A unix socket is ascii
 * unless the client says otherwise; nothing about the transport
 * decides it.
 *
 * TELNET is recognised, not offered.  There is no server side IAC on
 * connect, because that would put three bytes of protocol into the
 * first packet of every binary client.  Instead the first 0xFF in an
 * ascii connection is taken for an IAC and the connection speaks TELNET
 * from there on: a telnet(1) sends IAC IP rather than a 0x03 for the
 * interrupt key, and that first IAC is what tells us.  A binary
 * connection never looks, which is what makes it 8 bit clean.
 *
 * Commands (unambiguous prefixes work):
 *   ascii | binary          the mode, in a line of its own
 *   connect [--silent] [--keep] [--pid=XX] [--port <port>] [--mycall <call>]
 *           <port>[/<chan>]:<dest>[,<digi>,...] [< SRC]
 *   connect [--silent] ...  <dest>[,<digi>,...] [< SRC]
 *           without a port the configured default-port is used, and the
 *           netd resolves a digipeater path itself
 *   datagram [--silent] [--keep] [--pid=XX] [--port <port>] [--mycall <call>]
 *            <port>[/<chan>]:[<dest>[,<digi>,...]] [< SRC]
 *            Without a destination every line is a whole TNC2 frame
 *            ("SRC>DEST,DIGI,...:payload") whose header is its own;
 *            with one, the header is fixed and lines are payload.
 *   quit | bye
 *   help
 *
 * A port and its destination are one argument, "hf/2:DB0AAA", because
 * two arguments cannot say which of them is which: with an autorouter
 * on the far side, "connect hf DB0AAA" reads equally well as port hf to
 * DB0AAA and as a call to hf digipeated by DB0AAA.  The colon settles
 * it, and the form without a port stays the autoroute.
 *
 * The back side is one AGWPE connection to an ax25netd loop port, over
 * TCP or a unix domain socket, using the AGWPE client library.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <syslog.h>
#include <grp.h>
#include <netax25/agwpe.h>
#include <netax25/agwpe_client.h>
#include <netax25/agwpe_config.h>
#include <netax25/axcommon.h>
/* ax25_address, which axconfig.h names in a prototype of its own and does
 * not include the header for.  Before axconfig.h, or the build stops there
 * with a type it has not heard of.  */
#include <netax25/ax25.h>
#include <netax25/axconfig.h>

/* ------------------------------------------------------------------ */

enum {
	TPC_CMD = 0,		/* awaiting a command line */
	TPC_CONNECTING,		/* connect sent, waiting for the 'C' confirm */
	TPC_DATA,		/* connected, byte stream */
	TPC_DGRAM,		/* datagram mode */
};

#define	TPC_MAX_LISTEN	8
#define	TPC_MAX_ADDR	4	/* bound addresses per tcp listen line */
#define	TPC_MAX_CLIENT	128
#define	TPC_LINE_MAX	4096
#define	TPC_DATA_CHUNK	256	/* connected 'D' frame size */
#define	TPC_DEFAULT_MTU	256

/* A connect the netd never answers would keep the client waiting for
 * ever; give up after this many seconds.  */
#define	TPC_CONNECT_TIMEOUT	600	/* 10 min */

/* How long to wait for the netd to answer the version request at startup.
 * A server that never answers is not an AGWPE server (a stray service may
 * hold the port), and every later connect would just sit in CONNECTING.  */
#define	TPC_BACKSIDE_TIMEOUT	5	/* seconds */

/* The front side taken when no configuration file exists or it opens no
 * listener: one tcp port on every loopback address.  "localhost" (not a
 * literal 127.0.0.1) makes the listener cover both IPv4 and IPv6
 * loopback.  */
#define	TPC_DEFAULT_LISTEN_ADDR	"localhost"
#define	TPC_DEFAULT_LISTEN_PORT	8202

/* One port entry learned from the 'G' reply: the flat port byte and the
 * upstream name.  The lowest channel of an upstream is its base; the
 * "name/chan" syntax adds the channel number to that base.  */
struct tpc_port {
	unsigned char	port;
	char		name[AGWPE_UPSTREAM_NAME_MAX];
};

struct tpc_listen {
	int		fd[TPC_MAX_ADDR];	/* bound listening sockets */
	int		nfd;
	int		unix_sock;
	char		addr[64];	/* tcp bind address */
	int		port;
	char		path[108];	/* unix socket path */
	int		group_mode;
	char		group_name[64];
	mode_t		dir_mode;	/* mode of the socket's directory */
};

/* TELNET negotiation state of a connection that was recognised as
 * TELNET.  */
enum {
	TPC_TN_DATA,		/* normal data */
	TPC_TN_IAC,		/* saw IAC, expecting a command */
	TPC_TN_OPT,		/* saw WILL/WONT/DO/DONT, expecting option */
	TPC_TN_SB,		/* inside a subnegotiation */
	TPC_TN_SB_IAC,		/* inside a subnegotiation, saw IAC */
};

struct tpc_client {
	int		fd;
	int		state;
	int		ascii;		/* line oriented endpoint, the default;
					 * binary is the other one and is
					 * what "binary" on a line of its
					 * own selects */
	int		telnet;		/* a 0xFF was seen: TELNET from
					 * there on */
	int		may_telnet;	/* a 0xFF can mean anything: only
					 * a tcp stream looks, never a unix
					 * socket, where no telnet client
					 * is what the reader is */
	int		tn_state;
	unsigned char	tn_cmd;
	int		ascii_cr;	/* the last byte put on the wire in ascii
					 * mode was a CR, so that a CRLF split
					 * across two chunks does not turn into
					 * two CRs and a blank line on the far
					 * side */
	int		silent;
	int		keep;
	int		registered;	/* call_from registered on the netd */
	time_t		connect_deadline;	/* 0 = not connecting */
	int		dgram_tnc2;	/* datagram lines are TNC2 frames */
	unsigned char	pid;
	unsigned char	port;		/* resolved flat port */
	char		call_from[AGWPE_MAX_CALL];
	char		call_to[AGWPE_MAX_CALL];
	char		digis[AGWPE_MAX_DIGIS - 1][AGWPE_MAX_CALL];
	int		ndigis;

	unsigned char	*rbuf;		/* line accumulator */
	size_t		rlen, rcap;
	unsigned char	*dbuf;		/* pending packet data */
	size_t		dlen, dcap;
};

static struct {
	struct tpc_listen	listen[TPC_MAX_LISTEN];
	int			nlisten;
	int			mtu;
	char			default_call[AGWPE_MAX_CALL];

	/* The port a command that names none gets.  Empty means there is
	 * none, and a command without a port is refused rather than sent
	 * to a port nobody chose: the netd needs a port to pick an
	 * upstream, and "loop" would be a guess.  */
	char			default_port[64];

	int			target_tcp;	/* 0 = unix socket */
	int			target_set;	/* explicit 'target' line */
	char			target_host[64];
	int			target_port;
	char			target_sock[108];

	agwpe_client_t		*netd;
	int			backside_ok;	/* netd answered at startup */

	struct tpc_client	clients[TPC_MAX_CLIENT];

	struct tpc_port		ports[AGWPE_PORT_MAX];
	int			nports;

	int			verbose;	/* --verbose: say the decisions */
	int			debug;
	int			foreground;
} tpc;

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static void tpc_log(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (tpc.foreground || tpc.debug) {
		fprintf(stderr, "ax25tcpd: ");
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	} else {
		vsyslog(prio, fmt, ap);
	}
	va_end(ap);
}

/*
 * The decisions this daemon makes, on the same terms as ax25netd's
 * --verbose: off by default, because a line per connection is a lot for a
 * daemon that may carry many, and on it is what an operator turns to when
 * wondering whether a client got as far as the netd.  Repeatable, like the
 * other daemons'; the second one is the -d trace.
 */
static void tpc_verbose(const char *fmt, ...)
{
	va_list ap;

	if (!tpc.verbose)
		return;

	va_start(ap, fmt);
	if (tpc.foreground || tpc.debug) {
		fprintf(stderr, "ax25tcpd: ");
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	} else {
		vsyslog(LOG_INFO, fmt, ap);
	}
	va_end(ap);
}

/* Defined with the netd callbacks, below: this one is the other half of
 * tpc_parse_port_spec() and sits next to it.  */
static void tpc_client_printf(struct tpc_client *cl, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static void tpc_upper(char *s)
{
	for (; *s; s++)
		*s = toupper((unsigned char)*s);
}

/* Why the last tpc_parse_port_spec() said no, when it knows more than "no
 * such port".  Empty means it has nothing to add.
 *
 * A file-static rather than an argument because there are five call sites
 * that all do the same thing with the answer - refuse, and say why - and
 * threading a buffer through each of them to hold a sentence that is empty
 * in most cases would make the reason harder to see, not easier.
 */
static char tpc_port_why[160];

/* Resolve <port>[/<chan>] into a flat port byte.
 *
 * Three things know about port names and they are asked in this order:
 *
 *   the netd's own 'G' table, which is the server this daemon is actually
 *   connected to and the only authority on its numbering;
 *
 *   the library, through ax25_port_info(), which knows the axports names
 *   that the 'G' table does not carry - it lists upstream names, so
 *   "radio0" is not in it even when radio0 works for "call radio0".  That
 *   gap is the one this function exists to close: the answer was "no such
 *   port" for a port that existed.
 *
 *   and a plain number, which has always been accepted and is what a script
 *   that knows the answer wants.
 *
 * A name the library claims for the kernel or for a WAMPES node is refused
 * by name rather than quietly refused: this daemon is a pure AGWPE client
 * and has no way to send on either, and "no such port" is not the reason.
 */
static int tpc_parse_port_spec(const char *spec, unsigned char *port)
{
	const char *chan = strchr(spec, '/');
	char num[64], name[80];
	size_t len;
	int n = 0, p;
	struct ax25_port_info info;

	tpc_port_why[0] = '\0';

	if (chan != NULL) {
		char *end;
		long v;

		errno = 0;
		v = strtol(chan + 1, &end, 10);
		/* The whole of what follows the slash, and a number: a
		 * channel is one digit's worth of a channel number and
		 * "hf/DB0AAA" is a typo, not channel 0.  */
		if (errno != 0 || *end != '\0' || end == chan + 1 ||
		    v < 0 || v > 15) {
			snprintf(tpc_port_why, sizeof(tpc_port_why),
				 "a channel is a number, 0 to 15, not '%s'",
				 chan + 1);
			return -1;
		}
		n = (int)v;
		len = chan - spec;
	} else {
		len = strlen(spec);
	}
	if (len == 0 || len >= sizeof(num)) {
		snprintf(tpc_port_why, sizeof(tpc_port_why),
			 len == 0 ? "no port name before the slash" :
				"the port name is too long");
		return -1;
	}
	memcpy(num, spec, len);
	num[len] = '\0';

	if (strcasecmp(num, "loop") == 0) {
		*port = AGWPE_PORT_LOOP;
		return 0;
	}

	/* The connected server first: its table is the numbering that will
	 * actually be used, and a machine with a netd on another host has a
	 * library that would answer about a different one.
	 */
	for (p = 0; p < tpc.nports; p++)
		if (strcmp(tpc.ports[p].name, num) == 0) {
			*port = tpc.ports[p].port + n;
			return 0;
		}

	/* What axports and the backends make of the name.  Asked with the
	 * channel attached in the spelling axports uses, so that the
	 * library is the one place that decides what a channel is.
	 */
	if (chan != NULL)
		snprintf(name, sizeof(name), "%s:%d", num, n);
	else
		snprintf(name, sizeof(name), "%s", num);

	if (ax25_port_info(name, &info) == 0) {
		switch (info.backend) {
		case AX25_PORT_AGWPE:
			*port = (unsigned char)info.port;
			return 0;
		case AX25_PORT_KERNEL:
			snprintf(tpc_port_why, sizeof(tpc_port_why),
				 "it is a kernel AX.25 port, and this daemon "
				 "sends through ax25netd only");
			return -1;
		case AX25_PORT_WAMPES:
			snprintf(tpc_port_why, sizeof(tpc_port_why),
				 "it is a WAMPES port of node \"%s\", and "
				 "this daemon sends through ax25netd only",
				 info.node);
			return -1;
		default:
			break;	/* nobody claims it: fall through */
		}
	}

	for (p = 0; num[p]; p++)
		if (!isdigit((unsigned char)num[p]))
			return -1;
	n += atoi(num);
	if (n < 0 || n > 255)
		return -1;
	*port = n;
	return 0;
}

/* Say that a port was refused, with the reason when there is one that is
 * not "no such port".  Every refusal of a name goes through here, so that
 * the reasons are not left to whichever call site remembered to add one.
 */
static void tpc_port_refused(struct tpc_client *cl, const char *spec)
{
	if (tpc_port_why[0] != '\0')
		tpc_client_printf(cl, "*** ERROR: port '%s': %s\r\n", spec,
				  tpc_port_why);
	else
		tpc_client_printf(cl, "*** ERROR: no such port '%s'\r\n", spec);
}

/* The same, for the configured default-port.  Its own wording because the
 * name in it is not one the client typed: quoting it as the client's would
 * put a word in the client's mouth that it never used, and the client is
 * the one who will be reading the next thing they type.
 */
static void tpc_default_port_refused(struct tpc_client *cl)
{
	if (tpc_port_why[0] != '\0')
		tpc_client_printf(cl, "*** ERROR: default-port '%s': %s\r\n",
				  tpc.default_port, tpc_port_why);
	else
		tpc_client_printf(cl,
				  "*** ERROR: default-port '%s' is not a "
				  "known port\r\n", tpc.default_port);
}

static int tpc_parse_pid(const char *spec, unsigned char *pid)
{
	if (strcasecmp(spec, "text") == 0 || strcasecmp(spec, "ax25") == 0)
		*pid = AGWPE_PID_AX25;
	else if (strcasecmp(spec, "netrom") == 0)
		*pid = AGWPE_PID_NETROM;
	else if (strcasecmp(spec, "rose") == 0)
		*pid = AGWPE_PID_ROSE;
	else if (strcasecmp(spec, "flexnet") == 0 ||
		 strcasecmp(spec, "texnet") == 0)
		*pid = AGWPE_PID_TEXNET;
	else if (strcasecmp(spec, "l3") == 0)
		*pid = AGWPE_PID_L3;
	else {
		char *end;
		long v;

		errno = 0;
		v = strtol(spec, &end, 0);
		if (errno != 0 || *end != '\0' || v < 0 || v > 255)
			return -1;
		*pid = (unsigned char)v;
	}
	return 0;
}

static void tpc_parse_group(const char *arg, int *mode, char *name, size_t nlen)
{
	if (strcasecmp(arg, "all") == 0)
		*mode = AGWPE_GROUP_ALL;
	else if (strcasecmp(arg, "default") == 0)
		*mode = AGWPE_GROUP_DEFAULT;
	else {
		*mode = AGWPE_GROUP_NAMED;
		strncpy(name, arg, nlen - 1);
		name[nlen - 1] = '\0';
	}
}

static int tpc_read_config(const char *path)
{
	FILE *fp;
	char line[512];
	int lineno = 0;

	fp = fopen(path, "r");
	if (fp == NULL) {
		/* No configuration file is not an error: the defaults
		 * apply (target from ax25common.conf, one binary tcp
		 * front port, mtu 256).  Any other open failure is.  */
		if (errno == ENOENT)
			return 0;
		return -1;
	}

	while (fgets(line, sizeof(line), fp) != NULL) {
		char *p, *key, *tok[4];
		int ntok = 0;

		lineno++;
		p = line;
		while (*p && isspace((unsigned char)*p))
			p++;
		if (*p == '\0' || *p == '#')
			continue;
		key = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		if (*p)
			*p++ = '\0';
		while (ntok < 4) {
			while (*p && isspace((unsigned char)*p))
				p++;
			if (*p == '\0' || *p == '#')
				break;
			tok[ntok] = p;
			while (*p && !isspace((unsigned char)*p))
				p++;
			if (*p)
				*p++ = '\0';
			ntok++;
		}

		if (strcmp(key, "target") == 0) {
			tpc.target_set = 1;
			if (ntok < 2) {
				tpc_log(LOG_ERR, "%s:%d: target needs two arguments",
					path, lineno);
				fclose(fp);
				return -1;
			}
			if (strcmp(tok[0], "tcp") == 0) {
				tpc.target_tcp = 1;
				strncpy(tpc.target_host, tok[1], sizeof(tpc.target_host) - 1);
				tpc.target_host[sizeof(tpc.target_host) - 1] = '\0';
				if (ntok < 3) {
					tpc_log(LOG_ERR, "%s:%d: target tcp needs a port",
						path, lineno);
					fclose(fp);
					return -1;
				}
				tpc.target_port = atoi(tok[2]);
				if (tpc.target_port < 1 || tpc.target_port > 65535) {
					tpc_log(LOG_ERR, "%s:%d: bad target port %s",
						path, lineno, tok[2]);
					fclose(fp);
					return -1;
				}
			} else if (strcmp(tok[0], "socket") == 0) {
				tpc.target_tcp = 0;
				if (strlen(tok[1]) >= sizeof(tpc.target_sock)) {
					tpc_log(LOG_ERR, "%s:%d: target socket path too long",
						path, lineno);
					fclose(fp);
					return -1;
				}
				strncpy(tpc.target_sock, tok[1], sizeof(tpc.target_sock) - 1);
			} else {
				tpc_log(LOG_ERR, "%s:%d: unknown target transport %s",
					path, lineno, tok[0]);
				fclose(fp);
				return -1;
			}
		} else if (strcmp(key, "listen") == 0) {
			struct tpc_listen *l;

			if (ntok < 2) {
				tpc_log(LOG_ERR, "%s:%d: listen needs a transport",
					path, lineno);
				fclose(fp);
				return -1;
			}
			if (tpc.nlisten >= TPC_MAX_LISTEN) {
				tpc_log(LOG_ERR, "%s:%d: too many listen lines",
					path, lineno);
				fclose(fp);
				return -1;
			}
			l = &tpc.listen[tpc.nlisten++];
			memset(l, 0, sizeof(*l));
			if (strcmp(tok[0], "tcp") == 0) {
				if (ntok < 3) {
					tpc_log(LOG_ERR, "%s:%d: listen tcp needs an address and a port",
						path, lineno);
					fclose(fp);
					return -1;
				}
				strncpy(l->addr, tok[1], sizeof(l->addr) - 1);
				l->addr[sizeof(l->addr) - 1] = '\0';
				l->port = atoi(tok[2]);
				if (l->port < 1 || l->port > 65535) {
					tpc_log(LOG_ERR, "%s:%d: bad listen port %s",
						path, lineno, tok[2]);
					fclose(fp);
					return -1;
				}
				/* No fourth token any more, and saying so is
				 * better than ignoring it: an old file with
				 * a text port in it would otherwise come
				 * up with one port fewer than the sysop
				 * believes, and the second port would be
				 * something else or nothing.  */
				if (ntok >= 4) {
					tpc_log(LOG_ERR, "%s:%d: listen tcp takes "
						"an address and a port only; "
						"there is no text port any more, "
						"a client asks for ascii or binary",
						path, lineno);
					fclose(fp);
					return -1;
				}
			} else if (strcmp(tok[0], "unix") == 0) {
				l->unix_sock = 1;
				if (strlen(tok[1]) >= sizeof(l->path)) {
					tpc_log(LOG_ERR, "%s:%d: listen unix path too long",
						path, lineno);
					fclose(fp);
					return -1;
				}
				strncpy(l->path, tok[1], sizeof(l->path) - 1);
				if (ntok >= 4 && strcmp(tok[2], "group") == 0)
					tpc_parse_group(tok[3], &l->group_mode,
							l->group_name,
							sizeof(l->group_name));
			} else {
				tpc_log(LOG_ERR, "%s:%d: unknown listen transport %s",
					path, lineno, tok[0]);
				fclose(fp);
				return -1;
			}
		} else if (strcmp(key, "mtu") == 0) {
			if (ntok < 1) {
				tpc_log(LOG_ERR, "%s:%d: mtu needs a value",
					path, lineno);
				fclose(fp);
				return -1;
			}
			tpc.mtu = atoi(tok[0]);
			if (tpc.mtu < 16 || tpc.mtu > 4096) {
				tpc_log(LOG_ERR, "%s:%d: bad mtu %s",
					path, lineno, tok[0]);
				fclose(fp);
				return -1;
			}
		} else if (strcmp(key, "default-call") == 0) {
			if (ntok < 1) {
				tpc_log(LOG_ERR, "%s:%d: default-call needs a call",
					path, lineno);
				fclose(fp);
				return -1;
			}
			strncpy(tpc.default_call, tok[0], sizeof(tpc.default_call) - 1);
			tpc.default_call[sizeof(tpc.default_call) - 1] = '\0';
			tpc_upper(tpc.default_call);
		} else if (strcmp(key, "default-port") == 0) {
			if (ntok < 1) {
				tpc_log(LOG_ERR, "%s:%d: default-port needs a port",
					path, lineno);
				fclose(fp);
				return -1;
			}
			if (strlen(tok[0]) >= sizeof(tpc.default_port)) {
				tpc_log(LOG_ERR, "%s:%d: default-port name too long",
					path, lineno);
				fclose(fp);
				return -1;
			}
			strncpy(tpc.default_port, tok[0],
				sizeof(tpc.default_port) - 1);
			tpc.default_port[sizeof(tpc.default_port) - 1] = '\0';
		} else {
			tpc_log(LOG_ERR, "%s:%d: unknown directive %s",
				path, lineno, key);
			fclose(fp);
			return -1;
		}
	}
	fclose(fp);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Listeners                                                           */
/* ------------------------------------------------------------------ */

/*
 * Create the parent directory of a unix socket path.  Every missing
 * component is created, since the default path is
 * /var/run/ax25/sockets/ax25tcpd.sock and a single mkdir(2) of the last
 * component fails while /var/run/ax25 is missing.  mode comes from
 * ax25common.conf's "loop mode" and is shared with ax25netd, so the two
 * daemons cannot end up with different modes on the same directory
 * depending on which one started first.  An existing directory is left
 * alone, not chmodded and not chowned, so a mode set by an
 * administrator stays as it is.
 */
static int tpc_mkdir_parent(const char *path, mode_t mode)
{
	char *dup, *p, *slash;
	struct stat st;
	int rc = 0;

	if (path[0] == '\0')
		return -1;

	dup = strdup(path);
	if (dup == NULL)
		return -1;

	slash = strrchr(dup, '/');
	if (slash == NULL) {
		free(dup);
		return 0;			/* relative to cwd */
	}
	if (slash == dup) {
		free(dup);
		return 0;			/* the socket is in "/" */
	}
	*slash = '\0';

	for (p = dup + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (stat(dup, &st) == 0) {
			if (!S_ISDIR(st.st_mode)) {
				errno = ENOTDIR;
				rc = -1;
			}
		} else if (errno != ENOENT) {
			rc = -1;
		} else if (mkdir(dup, mode) != 0) {
			rc = -1;
		} else if (chmod(dup, mode) != 0) {
			/* mkdir(2) applies the umask, and the configured
			 * mode is meant to be the mode, not a suggestion
			 * the umask gets to reduce.  */
			rc = -1;
		}
		*p = '/';
		if (rc < 0)
			break;
	}

	/* The last component has no trailing separator of its own.  */
	if (rc == 0 && stat(dup, &st) == 0) {
		if (!S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			rc = -1;
		}
	} else if (rc == 0 && errno == ENOENT) {
		if (mkdir(dup, mode) != 0 || chmod(dup, mode) != 0)
			rc = -1;
	} else if (rc == 0) {
		rc = -1;
	}

	if (rc < 0) {
		int e = errno;

		free(dup);
		errno = e;
		return -1;
	}
	free(dup);
	return 0;
}

/* Bind every address the listen line resolves to, so that "localhost"
 * opens both the IPv4 and the IPv6 loopback.  An address that cannot be
 * bound (a family the host does not have, a port already taken) is
 * reported and skipped; the line fails only when none could be bound.  */
static int tpc_listen_tcp(struct tpc_listen *l)
{
	struct addrinfo hints, *res, *rp;
	char service[16];
	int on = 1, e;

	l->nfd = 0;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	snprintf(service, sizeof(service), "%d", l->port);

	e = getaddrinfo(l->addr, service, &hints, &res);
	if (e != 0) {
		tpc_log(LOG_ERR, "cannot resolve %s: %s", l->addr,
			gai_strerror(e));
		return -1;
	}
	for (rp = res; rp != NULL && l->nfd < TPC_MAX_ADDR; rp = rp->ai_next) {
		int s;

		if (rp->ai_family != AF_INET && rp->ai_family != AF_INET6)
			continue;
		s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (s < 0)
			continue;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		if (bind(s, rp->ai_addr, rp->ai_addrlen) != 0) {
			tpc_log(LOG_WARNING, "cannot bind tcp %s:%d: %s",
				l->addr, l->port, strerror(errno));
			close(s);
			continue;
		}
		if (listen(s, 16) != 0) {
			tpc_log(LOG_WARNING, "cannot listen on tcp %s:%d: %s",
				l->addr, l->port, strerror(errno));
			close(s);
			continue;
		}
		fcntl(s, F_SETFL, O_NONBLOCK);
		l->fd[l->nfd++] = s;
		tpc_log(LOG_INFO, "listening on tcp %s:%d", l->addr, l->port);
	}
	freeaddrinfo(res);
	return l->nfd == 0 ? -1 : 0;
}

static int tpc_listen_unix(const char *path, int group_mode,
			   const char *group_name, mode_t dir_mode)
{
	struct sockaddr_un sa;
	struct stat st;
	gid_t gid = getgid();
	mode_t sock_mode = 0660;
	int s;

	if (strlen(path) >= sizeof(sa.sun_path)) {
		tpc_log(LOG_ERR, "unix socket path too long: %s", path);
		return -1;
	}

	if (group_mode == AGWPE_GROUP_ALL) {
		sock_mode = 0666;
	} else if (group_mode == AGWPE_GROUP_NAMED) {
		long num;
		char *end;
		struct group *gr;

		errno = 0;
		num = strtol(group_name, &end, 10);
		if (errno == 0 && *end == '\0' && num > 0 && num < 65536)
			gid = (gid_t)num;
		else {
			gr = getgrnam(group_name);
			if (gr == NULL) {
				tpc_log(LOG_ERR, "no such group '%s' for the unix socket",
					group_name);
				return -1;
			}
			gid = gr->gr_gid;
		}
	}

	if (tpc_mkdir_parent(path, dir_mode) != 0) {
		tpc_log(LOG_ERR, "cannot create directory for %s: %s",
			path, strerror(errno));
		return -1;
	}

	/* A stale socket from a previous run blocks the bind.  Only
	 * remove it when it really is a socket, never another file.  */
	if (lstat(path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			tpc_log(LOG_ERR,
				"%s exists and is not a socket; not removing it",
				path);
			return -1;
		}
		if (unlink(path) != 0) {
			tpc_log(LOG_ERR, "cannot remove stale socket %s: %s",
				path, strerror(errno));
			return -1;
		}
	} else if (errno != ENOENT) {
		tpc_log(LOG_ERR, "cannot inspect %s: %s", path, strerror(errno));
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) {
		tpc_log(LOG_ERR, "unix socket: %s", strerror(errno));
		return -1;
	}
	if (bind(s, (struct sockaddr *)&sa, SUN_LEN(&sa)) != 0 ||
	    listen(s, 16) != 0) {
		tpc_log(LOG_ERR, "cannot listen on %s: %s", path,
			strerror(errno));
		close(s);
		return -1;
	}
	/* Set the permissions through the path: macOS does not allow
	 * fchmod(2)/fchown(2) on socket descriptors, while chmod(2) on
	 * the path works everywhere.  */
	if (chmod(path, sock_mode) != 0 ||
	    (geteuid() == 0 && chown(path, getuid(), gid) != 0)) {
		tpc_log(LOG_ERR, "cannot set ownership of %s: %s",
			path, strerror(errno));
		close(s);
		unlink(path);
		return -1;
	}
	fcntl(s, F_SETFL, O_NONBLOCK);
	tpc_log(LOG_INFO, "listening on unix socket %s", path);
	return s;
}

/* ------------------------------------------------------------------ */
/* Clients                                                             */
/* ------------------------------------------------------------------ */

/* Give the client's source call back to the netd.  A registration left
 * behind would keep the call owned by this frontend and route later
 * frames for it here instead of to whoever uses the call next.  */
static void tpc_client_unregister(struct tpc_client *cl)
{
	if (cl->registered && tpc.netd != NULL) {
		agwpe_client_unregister(tpc.netd, cl->port, cl->call_from);
		cl->registered = 0;
	}
}

static void tpc_client_remove(struct tpc_client *cl)
{
	/* The one place a front-side connection ends, so the one place to
	 * say so.  A "quit" command has already unregistered the client by
	 * the time the caller notices, so logging where the fd is still
	 * valid is the only way the line can name the client that went.  */
	if (cl->fd >= 0)
		tpc_verbose("client %d: gone", cl->fd);
	tpc_client_unregister(cl);
	if (cl->fd >= 0)
		close(cl->fd);
	cl->fd = -1;
	free(cl->rbuf);
	cl->rbuf = NULL;
	free(cl->dbuf);
	cl->dbuf = NULL;
	cl->rlen = cl->dlen = 0;
}

static struct tpc_client *tpc_client_add(int fd, int may_telnet)
{
	struct tpc_client *cl;

	for (cl = tpc.clients; cl < tpc.clients + TPC_MAX_CLIENT; cl++)
		if (cl->fd < 0)
			break;
	if (cl == tpc.clients + TPC_MAX_CLIENT)
		return NULL;
	memset(cl, 0, sizeof(*cl));
	cl->fd = fd;
	cl->state = TPC_CMD;
	cl->ascii = 1;		/* the default, before any "binary" line */
	cl->telnet = 0;
	cl->may_telnet = may_telnet;
	cl->pid = AGWPE_PID_AX25;
	cl->silent = 0;
	cl->keep = 0;
	return cl;
}

static int tpc_write_all(int fd, const unsigned char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n;

		n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += n;
	}
	return 0;
}

static void tpc_client_printf(struct tpc_client *cl, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0 || cl->fd < 0)
		return;
	if (cl->ascii) {
		/* Ascii: every line of ours ends with LF.  Drop the
		 * carriage return of each CRLF, and double a 0xFF if this
		 * connection turned out to speak TELNET - an unescaped one
		 * would reach the client as the start of a command.  */
		char out[sizeof(buf) * 2];
		char *d = out;
		int i;

		for (i = 0; i < n; i++) {
			if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n')
				continue;
			if (cl->telnet && (unsigned char)buf[i] == 0xff)
				*d++ = (char)0xff;
			*d++ = buf[i];
		}
		tpc_write_all(cl->fd, (unsigned char *)out, (size_t)(d - out));
	} else {
		/* Binary: 8 bit clean, packet radio lines end with CR.
		 * Drop the line feed of each CRLF and pass every byte,
		 * 0xFF included, exactly as it is.  */
		char out[sizeof(buf)];
		char *d = out;
		int i;

		for (i = 0; i < n; i++) {
			if (buf[i] == '\n' && i > 0 && buf[i - 1] == '\r')
				continue;
			*d++ = buf[i];
		}
		tpc_write_all(cl->fd, (unsigned char *)out, (size_t)(d - out));
	}
}

static void tpc_prompt(struct tpc_client *cl)
{
	tpc_client_printf(cl, "> ");
}

/* ------------------------------------------------------------------ */
/* Port table                                                          */
/* ------------------------------------------------------------------ */

/* Parse a 'G' reply: "count;PortN name: desc;..." plus a trailing NUL,
 * exactly as ax25netd and Direwolf build it.  */
static void tpc_port_parse(const unsigned char *data, size_t len)
{
	char buf[4096];
	char *tok, *save = NULL;
	int first = 1;

	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	memcpy(buf, data, len);
	buf[len] = '\0';

	tpc.nports = 0;
	tok = strtok_r(buf, ";", &save);
	while (tok != NULL) {
		char *p, *q;
		int n;

		if (first) {
			first = 0;		/* the count */
		} else if (strncmp(tok, "Port", 4) == 0) {
			p = tok + 4;
			n = (int)strtol(p, &q, 10);
			if (q != p && *q == ' ' && n >= 1 && n <= AGWPE_PORT_LOOP + 1) {
				p = q + 1;
				q = strchr(p, ':');
				if (q != NULL && q != p) {
					*q = '\0';
					if (tpc.nports < AGWPE_PORT_MAX &&
					    strlen(p) < sizeof(tpc.ports[0].name)) {
						tpc.ports[tpc.nports].port =
							(unsigned char)(n - 1);
						strcpy(tpc.ports[tpc.nports].name, p);
						tpc.nports++;
					}
				}
			}
		}
		tok = strtok_r(NULL, ";", &save);
	}
}

/* ------------------------------------------------------------------ */
/* netd callbacks                                                      */
/* ------------------------------------------------------------------ */

/* Defined with the TELNET decoder below and used from tpc_on_connection()
 * as well as from the read loop, so it is announced here.  */
static void tpc_session_send(struct tpc_client *cl, const unsigned char *data,
			     size_t len);

static struct tpc_client *tpc_client_find(const struct agwpe_s *hdr,
					  int want_connecting)
{
	struct tpc_client *cl;

	for (cl = tpc.clients; cl < tpc.clients + TPC_MAX_CLIENT; cl++) {
		if (cl->fd < 0 || cl->state == TPC_CMD)
			continue;
		if (want_connecting >= 0 &&
		    (cl->state == TPC_CONNECTING) != want_connecting)
			continue;
		if (cl->port != hdr->port)
			continue;
		if (strcmp(cl->call_from, hdr->call_to) != 0)
			continue;
		return cl;
	}
	return NULL;
}

static void tpc_on_raw_frame(agwpe_client_t *c, const struct agwpe_s *hdr,
			     const unsigned char *data, size_t len)
{
	(void)c;
	tpc.backside_ok = 1;
	if (tpc.debug && hdr->datakind != AGWPE_DK_DATA)
		tpc_log(LOG_DEBUG, "netd: kind='%c' port=%u from=%.10s to=%.10s len=%zu",
			hdr->datakind, hdr->port, hdr->call_from, hdr->call_to,
			len);
	if (hdr->datakind == AGWPE_DK_PORTS)
		tpc_port_parse(data, len);
}

static void tpc_strip_eol(char *m)
{
	size_t l = strlen(m);

	if (l > 0 && m[l - 1] == '\n')
		m[--l] = '\0';
	if (l > 0 && m[l - 1] == '\r')
		m[--l] = '\0';
}

static void tpc_on_connection(agwpe_client_t *c, const struct agwpe_s *hdr,
			      const char *msg)
{
	struct tpc_client *cl = tpc_client_find(hdr, 1);
	char m[128];

	(void)c;
	if (cl == NULL) {
		if (tpc.debug)
			tpc_log(LOG_DEBUG,
				"connect confirm without a waiting client: port=%u from=%.10s to=%.10s",
				hdr->port, hdr->call_from, hdr->call_to);
		return;
	}
	tpc_verbose("client %d: connected to %s on port %u",
		    cl->fd, cl->call_to, hdr->port);
	if (!cl->silent) {
		if (msg != NULL && msg[0] != '\0') {
			strncpy(m, msg, sizeof(m) - 1);
			m[sizeof(m) - 1] = '\0';
			tpc_strip_eol(m);
			tpc_client_printf(cl, "%s\r\n", m);
		} else {
			tpc_client_printf(cl, "*** CONNECTED to %s\r\n",
					  cl->call_to);
		}
	}
	cl->state = TPC_DATA;
	cl->connect_deadline = 0;

	/* Flush anything that arrived while connecting.  It goes out through
	 * the same path as everything after it, so the first packet of a
	 * session is framed like the second one and not like the raw bytes a
	 * script happened to write in the same packet as the command.  */
	if (cl->dlen > 0) {
		tpc_session_send(cl, cl->dbuf, cl->dlen);
		cl->dlen = 0;
	}
}

static void tpc_on_disconnect(agwpe_client_t *c, const struct agwpe_s *hdr,
			      const char *msg)
{
	struct tpc_client *cl = tpc_client_find(hdr, -1);
	char m[128];

	(void)c;
	if (cl == NULL || cl->state == TPC_DGRAM)
		return;
	tpc_verbose("client %d: disconnected from %s on port %u",
		    cl->fd, cl->call_to, hdr->port);
	if (msg != NULL && msg[0] != '\0') {
		strncpy(m, msg, sizeof(m) - 1);
		m[sizeof(m) - 1] = '\0';
		tpc_strip_eol(m);
	} else {
		m[0] = '\0';
	}
	if (!cl->silent) {
		if (m[0] != '\0')
			tpc_client_printf(cl, "%s\r\n", m);
		else
			tpc_client_printf(cl, "*** DISCONNECTED\r\n");
	}
	/* The session is over, so the source call is free again whether
	 * this client keeps its connection or not.  */
	tpc_client_unregister(cl);
	if (cl->state == TPC_CONNECTING) {
		cl->state = TPC_CMD;
		tpc_prompt(cl);
	} else if (cl->keep) {
		cl->state = TPC_CMD;
		tpc_prompt(cl);
	} else {
		tpc_client_remove(cl);
	}
}

static void tpc_on_data(agwpe_client_t *c, const struct agwpe_s *hdr,
			const unsigned char *data, size_t len)
{
	struct tpc_client *cl = tpc_client_find(hdr, 0);
	unsigned char buf[1024];
	size_t off, o;

	(void)c;
	if (cl == NULL || cl->state != TPC_DATA)
		return;
	if (!cl->ascii) {
		/* Binary: 8 bit clean, the remote's CR is passed through
		 * unchanged and so is everything else.  */
		if (tpc_write_all(cl->fd, data, len) != 0)
			tpc_client_remove(cl);
		return;
	}
	/* Ascii: telnet friendly, CR and CRLF become LF.  A telnet client
	 * must not see a raw 0xFF, escape it as IAC IAC - and only on a
	 * connection that was recognised as TELNET, which is the same
	 * condition under which binary was refused.  */
	for (off = 0; off < len; ) {
		for (o = 0; o < sizeof(buf) && off < len; ) {
			if (data[off] == '\r') {
				buf[o++] = '\n';
				if (off + 1 < len && data[off + 1] == '\n')
					off++;
			} else if (data[off] == 0xff && cl->telnet) {
				if (o + 2 > sizeof(buf))
					break;
				buf[o++] = 0xff;
				buf[o++] = 0xff;
			} else {
				buf[o++] = data[off];
			}
			off++;
		}
		if (tpc_write_all(cl->fd, buf, o) != 0) {
			tpc_client_remove(cl);
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void tpc_send_unproto(struct tpc_client *cl, const unsigned char *data,
			     size_t len)
{
	const char *digv[AGWPE_MAX_DIGIS - 1];
	int i;

	for (i = 0; i < cl->ndigis; i++)
		digv[i] = cl->digis[i];

	if (cl->ndigis > 0)
		agwpe_client_send_unproto_via(tpc.netd, cl->port, cl->pid,
					      cl->call_from, cl->call_to,
					      digv, cl->ndigis, data, len);
	else
		agwpe_client_send_unproto(tpc.netd, cl->port, cl->pid,
					  cl->call_from, cl->call_to,
					  data, len);
}

/* One datagram line is a whole TNC2 frame: "SRC>DEST,DIGI,...:payload".
 * The source and the path come out of the data, not out of the command;
 * a trailing '*' on a path element marks it as already-repeated and is
 * dropped.  A malformed line is always worth saying, even under
 * --silent; an empty payload is simply not a frame and goes quietly.  */
static void tpc_send_unproto_tnc2(struct tpc_client *cl, char *line)
{
	const char *digv[AGWPE_MAX_DIGIS - 1];
	char srcbuf[AGWPE_MAX_CALL], destbuf[AGWPE_MAX_CALL];
	char digibuf[AGWPE_MAX_DIGIS - 1][AGWPE_MAX_CALL];
	char *payload, *path, *digi, *star;
	int ndigis = 0, first = 1, i;

	while (isspace((unsigned char)*line))
		line++;
	if (*line == '\0')
		return;

	payload = strchr(line, ':');
	if (payload == NULL) {
		tpc_client_printf(cl, "*** ERROR: no colon in \"%s\"\r\n", line);
		return;
	}
	*payload++ = '\0';
	if (*payload == '\0')
		return;			/* no payload is no frame */

	path = strchr(line, '>');
	if (path == NULL) {
		tpc_client_printf(cl, "*** ERROR: no '>' in the header\r\n");
		return;
	}
	*path++ = '\0';
	if (strlen(line) >= sizeof(srcbuf)) {
		tpc_client_printf(cl, "*** ERROR: bad source call\r\n");
		return;
	}
	strcpy(srcbuf, line);
	tpc_upper(srcbuf);

	destbuf[0] = '\0';
	for (digi = strtok(path, ","); digi != NULL;
	     digi = strtok(NULL, ",")) {
		star = strchr(digi, '*');
		if (star != NULL)
			*star = '\0';
		if (first) {
			first = 0;
			if (strlen(digi) >= sizeof(destbuf)) {
				tpc_client_printf(cl, "*** ERROR: bad destination call\r\n");
				return;
			}
			strcpy(destbuf, digi);
		} else if (ndigis < AGWPE_MAX_DIGIS - 1) {
			strncpy(digibuf[ndigis], digi,
				sizeof(digibuf[0]) - 1);
			digibuf[ndigis][sizeof(digibuf[0]) - 1] = '\0';
			ndigis++;
		} else {
			tpc_client_printf(cl, "*** ERROR: too many digipeaters\r\n");
			return;
		}
	}
	if (first || destbuf[0] == '\0') {
		tpc_client_printf(cl, "*** ERROR: no destination\r\n");
		return;
	}
	tpc_upper(destbuf);
	for (i = 0; i < ndigis; i++)
		tpc_upper(digibuf[i]);

	for (i = 0; i < ndigis; i++)
		digv[i] = digibuf[i];

	if (ndigis > 0)
		agwpe_client_send_unproto_via(tpc.netd, cl->port, cl->pid,
					      srcbuf, destbuf, digv, ndigis,
					      (const unsigned char *)payload,
					      strlen(payload));
	else
		agwpe_client_send_unproto(tpc.netd, cl->port, cl->pid,
					  srcbuf, destbuf,
					  (const unsigned char *)payload,
					  strlen(payload));
}

static void tpc_dgram_flush(struct tpc_client *cl)
{
	size_t off = 0;

	while (off < cl->dlen) {
		size_t n = cl->dlen - off;

		if (n > (size_t)tpc.mtu)
			n = (size_t)tpc.mtu;
		tpc_send_unproto(cl, cl->dbuf + off, n);
		off += n;
	}
	cl->dlen = 0;
}

static int tpc_add_call(struct tpc_client *cl, const char *call)
{
	size_t n;

	if (cl->call_to[0] == '\0') {
		if (strlen(call) >= sizeof(cl->call_to))
			return -1;
		strcpy(cl->call_to, call);
		return 0;
	}
	if (cl->ndigis >= AGWPE_MAX_DIGIS - 1)
		return -1;
	n = strnlen(call, sizeof(cl->digis[0]) - 1);
	memcpy(cl->digis[cl->ndigis], call, n);
	cl->digis[cl->ndigis][n] = '\0';
	cl->ndigis++;
	return 0;
}

/* The destination and the digis, split as "DB0AAA-8 DB0BBB DB0CCC" or
 * "DB0AAA-8,DB0BBB,DB0CCC" or a mixture - commas separate a path exactly
 * as spaces do, and each word may itself carry commas.  spec[0] is the
 * destination, the rest are digipeaters.  */
static int tpc_parse_dest(struct tpc_client *cl, char *const *spec, int nspec)
{
	char buf[256], *p, *q;
	int i;

	cl->call_to[0] = '\0';
	cl->ndigis = 0;
	for (i = 0; i < nspec; i++) {
		if (strlen(spec[i]) >= sizeof(buf))
			return -1;
		strcpy(buf, spec[i]);
		p = buf;
		while ((q = strchr(p, ',')) != NULL) {
			*q = '\0';
			if (tpc_add_call(cl, p) != 0)
				return -1;
			p = q + 1;
		}
		if (*p != '\0' && tpc_add_call(cl, p) != 0)
			return -1;
	}
	return (cl->call_to[0] != '\0') ? 0 : -1;
}

/* One positional argument, "<port>[/<chan>]:<dest>" or "<dest>".
 *
 * A port and its destination are one argument because two arguments
 * cannot say which is which.  The far side has an autorouter, so
 * "connect DB0AAA" means "call DB0AAA, the netd finds the path", and
 * "connect hf DB0AAA" then reads equally well as port hf to DB0AAA and
 * as a call to hf digipeated by DB0AAA.  The colon settles it and the
 * form without a port keeps the autoroute.
 *
 * A slash inside means a channel of that port and nowhere else - a
 * callsign has no slash - so it is unambiguous even without the colon.
 *
 * npos is the number of positional arguments, and it is here for one
 * reason only: to recognise the old two-argument spelling.  "connect hf
 * DB0AAA" cannot be read as anything else once the port table is known -
 * hf is a port name and there is a second argument - so it is refused by
 * name rather than silently going somewhere the client did not mean.  */
static int tpc_split_target(struct tpc_client *cl, char *arg, int npos,
			    const char *next, char **destp)
{
	char *colon, *port_end = NULL;

	/* The first colon, always.  A callsign has no colon in it and
	 * neither has a channel number, so the first one is the boundary
	 * and looking further would only misread a path.  A slash before
	 * it belongs to the port ("hf/2:DB0AAA"), a slash after it would
	 * be nonsense and is left to the callsign parser.  */
	colon = strchr(arg, ':');
	if (colon != NULL) {
		*colon = '\0';
		port_end = colon + 1;
	} else {
		/* No port part, so the whole argument is the
		 * destination.  Not NULL: the check for an empty
		 * destination below reads it, and only the branch
		 * that sets it to the configured default is allowed
		 * to decide what the port is.  */
		port_end = arg;
	}

	if (colon != NULL) {
		if (tpc_parse_port_spec(arg, &cl->port) != 0) {
			tpc_port_refused(cl, arg);
			return -1;
		}
		if (*port_end == '\0') {
			tpc_client_printf(cl, "*** ERROR: no destination "
					  "after ':'\r\n");
			return -1;
		}
	} else {
		unsigned char tmp;

		if (npos >= 2 && tpc_parse_port_spec(arg, &tmp) == 0) {
			/* The next argument is quoted as it was typed, so
			 * that the message can be pasted after fixing the
			 * one character it is about.  */
			tpc_client_printf(cl, "*** ERROR: the port and the "
					  "destination are one argument: "
					  "use '%s:%s'\r\n",
					  arg, next);
			return -1;
		}

		/* No port given: the configured default, or a refusal.
		 * A bare "loop" is a port and takes the colon like any
		 * other, so what arrives here is a destination.  */
		if (tpc.default_port[0] == '\0') {
			tpc_client_printf(cl, "*** ERROR: no port given and no "
					  "default-port configured (try "
					  "'%s')\r\n", arg);
			return -1;
		}
		if (tpc_parse_port_spec(tpc.default_port, &cl->port) != 0) {
			tpc_default_port_refused(cl);
			return -1;
		}
	}

	*destp = port_end;
	return 0;
}

static int tpc_cmd_connect(struct tpc_client *cl, int argc, char **argv)
{
	char *dest = NULL, *src = NULL;
	char *pos[8];
	int npos = 0, i, silent = 0, keep = 0, port_given = 0;
	const char *usage = "usage: connect [--silent] [--keep] [--pid=XX] [--port <port>] [--mycall <call>] [<port>[/<chan>]:]<dest>[,<digi>,...] [< SRC]";

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--silent") == 0) {
			silent = 1;
		} else if (strcmp(argv[i], "--keep") == 0) {
			keep = 1;
		} else if (strncmp(argv[i], "--pid=", 6) == 0) {
			if (tpc_parse_pid(argv[i] + 6, &cl->pid) != 0) {
				tpc_client_printf(cl, "*** ERROR: bad pid '%s'\r\n",
						  argv[i] + 6);
				return 0;
			}
		} else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			/* An alternative spelling, kept because a script
			 * that builds the port from a variable finds the
			 * colon awkward.  It is the same resolution.  */
			if (tpc_parse_port_spec(argv[i + 1], &cl->port) != 0) {
				tpc_port_refused(cl, argv[i + 1]);
				return 0;
			}
			i++;
			port_given = 1;
		} else if (strcmp(argv[i], "--mycall") == 0) {
			if (src == NULL && i + 1 < argc)
				src = argv[++i];
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (strncmp(argv[i], "--mycall=", 9) == 0) {
			if (src == NULL && argv[i][9] != '\0')
				src = argv[i] + 9;
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (argv[i][0] == '-' && argv[i][1] == '-') {
			tpc_client_printf(cl, "*** ERROR: unknown option '%s'\r\n",
					  argv[i]);
			return 0;
		} else if (strcmp(argv[i], "<") == 0) {
			if (src == NULL && i + 1 < argc)
				src = argv[++i];
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (npos < 8) {
			pos[npos++] = argv[i];
		} else {
			tpc_client_printf(cl, "*** ERROR: too many arguments\r\n");
			return 0;
		}
	}

	if (npos < 1) {
		tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
		return 0;
	}

	if (cl->state != TPC_CMD) {
		tpc_client_printf(cl, "*** ERROR: already busy\r\n");
		return 0;
	}
	if (port_given) {
		/* --port already resolved the port.  A leading colon is
		 * accepted and ignored, because a script with the port in
		 * a variable will write one whether or not this is the
		 * invocation that needs it.  */
		if (*pos[0] == ':') {
			pos[0]++;
			if (*pos[0] == '\0') {
				tpc_client_printf(cl, "*** ERROR: no "
						  "destination\r\n");
				return 0;
			}
		}
	} else {
		/* In place: the colon becomes a NUL so that the
		 * destination is the same argument, and tpc_parse_dest()
		 * splits it and any further path arguments exactly as it
		 * always has.  */
		if (tpc_split_target(cl, pos[0], npos, npos > 1 ? pos[1] : "",
					  &dest) != 0)
			return 0;
		pos[0] = dest;
	}
	dest = pos[0];
	if (tpc_parse_dest(cl, pos, npos) != 0) {
		tpc_client_printf(cl, "*** ERROR: bad destination '%s'\r\n", dest);
		return 0;
	}
	tpc_upper(cl->call_to);
	for (i = 0; i < cl->ndigis; i++)
		tpc_upper(cl->digis[i]);

	if (src != NULL) {
		if (strlen(src) >= sizeof(cl->call_from)) {
			tpc_client_printf(cl, "*** ERROR: bad source call '%s'\r\n", src);
			return 0;
		}
		strcpy(cl->call_from, src);
		tpc_upper(cl->call_from);
	} else if (tpc.default_call[0] != '\0') {
		strcpy(cl->call_from, tpc.default_call);
	} else {
		tpc_client_printf(cl, "*** ERROR: source call required, use ' < call'\r\n");
		return 0;
	}

	cl->silent = silent;
	cl->keep = keep;

	/* Register our own call on the target port so the netd routes
	 * the connect confirm and later frames back to us.  */
	agwpe_client_register(tpc.netd, cl->port, cl->call_from);
	cl->registered = 1;
	if (tpc.debug)
		tpc_log(LOG_DEBUG, "connect: %s -> %s port %u pid 0x%02x",
			cl->call_from, cl->call_to, cl->port, cl->pid);
	tpc_verbose("client %d: connect %s -> %s on port %u (pid 0x%02x)",
		    cl->fd, cl->call_from, cl->call_to, cl->port, cl->pid);
	if (cl->ndigis > 0) {
		const char *digv[AGWPE_MAX_DIGIS - 1];

		for (i = 0; i < cl->ndigis; i++)
			digv[i] = cl->digis[i];
		agwpe_client_connect_via(tpc.netd, cl->port, cl->pid,
					 cl->call_from, cl->call_to,
					 digv, cl->ndigis);
	} else {
		agwpe_client_connect(tpc.netd, cl->port, cl->pid,
				     cl->call_from, cl->call_to);
	}
	cl->state = TPC_CONNECTING;
	cl->connect_deadline = time(NULL) + TPC_CONNECT_TIMEOUT;
	if (!silent)
		tpc_client_printf(cl, "*** CONNECTING to %s\r\n", cl->call_to);
	return 0;
}

static int tpc_cmd_datagram(struct tpc_client *cl, int argc, char **argv)
{
	char *dest = NULL, *src = NULL;
	char *pos[8];
	int npos = 0, i, silent = 0, keep = 0, port_given = 0;
	const char *usage = "usage: datagram [--silent] [--keep] [--pid=XX] [--port <port>] [--mycall <call>] [<port>[/<chan>]:][<dest>[,<digi>,...]] [< SRC]";

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--silent") == 0) {
			silent = 1;
		} else if (strcmp(argv[i], "--keep") == 0) {
			keep = 1;
		} else if (strncmp(argv[i], "--pid=", 6) == 0) {
			if (tpc_parse_pid(argv[i] + 6, &cl->pid) != 0) {
				tpc_client_printf(cl, "*** ERROR: bad pid '%s'\r\n",
						  argv[i] + 6);
				return 0;
			}
		} else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			if (tpc_parse_port_spec(argv[i + 1], &cl->port) != 0) {
				tpc_port_refused(cl, argv[i + 1]);
				return 0;
			}
			i++;
			port_given = 1;
		} else if (strcmp(argv[i], "--mycall") == 0) {
			if (src == NULL && i + 1 < argc)
				src = argv[++i];
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (strncmp(argv[i], "--mycall=", 9) == 0) {
			if (src == NULL && argv[i][9] != '\0')
				src = argv[i] + 9;
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (argv[i][0] == '-' && argv[i][1] == '-') {
			tpc_client_printf(cl, "*** ERROR: unknown option '%s'\r\n",
					  argv[i]);
			return 0;
		} else if (strcmp(argv[i], "<") == 0) {
			if (src == NULL && i + 1 < argc)
				src = argv[++i];
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (npos < 8) {
			pos[npos++] = argv[i];
		} else {
			tpc_client_printf(cl, "*** ERROR: too many arguments\r\n");
			return 0;
		}
	}

	if (cl->state != TPC_CMD) {
		tpc_client_printf(cl, "*** ERROR: already busy\r\n");
		return 0;
	}

	/*
	 * A datagram may name no destination at all: every line is then a
	 * whole TNC2 frame and carries its own header.  That is the one
	 * case where the target argument may be missing, so the port has
	 * to come from somewhere else - the default-port, or --port.  A
	 * datagram command with neither is refused here rather than
	 * sending frames from an unchosen port.
	 */
	if (npos == 0) {
		if (!port_given) {
			if (tpc.default_port[0] == '\0') {
				tpc_client_printf(cl, "*** ERROR: no port "
						  "given and no default-port "
						  "configured\r\n");
				return 0;
			}
			if (tpc_parse_port_spec(tpc.default_port,
						&cl->port) != 0) {
				tpc_default_port_refused(cl);
				return 0;
			}
		}
		cl->silent = silent;
		cl->keep = keep;
		cl->dgram_tnc2 = 1;
		cl->state = TPC_DGRAM;
		tpc_verbose("client %d: datagram mode, each line is a TNC2 "
			    "frame", cl->fd);
		return 0;
	}

	if (port_given) {
		if (*pos[0] == ':') {
			pos[0]++;
			if (*pos[0] == '\0') {
				tpc_client_printf(cl, "*** ERROR: no "
						  "destination\r\n");
				return 0;
			}
		}
	} else {
		if (tpc_split_target(cl, pos[0], npos, npos > 1 ? pos[1] : "",
					  &dest) != 0)
			return 0;
		pos[0] = dest;
	}
	dest = pos[0];

	cl->silent = silent;
	cl->keep = keep;

	if (tpc_parse_dest(cl, pos, npos) != 0) {
		tpc_client_printf(cl, "*** ERROR: bad destination '%s'\r\n", dest);
		return 0;
	}
	tpc_upper(cl->call_to);
	for (i = 0; i < cl->ndigis; i++)
		tpc_upper(cl->digis[i]);

	if (src != NULL) {
		if (strlen(src) >= sizeof(cl->call_from)) {
			tpc_client_printf(cl, "*** ERROR: bad source call '%s'\r\n", src);
			return 0;
		}
		strcpy(cl->call_from, src);
		tpc_upper(cl->call_from);
	} else if (tpc.default_call[0] != '\0') {
		strcpy(cl->call_from, tpc.default_call);
	} else {
		tpc_client_printf(cl, "*** ERROR: source call required, use ' < call'\r\n");
		return 0;
	}

	tpc_verbose("client %d: datagram %s -> %s on port %u",
		    cl->fd, cl->call_from, cl->call_to, cl->port);
	cl->state = TPC_DGRAM;
	return 0;
}

static int tpc_cmd_quit(struct tpc_client *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	tpc_client_printf(cl, "bye\r\n");
	tpc_client_remove(cl);
	return 1;		/* caller must not touch cl again */
}

static int tpc_cmd_help(struct tpc_client *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	tpc_client_printf(cl,
		"commands: ascii, binary, connect, datagram, quit, bye, help\r\n"
		"ascii | binary         mode, on a line of its own (default ascii)\r\n"
		"connect [<port>[/<chan>]:]<dest>[,<digi>,...] [< SRC]\r\n"
		"datagram [<port>[/<chan>]:][<dest>[,<digi>,...]] [< SRC]\r\n"
		"    no <dest>: each line is a TNC2 frame SRC>DEST,path:payload\r\n"
		"    no <port>: the configured default-port, netd autoroutes\r\n"
		"options: --silent --keep --pid=XX --port <port> --mycall <call>\r\n");
	return 0;
}

/* The mode, on a line of its own before the command - the way wampes
 * asks for it, so that a script sending a binary blob has said so
 * before the first byte of it rather than after.
 *
 * "binary" also gives up TELNET detection, because that is what 8 bit
 * clean means here: a 0xFF in the payload is a 0xFF.  Asking for binary
 * again afterwards is refused rather than silently half-honoured, since
 * a client that has done that has a binary stream we would now eat
 * bytes from.  */
static int tpc_cmd_ascii(struct tpc_client *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (cl->state != TPC_CMD) {
		tpc_client_printf(cl, "*** ERROR: already busy\r\n");
		return 0;
	}
	cl->ascii = 1;
	tpc_client_printf(cl, "*** ascii\r\n");
	return 0;
}

static int tpc_cmd_binary(struct tpc_client *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (cl->state != TPC_CMD) {
		tpc_client_printf(cl, "*** ERROR: already busy\r\n");
		return 0;
	}
	/* 8 bit clean means a 0xFF in the payload stays a 0xFF, and a
	 * TELNET connection has already been told what a 0xFF is.  A
	 * client that gets here has a stream we would eat bytes from,
	 * so it is refused rather than half honoured.  */
	if (cl->telnet) {
		tpc_client_printf(cl, "*** ERROR: already speaking TELNET, "
				  "binary is too late\r\n");
		return 0;
	}
	cl->ascii = 0;
	tpc_client_printf(cl, "*** binary\r\n");
	return 0;
}

static const struct tpc_cmd {
	const char	*name;
	int		(*fn)(struct tpc_client *, int, char **);
} tpc_commands[] = {
	{ "ascii",	tpc_cmd_ascii },
	{ "binary",	tpc_cmd_binary },
	{ "connect",	tpc_cmd_connect },
	{ "datagram",	tpc_cmd_datagram },
	{ "quit",	tpc_cmd_quit },
	{ "bye",	tpc_cmd_quit },
	{ "help",	tpc_cmd_help },
};

static int tpc_command_run(struct tpc_client *cl, char *line)
{
	char *argv[24];
	int argc = 0, i, match = 0;
	const struct tpc_cmd *cmdp = NULL;

	while (argc < 24) {
		while (*line && isspace((unsigned char)*line))
			line++;
		if (*line == '\0')
			break;
		argv[argc++] = line;
		while (*line && !isspace((unsigned char)*line))
			line++;
		if (*line)
			*line++ = '\0';
	}
	if (argc == 0) {
		tpc_prompt(cl);
		return 0;
	}

	for (i = 0; i < (int)(sizeof(tpc_commands) / sizeof(tpc_commands[0])); i++)
		if (strncmp(tpc_commands[i].name, argv[0],
			    strlen(argv[0])) == 0) {
			match++;
			cmdp = &tpc_commands[i];
		}

	if (match == 0) {
		tpc_client_printf(cl, "*** ERROR: unknown command '%s' (help for a list)\r\n",
				  argv[0]);
	} else if (match > 1) {
		tpc_client_printf(cl, "*** ERROR: ambiguous command '%s'\r\n", argv[0]);
	} else {
		int r = cmdp->fn(cl, argc - 1, argv + 1);

		if (r)
			return 1;	/* client gone */
	}
	if (cl->fd >= 0 && cl->state == TPC_CMD)
		tpc_prompt(cl);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Client input                                                        */
/* ------------------------------------------------------------------ */

/* Return the length of a complete line in buf, or -1 if a complete line
 * needs more data.  The line does not include the terminator; a
 * trailing '\r' of a CRLF pair is part of the line for the caller to
 * strip.  *consumed is the number of bytes to drop.  */
static ssize_t tpc_line_len(const unsigned char *buf, size_t len, size_t *consumed)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (buf[i] == '\n' || buf[i] == '\r') {
			if (buf[i] == '\r' && i + 1 == len)
				return -1;	/* wait for a following LF */
			*consumed = i + 1;
			if (buf[i] == '\r' && i + 1 < len && buf[i + 1] == '\n')
				(*consumed)++;
			return (ssize_t)i;
		}
	}
	return -1;
}

/* Refuse a TELNET request we do not implement, so that a telnet client
 * gives up negotiating instead of trying again and again.  */
static void tpc_telnet_reply(struct tpc_client *cl, unsigned char cmd,
			     unsigned char opt)
{
	unsigned char r[3];

	if (cmd != 0xfb && cmd != 0xfd)		/* only WILL and DO */
		return;
	r[0] = 0xff;				/* IAC */
	r[1] = (cmd == 0xfd) ? 0xfc : 0xfe;	/* DO -> WONT, WILL -> DONT */
	r[2] = opt;
	tpc_write_all(cl->fd, r, sizeof(r));
}

/* TELNET filter for the byte stream of a connection that speaks TELNET.
 * A telnet client does not send 0x03 for ^C but an IAC IP command; turn that
 * back into a real Ctrl-C so the interrupt reaches the remote shell.  IAC IAC
 * is a literal 0xFF and the negotiation commands are refused.  Returns
 * the number of application bytes in out.
 *
 * The state is on the client, not local, because a TELNET connection can
 * have one IAC in one read() and its command byte in the next, and two such
 * halves must not be taken for two separate streams.  */
static size_t tpc_telnet_decode(struct tpc_client *cl,
				const unsigned char *in, size_t len,
				unsigned char *out, size_t outsz)
{
	size_t i, o = 0;

	for (i = 0; i < len && o < outsz; i++) {
		unsigned char b = in[i];

		switch (cl->tn_state) {
		case TPC_TN_DATA:
			if (b == 0xff)
				cl->tn_state = TPC_TN_IAC;
			else
				out[o++] = b;
			break;
		case TPC_TN_IAC:
			if (b == 0xff) {		/* escaped 0xFF */
				out[o++] = 0xff;
				cl->tn_state = TPC_TN_DATA;
			} else if (b == 0xfa) {		/* IAC SB */
				cl->tn_state = TPC_TN_SB;
			} else if (b >= 0xfb && b <= 0xfe) {
				cl->tn_cmd = b;		/* WILL/WONT/DO/DONT */
				cl->tn_state = TPC_TN_OPT;
			} else {
				if (b == 0xf4)		/* IAC IP */
					out[o++] = 0x03;
				cl->tn_state = TPC_TN_DATA;
			}
			break;
		case TPC_TN_OPT:
			tpc_telnet_reply(cl, cl->tn_cmd, b);
			cl->tn_state = TPC_TN_DATA;
			break;
		case TPC_TN_SB:
			if (b == 0xff)
				cl->tn_state = TPC_TN_SB_IAC;
			break;
		case TPC_TN_SB_IAC:
			/* IAC SE ends the subnegotiation, IAC IAC is a
			 * literal 0xFF and anything else is data again.  */
			cl->tn_state = (b == 0xf0) ?
				TPC_TN_DATA : TPC_TN_SB;
			break;
		}
	}
	return o;
}

/* Everything a chunk of client bytes goes through on its way to the netd:
 * TELNET detection, TELNET decoding, the line ending a radio frame wants,
 * and the split into 'D' frames.  It is one function because there are two
 * ways in, and the two have to do the same thing.
 *
 * The first is the ordinary one: bytes arrive while the session is up and go
 * straight out.  The second is the bytes that arrived in the same read() as
 * the connect command and had to wait in cl->dbuf for the confirm - a
 * script does not wait for a reply before it writes its payload, so this is
 * normal traffic and not an edge case.  When that flush was done by the
 * confirm handler on its own it sent the buffer as it stood, and the first
 * packet of a session went out with a bare LF where every packet after it
 * had a CR, and with its IAC pairs still doubled.
 *
 * The detection is here rather than at the top of the read loop because it
 * has to happen in stream order.  A scan of whatever read() happened to
 * return decides out of order: a script that sends "binary" and its payload
 * in one write() has both in the same buffer, and the scan finds the
 * payload's 0xFF before the "binary" line has been parsed and refuses a
 * command that was still legal when it was typed.
 *
 * A command line cannot turn a connection into TELNET, and that is
 * deliberate.  No command carries a 0xFF, and the only byte a telnet client
 * sends before there is a session is the interrupt key, which at the prompt
 * means nothing.  The promise is about a session, and this is what a session
 * sends through.
 *
 * Binary is 8 bit clean, which is the whole of what it claims and the whole
 * of why it is worth a line of its own: the bytes go out exactly as they
 * came in, a 0xFF in a payload stays a 0xFF, and nothing is looked for.
 */
static void tpc_session_send(struct tpc_client *cl, const unsigned char *data,
			     size_t len)
{
	unsigned char dec[TPC_LINE_MAX];
	unsigned char out[TPC_LINE_MAX];
	size_t o, off;

	if (cl->ascii && cl->may_telnet && !cl->telnet &&
	    memchr(data, 0xff, len) != NULL)
		cl->telnet = 1;

	/* Decode before translating the line ending: a TELNET IAC IP has to
	 * be a 0x03 before the CR pass sees it, or the CR pass would pass a
	 * protocol byte through as if it were the user's interrupt.  */
	if (cl->ascii && cl->telnet) {
		len = tpc_telnet_decode(cl, data, len, dec, sizeof(dec));
		data = dec;
	}

	if (!cl->ascii) {
		memcpy(out, data, len);
		o = len;
	} else {
		/* Packet radio lines end with CR, so a CRLF or a bare LF
		 * becomes one CR.  The "was the last byte a CR" test is a
		 * field on the client rather than a look at out[o - 1],
		 * because a CRLF can be split across two chunks and each
		 * chunk is a separate read() or a separate buffer - and a
		 * second CR would be a blank line on the far side.
		 *
		 * It counts a CR the client typed itself and not only one
		 * this loop produced, so "one\r\n" is one CR and not two:
		 * the client is allowed to send either ending, and both
		 * have to arrive as the one the radio wants.  */
		for (o = 0, off = 0; off < len; off++) {
			if (data[off] == '\n') {
				if (cl->ascii_cr)
					continue;
				out[o++] = '\r';
				cl->ascii_cr = 1;
			} else {
				out[o++] = data[off];
				cl->ascii_cr = data[off] == '\r';
			}
		}
	}

	for (off = 0; off < o; ) {
		size_t c = o - off;

		if (c > TPC_DATA_CHUNK)
			c = TPC_DATA_CHUNK;
		agwpe_client_send_data(tpc.netd, cl->port, cl->pid,
				       cl->call_from, cl->call_to,
				       out + off, c);
		off += c;
	}
}

static int tpc_client_readable(struct tpc_client *cl)
{
	unsigned char tmp[4096];
	unsigned char app[sizeof(tmp)];
	ssize_t n;

	for (;;) {
		const unsigned char *data;
		size_t dn;

		n = read(cl->fd, tmp, sizeof(tmp));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			break;
		}
		if (n == 0)
			break;
		/*
		 * TELNET is decoded here when it is already known, and
		 * detected further down where the bytes are about to be
		 * sent to the netd.  The detection is not at the top of
		 * this loop on purpose: it has to happen in stream order,
		 * and a scan of whatever read() happened to return decides
		 * things out of order.  A script that sends "binary" and
		 * its payload in one write() has both in this buffer, and a
		 * scan here would find the payload's 0xFF before the
		 * "binary" line had been parsed, refusing a command that
		 * was still legal when it was typed.
		 *
		 * See the comment at the detection for the rest.
		 */
		if (cl->telnet) {
			dn = tpc_telnet_decode(cl, tmp, (size_t)n,
					       app, sizeof(app));
			data = app;
		} else {
			dn = (size_t)n;
			data = tmp;
		}
		if (cl->state == TPC_DATA) {
			tpc_session_send(cl, data, dn);
			continue;
		}
		if (cl->state == TPC_CONNECTING) {
			/* Hold until the connect is confirmed.  */
			if (cl->dlen + dn > cl->dcap) {
				size_t ncap = cl->dcap ? cl->dcap * 2 : 4096;

				while (cl->dlen + dn > ncap)
					ncap *= 2;
				cl->dbuf = realloc(cl->dbuf, ncap);
				if (cl->dbuf == NULL)
					break;
				cl->dcap = ncap;
			}
			memcpy(cl->dbuf + cl->dlen, data, dn);
			cl->dlen += dn;
			continue;
		}
		if (cl->state == TPC_DGRAM && !cl->ascii && !cl->dgram_tnc2) {
			/* Binary datagram: accumulate, flush on close.  */
			if (cl->dlen + dn > cl->dcap) {
				size_t ncap = cl->dcap ? cl->dcap * 2 : 4096;

				while (cl->dlen + dn > ncap)
					ncap *= 2;
				cl->dbuf = realloc(cl->dbuf, ncap);
				if (cl->dbuf == NULL)
					break;
				cl->dcap = ncap;
			}
			memcpy(cl->dbuf + cl->dlen, data, dn);
			cl->dlen += dn;
			continue;
		}

		/* CMD and datagram: accumulate and take lines.  */
		if (cl->rlen + dn > TPC_LINE_MAX * 4) {
			/* Never lets a line grow past the limit.  */
			cl->rlen = 0;
			if (cl->state == TPC_CMD)
				tpc_client_printf(cl, "*** ERROR: line too long\r\n");
			continue;
		}
		if (cl->rlen + dn > cl->rcap) {
			size_t ncap = cl->rcap ? cl->rcap * 2 : 1024;

			while (cl->rlen + dn > ncap)
				ncap *= 2;
			if (ncap > TPC_LINE_MAX * 4)
				ncap = TPC_LINE_MAX * 4;
			cl->rbuf = realloc(cl->rbuf, ncap);
			if (cl->rbuf == NULL)
				break;
			cl->rcap = ncap;
		}
		memcpy(cl->rbuf + cl->rlen, data, dn);
		cl->rlen += dn;

		for (;;) {
			size_t consumed;
			ssize_t llen;
			char line[TPC_LINE_MAX + 1];

			llen = tpc_line_len(cl->rbuf, cl->rlen, &consumed);
			if (llen < 0)
				break;
			if ((size_t)llen >= sizeof(line)) {
				/* Overlong line: drop the whole thing.  */
				memmove(cl->rbuf, cl->rbuf + consumed + (size_t)llen,
					cl->rlen - (consumed + (size_t)llen));
				cl->rlen -= consumed + (size_t)llen;
				if (cl->state == TPC_CMD)
					tpc_client_printf(cl,
						"*** ERROR: line too long\r\n");
				continue;
			}
			memcpy(line, cl->rbuf, (size_t)llen);
			line[llen] = '\0';
			if (llen > 0 && line[llen - 1] == '\r')
				line[llen - 1] = '\0';
			memmove(cl->rbuf, cl->rbuf + consumed,
				cl->rlen - consumed);
			cl->rlen -= consumed;

			if (cl->state == TPC_CMD) {
				if (tpc_command_run(cl, line))
					return 1;	/* client gone */
				if (cl->fd < 0)
					return 1;

				/*
				 * Whatever the client sent after the command
				 * line is already in the buffer, because a
				 * script does not wait for a reply before
				 * writing its payload.  Where those bytes
				 * belong depends on what the command just
				 * did, and taking them as another line is
				 * wrong in two of the three cases:
				 *
				 *   binary datagram  the bytes are payload,
				 *     and a line would cut them at the next
				 *     newline
				 *   connect          the bytes are payload
				 *     waiting for the confirm, and TPC_
				 *     CONNECTING is not handled below, so
				 *     they were dropped on the floor - a
				 *     script that sent command and payload
				 *     in one write() got a session that
				 *     silently ate the first packet
				 *   ascii datagram    the bytes are lines and
				 *     the loop below is right
				 *
				 * So the two non-line cases are taken out
				 * here and the loop is left to the one that
				 * wants lines.
				 */
				if (cl->rlen > 0 &&
				    (cl->state == TPC_CONNECTING ||
				     (cl->state == TPC_DGRAM && !cl->ascii))) {
					if (cl->dlen + cl->rlen > cl->dcap) {
						size_t ncap = cl->dcap ?
							cl->dcap * 2 : 4096;

						while (cl->dlen + cl->rlen > ncap)
							ncap *= 2;
						cl->dbuf = realloc(cl->dbuf, ncap);
						if (cl->dbuf == NULL) {
							cl->rlen = 0;
							break;
						}
						cl->dcap = ncap;
					}
					memcpy(cl->dbuf + cl->dlen, cl->rbuf,
					       cl->rlen);
					cl->dlen += cl->rlen;
					cl->rlen = 0;
					break;
				}
			} else if (cl->state == TPC_DGRAM) {
				if (cl->dgram_tnc2) {
					tpc_send_unproto_tnc2(cl, line);
				} else if (line[0] != '\0') {
					tpc_send_unproto(cl, (unsigned char *)line,
							 strlen(line));
				} else if (!cl->silent) {
					tpc_client_printf(cl,
						"*** ERROR: empty packet\r\n");
				}
			}
		}
	}
	return 1;		/* EOF or fatal error */
}

/* The netd never confirmed or refused the connect.  Tear the pending
 * link down, give the source call back and let the client try again.  */
static void tpc_connect_timeout(struct tpc_client *cl)
{
	cl->connect_deadline = 0;
	tpc_verbose("client %d: connect to %s timed out", cl->fd, cl->call_to);
	if (!cl->silent)
		tpc_client_printf(cl, "*** CONNECT to %s timed out\r\n",
				  cl->call_to);
	agwpe_client_disconnect(tpc.netd, cl->port,
				cl->call_from, cl->call_to);
	tpc_client_unregister(cl);
	cl->state = TPC_CMD;
	tpc_prompt(cl);
}

static void tpc_client_gone(struct tpc_client *cl)
{
	if (cl->state == TPC_DATA || cl->state == TPC_CONNECTING)
		agwpe_client_disconnect(tpc.netd, cl->port,
					cl->call_from, cl->call_to);
	else if (cl->state == TPC_DGRAM && cl->dlen > 0)
		tpc_dgram_flush(cl);
	tpc_client_remove(cl);
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

static void tpc_accept(struct tpc_listen *l, int lfd)
{
	int fd;

	for (;;) {
		fd = accept(lfd, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				tpc_log(LOG_WARNING, "accept: %s", strerror(errno));
			return;
		}
		/* The client read loop drains until EAGAIN and so needs a
		 * non-blocking descriptor.  Linux does not inherit the flag
		 * from the listening socket, BSD/macOS does.  */
		{
			int fl = fcntl(fd, F_GETFL);

			if (fl >= 0)
				fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		}
		/* A unix socket is never TELNET: the reader is a program on
		 * this machine, not telnet(1), and a 0xFF in its stream is
		 * data.  A tcp stream is watched for one, so telnet(1) is
		 * recognised when it sends its first IAC instead of being
		 * told about the rules in bytes it never asked for.  */
		if (tpc_client_add(fd, !l->unix_sock) == NULL) {
			static const unsigned char full[] = "*** ERROR: server full\r\n";

			tpc_verbose("client: refused, all %d slots are taken",
				    TPC_MAX_CLIENT);
			tpc_write_all(fd, full, sizeof(full) - 1);
			close(fd);
		} else {
			tpc_verbose("client %d: connected on the %s side", fd,
				    l->unix_sock ? "unix" : "tcp");
		}
	}
}

static void tpc_loop(void)
{
	for (;;) {
		fd_set rfds;
		int maxfd = -1, nfds, i, netd_fd = agwpe_client_fd(tpc.netd);
		struct timeval tv, *tvp = NULL;
		time_t now, next;

		FD_ZERO(&rfds);
		for (i = 0; i < tpc.nlisten; i++) {
			int k;

			for (k = 0; k < tpc.listen[i].nfd; k++) {
				FD_SET(tpc.listen[i].fd[k], &rfds);
				if (tpc.listen[i].fd[k] > maxfd)
					maxfd = tpc.listen[i].fd[k];
			}
		}
		FD_SET(netd_fd, &rfds);
		if (netd_fd > maxfd)
			maxfd = netd_fd;
		for (i = 0; i < TPC_MAX_CLIENT; i++)
			if (tpc.clients[i].fd >= 0) {
				FD_SET(tpc.clients[i].fd, &rfds);
				if (tpc.clients[i].fd > maxfd)
					maxfd = tpc.clients[i].fd;
			}

		/* Wake up in time for the earliest pending connect.  */
		now = time(NULL);
		next = 0;
		for (i = 0; i < TPC_MAX_CLIENT; i++) {
			struct tpc_client *cl = &tpc.clients[i];

			if (cl->fd < 0 || cl->state != TPC_CONNECTING ||
			    cl->connect_deadline == 0)
				continue;
			if (next == 0 || cl->connect_deadline < next)
				next = cl->connect_deadline;
		}
		if (next != 0) {
			long left = (long)(next - now);

			tv.tv_sec = left > 0 ? left : 0;
			tv.tv_usec = 0;
			tvp = &tv;
		}

		nfds = select(maxfd + 1, &rfds, NULL, NULL, tvp);
		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			tpc_log(LOG_ERR, "select: %s", strerror(errno));
			return;
		}

		if (nfds > 0) {
			for (i = 0; i < tpc.nlisten; i++) {
				int k;

				for (k = 0; k < tpc.listen[i].nfd; k++)
					if (FD_ISSET(tpc.listen[i].fd[k], &rfds))
						tpc_accept(&tpc.listen[i],
							   tpc.listen[i].fd[k]);
			}

			if (FD_ISSET(netd_fd, &rfds)) {
				if (agwpe_client_recv(tpc.netd) < 0) {
					tpc_log(LOG_ERR, "connection to the netd lost: %s",
						strerror(agwpe_client_err(tpc.netd)));
					return;
				}
			}

			for (i = 0; i < TPC_MAX_CLIENT; i++)
				if (tpc.clients[i].fd >= 0 &&
				    FD_ISSET(tpc.clients[i].fd, &rfds)) {
					struct tpc_client *cl = &tpc.clients[i];

					if (tpc_client_readable(cl))
						tpc_client_gone(cl);
				}
		}

		now = time(NULL);
		for (i = 0; i < TPC_MAX_CLIENT; i++) {
			struct tpc_client *cl = &tpc.clients[i];

			if (cl->fd >= 0 && cl->state == TPC_CONNECTING &&
			    cl->connect_deadline != 0 &&
			    now >= cl->connect_deadline)
				tpc_connect_timeout(cl);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

static void tpc_unlink_sockets(void)
{
	int i;

	for (i = 0; i < tpc.nlisten; i++)
		if (tpc.listen[i].unix_sock) {
			int k;

			for (k = 0; k < tpc.listen[i].nfd; k++)
				close(tpc.listen[i].fd[k]);
			tpc.listen[i].nfd = 0;
			unlink(tpc.listen[i].path);
		}
}

static void tpc_sigterm(int sig)
{
	(void)sig;
	tpc_unlink_sockets();
	_exit(0);
}

static int tpc_daemonize(void)
{
	pid_t pid;
	int fd;

	fflush(NULL);
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);
	if (setsid() < 0)
		return -1;
	fd = open("/dev/null", O_RDWR);
	if (fd >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-c file] [-C ax25common.conf] [-f] [-d] [--verbose]\n"
		"  -c file  configuration file (default %s)\n"
		"  -C file  shared ax25netd loop port configuration (default %s);\n"
		"           the back side of this frontend is taken from it\n"
		"  -f       stay in the foreground\n"
		"  -d       debug logging to stderr\n"
		"  --verbose  say the decisions this daemon makes (repeatable,\n"
		"             the second one is the -d trace)\n", prog,
		AX25_SYSCONFDIR "/ax25tcpd.conf",
		AX25_SYSCONFDIR "/ax25common.conf");
}

int main(int argc, char **argv)
{
	const struct agwpe_client_cb cb = {
		.raw_frame	= tpc_on_raw_frame,
		.connection	= tpc_on_connection,
		.disconnect	= tpc_on_disconnect,
		.data		= tpc_on_data,
	};
	const char *conf = AX25_SYSCONFDIR "/ax25tcpd.conf";
	const char *comconf = AX25_SYSCONFDIR "/ax25common.conf";
	mode_t loop_mode = AX25COMMON_MODE_DEFAULT;
	static const struct option longopt[] = {
		{ "verbose",	no_argument,	NULL,	1002 },
		{ NULL,		0,		NULL,	0 },
	};
	int c, i, r;

	while ((c = getopt_long(argc, argv, "c:C:fdh", longopt, NULL)) != -1) {
		switch (c) {
		case 'c':
			conf = optarg;
			break;
		case 'C':
			comconf = optarg;
			break;
		case 'f':
			tpc.foreground = 1;
			break;
		case 'd':
			tpc.debug = 1;
			break;
		case 1002:
			/* --verbose, repeatable: the second one is the -d
			 * trace, the same ladder ax25netd uses.  */
			if (++tpc.verbose >= 2)
				tpc.debug = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	tpc.mtu = TPC_DEFAULT_MTU;
	for (i = 0; i < TPC_MAX_CLIENT; i++)
		tpc.clients[i].fd = -1;

	if (tpc_read_config(conf) != 0) {
		tpc_log(LOG_ERR, "cannot read configuration %s: %s",
			conf, strerror(errno));
		return 1;
	}

	/* The back side is normally the shared ax25netd loop port from
	 * ax25common.conf, the file the daemon itself reads for its
	 * listener: one file, one semantics.  A 'target' line in this
	 * configuration overrides it for a netd on another host.  */
	if (!tpc.target_set) {
		struct ax25common com;

		if (ax25common_config_load(comconf, &com) < 0) {
			tpc_log(LOG_ERR, "cannot read %s", comconf);
			return 1;
		}
		/* The same "loop mode" ax25netd applies.  Both daemons
		 * create /var/run/ax25/sockets, and whichever of them
		 * starts first would otherwise decide the mode of a
		 * directory the other one then finds in place.  */
		loop_mode = com.loop_mode;
		if (com.loop_socket[0] != '\0') {
			tpc.target_tcp = 0;
			strncpy(tpc.target_sock, com.loop_socket,
				sizeof(tpc.target_sock) - 1);
			tpc.target_sock[sizeof(tpc.target_sock) - 1] = '\0';
		} else if (com.loop_tcp_enabled) {
			tpc.target_tcp = 1;
			strncpy(tpc.target_host, "127.0.0.1",
				sizeof(tpc.target_host) - 1);
			tpc.target_port = com.loop_tcp_port;
		}
	}
	if (!tpc.target_tcp && tpc.target_sock[0] == '\0') {
		tpc_log(LOG_ERR, "no target configured (ax25common.conf or target tcp|socket ...)");
		return 1;
	}
	if (tpc.nlisten == 0) {
		/* No listener configured: fall back to the default
		 * front side, one tcp port on every loopback address.
		 * Which mode a connection is in is the client's business,
		 * not the listener's, so there is no second port.  */
		struct tpc_listen *l = &tpc.listen[tpc.nlisten++];

		memset(l, 0, sizeof(*l));
		strncpy(l->addr, TPC_DEFAULT_LISTEN_ADDR,
			sizeof(l->addr) - 1);
		l->port = TPC_DEFAULT_LISTEN_PORT;
	}

	/* Tell the library where this daemon's server is, before it is
	 * asked anything.  tpc_parse_port_spec() falls back to
	 * ax25_port_info() for the axports names the netd's table does not
	 * carry, and the library resolves those through the AGWPE shim -
	 * which picks its own endpoint from AXSOCK_HOST or
	 * ax25common.conf.  Left alone that is a different answer whenever
	 * a "target tcp" or "target socket" line points somewhere other
	 * than the local default, and the difference would show up as a
	 * port number that belongs to another machine's numbering.
	 *
	 * Set here, before the first connect and before the first lookup,
	 * because the shim resolves its endpoint once and keeps it.
	 */
	if (tpc.target_tcp) {
		char portbuf[16];

		snprintf(portbuf, sizeof(portbuf), "%d", tpc.target_port);
		setenv("AXSOCK_HOST", tpc.target_host, 1);
		setenv("AXSOCK_PORT", portbuf, 1);
	} else {
		const char *path = tpc.target_sock;
		char *abs = NULL;

		/* A socket path is a path and not a host, and the shim
		 * tells the two apart by a leading '/'.  A relative
		 * "target socket ./ax25netd.sock" has no leading slash,
		 * so passing it through unchanged would have the library
		 * try to resolve it as a TCP host name - and quietly
		 * answer nothing, which is a different answer and not a
		 * visible one.  Ax25netd's own socket path is absolute
		 * everywhere else, but this daemon accepts a relative
		 * one, so the translation belongs here where that choice
		 * is made.
		 *
		 * realpath() with a NULL buffer allocates, because a
		 * buffer of its own would have to be PATH_MAX and this
		 * is not a buffer anybody wants on the stack of a
		 * daemon.  Not resolvable: keep what was configured, so
		 * the library fails the same way this daemon already
		 * has rather than for a second reason. */
		if (path[0] != '/')
			abs = realpath(path, NULL);
		setenv("AXSOCK_HOST", abs != NULL ? abs : path, 1);
		free(abs);
		/* A path carries no port; leaving a stale one in the
		 * environment would be read and ignored, and an ignored
		 * value that looks set is worse than one that is not. */
		unsetenv("AXSOCK_PORT");
	}

	tpc.netd = agwpe_client_new(&cb, NULL);
	if (tpc.netd == NULL) {
		tpc_log(LOG_ERR, "cannot allocate the netd client");
		return 1;
	}
	if (tpc.target_tcp)
		r = agwpe_client_connect_host(tpc.netd, tpc.target_host,
					      tpc.target_port);
	else
		r = agwpe_client_connect_unix(tpc.netd, tpc.target_sock);
	if (r < 0) {
		tpc_log(LOG_ERR, "cannot connect to %s: %s",
			tpc.target_tcp ? tpc.target_host : tpc.target_sock,
			strerror(agwpe_client_err(tpc.netd)));
		agwpe_client_free(tpc.netd);
		return 1;
	}

	if (tpc.debug) {
		if (tpc.target_tcp)
			tpc_log(LOG_DEBUG, "back side: tcp %s:%d",
				tpc.target_host, tpc.target_port);
		else
			tpc_log(LOG_DEBUG, "back side: unix socket %s",
				tpc.target_sock);
	}
	if (tpc.target_tcp)
		tpc_verbose("back side: tcp %s:%d",
			    tpc.target_host, tpc.target_port);
	else
		tpc_verbose("back side: unix socket %s", tpc.target_sock);

	/* A stray service holding the loop port accepts the connection but
	 * never speaks AGWPE; every connect would then sit in CONNECTING
	 * until it times out.  Ask for the version and require an answer so
	 * the mix-up is reported at once instead of looking like a dead
	 * radio.  Any AGWPE server (ax25netd, Direwolf, AGWPE) answers it.  */
	tpc.backside_ok = 0;
	agwpe_client_get_version(tpc.netd);
	agwpe_client_pump(tpc.netd, TPC_BACKSIDE_TIMEOUT * 1000);
	if (!tpc.backside_ok) {
		if (tpc.target_tcp)
			tpc_log(LOG_ERR,
				"no answer from tcp %s:%d - not an AGWPE netd?",
				tpc.target_host, tpc.target_port);
		else
			tpc_log(LOG_ERR,
				"no answer from unix socket %s - not an AGWPE netd?",
				tpc.target_sock);
		agwpe_client_free(tpc.netd);
		return 1;
	}

	for (i = 0; i < tpc.nlisten; i++) {
		struct tpc_listen *l = &tpc.listen[i];

		/* Set here rather than at parse time, so a listener gets
		 * the mode from ax25common.conf even when the file is
		 * read after this one.  */
		l->dir_mode = loop_mode;
		if (l->unix_sock) {
			int ufd = tpc_listen_unix(l->path, l->group_mode,
						  l->group_name, l->dir_mode);

			if (ufd < 0) {
				tpc_log(LOG_ERR, "cannot listen: %s",
					strerror(errno));
				tpc_unlink_sockets();
				agwpe_client_free(tpc.netd);
				return 1;
			}
			l->fd[0] = ufd;
			l->nfd = 1;
		} else if (tpc_listen_tcp(l) < 0) {
			tpc_log(LOG_ERR, "cannot listen on tcp %s:%d",
				l->addr, l->port);
			tpc_unlink_sockets();
			agwpe_client_free(tpc.netd);
			return 1;
		}
	}

	agwpe_client_get_ports(tpc.netd);

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, tpc_sigterm);
	signal(SIGINT, tpc_sigterm);

	if (!tpc.foreground && tpc_daemonize() < 0) {
		tpc_log(LOG_ERR, "daemonize: %s", strerror(errno));
		tpc_unlink_sockets();
		agwpe_client_free(tpc.netd);
		return 1;
	}

	tpc_log(LOG_INFO, "ax25tcpd started");
	tpc_loop();
	tpc_unlink_sockets();
	agwpe_client_free(tpc.netd);
	return 0;
}

/* ax25netctl - what a port name on this machine is
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */
/*
 * ax25netctl: say what a port name is, instead of leaving a program to find
 * out from a connect that failed.
 *
 * The question came out of ax25tcpd.  "connect hf/2:DB0AAA" resolves the port
 * from the 'G' table of the ax25netd it is connected to, and that table lists
 * upstream names, so "connect radio0" fails with "no such port" on a name that
 * "call radio0" uses without complaint.  Nothing else in the suite could
 * answer the question, so the way to see why a connect failed was to read three
 * files and work out the numbering by hand.
 *
 * So: one tool, two questions.
 *
 *   Which stack serves this name?  Kernel AX.25, a WAMPES node, or an ax25netd
 *   upstream - asked of the library through ax25_port_info(), which is where
 *   the shim answers the same question internally, so the two cannot disagree.
 *
 *   Is it actually usable?  An axports entry says a name exists; it does not
 *   say the upstream behind it is connected, or that it has answered the port
 *   list request at all.  That is only visible by asking ax25netd, so this
 *   tool connects and asks it for its version and its port table, and prints
 *   both next to what the library worked out.
 *
 * The second half is what makes the difference between "no such port" and a
 * reason.  A name in axports that ax25netd does not list is a name whose
 * upstream is down, or has not come up yet, or is not in
 * ax25netd_agwpe.conf at all - three different problems that all look the
 * same from the client side.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <netax25/agwpe.h>
#include <netax25/agwpe_client.h>
#include <netax25/axcommon.h>
#include <netax25/axlib.h>
#include <netax25/axconfig.h>

#ifndef AX25_SYSCONFDIR
#define	AX25_SYSCONFDIR	"/usr/local/etc/ax25"
#endif

#define	DEFAULT_COMMON	AX25_SYSCONFDIR "/ax25common.conf"

/* How long to wait for ax25netd's answers, in tenths of a second per step of
 * the pump loop below.  Generous, because a netd busy answering one client's
 * port table makes every other client wait, and a tool that gives up early is
 * the tool that is wrong when it matters.  It is a loop and not one blocking
 * read, so a server that answers with nothing at all - a stub in front of the
 * socket - runs into this timeout instead of into a hang.
 */
#define	NCTL_TICKS	200		/* 20 seconds at 100 ms a step */
#define	NCTL_TICK_MS	100

/* One row of ax25netd's own port table, as it arrived.  Kept as it arrived
 * rather than merged into the axports rows: the two disagreeing is the point,
 * and a table that had been merged could not show it. */
struct netd_port {
	int	port;			/* flat AGWPE port, -1 if unnumbered */
	char	up[24];			/* upstream name from the reply */
	char	desc[80];		/* what the server calls it */
};

/* Everything the live query found.  nports == 0 with reached == 1 is a server
 * that answered the version and not the port list, which is a different thing
 * from a server that answered neither.
 */
static struct {
	int			attempted;	/* a server was asked for */
	int			reached;	/* and one was there */
	int			got_ports;	/* the 'G' reply arrived, even
						 * an empty one */
	char			version[64];	/* "" until it answered */
	struct netd_port	ports[AGWPE_PORT_MAX];
	int			nports;
} netd;

static char endpoint[PATH_MAX] = "";
static const char *prog = "ax25netctl";

static void usage(FILE *fp)
{
	fprintf(fp,
"Usage: %s [-L] [-H endpoint] [-C file] [-v] [name ...]\n"
"\n"
"With no name, print one line for every axports entry.  With names, print\n"
"each one in detail, including why it cannot be used.  A name may carry a\n"
"channel as \"port/channel\", the notation ax25tcpd, axkill and axctl use:\n"
"hf/2 is channel 2 of the upstream \"hf\".\n"
"\n"
"  -L              read the local configuration only, do not contact\n"
"                  ax25netd.  Faster, and the answer is then only what\n"
"                  the files say - not whether an upstream is connected.\n"
"  -H <endpoint>   the ax25netd to ask, a path for its unix domain\n"
"                  socket or host:port for its TCP port.  Overrides\n"
"                  ax25common.conf, and is what the ports are resolved\n"
"                  against as well.\n"
"  -C <file>       shared loop port configuration (default %s)\n"
"  -v              print what axports says about each name too, which\n"
"                  for a name that has no entry of its own is often\n"
"                  the answer\n"
"  -h              this text\n"
"\n"
"Exit status is 0 when every name given resolved to a usable port, and 1\n"
"when one did not or when ax25netd could not be reached.  With no names it\n"
"is 0 whenever the tables could be read at all.\n",
		prog, DEFAULT_COMMON);
}

/* ------------------------------------------------------------------ */
/* The live query                                                     */
/* ------------------------------------------------------------------ */

static void on_version(agwpe_client_t *c, unsigned int major, unsigned int minor)
{
	(void)c;
	snprintf(netd.version, sizeof(netd.version), "%u.%u", major, minor);
}

/* The 'G' reply.  ports[] is the library's reading of the number in each
 * token, -1 for one it would not take as a port number, and the row is kept
 * either way: a table that is wrong should show as wrong rather than as
 * missing.
 */
static void on_ports(agwpe_client_t *c, struct agwpe_port_list *list)
{
	int i, n = list->count;

	(void)c;
	netd.got_ports = 1;
	if (n > AGWPE_PORT_MAX)
		n = AGWPE_PORT_MAX;
	for (i = 0; i < n; i++) {
		struct netd_port *p = &netd.ports[netd.nports];

		p->port = list->ports[i];
		snprintf(p->up, sizeof(p->up), "%s", list->ups[i]);
		snprintf(p->desc, sizeof(p->desc), "%s", list->descs[i]);
		netd.nports++;
	}
}

/* Pump the connection until want() is satisfied or the timeout is up.
 * Returns 1 when want() became true, 0 on a timeout, -1 on a broken
 * connection.
 */
typedef int (*netd_want_fn)(void);

static int pump(agwpe_client_t *c, netd_want_fn want)
{
	int ticks;

	for (ticks = 0; ticks < NCTL_TICKS; ticks++) {
		if (want())
			return 1;
		if (agwpe_client_pump(c, NCTL_TICK_MS) < 0)
			return -1;
	}
	return want() ? 1 : 0;
}

static int want_version(void)
{
	return netd.version[0] != '\0';
}

static int want_ports(void)
{
	/* The reply, not its contents: a netd with no upstream at all
	 * answers "0;" and must not be waited for twice. */
	return netd.got_ports;
}

/* Connect to the netd and ask it what it has.  Returns 0 on success, -1 with
 * a reason already printed.
 */
static int query_netd(void)
{
	static const struct agwpe_client_cb cb = {
		.version = on_version,
		.ports = on_ports
	};
	agwpe_client_t *c;
	int rc;

	c = agwpe_client_new(&cb, NULL);
	if (c == NULL) {
		fprintf(stderr, "%s: out of memory\n", prog);
		return -1;
	}

	if (endpoint[0] == '/') {
		rc = agwpe_client_connect_unix(c, endpoint);
	} else {
		const char *colon = strrchr(endpoint, ':');
		char host[128];

		if (colon == NULL) {
			fprintf(stderr,
				"%s: '%s' is neither a path nor host:port\n",
				prog, endpoint);
			agwpe_client_free(c);
			return -1;
		}
		if ((size_t)(colon - endpoint) >= sizeof(host)) {
			fprintf(stderr, "%s: '%s': host part too long\n",
				prog, endpoint);
			agwpe_client_free(c);
			return -1;
		}
		memcpy(host, endpoint, colon - endpoint);
		host[colon - endpoint] = '\0';
		rc = agwpe_client_connect_host(c, host, atoi(colon + 1));
	}
	if (rc < 0) {
		fprintf(stderr, "%s: cannot connect to %s: %s\n", prog,
			endpoint, strerror(agwpe_client_err(c)));
		agwpe_client_free(c);
		return -1;
	}
	netd.reached = 1;

	/* The version request first: something that is not an AGWPE server
	 * will not answer it, and the port table of something else is worth
	 * less than being told what it is. */
	agwpe_client_get_version(c);
	rc = pump(c, want_version);
	if (rc <= 0) {
		fprintf(stderr, "%s: %s %s the version request after %d "
			"seconds\n", prog, endpoint,
			rc < 0 ? "closed the connection during" : "did not answer",
			NCTL_TICKS * NCTL_TICK_MS / 1000);
		agwpe_client_free(c);
		return -1;
	}

	/* The table is not always ready when it is asked for: ax25netd waits
	 * for its upstreams to answer their own request first, and answers
	 * with whatever it has when its own timeout runs out.  So this waits
	 * for the reply and not for the answer to be complete - a server with
	 * no ports at all would otherwise be waited for twice. */
	agwpe_client_get_ports(c);
	pump(c, want_ports);

	agwpe_client_free(c);
	return 0;
}

/* Is this port in ax25netd's table?  Returns the entry, or NULL.
 *
 * Matched on the port number, which is the only thing both sides mean the
 * same by.  The upstream name is not compared: ax25netd answers with its own
 * conf's spelling while axports may carry the "agwpe-" marker, and a name
 * that differs by that prefix is the same port.
 */
static const struct netd_port *netd_find(int port)
{
	int i;

	for (i = 0; i < netd.nports; i++)
		if (netd.ports[i].port == port)
			return &netd.ports[i];
	return NULL;
}

/* The description of a row, with the leading "<upstream>: " taken off.
 *
 * The server writes that prefix into the description, so printing it beside a
 * UPSTREAM column says the same thing twice.  Taken off only when it is
 * really the upstream that is already in the column: a description that
 * happens to begin with a colon is left alone rather than cut wrong.
 */
static const char *desc_only(const struct netd_port *np)
{
	const char *d = np->desc;
	size_t ulen = strlen(np->up);

	if (np->up[0] != '\0' && strncasecmp(d, np->up, ulen) == 0 &&
	    d[ulen] == ':')
		d += ulen + 1;
	while (*d == ' ')
		d++;
	return d;
}

/* ------------------------------------------------------------------ */
/* Output                                                             */
/* ------------------------------------------------------------------ */

/*
 * Turn "hf/2" into the axports spelling "hf:2", which is what the library
 * and axports both use, and return it in buf.
 *
 * The slash spelling is the one ax25tcpd, axkill and axctl take on a command
 * line, so a name that is to work in any of them has to work here.  It is not
 * what axports writes, which is why it is translated rather than taught to
 * the library: the ":N" form is in the config files and in the bind path, and
 * a second spelling in the library would be a second thing to keep right.
 *
 * A port name has no slash in it, so the last one is the separator and there
 * is nothing to compare it against - the same reason the split is always at
 * the first colon in ax25tcpd, where a callsign has no colon.
 *
 * Returns 1 on success, 0 with the reason printed if the suffix is not a
 * channel.  No channel at all is not a failure: "hf/" is a port with a slash
 * on the end and is reported as the typo it is.
 */
static int spec_to_name(const char *spec, char *buf, size_t len)
{
	const char *slash = strrchr(spec, '/');
	size_t blen;

	if (slash == NULL) {
		snprintf(buf, len, "%s", spec);
		return 1;
	}
	blen = (size_t)(slash - spec);
	if (blen == 0 || blen >= len) {
		fprintf(stderr, "%s: '%s' names no port in front of the "
			"slash\n", prog, spec);
		return 0;
	}

	/* Checked here rather than left to the library, so that the message
	 * can name the spelling the operator typed.  The range is the
	 * server's: an upstream has sixteen channels, i*16+c with c 0 to 15.
	 */
	if (strspn(slash + 1, "0123456789") != strlen(slash + 1) ||
	    strlen(slash + 1) == 0 || atoi(slash + 1) > 15) {
		fprintf(stderr, "%s: '%s': a channel is a number, 0 to 15\n",
			prog, slash + 1);
		return 0;
	}

	memcpy(buf, spec, blen);
	buf[blen] = ':';
	snprintf(buf + blen + 1, len - blen - 1, "%s", slash + 1);
	return 1;
}

static const char *backend_name(enum ax25_port_backend b)
{
	switch (b) {
	case AX25_PORT_KERNEL:	return "kernel";
	case AX25_PORT_WAMPES:	return "wampes";
	case AX25_PORT_AGWPE:	return "agwpe";
	default:		return "none";
	}
}

/* What axports itself says, as extra lines.  A name the library resolved
 * without an entry of its own - the channel suffix on a name whose base has
 * one - is the case that surprises, and it is said out loud because a reader
 * looking for a line in axports will not find one and will conclude the name
 * is a typo.
 */
static void say_entry(const struct ax25_port_info *info)
{
	if (info->call[0] != '\0')
		printf("    axports: %s, %d baud, window %d, paclen %d, "
		       "device %s\n", info->call, info->baud, info->window,
		       info->paclen, info->dev[0] != '\0' ? info->dev : "-");
	else
		printf("    axports: no line of that name\n");
}

static void print_row(const char *name, const struct ax25_port_info *info,
		      int verbose)
{
	const struct netd_port *np = NULL;
	char num[8], chan[8];
	const char *who;

	if (info->backend == AX25_PORT_AGWPE) {
		snprintf(num, sizeof(num), "%d", info->port);
		snprintf(chan, sizeof(chan), "%d", info->channel);
		if (netd.reached)
			np = netd_find(info->port);
	} else {
		snprintf(num, sizeof(num), "-");
		snprintf(chan, sizeof(chan), "-");
	}

	switch (info->backend) {
	case AX25_PORT_AGWPE:	who = info->upstream; break;
	case AX25_PORT_WAMPES:	who = info->node; break;
	default:		who = ""; break;
	}

	printf("%-16s %-12s %-7s %-4s %-4s %-12s %-6s %s\n",
	       name,
	       info->call[0] != '\0' ? info->call : "-",
	       backend_name(info->backend), num, chan,
	       who[0] != '\0' ? who : "-",
	       /* "no" is a claim about the server and so is only made when
	 * there was one to ask.  "?" means the question was not asked,
	 * which is a different thing from an answer of no and must not be
	 * printed as one. */
	       info->backend != AX25_PORT_AGWPE ? "-" :
	       !netd.reached ? "?" :
	       np != NULL ? "yes" : "NO",
	       info->desc);
	if (verbose)
		say_entry(info);
}

/* Everything about one name, and the reason when there is nothing to be had.
 * Returns 0 when the name resolved to a port that is there, -1 when it did
 * not, which is what the exit status is made of.
 */
static int print_detail(const char *spec, int verbose)
{
	struct ax25_port_info info;
	char name[128];

	/* Translated before anything is printed, so a spec this tool cannot
	 * read produces its reason and nothing else - and the reason is on
	 * stderr, where it would otherwise land after a heading the user has
	 * already scrolled past. */
	if (!spec_to_name(spec, name, sizeof(name)))
		return -1;

	printf("%s:\n", spec);
	if (strcmp(name, spec) != 0)
		printf("    as \"%s\", the spelling axports uses\n", name);
	if (ax25_port_info(name, &info) != 0) {
		printf("    cannot be looked up\n");
		return -1;
	}

	printf("    backend: %s\n", backend_name(info.backend));
	if (verbose)
		say_entry(&info);

	switch (info.backend) {
	case AX25_PORT_AGWPE: {
		const struct netd_port *np;

		printf("    port: %d, channel %d, upstream \"%s\"\n",
		       info.port, info.channel, info.upstream);

		if (!netd.reached) {
			if (netd.attempted)
				printf("    ax25netd: could not be reached, so "
				       "this is the configuration\n"
				       "               and not the server; "
				       "whether port %d exists is\n"
				       "               not known\n",
				       info.port);
			else
				printf("    ax25netd: not asked (--no-netd), so "
				       "this is the configuration and not\n"
				       "               the server\n");
			return 0;
		}
		np = netd_find(info.port);
		if (np == NULL) {
			printf("    ax25netd: port %d is NOT in its port "
			       "table\n", info.port);
			printf("               so nothing sent on it can "
			       "leave.  Either the upstream \"%s\" is down,\n"
			       "               or it has not answered the "
			       "port list request yet, or it is not\n"
			       "               in ax25netd_agwpe.conf.\n",
			       info.upstream);
			return -1;
		}
		printf("    ax25netd: port %d, \"%s\"\n", np->port,
		       desc_only(np));
		if (np->up[0] != '\0' && info.upstream[0] != '\0' &&
		    strcasecmp(np->up, info.upstream) != 0)
			printf("               the library says upstream "
			       "\"%s\", the server says \"%s\"\n",
			       info.upstream, np->up);
		return 0;
	}

	case AX25_PORT_KERNEL:
		printf("    why: its callsign is an AX.25 interface of the "
		       "kernel stack\n");
		printf("         ax25netd is not involved, and a program "
		       "that only reaches\n");
		printf("         ax25netd cannot use it\n");
		return 0;

	case AX25_PORT_WAMPES:
		printf("    node: \"%s\"\n", info.node);
		printf("    why: it goes to that node's socket, not through "
		       "ax25netd\n");
		return 0;

	default:
		break;
	}

	/* The case that is a typo rather than a wrong name, and the one that
	 * a plain "no such port" leaves an operator with no way to tell
	 * apart.  Asked about the base name only, so that a name which
	 * resolves on its own never comes here: "wampes:hf1" is a WAMPES
	 * port, and its suffix is not a channel at all but the interface the
	 * node is to leave by.
	 *
	 * The number is the server's, not this tool's: ax25netd hands the
	 * channels of upstream i the numbers i*16..i*16+15, so a channel is
	 * 0 to 15 whatever else is true.  See ax25netd_agwpe.conf(5).
	 */
	{
		const char *colon = strchr(name, ':');
		struct ax25_port_info bi;
		char base[64], *end;
		long chan;

		if (colon != NULL && colon != name &&
		    (size_t)(colon - name) < sizeof(base)) {
			memcpy(base, name, colon - name);
			base[colon - name] = '\0';
			if (ax25_port_info(base, &bi) == 0 &&
			    bi.backend == AX25_PORT_AGWPE) {
				errno = 0;
				chan = strtol(colon + 1, &end, 10);
				if (errno != 0 || end == colon + 1 ||
				    *end != '\0' || chan < 0 || chan >= 16) {
					printf("    why: the base name "
					       "\"%s\" is upstream \"%s\", and a "
					       "channel of\n", base,
					       bi.upstream);
					printf("         it is 0 to 15: "
					       "\"%s\" names none\n",
					       colon + 1);
					return -1;
				}
			}
		}
	}

	printf("    why: no axports entry of that name, and neither a "
	       "node nor an\n");
	printf("         upstream of ax25netd claims it\n");
	return -1;
}

/* ------------------------------------------------------------------ */
/* The endpoint                                                       */
/* ------------------------------------------------------------------ */

/* Where ax25netd is, worked out the way ax25netd itself works it out: the
 * shared ax25common.conf, because that is the file that says where the daemon
 * listens, and a tool looking anywhere else would be describing a server that
 * may not be the one running.  -H and AXSOCK_HOST override it.
 *
 * Returns 0, or -1 with the reason already printed.  A missing file is not
 * that case: ax25common_config_load() leaves the built-in path in place,
 * which is where a netd without a file listens.
 */
static int endpoint_of(const char *host, const char *conffile)
{
	struct ax25common com;

	if (host != NULL && *host != '\0') {
		snprintf(endpoint, sizeof(endpoint), "%s", host);
		return 0;
	}
	if (ax25common_config_load(conffile, &com) < 0) {
		fprintf(stderr, "%s: cannot load %s\n", prog, conffile);
		return -1;
	}
	if (com.loop_socket[0] != '\0') {
		snprintf(endpoint, sizeof(endpoint), "%s", com.loop_socket);
		return 0;
	}
	if (com.loop_tcp_enabled) {
		snprintf(endpoint, sizeof(endpoint), "127.0.0.1:%d",
			 com.loop_tcp_port);
		return 0;
	}
	fprintf(stderr,
		"%s: %s names no loop port: no unix socket and no TCP port, "
		"so there is\n"
		"    nothing to ask.  Give one with -H, or enable one in "
		"the file.\n",
		prog, conffile);
	return -1;
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
	char *comconf = NULL;
	const char *host = NULL;
	int no_netd = 0, verbose = 0, rc = 0, i, ch;
	int nentries = 0;
	char *name;

	if (argc > 0 && argv[0] != NULL && *argv[0] != '\0') {
		const char *slash = strrchr(argv[0], '/');

		prog = slash != NULL ? slash + 1 : argv[0];
	}

	{
		static const struct option longopts[] = {
			{ "no-netd",  no_argument,       NULL, 'L' },
			{ "host",     required_argument, NULL, 'H' },
			{ "common",   required_argument, NULL, 'C' },
			{ "verbose",  no_argument,       NULL, 'v' },
			{ "help",     no_argument,       NULL, 'h' },
			{ NULL, 0, NULL, 0 }
		};

		while ((ch = getopt_long(argc, argv, "LH:C:vh", longopts,
					 NULL)) != -1) {
			switch (ch) {
			case 'L':
				no_netd = 1;
				break;
			case 'H':
				host = optarg;
				break;
			case 'C':
				comconf = optarg;
				break;
			case 'v':
				verbose = 1;
				break;
			case 'h':
				usage(stdout);
				return 0;
			default:
				usage(stderr);
				return 1;
			}
		}
		argc -= optind;
		argv += optind;
	}

	if (comconf == NULL)
		comconf = DEFAULT_COMMON;
	if (host == NULL && (host = getenv("AXSOCK_HOST")) != NULL &&
	    *host == '\0')
		host = NULL;

	/* Passed on to the library before anything asks it a question:
	 * ax25_port_info() resolves against the same server this tool asks,
	 * and the library takes its endpoint from AXSOCK_HOST - there is no
	 * other way to tell it.  Set before the first use, so the two agree
	 * by construction rather than by coincidence.
	 */
	if (host != NULL)
		setenv("AXSOCK_HOST", host, 1);

	if (ax25_config_load_ports() < 0) {
		fprintf(stderr, "%s: cannot load the axports file\n", prog);
		return 1;
	}

	if (!no_netd && endpoint_of(host, comconf) == 0) {
		netd.attempted = 1;
		if (query_netd() < 0)
			rc = 1;
	}

	if (argc > 0) {
		for (i = 0; i < argc; i++)
			if (print_detail(argv[i], verbose) != 0)
				rc = 1;
		return rc;
	}

	printf("%-16s %-12s %-7s %-4s %-4s %-12s %-6s %s\n",
	       "NAME", "CALL", "BACKEND", "PORT", "CHAN", "UPSTREAM",
	       "IN NETD", "DESCRIPTION");
	for (name = ax25_config_get_next(NULL); name != NULL;
	     name = ax25_config_get_next(name)) {
		struct ax25_port_info info;

		if (ax25_port_info(name, &info) == 0) {
			nentries++;
			print_row(name, &info, verbose);
		}
	}
	if (nentries == 0)
		printf("(no axports entries)\n");

	if (netd.reached) {
		printf("\nax25netd at %s, version %s, %d port%s in its table\n",
		       endpoint,
		       netd.version[0] != '\0' ? netd.version : "unknown",
		       netd.nports, netd.nports == 1 ? "" : "s");
		if (netd.nports == 0)
			printf("  (it answered the version request and not "
			       "the port table)\n");
		printf("  PORT  UPSTREAM    DESCRIPTION\n");
		for (i = 0; i < netd.nports; i++)
			printf("  %-5d %-12s %s\n", netd.ports[i].port,
			       netd.ports[i].up[0] != '\0' ?
			       netd.ports[i].up : "-",
			       desc_only(&netd.ports[i]));
	} else if (no_netd) {
		printf("\nax25netd not asked (--no-netd)\n");
	} else if (netd.attempted) {
		printf("\nax25netd at %s was not reached\n", endpoint);
	} else {
		printf("\nax25netd: no endpoint\n");
	}

	return rc;
}
/* ax25netctl - what a port name on this machine is
 * Copyright (C) 2026 Thomas Osterried <dl9sau@darc.de>
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
 * So: one tool, three questions.
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
 *   Which connections are running, and how does one end?  ax25netd tracks
 *   every session it carries and gives each an id for as long as it lives, so
 *   "-s" prints the table and "-k id" ends one without having to find the
 *   program that opened it or name its port and call pair.  The other half of
 *   -k is axkill(8)'s request - port:dest src - for a caller that already
 *   knows the pair, the port as its axports name or as the number the tables
 *   print for it, carried by the same SIOCAX25CTLCON axkill uses so that
 *   kernel, WAMPES and ax25netd ports all answer it alike.
 *
 * The second half of the first question is what makes the difference between
 * "no such port" and a reason.  A name in axports that ax25netd does not list
 * is a name whose upstream is down, or has not come up yet, or is not in
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
#include <sys/ioctl.h>

#include <netax25/agwpe.h>
#include <netax25/agwpe_client.h>
#include <netax25/wampes_client.h>
#include <netax25/ax25.h>
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
	int			got_sessions;	/* the session reply arrived */
	char			version[64];	/* "" until it answered */
	struct netd_port	ports[AGWPE_PORT_MAX];
	int			nports;
	struct agwpe_session_list sessions;
} netd;

static char endpoint[PATH_MAX] = "";
static const char *prog = "ax25netctl";

/* -H / AXSOCK_HOST and the ax25common.conf to read, kept for the query to
 * ax25netd.  That query is made on first need rather than at startup: a name
 * the kernel or a WAMPES node answers is settled from the files, and a machine
 * that runs no server must not be told about a connection it never needed.
 * -L turns the question off entirely and leaves netd_off set. */
static const char *ctl_host;
static const char *ctl_conffile;
static int netd_off;

static void usage(FILE *fp)
{
	fprintf(fp,
"Usage: %s [-L] [-H endpoint] [-C file] [-v] [name ...]\n"
"       %s -s\n"
"       %s -k id\n"
"       %s -k node:id\n"
"       %s -k port:dest src\n"
"\n"
"With no name, print one line for every axports entry.  With names, print\n"
"each one in detail, including why it cannot be used.  A name may carry a\n"
"channel as \"port/channel\", the notation ax25tcpd, axkill and axctl use:\n"
"hf/2 is channel 2 of the upstream \"hf\".\n"
"\n"
"  -L              read the local configuration only, do not contact\n"
"                  ax25netd.  Faster, and the answer is then only what\n"
"                  the files say - not whether an upstream is connected.\n"
"                  -s -L still lists the WAMPES nodes, which are not\n"
"                  ax25netd.\n"
"  -H <endpoint>   the ax25netd to ask, a path for its unix domain\n"
"                  socket or host:port for its TCP port.  Overrides\n"
"                  ax25common.conf, and is what the ports are resolved\n"
"                  against as well.\n"
"  -C <file>       shared loop port configuration (default %s)\n"
"  -s              list the sessions ax25netd and any WAMPES node are\n"
"                  carrying, and exit.  The id in the first column is what\n"
"                  -k names.\n"
"  -k id           end that session and exit.  The id is the one -s\n"
"                  printed: ax25netd's number, or \"<node>:<id>\" for a\n"
"                  forwarding of the WAMPES node <node>.  An id nothing\n"
"                  carries is reported, not sent.\n"
"  -k port:dest src\n"
"                  end the connection from src to dest, the same request\n"
"                  axkill(8) makes, and exit.  The port is its axports\n"
"                  name or the number in the PORT column.\n"
"  -v              print what axports says about each name too, which\n"
"                  for a name that has no entry of its own is often\n"
"                  the answer\n"
"  -h              this text\n"
"\n"
"Exit status is 0 when every name given resolved to a usable port, and 1\n"
"when one did not, or when ax25netd was needed and did not answer.  A name\n"
"the kernel or a WAMPES node answers needs no server, so neither its\n"
"absence nor -L makes one fail.  With no names it is 0 whenever the port\n"
"table could be read, ax25netd's own being noted and not required.\n",
		prog, prog, prog, prog, prog, DEFAULT_COMMON);
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

/* The session reply, kept whole: the tool prints it as it arrived.  The
 * table is bounded by the library, so a server with more rows than
 * AGWPE_SESSION_MAX has the rest cut off here. */
static void on_sessions(agwpe_client_t *c, struct agwpe_session_list *list)
{
	(void)c;
	netd.sessions = *list;
	netd.got_sessions = 1;
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

static int want_sessions(void)
{
	return netd.got_sessions;
}

/* Open the connection to ax25netd with the callbacks of the caller's choice.
 * Returns the client, or NULL with the reason already printed.  The endpoint
 * is the one endpoint_of() worked out into endpoint[]. */
static agwpe_client_t *netd_open(const struct agwpe_client_cb *cb)
{
	agwpe_client_t *c;
	int rc;

	c = agwpe_client_new(cb, NULL);
	if (c == NULL) {
		fprintf(stderr, "%s: out of memory\n", prog);
		return NULL;
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
			return NULL;
		}
		if ((size_t)(colon - endpoint) >= sizeof(host)) {
			fprintf(stderr, "%s: '%s': host part too long\n",
				prog, endpoint);
			agwpe_client_free(c);
			return NULL;
		}
		memcpy(host, endpoint, colon - endpoint);
		host[colon - endpoint] = '\0';
		rc = agwpe_client_connect_host(c, host, atoi(colon + 1));
	}
	if (rc < 0) {
		fprintf(stderr, "%s: cannot connect to %s: %s\n", prog,
			endpoint, strerror(agwpe_client_err(c)));
		agwpe_client_free(c);
		return NULL;
	}
	netd.reached = 1;
	return c;
}

/* Ask for the version and wait for it.  Every query does this first: a server
 * that does not answer it is not an AGWPE server at all, and "which server"
 * is worth having in the report either way.  Returns 0, or -1 with the
 * reason printed and the client already freed. */
static int ask_version(agwpe_client_t *c)
{
	int rc;

	agwpe_client_get_version(c);
	rc = pump(c, want_version);
	if (rc > 0)
		return 0;

	fprintf(stderr, "%s: %s %s the version request after %d seconds\n",
		prog, endpoint,
		rc < 0 ? "closed the connection during" : "did not answer",
		NCTL_TICKS * NCTL_TICK_MS / 1000);
	agwpe_client_free(c);
	return -1;
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
	agwpe_client_t *c = netd_open(&cb);

	if (c == NULL)
		return -1;
	if (ask_version(c) < 0)
		return -1;

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

/* Connect and get the session table.  Returns 0 on success, -1 on a failure
 * with the reason already printed. */
static int query_sessions(void)
{
	static const struct agwpe_client_cb cb = {
		.version = on_version,
		.sessions = on_sessions
	};
	agwpe_client_t *c = netd_open(&cb);

	if (c == NULL)
		return -1;
	if (ask_version(c) < 0)
		return -1;

	agwpe_client_get_sessions(c);
	pump(c, want_sessions);
	agwpe_client_free(c);
	return 0;
}

/* One row of the session table, whichever backend it came from.  The two word
 * a row differently - ax25netd's id is a bare number and its port a number, a
 * WAMPES node's port is an interface name and its id carries the node - so both
 * reach here as strings and the printer does not care which is which.  path
 * goes on a line of its own under FROM, in the FROM -> TO direction; "-"
 * prints nothing.
 */
static void print_session_row(const char *id, const char *port,
			      const char *from, const char *to,
			      unsigned proto, const char *state,
			      const char *up, uint32_t owner,
			      const char *prog, const char *path)
{
	char protobuf[8], pidbuf[16];

	/* Hex, as AX.25 PIDs are written: a small decimal number next to the
	 * id would read as a second one.  All values are four characters, so
	 * the column never shifts. */
	snprintf(protobuf, sizeof(protobuf), "0x%02X", proto);
	/* The pid of the local program, so a listener with no id can still be
	 * found and stopped by hand.  The pid, not the uid: the row names the
	 * process a sysop would stop, and its name is printed next to it. */
	if (owner)
		snprintf(pidbuf, sizeof(pidbuf), "%u", (unsigned)owner);
	else
		snprintf(pidbuf, sizeof(pidbuf), "-");
	printf("%-8s %-5s %-10s %-10s %-5s %-11s %-10s %-7s %s\n",
	       id, port, from, to, protobuf,
	       state[0] ? state : "-", up, pidbuf,
	       prog[0] ? prog : "-");
	if (path != NULL && path[0] != '\0' && strcmp(path, "-") != 0)
		printf("%-8s %-5s %s\n", "", "", path);
}

static void print_session_header(void)
{
	printf("%-8s %-5s %-10s %-10s %-5s %-11s %-10s %-7s %s\n",
	       "ID", "PORT", "FROM", "TO", "PROTO", "STATE", "UPSTREAM",
	       "PID", "PROG");
}

/* One WAMPES node's rows, printed as they come.  A row is the node's own
 * <id> <port> <from> <to> <proto> <state> <owner> <prog> <path>, so the port
 * is an interface name and the proto is decimal; the id is prefixed with the
 * node to keep it unique in a table that also carries ax25netd's numbers, and
 * the node is the UPSTREAM (documentation/wampes-sessions-in-netd.md §8.12).
 * A claimed callsign has no handle: its id stays "-", the instance is in the
 * UPSTREAM column either way.
 */
struct wampes_list {
	const char	*node;
	int		*count;
};

static int on_wampes_session(void *arg, const char *line)
{
	struct wampes_list *w = arg;
	char id[16] = "", port[80] = "", from[32] = "", to[32] = "";
	char state[16] = "", prog[16] = "", path[AGWPE_PATH_MAX] = "";
	char rowid[48];
	unsigned proto = 0, owner = 0;

	if (sscanf(line, "%15s %79s %31s %31s %u %15s %u %15s %87s",
		   id, port, from, to, &proto, state, &owner, prog,
		   path) < 4)
		return 0;		/* not a row this table should have */

	if (strcmp(id, "-") == 0)
		snprintf(rowid, sizeof(rowid), "-");
	else
		snprintf(rowid, sizeof(rowid), "%s:%s", w->node, id);

	print_session_row(rowid, port[0] ? port : "-", from, to, proto,
			  state, w->node, owner, prog, path);
	(*w->count)++;
	return 0;
}

/* Ask every configured WAMPES node for its sessions.  A node that cannot be
 * reached is said so and remembered as a failure, the way an unreachable
 * ax25netd is; the nodes that answer are printed all the same.
 */
static int show_wampes_sessions(int *count)
{
	int i, rc = 0, n = wampes_node_count();

	for (i = 0; i < n; i++) {
		const char *node = wampes_node_name(i);
		struct wampes_list w = { node, count };

		if (node == NULL)
			continue;
		if (wampes_sessions(node, on_wampes_session, &w) < 0) {
			fprintf(stderr, "%s: WAMPES node %s: %s\n", prog,
				node, strerror(errno));
			rc = 1;
		}
	}
	return rc;
}

/* Print the sessions of every backend in one table.  ax25netd is asked first,
 * so its rows come first and the WAMPES nodes follow.  A source that does not
 * answer is a reason to return 1, but not a reason to skip the other: a
 * machine with no ax25netd can still show what its node carries, and the other
 * way round.
 */
static int show_sessions(int with_netd)
{
	int i, rc = 0, n = 0;

	print_session_header();

	if (with_netd) {
		if (query_sessions() < 0 || !netd.got_sessions) {
			/* An unreachable server already said so in
			 * netd_open(), and a machine that runs no ax25netd
			 * still shows its WAMPES sessions, so that is not a
			 * failure.  Only a server that took the connection
			 * and then stayed silent is reported again here -
			 * and the version answer tells the two apart. */
			if (netd.reached) {
				if (netd.version[0] != '\0')
					fprintf(stderr,
						"%s: %s did not answer the "
						"session request\n",
						prog, endpoint);
				rc = 1;
			}
		} else {
			for (i = 0; i < netd.sessions.count; i++) {
				struct agwpe_session *s =
					&netd.sessions.sessions[i];
				char id[16], port[16];

				/* A listening row has no handle: nothing
				 * there can be killed, and a 0 would read as
				 * one that can. */
				if (s->id)
					snprintf(id, sizeof(id), "%u",
						 (unsigned)s->id);
				else
					snprintf(id, sizeof(id), "-");
				snprintf(port, sizeof(port), "%d", s->port);
				print_session_row(id, port, s->from, s->to,
						  s->pid, s->state, s->up,
						  s->owner, s->prog, s->path);
				n++;
			}
		}
	}
	if (show_wampes_sessions(&n) != 0)
		rc = 1;

	if (n == 0)
		printf("(no sessions)\n");
	return rc;
}

/* A number written the way both tables this tool prints give one: nothing but
 * digits.  A name is a word, so there is nothing a number could be read as
 * instead, and the two never have to be told apart by where they sit. */
static int all_digits(const char *s)
{
	return s[0] != '\0' && strspn(s, "0123456789") == strlen(s);
}

/* Is this name one wampes.conf gives a node?  Read here rather than left to
 * the close, because "close <node>" would otherwise reach the default node on
 * a name that is a typo - the node the request was aimed at is exactly what a
 * mistyped one must not be.
 */
static int wampes_node_known(const char *name)
{
	int i, n = wampes_node_count();

	for (i = 0; i < n; i++) {
		const char *node = wampes_node_name(i);

		if (node != NULL && strcasecmp(node, name) == 0)
			return 1;
	}
	return 0;
}

/* End a WAMPES node's forwarding by the handle -s printed, "<node>:<id>".
 * The node does the looking up and the closing - it is the only side that
 * knows its own forwardings - and this returns the exit status, saying what
 * went wrong where the node cannot: a misspelled node, a handle that is not a
 * number, an id the node does not have.
 */
static int kill_wampes_id(const char *node, const char *id)
{
	if (id[0] == '\0' || !all_digits(id)) {
		fprintf(stderr, "%s: '%s' is not a session id\n", prog, id);
		return 1;
	}
	if (!wampes_node_known(node)) {
		fprintf(stderr, "%s: no WAMPES node named %s\n", prog, node);
		return 1;
	}
	if (wampes_close_session(node, id) < 0) {
		if (errno == ESRCH)
			fprintf(stderr, "%s: %s has no session %s\n", prog,
				node, id);
		else
			fprintf(stderr, "%s: cannot close %s:%s: %s\n", prog,
				node, id, strerror(errno));
		return 1;
	}
	return 0;
}

/* End a session by the id the table gave.  Nothing is reported on success,
 * as in axkill(8): a kill has no answer, so there is nothing to report from
 * but the fact that the request left, and a line of output would be read as
 * a warning by whatever runs this from a script.  Returns the exit status.
 *
 * The silence is only for a kill that had something to end.  The kill frame
 * itself is never answered - ax25netd drops one that names no session, and a
 * server that is not ax25netd has no such command at all - so an id that
 * names nothing would be indistinguishable from a kill that worked, and "the
 * table a moment ago said 11" followed by "nothing happened" is the report a
 * sysop is left with.  So the table is asked first: it is the one place this
 * side can learn whether the id still means anything.  That is a question the
 * daemon does answer, and it is the reason the tool takes an id at all rather
 * than a port and a call pair.
 *
 * A session that goes between the ask and the kill is not an error: the id
 * is what the caller had, and the connection it named is gone either way.
 */
static int kill_session_id(unsigned long id)
{
	static const struct agwpe_client_cb cb = {
		.version = on_version,
		.sessions = on_sessions
	};
	agwpe_client_t *c = netd_open(&cb);
	int i;

	if (c == NULL)
		return 1;

	agwpe_client_get_sessions(c);
	if (pump(c, want_sessions) <= 0 || !netd.got_sessions) {
		fprintf(stderr, "%s: %s did not answer the session request\n",
			prog, endpoint);
		agwpe_client_free(c);
		return 1;
	}

	for (i = 0; i < netd.sessions.count; i++) {
		if (netd.sessions.sessions[i].id != (uint32_t)id)
			continue;

		{
			int rc = agwpe_client_kill_id(c, (uint32_t)id);

			if (rc < 0)
				fprintf(stderr, "%s: cannot send the kill to "
					"%s: %s\n", prog, endpoint,
					strerror(agwpe_client_err(c)));
			/* Let the frame leave before the socket closes. */
			agwpe_client_pump(c, NCTL_TICK_MS);
			agwpe_client_free(c);
			return rc < 0 ? 1 : 0;
		}
	}

	fprintf(stderr, "%s: no session with id %lu\n", prog, id);
	agwpe_client_free(c);
	return 1;
}

/* The number that token carries, or -1 when it is not one a port could have.
 * A flat port is a byte, 0 to the loop's 255, so a token wider than that is
 * not a small port but a long one - parsed rather than handed to atoi(), whose
 * behaviour on a value it cannot hold is not something to depend on.  It is
 * still reported as a port with nothing behind it, which is the truth of it. */
static int port_number(const char *s)
{
	unsigned long v;

	if (!all_digits(s))
		return -1;
	v = strtoul(s, NULL, 10);	/* digits only, so there is no end to
					 * check; one that does not fit comes
					 * back as ULONG_MAX and fails below */
	return v <= AGWPE_PORT_LOOP ? (int)v : -1;
}

/* The axports name of a flat AGWPE port: the reverse of what the port table
 * prints beside every name, so a number read off a table can be typed back as
 * it is.  Returns 1 with the name in buf, 0 when no entry names that port.
 *
 * Asked of axports and not of ax25netd, because that is where the number came
 * from in the first place: the flat port is what the library computes for a
 * name, so the entry that gives it back is already on this side of the wire,
 * and a -k does not need a server that is up to be told a number.
 *
 * Only an AGWPE entry can match.  Every other backend has no such number and
 * says so with -1, so a kernel or WAMPES port can never be picked out by a
 * number that happens to equal it.
 */
static int name_by_port(int port, char *buf, size_t len)
{
	char *name;

	for (name = ax25_config_get_next(NULL); name != NULL;
	     name = ax25_config_get_next(name)) {
		struct ax25_port_info info;

		if (ax25_port_info(name, &info) != 0)
			continue;
		if (info.backend == AX25_PORT_AGWPE && info.port == port) {
			snprintf(buf, len, "%s", name);
			return 1;
		}
	}
	return 0;
}

/* End a connection by name, axkill(8)'s notation: the port and destination,
 * with the source.  The same SIOCAX25CTLCON request axkill makes, so a kernel
 * connection, a WAMPES node and an ax25netd port are reached the same way and
 * this tool does not grow a second opinion about which is which.  Returns the
 * exit status.
 *
 * The port may be the number instead of the name - the PORT column that both
 * tables print, which is what a reader has just copied it out of.  It is
 * translated rather than accepted as a second spelling, because the request
 * carries a callsign and a number has none: the entry that names the port is
 * what has it.  A number no entry names is then said so, and not guessed at
 * from the upstream beside it, whose callsign may be another port's.
 *
 * port is a buffer of portlen bytes, which is rewritten on a translation.
 */
static int check_kill_name(const char *port, const char *dest,
			   const char *src, const char *host,
			   const char *comconf);

/* Ask ax25netd once, on first need; see the definition for why. */
static int netd_ensure(void);

static int kill_by_name(char *port, size_t portlen, const char *dest,
			const char *src, const char *host, const char *comconf)
{
	struct ax25_ctl_struct ax25_ctl;
	char *addr;
	int s;

	addr = ax25_config_get_addr(port);
	if (addr == NULL) {
		int n = port_number(port);
		char name[128];

		if (n >= 0 && name_by_port(n, name, sizeof(name))) {
			snprintf(port, portlen, "%s", name);
			addr = ax25_config_get_addr(port);
		}
	}
	if (addr == NULL) {
		if (all_digits(port))
			fprintf(stderr, "%s: no axports entry names port %s, "
				"so there is no callsign to kill by\n", prog,
				port);
		else
			fprintf(stderr, "%s: invalid port name - %s\n", prog,
				port);
		return 1;
	}
	/* A kill that names a listening call has nothing to end and is
	 * refused; one the session table does not have is sent, but said out
	 * loud.  Either way the caller hears the reason, which the kill
	 * itself never gives. */
	if (check_kill_name(port, dest, src, host, comconf))
		return 1;
	if (ax25_aton_entry(addr, (char *)&ax25_ctl.port_addr) == -1 ||
	    ax25_aton_entry(dest, (char *)&ax25_ctl.dest_addr) == -1 ||
	    ax25_aton_entry(src, (char *)&ax25_ctl.source_addr) == -1) {
		fprintf(stderr, "%s: bad callsign\n", prog);
		return 1;
	}

	s = socket(AF_AX25, SOCK_SEQPACKET, 0);
	if (s < 0) {
		fprintf(stderr, "%s: socket: %s\n", prog, strerror(errno));
		return 1;
	}
	ax25_ctl.cmd = AX25_KILL;
	ax25_ctl.arg = 0;
	ax25_ctl.digi_count = 0;
	if (ioctl(s, SIOCAX25CTLCON, &ax25_ctl) != 0) {
		fprintf(stderr, "%s: SIOCAX25CTLCON: %s\n", prog,
			strerror(errno));
		close(s);
		return 1;
	}
	close(s);
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

		/* Only an AGWPE name needs the server: its port table is
		 * what says whether the upstream is connected, which the
		 * files cannot.  A kernel or WAMPES name is answered without
		 * a connection, so the query waits until here. */
		if (!netd_off && !netd.attempted)
			netd_ensure();

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

/* Ask ax25netd once, on first need, and remember whether it was reached.
 *
 * Not done at startup, because whether it is needed is known only after the
 * library has said which backend serves a name: the kernel's ports are settled
 * by the interfaces and a WAMPES node's by wampes.conf, and only an AGWPE port
 * needs the server.  A machine with no ax25netd at all is therefore asked
 * nothing when every name names a kernel or WAMPES port, and gets no report of
 * a connection it never needed.  The listing does ask, because the server's
 * own table is part of what it prints.
 *
 * -L leaves the question unasked; the per-name path then says so.  Returns 0
 * when the server was reached, -1 otherwise.
 */
static int netd_ensure(void)
{
	if (!netd.attempted) {
		netd.attempted = 1;
		if (!netd_off && endpoint_of(ctl_host, ctl_conffile) == 0)
			query_netd();
	}
	return netd.reached ? 0 : -1;
}

/* Ask ax25netd what it has for the pair a kill by name names, and say what
 * that means for the kill.
 *
 * A listening call is not a session: it is a program waiting for a call that
 * has not arrived, which the daemon tracks as a registered call and not as a
 * link.  The kill finds no session, and the daemon answers it by telling that
 * program its end is gone - a frame a listener has no connection to receive,
 * so it is dropped and the request quietly does nothing.  That one is refused
 * before it is sent, with the pid and name the session table carries for the
 * row, which is the program to stop instead.
 *
 * A pair the table does not have at all is a different thing, and not a
 * refusal: the daemon can still end a link it never tracked - on a radio by
 * telling the upstream TNC to drop the pair, on the loop by the calls
 * registered for it.  So that kill is sent - but it answers nothing either, and
 * a kill that looks like it worked and did not is worse than a line too many.
 *
 * Only an ax25netd port is asked.  A WAMPES port is left alone here too: the
 * kill reaches the node through the ioctl, the way a kernel kill reaches the
 * kernel, and the node is the only side that knows its own forwardings - there
 * is nothing here to look up first.  A kernel port is likewise left alone: it
 * is the kernel's to answer.
 *
 * Returns 1 when the kill must not be sent (a listening call, already
 * reported), 0 when it should go on.
 */
static int check_kill_name(const char *port, const char *dest, const char *src,
			   const char *host, const char *comconf)
{
	struct ax25_port_info info;
	int i;

	if (ax25_port_info(port, &info) != 0)
		return 0;
	if (info.backend != AX25_PORT_AGWPE)
		return 0;
	if (endpoint_of(host, comconf) != 0)
		return 0;
	netd.attempted = 1;
	if (query_sessions() < 0 || !netd.got_sessions)
		return 0;

	for (i = 0; i < netd.sessions.count; i++) {
		const struct agwpe_session *s = &netd.sessions.sessions[i];

		if (s->port != info.port)
			continue;

		if (s->id != 0) {
			/* A session whose pair is this one, in either
			 * order - the two ways netd's session_find()
			 * tries it.  That kill has something to end. */
			if ((strcasecmp(s->from, src) == 0 &&
			     strcasecmp(s->to, dest) == 0) ||
			    (strcasecmp(s->from, dest) == 0 &&
			     strcasecmp(s->to, src) == 0))
				return 0;
			continue;
		}

		/* A listening row: no handle, remote '*', the state a
		 * program waiting for a call is shown in. */
		if (strcasecmp(s->to, "*") != 0)
			continue;
		if (strcasecmp(s->from, dest) != 0 &&
		    strcasecmp(s->from, src) != 0)
			continue;

		fprintf(stderr,
			"%s: %s on port %s is a listening call, not a "
			"connection:\n"
			"    a listen cannot be killed.  Stop the program "
			"instead", prog, s->from, port);
		if (s->owner)
			fprintf(stderr, " (pid %u, %s)", (unsigned)s->owner,
				s->prog[0] ? s->prog : "-");
		fprintf(stderr, ".\n");
		return 1;
	}

	/* Nothing in the table: no session to name and no listener either. */
	fprintf(stderr,
		"%s: no session on port %s from %s to %s; sending the kill "
		"anyway", prog, port, src, dest);
	if (info.port == AGWPE_PORT_LOOP)
		fprintf(stderr, " -\n"
			"    the loop daemon can still end a link it did not "
			"track, by the calls registered for it");
	else
		fprintf(stderr, " -\n"
			"    the daemon can still end a link it did not track, "
			"by telling the upstream to drop the pair");
	fprintf(stderr, ".\n");
	return 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
	char *comconf = NULL;
	const char *host = NULL;
	char *kill_arg = NULL;
	int show_sess = 0;
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
			{ "sessions", no_argument,       NULL, 's' },
			{ "kill",     required_argument, NULL, 'k' },
			{ "verbose",  no_argument,       NULL, 'v' },
			{ "help",     no_argument,       NULL, 'h' },
			{ NULL, 0, NULL, 0 }
		};

		while ((ch = getopt_long(argc, argv, "LH:C:sk:vh", longopts,
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
			case 's':
				show_sess = 1;
				break;
			case 'k':
				kill_arg = optarg;
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

	/* For netd_ensure(), which asks on first need rather than here. */
	ctl_host = host;
	ctl_conffile = comconf;
	netd_off = no_netd;

	/* -s and both -k by handle forms answer without the axports file;
	 * only -k by name needs it, below.  Answered first so that a machine
	 * without one can still list and kill. */
	if (kill_arg != NULL && show_sess) {
		fprintf(stderr, "%s: -s and -k are two different requests\n",
			prog);
		return 1;
	}
	if (kill_arg != NULL && argc == 0) {
		/* "<node>:<id>" - a forwarding of a WAMPES node, which the
		 * node itself closes.  Told apart from the port:dest form by
		 * what follows the colon: a handle is all digits, a
		 * destination is a callsign, and port:dest always carries its
		 * source as a second word anyway. */
		char *colon = strrchr(kill_arg, ':');

		if (colon != NULL && colon != kill_arg &&
		    all_digits(colon + 1)) {
			char node[64];

			if ((size_t)(colon - kill_arg) >= sizeof(node)) {
				fprintf(stderr, "%s: '%s': node name too "
					"long\n", prog, kill_arg);
				return 1;
			}
			memcpy(node, kill_arg, (size_t)(colon - kill_arg));
			node[colon - kill_arg] = '\0';
			return kill_wampes_id(node, colon + 1);
		}
	}
	if (kill_arg != NULL) {
		char *end;
		unsigned long id;

		errno = 0;
		id = strtoul(kill_arg, &end, 10);
		if (kill_arg[0] != '\0' && *end == '\0' && errno == 0) {
			if (argc > 0) {
				fprintf(stderr, "%s: -k %s takes no further "
					"argument\n", prog, kill_arg);
				return 1;
			}
			if (no_netd) {
				fprintf(stderr, "%s: -k needs ax25netd\n",
					prog);
				return 1;
			}
			if (endpoint_of(host, comconf) != 0)
				return 1;
			netd.attempted = 1;
			return kill_session_id(id);
		}
	}
	if (show_sess) {
		if (argc > 0) {
			fprintf(stderr, "%s: -s takes no names\n", prog);
			return 1;
		}
		/* -L is "do not ask ax25netd".  A WAMPES node is not ax25netd,
		 * so its sessions are still listed - which is what makes -s -L
		 * the way to see a node on a machine that runs no daemon. */
		if (!no_netd) {
			if (endpoint_of(host, comconf) != 0)
				return 1;
			netd.attempted = 1;
		}
		return show_sessions(!no_netd);
	}

	if (ax25_config_load_ports() < 0) {
		fprintf(stderr, "%s: cannot load the axports file\n", prog);
		return 1;
	}

	if (kill_arg != NULL) {
		char spec[256], port[256], *colon;

		if (argc != 1) {
			fprintf(stderr, "%s: expected -k port:dest src\n",
				prog);
			return 1;
		}
		snprintf(spec, sizeof(spec), "%s", kill_arg);
		colon = strrchr(spec, ':');
		if (colon == NULL || colon == spec || colon[1] == '\0') {
			fprintf(stderr, "%s: expected port:dest, got '%s'\n",
				prog, kill_arg);
			return 1;
		}
		*colon = '\0';
		/* Copied out before the lookup, which may rewrite it with
		 * the name a number stands for: the destination sits right
		 * behind the colon and would be overwritten with it. */
		snprintf(port, sizeof(port), "%s", spec);
		return kill_by_name(port, sizeof(port), colon + 1, argv[0],
				    host, comconf);
	}

	/* Named ports are answered one by one below, and only an AGWPE name
	 * makes netd_ensure() reach for the server.  The listing is the case
	 * that asks up front: ax25netd's own table is part of what it
	 * prints, and there is no name-by-name path to ask from. */
	if (argc > 0) {
		for (i = 0; i < argc; i++)
			if (print_detail(argv[i], verbose) != 0)
				rc = 1;
		return rc;
	}

	/* A missing ax25netd is said at the end and is not a failure of the
	 * port question, which the files answer.  -L leaves it unasked. */
	if (!no_netd)
		netd_ensure();

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
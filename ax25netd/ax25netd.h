/* ax25netd - AGWPE multiplexing daemon for libax25
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
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#ifndef AX25NETD_NETD_H
#define AX25NETD_NETD_H

#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netax25/agwpe.h>
#include <netax25/agwpe_client.h>
#include <netax25/agwpe_config.h>

#define	AX25NETD_PORT_DEFAULT	8100
#define	AX25NETD_BIND_DEFAULT	"127.0.0.1"
#define	AX25NETD_RECONNECT_DELAY	5

/*
 * Each radio upstream occupies a block of AX25NETD_PORT_STRIDE flat loop
 * port numbers; upstream i owns ports i*16 .. i*16+15, channel c of
 * upstream i is port i*16+c.  A single-channel Direwolf therefore keeps
 * its port number 0.  The block is fixed so that the numbering survives
 * a Direwolf restart with a changed channel count.
 */
#define	AX25NETD_PORT_STRIDE		16

/* How long a 'G' request waits for the upstream port lists before it
 * is answered with what is known so far.  */
#define	AX25NETD_PORTS_TIMEOUT		10

#define	AX25NETD_VERSION_MAJOR	1
#define	AX25NETD_VERSION_MINOR	0

#define	AX25NETD_BUF_MAX		(16 * 1024 * 1024)

/*
 * Bound on the per-client output queue.  A client that cannot keep up is
 * allowed to fall behind for this much traffic before it is dropped;
 * beyond it the backlog is a real failure, not a burst.
 */
#define	AX25NETD_OUT_MAX		(1 * 1024 * 1024)

struct ax25netd_client {
	int			fd;
	int			dead;
	int			authed;		/* passed the login (auth required) */
	unsigned char		*rbuf;
	size_t			rlen;
	size_t			ra;
	unsigned char		*obuf;		/* pending output, netle on wire */
	size_t			olen;
	size_t			oa;
	int			monitor;	/* wants monitored frames */
	int			raw;		/* wants raw frames */
	int			want_ports;	/* 'G' request deferred */
	time_t			ports_since;	/* when the request arrived */
};

struct ax25netd_call {
	char			call[AGWPE_MAX_CALL];
	unsigned char		chan;		/* radio channel on the upstream */
	int			fd;		/* owning loop client */
	int			listener;	/* 'L' listener: SSID 0 matches any */
};

/* One channel of a radio upstream, learned from its 'G' reply.  */
struct ax25netd_port {
	unsigned char		chan;		/* port byte on the upstream */
	char			desc[64];
};

/*
 * One logical session between a local (registered) call and a remote
 * station, identified by (call_from, call_to, pid).  Several sessions may
 * share the same AX.25 link: a link transports every PID at once, so a
 * NET/ROM session on an existing text link is not a second connection.
 * call_from is the local call, call_to the remote station.
 */
struct ax25netd_session {
	char			call_from[AGWPE_MAX_CALL];
	char			call_to[AGWPE_MAX_CALL];
	unsigned char		pid;
	unsigned char		chan;		/* radio channel on the upstream */
	int			fd;		/* owning loop client */

	/* Connection parameters set through the 'Q' control extension
	 * (axctl).  Zero means the default applies.  */
	unsigned int		window;
	unsigned int		t1, t2, t3;
	unsigned int		n2;
	unsigned int		idle;
	unsigned int		paclen;
};

struct ax25netd_upstream {
	int			index;
	char			name[24];
	char			host[AGWPE_UPSTREAM_HOST_MAX];
	int			tcp_port;
	int			virtual;	/* virtual loop upstream, port 255 */

	/* Optional AGWPE login credentials, from agwpe_shadow.conf.  */
	char			user[AGWPE_AUTH_NAME_MAX];
	char			pass[AGWPE_AUTH_PASS_MAX];

	agwpe_client_t		*cli;
	int			connected;
	int			dead;
	time_t			dead_since;

	int			monitor_on;	/* 'm' state on the upstream */
	int			raw_on;		/* 'k' state on the upstream */

	/* Radio channels reported by the upstream's 'G' reply.  */
	struct ax25netd_port	ports[AX25NETD_PORT_STRIDE];
	int			nports;
	int			ports_ready;	/* 'G' reply received */

	struct ax25netd_call	*calls;
	int			ncalls;
	int			acalls;

	struct ax25netd_session	*sessions;
	int			nsessions;
	int			asessions;

	int			heard_to;	/* client waiting for 'H' replies */
	int			out_to;		/* client waiting for 'y'/'Y' replies */
};

struct ax25netd_ctx {
	int			debug;
	int			mheard;		/* keep the mheard.dat heard list */

	/* Loop port client authentication mode: one of the AGWPE_AUTH_*
	 * constants from netax25/agwpe_config.h.  In AGWPE_AUTH_EXTERN (the
	 * default) and AGWPE_AUTH_ALWAYS mode, clients connecting from
	 * a non-loopback address must log in with credentials from
	 * agwpe_shadow.conf before any other frame is accepted.  The
	 * credentials also cover peer ax25netd boxes connecting in a
	 * chain.  */
	int			auth;
	struct agwpe_auth	*clients_auth;
	int			nclients_auth;

	int			nclients;
	int			capclients;
	struct ax25netd_client	*clients;

	int			listener_fd;	/* TCP listener, -1 when disabled */
	int			unix_fd;	/* unix socket listener, -1 when disabled */
	char			unix_path[108];
	int			unix_group_mode;	/* AGWPE_GROUP_* */
	gid_t			unix_group_gid;		/* resolved group, 0 for default */

	int			nup;
	struct ax25netd_upstream	*ups;

	struct ax25netd_upstream	loop;		/* virtual loop upstream */
	int			loop_enabled;

	/* "autoroute yes|no": resolve a digipeater path for connects
	 * without one by asking the ax25rtd route cache.  */
	int			autoroute;
};

extern struct ax25netd_ctx ax25netd;

extern void ax25netd_log(int prio, const char *fmt, ...);

/* loop.c */
extern int loop_init(const char *bindaddr, int port);
extern int loop_init_unix(const char *path, int group_mode,
			  const char *group_name, uid_t run_uid,
			  gid_t run_gid);
extern void loop_accept(int lfd);
extern void loop_read_client(struct ax25netd_client *cl);
extern void loop_flush_client(struct ax25netd_client *cl);
extern void loop_close_client(struct ax25netd_client *cl);
extern void loop_reap_dead(void);
extern int loop_send_client(struct ax25netd_client *cl, const struct agwpe_s *hdr,
			    const unsigned char *data, size_t len);
extern struct ax25netd_client *client_by_fd(int fd);

/* True if the sockaddr is a loopback address.  The whole IPv4 127/8
 * counts, not just 127.0.0.1.  */
extern int addr_is_loopback(const struct sockaddr *sa, socklen_t len);

/* True if the host string names a loopback address, resolved with
 * getaddrinfo (also understands "localhost").  */
extern int host_is_loopback(const char *host);

/* mux.c */
extern void mux_client_command(struct ax25netd_client *cl, const struct agwpe_s *hdr,
			       const unsigned char *data, size_t len);
extern void mux_client_disconnect(struct ax25netd_client *cl);
extern void mux_upstream_frame(struct ax25netd_upstream *u, const struct agwpe_s *hdr,
			       const unsigned char *data, size_t len);
extern void mux_upstream_reconnected(struct ax25netd_upstream *u);
extern void mux_upstream_lost(struct ax25netd_upstream *u);
extern void mux_recalc_toggles(void);
extern void mux_ports_tick(time_t now);

/* upstream.c */
extern int upstream_init_all(void);
extern void upstream_read(struct ax25netd_upstream *u);
extern void upstream_reconnect_tick(time_t now);

/* mheard.c */
extern int ax25netd_mheard_init(void);
extern void ax25netd_mheard_frame(struct ax25netd_upstream *u,
			      const unsigned char *frame, size_t len);

/* route.c */
extern int ax25netd_route_lookup(struct ax25netd_upstream *u, const char *call,
			     unsigned char *buf, size_t bufsz);

#endif

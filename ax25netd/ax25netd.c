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
/*
 * ax25netd: multiplex one or more AGWPE servers (upstreams) behind a
 * single local AGWPE port.  AGWPE client programs (Direwolf's agwpe
 * protocol, or libax25's agwpe_client) connect to the loop port; each
 * loop port number selects one radio channel of one upstream, learned
 * from the upstream's 'G' port info reply.
 *
 * The kernel AX.25 path of libax25 is untouched: on Linux the native
 * kernel stack continues to be used exactly as before.  ax25netd only
 * mediates between AGWPE clients and AGWPE servers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <getopt.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <syslog.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <netax25/agwpe_config.h>
#include <netax25/axcommon.h>

#include "ax25netd.h"

#ifndef AX25_SYSCONFDIR
#define	AX25_SYSCONFDIR	"/usr/local/etc/ax25"
#endif

#define	DEFAULT_CONF	AX25_SYSCONFDIR "/agwpe.conf"
#define	DEFAULT_COMMON	AX25_SYSCONFDIR "/ax25common.conf"

struct ax25netd_ctx ax25netd;

void ax25netd_log(int prio, const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	syslog(prio, "%s", buf);
	if (ax25netd.debug || prio == LOG_ERR || prio == LOG_WARNING)
		fprintf(stderr, "ax25netd: %s\n", buf);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-f] [-d] [-c config] [-C ax25common.conf]\n"
		"            [-b bindaddr] [-p port] [-U socket] [-g group]\n"
		"            [--no-tcp] [-u user]\n"
		"\n"
		"  -f         stay in the foreground\n"
		"  -d         log to stderr as well\n"
		"  -c <file>  configuration file (default %s)\n"
		"  -C <file>  shared loop port configuration, read by\n"
		"             ax25netd and ax25tcpd (default %s)\n"
		"  -b <addr>  bind address of the loop port; IPv4 and IPv6 are\n"
		"             supported (default %s)\n"
		"  -p <port>  TCP port of the loop port (default %d)\n"
		"  -U <path>  also listen on this unix domain socket (overrides\n"
		"             the 'loop socket' directive in ax25common.conf)\n"
		"  -g <name>  group allowed to connect to the unix socket: a\n"
		"             group name, a numeric gid, or 'all' for every\n"
		"             local user (default: the daemon run user)\n"
		"      --no-tcp  do not listen on TCP at all; the unix socket\n"
		"             becomes the only way in (requires -U or 'socket')\n"
		"  -u <user>  drop root privileges to this user after startup\n"
		"      --no-mheard  do not maintain the mheard.dat heard list\n",
		prog, DEFAULT_CONF, DEFAULT_COMMON, AX25NETD_BIND_DEFAULT,
		AX25NETD_PORT_DEFAULT);
}

/*
 * Remove the unix socket on shutdown, so no stale file is left behind
 * for the next start (which would remove it anyway, but this also keeps
 * a running daemon's directory clean).
 */
static void loop_unlink_unix(void)
{
	if (ax25netd.unix_path[0] != '\0')
		unlink(ax25netd.unix_path);
}

static void shutdown_signal(int sig)
{
	(void)sig;
	loop_unlink_unix();
	_exit(0);
}

/*
 * The daemon only needs to read the configuration, bind the loop port
 * and connect outbound to the upstreams; none of that requires root.
 * When started by root, drop to an unprivileged user right after the
 * sockets are set up.  Returns -1 (fatal) only on explicit errors.
 */
static int drop_privileges(const char *user)
{
	struct passwd *pw;

	if (user == NULL || geteuid() != 0)
		return 0;

	pw = getpwnam(user);
	if (pw == NULL) {
		ax25netd_log(LOG_ERR, "no such user: %s", user);
		return -1;
	}
	if (initgroups(user, pw->pw_gid) != 0) {
		ax25netd_log(LOG_ERR, "initgroups(%s): %s", user, strerror(errno));
		return -1;
	}
	if (setgid(pw->pw_gid) != 0) {
		ax25netd_log(LOG_ERR, "setgid: %s", strerror(errno));
		return -1;
	}
	if (setuid(pw->pw_uid) != 0) {
		ax25netd_log(LOG_ERR, "setuid: %s", strerror(errno));
		return -1;
	}

	ax25netd_log(LOG_INFO, "dropped privileges to %s", user);
	return 0;
}

/* The credentials file lives next to agwpe.conf, called
 * agwpe_shadow.conf.  */
static void shadow_path(const char *conf, char *buf, size_t buflen)
{
	const char *slash = strrchr(conf, '/');

	if (slash == NULL)
		snprintf(buf, buflen, "agwpe_shadow.conf");
	else
		snprintf(buf, buflen, "%.*sagwpe_shadow.conf",
			 (int)(slash - conf + 1), conf);
}

static int daemonize(void)
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

int main(int argc, char **argv)
{
	int ch, port = AX25NETD_PORT_DEFAULT;
	const char *conf = DEFAULT_CONF;
	const char *comconf = DEFAULT_COMMON;
	const char *bindaddr = AX25NETD_BIND_DEFAULT;
	const char *runuser = NULL;
	const char *unix_path = NULL;
	const char *unix_group = NULL;
	int no_tcp = 0;
	int port_set = 0;
	int foreground = 0;
	struct agwpe_config cfg;
	struct ax25common com;
	int i;
	time_t now;

	openlog("ax25netd", LOG_PID, LOG_DAEMON);

	ax25netd.mheard = 1;
	ax25netd.listener_fd = -1;
	ax25netd.unix_fd = -1;

	{
		static const struct option longopts[] = {
			{ "socket",  required_argument, NULL, 'U' },
			{ "group",   required_argument, NULL, 'g' },
			{ "no-tcp",  no_argument,       NULL, 1000 },
			{ "no-mheard", no_argument,     NULL, 'M' },
			{ NULL, 0, NULL, 0 }
		};

		while ((ch = getopt_long(argc, argv, "fdc:C:b:p:U:g:u:h",
					 longopts, NULL)) != -1) {
			switch (ch) {
			case 'f':
				foreground = 1;
				break;
			case 'd':
				ax25netd.debug = 1;
				break;
			case 'c':
				conf = optarg;
				break;
			case 'C':
				comconf = optarg;
				break;
			case 'b':
				bindaddr = optarg;
				break;
			case 'p':
				port = atoi(optarg);
				port_set = 1;
				break;
			case 'U':
				unix_path = optarg;
				break;
			case 'g':
				unix_group = optarg;
				break;
			case 1000:
				no_tcp = 1;
				break;
			case 'u':
				runuser = optarg;
				break;
			case 'M':
				ax25netd.mheard = 0;
				break;
			default:
				usage(argv[0]);
				return 1;
			}
		}
	}

	if (port <= 0 || port > 65535) {
		fprintf(stderr, "ax25netd: invalid port %d\n", port);
		return 1;
	}

	if (agwpe_config_load(conf, &cfg) < 0) {
		if (errno != ENOENT) {
			fprintf(stderr, "ax25netd: cannot load configuration from %s\n", conf);
			return 1;
		}
		fprintf(stderr, "ax25netd: warning: no %s, running without an upstream list\n", conf);
	}
	if (cfg.count == 0)
		fprintf(stderr, "ax25netd: warning: no upstreams configured (loop port only)\n");

	/*
	 * The loop port endpoint comes from the shared ax25common.conf,
	 * the file ax25netd and ax25tcpd read: the daemon listens where
	 * the file says, and the frontend connects to the same endpoint,
	 * so the client side always sees what the daemon listens on.
	 * The libax25 AGWPE shim never reads this file — its only choice
	 * of server is the AXSOCK_HOST/AXSOCK_PORT environment variables
	 * (default 127.0.0.1:8100), which happen to point here by
	 * default.  A missing file leaves the defaults in place (TCP %d,
	 * no unix socket); the command line options below override it.
	 */
	if (ax25common_config_load(comconf, &com) < 0) {
		fprintf(stderr, "ax25netd: cannot load %s\n", comconf);
		agwpe_config_free(&cfg);
		return 1;
	}
	strncpy(cfg.socket_path, com.loop_socket,
		sizeof(cfg.socket_path) - 1);
	cfg.socket_path[sizeof(cfg.socket_path) - 1] = '\0';
	cfg.tcp_enabled = com.loop_tcp_enabled;
	cfg.group_mode = com.group_mode;
	strncpy(cfg.group_name, com.group_name, sizeof(cfg.group_name) - 1);
	cfg.group_name[sizeof(cfg.group_name) - 1] = '\0';
	if (!port_set)
		port = com.loop_tcp_port;

	{
		char shadow[PATH_MAX];

		shadow_path(conf, shadow, sizeof(shadow));
		if (agwpe_config_load_shadow(shadow, &cfg) < 0) {
			fprintf(stderr, "ax25netd: cannot load credentials from %s\n",
				shadow);
			agwpe_config_free(&cfg);
			return 1;
		}
		if (cfg.auth && cfg.nclients == 0) {
			/* With AGWPE_AUTH_EXTERN and a loopback listener no
			 * client ever needs a login, so credentials are
			 * optional there; warn only when some client
			 * actually has to authenticate.  */
			int needs_login = 0;

			if (cfg.auth == AGWPE_AUTH_ALWAYS)
				needs_login = 1;
			else if (cfg.tcp_enabled && !no_tcp &&
				 !host_is_loopback(bindaddr))
				needs_login = 1;

			if (needs_login)
				fprintf(stderr, "ax25netd: warning: client authentication needs credentials, but %s defines none; remote clients cannot log in\n",
					shadow);
		}
	}

	/*
	 * The reserved virtual "loop" upstream is an enable switch for the
	 * local loopback port; it does not occupy an upstream slot, so the
	 * flat loop port numbering of the radio upstreams is preserved.  A
	 * configuration with only the virtual loop port is valid.
	 */
	{
		int nradio = 0;
		int j = 0;
		int saw_loop = 0;

		for (i = 0; i < cfg.count; i++) {
			if (cfg.upstreams[i].virtual)
				continue;
			nradio++;
		}

		if (nradio > 0) {
			ax25netd.ups = calloc(nradio, sizeof(struct ax25netd_upstream));
			if (ax25netd.ups == NULL) {
				agwpe_config_free(&cfg);
				return 1;
			}
		}

		for (i = 0; i < cfg.count; i++) {
			struct agwpe_upstream *u = &cfg.upstreams[i];

			if (u->virtual) {
				ax25netd.loop_enabled = 1;
				ax25netd.loop.index = AGWPE_PORT_LOOP;
				memcpy(ax25netd.loop.name, u->name,
					sizeof(ax25netd.loop.name) - 1);
				ax25netd.loop.name[sizeof(ax25netd.loop.name) - 1] = '\0';
				ax25netd.loop.virtual = 1;
				saw_loop = 1;
				continue;
			}
			memcpy(ax25netd.ups[j].name, u->name,
				sizeof(ax25netd.ups[j].name) - 1);
			ax25netd.ups[j].name[sizeof(ax25netd.ups[j].name) - 1] = '\0';
			memcpy(ax25netd.ups[j].host, u->host,
				sizeof(ax25netd.ups[j].host) - 1);
			ax25netd.ups[j].host[sizeof(ax25netd.ups[j].host) - 1] = '\0';
			memcpy(ax25netd.ups[j].user, u->user,
				sizeof(ax25netd.ups[j].user) - 1);
			ax25netd.ups[j].user[sizeof(ax25netd.ups[j].user) - 1] = '\0';
			memcpy(ax25netd.ups[j].pass, u->pass,
				sizeof(ax25netd.ups[j].pass) - 1);
			ax25netd.ups[j].pass[sizeof(ax25netd.ups[j].pass) - 1] = '\0';
			ax25netd.ups[j].tcp_port = u->tcp_port;
			j++;
		}
		if (!saw_loop) {
			/* No "loop" line in the configuration (or no
			 * configuration at all): enable the virtual
			 * loopback port anyway, so local clients still
			 * have a loop port to use.  */
			ax25netd.loop_enabled = 1;
			ax25netd.loop.index = AGWPE_PORT_LOOP;
			strncpy(ax25netd.loop.name, AGWPE_LOOP_NAME,
				sizeof(ax25netd.loop.name) - 1);
			ax25netd.loop.virtual = 1;
		}
		ax25netd.nup = nradio;
	}

	ax25netd.auth = cfg.auth;
	ax25netd.autoroute = cfg.autoroute;
	ax25netd.clients_auth = cfg.clients;
	ax25netd.nclients_auth = cfg.nclients;
	cfg.clients = NULL;
	cfg.nclients = 0;

	agwpe_config_free(&cfg);

	if (ax25netd.nup == 0 && !ax25netd.loop_enabled) {
		fprintf(stderr, "ax25netd: no radio upstreams configured in %s\n",
			conf);
		return 1;
	}

	/* Command line overrides for the unix socket listener.  */
	if (unix_path != NULL)
		strncpy(cfg.socket_path, unix_path,
			sizeof(cfg.socket_path) - 1);
	if (unix_group != NULL) {
		if (strcasecmp(unix_group, "all") == 0 ||
		    strcmp(unix_group, "*") == 0)
			cfg.group_mode = AGWPE_GROUP_ALL;
		else {
			cfg.group_mode = AGWPE_GROUP_NAMED;
			strncpy(cfg.group_name, unix_group,
				sizeof(cfg.group_name) - 1);
		}
	}
	if (no_tcp)
		cfg.tcp_enabled = 0;

	if (!cfg.tcp_enabled && cfg.socket_path[0] == '\0') {
		fprintf(stderr,
			"ax25netd: no listener enabled: --no-tcp requires a unix socket ('-U <path>' or 'loop socket' in ax25common.conf)\n");
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, shutdown_signal);
	signal(SIGINT, shutdown_signal);

	/*
	 * The unix socket belongs to the user the daemon will run as
	 * after the privilege drop; resolve the uid/gid now, before the
	 * drop.  Both listeners are set up before dropping, so the socket
	 * can live in a root-only directory.
	 */
	{
		uid_t run_uid;
		gid_t run_gid;

		if (runuser != NULL) {
			struct passwd *pw = getpwnam(runuser);

			if (pw == NULL) {
				fprintf(stderr, "ax25netd: no such user: %s\n",
					runuser);
				return 1;
			}
			run_uid = pw->pw_uid;
			run_gid = pw->pw_gid;
		} else {
			run_uid = geteuid();
			run_gid = getegid();
		}

		if (cfg.tcp_enabled) {
			if (loop_init(bindaddr, port) < 0)
				return 1;
		} else {
			ax25netd_log(LOG_INFO, "TCP listener disabled");
		}

		if (cfg.socket_path[0] != '\0' &&
		    loop_init_unix(cfg.socket_path, cfg.group_mode,
				   cfg.group_name, run_uid, run_gid) < 0)
			return 1;
	}

	if (upstream_init_all() < 0)
		return 1;

	/* The heard list file is created before dropping privileges, so a
	 * started-as-root daemon can set up the state directory.  */
	if (ax25netd.mheard)
		ax25netd_mheard_init();
	else
		ax25netd_log(LOG_INFO, "mheard: disabled by --no-mheard");

	if (drop_privileges(runuser) < 0)
		return 1;

	if (!foreground && daemonize() < 0) {
		ax25netd_log(LOG_ERR, "daemonize: %s", strerror(errno));
		return 1;
	}

	for (;;) {
		fd_set rfds, wfds;
		int maxfd;
		struct timeval tv;

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		maxfd = -1;

		if (ax25netd.listener_fd >= 0) {
			FD_SET(ax25netd.listener_fd, &rfds);
			maxfd = ax25netd.listener_fd;
		}
		if (ax25netd.unix_fd >= 0) {
			FD_SET(ax25netd.unix_fd, &rfds);
			if (ax25netd.unix_fd > maxfd)
				maxfd = ax25netd.unix_fd;
		}

		for (i = 0; i < ax25netd.nclients; i++) {
			if (ax25netd.clients[i].fd >= 0) {
				FD_SET(ax25netd.clients[i].fd, &rfds);
				if (ax25netd.clients[i].fd > maxfd)
					maxfd = ax25netd.clients[i].fd;
				/* A client with pending output must be
				 * drained as soon as its socket accepts
				 * more data.  */
				if (ax25netd.clients[i].olen > 0) {
					FD_SET(ax25netd.clients[i].fd, &wfds);
					if (ax25netd.clients[i].fd > maxfd)
						maxfd = ax25netd.clients[i].fd;
				}
			}
		}
		for (i = 0; i < ax25netd.nup; i++) {
			if (ax25netd.ups[i].connected) {
				int fd = agwpe_client_fd(ax25netd.ups[i].cli);

				FD_SET(fd, &rfds);
				if (fd > maxfd)
					maxfd = fd;
			}
		}

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (select(maxfd + 1, &rfds, &wfds, NULL, &tv) < 0) {
			if (errno == EINTR)
				continue;
			ax25netd_log(LOG_ERR, "select: %s", strerror(errno));
			break;
		}

		now = time(NULL);

		if (ax25netd.listener_fd >= 0 &&
		    FD_ISSET(ax25netd.listener_fd, &rfds))
			loop_accept(ax25netd.listener_fd);
		if (ax25netd.unix_fd >= 0 && FD_ISSET(ax25netd.unix_fd, &rfds))
			loop_accept(ax25netd.unix_fd);

		for (i = 0; i < ax25netd.nclients; i++) {
			struct ax25netd_client *cl = &ax25netd.clients[i];

			if (cl->fd >= 0 && !cl->dead &&
			    FD_ISSET(cl->fd, &rfds))
				loop_read_client(cl);
		}

		/* Drain the writable clients.  The output queue also makes
		 * the frames that arrived since the last iteration visible
		 * to the socket, so flush even when the descriptor was not
		 * marked writable: it costs one non-blocking send.  */
		for (i = 0; i < ax25netd.nclients; i++) {
			struct ax25netd_client *cl = &ax25netd.clients[i];

			if (cl->fd >= 0 && !cl->dead &&
			    (FD_ISSET(cl->fd, &wfds) || cl->olen > 0))
				loop_flush_client(cl);
		}

		loop_reap_dead();

		for (i = 0; i < ax25netd.nup; i++) {
			if (ax25netd.ups[i].connected &&
			    FD_ISSET(agwpe_client_fd(ax25netd.ups[i].cli), &rfds))
				upstream_read(&ax25netd.ups[i]);
		}

		upstream_reconnect_tick(now);
		mux_ports_tick(now);
	}

	return 0;
}

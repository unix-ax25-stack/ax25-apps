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
 * Local AGWPE server ("loop port").  AGWPE client programs connect here
 * and their commands are multiplexed to the configured upstreams.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <grp.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "ax25netd.h"

static struct ax25netd_client *client_slot(void)
{
	int i;

	if (ax25netd.clients == NULL) {
		ax25netd.capclients = 16;
		ax25netd.clients = calloc(ax25netd.capclients, sizeof(*ax25netd.clients));
		if (ax25netd.clients == NULL)
			return NULL;
		for (i = 0; i < ax25netd.capclients; i++)
			ax25netd.clients[i].fd = -1;
	}

	for (i = 0; i < ax25netd.nclients; i++)
		if (ax25netd.clients[i].fd == -1)
			return &ax25netd.clients[i];

	if (ax25netd.nclients == ax25netd.capclients) {
		struct ax25netd_client *nc;
		int ncap = ax25netd.capclients * 2;

		nc = realloc(ax25netd.clients, sizeof(*ax25netd.clients) * ncap);
		if (nc == NULL)
			return NULL;
		ax25netd.clients = nc;
		memset(&ax25netd.clients[ax25netd.capclients], 0,
		       sizeof(*ax25netd.clients) * (ncap - ax25netd.capclients));
		for (i = ax25netd.capclients; i < ncap; i++)
			ax25netd.clients[i].fd = -1;
		ax25netd.capclients = ncap;
	}

	i = ax25netd.nclients++;
	ax25netd.clients[i].fd = -1;
	return &ax25netd.clients[i];
}

struct ax25netd_client *client_by_fd(int fd)
{
	int i;

	for (i = 0; i < ax25netd.nclients; i++)
		if (ax25netd.clients[i].fd == fd)
			return &ax25netd.clients[i];
	return NULL;
}

/*
 * True if the sockaddr is a loopback address.  The whole IPv4 127/8
 * counts as local, not just 127.0.0.1.
 */
int addr_is_loopback(const struct sockaddr *sa, socklen_t len)
{
	if (sa == NULL)
		return 0;

	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *in = (const struct sockaddr_in *)sa;

		return len >= sizeof(*in) &&
		       (ntohl(in->sin_addr.s_addr) & 0xff000000) == 0x7f000000;
	}
	if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *in6 =
			(const struct sockaddr_in6 *)sa;

		return len >= sizeof(*in6) && IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
	}
	return 0;
}

/*
 * True if the host string names a loopback address.  Resolves the name
 * with getaddrinfo so that every spelling (127.0.0.1, 127.0.0.2, ::1,
 * "localhost", ...) is covered; unresolvable names count as remote so
 * that the caller warns on the safe side.
 */
int host_is_loopback(const char *host)
{
	struct addrinfo hints, *res;
	int ret, loopback = 0;

	if (host == NULL)
		return 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	ret = getaddrinfo(host, NULL, &hints, &res);
	if (ret != 0)
		return 0;

	{
		struct addrinfo *ai;

		for (ai = res; ai != NULL; ai = ai->ai_next) {
			if (addr_is_loopback(ai->ai_addr, ai->ai_addrlen)) {
				loopback = 1;
				break;
			}
		}
	}
	freeaddrinfo(res);
	return loopback;
}

int loop_init(const char *bindaddr, int port)
{
	struct addrinfo hints, *res, *ai;
	char portstr[16];
	int s, on = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	snprintf(portstr, sizeof(portstr), "%d", port);

	if (getaddrinfo(bindaddr, portstr, &hints, &res) != 0) {
		ax25netd_log(LOG_ERR, "loop: getaddrinfo(%s): %s", bindaddr,
			 strerror(EADDRNOTAVAIL));
		return -1;
	}

	for (ai = res; ai != NULL; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s < 0)
			continue;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		if (bind(s, ai->ai_addr, ai->ai_addrlen) == 0 &&
		    listen(s, 16) == 0)
			break;
		close(s);
	}
	freeaddrinfo(res);

	if (ai == NULL) {
		ax25netd_log(LOG_ERR, "loop: cannot listen on %s:%d", bindaddr, port);
		return -1;
	}

	/*
	 * Listening beyond loopback hands the radio to every caller that
	 * can reach this address.  That is only acceptable together with
	 * client authentication, so warn loudly when it is disabled.
	 */
	if (!addr_is_loopback(ai->ai_addr, ai->ai_addrlen) && !ax25netd.auth)
		ax25netd_log(LOG_WARNING,
			 "listening on non-loopback %s:%d without authentication: anyone who can reach this address controls your radio; consider 'auth required' in %s",
			 bindaddr, port, "agwpe.conf");

	ax25netd.listener_fd = s;
	ax25netd_log(LOG_INFO, "listening for AGWPE clients on %s:%d", bindaddr, port);
	return 0;
}

/*
 * Create the parent directory of a unix socket path.  Runtime
 * directories are volatile (/run on Linux is tmpfs and empty after
 * every reboot), so the daemon creates it itself.  Returns 0 on
 * success.  mode and the owning (uid, gid) apply to a directory this
 * call creates; an existing directory is left alone.
 */
static int loop_mkdir_parent(const char *path, mode_t mode,
			     uid_t uid, gid_t gid)
{
	char *dup, *slash, *dir;
	struct stat st;

	if (path[0] == '\0')
		return -1;

	dup = strdup(path);
	if (dup == NULL)
		return -1;

	slash = strrchr(dup, '/');
	if (slash == NULL)
		dir = ".";
	else if (slash == dup)
		dir = "/";
	else {
		*slash = '\0';
		dir = dup;
	}

	if (stat(dir, &st) == 0) {
		if (!S_ISDIR(st.st_mode)) {
			free(dup);
			errno = ENOTDIR;
			return -1;
		}
		free(dup);
		return 0;
	}
	if (errno != ENOENT) {
		free(dup);
		return -1;
	}

	if (mkdir(dir, mode) != 0) {
		free(dup);
		return -1;
	}
	if (chown(dir, uid, gid) != 0) {
		free(dup);
		return -1;
	}

	free(dup);
	return 0;
}

/*
 * Set up the unix domain socket listener for AGWPE clients.
 *
 * group_mode / group_name follow agwpe.conf's "group" directive:
 *
 *   AGWPE_GROUP_DEFAULT  the socket belongs to the daemon run user
 *                        (run_uid:run_gid), mode 0660;
 *   AGWPE_GROUP_NAMED    the socket belongs to run_uid and the given
 *                        group (name or numeric gid), mode 0660, so
 *                        only members of that group can connect;
 *   AGWPE_GROUP_ALL      mode 0666, every local user can connect.
 *
 * The parent directory is created with mode 0750 (0755 for
 * AGWPE_GROUP_ALL) and owned by (run_uid, gid); its setgid bit makes
 * the socket inherit the group even when it is recreated by another
 * process.  A stale socket file from a previous run is removed.
 * Returns 0 on success and stores the descriptor and path in ax25netd.
 */
int loop_init_unix(const char *path, int group_mode, const char *group_name,
		   uid_t run_uid, gid_t run_gid)
{
	struct sockaddr_un sa;
	struct stat st;
	gid_t gid = run_gid;
	mode_t dir_mode = 0750, sock_mode = 0660;
	int s;

	if (path == NULL || path[0] == '\0')
		return -1;

	if (strlen(path) >= sizeof(sa.sun_path)) {
		ax25netd_log(LOG_ERR, "loop: unix socket path too long: %s", path);
		return -1;
	}

	if (group_mode == AGWPE_GROUP_ALL) {
		dir_mode = 0755;
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
				ax25netd_log(LOG_ERR,
					 "loop: no such group '%s' for the unix socket",
					 group_name);
				return -1;
			}
			gid = gr->gr_gid;
		}
		dir_mode |= S_ISGID;
	}

	if (loop_mkdir_parent(path, dir_mode, run_uid, gid) != 0) {
		ax25netd_log(LOG_ERR, "loop: cannot create directory for %s: %s",
			 path, strerror(errno));
		return -1;
	}

	/* A stale socket from a previous run blocks the bind.  Only
	 * remove it when it really is a socket, never another file.  */
	if (lstat(path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			ax25netd_log(LOG_ERR,
				 "loop: %s exists and is not a socket; not removing it",
				 path);
			return -1;
		}
		if (unlink(path) != 0) {
			ax25netd_log(LOG_ERR, "loop: cannot remove stale socket %s: %s",
				 path, strerror(errno));
			return -1;
		}
	} else if (errno != ENOENT) {
		ax25netd_log(LOG_ERR, "loop: cannot inspect %s: %s", path,
			 strerror(errno));
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) {
		ax25netd_log(LOG_ERR, "loop: unix socket: %s", strerror(errno));
		return -1;
	}

	if (bind(s, (struct sockaddr *)&sa, SUN_LEN(&sa)) != 0 ||
	    listen(s, 16) != 0) {
		ax25netd_log(LOG_ERR, "loop: cannot listen on %s: %s", path,
			 strerror(errno));
		close(s);
		return -1;
	}

	/* Set the permissions through the path: macOS does not allow
	 * fchmod(2)/fchown(2) on socket descriptors, while chmod(2) on
	 * the path works everywhere.  A non-root daemon already owns
	 * its own socket, so only chown when privileged.  */
	if (chmod(path, sock_mode) != 0 ||
	    (geteuid() == 0 && chown(path, run_uid, gid) != 0)) {
		ax25netd_log(LOG_ERR, "loop: cannot set ownership of %s: %s",
			 path, strerror(errno));
		close(s);
		unlink(path);
		return -1;
	}

	if (geteuid() == 0 && run_uid == 0 && group_mode == AGWPE_GROUP_DEFAULT)
		ax25netd_log(LOG_WARNING,
			 "unix socket %s is root-only; the daemon should drop privileges with -u so its own user can connect",
			 path);

	ax25netd.unix_fd = s;
	strncpy(ax25netd.unix_path, path, sizeof(ax25netd.unix_path) - 1);
	ax25netd.unix_group_mode = group_mode;
	ax25netd.unix_group_gid = gid;
	ax25netd_log(LOG_INFO, "listening for AGWPE clients on unix socket %s", path);
	return 0;
}

/*
 * Validate a login ('P') frame against the credentials from
 * agwpe_shadow.conf.  The data area layout mirrors agwpe_client_login():
 * the user name occupies bytes 0..253, the password bytes 255..508,
 * both NUL padded.  Returns 0 on match, -1 otherwise.
 */
static int loop_check_login(struct ax25netd_client *cl,
			    const unsigned char *data, size_t len)
{
	char user[AGWPE_AUTH_NAME_MAX + 1];
	char pass[AGWPE_AUTH_PASS_MAX + 1];
	size_t n, i;

	(void)cl;

	if (data == NULL || len < 255)
		return -1;

	memset(user, 0, sizeof(user));
	memset(pass, 0, sizeof(pass));

	n = (len < 255 + AGWPE_AUTH_NAME_MAX) ? len - 255 : AGWPE_AUTH_NAME_MAX;
	memcpy(user, data, n);
	n = (len < 255 + AGWPE_AUTH_PASS_MAX) ? len - 255 : AGWPE_AUTH_PASS_MAX;
	memcpy(pass, data + 255, n);

	for (i = 0; i < ax25netd.nclients_auth; i++) {
		if (strcmp(ax25netd.clients_auth[i].user, user) == 0 &&
		    strcmp(ax25netd.clients_auth[i].pass, pass) == 0)
			return 0;
	}
	return -1;
}

/*
 * Whether a freshly accepted client may skip the login.  With
 * AGWPE_AUTH_EXTERN (the default) only peers connecting over loopback
 * are trusted; AGWPE_AUTH_ALWAYS trusts nobody and AGWPE_AUTH_OFF trusts
 * everybody.
 */
static int loop_peer_trusted(int fd)
{
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);

	if (ax25netd.auth == AGWPE_AUTH_OFF)
		return 1;
	if (ax25netd.auth == AGWPE_AUTH_ALWAYS)
		return 0;

	if (getpeername(fd, (struct sockaddr *)&ss, &slen) != 0)
		return 0;

	if (ss.ss_family == AF_UNIX) {
		/* A unix domain socket is gated by the file permissions
		 * of the socket: connecting requires write access.  That
		 * is the authentication, so no login is needed.  */
		return 1;
	}
	if (ss.ss_family == AF_INET) {
		const struct sockaddr_in *in =
			(const struct sockaddr_in *)&ss;

		return (ntohl(in->sin_addr.s_addr) & 0xff000000) == 0x7f000000;
	}
	if (ss.ss_family == AF_INET6) {
		const struct sockaddr_in6 *in6 =
			(const struct sockaddr_in6 *)&ss;

		return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
	}
	return 0;
}

void loop_accept(int lfd)
{
	struct ax25netd_client *cl;
	int fd;

	fd = accept(lfd, NULL, NULL);
	if (fd < 0) {
		ax25netd_log(LOG_WARNING, "loop: accept: %s", strerror(errno));
		return;
	}

	cl = client_slot();
	if (cl == NULL) {
		close(fd);
		ax25netd_log(LOG_WARNING, "loop: out of client slots");
		return;
	}

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

	/* Frames are small and sent one at a time; Nagle would hold each
	 * until the previous ACK and cap throughput at one frame per RTT.
	 */
	{
		int one = 1;

		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,
				 sizeof(one));
	}

	cl->fd = fd;
	cl->dead = 0;
	cl->authed = loop_peer_trusted(fd);
	cl->rbuf = malloc(4096);
	cl->ra = 4096;
	cl->rlen = 0;
	cl->monitor = 0;
	cl->raw = 0;
	if (cl->rbuf == NULL) {
		cl->dead = 1;
		loop_close_client(cl);
		return;
	}

	ax25netd_log(LOG_INFO, "AGWPE client %d connected", fd);
}

void loop_read_client(struct ax25netd_client *cl)
{
	unsigned char tmp[4096];
	ssize_t n;

	n = read(cl->fd, tmp, sizeof(tmp));
	if (n < 0 && errno == EAGAIN)
		return;
	if (n <= 0) {
		cl->dead = 1;
		return;
	}

	if (cl->rlen + n > cl->ra) {
		size_t ns = cl->ra;
		unsigned char *nb;

		while (ns < cl->rlen + n) {
			ns *= 2;
			if (ns > AX25NETD_BUF_MAX)
				ns = AX25NETD_BUF_MAX;
		}
		if (ns < cl->rlen + n) {
			cl->dead = 1;
			return;
		}
		nb = realloc(cl->rbuf, ns);
		if (nb == NULL) {
			cl->dead = 1;
			return;
		}
		cl->rbuf = nb;
		cl->ra = ns;
	}
	memcpy(cl->rbuf + cl->rlen, tmp, n);
	cl->rlen += n;

	while (cl->rlen >= AGWPE_HEADER_LEN) {
		struct agwpe_s hdr;
		uint32_t dlen;
		size_t total;

		memcpy(&hdr, cl->rbuf, AGWPE_HEADER_LEN);
		dlen = agwpe_netle2host(hdr.data_len);
		if (dlen > AX25NETD_BUF_MAX - AGWPE_HEADER_LEN) {
			cl->dead = 1;
			return;
		}
		total = AGWPE_HEADER_LEN + dlen;
		if (cl->rlen < total)
			break;

		if (!cl->authed) {
			/* Until the client has logged in, only the login
			 * frame itself is accepted; every other frame is
			 * silently dropped.  */
			if (hdr.datakind == AGWPE_CMD_LOGIN) {
				if (loop_check_login(cl, cl->rbuf + AGWPE_HEADER_LEN,
						     dlen) == 0) {
					cl->authed = 1;
					ax25netd_log(LOG_INFO,
						 "client %d: login accepted",
						 cl->fd);
				} else {
					ax25netd_log(LOG_WARNING,
						 "client %d: login rejected, dropping",
						 cl->fd);
					cl->dead = 1;
				}
			}
			cl->rlen -= total;
			memmove(cl->rbuf, cl->rbuf + total, cl->rlen);
			continue;
		}

		mux_client_command(cl, &hdr, cl->rbuf + AGWPE_HEADER_LEN, dlen);

		cl->rlen -= total;
		memmove(cl->rbuf, cl->rbuf + total, cl->rlen);
	}
}

void loop_close_client(struct ax25netd_client *cl)
{
	if (cl == NULL || cl->fd < 0)
		return;

	ax25netd_log(LOG_INFO, "AGWPE client %d disconnected", cl->fd);
	mux_client_disconnect(cl);
	close(cl->fd);
	cl->fd = -1;
	free(cl->rbuf);
	cl->rbuf = NULL;
	cl->rlen = cl->ra = 0;
	free(cl->obuf);
	cl->obuf = NULL;
	cl->olen = cl->oa = 0;
}

void loop_reap_dead(void)
{
	int i;

	for (i = 0; i < ax25netd.nclients; i++) {
		if (ax25netd.clients[i].fd >= 0 && ax25netd.clients[i].dead)
			loop_close_client(&ax25netd.clients[i]);
	}
}

/*
 * Send as much of a client's output queue as the socket accepts.  The
 * client descriptor is non-blocking, so a full socket buffer simply
 * leaves the remainder queued; the main loop calls this again when the
 * descriptor reports writable.  Returns 0 while the queue is empty or
 * blocked without error, -1 when the connection is broken.
 */
void loop_flush_client(struct ax25netd_client *cl)
{
	while (cl != NULL && cl->fd >= 0 && cl->olen > 0 && !cl->dead) {
		ssize_t n;

		n = send(cl->fd, cl->obuf, cl->olen, 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			ax25netd_log(LOG_WARNING, "client %d: send: %s",
				 cl->fd, strerror(errno));
			cl->dead = 1;
			return;
		}
		if (n == 0) {
			cl->dead = 1;
			return;
		}
		cl->olen -= n;
		memmove(cl->obuf, cl->obuf + n, cl->olen);
	}
}

int loop_send_client(struct ax25netd_client *cl, const struct agwpe_s *hdr,
		     const unsigned char *data, size_t len)
{
	struct agwpe_s out;
	size_t total;
	unsigned char *p;

	if (cl == NULL || cl->fd < 0 || cl->dead)
		return -1;

	total = AGWPE_HEADER_LEN + len;
	if (cl->olen + total > AX25NETD_OUT_MAX) {
		ax25netd_log(LOG_ERR,
			 "client %d: output queue exceeds %d bytes, dropping",
			 cl->fd, AX25NETD_OUT_MAX);
		cl->dead = 1;
		return -1;
	}

	/* Grow the queue buffer to hold the whole frame.  */
	if (cl->olen + total > cl->oa) {
		size_t ns = cl->oa ? cl->oa : 4096;
		unsigned char *nb;

		while (ns < cl->olen + total)
			ns *= 2;
		nb = realloc(cl->obuf, ns);
		if (nb == NULL) {
			ax25netd_log(LOG_ERR, "client %d: out of memory queueing %zu bytes",
				 cl->fd, total);
			cl->dead = 1;
			return -1;
		}
		cl->obuf = nb;
		cl->oa = ns;
	}

	out = *hdr;
	out.data_len = agwpe_host2netle(len);
	p = cl->obuf + cl->olen;
	memcpy(p, &out, AGWPE_HEADER_LEN);
	if (len > 0 && data != NULL)
		memcpy(p + AGWPE_HEADER_LEN, data, len);
	cl->olen += total;

	/* Try to get the frame out now; whatever the socket cannot take
	 * stays queued and is sent from the main loop once the descriptor
	 * is writable again.  */
	loop_flush_client(cl);

	return cl->dead ? -1 : 0;
}

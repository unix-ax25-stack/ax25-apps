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
#include <syslog.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "netd.h"

static struct netd_client *client_slot(void)
{
	int i;

	if (netd.clients == NULL) {
		netd.capclients = 16;
		netd.clients = calloc(netd.capclients, sizeof(*netd.clients));
		if (netd.clients == NULL)
			return NULL;
		for (i = 0; i < netd.capclients; i++)
			netd.clients[i].fd = -1;
	}

	for (i = 0; i < netd.nclients; i++)
		if (netd.clients[i].fd == -1)
			return &netd.clients[i];

	if (netd.nclients == netd.capclients) {
		struct netd_client *nc;
		int ncap = netd.capclients * 2;

		nc = realloc(netd.clients, sizeof(*netd.clients) * ncap);
		if (nc == NULL)
			return NULL;
		netd.clients = nc;
		memset(&netd.clients[netd.capclients], 0,
		       sizeof(*netd.clients) * (ncap - netd.capclients));
		for (i = netd.capclients; i < ncap; i++)
			netd.clients[i].fd = -1;
		netd.capclients = ncap;
	}

	i = netd.nclients++;
	netd.clients[i].fd = -1;
	return &netd.clients[i];
}

struct netd_client *client_by_fd(int fd)
{
	int i;

	for (i = 0; i < netd.nclients; i++)
		if (netd.clients[i].fd == fd)
			return &netd.clients[i];
	return NULL;
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
		netd_log(LOG_ERR, "loop: getaddrinfo(%s): %s", bindaddr,
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
		netd_log(LOG_ERR, "loop: cannot listen on %s:%d", bindaddr, port);
		return -1;
	}

	/*
	 * Listening beyond loopback hands the radio to every caller that
	 * can reach this address.  That is only acceptable together with
	 * client authentication, so warn loudly when it is disabled.
	 */
	{
		int is_loop = 0;

		if (ai->ai_family == AF_INET) {
			const struct sockaddr_in *in =
				(const struct sockaddr_in *)ai->ai_addr;

			is_loop = in->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
		} else if (ai->ai_family == AF_INET6) {
			const struct sockaddr_in6 *in6 =
				(const struct sockaddr_in6 *)ai->ai_addr;

			is_loop = IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
		}

		if (!is_loop && !netd.auth)
			netd_log(LOG_WARNING,
				 "listening on non-loopback %s:%d without authentication: anyone who can reach this address controls your radio; consider 'auth required' in %s",
				 bindaddr, port, "agwpe.conf");
	}

	netd.listener_fd = s;
	netd_log(LOG_INFO, "listening for AGWPE clients on %s:%d", bindaddr, port);
	return 0;
}

/*
 * Validate a login ('P') frame against the credentials from
 * agwpe_shadow.conf.  The data area layout mirrors agwpe_client_login():
 * the user name occupies bytes 0..253, the password bytes 255..508,
 * both NUL padded.  Returns 0 on match, -1 otherwise.
 */
static int loop_check_login(struct netd_client *cl,
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

	for (i = 0; i < netd.nclients_auth; i++) {
		if (strcmp(netd.clients_auth[i].user, user) == 0 &&
		    strcmp(netd.clients_auth[i].pass, pass) == 0)
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

	if (netd.auth == AGWPE_AUTH_OFF)
		return 1;
	if (netd.auth == AGWPE_AUTH_ALWAYS)
		return 0;

	if (getpeername(fd, (struct sockaddr *)&ss, &slen) != 0)
		return 0;

	if (ss.ss_family == AF_INET) {
		const struct sockaddr_in *in =
			(const struct sockaddr_in *)&ss;

		/* The whole 127/8 is local, not just 127.0.0.1.  */
		return (ntohl(in->sin_addr.s_addr) & 0xff000000) == 0x7f000000;
	}
	if (ss.ss_family == AF_INET6) {
		const struct sockaddr_in6 *in6 =
			(const struct sockaddr_in6 *)&ss;

		return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
	}
	return 0;
}

void loop_accept(void)
{
	struct netd_client *cl;
	int fd;

	fd = accept(netd.listener_fd, NULL, NULL);
	if (fd < 0) {
		netd_log(LOG_WARNING, "loop: accept: %s", strerror(errno));
		return;
	}

	cl = client_slot();
	if (cl == NULL) {
		close(fd);
		netd_log(LOG_WARNING, "loop: out of client slots");
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

	netd_log(LOG_INFO, "AGWPE client %d connected", fd);
}

void loop_read_client(struct netd_client *cl)
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
			if (ns > NETD_BUF_MAX)
				ns = NETD_BUF_MAX;
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
		if (dlen > NETD_BUF_MAX - AGWPE_HEADER_LEN) {
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
					netd_log(LOG_INFO,
						 "client %d: login accepted",
						 cl->fd);
				} else {
					netd_log(LOG_WARNING,
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

void loop_close_client(struct netd_client *cl)
{
	if (cl == NULL || cl->fd < 0)
		return;

	netd_log(LOG_INFO, "AGWPE client %d disconnected", cl->fd);
	mux_client_disconnect(cl);
	close(cl->fd);
	cl->fd = -1;
	free(cl->rbuf);
	cl->rbuf = NULL;
	cl->rlen = cl->ra = 0;
}

void loop_reap_dead(void)
{
	int i;

	for (i = 0; i < netd.nclients; i++) {
		if (netd.clients[i].fd >= 0 && netd.clients[i].dead)
			loop_close_client(&netd.clients[i]);
	}
}

int loop_send_client(struct netd_client *cl, const struct agwpe_s *hdr,
		     const unsigned char *data, size_t len)
{
	struct agwpe_s out;
	ssize_t n;

	if (cl == NULL || cl->fd < 0)
		return -1;

	out = *hdr;
	out.data_len = agwpe_host2netle(out.data_len);

	n = send(cl->fd, &out, AGWPE_HEADER_LEN, 0);
	if (n != AGWPE_HEADER_LEN)
		goto fail;

	if (len > 0 && data != NULL) {
		size_t sent = 0;

		while (sent < len) {
			n = send(cl->fd, data + sent, len - sent, 0);
			if (n <= 0)
				goto fail;
			sent += n;
		}
	}

	return 0;

fail:
	/* Cannot keep up; drop the client.  */
	cl->dead = 1;
	return -1;
}

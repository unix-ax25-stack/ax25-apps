/* libax25/ax25-apps AGWPE userspace netd - route resolution
 *
 * Copyright (C) 2026 Thomas Osterried
 * Copyright (C) 2026 The AGWPE userspace project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */
/*
 * Automatic digipeater resolution for connects without an explicit
 * digipeater path.  The daemon is the single place that resolves the
 * path, mirroring the kernel AX.25 stack: before a bare connect (no
 * 'v' digipeaters) is sent to a radio upstream, the ax25rtd route
 * cache is asked for the destination on that port and the learned
 * path is used instead.  An unresponsive or absent ax25rtd is not an
 * error: the caller falls back to the plain connect.
 *
 * The lookup is done over the ax25rtd control socket, a short
 * synchronous "get ax25 <call>\n" request that is answered with one
 * line per learned route (call, device, timestamp, optional digipeater
 * list) followed by a "." line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <netax25/agwpe.h>

#include "../pathnames.h"
#include "netd.h"

#define	RTD_TIMEOUT	1000	/* ms, per poll step */

/*
 * Ask the ax25rtd route cache for a digipeater path to 'call' on the
 * given upstream.  Returns the number of digipeaters (0 when none is
 * known or ax25rtd is not running) and fills 'buf' with the AGWPE 'v'
 * payload: a leading byte with the digi count followed by the packed
 * callsigns.  buf must hold at least 1 + 7 * AGWPE_MAX_CALL bytes.
 */
int netd_route_lookup(struct netd_upstream *u, const char *call,
		      unsigned char *buf, size_t bufsz)
{
	char cmd[256];
	char rbuf[2048];
	struct sockaddr_un sa;
	struct pollfd pfd;
	int fd, n, ndigi = 0, done = 0, off = 0;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return 0;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, DATA_AX25ROUTED_CTL_SOCK,
		sizeof(sa.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&sa, SUN_LEN(&sa)) < 0) {
		if (getenv("AX25NETD_DEBUG"))
			netd_log(LOG_DEBUG, "autoroute: ax25rtd not reachable: %s",
				 strerror(errno));
		close(fd);
		return 0;
	}

	snprintf(cmd, sizeof(cmd), "get ax25 %s\n", call);
	if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd))
		goto out;

	/* Collect the answer until the terminating ".\n" line, the
	 * buffer is full or we time out.  */
	while (!done && off < (int)sizeof(rbuf) - 1) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, RTD_TIMEOUT) <= 0)
			break;
		n = read(fd, rbuf + off, sizeof(rbuf) - 1 - off);
		if (n <= 0)
			break;
		off += n;
		rbuf[off] = '\0';
		if (strstr(rbuf, ".\n") != NULL)
			done = 1;
	}
	if (!done)
		goto out;

	/* Parse the reply: one route per line.  The device must match
	 * the upstream the connect is headed for.  A route on another
	 * port is simply not used.  */
	{
		char *save = NULL;
		char *line = strtok_r(rbuf, "\n", &save);

		while (line != NULL && ndigi == 0) {
			char *p = line;

			if (line[0] == '.')
				break;
			/* call device timestamp [digis ...] */
			{
				char *t, *d, *ts;

				t = strtok_r(p, " ", &p);
				d = strtok_r(NULL, " ", &p);
				ts = strtok_r(NULL, " ", &p);
				if (t == NULL || d == NULL || ts == NULL)
					goto nextline;
				if (strcmp(d, u->name) != 0)
					goto nextline;
			}
			/* remaining tokens are the digipeater path */
			while (ndigi < AGWPE_MAX_DIGIS - 1) {
				char *digi = strtok_r(NULL, " ", &p);
				if (digi == NULL)
					break;
				agwpe_call_pack((char *)(buf + 1 +
					    ndigi * AGWPE_MAX_CALL), digi);
				ndigi++;
			}
			if (ndigi > 0)
				buf[0] = ndigi;
nextline:
			line = strtok_r(NULL, "\n", &save);
		}
	}

out:
	close(fd);
	return ndigi;
}

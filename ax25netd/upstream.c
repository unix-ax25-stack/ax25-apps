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
 * Upstream AGWPE server connections.  One agwpe_client_t per configured
 * upstream; every frame received from an upstream is handed to the
 * multiplexer (mux_upstream_frame) via the raw_frame callback, so the
 * bytes are forwarded to the loop clients exactly as received.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <syslog.h>

#include "netd.h"

static void on_raw_frame(agwpe_client_t *c, const struct agwpe_s *hdr,
			 const unsigned char *data, size_t len)
{
	struct netd_upstream *u = agwpe_client_opaque(c);

	mux_upstream_frame(u, hdr, data, len);
}

/*
 * The upstream answered the 'G' port information query.  Every "PortN
 * description" token names one radio channel; the wire port byte of
 * channel N-1 is used for all frames sent towards the upstream, so the
 * flat loop port number of that channel is i*16+N-1.
 */
static void on_ports(agwpe_client_t *c, struct agwpe_port_list *list)
{
	struct netd_upstream *u = agwpe_client_opaque(c);
	int old = u->nports;
	int i;

	u->nports = 0;
	for (i = 0; i < list->count && u->nports < NETD_PORT_STRIDE; i++) {
		int n = atoi(list->names[i] + 4);	/* strip "Port" */

		if (n < 1 || n > NETD_PORT_STRIDE) {
			netd_log(LOG_WARNING,
				 "upstream %s: channel '%s' outside 1..%d ignored",
				 u->name, list->names[i], NETD_PORT_STRIDE);
			continue;
		}
		u->ports[u->nports].chan = (unsigned char)(n - 1);
		strncpy(u->ports[u->nports].desc, list->descs[i],
			sizeof(u->ports[u->nports].desc) - 1);
		u->ports[u->nports].desc[sizeof(u->ports[u->nports].desc) - 1] = '\0';
		u->nports++;
	}

	if (!u->ports_ready)
		netd_log(LOG_INFO, "upstream %s: %d channel%s via 'G'",
			 u->name, u->nports, u->nports == 1 ? "" : "s");
	else if (u->nports != old)
		netd_log(LOG_INFO, "upstream %s: now %d channel%s (was %d)",
			 u->name, u->nports, u->nports == 1 ? "" : "s", old);

	u->ports_ready = 1;
	mux_ports_tick(time(NULL));
}

static void upstream_connect(struct netd_upstream *u)
{
	if (u->virtual)
		return;

	if (agwpe_client_connect_host(u->cli, u->host, u->tcp_port) < 0) {
		netd_log(LOG_WARNING, "upstream %s: connect to %s:%d failed: %s",
			 u->name, u->host, u->tcp_port,
			 strerror(agwpe_client_err(u->cli)));
		u->connected = 0;
		u->dead = 1;
		u->dead_since = time(NULL);
		return;
	}

	u->connected = 1;
	u->dead = 0;

	/* If credentials are configured for this upstream, present them;
	 * otherwise send an empty login, which plain servers such as
	 * Direwolf ignore.  */
	if (u->user[0] != '\0')
		agwpe_client_login(u->cli, u->user, u->pass);
	else
		agwpe_client_login(u->cli, "", "");
	mux_upstream_reconnected(u);

	netd_log(LOG_INFO, "upstream %s: connected to %s:%d",
		 u->name, u->host, u->tcp_port);
}

int upstream_init_all(void)
{
	static const struct agwpe_client_cb cb = {
		.raw_frame = on_raw_frame,
		.ports = on_ports,
	};
	int i;

	for (i = 0; i < netd.nup; i++) {
		struct netd_upstream *u = &netd.ups[i];

		if (u->virtual)
			continue;

		u->index = i;
		u->cli = agwpe_client_new(&cb, u);
		if (u->cli == NULL)
			return -1;
		u->heard_to = -1;
		u->out_to = -1;
		u->connected = 0;
		u->dead = 0;
		u->monitor_on = 0;
		u->raw_on = 0;

		upstream_connect(u);
	}

	return 0;
}

void upstream_read(struct netd_upstream *u)
{
	int ret;

	ret = agwpe_client_recv(u->cli);
	if (ret < 0) {
		netd_log(LOG_WARNING, "upstream %s: connection lost (%s)",
			 u->name, agwpe_client_err(u->cli) ?
			 strerror(agwpe_client_err(u->cli)) : "EOF");
		mux_upstream_lost(u);
		u->connected = 0;
		u->dead = 1;
		u->dead_since = time(NULL);
	}
}

void upstream_reconnect_tick(time_t now)
{
	int i;

	for (i = 0; i < netd.nup; i++) {
		struct netd_upstream *u = &netd.ups[i];

		if (u->dead && now - u->dead_since >= NETD_RECONNECT_DELAY)
			upstream_connect(u);
	}
}

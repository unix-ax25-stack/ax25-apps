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
 * Multiplexing and routing.
 *
 * Each radio upstream occupies a block of NETD_PORT_STRIDE loop port
 * numbers (channel c of upstream i is loop port i*16+c).  A call sign
 * registered by a loop client on a port is owned by that client; frames
 * coming back from the upstream which are addressed to such a call sign
 * are delivered to its owner.  Monitor and raw frames are broadcast to
 * every subscribed client.
 *
 * The channel layout of an upstream is learned from its 'G' port info
 * reply: channel 0..N-1 of upstream i appear as flat loop ports i*16 ..
 * i*16+N-1.  Until the reply arrives, a client 'G' request is deferred
 * (mux_ports_tick), so the table never shows half-known upstreams.
 *
 * Port AGWPE_PORT_LOOP is the virtual loop upstream: instead of a radio,
 * it bridges frames between local clients, so a "call" on the loop port
 * reaches a service that registered a listening call sign there.  A
 * listener registered without an SSID (SSID 0) serves any SSID of its
 * base call.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <netax25/ax25.h>
#include <netax25/axlib.h>

#include "netd.h"

/* Mirror of local outbound frames, sent to raw monitor clients.  */
#define	MUX_RAW_MAX	2048

static struct netd_upstream *up_by_port(unsigned char port)
{
	if (port == AGWPE_PORT_LOOP && netd.loop_enabled)
		return &netd.loop;
	if (port / NETD_PORT_STRIDE >= netd.nup)
		return NULL;
	return &netd.ups[port / NETD_PORT_STRIDE];
}

/* The radio channel of upstream u that a flat loop port selects.  */
static unsigned char port_chan(const struct netd_upstream *u,
			       unsigned char port)
{
	return (unsigned char)(port - u->index * NETD_PORT_STRIDE);
}

/* The flat loop port number of channel c of upstream u.  */
static unsigned char port_flat(const struct netd_upstream *u,
			       unsigned char chan)
{
	return (unsigned char)(u->index * NETD_PORT_STRIDE + chan);
}

/* *out = *hdr with the port rewritten to the upstream's channel byte.  */
static void port_hdr(struct agwpe_s *out, const struct netd_upstream *u,
		     const struct agwpe_s *hdr)
{
	*out = *hdr;
	out->port = port_chan(u, hdr->port);
}

/* *out = *hdr with a channel byte rewritten to the flat loop port.  */
static void up_hdr(struct agwpe_s *out, const struct netd_upstream *u,
		   const struct agwpe_s *hdr)
{
	*out = *hdr;
	out->port = port_flat(u, hdr->port);
}

/* Send a client frame towards the upstream with the port rewritten.  */
static int client_send_upstream(struct netd_upstream *u,
				const struct agwpe_s *hdr,
				const unsigned char *data)
{
	struct agwpe_s ph;

	port_hdr(&ph, u, hdr);
	return agwpe_client_send_frame(u->cli, &ph, data);
}

/* Deliver an upstream frame to a loop client with the port rewritten.  */
static int loop_send_upstream(struct netd_client *cl,
			      struct netd_upstream *u,
			      const struct agwpe_s *hdr,
			      const unsigned char *data, size_t len)
{
	struct agwpe_s uh;

	up_hdr(&uh, u, hdr);
	return loop_send_client(cl, &uh, data, len);
}

/*
 * Deliver a raw ('K') frame to every client that turned on raw monitor.
 * The header carries the original port so listen shows it on the right
 * interface, and the data is KISS encapsulated exactly as the upstream
 * would send it (data[0] is the KISS data marker, stripped by listen).
 */
static void mux_mirror_send(const struct agwpe_s *in,
			    const unsigned char *data, size_t len)
{
	struct agwpe_s hdr;
	int i;

	agwpe_header_init(&hdr, in->port, AGWPE_DK_RAW, 0,
			  in->call_from, in->call_to, len);
	for (i = 0; i < netd.nclients; i++) {
		struct netd_client *cl = &netd.clients[i];

		if (cl->fd >= 0 && cl->raw)
			loop_send_client(cl, &hdr, data, len);
	}
}

/*
 * Mirror a frame a client is about to send outwards to every raw monitor
 * client (listen).  This is the transmit leg: without a packet socket on
 * macOS, netd stands in for the kernel and shows local outbound traffic
 * even while the radio upstream is unreachable.  Frames received from
 * the radio continue to come from the upstream as 'K', so there is no
 * doubling.  The raw frame is rebuilt from the AGWPE header fields: dest
 * and source addresses, optional digipeaters, a control byte derived
 * from the frame kind, the PID and the information field.
 */
static void mux_mirror_raw(const struct agwpe_s *in,
			   const unsigned char *data, size_t len)
{
	struct full_sockaddr_ax25 fsa;
	const unsigned char *info = data;
	unsigned char buf[MUX_RAW_MAX + 1];
	size_t ilen = len, ndigis = 0, i;
	unsigned char *p, ctl;

	if (data == NULL)
		ilen = 0;

	if (in->datakind == AGWPE_CMD_RAW) {
		/* The client supplied the raw frame itself.  */
		{
			struct netd_upstream *u = up_by_port(in->port);

			if (u != NULL && u != &netd.loop)
				netd_mheard_frame(u, data, len);
		}
		mux_mirror_send(in, data, len);
		return;
	}

	if (in->call_to[0] == '\0' || in->call_from[0] == '\0')
		return;

	buf[0] = 0;			/* KISS data marker, like direwolf */
	p = buf + 1;

	/* Encode the address flags the way direwolf transmits them, so a
	 * mirrored TX frame decodes in listen exactly like the same frame
	 * would if it came back from the radio: destination carries the
	 * H and reserved RR bits, source and digipeaters the RR bits, and
	 * the last address the end-of-field bit.  The RR bits left clear
	 * would otherwise make listen report a phantom "EAX25" (bit 0x40)
	 * and "[DAMA]" (bit 0x20) station.  */
	if (ax25_aton(in->call_to, &fsa) == -1)
		return;
	memcpy(p, fsa.fsa_ax25.sax25_call.ax25_call, 7);
	p[6] |= 0xE0;
	p += 7;

	if (ax25_aton(in->call_from, &fsa) == -1)
		return;
	memcpy(p, fsa.fsa_ax25.sax25_call.ax25_call, 7);
	p[6] |= 0x60;
	p += 7;

	switch (in->datakind) {
	case AGWPE_CMD_CONNECT_VIA:		/* digis follow, packed */
	case AGWPE_CMD_UNPROTO_VIA:
		if (info != NULL && ilen > 0) {
			ndigis = info[0];
			if (ndigis > AGWPE_MAX_DIGIS - 1)
				ndigis = AGWPE_MAX_DIGIS - 1;
			if (ilen < 1 + ndigis * AGWPE_MAX_CALL)
				ndigis = (ilen - 1) / AGWPE_MAX_CALL;
			for (i = 0; i < ndigis; i++) {
				char call[16];

				if (agwpe_call_unpack(call, sizeof(call),
						      (const char *)info + 1 +
						      i * AGWPE_MAX_CALL) < 0)
					continue;
				if (ax25_aton(call, &fsa) == -1)
					continue;
				memcpy(p, fsa.fsa_ax25.sax25_call.ax25_call, 7);
				p[6] |= 0x60;
				p += 7;
			}
			info += 1 + ndigis * AGWPE_MAX_CALL;
			ilen -= info - data;
		}
		break;
	}

	/* Mark the end of the address field on the last address byte.  */
	*(p - 1) |= 0x01;

	switch (in->datakind) {
	case AGWPE_CMD_CONNECT:			/* SABME, command */
	case AGWPE_CMD_CONNECT_PID:
	case AGWPE_CMD_CONNECT_VIA:
		ctl = 0x6F;
		break;
	case AGWPE_CMD_DISCONNECT:		/* DISC, command */
		ctl = 0x43;
		break;
	case AGWPE_CMD_DATA:			/* I frame */
		ctl = 0x00;
		break;
	default:				/* unproto: UI */
		ctl = 0x03;
		break;
	}
	*p++ = ctl;
	*p++ = in->pid;

	if (ilen > (size_t)(MUX_RAW_MAX - (p - buf)))
		ilen = MUX_RAW_MAX - (p - buf);
	if (info != NULL && ilen > 0)
		memcpy(p, info, ilen);
	p += ilen;

	{
		struct netd_upstream *u = up_by_port(in->port);

		if (u != NULL && u != &netd.loop)
			netd_mheard_frame(u, buf, p - buf);
	}

	mux_mirror_send(in, buf, p - buf);
}

static void reply_register(struct netd_client *cl, const struct agwpe_s *in,
			   int ok)
{
	struct agwpe_s hdr;
	unsigned char data = ok ? 1 : 0;

	agwpe_header_init(&hdr, in->port, AGWPE_DK_REGISTERED, 0,
			  in->call_from, NULL, 1);
	loop_send_client(cl, &hdr, &data, 1);
}

static int mux_call_register(struct netd_upstream *u, const char *call, int fd,
			     int listener, unsigned char chan)
{
	int i;

	for (i = 0; i < u->ncalls; i++) {
		if (strcmp(u->calls[i].call, call) == 0) {
			if (u->calls[i].fd == fd) {
				u->calls[i].listener = listener; /* idempotent */
				return 0;
			}
			return -1;		/* owned by someone else */
		}
	}

	if (u->ncalls == u->acalls) {
		struct netd_call *nc;
		int na = u->acalls ? u->acalls * 2 : 4;

		nc = realloc(u->calls, sizeof(*nc) * na);
		if (nc == NULL)
			return -1;
		u->calls = nc;
		u->acalls = na;
	}

	strncpy(u->calls[u->ncalls].call, call, AGWPE_MAX_CALL - 1);
	u->calls[u->ncalls].call[AGWPE_MAX_CALL - 1] = '\0';
	u->calls[u->ncalls].chan = chan;
	u->calls[u->ncalls].fd = fd;
	u->calls[u->ncalls].listener = listener;
	u->ncalls++;

	return 0;
}

static void mux_call_unregister(struct netd_upstream *u, const char *call, int fd)
{
	int i;

	if (u == NULL)
		return;

	for (i = 0; i < u->ncalls; i++) {
		if (strcmp(u->calls[i].call, call) == 0 &&
		    u->calls[i].fd == fd) {
			u->calls[i] = u->calls[u->ncalls - 1];
			u->ncalls--;
			return;
		}
	}
}

static struct netd_client *client_by_call(struct netd_upstream *u,
					  const char *call)
{
	int i;

	if (call == NULL || call[0] == '\0')
		return NULL;

	for (i = 0; i < u->ncalls; i++) {
		if (strcmp(u->calls[i].call, call) == 0)
			return client_by_fd(u->calls[i].fd);
	}
	return NULL;
}

/* A PID of 0 in a connect/data header means the default AX.25 text PID.  */
static unsigned char session_pid(unsigned char pid)
{
	return pid == 0 ? AGWPE_PID_AX25 : pid;
}

static struct netd_session *session_find(struct netd_upstream *u,
					 const char *local, const char *remote,
					 unsigned char pid, int fd)
{
	int i;

	/*
	 * An AX.25 connection is uniquely identified by its call pair
	 * (source and destination, including SSIDs).  Neither a different
	 * PID nor a different digipeater path makes it a different
	 * connection, so the PID is not part of the identity.
	 */
	(void)pid;

	for (i = 0; i < u->nsessions; i++) {
		if (strcmp(u->sessions[i].call_from, local) == 0 &&
		    strcmp(u->sessions[i].call_to, remote) == 0 &&
		    (fd < 0 || u->sessions[i].fd == fd))
			return &u->sessions[i];
	}
	return NULL;
}

static int session_pair_active(struct netd_upstream *u,
			       const char *local, const char *remote)
{
	int i;

	for (i = 0; i < u->nsessions; i++) {
		if (strcmp(u->sessions[i].call_from, local) == 0 &&
		    strcmp(u->sessions[i].call_to, remote) == 0)
			return 1;
	}
	return 0;
}

static struct netd_session *session_add(struct netd_upstream *u,
					const char *local, const char *remote,
					unsigned char pid, int fd,
					unsigned char chan)
{
	struct netd_session *s;

	if (u->nsessions == u->asessions) {
		struct netd_session *ns;
		int na = u->asessions ? u->asessions * 2 : 8;

		ns = realloc(u->sessions, sizeof(*ns) * na);
		if (ns == NULL)
			return NULL;
		u->sessions = ns;
		u->asessions = na;
	}

	s = &u->sessions[u->nsessions++];
	memset(s, 0, sizeof(*s));
	strncpy(s->call_from, local, AGWPE_MAX_CALL - 1);
	s->call_from[AGWPE_MAX_CALL - 1] = '\0';
	strncpy(s->call_to, remote, AGWPE_MAX_CALL - 1);
	s->call_to[AGWPE_MAX_CALL - 1] = '\0';
	s->pid = pid;
	s->chan = chan;
	s->fd = fd;
	return s;
}

static void session_remove(struct netd_upstream *u, struct netd_session *s)
{
	u->sessions[s - u->sessions] = u->sessions[u->nsessions - 1];
	u->nsessions--;
}

/*
 * Drop every session owned by fd on upstream u, optionally restricted to
 * one local call.  Any link that loses its last session is torn down
 * upstream.  Used when a client goes away or unregisters a call.
 */
static void session_drop_for_call(struct netd_upstream *u, int fd,
				  const char *call)
{
	int j;

	for (j = u->nsessions - 1; j >= 0; j--) {
		struct netd_session *s = &u->sessions[j];

		if (s->fd != fd)
			continue;
		if (call != NULL && strcmp(s->call_from, call) != 0)
			continue;
		{
			char lc[AGWPE_MAX_CALL], rc[AGWPE_MAX_CALL];
			unsigned char chan = s->chan;

			memcpy(lc, s->call_from, sizeof(lc));
			memcpy(rc, s->call_to, sizeof(rc));
			session_remove(u, s);
			if (!session_pair_active(u, lc, rc) && u->connected)
				agwpe_client_disconnect(u->cli, chan, lc, rc);
		}
	}
}

/*
 * Reply frames synthesised locally when a duplicate connect is refused.
 * The call fields are swapped relative to the request, exactly as
 * Direwolf sends them.
 */
static void reply_disconnect(struct netd_client *cl, const struct agwpe_s *in)
{
	struct agwpe_s hdr;
	char msg[32];

	snprintf(msg, sizeof(msg), "*** DISCONNECTED\r");
	agwpe_header_init(&hdr, in->port, AGWPE_DK_DISCONNECT, 0,
			  in->call_to, in->call_from, strlen(msg) + 1);
	loop_send_client(cl, &hdr, (unsigned char *)msg, strlen(msg) + 1);
}

/* A duplicate connect cannot work; refuse the way AGWPE refuses a
 * connect that fails: a 'd' frame with a retryout message.  */
static void reply_refuse(struct netd_client *cl, const struct agwpe_s *in)
{
	struct agwpe_s hdr;
	char msg[128];

	snprintf(msg, sizeof(msg), "*** DISCONNECTED RETRYOUT With %s\r",
		 in->call_to);
	agwpe_header_init(&hdr, in->port, AGWPE_DK_DISCONNECT, 0,
			  in->call_to, in->call_from, strlen(msg) + 1);
	loop_send_client(cl, &hdr, (unsigned char *)msg, strlen(msg) + 1);
}

/* --- Virtual loop bridge ------------------------------------------------
 *
 * The loop upstream (AGWPE_PORT_LOOP) has no radio behind it: a connect,
 * data or disconnect on it is routed to the loop client that owns the
 * destination call.  Frames are forwarded unchanged, so call_to is the
 * destination (from the receiver's point of view its own call) and
 * call_from the other end.  Sessions are tracked per direction so that a
 * disappearing client can tear the link down on the other side too.
 */

static int call_has_ssid(const char *call)
{
	return strchr(call, '-') != NULL;
}

static int call_base_equal(const char *a, const char *b)
{
	const char *da = strchr(a, '-');
	const char *db = strchr(b, '-');
	size_t la = da != NULL ? (size_t)(da - a) : strlen(a);
	size_t lb = db != NULL ? (size_t)(db - b) : strlen(b);

	return la == lb && strncasecmp(a, b, la) == 0;
}

/* Owner of a call on the loop: exact match first, then a listener
 * registered without an SSID (SSID 0) matches any SSID of its base.  */
static struct netd_client *loop_call_by_call(const char *call)
{
	struct netd_upstream *u = &netd.loop;
	int i;

	if (call == NULL || call[0] == '\0')
		return NULL;

	for (i = 0; i < u->ncalls; i++)
		if (strcmp(u->calls[i].call, call) == 0)
			return client_by_fd(u->calls[i].fd);

	for (i = 0; i < u->ncalls; i++) {
		if (u->calls[i].listener && !call_has_ssid(u->calls[i].call) &&
		    call_base_equal(u->calls[i].call, call))
			return client_by_fd(u->calls[i].fd);
	}
	return NULL;
}

/* Remove every session of the (a,b) link, in both directions.  */
static void loop_link_remove(struct netd_upstream *u, const char *a,
			     const char *b)
{
	int again, i;

	do {
		again = 0;
		for (i = u->nsessions - 1; i >= 0; i--) {
			if ((strcmp(u->sessions[i].call_from, a) == 0 &&
			     strcmp(u->sessions[i].call_to, b) == 0) ||
			    (strcmp(u->sessions[i].call_from, b) == 0 &&
			     strcmp(u->sessions[i].call_to, a) == 0)) {
				session_remove(u, &u->sessions[i]);
				again = 1;
				break;
			}
		}
	} while (again);
}

/* A client on the loop connects to a local listener.  The connect is
 * delivered to the listener's owner; a refused connect is answered
 * locally with a retryout disconnect, exactly as Direwolf does.  */
static void loop_connect(struct netd_client *cl, const struct agwpe_s *hdr,
			 const unsigned char *data, size_t len)
{
	struct netd_upstream *u = &netd.loop;
	struct netd_client *owner;
	unsigned char pid = session_pid(hdr->pid);

	owner = loop_call_by_call(hdr->call_to);
	if (owner == NULL) {
		reply_refuse(cl, hdr);
		return;
	}

	if (session_find(u, hdr->call_from, hdr->call_to, pid, -1) != NULL) {
		reply_refuse(cl, hdr);
		return;
	}

	session_add(u, hdr->call_from, hdr->call_to, pid, cl->fd, 0);
	if (!session_pair_active(u, hdr->call_to, hdr->call_from))
		session_add(u, hdr->call_to, hdr->call_from, pid, owner->fd, 0);

	loop_send_client(owner, hdr, data, len);

	/* Confirm the connect to the caller.  The call fields are swapped,
	 * as for any connect confirm: call_from is the other end.  */
	{
		struct agwpe_s ch;
		char tmp[AGWPE_MAX_CALL];

		ch = *hdr;
		memcpy(tmp, ch.call_from, sizeof(tmp));
		memcpy(ch.call_from, ch.call_to, sizeof(ch.call_from));
		memcpy(ch.call_to, tmp, sizeof(ch.call_to));
		ch.data_len = 0;
		loop_send_client(cl, &ch, NULL, 0);
	}
}

/* Data on an established loop connection.  First data on a link that has
 * no session yet establishes it, exactly as for a radio upstream.  */
static void loop_data(struct netd_client *cl, const struct agwpe_s *hdr,
		      const unsigned char *data, size_t len)
{
	struct netd_upstream *u = &netd.loop;
	struct netd_client *owner;
	unsigned char pid = session_pid(hdr->pid);

	owner = loop_call_by_call(hdr->call_to);
	if (owner == NULL)
		return;

	if (session_find(u, hdr->call_from, hdr->call_to, pid, cl->fd) == NULL) {
		if (!session_pair_active(u, hdr->call_from, hdr->call_to)) {
			struct agwpe_s ch;

			agwpe_header_init(&ch, hdr->port, AGWPE_CMD_CONNECT,
					  pid, hdr->call_from, hdr->call_to, 0);
			loop_connect(cl, &ch, NULL, 0);
		}
		if (session_find(u, hdr->call_from, hdr->call_to, pid,
				 cl->fd) == NULL)
			session_add(u, hdr->call_from, hdr->call_to, pid,
				    cl->fd, 0);
	}

	loop_send_client(owner, hdr, data, len);
}

/* Disconnect on the loop: tear the link down in both directions and
 * deliver the disconnect to the other side, if it is still there.  */
static void loop_disconnect(struct netd_client *cl, const struct agwpe_s *hdr,
			    const unsigned char *data, size_t len)
{
	struct netd_upstream *u = &netd.loop;
	struct netd_client *owner;

	owner = loop_call_by_call(hdr->call_to);
	loop_link_remove(u, hdr->call_from, hdr->call_to);
	if (owner != NULL && owner != cl)
		loop_send_client(owner, hdr, data, len);
}

/* Unproto, unproto via and raw frames on the loop are routed to the
 * owner of the destination call, like a frame heard on a local radio.  */
static void loop_unproto(struct netd_client *cl, const struct agwpe_s *hdr,
			 const unsigned char *data, size_t len)
{
	struct netd_client *owner;

	owner = loop_call_by_call(hdr->call_to);
	if (owner != NULL && owner != cl)
		loop_send_client(owner, hdr, data, len);
}

/* A loop client went away: unregister its calls and tear down every link
 * it was part of, delivering a disconnect to the other side.  */
static void loop_client_gone(struct netd_client *cl)
{
	struct netd_upstream *u = &netd.loop;
	int i, j;

	j = 0;
	while (j < u->ncalls) {
		if (u->calls[j].fd == cl->fd) {
			u->calls[j] = u->calls[u->ncalls - 1];
			u->ncalls--;
		} else {
			j++;
		}
	}

	for (i = u->nsessions - 1; i >= 0; i--) {
		struct netd_session *s = &u->sessions[i];
		struct netd_client *peer;
		char from[AGWPE_MAX_CALL], to[AGWPE_MAX_CALL];
		struct agwpe_s hdr;
		char msg[32];

		if (s->fd != cl->fd)
			continue;

		memcpy(from, s->call_from, sizeof(from));
		memcpy(to, s->call_to, sizeof(to));
		loop_link_remove(u, from, to);

		peer = loop_call_by_call(to);
		if (peer != NULL && peer != cl) {
			snprintf(msg, sizeof(msg), "*** DISCONNECTED\r");
			agwpe_header_init(&hdr, AGWPE_PORT_LOOP,
					  AGWPE_DK_DISCONNECT, 0,
					  from, to, strlen(msg) + 1);
			loop_send_client(peer, &hdr,
					 (unsigned char *)msg, strlen(msg) + 1);
		}
	}
}

/* --- 'Q' connection control -----------------------------------------------
 *
 * axctl/axkill send a 'Q' frame naming a connection (port, call_from,
 * call_to) plus a command.  Kill terminates the session and tears the
 * link down; Param adjusts a connection parameter and is passed on to
 * the radio upstream, so that the radio AGWPE can apply it.  The loop
 * upstream has no radio behind it, so parameters are only recorded.
 */

/* Deliver a disconnect notification to a session's owner.  */
static void ctl_disconnect_owner(struct netd_client *cl,
				 struct netd_upstream *u,
				 struct netd_session *s)
{
	struct netd_client *owner = client_by_fd(s->fd);
	struct agwpe_s hdr;
	char msg[32];

	if (owner == NULL || owner == cl)
		return;

	snprintf(msg, sizeof(msg), "*** DISCONNECTED\r");
	agwpe_header_init(&hdr, u->index, AGWPE_DK_DISCONNECT, 0,
			  s->call_to, s->call_from, strlen(msg) + 1);
	loop_send_client(owner, &hdr, (unsigned char *)msg, strlen(msg) + 1);
}

static void mux_ctl_kill(struct netd_client *cl, const struct agwpe_s *hdr)
{
	struct netd_upstream *u = up_by_port(hdr->port);
	char from[AGWPE_MAX_CALL], to[AGWPE_MAX_CALL];

	if (u == NULL)
		return;

	memcpy(from, hdr->call_from, sizeof(from));
	memcpy(to, hdr->call_to, sizeof(to));

	if (u == &netd.loop) {
		/* Tear the link down in both directions and tell both
		 * ends, if they are still connected.  */
		struct netd_client *af, *at;
		struct agwpe_s h;
		char msg[32];

		loop_link_remove(u, from, to);

		snprintf(msg, sizeof(msg), "*** DISCONNECTED\r");
		af = loop_call_by_call(from);
		at = loop_call_by_call(to);

		if (af != NULL && af != cl) {
			agwpe_header_init(&h, AGWPE_PORT_LOOP,
					  AGWPE_DK_DISCONNECT, 0,
					  to, from, strlen(msg) + 1);
			loop_send_client(af, &h, (unsigned char *)msg,
					 strlen(msg) + 1);
		}
		if (at != NULL && at != cl) {
			agwpe_header_init(&h, AGWPE_PORT_LOOP,
					  AGWPE_DK_DISCONNECT, 0,
					  from, to, strlen(msg) + 1);
			loop_send_client(at, &h, (unsigned char *)msg,
					 strlen(msg) + 1);
		}
		return;
	}

	{
		struct netd_session *s;

		s = session_find(u, from, to, session_pid(hdr->pid), -1);
		if (s == NULL)
			s = session_find(u, to, from, 0, -1);
		if (s == NULL)
			return;

		{
			char lc[AGWPE_MAX_CALL], rc[AGWPE_MAX_CALL];
			unsigned char chan = s->chan;

			memcpy(lc, s->call_from, sizeof(lc));
			memcpy(rc, s->call_to, sizeof(rc));
			ctl_disconnect_owner(cl, u, s);
			session_remove(u, s);
			if (!session_pair_active(u, lc, rc) && u->connected)
				agwpe_client_disconnect(u->cli, chan,
							lc, rc);
		}
	}
}

static void mux_ctl_param(struct netd_client *cl, const struct agwpe_s *hdr,
			  const unsigned char *data, size_t len)
{
	struct netd_upstream *u = up_by_port(hdr->port);
	struct netd_session *s;
	unsigned char scope, param;
	uint32_t value;

	if (u == NULL || data == NULL || len < 7)
		return;

	scope = data[1];
	param = data[2];
	memcpy(&value, data + 3, sizeof(value));
	value = agwpe_netle2host(value);

	if (scope != AGWPE_CTL_SCOPE_CONN) {
		/* Port defaults: pass through to the radio AGWPE.  There
		 * is no radio behind the loop port.  */
		if (u != &netd.loop && u->connected)
			client_send_upstream(u, hdr, data);
		return;
	}

	s = session_find(u, hdr->call_from, hdr->call_to,
			 session_pid(hdr->pid), -1);
	if (s == NULL)
		s = session_find(u, hdr->call_to, hdr->call_from, 0, -1);
	if (s == NULL)
		return;

	switch (param) {
	case AGWPE_CTL_PARAM_WINDOW:	s->window = value;	break;
	case AGWPE_CTL_PARAM_T1:	s->t1 = value;		break;
	case AGWPE_CTL_PARAM_T2:	s->t2 = value;		break;
	case AGWPE_CTL_PARAM_T3:	s->t3 = value;		break;
	case AGWPE_CTL_PARAM_N2:	s->n2 = value;		break;
	case AGWPE_CTL_PARAM_IDLE:	s->idle = value;	break;
	case AGWPE_CTL_PARAM_PACLEN:	s->paclen = value;	break;
	default:
		return;
	}

	/* Per-connection parameters reach the radio's AGWPE server.  */
	if (u != &netd.loop && u->connected)
		client_send_upstream(u, hdr, data);
}

/*
 * Every connected upstream has reported its channel layout ('G' reply).
 * A dead or not yet connected upstream is not required: it contributes
 * nothing until it is there, so a waiting client never hangs on it.
 */
static int mux_ports_ready(void)
{
	int i;

	for (i = 0; i < netd.nup; i++)
		if (netd.ups[i].connected && !netd.ups[i].ports_ready)
			return 0;
	return 1;
}

/*
 * The merged port information table.  Each radio channel of every
 * upstream is a flat port (i*16+channel) listed as "PortN", N being one
 * more than the port byte, exactly as the AGWPE interface manual and
 * Direwolf do ("Port1" is port 0).  The name comes from agwpe.conf, the
 * description from the upstream's own 'G' reply.  The virtual loop port
 * (255) is appended so remote clients can reach local services too.
 */
static void mux_ports_reply(struct netd_client *cl)
{
	struct agwpe_s h;
	char buf[4096];
	int n = 0, i, c, count = 0;

	for (i = 0; i < netd.nup; i++)
		if (netd.ups[i].ports_ready)
			count += netd.ups[i].nports;
	if (netd.loop_enabled)
		count++;

	n += snprintf(buf + n, sizeof(buf) - n, "%d;", count);

	for (i = 0; i < netd.nup; i++) {
		struct netd_upstream *u = &netd.ups[i];

		if (!u->ports_ready)
			continue;
		for (c = 0; c < u->nports; c++)
			n += snprintf(buf + n, sizeof(buf) - n, "Port%d %s: %s;",
				      port_flat(u, u->ports[c].chan) + 1,
				      u->name, u->ports[c].desc);
	}
	if (netd.loop_enabled)
		n += snprintf(buf + n, sizeof(buf) - n, "Port%d %s: %s;",
			      AGWPE_PORT_LOOP + 1, netd.loop.name,
			      "local loopback services");
	if (n < (int)sizeof(buf))
		n++;			/* trailing NUL, as Direwolf */

	agwpe_header_init(&h, 0, AGWPE_DK_PORTS, 0, NULL, NULL, n);
	loop_send_client(cl, &h, (unsigned char *)buf, n);
}

/*
 * Answer the deferred 'G' requests as soon as the upstream tables are
 * complete, or after NETD_PORTS_TIMEOUT seconds whatever is known.  The
 * timeout stops an upstream that never answers from hanging a client.
 */
void mux_ports_tick(time_t now)
{
	int i;

	for (i = 0; i < netd.nclients; i++) {
		struct netd_client *cl = &netd.clients[i];

		if (cl->fd < 0 || !cl->want_ports)
			continue;
		if (mux_ports_ready() ||
		    now - cl->ports_since >= NETD_PORTS_TIMEOUT) {
			cl->want_ports = 0;
			mux_ports_reply(cl);
		}
	}
}

void mux_client_command(struct netd_client *cl, const struct agwpe_s *hdr,
			const unsigned char *data, size_t len)
{
	struct netd_upstream *u;
	unsigned char port = hdr->port;

	/*
	 * Mirror local outbound traffic to raw monitor clients before the
	 * request is routed, so the attempt is visible even when the radio
	 * upstream is unreachable or the request is refused here.
	 */
	switch (hdr->datakind) {
	case AGWPE_CMD_CONNECT:
	case AGWPE_CMD_CONNECT_PID:
	case AGWPE_CMD_CONNECT_VIA:
	case AGWPE_CMD_DATA:
	case AGWPE_CMD_DISCONNECT:
	case AGWPE_CMD_UNPROTO:
	case AGWPE_CMD_UNPROTO_VIA:
	case AGWPE_CMD_RAW:
		mux_mirror_raw(hdr, data, len);
		break;
	}

	switch (hdr->datakind) {

	case 'P':				/* login */
		break;

	case 'X':				/* register callsign */
		u = up_by_port(port);
		if (u == NULL || hdr->call_from[0] == '\0') {
			reply_register(cl, hdr, 0);
			break;
		}
		if (mux_call_register(u, hdr->call_from, cl->fd, 0,
				      port_chan(u, port)) < 0) {
			reply_register(cl, hdr, 0);
			break;
		}
		if (u->connected)
			agwpe_client_register(u->cli, port_chan(u, port),
					      hdr->call_from);
		reply_register(cl, hdr, 1);
		break;

	case 'L':				/* listen (loop port only) */
		u = up_by_port(port);
		if (u != &netd.loop || hdr->call_from[0] == '\0') {
			reply_register(cl, hdr, 0);
			break;
		}
		if (mux_call_register(u, hdr->call_from, cl->fd, 1, 0) < 0) {
			reply_register(cl, hdr, 0);
			break;
		}
		reply_register(cl, hdr, 1);
		break;

	case 'x':				/* unregister callsign */
		u = up_by_port(port);
		if (u != NULL && u->connected)
			agwpe_client_unregister(u->cli, port_chan(u, port),
						hdr->call_from);
		mux_call_unregister(u, hdr->call_from, cl->fd);
		if (u != NULL)
			session_drop_for_call(u, cl->fd, hdr->call_from);
		break;

	case 'R':				/* version, answered locally */
		{
			struct agwpe_s h;
			unsigned char v[8];
			uint32_t major = agwpe_host2netle(NETD_VERSION_MAJOR);
			uint32_t minor = agwpe_host2netle(NETD_VERSION_MINOR);

			memcpy(v, &major, 4);
			memcpy(v + 4, &minor, 4);

			agwpe_header_init(&h, 0, AGWPE_DK_VERSION, 0, NULL, NULL, 8);
			loop_send_client(cl, &h, v, 8);
		}
		break;

	case 'G':				/* port information, answered locally */
		{
			/*
			 * Wait until every connected upstream reported its
			 * channel layout, so the table never shows a
			 * half-known upstream; mux_ports_tick answers it.
			 */
			if (mux_ports_ready())
				mux_ports_reply(cl);
			else {
				cl->want_ports = 1;
				cl->ports_since = time(NULL);
			}
		}
		break;

	case 'g':				/* capabilities, answered locally */
		{
			struct agwpe_s h;
			unsigned char c[12];
			uint32_t bytes = agwpe_host2netle(1);

			memset(c, 0, sizeof(c));
			c[1] = 1;		/* traffic level */
			c[2] = 0x19;		/* tx delay */
			c[3] = 4;		/* tx tail */
			c[4] = 0xc8;		/* persist */
			c[5] = 4;		/* slot time */
			c[6] = 7;		/* max frame */
			memcpy(c + 8, &bytes, 4);

			agwpe_header_init(&h, port, AGWPE_DK_CAPAB, 0, NULL, NULL, 12);
			loop_send_client(cl, &h, c, 12);
		}
		break;

	case 'H':				/* heard list, forwarded upstream */
		u = up_by_port(port);
		if (u == NULL)
			break;
		u->heard_to = cl->fd;
		if (u->connected)
			agwpe_client_get_heard(u->cli, port_chan(u, port));
		break;

	case 'y':				/* outstanding frames on a port */
	case 'Y':				/* outstanding frames for a connection */
		u = up_by_port(port);
		if (u == NULL)
			break;
		u->out_to = cl->fd;
		if (u->connected) {
			if (hdr->datakind == 'y')
				agwpe_client_outstanding_port(u->cli,
							      port_chan(u, port));
			else
				agwpe_client_outstanding_conn(u->cli,
							      port_chan(u, port),
							      hdr->call_from,
							      hdr->call_to);
		}
		break;

	case 'm':				/* monitor toggle */
		cl->monitor = !cl->monitor;
		mux_recalc_toggles();
		break;

	case 'k':				/* raw monitor toggle */
		cl->raw = !cl->raw;
		mux_recalc_toggles();
		break;

	case 'C':				/* connect */
	case 'c':				/* connect with PID */
	case 'v':				/* connect via digipeaters */
		{
			unsigned char pid = session_pid(hdr->pid);

			u = up_by_port(port);
			if (u == NULL)
				break;
			if (u == &netd.loop) {
				loop_connect(cl, hdr, data, len);
				break;
			}
			if (!u->connected)
				break;

			/*
			 * An AX.25 connection is identified by its call
			 * pair (source and destination, including SSIDs).
			 * Neither a digipeater path nor a different PID
			 * makes it a different connection, so a second
			 * connect for an established pair is refused.
			 */
			if (session_find(u, hdr->call_from, hdr->call_to,
					 pid, -1) != NULL) {
				reply_refuse(cl, hdr);
				break;
			}

			session_add(u, hdr->call_from, hdr->call_to,
				    pid, cl->fd, port_chan(u, port));
			client_send_upstream(u, hdr, data);
		}
		break;

	case 'D':				/* connected data */
		u = up_by_port(port);
		if (u == NULL)
			break;
		if (u == &netd.loop) {
			loop_data(cl, hdr, data, len);
			break;
		}
		if (!u->connected)
			break;

		if (session_find(u, hdr->call_from, hdr->call_to,
				 session_pid(hdr->pid), cl->fd) == NULL) {
			if (!session_pair_active(u, hdr->call_from,
						 hdr->call_to)) {
				/* First session on this link: establish it
				 * upstream before sending the data.
				 */
				struct agwpe_s ch;

				agwpe_header_init(&ch, port_chan(u, port),
						  AGWPE_CMD_CONNECT,
						  session_pid(hdr->pid),
						  hdr->call_from,
						  hdr->call_to, 0);
				agwpe_client_send_frame(u->cli, &ch, NULL);
			}
			session_add(u, hdr->call_from, hdr->call_to,
				    session_pid(hdr->pid), cl->fd,
				    port_chan(u, port));
		}
		client_send_upstream(u, hdr, data);
		break;

	case 'd':				/* disconnect */
		{
			struct netd_session *s;
			int i;

			u = up_by_port(port);
			if (u == NULL)
				break;
			if (u == &netd.loop) {
				loop_disconnect(cl, hdr, data, len);
				break;
			}
			if (!u->connected)
				break;

			s = session_find(u, hdr->call_from, hdr->call_to,
					 session_pid(hdr->pid), cl->fd);
			if (s == NULL) {
				/* No session with this PID; fall back to any
				 * session this client has on the link.
				 */
				for (i = 0; i < u->nsessions; i++) {
					if (u->sessions[i].fd == cl->fd &&
					    strcmp(u->sessions[i].call_from,
						   hdr->call_from) == 0 &&
					    strcmp(u->sessions[i].call_to,
						   hdr->call_to) == 0) {
						s = &u->sessions[i];
						break;
					}
				}
			}
			if (s == NULL) {
				/* Unknown link; pass the request through.  */
				client_send_upstream(u, hdr, data);
				break;
			}

			session_remove(u, s);
			if (session_pair_active(u, hdr->call_from,
						 hdr->call_to)) {
				/* Other sessions still use the link, so do
				 * not disconnect it upstream.
				 */
				reply_disconnect(cl, hdr);
			} else {
				client_send_upstream(u, hdr, data);
			}
		}
		break;

	case 'M':				/* unproto */
	case 'V':				/* unproto via */
	case 'K':				/* raw frame */
		u = up_by_port(port);
		if (u == NULL)
			break;
		if (u == &netd.loop) {
			loop_unproto(cl, hdr, data, len);
			break;
		}
		if (!u->connected)
			break;
		client_send_upstream(u, hdr, data);
		break;

	case 'Q':				/* connection control */
		if (data != NULL && len > 0 && data[0] == AGWPE_CTL_KILL)
			mux_ctl_kill(cl, hdr);
		else if (data != NULL && len > 0 &&
			 data[0] == AGWPE_CTL_PARAM)
			mux_ctl_param(cl, hdr, data, len);
		break;

	default:
		netd_log(LOG_WARNING, "client %d: unknown frame '%c'",
			 cl->fd, hdr->datakind);
		break;
	}
}

void mux_client_disconnect(struct netd_client *cl)
{
	int i, j;

	cl->want_ports = 0;

	for (i = 0; i < netd.nup; i++) {
		struct netd_upstream *u = &netd.ups[i];

		if (u->heard_to == cl->fd)
			u->heard_to = -1;
		if (u->out_to == cl->fd)
			u->out_to = -1;

		j = 0;
		while (j < u->ncalls) {
			if (u->calls[j].fd == cl->fd) {
				if (u->connected)
					agwpe_client_unregister(u->cli,
								u->calls[j].chan,
								u->calls[j].call);
				u->calls[j] = u->calls[u->ncalls - 1];
				u->ncalls--;
			} else {
				j++;
			}
		}

		/* Drop the client's sessions; the helper tears down any
		 * upstream link that no longer has a session on it.  */
		session_drop_for_call(u, cl->fd, NULL);
	}

	if (netd.loop_enabled) {
		if (netd.loop.heard_to == cl->fd)
			netd.loop.heard_to = -1;
		if (netd.loop.out_to == cl->fd)
			netd.loop.out_to = -1;
		loop_client_gone(cl);
	}

	mux_recalc_toggles();
}

void mux_upstream_frame(struct netd_upstream *u, const struct agwpe_s *hdr,
			const unsigned char *data, size_t len)
{
	struct netd_client *owner;
	int i;

	switch (hdr->datakind) {

	case 'C':				/* connection received */
		owner = client_by_call(u, hdr->call_to);
		if (owner == NULL)
			break;
		/* The link is up; remember it so that a later 'D' from
		 * this client is not mistaken for a new connection.  */
		if (!session_pair_active(u, hdr->call_to, hdr->call_from))
			session_add(u, hdr->call_to, hdr->call_from,
				    AGWPE_PID_AX25, owner->fd, hdr->port);
		loop_send_upstream(owner, u, hdr, data, len);
		break;

	case 'D':				/* connected data */
		owner = client_by_call(u, hdr->call_to);
		if (owner == NULL)
			break;
		if (session_find(u, hdr->call_to, hdr->call_from,
				 session_pid(hdr->pid), owner->fd) == NULL)
			session_add(u, hdr->call_to, hdr->call_from,
				    session_pid(hdr->pid), owner->fd, hdr->port);
		loop_send_upstream(owner, u, hdr, data, len);
		break;

	case 'd':				/* disconnect */
		owner = client_by_call(u, hdr->call_to);
		if (owner == NULL)
			break;
		/* The link is gone: terminate every session on it, so the
		 * owner sees a disconnect for each PID (text and NET/ROM).  */
		{
			int n = 0;

			for (i = u->nsessions - 1; i >= 0; i--) {
				if (strcmp(u->sessions[i].call_from,
					   hdr->call_to) == 0 &&
				    strcmp(u->sessions[i].call_to,
					   hdr->call_from) == 0) {
					loop_send_upstream(owner, u, hdr,
							   data, len);
					session_remove(u, &u->sessions[i]);
					n++;
				}
			}
			if (n == 0)
				loop_send_upstream(owner, u, hdr, data, len);
		}
		break;

	case 'I':				/* monitored information */
	case 'S':
	case 'U':
	case 'T':
		for (i = 0; i < netd.nclients; i++) {
			struct netd_client *cl = &netd.clients[i];

			if (cl->fd >= 0 && cl->monitor)
				loop_send_upstream(cl, u, hdr, data, len);
		}
		break;

	case 'K':				/* raw monitored frame */
		netd_mheard_frame(u, data, len);
		for (i = 0; i < netd.nclients; i++) {
			struct netd_client *cl = &netd.clients[i];

			if (cl->fd >= 0 && cl->raw)
				loop_send_upstream(cl, u, hdr, data, len);
		}
		break;

	case 'H':				/* heard list replies */
		if (u->heard_to >= 0) {
			owner = client_by_fd(u->heard_to);
			if (owner != NULL)
				loop_send_upstream(owner, u, hdr, data, len);
		}
		break;

	case 'y':				/* outstanding frames replies */
	case 'Y':
		if (u->out_to >= 0) {
			owner = client_by_fd(u->out_to);
			if (owner != NULL)
				loop_send_upstream(owner, u, hdr, data, len);
		}
		break;

	case 'X':				/* upstream registration reply */
		break;				/* we answer registrations locally */

	default:
		break;
	}
}

void mux_upstream_reconnected(struct netd_upstream *u)
{
	int i;

	u->monitor_on = 0;
	u->raw_on = 0;
	u->nsessions = 0;
	u->ports_ready = 0;
	u->nports = 0;

	for (i = 0; i < u->ncalls; i++)
		agwpe_client_register(u->cli, u->calls[i].chan,
				      u->calls[i].call);

	/* Re-learn the channel layout; a Direwolf restart may have added
	 * or removed channels.  */
	agwpe_client_get_ports(u->cli);

	mux_recalc_toggles();
}

/*
 * The upstream connection is gone; every AX.25 link it carried is gone
 * too.  Terminate each session inward, one 'd' per session, so a client
 * with several PIDs on one link sees every one of them disconnected.
 */
void mux_upstream_lost(struct netd_upstream *u)
{
	int i;

	for (i = 0; i < u->nsessions; i++) {
		struct netd_session *s = &u->sessions[i];
		struct netd_client *cl = client_by_fd(s->fd);
		struct agwpe_s hdr;
		char msg[32];

		if (cl == NULL)
			continue;

		snprintf(msg, sizeof(msg), "*** DISCONNECTED\r");
		agwpe_header_init(&hdr, port_flat(u, s->chan),
				  AGWPE_DK_DISCONNECT, 0,
				  s->call_to, s->call_from, strlen(msg) + 1);
		loop_send_client(cl, &hdr, (unsigned char *)msg, strlen(msg) + 1);
	}

	u->nsessions = 0;
}

void mux_recalc_toggles(void)
{
	int i, j, want_m, want_k;

	for (i = 0; i < netd.nup; i++) {
		struct netd_upstream *u = &netd.ups[i];

		/* Raw monitoring stays on while the heard list is enabled: the
		 * multiplexer needs every frame the radio receives for mheard,
		 * so a raw frame arrives even while no loop client listens.
		 * Without the heard list the upstream raw toggle follows the
		 * loop clients that asked for it.
		 */
		want_m = 0;
		want_k = netd.mheard;
		for (j = 0; j < netd.nclients; j++) {
			if (netd.clients[j].fd >= 0 && netd.clients[j].monitor)
				want_m = 1;
			if (netd.clients[j].fd >= 0 && netd.clients[j].raw)
				want_k = 1;
		}

		if (!u->connected)
			continue;

		if (want_m && !u->monitor_on) {
			agwpe_client_monitor(u->cli);
			u->monitor_on = 1;
		} else if (!want_m && u->monitor_on) {
			agwpe_client_monitor(u->cli);
			u->monitor_on = 0;
		}

		if (want_k && !u->raw_on) {
			agwpe_client_raw_toggle(u->cli);
			u->raw_on = 1;
		} else if (!want_k && u->raw_on) {
			agwpe_client_raw_toggle(u->cli);
			u->raw_on = 0;
		}
	}
}

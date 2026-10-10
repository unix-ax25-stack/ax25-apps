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
 * Each radio upstream occupies a block of AX25NETD_PORT_STRIDE loop port
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

#include "ax25netd.h"

/* Mirror of local outbound frames, sent to raw monitor clients.  */
#define	MUX_RAW_MAX	2048

/* Whether mux_uisub_any() has anything to say.  A count rather than a scan,
 * because the question is asked about every heard frame and the answer only
 * changes when a client comes or goes.  Declared here with the other
 * file-wide state because mux_ctl_uisub() is well above the frame path.  */
static int mux_uisub_clients;

static struct ax25netd_upstream *up_by_port(unsigned char port)
{
	if (port == AGWPE_PORT_LOOP && ax25netd.loop_enabled)
		return &ax25netd.loop;
	if (port / AX25NETD_PORT_STRIDE >= ax25netd.nup)
		return NULL;
	return &ax25netd.ups[port / AX25NETD_PORT_STRIDE];
}

/* The port numbers this daemon serves, in one line, for a message about a
 * port that is not among them.  Written out rather than counted, because
 * "no port %u" with nothing after it sends the reader off to look for the
 * list, and the list is short enough to print.  A static buffer because the
 * message is built for ax25netd_verbose() and the alternative is a caller
 * that has to free something. */
static const char *mux_ports_summary(void)
{
	static char buf[256];
	size_t n = 0;
	int i;

	n += (size_t)snprintf(buf, sizeof(buf), "the %d radio upstream%s",
			      ax25netd.nup, ax25netd.nup == 1 ? "" : "s");
	for (i = 0; i < ax25netd.nup && n + 8 < sizeof(buf); i++)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s %d..%d",
				      i == 0 ? " (" : ", ",
				      ax25netd.ups[i].name,
				      i * AX25NETD_PORT_STRIDE,
				      i * AX25NETD_PORT_STRIDE +
				      AX25NETD_PORT_STRIDE - 1);
	if (ax25netd.loop_enabled && n + 8 < sizeof(buf))
		snprintf(buf + n, sizeof(buf) - n, "%s%s %d",
			 n == 0 ? "" : (ax25netd.nup == 0 ? "" : "), "),
			 ax25netd.loop.name, AGWPE_PORT_LOOP);
	return buf;
}

/*
 * The port a client's frame names, and what to say when there is none.
 *
 * A frame for a port that does not exist is dropped, because AGWPE has no
 * "no such port" to send back on a 'C' or a 'D'.  That is right on the wire
 * and useless in practice: a client that picked a wrong port number waits,
 * and waits exactly as long as it would for a station that does not answer,
 * so the two are indistinguishable from the far end.  The refusal is
 * therefore logged - with the client, so a run can be lined up against the
 * program's own log - and the reason is the whole point of the message: an
 * unknown port number and an upstream that is not connected are different
 * faults with different fixes, and the operator's first question is which.
 */
static struct ax25netd_upstream *up_by_port_or_log(unsigned char port,
						    struct ax25netd_client *cl,
						    const char *what)
{
	struct ax25netd_upstream *u = up_by_port(port);

	if (u != NULL)
		return u;
	if (ax25netd.verbose && cl != NULL)
		ax25netd_verbose("client %d: %s on port %u dropped, there is "
				 "no such port; the ports are %s",
				 cl->fd, what, port, mux_ports_summary());
	return NULL;
}

/*
 * The upstream for a frame that has to go out, or NULL with the reason
 * logged.  "Not connected" is the common reason and the one worth naming:
 * the upstream is down, or has just reconnected and is still logging in, or
 * its port list has not come back - three states that look identical from the
 * client side and are fixed in three different places.
 *
 * The virtual loop port is passed straight through: it has no upstream behind
 * it and so nothing to be connected to, and saying "not connected" about it
 * would name a fault that does not exist.  The caller still has to recognise
 * it afterwards, because only the caller knows what to do with a loop frame.
 */
static struct ax25netd_upstream *up_by_port_radio(unsigned char port,
						  struct ax25netd_client *cl,
						  const char *what)
{
	struct ax25netd_upstream *u = up_by_port_or_log(port, cl, what);

	if (u == NULL)
		return NULL;
	if (u->virtual || u->connected)
		return u;

	if (ax25netd.verbose && cl != NULL)
		ax25netd_verbose("client %d: %s on port %u dropped: upstream "
				 "'%s' cannot be used, %s", cl->fd, what, port,
				 u->name,
				 u->ports_ready ? "it is not connected" :
				 "it is connected but has not answered the "
				 "port request yet, so no channel of it is known");
	return NULL;
}

/* The radio channel of upstream u that a flat loop port selects.  */
static unsigned char port_chan(const struct ax25netd_upstream *u,
			       unsigned char port)
{
	return (unsigned char)(port - u->index * AX25NETD_PORT_STRIDE);
}

/* The flat loop port number of channel c of upstream u.  */
static unsigned char port_flat(const struct ax25netd_upstream *u,
			       unsigned char chan)
{
	return (unsigned char)(u->index * AX25NETD_PORT_STRIDE + chan);
}

/* *out = *hdr with the port rewritten to the upstream's channel byte.  */
static void port_hdr(struct agwpe_s *out, const struct ax25netd_upstream *u,
		     const struct agwpe_s *hdr)
{
	*out = *hdr;
	out->port = port_chan(u, hdr->port);
}

/* *out = *hdr with a channel byte rewritten to the flat loop port.  */
static void up_hdr(struct agwpe_s *out, const struct ax25netd_upstream *u,
		   const struct agwpe_s *hdr)
{
	*out = *hdr;
	out->port = port_flat(u, hdr->port);
}

/* Send a client frame towards the upstream with the port rewritten.  */
static int client_send_upstream(struct ax25netd_upstream *u,
				const struct agwpe_s *hdr,
				const unsigned char *data)
{
	struct agwpe_s ph;

	port_hdr(&ph, u, hdr);
	return agwpe_client_send_frame(u->cli, &ph, data);
}

/* Deliver an upstream frame to a loop client with the port rewritten.  */
static int loop_send_upstream(struct ax25netd_client *cl,
			      struct ax25netd_upstream *u,
			      const struct agwpe_s *hdr,
			      const unsigned char *data, size_t len)
{
	struct agwpe_s uh;

	up_hdr(&uh, u, hdr);
	return loop_send_client(cl, &uh, data, len);
}

/*
 * How much of a raw frame this client wants.
 *
 * A raw monitor frame is KISS encapsulated: the data marker, two or more
 * addresses of seven bytes with 0x01 set on the last one, the control byte,
 * the PID, and then the information field.  Taking the payloads off means
 * stopping after the PID.
 *
 * A frame this cannot take apart goes out whole.  That is not politeness: a
 * frame whose address field never ends, or that is too short to hold a
 * control byte, is not a frame whose payload we may drop - it is a frame we
 * do not understand, and cutting one at a guessed offset produces something
 * that decodes as a different frame rather than as this one with less in it.
 * The mask says what may be left out of a frame, never where a frame ends.
 */
static size_t raw_monitor_len(unsigned char mask, const unsigned char *data,
			      size_t len)
{
	size_t off = 1;		/* past the KISS data marker */

	if (data == NULL)
		return 0;
	if ((mask & AGWPE_MONMASK_ALL) == AGWPE_MONMASK_ALL)
		return len;

	for (;;) {
		if (off + 7 > len)
			return len;			/* no end of address */
		off += 7;
		if (data[off - 1] & 0x01)
			break;
	}
	if (off + 2 > len)
		return len;				/* no control and PID */

	{
		unsigned char ctl = data[off];
		int is_i = (ctl & 0x01) == 0;		/* I frame */
		int is_ui = (ctl & 0x7f) == 0x03;	/* UI frame */

		/*
		 * The two bits are one frame apart, not two.  Anything
		 * else - SABM, DISC, UA, RR - has no information field to
		 * leave out, so it goes whole to everybody, and its length
		 * is off + 2 anyway.
		 */
		if ((is_i && !(mask & AGWPE_MONMASK_I)) ||
		    (is_ui && !(mask & AGWPE_MONMASK_UI)))
			return off + 2;
	}
	return len;
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

	for (i = 0; i < ax25netd.nclients; i++) {
		struct ax25netd_client *cl = &ax25netd.clients[i];
		size_t n;

		if (cl->fd < 0 || !cl->raw)
			continue;

		/* Per client, and not once for the frame: two monitors on
		 * one machine can want different things, and the mask is
		 * how they say so.  Everything here is per client, and a
		 * version that worked out one length and sent it to
		 * everybody is the bug this shape is written against. */
		n = raw_monitor_len(cl->monmask, data, len);
		agwpe_header_init(&hdr, in->port, AGWPE_DK_RAW, 0,
				  in->call_from, in->call_to, n);
		loop_send_client(cl, &hdr, data, n);
	}
}

/*
 * Mirror a frame a client is about to send outwards to every raw monitor
 * client (listen).  This is the transmit leg: without a packet socket on
 * macOS, ax25netd stands in for the kernel and shows local outbound traffic
 * even while the radio upstream is unreachable.  Frames received from
 * the radio continue to come from the upstream as 'K', so there is no
 * doubling.  The raw frame is rebuilt from the AGWPE header fields: dest
 * and source addresses, optional digipeaters, a control byte derived
 * from the frame kind, the PID and the information field.
 */
static void mux_mirror_raw_ctl(const struct agwpe_s *in,
			       const unsigned char *data, size_t len,
			       unsigned char ctl)
{
	struct full_sockaddr_ax25 fsa;
	const unsigned char *info = data;
	unsigned char buf[MUX_RAW_MAX + 1];
	size_t ilen = len, ndigis = 0, i;
	unsigned char *p;

	if (data == NULL)
		ilen = 0;

	if (in->datakind == AGWPE_CMD_RAW) {
		/* The client supplied the raw frame itself.  */
		{
			struct ax25netd_upstream *u = up_by_port(in->port);

			if (u != NULL && u != &ax25netd.loop)
				ax25netd_mheard_frame(u, data, len);
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

	*p++ = ctl;
	*p++ = in->pid;

	if (ilen > (size_t)(MUX_RAW_MAX - (p - buf)))
		ilen = MUX_RAW_MAX - (p - buf);
	if (info != NULL && ilen > 0)
		memcpy(p, info, ilen);
	p += ilen;

	{
		struct ax25netd_upstream *u = up_by_port(in->port);

		if (u != NULL && u != &ax25netd.loop)
			ax25netd_mheard_frame(u, buf, p - buf);
	}

	mux_mirror_send(in, buf, p - buf);
}

/*
 * The AX.25 control byte that spells a client frame's kind on the
 * monitor.  A connect is shown as a SABM, the level 2 mode this node
 * actually operates in.  The AGWPE connect command carries no mode
 * field, so nothing on this side of the link ever asks for the extended
 * (mod 128) mode and it cannot be coded here.  An upstream that speaks
 * eAX.25 may still put a SABME on the air while it tries v2.2 first and
 * falls back, but that is the upstream's link setup, not a mode this
 * connect requested, and that frame arrives as its own raw monitor
 * frame.  Showing SABME here would claim a capability the connect never
 * asked for and answer the wrong question.
 */
static unsigned char mux_frame_ctl(unsigned char datakind)
{
	switch (datakind) {
	case AGWPE_CMD_CONNECT:
	case AGWPE_CMD_CONNECT_PID:
	case AGWPE_CMD_CONNECT_VIA:
		return 0x2F;		/* SABM, command */
	case AGWPE_CMD_DISCONNECT:
		return 0x43;		/* DISC, command */
	case AGWPE_CMD_DATA:
		return 0x00;		/* I frame */
	default:
		return 0x03;		/* unproto: UI */
	}
}

static void mux_mirror_raw(const struct agwpe_s *in,
			   const unsigned char *data, size_t len)
{
	mux_mirror_raw_ctl(in, data, len, mux_frame_ctl(in->datakind));
}

static void reply_register(struct ax25netd_client *cl, const struct agwpe_s *in,
			   int ok)
{
	struct agwpe_s hdr;
	unsigned char data = ok ? 1 : 0;

	agwpe_header_init(&hdr, in->port, AGWPE_DK_REGISTERED, 0,
			  in->call_from, NULL, 1);
	loop_send_client(cl, &hdr, &data, 1);
}

static int mux_call_register(struct ax25netd_upstream *u, const char *call, int fd,
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
		struct ax25netd_call *nc;
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

/*
 * Why mux_call_register() said no, in words.
 *
 * It answers -1 for two unrelated things: the callsign is already
 * registered by another client, which the caller can do something about,
 * and the table could not be grown, which it cannot.  The client only ever
 * sees a one-byte refusal for both, so without this a station that had its
 * callsign taken looks the same as a daemon that is out of memory.
 *
 * The lookup re-scans the table rather than the caller passing a reason
 * out, because registration happens once per callsign per client and the
 * alternative threads a second answer through a function whose -1 is
 * already load-bearing in four places.
 */
static const char *mux_call_owner(struct ax25netd_upstream *u, const char *call)
{
	static char buf[64];
	int i;

	for (i = 0; i < u->ncalls; i++) {
		if (strcmp(u->calls[i].call, call) != 0)
			continue;
		snprintf(buf, sizeof(buf),
			 "client %d already has that callsign",
			 u->calls[i].fd);
		return buf;
	}
	/* Not in the table at all, so it was the realloc() that failed. */
	return "the callsign table could not be grown";
}

static void mux_call_unregister(struct ax25netd_upstream *u, const char *call, int fd)
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

static struct ax25netd_client *client_by_call(struct ax25netd_upstream *u,
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

static struct ax25netd_session *session_find(struct ax25netd_upstream *u,
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

static int session_pair_active(struct ax25netd_upstream *u,
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

/*
 * The handle a session is listed and killed by.  One counter for the whole
 * daemon, so an id is unique across the radio upstreams and the loop, and
 * monotonic, so a value is never reused while the daemon runs and an id read
 * from a table a moment ago cannot name a different connection.  Wrapping at
 * 2^32 is left to happen: it would take four billion sessions, and the same
 * chance of a stale id colliding exists in any scheme that hands one out.
 */
static uint32_t mux_session_id_next = 1;

static struct ax25netd_session *session_add(struct ax25netd_upstream *u,
					const char *local, const char *remote,
					unsigned char pid, int fd,
					unsigned char chan)
{
	struct ax25netd_session *s;

	if (u->nsessions == u->asessions) {
		struct ax25netd_session *ns;
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
	s->id = mux_session_id_next++;
	ax25netd_verbose("session %u: %s -> %s on port %u (%s)",
			 s->id, s->call_from, s->call_to,
			 (unsigned)(u->virtual ? AGWPE_PORT_LOOP :
				     port_flat(u, s->chan)),
			 u->name);
	return s;
}

static void session_remove(struct ax25netd_upstream *u, struct ax25netd_session *s)
{
	u->sessions[s - u->sessions] = u->sessions[u->nsessions - 1];
	u->nsessions--;
}

/*
 * Drop every session owned by fd on upstream u, optionally restricted to
 * one local call.  Any link that loses its last session is torn down
 * upstream.  Used when a client goes away or unregisters a call.
 */
static void session_drop_for_call(struct ax25netd_upstream *u, int fd,
				  const char *call)
{
	int j;

	for (j = u->nsessions - 1; j >= 0; j--) {
		struct ax25netd_session *s = &u->sessions[j];

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
static void reply_disconnect(struct ax25netd_client *cl, const struct agwpe_s *in)
{
	struct agwpe_s hdr;
	char msg[32];

	snprintf(msg, sizeof(msg), "*** DISCONNECTED\r");
	agwpe_header_init(&hdr, in->port, AGWPE_DK_DISCONNECT, 0,
			  in->call_to, in->call_from, strlen(msg) + 1);
	loop_send_client(cl, &hdr, (unsigned char *)msg, strlen(msg) + 1);
}

/*
 * Refuse a connect the way AGWPE does: a 'd' frame carrying a message.  The
 * word in it is the only place the reason can go, and there are two reasons
 * that want telling apart - nobody is listening for that callsign, and the
 * pair is already connected.  RETRYOUT is what a station that never answered
 * looks like and is what AGWPE says; BUSY is ours, and a client that does not
 * know it still sees a refused connect, which is the truth either way.
 */
static void reply_refuse(struct ax25netd_client *cl, const struct agwpe_s *in,
			 const char *why)
{
	struct agwpe_s hdr;
	char msg[128];

	snprintf(msg, sizeof(msg), "*** DISCONNECTED %s With %s\r", why,
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
static struct ax25netd_client *loop_call_by_call(const char *call)
{
	struct ax25netd_upstream *u = &ax25netd.loop;
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
static void loop_link_remove(struct ax25netd_upstream *u, const char *a,
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
static void loop_connect(struct ax25netd_client *cl, const struct agwpe_s *hdr,
			 const unsigned char *data, size_t len)
{
	struct ax25netd_upstream *u = &ax25netd.loop;
	struct ax25netd_client *owner;
	unsigned char pid = session_pid(hdr->pid);

	owner = loop_call_by_call(hdr->call_to);
	if (owner == NULL) {
		reply_refuse(cl, hdr, "RETRYOUT");
		return;
	}

	if (session_find(u, hdr->call_from, hdr->call_to, pid, -1) != NULL) {
		reply_refuse(cl, hdr, "BUSY");
		return;
	}

	session_add(u, hdr->call_from, hdr->call_to, pid, cl->fd, 0);
	if (!session_pair_active(u, hdr->call_to, hdr->call_from))
		session_add(u, hdr->call_to, hdr->call_from, pid, owner->fd, 0);

	/*
	 * Both notifications go out as 'C', which is how a server reports a
	 * connection.  The frame arrived as 'C', 'v' or 'c' - the three ways
	 * an application spells a connect - and handing that spelling on was
	 * passing a request off as an answer: a client written to the AGWPE
	 * specification knows only 'C' here and would miss the call.  The pid
	 * stays in the header where it belongs, and the digipeater list is
	 * dropped because a 'C' carries none and AGWPE conveys none either.
	 */
	{
		struct agwpe_s ch = *hdr;

		ch.datakind = AGWPE_DK_CONNECT;
		ch.data_len = 0;
		loop_send_client(owner, &ch, NULL, 0);
	}

	(void) data;
	(void) len;

	/* Confirm the connect to the caller.  The call fields are swapped,
	 * as for any connect confirm: call_from is the other end.  */
	{
		struct agwpe_s ch;
		char tmp[AGWPE_MAX_CALL];

		ch = *hdr;
		ch.datakind = AGWPE_DK_CONNECT;
		memcpy(tmp, ch.call_from, sizeof(tmp));
		memcpy(ch.call_from, ch.call_to, sizeof(ch.call_from));
		memcpy(ch.call_to, tmp, sizeof(ch.call_to));
		ch.data_len = 0;
		loop_send_client(cl, &ch, NULL, 0);
	}
}

/* Data on an established loop connection.  The session has to exist
 * already: AGWPE carries neither a stream nor connection state, so the
 * session recorded at connect time is the only proof the link is up.  An
 * I-frame arriving without one is dropped - creating a session here, as
 * this used to, let stray data raise a disconnected link and a fresh
 * login on the far side.  The peer is named by the reverse session, which
 * keeps the delivery tied to the connection rather than to whichever
 * client happens to hold the destination call.  */
static void loop_data(struct ax25netd_client *cl, const struct agwpe_s *hdr,
		      const unsigned char *data, size_t len)
{
	struct ax25netd_upstream *u = &ax25netd.loop;
	struct ax25netd_session *s;
	struct ax25netd_client *owner;

	if (session_find(u, hdr->call_from, hdr->call_to, 0, cl->fd) == NULL)
		return;

	s = session_find(u, hdr->call_to, hdr->call_from, 0, -1);
	owner = s != NULL ? client_by_fd(s->fd)
			  : loop_call_by_call(hdr->call_to);
	if (owner == NULL || owner == cl)
		return;

	loop_send_client(owner, hdr, data, len);
}

/* Disconnect on the loop: tear the link down in both directions and
 * deliver the disconnect to the other side, if it is still there.  The
 * peer is the other end of the session being torn down; asking for it by
 * callsign could name a client that merely holds the call by now.  */
static void loop_disconnect(struct ax25netd_client *cl, const struct agwpe_s *hdr,
			    const unsigned char *data, size_t len)
{
	struct ax25netd_upstream *u = &ax25netd.loop;
	struct ax25netd_session *s;
	struct ax25netd_client *owner;

	s = session_find(u, hdr->call_to, hdr->call_from, 0, -1);
	owner = s != NULL ? client_by_fd(s->fd)
			  : loop_call_by_call(hdr->call_to);
	loop_link_remove(u, hdr->call_from, hdr->call_to);
	if (owner != NULL && owner != cl)
		loop_send_client(owner, hdr, data, len);
}

/* Unproto, unproto via and raw frames on the loop are routed to the
 * owner of the destination call, like a frame heard on a local radio.  */
static void loop_unproto(struct ax25netd_client *cl, const struct agwpe_s *hdr,
			 const unsigned char *data, size_t len)
{
	struct ax25netd_client *owner;

	owner = loop_call_by_call(hdr->call_to);
	if (owner != NULL && owner != cl)
		loop_send_client(owner, hdr, data, len);
}

/* A loop client went away: unregister its calls and tear down every link
 * it was part of, delivering a disconnect to the other side.  */
static void loop_client_gone(struct ax25netd_client *cl)
{
	struct ax25netd_upstream *u = &ax25netd.loop;
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
		struct ax25netd_session *s = &u->sessions[i];
		struct ax25netd_client *peer;
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
static void ctl_disconnect_owner(struct ax25netd_client *cl,
				 struct ax25netd_upstream *u,
				 struct ax25netd_session *s)
{
	struct ax25netd_client *owner = client_by_fd(s->fd);
	struct agwpe_s hdr;
	char msg[32];

	if (owner == NULL || owner == cl)
		return;

	snprintf(msg, sizeof(msg), "*** DISCONNECTED\r");
	agwpe_header_init(&hdr, u->index, AGWPE_DK_DISCONNECT, 0,
			  s->call_to, s->call_from, strlen(msg) + 1);
	loop_send_client(owner, &hdr, (unsigned char *)msg, strlen(msg) + 1);
}

/* Declared before mux_ctl_kill(), which reaches for it; it is defined below,
 * next to mux_ctl_killid(), the other caller. */
static void mux_kill_session(struct ax25netd_client *cl,
			     struct ax25netd_upstream *u,
			     struct ax25netd_session *s);

static void mux_ctl_kill(struct ax25netd_client *cl, const struct agwpe_s *hdr)
{
	struct ax25netd_upstream *u = up_by_port_or_log(hdr->port, cl, "kill");
	char from[AGWPE_MAX_CALL], to[AGWPE_MAX_CALL];

	if (u == NULL)
		return;

	memcpy(from, hdr->call_from, sizeof(from));
	memcpy(to, hdr->call_to, sizeof(to));

	if (u == &ax25netd.loop) {
		struct ax25netd_session *s;

		/* The pair is asked of the session table first, and only of
		 * the registered calls when no session is tracked for it.
		 *
		 * A session knows which client holds each of its ends, which
		 * a registered call does not: a program is free to open a
		 * connection without registering the call it connected with,
		 * and the call lookup alone would then tell only the end that
		 * did register.  That end is left holding a link whose
		 * sessions are already gone, with nothing left that would
		 * ever tell it - which is not what ending a connection means.
		 *
		 * A pair no session is tracked for is a link this daemon
		 * never saw come up, and there the registrations are all
		 * there is. */
		s = session_find(u, from, to, session_pid(hdr->pid), -1);
		if (s == NULL)
			s = session_find(u, to, from, 0, -1);
		if (s != NULL) {
			mux_kill_session(cl, u, s);
			return;
		}

		/* Tear the link down in both directions and tell both
		 * ends, if they are still connected.  */
		struct ax25netd_client *af, *at;
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
		struct ax25netd_session *s;

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

/*
 * End one session: the work a kill by id does, and the same work a kill by
 * name does on a radio.  A radio session has an owner to tell and a link to
 * drop upstream once its last session is gone.  A loop session has two ends
 * and no upstream, and the caller of a kill by id is neither end, so both are
 * told - unlike a kill by name, where the caller is one end and only the
 * other has to hear.
 */
static void mux_kill_session(struct ax25netd_client *cl,
			     struct ax25netd_upstream *u,
			     struct ax25netd_session *s)
{
	char lc[AGWPE_MAX_CALL], rc[AGWPE_MAX_CALL];
	unsigned char chan = s->chan;
	unsigned char pid = s->pid;

	memcpy(lc, s->call_from, sizeof(lc));
	memcpy(rc, s->call_to, sizeof(rc));

	if (u == &ax25netd.loop) {
		struct ax25netd_session *rev = session_find(u, rc, lc, 0, -1);
		struct ax25netd_client *a = client_by_fd(s->fd);
		struct ax25netd_client *b = rev != NULL ?
			client_by_fd(rev->fd) : loop_call_by_call(rc);
		struct agwpe_s h;
		char msg[32];

		snprintf(msg, sizeof(msg), "*** DISCONNECTED\r");
		loop_link_remove(u, lc, rc);

		if (a != NULL && a != cl) {
			agwpe_header_init(&h, AGWPE_PORT_LOOP,
					  AGWPE_DK_DISCONNECT, 0, rc, lc,
					  strlen(msg) + 1);
			loop_send_client(a, &h, (unsigned char *)msg,
					 strlen(msg) + 1);
		}
		if (b != NULL && b != a && b != cl) {
			agwpe_header_init(&h, AGWPE_PORT_LOOP,
					  AGWPE_DK_DISCONNECT, 0, lc, rc,
					  strlen(msg) + 1);
			loop_send_client(b, &h, (unsigned char *)msg,
					 strlen(msg) + 1);
		}
		return;
	}

	/*
	 * Show the teardown on the raw monitors, the way every other local
	 * frame is shown: the upstream is told, but a disconnect is not a
	 * frame it echoes, and a kill that leaves nothing in listen(1) has
	 * no visible effect at all.  The frame is a DM and not a DISC - the
	 * connection is dropped from this side and there is no longer a
	 * command to answer, so the DISC would wait for a UA from a station
	 * that is already gone.
	 */
	{
		struct agwpe_s h;

		agwpe_header_init(&h, port_flat(u, chan),
				  AGWPE_CMD_DISCONNECT, pid, lc, rc, 0);
		mux_mirror_raw_ctl(&h, NULL, 0, 0x0F);	/* DM */
	}

	ctl_disconnect_owner(cl, u, s);
	session_remove(u, s);
	if (!session_pair_active(u, lc, rc) && u->connected)
		agwpe_client_disconnect(u->cli, chan, lc, rc);
}

/*
 * End the session with this id, wherever it is.  The id comes from a table
 * this server handed out, so the port and call pair need not be known.  An id
 * no session carries is dropped in silence: there is nothing useful to send
 * back, and a caller cannot be told apart from one that named the wrong id.
 */
static void mux_ctl_killid(struct ax25netd_client *cl,
			   const unsigned char *data, size_t len)
{
	struct ax25netd_upstream *u;
	uint32_t id;
	int i, j;

	if (data == NULL || len < 5)
		return;
	memcpy(&id, data + 1, sizeof(id));
	id = agwpe_netle2host(id);

	for (i = 0; i < ax25netd.nup; i++) {
		u = &ax25netd.ups[i];
		for (j = 0; j < u->nsessions; j++) {
			if (u->sessions[j].id != id)
				continue;
			mux_kill_session(cl, u, &u->sessions[j]);
			return;
		}
	}
	if (ax25netd.loop_enabled) {
		u = &ax25netd.loop;
		for (j = 0; j < u->nsessions; j++) {
			if (u->sessions[j].id != id)
				continue;
			mux_kill_session(cl, u, &u->sessions[j]);
			return;
		}
	}
	ax25netd_verbose("client %d: kill of session %u: no such session",
			 cl->fd, id);
}

/*
 * The session table, as text: the subcommand byte, then ';'-separated rows
 * with the row count first, the same shape as an upstream's 'G' port list (and
 * read by the same kind of tokenizer) except for the leading byte, which says
 * which 'Q' answer this is.  A row is
 *
 *	id port upstream chan from to pid
 *
 * with decimal numbers and the call pair as the server spells it.  ax25netctl(8)
 * is what reads it.
 *
 * Sessions are tracked per direction on the loop, so a loop connection has
 * two rows, one per end.  Only one is listed: the two name the same
 * connection, and a table that showed it twice would invite a kill of an
 * already-broken link.  A radio upstream keeps each of its connections once,
 * even when two of them run between the same pair of stations, so none of its
 * rows are folded together.
 */
static int session_listed_before(struct ax25netd_upstream *u, int idx)
{
	int i;

	if (u != &ax25netd.loop)
		return 0;

	for (i = 0; i < idx; i++) {
		if (strcmp(u->sessions[i].call_from,
			   u->sessions[idx].call_from) == 0 &&
		    strcmp(u->sessions[i].call_to,
			   u->sessions[idx].call_to) == 0)
			return 1;
		if (strcmp(u->sessions[i].call_from,
			   u->sessions[idx].call_to) == 0 &&
		    strcmp(u->sessions[i].call_to,
			   u->sessions[idx].call_from) == 0)
			return 1;
	}
	return 0;
}

/* One row, without the ';' that follows it.  buf may be NULL with cap 0 to
 * ask for the length only, as snprintf() allows. */
static int session_row(char *buf, size_t cap,
		       const struct ax25netd_upstream *u,
		       const struct ax25netd_session *s, int loop)
{
	return snprintf(buf, cap, "%u %u %s %u %s %s %u", s->id,
			loop ? AGWPE_PORT_LOOP : port_flat(u, s->chan),
			u->name, s->chan, s->call_from, s->call_to, s->pid);
}

static void mux_ctl_sessions(struct ax25netd_client *cl)
{
	struct ax25netd_upstream *u;
	struct agwpe_s hdr;
	char *buf;
	size_t cap, n = 0;
	int i, j, count = 0;

	for (i = 0; i < ax25netd.nup; i++)
		for (j = 0; j < ax25netd.ups[i].nsessions; j++)
			if (!session_listed_before(&ax25netd.ups[i], j))
				count++;
	if (ax25netd.loop_enabled) {
		u = &ax25netd.loop;
		for (j = 0; j < u->nsessions; j++)
			if (!session_listed_before(u, j))
				count++;
	}

	/* The reply goes out as one frame, so its length is worked out
	 * before the first byte is written: the subcommand byte, the row
	 * count, then the rows. */
	cap = 1 + (size_t)snprintf(NULL, 0, "%d;", count);
	for (i = 0; i < ax25netd.nup; i++) {
		u = &ax25netd.ups[i];
		for (j = 0; j < u->nsessions; j++)
			if (!session_listed_before(u, j))
				cap += (size_t)session_row(NULL, 0, u,
						&u->sessions[j], 0) + 1;
	}
	if (ax25netd.loop_enabled) {
		u = &ax25netd.loop;
		for (j = 0; j < u->nsessions; j++)
			if (!session_listed_before(u, j))
				cap += (size_t)session_row(NULL, 0, u,
						&u->sessions[j], 1) + 1;
	}

	buf = malloc(cap + 1);		/* + trailing NUL, as the port list */
	if (buf == NULL) {
		/* An empty table beats no answer at all. */
		buf = malloc(4);
		if (buf == NULL)
			return;
		n = (size_t)snprintf(buf, 4, "%c0;", AGWPE_CTL_SESSIONS);
		agwpe_header_init(&hdr, 0, AGWPE_CMD_CTL, 0, NULL, NULL, n);
		loop_send_client(cl, &hdr, (unsigned char *)buf, n);
		free(buf);
		return;
	}

	buf[n++] = AGWPE_CTL_SESSIONS;
	n += (size_t)snprintf(buf + n, cap + 1 - n, "%d;", count);
	for (i = 0; i < ax25netd.nup; i++) {
		u = &ax25netd.ups[i];
		for (j = 0; j < u->nsessions; j++) {
			if (session_listed_before(u, j))
				continue;
			n += (size_t)session_row(buf + n, cap + 1 - n, u,
						 &u->sessions[j], 0);
			buf[n++] = ';';
		}
	}
	if (ax25netd.loop_enabled) {
		u = &ax25netd.loop;
		for (j = 0; j < u->nsessions; j++) {
			if (session_listed_before(u, j))
				continue;
			n += (size_t)session_row(buf + n, cap + 1 - n, u,
						 &u->sessions[j], 1);
			buf[n++] = ';';
		}
	}
	buf[n++] = '\0';

	agwpe_header_init(&hdr, 0, AGWPE_CMD_CTL, 0, NULL, NULL, n);
	loop_send_client(cl, &hdr, (unsigned char *)buf, n);
	free(buf);
}

static void mux_ctl_monmask(struct ax25netd_client *cl, unsigned char mask)
{
	/*
	 * What this client wants its raw frames to carry.  It takes effect
	 * from the next frame on and is not retroactive, which is the only
	 * way it can work: a frame is already in the client's socket buffer
	 * by the time it is asked about, and there is no way back into it.
	 *
	 * Per client, and that is the whole point.  listen(1) and mheardd(8)
	 * on one machine are two clients on one server and want opposite
	 * things - every byte of the frame, and none of the payload - and
	 * whichever way this is decided, it is decided per client and not
	 * for the server.
	 *
	 * Only about the frames this server sends.  A kernel packet socket
	 * is the kernel's, and the library says under AXSOCK_DEBUG that the
	 * mask does not reach it: a frame that is cut here becomes a frame
	 * that decodes as something else.
	 */
	cl->monmask = (unsigned char)(mask & AGWPE_MONMASK_ALL);
	ax25netd_log(LOG_INFO, "client %d: raw monitor payload mask 0x%02x",
		     cl->fd, cl->monmask);
}

/*
 * "Give me the UI frames addressed to the call signs I have registered", as
 * 'M' frames.  See mux_upstream_ui() for what it is for.
 *
 * Per connection and per client, and like the mask above not forwarded to an
 * upstream: it says what this client wants from this server, and an upstream
 * has no such command.  A frame the client could have picked out of the raw
 * stream instead arrives this way even when the raw stream is off, which is
 * the whole point - and a client that wanted the stream as well still gets
 * it, because the raw stream is a separate toggle and says nothing about this.
 */
static void mux_ctl_uisub(struct ax25netd_client *cl)
{
	if (cl->uisub)
		return;

	cl->uisub = 1;
	mux_uisub_clients++;
	ax25netd_log(LOG_INFO, "client %d: UI addressed to its own call "
		     "signs will be delivered", cl->fd);
}

static void mux_ctl_param(struct ax25netd_client *cl, const struct agwpe_s *hdr,
			  const unsigned char *data, size_t len)
{
	struct ax25netd_upstream *u = up_by_port_or_log(hdr->port, cl, "control");
	struct ax25netd_session *s;
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
		if (u != &ax25netd.loop && u->connected)
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
	if (u != &ax25netd.loop && u->connected)
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

	for (i = 0; i < ax25netd.nup; i++)
		if (ax25netd.ups[i].connected && !ax25netd.ups[i].ports_ready)
			return 0;
	return 1;
}

/* Which connected upstreams have not reported their channel layout yet.
 * The reason a client waits for its port list, so it belongs next to the
 * wait rather than in a file the reader has to know to look in. */
static const char *mux_ports_pending(void)
{
	static char buf[256];
	size_t n = 0;
	int i;

	for (i = 0; i < ax25netd.nup && n + 8 < sizeof(buf); i++) {
		struct ax25netd_upstream *u = &ax25netd.ups[i];

		if (!u->connected || u->ports_ready)
			continue;
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s'%s'",
				      n == 0 ? "" : ", ", u->name);
	}
	if (n == 0)
		snprintf(buf, sizeof(buf), "nothing - the table is complete");
	return buf;
}

/*
 * The merged port information table.  Each radio channel of every
 * upstream is a flat port (i*16+channel) listed as "PortN", N being one
 * more than the port byte, exactly as the AGWPE interface manual and
 * Direwolf do ("Port1" is port 0).  The name comes from the upstream
 * configuration, the
 * description from the upstream's own 'G' reply.  The virtual loop port
 * (255) is appended so remote clients can reach local services too.
 */
static void mux_ports_reply(struct ax25netd_client *cl)
{
	struct agwpe_s h;
	char buf[4096];
	int n = 0, i, c, count = 0;

	for (i = 0; i < ax25netd.nup; i++)
		if (ax25netd.ups[i].ports_ready)
			count += ax25netd.ups[i].nports;
	if (ax25netd.loop_enabled)
		count++;

	n += snprintf(buf + n, sizeof(buf) - n, "%d;", count);

	for (i = 0; i < ax25netd.nup; i++) {
		struct ax25netd_upstream *u = &ax25netd.ups[i];

		if (!u->ports_ready)
			continue;
		for (c = 0; c < u->nports; c++)
			n += snprintf(buf + n, sizeof(buf) - n, "Port%d %s: %s;",
				      port_flat(u, u->ports[c].chan) + 1,
				      u->name, u->ports[c].desc);
	}
	if (ax25netd.loop_enabled)
		n += snprintf(buf + n, sizeof(buf) - n, "Port%d %s: %s;",
			      AGWPE_PORT_LOOP + 1, ax25netd.loop.name,
			      "local loopback services");
	if (n < (int)sizeof(buf))
		n++;			/* trailing NUL, as Direwolf */

	agwpe_header_init(&h, 0, AGWPE_DK_PORTS, 0, NULL, NULL, n);
	loop_send_client(cl, &h, (unsigned char *)buf, n);
}

/*
 * Answer the deferred 'G' requests as soon as the upstream tables are
 * complete, or after AX25NETD_PORTS_TIMEOUT seconds whatever is known.  The
 * timeout stops an upstream that never answers from hanging a client.
 */
void mux_ports_tick(time_t now)
{
	int i;

	for (i = 0; i < ax25netd.nclients; i++) {
		struct ax25netd_client *cl = &ax25netd.clients[i];

		if (cl->fd < 0 || !cl->want_ports)
			continue;
		if (mux_ports_ready() ||
		    now - cl->ports_since >= AX25NETD_PORTS_TIMEOUT) {
			cl->want_ports = 0;
			/* A table that arrived incomplete is a normal
			 * outcome - an upstream that is down has no
			 * channels to report - but it is the reason a
			 * port the client then tried is missing, so it
			 * is said out loud rather than left to be worked
			 * out from the missing entry. */
			if (!mux_ports_ready())
				ax25netd_verbose("client %d: port list "
						 "answered after %d s without %s",
						 cl->fd,
						 AX25NETD_PORTS_TIMEOUT,
						 mux_ports_pending());
			mux_ports_reply(cl);
		}
	}
}

void mux_client_command(struct ax25netd_client *cl, const struct agwpe_s *hdr,
			const unsigned char *data, size_t len)
{
	struct ax25netd_upstream *u;
	unsigned char port = hdr->port;

	if (ax25netd.debug)
		ax25netd_log(LOG_DEBUG,
			     "client %d: kind='%c' port=%u from=%.10s to=%.10s len=%zu",
			     cl->fd, hdr->datakind, hdr->port, hdr->call_from,
			     hdr->call_to, len);

	/*
	 * Mirror local outbound traffic to raw monitor clients before the
	 * request is routed, so the attempt is visible even when the radio
	 * upstream is unreachable or the request is refused here.
	 */
	switch (hdr->datakind) {
	case AGWPE_CMD_CONNECT:
	case AGWPE_CMD_CONNECT_PID:
	case AGWPE_CMD_CONNECT_VIA:
		/*
		 * A connect for a pair that is already up is answered with
		 * a refusal and never reaches the upstream, but the SABM
		 * is the frame that names the pair: mirrored, it says a
		 * connection this station already has was called again,
		 * and there is no such frame.  The pair is asked here the
		 * same way the router asks it a few lines down, and a
		 * pair that is up leaves no mirror behind.  The refusal
		 * still reaches the client.
		 */
		{
			struct ax25netd_upstream *cu = up_by_port(port);

			if (cu != NULL &&
			    session_find(cu, hdr->call_from, hdr->call_to,
					 session_pid(hdr->pid), -1) != NULL)
				break;
		}
		mux_mirror_raw(hdr, data, len);
		break;
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
		u = up_by_port_or_log(port, cl, "register");
		if (u == NULL) {
			reply_register(cl, hdr, 0);
			break;
		}
		/* Every refusal here is a one-byte 'Z' reply that says
		 * nothing about which of the three things went wrong, and
		 * the client then either retries the callsign forever or
		 * gives up on it.  The three are: an empty callsign, which
		 * is the client's own bug; the callsign already being
		 * registered by another client, which is a name clash and
		 * is the common one; and no memory for the entry, which is
		 * not the caller's fault at all. */
		if (hdr->call_from[0] == '\0') {
			ax25netd_verbose("client %d: register on port "
					 "%u refused, no callsign given",
					 cl->fd, port);
			reply_register(cl, hdr, 0);
			break;
		}
		if (mux_call_register(u, hdr->call_from, cl->fd, 0,
				      port_chan(u, port)) < 0) {
			ax25netd_verbose("client %d: register of '%s' on "
					 "port %u refused, %s", cl->fd,
					 hdr->call_from, port,
					 mux_call_owner(u, hdr->call_from));
			reply_register(cl, hdr, 0);
			break;
		}
		ax25netd_verbose("client %d: registered '%s' on port %u "
				 "(%s, upstream %s)",
				 cl->fd, hdr->call_from, port,
				 u->connected ? "connected" :
				 "upstream not connected, so not sent on",
				 u->name);
		if (u->connected)
			agwpe_client_register(u->cli, port_chan(u, port),
					      hdr->call_from);
		reply_register(cl, hdr, 1);
		break;

	case 'L':				/* listen (loop port only) */
		u = up_by_port_or_log(port, cl, "listen");
		if (u != &ax25netd.loop) {
			/* A listen on a radio port is refused the same
			 * way every time and is the sort of thing that is
			 * only ever tried once, so it is worth a line:
			 * the client's registration table does not show
			 * it and the refusal frame does not say why. */
			ax25netd_verbose("client %d: listen on port %u "
					 "refused, only the loop port %d "
					 "accepts one", cl->fd, port,
					 AGWPE_PORT_LOOP);
			reply_register(cl, hdr, 0);
			break;
		}
		if (hdr->call_from[0] == '\0') {
			ax25netd_verbose("client %d: listen on the loop "
					 "port refused, no callsign given",
					 cl->fd);
			reply_register(cl, hdr, 0);
			break;
		}
		if (mux_call_register(u, hdr->call_from, cl->fd, 1, 0) < 0) {
			ax25netd_verbose("client %d: listen of '%s' on "
					 "the loop port refused, %s", cl->fd,
					 hdr->call_from,
					 mux_call_owner(u, hdr->call_from));
			reply_register(cl, hdr, 0);
			break;
		}
		ax25netd_verbose("client %d: listening for '%s' on the "
				 "loop port", cl->fd, hdr->call_from);
		reply_register(cl, hdr, 1);
		break;

	case 'x':				/* unregister callsign */
		u = up_by_port_or_log(port, cl, "unregister");
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
			uint32_t major = agwpe_host2netle(AX25NETD_VERSION_MAJOR);
			uint32_t minor = agwpe_host2netle(AX25NETD_VERSION_MINOR);

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
				ax25netd_verbose("client %d: port list held back "
						 "for up to %d s, waiting for %s",
						 cl->fd,
						 AX25NETD_PORTS_TIMEOUT,
						 mux_ports_pending());
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
		u = up_by_port_or_log(port, cl, "heard list");
		if (u == NULL)
			break;
		u->heard_to = cl->fd;
		if (u->connected)
			agwpe_client_get_heard(u->cli, port_chan(u, port));
		break;

	case 'y':				/* outstanding frames on a port */
	case 'Y':				/* outstanding frames for a connection */
		u = up_by_port_or_log(port, cl, "outstanding frames");
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
		ax25netd_verbose("client %d: monitor %s", cl->fd,
				 cl->monitor ? "on" : "off");
		mux_recalc_toggles();
		break;

	case 'k':				/* raw monitor toggle */
		cl->raw = !cl->raw;
		ax25netd_verbose("client %d: raw monitor %s", cl->fd,
				 cl->raw ? "on" : "off");
		mux_recalc_toggles();
		break;

	case 'C':				/* connect */
	case 'c':				/* connect with PID */
	case 'v':				/* connect via digipeaters */
		{
			unsigned char pid = session_pid(hdr->pid);

			u = up_by_port_radio(port, cl, "connect");
			if (u == NULL)
				break;
			if (u == &ax25netd.loop) {
				loop_connect(cl, hdr, data, len);
				break;
			}

			/*
			 * An AX.25 connection is identified by its call
			 * pair (source and destination, including SSIDs).
			 * Neither a digipeater path nor a different PID
			 * makes it a different connection, so a second
			 * connect for an established pair is refused.
			 */
			if (session_find(u, hdr->call_from, hdr->call_to,
					 pid, -1) != NULL) {
				reply_refuse(cl, hdr, "BUSY");
				break;
			}

			session_add(u, hdr->call_from, hdr->call_to,
				    pid, cl->fd, port_chan(u, port));

			/*
			 * Autoroute: a connect that names no digipeaters
			 * asks the ax25rtd route cache for a learned
			 * path to the destination on this port.  A
			 * connect with an explicit digipeater path ('v')
			 * is never touched.  No route, or an
			 * unresponsive route daemon, falls back to the
			 * plain connect.
			 */
			if (ax25netd.autoroute &&
			    (hdr->datakind == AGWPE_CMD_CONNECT ||
			     hdr->datakind == AGWPE_CMD_CONNECT_PID)) {
				struct agwpe_s vh;
				unsigned char digis[1 +
					(AGWPE_MAX_DIGIS - 1) * AGWPE_MAX_CALL];
				int ndigi;

				ndigi = ax25netd_route_lookup(u, hdr->call_to,
							  digis, sizeof(digis));
				if (ndigi > 0) {
					vh = *hdr;
					vh.datakind = AGWPE_CMD_CONNECT_VIA;
					vh.data_len = agwpe_host2netle(
						1 + ndigi * AGWPE_MAX_CALL);
					client_send_upstream(u, &vh, digis);
					break;
				}
			}
			client_send_upstream(u, hdr, data);
		}
		break;

	case 'D':				/* connected data */
		u = up_by_port_radio(port, cl, "data");
		if (u == NULL)
			break;
		if (u == &ax25netd.loop) {
			loop_data(cl, hdr, data, len);
			break;
		}

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
			struct ax25netd_session *s;
			int i;

			u = up_by_port_radio(port, cl, "disconnect");
			if (u == NULL)
				break;
			if (u == &ax25netd.loop) {
				loop_disconnect(cl, hdr, data, len);
				break;
			}

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
		u = up_by_port_radio(port, cl, "frame");
		if (u == NULL)
			break;
		if (u == &ax25netd.loop) {
			loop_unproto(cl, hdr, data, len);
			break;
		}
		client_send_upstream(u, hdr, data);
		break;

	case 'Q':				/* connection control */
		if (data != NULL && len > 0 && data[0] == AGWPE_CTL_KILL)
			mux_ctl_kill(cl, hdr);
		else if (data != NULL && len > 0 &&
			 data[0] == AGWPE_CTL_PARAM)
			mux_ctl_param(cl, hdr, data, len);
		else if (data != NULL && len > 1 &&
			 data[0] == AGWPE_CTL_MONMASK)
			mux_ctl_monmask(cl, data[1]);
		else if (data != NULL && len > 0 &&
			 data[0] == AGWPE_CTL_UISUB)
			mux_ctl_uisub(cl);
		else if (data != NULL && len > 0 &&
			 data[0] == AGWPE_CTL_SESSIONS)
			mux_ctl_sessions(cl);
		else if (data != NULL && len >= 5 &&
			 data[0] == AGWPE_CTL_KILLID)
			mux_ctl_killid(cl, data, len);
		break;

	default:
		ax25netd_log(LOG_WARNING, "client %d: unknown frame '%c'",
			 cl->fd, hdr->datakind);
		break;
	}
}

void mux_client_disconnect(struct ax25netd_client *cl)
{
	int i, j;

	cl->want_ports = 0;

	for (i = 0; i < ax25netd.nup; i++) {
		struct ax25netd_upstream *u = &ax25netd.ups[i];

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

	if (ax25netd.loop_enabled) {
		if (ax25netd.loop.heard_to == cl->fd)
			ax25netd.loop.heard_to = -1;
		if (ax25netd.loop.out_to == cl->fd)
			ax25netd.loop.out_to = -1;
		loop_client_gone(cl);
	}

	/* Its subscription ends with it.  Nothing else has to be undone: the
	 * frames it asked for are addressed to its call signs, and it is no
	 * longer on the loop port to be given them.  The count is kept
	 * because mux_upstream_ui() asks it about every heard frame, and a
	 * client that has gone must not keep the answer at "yes".  */
	if (cl->uisub) {
		cl->uisub = 0;
		mux_uisub_clients--;
	}

	mux_recalc_toggles();
}

/* Whether any client subscribed to the UI delivery at all.  The answer decides
 * whether a heard frame has to be taken apart at all. */
static int mux_uisub_any(void)
{
	return mux_uisub_clients > 0;
}

/*
 * A UI frame off the radio, addressed to a call sign one of our clients holds,
 * handed to that client as an 'M' frame.
 *
 * This is the loop port's own mechanism - loop_unproto() above does the same
 * for a frame from another local client - and putting it on the radio ports is
 * what lets a datagram socket read its UI frames without the raw monitor
 * stream.  That was the alternative: the socket turned the stream on, which is
 * a toggle on the whole connection, so from then on every heard frame with its
 * full payload was duplicated into that connection for it to pick one kind out
 * of - and it stayed on, for every userland program on the machine, for as long
 * as that one socket was open.
 *
 * Three things are deliberately not done here:
 *
 *   - The frame is not taken away from the monitors.  A monitor wants
 *     everything, including the frames a client already has; that is what a
 *     monitor is.  So the raw fan-out in mux_upstream_frame() still sees it.
 *
 *   - Our own transmissions are not filtered.  A station is not told its own
 *     frames, but a UI frame this process digipeated comes back addressed to us
 *     and would be delivered as if it were somebody else's.  The library keeps
 *     a record of what it sent and drops those; the server has no such record
 *     and cannot keep one per client without becoming the thing it exists to
 *     avoid.  A program that digipeates its own UI frames is the one place
 *     where this is visible.
 *
 *   - Frames for a client that did not ask are not delivered.  Registering a
 *     call sign says "watch for connections to it"; subscribing says "and hand
 *     me the UI frames too".  A program that only does the first must not get
 *     the second, or it would find unrelated packets in a read().
 */
static void mux_upstream_ui(struct ax25netd_upstream *u, const struct agwpe_s *hdr,
			    const unsigned char *data, size_t len)
{
	char dst[AGWPE_MAX_CALL], src[AGWPE_MAX_CALL];
	const unsigned char *info;
	struct ax25netd_client *owner;
	struct agwpe_s out;
	size_t ilen;
	unsigned char pid;
	int repeated;

	/*
	 * The loop port routes its own frames, in loop_unproto(); coming
	 * back through here would deliver them twice.
	 */
	if (u->virtual)
		return;

	/*
	 * Nobody to give it to.  Asked before the frame is taken apart,
	 * because taking one apart is more work than the answer, and this
	 * stands on the path of every heard frame of a server whose clients
	 * only monitor - which is the common case, and the one where this
	 * must cost nothing.
	 */
	if (!mux_uisub_any())
		return;

	/*
	 * The library's own decoder, not a second one.  It is the library
	 * that turns these addresses into the strings its sockets are
	 * matched by, so anything else here is a second answer to the same
	 * question - and the wrong one silently, which is a frame delivered
	 * to the wrong socket.
	 */
	if (!agwpe_kiss_ui_parse(data, len, dst, sizeof(dst), src,
				 sizeof(src), &pid, &info, &ilen,
				 &repeated))
		return;			/* not a UI frame, or not whole */

	owner = client_by_call(u, dst);
	if (owner == NULL || !owner->uisub)
		return;

	agwpe_header_init(&out, hdr->port, AGWPE_CMD_UNPROTO, pid, src, dst,
			  (uint32_t)ilen);
	loop_send_client(owner, &out, info, ilen);

	if (ax25netd.debug)
		ax25netd_log(LOG_DEBUG,
			     "client %d: UI %s>%s on port %u, pid %u, "
			     "%zu bytes", owner->fd, src, dst, hdr->port,
			     pid, ilen);
}

void mux_upstream_frame(struct ax25netd_upstream *u, const struct agwpe_s *hdr,
			const unsigned char *data, size_t len)
{
	struct ax25netd_client *owner;
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
		for (i = 0; i < ax25netd.nclients; i++) {
			struct ax25netd_client *cl = &ax25netd.clients[i];

			if (cl->fd >= 0 && cl->monitor)
				loop_send_upstream(cl, u, hdr, data, len);
		}
		break;

	case 'K':				/* raw monitored frame */
		ax25netd_mheard_frame(u, data, len);
		mux_upstream_ui(u, hdr, data, len);
		for (i = 0; i < ax25netd.nclients; i++) {
			struct ax25netd_client *cl = &ax25netd.clients[i];
			size_t n;

			if (cl->fd < 0 || !cl->raw)
				continue;
			/* Per client, same as the local mirror above. */
			n = raw_monitor_len(cl->monmask, data, len);
			loop_send_upstream(cl, u, hdr, data, n);
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

void mux_upstream_reconnected(struct ax25netd_upstream *u)
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
void mux_upstream_lost(struct ax25netd_upstream *u)
{
	int i;

	for (i = 0; i < u->nsessions; i++) {
		struct ax25netd_session *s = &u->sessions[i];
		struct ax25netd_client *cl = client_by_fd(s->fd);
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

	for (i = 0; i < ax25netd.nup; i++) {
		struct ax25netd_upstream *u = &ax25netd.ups[i];

		/* Raw monitoring stays on while the heard list is enabled: the
		 * multiplexer needs every frame the radio receives for mheard,
		 * so a raw frame arrives even while no loop client listens.
		 * Without the heard list the upstream raw toggle follows the
		 * loop clients that asked for it.
		 */
		want_m = 0;
		want_k = ax25netd.mheard;
		for (j = 0; j < ax25netd.nclients; j++) {
			if (ax25netd.clients[j].fd >= 0 && ax25netd.clients[j].monitor)
				want_m = 1;
			if (ax25netd.clients[j].fd >= 0 && ax25netd.clients[j].raw)
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

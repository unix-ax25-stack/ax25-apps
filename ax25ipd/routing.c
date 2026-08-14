/* routing.c    Routing table manipulation routines
 *
 * Copyright 1991, Michael Westerhof, Sun Microsystems, Inc.
 * This software may be freely used, distributed, or modified, providing
 * this header is not removed.
 *
 */

#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "ax25ipd.h"

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

/* The routing table structure is not visible outside this module. */

/*
 * The address of the far end, port included.  This used to be four bytes of
 * IPv4 address followed by the port, and call_to_ip() handed out a pointer to
 * the first of them so that the caller could read the port from the bytes
 * behind it - the comment on that function called it ugly and meant to fix it
 * later.  Later is now: an IPv6 address does not fit that shape at all.
 *
 * A port of zero still means "send this as protocol 93", a non-zero port
 * means "send it as UDP to that port".  That is how the configuration file
 * has always distinguished axip from axudp, and it stays that way.
 */
struct route_table_entry {
	unsigned char callsign[7];	/* the callsign and ssid */
	unsigned char padcall;	/* always set to zero */
	struct sockaddr_storage addr;	/* where to send it, port included */
	socklen_t addrlen;
	unsigned int flags;	/* route flags */
	struct route_table_entry *next;
};

static struct route_table_entry *route_tbl;
static struct route_table_entry *default_route;

/* The Broadcast address structure is not visible outside this module either */

struct bcast_table_entry {
	unsigned char callsign[7];	/* The broadcast address */
	struct bcast_table_entry *next;
};

static struct bcast_table_entry *bcast_tbl;

/* Initialize the routing module */
void route_init(void)
{
	route_tbl = NULL;
	default_route = NULL;
	bcast_tbl = NULL;
}

/*
 * Printable form of a route address, "addr" or "[addr]" for IPv6, without
 * the port.  Returns a pointer to a static buffer, like inet_ntoa() did.
 */
const char *addr_to_a(const struct sockaddr *sa)
{
	static char buf[ADDR_STRLEN];
	char host[INET6_ADDRSTRLEN];
	const void *src;

	if (sa == NULL)
		return "(none)";
	switch (sa->sa_family) {
	case AF_INET:
		src = &((const struct sockaddr_in *) sa)->sin_addr;
		if (inet_ntop(AF_INET, src, host, sizeof host) == NULL)
			return "(bad)";
		snprintf(buf, sizeof buf, "%s", host);
		return buf;
#ifdef AF_INET6
	case AF_INET6:
		src = &((const struct sockaddr_in6 *) sa)->sin6_addr;
		if (inet_ntop(AF_INET6, src, host, sizeof host) == NULL)
			return "(bad)";
		snprintf(buf, sizeof buf, "[%s]", host);
		return buf;
#endif
	default:
		return "(unknown family)";
	}
}

/* The port of a route address, in host byte order; 0 means protocol 93 */
unsigned short addr_port(const struct sockaddr *sa)
{
	if (sa == NULL)
		return 0;
	if (sa->sa_family == AF_INET)
		return ntohs(((const struct sockaddr_in *) sa)->sin_port);
#ifdef AF_INET6
	if (sa->sa_family == AF_INET6)
		return ntohs(((const struct sockaddr_in6 *) sa)->sin6_port);
#endif
	return 0;
}

/* Add a new route entry */
void route_add(const struct sockaddr *sa, socklen_t salen,
	unsigned char *call, unsigned int flags)
{
	struct route_table_entry *rl, *rn;
	int i;

	/* Check we have an address */
	if (sa == NULL || salen == 0 || (size_t) salen > sizeof(rn->addr))
		return;

	/* Check we have a callsign */
	if (call == NULL)
		return;

	/* Find the last entry in the list */
	rl = route_tbl;
	if (route_tbl)
		while (rl->next)
			rl = rl->next;

	rn = (struct route_table_entry *)
	    malloc(sizeof(struct route_table_entry));
	if (rn == NULL)
		return;

	/* Build this entry ... */
	memset(rn, 0, sizeof(*rn));
	for (i = 0; i < 6; i++)
		rn->callsign[i] = call[i] & 0xfe;
	rn->callsign[6] = (call[6] & 0x1e) | 0x60;
	rn->padcall = 0;
	memcpy(&rn->addr, sa, (size_t) salen);
	rn->addrlen = salen;
	rn->flags = flags;
	rn->next = NULL;

	/* Update the default_route pointer if this is a default route */
	if (flags & AXRT_DEFAULT)
		default_route = rn;

	if (rl)			/* ... the list is already started add the new route */
		rl->next = rn;
	else			/* ... start the list off */
		route_tbl = rn;

	/* Log this entry ... */
	LOGL4("added route: %s %s %s %d %d\n",
	      call_to_a(rn->callsign),
	      addr_to_a((struct sockaddr *) &rn->addr),
	      addr_port((struct sockaddr *) &rn->addr) ? "udp" : "ip",
	      addr_port((struct sockaddr *) &rn->addr), flags);
}

/* Add a new broadcast address entry */
void bcast_add(unsigned char *call)
{
	struct bcast_table_entry *bl, *bn;
	int i;

	/* Check we have a callsign */
	if (call == NULL)
		return;

	/* Find the last entry in the list */
	bl = bcast_tbl;
	if (bcast_tbl)
		while (bl->next)
			bl = bl->next;

	bn = (struct bcast_table_entry *)
	    malloc(sizeof(struct bcast_table_entry));

	/* Build this entry ... */
	for (i = 0; i < 6; i++)
		bn->callsign[i] = call[i] & 0xfe;
	bn->callsign[6] = (call[6] & 0x1e) | 0x60;

	bn->next = NULL;

	if (bl)			/* ... the list is already started add the new route */
		bl->next = bn;
	else			/* ... start the list off */
		bcast_tbl = bn;

	/* Log this entry ... */
	LOGL4("added broadcast address: %s\n", call_to_a(bn->callsign));
}

/*
 * Return the address to send to for a callsign, or NULL if there is none.
 * The length goes to *lenp.  The port is part of the address; a port of zero
 * means protocol 93 rather than UDP.
 */

const struct sockaddr *call_to_addr(unsigned char *call, socklen_t *lenp)
{
	struct route_table_entry *rp;
	unsigned char mycall[7];
	int i;

	if (call == NULL)
		return NULL;

	for (i = 0; i < 6; i++)
		mycall[i] = call[i] & 0xfe;

	mycall[6] = (call[6] & 0x1e) | 0x60;

	LOGL4("lookup call %s ", call_to_a(mycall));

	rp = route_tbl;
	while (rp) {
		if (addrmatch(mycall, rp->callsign)) {
			LOGL4("found addr %s\n",
			      addr_to_a((struct sockaddr *) &rp->addr));
			if (lenp)
				*lenp = rp->addrlen;
			return (const struct sockaddr *) &rp->addr;
		}
		rp = rp->next;
	}

	/*
	 * No match found in the routing table, use the default route if
	 * we have one defined.
	 */
	if (default_route) {
		LOGL4("failed, using default addr %s\n",
		      addr_to_a((struct sockaddr *) &default_route->addr));
		if (lenp)
			*lenp = default_route->addrlen;
		return (const struct sockaddr *) &default_route->addr;
	}

	LOGL4("failed.\n");
	return NULL;
}

/*
 * Accept a callsign and return true if it is a broadcast address, or false
 * if it is not found on the list
 */
int is_call_bcast(unsigned char *call)
{
	struct bcast_table_entry *bp;
	unsigned char bccall[7];
	int i;

	if (call == NULL)
		return FALSE;

	for (i = 0; i < 6; i++)
		bccall[i] = call[i] & 0xfe;

	bccall[6] = (call[6] & 0x1e) | 0x60;

	LOGL4("lookup broadcast %s ", call_to_a(bccall));

	bp = bcast_tbl;
	while (bp) {
		if (addrmatch(bccall, bp->callsign)) {
			LOGL4("found broadcast %s\n",
			      call_to_a(bp->callsign));
			return TRUE;
		}
		bp = bp->next;
	}
	return FALSE;
}

/* Traverse the routing table, transmitting the packet to each bcast route */
void send_broadcast(unsigned char *buf, int l)
{
	struct route_table_entry *rp;

	rp = route_tbl;
	while (rp) {
		if (rp->flags & AXRT_BCAST) {
			send_ip(buf, l, (struct sockaddr *) &rp->addr,
				rp->addrlen);
		}
		rp = rp->next;
	}
}

/* Do we have any route to this address family?  Used to warn about routes
 * that can never be used because no socket of that family got opened.
 */
int routes_have_family(int family)
{
	struct route_table_entry *rp;

	for (rp = route_tbl; rp; rp = rp->next)
		if (rp->addr.ss_family == family)
			return 1;
	return 0;
}

/* print out the list of routes */
void dump_routes(void)
{
	struct route_table_entry *rp;
	int i;

	for (rp = route_tbl, i = 0; rp; rp = rp->next)
		i++;

	LOGL1("\n%d active routes.\n", i);

	rp = route_tbl;
	while (rp) {
		LOGL1("  %s\t%s\t%s\t%d\t%d\n",
		      call_to_a(rp->callsign),
		      addr_to_a((struct sockaddr *) &rp->addr),
		      addr_port((struct sockaddr *) &rp->addr) ? "udp" : "ip",
		      addr_port((struct sockaddr *) &rp->addr), rp->flags);
		rp = rp->next;
	}
	fflush(stdout);
}

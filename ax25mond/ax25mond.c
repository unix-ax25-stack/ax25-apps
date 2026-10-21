/*
 *  ax25mond.c - Wrapper for offering AX.25 traffic to non-root processes
 *
 *  Copyright (C) 1998-1999 Johann Hanne, DH3MB. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/*--------------------------------------------------------------------------*/

#define CONFFILE "/etc/ax25/ax25mond.conf"
#define MAX_SOCKETS  5
#define MAX_CONNECTS 50

/*--------------------------------------------------------------------------*/

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/fcntl.h>
#include <sys/signal.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <config.h>

/* For older kernels  */
#ifndef PF_PACKET
#define PF_PACKET PF_INET
#endif

/* Protocol numbers for PF_PACKET sockets (from linux/if_ether.h) */
#ifndef ETH_P_AX25
#define ETH_P_AX25	0x0002
#endif
#ifndef ETH_P_ALL
#define ETH_P_ALL	0x0003
#endif

#include <netax25/ax25.h>
#include <netax25/axmon.h>
#include <netax25/axconfig.h>

/*--------------------------------------------------------------------------*/

static union {
	struct sockaddr sa;
	struct sockaddr_in si;
	struct sockaddr_un su;
} addr;

/*--------------------------------------------------------------------------*/

static int sock_list[MAX_SOCKETS];
static char sock_monmode[MAX_SOCKETS];
static char sock_filename[MAX_SOCKETS][100];
static int sock_num = 0;
static int conn_list[MAX_CONNECTS];
static struct sockaddr conn_addr[MAX_CONNECTS];
static socklen_t conn_addrlen[MAX_CONNECTS];
static char conn_monmode[MAX_CONNECTS];
static int conn_num = 0;
static int highest_sock_fd;
static int end = 0;

/*--------------------------------------------------------------------------*/

/* from buildsaddr.c */
static struct sockaddr *build_sockaddr(const char *name, int *addrlen)
{
	char *host_name;
	char *serv_name;
	char buf[1024];

	memset(&addr, 0, sizeof(addr));
	*addrlen = 0;

	host_name = strcpy(buf, name);
	serv_name = strchr(buf, ':');
	if (!serv_name)
		return NULL;
	*serv_name++ = 0;
	if (!*host_name || !*serv_name)
		return NULL;

	if (!strcmp(host_name, "local") || !strcmp(host_name, "unix")) {
		addr.su.sun_family = AF_UNIX;
		snprintf(addr.su.sun_path, sizeof(addr.su.sun_path), "%s",
			 serv_name);
		*addrlen = sizeof(struct sockaddr_un);
		return &addr.sa;
	}

	addr.si.sin_family = AF_INET;

	if (!strcmp(host_name, "*")) {
		addr.si.sin_addr.s_addr = INADDR_ANY;
	} else if (!strcmp(host_name, "loopback")) {
		addr.si.sin_addr.s_addr = inet_addr("127.0.0.1");
	} else {addr.si.sin_addr.s_addr = inet_addr(host_name);
		if (addr.si.sin_addr.s_addr == -1) {
			struct hostent *hp = gethostbyname(host_name);
			endhostent();
			if (!hp)
				return NULL;
			addr.si.sin_addr.s_addr =
			    ((struct in_addr *) (hp->h_addr))->s_addr;
		}
	}

	if (isdigit(*serv_name & 0xff)) {
		addr.si.sin_port = htons(atoi(serv_name));
	} else {
		struct servent *sp = getservbyname(serv_name, NULL);
		endservent();
		if (!sp)
			return NULL;
		addr.si.sin_port = sp->s_port;
	}

	*addrlen = sizeof(struct sockaddr_in);
	return &addr.sa;
}

/*--------------------------------------------------------------------------*/

static void add_socket(char *sockname, char monmode)
{
	struct sockaddr *saddr;
	int saddrlen;

	if (sock_num >= MAX_SOCKETS) {
		fprintf(stderr,
			"WARNING: Too many sockets defined - only %d are "
			"allowed\n", MAX_SOCKETS);
		return;
	}

	if (!(saddr = build_sockaddr(sockname, &saddrlen))) {
		fprintf(stderr, "WARNING: Invalid socket name: \"%s\"\n",
			sockname);
		return;
	}

	if (saddr->sa_family == AF_UNIX) {
		snprintf(sock_filename[sock_num], sizeof(sock_filename[0]),
			 "%s", strchr(sockname, ':') + 1);
	} else
		sock_filename[sock_num][0] = 0;

	sock_list[sock_num] = socket(saddr->sa_family, SOCK_STREAM, 0);
	if (sock_list[sock_num] < 0) {
		fprintf(stderr,
			"WARNING: Error opening socket \"%s\": %s\n",
			sockname, strerror(errno));
		return;
	}

	if (bind(sock_list[sock_num], saddr, saddrlen) < 0) {
		fprintf(stderr,
			"WARNING: Error binding socket \"%s\": %s\n",
			sockname, strerror(errno));
		return;
	}

	if (listen(sock_list[sock_num], 5) < 0) {
		fprintf(stderr,
			"WARNING: Error listening on socket \"%s\": %s\n",
			sockname, strerror(errno));
		return;
	}

	/* Allow immediate rebind after restart (avoid TIME_WAIT) */
	{
		int opt = 1;
		setsockopt(sock_list[sock_num], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	}

	fcntl(sock_list[sock_num], F_SETFL, O_NONBLOCK);

	if (sock_list[sock_num] > highest_sock_fd)
		highest_sock_fd = sock_list[sock_num];

	sock_monmode[sock_num] = monmode;
	sock_num++;
}

/*--------------------------------------------------------------------------*/

static void close_sockets(void)
{
	int i;

	for (i = 0; i < sock_num; i++) {
		close(sock_list[i]);

		if (sock_filename[i][0])
			unlink(sock_filename[i]);
	}
}

/*--------------------------------------------------------------------------*/

static void quit_handler(int dummy)
{
	end = 1;
}

/*--------------------------------------------------------------------------*/

int main(int argc, char *argv[])
{
	fd_set monavail, conn_request;
	struct axmon mon_rx, mon_rxtx;
	struct timeval tv;
	char buf[500];
	int size, i;
	FILE *conffile;
	char confline[100];
	char *mode;
	char *sockname;

	if (argc > 1) {
		if (argc == 2 &&
		    (strcmp(argv[1], "-v") == 0
		     || strcmp(argv[1], "--version") == 0)) {
			printf("%s: Version " VERSION "\n\n", argv[0]);
			printf
			    ("Copyright (C) 1998-1999 Johann Hanne, DH3MB. All rights reserved.\n"
			     "This program is free software; you can redistribute it and/or modify\n"
			     "it under the terms of the GNU General Public License as published by\n"
			     "the Free Software Foundation; either version 2 of the License, or\n"
			     "(at your option) any later version.\n\n"
			     "This program is distributed in the hope that it will be useful,\n"
			     "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
			     "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n\n");
			exit(1);
		} else {
			printf("Usage: %s [-v|--version]\n", argv[0]);
			exit(0);
		}
	}
	/* At first, read the configuration file  */
	if (!(conffile = fopen(CONFFILE, "r"))) {
		fprintf(stderr, "Unable to open " CONFFILE ".\n");
		exit(1);
	}
	while (fgets(confline, 100, conffile)) {
		size_t len;

		if (confline[0] == '#')	/* Comment  */
			continue;

		len = strlen(confline);
		if (len == 0)
			continue;
		if (confline[len - 1] == '\n')
			confline[len - 1] = 0;	/* Cut the LF  */

		if (!(sockname = strchr(confline, ' '))) {
			fprintf(stderr,
				"WARNING: The following configuration line includes "
				"one or more errors:\n%s\n", confline);
			continue;
		}

		*(sockname++) = 0;
		mode = confline;
		if (strcasecmp(mode, "RX") == 0)
			add_socket(sockname, 0);
		else if (strcasecmp(mode, "RXTX") == 0)
			add_socket(sockname, 1);
		else
			fprintf(stderr,
				"WARNING: Mode \"%s\" not supported\n",
				mode);

	}
	fclose(conffile);

	if (sock_num == 0) {
		fprintf(stderr, "FATAL: No usable socket found\n");
		exit(1);
	}
	/* Fork into background  */
	if (fork())
		exit(0);

	/* Close stdout, stderr and stdin  */
	fclose(stdout);
	fclose(stderr);
	fclose(stdin);

	/* Set some signal handlers  */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, quit_handler);

	/* Load port configuration for axmon */
	ax25_config_load_ports();

	/* Open monitor for RX-only (ETH_P_AX25) using libax25 axmon_open() */
	if (axmon_open(htons(ETH_P_AX25), NULL, &mon_rx) < 0) {
		syslog(LOG_ERR, "Error opening RX monitor: %s\n", strerror(errno));
		exit(1);
	}

	/* Open monitor for RX+TX (ETH_P_ALL) using libax25 axmon_open() */
	if (axmon_open(htons(ETH_P_ALL), NULL, &mon_rxtx) < 0) {
		syslog(LOG_ERR, "Error opening RXTX monitor: %s\n", strerror(errno));
		exit(1);
	}

	/* Set monitor sockets non-blocking */
	for (i = 0; i < mon_rx.nfd; i++)
		fcntl(mon_rx.fd[i], F_SETFL, O_NONBLOCK);
	for (i = 0; i < mon_rxtx.nfd; i++)
		fcntl(mon_rxtx.fd[i], F_SETFL, O_NONBLOCK);

	while (!end) {
		/* Look for incoming connects on all open sockets  */
		FD_ZERO(&conn_request);
		for (i = 0; i < sock_num; i++)
			FD_SET(sock_list[i], &conn_request);
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		select(highest_sock_fd + 1, &conn_request, NULL, NULL, &tv);
		for (i = 0; i < sock_num; i++)
			if (FD_ISSET(sock_list[i], &conn_request)) {
				int new_fd;

				conn_addrlen[conn_num] =
				    sizeof(conn_addr[conn_num]);
				new_fd = accept(sock_list[i],
						&conn_addr[conn_num],
						&conn_addrlen[conn_num]);
				if (new_fd < 0)
					continue;
				if (conn_num >= MAX_CONNECTS) {
					close(new_fd);
					continue;
				}
				conn_list[conn_num] = new_fd;
				conn_monmode[conn_num] = sock_monmode[i];
				conn_num++;
			}

		/* Check if there is new data on the RX-only monitor sockets */
		FD_ZERO(&monavail);
		for (i = 0; i < mon_rx.nfd; i++)
			FD_SET(mon_rx.fd[i], &monavail);
		tv.tv_sec = 0;
		tv.tv_usec = 10;
		if (select(mon_rx.fd[mon_rx.nfd - 1] + 1, &monavail, NULL, NULL, &tv) > 0) {
			for (int m = 0; m < mon_rx.nfd; m++) {
				if (FD_ISSET(mon_rx.fd[m], &monavail)) {
					struct sockaddr monfrom;
					socklen_t monfromlen = sizeof(struct sockaddr);
					size = axmon_read(mon_rx.fd[m], mon_rx.framed[m], buf,
							  sizeof(buf), &monfrom, &monfromlen);
					if (size <= 0) {
						syslog(LOG_ERR, "AX.25 monitor closed: %s\n", strerror(errno));
						end = 1;
						break;
					}
					for (int j = 0; j < conn_num; j++) {
						if (conn_monmode[j] == 0)
							if (send(conn_list[j], buf, size, 0) < 0)
								if (errno != EAGAIN) {
									syslog(LOG_ERR, "Error sending monitor data: %s\n", strerror(errno));
									close(conn_list[j]);
									conn_list[j] = conn_list[conn_num - 1];
									conn_num--;
								}
					}
				}
			}
		}

		/* Check if there is new data on the RX+TX monitor sockets */
		FD_ZERO(&monavail);
		for (int m = 0; m < mon_rxtx.nfd; m++)
			FD_SET(mon_rxtx.fd[m], &monavail);
		tv.tv_sec = 0;
		tv.tv_usec = 10;
		if (select(mon_rxtx.fd[mon_rxtx.nfd - 1] + 1, &monavail, NULL, NULL, &tv) > 0) {
			for (int m = 0; m < mon_rxtx.nfd; m++) {
				if (FD_ISSET(mon_rxtx.fd[m], &monavail)) {
					struct sockaddr monfrom;
					socklen_t monfromlen = sizeof(struct sockaddr);
					size = axmon_read(mon_rxtx.fd[m], mon_rxtx.framed[m], buf,
							  sizeof(buf), &monfrom, &monfromlen);
					if (size <= 0) {
						syslog(LOG_ERR, "AX.25 monitor closed: %s\n", strerror(errno));
						end = 1;
						break;
					}
					for (int j = 0; j < conn_num; j++) {
						if (conn_monmode[j] == 1)
							if (send(conn_list[j], buf, size, 0) < 0)
								if (errno != EAGAIN) {
									syslog(LOG_ERR, "Error sending monitor data: %s\n", strerror(errno));
									close(conn_list[j]);
									conn_list[j] = conn_list[conn_num - 1];
									conn_num--;
								}
					}
				}
			}
		}
	}

	/* Cleanup */
	for (i = 0; i < conn_num; i++)
		close(conn_list[i]);
	close_sockets();
	axmon_close(&mon_rx);
	axmon_close(&mon_rxtx);
	closelog();
	return 0;
}

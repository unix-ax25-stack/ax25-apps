/* AX25TPCD - interactive frontend to an AGWPE multiplexer
 *
 * ax25tcpd exposes an ax25netd instance as a small TCP and unix domain
 * service.  Each incoming connection is an interactive session: the
 * first line is a command, later bytes are packet data.
 *
 * Ports:
 *   X      binary: everything sent after the command line is one
 *                  packet, split into mtu sized chunks on close.
 *   X+1    text:   each CR/LF terminated line is one packet.
 *   unix socket:   behaves like the text port.
 *
 * Commands (unambiguous prefixes work):
 *   connect [--silent] [--keep] [--pid=XX] [--port <port>] [--mycall <call>]
 *           <port>[:chan] <dest>[,<digi>,...] [< SRC]
 *   datagram [--silent] [--keep] [--pid=XX] [--port <port>] [--mycall <call>]
 *            <port>[:chan] [<dest>[,<digi>,...]] [< SRC]
 *            Without a destination every line is a whole TNC2 frame
 *            ("SRC>DEST,DIGI,...:payload") whose header is its own;
 *            with one, the header is fixed and lines are payload.
 *   quit | bye
 *   help
 *
 * Answers are in the client's own line convention: a client ending its
 * lines with '\r' gets CRLF replies, any other gets bare LF.
 *
 * The back side is one AGWPE connection to an ax25netd loop port, over
 * TCP or a unix domain socket, using the AGWPE client library.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <syslog.h>
#include <grp.h>
#include <netax25/agwpe.h>
#include <netax25/agwpe_client.h>
#include <netax25/agwpe_config.h>
#include <netax25/axcommon.h>

/* ------------------------------------------------------------------ */

enum {
	TPC_CMD = 0,		/* awaiting a command line */
	TPC_CONNECTING,		/* connect sent, waiting for the 'C' confirm */
	TPC_DATA,		/* connected, byte stream */
	TPC_DGRAM,		/* datagram mode */
};

#define	TPC_MAX_LISTEN	8
#define	TPC_MAX_CLIENT	128
#define	TPC_LINE_MAX	4096
#define	TPC_DATA_CHUNK	256	/* connected 'D' frame size */
#define	TPC_DEFAULT_MTU	256

/* The front side taken when no configuration file exists or it opens no
 * listener: one binary tcp port, whose text port (X + 1) follows by
 * default, exactly as a single "listen tcp 127.0.0.1 8101" line would.  */
#define	TPC_DEFAULT_LISTEN_ADDR	"127.0.0.1"
#define	TPC_DEFAULT_LISTEN_PORT	8101

/* One port entry learned from the 'G' reply: the flat port byte and the
 * upstream name.  The lowest channel of an upstream is its base; the
 * "name:chan" syntax adds the channel number to that base.  */
struct tpc_port {
	unsigned char	port;
	char		name[AGWPE_UPSTREAM_NAME_MAX];
};

struct tpc_listen {
	int		fd;
	int		unix_sock;
	int		text;		/* tcp text port */
	char		addr[64];	/* tcp bind address */
	int		port;		/* binary port */
	int		text_port;	/* text port, 0 = port + 1 */
	char		path[108];	/* unix socket path */
	int		group_mode;
	char		group_name[64];
};

struct tpc_client {
	int		fd;
	int		state;
	int		text;		/* line oriented endpoint */
	int		silent;
	int		keep;
	int		crlf;		/* client's terminator uses '\r' */
	int		dgram_tnc2;	/* datagram lines are TNC2 frames */
	unsigned char	pid;
	unsigned char	port;		/* resolved flat port */
	char		call_from[AGWPE_MAX_CALL];
	char		call_to[AGWPE_MAX_CALL];
	char		digis[AGWPE_MAX_DIGIS - 1][AGWPE_MAX_CALL];
	int		ndigis;

	unsigned char	*rbuf;		/* line accumulator */
	size_t		rlen, rcap;
	unsigned char	*dbuf;		/* pending packet data */
	size_t		dlen, dcap;
};

static struct {
	struct tpc_listen	listen[TPC_MAX_LISTEN];
	int			nlisten;
	int			mtu;
	char			default_call[AGWPE_MAX_CALL];

	int			target_tcp;	/* 0 = unix socket */
	int			target_set;	/* explicit 'target' line */
	char			target_host[64];
	int			target_port;
	char			target_sock[108];

	agwpe_client_t		*netd;

	struct tpc_client	clients[TPC_MAX_CLIENT];

	struct tpc_port		ports[AGWPE_PORT_MAX];
	int			nports;

	int			debug;
	int			foreground;
} tpc;

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static void tpc_log(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (tpc.foreground || tpc.debug) {
		fprintf(stderr, "ax25tcpd: ");
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	} else {
		vsyslog(prio, fmt, ap);
	}
	va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static void tpc_upper(char *s)
{
	for (; *s; s++)
		*s = toupper((unsigned char)*s);
}

/* Resolve <port>[:<chan>] into a flat port byte.  Names come from the
 * 'G' reply; "loop" and 255 are the virtual loopback port.  */
static int tpc_parse_port_spec(const char *spec, unsigned char *port)
{
	const char *chan = strchr(spec, ':');
	char num[32];
	size_t len;
	int n = 0, p;

	if (chan != NULL) {
		n = atoi(chan + 1);
		if (n < 0 || n > 15)
			return -1;
		len = chan - spec;
	} else {
		len = strlen(spec);
	}
	if (len == 0 || len >= sizeof(num))
		return -1;
	memcpy(num, spec, len);
	num[len] = '\0';

	if (strcasecmp(num, "loop") == 0) {
		*port = AGWPE_PORT_LOOP;
		return 0;
	}
	for (p = 0; p < tpc.nports; p++)
		if (strcmp(tpc.ports[p].name, num) == 0) {
			*port = tpc.ports[p].port + n;
			return 0;
		}
	for (p = 0; num[p]; p++)
		if (!isdigit((unsigned char)num[p]))
			return -1;
	n += atoi(num);
	if (n < 0 || n > 255)
		return -1;
	*port = n;
	return 0;
}

static int tpc_parse_pid(const char *spec, unsigned char *pid)
{
	if (strcasecmp(spec, "text") == 0 || strcasecmp(spec, "ax25") == 0)
		*pid = AGWPE_PID_AX25;
	else if (strcasecmp(spec, "netrom") == 0)
		*pid = AGWPE_PID_NETROM;
	else if (strcasecmp(spec, "rose") == 0)
		*pid = AGWPE_PID_ROSE;
	else if (strcasecmp(spec, "flexnet") == 0 ||
		 strcasecmp(spec, "texnet") == 0)
		*pid = AGWPE_PID_TEXNET;
	else if (strcasecmp(spec, "l3") == 0)
		*pid = AGWPE_PID_L3;
	else {
		char *end;
		long v;

		errno = 0;
		v = strtol(spec, &end, 0);
		if (errno != 0 || *end != '\0' || v < 0 || v > 255)
			return -1;
		*pid = (unsigned char)v;
	}
	return 0;
}

static void tpc_parse_group(const char *arg, int *mode, char *name, size_t nlen)
{
	if (strcasecmp(arg, "all") == 0)
		*mode = AGWPE_GROUP_ALL;
	else if (strcasecmp(arg, "default") == 0)
		*mode = AGWPE_GROUP_DEFAULT;
	else {
		*mode = AGWPE_GROUP_NAMED;
		strncpy(name, arg, nlen - 1);
		name[nlen - 1] = '\0';
	}
}

static int tpc_read_config(const char *path)
{
	FILE *fp;
	char line[512];
	int lineno = 0;

	fp = fopen(path, "r");
	if (fp == NULL) {
		/* No configuration file is not an error: the defaults
		 * apply (target from ax25common.conf, one binary tcp
		 * front port, mtu 256).  Any other open failure is.  */
		if (errno == ENOENT)
			return 0;
		return -1;
	}

	while (fgets(line, sizeof(line), fp) != NULL) {
		char *p, *key, *tok[4];
		int ntok = 0;

		lineno++;
		p = line;
		while (*p && isspace((unsigned char)*p))
			p++;
		if (*p == '\0' || *p == '#')
			continue;
		key = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		if (*p)
			*p++ = '\0';
		while (ntok < 4) {
			while (*p && isspace((unsigned char)*p))
				p++;
			if (*p == '\0' || *p == '#')
				break;
			tok[ntok] = p;
			while (*p && !isspace((unsigned char)*p))
				p++;
			if (*p)
				*p++ = '\0';
			ntok++;
		}

		if (strcmp(key, "target") == 0) {
			tpc.target_set = 1;
			if (ntok < 2) {
				tpc_log(LOG_ERR, "%s:%d: target needs two arguments",
					path, lineno);
				fclose(fp);
				return -1;
			}
			if (strcmp(tok[0], "tcp") == 0) {
				tpc.target_tcp = 1;
				strncpy(tpc.target_host, tok[1], sizeof(tpc.target_host) - 1);
				tpc.target_host[sizeof(tpc.target_host) - 1] = '\0';
				if (ntok < 3) {
					tpc_log(LOG_ERR, "%s:%d: target tcp needs a port",
						path, lineno);
					fclose(fp);
					return -1;
				}
				tpc.target_port = atoi(tok[2]);
				if (tpc.target_port < 1 || tpc.target_port > 65535) {
					tpc_log(LOG_ERR, "%s:%d: bad target port %s",
						path, lineno, tok[2]);
					fclose(fp);
					return -1;
				}
			} else if (strcmp(tok[0], "socket") == 0) {
				tpc.target_tcp = 0;
				if (strlen(tok[1]) >= sizeof(tpc.target_sock)) {
					tpc_log(LOG_ERR, "%s:%d: target socket path too long",
						path, lineno);
					fclose(fp);
					return -1;
				}
				strncpy(tpc.target_sock, tok[1], sizeof(tpc.target_sock) - 1);
			} else {
				tpc_log(LOG_ERR, "%s:%d: unknown target transport %s",
					path, lineno, tok[0]);
				fclose(fp);
				return -1;
			}
		} else if (strcmp(key, "listen") == 0) {
			struct tpc_listen *l;

			if (ntok < 2) {
				tpc_log(LOG_ERR, "%s:%d: listen needs a transport",
					path, lineno);
				fclose(fp);
				return -1;
			}
			if (tpc.nlisten >= TPC_MAX_LISTEN) {
				tpc_log(LOG_ERR, "%s:%d: too many listen lines",
					path, lineno);
				fclose(fp);
				return -1;
			}
			l = &tpc.listen[tpc.nlisten++];
			memset(l, 0, sizeof(*l));
			if (strcmp(tok[0], "tcp") == 0) {
				if (ntok < 3) {
					tpc_log(LOG_ERR, "%s:%d: listen tcp needs an address and a port",
						path, lineno);
					fclose(fp);
					return -1;
				}
				strncpy(l->addr, tok[1], sizeof(l->addr) - 1);
				l->addr[sizeof(l->addr) - 1] = '\0';
				l->port = atoi(tok[2]);
				if (l->port < 1 || l->port > 65535) {
					tpc_log(LOG_ERR, "%s:%d: bad listen port %s",
						path, lineno, tok[2]);
					fclose(fp);
					return -1;
				}
				if (ntok >= 4) {
					l->text_port = atoi(tok[3]);
					if (l->text_port < 1 || l->text_port > 65535) {
						tpc_log(LOG_ERR, "%s:%d: bad text port %s",
							path, lineno, tok[3]);
						fclose(fp);
						return -1;
					}
				} else {
					l->text_port = (l->port == 65535) ? 0 : l->port + 1;
				}
			} else if (strcmp(tok[0], "unix") == 0) {
				l->unix_sock = 1;
				l->text = 1;
				if (strlen(tok[1]) >= sizeof(l->path)) {
					tpc_log(LOG_ERR, "%s:%d: listen unix path too long",
						path, lineno);
					fclose(fp);
					return -1;
				}
				strncpy(l->path, tok[1], sizeof(l->path) - 1);
				if (ntok >= 4 && strcmp(tok[2], "group") == 0)
					tpc_parse_group(tok[3], &l->group_mode,
							l->group_name,
							sizeof(l->group_name));
			} else {
				tpc_log(LOG_ERR, "%s:%d: unknown listen transport %s",
					path, lineno, tok[0]);
				fclose(fp);
				return -1;
			}
		} else if (strcmp(key, "mtu") == 0) {
			if (ntok < 1) {
				tpc_log(LOG_ERR, "%s:%d: mtu needs a value",
					path, lineno);
				fclose(fp);
				return -1;
			}
			tpc.mtu = atoi(tok[0]);
			if (tpc.mtu < 16 || tpc.mtu > 4096) {
				tpc_log(LOG_ERR, "%s:%d: bad mtu %s",
					path, lineno, tok[0]);
				fclose(fp);
				return -1;
			}
		} else if (strcmp(key, "default-call") == 0) {
			if (ntok < 1) {
				tpc_log(LOG_ERR, "%s:%d: default-call needs a call",
					path, lineno);
				fclose(fp);
				return -1;
			}
			strncpy(tpc.default_call, tok[0], sizeof(tpc.default_call) - 1);
			tpc.default_call[sizeof(tpc.default_call) - 1] = '\0';
			tpc_upper(tpc.default_call);
		} else {
			tpc_log(LOG_ERR, "%s:%d: unknown directive %s",
				path, lineno, key);
			fclose(fp);
			return -1;
		}
	}
	fclose(fp);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Listeners                                                           */
/* ------------------------------------------------------------------ */

static int tpc_mkdir_parent(const char *path)
{
	char *dup, *slash;
	struct stat st;

	dup = strdup(path);
	if (dup == NULL)
		return -1;
	slash = strrchr(dup, '/');
	if (slash == NULL)
		strcpy(dup, ".");
	else if (slash == dup)
		strcpy(dup, "/");
	else
		*slash = '\0';

	if (stat(dup, &st) == 0) {
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
	if (mkdir(dup, 0750) != 0) {
		free(dup);
		return -1;
	}
	free(dup);
	return 0;
}

static int tpc_listen_tcp(const char *addr, int port)
{
	struct addrinfo hints, *res, *rp;
	char service[16];
	int s = -1;
	int on = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	snprintf(service, sizeof(service), "%d", port);

	if (getaddrinfo(addr, service, &hints, &res) != 0)
		return -1;
	for (rp = res; rp != NULL; rp = rp->ai_next) {
		s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (s < 0)
			continue;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		if (bind(s, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(s);
		s = -1;
	}
	freeaddrinfo(res);
	if (s < 0)
		return -1;
	if (listen(s, 16) != 0) {
		close(s);
		return -1;
	}
	fcntl(s, F_SETFL, O_NONBLOCK);
	tpc_log(LOG_INFO, "listening on tcp %s:%d", addr, port);
	return s;
}

static int tpc_listen_unix(const char *path, int group_mode,
			   const char *group_name)
{
	struct sockaddr_un sa;
	struct stat st;
	gid_t gid = getgid();
	mode_t sock_mode = 0660;
	int s;

	if (strlen(path) >= sizeof(sa.sun_path)) {
		tpc_log(LOG_ERR, "unix socket path too long: %s", path);
		return -1;
	}

	if (group_mode == AGWPE_GROUP_ALL) {
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
				tpc_log(LOG_ERR, "no such group '%s' for the unix socket",
					group_name);
				return -1;
			}
			gid = gr->gr_gid;
		}
	}

	if (tpc_mkdir_parent(path) != 0) {
		tpc_log(LOG_ERR, "cannot create directory for %s: %s",
			path, strerror(errno));
		return -1;
	}

	/* A stale socket from a previous run blocks the bind.  Only
	 * remove it when it really is a socket, never another file.  */
	if (lstat(path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			tpc_log(LOG_ERR,
				"%s exists and is not a socket; not removing it",
				path);
			return -1;
		}
		if (unlink(path) != 0) {
			tpc_log(LOG_ERR, "cannot remove stale socket %s: %s",
				path, strerror(errno));
			return -1;
		}
	} else if (errno != ENOENT) {
		tpc_log(LOG_ERR, "cannot inspect %s: %s", path, strerror(errno));
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) {
		tpc_log(LOG_ERR, "unix socket: %s", strerror(errno));
		return -1;
	}
	if (bind(s, (struct sockaddr *)&sa, SUN_LEN(&sa)) != 0 ||
	    listen(s, 16) != 0) {
		tpc_log(LOG_ERR, "cannot listen on %s: %s", path,
			strerror(errno));
		close(s);
		return -1;
	}
	/* Set the permissions through the path: macOS does not allow
	 * fchmod(2)/fchown(2) on socket descriptors, while chmod(2) on
	 * the path works everywhere.  */
	if (chmod(path, sock_mode) != 0 ||
	    (geteuid() == 0 && chown(path, getuid(), gid) != 0)) {
		tpc_log(LOG_ERR, "cannot set ownership of %s: %s",
			path, strerror(errno));
		close(s);
		unlink(path);
		return -1;
	}
	fcntl(s, F_SETFL, O_NONBLOCK);
	tpc_log(LOG_INFO, "listening on unix socket %s", path);
	return s;
}

/* ------------------------------------------------------------------ */
/* Clients                                                             */
/* ------------------------------------------------------------------ */

static void tpc_client_remove(struct tpc_client *cl)
{
	if (cl->fd >= 0)
		close(cl->fd);
	cl->fd = -1;
	free(cl->rbuf);
	cl->rbuf = NULL;
	free(cl->dbuf);
	cl->dbuf = NULL;
	cl->rlen = cl->dlen = 0;
}

static struct tpc_client *tpc_client_add(int fd, int text)
{
	struct tpc_client *cl;

	for (cl = tpc.clients; cl < tpc.clients + TPC_MAX_CLIENT; cl++)
		if (cl->fd < 0)
			break;
	if (cl == tpc.clients + TPC_MAX_CLIENT)
		return NULL;
	memset(cl, 0, sizeof(*cl));
	cl->fd = fd;
	cl->state = TPC_CMD;
	cl->text = text;
	cl->pid = AGWPE_PID_AX25;
	cl->silent = 0;
	cl->keep = 0;
	return cl;
}

static int tpc_write_all(int fd, const unsigned char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n;

		n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += n;
	}
	return 0;
}

static void tpc_client_printf(struct tpc_client *cl, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0 || cl->fd < 0)
		return;
	if (!cl->crlf) {
		/* The client ends its lines with bare LF: answer with bare
		 * LF too, i.e. drop the '\r' of every CRLF in the message.  */
		char out[sizeof(buf)];
		char *d = out;
		int i;

		for (i = 0; i < n; i++) {
			if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n')
				continue;
			*d++ = buf[i];
		}
		tpc_write_all(cl->fd, (unsigned char *)out, (size_t)(d - out));
	} else {
		tpc_write_all(cl->fd, (unsigned char *)buf, (size_t)n);
	}
}

static void tpc_prompt(struct tpc_client *cl)
{
	tpc_client_printf(cl, "> ");
}

/* ------------------------------------------------------------------ */
/* Port table                                                          */
/* ------------------------------------------------------------------ */

/* Parse a 'G' reply: "count;PortN name: desc;..." plus a trailing NUL,
 * exactly as ax25netd and Direwolf build it.  */
static void tpc_port_parse(const unsigned char *data, size_t len)
{
	char buf[4096];
	char *tok, *save = NULL;
	int first = 1;

	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	memcpy(buf, data, len);
	buf[len] = '\0';

	tpc.nports = 0;
	tok = strtok_r(buf, ";", &save);
	while (tok != NULL) {
		char *p, *q;
		int n;

		if (first) {
			first = 0;		/* the count */
		} else if (strncmp(tok, "Port", 4) == 0) {
			p = tok + 4;
			n = (int)strtol(p, &q, 10);
			if (q != p && *q == ' ' && n >= 1 && n <= AGWPE_PORT_LOOP + 1) {
				p = q + 1;
				q = strchr(p, ':');
				if (q != NULL && q != p) {
					*q = '\0';
					if (tpc.nports < AGWPE_PORT_MAX &&
					    strlen(p) < sizeof(tpc.ports[0].name)) {
						tpc.ports[tpc.nports].port =
							(unsigned char)(n - 1);
						strcpy(tpc.ports[tpc.nports].name, p);
						tpc.nports++;
					}
				}
			}
		}
		tok = strtok_r(NULL, ";", &save);
	}
}

/* ------------------------------------------------------------------ */
/* netd callbacks                                                      */
/* ------------------------------------------------------------------ */

static struct tpc_client *tpc_client_find(const struct agwpe_s *hdr,
					  int want_connecting)
{
	struct tpc_client *cl;

	for (cl = tpc.clients; cl < tpc.clients + TPC_MAX_CLIENT; cl++) {
		if (cl->fd < 0 || cl->state == TPC_CMD)
			continue;
		if (cl->state == TPC_CONNECTING != want_connecting)
			continue;
		if (cl->port != hdr->port)
			continue;
		if (strcmp(cl->call_from, hdr->call_to) != 0)
			continue;
		return cl;
	}
	return NULL;
}

static void tpc_on_raw_frame(agwpe_client_t *c, const struct agwpe_s *hdr,
			     const unsigned char *data, size_t len)
{
	(void)c;
	if (hdr->datakind == AGWPE_DK_PORTS)
		tpc_port_parse(data, len);
}

static void tpc_strip_eol(char *m)
{
	size_t l = strlen(m);

	if (l > 0 && m[l - 1] == '\n')
		m[--l] = '\0';
	if (l > 0 && m[l - 1] == '\r')
		m[--l] = '\0';
}

static void tpc_on_connection(agwpe_client_t *c, const struct agwpe_s *hdr,
			      const char *msg)
{
	struct tpc_client *cl = tpc_client_find(hdr, 1);
	char m[128];

	(void)c;
	if (cl == NULL)
		return;
	if (!cl->silent) {
		if (msg != NULL && msg[0] != '\0') {
			strncpy(m, msg, sizeof(m) - 1);
			m[sizeof(m) - 1] = '\0';
			tpc_strip_eol(m);
			tpc_client_printf(cl, "%s\r\n", m);
		} else {
			tpc_client_printf(cl, "*** CONNECTED to %s\r\n",
					  cl->call_to);
		}
	}
	cl->state = TPC_DATA;

	/* Flush anything that arrived while connecting.  */
	if (cl->dlen > 0) {
		agwpe_client_send_data(tpc.netd, cl->port, cl->pid,
				       cl->call_from, cl->call_to,
				       cl->dbuf, cl->dlen);
		cl->dlen = 0;
	}
}

static void tpc_on_disconnect(agwpe_client_t *c, const struct agwpe_s *hdr,
			      const char *msg)
{
	struct tpc_client *cl = tpc_client_find(hdr, 0);
	char m[128];

	(void)c;
	if (cl == NULL || cl->state == TPC_DGRAM)
		return;
	if (msg != NULL && msg[0] != '\0') {
		strncpy(m, msg, sizeof(m) - 1);
		m[sizeof(m) - 1] = '\0';
		tpc_strip_eol(m);
	} else {
		m[0] = '\0';
	}
	if (!cl->silent) {
		if (m[0] != '\0')
			tpc_client_printf(cl, "%s\r\n", m);
		else
			tpc_client_printf(cl, "*** DISCONNECTED\r\n");
	}
	if (cl->keep) {
		cl->state = TPC_CMD;
		tpc_prompt(cl);
	} else {
		tpc_client_remove(cl);
	}
}

static void tpc_on_data(agwpe_client_t *c, const struct agwpe_s *hdr,
			const unsigned char *data, size_t len)
{
	struct tpc_client *cl = tpc_client_find(hdr, 0);

	(void)c;
	if (cl == NULL || cl->state != TPC_DATA)
		return;
	if (tpc_write_all(cl->fd, data, len) != 0)
		tpc_client_remove(cl);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void tpc_send_unproto(struct tpc_client *cl, const unsigned char *data,
			     size_t len)
{
	const char *digv[AGWPE_MAX_DIGIS - 1];
	int i;

	for (i = 0; i < cl->ndigis; i++)
		digv[i] = cl->digis[i];

	if (cl->ndigis > 0)
		agwpe_client_send_unproto_via(tpc.netd, cl->port, cl->pid,
					      cl->call_from, cl->call_to,
					      digv, cl->ndigis, data, len);
	else
		agwpe_client_send_unproto(tpc.netd, cl->port, cl->pid,
					  cl->call_from, cl->call_to,
					  data, len);
}

/* One datagram line is a whole TNC2 frame: "SRC>DEST,DIGI,...:payload".
 * The source and the path come out of the data, not out of the command;
 * a trailing '*' on a path element marks it as already-repeated and is
 * dropped.  A malformed line is always worth saying, even under
 * --silent; an empty payload is simply not a frame and goes quietly.  */
static void tpc_send_unproto_tnc2(struct tpc_client *cl, char *line)
{
	const char *digv[AGWPE_MAX_DIGIS - 1];
	char srcbuf[AGWPE_MAX_CALL], destbuf[AGWPE_MAX_CALL];
	char digibuf[AGWPE_MAX_DIGIS - 1][AGWPE_MAX_CALL];
	char *payload, *path, *digi, *star;
	int ndigis = 0, first = 1, i;

	while (isspace((unsigned char)*line))
		line++;
	if (*line == '\0')
		return;

	payload = strchr(line, ':');
	if (payload == NULL) {
		tpc_client_printf(cl, "*** ERROR: no colon in \"%s\"\r\n", line);
		return;
	}
	*payload++ = '\0';
	if (*payload == '\0')
		return;			/* no payload is no frame */

	path = strchr(line, '>');
	if (path == NULL) {
		tpc_client_printf(cl, "*** ERROR: no '>' in the header\r\n");
		return;
	}
	*path++ = '\0';
	if (strlen(line) >= sizeof(srcbuf)) {
		tpc_client_printf(cl, "*** ERROR: bad source call\r\n");
		return;
	}
	strcpy(srcbuf, line);
	tpc_upper(srcbuf);

	destbuf[0] = '\0';
	for (digi = strtok(path, ","); digi != NULL;
	     digi = strtok(NULL, ",")) {
		star = strchr(digi, '*');
		if (star != NULL)
			*star = '\0';
		if (first) {
			first = 0;
			if (strlen(digi) >= sizeof(destbuf)) {
				tpc_client_printf(cl, "*** ERROR: bad destination call\r\n");
				return;
			}
			strcpy(destbuf, digi);
		} else if (ndigis < AGWPE_MAX_DIGIS - 1) {
			strncpy(digibuf[ndigis], digi,
				sizeof(digibuf[0]) - 1);
			digibuf[ndigis][sizeof(digibuf[0]) - 1] = '\0';
			ndigis++;
		} else {
			tpc_client_printf(cl, "*** ERROR: too many digipeaters\r\n");
			return;
		}
	}
	if (first || destbuf[0] == '\0') {
		tpc_client_printf(cl, "*** ERROR: no destination\r\n");
		return;
	}
	tpc_upper(destbuf);
	for (i = 0; i < ndigis; i++)
		tpc_upper(digibuf[i]);

	for (i = 0; i < ndigis; i++)
		digv[i] = digibuf[i];

	if (ndigis > 0)
		agwpe_client_send_unproto_via(tpc.netd, cl->port, cl->pid,
					      srcbuf, destbuf, digv, ndigis,
					      (const unsigned char *)payload,
					      strlen(payload));
	else
		agwpe_client_send_unproto(tpc.netd, cl->port, cl->pid,
					  srcbuf, destbuf,
					  (const unsigned char *)payload,
					  strlen(payload));
}

static void tpc_dgram_flush(struct tpc_client *cl)
{
	size_t off = 0;

	while (off < cl->dlen) {
		size_t n = cl->dlen - off;

		if (n > (size_t)tpc.mtu)
			n = (size_t)tpc.mtu;
		tpc_send_unproto(cl, cl->dbuf + off, n);
		off += n;
	}
	cl->dlen = 0;
}

static int tpc_add_call(struct tpc_client *cl, const char *call)
{
	if (cl->call_to[0] == '\0') {
		if (strlen(call) >= sizeof(cl->call_to))
			return -1;
		strcpy(cl->call_to, call);
		return 0;
	}
	if (cl->ndigis >= AGWPE_MAX_DIGIS - 1)
		return -1;
	strncpy(cl->digis[cl->ndigis], call, sizeof(cl->digis[0]) - 1);
	cl->digis[cl->ndigis][sizeof(cl->digis[0]) - 1] = '\0';
	cl->ndigis++;
	return 0;
}

/* The destination and the digis, split as "DB0AAA-8 DB0BBB DB0CCC" or
 * "DB0AAA-8,DB0BBB,DB0CCC" or a mixture - commas separate a path exactly
 * as spaces do, and each word may itself carry commas.  spec[0] is the
 * destination, the rest are digipeaters.  */
static int tpc_parse_dest(struct tpc_client *cl, char *const *spec, int nspec)
{
	char buf[256], *p, *q;
	int i;

	cl->call_to[0] = '\0';
	cl->ndigis = 0;
	for (i = 0; i < nspec; i++) {
		if (strlen(spec[i]) >= sizeof(buf))
			return -1;
		strcpy(buf, spec[i]);
		p = buf;
		while ((q = strchr(p, ',')) != NULL) {
			*q = '\0';
			if (tpc_add_call(cl, p) != 0)
				return -1;
			p = q + 1;
		}
		if (*p != '\0' && tpc_add_call(cl, p) != 0)
			return -1;
	}
	return (cl->call_to[0] != '\0') ? 0 : -1;
}

static int tpc_cmd_connect(struct tpc_client *cl, int argc, char **argv)
{
	char *port_spec = NULL, *dest = NULL, *src = NULL;
	char *pos[8];
	int npos = 0, i, silent = 0, keep = 0, port_given = 0;
	const char *usage = "usage: connect [--silent] [--keep] [--pid=XX] [--port <port>] [--mycall <call>] <port[:chan]> <dest>[,<digi>,...] [< SRC]";

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--silent") == 0) {
			silent = 1;
		} else if (strcmp(argv[i], "--keep") == 0) {
			keep = 1;
		} else if (strncmp(argv[i], "--pid=", 6) == 0) {
			if (tpc_parse_pid(argv[i] + 6, &cl->pid) != 0) {
				tpc_client_printf(cl, "*** ERROR: bad pid '%s'\r\n",
						  argv[i] + 6);
				return 0;
			}
		} else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			port_spec = argv[++i];
			port_given = 1;
		} else if (strcmp(argv[i], "--mycall") == 0) {
			if (src == NULL && i + 1 < argc)
				src = argv[++i];
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (strncmp(argv[i], "--mycall=", 9) == 0) {
			if (src == NULL && argv[i][9] != '\0')
				src = argv[i] + 9;
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (argv[i][0] == '-' && argv[i][1] == '-') {
			tpc_client_printf(cl, "*** ERROR: unknown option '%s'\r\n",
					  argv[i]);
			return 0;
		} else if (strcmp(argv[i], "<") == 0) {
			if (src == NULL && i + 1 < argc)
				src = argv[++i];
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (npos < 8) {
			pos[npos++] = argv[i];
		} else {
			tpc_client_printf(cl, "*** ERROR: too many arguments\r\n");
			return 0;
		}
	}

	if (port_given) {
		if (npos < 1) {
			tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
			return 0;
		}
		dest = pos[0];
	} else {
		if (npos < 2) {
			tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
			return 0;
		}
		port_spec = pos[0];
		dest = pos[1];
	}

	if (cl->state != TPC_CMD) {
		tpc_client_printf(cl, "*** ERROR: already busy\r\n");
		return 0;
	}
	if (tpc_parse_port_spec(port_spec, &cl->port) != 0) {
		tpc_client_printf(cl, "*** ERROR: no such port '%s'\r\n", port_spec);
		return 0;
	}
	if (tpc_parse_dest(cl, pos + (port_given ? 0 : 1),
			   npos - (port_given ? 0 : 1)) != 0) {
		tpc_client_printf(cl, "*** ERROR: bad destination '%s'\r\n", dest);
		return 0;
	}
	tpc_upper(cl->call_to);
	for (i = 0; i < cl->ndigis; i++)
		tpc_upper(cl->digis[i]);

	if (src != NULL) {
		if (strlen(src) >= sizeof(cl->call_from)) {
			tpc_client_printf(cl, "*** ERROR: bad source call '%s'\r\n", src);
			return 0;
		}
		strcpy(cl->call_from, src);
		tpc_upper(cl->call_from);
	} else if (tpc.default_call[0] != '\0') {
		strcpy(cl->call_from, tpc.default_call);
	} else {
		tpc_client_printf(cl, "*** ERROR: source call required, use ' < call'\r\n");
		return 0;
	}

	cl->silent = silent;
	cl->keep = keep;

	/* Register our own call on the target port so the netd routes
	 * the connect confirm and later frames back to us.  */
	agwpe_client_register(tpc.netd, cl->port, cl->call_from);
	if (cl->ndigis > 0) {
		const char *digv[AGWPE_MAX_DIGIS - 1];

		for (i = 0; i < cl->ndigis; i++)
			digv[i] = cl->digis[i];
		agwpe_client_connect_via(tpc.netd, cl->port, cl->pid,
					 cl->call_from, cl->call_to,
					 digv, cl->ndigis);
	} else {
		agwpe_client_connect(tpc.netd, cl->port, cl->pid,
				     cl->call_from, cl->call_to);
	}
	cl->state = TPC_CONNECTING;
	if (!silent)
		tpc_client_printf(cl, "*** CONNECTING to %s\r\n", cl->call_to);
	return 0;
}

static int tpc_cmd_datagram(struct tpc_client *cl, int argc, char **argv)
{
	char *port_spec = NULL, *dest = NULL, *src = NULL;
	char *pos[8];
	int npos = 0, i, silent = 0, keep = 0, port_given = 0;
	const char *usage = "usage: datagram [--silent] [--keep] [--pid=XX] [--port <port>] [--mycall <call>] <port[:chan]> [<dest>[,<digi>,...]] [< SRC]";

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--silent") == 0) {
			silent = 1;
		} else if (strcmp(argv[i], "--keep") == 0) {
			keep = 1;
		} else if (strncmp(argv[i], "--pid=", 6) == 0) {
			if (tpc_parse_pid(argv[i] + 6, &cl->pid) != 0) {
				tpc_client_printf(cl, "*** ERROR: bad pid '%s'\r\n",
						  argv[i] + 6);
				return 0;
			}
		} else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			port_spec = argv[++i];
			port_given = 1;
		} else if (strcmp(argv[i], "--mycall") == 0) {
			if (src == NULL && i + 1 < argc)
				src = argv[++i];
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (strncmp(argv[i], "--mycall=", 9) == 0) {
			if (src == NULL && argv[i][9] != '\0')
				src = argv[i] + 9;
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (argv[i][0] == '-' && argv[i][1] == '-') {
			tpc_client_printf(cl, "*** ERROR: unknown option '%s'\r\n",
					  argv[i]);
			return 0;
		} else if (strcmp(argv[i], "<") == 0) {
			if (src == NULL && i + 1 < argc)
				src = argv[++i];
			else {
				tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
				return 0;
			}
		} else if (npos < 8) {
			pos[npos++] = argv[i];
		} else {
			tpc_client_printf(cl, "*** ERROR: too many arguments\r\n");
			return 0;
		}
	}

	if (cl->state != TPC_CMD) {
		tpc_client_printf(cl, "*** ERROR: already busy\r\n");
		return 0;
	}

	if (port_given) {
		if (npos > 0)
			dest = pos[0];
	} else {
		if (npos < 1) {
			tpc_client_printf(cl, "*** ERROR: %s\r\n", usage);
			return 0;
		}
		port_spec = pos[0];
		if (npos >= 2)
			dest = pos[1];
	}

	if (tpc_parse_port_spec(port_spec, &cl->port) != 0) {
		tpc_client_printf(cl, "*** ERROR: no such port '%s'\r\n", port_spec);
		return 0;
	}

	cl->silent = silent;
	cl->keep = keep;
	cl->dgram_tnc2 = (dest == NULL);

	if (dest != NULL) {
		if (tpc_parse_dest(cl, pos + (port_given ? 0 : 1),
				   npos - (port_given ? 0 : 1)) != 0) {
			tpc_client_printf(cl, "*** ERROR: bad destination '%s'\r\n", dest);
			return 0;
		}
		tpc_upper(cl->call_to);
		for (i = 0; i < cl->ndigis; i++)
			tpc_upper(cl->digis[i]);

		if (src != NULL) {
			if (strlen(src) >= sizeof(cl->call_from)) {
				tpc_client_printf(cl, "*** ERROR: bad source call '%s'\r\n", src);
				return 0;
			}
			strcpy(cl->call_from, src);
			tpc_upper(cl->call_from);
		} else if (tpc.default_call[0] != '\0') {
			strcpy(cl->call_from, tpc.default_call);
		} else {
			tpc_client_printf(cl, "*** ERROR: source call required, use ' < call'\r\n");
			return 0;
		}
	}

	cl->state = TPC_DGRAM;
	return 0;
}

static int tpc_cmd_quit(struct tpc_client *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	tpc_client_printf(cl, "bye\r\n");
	tpc_client_remove(cl);
	return 1;		/* caller must not touch cl again */
}

static int tpc_cmd_help(struct tpc_client *cl, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	tpc_client_printf(cl,
		"commands: connect, datagram, quit, bye, help\r\n"
		"connect <port[:chan]> <dest>[,<digi>,...] [< SRC]\r\n"
		"datagram <port[:chan]> [<dest>[,<digi>,...]] [< SRC]\r\n"
		"    no <dest>: each line is a TNC2 frame SRC>DEST,path:payload\r\n"
		"options: --silent --keep --pid=XX --port <port> --mycall <call>\r\n");
	return 0;
}

static const struct tpc_cmd {
	const char	*name;
	int		(*fn)(struct tpc_client *, int, char **);
} tpc_commands[] = {
	{ "connect",	tpc_cmd_connect },
	{ "datagram",	tpc_cmd_datagram },
	{ "quit",	tpc_cmd_quit },
	{ "bye",	tpc_cmd_quit },
	{ "help",	tpc_cmd_help },
};

static int tpc_command_run(struct tpc_client *cl, char *line)
{
	char *argv[24];
	int argc = 0, i, match = 0;
	const struct tpc_cmd *cmdp = NULL;

	while (argc < 24) {
		while (*line && isspace((unsigned char)*line))
			line++;
		if (*line == '\0')
			break;
		argv[argc++] = line;
		while (*line && !isspace((unsigned char)*line))
			line++;
		if (*line)
			*line++ = '\0';
	}
	if (argc == 0) {
		tpc_prompt(cl);
		return 0;
	}

	for (i = 0; i < (int)(sizeof(tpc_commands) / sizeof(tpc_commands[0])); i++)
		if (strncmp(tpc_commands[i].name, argv[0],
			    strlen(argv[0])) == 0) {
			match++;
			cmdp = &tpc_commands[i];
		}

	if (match == 0) {
		tpc_client_printf(cl, "*** ERROR: unknown command '%s' (help for a list)\r\n",
				  argv[0]);
	} else if (match > 1) {
		tpc_client_printf(cl, "*** ERROR: ambiguous command '%s'\r\n", argv[0]);
	} else {
		int r = cmdp->fn(cl, argc - 1, argv + 1);

		if (r)
			return 1;	/* client gone */
	}
	if (cl->fd >= 0 && cl->state == TPC_CMD)
		tpc_prompt(cl);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Client input                                                        */
/* ------------------------------------------------------------------ */

/* Return the length of a complete line in buf, or -1 if a complete line
 * needs more data.  The line does not include the terminator; a
 * trailing '\r' of a CRLF pair is part of the line for the caller to
 * strip.  *consumed is the number of bytes to drop.  */
static ssize_t tpc_line_len(const unsigned char *buf, size_t len, size_t *consumed)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (buf[i] == '\n' || buf[i] == '\r') {
			if (buf[i] == '\r' && i + 1 == len)
				return -1;	/* wait for a following LF */
			*consumed = i + 1;
			if (buf[i] == '\r' && i + 1 < len && buf[i + 1] == '\n')
				(*consumed)++;
			return (ssize_t)i;
		}
	}
	return -1;
}

static int tpc_client_readable(struct tpc_client *cl)
{
	unsigned char tmp[4096];
	ssize_t n;

	for (;;) {
		n = read(cl->fd, tmp, sizeof(tmp));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			break;
		}
		if (n == 0)
			break;
		if (cl->state == TPC_DATA) {
			size_t off = 0;

			/* Byte stream: send each chunk as a 'D' frame.  */
			while (off < (size_t)n) {
				size_t c = (size_t)n - off;

				if (c > TPC_DATA_CHUNK)
					c = TPC_DATA_CHUNK;
				agwpe_client_send_data(tpc.netd, cl->port,
						       cl->pid, cl->call_from,
						       cl->call_to,
						       tmp + off, c);
				off += c;
			}
			continue;
		}
		if (cl->state == TPC_CONNECTING) {
			/* Hold until the connect is confirmed.  */
			if (cl->dlen + (size_t)n > cl->dcap) {
				size_t ncap = cl->dcap ? cl->dcap * 2 : 4096;

				while (cl->dlen + (size_t)n > ncap)
					ncap *= 2;
				cl->dbuf = realloc(cl->dbuf, ncap);
				if (cl->dbuf == NULL)
					break;
				cl->dcap = ncap;
			}
			memcpy(cl->dbuf + cl->dlen, tmp, (size_t)n);
			cl->dlen += (size_t)n;
			continue;
		}
		if (cl->state == TPC_DGRAM && !cl->text && !cl->dgram_tnc2) {
			/* Binary datagram: accumulate, flush on close.  */
			if (cl->dlen + (size_t)n > cl->dcap) {
				size_t ncap = cl->dcap ? cl->dcap * 2 : 4096;

				while (cl->dlen + (size_t)n > ncap)
					ncap *= 2;
				cl->dbuf = realloc(cl->dbuf, ncap);
				if (cl->dbuf == NULL)
					break;
				cl->dcap = ncap;
			}
			memcpy(cl->dbuf + cl->dlen, tmp, (size_t)n);
			cl->dlen += (size_t)n;
			continue;
		}

		/* CMD and text datagram: accumulate and take lines.  */
		if (cl->rlen + (size_t)n > TPC_LINE_MAX * 4) {
			/* Never lets a line grow past the limit.  */
			cl->rlen = 0;
			if (cl->state == TPC_CMD)
				tpc_client_printf(cl, "*** ERROR: line too long\r\n");
			continue;
		}
		if (cl->rlen + (size_t)n > cl->rcap) {
			size_t ncap = cl->rcap ? cl->rcap * 2 : 1024;

			while (cl->rlen + (size_t)n > ncap)
				ncap *= 2;
			if (ncap > TPC_LINE_MAX * 4)
				ncap = TPC_LINE_MAX * 4;
			cl->rbuf = realloc(cl->rbuf, ncap);
			if (cl->rbuf == NULL)
				break;
			cl->rcap = ncap;
		}
		memcpy(cl->rbuf + cl->rlen, tmp, (size_t)n);
		cl->rlen += (size_t)n;

		for (;;) {
			size_t consumed;
			ssize_t llen;
			char line[TPC_LINE_MAX + 1];

			llen = tpc_line_len(cl->rbuf, cl->rlen, &consumed);
			if (llen < 0)
				break;
			if (cl->rbuf[llen] == '\r')
				cl->crlf = 1;	/* answer in CRLF from here on */
			if ((size_t)llen >= sizeof(line)) {
				/* Overlong line: drop the whole thing.  */
				memmove(cl->rbuf, cl->rbuf + consumed + (size_t)llen,
					cl->rlen - (consumed + (size_t)llen));
				cl->rlen -= consumed + (size_t)llen;
				if (cl->state == TPC_CMD)
					tpc_client_printf(cl,
						"*** ERROR: line too long\r\n");
				continue;
			}
			memcpy(line, cl->rbuf, (size_t)llen);
			line[llen] = '\0';
			if (llen > 0 && line[llen - 1] == '\r')
				line[llen - 1] = '\0';
			memmove(cl->rbuf, cl->rbuf + consumed,
				cl->rlen - consumed);
			cl->rlen -= consumed;

			if (cl->state == TPC_CMD) {
				if (tpc_command_run(cl, line))
					return 1;	/* client gone */
				if (cl->fd < 0)
					return 1;
				/* The command may have switched to binary
				 * datagram mode; any bytes already read
				 * after the command line are the start of
				 * the datagram payload.  */
				if (cl->state == TPC_DGRAM && !cl->text &&
				    !cl->dgram_tnc2 && cl->rlen > 0) {
					if (cl->dlen + cl->rlen > cl->dcap) {
						size_t ncap = cl->dcap ?
							cl->dcap * 2 : 4096;

						while (cl->dlen + cl->rlen > ncap)
							ncap *= 2;
						cl->dbuf = realloc(cl->dbuf, ncap);
						if (cl->dbuf == NULL) {
							cl->rlen = 0;
							break;
						}
						cl->dcap = ncap;
					}
					memcpy(cl->dbuf + cl->dlen, cl->rbuf,
					       cl->rlen);
					cl->dlen += cl->rlen;
					cl->rlen = 0;
				}
			} else if (cl->state == TPC_DGRAM) {
				if (cl->dgram_tnc2) {
					tpc_send_unproto_tnc2(cl, line);
				} else if (line[0] != '\0') {
					tpc_send_unproto(cl, (unsigned char *)line,
							 strlen(line));
				} else if (!cl->silent) {
					tpc_client_printf(cl,
						"*** ERROR: empty packet\r\n");
				}
			}
		}
	}
	return 1;		/* EOF or fatal error */
}

static void tpc_client_gone(struct tpc_client *cl)
{
	if (cl->state == TPC_DATA || cl->state == TPC_CONNECTING)
		agwpe_client_disconnect(tpc.netd, cl->port,
					cl->call_from, cl->call_to);
	else if (cl->state == TPC_DGRAM && cl->dlen > 0)
		tpc_dgram_flush(cl);
	tpc_client_remove(cl);
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

static void tpc_accept(struct tpc_listen *l)
{
	int fd;

	for (;;) {
		fd = accept(l->fd, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				tpc_log(LOG_WARNING, "accept: %s", strerror(errno));
			return;
		}
		if (tpc_client_add(fd, l->text) == NULL) {
			static const unsigned char full[] = "*** ERROR: server full\r\n";

			tpc_write_all(fd, full, sizeof(full) - 1);
			close(fd);
		}
	}
}

static void tpc_loop(void)
{
	for (;;) {
		fd_set rfds;
		int maxfd = -1, nfds, i, netd_fd = agwpe_client_fd(tpc.netd);

		FD_ZERO(&rfds);
		for (i = 0; i < tpc.nlisten; i++) {
			FD_SET(tpc.listen[i].fd, &rfds);
			if (tpc.listen[i].fd > maxfd)
				maxfd = tpc.listen[i].fd;
		}
		FD_SET(netd_fd, &rfds);
		if (netd_fd > maxfd)
			maxfd = netd_fd;
		for (i = 0; i < TPC_MAX_CLIENT; i++)
			if (tpc.clients[i].fd >= 0) {
				FD_SET(tpc.clients[i].fd, &rfds);
				if (tpc.clients[i].fd > maxfd)
					maxfd = tpc.clients[i].fd;
			}

		nfds = select(maxfd + 1, &rfds, NULL, NULL, NULL);
		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			tpc_log(LOG_ERR, "select: %s", strerror(errno));
			return;
		}
		if (nfds == 0)
			continue;

		for (i = 0; i < tpc.nlisten; i++)
			if (FD_ISSET(tpc.listen[i].fd, &rfds))
				tpc_accept(&tpc.listen[i]);

		if (FD_ISSET(netd_fd, &rfds)) {
			if (agwpe_client_recv(tpc.netd) < 0) {
				tpc_log(LOG_ERR, "connection to the netd lost: %s",
					strerror(agwpe_client_err(tpc.netd)));
				return;
			}
		}

		for (i = 0; i < TPC_MAX_CLIENT; i++)
			if (tpc.clients[i].fd >= 0 &&
			    FD_ISSET(tpc.clients[i].fd, &rfds)) {
				struct tpc_client *cl = &tpc.clients[i];

				if (tpc_client_readable(cl))
					tpc_client_gone(cl);
			}
	}
}

/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

static void tpc_unlink_sockets(void)
{
	int i;

	for (i = 0; i < tpc.nlisten; i++)
		if (tpc.listen[i].unix_sock && tpc.listen[i].fd >= 0) {
			close(tpc.listen[i].fd);
			tpc.listen[i].fd = -1;
			unlink(tpc.listen[i].path);
		}
}

static void tpc_sigterm(int sig)
{
	(void)sig;
	tpc_unlink_sockets();
	_exit(0);
}

static int tpc_daemonize(void)
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

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-c file] [-C ax25common.conf] [-f] [-d]\n"
		"  -c file  configuration file (default %s)\n"
		"  -C file  shared ax25netd loop port configuration (default %s);\n"
		"           the back side of this frontend is taken from it\n"
		"  -f       stay in the foreground\n"
		"  -d       debug logging to stderr\n", prog,
		AX25_SYSCONFDIR "/ax25tcpd.conf",
		AX25_SYSCONFDIR "/ax25common.conf");
}

int main(int argc, char **argv)
{
	const struct agwpe_client_cb cb = {
		.raw_frame	= tpc_on_raw_frame,
		.connection	= tpc_on_connection,
		.disconnect	= tpc_on_disconnect,
		.data		= tpc_on_data,
	};
	const char *conf = AX25_SYSCONFDIR "/ax25tcpd.conf";
	const char *comconf = AX25_SYSCONFDIR "/ax25common.conf";
	int c, i, r;

	while ((c = getopt(argc, argv, "c:C:fdh")) != -1) {
		switch (c) {
		case 'c':
			conf = optarg;
			break;
		case 'C':
			comconf = optarg;
			break;
		case 'f':
			tpc.foreground = 1;
			break;
		case 'd':
			tpc.debug = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	tpc.mtu = TPC_DEFAULT_MTU;
	for (i = 0; i < TPC_MAX_CLIENT; i++)
		tpc.clients[i].fd = -1;

	if (tpc_read_config(conf) != 0) {
		tpc_log(LOG_ERR, "cannot read configuration %s: %s",
			conf, strerror(errno));
		return 1;
	}

	/* The back side is normally the shared ax25netd loop port from
	 * ax25common.conf, the file the daemon itself reads for its
	 * listener: one file, one semantics.  A 'target' line in this
	 * configuration overrides it for a netd on another host.  */
	if (!tpc.target_set) {
		struct ax25common com;

		if (ax25common_config_load(comconf, &com) < 0) {
			tpc_log(LOG_ERR, "cannot read %s", comconf);
			return 1;
		}
		if (com.loop_socket[0] != '\0') {
			tpc.target_tcp = 0;
			strncpy(tpc.target_sock, com.loop_socket,
				sizeof(tpc.target_sock) - 1);
		} else if (com.loop_tcp_enabled) {
			tpc.target_tcp = 1;
			strncpy(tpc.target_host, "127.0.0.1",
				sizeof(tpc.target_host) - 1);
			tpc.target_port = com.loop_tcp_port;
		}
	}
	if (!tpc.target_tcp && tpc.target_sock[0] == '\0') {
		tpc_log(LOG_ERR, "no target configured (ax25common.conf or target tcp|socket ...)");
		return 1;
	}
	if (tpc.nlisten == 0) {
		/* No listener configured: fall back to the default
		 * front side, one binary tcp port on 127.0.0.1:8101
		 * whose text port (8102) follows automatically.  */
		struct tpc_listen *l = &tpc.listen[tpc.nlisten++];

		memset(l, 0, sizeof(*l));
		strncpy(l->addr, TPC_DEFAULT_LISTEN_ADDR,
			sizeof(l->addr) - 1);
		l->port = TPC_DEFAULT_LISTEN_PORT;
		l->text_port = TPC_DEFAULT_LISTEN_PORT + 1;
	}

	tpc.netd = agwpe_client_new(&cb, NULL);
	if (tpc.netd == NULL) {
		tpc_log(LOG_ERR, "cannot allocate the netd client");
		return 1;
	}
	if (tpc.target_tcp)
		r = agwpe_client_connect_host(tpc.netd, tpc.target_host,
					      tpc.target_port);
	else
		r = agwpe_client_connect_unix(tpc.netd, tpc.target_sock);
	if (r < 0) {
		tpc_log(LOG_ERR, "cannot connect to %s: %s",
			tpc.target_tcp ? tpc.target_host : tpc.target_sock,
			strerror(agwpe_client_err(tpc.netd)));
		agwpe_client_free(tpc.netd);
		return 1;
	}

	for (i = 0; i < tpc.nlisten; i++) {
		struct tpc_listen *l = &tpc.listen[i];

		if (l->unix_sock)
			l->fd = tpc_listen_unix(l->path, l->group_mode,
						l->group_name);
		else
			l->fd = tpc_listen_tcp(l->addr, l->port);
		if (l->fd < 0) {
			tpc_log(LOG_ERR, "cannot listen: %s", strerror(errno));
			tpc_unlink_sockets();
			agwpe_client_free(tpc.netd);
			return 1;
		}
	}

	/* The text port (X+1) is one line above each binary tcp port.  */
	for (i = 0; i < tpc.nlisten; i++) {
		struct tpc_listen *l = &tpc.listen[i], *lt;
		int j;

		if (l->unix_sock || l->text_port <= 0)
			continue;
		for (j = 0; j < tpc.nlisten; j++)
			if (tpc.listen[j].text && !tpc.listen[j].unix_sock &&
			    strcmp(tpc.listen[j].addr, l->addr) == 0 &&
			    tpc.listen[j].port == l->text_port)
				break;		/* already there */
		if (j < tpc.nlisten)
			continue;
		if (tpc.nlisten >= TPC_MAX_LISTEN)
			break;
		lt = &tpc.listen[tpc.nlisten++];
		memset(lt, 0, sizeof(*lt));
		lt->text = 1;
		strncpy(lt->addr, l->addr, sizeof(lt->addr) - 1);
		lt->addr[sizeof(lt->addr) - 1] = '\0';
		lt->port = l->text_port;
		lt->fd = tpc_listen_tcp(lt->addr, lt->port);
		if (lt->fd < 0) {
			tpc_log(LOG_ERR, "cannot listen: %s", strerror(errno));
			tpc_unlink_sockets();
			agwpe_client_free(tpc.netd);
			return 1;
		}
	}

	agwpe_client_get_ports(tpc.netd);

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, tpc_sigterm);
	signal(SIGINT, tpc_sigterm);

	if (!tpc.foreground && tpc_daemonize() < 0) {
		tpc_log(LOG_ERR, "daemonize: %s", strerror(errno));
		tpc_unlink_sockets();
		agwpe_client_free(tpc.netd);
		return 1;
	}

	tpc_log(LOG_INFO, "ax25tcpd started");
	tpc_loop();
	tpc_unlink_sockets();
	agwpe_client_free(tpc.netd);
	return 0;
}

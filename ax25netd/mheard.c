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
 * Heard-station log (mheard.dat).
 *
 * On a native Linux AX.25 system mheardd(8) captures every frame on the
 * radio channels from the kernel packet socket and records who has been
 * heard.  ax25netd has no packet socket: the radio traffic it knows of
 * is what its loop clients send out and what the upstream AGWPE server
 * forwards back as raw ('K') frames.  This file maintains the same
 * mheard.dat list (fixed-size struct mheard_struct records, as
 * documented in netax25/mheard.h) from those two legs, so that
 * mheard(1) works on AGWPE-only systems too.
 *
 * The file format is append-only per entry: a station's record is
 * written at its own offset and new stations are appended.  A system
 * that runs the native kernel stack may have mheardd updating the same
 * file at the same time, so every write takes an exclusive flock() on
 * the file; both daemons use flock(), and the append-only layout keeps
 * each writer's recorded offsets valid.
 *
 * Writes are batched to protect flash storage: a station's record is
 * written at most every MHEARD_FLUSH_SECS, new stations on their first
 * hear.  ax25netd --no-mheard turns the list off altogether.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <syslog.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <netax25/ax25.h>
#include <netax25/axlib.h>
#include <netax25/mheard.h>

#include "../pathnames.h"

#include "ax25netd.h"

#define	AXLEN		7
#define	ALEN		6

#define	HDLCAEB		0x01
#define	SSSID_SPARE	0x40

#define	PID_TEXT	0xF0

#define	MHEARD_MAX	1000

/* Writes are batched: a station's record is written at most once every
 * MHEARD_FLUSH_SECS (new stations are written on their first hear).
 * Every frame would otherwise turn into a small file write, which wears
 * out flash storage on embedded systems.  */
#define	MHEARD_FLUSH_SECS	60

struct mheard_entry {
	int			in_use;
	struct mheard_struct entry;
	long			position;	/* 0xFFFFFF = append at end */
	time_t			last_flush;	/* last write to the file */
};

static struct mheard_entry mheard_list[MHEARD_MAX];

static int ftype(const unsigned char *data, int *type, int extseq)
{
	if (extseq) {
		if ((*data & 0x01) == 0) {	/* I frame */
			*type = MHEARD_TYPE_I;
			return 2;
		}
		if (*data & 0x02) {
			*type = *data & ~0x10;	/* U frame, strip P/F */
			return 1;
		} else {
			*type = *data;
			return 2;
		}
	} else {
		if ((*data & 0x01) == 0) {	/* I frame */
			*type = MHEARD_TYPE_I;
			return 1;
		}
		if (*data & 0x02) {		/* U frame, strip P/F */
			*type = *data & ~0x10;
			return 1;
		} else {			/* S frame */
			*type = *data & 0x0F;
			return 1;
		}
	}
}

/* Write one record, holding an exclusive flock for the whole update.
 * Mirrors mheardd(8) so that the two writers never tear each other's
 * records.  */
static void mheard_write(struct mheard_entry *m)
{
	FILE *fp;
	int fd;

	fp = fopen(DATA_MHEARD_FILE, "r+");
	if (fp == NULL)
		return;
	fd = fileno(fp);

	(void)flock(fd, LOCK_EX);

	if (m->position == 0xFFFFFF) {
		fseek(fp, 0L, SEEK_END);
		m->position = ftell(fp);
	}

	fseek(fp, m->position, SEEK_SET);
	fwrite(&m->entry, sizeof(struct mheard_struct), 1, fp);
	fflush(fp);

	(void)flock(fd, LOCK_UN);
	fclose(fp);
}

static struct mheard_entry *findentry(const ax25_address *call, const char *port)
{
	struct mheard_entry *oldest = NULL;
	int i;

	for (i = 0; i < MHEARD_MAX; i++) {
		if (mheard_list[i].in_use &&
		    ax25_cmp(&mheard_list[i].entry.from_call, call) == 0 &&
		    strcmp(mheard_list[i].entry.portname, port) == 0)
			return &mheard_list[i];
	}

	for (i = 0; i < MHEARD_MAX; i++) {
		if (!mheard_list[i].in_use) {
			mheard_list[i].in_use = 1;
			mheard_list[i].position = 0xFFFFFF;
			return &mheard_list[i];
		}
	}

	for (i = 0; i < MHEARD_MAX; i++) {
		if (mheard_list[i].in_use &&
		    (oldest == NULL ||
		     mheard_list[i].entry.last_heard <
		     oldest->entry.last_heard))
			oldest = &mheard_list[i];
	}

	memset(&oldest->entry, 0x00, sizeof(struct mheard_struct));
	oldest->position = 0xFFFFFF;

	return oldest;
}

/*
 * Update the heard list from one raw frame.  frame points at the KISS
 * data marker (as delivered by the upstream and as mux_mirror_raw()
 * builds for transmissions), len its length.  The parsing follows
 * mheardd(8) so the resulting records are identical.
 */
void ax25netd_mheard_frame(struct ax25netd_upstream *u, const unsigned char *frame,
		       size_t len)
{
	const unsigned char *data = frame;
	struct mheard_entry *m;
	size_t size = len;
	char port[20];
	time_t now;
	int ctlen, type, extseq, end, is_new;

	if (!ax25netd.mheard)
		return;

	if (frame == NULL || len < 1 + AXLEN + AXLEN + 1)
		return;

	if ((data[0] & 0x0F) != 0x00)		/* KISS data marker */
		return;
	data++;
	size--;

	strncpy(port, u->name, sizeof(port) - 1);
	port[sizeof(port) - 1] = '\0';

	m = findentry((const ax25_address *)(data + AXLEN), port);

	if (!ax25_validate((const char *)data + 0) ||
	    !ax25_validate((const char *)data + AXLEN))
		return;

	is_new = (m->entry.count == 0 && m->entry.first_heard == 0);

	memcpy(&m->entry.from_call, data + AXLEN, sizeof(ax25_address));
	memcpy(&m->entry.to_call,   data + 0,     sizeof(ax25_address));
	strcpy(m->entry.portname, port);
	m->entry.ndigis = 0;

	extseq = ((data[AXLEN + ALEN] & SSSID_SPARE) != SSSID_SPARE);
	end    = (data[AXLEN + ALEN] & HDLCAEB);

	data += AXLEN + AXLEN;
	size -= AXLEN + AXLEN;

	while (!end) {
		if (m->entry.ndigis >= 8 || size < AXLEN)
			return;
		memcpy(&m->entry.digis[m->entry.ndigis], data,
		       sizeof(ax25_address));
		m->entry.ndigis++;
		end = (data[ALEN] & HDLCAEB);
		data += AXLEN;
		size -= AXLEN;
	}

	if (size == 0)
		return;

	ctlen = ftype(data, &type, extseq);
	if ((size_t)ctlen > size)
		return;

	m->entry.count++;
	switch (type) {
	case 0x2F:	m->entry.type = MHEARD_TYPE_SABM;	break;
	case 0x6F:	m->entry.type = MHEARD_TYPE_SABME;	break;
	case 0x43:	m->entry.type = MHEARD_TYPE_DISC;	break;
	case 0x63:	m->entry.type = MHEARD_TYPE_UA;		break;
	case 0x0F:	m->entry.type = MHEARD_TYPE_DM;		break;
	case 0x01:	m->entry.type = MHEARD_TYPE_RR;		break;
	case 0x05:	m->entry.type = MHEARD_TYPE_RNR;	break;
	case 0x09:	m->entry.type = MHEARD_TYPE_REJ;	break;
	case 0x87:	m->entry.type = MHEARD_TYPE_FRMR;	break;
	case 0x03:	m->entry.type = MHEARD_TYPE_UI;		break;
	default:	m->entry.type = MHEARD_TYPE_UNKNOWN;	break;
	}

	switch (type) {
	case 0x2F: case 0x6F: case 0x43: case 0x63:
	case 0x0F: case 0x87: case 0x03:
		m->entry.uframes++;
		break;
	case 0x01: case 0x05: case 0x09:
		m->entry.sframes++;
		break;
	default:
		m->entry.iframes++;
		break;
	}

	data += ctlen;
	size -= ctlen;

	if ((type == MHEARD_TYPE_I || type == MHEARD_TYPE_UI) &&
	    size >= 1) {
		switch (*data) {
		case 0xF0:	m->entry.mode |= MHEARD_MODE_TEXT;	break;
		case 0xCD:	m->entry.mode |= MHEARD_MODE_ARP;	break;
		case 0xCC:	m->entry.mode |= (type == MHEARD_TYPE_I) ?
					    MHEARD_MODE_IP_VC : MHEARD_MODE_IP_DG;
				break;
		case 0xCF:	m->entry.mode |= MHEARD_MODE_NETROM;	break;
		case 0x01:	m->entry.mode |= MHEARD_MODE_ROSE;	break;
		case 0xCE:	m->entry.mode |= MHEARD_MODE_FLEXNET;	break;
		case 0xC3:	m->entry.mode |= MHEARD_MODE_TEXNET;	break;
		case 0xBD:	m->entry.mode |= MHEARD_MODE_PSATPB;	break;
		case 0xBB:	m->entry.mode |= MHEARD_MODE_PSATFT;	break;
		default:	m->entry.mode |= MHEARD_MODE_UNKNOWN;	break;
		}
	}

	if (m->entry.first_heard == 0)
		time(&m->entry.first_heard);
	time(&m->entry.last_heard);

	now = m->entry.last_heard;
	if (is_new || now - m->last_flush >= MHEARD_FLUSH_SECS) {
		m->last_flush = now;
		mheard_write(m);
	}
}

/* Create a directory and any missing parents (mkdir -p).  */
static void mkdir_parents(const char *path)
{
	char tmp[256];
	size_t i;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (i = 1; tmp[i] != '\0'; i++) {
		if (tmp[i] == '/') {
			tmp[i] = '\0';
			(void)mkdir(tmp, 0755);
			tmp[i] = '/';
		}
	}
	(void)mkdir(tmp, 0755);
}

/* Preload the list from an existing mheard.dat so that stations heard
 * before a restart keep their record instead of being appended again.
 * Also creates the file (and its directory) if it is missing.  */
int ax25netd_mheard_init(void)
{
	FILE *fp;
	struct mheard_struct buf;
	long position;
	int n = 0;

	fp = fopen(DATA_MHEARD_FILE, "r");
	if (fp == NULL) {
		char dir[256];
		char *slash;

		snprintf(dir, sizeof(dir), "%s", DATA_MHEARD_FILE);
		slash = strrchr(dir, '/');
		if (slash != NULL) {
			*slash = '\0';
			mkdir_parents(dir);
		}

		fp = fopen(DATA_MHEARD_FILE, "w");
		if (fp != NULL) {
			fclose(fp);
			ax25netd_log(LOG_INFO, "mheard: created %s",
				 DATA_MHEARD_FILE);
		} else {
			ax25netd_log(LOG_WARNING, "mheard: cannot create %s: %s",
				 DATA_MHEARD_FILE, strerror(errno));
		}

		fp = fopen(DATA_MHEARD_FILE, "r");
	}
	if (fp == NULL) {
		ax25netd_log(LOG_WARNING, "mheard: cannot open %s: %s",
			 DATA_MHEARD_FILE, strerror(errno));
		return -1;
	}

	position = 0;
	while (n < MHEARD_MAX &&
	       fread(&buf, sizeof(struct mheard_struct), 1, fp) == 1) {
		mheard_list[n].in_use = 1;
		memcpy(&mheard_list[n].entry, &buf, sizeof(struct mheard_struct));
		mheard_list[n].position = position;
		position = ftell(fp);
		n++;
	}

	fclose(fp);
	return 0;
}

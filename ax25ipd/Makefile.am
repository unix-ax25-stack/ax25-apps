
sbin_PROGRAMS = ax25ipd

man_MANS = ax25ipd.8 ax25ipd.conf.5

EXTRA_DIST = $(man_MANS) 

CFLAGS = -DUSE_TERMIO
ax25ipd_LDADD = $(AX25_LIB)

ax25ipd_SOURCES =	\
	config.c	\
	crc.c		\
	io.c		\
	kiss.c		\
	ax25ipd.c	\
	ax25ipd.h	\
	process.c	\
	routing.c	

# ax25netctl is a program, not a daemon, so it goes in bindir and not in
# sbindir: it reads the same files as everything else in this package and
# needs no privilege of its own to do it.  The loop socket it opens may well
# be restricted to a group - see "loop group" in ax25common.conf(5) - and then
# the question of who runs this is the same question as for ax25tcpd, which is
# a bindir program too.

bin_PROGRAMS = ax25netctl

ax25netctl_SOURCES = ax25netctl.c

ax25netctl_LDADD = $(AX25_LIB)

man_MANS = ax25netctl.8

EXTRA_DIST = ax25netctl.man
CLEANFILES = ax25netctl.8 ax25netctl.8.tmp

ax25netctl.8: ax25netctl.man
	name_ax25netctl=$$(echo ax25netctl | sed -e '$(transform)')		\
	sed -e "s/@@@ax25netctl@@@/$$name_ax25netctl/g"			\
	    -e "s|@@@AX25_SYSCONFDIR@@@|$(AX25_SYSCONFDIR)|g"		\
	    $(srcdir)/ax25netctl.man > ax25netctl.8.tmp &&			\
	mv ax25netctl.8.tmp ax25netctl.8;

AX25_SYSCONFDIR=$(sysconfdir)/ax25
AX25_LOCALSTATEDIR=$(localstatedir)/ax25

AM_CPPFLAGS = -DAX25_SYSCONFDIR=\""$(AX25_SYSCONFDIR)"\" \
	      -DAX25_LOCALSTATEDIR=\""$(AX25_LOCALSTATEDIR)"\"
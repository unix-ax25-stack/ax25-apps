sbin_PROGRAMS = ax25tcpd

ax25tcpd_SOURCES =	\
	ax25tcpd.c

ax25tcpd_LDADD = $(AX25_LIB)

man_MANS = ax25tcpd.8 ax25tcpd.conf.5

EXTRA_DIST = $(etcfiles) ax25tcpd.man ax25tcpd.conf.man
CLEANFILES = ax25tcpd.8 ax25tcpd.8.tmp ax25tcpd.conf.5 ax25tcpd.conf.5.tmp

ax25tcpd.8: ax25tcpd.man
	name_ax25tcpd=$$(echo ax25tcpd | sed -e '$(transform)')			\
	sed -e "s/@@@ax25tcpd@@@/$$name_ax25tcpd/g"				\
	    -e "s|@@@AX25_SYSCONFDIR@@@|$(AX25_SYSCONFDIR)|g"			\
	    -e "s|@@@AX25_LOCALSTATEDIR@@@|$(AX25_LOCALSTATEDIR)|g"		\
	    $(srcdir)/ax25tcpd.man > ax25tcpd.8.tmp &&				\
	mv ax25tcpd.8.tmp ax25tcpd.8;

ax25tcpd.conf.5: ax25tcpd.conf.man
	sed -e "s|@@@AX25_SYSCONFDIR@@@|$(AX25_SYSCONFDIR)|g"			\
	    $(srcdir)/ax25tcpd.conf.man > ax25tcpd.conf.5.tmp &&			\
	mv ax25tcpd.conf.5.tmp ax25tcpd.conf.5;

# Needed so that install is optional
etcfiles = ax25tcpd.conf
installconf:
	$(mkinstalldirs) $(DESTDIR)$(AX25_SYSCONFDIR)
	@list='$(etcfiles)'; for p in $$list; do \
	  echo " $(INSTALL_DATA) $$p $(DESTDIR)$(AX25_SYSCONFDIR)/$$p"; \
	  $(INSTALL_DATA) $$p $(DESTDIR)$(AX25_SYSCONFDIR)/$$p; \
	done

AX25_SYSCONFDIR=$(sysconfdir)/ax25
AX25_LOCALSTATEDIR=$(localstatedir)/ax25

AM_CPPFLAGS = -DAX25_SYSCONFDIR=\""$(AX25_SYSCONFDIR)"\" \
	      -DAX25_LOCALSTATEDIR=\""$(AX25_LOCALSTATEDIR)"\"

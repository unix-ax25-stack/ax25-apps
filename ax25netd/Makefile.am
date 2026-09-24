
sbin_PROGRAMS = ax25netd

ax25netd_SOURCES =	\
	ax25netd.c	\
	ax25netd.h	\
	loop.c		\
	mheard.c	\
	mux.c		\
	route.c		\
	upstream.c

ax25netd_LDADD = $(AX25_LIB)

man_MANS = ax25netd.8 agwpe.conf.5 agwpe_shadow.conf.5 ax25common.conf.5

EXTRA_DIST = $(etcfiles) agwpe_shadow.conf.example ax25netd.man agwpe.conf.man \
	agwpe_shadow.conf.man ax25common.conf.man
CLEANFILES = ax25netd.8 ax25netd.8.tmp agwpe.conf.5 agwpe.conf.5.tmp \
	agwpe_shadow.conf.5 agwpe_shadow.conf.5.tmp \
	ax25common.conf.5 ax25common.conf.5.tmp

ax25netd.8: ax25netd.man
	name_ax25netd=$$(echo ax25netd | sed -e '$(transform)')			\
	sed -e "s/@@@ax25netd@@@/$$name_ax25netd/g"				\
	    -e "s|@@@AX25_SYSCONFDIR@@@|$(AX25_SYSCONFDIR)|g"			\
	    -e "s|@@@AX25_LOCALSTATEDIR@@@|$(AX25_LOCALSTATEDIR)|g"		\
	    $(srcdir)/ax25netd.man > ax25netd.8.tmp &&				\
	mv ax25netd.8.tmp ax25netd.8;

agwpe.conf.5: agwpe.conf.man
	sed -e "s|@@@AX25_SYSCONFDIR@@@|$(AX25_SYSCONFDIR)|g"			\
	    $(srcdir)/agwpe.conf.man > agwpe.conf.5.tmp &&			\
	mv agwpe.conf.5.tmp agwpe.conf.5;

agwpe_shadow.conf.5: agwpe_shadow.conf.man
	sed -e "s|@@@AX25_SYSCONFDIR@@@|$(AX25_SYSCONFDIR)|g"			\
	    $(srcdir)/agwpe_shadow.conf.man > agwpe_shadow.conf.5.tmp &&		\
	mv agwpe_shadow.conf.5.tmp agwpe_shadow.conf.5;

ax25common.conf.5: ax25common.conf.man
	sed -e "s|@@@AX25_SYSCONFDIR@@@|$(AX25_SYSCONFDIR)|g"			\
	    $(srcdir)/ax25common.conf.man > ax25common.conf.5.tmp &&		\
	mv ax25common.conf.5.tmp ax25common.conf.5;

# Needed so that install is optional
etcfiles = agwpe.conf ax25common.conf
installconf:
	$(mkinstalldirs) $(DESTDIR)$(AX25_SYSCONFDIR)
	@list='$(etcfiles)'; for p in $$list; do \
	  echo " $(INSTALL_DATA) $$p $(DESTDIR)$(AX25_SYSCONFDIR)/$$p"; \
	  $(INSTALL_DATA) $$p $(DESTDIR)$(AX25_SYSCONFDIR)/$$p; \
	done
	@if test ! -f $(DESTDIR)$(AX25_SYSCONFDIR)/agwpe_shadow.conf; then \
	  echo " $(INSTALL_DATA) -m 600 agwpe_shadow.conf.example $(DESTDIR)$(AX25_SYSCONFDIR)/agwpe_shadow.conf"; \
	  $(INSTALL_DATA) -m 600 agwpe_shadow.conf.example $(DESTDIR)$(AX25_SYSCONFDIR)/agwpe_shadow.conf; \
	fi

AX25_SYSCONFDIR=$(sysconfdir)/ax25
AX25_LOCALSTATEDIR=$(localstatedir)/ax25

AM_CPPFLAGS = -DAX25_SYSCONFDIR=\""$(AX25_SYSCONFDIR)"\" \
	      -DAX25_LOCALSTATEDIR=\""$(AX25_LOCALSTATEDIR)"\"

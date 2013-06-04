
etcfiles = ax25rtd.conf
varfiles = ax25_route ip_route

man_MANS = ax25rtd.8 ax25rtctl.8 ax25rtd.conf.5
CLEANFILES = ax25rtd.8 ax25rtd.8.tmp					\
	     ax25rtctl.8 ax25rtctl.8.tmp				\
	     ax25rtd.conf.5 ax25rtd.conf.5.tmp

ax25rtd.8: ax25rtd.man
	name_ax25rtd=$$(echo ax25rtd | sed -e '$(transform)')		\
	name_Ax25rtd=$$(echo $$name_ax25rtd | sed -r 's@^(.)@\U\1\E@') && \
	name_AX25RTD=$$(echo $$name_ax25rtd | sed -r 's@^(.*)@\U\1\E@') && \
	name_ax25rtctl=$$(echo ax25rtctl | sed -e '$(transform)') &&	\
	name_Ax25rtctl=$$(echo $$name_ax25rtctl | sed -r 's@^(.)@\U\1\E@') && \
	name_AX25RTCTL=$$(echo $$name_ax25rtctl | sed -r 's@^(.*)@\U\1\E@') && \
	sed -e "s/@@@ax25rtd@@@/$$name_ax25rtd/g"			\
	    -e "s/@@@Ax25rtd@@@/$$name_Ax25rtd/g"			\
	    -e "s/@@@AX25RTD@@@/$$name_AX25RTD/g"			\
	    -e "s/@@@ax25rtctl@@@/$$name_ax25rtctl/g"			\
	    -e "s/@@@Ax25rtctl@@@/$$name_Ax25rtctl/g"			\
	    -e "s/@@@AX25RTCTL@@@/$$name_AX25RTCTL/g"			\
	    $(srcdir)/ax25rtd.man > ax25rtd.8.tmp &&			\
	mv ax25rtd.8.tmp ax25rtd.8;

ax25rtctl.8: ax25rtctl.man
	name_ax25rtd=$$(echo ax25rtd | sed -e '$(transform)')		\
	name_Ax25rtd=$$(echo $$name_ax25rtd | sed -r 's@^(.)@\U\1\E@') && \
	name_AX25RTD=$$(echo $$name_ax25rtd | sed -r 's@^(.*)@\U\1\E@') && \
	name_ax25rtctl=$$(echo ax25rtctl | sed -e '$(transform)') &&	\
	name_Ax25rtctl=$$(echo $$name_ax25rtctl | sed -r 's@^(.)@\U\1\E@') && \
	name_AX25RTCTL=$$(echo $$name_ax25rtctl | sed -r 's@^(.*)@\U\1\E@') && \
	sed -e "s/@@@ax25rtd@@@/$$name_ax25rtd/g"			\
	    -e "s/@@@Ax25rtd@@@/$$name_Ax25rtd/g"			\
	    -e "s/@@@AX25RTD@@@/$$name_AX25RTD/g"			\
	    -e "s/@@@ax25rtctl@@@/$$name_ax25rtctl/g"			\
	    -e "s/@@@Ax25rtctl@@@/$$name_Ax25rtctl/g"			\
	    -e "s/@@@AX25RTCTL@@@/$$name_AX25RTCTL/g"			\
	    $(srcdir)/ax25rtctl.man > ax25rtctl.8.tmp &&		\
	mv ax25rtctl.8.tmp ax25rtctl.8;

ax25rtd.conf.5: ax25rtd.conf.man
	name_ax25rtd=$$(echo ax25rtd | sed -e '$(transform)')		\
	name_Ax25rtd=$$(echo $$name_ax25rtd | sed -r 's@^(.)@\U\1\E@') && \
	name_AX25RTD=$$(echo $$name_ax25rtd | sed -r 's@^(.*)@\U\1\E@') && \
	name_ax25rtctl=$$(echo ax25rtctl | sed -e '$(transform)') &&	\
	name_Ax25rtctl=$$(echo $$name_ax25rtctl | sed -r 's@^(.)@\U\1\E@') && \
	name_AX25RTCTL=$$(echo $$name_ax25rtctl | sed -r 's@^(.*)@\U\1\E@') && \
	sed -e "s/@@@ax25rtd@@@/$$name_ax25rtd/g"			\
	    -e "s/@@@Ax25rtd@@@/$$name_Ax25rtd/g"			\
	    -e "s/@@@AX25RTD@@@/$$name_AX25RTD/g"			\
	    -e "s/@@@ax25rtctl@@@/$$name_ax25rtctl/g"			\
	    -e "s/@@@Ax25rtctl@@@/$$name_Ax25rtctl/g"			\
	    -e "s/@@@AX25RTCTL@@@/$$name_AX25RTCTL/g"			\
	    $(srcdir)/ax25rtd.conf.man > ax25rtd.conf.5.tmp &&		\
	mv ax25rtd.conf.5.tmp ax25rtd.conf.5;

installconf:
	$(mkinstalldirs) $(DESTDIR)$(AX25_SYSCONFDIR)
	@list='$(etcfiles)'; for p in $$list; do \
	  echo " $(INSTALL_DATA) $$p $(DESTDIR)$(AX25_SYSCONFDIR)/$$p"; \
	  $(INSTALL_DATA) $$p $(DESTDIR)$(AX25_SYSCONFDIR)/$$p; \
	done
	$(mkinstalldirs) $(DESTDIR)$(AX25_LOCALSTATEDIR)/ax25rtd
	@list='$(varfiles)'; for p in $$list; do \
	  echo " $(INSTALL_DATA) $$p $(DESTDIR)$(AX25_LOCALSTATEDIR)/ax25rtd/$$p"; \
	  $(INSTALL_DATA) $$p $(DESTDIR)$(AX25_LOCALSTATEDIR)/ax25rtd/$$p; \
	done

sbin_PROGRAMS = ax25rtd ax25rtctl

LDADD = $(AX25_LIB)

EXTRA_DIST = $(etcfiles) ax25rtd.man ax25rtctl.man ax25rtd.conf.man $(doc_DATA)

doc_DATA= TODO.ax25rtd README.ax25rtd

ax25rtd_SOURCES =	\
	ax25rtd.c	\
	ax25rtd.h	\
	cache_ctl.c	\
	cache_dump.c	\
	config.c	\
	listener.c

AX25_SYSCONFDIR=$(sysconfdir)/ax25
AX25_LOCALSTATEDIR=$(localstatedir)/ax25

INCLUDES = -DAX25_SYSCONFDIR=\""$(AX25_SYSCONFDIR)"\" \
	   -DAX25_LOCALSTATEDIR=\""$(AX25_LOCALSTATEDIR)"\" 

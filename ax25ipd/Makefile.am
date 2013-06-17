
sbin_PROGRAMS = ax25ipd

man_MANS = ax25ipd.8 ax25ipd.conf.5

EXTRA_DIST = ax25ipd.man ax25ipd.conf.man $(etcfiles) $(doc_DATA)
CLEANFILES = ax25ipd.8 ax25ipd.8.tmp ax25ipd.conf.5 ax25ipd.conf.5.tmp

ax25ipd.8: ax25ipd.man
	name_ax25ipd=$$(echo ax25ipd | sed -e '$(transform)')			\
	name_Ax25ipd=$$(echo $$name_ax25ipd | sed -r 's@^(.)@\U\1\E@') &&		\
	name_AX25IPD=$$(echo $$name_ax25ipd | sed -r 's@^(.*)@\U\1\E@') &&		\
	sed -e "s/@@@ax25ipd@@@/$$name_ax25ipd/g"				\
	    -e "s/@@@Ax25ipd@@@/$$name_Ax25ipd/g"				\
	    -e "s/@@@AX25IPD@@@/$$name_AX25IPD/g"				\
	     $(srcdir)/ax25ipd.man > ax25ipd.8.tmp &&					\
	mv ax25ipd.8.tmp ax25ipd.8;

ax25ipd.conf.5: ax25ipd.conf.man
	name_ax25ipd=$$(echo ax25ipd | sed -e '$(transform)')			\
	name_Ax25ipd=$$(echo $$name_ax25ipd | sed -r 's@^(.)@\U\1\E@') &&		\
	name_AX25IPD=$$(echo $$name_ax25ipd | sed -r 's@^(.*)@\U\1\E@') &&		\
	sed -e "s/@@@ax25ipd@@@/$$name_ax25ipd/g"				\
	    -e "s/@@@Ax25ipd@@@/$$name_Ax25ipd/g"				\
	    -e "s/@@@AX25IPD@@@/$$name_AX25IPD/g"				\
	    $(srcdir)/ax25ipd.conf.man > ax25ipd.conf.5.tmp &&				\
	mv ax25ipd.conf.5.tmp ax25ipd.conf.5;

doc_DATA = README.ax25ipd HISTORY.ax25ipd COPYING.ax25ipd

AM_CFLAGS = -DUSE_TERMIO
ax25ipd_LDADD = $(AX25_LIB)

ax25ipd_SOURCES =	\
	config.c	\
	crc.c		\
	io.c		\
	kiss.c		\
	ax25ipd.c	\
	ax25ipd.h	\
	process.c	\
	routing.c	\
	syslog.c	\
	bpqether.c

# Needed so that install is optional
etcfiles = ax25ipd.conf
installconf:
	$(mkinstalldirs) $(DESTDIR)$(AX25_SYSCONFDIR)
	@list='$(etcfiles)'; for p in $$list; do \
	  echo " $(INSTALL_DATA) $$p $(DESTDIR)$(AX25_SYSCONFDIR)/$$p"; \
	  $(INSTALL_DATA) $$p $(DESTDIR)$(AX25_SYSCONFDIR)/$$p; \
	done

AX25_SYSCONFDIR=$(sysconfdir)/ax25
AX25_LOCALSTATEDIR=$(localstatedir)/ax25

INCLUDES = -DAX25_SYSCONFDIR=\""$(AX25_SYSCONFDIR)"\" \
	   -DAX25_LOCALSTATEDIR=\""$(AX25_LOCALSTATEDIR)"\" 


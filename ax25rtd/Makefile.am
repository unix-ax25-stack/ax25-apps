
etcfiles = ax25rtd.conf
etcdir = $(sysconfdir)/ax25

installconf:
	$(mkinstalldirs) $(DESTDIR)$(etcdir)
	@list='$(etcfiles)'; for p in $$list; do \
	  echo " $(INSTALL_DATA) $$p $(DESTDIR)$(etcdir)/$$p"; \
	  $(INSTALL_DATA) $$p $(DESTDIR)$(etcdir)/$$p; \
	done

sbin_PROGRAMS = ax25rtd ax25rtctl

LDADD = $(AX25_LIB)

EXTRA_DIST = $(etcfiles)

ax25rtd_SOURCES =	\
	ax25rtd.c	\
	ax25rtd.h	\
	cache_ctl.c	\
	cache_dump.c	\
	config.c	\
	listener.c



sbin_PROGRAMS = ax25mond

man_MANS = ax25mond.8 ax25mond.conf.5

EXTRA_DIST = ax25mond.man ax25mond.conf.man $(etcfiles)
CLEANFILES = ax25mond.8 ax25mond.8.tmp ax25mond.conf.5 ax25mond.conf.5.tmp

ax25mond.8: ax25mond.man
	name_ax25mond=$$(echo call | sed -e '$(transform)')			\
	name_Ax25mond=$$(echo $$name_ax25mond | sed -r 's@^(.)@\U\1\E@') &&	\
	name_AX25MOND=$$(echo $$name_ax25mond | sed -r 's@^(.*)@\U\1\E@') &&	\
	sed -e "s/@@@ax25mond@@@/$$name_ax25mond/g"				\
	    -e "s/@@@Ax25mond@@@/$$name_Ax25mond/g"				\
	    -e "s/@@@AX25MOND@@@/$$name_AX25MOND/g"				\
	    $(srcdir)/ax25mond.man > ax25mond.8.tmp &&				\
	mv ax25mond.8.tmp ax25mond.8;

ax25mond.conf.5: ax25mond.conf.man
	name_ax25mond=$$(echo call | sed -e '$(transform)')			\
	name_Ax25mond=$$(echo $$name_ax25mond | sed -r 's@^(.)@\U\1\E@') &&	\
	name_AX25MOND=$$(echo $$name_ax25mond | sed -r 's@^(.*)@\U\1\E@') &&	\
	sed -e "s/@@@ax25mond@@@/$$name_ax25mond/g"				\
	    -e "s/@@@Ax25mond@@@/$$name_Ax25mond/g"				\
	    -e "s/@@@AX25MOND@@@/$$name_AX25MOND/g"				\
	    $(srcdir)/ax25mond.conf.man > ax25mond.conf.5.tmp &&		\
	mv ax25mond.conf.5.tmp ax25mond.conf.5;

ax25mond_LDADD = $(AX25_LIB)

ax25mond_SOURCES =	\
	ax25mond.c

# Needed so that install is optional
etcfiles = ax25mond.conf
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



vardir = $(sysconfdir)/ax25

installconf:
	$(mkinstalldirs) $(DESTDIR)$(etcdir)


bin_PROGRAMS = call

man_MANS = call.1
CLEANFILES = call.1 call.1.tmp

EXTRA_DIST = call.man

call.1 : call.man
	name_call=$$(echo call | sed -e '$(transform)')			\
	name_Call=$$(echo $$name_call | awk '{print toupper(substr($$0,1,1)) substr($$0,2)}') &&	\
	name_CALL=$$(echo $$name_call | awk '{print toupper($$0)}') &&	\
	name_listen=$$(echo listen | sed -e '$(transform)')		\
	name_Listen=$$(echo $$name_listen | awk '{print toupper(substr($$0,1,1)) substr($$0,2)}') &&	\
	name_LISTEN=$$(echo $$name_listen | awk '{print toupper($$0)}') &&\
	sed -e "s/@@@call@@@/$$name_call/g"				\
	    -e "s/@@@Call@@@/$$name_Call/g"				\
	    -e "s/@@@CALL@@@/$$name_CALL/g"				\
	    -e "s/@@@listen@@@/$$name_listen/g"				\
	    -e "s/@@@Listen@@@/$$name_Listen/g"				\
	    -e "s/@@@LISTEN@@@/$$name_LISTEN/g"				\
		-e "s|@@@AX25_SYSCONFDIR@@@|$(sysconfdir)/ax25|g"				\
		-e "s|@@@AX25_LOCALSTATEDIR@@@|$(localstatedir)/ax25|g"				\
	    $(srcdir)/call.man > call.1.tmp &&				\
	mv call.1.tmp call.1;

call_LDADD = $(NCURSES_LIB) $(AX25_LIB) $(ICONV_LIB)

call_SOURCES =		\
	call.c		\
	call.h		\
	menu.c		\
	menu.h		\
	crc.c		\
	crc.h		\
	yapp.c		\
	dostime.c


vardir = $(sysconfdir)/ax25

installconf:
	$(mkinstalldirs) $(DESTDIR)$(etcdir)


bin_PROGRAMS = call

man_MANS = call.1
CLEANFILES = call.1 call.1.tmp

EXTRA_DIST = call.man

call.1 : call.man
	name_call=$$(echo call | sed -e '$(transform)')			\
	name_Call=$$(echo $$name_call | sed -r 's@^(.)@\U\1\E@') &&	\
	name_CALL=$$(echo $$name_call | sed -r 's@^(.*)@\U\1\E@') &&	\
	name_listen=$$(echo listen | sed -e '$(transform)')		\
	name_Listen=$$(echo $$name_listen | sed -r 's@^(.)@\U\1\E@') &&	\
	name_LISTEN=$$(echo $$name_listen | sed -r 's@^(.*)@\U\1\E@') &&\
	sed -e "s/@@@call@@@/$$name_call/g"				\
	    -e "s/@@@Call@@@/$$name_Call/g"				\
	    -e "s/@@@CALL@@@/$$name_CALL/g"				\
	    -e "s/@@@listen@@@/$$name_listen/g"				\
	    -e "s/@@@Listen@@@/$$name_Listen/g"				\
	    -e "s/@@@LISTEN@@@/$$name_LISTEN/g"				\
	    $(srcdir)/call.man > call.1.tmp &&				\
	mv call.1.tmp call.1;

call_LDADD = $(NCURSES_LIB) $(AX25_LIB)

call_SOURCES =		\
	call.c		\
	call.h		\
	menu.c		\
	menu.h		\
	crc.c		\
	crc.h		\
	yapp.c		\
	dostime.c


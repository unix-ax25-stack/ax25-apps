
installconf:

bin_PROGRAMS = listen

man_MANS = listen.1

EXTRA_DIST = listen.man
CLEANFILES = listen.1 listen.1.tmp

listen.1 : listen.man
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
	    $(srcdir)/listen.man > listen.1.tmp &&			\
	mv listen.1.tmp listen.1;

LDADD = $(AX25_LIB) $(NCURSES_LIB)

listen_SOURCES = 	\
	listen.c	\
	listen.h	\
	kissdump.c	\
	ax25dump.c	\
	nrdump.c	\
	arpdump.c	\
	ipdump.c	\
	icmpdump.c	\
	udpdump.c	\
	tcpdump.c	\
	rspfdump.c	\
	ripdump.c	\
	rosedump.c	\
	flexnetdump.c	\
	opentracdump.c \
	utils.c

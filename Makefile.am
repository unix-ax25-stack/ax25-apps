if HAVE_KERNEL_AX25
KERNEL_AX25_SUBDIRS = ax25mond
endif

SUBDIRS = ax25ipd call ax25netd ax25rtd listen $(KERNEL_AX25_SUBDIRS)

EXTRA_DIST = pathnames.h ax25-apps.spec

installconf:
	@for app in $(SUBDIRS); do $(MAKE) -C $$app installconf; done

AX25_SYSCONFDIR=$(sysconfdir)/ax25
AX25_LOCALSTATEDIR=$(localstatedir)/ax25

AM_CPPFLAGS = -DAX25_SYSCONFDIR=\""$(AX25_SYSCONFDIR)"\" \
	      -DAX25_LOCALSTATEDIR=\""$(AX25_LOCALSTATEDIR)"\"

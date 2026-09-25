if HAVE_KERNEL_AX25
KERNEL_AX25_SUBDIRS = ax25mond
endif

SUBDIRS = ax25ipd call ax25netd ax25rtd ax25tcpd listen $(KERNEL_AX25_SUBDIRS)

EXTRA_DIST = pathnames.h ax25-apps.spec

dist_doc_DATA = README.compile README-hints-for-non-kernel-AX25-hosts.txt \
		README-linux-systemd.txt

# The state directory is created on install so the runtime daemons do not
# have to make it themselves: mheardd keeps mheard/mheard.dat here and
# ax25rtd keeps its route cache and control socket under ax25rtd/.
install-data-local:
	$(mkinstalldirs) $(DESTDIR)$(AX25_LOCALSTATEDIR)
	$(mkinstalldirs) $(DESTDIR)$(AX25_LOCALSTATEDIR)/mheard
	$(mkinstalldirs) $(DESTDIR)$(AX25_LOCALSTATEDIR)/ax25rtd

installconf:
	$(mkinstalldirs) $(DESTDIR)$(AX25_LOCALSTATEDIR)
	$(mkinstalldirs) $(DESTDIR)$(AX25_LOCALSTATEDIR)/mheard
	$(mkinstalldirs) $(DESTDIR)$(AX25_LOCALSTATEDIR)/ax25rtd
	@for app in $(SUBDIRS); do $(MAKE) -C $$app installconf; done

AX25_SYSCONFDIR=$(sysconfdir)/ax25
AX25_LOCALSTATEDIR=$(localstatedir)/ax25

AM_CPPFLAGS = -DAX25_SYSCONFDIR=\""$(AX25_SYSCONFDIR)"\" \
	      -DAX25_LOCALSTATEDIR=\""$(AX25_LOCALSTATEDIR)"\"

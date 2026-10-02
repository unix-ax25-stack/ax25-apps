Hints for hosts without a kernel AX.25 stack
===========================================

The AX.25 stack leaves the Linux kernel in 7.1, and macOS and BSD never had
one.  Programs written against libax25 still expect socket(AF_AX25, ...) to
work, and libax25 can answer them itself.  This file is the short version;
axsock(7) is the whole story: axports(5) maps the AX.25 port names onto
the AGWPE or WAMPES servers, ax25netd_agwpe.conf(5) tells ax25netd what to
multiplex, and wampes.conf(5) names the WAMPES nodes.

libax25 can intercept the AX.25 socket calls and serve them from userspace
instead of from the kernel stack - from an AGWPE server (direwolf, or
ax25netd in front of one), or from a WAMPES node.  To build that in:

  ./configure ... --enable-userspace-ax25

Without the option the decision is made by platform: where there is no
kernel AX.25 stack at all (macOS, BSD) it is built regardless, and on Linux
it is left out.  So on Linux say --enable-userspace-ax25 when

  - the kernel has no AX.25 (module not loaded, not built, or a kernel that
    no longer offers it), or

  - one libax25 should serve both ways.  With the interception built in,
    nothing is decided at build time: a probe at the first socket() asks
    whether the kernel answers AF_AX25 and takes it if it does, userspace
    if it does not.

AXSOCK_BACKEND=kernel|agwpe|wampes overrides that choice for one process.
The AGWPE backend keeps one connection, to the server named by the
environment variable AXSOCK_HOST.  A value starting with / is a unix
socket and is used as given; anything else is a TCP host, and AXSOCK_PORT
then gives the port (default 8200).  With neither variable set, libax25
reads ax25common.conf - the same file ax25netd(8) reads - and uses the
loop port it finds there, so the two cannot drift apart.  That is a unix
socket by default, /var/run/ax25/sockets/ax25netd.sock, so by default
every AGWPE socket goes through the daemon, which joins several radio
programs and offers each upstream as an AGWPE port.  Point AXSOCK_HOST at
a radio program instead - a direwolf listens on 8000 - and the daemon is
skipped and libax25 speaks AGWPE with that program directly.
ax25netd_agwpe.conf(5) is read by ax25netd alone and tells it which
upstreams to join; libax25
never reads it.  wampes.conf(5) names the WAMPES nodes, and a port named
there is handed to its node at bind(2).

ax25-apps and ax25-tools have no such option - they use whatever the
libax25 they were built and linked against provides.

A program that was never linked against libax25 - a script, or a daemon that
opens AF_AX25 by itself - can be served all the same, by loading the library
before the C library:

  LD_PRELOAD=/usr/lib/libax25.so.0 program

On Linux the library defines socket(), bind(), connect() and the rest as
ordinary strong symbols, so getting it loaded first is the whole trick: ELF
has a flat namespace.  On macOS that is not enough - Mach-O binds two-level
and DYLD_FORCE_FLAT_NAMESPACE is no longer honoured (measured on macOS
15.7.9: the library is loaded and then asked nothing).  There the library
carries an interposing table instead, and the insertion alone is enough:

  DYLD_INSERT_LIBRARIES=/usr/local/lib/libax25.0.dylib program

That has one consequence for anybody upgrading on macOS: the libc names are
no longer exported from the library, so programs linked against an older
libax25 must be relinked.  Build and install libax25, ax25-apps and
ax25-tools together.

axsock(7) has the whole story: which calls are intercepted, what preloading
cannot reach (setuid, statically linked, syscall() by hand), what ax25d hands
its children, and the environment variables.

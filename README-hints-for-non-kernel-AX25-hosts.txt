Hints for hosts without a kernel AX.25 stack
===========================================

The AX.25 stack leaves the Linux kernel in 7.1, and macOS and BSD never had
one.  Programs written against libax25 still expect socket(AF_AX25, ...) to
work, and libax25 can answer them itself.  This file is the short version;
axsock(7) is the whole story, axports(5), agwpe.conf(5) and wampes.conf(5)
say which backend serves which port.

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
Which userspace backend serves a port follows from the configuration:
agwpe.conf(5) describes the AGWPE upstreams, wampes.conf(5) the WAMPES
nodes, and a port named in wampes.conf is handed to its node at bind(2).

ax25-apps and ax25-tools have no such option - they use whatever the
libax25 they were built and linked against provides.

A program that was never linked against libax25 - a script, or a daemon that
opens AF_AX25 by itself - can be served all the same, by loading the library
before the C library:

  LD_PRELOAD=/usr/local/lib/libax25.so.0 program

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

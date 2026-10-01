Running the AX.25 daemons under systemd
=======================================

The units below are written out because the obvious form is wrong for these
programs, and the way it is wrong is silent: the service never comes up while
the programs it starts for callers run perfectly well.

Type=simple, not Type=forking
-----------------------------

ax25d and its relatives background themselves the traditional way - fork, the
parent exits, the child carries on.  Type=forking describes exactly that, and
it does not work here.

daemon_start() in libax25 skips the fork when getppid() answers 1, taking
that for "started by init".  Under systemd the parent of a service IS process
1, so the fork never happens, the first process never exits, and a unit that
waits for it waits for ever.

So say Type=simple, and say -f where the program has it:

    ax25d -f                    since ax25-tools 0.0.10
    ax25netd -f
    ax25tcpd -f
    conversd -f                 (a separate project, see below)

A program with no such switch is fine under Type=simple as long as it does
not fork on its own - which, under systemd, is what daemon_start() already
arranges by accident.  It is worth saying -f where it exists, so that the
unit does not depend on that accident.

But the accident is not available to everything here, and for the two
ax25-apps daemons -f is not tidiness, it is the difference between a unit
that supervises the daemon and one that loses it.  ax25d reaches for
daemon_start(), which skips its fork under systemd, as described above.
ax25netd and ax25tcpd do not use the library's helper: each carries its own
daemonize() that forks unconditionally and only then looks at the -f flag.
Under systemd that fork is not skipped, so without -f the daemon detaches,
the parent exits 0, and systemd is left supervising a process that is no
longer there.  A unit that does that still reports the service as started,
which is what makes it hard to notice.

Worse, the same daemonize() points its own standard input, output and error
at /dev/null after forking.  What the daemon prints while it sets up reaches
the journal; everything it has to say afterwards - a client that was refused,
an upstream that went away - is written to /dev/null and gone.  Measured on
ax25netd, forking and then connecting a client to the loop port: the four
startup lines are in the log and the connection that follows adds nothing,
while with -f the same four lines arrive and the later ones with them.  That
silence is not the absence of events.


ax25d
-----

    [Unit]
    Description=AX.25 daemon
    After=network.target

    [Service]
    Type=simple
    ExecStart=/usr/sbin/ax25d -f -l
    Restart=on-failure
    RestartSec=5

    [Install]
    WantedBy=multi-user.target

-l puts the connection log into syslog.  It is worth having: without it a
refused call - a station that does not match any entry in ax25d.conf(5) - is
closed without a word, and "rejected - no default" in the log is the only
sign that it happened at all.

Sessions still fork, as they must.  Only the daemon stays in the foreground,
and it ignores SIGCHLD either way, so finished sessions leave no zombies.


ax25netd
--------

    [Unit]
    Description=AGWPE multiplexer for AX.25 clients
    After=network.target

    [Service]
    Type=simple
    RuntimeDirectory=ax25
    RuntimeDirectoryMode=1775
    ExecStart=/usr/sbin/ax25netd -f
    Restart=on-failure
    RestartSec=5

    [Install]
    WantedBy=multi-user.target

-f is not optional here, and the reason is not the usual one.  This daemon
forks through its own daemonize() rather than through the library's
daemon_start(), so it does not get the getppid() == 1 test that saves ax25d,
and it sends its own stdin, stdout and error to /dev/null as part of the same
call.  Without -f, the parent exits at once, systemd is left holding a process
that no longer exists, and every message from then on goes to /dev/null
instead of the journal.  See the section on Type=simple above.

It reaches its upstreams over TCP, so After=network.target is not decoration.
If direwolf runs on the same machine, order it after that unit as well, or
accept that ax25netd retries until the server answers.


The loop socket and /run/ax25
-----------------------------

The loop port is a unix domain socket under /run, and /run is a tmpfs, so
the directory has to exist on every start.  ax25netd creates it, so the
unit works either way; the settings below only decide who puts it there
and with which mode.  What the socket path, 'loop group' and 'loop mode'
mean is in ax25common.conf(5).

If ax25tcpd runs as a unit of its own, give it -f for the reason given under
ax25netd above: it carries its own daemonize(), and it also sends its own
standard streams to /dev/null, so one that was not asked to stay in the
foreground takes its log with it.

RuntimeDirectory=ax25 is the tidier way to have it: systemd creates
/run/ax25 before ExecStart and removes it again on stop, so nothing is left
behind on shutdown.  It is not required, only preferable - and the two are
not in conflict, because a program that finds the directory already there
leaves it alone, mode and owner included.

RuntimeDirectoryMode=1775 is the mode ax25netd applies by default, and it
has to match - it is 'loop mode' that ends up on the directory.  1775 keeps
the directory world readable and traversable, so a client running as an
unprivileged user can find the socket inside it, and the sticky bit keeps
one local account from renaming or removing a socket file that belongs to
another.  It is set here rather than left to the daemons because with two
of them creating the same directory, the one that loses the race would
otherwise inherit the umask of whichever unit happened to win.

If you narrow the loop port to one group with

    loop group hams

in ax25common.conf(5), then 0750 is the mode to use here, and the unit needs
the group as well, or the socket ends up in a group nothing can reach:

    RuntimeDirectory=ax25
    RuntimeDirectoryMode=0750
    Group=hams

Where a unit runs the daemon as a user rather than as root, say so under
[Service] with User= and Group=; the socket then belongs to that user and
'loop group default' means the same account.  The socket mode itself
(0660 for a named group, 0666 for 'all') is not a RuntimeDirectory setting -
it comes from 'loop group' in ax25common.conf(5) and is applied to the socket
file, not to the directory.


A program that knows nothing of libax25
---------------------------------------

Where the kernel has no AX.25 stack, a program that opens AF_AX25 itself can
still be served: load the library before the C library and it answers the
socket calls.  Nothing is recompiled.  conversd from

    https://github.com/dl9sau/conversd-saupp

is the example, because it is a real one - the unit that runs it here:

    [Unit]
    Description=DL/Euro-Convers Daemon
    After=network.target

    [Service]
    Type=simple
    Environment="LD_PRELOAD=/usr/lib/libax25.so.0"
    ExecStart=/usr/local/sbin/conversd -f
    Restart=on-failure
    RestartSec=5

    [Install]
    WantedBy=multi-user.target

Two things about that Environment= line.  The path is the library as it is
installed, not a name to be resolved - LD_PRELOAD takes a path.  And it must
be the library built with --enable-userspace-ax25, or there is nothing in it
to preload; on Linux that option is off by default.  See
README-hints-for-non-kernel-AX25-hosts.txt.

Which ports conversd may use is its own configuration, and it meets libax25
in axports(5):

    conversd.conf:  Listeners  ax25:db0fhn-11,db0fhn-10
    axports:        wampes     DB0FHN-10  9600 256 2  WAMPES node

The callsign it asks for has to exist on the other side too - with the WAMPES
backend that means a line in the node's net.rc:

    listen ax25 add --silent db0fhn-11 client

What a preloaded program cannot do is inherit the variable through a program
that drops the environment.  ax25d passes LD_PRELOAD and LD_LIBRARY_PATH to
the services it starts and nothing else, on purpose; see ax25d(8).


Checking that it took
---------------------

    systemctl status ax25d
    journalctl -u ax25d -f

and for the preload case, once:

    AXSOCK_DEBUG=1 <program>

which says on standard error which backend answered each socket call.  It is
the quickest way to tell "the library is not loaded" from "the node refused
us", and those two look identical from the outside.

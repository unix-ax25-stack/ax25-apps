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
    mheardd -f                  emptied mheard.dat until now, see mheardd(8)
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

    /etc/systemd/system/ax25d.service:

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

ax25d needs root, and that is not an oversight: every session starts a
program, axspawn(8) in the usual configuration, and axspawn refuses to run as
anybody else because it has to change to the account of the caller.  What its
services may do is what ax25d holds.  It is one of two of these four daemons
that do - the other is mheardd on a host whose kernel has AX.25, see
ax25tcpd and mheardd below.

No Wants=ax25netd.service here, unlike the units below: ax25d needs it only
where ax25netd is its backend - AGWPE ports, and the sessions ax25netctl(1)
is to list or kill.  A host whose AX.25 is the kernel's needs nobody, and
with a WAMPES node the library speaks to the node itself.  Where the backend
is ax25netd, add the two lines from Starting ax25netd first below to this
unit too; elsewhere they start a service that has nothing to serve.


ax25netd
--------

    /etc/systemd/system/ax25netd.service:

    [Unit]
    Description=AGWPE multiplexer for AX.25 clients
    After=network.target

    [Service]
    Type=simple
    RuntimeDirectory=ax25
    RuntimeDirectoryMode=1775
    User=daemon
    Group=daemon
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

Nothing in this unit needs root.  The configuration is read, the loop port is
bound to a path under /run, the upstreams are outbound and nothing else is
touched, so the daemon runs as an ordinary account - User=daemon Group=daemon
- and everything it creates then belongs to that account rather than to root.
The daemon has a -u <user> of its own for the same purpose, and the two are
not equivalent: -u starts privileged, makes what it needs as root and then
drops, so the state directory below is settled by it as well; User= runs as
that account from the first instruction and never as root, which is why the
chown below becomes the installation's job.  Under systemd User= is the
better of the two - what the daemon may do is declared in the unit rather
than following from when it drops - and the chown is what it costs.

An unprivileged daemon also cannot create a directory in /run, so the
RuntimeDirectory= lines are what keep this unit alive at all rather than
merely tidy.  See the loop socket section below.

One thing this daemon writes is the heard list, in the mheard directory of
the build's state directory - the same file mheardd(8) writes, with a lock
held so that the two writers cannot tear each other's records.  That
directory belongs to daemon:daemon, the account this daemon runs as, because
an account that does not own it logs "mheard: cannot create ...: Permission
denied" once at every start and keeps no list from then on.  make install
gives it that owner; the directory is mheard/ under the build's state
directory, which --localstatedir decides.  A staged install by a plain user
cannot change an owner and skips the chown instead of failing; on a host
whose AX.25 is the kernel's, where mheardd is the writer and runs as root,
the owner is of no consequence.

It reaches its upstreams over TCP, so After=network.target is not decoration.
If direwolf runs on the same machine, order it after that unit as well, or
accept that ax25netd retries until the server answers.


The loop socket and /run/ax25
-----------------------------

The loop port is a unix domain socket under /run, and /run is a tmpfs, so
the directory has to exist on every start.  ax25netd creates it itself -
every missing component of the path from /run down - and that is what makes
the unit work while the daemon is root.  With User= in the unit it cannot
create anything in /run, and RuntimeDirectory= is then not the tidier form
but the only one that works.  The settings below only decide who puts it
there and with which mode.  What the socket path, 'loop group' and
'loop mode' mean is in ax25common.conf(5).

RuntimeDirectory=ax25 is the tidier way to have it: systemd creates
/run/ax25 before ExecStart and removes it again on stop, so nothing is left
behind on shutdown, and the two are not in conflict - a program that finds
the directory already there leaves it alone, mode and owner included.

Only ax25netd's unit may carry it, however.  systemd removes the directory
when the unit that carries it stops, so a second unit with the same
RuntimeDirectory= would take /run/ax25 away from the first one whenever it
stopped, and the two would not agree on the mode either - each applies its
own.  ax25tcpd would create the same path itself if it found it missing,
every component of it with 'loop mode' - but it runs as an unprivileged
account now, so it cannot, and what it does make, sockets/, is one level
down and exists only as long as the level above does.  That is the next
section.

RuntimeDirectoryMode=1775 is there because systemd's own default for that
setting is 0755, which is not 'loop mode': the directory ax25netd makes for
itself carries 'loop mode', and the same value here keeps the result
independent of whether systemd or the daemon got there first.  1775 keeps
the directory world readable and traversable, so a client running as an
unprivileged user can find the socket inside it, and the sticky bit keeps
one local account from renaming or removing a socket file that belongs to
another.

If you narrow the loop port to one group with

    loop group hams

in ax25common.conf(5), then 0750 is the mode to use here, and the unit needs
the group as well, or the socket ends up in a group nothing can reach:

    RuntimeDirectory=ax25
    RuntimeDirectoryMode=0750
    Group=hams

Where a unit runs the daemon as a user rather than as root - ax25netd and
the two units below do - say so under [Service] with User= and Group=; the
socket then belongs to that user and 'loop group default' means the same
account.  The socket mode itself
(0660 for a named group, 0666 for 'all') is not a RuntimeDirectory setting -
it comes from 'loop group' in ax25common.conf(5) and is applied to the socket
file, not to the directory.


Starting ax25netd first
-----------------------

ax25tcpd and mheardd carry two more lines in [Unit], and ax25d where
ax25netd is its backend:

    Wants=ax25netd.service
    After=network.target ax25netd.service

Both lines, and in that combination:
Wants= says the service is wanted, After= says when, and without the
second one systemd starts them all in parallel - a boot orders nothing by
itself.  Wants= and not Requires=: on a host whose AX.25 is the kernel's,
or whose node is a WAMPES node the library reaches itself, there is no
ax25netd and no ax25tcpd at all, and Requires= names a unit that does not
exist - the job fails with "Unit ax25netd.service not found" and takes the
others with it, on a machine where they are wanted and none is optional.
Wants= asks for the same start without making it a condition; a missing
unit is not an error, and ordering against it costs nothing.

Where ax25netd is there but not yet up, they meet a loop socket that is
not there yet, and none of them meets it well:

  - ax25tcpd exits.  Its back side is the loop socket, and it connects
    there before it opens anything: "cannot connect to
    /var/run/ax25/sockets/ax25netd.sock: No such file or directory",
    exit code 1 - and Restart=on-failure then starts it again every
    RestartSec until the other unit happens to be up.

  - ax25d waits, where it carries the ordering.  Opening a port that
    belongs to ax25netd waits for the link before listen(2) returns, and
    gives up only after ten seconds in
    which the reader has not managed to connect: the reader retries in the
    background, and every failed attempt starts those ten seconds over.
    Measured with two userspace ports and no ax25netd answering - 25
    seconds for the first listen, 16 for the second.  The ports are not
    registered while the link is down either; the reader registers them
    when it comes up.

  - mheardd starts, and does not wait.  It opens its monitor once, at
    start-up, and the open is refused only when nothing at all could feed
    it: no kernel AX.25 port it may open, no WAMPES node, and no AGWPE
    server named - then "cannot watch for AX.25 frames: Device not
    configured", exit code 1.  Any one of the three is enough.  A server
    that is named and not up yet is not a missing source: it is handed the
    same quiet descriptor a fed monitor gets and starts receiving when the
    reader has the link.  On the machine this README is written for - a
    WAMPES node is configured - the ordering saves it only the retries it
    no longer has to make.

All of them recover - the library reconnects with a backoff and registers
again what this process had registered before - but recovery happens after
everybody has already waited, and at boot it happens at every single
start.  Two lines take that away.

After= says the same in both cases and is never the strict one: it orders
when the start happens and does not insist on it.  What it buys with
Wants= above is the start of ax25netd before them, not a dependency on it,
so a restart of ax25netd - which is what Restart=on-failure does after a
crash - restarts nothing else: the others keep running and reconnect, which
is what they are built for, and
"systemctl restart ax25netd" is a restart of one unit.  Should a host
prefer the other behaviour - every client of the loop socket restarted
with the netd, so that nobody is left beside a /run/ax25 that has just
been removed and created anew - then Requires= is the line for it, and
the cost is the missing-unit failure above on a host that runs no
ax25netd.  The two are a choice between those two machines, not one
answer for both.


ax25tcpd and mheardd
---------------------

ax25tcpd needs no root, and on a host whose AX.25 is the userspace one
neither does mheardd - so both run as the account ax25netd runs as, and both
carry the ordering above:

    /etc/systemd/system/ax25tcpd.service:

    [Unit]
    Description=AX.25 to TCP bridge
    Wants=ax25netd.service
    After=network.target ax25netd.service

    [Service]
    Type=simple
    User=daemon
    Group=daemon
    ExecStart=/usr/sbin/ax25tcpd -f
    Restart=always
    RestartSec=5

    [Install]
    WantedBy=multi-user.target

    /etc/systemd/system/mheardd.service:

    [Unit]
    Description=AX.25 heard list
    Wants=ax25netd.service
    After=network.target ax25netd.service

    [Service]
    Type=simple
    User=daemon
    Group=daemon
    ExecStart=/usr/sbin/mheardd -f
    Restart=on-failure
    RestartSec=5

    [Install]
    WantedBy=multi-user.target

Restart=always for ax25tcpd, and it is the one that is easy to get wrong.
The loss of the netd is not a failure to this daemon: its loop returns, main
returns 0, and the exit code is that of a program that finished its work.
Restart=on-failure would call that success and leave the bridge down until
somebody notices - so restart it whatever it exits with, which is what
always means.

mheardd's unit is the one that changes with the machine.  Its first source
is a packet socket on a kernel AX.25 port, and opening one needs
CAP_NET_RAW; as User=daemon the open fails, and it fails silently - there
is no message for it, only no source.  What remains is what the library
can reach, and any one of them is enough to start it: a WAMPES node from
wampes.conf, or the AGWPE monitor of the ax25netd the unit wants -
ax25netd is named by default, so on a host that runs it that source is
always there, up or not.  As User=daemon with ax25netd running, the list
therefore stays up and holds the frames ax25netd carries; every frame the
kernel carries is missing from it, and nothing says so.

The open is refused, "cannot watch for AX.25 frames: Device not
configured" and exit code 1, only when none of the three exists: no
kernel port it may open, no WAMPES node, and no AGWPE server named.  That
is a configuration fault, and Restart=on-failure then repeats it for ever.

If the ports are ax25netd's there is no kernel source to lose and
User=daemon is right as it stands.  A host with kernel AX.25 ports wants
mheardd to hear them, and has two ways to give it that:

    User=root
    Group=root

or the capability alone, which is the narrower of the two:

    AmbientCapabilities=CAP_NET_RAW
    CapabilityBoundingSet=CAP_NET_RAW

Either way the packet socket opens and the list has both sources.  ax25d
above stays root for a different and unchangeable reason - axspawn(8)
insists on it - while this one is a question of what the machine has.

-f is what puts their log where systemd is looking.  ax25tcpd writes to
standard error as soon as it is in the foreground and to syslog when it is
not, and when it daemonizes it takes its standard streams with it to
/dev/null, so without -f its log goes the same way - the reason is the one
given under ax25netd above.  mheardd writes its errors to standard error
unless -l is given, and syslog reaches the journal as well; the unit above
takes the first of the two.

The heard list is settled by the install: make install gives mheard/ to
daemon:daemon, the account the two daemons share, and make installconf
leaves the empty mheard.dat mheard(1) expects there under the same owner,
so the daemon that fills it can.  The ax25netd section says why the account
is that one.  What is left to the configuration is a consequence of running
as somebody other than root - one line in ax25tcpd.conf(5) can still ask
for privileges:

    listen tcp 0.0.0.0 80

a port below 1024 may not be bound by an unprivileged process; either move
the port above it or leave the unit root for that case.


A program that knows nothing of libax25
---------------------------------------

Where the kernel has no AX.25 stack, a program that opens AF_AX25 itself can
still be served: load the library before the C library and it answers the
socket calls.  Nothing is recompiled.  conversd from

    https://github.com/dl9sau/conversd-saupp

is the example, because it is a real one - the unit that runs it here:

    /etc/systemd/system/conversd.service:

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

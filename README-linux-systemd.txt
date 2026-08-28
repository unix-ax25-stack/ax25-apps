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

So say Type=simple, and where the program offers it, tell it not to fork:

    ax25d -f                    since ax25-tools 0.0.10
    conversd -f                 (a separate project, see below)

A program with no such switch is fine under Type=simple as long as it does
not fork on its own - which, under systemd, is what daemon_start() already
arranges by accident.  It is worth saying -f where it exists, so that the
unit does not depend on that accident.


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
    ExecStart=/usr/sbin/ax25netd
    Restart=on-failure
    RestartSec=5

    [Install]
    WantedBy=multi-user.target

It reaches its upstreams over TCP, so After=network.target is not decoration.
If direwolf runs on the same machine, order it after that unit as well, or
accept that ax25netd retries until the server answers.


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

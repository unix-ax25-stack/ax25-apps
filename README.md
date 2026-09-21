# unix-ax25-stack

AX.25 networking for Unix — userspace and kernelspace, with connector to the outer world.

## The suite

- **libax25 / ax25apps / ax25tools** — the classic AX.25 library, applications, and tools
- **wampes** — a `net` derivative (ka9q nos) with a full userspace AX.25 stack

## What's new

The suite used to run on Linux only, tied to the in-kernel AX.25 stack. Today
*libax25* also runs without it (`--enable-userspace-ax25`) and talks directly
to WAMPES' userspace *net* stack — bringing AX.25 to other Unices for the
first time (tested on macOS). Kernel-mode AX.25 remains fully supported.

## Status

Proof of concept, in production on **DB0FHN** since August 2026. Testing and
bug reports are welcome; so are ideas — use GitHub Discussions.

## Background

The Linux AX.25 stack had numerous bugs and stability problems, and with
kernel 7.1 in-kernel AX.25 support is at risk of disappearing. A timely
solution was needed — this project provides one.

Concept and implementation took six weeks (from early August 2026). The suite
has been running on the modernized DB0FHN in production for two weeks and
received its first polish. There may still be bugs — all I have encountered
are fixed. Part of the development was non-trivial; without AI support and
long days this would probably still be in the planning stage.

## Configuration is trivial

The Unix side — `/etc/ax25/axports`:

```
wampes DB0FHN-10        9600    256     2       WAMPES node, port xnet
```

The WAMPES side — the essentials from `net.rc`:

```
ax25 mycall db0fhn-10
ip addr 44.130.60.101

atta tun ax25 1500 linux
route add 44.130.60.100 linux

atta axip xnet udp 930:9393 bind=127.0.0.1
ifc xnet encap ax25i mtu 1500
axip rou add db0fhn 127.0.0.1
```

That's it — a node on the air, bridged through WAMPES' AX.25 userspace stack,
with no kernel AX.25 involved. The complete, hardened example (incl. `ip learn`
and `listen ax25 add`) is in the folded section below. systemd startup:
`wampes/doc/linux-startup-systemd.txt`.

Please adjust /tcp/sockets for your needs.

After installing a new libax25, rebuild and relink the tools against it — the
library has changed a lot. Compile details incl. `--enable-userspace-ax25`:
`libax25/README.compile`. Further documentation: `libax25/doc`.


## Upstream

Previously hosted at linux-ax25.in-berlin.de (see its wiki); consindering
moving to GitHub for easier discussions by the community and push requests.
Active developmennt lives here.

WAMPES was converted from CVS to git.

Licenses are GPL-2.0 (see each repo).

<details>
<summary><b>DB0FHN rundown</b> — full <code>net.rc</code> with routing & hardening</summary>

Unix side, `/etc/ax25/axports`:

```
wampes DB0FHN-10        9600    256     2       WAMPES node, port xnet
```

WAMPES side, `net.rc` (excerpt):

```
ax25 mycall db0fhn-10
ip addr 44.130.60.101

atta tun ax25 1500 linux
route add 44.130.60.100 linux

atta axip xnet udp 930:9393 bind=127.0.0.1
ifc xnet encap ax25i mtu 1500
axip rou add db0fhn 127.0.0.1

ax25 rou add permanent vj xnet db0fhn
ax25 rou add permanent vj xnet igate db0fhn
ax25 rou add permanent vj xnet igateb db0fhn

arp add 44.130.254.254 ax25 igate
rou add 44.130.254.254 xnet

rou add 44.130.254.0/24 xnet 44.130.254.254
rou add 44.130.0.0/16 linux 44.130.254.254 call=igate # igate himself
# deny for net: don't learn /32. use routing table
ip learn deny 44.130.254.0/24 call=igate # PR users via inet, igate. pool getip
ip learn deny 44.130.0.0/16 iface=xnet # PR users via inet, igate. pool getip
ip learn deny 44.224.0.0/15 iface=linux # hamnet RF-DL via host
ip learn deny 44.128.0.0/10 iface=linux # hamnet RF via host
ip learn deny 44.0.0.0/9 iface=linux # amprnet inet via host
ip learn deny 0.0.0.0/0 # the rest of the universe

listen ax25 add --silent db0fhn-9 client # ax25d to axspawn login
listen ax25 add --silent db0fhn-11 client # conversd
listen ax25 add --silent db0fhn-12 client # wconversd
listen ax25 add --silent db0fhn-13 client # lconversd
```


DB0FHN is a system open to radio amateurs — its installation and configuration
can be inspected on site.

</details>

<details>
<summary><b>Connect demos</b> — live AX.25 calls via WAMPES</summary>

```
$ call -r -s dl9sau-1 wampes db0fhn
GW4PTS AX.25 Connect v1.11
Trying...
*** Connected to db0fhn
Rawmode
(X)NET/LINUX V1.39 Digipeater Nuremberg Institut of Technology. Loc: JN59NK.
```

WAMPES knows the path to the igate:

```
$ call -r -s dl9sau-1 wampes igate
GW4PTS AX.25 Connect v1.11
Trying...
*** Connected to igate
Rawmode
This is IGATE. Internetgatewaysystem for the Packet Radio Network.
```

Connect via WAMPES' `xnet` iface (note: `/etc/ax25/axports` need not know
`wampes:xnet`):

```
# call -r -s dl9sau-1 wampes:xnet igate
GW4PTS AX.25 Connect v1.11
Trying...
*** Connected to igate
Rawmode
This is IGATE. Internetgatewaysystem for the Packet Radio Network.
```

Loopback connect:

```
$ call -r -s dl9sau-1 wampes db0fhn-9
GW4PTS AX.25 Connect v1.11
Trying...
*** Connected to db0fhn-9
Rawmode
DB0FHN>  17 8 19 6 1
```

When the iface is specified explicitly (`wampes:xnet`), the assigned source
call is used:

```
wampes       DB0FHN-10   9600  256  2   WAMPES node, port xnet
wampes:xnet  DB0FHN-8    9600  256  2   WAMPES node, port xnet

$ call -r wampes db0fhn
..
9:DB0FHN-10               <-> con
..

$ call -r wampes:xnet db0fhn
..
9:DB0FHN-8                <-> con
..
```

</details>

<details>
<summary><b>AGWPE notes</b> — status & open questions</summary>

The AGWPE branch in libax25 was not pursued further, because the own protocol
to WAMPES is more promising and efficient. AGWPE is tested and works, e.g.
with direwolf as backend. Untested: AGWPE clients, and whether the
direwolf-backend or the AGWPE-client path to wampes works. Feedback is very
welcome.

</details>

<details>
<summary><b>Compilation example</b></summary>

Quick example how to compile and install libax25 ax25-apps ax25-tools and wampes
after git clone:

Check if installed:  make autoconf automake (>1.9) zlib1g-dev libtool and libncursesw6-dev (previously libncurses-dev) and optionaly libfltk1.3-dev (for ax25-tools/hdlcutil). Parts of the ax25-tools package will also need fltk and fltk development packages (and are silently skipped on compile if these packages are not installed).
wampes also needs libgdbm-dev.

```
Debian, Ubuntu, Raspberry Pi OS   apt install libgdbm-compat-dev libgdbm-dev libncurses-dev
Fedora, RHEL                      dnf install gdbm-devel ...
Arch                              pacman -S gdbm ...
Alpine                            apk add gdbm-dev ...
```

David Ranch KI6ZHD provided a build-script: build-unix-ax25-stack.sh


```
cd wampes.git; make install; cd ..
for i in libax25.git ax25-apps.git ax25-tools.git; do
  cd $i ; autoreconf --install --force ; ./configure --enable-userspace-ax25 --prefix=/usr --sysconfdir=/etc --localstatedir=/var --mandir=/usr/share/man; make clean; make install
  # For first-time-install of the configuration files: make installconf
  cd ..
done
```


</details>

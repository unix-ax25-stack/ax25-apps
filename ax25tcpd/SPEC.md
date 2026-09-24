# ax25tcpd — interaktives AX.25-Frontend über TCP / Unix-Socket

> Spec. Zusammenfassung der am 2026-08-11 (Session `ses_027779655ffe0fpFUBG0bCYFcQ`)
> diskutierten und fixierten Entscheidungen. Dauerhafte Notiz, damit die Planung
> einen Session-Wechsel überlebt.

## Rolle

Eigenständiger Daemon in `ax25-apps/` (bewusst NICHT in ax25netd). Das
interaktive Frontend für Menschen/Skripte: **eine AX.25-Session pro Verbindung**,
`connect`/`datagram`. Die Rückseite spricht das AGWPE-Protokoll als Client zum
Loop-Port 255 von ax25netd (über dessen Unix-Socket oder TCP); netax25
(libax25-Shim) bleibt unverändert auf AGWPE.

## Transports (Client-Seite)

- TCP-Listener IPv4/IPv6 (`bindaddr:port`), zwei konsekutive Ports:
  - **Port X (binary)**: Datagramm-Inhalt = **alles als ein Paket**; bei
    Inhalt > MTU wird in Chunks von `mtu` (default 256) geteilt. Danach endet
    die Session.
  - **Port X+1 (text)**: **je Zeile ein Paket** (CR/LF/CRLF terminiert).
- Unix-Socket (`listen unix <path> [group <g>]`): verhält sich wie der
  Text-Port; Zugriff über Datei-Rechte (group-Semantik wie netd).
- Clients: `telnet`, `nc`, `socat`, `conversd`, eigene Skripte.

## Protokoll pro Verbindung

1. Verbindung → Prompt.
2. **Erste Zeile = Befehl** (EOL egal: CR, LF, CRLF), wampes-artig
   **prefix-abkürzbar** (`c`, `co`, `conn`, …). Bei Mehrdeutigkeit Kandidatenliste.
3. Danach **Datenmodus**: Byte-Strom läuft in die AX.25-Session
   (connectet: `'D'`-Frames; unconnectet: Framing laut Port X/X+1).
4. **SYN / ACK SYN** = AX.25-Handshake über den netd-Loop-Port
   (`'C'`/`'v'`/`'c'`-Anfrage). Die Bestätigung kommt als `'C'`-Frame
   (datakind 'C', `call_to` = eigener Call) vom netd-Loop-Port zurück;
   abgelehnt wird per `'d'`-Frame.

## Befehle

```
connect  [--silent] [--keep] [--pid=XX|name] [--port <port>] [--mycall <call>]
         <port>[:chan] <dest>[,<digi>,…] [< SRC]
datagram [--silent] [--keep] [--pid=XX|name] [--port <port>] [--mycall <call>]
         <port>[:chan] [<dest>[,<digi>,…]] [< SRC]
quit | bye        Verabschiedung, Verbindung schließt
help              Kommando-Übersicht
```

- `--silent`: keine `*** connected`/`*** disconnected`-Statuszeilen (für
  fd-Übergabe an Programme, die den Nachrichteninhalt erwarten).
- `--keep`: TCP bleibt nach Session-Ende offen, neuer `connect`/`datagram`
  möglich. **Default (ohne `--keep`)**: Session-Ende → Verbindung schließt
  (klassisches telnet: `telnet localhost pop3` … `quit`).
- `--pid=XX` hex bzw. `pid=name`: `text`→F0, `netrom`→CF, `flexnet`→C4,
  `rose`→C3, `l3`→CC.
- `--port <port>`: Alternativschreibweise zu `connect <port> …`.
- `--mycall <call>`: gleichwertig zu `< SRC`, zuletzt gewinnt.
- Digipeater-Pfad: Komma **und** Leerzeichen trennen, gemischt erlaubt
  (`DB0AAA-8 DB0BBB` = `DB0AAA-8,DB0BBB`), wampes-Grammatik.
- `datagram` ohne `<dest>`: jede Zeile ist ein ganzer TNC2-Rahmen
  `SRC>DEST,DIGI,…:payload` (Header aus den Daten, nicht aus dem Kommando);
  mit `<dest>`: fester Header, jede Zeile ist Payload. Der Quell-Call einer
  TNC2-Zeile steht in der Zeile; im festen Modus wie beim `connect`.
  Ein leerer Payload sendet nichts; kaputte Rahmen melden einen Fehler, auch
  unter `--silent`.
- Antworten kommen in der EOL des Clients: enden die Zeilen mit `\r`
  (CRLF oder CR allein), antwortet tcpd mit CRLF, sonst mit nacktem LF.
  Der Datenstrom selbst wird nie umgeschrieben.
- Autoroute (Digipeater-Pfad bei connect ohne Digis) löst **zentral der
  ax25netd** (siehe `autoroute` in `agwpe.conf(5)`); kein tcpd-Flag nötig.
- Status im TNC-Stil: `*** connected to DB0AAA`, `*** disconnected`.
- Fehlt `[< SRC]`: Fehler `source call required` (bzw. optionaler
  `default-call` pro Target in der Konfiguration).
```

## AGWPE-Session-Parameter (entschieden)

- Das AGWPE-Protokoll hat **keinen** Befehl für Session-Parameter
  (MTU/Maxframe/Window). Direwolf kennt nur `P X x G g R m H y Y M C D d v V c K k`;
  die Parameter liegen beim TNC/Server (Direwolf-Kanal-Konfiguration).
- netd merkt sich Session-Parameter nur deshalb (`mux.c:810-816`), weil
  `axsock`/`axctl` Kernel-AX.25-SetCtl in `'Q'`-Frames übersetzen — das wirkt
  nur gegen die Loop/netax25-Seite.
- Konsequenz: `mtu` in der Konfiguration ist **nur das Datagramm-Chunking**
  (binary Port), keine Session-Übergabe.
- Phase 2: `param window=… paclen=…` → `'Q'`-Frames (nur Loop/axsock-seitig
  sinnvoll, Direwolf ignoriert `'Q'`).

## Konfiguration `ax25tcpd.conf`

```
# Vorderseite: Listener
listen tcp <addr> <port>          # X = binary, X+1 = text
listen unix <path> [group <g>]    # Text-Port über Unix-Socket

mtu <n>                           # Datagramm-Chunking, default 256
default-call <call>               # optionales < SRC für connect/datagram
```

Die **Rückseite** kommt aus der gemeinsamen Loop-Port-Config
`/etc/ax25/ax25common.conf` (`loop socket <path>` / `loop tcp <port>`),
derselben Datei, aus der netd seinen Listener liest — eine Datei, eine
Semantik (implementiert als `ax25common_config_load`, libax25/axcommon.c,
auch vom AGWPE-Shim genutzt). Für einen netd auf anderem Host überschreibt
eine explizite Zeile:

```
target tcp <host> <port>          # netd-Loop über TCP (Override)
target socket <path>              # netd-Loop über Unix-Socket (Override)
```

## Portvergabe

| Port            | Rolle                                       |
|-----------------|---------------------------------------------|
| 8000            | Direwolf-AGWPE-Server (netd-Outgoing)       |
| 8100            | netd-Loop TCP (default)                     |
| 8101/8102       | ax25tcpd-Front binary/text                  |
| /var/ax25/ax25netd.sock | netd-Loop-Unix-Socket             |
| /var/ax25/ax25tcpd.sock | ax25tcpd-Front-Socket (Sample)      |

## Port-Namensauflösung (axports/`:N`)

- Interface-Namen mit `agwpe-`-Präfix = virtuelle (Loop-)Schnittstellen,
  kollidieren nicht mit Kernel-AX.25-Interfaces.
- `name:N` = **Channel N des benannten Upstreams** (nicht Upstream-Index):
  `agwpe-direwolf2:4` = Upstream `direwolf2`, Channel 4. Umsetzung in
  libax25/axsock.c (`axsock_strip_prefix` + `axsock_gport_channel`,
  Kontiguität first+N); Fallback ohne G-Tabelle: `pos*16+idx`.
- axports-Einträge liefern per Port `default-call` (callsign) und MTU
  (paclen): Fallback-Kette Frame/Kommando → axports → Conf-Default
  (`default-mtu`, auskommentiert) → builtin 256.

## Implementierung (ax25-apps/ax25tcpd/)

- `ax25tcpd.c` — Konfig-Parser, Listener (getaddrinfo-Bind + Unix-Socket nach
  netd-`loop.c`-Muster: Stale-Socket-Unlink, `mkdir`-Parent, chgrp nach group),
  Select-Loop.
- AGWPE-Client über `libax25` `agwpe_client_*` (`connect_unix`/`connect_host`,
  `send_unproto(_via)`, `connect(_via)`, `send_data`, `disconnect`), `'G'`-Tabelle
  für Name→Port-Mapping.
- Zustandsmaschine je Client: `CMD → CONNECTING → DATA` (bzw. `DGRAM`);
  TCP-Close im Datenmodus = Disconnect.
- Prefix-Abkürzung: eindeutiges Präfix über die Kommandotabelle.

## Tests (E2E)

- netd (Loop + Mock-Upstream) + ax25tcpd; Python-Client:
  connect+Daten+disconnect, datagram binary (Chunk bei >MTU: 700 B → 256/256/188),
  datagram text (Zeile→Paket), datagram TNC2 (Zeile→Rahmen), `--silent`,
  `--keep`, `--mycall`, Komma-/Leerzeichen-Pfad, EOL-Adaption (LF↔CRLF),
  Unix-Socket, `quit`/`bye`, Prefix-Abkürzungen, Fehlerpfade. Mock mit
  Ctrl-Port (8011) für Downstream-Frames; Testskripte `mock_upstream.py`,
  `test_data.py`, `test_full.py`, `test_syntax.py` in
  `/var/folders/wc/51f1ls413b70nmmttqww4tyr0000gn/T/opencode/e2e/`.
- Config-Layout im E2E: `agwpe.conf` (nur noch auth/radio/loop), `ax25common.conf`
  (`loop socket …/ax25netd.sock`), `ax25tcpd.conf` ohne `target` (Rückseite via
  ax25common.conf). netd + ax25tcpd laufen mit `-C ax25common.conf`.
  Der Fehlerpfad: `socket`/`tcp`/`group` in agwpe.conf → „belongs in
  ax25common.conf, not here" und netd-Start bricht ab.
- Gefundene/behobene Bugs in der E2E-Phase:
  - Binäres Datagramm: Payload im selben TCP-Segment wie die Kommandozeile wurde
    als Zeilenrest in `rbuf` sortiert statt in `dbuf` → nach Befehlswechsel zu
    `TPC_DGRAM` Rest aus `rbuf` in `dbuf` übernehmen.
  - `tpc_on_disconnect` ignoriert `TPC_DGRAM`-Clients (Datagramm hat keine
    Verbindung; ein verspäteter `'d'`-Echo darf die Session nicht beenden).
  - netd benennt Upstream-Ports nach seiner conf (`radio`), nicht nach dem
    Selbstname des Upstreams — Port-Lookup beim ersten Test schlug fehl.

## Phase 2 / offene Punkte

- Login/Auth am ax25tcpd-Listener (default off, bind loopback); Userverwaltung
  wird später auch die AGWPE-Authentifizierung am netd-Port stellen (libax25
  muss das können — `agwpe_client_login` existiert bereits).
- `param window=…/paclen=…` via `'Q'`.

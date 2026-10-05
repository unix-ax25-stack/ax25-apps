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

- **Ein Listener pro `listen`-Zeile**, nicht einer pro Modus. TCP
  (`bindaddr:port`) oder Unix-Socket (`listen unix <path> [group <g>]`).
- Ob eine Verbindung `ascii` oder `binary` ist, entscheidet **der Client pro
  Verbindung**, mit einer Zeile für sich vor dem Kommando. Es gibt deshalb
  keinen zweiten Port, den man sich merken müßte, und der Daemon weiß vor
  der ersten Zeile nichts darüber, wie der Client sprechen wird.
  - **ascii** (Vorgabe): je Zeile ein Paket, Zeilenende auf CR normalisiert;
    eigene Meldungen mit LF; ein CR aus der Ferne wird zu LF. Das ist, was
    ein Mensch am Terminal und ein `telnet`-Client brauchen.
  - **binary**: 8 bit sauberer Bytestrom, Ferndaten unverändert; ein
    unverbundenes Datagramm ist **alles als ein Paket**, bei Inhalt > MTU in
    Chunks von `mtu` (default 256) geteilt, Auslieferung beim Schließen.
- Der Modus gehört der **Verbindung**, nicht dem Kommando: ein Skript sagt
  einmal `binary` und connectet danach beliebig oft; wer `binary` gesagt hat,
  bekommt `binary` bis er `ascii` sagt. Beim Disconnect wird er nicht
  zurückgesetzt — genau wie `--keep`.
- Clients: `telnet`, `nc`, `socat`, `conversd`, eigene Skripte.

### TELNET: erkannt, nicht angeboten

- Kein serverseitiges IAC beim Connect: das würde drei Protokollbytes vor
  das erste Paket jedes binären Clients legen, und wer nicht nach TELNET
  gefragt hat, hat nicht darum gebeten, eines zu bekommen.
- Stattdessen gilt das **erste 0xFF einer ascii-Verbindung** als IAC, und
  die Verbindung spricht von da an TELNET. Mit einem Server, der nichts
  sagt, ist `telnet(1)` still, bis die Unterbrechungstaste gedrückt wird —
  und das ist genau das erste Byte: `IAC IP` statt 0x03. Die Erkennung
  landet also auf dem ^C, der als echtes 0x03 ankommt. Ein 0xFF der
  Nutzdaten wird als `IAC IAC` maskiert, `IAC`-Verhandlungen abgelehnt.
- Die Suche läuft **in einer Session und in Datenstrom-Reihenfolge**, nicht
  über einen Scan dessen, was ein `read()` gerade zurückgab: ein Skript, das
  Kommando und Payload in einem `write()` schickt, hat beides im selben
  Puffer, und ein Scan am Schleifenkopf hätte das 0xFF gefunden, bevor die
  `binary`-Zeile überhaupt geparst war. Eine Kommandozeile kann eine
  Verbindung nicht zu TELNET machen — kein Befehl trägt ein 0xFF.
- `binary` schaut nie, das ist der 8-bit-saubere Kern. Einmal als TELNET
  erkannt, wird `binary` für den Rest dieser Verbindung **abgelehnt**; wer
  binär will, öffnet eine neue Verbindung.

## Protokoll pro Verbindung

1. Verbindung → Prompt.
2. **Zeile für sich = Modus** (`ascii`/`binary`), danach **erste Zeile =
   Befehl** (EOL egal: CR, LF, CRLF), wampes-artig **prefix-abkürzbar**
   (`c`, `co`, `conn`, …). Bei Mehrdeutigkeit Kandidatenliste.
3. Danach **Datenmodus**: Byte-Strom läuft in die AX.25-Session
   (connectet: `'D'`-Frames; unconnectet: Framing laut Modus).
4. **SYN / ACK SYN** = AX.25-Handshake über den netd-Loop-Port
   (`'C'`/`'v'`/`'c'`-Anfrage). Die Bestätigung kommt als `'C'`-Frame
   (datakind 'C', `call_to` = eigener Call) vom netd-Loop-Port zurück;
   abgelehnt wird per `'d'`-Frame.
5. Bytes, die **im selben `read()` wie der `connect`-Befehl** ankommen,
   warten auf die Bestätigung und gehen danach durch **denselben Pfad** wie
   alles Weitere. Ein Skript wartet nicht auf eine Antwort, bevor es seine
   Nutzlast schreibt; das ist Normalfall, kein Randfall.

## Befehle

```
ascii | binary            Modus, in einer Zeile für sich, vor dem Kommando
connect  [--silent] [--keep] [--pid=XX|name] [--port <port>] [--mycall <call>]
         [<port>[/<chan>]:]<dest>[,<digi>,…] [< SRC]
datagram [--silent] [--keep] [--pid=XX|name] [--port <port>] [--mycall <call>]
         [<port>[/<chan>]:][<dest>[,<digi>,…]] [< SRC]
quit | bye        Verabschiedung, Verbindung schließt
help              Kommando-Übersicht
```

- **Port und Ziel sind ein Argument**, `<port>[/<chan>]:<dest>`. Zwei
  Argumente können nicht sagen, welches von beiden welches ist: mit einem
  Autorouter auf der Gegenseite liest sich `connect hf DB0AAA` genauso gut
  als Port `hf` nach `DB0AAA` wie als Call zu `hf` digipeated über
  `DB0AAA`. Der Doppelpunkt entscheidet es. Getrennt wird am **ersten**
  Doppelpunkt, immer — ein Rufzeichen enthält keinen, und ein Schrägstrich
  vor dem Doppelpunkt gehört zum Port (`hf/2:DB0AAA`, Kanal 2 von hf).
- Ein Kanal hinter `/` ist eine Zahl 0–15. Alles andere ist ein Fehler und
  **nicht** Kanal 0: `hf/x:DB0AAA` wird abgelehnt, statt still zu etwas
  zu werden, das funktioniert.
- Die alte Zwei-Argument-Form wird abgelehnt, mit einer Meldung, die das
  getippte Argument **wörtlich** nennt und die neue Form danebenstellt —
  so läßt sich die Meldung nach der einen Korrektur wieder einfügen.
  `connect DB0AAA DB0BBB` (Call plus Digipeater) bleibt dagegen richtig.
- Ohne Port gilt der konfigurierte `default-port`, und der netd löst den
  Digipeater-Pfad selbst auf. Ohne beides wird abgelehnt.
- `--silent`: keine `*** connected`/`*** disconnected`-Statuszeilen (für
  fd-Übergabe an Programme, die den Nachrichteninhalt erwarten).
- `--keep`: TCP bleibt nach Session-Ende offen, neuer `connect`/`datagram`
  möglich. **Default (ohne `--keep`)**: Session-Ende → Verbindung schließt
  (klassisches telnet: `telnet localhost pop3` … `quit`).
- `--pid=XX` hex bzw. `pid=name`: `text`→F0, `netrom`→CF, `flexnet`→C4,
  `rose`→C3, `l3`→CC.
- `--port <port>`: Alternativschreibweise zur Doppelpunkt-Form, für ein
  Skript, das den Port in einer Variablen hat und den Doppelpunkt als
  unbequem empfindet. Nimmt auch `hf/2` und ein führendes `:` beim Ziel.
- `--mycall <call>`: gleichwertig zu `< SRC`, zuletzt gewinnt.
- Digipeater-Pfad: Komma **und** Leerzeichen trennen, gemischt erlaubt
  (`DB0AAA-8 DB0BBB` = `DB0AAA-8,DB0BBB`), wampes-Grammatik.
- `datagram` ohne `<dest>`: jede Zeile ist ein ganzer TNC2-Rahmen
  `SRC>DEST,DIGI,…:payload` (Header aus den Daten, nicht aus dem Kommando);
  mit `<dest>`: fester Header, jede Zeile ist Payload. Der Quell-Call einer
  TNC2-Zeile steht in der Zeile; im festen Modus wie beim `connect`.
  Ein leerer Payload sendet nichts; kaputte Rahmen melden einen Fehler, auch
  unter `--silent`. Auch hier wird der Port gebraucht, sonst wird abgelehnt.
- Antworten kommen in der EOL des Clients: enden die Zeilen mit `\r`
  (CRLF oder CR allein), antwortet tcpd mit CRLF, sonst mit nacktem LF.
  Der Datenstrom selbst wird nie umgeschrieben.
- Autoroute (Digipeater-Pfad bei connect ohne Digis) löst **zentral der
  ax25netd** (siehe `autoroute` in `ax25netd_agwpe.conf(5)`); kein tcpd-Flag
  nötig.
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
# Vorderseite: Listener, einer pro Zeile
listen tcp <addr> <port>          # ein Listener, beide Modi
listen unix <path> [group <g>]    # ascii, außer der Client sagt binary

mtu <n>                           # Datagramm-Chunking, default 256
default-call <call>               # optionales < SRC für connect/datagram
default-port <port>               # Port, wenn ein Kommando keinen nennt
```

`default-port` ist **global**, wie das bereits vorhandene `default-call`: ein
Client wählt seinen Modus, aber nicht, was ein fehlender Port bedeutet. Es
ist nicht nur eine kürzere Schreibweise, es ist die Voraussetzung dafür, daß
`connect DB0AAA` überhaupt etwas ist — der netd braucht auch zum Autorouten
einen Port (`up_by_port(port)` + `ax25netd_route_lookup()`), also wäre die
Form ohne Port ohne diese Zeile toter Code. Voreingestellt nicht gesetzt, und
dann wird ein Kommando ohne Port mit einer Meldung abgelehnt, die diese Zeile
nennt, statt still über einen Port zu gehen, den niemand gewählt hat.

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
| 8200            | netd-Loop TCP, nur wenn ax25common.conf es  |
|                  | aktiviert - Default ist der Unix-Socket      |
| 8202            | ax25tcpd-Front, **ein** Listener für beide Modi |
| /var/run/ax25/sockets/ax25netd.sock | netd-Loop-Unix-Socket (Default) |
| /var/run/ax25/sockets/ax25tcpd.sock | ax25tcpd-Front-Socket (Sample)  |

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

### Backend-Anbindung (offen, Alternativen)

`call` erreicht jeden axports-Port, weil es die libax25-Socket-API nutzt und
`bind`/`connect` pro Port auf das Backend verteilen (wampes.conf → WAMPES,
sonst AGWPE, Kernel per Probe).  `ax25tcpd` ist dagegen ein reiner
AGWPE-Client: `tpc_parse_port_spec()` löst Namen nur aus der `'G'`-Tabelle
des netd auf, und dort stehen nur die `ports_ready`-AGWPE-Upstreams.
WAMPES- und Kernel-Ports (`radio0`, `bpq0`) fehlen, deshalb scheitert
`connect radio0`, während `call radio0` geht.  Einen öffentlichen
libax25-Aufruf für axports-Name → flacher Port gibt es nicht
(`axsock_port_of_entry`, `axsock_gport_channel`, `axsock_ports_fetch` sind
`static`).

1. **A (klein)**: libax25-Wrapper um `axsock_port_of_entry()` öffentlich
   machen, in `tpc_parse_port_spec()` nutzen, `'G'`-Tabelle erneuern.  Behebt
   nur AGWPE-Namen.
2. **B (groß)**: ax25tcpd auf die libax25-Socket-API umbauen (Parität zu
   `call`, Backends Kernel/WAMPES/AGWPE).  Der AGWPE-Client entfällt.
3. **C (Hybrid, bevorzugt)**: `connect` über die Socket-API (Backend pro
   Port), `datagram` über AGWPE auf allen dreien.  Der AGWPE-Client bleibt;
   UI/PID/Digis sind über `SOCK_DGRAM` + `sax25_pid` + `agwpe_sendto` /
   `wampes_sendto` ausdrückbar.

Randbedingung: der Linux-Shim (AGWPE/WAMPES) wird nur mit
`--enable-userspace-ax25` gebaut; ohne ihn geht `AF_AX25` an den Kernel und
der Userspace-Zweig fehlt.  tcpd müsste dann seinen direkten AGWPE-Pfad
behalten.

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
- `tpc_split_target()` trennt am **ersten** Doppelpunkt und setzt ihn **an Ort
  und Stelle** auf NUL, damit `tpc_parse_dest()` weiter über alle
  Positional-Argumente hinweg Kommas trennen kann. Eine Kopie in einen
  lokalen Puffer mit Rückgabe eines Zeigers war der erste Versuch und
  bricht das.
- `tpc_session_send()` ist der **eine** Weg von Client-Bytes zu `'D'`-Frames:
  TELNET-Erkennung, TELNET-Dekodierung, CR-Normalisierung, Chunking. Zwei
  Wege gab es (Read-Loop und Flush nach der Connect-Bestätigung) und die
  haben sich unterschieden; siehe Bugs.

## Tests (E2E)

- netd (Loop + Mock-Upstream) + ax25tcpd; Python-Client:
  connect+Daten+disconnect, datagram binary (Chunk bei >MTU: 700 B → 256/256/188),
  datagram ascii (Zeile→Paket), datagram TNC2 (Zeile→Rahmen), `--silent`,
  `--keep`, `--mycall`, Komma-/Leerzeichen-Pfad, EOL-Adaption (LF↔CRLF),
  Unix-Socket, `quit`/`bye`, Prefix-Abkürzungen, Fehlerpfade.
- Für die Modus-Arbeit ein **Mock-netd mit Bestätigung** (`mocknetd.py`),
  weil die ascii/binary-Unterscheidung im Bytestrom liegt und ein Mock ohne
  Connect-Bestätigung nie in `TPC_DATA` kommt. Er bestätigt `'C'`, loggt
  jeden Rahmen **vor** der Antwort und beendet die Sitzung auf dem Payload
  `BYE`, damit ein Test ohne 10-Minuten-Timeout wieder am Prompt landet.
  Nötige Details, die nicht in der Doku stehen: der AGWPE-Rahmen ist 36
  Bytes (`8B + 10s + 10s + 2u32`, Calls NUL-gepolstert); `AGWPE_DK_VERSION`
  ist `'R'` und `AGWPE_DK_PORTS` ist `'G'`, keine kleinen Nummern; die
  `'G'`-Antwort **zählt ab 1** (`"2;Port1 hf: hf radio;Port2 loop: loop;"`),
  weil `tpc_port_parse()` `n - 1` speichert — eine 0-basierte Tabelle wird
  kommentarlos verworfen und sieht aus wie ein netd, der nie antwortet.
- Testskripte liegen in
  `/private/var/folders/wc/51f1ls413b70nmmttqww4tyr0000gn/T/opencode/`
  (`mocknetd.py`, `drive.py`, `test_tpc.py`; das früher genannte `e2e/`
  existiert nicht mehr). Zwei ax25tcpd-Instanzen auf 18202/18203, weil
  `default-port` und dessen Fehlen **beide** getestet werden müssen und
  eine Instanz nicht beides kann.
- Gefundene/behobene Bugs in der E2E-Phase:
  - Binäres Datagramm: Payload im selben TCP-Segment wie die Kommandozeile wurde
    als Zeilenrest in `rbuf` sortiert statt in `dbuf` → nach Befehlswechsel zu
    `TPC_DGRAM` Rest aus `rbuf` in `dbuf` übernehmen.
  - Dasselbe für `connect`: `TPC_CONNECTING` war in der Zeilenschleife gar
    nicht behandelt, also fielen die Bytes **stillschweigend auf den Boden**.
    Ein Skript, das Befehl und Payload in einem `write()` schickt, bekam
    eine Sitzung, die ihr erstes Paket verschluckt. Die Schleife nimmt jetzt
    nach einem Kommando, das den Zustand wechselt, die übrigen Bytes in
    `dbuf` — für binäres Datagramm **und** für Connect.
  - `tpc_split_target()` dereferenzierte `port_end` im Zweig ohne Doppelpunkt
    (NULL) → **Segfault** bei jedem `connect` ohne Port, also genau bei der
    `default-port`-Form. Mit zwei Argumenten fiel es nie auf.
  - Der Flush nach der Connect-Bestätigung schickte `cl->dbuf` **roh**:
    ohne CR-Normalisierung, ohne TELNET-Dekodierung. Das erste Paket einer
    Sitzung ging mit nacktem LF raus, wo alle folgenden ein CR hatten, und
    mit noch verdoppelten IAC-Paaren.
  - Die TELNET-Erkennung hat den **ganzen** `read()`-Puffer gescannt und
    damit rückwirkend entschieden: ein Skript, das `binary` und Payload in
    einem `write()` schickt, bekam „binary is too late" für ein Kommando,
    das beim Tippen noch legal war. Die Erkennung läuft jetzt in
    Datenstrom-Reihenfolge, in `tpc_session_send()`.
  - Die CR-Normalisierung brauchte ein Feld auf dem Client (`ascii_cr`):
    ein über zwei Chunks geteiltes CRLF wurde zu CR CR und damit zu einer
    Leerzeile auf der Gegenseite. Zählt auch einen **getippten** CR, sonst
    wird `one\r\n` zu zwei CR.
  - `tpc_on_disconnect` ignoriert `TPC_DGRAM`-Clients (Datagramm hat keine
    Verbindung; ein verspäteter `'d'`-Echo darf die Session nicht beenden).
  - netd benennt Upstream-Ports nach seiner conf (`radio`), nicht nach dem
    Selbstname des Upstreams — Port-Lookup beim ersten Test schlug fehl.

## Phase 2 / offene Punkte

- Login/Auth am ax25tcpd-Listener (default off, bind loopback); Userverwaltung
  wird später auch die AGWPE-Authentifizierung am netd-Port stellen (libax25
  muss das können — `agwpe_client_login` existiert bereits).
- `param window=…/paclen=…` via `'Q'`.

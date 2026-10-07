[English](README.md) · **Deutsch**

# LoRaITP — LoRa Image Transfer Protocol

**Ein Foto 30 Kilometer weit schicken, einmal am Tag, mit einem Akku — legal.**

LoRaITP ist ein schlankes Protokoll, das eine ganze Bilddatei über eine
einzelne LoRa-Strecke überträgt. Es ist für den Fall gebaut, in dem die
Funkstrecke die knappste Ressource im System ist: ein Kameraknoten mit
Solar- oder Akkubetrieb irgendwo ohne Mobilfunk, eine Basisstation in
einigen Dutzend Kilometern Entfernung und ein regulatorisches
Sendezeitbudget, das in Sekunden pro Stunde gemessen wird.

Es läuft direkt auf dem LoRa-PHY — kein LoRaWAN, kein Netzwerkserver,
kein Gateway. Ein Sender und ein Empfänger, die sich auf eine Frequenz
einigen, genügen.

> **Stand: Alles baut und besteht die Tests; nichts war bisher auf
> Sendung.** Die Spezifikation ist vollständig, und jede Zahl darin ist
> berechnet statt geschätzt. Eine Python-Referenzimplementierung fährt
> vollständige Übertragungen gegen einen simulierten Kanal, ein
> portabler C-Kern besteht 109 Prüfungen gegen daraus erzeugte Vektoren,
> und die Firmware baut für alle drei ESP32-Ziele — 44 % Flash und 40 %
> RAM auf dem knappsten davon. Der nächste Schritt sind zwei Boards auf
> dem Schreibtisch, und
> **[docs/bench-loopback.md](docs/bench-loopback.md)** beschreibt das
> Vorgehen dafür. Siehe auch [den Fahrplan](#fahrplan).

Die verlinkten Dokumente unter `docs/` und die Spezifikation gibt es
bisher nur auf Englisch.

---

## Warum es das gibt

LoRa ist langsam — ein paar hundert Bit pro Sekunde bei großer
Reichweite — und in Europa darf man nur etwa 1 % der Zeit senden. Der
übliche Schluss daraus: Bilder kommen nicht in Frage.

Dieser Schluss ist falsch, und zwar deutlich. Nachgerechnet:

| Bild | Spreizfaktor | Reine Sendezeit | Dauer auf einem 10-%-Band |
|---|---|---|---|
| 5 kB | SF12 (max. Reichweite) | 3,6 min | 36 min |
| 10 kB | SF12 (max. Reichweite) | 7,2 min | 72 min |
| 10 kB | SF10 | 1,8 min | 18 min |
| 20 kB | SF10 | 3,6 min | 36 min |

Ein 10-kB-JPEG in Graustufen kostet bei maximaler Reichweite **7 Minuten
Sendezeit und 24 mAh**. Das sind 5 % des legalen Tagesbudgets auf dem
869,4-MHz-Band und etwa 125 Bilder aus einer einzigen 3000-mAh-Zelle.

Ein Bild am Tag ist nicht knapp. Es ist bequem.

*(Jede Zahl hier stammt aus [`tools/airtime.py`](tools/airtime.py); das
Skript implementiert die Time-on-Air-Formel von Semtech und stimmt mit
veröffentlichten LoRaWAN-Referenzwerten auf 0,5 ms überein. Rechne es
selbst nach.)*

---

## Der Entwurf auf einer Seite

**Ein 4-Byte-Header auf Datenpaketen.** Der LoRa-PHY liefert bereits
eine CRC, einen expliziten Längenheader und ein Sync-Wort. LoRaITP
wiederholt nichts davon. Ein Datenpaket trägt eine Sitzungs-ID und einen
16-Bit-Chunk-Index, sonst nichts — 2 % Overhead bei Chunks von 196 Byte.

**Bitmap-NACK statt ACK pro Paket.** Der Empfänger schweigt einen ganzen
Block lang und meldet dann als Bitmap, welche Chunks fehlen: 32 Byte
decken 256 Pakete ab. Eine saubere 10-kB-Übertragung kostet genau einen
Hin- und Rückweg, nicht fünfzig.

**Ein Duty-Cycle-Wächter, der Teil des Protokolls ist.** Jede Aussendung
wird vorher von einer Budgetverwaltung freigegeben. Es gibt keinen
Codepfad, der sendet, ohne zu fragen. Die regionalen Profile stammen aus
der veröffentlichten Frequenzzuteilung — für Deutschland BNetzA
Vfg. 91/2025 — statt aus Hörensagen, und der Standard ist `EU868_G3`
(869,4–869,65 MHz, Zeile 54): **zehnmal so viel Sendezeitbudget und
13 dB mehr Leistung** als die LoRaWAN-Standardkanäle, zu denen die
meisten Projekte greifen, und ohne Bandbreitenbeschränkung, die im Weg
steht.

**Ein Amateurfunkmodus, der seine Pflichten ernst nimmt.** Lizenzierte
Funkamateure können die Duty-Cycle-Grenze aufheben. Im Gegenzug
*erzwingt* der Stack, was der Amateurfunkdienst verlangt: Ohne
eingetragenes Rufzeichen verweigert er das Senden, Kennungsrahmen werden
bei langen Übertragungen automatisch eingefügt, und die Verschlüsselung
wird abgeschaltet und lässt sich nicht wieder einschalten.

**Ein Rundsendemodus für Strecken ohne Rückkanal.** Der Empfänger kann
vielleicht nicht senden, oder will nicht, oder es gibt viele davon. Der
Sender schickt dann das Bild samt Reed-Solomon-Parität und erfährt nie,
was angekommen ist — und der Empfänger kann *exakt*, nicht heuristisch,
entscheiden, ob das Bild noch wiederherstellbar ist, und schaltet in dem
Moment ab, in dem es das nachweislich nicht mehr ist. Das Bild
stattdessen dreimal zu senden kostet dieselbe Sendezeit und scheitert
bei 20 % Paketverlust in 34 % der Fälle, während der Code praktisch nie
scheitert.

**Bilder, die schlechter werden, statt zu scheitern.** JPEG-Restart-Marker
geben einem Decoder eine Stelle, an der er nach einem verlorenen Paket
wieder aufsetzen kann. Gemessen: 300 verlorene Bytes beschädigen
**16 von 240 Zeilen** mit Markern gegenüber **72 ohne**, bei 1,1 %
größerer Datei. Eine optionale Vorschaustufe sendet zuerst ein
80×60-Vorschaubild, sodass auch eine zusammenbrechende Strecke noch
etwas liefert.

**Adaptiver Spreizfaktor.** Eine Messung von 1,3 Sekunden vermisst die
Strecke, und die Sitzung wählt die schnellste Einstellung mit 6 dB
Reserve. Jede SF-Stufe verdoppelt die Sendezeit; das richtig zu treffen
ist also mehr wert als jede andere einzelne Optimierung.

Alle Einzelheiten in **[SPEC.md](SPEC.md)**.

---

## Erste Schritte

> **Zuerst lesen.** Das Protokoll ist fertig und gründlich getestet, und
> die Firmware baut für alle drei ESP32-Boards — aber **noch nie wurde
> ein Board eingeschaltet**. Du wärst die erste Person. Rechne damit,
> etwas debuggen zu müssen, und sieh dann unter
> [Wenn nichts ankommt](#wenn-nichts-ankommt) nach.

Du brauchst zwei Boards. Auf beiden läuft **dieselbe Firmware** —
welches Ende der Strecke ein Board ist, ist eine Einstellung, kein
eigener Build.

### Was du kaufen musst

| Rolle | Board | Hinweis |
|---|---|---|
| Kameraknoten | **Seeed XIAO ESP32S3 Sense** + **Wio-SX1262 for XIAO** | das Sense bringt die Kamera mit |
| Basisstation | **Heltec WiFi LoRa 32 V3 oder V4** | an diesem Ende ist keine Kamera nötig |

Besorge Antennen für das richtige Band und **schraube sie an, bevor du
irgendetwas einschaltest**. Senden ohne Antenne kann das Funkmodul
beschädigen.

> Eine Falle, die man kennen sollte: Seeed verkauft zwei Boards namens
> *Wio-SX1262 for XIAO*. Die **Kit**-Version steckt im flachen Verbinder
> unter dem XIAO — demselben, den die Kamera benutzt, und sie teilt sich
> auch GPIOs mit der Kamera, lässt sich also nicht mit dem Sense
> kombinieren. Du willst die Version mit zwei 7-poligen Buchsenleisten,
> in die das XIAO von oben gesteckt wird.

### Den Kameraknoten verdrahten

Wenn das XIAO mit aufgesteckter Kameraplatine in die Buchsen des
Funkboards passt, steck es einfach ein — fertig. Wenn es mechanisch
kollidiert, nimm stattdessen Steckbrücken; die Verdrahtung ist in beiden
Fällen gleich:

| Funkboard | XIAO-Pin |
|---|---|
| 3V3, GND | 3V3, GND |
| MOSI, MISO, SCK | D10, D9, D8 |
| DIO1 | D1 |
| RST | D2 |
| BUSY | D3 |
| NSS | D4 |
| RF_SW | D5 |

**Lass den microSD-Slot leer.** Die Reset-Leitung des Funkmoduls landet
auf demselben Pin wie der Chip-Select der Karte, und eine Karte im Slot
verfälscht jeden Lesezugriff auf das Funkmodul. Bilder werden im
eigenen Flash des Boards gespeichert — es passen mehrere hundert — die
Karte wird also nicht gebraucht.

### Flashen

Öffne **[die Flasher-Seite](https://technikweber.github.io/LoRaITP/flash/)**
in Chrome, Edge oder Opera, schließe das Board per USB an, wähle dein
Board und klicke auf Install. Das ist der ganze Vorgang — es muss keine
Software installiert werden.

Firefox und Safari können das nicht: Flashen über USB braucht eine API,
die beide nicht implementieren wollen. Es gibt nichts einzuschalten, du
brauchst einen anderen Browser.

Wenn das Board nicht in der Liste der Ports erscheint, halte beim
Einstecken des USB-Kabels dessen BOOT-Taste gedrückt.

### Erster Start

> Machst du das zum ersten Mal, mit zwei Boards, die noch nie gesendet
> haben? Dann folge stattdessen
> **[docs/bench-loopback.md](docs/bench-loopback.md)**. Das Dokument
> deckt dasselbe der Reihe nach ab, sagt, mit welchem Boardpaar man
> anfängt und warum, und listet auf, was zu prüfen ist, wenn nichts
> ankommt.

1. Verbinde dich am Handy oder Laptop mit dem WLAN **`LoRaITP-XXXX`**,
   das das Board aufspannt.
2. Öffne **`http://192.168.4.1`**. Du siehst die Bildergalerie, die
   Statusseite und die Einstellungen.
3. Stelle am **Kameraknoten** die Rolle auf **Sender**. Lass das andere
   Board auf **Receiver**. (Ein Board mit Kamera steht schon von sich
   aus auf Sender, du musst also vielleicht gar nichts ändern. Zwei
   Boards ohne Kamera stehen beide auf Receiver, und einem davon muss
   man es anders sagen.)
4. Warte. Der Sender sendet etwa drei Sekunden nach jedem Start, die
   Galerie des Empfängers sollte also innerhalb weniger Minuten ein
   Bild zeigen. Der Empfänger hört durchgehend zu und braucht keinen
   Anstoß.
5. Der Zeitplan steht ab Werk auf **eine Übertragung alle 24 Stunden** —
   der Fall „ein Bild am Tag", um den es in der Sendezeittabelle oben
   geht. Eine erste Übertragung läuft wenige Sekunden nach dem Start,
   du wartest also keinen Tag, um zu sehen, ob die Strecke
   funktioniert; nutze danach **Send now** auf der Statusseite, statt
   das Intervall zu verkürzen. Geändert wird es unter **One transfer
   every _n_ seconds / minutes / hours** auf der Einstellungsseite des
   Senders; dort steht auch eine Schätzung, wie lange eine Übertragung
   mit den aktuellen Einstellungen dauert, damit man das Intervall mit
   dieser Zahl vor Augen wählen kann. Die Periode zählt ab Beginn einer
   Übertragung, nicht ab deren Ende — ein Bild, dessen Übertragung
   72 Minuten dauert, schiebt das nächste also nicht nach hinten.

### Betrieb am Akku

Zwischen den Übertragungen wartet der Sender mit laufendem Access
Point, was 100–150 mA kostet — mehr, als das Funkmodul beim Senden
braucht, und weit mehr als alles andere zusammen. **Deep sleep between
transfers** auf der Einstellungsseite schaltet das Board stattdessen ab.
Es ist ab Werk aus, wegen dem, was es dich kostet:

* **Der Access Point ist weg, bis du RESET drückst.** Ein Tiefschlaf
  endet in einem Neustart; das Board wacht auf, sendet und schaltet
  wieder ab, ohne je das WLAN hochzufahren. Genau das ist der Zweck —
  sonst gäbe der Schlaf das meiste von dem zurück, was er gespart hat.
  Jeder Start, der kein geplantes Aufwachen ist, bringt die Seite ganz
  normal hoch, RESET ist also der Weg zurück hinein.

  Auch ohne Tiefschlaf verschwindet der Access Point **fünf Minuten
  nach der letzten Anfrage**, und RESET bringt ihn zurück; dieselbe
  Taste deckt also beide Fälle ab, und sonst gibt es nichts zu drücken.
  Die Frist läuft ab der letzten Anfrage, nicht ab dem Start — er
  verschwindet nicht, während du ihn noch benutzt. *Stay on* unter
  **Access point** ist die Einstellung für den Labortisch.
* **Er wird übersprungen, wenn die Pause zu kurz ist, um sicher zu
  sein.** Das rollende Fenster des Duty-Cycle-Wächters liegt im RAM und
  überlebt den Neustart nicht; ein Board, das einen Teil davon
  verschlafen hat, würde also in dem Glauben aufwachen, sein ganzes
  Stundenbudget sei unangetastet — auf einem 1-%- oder 10-%-Band ist
  das ein Verstoß, kein Rundungsfehler. Auf einem Band mit Grenze
  schläft die Firmware deshalb nur, wenn sie eine Stunde später noch
  schläft; bis dahin ist ohnehin alles, was sie gesendet hat, aus dem
  Fenster herausgealtert. Auf einem Band ohne Grenze gibt es nichts zu
  verlieren, und sie schläft immer. Eine kürzere Pause wird wach
  verbracht, und das Log sagt das auch.

Tiefschlaf ist eine Einstellung auf der Senderseite. Ein Empfänger muss
durchgehend zuhören — siehe den Hinweis zu gemeinsamen Uhren weiter
unten.

Die Boards starten auf **869,85 MHz mit 5 mW** — dem Teilband, das gar
keine Sendezeitgrenze hat; du kannst also so viel experimentieren, wie
du willst, ohne Budget zu verbrauchen und ohne Lizenz. Die Reichweite
ist kurz. Sobald es funktioniert, stelle beide Boards auf der
Einstellungsseite auf `EU868_G3` um, für 500 mW und echte Entfernung,
und lies vorher [docs/duty-cycle.md](docs/duty-cycle.md).

### Wenn nichts ankommt

**Ändere zuerst die Einstellung des Antennenschalters.** Wähle auf der
Einstellungsseite unter *Antenna switch* die andere Option und
speichere. Das ist die mit Abstand wahrscheinlichste Ursache: Das
Funkmodul hat einen Pin, der seine Antenne zwischen Senden und
Empfangen umschaltet, und wie herum er arbeitet, ist für dieses Modul
nicht dokumentiert. Steht er falsch, sendet das Board in eine Sackgasse
und hört nichts — was genauso aussieht wie außer Reichweite. Umschalten
kostet einen Fingertipp; es auf anderem Weg auszuschließen kostet einen
Nachmittag.

Wenn das nicht hilft:

- Sind beide Boards auf **derselben Frequenz und demselben
  Spreizfaktor**? Prüfe die Statusseite auf beiden.
- Sind die **Antennen** angeschlossen?
- Steht ein Board auf **Sender** und das andere auf **Receiver**?
- Schließe den Sender per USB an und öffne einen seriellen Monitor mit
  115200 Baud. Er gibt aus, was er tut, einschließlich jeder
  Konfiguration, die die Firmware abgelehnt hat, und warum.

### Ein Hinweis zu den Funkregeln

Die Firmware lässt dich nicht außerhalb der Grenzen der gewählten Region
senden — falsche Frequenz, zu viel Leistung oder Amateurfunkmodus ohne
Rufzeichen, und sie verweigert und sagt das, statt zu senden. Das ist
Absicht. Es ist allerdings eine Hilfe, keine Garantie: Sie kann weder
deinen Antennengewinn noch deine örtlichen Regeln kennen, und legal zu
bleiben bleibt deine Sache.

Die regionalen Profile entsprechen der veröffentlichten deutschen
Zuteilung; das ist der richtige Standard und der falsche, wenn die
Tabellen nicht auf dich zutreffen — eine Lizenz, die mehr erlaubt, ein
anderes Land, eine Schirmkammer. Der **Expert mode** auf der
Einstellungsseite schaltet das Profil `LOCAL` frei und hört auf,
Frequenz und Leistung an eine europäische Zeile zu binden.

Er schaltet den Wächter nicht ab, und genau darum geht es: Unter
`LOCAL` trägst du den Duty Cycle ein, den deine eigenen Regeln
vorgeben, und er wird genauso durchgesetzt wie ein veröffentlichter —
dieselbe rollende Stunde, dieselbe Sendepause, dieselbe Verweigerung,
sobald das Budget aufgebraucht ist. Es gibt weiterhin keinen Codepfad,
der sendet, ohne zu fragen; es ändert sich nur, wen die Firmware fragt.
Schaltest du den Expert mode wieder aus, steht das Board wieder auf
`EU868_G4_LP` mit 5 mW; der Schalter ist also keine Falltür.

## Aufbau des Repositorys

```
SPEC.md          das Format auf der Leitung — normativ
tools/           Rechner; jede Zahl in der Doku stammt von hier
sim/             Python-Referenzimplementierung + Kanalsimulator
src/             portabler C-Kern   <- keine Plattform-Header, kein malloc
port/            Plattformanbindung <- keine Protokolllogik
                 Funk (RadioLib) und Speicher (LittleFS)
firmware/        app/ (ein Binary, beide Rollen), boards/ (Pinbelegungen)
tests/           Kern gegen den Simulator, auf dem Host
docs/            Hardware, Duty Cycle, Kamera, Speicher, Inbetriebnahme
```

Die Grenze zwischen `src/` und `port/` ist die, auf die es ankommt, und
sie wird durch die Verzeichnisstruktur erzwungen statt durch
Konvention — siehe [CONTRIBUTING.md](CONTRIBUTING.md). Sie sorgt dafür,
dass derselbe Objektcode des Kerns in den Tests, im Simulator und auf
der Hardware läuft.

## Werkzeuge

Drei Rechner und eine Konsistenzprüfung, ohne Abhängigkeiten über die
Python-Standardbibliothek hinaus. `check_flash_manifest.py` fällt aus
der Reihe: Es vergleicht die Manifeste des Web-Flashers mit den
Partitionstabellen, weil beide dasselbe Flash-Layout in zwei Dateien
beschreiben, die sonst nichts verbindet.

```console
$ python3 tools/airtime.py
== Time on air per packet  (BW 125 kHz, CR 4/5, 8 sym preamble, CRC on)
PL(B) |   SF7   |   SF8   |   SF9   |   SF10  |   SF11  |   SF12
   200 |  318 ms |  564 ms |  1.00 s |  1.85 s |  4.02 s |  7.22 s
...

$ python3 tools/linkbudget.py --dist 30 --freq 868 --erp 27
  -> received level        -87.6 dBm
  12 |   -137.0 dBm |   49.4 dB  OK

  1st Fresnel zone radius at midpoint     50.9 m
  earth bulge at midpoint (k=4/3)         13.2 m

$ python3 tools/fec_compare.py
  loss | P(fail) repetition | P(fail) erasure code
    20% |            34.142% |                    0
```

Die zweite Ausgabe ist das Nützlichste in diesem Repository. Über 30 km
geht die Streckenbilanz mit **50 dB Reserve** auf — Leistung ist nicht
das Problem. Die erste Fresnelzone ist in der Mitte 51 Meter breit, und
die Erde wölbt sich 13 Meter in den Pfad. Stecke deinen Aufwand in
Antennenhöhe und freie Sichtlinie, nicht in einen größeren Verstärker.

---

## Die Tests ausführen

```console
$ python3 sim/selftest.py     # 68 checks: crypto vectors, erasure coding,
                              # governor rules, feasibility property tests
$ python3 sim/run.py          # 12 full transfers against simulated loss
$ python3 tools/check_flash_manifest.py   # flasher vs. partition tables
$ cd tests && make run        # 109 checks on the C core
$ cd tests && make san        # the same, under ASan and UBSan
$ cd tests && make port       # 24 checks on the RadioLib adapter
$ cd tests && make store      # 37 checks on the image store, real files
$ cd tests && make jpeg       # 13 checks on the JPEG encoder
$ cd tests && make boards     # 29 checks on the board pin maps
$ ./tests/test_jpeg /tmp/s && python3 tests/verify_jpeg.py /tmp/s
```

Keine Abhängigkeiten, keine Hardware, kein Warten — eine Übertragung,
die auf einem Band mit 1 % Duty Cycle vier Stunden dauert, läuft hier in
unter einer Sekunde, mit demselben Zustandsautomaten. Zusammen haben
diese Tests vier echte Fehler gefunden: zwei in der Spezifikation, einen
im C-Kern, einen in der Python-Referenz.
[`sim/README.md`](sim/README.md) sagt, welche.

### Wo die Lücken sind

Der Protokollkern, die Funkanbindung, der Bildspeicher, der JPEG-Encoder
und die Pinbelegungen der Boards haben alle Tests. **Die
Anwendungsschicht der Firmware — die Schleife in
`firmware/app/main.cpp` — hat keine**, weil sie sich ohne Hardware nicht
ausführen lässt.

Das ist von Belang, denn drei echte Fehler, die bei der Inbetriebnahme
gefunden wurden, lagen genau dort, und keiner war für einen Compiler
oder die vorhandenen Tests sichtbar: Der Kern war korrekt und die
Anwendung war korrekt, und sie waren falsch miteinander verbunden.

* Das Duty-Cycle-Fenster wurde vor jeder Übertragung neu aufgebaut,
  sodass jede Sitzung glaubte, das ganze Stundenbudget sei
  unangetastet. Auf einem Band ohne Grenze ist das unsichtbar; auf
  einem 1-%- oder 10-%-Band ist es ein Verstoß. Dafür gibt es jetzt
  einen Regressionstest.
* Der Empfänger wandte das Intervall des Senders auf sich selbst an und
  wurde zwischen den Hörfenstern taub, ohne gemeinsame Uhr, die sagen
  könnte, wohin die Lücke fällt.
* Die Statusseite fragte das Duty-Cycle-Fenster vom anderen Kern aus
  ab, während das Funkmodul es gerade schrieb.
* Der Web-Flasher schrieb die Anwendung an die Arduino-Standardadresse,
  während die Partitionstabelle sie woanders ablegte, sodass ein
  geflashtes Board nicht starten konnte. Alles baute, jeder Test
  bestand, der Flasher meldete Erfolg. `tools/check_flash_manifest.py`
  vergleicht die beiden jetzt bei jedem Push.
* Eine Funkeinstellung, die der Treiber ablehnte — 27 dBm, wozu die
  ERP-Grenze von 500 mW auf `EU868_G3` einlädt und was der SX1262 nicht
  kann — beendete die Firmware, bevor der Webserver startete, sodass
  sich das nur per USB-Kabel rückgängig machen ließ. Eine Konfiguration
  darf nicht das lahmlegen können, womit man die Konfiguration
  bearbeitet.
* Das Duty-Cycle-Fenster ging bei jedem Neustart verloren, und es gibt
  vier Wege zu einem Neustart: eine geänderte Einstellung, ein
  Tiefschlaf, ein Watchdog, ein Spannungseinbruch. Es überlebt sie
  jetzt; siehe [docs/duty-cycle.md](docs/duty-cycle.md).

Wenn sich also beim ersten Einschalten etwas seltsam verhält, ist diese
Schicht die wahrscheinlichste Stelle, nicht das Protokoll.

## Fahrplan

- [x] Protokollspezifikation v0.1
- [x] Rechner für Sendezeit, Duty Cycle, Energie und Streckenbilanz
- [x] Repository-Gerüst und die Grenze zwischen Kern und Port
- [x] Python-Referenzimplementierung und Kanalsimulator — 68 Prüfungen,
      12 Übertragungsszenarien, keine Hardware
- [x] Portabler C-Kern — 109 Prüfungen, ohne Warnungen, sauber unter
      den Sanitizern, 3,9 kB Kontext und keine Allokation
- [x] `port_radiolib.cpp` und Pinbelegungen — ein Adapter für alle vier
      Boards, 19 Vertragsprüfungen gegen eine gemockte RadioLib
- [x] **Baut gegen die echte RadioLib und den Arduino-Core** — drei
      ESP32-Ziele, grün in der CI
- [ ] Die XIAO-Pinnummern von einem Board bestätigt, das tatsächlich
      funktioniert
- [ ] **Loopback auf dem Labortisch auf `EU868_G4_LP`** (5 mW, kein Duty
      Cycle: kein verbranntes Budget, keine Lizenz nötig) — die Firmware
      ist geschrieben und baut; sie ist noch nie gelaufen. Das Vorgehen
      steht in [docs/bench-loopback.md](docs/bench-loopback.md)
- [x] LittleFS-Bildspeicher — Ringpuffer, Statistik in Begleitdateien,
      Wiederaufnahme nach einer unterbrochenen Übertragung
- [x] Kamerapipeline — Graustufenaufnahme, Software-JPEG mit
      Restart-Markern, gegen einen unabhängigen Decoder verifiziert
- [x] WLAN-Access-Point und Weboberfläche — Galerie, Sendezeitbudget
      live und die Einstellungen, auf die es im Feld ankommt
- [x] Zeitplan und Tiefschlaf — eine Übertragung alle _n_ Stunden, und
      dazwischen ist das Board abgeschaltet
- [x] Ein Duty-Cycle-Fenster, das einen Neustart überlebt — nach jeder
      Aussendung in den RTC-Speicher gespiegelt, sodass ein Neustart,
      ein Schlaf oder ein Watchdog der Station ihr Budget nicht
      zurückgibt
- [x] Web-Flasher und CI
- [ ] Web-Flasher auf GitHub Pages (WebSerial für die ESP32, UF2 für
      den nRF52840)
- [ ] Kamera- und Bildpipeline: Graustufenaufnahme, Software-JPEG mit
      an Chunks ausgerichteten Restart-Markern
- [ ] Reichweitenversuch in der Praxis und eine gemessene
      Einstellungstabelle

## Stand der Technik

Kein einzelner Mechanismus hier ist neu, und
[`docs/prior-art.md`](docs/prior-art.md) sagt das im Detail, Behauptung
für Behauptung.
[`pmanzoni/loractp`](https://github.com/pmanzoni/loractp) ist der
nächste Nachbar und lesenswert. Wirklich ungewöhnlich scheint zu sein,
das regulatorische Budget als normativen Teil des Protokolls zu
behandeln statt als Problem des Betreibers — alles andere ist die
sorgfältige Anwendung gut verstandener Ideen.

## Mitwirken

Die Spezifikation ist ein Entwurf, und Anmerkungen dazu sind im Moment
wertvoller als Code. Eröffne ein Issue.

## Lizenz

[MIT](LICENSE).

## Haftungsausschluss

LoRaITP kann weder die Lizenz von irgendjemandem prüfen noch dessen
örtliche Vorschriften. Die regulatorischen Profile sind technische
Hilfen, die den regelkonformen Weg zum einfachen machen sollen. Die
Verantwortung für den rechtmäßigen Betrieb liegt vollständig beim
Betreiber.

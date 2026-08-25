# Target boards, the camera, and flashing

## Is the code ready to flash?

**No.** What exists is the protocol, not a product.

| Layer | State |
|---|---|
| Specification | complete, v0.1 draft |
| Python reference + channel simulator | 68 checks, 12 scenarios |
| Portable C core | 81 checks, warning-free, sanitizer-clean |
| `port_sim.c` — in-memory radio for tests | done |
| `port_radiolib.cpp` — SX1262 on ESP32 and nRF52840 | written, 19 contract checks; never built against real RadioLib |
| `loraitp_store.c` — LittleFS image store | written, 37 checks against real files |
| `jpeg.c` + `camera.cpp` — grayscale capture and encode | written; encoder verified against an independent decoder |
| `webui.cpp` — access point, gallery, settings | written |
| `main.cpp` — schedule, roles, pin maps | written |
| **A build against the real toolchain** | **in progress** |
| **Anything on actual hardware** | **not done** |

Everything above the radio is written and tested on a host. What has not
happened is a board being switched on, and until that has, treat the
firmware as a well-argued draft rather than something that works.

## The boards

Numbers from `python3 tools/storage.py`.

| Board | MCU | Flash | RAM | Camera | WiFi | Images (8 kB) |
|---|---|---|---|---|---|---|
| Heltec WiFi LoRa 32 **V3** | ESP32-S3FN8 | 8 MB | 512 kB SRAM | no | yes | 472 |
| Heltec WiFi LoRa 32 **V4** | ESP32-S3R2 | **16 MB** | 512 kB + **2 MB PSRAM** | no | yes | **1435** |
| **XIAO ESP32S3 Sense** | ESP32-S3R8 | 8 MB | 512 kB + **8 MB PSRAM** | **yes, included** | yes | 472 |
| XIAO nRF52840 + Wio-SX1262 | nRF52840 | 1 MB + 2 MB QSPI | 256 kB | no | **no** | 240 |

Storage is a non-issue everywhere. Five images is 0.35–2 % of the image
store on every board, and flash wear is thousands of years out.

### Heltec V4 is a genuine upgrade

Same form factor and pin layout as V3, but 16 MB flash, 2 MB PSRAM and
up to 28 dBm on the high-power version. The PSRAM matters if the board
ever has to encode an image; the flash doubles the archive.

**One thing to watch: 28 dBm exceeds the 27 dBm ERP limit of `EU868_G3`.**
The governor already refuses a configuration above a region's limit
(`LORAITP_E_POWER`), so this is caught rather than shipped — but the
board can physically transmit illegally on that band, and antenna gain
counts towards ERP as well.

### The nRF52840 is the odd one out

No WiFi means no access point, so images have to leave it another way —
USB, BLE, or NFC for a handshake. No camera interface at all. What it
does have is a 5 µA deep sleep and a Cortex-M4, which makes it a good
low-power *relay* or a pocket receiver, and a poor camera node.

It also validates the architecture: the C core is plain C99 with no
platform headers, so the nRF52840 needs a new `port/`, not a new core.

## The camera: what to actually buy

**Yes — there is a ready-made plug-on module, and it is the one you
already had your eye on.**

The **Seeed XIAO ESP32S3 Sense** ships with an OV2640 on a small
daughterboard that clicks onto a board-to-board connector on the
underside of the XIAO. No wiring, no 15 GPIOs, no 20 MHz parallel bus
over jumper leads. The same socket takes an OV5640 if you later want
better optics. An SD slot comes with it.

That solves the problem I raised in [camera.md](camera.md) — wire the
radio to a camera board rather than a camera to a radio board — because
Seeed already did the hard half.

### The catch, and it is worse than a connector clash

**The camera and the "XIAO ESP32S3 & Wio-SX1262 Kit" collide at the GPIO
level, not just at the socket.** The Kit's B2B wiring is:

| Signal | GPIO | What else uses it on a Sense |
|---|---|---|
| NSS | 41 | PDM microphone data |
| RESET | 42 | PDM microphone clock |
| BUSY | 40 | **camera DVP** |
| DIO1 | 39 | **camera DVP** |
| RF switch | 38 | **camera DVP** |

Three of the five are camera data lines. No amount of rewiring saves
that combination.

**The good news, and it corrects an earlier worry in this file:** the
Sense camera does not touch a single *edge* pin. Its DVP bus lives on
GPIO 10–18, 38, 39, 40, 47 and 48, all of which reach the B2B connector
and nowhere else. Every one of D0..D10 is free for the radio.

So the fix is simply to wire the radio to the edge pins instead — with
the non-Kit board, or with jumper leads. SPI at a few megahertz over
flying leads is entirely reasonable engineering. (The camera would not
be: 20 MHz parallel. Which is precisely why it stays on the B2B where it
belongs.)

Seeed sells two different boards under the name *Wio-SX1262 for XIAO*:

* the **Kit** version, which connects over the B2B connector — this is
  the one Meshtastic and RNode support, and the one that cannot coexist
  with the camera;
* the **non-Kit** version, which connects over ordinary soldered pin
  headers, and therefore leaves the B2B connector free.

So the camera node is:

> **XIAO ESP32S3 Sense** (camera on the B2B connector)
> **+ Wio-SX1262 wired to the edge pins** — the non-Kit board, or the Kit
> board reached with jumper leads

**Confirmed** against the silkscreen of an actual Wio-SX1262 for XIAO —
the plug-on variant with two 7-pin sockets. Read from underneath it says
`VIN GND 3V3 MOSI MISO SCK D7` down one side and
`D0 DIO1 RST BUSY NSS RF_SW D6` down the other, which flips to:

| Signal | XIAO pin | GPIO | Note |
|---|---|---|---|
| MOSI | D10 | 9 | SPI bus, shared with the Sense SD card |
| MISO | D9 | 8 | " |
| SCK | D8 | 7 | " |
| DIO1 | D1 | 2 | |
| **RST** | **D2** | **3** | **also the Sense SD card's chip select** |
| BUSY | D3 | 4 | |
| NSS | D4 | 5 | |
| **RF_SW** | **D5** | **6** | **must be driven by the host** |

D0, D6 and D7 pass through unused; D6/D7 are the serial console, so D0
(GPIO1) is the one free edge pin — enough for the button that raises the
access point.

**`RF_SW` settles the question that mattered most.** This module does not
steer its antenna switch from DIO2 internally; it expects GPIO6 to be
driven. Configured wrongly, the radio transmits into a matched load and
hears nothing, which is indistinguishable from being out of range. The
port drives it high to transmit — the convention, not a measurement, so
if the link works in one direction only that is the first thing to
invert.

**`RST` on GPIO3 collides with the Sense SD card's chip select**, on the
same SPI bus. With a card fitted, every radio reset would select the SD
card and it would drive MISO. Leave the slot empty — images live in
internal flash — or move `RST` to D0 with a jumper.

### Will it physically stack?

Electrically it is settled either way. Mechanically depends on whether
the Sense expansion board passes the XIAO's edge pins through: the camera
sits under the XIAO on the B2B connector, and the radio board's sockets
want that same space. If the three do not stack, jumper leads from the
radio board's sockets to the XIAO's pins give exactly the same wiring —
SPI at a few megahertz over flying leads is fine.

If you would rather keep everything on a Heltec, the alternative is an
**ArduCam Mini 2MP Plus**, an OV2640 behind an SPI interface — about six
pins instead of fifteen. It costs more than the whole XIAO Sense, and it
is slower to read out, which does not matter at all against a forty-minute
transmission. It is the right answer only if you specifically want the
Heltec to be the camera node.

### What I would order

| Role | Board | Why |
|---|---|---|
| Camera node | XIAO ESP32S3 Sense + non-Kit Wio-SX1262 | camera included, 8 MB PSRAM for software JPEG encoding |
| Base station | Heltec V4 | 16 MB flash, OLED for status at a glance, 28 dBm |
| Second node / spare | the Heltec V3s you have | pin-compatible with V4, same firmware |
| Low-power relay | XIAO nRF52840 + Wio-SX1262 | 5 µA sleep, no camera, no AP |

Whatever the sensor, capture raw grayscale and encode JPEG in software so
the restart interval lands on chunk boundaries — see
[camera.md](camera.md). That decision is independent of which module you
buy, and it is worth more than the sensor choice.

## One radio driver for four boards

Rather than writing an SX1262 driver per platform, the port should sit on
**RadioLib**, which supports the SX1262 on both ESP32 and nRF52840 and is
what Meshtastic uses in the field. That turns four radio ports into one
thin adapter plus four pin maps.

```
loraitp core (C99)
   |
port/port_radiolib.cpp   -> RadioLib -> SX1262
   |
firmware/boards/*.h      -> pin map per board
```

The pin maps are the only genuinely board-specific part, and they belong
in version control as data, not scattered through `#ifdef`s.

> The Heltec V3 pin map is widely documented and stable. The XIAO
> mappings differ between the Kit and non-Kit radio boards, so take them
> from the vendor pinout or from Meshtastic's variant files rather than
> from a forum post — a wrong BUSY pin produces a radio that appears to
> work and then hangs.

## Flashing without a toolchain

Two paths, because the two chip families boot differently.

**ESP32-S3 boards — a web flasher.** `esptool-js` speaks WebSerial, so a
static page on GitHub Pages can flash a board over USB with no install:
pick your board, click, done. Works in Chrome, Edge and Opera. **Firefox
and Safari do not implement WebSerial** and never will on current plans,
so the page must say so plainly rather than silently failing.

**XIAO nRF52840 — UF2, which is simpler still.** Double-tap reset, a USB
drive appears, drag the `.uf2` onto it. No browser, no drivers, works on
any operating system.

So: one page, four buttons, three of them WebSerial and one a download
link. Built by GitHub Actions on every tag, so the binaries always match
a commit.

```
.github/workflows/firmware.yml   build all targets, publish artifacts
docs/flash/index.html            ESP Web Tools + the UF2 download
docs/flash/manifest-*.json       one per ESP32 target
```

GitHub Pages serves it for free from the same repository, so there is no
infrastructure to run.

## What has to be built, in order

1. ~~`port_radiolib.cpp` plus pin maps~~ — done.
2. ~~LittleFS storage behind `image_read` / `image_write`~~ — done.
3. **The first build against the real RadioLib and Arduino core**, and
   the XIAO radio pin numbers confirmed against the module you have.
4. **A loopback on the bench** — `firmware/node/main.cpp` is written:
   two boards, `EU868_G4_LP` (5 mW, no duty cycle), a synthetic image
   from flash. This is the profile to develop against; it burns no budget
   and needs no licence, so a hundred transfers cost an afternoon.
5. **Camera and software JPEG** on the XIAO Sense.
6. **Application** — schedule, deep sleep, button, AP.
7. **Web flasher and CI.**

Step 4 is the one that turns this from a protocol into a project, and it
needs nothing but step 3 and two boards on a desk.

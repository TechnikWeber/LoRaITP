# Target boards, the camera, and flashing

## Is the code ready to flash?

**No.** What exists is the protocol, not a product.

| Layer | State |
|---|---|
| Specification | complete, v0.1 draft |
| Python reference + channel simulator | 68 checks, 12 scenarios |
| Portable C core | 81 checks, warning-free, sanitizer-clean |
| `port_sim.c` — in-memory radio for tests | done |
| **`port_*_sx126x.c` — a real radio driver** | **not written** |
| **Camera capture and JPEG encode** | **not written** |
| **LittleFS storage** | **not written** |
| **WiFi AP and web UI** | **not written** |
| **Application: schedule, sleep, board pin maps** | **not written** |

The core is the hard, fiddly, easy-to-get-subtly-wrong part, and it is
finished and tested. What remains is mostly plumbing — but it is real
work, and flashing two boards today would get you nothing but a blinking
LED.

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

### The catch, and it matters before you order

**The camera and the "XIAO ESP32S3 & Wio-SX1262 Kit" use the same B2B
connector.** You cannot stack both. The Kit's radio board and the Sense's
camera board compete for one socket.

Seeed sells two different boards under the name *Wio-SX1262 for XIAO*:

* the **Kit** version, which connects over the B2B connector — this is
  the one Meshtastic and RNode support, and the one that clashes with the
  camera;
* the **non-Kit** version, which connects over ordinary soldered pin
  headers, and therefore leaves the B2B connector free for the camera.
  It uses different pins for the radio.

So the camera node is:

> **XIAO ESP32S3 Sense** (camera on the B2B connector)
> **+ Wio-SX1262 for XIAO, the non-Kit version** (radio on the edge pins)

The XIAO exposes 11 GPIOs on its edge; an SX1262 needs about seven
(SCK, MOSI, MISO, NSS, RST, BUSY, DIO1). That fits, with a little room
left over — but **confirm the free-pin list against the Sense's own
pinout before ordering**, because the Sense's B2B already claims the
camera bus, the SD SPI and the microphone, and some of those signals also
appear on edge pins.

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

1. **`port_radiolib.cpp`** plus pin maps. Until this exists nothing runs
   on hardware.
2. **A loopback on the bench** — two boards, `EU868_G4_LP` (5 mW, no duty
   cycle, see [duty-cycle.md](duty-cycle.md)), a fixed test image from
   flash. This is the profile to develop against: it burns no budget and
   needs no licence.
3. **LittleFS storage** behind `image_read` / `image_write`.
4. **Camera and software JPEG** on the XIAO Sense.
5. **Application** — schedule, deep sleep, button, AP.
6. **Web flasher and CI.**

Step 2 is the one that turns this from a protocol into a project, and it
needs nothing but step 1 and two boards on a desk.

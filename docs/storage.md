# Storage and the WiFi access point

## Where images live today

Nowhere. The core moves bytes through the port's `image_read` and
`image_write` callbacks and never holds a whole image; the reference
implementation keeps them in a Python buffer and the C tests in a static
array. Persisting them is a port and firmware concern, which is the
right place for it — the same protocol code runs against internal flash,
an SD card or a file on a Raspberry Pi without noticing the difference.

## Internal flash, and it is not close

Run `python3 tools/storage.py --ota`:

```
== Partition budget, 8 MB flash, with OTA
  bootloader, table, nvs, phy       72 kB
  application (2 slots)           4096 kB
  -> data partition               4024 kB
  -> usable after LittleFS        3782 kB

  image size |   images | at 1/day lasts
       5 kB  |      756 |      2.1 years
       8 kB  |      472 |      1.3 years
      20 kB  |      189 |      0.5 years
```

**Five images is 40 kB — about one per cent of the data partition**, even
after reserving two full application slots for over-the-air update. Drop
OTA and there is 5.7 MB. The question was whether five fit; the answer is
that several hundred fit.

Flash wear is not a constraint either. One 8 kB image a day is 2.9 MB
written per year against a wear-levelled budget of roughly 36 GB — some
thousands of years. LittleFS spreads writes across the whole partition,
so this is not a case where a small hot region wears out early.

### Partition table

```
# Name     Type  SubType  Offset    Size
nvs        data  nvs      0x9000    0x6000
otadata    data  ota      0xf000    0x2000
phy_init   data  phy      0x11000   0x1000
ota_0      app   ota_0    0x20000   0x200000
ota_1      app   ota_1    0x220000  0x200000
images     data  spiffs   0x420000  0x3E0000   # LittleFS
```

LittleFS rather than SPIFFS: SPIFFS is deprecated in ESP-IDF, has no
real directory support, and degrades badly when nearly full. LittleFS is
power-fail safe, which matters for a node that may brown out mid-write
on a cold morning.

### Layout on the filesystem

```
/images/0007_20260825T0600.jpg     image_id and capture time
/images/0007_20260825T0600.json    RSSI, SNR, loss rate, rounds, airtime
/images/index.json                 newest first, for the web UI
```

The sidecar is not decoration. Measured loss rate and repair-round count
per session are what turn the specification's guessed constants into
chosen ones — the 2-second time-on-air cap in particular is still an
estimate and can only be settled with real data.

Keep the last N images as a ring, N configurable, default 32. A transfer
that failed keeps its partial image and its sidecar, because a partial
image plus its loss statistics is exactly the evidence you want.

## When an SD card does earn its place

Not for five images. It does earn its place for:

* **Archival** — years of images rather than months, or full-resolution
  originals kept locally while only a thumbnail is transmitted.
* **Physically carrying data off a site** with no other link.
* **Logging raw radio traces** for protocol debugging, which are far
  larger than the images.

If it is added, it should sit behind the same `image_read` /
`image_write` port callbacks, with the filesystem chosen at mount time.
The protocol core must not learn what an SD card is.

**On hot-plug:** removing an SD card mid-write corrupts the filesystem,
and FAT is unusually bad at surviving it. If cards are to be pulled and
re-inserted casually, the firmware should mount read-only by default,
mount read-write only around a write, unmount immediately after, and
watch the card-detect pin. Even then, treat a pulled card as possibly
damaged. Given that internal flash already holds several hundred images
and can be read over WiFi without touching the hardware, hot-plug is a
lot of failure modes to take on for a convenience the AP already
provides.

## The access point

WiFi is **off by default** and comes up on a button press.

The usual argument is interference, and it is worth being precise about
it: 2.4 GHz WiFi and 868 MHz do not overlap in frequency. What actually
hurts on a shared board is more mundane —

* WiFi transmit bursts pull hundreds of milliamps for a few hundred
  microseconds. On a small regulator that is enough supply disturbance to
  desense a co-located sub-GHz receiver.
* Broadband switching noise from the same PCB and the same ground plane.
* **CPU and interrupt contention, which is the one that really bites.**
  A missed DIO1 interrupt during a 2-second SF12 frame loses that frame,
  and on a duty-cycled band a lost frame costs minutes of wall clock, not
  milliseconds.

But the stronger argument is power. An ESP32-S3 in AP mode draws roughly
100–150 mA *continuously*. That is more than the SX1262 draws while
transmitting at +22 dBm, and unlike the radio it is not duty-cycled. A
node that leaves its AP up does not last a season on a battery, and the
whole point of the energy budget in SPEC.md §8.4 evaporates.

### Behaviour

| Event | Action |
|---|---|
| boot | WiFi off |
| button press (PRG, GPIO0) | AP up, SSID `LoRaITP-<node>`, image gallery on `http://192.168.4.1` |
| page open | browser sends a heartbeat every 30 s |
| no heartbeat and no request for 5 min | AP down |
| button press while AP is up | AP down immediately |
| LoRa session becomes due | see below |

The heartbeat is what makes "do not close it while I am on the page"
work. A gallery is mostly static: someone can look at it for ten minutes
without generating a single request, and a plain request-activity timer
would close the AP underneath them. Thirty seconds is frequent enough to
be responsive and rare enough to cost nothing.

### Who wins, the radio or the browser

They are mutually exclusive, and the radio wins — but not abruptly.

On the **sender**, sessions are scheduled, so the firmware knows when one
is due. When the AP is up and a session comes due, the page is told, gets
a 60-second grace period with a visible countdown, and then the AP drops
and the transfer starts. Missing a scheduled window costs a day.

On the **receiver**, transfers arrive whenever the sender decides, so
there is no schedule to consult. AP up means images may be missed, and
the UI should say so plainly rather than hiding it — a banner, not a
footnote. This is a good reason to keep the base station mains-powered
and listening continuously, and to do the browsing from a phone against
a *different* radio if one is available.

### What the page needs to do

Little. A list of images newest first, each with its thumbnail, capture
time, size, RSSI, SNR and how many repair rounds it took; tap to view
full size; a download link. Plus the two things that are genuinely
useful in the field: **the remaining duty-cycle budget for today**
(`loraitp_budget_bytes_remaining()` already reports it) and the last
session's loss rate.

Serve it from the same LittleFS partition. No framework, no CDN — the
node has no internet, so every asset has to be local anyway.

## To verify on hardware

- PSRAM presence on the specific Heltec V3 variant. The ESP32-S3FN8 has
  8 MB flash; whether the board carries PSRAM should be confirmed rather
  than assumed, because the software JPEG encoder's working set depends
  on it.
- Actual application size once the camera, HTTP server and LoRa driver
  are all in. The 2 MB slot assumed above is generous, but generous
  assumptions are how partition tables end up needing to change after
  units are deployed.
- Measured AP current on the actual board, and the measured effect of AP
  traffic on SX1262 packet error rate. The interference argument above is
  reasoning, not measurement.

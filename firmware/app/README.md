# `app/` — the node firmware

One binary for both ends of the link. Which end a board is is a stored
setting, not a build flag, so there is one firmware to build, one to
flash, and a base station can be turned into a second node from a phone
with no toolchain anywhere near it. A board with a camera defaults to
sending; one without defaults to listening.

```console
$ cd firmware
$ pio run -e xiao_esp32s3_sense -t upload
$ pio device monitor
```

| File | What it does |
|---|---|
| `main.cpp` | setup, the schedule, and the sender/receiver dispatch |
| `appcfg.cpp` | settings in NVS: role, region, radio, budget, AP behaviour |
| `camera.cpp` | grayscale capture on the XIAO Sense |
| `jpeg.c` | grayscale baseline JPEG encoder with restart markers |
| `webui.cpp` | access point, image gallery, settings page |

## Two things worth knowing before the first upload

**It starts on `EU868_G4_LP`** — 869.85 MHz at 5 mW, the sub-band with no
duty-cycle limit at all. It burns none of the daily budget and needs no
licence, so a hundred transfers cost an afternoon rather than a week. Move
to `EU868_G3` from the settings page once the link works. A default that
cannot get anyone into trouble is worth more than a fast one.

**The access point is on and stays on.** WiFi draws 100–150 mA
continuously — more than the SX1262 while transmitting at +22 dBm — so
that is not the long-term answer for a battery node. The auto-off is
built and selectable on the settings page; it defaults to off, because a
bench board whose network keeps vanishing is a nuisance and the power
only matters in the field.

The web server runs pinned to the second core. A receive session blocks
for minutes at a time, and an access point that stops answering for the
length of a transfer is worse than none.

## The image pipeline

```
capture PIXFORMAT_GRAYSCALE 320x240      76.8 kB
  -> loraitp_jpeg_encode_to_budget()     ~4-8 kB
  -> store on LittleFS                   ring of N
  -> LoRaITP session                     the core reads it back
```

Grayscale is chosen, not a limitation: for "what does the camera see"
chroma is the first thing to spend, and it halves the frame buffer as
well. The encoder aims at a *byte budget* rather than a quality number,
because the duty cycle constrains bytes.

The restart interval is one MCU row. Markers cannot be aligned to chunk
boundaries — `DRI` counts MCUs and the compressed size of an interval
varies — but one per row costs about 1 % of the file and cuts the damage
from a lost packet from 72 rows to 16. See
[../../docs/camera.md](../../docs/camera.md).

## Status

Compiles and is contract-tested on a host; **not yet run on hardware.**
The XIAO pin map is read off the module silkscreen and the `RF_SW`
polarity is the convention rather than a measurement — if the link works
in one direction only, that is the first thing to invert.

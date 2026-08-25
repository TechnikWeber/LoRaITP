# Camera node

Builds for every target in [`../boards/`](../boards/README.md).
The camera node itself wants a board with a camera interface, which
means the XIAO ESP32S3 Sense — see [../../docs/hardware.md](../../docs/hardware.md).

The battery end: wake, capture, encode, transmit, sleep.

PlatformIO, one environment per board. The radio sits on RadioLib
through a single `port/port_radiolib.cpp`, so four boards need four pin
maps rather than four drivers. All protocol behaviour lives in `src/`.

## Pipeline

```
RTC alarm -> camera on -> capture RGB565 -> JPEG encode -> LoRaITP -> deep sleep
```

The JPEG encoding is done in software on the S3 rather than by the
camera's own encoder. That costs a few hundred milliseconds and some
PSRAM, against a transmission measured in tens of minutes — and it buys
the three things that actually matter for airtime: grayscale output, a
quality setting we choose, and a restart interval aligned to the chunk
size so a partial transfer still decodes. See
[docs/camera.md](../../docs/camera.md).

## Hardware

Settled: mount the radio on a camera board rather than a camera on a
radio board. The XIAO ESP32S3 Sense ships with a plug-on OV2640, so the
hard half is already done — but its camera and the *Kit* version of the
Wio-SX1262 compete for the same B2B connector, so the radio has to be
the non-Kit variant on the edge pins.
See [../../docs/hardware.md](../../docs/hardware.md).

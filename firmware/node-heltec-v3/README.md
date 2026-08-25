# Camera node — ESP32-S3 + SX1262

The battery end: wake, capture, encode, transmit, sleep.

PlatformIO, ESP-IDF framework. Uses `port/port_espidf_sx126x.c`; all
protocol behaviour lives in `src/`.

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

## Open hardware questions

See [docs/camera.md](../../docs/camera.md) — in short, wiring a DVP
camera to the Heltec V3 is pin-tight, and mounting the radio on a camera
board is probably the better arrangement.

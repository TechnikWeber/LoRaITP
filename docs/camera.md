# Camera and image pipeline

## Can an OV2640 be wired to the ESP32-S3?

Yes. The ESP32-S3 has an LCD_CAM peripheral with a DVP camera interface,
and Espressif's `esp32-camera` driver supports it. The cost is pins:
8 data lines, XCLK, PCLK, VSYNC, HREF, plus SCCB (two I²C lines), plus
optional PWDN and RESET — **roughly 15 GPIOs**, several of them carrying
a 10–20 MHz clock.

On a Heltec WiFi LoRa 32 V3 specifically, that is tight. The SX1262
occupies seven pins (SCK, MOSI, MISO, NSS, RST, BUSY, DIO1), the OLED
takes three more, and several of the remaining pins are strapping or USB
pins you would rather not repurpose. It is probably achievable if you
give up the display, but you would be running a 20 MHz parallel bus over
jumper wires — the kind of thing that works on the bench and fails in
the cold.

## The better arrangement

**Wire the radio to a camera board, not the camera to the radio board.**

An SX1262 module needs about seven pins over SPI. A DVP camera needs
fifteen at high speed. Mounting the harder interface on the board that
was designed for it, and the easier one on flying leads, is the obvious
way round — and it is the opposite of what the "I have a Heltec" framing
suggests.

| Option | Camera side | Radio side | Notes |
|---|---|---|---|
| **ESP32-S3 camera board + SX1262 module** | already routed | 7 pins SPI | recommended; boards with 8 MB PSRAM are common and cheap |
| **Heltec V3 + SPI camera** (ArduCam Mini 2MP Plus) | ~6 pins SPI | already on board | keeps the Heltec; camera costs more; slower readout, which does not matter here |
| **Heltec V3 + DVP OV2640** | ~15 pins | already on board | possible, pin-tight, signal integrity risk |

Readout speed is irrelevant for this application. A camera that takes
half a second to hand over a frame is competing against a transmission
that takes forty minutes.

## Which sensor

Less important than it looks. At a 5 kB target for a 320×240 image, the
bitrate is so far below what any modern sensor resolves that the
limiting factor is the JPEG encoder and the optics, not the silicon.

* **OV2640** — cheapest, best supported, 2 MP. Its on-chip JPEG encoder
  has weak rate control, which matters (see below). Fine.
* **OV5640** — 5 MP, better low-light behaviour, autofocus variants
  exist. More pins, more power, larger images to downscale. Worth it
  only if the scene needs the optics.
* **OV7670** — no JPEG, no PSRAM-friendly modes. Avoid.

Spend the money on the lens and the enclosure window before the sensor.

## Do not use the camera's JPEG encoder

Capture **raw** and encode in software on the ESP32-S3. This costs a few
hundred milliseconds and some SRAM, against a transmission measured in
tens of minutes, and it buys three things that directly determine
airtime:

**Grayscale.** `PIXFORMAT_GRAYSCALE` at 320×240 is a 76.8 kB buffer —
half of RGB565 — and grayscale JPEG is roughly 30–40 % smaller than
colour at equal perceived detail. For "what does the camera see", chroma
is the first thing to spend.

**A quality setting chosen for the link, not for the eye.** Encoding to
hit a byte budget matters more than encoding to hit a quality number,
because the byte budget is what the duty cycle actually constrains.

**Chunk-aligned restart markers.** This is the one that resolves an open
question from the original design. Baseline JPEG is a single
entropy-coded stream with a DC prediction chain running through it: a
gap anywhere destroys everything after it. Setting the restart interval
(`DRI`) so markers land on LoRaITP chunk boundaries makes each chunk an
independently decodable strip, at a cost of 1–2 % in file size.

Whether the OV2640's hardware encoder exposes a usable `DRI` register
through the driver was listed as something to check. Encoding in
software makes the question moot — we set the restart interval because
we wrote the encoder call, not because a sensor vendor exposed a
register.

## Pipeline

```
RTC alarm
  -> camera power on, ~100 ms settle
  -> capture PIXFORMAT_GRAYSCALE 320x240      (76.8 kB)
  -> software JPEG encode, DRI = chunk size    (~4-8 kB)
  -> optional: 80x60 thumbnail, LAYER 0        (~0.5 kB)
  -> LoRaITP session
  -> deep sleep
```

Camera current is 40–60 mA for well under a second. Against 118 mA for
tens of minutes of transmitting, the camera is a rounding error in the
energy budget — which is worth remembering before optimising it.

## To verify on hardware

- PSRAM presence on the specific board variant. 320×240 grayscale fits
  in the S3's internal SRAM, but the software JPEG encoder's working set
  should be measured rather than assumed.
- Software JPEG encode time at 320×240 on a 240 MHz S3.
- Achieved file size at the quality settings that look acceptable, since
  every number in `docs/` downstream of "5 kB" depends on it.

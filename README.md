# LoRaITP — LoRa Image Transfer Protocol

**Send a photograph 30 kilometres, once a day, on a battery — legally.**

LoRaITP is a lightweight protocol for moving a whole image file across a
single LoRa hop. It is built for the case where the radio link is the
scarcest resource in the system: a solar or battery powered camera node
somewhere with no cellular coverage, a base station tens of kilometres
away, and a regulatory airtime budget measured in seconds per hour.

It runs directly on the LoRa PHY — no LoRaWAN, no network server, no
gateway. A sender and a receiver that agree on a frequency are enough.

> **Status: everything builds and passes; nothing has been on the air.**
> The specification is complete and every number in it is computed rather
> than estimated. A Python reference implementation runs full transfers
> against a simulated channel, a portable C core passes 85 checks against
> vectors generated from it, and the firmware builds for all three ESP32
> targets — 43 % of flash, 38 % of RAM on the tightest of them. The next
> step is two boards on a desk. See [the roadmap](#roadmap).

---

## Why this exists

LoRa is slow — a few hundred bits per second at long range — and in
Europe you may only transmit for about 1 % of the time. The usual
conclusion is that images are out of the question.

That conclusion is wrong, and by a wide margin. Run the numbers:

| Image | Spreading factor | Pure airtime | Wall clock on a 10 % band |
|---|---|---|---|
| 5 kB | SF12 (max range) | 3.6 min | 36 min |
| 10 kB | SF12 (max range) | 7.2 min | 72 min |
| 10 kB | SF10 | 1.8 min | 18 min |
| 20 kB | SF10 | 3.6 min | 36 min |

A 10 kB grayscale JPEG at maximum range costs **7 minutes of airtime and
24 mAh**. That is 5 % of a day's legal budget on the 869.4 MHz band, and
about 125 pictures from a single 3000 mAh cell.

One image a day is not marginal. It is comfortable.

*(Every number here comes from [`tools/airtime.py`](tools/airtime.py),
which implements the Semtech time-on-air formula and agrees with
published LoRaWAN reference values to within 0.5 ms. Run it yourself.)*

---

## Design in one page

**A 4-byte header on bulk packets.** The LoRa PHY already provides a
CRC, an explicit length header and a sync word. LoRaITP does not repeat
any of that. A data packet carries a session id and a 16-bit chunk
index, and nothing else — 2 % overhead at 196-byte chunks.

**Bitmap NACK, not per-packet ACK.** The receiver stays silent through a
whole block and then reports which chunks are missing as a bitmap: 32
bytes covers 256 packets. A clean 10 kB transfer costs exactly one round
trip, not fifty.

**A duty-cycle governor that is part of the protocol.** Every
transmission is granted by a budget accountant before it happens. There
is no code path that transmits without asking. The regional profiles are
taken from the published allocation — for Germany, BNetzA Vfg. 91/2025 —
rather than from folklore, and the default is `EU868_G3`
(869.4–869.65 MHz, row 54): **ten times the airtime budget and 13 dB
more power** than the LoRaWAN default channels most projects reach for,
with no bandwidth restriction to get in the way.

**An amateur-radio mode that takes its obligations seriously.** Licensed
operators can lift the duty-cycle limit. In exchange the stack *enforces*
what the amateur service requires: a call sign must be configured or it
refuses to transmit, identification frames are injected automatically
during long transfers, and encryption is switched off and cannot be
switched back on.

**A broadcast mode for links with no return channel.** The receiver may
be unable to transmit, or unwilling, or there may be many of them. The
sender then emits the image plus Reed–Solomon parity and never learns
what arrived — and the receiver can decide *exactly*, not heuristically,
whether the image is still recoverable, so it powers down the moment it
provably is not. Sending the image three times instead costs the same
airtime and fails 34 % of the time at 20 % packet loss, where the code
fails essentially never.

**Images that degrade instead of failing.** JPEG restart markers give a
decoder somewhere to resynchronise after a lost packet. Measured: 300
lost bytes damage **16 of 240 rows** with markers against **72 without**,
for 1.1 % more file. An optional thumbnail layer sends an 80×60 preview
first, so a collapsing link still produces something.

**Adaptive spreading factor.** A 1.3-second probe measures the link, and
the session picks the fastest setting with 6 dB of margin. Each SF step
doubles the airtime, so getting this right is worth more than any other
single optimisation.

Full details in **[SPEC.md](SPEC.md)**.

---

## Getting started

> **Read this first.** The protocol is finished and heavily tested, and
> the firmware builds for all three ESP32 boards — but **no board has ever
> been switched on**. You would be the first. Expect to debug something,
> and see [if nothing arrives](#if-nothing-arrives) when you do.

You need two boards. They run the **same firmware** — which end of the
link a board is is a setting, not a separate build.

### What to buy

| Role | Board | Note |
|---|---|---|
| Camera node | **Seeed XIAO ESP32S3 Sense** + **Wio-SX1262 for XIAO** | the Sense includes the camera |
| Base station | **Heltec WiFi LoRa 32 V3 or V4** | no camera needed at this end |

Get antennas for the right band and **screw them on before powering
anything up**. Transmitting without an antenna can damage the radio.

> One trap worth knowing: Seeed sells two boards called *Wio-SX1262 for
> XIAO*. The **Kit** version plugs into the flat connector underneath the
> XIAO — the same one the camera uses, and it shares GPIOs with the camera
> as well, so it cannot be combined with the Sense. You want the one with
> two 7-pin sockets that the XIAO plugs into from above.

### Wiring the camera node

If the XIAO with its camera board attached fits into the radio board's
sockets, just plug it in — done. If it fouls mechanically, use jumper
leads instead; the wiring is identical either way:

| Radio board | XIAO pin |
|---|---|
| 3V3, GND | 3V3, GND |
| MOSI, MISO, SCK | D10, D9, D8 |
| DIO1 | D1 |
| RST | D2 |
| BUSY | D3 |
| NSS | D4 |
| RF_SW | D5 |

**Leave the microSD slot empty.** The radio's reset line lands on the same
pin as the card's chip select, and a card in the slot will corrupt every
radio read. Pictures are stored in the board's own flash — several hundred
of them fit — so the card is not needed.

### Flashing

Open **[the flasher page](https://technikweber.github.io/LoRaITP/flash/)**
in Chrome, Edge or Opera, plug the board in over USB, pick your board,
click Install. That is the whole procedure — no software to install.

Firefox and Safari cannot do this: flashing over USB needs an API they
have both declined to implement. There is nothing to enable, you need a
different browser.

If the board does not appear in the list of ports, hold its BOOT button
while plugging the USB cable in.

### First run

1. On your phone or laptop, connect to the WiFi network **`LoRaITP-XXXX`**
   that the board creates.
2. Open **`http://192.168.4.1`**. You will see the image gallery, the
   status page and the settings.
3. On the **camera node**, set the role to **Sender**. Leave the other
   board on **Receiver**. (A board with a camera already defaults to
   Sender, so you may not need to change anything.)
4. Wait. Within a few minutes the receiver's gallery should show a
   picture.
5. Once it works, set the schedule: **One transfer every _n_
   seconds / minutes / hours** on the sender's settings page. It ships at
   five minutes, which is right for a bench and wrong for a field —
   *24 hours* is the one-picture-a-day case the airtime table above is
   about. The period is counted from the start of a transfer, not the end
   of it, so a picture that takes 72 minutes to send does not push the
   next one back. The page also estimates how long one transfer takes at
   your current settings, so the interval can be chosen with that number
   in view.

### Running on a battery

Between transfers the sender idles with the access point up, which costs
100–150 mA — more than the radio uses while transmitting, and far more
than everything else put together. **Deep sleep between transfers** on
the settings page powers the board down instead. It is off by default,
because of what it costs you:

* **The access point is gone until you press RESET.** A deep sleep ends
  in a reboot; the board wakes, sends, and powers down again without ever
  bringing WiFi up. That is the point — otherwise the sleep gives back
  most of what it saved. Any boot that is not a scheduled wake brings the
  page up as normal, so RESET is the way back in.
* **It is skipped when the pause is too short to be safe.** The
  duty-cycle governor's rolling window lives in RAM and does not survive
  the reboot, so a board that slept through part of it would wake
  believing its whole hourly budget was untouched — on a 1 % or 10 % band
  that is an offence, not a rounding error. So on a band with a limit the
  firmware sleeps only if it will still be asleep an hour later, by which
  time everything it sent has aged out of the window anyway. On a band
  with no limit there is nothing to lose and it always sleeps. A pause
  shorter than that is spent awake, and the log says so.

Deep sleep is a sender-side setting. A receiver must listen
continuously — see the note about shared clocks above.

The boards start on **869.85 MHz at 5 mW** — the sub-band that has no
airtime limit at all, so you can experiment as much as you like without
using up any budget and without needing a licence. Range will be short.
Once it works, switch both boards to `EU868_G3` on the settings page for
500 mW and real distance, and read
[docs/duty-cycle.md](docs/duty-cycle.md) first.

### If nothing arrives

**Change the antenna-switch setting first.** On the settings page, under
*Antenna switch*, pick the other option and save. This is the single most
likely cause: the radio module has a pin that steers its antenna between
transmit and receive, and which way round it goes is not documented for
this module. With it wrong, the board transmits into a dead end and hears
nothing — which looks exactly like being out of range. Flipping it takes
a tap; ruling it out any other way takes an afternoon.

If that does not help:

- Are both boards on the **same frequency and spreading factor**? Check
  the status page on each.
- Are the **antennas** attached?
- Is one board set to **Sender** and the other to **Receiver**?
- Plug the sender into USB and open a serial monitor at 115200 baud. It
  prints what it is doing, including any configuration the firmware
  refused and why.

### A note on the radio rules

The firmware will not let you transmit outside the limits of whichever
region you select — wrong frequency, too much power, or amateur mode
without a call sign, and it refuses and says so rather than transmitting.
That is deliberate. It is an aid, though, not a guarantee: it cannot know
your antenna gain or your local rules, and staying legal remains yours.

## Repository layout

```
SPEC.md          the wire format — normative
tools/           calculators; every number in the docs comes from here
sim/             Python reference implementation + channel simulator
src/             portable C core   <- no platform headers, no malloc
port/            platform shims    <- no protocol logic
firmware/        boards/ (pin maps), node/ (sender), base/ (receiver)
port/            radio (RadioLib) and storage (LittleFS) shims
tests/           core against the simulator, on the host
```

The boundary between `src/` and `port/` is the one that matters, and it
is enforced by directory rather than convention — see
[CONTRIBUTING.md](CONTRIBUTING.md). It is what lets the same core object
code run in tests, in the simulator and on hardware.

## Tools

Three calculators, no dependencies beyond the Python standard library.

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

That second output is the most useful thing in this repository. Over
30 km the link budget closes with **50 dB to spare** — power is not the
problem. The first Fresnel zone is 51 metres wide at the midpoint, and
the earth bulges 13 metres into the path. Spend your effort on antenna
height and a clear line of sight, not on a bigger amplifier.

---

## Running the tests

```console
$ python3 sim/selftest.py     # 68 checks: crypto vectors, erasure coding,
                              # governor rules, feasibility property tests
$ python3 sim/run.py          # 12 full transfers against simulated loss
$ cd tests && make run        # 85 checks on the C core
$ cd tests && make san        # the same, under ASan and UBSan
$ cd tests && make port       # 24 checks on the RadioLib adapter
$ cd tests && make store      # 37 checks on the image store, real files
$ cd tests && make jpeg       # 13 checks on the JPEG encoder
$ cd tests && make boards     # 29 checks on the board pin maps
$ ./tests/test_jpeg /tmp/s && python3 tests/verify_jpeg.py /tmp/s
```

No dependencies, no hardware, no waiting — a transfer that takes four
hours on a 1 % duty-cycle band runs here in under a second, using the
same state machine. Between them these have found four real bugs: two in
the specification, one in the C core, one in the Python reference.
[`sim/README.md`](sim/README.md) says which.

### Where the gaps are

The protocol core, the radio port, the image store, the JPEG encoder and
the board pin maps all have tests. **The firmware's application layer —
the loop in `firmware/app/main.cpp` — does not**, because it cannot be
exercised without hardware.

That matters, because three real bugs found during bring-up all lived
exactly there, and none was visible to a compiler or to the existing
tests: the core was correct and the application was correct, and they
were wired together wrongly.

* The duty-cycle window was rebuilt before every transfer, so each
  session believed the whole hourly budget was untouched. On a band with
  no limit this is invisible; on a 1 % or 10 % band it is an offence.
  There is a regression test for it now.
* The receiver applied the sender's interval to itself and went deaf
  between listening windows, with no shared clock to say where the gap
  would land.
* The status page queried the duty-cycle window from the other core while
  the radio was writing it.

So if something behaves oddly on first power-up, that layer is the
likeliest place, not the protocol.

## Roadmap

- [x] Protocol specification v0.1
- [x] Airtime, duty-cycle, energy and link-budget calculators
- [x] Repository skeleton and the core/port boundary
- [x] Python reference implementation and channel simulator — 68 checks,
      12 transfer scenarios, no hardware
- [x] Portable C core — 85 checks, warning-free, sanitizer-clean,
      3.9 kB of context and no allocation
- [x] `port_radiolib.cpp` and pin maps — one adapter for all four boards,
      19 contract checks against a mocked RadioLib
- [x] **Builds against the real RadioLib and Arduino core** — three ESP32
      targets, green in CI
- [ ] The XIAO pin numbers confirmed by a board that actually works
- [ ] **Bench loopback on `EU868_G4_LP`** (5 mW, no duty cycle: no budget
      burnt, no licence needed) — the firmware is written and builds; it
      has never been run
- [x] LittleFS image store — ring buffer, sidecar statistics, recovery
      from an interrupted transfer
- [x] Camera pipeline — grayscale capture, software JPEG with restart
      markers, verified against an independent decoder
- [x] WiFi access point and web UI — gallery, live airtime budget, and
      the settings that matter in the field
- [x] Schedule and deep sleep — a transfer every _n_ hours, and the board
      powered down in between when the pause is long enough that the
      duty-cycle window can be safely forgotten
- [x] Web flasher and CI
- [ ] Web flasher on GitHub Pages (WebSerial for the ESP32s, UF2 for the
      nRF52840)
- [ ] Camera and image pipeline: grayscale capture, software JPEG with
      chunk-aligned restart markers
- [ ] Real-world range trial and a measured settings table

## Prior art

No single mechanism here is new, and [`docs/prior-art.md`](docs/prior-art.md)
says so in detail, claim by claim.
[`pmanzoni/loractp`](https://github.com/pmanzoni/loractp) is the nearest
neighbour and worth reading. What seems genuinely unusual is treating the
regulatory budget as a normative part of the protocol rather than the
operator's problem — everything else is a careful application of
well-understood ideas.

## Contributing

The specification is a draft and comments on it are more valuable right
now than code. Open an issue.

## Licence

MIT — see [LICENSE](LICENSE).

## Disclaimer

LoRaITP cannot verify anyone's licence or check anyone's local
regulations. The regulatory profiles are engineering aids designed to
make the compliant path the easy one. Responsibility for lawful
operation remains entirely with the operator.

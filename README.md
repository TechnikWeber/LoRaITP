# LoRaITP — LoRa Image Transfer Protocol

**Send a photograph 30 kilometres, once a day, on a battery — legally.**

LoRaITP is a lightweight protocol for moving a whole image file across a
single LoRa hop. It is built for the case where the radio link is the
scarcest resource in the system: a solar or battery powered camera node
somewhere with no cellular coverage, a base station tens of kilometres
away, and a regulatory airtime budget measured in seconds per hour.

It runs directly on the LoRa PHY — no LoRaWAN, no network server, no
gateway. A sender and a receiver that agree on a frequency are enough.

> **Status: the protocol works, the radio has not been switched on yet.**
> The specification is complete and every number in it is computed rather
> than estimated. A Python reference implementation runs full transfers
> against a simulated channel, and a portable C core passes 81 checks
> against vectors generated from it. Firmware is next.
> See [the roadmap](#roadmap).

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

**Images that degrade instead of failing.** With JPEG restart markers
aligned to chunk boundaries, a transfer that dies at 90 % yields 90 % of
the picture and a grey band, rather than a decoder error. An optional
thumbnail layer sends an 80×60 preview first, so a collapsing link still
produces something.

**Adaptive spreading factor.** A 1.3-second probe measures the link, and
the session picks the fastest setting with 6 dB of margin. Each SF step
doubles the airtime, so getting this right is worth more than any other
single optimisation.

Full details in **[SPEC.md](SPEC.md)**.

---

## Try it

```console
$ python3 sim/selftest.py     # 68 checks: crypto vectors, erasure coding,
                              # governor rules, feasibility property tests
$ python3 sim/run.py          # 12 full transfers against simulated loss
$ cd tests && make run        # 81 checks on the C core
$ cd tests && make san        # the same, under ASan and UBSan
```

No dependencies, no hardware, no waiting — a transfer that takes four
hours on a 1 % duty-cycle band runs here in under a second, using the
same state machine. It has already found two bugs in the specification;
[`sim/README.md`](sim/README.md) says which.

## Repository layout

```
SPEC.md          the wire format — normative
tools/           calculators; every number in the docs comes from here
sim/             Python reference implementation + channel simulator
src/             portable C core   <- no platform headers, no malloc
port/            platform shims    <- no protocol logic
firmware/        node-heltec-v3 (sender), base-linux (receiver)
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

## Target hardware

| Role | Hardware |
|---|---|
| Camera node | ESP32-S3 + SX1262 (Heltec WiFi LoRa 32 V3 or similar) + OV2640/OV5640 |
| Base station | Same board, or an SX1262 HAT on a Raspberry Pi |

Nothing in the protocol is specific to these parts — it needs a LoRa
radio with a 255-byte FIFO and a microcontroller with room for the
image.

---

## Roadmap

- [x] Protocol specification v0.1
- [x] Airtime, duty-cycle, energy and link-budget calculators
- [x] Repository skeleton and the core/port boundary
- [x] Python reference implementation and channel simulator — 68 checks,
      12 transfer scenarios, no hardware
- [x] Portable C core — 81 checks, warning-free, sanitizer-clean,
      3.9 kB of context and no allocation
- [ ] ESP32-S3 / SX1262 sender firmware
- [ ] Receiver + image reassembly on Linux
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

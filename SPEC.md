# LoRaITP — LoRa Image Transfer Protocol

**Version 0.1 (draft) — 2026-08-25**

A link-layer protocol for moving a whole image file across a single
LoRa hop, on hardware with ~320 kB of usable RAM, under a regulatory
duty-cycle budget.

LoRaITP sits directly on the LoRa PHY (SX126x / SX127x / LR11xx). It is
**not** LoRaWAN and needs no network server, no join procedure and no
gateway infrastructure — a sender and a receiver that agree on
frequency, bandwidth, spreading factor and sync word are enough.

---

## 1. Design goals

| Goal | Consequence in the design |
|---|---|
| Lightweight | 4-byte header on bulk packets; no per-packet CRC of our own; static memory only, no `malloc` on the node |
| Efficient | Bitmap NACK instead of per-packet ACK; retransmit only what was lost; payload sized to the PHY, not to a fixed 51 bytes |
| Legal by default | The duty-cycle governor is part of the protocol, not an afterthought. It cannot be bypassed by accident. |
| Usable by radio amateurs | An explicit amateur-service mode lifts the duty cycle, but *enforces* call-sign identification and forbids encryption |
| Degrades gracefully | A partially received image still decodes into a partial picture, not into garbage |
| Verifiable without hardware | Every timing claim in this repo is produced by `tools/airtime.py`, not estimated |

### Non-goals

* Routing, meshing, multi-hop. One hop, point-to-point (or many senders → one receiver).
* Streaming / low latency. This protocol trades latency for range and energy.
* LoRaWAN compatibility. A 10 kB payload does not belong on a shared LoRaWAN network.

---

## 2. Terminology

* **Transfer** — one image moved from sender to receiver.
* **Session** — one transfer attempt, identified by a `SID`.
* **Chunk** — the image bytes carried by one `DATA` packet.
* **Block** — a group of up to 1024 chunks that is acknowledged as a unit.
* **Round** — one pass of *send missing chunks → request status → receive status*.

---

## 3. Frame format

All multi-byte fields are **little-endian**. All frames use the LoRa
explicit header with PHY CRC enabled, so the PHY already guarantees
integrity of each frame; LoRaITP adds no per-packet checksum. Integrity
of the *image* is covered by a CRC-32 in `META`.

Every frame starts with the same two bytes:

```
 0        1        2
+--------+--------+ - -
|  CTRL  |  SID   | type-specific ...
+--------+--------+ - -

CTRL:  bit 7..5   VER    protocol version, 0b000 for this document
       bit 4..0   TYPE   frame type
SID:              session id, incremented per transfer, wraps at 256
```

### 3.1 Frame types

| Value | Name | Direction | Purpose |
|---|---|---|---|
| `0x01` | `META` | TX → RX | announces a transfer |
| `0x02` | `DATA` | TX → RX | carries one chunk |
| `0x03` | `EOB` | TX → RX | end of block, please report |
| `0x04` | `STAT` | RX → TX | which chunks are missing |
| `0x05` | `FIN` | TX → RX | transfer finished |
| `0x06` | `FINACK` | RX → TX | image complete and CRC verified |
| `0x07` | `PROBE` | TX → RX | link measurement request |
| `0x08` | `PROBEACK` | RX → TX | link measurement result |
| `0x09` | `IDENT` | either | call-sign identification (amateur mode) |
| `0x0A` | `PARITY` | TX → RX | FEC parity chunk (optional profile) |
| `0x0B` | `ABORT` | either | give up on this session |

### 3.2 `META` — transfer announcement

Sent 1–3 times at the start of a session, always at the session's
spreading factor.

```
 0      1      2      3      4      5      6      7      8      9
+------+------+------+------+------+------+------+------+------+------+
|CTRL  |SID   | IMG_ID      |LAYER | IMG_LEN            |CHUNK |CODEC |
+------+------+------+------+------+------+------+------+------+------+
 10     11     12     13     14     15     16     17     18
+------+------+------+------+------+------+------+------+------+ - -
| IMG_CRC32                 | WIDTH       | HEIGHT      |FLAGS | TLVs
+------+------+------+------+------+------+------+------+------+ - -
```

| Field | Size | Meaning |
|---|---|---|
| `IMG_ID` | 2 | identifies the picture; a preview and its full version share one `IMG_ID` |
| `LAYER` | 1 | 0 = preview/thumbnail, 1 = main image, 2+ = reserved for refinement layers |
| `IMG_LEN` | 3 | image size in bytes, max 16 MiB |
| `CHUNK` | 1 | payload bytes per `DATA` frame, 1–247 |
| `CODEC` | 1 | 0 raw, 1 JPEG baseline, 2 JPEG with chunk-aligned restart markers, 3 grayscale JPEG, 4+ reserved |
| `IMG_CRC32` | 4 | CRC-32 (IEEE 802.3, poly `0xEDB88320`) over the whole image |
| `WIDTH` / `HEIGHT` | 2+2 | pixels, 0 if unknown |
| `FLAGS` | 1 | bit0 `PARITY_PRESENT`, bit1 `ENCRYPTED`, bit2 `LAST_LAYER`, bit3 `AMATEUR_MODE`, bit4..7 reserved |

The number of chunks is derived, never transmitted:
`NCHUNKS = ceil(IMG_LEN / CHUNK)`, and
`NBLOCKS = ceil(NCHUNKS / BLOCK_SIZE)`.

Optional TLV extensions follow, each `TYPE(1) LEN(1) VALUE(LEN)`:

| TLV | Meaning |
|---|---|
| `0x01` | call sign / node name, ASCII |
| `0x02` | Unix timestamp, 4 bytes |
| `0x03` | battery millivolts, 2 bytes |
| `0x04` | position, lat/lon as two int32 in 1e-7 degrees |
| `0x05` | temperature in 0.1 °C, int16 |
| `0x06` | free-form text comment, UTF-8 |

TLVs a receiver does not know are skipped, never rejected.

### 3.3 `DATA` — the bulk carrier

```
 0      1      2      3      4                    4+CHUNK
+------+------+------+------+--------------------+
|CTRL  |SID   | SEQ         | payload            |
+------+------+------+------+--------------------+
```

`SEQ` is a 16-bit chunk index, 0-based, absolute within the transfer
(not per block). The final chunk may be shorter than `CHUNK`; its
length is implied by `IMG_LEN`.

**4 bytes of header.** With `CHUNK = 196` that is 2 % overhead. This is
the only frame type that is sent thousands of times, which is why the
session id and sequence number are the only things in it — addressing,
image metadata and identification all live in `META`, which is sent
once.

### 3.4 `EOB` — end of block

```
 0      1      2      3      4      5
+------+------+------+------+------+------+
|CTRL  |SID   | BLOCK       |ROUND |RSVD  |
+------+------+------+------+------+------+
```

Sent after the last `DATA` frame of a block (or of a retransmission
round). It asks the receiver for a `STAT`. `ROUND` counts from 0 and
lets the sender discard stale `STAT` frames.

### 3.5 `STAT` — the negative acknowledgement

```
 0      1      2      3      4      5      6      7      8
+------+------+------+------+------+------+------+------+------+ - -
|CTRL  |SID   | BLOCK       |ROUND |ENC   |RSSI  |SNR   |N     | body
+------+------+------+------+------+------+------+------+------+ - -
```

| Field | Meaning |
|---|---|
| `ENC` | 0 = bitmap, 1 = list of missing `SEQ`, 2 = block complete (no body) |
| `RSSI` | int8, dBm, of the last received `DATA` frame |
| `SNR` | int8, in 0.25 dB steps, as reported by the SX126x |
| `N` | body length in bytes |

**Bitmap encoding (`ENC = 0`)** — `N` bytes, LSB-first, bit *i*
corresponds to chunk `BLOCK * BLOCK_SIZE + i`. A set bit means
*missing*. 256 chunks cost 32 bytes.

**List encoding (`ENC = 1`)** — `N/2` uint16 absolute `SEQ` values.

The receiver picks whichever encoding is smaller. Below ~6 % loss the
list wins; above it the bitmap does. A `STAT` for a 256-chunk block is
at most 41 bytes — 1.3 s at SF12, negligible against the block itself.

Feedback is per block, not per packet. This is the core efficiency
decision of the protocol: a 10 kB image at `CHUNK = 196` is 52 chunks,
so a clean transfer costs exactly **one** round trip.

### 3.6 `FIN` / `FINACK`

`FIN` (`CTRL`, `SID`) announces that the sender has nothing left to
send. `FINACK` (`CTRL`, `SID`, `STATUS`) reports `0x00` = image
reassembled and CRC-32 matched, `0x01` = CRC mismatch, `0x02` =
incomplete. On `0x01` the receiver has a complete-looking image whose
CRC is wrong, which can only mean an undetected PHY error — the sender
should retransmit the whole image at the next higher SF.

### 3.7 `PROBE` / `PROBEACK`

`PROBE` carries `CTRL`, `SID`, `SF` (the SF it was sent at) and a 1-byte
sequence. `PROBEACK` returns `RSSI` and `SNR` as measured by the
receiver, plus the receiver's own `RSSI`/`SNR` from an earlier frame, so
both ends learn the asymmetry of the link.

### 3.8 `IDENT`

`CTRL`, `SID`, then an ASCII call sign, unencrypted, no compression.
Required in amateur mode — see §6.

### 3.9 `PARITY` (optional profile)

Same layout as `DATA`, but `SEQ` indexes the parity chunks of the
current block. See §5.

---

## 4. Session flow

```
   SENDER                                  RECEIVER
     |                                        |
     |------------- PROBE (SF12) ------------>|   optional, once per day
     |<------------ PROBEACK ---------------- |   -> pick SF for this session
     |                                        |
     |------------- META ------------------->|   1..3 repeats
     |------------- DATA seq 0 -------------->|
     |------------- DATA seq 1 -------------->|
     |                 ...                    |
     |------------- DATA seq 51 ------------->|
     |------------- EOB block 0 round 0 ----->|
     |<------------ STAT  missing {7,23} -----|
     |------------- DATA seq 7 -------------->|
     |------------- DATA seq 23 ------------->|
     |------------- EOB block 0 round 1 ----->|
     |<------------ STAT  complete -----------|
     |------------- FIN --------------------->|
     |<------------ FINACK ok ----------------|
     |                                        |
   deep sleep                            write JPEG
```

### 4.1 Receiver windows

The receiver is in continuous RX for the whole session; only the sender
sleeps. This is deliberate — the sender is the battery-powered node in
the field, the receiver is the base station with mains power.

For a symmetric, both-ends-battery deployment, LoRaITP defines a
**scheduled mode**: both ends wake at an agreed wall-clock time
(RTC or GPS disciplined), the sender transmits `META` repeatedly for a
guard interval, and the receiver listens for that interval.

### 4.2 Timers and retries

| Timer | Default | Meaning |
|---|---|---|
| `T_STAT` | `4 × ToA(STAT) + 500 ms` | how long the sender waits for a `STAT` after `EOB` |
| `EOB_RETRY` | 3 | `EOB` repeats before the block is abandoned |
| `MAX_ROUNDS` | 8 | repair rounds per block before giving up |
| `T_SESSION` | configurable, default 4 h | hard stop for the whole session |

If `MAX_ROUNDS` is exhausted the sender sends `FIN` anyway. An
incomplete image is still delivered — see §7.

---

## 5. Optional forward error correction

FEC is **off by default**. With a working return channel, ARQ is
strictly cheaper: you retransmit exactly the lost chunks, whereas
parity costs airtime on every transfer whether or not anything was lost.

FEC earns its place in two cases: a one-way link (no receiver
transmitter at all), or a link whose round trip is so expensive that a
repair round costs more than the parity would have.

The defined profile is packet-level **Reed–Solomon over GF(256)**: for
a block of *k* chunks the sender appends *r* parity chunks, and the
receiver reconstructs the block from **any** *k* of the *k + r* chunks.
Because the PHY CRC turns every corrupted frame into a clean erasure
(you never receive a damaged chunk, you receive nothing), erasure
decoding applies and *r* parity chunks recover exactly *r* losses.

Recommended `r = ceil(0.15 × k)` for a link measured at <10 % loss.

---

## 6. Regulatory profile — the duty-cycle governor

This is a normative part of the protocol, not a helper.

Before every transmission the stack calls the governor with the frame's
computed time on air. The governor either grants it or returns the
timestamp at which it may be sent. There is no code path that transmits
without asking.

### 6.1 The off-time rule

After transmitting for `T_on` seconds on a sub-band with duty cycle
`dc`, that sub-band is blocked until

```
t_free = t_end + T_on × (1/dc − 1)
```

At 1 % a 1.85 s SF10 frame blocks the band for 183 s. At 10 % the same
frame blocks it for 16.6 s. This rule is what makes the wall-clock
numbers in §8 so different from the raw airtime numbers.

### 6.2 Defined regions

| Profile | Band | Duty | Max power | Notes |
|---|---|---|---|---|
| `EU868_G1` | 868.0–868.6 MHz | 1 % | 14 dBm ERP | the LoRaWAN default channels, busy |
| `EU868_G2` | 868.7–869.2 MHz | 0.1 % | 14 dBm ERP | too narrow a budget for images |
| `EU868_G3` | 869.4–869.65 MHz | **10 %** | **27 dBm ERP** | the best ISM choice for this protocol |
| `EU868_G4` | 869.7–870.0 MHz | 1 % | 14 dBm ERP | |
| `EU433` | 433.05–434.79 MHz | 10 % | 10 dBm ERP | low power, but a quiet band |
| `US915` | 902–928 MHz | none | 30 dBm | 400 ms dwell-time limit per frame instead |
| `AMATEUR` | per licence | **none** | per licence | see §6.4 |
| `TEST_UNRESTRICTED` | — | none | — | shielded-chamber / dummy-load only |

`EU868_G3` deserves emphasis: it gives **ten times the airtime budget
and 13 dB more power** than the channels most LoRa projects default to.
For a protocol whose entire problem is airtime, that is the single
largest lever available without an amateur licence.

### 6.3 Budget accounting

The governor keeps a sliding one-hour window of transmissions per
sub-band, which is what EN 300 220 actually specifies, and additionally
exposes a daily counter for planning. Both are queryable so an
application can decide *"I have 4 kB of budget left today, send the
preview only."*

### 6.4 Amateur-service mode

`AMATEUR` removes the duty-cycle limit. In exchange the protocol
imposes obligations that the ISM profiles do not have:

1. **A call sign is mandatory.** Enabling `AMATEUR` without a configured
   call sign is a configuration error and the stack refuses to
   transmit. This is a hard failure, not a warning.
2. **`IDENT` is transmitted automatically** at the start of a session,
   at its end, and at least every `T_IDENT` (default 540 s, i.e. below
   the common 10-minute requirement) during long transfers. The
   governor injects it; the application cannot suppress it.
3. **Encryption is disabled and cannot be enabled.** Setting
   `ENCRYPTED` in `META` while in `AMATEUR` mode is a configuration
   error. Amateur radio is an open service; obscuring the meaning of
   transmissions is not permitted.
4. **Frequency is not defaulted.** There is no built-in amateur
   frequency, because the correct one depends on the operator's licence
   class, country and band plan. The operator sets it explicitly.

The mode is a runtime setting, but building it in requires the compile
flag `LORAITP_ENABLE_AMATEUR`, so an ISM-only product cannot reach it
by a misconfiguration.

> LoRaITP does not and cannot verify anyone's licence. The regulatory
> profiles are engineering aids that make the compliant path the easy
> one. Responsibility for lawful operation stays with the operator.

---

## 7. Image layer

The protocol carries opaque bytes, but the choice of what to put in
them dominates the result far more than any protocol detail. A 20 kB
image and a 4 kB image of the same scene differ by a factor of five in
airtime, energy and legal budget. Recommendations:

**Grayscale.** For "what does the camera see right now", chroma is the
first thing to spend. Grayscale JPEG is roughly 30–40 % smaller at
equal perceived detail.

**Small.** 320×240 at quality ~12 lands near 4–8 kB; 160×120 near
1.5–3 kB. Resolution is cheaper to give up than update rate.

**Chunk-aligned restart markers (`CODEC = 2`).** A baseline JPEG is a
single entropy-coded stream: lose one byte in the middle and everything
after it is lost, because the DC coefficient prediction chain breaks.
Setting a JPEG restart interval (`DRI`) so that restart markers fall on
chunk boundaries turns each chunk into an independently decodable strip.
A transfer that ends 90 % complete then yields 90 % of the picture with
a grey band at the bottom, instead of a decoder error. This costs about
1–2 % in file size and is the single highest-value trick in the whole
image layer.

**Preview first (`LAYER = 0`).** Send an 80×60 thumbnail — typically
400–800 bytes, under a minute even at SF12 — as its own transfer before
the main image. If conditions collapse halfway through the main
transfer, the day still produced a picture.

---

## 8. Measured numbers

All values below come from `tools/airtime.py`, which implements the
Semtech time-on-air formula and agrees with published LoRaWAN reference
values to better than 0.5 ms.

### 8.1 Time on air per frame, BW 125 kHz, CR 4/5

| Payload | SF7 | SF8 | SF9 | SF10 | SF11 | SF12 |
|---|---|---|---|---|---|---|
| 16 B | 51 ms | 93 ms | 165 ms | 330 ms | 659 ms | 1.32 s |
| 51 B | 103 ms | 185 ms | 329 ms | 616 ms | 1.31 s | 2.47 s |
| 100 B | 174 ms | 308 ms | 554 ms | 1.03 s | 2.22 s | 3.94 s |
| 200 B | 318 ms | 564 ms | 1.00 s | 1.85 s | 4.02 s | 7.22 s |
| 251 B | 394 ms | 697 ms | 1.23 s | 2.25 s | 4.92 s | 9.02 s |

### 8.2 Suggested chunk size

Large frames are more efficient — the preamble and header are paid once
per frame regardless of length — but a frame that occupies the channel
for 9 s is exposed to fading for 9 s, and a single bit error costs the
whole 251 bytes. LoRaITP caps time on air at **2 s per frame**:

| SF | payload | ToA | net goodput | frames for 10 kB |
|---|---|---|---|---|
| SF7 | 251 B | 394 ms | 626 B/s | 41 |
| SF8 | 251 B | 697 ms | 355 B/s | 41 |
| SF9 | 251 B | 1.23 s | 201 B/s | 41 |
| SF10 | 219 B | 1.97 s | 109 B/s | 47 |
| SF11 | 90 B | 1.97 s | 43.7 B/s | 117 |
| SF12 | 40 B | 1.97 s | 18.2 B/s | 278 |

### 8.3 Wall-clock time for one image, including duty cycle

Assumes `CHUNK` for 200-byte frames and 15 % protocol overhead
(`META`, `EOB`, `STAT`, retransmissions).

| Image | SF | Airtime | EU868 g1 (1 %) | EU868 g3 (10 %) | Amateur |
|---|---|---|---|---|---|
| 2 kB | SF9 | 12.7 s | 21 min | 2.1 min | 13 s |
| 2 kB | SF12 | 1.5 min | 2.5 h | 15 min | 1.5 min |
| 5 kB | SF9 | 30 s | 50 min | 5.0 min | 30 s |
| 5 kB | SF12 | 3.6 min | 6.0 h | 36 min | 3.6 min |
| 10 kB | SF10 | 1.8 min | 3.1 h | 18 min | 1.8 min |
| 10 kB | SF12 | 7.2 min | 12.0 h | 72 min | 7.2 min |
| 20 kB | SF10 | 3.6 min | 6.1 h | 36 min | 3.6 min |
| 20 kB | SF12 | 14.2 min | 23.8 h | 2.4 h | 14.2 min |

**The headline result: one image per day is comfortably legal.** Even
the worst case in the table — 20 kB at SF12 on a 1 % band — fits into a
day at 98.9 % of the budget. Everything else has large margin. On
`EU868_G3` a 10 kB image at SF12 takes 72 minutes of wall clock and
consumes 5 % of the daily budget, which leaves room for twenty images
a day.

### 8.4 Energy

Sender only; SX1262 at +22 dBm (118 mA), ESP32-S3 awake at 35 mA, RX
listening for 15 % of the TX time, 3.7 V.

| Image | SF12, 64 B chunks | SF10, 200 B chunks |
|---|---|---|
| 2 kB | 4.8 mAh | 1.0 mAh |
| 5 kB | 11.9 mAh | 2.4 mAh |
| 10 kB | 23.7 mAh | 4.9 mAh |
| 20 kB | 47.4 mAh | 9.7 mAh |

Deep sleep costs about 0.5 mAh/day at 20 µA board level. A 3000 mAh
cell therefore supports roughly **125 transfers of 10 kB at SF12**, or
over 600 at SF10 — comfortably a year of daily images at SF10, four
months at SF12, before accounting for the camera and for self-discharge.

Energy is *not* the binding constraint on this design. Airtime is.

---

## 9. Adaptive spreading factor

The required SNR for each SF is a property of the LoRa demodulator:

| SF | SF7 | SF8 | SF9 | SF10 | SF11 | SF12 |
|---|---|---|---|---|---|---|
| required SNR | −7.5 dB | −10 dB | −12.5 dB | −15 dB | −17.5 dB | −20 dB |

Each step down costs 2.5 dB of sensitivity and roughly doubles the
airtime. The algorithm:

1. `PROBE` at SF12 with a 16-byte frame — 1.3 s, cheap.
2. The receiver returns measured SNR.
3. Pick the fastest SF whose required SNR is at least `MARGIN` (default
   6 dB) below the measurement.
4. Store the choice; start there next time and re-probe only if the
   previous session needed more than two repair rounds.

Because airtime doubles per step, being one SF too conservative wastes
half the budget, and being one SF too optimistic risks the transfer.
The 6 dB default is deliberately cautious — over a 30 km path, fading
and weather move the link by several dB between sessions.

---

## 10. Range

At 868 MHz over 30 km, free-space path loss is 121 dB. With 27 dBm ERP
and SF12 sensitivity of −137 dBm, the link closes with roughly **50 dB
of margin**. The link budget is not the problem.

The problem is geometry. At 30 km the first Fresnel zone has a radius of
**51 m** at midpoint, and earth curvature adds another **13 m** of bulge.
Any obstacle within that ellipsoid takes the margin away in a hurry, and
a treeline in the path can cost 30 dB by itself.

The practical consequence: spend effort on antenna height and a clear
path, not on transmit power. See `tools/linkbudget.py`.

---

## 11. Open points for v0.2

* Whether authentication (a CMAC without encryption) is acceptable in
  the amateur service — it does not obscure meaning, but the question
  deserves a proper answer before it goes in the spec.
* Multiple senders to one receiver: currently only avoidable by
  scheduling. A slotted TDMA profile is the obvious extension.
* Whether `PARITY` should use RaptorQ (rateless, better for one-way)
  instead of Reed–Solomon (simpler, fixed rate).
* A compressed `STAT` encoding for very high loss rates.

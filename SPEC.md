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

In interactive mode `META` is sent 1–3 times at the start of a session.
In broadcast mode it is **repeated every 32 frames throughout the
transmission** — it is the only frame that makes any of the others
interpretable, and a one-way receiver that misses it, or that wakes up
late, has no way to ask for it again.

```
 0      1      2      3      4      5      6      7      8      9
+------+------+------+------+------+------+------+------+------+------+
|CTRL  |SID   | IMG_ID      |LAYER | IMG_LEN            |CHUNK |CODEC |
+------+------+------+------+------+------+------+------+------+------+
 10     11     12     13     14     15     16     17     18     19
+------+------+------+------+------+------+------+------+------+------+
| IMG_CRC32                 | WIDTH       | HEIGHT      |FLAGS |BLOCK |
+------+------+------+------+------+------+------+------+------+------+
 20     21     22     23     24     25
+------+------+------+------+------+------+ - -
| NPARITY     | NONCE                     | TLVs ...
+------+------+------+------+------+------+ - -
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
| `FLAGS` | 1 | bit0 `PARITY_PRESENT`, bit1 `ENCRYPTED`, bit2 `LAST_LAYER`, bit3 `AMATEUR_MODE`, bit4 `BROADCAST`, bit5 `MAC_DATA`, bit6..7 reserved |
| `BLOCK` | 1 | chunks per block; 0 means 256 |
| `NPARITY` | 2 | parity chunks **per block**, 0 when FEC is off |
| `NONCE` | 4 | random per session; replay protection for the MAC (§11) |

`BLOCK` and `NPARITY` are transmitted rather than assumed because the
broadcast receiver needs both to decide whether a transfer can still be
recovered (§5.3). Derived, never transmitted:

```
NCHUNKS  = ceil(IMG_LEN / CHUNK)
NBLOCKS  = ceil(NCHUNKS / BLOCK)
frames   = NCHUNKS + NBLOCKS * NPARITY
```

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

## 5. Broadcast mode and forward error correction

### 5.1 When there is no return channel

Interactive mode assumes the receiver can transmit. That assumption
fails in real deployments: the receiver may be a listening station that
is not licensed to transmit on that band, there may be many receivers
for one sender, or the sender may simply be cheaper to build without a
receive path at all.

`LORAITP_MODE_BROADCAST` removes the return channel entirely. There is
no `STAT`, no `FINACK`, no repair round. The sender emits the image plus
parity and never learns whether any of it arrived.

Three things change:

1. **FEC becomes mandatory.** Without it a single lost frame is
   unrecoverable.
2. **`META` is repeated throughout**, every 32 frames, not just at the
   start — see §3.2.
3. **The transmission is self-describing in length.** `META` announces
   `NCHUNKS` and `NPARITY`, so a receiver can compute exactly how many
   frames the sender will send and how many are still to come. This is
   what makes §5.3 possible.

Frame order is **systematic first**: all source chunks of a block, then
its parity. A receiver on a good link therefore has the whole image
after the source chunks and can stop listening before the parity even
starts, which on a battery-powered receiver is worth more than it
sounds.

### 5.2 The code

Packet-level **Reed–Solomon over GF(256)**, systematic. For a block of
*k* source chunks the sender appends *r* parity chunks, and the receiver
reconstructs from **any** *k* of the *k + r*.

Because the PHY CRC turns every corrupted frame into a clean erasure —
you never receive a damaged chunk, you receive nothing — erasure
decoding applies, and *r* parity chunks recover exactly *r* losses. This
is far stronger than error correction over the same overhead.

GF(256) constrains `k + r ≤ 255`, which is why the default block size
drops from 256 to **128** when FEC is enabled, leaving room for up to
100 % redundancy. Chunks within a block are zero-padded to equal length
for coding purposes; the true image length comes from `IMG_LEN`.

### 5.3 Knowing when to give up

A broadcast receiver has exactly two decisions available: keep listening,
or stop. Both are costly to get wrong — listening burns power, stopping
early loses the image. With an erasure code the question is not a
heuristic, it is **exactly decidable**.

For each block, at any moment during the transmission:

```
achievable = received_distinct + (frames_for_block − frames_elapsed)

if achievable < k:   the block can never be completed
```

No amount of further listening changes that, because every remaining
frame is already counted in `achievable`. The receiver stops accounting
for that block immediately.

Two useful consequences:

* **Early completion.** Once `received_distinct ≥ k` for every block, the
  receiver decodes and powers down, even though the sender is still
  transmitting parity it no longer needs.
* **Early abandonment.** Once no block is still recoverable, the receiver
  powers down instead of waiting out a session timeout that may be hours
  long.

Crucially, "unrecoverable" is not "worthless". Because the code is
systematic, every source chunk that arrived is readable without
decoding, and with chunk-aligned restart markers (§7) those chunks
decode into a partial picture. A block that fails RS decoding still
contributes its received strips. The receiver always writes out what it
has.

This is exposed as `loraitp_rx_still_recoverable()` and
`loraitp_rx_progress()`.

### 5.4 Why not just send the image three times

Repetition is the obvious approach and it is strictly worse. Both
options below cost the same airtime and the same energy —
`tools/fec_compare.py` computes the comparison:

| Packet loss | Send it 3× | k=52, r=104 erasure code |
|---|---|---|
| 2 % | 0.042 % fail | ~0 |
| 5 % | 0.648 % fail | ~0 |
| 10 % | **5.1 % fail** | ~0 |
| 20 % | **34.1 % fail** | ~0 |
| 30 % | **75.9 % fail** | ~0 |
| 50 % | 99.9 % fail | 0.001 % fail |
| 60 % | 100 % fail | 3.6 % fail |

The reason is straightforward. Repetition requires *every specific*
chunk to arrive at least once, so one unlucky chunk fails the whole
image. An erasure code requires only that *enough* frames arrive, and
does not care which — at 20 % loss the code needs 52 of 156 and receives
about 125.

Repetition's only real advantage is that it needs no decoder, and a
systematic code takes even that away: the source chunks go out in the
clear either way.

There is one place repetition is right, and it is already in the
design — `META` is repeated rather than coded, because it is the frame
everything else depends on and it is too small to code usefully.

**Time diversity still matters.** Fades are correlated in time, so
parity sent immediately after its source chunks defends poorly against a
30-second fade. The duty-cycle governor helps here by accident: on a 1 %
band it spreads frames minutes apart, which is exactly the interleaving
a burst-loss channel calls for. On an unrestricted band the sender
should interleave deliberately.

### 5.5 Choosing r

`parity_percent` is configured as a fraction of *k*, and 0 is illegal in
broadcast mode.

**The trap:** parity is a fraction of *k*, but what the code tolerates is
`r / (k + r)` of the frames actually transmitted. 30 % parity therefore
survives 23 % loss, not 30 %. Solving `r/(k+r) = L` gives:

| Target loss | Required parity |
|---|---|
| 5 % | 5 % |
| 10 % | 11 % |
| 20 % | **25 %** |
| 30 % | **43 %** |
| 40 % | **67 %** |

That is before any margin, and it is a break-even: at exactly the design
loss rate the transfer succeeds about half the time. Apply a factor of
about 1.5 to the loss estimate before reading the table.

| Situation | Suggested `r` |
|---|---|
| interactive, return channel healthy | 0 — ARQ is cheaper |
| interactive, expensive round trips | 10 % |
| broadcast, link measured at 10 % loss | 18 % |
| broadcast, link measured at 20 % loss | 43 % |
| broadcast, link unknown | 80–100 % |

**Open question:** Reed–Solomon is fixed-rate — *r* must be chosen
before the loss rate is known, which is precisely the situation
broadcast mode is in. A rateless code (RaptorQ, or an LT code) would let
the sender simply emit parity until its airtime budget runs out, with no
prior guess. That is a materially better fit, at the cost of
considerably more code and a patent history worth checking. It is the
strongest candidate for v0.2.

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

Verified against the German allocation **BNetzA Vfg. 91/2025**
(November 2025, valid to 31.12.2035), Table 2. That document defines the
duty cycle as `Σ(Ton)/Tobs` with `Tobs` a **rolling one-hour window**,
which is what the governor implements.

| Profile | Band | Duty | Max power | Vfg. row |
|---|---|---|---|---|
| **`EU868_G3`** | **869.4–869.65 MHz** | **10 %** | **500 mW ERP** | 54 |
| `EU868_G1` | 868.0–868.6 MHz | 1 % | 25 mW ERP | 48 |
| `EU868_G2` | 868.7–869.2 MHz | 0.1 % | 25 mW ERP | 50 |
| `EU868_G4` | 869.7–870.0 MHz | 1 % | 25 mW ERP | 56b |
| `EU868_G4_LP` | 869.7–870.0 MHz | **none** | 5 mW ERP | 56a |
| `EU433` | 433.05–434.79 MHz | 10 % | 10 mW ERP | 44b |
| `EU433_NARROW` | 434.04–434.79 MHz | **none**, BW ≤ 25 kHz | 10 mW ERP | 45c |
| `AMATEUR` | per licence | none | per licence | §6.4 |
| `TEST_UNRESTRICTED` | — | none | — | dummy load / simulator only |

**`EU868_G3` is the default.** It is the row that makes this protocol
practical without a licence: *"Geräte mit geringer Reichweite für nicht
näher spezifizierte Anwendungen, 500 mW (ERP) … Alternativ,
Arbeitszyklus: ≤ 10 %"*. Compared with the 868.1/868.3/868.5 MHz
channels that every LoRa library ships preconfigured, it gives **ten
times the airtime budget and 13 dB more transmit power**, and unlike the
alarm-system rows in the same range it carries **no channel bandwidth
restriction**, so a 125 kHz LoRa channel fits.

The alternative to the duty cycle in rows 48, 50, 54 and 56b is
"Anforderungen an Frequenzzugangs- und Störungsminderungstechniken",
i.e. listen-before-talk with adaptive frequency agility. LoRaITP does not
implement LBT and therefore always takes the duty-cycle option.

Two rows are worth knowing about even though they are not the default:

* **`EU868_G4_LP` — 5 mW with no duty-cycle limit at all.** Twenty dB
  less power than g3, but unlimited airtime. For bench work, short-range
  integration testing and protocol debugging on real radios, this is the
  right profile: it burns no budget and needs no licence.
* **`EU433_NARROW` — unlimited duty cycle at ≤ 25 kHz bandwidth.** The
  SX1262 supports 20.83 kHz, so this is usable, though the narrow
  bandwidth makes it very slow and demands a good crystal.

> These figures are the German implementation. Other CEPT countries
> follow the same ETSI basis but verify before deploying, and note that
> sub-band definitions have changed between revisions.

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

**Frequent restart markers (`CODEC = 2`).** A baseline JPEG is a single
entropy-coded stream: lose one byte in the middle and everything after
it is lost, because the DC coefficient prediction chain breaks. A JPEG
restart interval (`DRI`) breaks that chain at known points and gives a
decoder somewhere to resynchronise.

> **Correction.** Earlier drafts of this document said "chunk-aligned"
> restart markers, and writing the encoder showed that is not achievable.
> `DRI` counts MCUs, and the compressed size of an interval varies with
> the content, so where a marker lands in the byte stream is not
> something an encoder chooses. What *is* achievable, and what actually
> matters, is markers frequent enough that a lost chunk damages a bounded
> strip rather than the remainder of the picture.

One marker per MCU row is a good setting. Measured on a 320×240 grayscale
frame at quality 50, by `tests/verify_jpeg.py` decoding the output with an
independent decoder:

| | file size | rows damaged by 300 lost bytes |
|---|---|---|
| no restart markers | 10 002 B | **72 of 240** |
| one marker per row | 10 116 B (+1.1 %) | **16 of 240** |

So 1.1 % of the file buys a 4.5× reduction in the damage a lost packet
does. That is the single highest-value trick in the image layer, and it
is now measured rather than argued.

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

## 11. Authentication

Frame authentication is **AES-128-CMAC truncated to 4 bytes**, over a
pre-shared 128-bit key, with the session `NONCE` from `META` prepended to
the MAC input so a recorded session cannot be replayed into a later one.

**`META` is the exception: it is authenticated under an all-zero nonce.**
`META` is the frame that *delivers* the nonce, so a receiver verifying it
does not have the nonce yet — the obvious construction is circular, and
in the reference implementation it rejected every `META` before this was
corrected. The nonce sits inside `META`'s own authenticated bytes, so a
replayed `META` is bit-identical to the original; replay is caught by the
receiver tracking recently seen `(IMG_ID, NONCE)` pairs instead.

### 11.1 What it is for

The interesting threat here is not an attacker. It is **the neighbours**.
869.4–869.65 MHz is a shared band, and any other device using the same
spreading factor and sync word will hand LoRaITP well-formed-looking
frames that happen to be someone else's traffic. A 4-byte MAC discards
them at essentially no cost, which is a more likely everyday benefit
than defence against malice.

Where malice does matter, it matters in the control plane, not the bulk
data:

| Forged frame | Consequence |
|---|---|
| `STAT` claiming "complete" | the image is silently lost |
| `STAT` claiming "everything missing" | the sender burns its **entire daily budget** on retransmission |
| `ABORT` | the session dies |
| `META` | the receiver misinterprets everything that follows |

Those are cheap attacks with expensive outcomes, and airtime is the
scarcest resource in the system.

### 11.2 What is authenticated by default

**Control frames — on by default.** `META`, `EOB`, `STAT`, `FIN`,
`FINACK`, `PROBE`, `PROBEACK` and `ABORT` carry a MAC. They are rare and
small, so the cost is negligible.

**`DATA` frames — off by default.** A 4-byte MAC doubles the bulk header
from 4 to 8 bytes, taking overhead from 2 % to 4 % of the entire
transfer. It buys little in interactive mode: the image CRC-32 already
detects a forged chunk end-to-end, and an adversary who can inject
frames can also simply jam, which no MAC prevents.

**Exception — `PARITY` frames in broadcast mode should be
authenticated.** A forged parity chunk corrupts the Reed–Solomon decode
across a whole block, so it does more damage than a forged source chunk,
and there is no repair round to recover from it. Set `MAC_DATA` when
broadcasting.

### 11.3 Authentication is not encryption

`ENCRYPTED` and the MAC are separate flags for a reason. A CMAC appended
to a plaintext frame does not obscure the meaning of the transmission —
the content is fully readable by anyone — it only proves who sent it.
On that basis LoRaITP permits authentication in amateur mode and enables
it by default, while encryption remains hard-disabled there (§6.4).

Operators should satisfy themselves that this reading matches their own
regulator's position; `key_present = false` disables it entirely.

## 12. Open points for v0.2

* **A rateless code instead of Reed–Solomon.** See §5.5 — fixed-rate
  coding forces a guess about the loss rate at exactly the moment we
  have no feedback about it.
* **Multiple senders to one receiver**, currently avoidable only by
  scheduling them at different times of day. A slotted TDMA profile is
  the obvious extension; it needs a time source, and GPS costs power
  while a free-running RTC drifts.
* **A compressed `STAT` encoding for very high loss rates.** At 40 %
  loss a bitmap is mostly ones and a run-length form would be smaller —
  which is exactly when saving airtime matters most.
* **Session resumption.** If a transfer fails at 80 %, should tomorrow
  resume it or start a fresh picture? Probably an application policy,
  but the protocol must not preclude it.
* **Listen-before-talk** as an alternative to the duty cycle, which the
  allocation explicitly permits and which would lift the airtime cap
  entirely on g3. Materially harder to get right than duty-cycle
  accounting, and easy to get wrong in a way that is invisible.

# Design notes and open questions

Rationale for the decisions in [SPEC.md](../SPEC.md), and the things
that are still undecided.

## Why not LoRaWAN

LoRaWAN solves a different problem: many nodes, shared infrastructure,
small telemetry payloads, a network server that handles keys and
routing. Every one of those properties is a cost here. A 10 kB transfer
would monopolise a shared gateway for an hour, the fair-use policies of
public networks forbid it outright, and the join procedure and MAC
overhead buy us nothing on a dedicated point-to-point link.

Plain LoRa PHY gives full control of the spreading factor, the channel,
the frame size and the timing — all four of which this protocol needs to
manipulate directly.

## Why no per-packet CRC

The SX126x computes and verifies a CRC-16 over every frame in hardware,
and discards frames that fail. A corrupted packet is therefore never
delivered to us — it simply does not arrive. Adding a second checksum
would cost 2 bytes on every one of thousands of packets to detect
something that cannot reach us.

This also has a pleasant consequence for FEC: because errors present as
erasures (missing packets at known indices) rather than as corruption,
erasure coding applies, and *r* parity packets recover exactly *r*
losses. Erasure decoding is far more efficient than error correction.

The image-level CRC-32 in `META` remains, to catch the rare undetected
PHY error and to prove reassembly was correct.

## Why bitmap NACK instead of stop-and-wait

Stop-and-wait — send a packet, await its ACK — is the obvious design and
the wrong one here. At SF12 an ACK costs over a second of airtime and,
worse, a full duty-cycle penalty on the receiver's side. For 52 packets
that is 52 round trips.

Reporting once per block collapses that to one round trip in the common
case. The bitmap is dense (1 bit per packet, 32 bytes for 256 packets)
and its size does not grow with the loss rate, which matters precisely
when the link is bad. Below about 6 % loss an explicit list of missing
sequence numbers is smaller, so both encodings are defined and the
receiver picks the cheaper one.

## Why cap time on air at 2 seconds

Larger frames are more efficient — preamble and header are paid once per
frame regardless of payload — so pure efficiency argues for the full 251
bytes always. Two things argue against it:

At SF12, 251 bytes is **9 seconds** on air. That is 9 seconds during
which a fade, an interferer or a passing vehicle destroys the entire
frame, and 9 seconds of duty-cycle budget spent on a retransmission.
Packet error rate grows with frame length, and the cost of each error
grows with it too, so the product grows quadratically.

Nine seconds is also long enough to sit uncomfortably against some
regulatory and stack-level dwell limits.

The 2-second cap is a compromise, not a derived optimum. It costs about
20 % efficiency at SF12 and buys a large reduction in the cost of a bad
link. It should be revisited once real packet-error-rate measurements
exist — which is exactly what the first range trial is for.

## Why the image layer is in the spec at all

A protocol specification arguably has no business telling you what to
put in the payload. But the choice of image encoding moves the result by
a factor of five, and no amount of protocol cleverness recovers that.
Grayscale instead of colour, 320×240 instead of 640×480, and a quality
setting chosen for the link rather than for the eye are worth more than
every framing optimisation in this document combined.

The restart-marker trick is the one genuinely non-obvious piece.
Baseline JPEG is a single entropy-coded stream with a DC prediction
chain running through it, so a gap anywhere destroys everything after
it. Setting the restart interval so markers land on chunk boundaries
makes each chunk independently decodable, at a cost of 1–2 % in file
size. A transfer that ends 90 % complete then produces 90 % of the
picture instead of a decoder error. For a link that will sometimes fail
halfway, this changes the character of the whole system.

This question used to be "does the OV2640's hardware encoder expose a
usable restart-interval register?". It is now moot: the pipeline
captures raw grayscale and encodes JPEG in software on the S3, which
costs a few hundred milliseconds against a transmission of tens of
minutes and gives full control over grayscale, quality and `DRI`. See
[camera.md](camera.md).

## Why FEC is off by default

With a working return channel, ARQ is strictly cheaper. Parity costs
airtime on every transfer whether or not anything is lost; ARQ costs
airtime only for what actually went missing. At a 5 % loss rate, ARQ
costs 5 % extra airtime plus one round trip, while 15 % parity costs
15 % — every single time.

FEC earns its place when there is no return channel at all (a
transmit-only node, or a receiver whose transmissions would be
illegal or impossible), or when the round trip is so expensive that it
outweighs the parity. Both are real situations, which is why the profile
is defined; neither is the default case.

**Open question:** Reed–Solomon over GF(256) is simple, well understood
and easy to implement on an ESP32, but it is a fixed-rate code — you
must decide *r* before you know the loss rate. RaptorQ is rateless: the
sender emits parity packets until the receiver says stop, which is a
much better fit for a link whose quality is unknown. It is also
considerably more code and carries a patent history worth investigating.

## Why a probe instead of an ACK-driven rate adaptation

LoRaWAN's ADR adjusts the rate over many uplinks based on the network's
long-term view. LoRaITP transmits once a day, so there is no long term
to average over, and each session must pick its own setting from cold.

A single 16-byte probe at SF12 costs 1.3 seconds and returns a direct
SNR measurement, which maps onto the required-SNR table for each
spreading factor. The 6 dB default margin is deliberately conservative:
over a 30 km path the link moves by several dB with weather and season,
and being one SF too optimistic costs the entire transfer while being
one SF too conservative costs half the budget. Asymmetric risk, so lean
towards caution.

## Open questions for the specification

**Authentication is now specified** (SPEC.md §11) and on by default for
control frames. The reasoning that settled it: the most valuable thing a
MAC does here is not defeating an attacker but discarding *the
neighbours* — 869.4 MHz is shared, and another device on the same SF and
sync word delivers well-formed frames that are not ours. Where malice
does matter it matters in the control plane: a forged `STAT` claiming
"everything missing" makes the sender burn its entire daily budget.

It stays off for bulk `DATA` frames, because 4 bytes on every chunk
takes overhead from 2 % to 4 % of the whole transfer to detect something
the image CRC-32 already catches — and an adversary who can inject can
also jam, which no MAC prevents.

**Multiple senders.** The current design is point-to-point and handles
several nodes only by scheduling them at different times of day, which
works but is fragile. A slotted TDMA profile — nodes with synchronised
clocks transmitting in assigned windows — is the natural extension and
is a well-trodden design. It needs a time source; GPS is the obvious one
but costs power, and a disciplined RTC drifts.

**Very high loss rates.** At 40 % loss a 256-bit bitmap is mostly ones,
and a run-length or arithmetic-coded form would be smaller. This only
matters on links that are barely working — which is precisely when
saving airtime matters most.

**Session resumption.** If a transfer fails at 80 %, should the next
day's session resume it, or start a fresh picture? Resuming saves
airtime but delivers yesterday's news. Probably an application-level
policy rather than a protocol feature, but the protocol must at least
not prevent it.

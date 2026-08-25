# Duty cycle, airtime budgets and the amateur exception

## The rule that shapes everything

In the European 863–870 MHz band, ETSI EN 300 220 limits how much of
each hour a transmitter may occupy the channel. It is not a limit on
total data, on power, or on how often you may send — it is a limit on
*time*, and LoRa spends time very generously.

The enforcement rule used throughout LoRaITP is the standard off-time
formulation. After a transmission lasting `T_on`:

```
t_free = t_end + T_on × (1 / dc − 1)
```

At a 1 % duty cycle, every second on air buys 99 seconds of silence.

| Duty cycle | Airtime per hour | Airtime per day |
|---|---|---|
| 0.1 % | 3.6 s | 86 s |
| 1 % | 36 s | 864 s (14.4 min) |
| 10 % | 360 s | 8 640 s (2.4 h) |

## The sub-bands, and the one everybody forgets

| Sub-band | Range | Duty | Max power |
|---|---|---|---|
| g1 | 868.0–868.6 MHz | 1 % | 14 dBm ERP |
| g2 | 868.7–869.2 MHz | 0.1 % | 14 dBm ERP |
| **g3** | **869.4–869.65 MHz** | **10 %** | **27 dBm ERP** |
| g4 | 869.7–870.0 MHz | 1 % | 14 dBm ERP |

Most LoRa projects use 868.1 / 868.3 / 868.5 MHz, because those are the
LoRaWAN default channels and every library ships with them configured.
All three sit in g1: 1 % duty, 14 dBm.

**g3 gives ten times the airtime budget and 13 dB more transmit power.**
For a protocol whose entire problem is airtime, and whose secondary
problem is range, this is the largest single improvement available
without a licence — and it costs nothing but a configuration change.

The trade-off is that g3 is only 250 kHz wide, which fits one 125 kHz
LoRa channel comfortably and two at a squeeze. It is a shared band, and
it is also where LoRaWAN puts its RX2 downlink at 869.525 MHz, so it is
not empty. For a point-to-point link that transmits for an hour a day,
that is an acceptable neighbourhood.

> Verify the current allocation and its conditions against the ETSI
> standard and your national regulator before deploying. Sub-band
> definitions have changed between revisions of EN 300 220.

## What this means for images

Daily budget at 1 % is 864 seconds of airtime. At SF12 with 200-byte
frames the protocol moves about 27 net bytes per second on air, so the
theoretical daily ceiling is roughly 23 kB — a single 20 kB image, with
essentially no margin for retransmission.

The same budget on g3 is 8 640 seconds, about 230 kB per day. That
turns "one image a day, if nothing goes wrong" into "twenty images a
day, with room for a bad link".

## The amateur service

An amateur radio licence is not a way around the 868 MHz rules. The ISM
allocation and the amateur allocations are separate regulatory regimes,
and holding a licence for one does not lift the conditions of the other.
Operating an 868 MHz device outside its allocation conditions is not
made lawful by being a radio amateur.

What a licence does allow is operating *on an amateur band* under
amateur rules, and those have no duty-cycle restriction and permit
vastly higher power. For a LoRa transceiver the practical option is the
70 cm band; the SX1262 is specified from 150 MHz upwards, so 2 m is
outside its range.

The obligations that come with it are real, and LoRaITP enforces them
rather than documenting them:

**Identification.** Transmissions must be identified by call sign, in
clear, at intervals. LoRaITP injects `IDENT` frames automatically at
session start, at session end, and every 540 seconds during long
transfers. The application cannot suppress them, and the stack refuses
to transmit at all if no call sign is configured.

**No encryption.** The amateur service is an open service; obscuring the
meaning of a transmission is not permitted. In amateur mode LoRaITP
disables encryption and rejects any attempt to enable it as a
configuration error.

**No default frequency.** LoRaITP ships no built-in amateur frequency,
because the correct choice depends on licence class, country and band
plan. The operator sets it deliberately, or the stack does not transmit.

Amateur mode additionally requires the compile-time flag
`LORAITP_ENABLE_AMATEUR`, so an ISM-only build cannot reach it through a
runtime misconfiguration.

## Testing without transmitting

For protocol development, `TEST_UNRESTRICTED` removes all limits. It is
intended for a dummy load, a shielded enclosure, or the channel
simulator — not for an antenna. The planned host-side simulator is the
better answer for almost all development work: it runs the full state
machine at any speed you like, with configurable loss and fading, and
uses no airtime whatsoever.

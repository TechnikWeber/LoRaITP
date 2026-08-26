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

Verified against **BNetzA Vfg. 91/2025** (November 2025, valid to
31.12.2035), Table 2 — the German implementation of the harmonised
SRD allocation.

| Row | Band | Category | Power | Duty |
|---|---|---|---|---|
| 48 | 868.0–868.6 MHz | nicht näher spezifizierte Anwendungen | 25 mW ERP | ≤ 1 % |
| 50 | 868.7–869.2 MHz | nicht näher spezifizierte Anwendungen | 25 mW ERP | ≤ 0.1 % |
| **54** | **869.4–869.65 MHz** | **nicht näher spezifizierte Anwendungen** | **500 mW ERP** | **≤ 10 %** |
| 56a | 869.7–870.0 MHz | nicht näher spezifizierte Anwendungen | 5 mW ERP | **none** |
| 56b | 869.7–870.0 MHz | nicht näher spezifizierte Anwendungen | 25 mW ERP | ≤ 1 % |
| 44b | 433.05–434.79 MHz | nicht näher spezifizierte Anwendungen | 10 mW ERP | ≤ 10 % |
| 45c | 434.04–434.79 MHz | nicht näher spezifizierte Anwendungen | 10 mW ERP | **none**, BW ≤ 25 kHz |

Rows 48, 50, 54 and 56b state the duty cycle as the *alternative* to
"Anforderungen an Frequenzzugangs- und Störungsminderungstechniken",
i.e. listen-before-talk with adaptive frequency agility. LoRaITP does
not implement LBT and always takes the duty-cycle option.

Vfg. 91/2025 also fixes the definition that the governor implements:

> ‚Arbeitszyklus' ist das in Prozent ausgedrückte Verhältnis von
> Σ(Ton)/(Tobs) … Sofern in den Tabellen 2 und 3 nicht anders bestimmt,
> ist Tobs ein fortlaufender Zeitraum von einer Stunde.

A **rolling** one-hour window, not a calendar hour — which is why the
governor keeps a sliding window rather than resetting a counter.

### Row 54 is the one that matters

Most LoRa projects use 868.1 / 868.3 / 868.5 MHz, because those are the
LoRaWAN default channels and every library ships with them configured.
All three sit in row 48: 1 % duty, 25 mW.

**Row 54 gives ten times the airtime budget and 13 dB more transmit
power** — and, unlike the alarm-system rows in the same range, it
carries **no channel bandwidth restriction**, so a 125 kHz LoRa channel
fits. For a protocol whose entire problem is airtime and whose secondary
problem is range, this is the largest improvement available without a
licence, and it costs a configuration change.

It is not empty: 869.525 MHz is where LoRaWAN puts its RX2 downlink. For
a point-to-point link transmitting for an hour a day, that is an
acceptable neighbourhood — and it is an argument for the frame
authentication in SPEC.md §11, which discards the neighbours' traffic
cheaply.

### Two rows worth knowing about

**Row 56a — 5 mW with no duty-cycle limit at all.** Twenty dB less power
than row 54, but unlimited airtime. For bench work and protocol
debugging on real radios this is the right profile: it burns no budget
and needs no licence. `EU868_G4_LP`.

**Row 45c — unlimited duty cycle at ≤ 25 kHz bandwidth.** The SX1262
supports 20.83 kHz, so it is usable, though the narrow bandwidth is slow
and demands a good crystal. `EU433_NARROW`.

## What this means for images

Daily budget at 1 % is 864 seconds of airtime. At SF12 with 200-byte
frames the protocol moves about 27 net bytes per second on air, so the
theoretical daily ceiling is roughly 23 kB — a single 20 kB image, with
essentially no margin for retransmission.

The same budget on g3 is 8 640 seconds, about 230 kB per day. That
turns "one image a day, if nothing goes wrong" into "twenty images a
day, with room for a bad link".

## The window has to outlive the program

The rule is a *rolling* hour, and a rolling hour does not stop when the
board does. A station that reboots and forgets what it sent goes on
transmitting in perfect good faith and is still over its budget.

That is not a hypothetical: this firmware can lose RAM four ways — a
settings change restarts it, deep sleep ends in a reboot, and a watchdog
or a brown-out arrives unannounced. Each of them used to hand the
governor a clean sheet.

So the window is mirrored into RTC memory, which survives a reset, after
every transmission. Two details make it work across the reboot:

* **Entries carry an age, not a timestamp.** The millisecond clock
  restarts at zero, so "3 600 000" means nothing afterwards while "one
  hour ago" still does.
* **The time spent away is recorded before a planned sleep**, and
  subtracted on the way back, so what genuinely aged out while the board
  was off is not counted against it. For an unplanned reboot the time
  away is taken as zero, which counts a second or two too much — the
  error that makes the station stricter, never looser.

Nothing in RTC memory is trusted: a magic number and a CRC-32 decide
whether it holds a budget or whatever the previous firmware left at that
address, and a snapshot that fails either is discarded rather than
interpreted. A board that has genuinely lost power starts with an empty
window, which is correct — it was not transmitting while it was off.

The one case this does not cover is a power failure long enough to
clear RTC memory but shorter than the observation window. There is no
clock to consult and no record to keep, so the station starts the hour
believing it is empty. Solving that needs a battery-backed real-time
clock, which is not a thing this design assumes.

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
simulator — not for an antenna. For an antenna outside a German
allocation there is `LOCAL`, where the operator writes down the duty
cycle their own rules impose and the governor enforces that instead —
see [SPEC.md 6.5](../SPEC.md). It is behind the expert-mode switch on
the settings page, which is off by default and says whose
responsibility it becomes. The planned host-side simulator is the
better answer for almost all development work: it runs the full state
machine at any speed you like, with configurable loss and fading, and
uses no airtime whatsoever.

---

## Sources

- Bundesnetzagentur, *Allgemeinzuteilung von Frequenzen zur Nutzung durch
  Geräte geringer Reichweite (SRD)*, Vfg. 91/2025, November 2025 —
  Table 2, rows 44b, 45c, 48, 50, 54, 56a, 56b, and the `Arbeitszyklus`
  definition on page 3.

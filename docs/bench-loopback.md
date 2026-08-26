# The bench loopback

The first time any of this touches an antenna. Two boards on a desk, a
metre apart, one picture from one to the other.

Everything in this repository has been tested except the thing that
cannot be: the protocol core has 109 checks, the Python reference has
68, the encoder is verified against an independent decoder, and none of
it has ever caused a radio to emit anything. This is the procedure that
changes that.

Budget an evening. Most of it is the first ten minutes.

---

## What it settles, and what it does not

**It answers one question: does one board hear the other at all?** That
question currently has no answer, and almost every other open question
is downstream of it.

Specifically, a successful loopback establishes:

* the radio pin map is right for the board you used — for the XIAO this
  is genuinely unconfirmed, see [hardware.md](hardware.md);
* the antenna switch is steered the right way round, which on a XIAO is
  a coin toss until somebody flips it;
* the state machine that passes 109 checks against a simulated channel
  also works against a real one — `META`, `DATA`, `EOB`, the `STAT`
  bitmap, the repair round, `FIN`;
* the two-core split holds, i.e. the access point still answers while a
  transfer has the radio;
* the image arrives byte-identical, because the CRC-32 says so.

**It settles nothing about range.** At one metre every packet arrives —
that is the entire point of doing it at one metre. Sensitivity, loss
rates, the 2 s time-on-air cap in [SPEC.md](../SPEC.md), how much parity
broadcast mode really needs, the link budget: all of that needs distance
and stays an estimate until a field trial. Do not read a clean bench run
as evidence for any number in the documentation.

---

## What you need

* **Two supported boards.** Any pair; they do not have to match.
* **Both antennas fitted.** Before power, every time. Transmitting into
  an open connector damages the output stage.
* **Two USB cables** and a machine with Chrome, Edge or Opera. Firefox
  and Safari cannot flash over USB.
* A phone or laptop for the web pages. Two browser tabs is easiest, one
  per board.

### Which pair to start with

If you have a choice, **start with two Heltecs.** Not preference —
process of elimination:

| Pair | Pin map | Antenna switch | Camera |
|---|---|---|---|
| Heltec ↔ Heltec | confirmed by the vendor | steered from DIO2, no setting involved | none: sends a test pattern |
| anything ↔ XIAO | **unconfirmed** | **host-driven, polarity unknown** | Sense only |

A Heltec pair has neither unknown in it. If a Heltec pair does not work,
the fault is in the firmware or the protocol, and that is a much smaller
place to look. Bring the XIAO in second, once you know the software side
is sound — then a failure means pins or polarity, and you have a working
reference to compare against.

The lack of a camera is not a limitation here. With no camera the sender
transmits a **deterministic synthetic pattern**, and the receiver checks
the image CRC-32 — so a Heltec pair verifies the whole path byte for
byte, which a photograph cannot do.

---

## Before you plug anything in

**Fit both antennas.**

**On a XIAO, leave the microSD slot empty.** The radio's reset line
lands on the same pin as the card's chip select, and a card in the slot
corrupts every radio access. Images live in the board's own flash;
hundreds fit.

**Do not change the defaults yet.** The firmware ships on
`EU868_G4_LP` — 869.85 MHz, 5 mW, no duty-cycle limit, no licence
needed. It is deliberately the weakest legal setting available, and it
is the right one here: nothing to wait for between attempts, no budget
spent, and at one metre 5 mW is already far more than enough.

---

## 1. Flash both boards

Open **[the flasher page](https://technikweber.github.io/LoRaITP/flash/)**
in Chrome, Edge or Opera, plug a board in, pick its type, click Install.
Repeat for the second board.

If the board does not appear in the port list, hold **BOOT** while
plugging the cable in.

The page serves the most recent tagged release, and states the commit it
was built from. Note that commit — if you end up reporting something, it
is the first thing anyone will ask.

## 2. First contact

Power both boards. Each creates an open WiFi network called
**`LoRaITP-XXXX`**, the suffix taken from its MAC, so the two are
distinguishable. Join one and open **`http://192.168.4.1`**.

Check the **Status** tab before anything else:

| Row | What you want to see |
|---|---|
| Board | the board you think you are holding |
| Region | `EU868_G4_LP — no duty limit` |
| Frequency | `869.850 MHz, 125 kHz, SF10, 4/5` |
| Camera | `ready` on a Sense, `not fitted` elsewhere |
| Last session | anything but `refused` |

If **Last session** says the radio or the governor refused the
configuration, stop and read it — the board is telling you what is
wrong, and it will not transmit until it is fixed. That the page is up
at all means the failure is a setting rather than the board.

> **Keep the tab open.** The access point drops five minutes after the
> last request. An open page polls every five seconds, so it stays up
> while you are looking at it — but close the tab, walk away, and you
> will need to press **RESET** to get back in. That is the intended
> behaviour on a battery; it is merely surprising on a desk. *Stay on*
> under **Access point** in the settings turns it off for the duration.

## 3. Set the roles

On each board, **Settings** → **Role**:

* one board **Sender**,
* the other **Receiver**.

A board with a camera already defaults to Sender and a board without to
Receiver, so a Heltec–XIAO Sense pair may need no change at all. Two
Heltecs will both have defaulted to Receiver — one of them has to be
told otherwise.

Saving restarts the board. That is expected, and the duty-cycle window
survives it.

## 4. The transfer

**Order matters.** The receiver listens in ten-minute windows, back to
back, so it must be running first. The sender fires once about three
seconds after boot and then not again for twenty-four hours — the
schedule now defaults to one picture a day, which is right for a mast
and useless for a desk.

So:

1. Make sure the **receiver** is powered and its status page says it is
   listening.
2. On the **sender**, press **Send / listen now** at the top of the page.
3. Watch the **Live log** on both.

Do not shorten the interval to force repeats. **Send / listen now** is
what that button is for, and a short interval on a duty-limited band
later is a mistake you would have to remember to undo.

## 5. What a good run looks like

On the sender's live log, at the default level:

```
sending 8000 B
TX  META     seq 0      24 B   371 ms air
TX  EOB      seq 0      12 B   ...
RX  STAT     seq 0      18 B   -41 dBm  9.75 dB
TX  FIN      ...
sent: rc 0, 45 frames, 16700 ms airtime, 1 round(s), 17200 ms wall
```

The `DATA` frames are hidden at this level because a transfer is fifty
of them. Tick **every frame (verbose)** on the log tab when you want to
see them — which is exactly what you want when *nothing* arrives, since
it is the difference between "heard nothing" and "heard something
broken".

On the receiver:

```
listening...
RX  META     seq 0      24 B   -39 dBm  9.50 dB
RX  EOB      ...
TX  STAT     ...
received: rc 0, result 0, 41/41 chunks, RSSI -39 dBm, SNR 9.50 dB
saved img00000001.jpg
```

`result 0` is `LORAITP_RX_COMPLETE`: every chunk present and the image
CRC-32 verified. Then open the **Images** tab — the picture, or the test
pattern, should be there.

**One round is the target.** A clean transfer at one metre should need
exactly one round trip: the receiver stays silent through the whole
block and reports what it missed once. If you see two or three rounds on
a desk, something is losing packets that has no business losing them,
and that is worth chasing before you go outdoors.

## 6. Write these down

This is the first real data the project has. From the receiver's status
page and the final log lines:

* **RSSI and SNR**, and the link margin the status page computes;
* **chunks received / total**, and how many **rounds** it took;
* **airtime** versus **wall clock** — on `EU868_G4_LP` these should be
  nearly equal, since there is no duty cycle making it wait;
* whether the **access point stayed reachable** for the whole transfer;
* image bytes, and that the CRC verified.

Then repeat it ten or twenty times. One success is an anecdote; a
consistent round count and a stable RSSI is a result.

---

## When nothing arrives

In order. The first item is first for a reason.

**1. Flip the antenna switch — but only on a XIAO.** Settings →
*Antenna switch* → the other option → save. On the Wio-SX1262 the
antenna is steered by a host pin, and which way round it goes is a
convention rather than something measured on your module. Wrong, and the
board transmits into a dead end and hears nothing — which looks exactly
like being out of range. One tap to rule out, an afternoon any other
way.

*On a Heltec this does nothing.* Those modules steer the switch from
DIO2 internally, and the firmware does not drive a pin at all. The
status page shows the row regardless, which is a display quirk, not a
setting that matters there.

**2. Are both boards on the same frequency and spreading factor?** The
status page header shows both. A mismatch is silent by design — the
radios simply do not hear each other.

**3. Is one Sender and the other Receiver?** Two receivers wait
politely forever. Two senders never listen.

**4. Are both antennas actually attached?**

**5. Turn on verbose logging on the receiver** and send again. This
splits the problem in half: frames appearing means the RF path works and
the fault is in decoding or configuration; a completely silent log means
nothing is being heard at all, and you are back to items 1 to 4.

**6. Too close.** At a metre with both antennas fitted the receiver can
overload. If everything above checks out and the log is silent, try
another room, or take the antenna off the *sender* — reducing what is
radiated is safe; removing the receiver's is pointless.

**7. Plug the sender into USB and open a serial monitor at 115200
baud.** It prints what it is doing from the first line of boot,
including any configuration it refused and why — which is the part the
web page cannot show you if the failure happens before the page exists.

---

## Things worth watching

The application layer — `firmware/app/main.cpp` and `webui.cpp` — is the
one part of this project with no automated tests, because it cannot be
exercised without hardware. Every fault found so far has lived exactly
there. Some specific suspicions, so they are recognised rather than
discovered:

**Does the access point survive a transfer?** On a duty-limited band the
governor waits between frames, and the port spends those waits in light
sleep, which powers down the WiFi MAC. On `EU868_G4_LP` there are no
waits, so this loopback will not reveal it — but the moment you move to
`EU868_G3` the page may go quiet mid-transfer. If it does, that is a
known suspicion and not your setup.

**The XIAO pin numbers.** Taken from the Seeed documentation, never
confirmed against a board. If a XIAO hears nothing and the antenna
switch is not the cause, this is the next place to look.

**The first transfer after boot.** It fires three seconds in. If the
receiver was not ready, it is simply lost — and the next one is a day
away. That is the schedule working as designed, not a fault.

---

## When it works

You have the thing the roadmap has been waiting on. Then:

1. **Repeat it enough to trust it.** Twenty transfers, stable numbers.
2. **Move to `EU868_G3`** — 500 mW, 10 % duty cycle, real distance — and
   read [duty-cycle.md](duty-cycle.md) first. Expect it to become far
   slower: a 10 kB image at SF12 is 72 minutes of wall clock for 7
   minutes of airtime, and that is the governor doing its job.
3. **Raise the power carefully.** The SX1262 gives at most 22 dBm at the
   module. `EU868_G3` permits 500 mW *ERP*, which is that 22 dBm plus
   antenna gain — not a larger number in the field.
4. **Then take them apart.** Range is a different test, on a different
   day, ideally with two people and phones.

And update the roadmap in the [README](../README.md). That unticked box
is the whole project.

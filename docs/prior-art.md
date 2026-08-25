# Prior art, and what is actually new here

An honest attempt to answer "have we built something unique?".

**Short answer: no single mechanism in LoRaITP is new. The combination
is unusual, and one design decision — treating the regulatory budget as
a normative part of the protocol rather than an application concern — is
the part I have not seen done elsewhere.**

That is a smaller claim than "novel protocol", and it is the accurate
one. It is also not nothing: most of the value here is in being a
complete, verified, usable implementation of things that are individually
well understood.

> A caveat that matters: this is a comparison against the specific work
> we looked at, not a systematic literature search. Chunked transfer over
> LoRa is an active area and there is certainly work here I have not
> read. Treat the "not seen elsewhere" claims as "not found so far".

## What we compared against

**[`pmanzoni/loractp`](https://github.com/pmanzoni/loractp)** — a
TCP-like chunked transfer protocol for arbitrary files over LoRa, with
CSMA and per-chunk acknowledgement. The nearest neighbour, and worth
reading. It is a general file-transfer protocol; duty cycle is left to
the operator, there is no forward error correction, and nothing about it
is image-aware.

**Multi-Packet LoRa (MPLR)** — research work on reducing acknowledgement
overhead for bulk transfer, exactly the stop-and-wait problem that makes
naive designs slow. LoRaITP's block-level bitmap NACK is the same family
of idea.

**Hybrid LoRa + IEEE 802.15.4** — separates control traffic (LoRa) from
bulk data (a local high-rate network). A different architecture for a
different problem: it assumes the bulk data has somewhere faster to go,
which is precisely what our case does not have.

**TDM scheduling for multiple cameras** — assigns transmit windows to
avoid collisions. LoRaITP does not do this; it is listed in SPEC.md §12
as the obvious extension for multi-node deployments.

## Claim by claim

### The duty-cycle governor as a protocol component — *probably the real contribution*

Most LoRa bulk-transfer work treats the regulatory budget as somebody
else's problem: the protocol sends, and the operator is expected not to
break the law. LoRaITP makes the budget normative. Every transmission is
granted by an accountant before it happens, there is no code path that
transmits without asking, the rolling-window definition comes from the
allocation text rather than folklore, and the remaining budget is
exposed to the application so it can decide *"I have 4 kB left today,
send the thumbnail only."*

The consequence is not just compliance. It changes the protocol's
timing: `EOB`/`STAT` round trips are cheap in airtime but expensive in
*wall clock* on a 1 % band, and a design that cannot see the duty cycle
cannot make that trade correctly.

### Making `EU868_G3` the default — *not new knowledge, unusual as a decision*

That 869.4–869.65 MHz allows 10 % duty cycle and 500 mW ERP is public
and widely used — LoRaWAN puts its RX2 downlink there. What seems
uncommon is building a bulk-transfer protocol whose default sits there
rather than on the 868.1/868.3/868.5 channels every library ships
preconfigured, and quantifying what that buys: ten times the airtime and
13 dB. Choosing the sub-band is a bigger lever than any framing
optimisation in the spec, and it costs a config change.

### Amateur mode with enforced obligations — *unusual, and niche*

Refusing to transmit without a call sign, injecting identification
frames the application cannot suppress, and hard-disabling encryption is
not something I have seen in a LoRa protocol implementation. It is also
a small audience. Its real merit is that it makes the compliant path the
easy one instead of leaving it to a README warning.

### The broadcast feasibility test — *correct application, not invention*

"A block of *k* decodes from any *k* of *k + r*, so compute whether the
remaining frames can still get you there" is a direct consequence of
erasure coding, not a new idea. Rateless dissemination over lossy links
is well-trodden ground (Deluge and its descendants). What LoRaITP does
is apply it to the receiver's *power* decision — stop listening the
moment recovery is provably impossible, and equally the moment the image
is already in hand. In the simulator that turned a 6-hour listening
window into 7 minutes. Useful; not novel.

### Chunk-aligned JPEG restart markers — *an old technique, a good integration*

Using JPEG restart markers for error resilience dates to the 1990s and
is standard practice for image transmission over lossy channels. The
contribution here is only the integration: aligning the restart interval
to the *protocol's* chunk size so that a lost frame maps exactly onto one
independently decodable strip. That alignment is what makes a
90 %-complete transfer yield 90 % of the picture instead of a decoder
error, and it falls out of controlling both layers.

### Frame authentication scoped to the control plane — *a good trade, not a new one*

Authenticating rare control frames and leaving bulk frames unauthenticated
is standard practice wherever overhead matters. The reasoning that led to
it here is worth recording though, because it is not the obvious one:
the most valuable thing a MAC does on a shared band is discard *the
neighbours*, not defeat an attacker.

## What is genuinely different about the artifact

Less about mechanisms, more about how it was built:

**Every number is computed and re-derivable.** The airtime tables, the
duty-cycle budgets, the energy figures, the link budget, the FEC-vs-repetition
comparison, the flash budget — each comes from something in `tools/` that
you can run. The original estimate this project started from was wrong by
a factor of fifteen, which is exactly what happens without this
discipline.

**The regulatory table is verified against the allocation text**, cited
by document, version and row number, rather than copied from a forum
post.

**There is an executable specification** in `sim/`, and it has already
found three real bugs — two in the specification itself, one in the C
core. The specification and the implementation check each other, and the
test vectors in `tests/vectors/` are generated from the reference so the
C and Python cannot drift apart silently.

**It is a working implementation with a licence**, not a paper. For
somebody who wants to move an image over LoRa next month, that is worth
more than novelty.

## Where an actual contribution might still be

If this project ends up with something genuinely new, the most likely
candidate is the interaction the duty-cycle governor has with everything
else: a rateless code whose redundancy is chosen from the *remaining
legal airtime budget* rather than from an estimated loss rate. The sender
knows exactly how many seconds it may still transmit today; spending
them on parity until the budget runs out is a policy that only makes
sense if the protocol can see the budget — which is the one thing here
that is not standard.

That is speculation. It is not built.

# FHSS — Frequency-Hopping Spread Spectrum

Saitama can operate its LoRa radio in one of three mutually exclusive modes,
selected in **Settings → Radio Mode**:

| Mode | Behaviour |
|---|---|
| **Continuous RX** | Radio listens continuously on one fixed frequency. Highest power draw, lowest latency. Default. |
| **LoRa Duty Cycle** | SX1262 hardware RX duty cycle (250 ms RX / 250 ms sleep) once the channel has been idle for 10 s. Fixed frequency. |
| **FHSS** | The radio retunes every 8 s to a channel derived from a keyed hop sequence. |

This document covers the third one.

---

## 1. Attribution — this is a port, and we say so

**The FHSS design implemented here is not ours.** It is ported, with
attribution and gratitude, from the SPECTRA mesh protocol:

- **Project:** spectra-mesh
- **Source:** <https://codeberg.org/thewrew/spectra-mesh>
- **Reference files:**
  - `spectra-protocol/src/fhss.rs` — the canonical Rust implementation
  - `spectra-firmware-tdeck-pio/lib/spectra-protocol/spectra_fhss.{h,cpp}`
  - `spectra-firmware-tdeck-pio/lib/spectra-protocol/spectra_fhss_state.h`

Our port lives in [`src/mesh/Fhss.h`](../src/mesh/Fhss.h) and
[`src/mesh/Fhss.cpp`](../src/mesh/Fhss.cpp). Both files carry a provenance
header pointing back here and at the upstream source.

The channel plan, the hop-sequence algorithm, the double-precision frequency
arithmetic, and the region constants are all theirs. Where we have deliberately
departed from the reference, the code says so in a comment beginning
`DEVIATION FROM UPSTREAM` at the exact point of departure, so this port can be
diffed against the original by anyone auditing it. Section 4 lists those
deviations in full.

If you improve something in the ported core, consider whether it is a fix that
belongs upstream too.

---

## 2. How it works

The hop channel is a **pure function of three inputs** and is never negotiated,
announced, or transmitted:

```
channel = HMAC-SHA256(networkKey, frameNumber as u32 LE)[0..2] as u16 LE
          % region.numChannels
```

Each node evaluates this independently and retunes. There is no "I am hopping
to channel 47" packet, because there is no need for one — and no room for one
either, since a node that has already drifted could not be trusted to send it.

The three inputs:

**`region`** — the channel plan. A base frequency, an offset, a spacing, and a
channel count. Frequency for channel *n* is `base + offset + n * spacing`.
Region is chosen automatically from the configured radio profile's band:

| Region | Band | Channels | Spacing |
|---|---|---|---|
| EU868 | 868.1 MHz + | 3 | 200 kHz |
| US915 | 902.1 MHz + | 130 | 200 kHz |
| AU915 | 915.1 MHz + | 65 | 200 kHz |
| IN865 | 865.0625 MHz + | 3 | 340 kHz |
| RU864 | 864.1 MHz + | 7 | 200 kHz |

The 433 MHz profiles have no hop plan; FHSS reports itself unavailable and the
radio stays on the fixed frequency.

### Which profiles actually offer FHSS

| Profiles | FHSS | Region used |
|---|---|---|
| Australia, AU-Victoria, NZ, NZ Narrow, Vietnam | ✅ offered | AU915 — 65 ch, 915.1–927.9 MHz |
| USA / Canada | ✅ offered | US915 — 130 ch, 902.1–927.9 MHz |
| EU/UK Narrow, EU/UK Long, EU/UK Medium, Czech, Portugal 868, Switzerland | ❌ **not offered** | see below |
| EU 433, Portugal 433 | ❌ not offered | no hop plan exists for 433 MHz |

### ⚠ Why EU 868 does not get FHSS

The channel-count asymmetry is real and inherited from upstream: **US915 gets
130 channels across 25.8 MHz, EU868 gets 3 channels across 400 kHz.** That is
not a transcription error — it is what the 868 MHz regulatory landscape
allows. But it makes the feature a bad trade in EU, so `regionForProfile()`
returns `nullptr` for every EU 868 profile and FHSS reports itself unavailable
there.

The reasoning:

- Saitama's EU profiles run at **869.525 / 869.618 MHz**, in the
  `869.4–869.65` sub-band: **500 mW, 10 % duty cycle**.
- Upstream's EU868 hop plan is at **868.1 / 868.3 / 868.5 MHz**, in the
  `868.0–868.6` sub-band: **25 mW, 1 % duty cycle**.

Enabling it would therefore move the node **off the frequency the rest of the
mesh is on**, cut its legal power limit by **~13 dB** and its duty-cycle
allowance by **10×**, and buy in exchange hopping across just three channels
400 kHz apart. Three channels is channel rotation rather than spread spectrum:
it earns none of the regulatory FHSS relief that wideband hopping gets under
FCC Part 15.247, and gives very little interference diversity.

**For EU users, Continuous or Duty Cycle on 869.525 is the better setup.** The
Radio Mode dialog says so directly rather than silently offering a mode that
would degrade the link.

`REGION_EU868` (along with `REGION_IN865` and `REGION_RU864`, which no profile
maps to either) is **kept in the region table** and remains covered by the
reference test vectors in §3. Deleting it would make this port a partial mirror
of upstream and lose that verification coverage; it is simply not wired to a
profile. If a future EU channel plan makes hopping worthwhile there, the
arithmetic is already in place and already validated.

### Power limit enforcement

Regardless of region, `MeshService` **enforces `maxTxPowerDbm`**: on engaging
FHSS it clamps TX output to the region limit and restores the configured power
on disengage. This exists because a hop plan can legitimately sit in a
sub-band with a lower limit than the node's normal fixed frequency, and
carrying a configured 22 dBm across that boundary would transmit over the
limit. It is defensive on US915/AU915 (both 22 dBm) but the guard belongs in
the code, not in the operator's memory.

**`networkKey`** — `SHA-256(public channel PSK)`. Every node already shares the
public channel PSK, so an existing mesh starts hopping together with no new
configuration, and two meshes with different PSKs hop independently. This key
seeds the hop sequence only; it encrypts nothing and we rely on it for no
confidentiality guarantee.

**`frameNumber`** — a counter that advances once per 8 s frame. This is where
we depart from upstream; see section 4.

---

## 3. Why the arithmetic must stay byte-identical

This is the part worth internalising before touching `Fhss.cpp`.

FHSS has **no negotiation step and no error path**. Two nodes remain on
speaking terms only while `hopSequence()` returns the same number on both of
them for the same inputs. A node that computes a different channel does not
report a fault, does not log an error, and does not retry. It transmits into
empty air and hears silence.

**The failure mode of a mismatch is a mesh that looks completely healthy but
carries no traffic.** Both nodes show a working radio, a valid clock, sensible
stats, and zero received packets. That is dramatically harder to diagnose than
a crash, and it is why every detail of this computation is effectively a wire
format even though none of it is ever serialised:

- **The HMAC input encoding.** Frame number is hashed as **4 bytes
  little-endian**. Flip to big-endian and you get a completely different — and
  still perfectly plausible-looking — hop sequence. Nothing warns you.
- **The truncation width and order.** The first **2** digest bytes, read back
  as a **little-endian u16**. Take 1 byte instead, or 2 bytes big-endian, and
  every channel index changes.
- **The modulus.** `% region.numChannels`. A region table with the wrong
  channel count silently reshuffles the entire sequence for that region.
- **The frequency arithmetic *and its precision*.** The channel → frequency
  product is computed in `double`, not `float`. This is not a
  micro-optimisation — it is a compatibility fix inherited from upstream
  (their `qs#235`), whose canonical Rust implementation uses `f64` throughout.

  We measured the difference rather than taking it on faith. Computing the
  US915 plan in `float` instead of `double` shifts **52 of the 130 channels**,
  by **up to 61 Hz** on the float-MHz value passed to `setFrequency()` (and up
  to 32 Hz on the integer-Hz variant). Upstream's comment cites 50–500 Hz; we
  did not reproduce the upper end of that range, but the effect is real,
  it is free to avoid, and a systematic tuning offset against a
  correctly-computing peer is not something to spend margin on.
- **The frame duration.** 8000 ms. Two nodes using different frame durations
  diverge on the very first hop.

So the standing rule for `Fhss.cpp` is: **if you are tempted to tidy up the byte
order, the truncation width, the integer types, or the float precision — don't.**
Those are not implementation details, they are the protocol. Any change must be
made in lockstep with the upstream reference and with every other node in the
fleet, or the mesh partitions.

Keeping the ported core byte-identical also buys us something concrete: this
implementation can be validated against the upstream reference and its test
vectors. A port that has quietly "improved" the arithmetic cannot be.

### Verifying the port

`Fhss.cpp` has no Arduino dependencies beyond `SHA256.h`, so it compiles on the
host against a shim exposing the same `reset()`/`update()`/`finalize()` API.
That makes it straightforward to diff our output against an independent
implementation of the spec:

```python
# Independent reference — from the spec, not from our C++
import hmac, hashlib, struct
def hop(key, frame, num_channels):
    tag  = hmac.new(key, struct.pack('<I', frame), hashlib.sha256).digest()
    wide = tag[0] | (tag[1] << 8)          # first 2 bytes, little-endian u16
    return wide % num_channels
```

This port was checked this way across 81 vectors — all five regions over
frames 0–11, frame numbers chosen to exercise every byte of the little-endian
encoding (255, 256, 65535, 65536, 16777215, 16777216, 2³²−1), and the high
US915 channel indices where the float/double question bites — and matched on
every one. Re-run that comparison after any change to this file.

---

## 4. Deviations from upstream

### 4.1 Time base — UTC instead of beacon-synchronised `millis()`

**Upstream** derives the frame number from a local `millis()` epoch anchored by
a `JoinBeacon` heard on a fixed rendezvous channel, then keeps it aligned with
`SYNC` packets carrying a `frames_since_sync` authority metric:

```
current_frame = base_frame + (millis() - hop_epoch_ms) / frame_duration_ms
```

That design exists because SPECTRA targets boards with no dependable clock, so
the mesh has to bootstrap a shared notion of time over the air.

**We derive the frame number directly from UTC:**

```
frameNumber = (utcEpochSec * 1000) / 8000
```

The T-Deck Plus has a built-in GPS and an RTC that MeshCore already keeps set,
so every node with a valid clock agrees on the frame number with **no beacons,
no rendezvous channel, and no drift-correction protocol at all**.

Consequences, stated plainly:

- We do **not** interoperate with upstream SPECTRA nodes' frame numbering.
  (We could not anyway — our packets are MeshCore, theirs are not. FHSS
  compatibility alone would not make the two stacks talk.)
- **FHSS is unavailable until the clock is set.** `MeshService` checks
  `fhss::clockIsValid()` every tick and holds the fixed mesh frequency while it
  is false. Hopping on a guessed frame number is strictly worse than not
  hopping: it silently parks the node on a channel nobody is listening to.
- Frame boundaries land on 8 s multiples of the UTC epoch. RTC resolution is
  one second, so a node whose clock is a second off can hop up to a second
  late and miss traffic in that window. GPS-disciplined time keeps this well
  inside tolerance; a badly drifted RTC will degrade throughput before it
  fails outright.

### 4.2 JoinBeacon and SYNC are not ported

Both are dead weight when the time base is absolute, so neither the 19-byte
HMAC-signed `JoinBeacon` wire format nor the `frames_since_sync` authority
mechanism is present here.

If clockless operation is ever needed, the work is: port `JoinBeacon`
serialize/deserialize byte-identically, add a rendezvous-channel listen at
boot, transmit beacons on a per-node jittered frame slot (`frame % 8 ==
own_hash % 8`), and add the SYNC drift-correction consumer. Upstream's
`spectra-firmware-tdeck-pio/src/main.cpp` is the reference for all four.

### 4.3 Not ported: TDMA slotting

Upstream pairs FHSS with a TDMA slot assignment (`tdma_assign_slot`) that gates
when a node may transmit within a frame. We let MeshCore's existing CSMA/
duty-cycle logic own transmit timing. FHSS here changes *where* the radio
listens, not *when* it talks.

---

## 5. Implementation notes

- **Hop tick.** `_tickFhss()` runs from `MeshService::tick()`, before the duty
  cycle tick. It recomputes the target channel each tick and calls
  `setFrequency()` + `startReceive()` only when the channel actually changes.
- **TX guard.** The tick returns early while `P_LORA_BUSY` is high, so we never
  retune underneath an in-flight transmission. The hop is picked up on the next
  tick.
- **Mode exclusivity.** The hardware RX duty cycle parks the radio asleep on
  one frequency, which is precisely what a hopping node must not do. The duty
  cycle tick is gated on `radioPowerMode == RADIO_POWER_DUTY_CYCLE`, so
  selecting FHSS disarms it.
- **Falling back.** Whenever FHSS cannot run — mode changed, no hop plan for
  the band, clock lost — `_fhssRestoreFixedFreq()` returns the radio to the
  configured profile/override frequency and logs why.
- **Status.** `MeshService::fhssStatus()` exposes region, channel, frequency,
  hop-plan range, power cap, frame number, hop count, and the two "can't run"
  reasons for the UI.
- **TX power limit.** `_fhssApplyTxLimit()` clamps output power to
  `region->maxTxPowerDbm` on engaging, and `_fhssRestoreFixedFreq()` puts the
  configured power back on disengaging. See the EU sub-band warning in §2.

## 6. Config storage

`Config::radioPowerMode` (`0` continuous, `1` duty cycle, `2` FHSS) is appended
at the end of the struct, per the NVS layout rule in `Config.h`. The legacy
`loraDutyCycle` bool is retained in its original position and kept in sync on
save, so an older firmware reading the same NVS blob or `settings.json` still
behaves sensibly. `config::init()` migrates blobs that predate the new field by
carrying the old bool across.

---

## 7. Regulatory note

Channel plans, dwell times, and duty-cycle obligations vary by jurisdiction,
and the region tables here are inherited from upstream rather than
independently certified. Operating a hopping transmitter is your
responsibility: confirm that the plan you select is permitted in your country
and under your licence class before enabling FHSS.

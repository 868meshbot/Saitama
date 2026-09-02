// Saitama — Fhss.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// ─────────────────────────────────────────────────────────────────────────────
// PROVENANCE — this is a port, not original work
// ─────────────────────────────────────────────────────────────────────────────
// The channel plan, the hop-sequence algorithm, and the frame-counter
// arithmetic in this file are ported from the SPECTRA mesh protocol:
//
//     Project : spectra-mesh
//     Source  : https://codeberg.org/thewrew/spectra-mesh
//     Files   : spectra-protocol/src/fhss.rs                    (canonical, Rust)
//               spectra-firmware-tdeck-pio/lib/spectra-protocol/
//                   spectra_fhss.h  spectra_fhss.cpp  spectra_fhss_state.h
//
// The design is theirs and is reproduced here with attribution. Every
// deviation we make is called out explicitly in a "DEVIATION" comment at
// the point it occurs, so this file can be diffed against the upstream
// reference by anyone auditing it. See docs/FHSS.md for the full write-up.
//
// ─────────────────────────────────────────────────────────────────────────────
// WHY THE ARITHMETIC MUST STAY BYTE-IDENTICAL
// ─────────────────────────────────────────────────────────────────────────────
// FHSS has no negotiation step. No node ever transmits "I am about to hop
// to channel 47". Each node independently evaluates
//
//     channel = hopSequence(networkKey, frameNumber, region)
//
// and retunes. Two nodes stay on speaking terms *only* while that function
// returns the same number on both of them for the same inputs. There is no
// error path: a node that computes a different channel does not report a
// fault, it simply transmits into empty air and hears silence. The failure
// mode of a mismatch is a mesh that looks powered-on and healthy but carries
// no traffic, which is far harder to diagnose than a crash.
//
// That makes every part of this computation a wire format, even though none
// of it is ever serialised:
//
//   * The HMAC input encoding — frame number as 4 bytes little-endian. Flip
//     to big-endian and you get an entirely different, still perfectly
//     plausible-looking hop sequence.
//   * The truncation — first 2 bytes of the HMAC-SHA256 digest, read back
//     as a little-endian u16. Taking 1 byte, or 2 bytes big-endian, changes
//     every channel index.
//   * The modulus — `% region->numChannels`. A region table with the wrong
//     channel count silently reshuffles the whole sequence.
//   * The channel→frequency arithmetic, including its precision. See the
//     double-precision note in Fhss.cpp: upstream found that doing this in
//     float drifts a few hundred Hz at high channel indices, which is enough
//     to walk a receiver off-tune from a correctly-tuned peer.
//
// So the rule for this file is: if you are tempted to "clean up" the byte
// order, the truncation width, the integer types, or the float precision —
// don't. Those are not implementation details, they are the protocol. Any
// change here must be made in lockstep with the upstream reference and every
// other node in the fleet, or the mesh partitions.
//
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace ops {
namespace fhss {

// ── Region / channel plan ────────────────────────────────────────────────────
// Mirrors upstream `FhssRegion`. Field semantics are identical; only the
// names are restyled to our conventions.
struct Region
{
    const char* name;
    float   baseFreqMHz;        // e.g. 902.0 (US), 868.0 (EU)
    float   channelOffsetMHz;   // e.g. 0.1
    float   channelSpacingMHz;  // e.g. 0.2
    uint16_t numChannels;       // 130 (US), 3 (EU), ...
    uint16_t rendezvousChannel; // upstream's join-beacon channel; see DEVIATION below
    int8_t  maxTxPowerDbm;
};

// Predefined regions — values copied verbatim from the upstream table.
// Changing any number here changes the hop sequence for that region.
extern const Region REGION_EU868;
extern const Region REGION_US915;
extern const Region REGION_AU915;
extern const Region REGION_IN865;
extern const Region REGION_RU864;

// Largest numChannels across the table above (US915), for array sizing.
static constexpr uint16_t MAX_CHANNELS = 130;

// Upstream's TDMA frame duration. This is the quantum the frame counter
// advances on, so it is as much a shared constant as the region table:
// two nodes using different frame durations diverge immediately.
static constexpr uint32_t FRAME_DURATION_MS = 8000;

// ── Channel plan ─────────────────────────────────────────────────────────────

// Centre frequency of a channel, in MHz. Returns 0.0f when channel is out of
// range for the region.
float channelToFreqMHz(uint8_t channel, const Region* region);

// Centre frequency of a channel, in Hz. Returns 0 when out of range.
uint32_t channelToFreqHz(uint8_t channel, const Region* region);

// ── Hop sequence ─────────────────────────────────────────────────────────────

// The core of the protocol.
//
//     tag  = HMAC-SHA256(networkKey, frameNumber as u32 LE)[0..2]
//     wide = tag as u16 LE
//     ch   = wide % region->numChannels
//
// Pure function — same inputs give the same channel on every node, forever.
// See the byte-identity note at the top of this file before touching it.
uint8_t hopSequence(const uint8_t networkKey[32], uint32_t frameNumber,
                    const Region* region);

// ── Frame counter ────────────────────────────────────────────────────────────
//
// DEVIATION FROM UPSTREAM — time base.
//
// Upstream derives the frame number from a local millis() epoch that is
// anchored by a JoinBeacon heard on a fixed rendezvous channel, then kept
// aligned by SYNC packets carrying a `frames_since_sync` authority metric:
//
//     current_frame = base_frame + (millis() - hop_epoch_ms) / frame_duration
//
// That design exists because SPECTRA targets boards with no dependable clock,
// so the mesh has to bootstrap a shared notion of time over the air.
//
// The T-Deck Plus has a built-in GPS and an RTC that MeshCore already keeps
// set, so we take the shorter road: the frame number is derived directly from
// UTC. Every node with a valid clock agrees on the frame number with no
// beacons, no rendezvous channel, and no drift-correction protocol at all.
//
//     frameNumber = (utcEpochMs) / FRAME_DURATION_MS
//
// The consequences of this choice, stated plainly:
//   * We do NOT interoperate with upstream SPECTRA nodes' frame numbering.
//     (We could not anyway — our packets are MeshCore, theirs are not.)
//   * FHSS is unavailable until the clock is set. Callers must check
//     clockIsValid() and stay on the fixed mesh frequency when it is false,
//     rather than hopping alone off into a channel nobody else is on.
//   * The JoinBeacon wire format and the SYNC authority mechanism are
//     deliberately NOT ported. They are dead weight when the time base is
//     absolute. docs/FHSS.md records what it would take to add them back.
//
// What is emphatically NOT deviated from: hopSequence() and the region
// table. Those stay byte-identical to upstream so this port stays auditable
// against the reference implementation and its test vectors.

// True when the RTC holds a plausible wall-clock time (post-2023). FHSS must
// not be engaged when this is false.
bool clockIsValid(uint32_t utcEpochSec);

// Frame number for a given UTC timestamp. Returns 0 when the clock is invalid.
uint32_t frameNumberForUtc(uint32_t utcEpochSec);

// ── Network key ──────────────────────────────────────────────────────────────

// Derives the 32-byte hop-sequence key from the shared public-channel PSK:
//
//     networkKey = SHA-256(psk string)
//
// Every node on the same public channel therefore lands on the same hop
// sequence with no extra configuration, and two meshes with different PSKs
// hop independently. This key seeds hopSequence() only — it is never used to
// encrypt anything, and it is not a secret we rely on for confidentiality.
void deriveNetworkKey(const char* psk, uint8_t out[32]);

// ── Region selection ─────────────────────────────────────────────────────────

// Why FHSS is not offered on a given radio profile.
enum class Unavailable : uint8_t {
    None = 0,           // FHSS is available on this profile
    NoPlanForBand,      // 433 MHz — upstream defines no hop plan for this band
    EuSubBandTradeoff,  // EU 868 — a plan exists but we decline to use it
};

// Explains whether FHSS is offered on a profile, and if not, why.
Unavailable availability(uint8_t radioProfile);

// Maps one of MeshService's radio profile indices to the FHSS region whose
// band it sits in. Returns nullptr whenever availability() != None — callers
// must treat that as "FHSS unavailable" and stay on the fixed frequency.
//
// NOTE — EU profiles deliberately return nullptr even though REGION_EU868
// exists and is a correct port. See the rationale on availability() in
// Fhss.cpp and the EU section of docs/FHSS.md. The region is retained in the
// table so this port stays a faithful, testable mirror of upstream; it is
// simply not wired to any profile.
const Region* regionForProfile(uint8_t radioProfile);

}  // namespace fhss
}  // namespace ops

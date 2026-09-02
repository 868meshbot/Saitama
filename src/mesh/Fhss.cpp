// Saitama — Fhss.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Ported from the SPECTRA mesh protocol — see Fhss.h for full provenance,
// attribution, and the explanation of why this arithmetic must stay
// byte-identical to the reference implementation.
//
//     Upstream: https://codeberg.org/thewrew/spectra-mesh
//     Reference: spectra-protocol/src/fhss.rs  (canonical)
//                spectra-firmware-tdeck-pio/lib/spectra-protocol/spectra_fhss.cpp

#include "Fhss.h"
#include <string.h>
#include <SHA256.h>

namespace ops {
namespace fhss {

// ── Region table ─────────────────────────────────────────────────────────────
// Values copied verbatim from the upstream region table. These numbers are
// protocol constants: numChannels feeds the modulus in hopSequence(), and the
// three frequency fields feed the channel→frequency mapping. Editing any of
// them desynchronises this node from every other node using the same region.

const Region REGION_EU868 = {
    "EU868", 868.0f, 0.1f,    0.2f,    3,   0, 14
};

const Region REGION_US915 = {
    "US915", 902.0f, 0.1f,    0.2f,  130,   0, 22
};

const Region REGION_AU915 = {
    "AU915", 915.0f, 0.1f,    0.2f,   65,   0, 22
};

const Region REGION_IN865 = {
    "IN865", 865.0f, 0.0625f, 0.340f,  3,   0, 30
};

const Region REGION_RU864 = {
    "RU864", 864.0f, 0.1f,    0.2f,    7,   0, 14
};

// ── Channel plan ─────────────────────────────────────────────────────────────

// Compute the channel centre frequency in MHz using double precision,
// regardless of the Region fields' float storage.
//
// This is not an optimisation, it is a compatibility fix inherited from
// upstream (their qs#235). The canonical Rust implementation does this
// arithmetic in f64 throughout, so a float version here would tune to
// measurably different frequencies than a Rust-built peer.
//
// Measured on this expression for US915 (see docs/FHSS.md §3): computing in
// float instead of double shifts 52 of the 130 channels, by up to 61 Hz on
// the float-MHz value we hand to setFrequency(). Upstream's own comment cites
// 50-500 Hz; we have not reproduced the upper end of that range, but the
// effect is real and there is no reason to spend it.
//
// The struct fields stay float because the regional constants (902.0, 0.2,
// ...) are exact at f32 precision; only the accumulating `channel * spacing`
// product needs the wider type.
static double channelFreqMHzDouble(uint8_t channel, const Region* region)
{
    return static_cast<double>(region->baseFreqMHz) +
           static_cast<double>(region->channelOffsetMHz) +
           static_cast<double>(channel) *
               static_cast<double>(region->channelSpacingMHz);
}

float channelToFreqMHz(uint8_t channel, const Region* region)
{
    if (!region || channel >= region->numChannels) return 0.0f;
    return static_cast<float>(channelFreqMHzDouble(channel, region));
}

uint32_t channelToFreqHz(uint8_t channel, const Region* region)
{
    if (!region || channel >= region->numChannels) return 0;
    // Round-to-nearest Hz, matching upstream's parity note for the Rust
    // f64 -> u32 cast.
    return static_cast<uint32_t>(
        channelFreqMHzDouble(channel, region) * 1000000.0 + 0.5);
}

// ── HMAC-SHA256 ──────────────────────────────────────────────────────────────
//
// Plain RFC 2104 HMAC. Written out longhand rather than using the Crypto
// library's resetHMAC()/finalizeHMAC() helpers so that it can be read
// side-by-side against the upstream `hmac_sha256_full()` and verified to be
// doing the same thing to the same bytes in the same order.

static constexpr size_t SHA256_BLOCK = 64;
static constexpr size_t SHA256_DIGEST = 32;

static void sha256Hash(const uint8_t* data, size_t len, uint8_t out[32])
{
    SHA256 h;
    h.reset();
    h.update(data, len);
    h.finalize(out, SHA256_DIGEST);
}

static void hmacSha256Full(const uint8_t* key, size_t keyLen,
                           const uint8_t* data, size_t dataLen,
                           uint8_t out[32])
{
    uint8_t kPrime[SHA256_BLOCK];
    memset(kPrime, 0, SHA256_BLOCK);

    if (keyLen > SHA256_BLOCK) {
        sha256Hash(key, keyLen, kPrime);   // K' = H(K)
    } else {
        memcpy(kPrime, key, keyLen);
    }

    // Inner hash: H((K' ^ ipad) || message)
    uint8_t ipad[SHA256_BLOCK];
    for (size_t i = 0; i < SHA256_BLOCK; ++i) ipad[i] = kPrime[i] ^ 0x36;

    SHA256 inner;
    inner.reset();
    inner.update(ipad, SHA256_BLOCK);
    inner.update(data, dataLen);
    uint8_t innerHash[SHA256_DIGEST];
    inner.finalize(innerHash, SHA256_DIGEST);

    // Outer hash: H((K' ^ opad) || innerHash)
    uint8_t opad[SHA256_BLOCK];
    for (size_t i = 0; i < SHA256_BLOCK; ++i) opad[i] = kPrime[i] ^ 0x5C;

    SHA256 outer;
    outer.reset();
    outer.update(opad, SHA256_BLOCK);
    outer.update(innerHash, SHA256_DIGEST);
    outer.finalize(out, SHA256_DIGEST);
}

// ── Hop sequence ─────────────────────────────────────────────────────────────

uint8_t hopSequence(const uint8_t networkKey[32], uint32_t frameNumber,
                    const Region* region)
{
    if (!region || region->numChannels == 0) return 0;

    // Frame number as 4 bytes LITTLE-ENDIAN. This encoding is part of the
    // protocol — see the byte-identity note in Fhss.h.
    uint8_t frameLe[4];
    frameLe[0] = static_cast<uint8_t>( frameNumber        & 0xFF);
    frameLe[1] = static_cast<uint8_t>((frameNumber >> 8)  & 0xFF);
    frameLe[2] = static_cast<uint8_t>((frameNumber >> 16) & 0xFF);
    frameLe[3] = static_cast<uint8_t>((frameNumber >> 24) & 0xFF);

    uint8_t full[SHA256_DIGEST];
    hmacSha256Full(networkKey, 32, frameLe, 4, full);

    // First 2 digest bytes, read back as a little-endian u16. Upstream uses
    // 2 bytes rather than 1 to widen the modulo base; the width and the byte
    // order both change every channel index if altered.
    uint16_t wide = static_cast<uint16_t>(full[0]) |
                    (static_cast<uint16_t>(full[1]) << 8);

    return static_cast<uint8_t>(wide % region->numChannels);
}

// ── Frame counter (DEVIATION — UTC time base; see Fhss.h) ────────────────────

// 2023-11-14; anything below this is an unset clock, not a real timestamp.
static constexpr uint32_t MIN_VALID_EPOCH = 1700000000UL;

bool clockIsValid(uint32_t utcEpochSec)
{
    return utcEpochSec >= MIN_VALID_EPOCH;
}

uint32_t frameNumberForUtc(uint32_t utcEpochSec)
{
    if (!clockIsValid(utcEpochSec)) return 0;
    // 64-bit intermediate so the ms conversion cannot overflow. Not a hot
    // path — evaluated once per MeshService tick.
    uint64_t nowMs = static_cast<uint64_t>(utcEpochSec) * 1000ULL;
    return static_cast<uint32_t>(nowMs / FRAME_DURATION_MS);
}

// ── Network key ──────────────────────────────────────────────────────────────

void deriveNetworkKey(const char* psk, uint8_t out[32])
{
    if (!psk) {
        memset(out, 0, 32);
        return;
    }
    sha256Hash(reinterpret_cast<const uint8_t*>(psk), strlen(psk), out);
}

// ── Region selection ─────────────────────────────────────────────────────────

// FHSS is deliberately NOT offered on the EU 868 profiles.
//
// REGION_EU868 above is a correct, verified port of upstream's plan
// (868.1 / 868.3 / 868.5). The problem is not the port, it is that using it
// here is a bad trade for a Saitama node:
//
//   * Saitama's EU profiles run at 869.525 / 869.618 MHz, which is in the
//     869.4-869.65 sub-band: 500 mW, 10 % duty cycle.
//   * The EU868 hop plan is in the 868.0-868.6 sub-band: 25 mW, 1 % duty.
//
// So enabling it would move the node off the frequency the rest of the mesh
// is on, cut its legal power limit by ~13 dB and its duty-cycle allowance by
// 10x — and buy, in exchange, hopping across just three channels 400 kHz
// apart. Three channels is channel rotation rather than spread spectrum: it
// earns none of the regulatory FHSS relief that wideband hopping gets under
// FCC Part 15.247, and gives very little interference diversity.
//
// US915 (130 channels / 25.8 MHz) and AU915 (65 / 12.8 MHz) are where this
// feature actually pays for itself, so those are the profiles that get it.
//
// REGION_EU868, REGION_IN865 and REGION_RU864 stay in the table above and
// remain covered by the reference test vectors, so the port stays faithful
// and auditable against upstream. They are simply not mapped to a profile.
Unavailable availability(uint8_t radioProfile)
{
    switch (radioProfile) {
        case 6:   // EU 433
        case 9:   // Portugal 433
            return Unavailable::NoPlanForBand;
        case 2:   // EU/UK Narrow         869.618
        case 3:   // EU/UK Long Range     869.525
        case 4:   // EU/UK Medium Range   869.525
        case 5:   // Czech Narrow         869.525
        case 10:  // Portugal 868         869.618
        case 11:  // Switzerland          869.618
            return Unavailable::EuSubBandTradeoff;
        default:
            return Unavailable::None;
    }
}

const Region* regionForProfile(uint8_t radioProfile)
{
    if (availability(radioProfile) != Unavailable::None) return nullptr;

    // Indices match MeshService::applyLoraProfile()'s kProfiles table.
    switch (radioProfile) {
        case 0:  return &REGION_AU915;  // Australia            915.800
        case 1:  return &REGION_AU915;  // Australia Victoria   916.575
        case 7:  return &REGION_AU915;  // New Zealand          917.375
        case 8:  return &REGION_AU915;  // New Zealand Narrow   917.375
        case 12: return &REGION_US915;  // USA/Canada           910.525
        case 13: return &REGION_AU915;  // Vietnam              920.250
        default: return nullptr;        // unknown profile — do not hop
    }
}

}  // namespace fhss
}  // namespace ops

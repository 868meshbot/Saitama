// Saitama — Crypto.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include <cstddef>
#include <cstdint>

namespace ops {
namespace crypto {

// ── What this protects, and what it does not ─────────────────────────
// Secrets stored on the device leave it on the removable SD card: /ops/
// is a documented reflash-proof backup and every file in it is plain
// JSON. This module seals a secret with AES-256-GCM under a key derived
// from a *storage password* kept in its own NVS namespace on internal
// flash — a namespace that is never mirrored to SD. Lift the card and
// you get ciphertext.
//
// It does not protect against someone holding the device or a flash
// dump: they have the salt and the storage password, so they have the
// key. That is the price of never prompting for a passphrase at boot,
// which is the whole point of the feature. Say so plainly in the UI
// rather than implying more than this delivers.

// A sealed secret is a fixed-size blob, so its length says nothing about
// the plaintext: 12-byte IV || 64-byte ciphertext || 16-byte GCM tag.
static constexpr size_t SECRET_MAX   = 64;    // plaintext slot, NUL included
static constexpr size_t IV_LEN       = 12;
static constexpr size_t TAG_LEN      = 16;
static constexpr size_t BLOB_LEN     = IV_LEN + SECRET_MAX + TAG_LEN;  // 92
static constexpr size_t BLOB_HEX_LEN = BLOB_LEN * 2 + 1;               // 185

// The storage password every device starts with. It is published in the
// source and the docs, so it is not a secret — Settings > Storage
// Password exists to replace it, and the UI says when it is still this.
extern const char* const DEFAULT_PASSWORD;    // "changeme1"
static constexpr size_t  PASSWORD_MAX = 32;
static constexpr size_t  PASSWORD_MIN = 4;

// Loads (or creates) the salt and storage password, then derives the
// AES-256 key. Call once from setup(), before repeaters::init().
void init();
bool ready();

// AES-256-GCM. `aad` is authenticated but not encrypted — pass something
// that identifies the record (a repeater's key prefix) so a blob cannot
// be transplanted onto a different entry by editing the JSON.
bool seal(const char* plain, const uint8_t* aad, size_t aadLen,
          uint8_t out[BLOB_LEN]);
bool unseal(const uint8_t blob[BLOB_LEN], const uint8_t* aad, size_t aadLen,
            char* out, size_t outMax);

// Hex helpers, for putting a blob in JSON.
void toHex  (const uint8_t* in, size_t len, char* out, size_t outMax);
bool fromHex(const char* hex, uint8_t* out, size_t len);

// ── Storage password ─────────────────────────────────────────────────
bool matchesStoragePassword(const char* pw);
bool storagePasswordIsDefault();

// Two-phase re-key, so callers can re-encrypt what they hold in one pass.
//
// beginRekey() verifies `oldPw`, derives the new key and *persists the new
// password immediately* — the one step that can fail is done first and
// checked. Until endRekey(), seal() uses the new key while unseal() still
// uses the old one, so a caller can walk its records decrypt-then-encrypt.
// endRekey() makes the new key active for both and must always be called.
bool beginRekey(const char* oldPw, const char* newPw);
void endRekey();
bool rekeyInProgress();

}  // namespace crypto
}  // namespace ops

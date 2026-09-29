// Saitama — IdentityBackup.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include <cstddef>
#include <cstdint>

namespace ops {
namespace idbackup {

// ── Encrypted node identity on the SD card ───────────────────────────
// /ops/identity.enc holds the node's private key, sealed with the storage
// password (ops::crypto). The file carries the salt, so after internal flash
// is wiped the identity can be restored from the password alone.
//
//   off  len
//     0    4  magic "SIDE"
//     4    1  version (1)
//     5   16  salt           \  public; authenticated as AAD so an edited
//    21   32  public key     /  header fails to decrypt
//    53   12  IV
//    65  128  ciphertext: pub[32] + prv[64] + name[32] (IdentityStore format)
//   193   16  GCM tag
//   209
//
// The public key is in the clear so "does the card match this node?" needs
// no password. See docs/STORED_SECRETS.md.

static constexpr size_t PLAIN_LEN = 128;
static constexpr const char* ENC_PATH    = "/ops/identity.enc";
static constexpr const char* LEGACY_PATH = "/ops/identity.bin";   // plaintext, pre-encryption

enum class Status { None, Ok, Locked, Corrupt };

// Seals `plain` (IdentityStore format) under the storage password and writes
// ENC_PATH, then deletes any plaintext copies left by older firmware.
bool write(const uint8_t plain[PLAIN_LEN]);

// Reads ENC_PATH with the current storage password. Locked = the file is
// intact but this password doesn't open it (e.g. internal flash was wiped
// and the password was changed before).
// fileSaltOut (optional) receives the file's salt on success.
Status read(uint8_t out[PLAIN_LEN], uint8_t fileSaltOut[16] = nullptr);

// Tries `pw` against ENC_PATH. On success `fileSaltOut` holds the file's
// salt, for crypto::adopt().
bool unlock(const char* pw, uint8_t out[PLAIN_LEN], uint8_t fileSaltOut[16]);

// Public key from the file header — no password needed.
bool publicKey(uint8_t out[32]);

bool exists();
// Moves a locked backup aside (to identity.enc.old) rather than deleting it.
bool setAside();

}  // namespace idbackup
}  // namespace ops

// Saitama — IdentityBackup.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "IdentityBackup.h"
#include "Crypto.h"
#include "Log.h"
#include "SDCard.h"
#include <SD.h>
#include <cstring>

namespace ops {
namespace idbackup {

static constexpr uint8_t VERSION  = 1;
static constexpr size_t  OFF_SALT = 5;
static constexpr size_t  OFF_PUB  = OFF_SALT + crypto::SALT_LEN;   // 21
static constexpr size_t  HDR_LEN  = OFF_PUB + 32;                  // 53 — the AAD
static constexpr size_t  OFF_IV   = HDR_LEN;
static constexpr size_t  OFF_CT   = OFF_IV + crypto::IV_LEN;       // 65
static constexpr size_t  OFF_TAG  = OFF_CT + PLAIN_LEN;            // 193
static constexpr size_t  FILE_LEN = OFF_TAG + crypto::TAG_LEN;     // 209

static const char* const OLD_PATH        = "/ops/identity.enc.old";
static const char* const LEGACY_BAK_PATH = "/ops/identity.bak";

static bool _readFile(uint8_t f[FILE_LEN])
{
    if (!sdcard::isMounted()) return false;
    size_t len = 0;
    if (!sdcard::readFile(ENC_PATH, f, FILE_LEN, &len) || len != FILE_LEN) return false;
    return memcmp(f, "SIDE", 4) == 0 && f[4] == VERSION;
}

bool exists() { return sdcard::isMounted() && SD.exists(ENC_PATH); }

bool write(const uint8_t plain[PLAIN_LEN])
{
    if (!sdcard::isMounted()) return false;
    if (!crypto::ready()) {
        // Never fall back to plaintext: no backup beats a readable key.
        OPS_LOG("IdBackup", "Storage key unavailable - identity not backed up");
        return false;
    }

    uint8_t f[FILE_LEN];
    memcpy(f, "SIDE", 4);
    f[4] = VERSION;
    memcpy(f + OFF_SALT, crypto::salt(), crypto::SALT_LEN);
    memcpy(f + OFF_PUB, plain, 32);   // IdentityStore format starts with pub[32]
    bool ok = crypto::sealBytes(plain, PLAIN_LEN, f, HDR_LEN,
                                f + OFF_IV, f + OFF_CT, f + OFF_TAG)
           && sdcard::writeFile(ENC_PATH, f, FILE_LEN);
    memset(f, 0, sizeof(f));
    if (!ok) {
        OPS_LOG("IdBackup", "Writing %s failed", ENC_PATH);
        return false;
    }

    // The encrypted copy is on the card: drop plaintext ones older firmware left.
    if (SD.exists(LEGACY_PATH))     SD.remove(LEGACY_PATH);
    if (SD.exists(LEGACY_BAK_PATH)) SD.remove(LEGACY_BAK_PATH);
    return true;
}

static bool _open(const char* pw, const uint8_t f[FILE_LEN], uint8_t out[PLAIN_LEN])
{
    return crypto::unsealBytes(pw, f + OFF_SALT, f + OFF_IV, f + OFF_CT, PLAIN_LEN,
                               f + OFF_TAG, f, HDR_LEN, out);
}

Status read(uint8_t out[PLAIN_LEN], uint8_t fileSaltOut[16])
{
    if (!exists()) return Status::None;
    uint8_t f[FILE_LEN];
    if (!_readFile(f)) {
        OPS_LOG("IdBackup", "%s unreadable or unknown format", ENC_PATH);
        return Status::Corrupt;
    }
    bool ok = _open(nullptr, f, out);
    if (ok && fileSaltOut) memcpy(fileSaltOut, f + OFF_SALT, crypto::SALT_LEN);
    memset(f, 0, sizeof(f));
    return ok ? Status::Ok : Status::Locked;
}

bool unlock(const char* pw, uint8_t out[PLAIN_LEN], uint8_t fileSaltOut[16])
{
    uint8_t f[FILE_LEN];
    if (!pw || !_readFile(f)) return false;
    bool ok = _open(pw, f, out);
    if (ok && fileSaltOut) memcpy(fileSaltOut, f + OFF_SALT, crypto::SALT_LEN);
    memset(f, 0, sizeof(f));
    return ok;
}

bool publicKey(uint8_t out[32])
{
    uint8_t f[FILE_LEN];
    if (!_readFile(f)) return false;
    memcpy(out, f + OFF_PUB, 32);
    return true;
}

bool setAside()
{
    if (!exists()) return true;
    if (SD.exists(OLD_PATH)) SD.remove(OLD_PATH);
    bool ok = SD.rename(ENC_PATH, OLD_PATH);
    OPS_LOG("IdBackup", "Locked backup moved to %s: %s", OLD_PATH, ok ? "ok" : "FAILED");
    return ok;
}

}  // namespace idbackup
}  // namespace ops

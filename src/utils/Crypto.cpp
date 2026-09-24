// Saitama — Crypto.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "Crypto.h"
#include "Log.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <cstring>

namespace ops {
namespace crypto {

const char* const DEFAULT_PASSWORD = "changeme1";

// The storage password lives in its own NVS namespace, deliberately not in
// ops::Config — config::save() mirrors every setting to /ops/settings.json,
// which would write the key material onto the very card this is meant to
// protect.
static constexpr const char* NVS_NS   = "opscrypt";
static constexpr size_t      SALT_LEN = 16;

// PBKDF2 rounds buy nothing against an attacker holding the flash (they have
// the password too). They are here for the case the guard actually covers: a
// lifted SD card plus a guessable password.
static constexpr int PBKDF2_ITERS = 20000;

static uint8_t s_salt[SALT_LEN]            = {};
static uint8_t s_key[32]                   = {};
static uint8_t s_newKey[32]                = {};
static char    s_pw[PASSWORD_MAX + 1]      = {};
static bool    s_ready                     = false;
static bool    s_rekeying                  = false;

// ── Key derivation ───────────────────────────────────────────────────

static bool _deriveKey(const char* pw, uint8_t out[32])
{
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = false;
    if (info && mbedtls_md_setup(&ctx, info, 1) == 0) {
        ok = mbedtls_pkcs5_pbkdf2_hmac(&ctx,
                                       (const unsigned char*)pw, strlen(pw),
                                       s_salt, SALT_LEN,
                                       PBKDF2_ITERS, 32, out) == 0;
    }
    mbedtls_md_free(&ctx);
    return ok;
}

// ── init() ───────────────────────────────────────────────────────────

void init()
{
    Preferences p;
    if (!p.begin(NVS_NS, false)) {
        OPS_LOG("Crypto", "NVS open failed - stored secrets unavailable");
        return;
    }

    if (p.getBytesLength("salt") != SALT_LEN ||
        p.getBytes("salt", s_salt, SALT_LEN) != SALT_LEN) {
        esp_fill_random(s_salt, SALT_LEN);
        p.putBytes("salt", s_salt, SALT_LEN);
        OPS_LOG("Crypto", "Generated new storage salt");
    }

    if (p.getString("pw", s_pw, sizeof(s_pw)) == 0 || !s_pw[0]) {
        strncpy(s_pw, DEFAULT_PASSWORD, PASSWORD_MAX);
        s_pw[PASSWORD_MAX] = '\0';
        p.putString("pw", s_pw);
        OPS_LOG("Crypto", "Storage password set to default - change it in Settings");
    }
    p.end();

    uint32_t t0 = millis();
    s_ready = _deriveKey(s_pw, s_key);
    if (s_ready)
        OPS_LOG("Crypto", "Storage key derived in %lu ms%s",
                (unsigned long)(millis() - t0),
                storagePasswordIsDefault() ? " (default password)" : "");
    else
        OPS_LOG("Crypto", "Key derivation FAILED - stored secrets unavailable");
}

bool ready()          { return s_ready; }
bool rekeyInProgress(){ return s_rekeying; }

// ── seal / unseal ────────────────────────────────────────────────────

bool seal(const char* plain, const uint8_t* aad, size_t aadLen,
          uint8_t out[BLOB_LEN])
{
    if (!s_ready || !out) return false;
    if (!plain) plain = "";
    size_t len = strlen(plain);
    if (len >= SECRET_MAX) return false;

    // Zero-pad to the full slot: every blob is the same size, so the
    // ciphertext length reveals nothing about the password length.
    uint8_t pt[SECRET_MAX];
    memset(pt, 0, sizeof(pt));
    memcpy(pt, plain, len);

    esp_fill_random(out, IV_LEN);   // fresh IV per seal — never reuse under one key

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    const uint8_t* key = s_rekeying ? s_newKey : s_key;
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0
           && mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, SECRET_MAX,
                                        out, IV_LEN, aad, aadLen,
                                        pt, out + IV_LEN,
                                        TAG_LEN, out + IV_LEN + SECRET_MAX) == 0;
    mbedtls_gcm_free(&gcm);
    memset(pt, 0, sizeof(pt));
    if (!ok) memset(out, 0, BLOB_LEN);
    return ok;
}

bool unseal(const uint8_t blob[BLOB_LEN], const uint8_t* aad, size_t aadLen,
            char* out, size_t outMax)
{
    if (!out || outMax == 0) return false;
    out[0] = '\0';
    if (!s_ready || !blob) return false;

    uint8_t pt[SECRET_MAX];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_key, 256) == 0
           && mbedtls_gcm_auth_decrypt(&gcm, SECRET_MAX,
                                       blob, IV_LEN, aad, aadLen,
                                       blob + IV_LEN + SECRET_MAX, TAG_LEN,
                                       blob + IV_LEN, pt) == 0;
    mbedtls_gcm_free(&gcm);
    if (!ok) { memset(pt, 0, sizeof(pt)); return false; }

    pt[SECRET_MAX - 1] = '\0';   // a tampered-but-authentic blob still cannot run off the end
    strncpy(out, (const char*)pt, outMax - 1);
    out[outMax - 1] = '\0';
    memset(pt, 0, sizeof(pt));
    return true;
}

// ── Hex ──────────────────────────────────────────────────────────────

void toHex(const uint8_t* in, size_t len, char* out, size_t outMax)
{
    if (!out || outMax == 0) return;
    out[0] = '\0';
    if (!in || outMax < len * 2 + 1) return;
    for (size_t i = 0; i < len; i++)
        snprintf(out + i * 2, 3, "%02X", in[i]);
}

bool fromHex(const char* hex, uint8_t* out, size_t len)
{
    if (!hex || !out) return false;
    if (strlen(hex) != len * 2) return false;
    for (size_t i = 0; i < len; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
        char* end = nullptr;
        long v = strtol(b, &end, 16);
        if (end != b + 2) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

// ── Storage password ─────────────────────────────────────────────────

bool matchesStoragePassword(const char* pw)
{
    if (!s_ready || !pw) return false;
    return strncmp(pw, s_pw, PASSWORD_MAX + 1) == 0;
}

bool storagePasswordIsDefault()
{
    return strcmp(s_pw, DEFAULT_PASSWORD) == 0;
}

bool beginRekey(const char* oldPw, const char* newPw)
{
    if (!s_ready || s_rekeying)        return false;
    if (!oldPw || !newPw)              return false;
    size_t nlen = strlen(newPw);
    if (nlen < PASSWORD_MIN || nlen > PASSWORD_MAX) return false;
    if (!matchesStoragePassword(oldPw)) return false;

    if (!_deriveKey(newPw, s_newKey)) return false;

    // Persist before re-encrypting anything. If this write fails we abandon
    // the change with nothing touched; if it succeeds and the caller then
    // dies mid-pass, the worst case is losing stored secrets, which the user
    // can re-enter — not a device that cannot read its own storage password.
    Preferences p;
    if (!p.begin(NVS_NS, false)) { memset(s_newKey, 0, sizeof(s_newKey)); return false; }
    bool wrote = p.putString("pw", newPw) > 0;
    p.end();
    if (!wrote) { memset(s_newKey, 0, sizeof(s_newKey)); return false; }

    strncpy(s_pw, newPw, PASSWORD_MAX);
    s_pw[PASSWORD_MAX] = '\0';
    s_rekeying = true;
    return true;
}

void endRekey()
{
    if (!s_rekeying) return;
    memcpy(s_key, s_newKey, sizeof(s_key));
    memset(s_newKey, 0, sizeof(s_newKey));
    s_rekeying = false;
    OPS_LOG("Crypto", "Storage password changed");
}

}  // namespace crypto
}  // namespace ops

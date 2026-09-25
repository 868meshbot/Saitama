// Saitama — Repeaters.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include "Contacts.h"   // PATH_AT_PINNED, outPathByteCount()
#include <Arduino.h>
#include <cstdint>
#include "Crypto.h"

namespace ops {

struct Repeater {
    char     name[32];
    uint8_t  pubKeyPrefix[4];
    uint32_t lastSeen;
    float    lastRssi;
    uint8_t  pubKey[32];     // full 32-byte public key; populated from advert
    int32_t  lat;            // last-known latitude  × 1 000 000 (0 = unknown)
    int32_t  lon;            // last-known longitude × 1 000 000 (0 = unknown)
    bool     outPathValid;   // true = outPath/outPathLen hold a usable route
    uint8_t  outPathLen;     // 0 = direct neighbour; 0xFF = unknown
    bool     favourite;      // pinned to top of list; occupies former _pathPad[0]
    uint8_t  _pathPad[1];
    uint8_t  outPath[64];    // MeshCore out_path bytes (MAX_PATH_SIZE = 64)
    bool     hasAdminPw;     // true = adminPwEnc holds a sealed admin password
    uint8_t  _pwPad[3];
    // Admin password sealed with AES-256-GCM under the storage key, with
    // pubKeyPrefix as additional authenticated data. Written to SD in this
    // form and never in the clear. See utils/Crypto.h for the threat model.
    uint8_t  adminPwEnc[crypto::BLOB_LEN];
    // When outPath was confirmed (unix time): 0 = unknown age, PATH_AT_PINNED
    // (Contacts.h) = set by hand. Appended last: NVS blobs are read as a prefix.
    uint32_t pathAt;
};

namespace repeaters {
    static constexpr int CAPACITY = 250;

    void init();   // loads from SD; no-op if SD not mounted
    void save();   // writes /ops/repeaters.json
    // Reload from SD JSON and resave to NVS. Returns count loaded, or -1 on failure.
    int  reloadFromSD();
    int  count();
    bool get(int idx, Repeater& out);
    bool findByKey(const uint8_t prefix[4], int* outIdx = nullptr);
    void add(const Repeater& r);   // insert or update by pubKeyPrefix
    void setFavourite(int idx, bool fav);
    void remove(int idx);
    // Persist a MeshCore path for a repeater. pathLen is MeshCore's encoded
    // out_path_len; learnedAt is when it was confirmed (or PATH_AT_PINNED).
    // Only saves when the path changes.
    void setPath(int idx, uint8_t pathLen, const uint8_t* path, uint32_t learnedAt);
    // Clear a repeater's path (sets outPathValid=false, outPathLen=0xFF) and persists.
    // Called on direct-send timeout so a reboot does not reload the stale path.
    void clearPath(int idx);
    // Update lat/lon in the in-memory array without triggering a full NVS save.
    // Position is persisted on the next natural save() call.
    void setPosition(int idx, int32_t lat, int32_t lon);
    // Populate pubKey[32] from a live advert if it is not yet set (all zeros).
    // No-op and no save if the key is already known; saves immediately on first fill.
    void setFullKey(int idx, const uint8_t* pubKey32);
    // Update name, lastSeen, and lastRssi from a live advert/packet without
    // triggering a full NVS save. Persisted on the next natural save() call.
    void setLiveData(int idx, const char* name, uint32_t lastSeen, float lastRssi);

    // ── Remembered admin password ─────────────────────────────────────
    bool hasAdminPassword(int idx);
    // Seals and persists `plain`. nullptr or "" forgets the stored password.
    // Returns false if the secret is too long or crypto is unavailable.
    bool setAdminPassword(int idx, const char* plain);
    // Unseals into `out`. False (and out[0] = 0) when nothing is stored or the
    // blob will not authenticate — a wrong storage key, or an edited SD file.
    bool getAdminPassword(int idx, char* out, size_t outMax);
    // Re-encrypts every stored password under a new storage key. Call only
    // between crypto::beginRekey() and crypto::endRekey(). Entries that fail
    // to unseal are forgotten rather than left unreadable; `outLost` receives
    // that count. Returns the number successfully re-encrypted.
    int  rekeyAdminPasswords(int* outLost = nullptr);
}

}  // namespace ops

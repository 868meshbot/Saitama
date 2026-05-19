// Saitama — Contacts.h
// Copyright 2026 Saitama — MIT License

#pragma once
#include <Arduino.h>
#include <cstdint>

namespace ops {

struct Contact {
    char     name[32];
    uint8_t  pubKeyPrefix[4];   // first 4 bytes of public key
    uint8_t  pubKey[32];        // full 32-byte Ed25519 public key; zeros = not yet known
    uint32_t lastSeen;           // unix timestamp
    float    lastRssi;
    bool     hasUnread;
    bool     favourite;          // pinned to top of list; occupies former _pad[0]
    uint8_t  _pad[2];            // pad to 4-byte boundary
    int32_t  lat;                // last-known latitude  × 1 000 000 (0 = unknown)
    int32_t  lon;                // last-known longitude × 1 000 000 (0 = unknown)
    bool     outPathValid;       // true = outPath/outPathLen hold a usable route
    uint8_t  outPathLen;         // 0 = direct neighbour; 0xFF = unknown
    uint8_t  _pathPad[2];
    uint8_t  outPath[64];        // MeshCore out_path bytes (MAX_PATH_SIZE = 64)
};

namespace contacts {
    static constexpr int CAPACITY = 50;

    void init();
    void save();
    // Reload from SD JSON and resave to NVS. Returns count loaded, or -1 on failure.
    int  reloadFromSD();
    int  count();
    bool get(int idx, Contact& out);
    bool anyUnread();                        // true if any contact has hasUnread set
    // Returns true if a contact with this 4-byte prefix exists; fills *outIdx.
    bool findByKey(const uint8_t prefix[4], int* outIdx = nullptr);
    void add(const Contact& c);              // insert or update by pubKeyPrefix
    void setUnread(int idx, bool unread);
    void setFavourite(int idx, bool fav);
    void remove(int idx);
    // Persist learned MeshCore path for a contact. Only saves when path changes.
    void setPath(int idx, uint8_t pathLen, const uint8_t* path);
    // Clear a contact's path (sets outPathValid=false, outPathLen=0xFF) and persists.
    // Called on direct-send timeout so a reboot does not reload the stale path.
    void clearPath(int idx);
    // Update lat/lon in the in-memory array without triggering a full NVS save.
    // Position is persisted on the next natural save() call.
    void setPosition(int idx, int32_t lat, int32_t lon);
}

}  // namespace ops

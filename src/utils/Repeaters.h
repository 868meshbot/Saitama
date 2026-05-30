// Saitama — Repeaters.h
// Copyright 2026 Saitama — MIT License

#pragma once
#include <Arduino.h>
#include <cstdint>

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
    // Persist learned MeshCore path for a repeater. Only saves when path changes.
    void setPath(int idx, uint8_t pathLen, const uint8_t* path);
    // Clear a repeater's path (sets outPathValid=false, outPathLen=0xFF) and persists.
    // Called on direct-send timeout so a reboot does not reload the stale path.
    void clearPath(int idx);
    // Update lat/lon in the in-memory array without triggering a full NVS save.
    // Position is persisted on the next natural save() call.
    void setPosition(int idx, int32_t lat, int32_t lon);
    // Populate pubKey[32] from a live advert if it is not yet set (all zeros).
    // No-op and no save if the key is already known; saves immediately on first fill.
    void setFullKey(int idx, const uint8_t* pubKey32);
}

}  // namespace ops

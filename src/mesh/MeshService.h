// Saitama — MeshService.h
// Copyright 2026 Saitama — MIT License
//
// Singleton wrapper around the MeshCore radio + protocol stack.
// Board.cpp owns the hardware peripherals; MeshService owns the radio.

#pragma once

#include <Arduino.h>
#include <cstdint>

namespace ops {

// Trace result: populated by onTraceRecv (fires when TRACE reaches end of its path).
// numHops  = number of hashes in the path (each hash = 1–2 bytes of a node's pub_key).
// hashes[] = path_hashes byte array, laid out as numHops * hashSz bytes.
// snrs[]   = SNR*4 (int8) per forwarding node (numHops − 1 values before final bounce).
struct TraceResult {
    uint32_t tag;
    uint8_t  numHops;      // total hashes in path_hashes (includes contact + repeaters)
    uint8_t  hashSz;       // bytes per hash (1 or 2)
    int8_t   snrs[64];     // accumulated SNR*4 values (pkt->path at time of receipt)
    uint8_t  hashes[128];  // raw path_hashes bytes
    uint8_t  numSnrs;      // count of valid SNR values (may be numHops or numHops-1)
    uint8_t  targetPubKeyPrefix[4];
};

struct RxMessage {
    uint32_t timestamp;
    char     senderName[32];
    char     text[160];
    float    rssi;
    float    snr;
    bool     isDirect;
    uint8_t  hops;             // number of hops the packet took (0 = direct neighbour)
    char     channelName[32];  // empty string when isDirect
    uint8_t  pubKeyPrefix[4];  // sender's first 4 key bytes (zeros for channel msgs)
    char     pathStr[36];      // ">"-separated uppercase hex repeater codes, e.g. "AA>B1>3F"
};

struct PathInfo {
    bool    found;       // false when contact not in mesh table
    bool    known;       // false when out_path_len == OUT_PATH_UNKNOWN
    bool    direct;      // true when known path has 0 hops (direct neighbour)
    uint8_t hopCount;    // number of relay hops (0 when direct)
    uint8_t hashSz;      // bytes per hash (1 or 2)
};

struct RadioStats {
    uint32_t packetsSent;
    uint32_t packetsRecv;
    uint32_t packetsRecvError;
    float    lastRssi;
    float    lastSnr;
    int16_t  noiseFloor;
    bool     radioOk;
    bool     active;
    // Per-type packet counters (from Dispatcher)
    uint32_t floodSent;
    uint32_t floodRecv;
    uint32_t directSent;
    uint32_t directRecv;
    // Cumulative airtime in milliseconds (from Dispatcher)
    uint32_t airtimeTxMs;
    uint32_t airtimeRxMs;
    bool     loraDutyCycleActive;
};

struct PeerInfo {
    char     name[32];
    uint8_t  pubKeyPrefix[4];
    uint8_t  pubKey[32];  // full 32-byte public key; populated from advert
    uint8_t  type;        // 1=Chat, 2=Repeater, 3=Room
    uint32_t lastSeen;    // unix timestamp
    float    lastRssi;
    int32_t  lat;         // last-known latitude  × 1 000 000 (0 = unknown)
    int32_t  lon;         // last-known longitude × 1 000 000 (0 = unknown)
};

// Result from a DISCOVER_REQ zero-hop scan (direct RF neighbors only).
struct DiscoverEntry {
    char     name[32];       // node name if found in peer list; empty if unknown
    uint8_t  pubKey[32];     // full 32-byte public key from discover response
    uint8_t  pubKeyPrefix[4];
    uint8_t  nodeType;       // ADV_TYPE_CHAT=1, ADV_TYPE_REPEATER=2, etc.
    float    rssi;           // RSSI of the discover response at this node
    int8_t   snrInbound;     // SNR×4 at the remote node (their receive quality)
};

class MeshService {
public:
    static MeshService& instance();

    void init();
    void tick();

    // ── Transmit ─────────────────────────────────────────────────────
    bool sendChannel(int chIdx, const char* text);    // chIdx 0=public, 1-9=custom slots
    bool sendDirect(const uint8_t* pubKeyPrefix4, const char* text);
    bool sendAdvert(int delayMs = 0, bool flood = true);

    // ── Receive ───────────────────────────────────────────────────────
    bool dequeueMessage(RxMessage& out);
    int  messageCount() const;

    // ── Peers ─────────────────────────────────────────────────────────
    int      peerCount()  const;
    uint32_t peerSerial() const;  // increments on every peer add or update
    bool getPeer(int idx, PeerInfo& out) const;

    // ── ACK ───────────────────────────────────────────────────────────
    // Returns true (once) when an ACK matching a previously sent DM arrives.
    // Fills acked_crc with the CRC of the acked message.
    bool pollAck(uint32_t& acked_crc);
    // CRC the last sendDirect expects to be ACKed (0 if send failed).
    uint32_t lastExpectedAck() const;

    // ── Trace ─────────────────────────────────────────────────────────
    // Sends a TRACE (0x09) packet along the known path to a contact.
    // Appends the contact's own hash so the final retransmission triggers
    // onTraceRecv on any node in range — including the initiator.
    // Returns true if the trace was queued; fills out_tag with the random tag.
    bool sendTrace(const uint8_t* pubKeyPrefix4, uint32_t& out_tag);

    // Returns true (once) when a trace result is waiting.
    bool pollTraceResult(struct TraceResult& out);

    // True if this contact has a known direct path (can be traced).
    bool hasPathTo(const uint8_t* pubKeyPrefix4) const;

    // ── Channel PSK utility ───────────────────────────────────────────
    // Derives the standard MeshCore channel PSK from a name.
    // Output is a null-terminated base64 string (24 chars + '\0') in psk_out.
    static void deriveChannelPsk(const char* name, char* psk_out, int psk_size);

    // Normalizes a user-supplied PSK to the base64 form MeshCore expects.
    // Accepts 32-char hex (share-link format) and converts it to 24-char base64.
    // Any other input is copied unchanged (assumed already base64).
    // out must be at least 25 bytes.
    static void normalizePsk(const char* in, char* out, int outSize);

    // Updates the live channel registration for config slot chIdx without reboot.
    // If the slot already has a registered channel, its secret and hash are updated
    // in place. If the slot is newly configured, it is registered via addChannel.
    void syncChannel(int chIdx);

    // ── Repeater queries ──────────────────────────────────────────────
    // Sends REQ_TYPE_GET_STATUS to every repeater in the repeater list.
    // Responses arrive asynchronously via pollContactResponse().
    bool sendRepeatersStatus(int timeoutSecs = 30);
    // Sends a login request to the contact identified by prefix4.
    // Response arrives via pollContactResponse().
    bool sendRepeaterLogin(const uint8_t* prefix4, const char* password);
    // Sends an admin CLI command to the contact identified by prefix4.
    // The repeater must have accepted a prior login from this node.
    // Response arrives via pollContactResponse().
    bool sendAdminCommand(const uint8_t* prefix4, const char* command);
    // Single-target GET_STATUS request using the REQ_TYPE binary protocol.
    bool sendRepeaterStatusReq(const uint8_t* prefix4);
    // GET_NEIGHBOURS request (10 entries, newest first, 4-byte prefix).
    bool sendRepeaterNeighboursReq(const uint8_t* prefix4);
    // Returns true (once per response) when a contact response line is waiting.
    bool pollContactResponse(char* out, int outMax);
    // Returns true (once) when a login attempt result is ready; ok=true on success.
    bool pollLoginResult(bool& ok);

    // ── Radio control / stats ─────────────────────────────────────────
    void suspendDutyCycle(bool suspend);
    RadioStats radioStats() const;
    void  setActive(bool active);
    bool  isActive() const;
    float getFreqMHz() const;   // returns actual current value (respects custom override)
    void  setFreqMHz(float mhz);
    void  setSpreadingFactor(uint8_t sf);
    void  setBandwidth(float bw_khz);
    void  setCodingRate(uint8_t cr);
    void  setTxPower(int8_t dbm);
    void  setRxBoost(bool boost);
    void  applyLoraProfile(uint8_t profile);
    void  applyRadioOverrides();  // apply cfg.radioCustom overrides after profile

    // ── Identity ──────────────────────────────────────────────────────
    // Update the node name used in adverts and group messages.
    // Also re-saves the identity to LittleFS so the new name survives reboot.
    void setCallsign(const char* cs);

    // ── Contact management ────────────────────────────────────────────
    // Preload a contact (full 32-byte key) into MeshCore routing table.
    void preloadContact(const uint8_t* pubKey32, const char* name);
    // Manually set routing path for a contact already in the mesh table.
    bool setContactPath(const uint8_t* prefix4, const uint8_t* pathBytes,
                        uint8_t numHops, uint8_t hashSz);
    void  getSelfPubKeyPrefix(uint8_t out[4]) const;
    void  getSelfPubKey(uint8_t out[32]) const;

    // Path introspection / reset
    bool getContactPath(const uint8_t* prefix4, PathInfo& out) const;
    void resetContactPath(const uint8_t* prefix4);
    void resetAllContactPaths();

    // ── Finder (zero-hop node discovery) ─────────────────────────────
    // Sends DISCOVER_REQ (0x80) as a zero-hop broadcast.
    // typeFilter: bitmask of ADV_TYPE_* bits (0x06 = Chat+Repeater).
    // Responses arrive via pollDiscoverResult() for up to 10 seconds.
    bool sendDiscoverReq(uint8_t typeFilter = 0x06);
    // Returns true (once per result) when a discover response is queued.
    bool pollDiscoverResult(DiscoverEntry& out);

    // ── BT companion ──────────────────────────────────────────────────
    // Initialises BLE and wires the companion protocol. Safe to call multiple
    // times (re-enables advertising after stop).
    void startCompanionBLE();
    // Disconnects and stops BLE advertising. No-op if not running.
    void stopCompanionBLE();
    // True if the BLE hardware has been initialised and advertising is active.
    bool isBLERunning() const;
    // True if a mobile app is currently connected via BLE.
    bool isBLEConnected() const;

    bool initialized() const { return _initialized; }

private:
    bool _initialized = false;
};

}  // namespace ops

// Saitama — MeshService.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Bridges the MeshCore library to the rest of Saitama.
// Owns the SX1262 radio, identity, and the BaseChatMesh loop.

#include "MeshService.h"
#include "Fhss.h"
#include "../bt/BTCompanionService.h"
#include "../version.h"
#include "../hardware/Board.h"
#include "../utils/Config.h"
#include "../utils/Contacts.h"
#include "../utils/Repeaters.h"
#include "../utils/SDCard.h"
#include "../utils/Log.h"

#include <Arduino.h>
#include <algorithm>
#include <SPI.h>
#include <LittleFS.h>
#include <esp_random.h>
#include <Preferences.h>

#include <Mesh.h>
#include <Utils.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include "../utils/IdentityBackup.h"
#include "../utils/Regions.h"
#include "../utils/BtPin.h"
#include "../utils/Crypto.h"
#include <helpers/BaseChatMesh.h>
#include <helpers/TransportKeyStore.h>
#include <SHA256.h>
#include <helpers/ESP32Board.h>
#include <helpers/radiolib/CustomSX1262.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <soc/gpio_reg.h>

// Not in BaseChatMesh.h — defined only in simple_repeater example
#define REQ_TYPE_GET_NEIGHBOURS 0x06

#define PUBLIC_GROUP_PSK                "izOH6cXN6mrJ5e26oRXNcg=="
#define SEND_TIMEOUT_BASE_MILLIS        500
#define FLOOD_SEND_TIMEOUT_FACTOR       16.0f
#define DIRECT_SEND_PERHOP_FACTOR        6.0f
#define DIRECT_SEND_PERHOP_EXTRA_MILLIS 250

// ── BT Companion protocol constants ────────────────────────────────────
#define COMP_CMD_APP_START              1
#define COMP_CMD_SEND_TXT_MSG           2
#define COMP_CMD_SEND_CHANNEL_TXT_MSG   3
#define COMP_CMD_GET_CONTACTS           4
#define COMP_CMD_GET_DEVICE_TIME        5
#define COMP_CMD_SET_DEVICE_TIME        6
#define COMP_CMD_SEND_SELF_ADVERT       7
#define COMP_CMD_ADD_UPDATE_CONTACT     9
#define COMP_CMD_SYNC_NEXT_MESSAGE     10
#define COMP_CMD_RESET_PATH            13
#define COMP_CMD_REMOVE_CONTACT        15
#define COMP_CMD_REBOOT                19
#define COMP_CMD_GET_BATT_AND_STORAGE  20
#define COMP_CMD_DEVICE_QUERY          22
#define COMP_CMD_SEND_LOGIN            26
#define COMP_CMD_SEND_STATUS_REQ       27
#define COMP_CMD_HAS_CONNECTION        28
#define COMP_CMD_LOGOUT                29
#define COMP_CMD_GET_CONTACT_BY_KEY    30
#define COMP_CMD_GET_CHANNEL           31
#define COMP_CMD_SET_CHANNEL           32
#define COMP_CMD_GET_ADVERT_PATH       42
#define COMP_CMD_SET_FLOOD_SCOPE_KEY   54

#define COMP_RESP_OK                    0
#define COMP_RESP_ERR                   1
#define COMP_RESP_CONTACTS_START        2
#define COMP_RESP_CONTACT               3
#define COMP_RESP_END_OF_CONTACTS       4
#define COMP_RESP_SELF_INFO             5
#define COMP_RESP_SENT                  6
#define COMP_RESP_CONTACT_MSG_RECV      7
#define COMP_RESP_CHANNEL_MSG_RECV      8
#define COMP_RESP_CURR_TIME             9
#define COMP_RESP_NO_MORE_MESSAGES     10
#define COMP_RESP_BATT_AND_STORAGE     12
#define COMP_RESP_DEVICE_INFO          13
#define COMP_RESP_CONTACT_MSG_RECV_V3  16
#define COMP_RESP_CHANNEL_MSG_RECV_V3  17
#define COMP_RESP_CHANNEL_INFO         18

#define COMP_PUSH_ADVERT               0x80
#define COMP_PUSH_PATH_UPDATED         0x81
#define COMP_PUSH_MSG_WAITING          0x83
#define COMP_PUSH_NEW_ADVERT           0x8A

#define COMP_ERR_UNSUPPORTED            1
#define COMP_ERR_NOT_FOUND              2
#define COMP_ERR_TABLE_FULL             3
#define COMP_ERR_BAD_STATE              4
#define COMP_ERR_ILLEGAL_ARG            6

#define COMP_FIRMWARE_VER              10
#define COMP_OFFLINE_QUEUE_SIZE        16

namespace ops {

// ── Board shim ─────────────────────────────────────────────────────
// Extends ESP32Board but skips Wire.begin() (Board.cpp already owns it)
// and delegates battery millivolts to the Board singleton.
class OMSBoard : public ESP32Board {
public:
    uint16_t getBattMilliVolts() override {
        // Board returns 0-100 percent; map to ~3200-4200 mV LiPo range
        uint8_t pct = Board::instance().batteryPercent();
        return (uint16_t)(3200 + pct * 10);
    }
    const char* getManufacturerName() const override { return "LilyGo T-Deck Plus"; }
    uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

// ── Minimal base64 decoder (avoids re-including base64.hpp which is
//    already compiled into libMeshCore and would cause duplicate symbols) ──
static int8_t _b64val(char c) {
    if (c >= 'A' && c <= 'Z') return (int8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int8_t)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (int8_t)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;  // '=' padding or invalid
}
static int _b64decode(const char* in, uint8_t* out, int outMax) {
    int inLen = (int)strlen(in);
    int j = 0;
    for (int i = 0; i + 3 < inLen && j < outMax; i += 4) {
        int8_t a = _b64val(in[i]);
        int8_t b = _b64val(in[i+1]);
        int8_t c = _b64val(in[i+2]);
        int8_t d = _b64val(in[i+3]);
        if (a < 0 || b < 0) break;
        out[j++] = (uint8_t)((a << 2) | (b >> 4));
        if (c >= 0 && j < outMax) out[j++] = (uint8_t)((b << 4) | (c >> 2));
        if (d >= 0 && j < outMax) out[j++] = (uint8_t)((c << 6) | d);
    }
    return j;
}

// ── Module-level radio stack ───────────────────────────────────────
// Declared in dependency order — C++ initialises statics in declaration
// order within a translation unit, so each object is ready before
// anything that references it is constructed.
static OMSBoard                  oms_board;
static SPIClass                  lora_spi(FSPI);
static CustomSX1262              sx1262(new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, lora_spi));

// Light sleep vs. MeshCore's DIO1 interrupt
// ─────────────────────────────────────────
// The ESP32 can only wake from light sleep on a GPIO *level*, but RadioLib
// attaches MeshCore's "packet ready" ISR to DIO1 as a *rising edge*, and that
// edge happens while the CPU is asleep, so the ISR never runs. MeshCore's flag
// lives in a file-static in RadioLibWrappers.cpp, so it can't be set from
// here. Instead MeshService::lightSleep() flags s_rxMissedInSleep when it wakes
// to DIO1 already high in RX mode, and this wrapper reads that packet exactly
// as RadioLibWrapper::recvRaw() would.
static volatile bool s_rxMissedInSleep = false;
static MeshService::SleepStats s_sleepStats = {};

class OpsSX1262Wrapper : public CustomSX1262Wrapper {
public:
    OpsSX1262Wrapper(CustomSX1262& radio, mesh::MainBoard& board)
        : CustomSX1262Wrapper(radio, board) {}

    int recvRaw(uint8_t* bytes, int sz) override {
        if (s_rxMissedInSleep) {
            s_rxMissedInSleep = false;
            if (isInRecvMode() && digitalRead(P_LORA_DIO_1) == HIGH) {
                s_sleepStats.rxAfterSleep++;
                int len = _radio->getPacketLength();
                if (len > 0) {
                    if (len > sz) len = sz;
                    if (_radio->readData(bytes, len) == RADIOLIB_ERR_NONE) {
                        n_recv++;
                    } else {
                        len = 0;
                        n_recv_errors++;
                    }
                }
                _radio->startReceive();
                return len;
            }
        }
        return CustomSX1262Wrapper::recvRaw(bytes, sz);
    }
};

static OpsSX1262Wrapper          radio_driver(sx1262, oms_board);
static bool                      s_sigGenActive = false;
static ArduinoMillis             ms_clock;
static StdRNG                    std_rng;
static ESP32RTCClock             rtc_clock;
static constexpr int             PKT_POOL_SIZE = 32;
static StaticPoolPacketManager   pkt_mgr(PKT_POOL_SIZE);
static SimpleMeshTables          mesh_tables;

// ── LoRa duty cycle state ─────────────────────────────────────────
static uint32_t s_dcLastPacketMs  = 0;
static uint32_t s_dcLastRecvCount = 0;
static uint32_t s_dcLastSentCount = 0;
static bool     s_dcApplied       = false;
static bool     s_dcSuspended     = false;
static constexpr uint32_t DC_RX_US  = 250000;  // 250 ms RX — 50 % duty cycle
static constexpr uint32_t DC_SLP_US = 250000;  // 250 ms sleep
static constexpr uint32_t DC_ARM_MS = 10000;   // arm after 10 s idle (less aggressive)

// ── OMSMesh ────────────────────────────────────────────────────────
class OMSMesh : public BaseChatMesh {
    static constexpr int RX_QUEUE_SIZE = 24;
    static constexpr int MAX_PEERS     = 60;

    RxMessage _rxBuf[RX_QUEUE_SIZE];
    int       _rxHead  = 0;
    int       _rxTail  = 0;
    int       _rxCount = 0;

    PeerInfo  _peers[MAX_PEERS];
    int       _peerCount  = 0;
    uint32_t  _peerSerial = 0;  // increments on every peer add or update

    ChannelDetails* _channels[10] = {};  // indexed by config channel slot (0=Public)
    bool            _active = true;
    char            _callsign[32] = {};

    // The SD identity backup exists but the storage password doesn't open
    // it. We run on a temporary identity that is never saved anywhere, so the
    // backup survives until it is unlocked or deliberately set aside.
    bool _identityLocked = false;

    // ── Own channel floods: "repeated xN" ────────────────────────────
    // A channel message is flooded once with no ACK. Hearing a repeater
    // re-send it is the only sign it got out, so for 30 s after each send we
    // count copies by packet hash (payload only, so every copy matches),
    // once per distinct repeater (the last hop in the copy's path).
    static constexpr int      OWN_FLOODS   = 6;
    static constexpr uint32_t OWN_FLOOD_MS = 30000;
    struct OwnFlood {
        uint8_t  hash[MAX_HASH_SIZE];
        uint32_t id;          // first 4 hash bytes, never 0 — what the UI holds
        uint32_t sentMs;
        uint8_t  count;
        uint8_t  lastHops[8]; // first byte of each repeater heard
        bool     active;
    };
    OwnFlood _own[OWN_FLOODS] = {};
    int      _ownNext        = 0;
    uint32_t _lastFloodId    = 0;
    struct FloodEvent { uint32_t id; uint8_t count; };
    FloodEvent _floodQ[8]    = {};
    int      _floodQHead = 0, _floodQCount = 0;

    // ── Region discovery ──────────────────────────────────────────────
    // Outstanding ANON_REQ_TYPE_REGIONS requests, matched on the timestamp
    // the repeater echoes as the first 4 bytes of its reply.
    static constexpr uint8_t ANON_REQ_REGIONS = 0x01;   // simple_repeater ANON_REQ_TYPE_REGIONS
    // On-air sizes for airtime estimates: header + dest hash + our pub key +
    // MAC + encrypted 6-byte request; reply = header + hashes + MAC + up to
    // 8 + ~100 bytes of names.
    static constexpr int     REG_REQ_BYTES   = 2 + 1 + 32 + 2 + 16;
    static constexpr int     REG_REPLY_BYTES = 2 + 2 + 2 + 112;
    static constexpr int     REG_MAX = 8;
    // One per repeater asked. Every request sent to it (first try + retry)
    // leaves a tag here, so a late answer to the first one still counts.
    struct RegTarget {
        uint8_t  pubKey[PUB_KEY_SIZE];
        char     name[32];
        uint32_t tags[2];
        uint8_t  tagCount;
        bool     answered;
    };
    RegTarget   _regTargets[REG_MAX] = {};
    int         _regTargetCount = 0;
    // While millis() < this, repeaters answering our zero-hop discover scan
    // are asked for their regions as their replies arrive.
    uint32_t    _regScanUntil = 0;
    RegionReply _regQ[REG_MAX] = {};
    int         _regQHead = 0, _regQCount = 0;

    // ── Outgoing DM retry engine ──────────────────────────────────────
    // Each in-flight DM keeps one timestamp for all its attempts; the attempt
    // number (folded into the payload by MeshCore) changes per retry, so every
    // retry is a distinct packet repeaters will forward, while the receiver
    // recognises it as the same message (see the duplicate guard below).
    // Timeouts are tracked here per slot, not by BaseChatMesh's single timer.
    static constexpr int DM_SLOTS = 4;
    struct PendingDm {
        bool          active;
        bool          lastWasDirect;
        uint8_t       key[4];
        uint8_t       attempt;       // attempt number of the NEXT send
        uint8_t       directLeft;    // direct retries remaining
        uint8_t       floodLeft;     // flood retries remaining
        uint8_t       ackSet;        // bit n = acks[n] valid
        uint32_t      ts;
        uint32_t      id;            // first attempt's ACK — what the UI tracks
        uint32_t      acks[4];       // expected ACK per (attempt & 3)
        unsigned long deadline;
        char          text[MAX_TEXT_LEN + 1];
    };
    PendingDm _dms[DM_SLOTS] = {};
    uint32_t  _txtLastTs       = 0;
    uint32_t  _lastExpectedAck = 0;   // id of the most recent sendDirectMsg()

    // Acked DM ids waiting for the UI (pollAck).
    static constexpr int ACK_Q = 8;
    uint32_t _ackQ[ACK_Q] = {};
    int      _ackQHead = 0, _ackQCount = 0;
    // DM ids whose retries ran out unacknowledged (pollDmFailed).
    uint32_t _failQ[ACK_Q] = {};
    int      _failQHead = 0, _failQCount = 0;

    // ── Incoming DM duplicate guard ───────────────────────────────────
    // Retries (ours or another client's) can deliver the same DM more than
    // once. Same sender + sender timestamp + text = already shown.
    static constexpr int DUP_SLOTS = 16;
    struct SeenDm { uint8_t key[4]; uint32_t ts; uint32_t textHash; };
    SeenDm _seenDms[DUP_SLOTS] = {};
    int    _seenNext = 0;

    TraceResult _traceResult{};
    bool        _hasTraceResult = false;

    // Raw packet capture — filled by logRxRaw() below, drained by ScreenPcap.
    static constexpr int PCAP_QUEUE_SIZE = 12;
    CapturedPacket _pcapBuf[PCAP_QUEUE_SIZE];
    int  _pcapHead = 0, _pcapTail = 0, _pcapCount = 0;
    bool _pcapCaptureEnabled = false;

    // Tracks what binary response format to expect from the next onContactResponse()
    enum PendingResp { PENDING_NONE = 0, PENDING_STATUS, PENDING_NEIGHBOURS };
    PendingResp _pendingResp = PENDING_NONE;

    // Contact response queue — filled by onContactResponse() for repeater replies
    static constexpr int RESP_QUEUE_SIZE = 8;
    char _respBuf[RESP_QUEUE_SIZE][132];
    int  _respHead  = 0;
    int  _respTail  = 0;
    int  _respCount = 0;

    // Login result — set by onContactResponse() when a login reply arrives
    bool _waitingForLoginResp = false;
    bool _loginResultPending  = false;
    bool _loginResultOk       = false;

    // Discover queue — filled by onControlDataRecv() for DISCOVER_RESP packets
    static constexpr int DISCOVER_QUEUE_SIZE = 20;
    DiscoverEntry _discoverBuf[DISCOVER_QUEUE_SIZE];
    int      _discoverHead      = 0;
    int      _discoverTail      = 0;
    int      _discoverCount     = 0;
    uint32_t _discoverTag       = 0;   // tag of the active scan (0 = no active scan)
    uint32_t _discoverDeadlineMs = 0;  // millis() deadline for accepting responses
    uint32_t _lastDiscoverRespTag = 0; // last tag we responded to (rate limit)

    // ── BT Companion state ────────────────────────────────────────────
    struct CompFrame {
        uint8_t buf[MAX_FRAME_SIZE];
        uint8_t len;
        bool isChannelMsg() const {
            return buf[0] == COMP_RESP_CHANNEL_MSG_RECV ||
                   buf[0] == COMP_RESP_CHANNEL_MSG_RECV_V3;
        }
    };
    enum CompIterSrc : uint8_t { ITER_NONE = 0, ITER_CONTACTS, ITER_REPEATERS };

    BaseSerialInterface* _btSerial     = nullptr;
    uint8_t  _cmdFrame[MAX_FRAME_SIZE + 1];
    uint8_t  _outFrame[MAX_FRAME_SIZE + 1];
    CompFrame _compQueue[COMP_OFFLINE_QUEUE_SIZE];
    int       _compQueueLen  = 0;
    uint8_t   _appTargetVer  = 0;
    CompIterSrc _contactIterSrc    = ITER_NONE;
    int         _contactIterIdx    = 0;
    uint32_t    _contactIterLastmod = 0;

    void _enqueueResp(const char* text)
    {
        if (_respCount >= RESP_QUEUE_SIZE) return;
        strncpy(_respBuf[_respTail], text, 131);
        _respBuf[_respTail][131] = '\0';
        _respTail = (_respTail + 1) % RESP_QUEUE_SIZE;
        _respCount++;
    }

    static void _fmtPathStr(char* out, size_t outSize, const mesh::Packet* pkt) {
        out[0] = '\0';
        uint8_t hops   = pkt->getPathHashCount();
        uint8_t hashSz = pkt->getPathHashSize();
        if (hops == 0 || hashSz == 0) return;
        size_t pos = 0;
        for (uint8_t i = 0; i < hops; i++) {
            if (i > 0) { if (pos + 1 >= outSize) break; out[pos++] = '>'; }
            for (uint8_t b = 0; b < hashSz; b++) {
                if (pos + 2 >= outSize) goto done;
                uint8_t byte = pkt->path[i * hashSz + b];
                out[pos++] = "0123456789ABCDEF"[byte >> 4];
                out[pos++] = "0123456789ABCDEF"[byte & 0xF];
            }
        }
        done: out[pos] = '\0';
    }

    void _enqueueRx(RxMessage& msg) {
        if (_rxCount >= RX_QUEUE_SIZE) return;  // drop when full
        _rxBuf[_rxTail] = msg;
        _rxTail = (_rxTail + 1) % RX_QUEUE_SIZE;
        _rxCount++;
    }

    void _autoAddPeer(const PeerInfo& p) {
        const auto& cfg = ops::config::get();
        if (p.type == 2) {
            // Repeater
            if (!cfg.autoAddRepeater) return;
            if (ops::repeaters::findByKey(p.pubKeyPrefix)) return;
            ops::Repeater r{};
            strncpy(r.name, p.name, sizeof(r.name) - 1);
            memcpy(r.pubKeyPrefix, p.pubKeyPrefix, 4);
            memcpy(r.pubKey,       p.pubKey,       32);
            r.lastSeen = p.lastSeen;
            r.lastRssi = p.lastRssi;
            r.lat      = p.lat;
            r.lon      = p.lon;
            ops::repeaters::add(r);
            OPS_LOG("Mesh", "Auto-added repeater: %s", p.name);
        } else {
            // Client / Companion (type 1) or Room (type 3)
            if (!cfg.autoAddClient) return;
            if (ops::contacts::findByKey(p.pubKeyPrefix)) return;
            ops::Contact c{};
            strncpy(c.name, p.name, sizeof(c.name) - 1);
            memcpy(c.pubKeyPrefix, p.pubKeyPrefix, 4);
            memcpy(c.pubKey,       p.pubKey,       32);
            c.lastSeen  = p.lastSeen;
            c.lastRssi  = p.lastRssi;
            c.hasUnread = false;
            c.lat       = p.lat;
            c.lon       = p.lon;
            ops::contacts::add(c);
            OPS_LOG("Mesh", "Auto-added contact: %s", p.name);
        }
    }

    // hops: how far the packet that brought it travelled (0xFF = not known,
    // e.g. a direct-routed packet — keeps the previous value).
    void _upsertPeer(const ContactInfo& contact, uint8_t hops = 0xFF) {
        for (int i = 0; i < _peerCount; i++) {
            if (memcmp(_peers[i].pubKeyPrefix, contact.id.pub_key, 4) == 0) {
                if (hops != 0xFF) _peers[i].hops = hops;
                strncpy(_peers[i].name, contact.name, 31);
                memcpy(_peers[i].pubKey, contact.id.pub_key, 32);
                _peers[i].lastSeen = getRTCClock()->getCurrentTime();
                _peers[i].lastRssi = radio_driver.getLastRSSI();
                _peers[i].lat      = contact.gps_lat;
                _peers[i].lon      = contact.gps_lon;
                _peerSerial++;
                _updateStoredLive(contact, hops);
                return;
            }
        }
        // New peer. When the list is full, the one heard longest ago makes
        // room, so a busy session keeps showing newly heard stations.
        int slot = _peerCount;
        if (_peerCount < MAX_PEERS) {
            _peerCount++;
        } else {
            slot = 0;
            for (int i = 1; i < _peerCount; i++)
                if (_peers[i].lastSeen < _peers[slot].lastSeen) slot = i;
            OPS_LOG("Mesh", "Heard list full - replacing %s", _peers[slot].name);
        }
        {
            PeerInfo& p = _peers[slot];
            memset(&p, 0, sizeof(p));
            strncpy(p.name, contact.name, 31);
            p.name[31] = '\0';
            memcpy(p.pubKeyPrefix, contact.id.pub_key, 4);
            memcpy(p.pubKey,       contact.id.pub_key, 32);
            p.type     = contact.type;
            p.lastSeen = getRTCClock()->getCurrentTime();
            p.lastRssi = radio_driver.getLastRSSI();
            p.lat      = contact.gps_lat;
            p.lon      = contact.gps_lon;
            p.hops     = hops;
            _peerSerial++;
            _autoAddPeer(p);
        }
        // First time heard this session: refresh the saved entry too (it was
        // only updated on later hearings, so a reboot-old lastSeen stuck).
        _updateStoredLive(contact, hops);
    }

    // Live data (name, time, RSSI, hops, position) into the saved contact or
    // repeater, without a save — it persists on the next natural save().
    void _updateStoredLive(const ContactInfo& contact, uint8_t hops) {
        int idx = -1;
        uint32_t now = (uint32_t)getRTCClock()->getCurrentTime();
        float    rssi = radio_driver.getLastRSSI();
        if (contact.type == 2) {
            if (ops::repeaters::findByKey(contact.id.pub_key, &idx)) {
                ops::repeaters::setLiveData(idx, contact.name, now, rssi);
                ops::repeaters::setPosition(idx, contact.gps_lat, contact.gps_lon);
                ops::repeaters::setFullKey(idx, contact.id.pub_key);
            }
        } else {
            if (ops::contacts::findByKey(contact.id.pub_key, &idx)) {
                ops::contacts::setLiveData(idx, contact.name, now, rssi, hops);
                ops::contacts::setPosition(idx, contact.gps_lat, contact.gps_lon);
                ops::contacts::setFullKey(idx, contact.id.pub_key);
            }
        }
    }

    // ── BT Companion helpers ───────────────────────────────────────────

    void _compWriteOK()
    {
        uint8_t b = COMP_RESP_OK;
        _btSerial->writeFrame(&b, 1);
    }

    void _compWriteErr(uint8_t code)
    {
        uint8_t b[2] = { COMP_RESP_ERR, code };
        _btSerial->writeFrame(b, 2);
    }

    void _compWriteContactFrame(uint8_t code, const ContactInfo& ci)
    {
        if (!_btSerial) return;
        int i = 0;
        _outFrame[i++] = code;
        memcpy(&_outFrame[i], ci.id.pub_key, PUB_KEY_SIZE); i += PUB_KEY_SIZE;
        _outFrame[i++] = ci.type;
        _outFrame[i++] = ci.flags;
        _outFrame[i++] = ci.out_path_len;
        memcpy(&_outFrame[i], ci.out_path, MAX_PATH_SIZE); i += MAX_PATH_SIZE;
        strncpy((char*)&_outFrame[i], ci.name, 31); _outFrame[i + 31] = '\0'; i += 32;
        memcpy(&_outFrame[i], &ci.last_advert_timestamp, 4); i += 4;
        memcpy(&_outFrame[i], &ci.gps_lat, 4); i += 4;
        memcpy(&_outFrame[i], &ci.gps_lon, 4); i += 4;
        memcpy(&_outFrame[i], &ci.lastmod, 4); i += 4;
        _btSerial->writeFrame(_outFrame, i);
    }

    void _compAddToQueue(const uint8_t frame[], uint8_t frameLen)
    {
        if (frameLen > MAX_FRAME_SIZE) frameLen = MAX_FRAME_SIZE;
        if (_compQueueLen >= COMP_OFFLINE_QUEUE_SIZE) {
            for (int pos = 0; pos < _compQueueLen; pos++) {
                if (_compQueue[pos].isChannelMsg()) {
                    for (int k = pos; k < _compQueueLen - 1; k++)
                        _compQueue[k] = _compQueue[k + 1];
                    _compQueue[_compQueueLen - 1].len = frameLen;
                    memcpy(_compQueue[_compQueueLen - 1].buf, frame, frameLen);
                    return;
                }
            }
            return; // all DMs — drop new item
        }
        _compQueue[_compQueueLen].len = frameLen;
        memcpy(_compQueue[_compQueueLen].buf, frame, frameLen);
        _compQueueLen++;
    }

    int _compGetFromQueue(uint8_t out[])
    {
        if (_compQueueLen == 0) return 0;
        int len = _compQueue[0].len;
        memcpy(out, _compQueue[0].buf, len);
        _compQueueLen--;
        for (int i = 0; i < _compQueueLen; i++)
            _compQueue[i] = _compQueue[i + 1];
        return len;
    }

    void _compQueueContactMsg(const ContactInfo& from, mesh::Packet* pkt,
                              uint32_t sender_timestamp, const char* text)
    {
        if (!_btSerial) return;
        int i = 0;
        if (_appTargetVer >= 3) {
            _outFrame[i++] = COMP_RESP_CONTACT_MSG_RECV_V3;
            _outFrame[i++] = (int8_t)(pkt->getSNR() * 4);
            _outFrame[i++] = 0; _outFrame[i++] = 0;
        } else {
            _outFrame[i++] = COMP_RESP_CONTACT_MSG_RECV;
        }
        memcpy(&_outFrame[i], from.id.pub_key, 6); i += 6;
        _outFrame[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
        _outFrame[i++] = 0; // TXT_TYPE_PLAIN
        memcpy(&_outFrame[i], &sender_timestamp, 4); i += 4;
        int tlen = (int)strlen(text);
        if (i + tlen > MAX_FRAME_SIZE) tlen = MAX_FRAME_SIZE - i;
        memcpy(&_outFrame[i], text, tlen); i += tlen;
        _compAddToQueue(_outFrame, (uint8_t)i);
        if (_btSerial->isConnected()) {
            uint8_t tick = COMP_PUSH_MSG_WAITING;
            _btSerial->writeFrame(&tick, 1);
        }
    }

    void _compQueueChannelMsg(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                              uint32_t timestamp, const char* text)
    {
        if (!_btSerial) return;
        int i = 0;
        if (_appTargetVer >= 3) {
            _outFrame[i++] = COMP_RESP_CHANNEL_MSG_RECV_V3;
            _outFrame[i++] = (int8_t)(pkt->getSNR() * 4);
            _outFrame[i++] = 0; _outFrame[i++] = 0;
        } else {
            _outFrame[i++] = COMP_RESP_CHANNEL_MSG_RECV;
        }
        int chIdx = findChannelIdx(channel);
        _outFrame[i++] = (uint8_t)(chIdx >= 0 ? chIdx : 0);
        _outFrame[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
        _outFrame[i++] = 0; // TXT_TYPE_PLAIN
        memcpy(&_outFrame[i], &timestamp, 4); i += 4;
        int tlen = (int)strlen(text);
        if (i + tlen > MAX_FRAME_SIZE) tlen = MAX_FRAME_SIZE - i;
        memcpy(&_outFrame[i], text, tlen); i += tlen;
        _compAddToQueue(_outFrame, (uint8_t)i);
        if (_btSerial->isConnected()) {
            uint8_t tick = COMP_PUSH_MSG_WAITING;
            _btSerial->writeFrame(&tick, 1);
        }
    }

    void _compPumpContactIter()
    {
        if (!_btSerial || _contactIterSrc == ITER_NONE) return;
        if (_btSerial->isWriteBusy()) return;

        if (_contactIterSrc == ITER_CONTACTS) {
            if (_contactIterIdx < ops::contacts::count()) {
                ops::Contact c;
                if (ops::contacts::get(_contactIterIdx, c)) {
                    bool hasKey = false;
                    for (int b = 0; b < 32; b++) if (c.pubKey[b]) { hasKey = true; break; }
                    if (hasKey) {
                        ContactInfo ci{};
                        ci.id = mesh::Identity(c.pubKey);
                        strncpy(ci.name, c.name, 31);
                        ci.type = ADV_TYPE_CHAT;
                        ci.last_advert_timestamp = c.lastSeen;
                        ci.lastmod = c.lastSeen;
                        ContactInfo* mi = lookupContactByPubKey(c.pubKeyPrefix, 4);
                        if (mi) {
                            ci.out_path_len = mi->out_path_len;
                            memcpy(ci.out_path, mi->out_path, MAX_PATH_SIZE);
                        } else {
                            ci.out_path_len = OUT_PATH_UNKNOWN;
                        }
                        if (c.lastSeen > _contactIterLastmod) _contactIterLastmod = c.lastSeen;
                        _compWriteContactFrame(COMP_RESP_CONTACT, ci);
                        OPS_LOG("BT", "Sync contact[%d] '%s'", _contactIterIdx, c.name);
                    }
                }
                _contactIterIdx++;
                return;
            }
            _contactIterSrc = ITER_REPEATERS;
            _contactIterIdx = 0;
            return;
        }

        if (_contactIterSrc == ITER_REPEATERS) {
            if (_contactIterIdx < ops::repeaters::count()) {
                ops::Repeater r;
                if (ops::repeaters::get(_contactIterIdx, r)) {
                    bool hasKey = false;
                    for (int b = 0; b < 32; b++) if (r.pubKey[b]) { hasKey = true; break; }
                    if (hasKey) {
                        ContactInfo ci{};
                        ci.id = mesh::Identity(r.pubKey);
                        strncpy(ci.name, r.name, 31);
                        ci.type = ADV_TYPE_REPEATER;
                        ci.last_advert_timestamp = r.lastSeen;
                        ci.lastmod = r.lastSeen;
                        ContactInfo* mi = lookupContactByPubKey(r.pubKeyPrefix, 4);
                        if (mi) {
                            ci.out_path_len = mi->out_path_len;
                            memcpy(ci.out_path, mi->out_path, MAX_PATH_SIZE);
                        } else {
                            ci.out_path_len = OUT_PATH_UNKNOWN;
                        }
                        if (r.lastSeen > _contactIterLastmod) _contactIterLastmod = r.lastSeen;
                        _compWriteContactFrame(COMP_RESP_CONTACT, ci);
                        OPS_LOG("BT", "Sync repeater[%d] '%s'", _contactIterIdx, r.name);
                    }
                }
                _contactIterIdx++;
                return;
            }
            // END_OF_CONTACTS: 5 bytes (code + 4-byte most-recent lastmod) matching reference.
            uint8_t end[5];
            end[0] = COMP_RESP_END_OF_CONTACTS;
            memcpy(&end[1], &_contactIterLastmod, 4);
            _btSerial->writeFrame(end, 5);
            OPS_LOG("BT", "Sync END_OF_CONTACTS lastmod=%lu", (unsigned long)_contactIterLastmod);
            _contactIterSrc = ITER_NONE;
            _contactIterIdx = 0;
        }
    }

    void _handleCmdFrame(size_t len)
    {
        if (!_btSerial || len == 0) return;
        // Null-terminate so text extracted from the frame is safe to use as C-string
        _cmdFrame[len < MAX_FRAME_SIZE ? len : MAX_FRAME_SIZE] = 0;
        uint8_t cmd = _cmdFrame[0];

        if (cmd == COMP_CMD_DEVICE_QUERY && len >= 2) {
            _appTargetVer = _cmdFrame[1];
            int i = 0;
            _outFrame[i++] = COMP_RESP_DEVICE_INFO;
            _outFrame[i++] = COMP_FIRMWARE_VER;
            _outFrame[i++] = (uint8_t)(MAX_CONTACTS / 2);
            _outFrame[i++] = (uint8_t)MAX_GROUP_CHANNELS;
            uint32_t pin = 0;
            memcpy(&_outFrame[i], &pin, 4); i += 4;
            memset(&_outFrame[i], 0, 12); // build date
            strncpy((char*)&_outFrame[i], "2026-05-01", 12); i += 12;
            memset(&_outFrame[i], 0, 40); // manufacturer
            strncpy((char*)&_outFrame[i], "LilyGo T-Deck Plus", 39); i += 40;
            memset(&_outFrame[i], 0, 20); // firmware version
            strncpy((char*)&_outFrame[i], OPS_VERSION_STRING, 19); i += 20;
            _outFrame[i++] = 0; // client_repeat
            _outFrame[i++] = 0; // path_hash_mode
            _btSerial->writeFrame(_outFrame, i);

        } else if (cmd == COMP_CMD_APP_START && len >= 2) {
            _contactIterSrc = ITER_NONE;
            int i = 0;
            _outFrame[i++] = COMP_RESP_SELF_INFO;
            _outFrame[i++] = ADV_TYPE_CHAT;
            _outFrame[i++] = 22; // tx_power_dbm
            _outFrame[i++] = 22; // max_tx_power
            memcpy(&_outFrame[i], self_id.pub_key, PUB_KEY_SIZE); i += PUB_KEY_SIZE;
            const auto& cfg = ops::config::get();
            int32_t lat = cfg.locationSharing ? (int32_t)(cfg.manualLat * 1000000.0f) : 0;
            int32_t lon = cfg.locationSharing ? (int32_t)(cfg.manualLon * 1000000.0f) : 0;
            memcpy(&_outFrame[i], &lat, 4); i += 4;
            memcpy(&_outFrame[i], &lon, 4); i += 4;
            _outFrame[i++] = 0; // multi_acks
            _outFrame[i++] = 0; // advert_loc_policy
            _outFrame[i++] = 0; // telemetry mode bits
            _outFrame[i++] = 0; // manual_add_contacts
            float freqMHz = (cfg.radioCustom && cfg.freqMHz > 0.0f) ? cfg.freqMHz : 869.618f;
            uint32_t freq = (uint32_t)(freqMHz * 1000.0f);
            memcpy(&_outFrame[i], &freq, 4); i += 4;
            uint32_t bw = 62500; // 62.5 kHz in Hz
            memcpy(&_outFrame[i], &bw, 4); i += 4;
            _outFrame[i++] = 8; // sf
            _outFrame[i++] = 8; // cr
            const char* name = _callsign[0] ? _callsign : "OMS-NODE";
            int nlen = (int)strlen(name);
            if (i + nlen > MAX_FRAME_SIZE) nlen = MAX_FRAME_SIZE - i;
            memcpy(&_outFrame[i], name, nlen); i += nlen;
            _btSerial->writeFrame(_outFrame, i);

        } else if (cmd == COMP_CMD_GET_CONTACTS) {
            if (_contactIterSrc != ITER_NONE) {
                _compWriteErr(COMP_ERR_BAD_STATE);
            } else {
                // Only count entries that will actually be sent (non-zero pubKey).
                // Phone waits for exactly this many contact frames before expecting END_OF_CONTACTS.
                uint32_t total = 0;
                for (int i = 0; i < ops::contacts::count(); i++) {
                    ops::Contact c;
                    if (ops::contacts::get(i, c)) {
                        for (int b = 0; b < 32; b++) { if (c.pubKey[b]) { total++; break; } }
                    }
                }
                for (int i = 0; i < ops::repeaters::count(); i++) {
                    ops::Repeater r;
                    if (ops::repeaters::get(i, r)) {
                        for (int b = 0; b < 32; b++) { if (r.pubKey[b]) { total++; break; } }
                    }
                }
                OPS_LOG("BT", "GET_CONTACTS: %lu sendable", (unsigned long)total);
                uint8_t reply[5];
                reply[0] = COMP_RESP_CONTACTS_START;
                memcpy(&reply[1], &total, 4);
                _btSerial->writeFrame(reply, 5);
                _contactIterSrc    = ITER_CONTACTS;
                _contactIterIdx    = 0;
                _contactIterLastmod = 0;
            }

        } else if (cmd == COMP_CMD_SYNC_NEXT_MESSAGE) {
            int out_len = _compGetFromQueue(_outFrame);
            if (out_len > 0) {
                _btSerial->writeFrame(_outFrame, out_len);
            } else {
                uint8_t noMore = COMP_RESP_NO_MORE_MESSAGES;
                _btSerial->writeFrame(&noMore, 1);
            }

        } else if (cmd == COMP_CMD_SEND_TXT_MSG && len >= 14) {
            int i = 1;
            i++; // txt_type
            i++; // attempt
            uint32_t ts;
            memcpy(&ts, &_cmdFrame[i], 4); i += 4;
            ContactInfo* ci = lookupContactByPubKey(&_cmdFrame[i], 6); i += 6;
            if (!ci) {
                _compWriteErr(COMP_ERR_NOT_FOUND);
            } else {
                char* text = (char*)&_cmdFrame[i];
                int tlen = (int)len - i;
                if (tlen > 0 && i + tlen < MAX_FRAME_SIZE) text[tlen] = 0;
                uint32_t expected_ack = 0, est_timeout = 0;
                int result = sendMessage(*ci, ts, 0, text, expected_ack, est_timeout);
                if (result == MSG_SEND_FAILED) {
                    _compWriteErr(COMP_ERR_TABLE_FULL);
                } else {
                    int j = 0;
                    _outFrame[j++] = COMP_RESP_SENT;
                    _outFrame[j++] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
                    memcpy(&_outFrame[j], &expected_ack, 4); j += 4;
                    memcpy(&_outFrame[j], &est_timeout, 4); j += 4;
                    _btSerial->writeFrame(_outFrame, j);
                }
            }

        } else if (cmd == COMP_CMD_SEND_CHANNEL_TXT_MSG && len >= 7) {
            int i = 1;
            i++; // txt_type
            uint8_t ch_idx = _cmdFrame[i++];
            uint32_t ts;
            memcpy(&ts, &_cmdFrame[i], 4); i += 4;
            const char* text = (const char*)&_cmdFrame[i];
            int tlen = (int)len - i;
            ChannelDetails ch;
            if (getChannel(ch_idx, ch) && sendGroupMessage(ts, ch.channel, _callsign, text, tlen)) {
                _compWriteOK();
            } else {
                _compWriteErr(COMP_ERR_NOT_FOUND);
            }

        } else if (cmd == COMP_CMD_GET_DEVICE_TIME) {
            uint8_t reply[5];
            reply[0] = COMP_RESP_CURR_TIME;
            uint32_t now = getRTCClock()->getCurrentTime();
            memcpy(&reply[1], &now, 4);
            _btSerial->writeFrame(reply, 5);

        } else if (cmd == COMP_CMD_SET_DEVICE_TIME && len >= 5) {
            uint32_t secs;
            memcpy(&secs, &_cmdFrame[1], 4);
            uint32_t curr = getRTCClock()->getCurrentTime();
            if (secs >= curr) {
                getRTCClock()->setCurrentTime(secs);
                _compWriteOK();
            } else {
                _compWriteErr(COMP_ERR_ILLEGAL_ARG);
            }

        } else if (cmd == COMP_CMD_SEND_SELF_ADVERT) {
            mesh::Packet* pkt = createSelfAdvert(_callsign);
            if (pkt) { _floodInScope(pkt, config::get().scopeTag, 0); _compWriteOK(); }
            else      { _compWriteErr(COMP_ERR_TABLE_FULL); }

        } else if (cmd == COMP_CMD_RESET_PATH && len >= 1 + PUB_KEY_SIZE) {
            ContactInfo* ci = lookupContactByPubKey(&_cmdFrame[1], PUB_KEY_SIZE);
            if (ci) { ci->out_path_len = OUT_PATH_UNKNOWN; _compWriteOK(); }
            else    { _compWriteErr(COMP_ERR_NOT_FOUND); }

        } else if (cmd == COMP_CMD_ADD_UPDATE_CONTACT && len >= 1 + PUB_KEY_SIZE + 3) {
            uint8_t* pub_key = &_cmdFrame[1];
            ContactInfo* existing = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
            if (existing) {
                // Update name from frame (offset: 1 + 32 + type + flags + path_len + MAX_PATH_SIZE)
                int off = 1 + PUB_KEY_SIZE + 3 + MAX_PATH_SIZE;
                if ((size_t)off + 1 < len) {
                    strncpy(existing->name, (char*)&_cmdFrame[off], 31);
                    existing->name[31] = '\0';
                }
                _compWriteOK();
            } else {
                ContactInfo ci{};
                memcpy(ci.id.pub_key, pub_key, PUB_KEY_SIZE);
                int off = 1 + PUB_KEY_SIZE;
                ci.type = _cmdFrame[off++];
                ci.flags = _cmdFrame[off++];
                ci.out_path_len = _cmdFrame[off++];
                if (off + MAX_PATH_SIZE < (int)len)
                    memcpy(ci.out_path, &_cmdFrame[off], MAX_PATH_SIZE);
                off += MAX_PATH_SIZE;
                if (off + 1 < (int)len) {
                    strncpy(ci.name, (char*)&_cmdFrame[off], 31);
                    ci.name[31] = '\0';
                }
                if (addContact(ci)) _compWriteOK();
                else                _compWriteErr(COMP_ERR_TABLE_FULL);
            }

        } else if (cmd == COMP_CMD_REMOVE_CONTACT && len >= 1 + PUB_KEY_SIZE) {
            ContactInfo* ci = lookupContactByPubKey(&_cmdFrame[1], PUB_KEY_SIZE);
            if (ci && removeContact(*ci)) _compWriteOK();
            else                          _compWriteErr(COMP_ERR_NOT_FOUND);

        } else if (cmd == COMP_CMD_REBOOT && len >= 7 &&
                   memcmp(&_cmdFrame[1], "reboot", 6) == 0) {
            _compWriteOK();
            delay(100);
            ESP.restart();

        } else if (cmd == COMP_CMD_GET_BATT_AND_STORAGE) {
            int i = 0;
            _outFrame[i++] = COMP_RESP_BATT_AND_STORAGE;
            uint16_t batt_mv = (uint16_t)(3200 + (uint16_t)Board::instance().batteryPercent() * 10);
            memcpy(&_outFrame[i], &batt_mv, 2); i += 2;
            uint32_t used = 0, total = 0;
            memcpy(&_outFrame[i], &used,  4); i += 4;
            memcpy(&_outFrame[i], &total, 4); i += 4;
            _outFrame[i++] = 0; // storage_pct
            _btSerial->writeFrame(_outFrame, i);

        } else if (cmd == COMP_CMD_HAS_CONNECTION || cmd == COMP_CMD_LOGOUT) {
            _compWriteOK();

        } else if (cmd == COMP_CMD_GET_CHANNEL && len >= 2) {
            uint8_t chIdx = _cmdFrame[1];
            ChannelDetails ch;
            if (getChannel(chIdx, ch)) {
                int i = 0;
                _outFrame[i++] = COMP_RESP_CHANNEL_INFO;
                _outFrame[i++] = chIdx;
                memset(&_outFrame[i], 0, 32);
                strncpy((char*)&_outFrame[i], ch.name, 31); i += 32;
                // companion protocol expects 16-byte (128-bit) PSK
                memcpy(&_outFrame[i], ch.channel.secret, 16); i += 16;
                _btSerial->writeFrame(_outFrame, i);
            } else {
                _compWriteErr(COMP_ERR_NOT_FOUND);
            }

        } else if (cmd == COMP_CMD_SET_CHANNEL && len >= 2 + 32 + 16) {
            uint8_t chIdx = _cmdFrame[1];
            ChannelDetails ch;
            if (getChannel(chIdx, ch)) {
                memset(ch.name, 0, sizeof(ch.name));
                strncpy(ch.name, (char*)&_cmdFrame[2], 31);
                ch.name[31] = '\0';
                memset(ch.channel.secret, 0, sizeof(ch.channel.secret));
                memcpy(ch.channel.secret, &_cmdFrame[2 + 32], 16);
                if (setChannel(chIdx, ch)) _compWriteOK();
                else                        _compWriteErr(COMP_ERR_NOT_FOUND);
            } else {
                _compWriteErr(COMP_ERR_NOT_FOUND);
            }

        } else if (cmd == COMP_CMD_SEND_LOGIN && len >= 1 + PUB_KEY_SIZE) {
            ContactInfo* ci = lookupContactByPubKey(&_cmdFrame[1], PUB_KEY_SIZE);
            if (!ci) {
                _compWriteErr(COMP_ERR_NOT_FOUND);
            } else {
                const char* pass = (const char*)&_cmdFrame[1 + PUB_KEY_SIZE];
                int plen = (int)len - (1 + PUB_KEY_SIZE);
                char passBuf[64] = {};
                if (plen > 0 && plen < (int)sizeof(passBuf))
                    memcpy(passBuf, pass, plen);
                uint32_t timeout = 0;
                int ret = sendLogin(*ci, passBuf, timeout);
                if (ret != MSG_SEND_FAILED) _compWriteOK();
                else                         _compWriteErr(COMP_ERR_TABLE_FULL);
            }

        } else if (cmd == COMP_CMD_SEND_STATUS_REQ && len >= 1 + PUB_KEY_SIZE) {
            ContactInfo* ci = lookupContactByPubKey(&_cmdFrame[1], PUB_KEY_SIZE);
            if (!ci) {
                _compWriteErr(COMP_ERR_NOT_FOUND);
            } else {
                uint32_t tag = 0, timeout = 0;
                int ret = sendRequest(*ci, REQ_TYPE_GET_STATUS, tag, timeout);
                if (ret != MSG_SEND_FAILED) _compWriteOK();
                else                         _compWriteErr(COMP_ERR_TABLE_FULL);
            }

        } else if (cmd == COMP_CMD_GET_CONTACT_BY_KEY && len >= 1 + PUB_KEY_SIZE) {
            ContactInfo* ci = lookupContactByPubKey(&_cmdFrame[1], PUB_KEY_SIZE);
            if (ci) {
                _compWriteContactFrame(COMP_RESP_CONTACT, *ci);
            } else {
                _compWriteErr(COMP_ERR_NOT_FOUND);
            }

        } else if (cmd == COMP_CMD_GET_ADVERT_PATH) {
            // We don't maintain an advert-path table — tell the phone it's unknown.
            _compWriteErr(COMP_ERR_NOT_FOUND);

        } else if (cmd == COMP_CMD_SET_FLOOD_SCOPE_KEY) {
            // Accept but ignore flood-scope key — we don't implement scoped flooding yet.
            _compWriteOK();

        } else {
            OPS_LOG("BT", "Unhandled cmd 0x%02X len=%d", cmd, (int)len);
            _compWriteErr(COMP_ERR_UNSUPPORTED);
        }
    }

    // ── Mesh virtuals ─────────────────────────────────────────────

    // Must return true for this node to relay flood packets and forward TRACE hops.
    bool allowPacketForward(const mesh::Packet* /*packet*/) override {
        return _active && ops::config::get().autoForward;
    }

    void _noteOwnFlood(const mesh::Packet* pkt) {
        OwnFlood& o = _own[_ownNext];
        _ownNext = (_ownNext + 1) % OWN_FLOODS;
        memset(&o, 0, sizeof(o));
        pkt->calculatePacketHash(o.hash);
        memcpy(&o.id, o.hash, 4);
        if (o.id == 0) o.id = 1;
        o.sentMs = millis();
        o.active = true;
        _lastFloodId = o.id;
    }

    void _pushFloodEvent(uint32_t id, uint8_t count) {
        if (_floodQCount == 8) { _floodQHead = (_floodQHead + 1) % 8; _floodQCount--; }
        _floodQ[(_floodQHead + _floodQCount) % 8] = { id, count };
        _floodQCount++;
    }

    // A frame heard: is it a repeater re-sending one of our channel floods?
    void _checkOwnFloodEcho(const uint8_t raw[], int len) {
        bool any = false;
        for (const OwnFlood& o : _own) if (o.active) { any = true; break; }
        if (!any) return;
        static mesh::Packet pkt;   // static: ~250 bytes, kept off the stack
        if (!pkt.readFrom(raw, (uint8_t)len)) return;
        if (pkt.getPayloadType() != PAYLOAD_TYPE_GRP_TXT || !pkt.isRouteFlood()) return;
        uint8_t hops = pkt.getPathHashCount();
        if (hops == 0) return;
        uint8_t h[MAX_HASH_SIZE];
        pkt.calculatePacketHash(h);
        for (OwnFlood& o : _own) {
            if (!o.active || memcmp(o.hash, h, MAX_HASH_SIZE) != 0) continue;
            uint8_t hashSz = (pkt.path_len >> 6) + 1;
            uint8_t last   = pkt.path[(hops - 1) * hashSz];
            for (int k = 0; k < o.count && k < 8; k++) if (o.lastHops[k] == last) return;
            if (o.count < 8) o.lastHops[o.count] = last;
            if (o.count < 255) o.count++;
            _pushFloodEvent(o.id, o.count);
            OPS_LOG("Mesh", "Channel msg %08X repeated x%u (via %02X)", o.id, o.count, last);
            return;
        }
    }

public:
    // Ends each flood's listening window; one that no repeater re-sent is
    // reported with count 0.
    void serviceOwnFloods() {
        uint32_t now = millis();
        for (OwnFlood& o : _own) {
            if (!o.active || now - o.sentMs < OWN_FLOOD_MS) continue;
            o.active = false;
            if (o.count == 0) {
                _pushFloodEvent(o.id, 0);
                OPS_LOG("Mesh", "Channel msg %08X: no repeater heard re-sending it", o.id);
            }
        }
    }
    uint32_t lastChannelFloodId() const { return _lastFloodId; }
    bool pollFloodRepeat(uint32_t& id, uint8_t& count) {
        if (_floodQCount == 0) return false;
        id    = _floodQ[_floodQHead].id;
        count = _floodQ[_floodQHead].count;
        _floodQHead = (_floodQHead + 1) % 8;
        _floodQCount--;
        return true;
    }
private:

    // Fires for every received frame, before parsing — raw bytes as they came
    // off the radio. Also buffered for ScreenPcap while a capture runs.
    void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override {
        if (len > 0) _checkOwnFloodEcho(raw, len);
        if (!_pcapCaptureEnabled || len <= 0) return;
        if (_pcapCount >= PCAP_QUEUE_SIZE) return;  // drop — screen isn't draining fast enough
        CapturedPacket& p = _pcapBuf[_pcapTail];
        p.timestamp = (uint32_t)getRTCClock()->getCurrentTime();
        p.usec      = (millis() % 1000) * 1000;
        p.rssi      = rssi;
        p.snr       = snr;
        p.len       = (uint8_t)(len > 255 ? 255 : len);
        memcpy(p.data, raw, p.len);
        _pcapTail = (_pcapTail + 1) % PCAP_QUEUE_SIZE;
        _pcapCount++;
    }

    // ── BaseChatMesh pure virtuals ────────────────────────────────

    // The advert accumulated path (path[]) runs from the first relay to the last
    // (closest to us). To send a DIRECT packet back, we traverse in reverse.
    // Encodes result directly into ci.out_path / ci.out_path_len.
    static void _applyReverseOutPath(ContactInfo& ci, const uint8_t* path, uint8_t path_len) {
        uint8_t hash_sz   = (path_len >> 6) + 1;
        uint8_t hop_count = path_len & 63;
        uint8_t byte_count = hop_count * hash_sz;
        if (byte_count > MAX_PATH_SIZE) return;
        for (uint8_t i = 0; i < hop_count; i++) {
            memcpy(&ci.out_path[i * hash_sz],
                   &path[(hop_count - 1 - i) * hash_sz],
                   hash_sz);
        }
        ci.out_path_len = path_len;
    }

    void onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) override {
        _upsertPeer(contact, path_len & 63);   // low 6 bits = hop count
        // Record reverse of the inbound advert path so trace (and sendDirect) can
        // route to this node without needing a prior bidirectional DM exchange.
        // RAM only: links aren't always symmetric, so a path guessed from an
        // advert is never saved — only ones confirmed by a path return or ACK.
        if (contact.out_path_len == OUT_PATH_UNKNOWN) {
            if (path_len == 0) {
                contact.out_path_len = 0;   // direct neighbour, empty path
            } else {
                _applyReverseOutPath(contact, path, path_len);
            }
        }
        if (_btSerial && _btSerial->isConnected()) {
            if (is_new) {
                _compWriteContactFrame(COMP_PUSH_NEW_ADVERT, contact);
            } else {
                int i = 0;
                _outFrame[i++] = COMP_PUSH_ADVERT;
                memcpy(&_outFrame[i], contact.id.pub_key, PUB_KEY_SIZE); i += PUB_KEY_SIZE;
                _btSerial->writeFrame(_outFrame, i);
            }
        }
        OPS_LOG("Mesh", "Peer: %s (new=%d hops=%d)", contact.name, is_new, path_len);
    }

    ContactInfo* processAck(const uint8_t* data) override {
        uint32_t crc;
        memcpy(&crc, data, 4);
        if (crc == 0) return nullptr;
        for (int s = 0; s < DM_SLOTS; s++) {
            PendingDm& d = _dms[s];
            if (!d.active) continue;
            bool match = false;
            for (int k = 0; k < 4; k++)
                if ((d.ackSet & (1 << k)) && d.acks[k] == crc) { match = true; break; }
            if (!match) continue;
            // An ACK for a direct send confirms the route: refresh its age.
            if (d.lastWasDirect) {
                ContactInfo* ci = lookupContactByPubKey(d.key, 4);
                if (ci) _persistContactPath(*ci, getRTCClock()->getCurrentTime());
            }
            _pushAck(d.id);
            OPS_LOG("Mesh", "ACK crc=%08X for DM id=%08X after %u attempt(s)",
                    crc, d.id, (unsigned)d.attempt);
            d.active = false;
            break;
        }
        return nullptr;
    }

    // Ends a DM's retries without an ACK and tells the UI (pollDmFailed).
    void _failDm(PendingDm& d) {
        d.active = false;
        if (d.id == 0) return;   // first send never went out — UI already says "not sent"
        if (_failQCount == ACK_Q) { _failQHead = (_failQHead + 1) % ACK_Q; _failQCount--; }
        _failQ[(_failQHead + _failQCount) % ACK_Q] = d.id;
        _failQCount++;
    }

    void _pushAck(uint32_t id) {
        if (_ackQCount == ACK_Q) { _ackQHead = (_ackQHead + 1) % ACK_Q; _ackQCount--; }
        _ackQ[(_ackQHead + _ackQCount) % ACK_Q] = id;
        _ackQCount++;
    }

    static uint32_t _fnv1a(const char* t) {
        uint32_t h = 2166136261u;
        for (; *t; t++) { h ^= (uint8_t)*t; h *= 16777619u; }
        return h;
    }

    // True if this DM was already delivered to the UI; records it otherwise.
    bool _isDuplicateDm(const uint8_t* key, uint32_t ts, const char* text) {
        uint32_t h = _fnv1a(text);
        for (int i = 0; i < DUP_SLOTS; i++) {
            const SeenDm& e = _seenDms[i];
            if (e.ts == ts && e.textHash == h && memcmp(e.key, key, 4) == 0) return true;
        }
        SeenDm& e = _seenDms[_seenNext];
        memcpy(e.key, key, 4);
        e.ts       = ts;
        e.textHash = h;
        _seenNext  = (_seenNext + 1) % DUP_SLOTS;
        return false;
    }

    void onContactPathUpdated(const ContactInfo& contact) override {
        _upsertPeer(contact);
        // A path return from the node itself — the path is confirmed working.
        _persistContactPath(contact, getRTCClock()->getCurrentTime());
        if (_btSerial && _btSerial->isConnected()) {
            int i = 0;
            _outFrame[i++] = COMP_PUSH_PATH_UPDATED;
            memcpy(&_outFrame[i], contact.id.pub_key, PUB_KEY_SIZE); i += PUB_KEY_SIZE;
            _btSerial->writeFrame(_outFrame, i);
        }
        OPS_LOG("Mesh", "Path: %s len=%d", contact.name, contact.out_path_len);
    }

    // ── TRACE (0x09) receipt ──────────────────────────────────────────
    // Fires when a TRACE packet arrives at the end of its path.
    // path_snrs = SNR*4 per forwarding hop (one value per node that matched
    //             and retransmitted); path_hashes = raw hash bytes as sent.
    void onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code,
                     uint8_t flags, const uint8_t* path_snrs,
                     const uint8_t* path_hashes, uint8_t path_len) override {
        uint8_t path_sz  = flags & 0x03;               // 0→1-byte, 1→2-byte
        uint8_t hash_sz  = (uint8_t)(1 << path_sz);    // bytes per hash
        uint8_t num_hops = path_len >> path_sz;         // total hashes in payload
        uint8_t num_snrs = packet->path_len;            // how many SNRs accumulated

        if (num_hops == 0 || path_len > 64 || num_snrs > 64) return;

        TraceResult res{};
        res.tag     = tag;
        res.numHops = num_hops;
        res.hashSz  = hash_sz;
        res.numSnrs = num_snrs;
        memcpy(res.hashes, path_hashes, path_len);
        if (num_snrs > 0) memcpy(res.snrs, path_snrs, num_snrs);
        res.rxSnr   = (int8_t)(packet->getSNR() * 4);

        _traceResult    = res;
        _hasTraceResult = true;

        OPS_LOG("Trace", "onTraceRecv tag=%08X hops=%d snrs=%d",
                tag, num_hops, num_snrs);
    }

    void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp, const char* text) override {
        _upsertPeer(from, pkt->isRouteFlood() ? pkt->getPathHashCount() : 0xFF);
        // A retry of a DM we already showed: drop it here. BaseChatMesh still
        // ACKs it after we return, which is what stops the sender retrying.
        if (_isDuplicateDm(from.id.pub_key, sender_timestamp, text)) {
            OPS_LOG("RX", "[DM] duplicate from %s dropped (ts=%lu)",
                    from.name, (unsigned long)sender_timestamp);
            return;
        }
        // Opportunistically record the return path from a flood DM. RAM only,
        // like the advert case: it's a guess until a path return confirms it.
        if (from.out_path_len == OUT_PATH_UNKNOWN && pkt->isRouteFlood()) {
            ContactInfo* ci = lookupContactByPubKey(from.id.pub_key, PUB_KEY_SIZE);
            if (ci) {
                if (pkt->path_len == 0) ci->out_path_len = 0;
                else _applyReverseOutPath(*ci, pkt->path, pkt->path_len);
            }
        }
        if (_blankText(text)) {   // nothing to show; still ACKed by BaseChatMesh
            OPS_LOG("RX", "[DM] empty message from %s dropped", from.name);
            _compQueueContactMsg(from, pkt, sender_timestamp, text);
            return;
        }
        RxMessage msg{};
        // No sender clock: show when we received it rather than "--:--".
        msg.timestamp = sender_timestamp ? sender_timestamp : getRTCClock()->getCurrentTime();
        strncpy(msg.senderName, from.name, 31);
        strncpy(msg.text, text, 159);
        msg.rssi     = radio_driver.getLastRSSI();
        msg.snr      = radio_driver.getLastSNR();
        msg.isDirect = true;  // onMessageRecv is always a DM; isRouteDirect() describes routing, not message type
        msg.hops     = pkt->getPathHashCount();
        _fmtPathStr(msg.pathStr, sizeof(msg.pathStr), pkt);
        memcpy(msg.pubKeyPrefix, from.id.pub_key, 4);
        OPS_LOG("RX", "[DM] %s: %.100s  h:%u rssi:%.0f snr:%.0f",
                from.name, text, msg.hops, (double)msg.rssi, (double)msg.snr);
        _enqueueRx(msg);
        _compQueueContactMsg(from, pkt, sender_timestamp, text);
    }

    void onCommandDataRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp, const char* text) override {}

    // ── Zero-hop control data (Finder / discover protocol) ────────────
    static constexpr uint8_t CTL_DISCOVER_REQ  = 0x80;
    static constexpr uint8_t CTL_DISCOVER_RESP = 0x90;

    void onControlDataRecv(mesh::Packet* pkt) override {
        if (pkt->payload_len < 1) return;
        uint8_t typeMask = pkt->payload[0] & 0xF0;

        if (typeMask == CTL_DISCOVER_REQ && pkt->payload_len >= 6) {
            // Respond if our node type matches the requested filter
            uint8_t filter = pkt->payload[1];
            uint32_t tag;
            memcpy(&tag, &pkt->payload[2], 4);

            bool matches = (filter == 0) || ((filter & (1 << ADV_TYPE_CHAT)) != 0);
            if (!matches) return;
            if (tag == _lastDiscoverRespTag) return;  // already responded to this scan
            _lastDiscoverRespTag = tag;

            uint8_t resp[38];
            resp[0] = CTL_DISCOVER_RESP | ADV_TYPE_CHAT;
            resp[1] = (uint8_t)pkt->_snr;
            memcpy(&resp[2], &tag, 4);
            memcpy(&resp[6], self_id.pub_key, 32);
            mesh::Packet* rpkt = createControlData(resp, 38);
            if (rpkt) sendZeroHop(rpkt, getRetransmitDelay(rpkt) * 4);
            OPS_LOG("Finder", "Replied to discover req tag=%08X", tag);

        } else if (typeMask == CTL_DISCOVER_RESP && pkt->payload_len >= 6 + 32) {
            // Only accept responses for an active scan within the time window
            if (_discoverTag == 0 || millis() > _discoverDeadlineMs) return;
            uint32_t tag;
            memcpy(&tag, &pkt->payload[2], 4);
            if (tag != _discoverTag) return;

            // Skip our own response (loopback)
            const uint8_t* pubKey = &pkt->payload[6];
            if (memcmp(pubKey, self_id.pub_key, 4) == 0) return;

            // Deduplicate: skip if same prefix already queued
            for (int i = 0; i < _discoverCount; i++) {
                int idx = (_discoverHead + i) % DISCOVER_QUEUE_SIZE;
                if (memcmp(_discoverBuf[idx].pubKeyPrefix, pubKey, 4) == 0) return;
            }

            if (_discoverCount >= DISCOVER_QUEUE_SIZE) return;

            uint8_t nodeType = pkt->payload[0] & 0x0F;
            DiscoverEntry& e = _discoverBuf[_discoverTail];
            memset(&e, 0, sizeof(e));
            memcpy(e.pubKey,       pubKey, 32);
            memcpy(e.pubKeyPrefix, pubKey, 4);
            e.nodeType   = nodeType;
            e.rssi       = radio_driver.getLastRSSI();
            e.snrInbound = (int8_t)pkt->payload[1];

            // Lookup name from known peers
            for (int i = 0; i < _peerCount; i++) {
                if (memcmp(_peers[i].pubKey, pubKey, 32) == 0) {
                    strncpy(e.name, _peers[i].name, 31);
                    break;
                }
            }
            if (!e.name[0]) {   // not heard since boot — try the saved lists
                int si;
                ops::Repeater sr;
                ops::Contact  sc;
                if (ops::repeaters::findByKey(pubKey, &si) && ops::repeaters::get(si, sr))
                    strncpy(e.name, sr.name, 31);
                else if (ops::contacts::findByKey(pubKey, &si) && ops::contacts::get(si, sc))
                    strncpy(e.name, sc.name, 31);
            }

            _discoverTail = (_discoverTail + 1) % DISCOVER_QUEUE_SIZE;
            _discoverCount++;
            OPS_LOG("Finder", "RESP %02X%02X type=%d rssi=%.0f",
                    pubKey[0], pubKey[1], nodeType, (double)e.rssi);

            // Add to in-memory routing table as a direct neighbor (path_len=0)
            ContactInfo* known = lookupContactByPubKey(pubKey, 4);
            if (!known) {
                ContactInfo ci{};
                ci.id = mesh::Identity(pubKey);
                if (e.name[0]) strncpy(ci.name, e.name, 31);
                ci.type        = nodeType;
                ci.out_path_len = 0;
                addContact(ci);
            } else {
                if (!known->name[0] && e.name[0]) strncpy(known->name, e.name, 31);
                // The reply proves a direct link both ways — route straight
                // to it (unless a path is already set; it may be pinned), and
                // save that so it is reloaded after a reboot.
                if (known->out_path_len == OUT_PATH_UNKNOWN) known->out_path_len = 0;
                if (known->out_path_len == 0)
                    _persistContactPath(*known, getRTCClock()->getCurrentTime());
            }
            if (nodeType == ADV_TYPE_REPEATER && millis() < _regScanUntil)
                _regAskNeighbour(pubKey, e.name);

            // Auto-add to NVS lists when configured — name falls back to hex prefix
            char fallbackName[16];
            snprintf(fallbackName, sizeof(fallbackName), "%02X%02X%02X%02X",
                     e.pubKeyPrefix[0], e.pubKeyPrefix[1],
                     e.pubKeyPrefix[2], e.pubKeyPrefix[3]);
            const char* useName = e.name[0] ? e.name : fallbackName;
            const auto& cfg = ops::config::get();
            bool isRpt = (nodeType == ADV_TYPE_REPEATER);
            if (isRpt && cfg.autoAddRepeater) {
                if (!ops::repeaters::findByKey(e.pubKeyPrefix)) {
                    ops::Repeater r{};
                    strncpy(r.name, useName, sizeof(r.name) - 1);
                    memcpy(r.pubKeyPrefix, e.pubKeyPrefix, 4);
                    memcpy(r.pubKey,       e.pubKey,       32);
                    r.lastSeen     = getRTCClock()->getCurrentTime();
                    r.lastRssi     = e.rssi;
                    r.outPathLen   = 0;
                    r.outPathValid = true;
                    // A discover reply proves the link both ways — a confirmed path.
                    r.pathAt       = ops::pathAtFor(r.lastSeen);
                    ops::repeaters::add(r);
                    OPS_LOG("Finder", "Auto-added repeater: %s", useName);
                }
            } else if (!isRpt && cfg.autoAddClient) {
                if (!ops::contacts::findByKey(e.pubKeyPrefix)) {
                    ops::Contact c{};
                    strncpy(c.name, useName, sizeof(c.name) - 1);
                    memcpy(c.pubKeyPrefix, e.pubKeyPrefix, 4);
                    memcpy(c.pubKey,       e.pubKey,       32);
                    c.lastSeen     = getRTCClock()->getCurrentTime();
                    c.lastRssi     = e.rssi;
                    c.outPathLen   = 0;
                    c.outPathValid = true;
                    c.pathAt       = ops::pathAtFor(c.lastSeen);
                    ops::contacts::add(c);
                    OPS_LOG("Finder", "Auto-added contact: %s", useName);
                }
            }
        }
    }

    void onSignedMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp,
                             const uint8_t* sender_prefix, const char* text) override {
        onMessageRecv(from, pkt, sender_timestamp, text);
    }

    uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override {
        return SEND_TIMEOUT_BASE_MILLIS + (uint32_t)(FLOOD_SEND_TIMEOUT_FACTOR * pkt_airtime_millis);
    }

    uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override {
        uint8_t hops = path_len & 63;
        return SEND_TIMEOUT_BASE_MILLIS +
               (uint32_t)((pkt_airtime_millis * DIRECT_SEND_PERHOP_FACTOR + DIRECT_SEND_PERHOP_EXTRA_MILLIS) * (hops + 1));
    }

    // BaseChatMesh's single send timer. DM timeouts are handled per message by
    // _serviceDmRetries(), so there is nothing to do here.
    void onSendTimeout() override {}

    // Direct path failed: forget it in RAM and in the NVS/SD stores, so the
    // next send floods and a reboot doesn't reload the stale path.
    void _dropFailedPath(ContactInfo& ci) {
        OPS_LOG("Mesh", "Direct attempts to '%s' exhausted — resetting path", ci.name);
        resetPathTo(ci);
        int idx;
        if (ops::contacts::findByKey(ci.id.pub_key, &idx))  ops::contacts::clearPath(idx);
        if (ops::repeaters::findByKey(ci.id.pub_key, &idx)) ops::repeaters::clearPath(idx);
    }

    // Sends the slot's next attempt. Returns false (and frees the slot) if it
    // could not be sent.
    bool _sendDmAttempt(PendingDm& d) {
        ContactInfo* ci = lookupContactByPubKey(d.key, 4);
        if (!ci) {
            OPS_LOG("Mesh", "DM id=%08X: contact no longer in table", d.id);
            _failDm(d);
            return false;
        }
        // MeshCore only fits attempt numbers above 3 when the text leaves two
        // spare bytes; past that a retry would repeat attempt 0's packet.
        if (d.attempt > 3 && strlen(d.text) > MAX_TEXT_LEN - 2) {
            OPS_LOG("Mesh", "DM id=%08X: text too long for more than 4 attempts", d.id);
            _failDm(d);
            return false;
        }
        uint32_t ack = 0, est = 0;
        int r = sendMessage(*ci, d.ts, d.attempt, d.text, ack, est);
        if (r == MSG_SEND_FAILED) {
            OPS_LOG("Mesh", "DM id=%08X: send failed (attempt %u)", d.id, (unsigned)d.attempt);
            _failDm(d);
            return false;
        }
        if (d.attempt == 0) d.id = ack;
        d.acks[d.attempt & 3] = ack;
        d.ackSet |= (uint8_t)(1 << (d.attempt & 3));
        d.lastWasDirect = (r == MSG_SEND_SENT_DIRECT);
        d.deadline = millis() + est;
        OPS_LOG("Mesh", "DM id=%08X attempt %u sent %s (timeout %lu ms)", d.id,
                (unsigned)d.attempt, d.lastWasDirect ? "direct" : "flood", (unsigned long)est);
        d.attempt++;
        return true;
    }

    // Called every tick: retries timed-out DMs — direct up to dmDirectRetries
    // times, then drops the path and floods up to dmFloodRetries times.
    void _serviceDmRetries() {
        unsigned long now = millis();
        for (int s = 0; s < DM_SLOTS; s++) {
            PendingDm& d = _dms[s];
            if (!d.active || (long)(now - d.deadline) < 0) continue;

            ContactInfo* ci = lookupContactByPubKey(d.key, 4);
            if (!ci) { _failDm(d); continue; }

            // Pick the budget for the send that would go out next.
            if (ci->out_path_len != OUT_PATH_UNKNOWN) {
                if (d.directLeft > 0) {
                    d.directLeft--;
                    _sendDmAttempt(d);
                    continue;
                }
                _dropFailedPath(*ci);   // falls through to flood
            }
            if (d.floodLeft > 0) {
                d.floodLeft--;
                _sendDmAttempt(d);
            } else {
                OPS_LOG("Mesh", "DM id=%08X to '%s' not acknowledged after %u attempt(s) — giving up",
                        d.id, ci->name, (unsigned)d.attempt);
                _failDm(d);
            }
        }
    }

    // True if text has nothing to show (empty or only whitespace).
    static bool _blankText(const char* t) {
        for (; t && *t; t++) if (!isspace((unsigned char)*t)) return false;
        return true;
    }

    void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                              uint32_t timestamp, const char* text) override {
        RxMessage msg{};
        msg.timestamp = timestamp;
        msg.rssi      = radio_driver.getLastRSSI();
        msg.snr       = radio_driver.getLastSNR();
        msg.isDirect  = false;
        msg.hops      = pkt->getPathHashCount();
        _fmtPathStr(msg.pathStr, sizeof(msg.pathStr), pkt);

        // Channel messages are "<sender>: <text>" by convention
        const char* colon = strstr(text, ": ");
        if (colon && (colon - text) < (int)sizeof(msg.senderName) - 1) {
            int n = colon - text;
            strncpy(msg.senderName, text, n);
            msg.senderName[n] = '\0';
            strncpy(msg.text, colon + 2, 159);
        } else {
            strncpy(msg.senderName, "?", 31);
            strncpy(msg.text, text, 159);
        }

        int idx = findChannelIdx(channel);
        if (idx >= 0) {
            ChannelDetails cd;
            if (getChannel(idx, cd)) strncpy(msg.channelName, cd.name, sizeof(msg.channelName) - 1);
        }
        // Some senders (bots, clients without a clock) send empty text with a
        // zero timestamp: nothing to show, so no alert / "?" bubble.
        if (_blankText(msg.text)) {
            OPS_LOG("RX", "[#%s] empty message from %s dropped",
                    msg.channelName[0] ? msg.channelName : "?", msg.senderName);
            _compQueueChannelMsg(channel, pkt, timestamp, text);   // the phone app still gets it
            return;
        }
        // No sender clock: show when we received it rather than "--:--".
        if (msg.timestamp == 0) msg.timestamp = getRTCClock()->getCurrentTime();
        OPS_LOG("RX", "[#%s] %s: %.100s  rssi:%.0f snr:%.0f",
                msg.channelName[0] ? msg.channelName : "?",
                msg.senderName, msg.text, (double)msg.rssi, (double)msg.snr);
        _enqueueRx(msg);
        _compQueueChannelMsg(channel, pkt, timestamp, text);
    }

    uint8_t onContactRequest(const ContactInfo& contact, uint32_t sender_timestamp,
                             const uint8_t* data, uint8_t len, uint8_t* reply) override {
        return 0;
    }

    // Rough state of charge for a single Li-ion/LiPo cell from its voltage
    // (typical resting discharge curve, interpolated). 4.2 V and above —
    // full, or on external power/charging (USB reads ~4.3 V) — is 100%.
    static int _liionPercent(uint16_t mv) {
        static const uint16_t kMv[]  = { 3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200 };
        static const uint8_t  kPct[] = {    0,    5,   10,   20,   30,   40,   50,   60,   75,   90,  100 };
        const int n = sizeof(kMv) / sizeof(kMv[0]);
        if (mv >= kMv[n - 1]) return 100;
        if (mv <= kMv[0])     return 0;
        for (int i = 1; i < n; i++) {
            if (mv <= kMv[i])
                return kPct[i - 1] + (int)(mv - kMv[i - 1]) * (kPct[i] - kPct[i - 1]) / (kMv[i] - kMv[i - 1]);
        }
        return 100;
    }

    void onContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len) override
    {
        if (_takeRegionReply(contact, data, len)) return;

        // All repeater responses are prefixed with a 4-byte timestamp.
        // The actual response code/text starts at data[4].
        const uint8_t* payload    = (len > 4) ? &data[4] : data;
        uint8_t        payloadLen = (len > 4) ? len - 4  : len;

        bool isLoginOk = (payloadLen >= 1 && payload[0] == RESP_SERVER_LOGIN_OK);

        if (_waitingForLoginResp) {
            _waitingForLoginResp = false;
            _loginResultOk       = isLoginOk;
            _loginResultPending  = true;
        }

        OPS_LOG("Mesh", "ContactResponse from %s len=%d payload[0]=0x%02X",
                contact.name, len, payloadLen > 0 ? payload[0] : 0xFF);

        char buf[132];
        if (isLoginOk) {
            snprintf(buf, sizeof(buf), "[%s] Login OK", contact.name);
            _enqueueResp(buf);
        } else if (_pendingResp == PENDING_STATUS && payloadLen >= 16) {
            _pendingResp = PENDING_NONE;
            // Binary RepeaterStats struct — parse field-by-field.
            // Layout (little-endian):
            //   u16 batt_mv, u16 tx_queue, i16 noise, i16 rssi,
            //   u32 n_recv, u32 n_sent, u32 airtime_tx_s, u32 uptime_s,
            //   u32 n_sent_flood, u32 n_sent_direct, u32 n_recv_flood, u32 n_recv_direct,
            //   u16 err, i16 snr_x4, u16 direct_dups, u16 flood_dups,
            //   u32 airtime_rx_s, u32 n_errors
            uint16_t batt_mv   = 0;
            uint16_t tx_queue  = 0;
            int16_t  noise     = 0;
            int16_t  rssi      = 0;
            uint32_t n_recv    = 0, n_sent   = 0;
            uint32_t air_tx    = 0, uptime_s = 0;
            uint32_t f_sent    = 0, d_sent   = 0;
            uint32_t f_recv    = 0, d_recv   = 0;
            int16_t  snr_x4   = 0;
            uint32_t air_rx    = 0, n_err    = 0;

            int o = 0;
#define RD16(dst) do { if (o + 2 <= (int)payloadLen) { memcpy(&(dst), payload + o, 2); o += 2; } } while(0)
#define RD32(dst) do { if (o + 4 <= (int)payloadLen) { memcpy(&(dst), payload + o, 4); o += 4; } } while(0)
#define SKIP16()  do { if (o + 2 <= (int)payloadLen) { o += 2; } } while(0)
            RD16(batt_mv); RD16(tx_queue); RD16(noise); RD16(rssi);
            RD32(n_recv);  RD32(n_sent);   RD32(air_tx); RD32(uptime_s);
            RD32(f_sent);  RD32(d_sent);   RD32(f_recv); RD32(d_recv);
            SKIP16();  // err_events
            RD16(snr_x4);
            SKIP16(); SKIP16();  // n_direct_dups, n_flood_dups
            RD32(air_rx); RD32(n_err);
#undef RD16
#undef RD32
#undef SKIP16

            uint32_t up_d = uptime_s / 86400;
            uint32_t up_h = (uptime_s % 86400) / 3600;
            uint32_t up_m = (uptime_s % 3600) / 60;
            int      snr_i = snr_x4 / 4;
            int      snr_f = ((snr_x4 < 0 ? -snr_x4 : snr_x4) % 4) * 25;

            snprintf(buf, sizeof(buf), "[%s] --- Status ---", contact.name);
            _enqueueResp(buf);
            char battBuf[24];
            if (batt_mv == 0) snprintf(battBuf, sizeof(battBuf), "n/a");   // board can't measure
            else              snprintf(battBuf, sizeof(battBuf), "%dmV (%d%%)",
                                       (int)batt_mv, _liionPercent(batt_mv));
            snprintf(buf, sizeof(buf),
                "[%s] Batt:%s  RSSI:%d  SNR:%d.%02d  Noise:%d",
                contact.name, battBuf, (int)rssi, snr_i, snr_f, (int)noise);
            _enqueueResp(buf);
            snprintf(buf, sizeof(buf),
                "[%s] Recv:%u  Sent:%u  Err:%u  Queue:%u",
                contact.name, (unsigned)n_recv, (unsigned)n_sent,
                (unsigned)n_err, (unsigned)tx_queue);
            _enqueueResp(buf);
            snprintf(buf, sizeof(buf),
                "[%s] Flood R/T:%u/%u  Direct R/T:%u/%u",
                contact.name, (unsigned)f_recv, (unsigned)f_sent,
                (unsigned)d_recv, (unsigned)d_sent);
            _enqueueResp(buf);
            snprintf(buf, sizeof(buf),
                "[%s] TxAir:%us  RxAir:%us  Up:%ud%uh%02um",
                contact.name, (unsigned)air_tx, (unsigned)air_rx,
                (unsigned)up_d, (unsigned)up_h, (unsigned)up_m);
            _enqueueResp(buf);
        } else if (_pendingResp == PENDING_NEIGHBOURS && payloadLen >= 4) {
            _pendingResp = PENDING_NONE;
            // Neighbours response layout (after 4-byte timestamp stripped):
            //   u16 total_count, u16 returned_count
            //   Per entry: prefix_len(4) bytes, u32 secs_ago, i8 snr_x4
            uint16_t total_ct = 0, ret_ct = 0;
            memcpy(&total_ct, payload + 0, 2);
            memcpy(&ret_ct,   payload + 2, 2);
            snprintf(buf, sizeof(buf), "[%s] Nbrs: %u known, %u returned",
                contact.name, (unsigned)total_ct, (unsigned)ret_ct);
            _enqueueResp(buf);
            int o = 4;
            for (int i = 0; i < (int)ret_ct && o + 9 <= (int)payloadLen; i++) {
                uint8_t  pfx[4];
                uint32_t secs_ago = 0;
                int8_t   snr8     = 0;
                memcpy(pfx,       payload + o, 4); o += 4;
                memcpy(&secs_ago, payload + o, 4); o += 4;
                memcpy(&snr8,     payload + o, 1); o += 1;
                uint32_t m = secs_ago / 60;
                uint32_t h = m / 60;
                char ago[16];
                if (h > 0)          snprintf(ago, sizeof(ago), "%uh ago", (unsigned)h);
                else if (m > 0)     snprintf(ago, sizeof(ago), "%um ago", (unsigned)m);
                else                snprintf(ago, sizeof(ago), "%us ago", (unsigned)secs_ago);
                int snr_i2 = snr8 / 4;
                int snr_f2 = ((snr8 < 0 ? -snr8 : snr8) % 4) * 25;
                snprintf(buf, sizeof(buf),
                    "[%s] %02X:%02X:%02X:%02X  %s  SNR:%d.%02d",
                    contact.name,
                    pfx[0], pfx[1], pfx[2], pfx[3],
                    ago, snr_i2, snr_f2);
                _enqueueResp(buf);
            }
        } else if (payloadLen > 0) {
            _pendingResp = PENDING_NONE;
            // Short text response (CLI reply, error message, etc.)
            char text[120] = {};
            int n = (payloadLen < (int)sizeof(text) - 1) ? payloadLen : (int)sizeof(text) - 1;
            memcpy(text, payload, n);
            text[n] = '\0';
            snprintf(buf, sizeof(buf), "[%s] %s", contact.name, text);
            _enqueueResp(buf);
        } else {
            snprintf(buf, sizeof(buf), "[%s] (empty response)", contact.name);
            _enqueueResp(buf);
        }
    }

public:
    bool dequeueLoginResult(bool& ok)
    {
        if (!_loginResultPending) return false;
        ok = _loginResultOk;
        _loginResultPending = false;
        return true;
    }

    // Retries timed-out DMs; called from MeshService::tick().
    void serviceDmRetries() { _serviceDmRetries(); }

    bool pollDmFailed(uint32_t& id) {
        if (_failQCount == 0) return false;
        id = _failQ[_failQHead];
        _failQHead = (_failQHead + 1) % ACK_Q;
        _failQCount--;
        return true;
    }

    bool pollAck(uint32_t& crc) {
        if (_ackQCount == 0) return false;
        crc = _ackQ[_ackQHead];
        _ackQHead = (_ackQHead + 1) % ACK_Q;
        _ackQCount--;
        return true;
    }

    bool dequeueResp(char* out, int outMax) {
        if (_respCount == 0) return false;
        strncpy(out, _respBuf[_respHead], outMax - 1);
        out[outMax - 1] = '\0';
        _respHead = (_respHead + 1) % RESP_QUEUE_SIZE;
        _respCount--;
        return true;
    }

    bool sendRepeatersStatusReq(int timeoutSecs) {
        if (!_active) return false;
        (void)timeoutSecs;
        int n = ops::repeaters::count();
        if (n == 0) {
            _enqueueResp("[repeaters] No repeaters in list.");
            return false;
        }
        int sent = 0;
        for (int i = 0; i < n; i++) {
            ops::Repeater r;
            if (!ops::repeaters::get(i, r)) continue;
            ContactInfo* ci = lookupContactByPubKey(r.pubKeyPrefix, 4);
            if (!ci) {
                char buf[80];
                snprintf(buf, sizeof(buf), "[%s] Not in mesh table (send advert first)", r.name);
                _enqueueResp(buf);
                continue;
            }
            uint32_t tag = 0, timeout = 0;
            int ret = sendRequest(*ci, REQ_TYPE_GET_STATUS, tag, timeout);
            char buf[80];
            snprintf(buf, sizeof(buf), "[%s] Status request %s",
                     r.name, ret != MSG_SEND_FAILED ? "sent" : "FAILED");
            _enqueueResp(buf);
            if (ret != MSG_SEND_FAILED) sent++;
        }
        return sent > 0;
    }

    bool sendRepeaterLoginReq(const uint8_t* prefix4, const char* password) {
        if (!_active) return false;
        ContactInfo* ci = lookupContactByPubKey(prefix4, 4);
        if (!ci) {
            _enqueueResp("[repeateradmin] Contact not in mesh table.");
            return false;
        }
        uint32_t timeout = 0;
        int ret = sendLogin(*ci, password ? password : "", timeout);
        if (ret != MSG_SEND_FAILED)
            _waitingForLoginResp = true;
        char buf[100];
        snprintf(buf, sizeof(buf), "[%s] Login %s",
                 ci->name[0] ? ci->name : "repeater",
                 ret != MSG_SEND_FAILED ? "request sent" : "request FAILED");
        _enqueueResp(buf);
        return ret != MSG_SEND_FAILED;
    }

    bool sendSingleRepeaterStatus(const uint8_t* prefix4) {
        if (!_active) return false;
        ContactInfo* ci = lookupContactByPubKey(prefix4, 4);
        if (!ci) {
            _enqueueResp("[repadmin] Contact not in mesh table.");
            return false;
        }
        // Force flood so the repeater uses createPathReturn with the response
        // embedded — the same reliable delivery path used for login replies.
        ContactInfo ci_flood = *ci;
        ci_flood.out_path_len = OUT_PATH_UNKNOWN;
        _pendingResp = PENDING_STATUS;
        uint32_t tag = 0, timeout = 0;
        int ret = sendRequest(ci_flood, REQ_TYPE_GET_STATUS, tag, timeout);
        if (ret == MSG_SEND_FAILED) {
            char buf[80];
            snprintf(buf, sizeof(buf), "[%s] Status request FAILED",
                     ci->name[0] ? ci->name : "repeater");
            _enqueueResp(buf);
        }
        return ret != MSG_SEND_FAILED;
    }

    bool sendSingleRepeaterNeighbours(const uint8_t* prefix4) {
        if (!_active) return false;
        ContactInfo* ci = lookupContactByPubKey(prefix4, 4);
        if (!ci) {
            _enqueueResp("[repadmin] Contact not in mesh table.");
            return false;
        }
        ContactInfo ci_flood = *ci;
        ci_flood.out_path_len = OUT_PATH_UNKNOWN;
        // Payload: req_type, version=0, count=10, offset=0 (2 bytes),
        //          order_by=0 (newest first), prefix_len=4, rng (4 bytes)
        uint8_t req[11] = {
            REQ_TYPE_GET_NEIGHBOURS, 0, 10,
            0, 0,   // offset lo, hi
            0,      // order_by: newest first
            4,      // pubkey_prefix_length
            0, 0, 0, 0  // random blob
        };
        _pendingResp = PENDING_NEIGHBOURS;
        uint32_t tag = 0, timeout = 0;
        int ret = sendRequest(ci_flood, req, sizeof(req), tag, timeout);
        if (ret == MSG_SEND_FAILED) {
            _pendingResp = PENDING_NONE;
            char buf[80];
            snprintf(buf, sizeof(buf), "[%s] Nbrs request FAILED",
                     ci->name[0] ? ci->name : "repeater");
            _enqueueResp(buf);
        }
        return ret != MSG_SEND_FAILED;
    }

    bool sendAdminCommandTo(const uint8_t* prefix4, const char* command) {
        if (!_active) return false;
        ContactInfo* ci = lookupContactByPubKey(prefix4, 4);
        if (!ci) {
            _enqueueResp("[repadmin] Contact not in mesh table.");
            return false;
        }
        uint32_t ts = getRTCClock()->getCurrentTime();
        uint32_t timeout = 0;
        int ret = sendCommandData(*ci, ts, 0, command, timeout);
        if (ret == MSG_SEND_FAILED) {
            char buf[80];
            snprintf(buf, sizeof(buf), "[%s] Command send FAILED",
                     ci->name[0] ? ci->name : "repeater");
            _enqueueResp(buf);
        }
        return ret != MSG_SEND_FAILED;
    }

    bool pollTraceResult(TraceResult& out) {
        if (!_hasTraceResult) return false;
        out = _traceResult;
        _hasTraceResult = false;
        return true;
    }

    bool sendDiscoverReqMsg(uint8_t typeFilter) {
        if (!_active) return false;
        uint8_t data[10];
        data[0] = CTL_DISCOVER_REQ;
        data[1] = typeFilter;
        _discoverTag = (uint32_t)std_rng.nextInt(1, 0x7FFFFFFF);
        memcpy(&data[2], &_discoverTag, 4);
        uint32_t since = 0;
        memcpy(&data[6], &since, 4);
        mesh::Packet* pkt = createControlData(data, sizeof(data));
        if (!pkt) return false;
        sendZeroHop(pkt, (uint32_t)0);
        _discoverDeadlineMs = millis() + 10000;
        // Clear any stale results from a previous scan
        _discoverHead = 0; _discoverTail = 0; _discoverCount = 0;
        OPS_LOG("Finder", "Scan sent tag=%08X filter=%02X", _discoverTag, typeFilter);
        return true;
    }

    bool pollDiscoverResult(DiscoverEntry& out) {
        if (_discoverCount == 0) return false;
        out = _discoverBuf[_discoverHead];
        _discoverHead = (_discoverHead + 1) % DISCOVER_QUEUE_SIZE;
        _discoverCount--;
        return true;
    }

    bool hasPathToContact(const uint8_t* prefix4) const {
        // const_cast: lookupContactByPubKey is non-const in BaseChatMesh
        OMSMesh* self = const_cast<OMSMesh*>(this);
        ContactInfo* ci = self->lookupContactByPubKey(prefix4, 4);
        return ci && ci->out_path_len != OUT_PATH_UNKNOWN;
    }

    // Sends TRACE (0x09) along the known path to a contact, out and back.
    // A TRACE ends where its hash list runs out, so a one-way route
    // [R1..Rn, target] only reports back if we can hear the target directly.
    // Sending [R1..Rn, target, Rn..R1] makes R1 the last hop instead. The same
    // node may appear twice: MeshCore folds path_len into a TRACE's hash.
    // Non-repeaters usually don't forward, so for a contact behind relays the
    // route turns at its last relay: [R1..Rn..R1].
    bool sendTraceToContact(const uint8_t* prefix4, uint32_t& out_tag, int& out_hops) {
        if (!_active) return false;
        ContactInfo* ci = lookupContactByPubKey(prefix4, 4);
        if (!ci || ci->out_path_len == OUT_PATH_UNKNOWN) {
            OPS_LOG("Trace", "sendTrace: no path to %02X%02X", prefix4[0], prefix4[1]);
            return false;
        }

        uint8_t hash_sz_code = ci->out_path_len >> 6;   // 0,1,2
        uint8_t hop_count    = ci->out_path_len & 63;
        uint8_t hash_sz      = hash_sz_code + 1;         // 1,2,3 bytes

        // TRACE path_sz flag uses 1<<path_sz encoding; only 1-byte and 2-byte paths
        // produce exact powers of two, so cap at 2 byte.
        if (hash_sz > 2) {
            OPS_LOG("Trace", "sendTrace: 3-byte hash paths not supported");
            return false;
        }
        uint8_t flags = hash_sz - 1;                     // 0 for 1-byte, 1 for 2-byte

        // Turn at the target when it forwards (repeater) or is a direct
        // neighbour (nothing else to turn at); otherwise at the last relay.
        bool viaTarget = (ci->type == ADV_TYPE_REPEATER) || hop_count == 0;
        int  nodes     = viaTarget ? 2 * hop_count + 1 : 2 * hop_count - 1;

        static uint8_t combined[MAX_PATH_SIZE];
        if (nodes * hash_sz > (int)sizeof(combined)) {
            OPS_LOG("Trace", "sendTrace: route too long to trace out and back (%d nodes)", nodes);
            return false;
        }
        int n = 0;
        for (int i = 0; i < hop_count; i++, n += hash_sz)            // out
            memcpy(combined + n, ci->out_path + i * hash_sz, hash_sz);
        if (viaTarget) {                                              // turn
            memcpy(combined + n, ci->id.pub_key, hash_sz);
            n += hash_sz;
        }
        for (int i = hop_count - 2 + (viaTarget ? 1 : 0); i >= 0; i--, n += hash_sz)  // back
            memcpy(combined + n, ci->out_path + i * hash_sz, hash_sz);

        uint32_t tag  = (uint32_t)std_rng.nextInt(1, 0x7FFFFFFF);
        uint32_t auth = (uint32_t)std_rng.nextInt(1, 0x7FFFFFFF);

        mesh::Packet* pkt = createTrace(tag, auth, flags);
        if (!pkt) return false;
        sendDirect(pkt, combined, (uint8_t)n, 0);

        out_tag  = tag;
        out_hops = nodes;

        // Log path bytes so the user can verify the path is correct
        char pathHex[3 * MAX_PATH_SIZE + 1] = {};
        for (int b = 0; b < n; b++)
            snprintf(pathHex + b * 3, 4, "%02X ", combined[b]);
        OPS_LOG("Trace", "Sent trace to %02X%02X nodes=%d flags=%02X path=[%s]tag=%08X",
                prefix4[0], prefix4[1], nodes, flags, pathHex, tag);
        return true;
    }

    uint32_t lastExpectedAck() const { return _lastExpectedAck; }

    // ── BT Companion public interface ──────────────────────────────────
    void startCompanionInterface(BaseSerialInterface& serial)
    {
        _btSerial = &serial;
        serial.enable();
        _compQueueLen   = 0; // clear stale queue from last session
        _contactIterSrc = ITER_NONE;
        _contactIterIdx = 0;
        _appTargetVer   = 0;
        OPS_LOG("BT", "Companion interface wired and enabled");
    }

    void stopCompanionInterface()
    {
        // Intentionally NOT calling _btSerial->disable() here.
        // disable() calls pService->stop(); the subsequent pService->start() on
        // re-enable then corrupts the ESP32 BLE GATT heap (observed as
        // "GATTS_StopService not in use" → Malloc failed → hash_map_set assert).
        // Nulling _btSerial is sufficient: checkRecvFrame() won't be called so
        // the auto-restart advertising timer cannot fire while BT is disabled.
        // Advertising stop and client disconnect are handled by
        // BTCompanionService::stop() via BLEDevice::getAdvertising().
        _btSerial       = nullptr;
        _contactIterSrc = ITER_NONE;
        _contactIterIdx = 0;
        _appTargetVer   = 0;
        OPS_LOG("BT", "Companion interface stopped");
    }

    bool isBTConnected() const
    {
        return _btSerial && _btSerial->isConnected();
    }

    void checkSerialInterface()
    {
        if (!_btSerial) return;
        // Always drain the BLE send queue and read any incoming command first.
        // checkRecvFrame() sends one pending frame (if the 60ms throttle allows)
        // AND returns the next received command. Handling commands before pumping
        // matches the reference implementation and ensures the phone's retries or
        // interleaved commands are never silently dropped.
        size_t len = _btSerial->checkRecvFrame(_cmdFrame);
        if (len > 0) {
            _handleCmdFrame(len);
            return;
        }
        // Pump contact iteration only when there is no incoming command this tick.
        if (_contactIterSrc != ITER_NONE) {
            _compPumpContactIter();
        }
    }

    OMSMesh()
        : BaseChatMesh(radio_driver, ms_clock, std_rng, rtc_clock, pkt_mgr, mesh_tables)
    {}

    void begin_mesh(const char* callsign) {
        strncpy(_callsign, callsign, 31);
        _callsign[31] = '\0';

        BaseChatMesh::begin();

        uint32_t seed = esp_random() ^ (uint32_t)millis();
        std_rng.begin((long)seed);

        // LittleFS is optional — the Launcher partition layout may omit the spiffs
        // partition entirely.  Continue without it; NVS and SD are the fallbacks.
        bool fsOk = LittleFS.begin(true);
        if (!fsOk) OPS_LOG("Mesh", "LittleFS unavailable — using NVS/SD for identity");
        if (fsOk && !LittleFS.exists("/mesh")) LittleFS.mkdir("/mesh");

        IdentityStore store(LittleFS, "/mesh");
        if (fsOk) store.begin();

        char stored_name[32] = {};
        bool loaded = fsOk && store.load("self", self_id, stored_name, sizeof(stored_name));

        // Helper: load LocalIdentity from raw Stream-format bytes (pub[32]+prv[64]+name[32]).
        // readFrom(buf,96) expects prv[64]+pub[32] — rearrange before calling.
        auto loadFromBytes = [&](const uint8_t* buf, size_t len) -> bool {
            if (len < 96) return false;
            uint8_t reordered[96];
            memcpy(reordered,      buf + PUB_KEY_SIZE, PRV_KEY_SIZE);   // prv
            memcpy(reordered + PRV_KEY_SIZE, buf,      PUB_KEY_SIZE);   // pub
            self_id.readFrom(reordered, (size_t)(PRV_KEY_SIZE + PUB_KEY_SIZE));
            if (len >= 128) {
                int n = (int)sizeof(stored_name) - 1;
                memcpy(stored_name, buf + 96, n);
                stored_name[n] = '\0';
            }
            for (int i = 0; i < PUB_KEY_SIZE; i++) if (self_id.pub_key[i]) return true;
            return false;
        };

        if (!loaded) {
            // NVS backup survives LittleFS format events and missing partitions.
            Preferences idPrefs;
            if (idPrefs.begin("opsMesh", /*readOnly=*/true)) {
                size_t blen = idPrefs.getBytesLength("selfId");
                if (blen > 0 && blen <= 256) {
                    uint8_t buf[256];
                    if (idPrefs.getBytes("selfId", buf, blen) == blen) {
                        if (fsOk) {
                            File idFile = LittleFS.open("/mesh/self.id", "w", true);
                            if (idFile) { idFile.write(buf, blen); idFile.close(); }
                            loaded = store.load("self", self_id, stored_name, sizeof(stored_name));
                        }
                        if (!loaded) loaded = loadFromBytes(buf, blen);
                        if (loaded) OPS_LOG("Mesh", "Identity restored from NVS (%d bytes)", (int)blen);
                    }
                }
                idPrefs.end();
            }
        }

        // Encrypted SD backup (see IdentityBackup.h).
        if (!loaded && ops::sdcard::isMounted()) {
            uint8_t buf[ops::idbackup::PLAIN_LEN];
            uint8_t fileSalt[ops::crypto::SALT_LEN];
            ops::idbackup::Status st = ops::idbackup::read(buf, fileSalt);
            if (st == ops::idbackup::Status::Ok) {
                // Flash was wiped but the password still opens it: take the
                // backup's salt back, so repeater passwords sealed before the
                // wipe (in repeaters.json) decrypt again.
                if (memcmp(fileSalt, ops::crypto::salt(), sizeof(fileSalt)) != 0)
                    ops::crypto::adopt(nullptr, fileSalt);
                if (fsOk) {
                    File idFile = LittleFS.open("/mesh/self.id", "w", true);
                    if (idFile) { idFile.write(buf, sizeof(buf)); idFile.close(); }
                    loaded = store.load("self", self_id, stored_name, sizeof(stored_name));
                }
                if (!loaded) loaded = loadFromBytes(buf, sizeof(buf));
                if (loaded) OPS_LOG("Mesh", "Identity restored from encrypted SD backup");
            } else if (st == ops::idbackup::Status::Locked) {
                _identityLocked = true;
                OPS_LOG("Mesh", "SD identity backup is locked - storage password needed");
            } else if (st == ops::idbackup::Status::Corrupt) {
                // Keep the damaged file for inspection rather than letting a
                // new identity overwrite it.
                ops::idbackup::setAside();
            }
            memset(buf, 0, sizeof(buf));
        }

        // Plaintext backup written by older firmware: read once, then
        // _saveIdentityBackups() replaces it with the encrypted file.
        if (!loaded && !_identityLocked && ops::sdcard::isMounted()
                && ops::sdcard::hasFile(ops::idbackup::LEGACY_PATH)) {
            uint8_t buf[256];
            size_t  len = 0;
            if (ops::sdcard::readFile(ops::idbackup::LEGACY_PATH, buf, sizeof(buf), &len) && len > 0) {
                if (fsOk) {
                    File idFile = LittleFS.open("/mesh/self.id", "w", true);
                    if (idFile) { idFile.write(buf, len); idFile.close(); }
                    loaded = store.load("self", self_id, stored_name, sizeof(stored_name));
                }
                if (!loaded) loaded = loadFromBytes(buf, len);
                if (loaded) OPS_LOG("Mesh", "Identity restored from plaintext SD backup (%d bytes)", (int)len);
            }
        }

        if (!loaded) {
            OPS_LOG("Mesh", "Generating new identity for '%s'", _callsign);
            self_id = mesh::LocalIdentity(getRNG());
            for (int i = 0; i < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF); i++)
                self_id = mesh::LocalIdentity(getRNG());
            if (_identityLocked)
                OPS_LOG("Mesh", "Running on a temporary identity until the SD backup is unlocked");
            else if (fsOk)
                store.save("self", self_id, _callsign);
        } else if (stored_name[0] != '\0') {
            if (_callsign[0] != '\0' && strcmp(_callsign, stored_name) != 0) {
                if (fsOk) store.save("self", self_id, _callsign);
                OPS_LOG("Mesh", "Identity name: '%s' -> '%s' (cfg wins)", stored_name, _callsign);
            } else {
                strncpy(_callsign, stored_name, 31);
            }
        }

        _saveIdentityBackups();

        rtc_clock.begin();
        bootstrapRTCfromContacts();

        // Register channels: slot 0 uses the standard MeshCore public PSK;
        // slots 1-9 derive their PSK from the room name via SHA256 so any two
        // nodes with the same room name automatically share the channel.
        const auto& chCfg = ops::config::get();
        for (int i = 0; i < 10; i++) {
            if (i != 0 && !chCfg.channels[i].name[0]) continue;
            const char* chName = chCfg.channels[i].name[0]
                                  ? chCfg.channels[i].name : "Public";
            char psk64[28] = {};
            if (chCfg.channels[i].psk[0]) {
                strncpy(psk64, chCfg.channels[i].psk, sizeof(psk64) - 1);
            } else if (i == 0) {
                strncpy(psk64, PUBLIC_GROUP_PSK, sizeof(psk64) - 1);
            } else {
                MeshService::deriveChannelPsk(chName, psk64, sizeof(psk64));
            }
            _channels[i] = addChannel(chName, psk64);
            OPS_LOG("Mesh", "Channel %d '%s' psk=%s %s", i, chName, psk64,
                    _channels[i] ? "registered" : "FAILED");
        }
        preloadStoredContacts();
        OPS_LOG("Mesh", "Ready as '%s'", _callsign);
    }

    // ── Loading saved contacts/repeaters into MeshCore ────────────────
    // MeshCore's table holds MAX_CONTACTS entries (plus MAX_ANON_CONTACTS
    // transient slots), far fewer than we store, so it is filled by priority:
    // favourites, then chat contacts, then repeaters, each most recently heard
    // first. PRELOAD_HEADROOM slots stay free for newly heard nodes; beyond
    // that, shouldOverwriteWhenFull() evicts the least recently heard.
    //
    // Saved paths are loaded only when still trustworthy: set by hand, or
    // confirmed within PATH_MAX_AGE_S. Anything else starts unknown, so the
    // first message floods and MeshCore learns a fresh route from the reply —
    // rather than going direct down a days-old path and timing out.
    static constexpr int      PRELOAD_HEADROOM = 16;
    static constexpr uint32_t PATH_MAX_AGE_S   = 24UL * 3600UL;

    struct PreloadCand {
        uint32_t lastSeen;
        uint16_t idx;
        uint8_t  rank;      // 0 = favourite, 1 = contact, 2 = repeater
        bool     isRepeater;
    };

    // True until the clock is valid, when stored paths could not be age-checked
    // at boot; restoreDeferredPaths() applies them once GPS sets the time.
    bool _pathsDeferred = false;

    static bool _clockValid(uint32_t now) { return now >= 1700000000UL; }

    static bool _pathTrusted(uint32_t pathAt, uint32_t now) {
        if (pathAt == ops::PATH_AT_PINNED) return true;
        if (pathAt == 0 || now < 1700000000UL) return false;  // unknown age / clock unset
        return now >= pathAt && now - pathAt < PATH_MAX_AGE_S;
    }

    static bool _hasKey(const uint8_t* k) {
        for (int b = 0; b < 32; b++) if (k[b]) return true;
        return false;
    }

    void preloadStoredContacts() {
        const int nc = ops::contacts::count();
        const int nr = ops::repeaters::count();
        PreloadCand* cand = (PreloadCand*)ps_malloc((size_t)(nc + nr + 1) * sizeof(PreloadCand));
        if (!cand) { OPS_LOG("Mesh", "Preload: out of memory"); return; }

        int n = 0;
        for (int i = 0; i < nc; i++) {
            ops::Contact c;
            if (!ops::contacts::get(i, c) || !_hasKey(c.pubKey)) continue;
            cand[n++] = { c.lastSeen, (uint16_t)i, (uint8_t)(c.favourite ? 0 : 1), false };
        }
        for (int i = 0; i < nr; i++) {
            ops::Repeater r;
            if (!ops::repeaters::get(i, r) || !_hasKey(r.pubKey)) continue;
            cand[n++] = { r.lastSeen, (uint16_t)i, (uint8_t)(r.favourite ? 0 : 2), true };
        }
        std::sort(cand, cand + n, [](const PreloadCand& a, const PreloadCand& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.lastSeen > b.lastSeen;
        });

        const uint32_t now = getRTCClock()->getCurrentTime();
        const int limit = MAX_CONTACTS - PRELOAD_HEADROOM;
        int loaded = 0, withPath = 0;
        for (int k = 0; k < n && loaded < limit; k++) {
            ContactInfo ci{};
            uint32_t pathAt;
            if (cand[k].isRepeater) {
                ops::Repeater r;
                if (!ops::repeaters::get(cand[k].idx, r)) continue;
                if (lookupContactByPubKey(r.pubKey, 4)) continue;
                ci.id   = mesh::Identity(r.pubKey);
                strncpy(ci.name, r.name, 31);
                ci.type = ADV_TYPE_REPEATER;
                ci.out_path_len = r.outPathValid ? r.outPathLen : OUT_PATH_UNKNOWN;
                memcpy(ci.out_path, r.outPath, MAX_PATH_SIZE);
                pathAt = r.pathAt;
                if (r.favourite) ci.flags |= 0x01;
            } else {
                ops::Contact c;
                if (!ops::contacts::get(cand[k].idx, c)) continue;
                if (lookupContactByPubKey(c.pubKey, 4)) continue;
                ci.id   = mesh::Identity(c.pubKey);
                strncpy(ci.name, c.name, 31);
                // Must not be ADV_TYPE_NONE (0): addContact() puts type-0
                // entries in the 8 transient anon slots, each overwriting the
                // last — which previously left at most 8 contacts loaded.
                ci.type = ADV_TYPE_CHAT;
                ci.out_path_len = c.outPathValid ? c.outPathLen : OUT_PATH_UNKNOWN;
                memcpy(ci.out_path, c.outPath, MAX_PATH_SIZE);
                pathAt = c.pathAt;
                if (c.favourite) ci.flags |= 0x01;
            }
            ci.lastmod = cand[k].lastSeen;   // eviction order when the table fills
            if (ci.out_path_len != OUT_PATH_UNKNOWN && !_pathTrusted(pathAt, now)) {
                ci.out_path_len = OUT_PATH_UNKNOWN;
                memset(ci.out_path, 0, MAX_PATH_SIZE);
            }
            if (ci.out_path_len != OUT_PATH_UNKNOWN) withPath++;
            if (addContact(ci)) loaded++;
        }
        OPS_LOG("Mesh", "Preloaded %d of %d saved contacts/repeaters (%d with a trusted path)",
                loaded, n, withPath);
        free(cand);
        if (!_clockValid(now)) {
            _pathsDeferred = true;
            OPS_LOG("Mesh", "Clock not set — saved paths deferred until it is");
        }
    }

    // Applies saved paths that preload couldn't age-check (clock unset at boot)
    // to contacts whose path is still unknown. Called once the clock is valid.
    void restoreDeferredPaths() {
        if (!_pathsDeferred) return;
        const uint32_t now = getRTCClock()->getCurrentTime();
        if (!_clockValid(now)) return;
        _pathsDeferred = false;

        int restored = 0;
        auto apply = [&](const uint8_t* key, bool valid, uint8_t len,
                         const uint8_t* path, uint32_t pathAt) {
            if (!valid || len == OUT_PATH_UNKNOWN || !_pathTrusted(pathAt, now)) return;
            ContactInfo* ci = lookupContactByPubKey(key, PUB_KEY_SIZE);
            if (!ci || ci->out_path_len != OUT_PATH_UNKNOWN) return;  // learned since boot
            ci->out_path_len = len;
            memcpy(ci->out_path, path, MAX_PATH_SIZE);
            restored++;
        };
        for (int i = 0; i < ops::contacts::count(); i++) {
            ops::Contact c;
            if (ops::contacts::get(i, c)) apply(c.pubKey, c.outPathValid, c.outPathLen, c.outPath, c.pathAt);
        }
        for (int i = 0; i < ops::repeaters::count(); i++) {
            ops::Repeater r;
            if (ops::repeaters::get(i, r)) apply(r.pubKey, r.outPathValid, r.outPathLen, r.outPath, r.pathAt);
        }
        OPS_LOG("Mesh", "Clock set — restored %d saved paths", restored);
    }

    // MeshCore's table is full: evict the least recently heard non-favourite
    // instead of silently refusing new contacts (who then can't DM us).
    bool shouldOverwriteWhenFull() const override { return true; }

    void onContactOverwrite(const uint8_t* pub_key) override {
        OPS_LOG("Mesh", "Contact table full — evicted %02X%02X%02X%02X",
                pub_key[0], pub_key[1], pub_key[2], pub_key[3]);
    }

    void onContactsFull() override {
        OPS_LOG("Mesh", "Contact table full — new contact not added");
    }

    // Saves ci's current path with its confirmation time (PATH_AT_PINNED for a
    // hand-set path). Only confirmed paths are saved — see preloadStoredContacts().
    void _persistContactPath(const ContactInfo& ci, uint32_t learnedAt) {
        if (ci.out_path_len == OUT_PATH_UNKNOWN) return;
        // Without a real clock the time is meaningless — save as unknown age.
        if (learnedAt != ops::PATH_AT_PINNED && !_clockValid(learnedAt)) learnedAt = 0;
        int idx;
        if (ops::repeaters::findByKey(ci.id.pub_key, &idx))
            ops::repeaters::setPath(idx, ci.out_path_len, ci.out_path, learnedAt);
        if (ops::contacts::findByKey(ci.id.pub_key, &idx))
            ops::contacts::setPath(idx, ci.out_path_len, ci.out_path, learnedAt);
    }

    void _forgetStoredPath(const uint8_t* prefix4) {
        int idx;
        if (ops::contacts::findByKey(prefix4, &idx))  ops::contacts::clearPath(idx);
        if (ops::repeaters::findByKey(prefix4, &idx)) ops::repeaters::clearPath(idx);
    }

    void updateCallsign(const char* cs) {
        strncpy(_callsign, cs, 31);
        _callsign[31] = '\0';
        if (LittleFS.begin(false)) {
            IdentityStore store(LittleFS, "/mesh");
            store.begin();
            // A temporary identity (locked backup) is never persisted.
            if (!_identityLocked) store.save("self", self_id, _callsign);
        }
        _saveIdentityBackups();
        OPS_LOG("Mesh", "Callsign updated to '%s'", _callsign);
    }

    void regenerateIdentity() {
        // Choosing a new identity while the backup is locked gives up on it:
        // move it aside, or the new one would overwrite it on the next save.
        if (_identityLocked) {
            if (!ops::idbackup::setAside()) {
                OPS_LOG("Mesh", "regenerateIdentity: locked backup could not be moved aside");
                return;
            }
            _identityLocked = false;
        }
        self_id = mesh::LocalIdentity(getRNG());
        for (int i = 0; i < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF); i++)
            self_id = mesh::LocalIdentity(getRNG());
        if (LittleFS.begin(false)) {
            IdentityStore store(LittleFS, "/mesh");
            store.begin();
            store.save("self", self_id, _callsign);
        }
        _saveIdentityBackups();
        OPS_LOG("Mesh", "Identity regenerated");
    }

    void syncChannelSlot(int cfgSlot) {
        if (cfgSlot < 0 || cfgSlot > 9) return;
        const auto& cfg = config::get();
        const auto& ch  = cfg.channels[cfgSlot];
        const char* chName = ch.name[0] ? ch.name : (cfgSlot == 0 ? "Public" : nullptr);
        if (!chName) { _channels[cfgSlot] = nullptr; return; }
        char psk64[28] = {};
        if (ch.psk[0]) {
            strncpy(psk64, ch.psk, sizeof(psk64) - 1);
        } else if (cfgSlot == 0) {
            strncpy(psk64, PUBLIC_GROUP_PSK, sizeof(psk64) - 1);
        } else {
            MeshService::deriveChannelPsk(chName, psk64, sizeof(psk64));
        }
        if (_channels[cfgSlot]) {
            // Already registered — update name, secret and recompute hash in place
            strncpy(_channels[cfgSlot]->name, chName, 31);
            _channels[cfgSlot]->name[31] = '\0';
            memset(_channels[cfgSlot]->channel.secret, 0, sizeof(_channels[cfgSlot]->channel.secret));
            int len = _b64decode(psk64, _channels[cfgSlot]->channel.secret,
                                 (int)sizeof(_channels[cfgSlot]->channel.secret));
            if (len == 16 || len == 32) {
                mesh::Utils::sha256(_channels[cfgSlot]->channel.hash,
                                    sizeof(_channels[cfgSlot]->channel.hash),
                                    _channels[cfgSlot]->channel.secret, len);
                OPS_LOG("Mesh", "Channel %d synced in place: '%s' (len=%d)", cfgSlot, chName, len);
            } else {
                OPS_LOG("Mesh", "Channel %d sync: decode failed (len=%d psk=%s)", cfgSlot, len, psk64);
            }
        } else {
            // Not yet registered — add it now
            _channels[cfgSlot] = addChannel(chName, psk64);
            OPS_LOG("Mesh", "Channel %d registered live: %s", cfgSlot,
                    _channels[cfgSlot] ? "OK" : "FAILED");
        }
    }

    // Region discovery ────────────────────────────────────────────────
    // Spacing between requests: one request's airtime plus a little, so
    // they don't queue behind each other on slow presets.
    uint32_t _regStaggerMs() {
        uint32_t req = radio_driver.getEstAirtimeFor(REG_REQ_BYTES);
        return req + 150 > 400 ? req + 150 : 400;
    }

    bool _sendRegionReq(RegTarget& t, uint32_t delayMs) {
        if (t.tagCount >= 2) return false;
        ContactInfo* c = lookupContactByPubKey(t.pubKey, PUB_KEY_SIZE);
        if (!c) return false;
        uint8_t req[6];
        uint32_t tag = getRTCClock()->getCurrentTimeUnique();
        memcpy(req, &tag, 4);
        req[4] = ANON_REQ_REGIONS;
        req[5] = 0;   // reply path length: 0 = answer straight back, zero hop
        mesh::Packet* pkt = createAnonDatagram(PAYLOAD_TYPE_ANON_REQ, self_id, c->id,
                                               c->getSharedSecret(self_id), req, sizeof(req));
        if (!pkt) return false;
        sendDirect(pkt, c->out_path, 0, delayMs);
        t.tags[t.tagCount++] = tag;
        OPS_LOG("Regions", "Asked %s for its regions (tag %08X, try %d)", t.name, tag, t.tagCount);
        return true;
    }

    int discoverRegions() {
        if (!_active) return 0;
        _regTargetCount = 0;
        _regQHead = _regQCount = 0;
        ContactsIterator it = startContactsIterator();
        ContactInfo c;
        uint32_t stagger = _regStaggerMs();
        int asked = 0;
        // Ask for repeaters in range right now too: stored paths don't
        // survive a reboot, so a neighbour may not be marked 0-hop yet.
        _regScanUntil = 0;
        if (sendDiscoverReqMsg(1 << ADV_TYPE_REPEATER))
            _regScanUntil = millis() + regionScanMs();
        while (it.hasNext(this, c) && _regTargetCount < REG_MAX) {
            // Zero hops = heard directly, which is what the request needs:
            // repeaters only answer it when it arrives direct.
            if (c.type != ADV_TYPE_REPEATER || c.out_path_len != 0) continue;
            RegTarget& t = _regTargets[_regTargetCount];
            memset(&t, 0, sizeof(t));
            memcpy(t.pubKey, c.id.pub_key, PUB_KEY_SIZE);
            strncpy(t.name, c.name, sizeof(t.name) - 1);
            // Stagger so the replies don't all key up at once.
            if (!_sendRegionReq(t, (uint32_t)asked * stagger)) continue;
            _regTargetCount++;
            asked++;
        }
        return asked;
    }

    // A repeater answered the discover scan: ask it too, unless it already was.
    void _regAskNeighbour(const uint8_t* pubKey, const char* name) {
        if (_regTargetCount >= REG_MAX) return;
        for (int i = 0; i < _regTargetCount; i++)
            if (memcmp(_regTargets[i].pubKey, pubKey, PUB_KEY_SIZE) == 0) return;
        RegTarget& t = _regTargets[_regTargetCount];
        memset(&t, 0, sizeof(t));
        memcpy(t.pubKey, pubKey, PUB_KEY_SIZE);
        ops::Repeater saved;
        int ri;
        if (name && name[0]) strncpy(t.name, name, sizeof(t.name) - 1);
        else if (ops::repeaters::findByKey(pubKey, &ri) && ops::repeaters::get(ri, saved) && saved.name[0])
            strncpy(t.name, saved.name, sizeof(t.name) - 1);
        else snprintf(t.name, sizeof(t.name), "%02X%02X%02X%02X",
                      pubKey[0], pubKey[1], pubKey[2], pubKey[3]);
        if (_sendRegionReq(t, 0)) _regTargetCount++;
    }

    // How long repeaters take to answer a discover scan: they reply after a
    // random delay of up to ~10.4x the reply's airtime (getRetransmitDelay x 4).
    uint32_t regionScanMs() {
        return 1500 + 11 * radio_driver.getEstAirtimeFor(2 + 6 + 32 + 4);
    }

    int regionAskedCount() const { return _regTargetCount; }
    bool regionScanning() const  { return millis() < _regScanUntil; }

    // Asks every repeater that hasn't answered once more. Returns how many.
    int retryRegions() {
        if (!_active) return 0;
        uint32_t stagger = _regStaggerMs();
        int asked = 0;
        for (int i = 0; i < _regTargetCount; i++) {
            RegTarget& t = _regTargets[i];
            if (t.answered) continue;
            if (_sendRegionReq(t, (uint32_t)asked * stagger)) asked++;
        }
        return asked;
    }

    // How long a round of n requests needs before giving up on the replies:
    // the staggered sends, then each repeater's 300 ms turnaround and reply
    // airtime (replies can't overlap), plus a margin for CAD backoff.
    uint32_t regionWaitMs(int n) {
        if (n <= 0) return 0;
        uint32_t reqAir = radio_driver.getEstAirtimeFor(REG_REQ_BYTES);
        uint32_t repAir = radio_driver.getEstAirtimeFor(REG_REPLY_BYTES);
        return (uint32_t)n * _regStaggerMs() + reqAir
             + (uint32_t)n * (300 + repAir + 200) + 1500;
    }

    // Names of repeaters asked that haven't answered.
    int regionUnanswered(char names[][32], int max) {
        int n = 0;
        for (int i = 0; i < _regTargetCount && n < max; i++) {
            if (_regTargets[i].answered) continue;
            memcpy(names[n], _regTargets[i].name, 32);
            names[n][31] = '\0';
            n++;
        }
        return n;
    }

    bool pollRegionReply(RegionReply& out) {
        if (_regQCount == 0) return false;
        out = _regQ[_regQHead];
        _regQHead = (_regQHead + 1) % REG_MAX;
        _regQCount--;
        return true;
    }

    // Reply layout: our tag(4) + their clock(4) + comma-separated region names.
    bool _takeRegionReply(const ContactInfo& contact, const uint8_t* data, uint8_t len) {
        if (len < 8 || _regTargetCount == 0) return false;
        uint32_t tag;
        memcpy(&tag, data, 4);
        RegTarget* t = nullptr;
        for (int i = 0; i < _regTargetCount && !t; i++)
            for (int k = 0; k < _regTargets[i].tagCount; k++)
                if (_regTargets[i].tags[k] == tag) { t = &_regTargets[i]; break; }
        if (!t) return false;
        if (t->answered) return true;   // first try and retry both answered
        t->answered = true;
        // Asked and answered at zero hops: a confirmed direct path — save it.
        ContactInfo* ci = lookupContactByPubKey(t->pubKey, PUB_KEY_SIZE);
        if (ci && ci->out_path_len == 0)
            _persistContactPath(*ci, getRTCClock()->getCurrentTime());

        RegionReply r{};
        // A repeater found by the scan may have no name yet (no advert heard
        // since boot): try the saved repeater list, then the key prefix.
        const char* nm = contact.name;
        ops::Repeater saved;
        int ri;
        if (!nm[0] && ops::repeaters::findByKey(contact.id.pub_key, &ri) &&
            ops::repeaters::get(ri, saved) && saved.name[0]) {
            strncpy(t->name, saved.name, sizeof(t->name) - 1);
        }
        if (!nm[0]) nm = t->name;
        strncpy(r.repeater, nm, sizeof(r.repeater) - 1);
        r.snr = radio_driver.getLastSNR();   // this reply is the packet just received
        int n = len - 8;
        if (n > (int)sizeof(r.regions) - 1) n = sizeof(r.regions) - 1;
        memcpy(r.regions, data + 8, n);
        r.regions[n] = '\0';
        for (char* p = r.regions; *p; p++)   // keep only printable name characters
            if ((unsigned char)*p < ' ' || (unsigned char)*p > '~') *p = '?';

        // Not saved automatically: the Regions screen offers unsaved names
        // as "+ NAME" chips for the user to add.
        if (_regQCount == REG_MAX) { _regQHead = (_regQHead + 1) % REG_MAX; _regQCount--; }
        _regQ[(_regQHead + _regQCount) % REG_MAX] = r;
        _regQCount++;
        OPS_LOG("Regions", "%s floods for: %s", r.repeater, r.regions[0] ? r.regions : "(none)");
        return true;
    }

    // Transport key for a region scope, as repeaters derive it
    // (RegionMap::getTransportKeysFor): a plain name like "AU" is an implicit
    // hashtag region, keyed SHA-256("#AU"). A legacy "#AU" is accepted too.
    // Private "$" regions need a shared key and aren't supported.
    // Surrounding spaces are trimmed (a channel saved as "AU " still keys as
    // "#AU"); case is kept, since repeaters match it exactly.
    static bool _regionKey(const char* scope, TransportKey& tk) {
        if (!scope) return false;
        while (*scope == ' ') scope++;
        if (scope[0] == '#') scope++;
        size_t n = strlen(scope);
        while (n && scope[n - 1] == ' ') n--;
        if (n == 0 || scope[0] == '$') return false;
        char tag[34];
        snprintf(tag, sizeof(tag), "#%.*s", (int)n, scope);
        SHA256 sha;
        sha.update(tag, strlen(tag));
        sha.finalize(tk.key, sizeof(tk.key));
        return true;
    }

    // Floods pkt with the region code for `scope`, or unscoped when there is
    // none. The code covers the payload, so pkt must be complete.
    void _floodInScope(mesh::Packet* pkt, const char* scope, uint32_t delay_millis) {
        TransportKey tk;
        if (_regionKey(scope, tk)) {
            uint16_t codes[2] = { tk.calcTransportCode(pkt), 0 };
            sendFlood(pkt, codes, delay_millis);
        } else {
            sendFlood(pkt, delay_millis);
        }
    }

    // DMs, ACKs, path returns, logins, admin commands and telemetry all flood
    // through here (BaseChatMesh): use the default scope (Settings > Region
    // Scope), so they propagate on a mesh whose repeaters deny unscoped floods.
    void sendFloodScoped(const ContactInfo& /*recipient*/, mesh::Packet* pkt, uint32_t delay_millis = 0) override {
        _floodInScope(pkt, config::get().scopeTag, delay_millis);
    }

    void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis = 0) override {
        _noteOwnFlood(pkt);
        // Find the config slot that owns this channel object
        const auto& cfg = config::get();
        for (int i = 0; i < 10; i++) {
            if (!_channels[i]) continue;
            if (&_channels[i]->channel != &channel) continue;
            // Matched — apply scope if configured for this slot
            // The channel's own scope wins; otherwise the default scope.
            const char* scope = cfg.channels[i].scope;
            _floodInScope(pkt, (scope && scope[0]) ? scope : cfg.scopeTag, delay_millis);
            return;
        }
        _floodInScope(pkt, cfg.scopeTag, delay_millis);
    }

    bool sendChannelMsg(int chIdx, const char* text) {
        if (!_active) { OPS_LOG("Mesh", "TX blocked: radio inactive"); return false; }
        if (chIdx < 0 || chIdx > 9 || !_channels[chIdx]) {
            OPS_LOG("Mesh", "TX blocked: channel %d not registered", chIdx);
            return false;
        }
        uint32_t ts = getRTCClock()->getCurrentTime();
        int len = (int)strlen(text);
        if (len > MAX_TEXT_LEN) len = MAX_TEXT_LEN;
        bool ok = sendGroupMessage(ts, _channels[chIdx]->channel, _callsign, text, len);
        OPS_LOG("Mesh", "TX ch%d '%s' -> %s", chIdx, text, ok ? "queued" : "FAILED");
        return ok;
    }

    bool sendDirectMsg(const uint8_t* pubKeyPrefix4, const char* text) {
        if (!_active) { OPS_LOG("Mesh", "sendDirect: radio inactive"); return false; }
        ContactInfo* ci = lookupContactByPubKey(pubKeyPrefix4, 4);
        if (!ci) {
            OPS_LOG("Mesh", "sendDirect: %02X%02X%02X%02X not in routing table",
                    pubKeyPrefix4[0], pubKeyPrefix4[1], pubKeyPrefix4[2], pubKeyPrefix4[3]);
            return false;
        }
        if (strlen(text) > MAX_TEXT_LEN) {
            OPS_LOG("Mesh", "sendDirect: text too long (%u)", (unsigned)strlen(text));
            return false;
        }

        // Free slot, or reuse the one closest to giving up.
        int slot = -1;
        for (int s = 0; s < DM_SLOTS; s++) if (!_dms[s].active) { slot = s; break; }
        if (slot < 0) {
            slot = 0;
            for (int s = 1; s < DM_SLOTS; s++)
                if ((long)(_dms[s].deadline - _dms[slot].deadline) < 0) slot = s;
            OPS_LOG("Mesh", "DM retry table full — abandoning retries of id=%08X", _dms[slot].id);
            _failDm(_dms[slot]);
        }

        // One timestamp per message, unique even for identical texts sent in
        // the same second (otherwise the packets are identical and the mesh
        // drops the second as a repeat). All retries reuse it.
        uint32_t ts = getRTCClock()->getCurrentTime();
        if (ts <= _txtLastTs) ts = _txtLastTs + 1;
        _txtLastTs = ts;

        const auto& cfg = ops::config::get();
        PendingDm& d = _dms[slot];
        d = PendingDm{};
        d.active     = true;
        memcpy(d.key, ci->id.pub_key, 4);
        d.ts         = ts;
        d.directLeft = cfg.dmDirectRetries > 10 ? 10 : cfg.dmDirectRetries;
        d.floodLeft  = cfg.dmFloodRetries  > 5  ? 5  : cfg.dmFloodRetries;
        strncpy(d.text, text, sizeof(d.text) - 1);

        if (!_sendDmAttempt(d)) return false;
        _lastExpectedAck = d.id;
        return true;
    }

    bool sendSelfAdvert(int delay_ms, bool flood = true) {
        if (!_active) return false;
        const auto& cfg = config::get();
        mesh::Packet* pkt = nullptr;
        if (cfg.locationSharing) {
            if (Board::instance().hasGPSFix()) {
                double lat = Board::instance().gpsLat();
                double lon = Board::instance().gpsLng();
                pkt = createSelfAdvert(_callsign, lat, lon);
                OPS_LOG("Mesh", "Advert+GPS: %.5f, %.5f", lat, lon);
            } else if (cfg.manualLat != 0.0f || cfg.manualLon != 0.0f) {
                pkt = createSelfAdvert(_callsign, (double)cfg.manualLat, (double)cfg.manualLon);
                OPS_LOG("Mesh", "Advert+manual: %.5f, %.5f",
                        (double)cfg.manualLat, (double)cfg.manualLon);
            }
        }
        if (!pkt) pkt = createSelfAdvert(_callsign);
        if (!pkt) return false;
        if (flood) {
            _floodInScope(pkt, cfg.scopeTag, delay_ms);
        } else {
            sendZeroHop(pkt, delay_ms);
        }
        OPS_LOG("Mesh", "Advert sent (%s)", flood ? "flood" : "zero-hop");
        return true;
    }

    void preloadOne(const uint8_t* pubKey32, const char* name) {
        if (lookupContactByPubKey(pubKey32, 4)) return;
        ContactInfo ci{};
        ci.id = mesh::Identity(pubKey32);
        strncpy(ci.name, name, 31);
        ci.out_path_len = OUT_PATH_UNKNOWN;
        addContact(ci);
        OPS_LOG("Mesh", "Preloaded: %s", name);
    }

    bool setContactPathBytes(const uint8_t* prefix4, const uint8_t* pathBytes,
                             uint8_t numHops, uint8_t hashSz)
    {
        ContactInfo* ci = lookupContactByPubKey(prefix4, 4);
        if (!ci) return false;
        uint8_t byteCount = numHops * hashSz;
        if (byteCount > MAX_PATH_SIZE) return false;
        memcpy(ci->out_path, pathBytes, byteCount);
        ci->out_path_len = (uint8_t)(((hashSz - 1) << 6) | (numHops & 63));
        _persistContactPath(*ci, ops::PATH_AT_PINNED);   // hand-set: survives reboots
        OPS_LOG("Mesh", "setContactPath: %02X%02X hops=%d hashSz=%d",
                prefix4[0], prefix4[1], numHops, hashSz);
        return true;
    }

    bool dequeueRx(RxMessage& out) {
        if (_rxCount == 0) return false;
        out = _rxBuf[_rxHead];
        _rxHead = (_rxHead + 1) % RX_QUEUE_SIZE;
        _rxCount--;
        return true;
    }

    void enablePcapCapture(bool en) {
        _pcapCaptureEnabled = en;
        if (!en) { _pcapHead = _pcapTail = _pcapCount = 0; }
    }

    bool dequeuePcap(CapturedPacket& out) {
        if (_pcapCount == 0) return false;
        out = _pcapBuf[_pcapHead];
        _pcapHead = (_pcapHead + 1) % PCAP_QUEUE_SIZE;
        _pcapCount--;
        return true;
    }

    int      rxCount()      const { return _rxCount;   }
    int      numPeers()     const { return _peerCount;  }
    uint32_t numPeerSerial() const { return _peerSerial; }

    bool getPeerInfo(int idx, PeerInfo& out) const {
        if (idx < 0 || idx >= _peerCount) return false;
        out = _peers[idx];
        return true;
    }

    void clearPeerInfo() {
        _peerCount = 0;
        _peerSerial++;
    }

    RadioStats getStats() const {
        RadioStats s{};
        s.packetsSent      = radio_driver.getPacketsSent();
        s.packetsRecv      = radio_driver.getPacketsRecv();
        s.packetsRecvError = radio_driver.getPacketsRecvErrors();
        s.lastRssi         = radio_driver.getLastRSSI();
        s.lastSnr          = radio_driver.getLastSNR();
        s.noiseFloor       = (int16_t)radio_driver.getNoiseFloor();
        s.radioOk          = true;
        s.active           = _active;
        s.floodSent        = getNumSentFlood();
        s.floodRecv        = getNumRecvFlood();
        s.directSent       = getNumSentDirect();
        s.directRecv       = getNumRecvDirect();
        s.airtimeTxMs         = (uint32_t)getTotalAirTime();
        s.airtimeRxMs         = (uint32_t)getReceiveAirTime();
        s.loraDutyCycleActive = s_dcApplied;
        return s;
    }

    void setRadioActive(bool active) {
        _active = active;
        if (!active) radio_driver.powerOff();
    }

    bool isRadioActive() const { return _active; }
    void getSelfPubKeyPrefix(uint8_t out[4])  const { memcpy(out, self_id.pub_key, 4);  }
    void getSelfPubKey(uint8_t out[32])       const { memcpy(out, self_id.pub_key, 32); }
    void getSelfPrvKey(uint8_t out[64]) const {
        // writeTo(buf,96) layout: prv_key[64] then pub_key[32].
        // The method is not marked const in MeshCore but does not mutate self_id.
        uint8_t buf[96];
        auto& id = const_cast<mesh::LocalIdentity&>(self_id);
        if (id.writeTo(buf, sizeof(buf)) >= 64) memcpy(out, buf, 64);
        else memset(out, 0, 64);
    }

    bool getContactPathInfo(const uint8_t* prefix4, PathInfo& out) const {
        OMSMesh* self = const_cast<OMSMesh*>(this);
        ContactInfo* ci = self->lookupContactByPubKey(prefix4, 4);
        if (!ci) { out = {}; return false; }
        out.found  = true;
        out.known  = (ci->out_path_len != OUT_PATH_UNKNOWN);
        out.direct = out.known && (ci->out_path_len == 0);
        if (out.known && !out.direct) {
            out.hashSz   = (ci->out_path_len >> 6) + 1;
            out.hopCount = ci->out_path_len & 63;
        }
        return true;
    }

    // Also forget the saved copy — otherwise the old path came back on reboot.
    void resetContactPath(const uint8_t* prefix4) {
        ContactInfo* ci = lookupContactByPubKey(prefix4, 4);
        if (ci) ci->out_path_len = OUT_PATH_UNKNOWN;
        _forgetStoredPath(prefix4);
    }

    void resetAllContactPaths() {
        for (int i = 0; i < _peerCount; i++) {
            ContactInfo* ci = lookupContactByPubKey(_peers[i].pubKeyPrefix, 4);
            if (ci) ci->out_path_len = OUT_PATH_UNKNOWN;
            _forgetStoredPath(_peers[i].pubKeyPrefix);
        }
    }

    // Builds identity bytes from the in-memory self_id and writes to NVS + SD.
    // Does NOT require LittleFS — safe to call even when the spiffs partition is absent.
    // Stream format: pub[32] + prv[64] + name[32] = 128 bytes total.
    void _saveIdentityBackups() {
        // Sanity check: bail if pub_key is all-zeros (identity not loaded).
        bool valid = false;
        for (int i = 0; i < PUB_KEY_SIZE; i++) if (self_id.pub_key[i]) { valid = true; break; }
        if (!valid) { OPS_LOG("Mesh", "_saveIdentityBackups: identity is zeroed, skip"); return; }
        // A temporary identity must not overwrite anything, least of all the
        // locked backup it stands in for.
        if (_identityLocked) return;

        // writeTo(buf, 96) emits prv[64]+pub[32].  Extract prv from the front.
        uint8_t writeBuf[PRV_KEY_SIZE + PUB_KEY_SIZE];
        if (const_cast<mesh::LocalIdentity&>(self_id).writeTo(writeBuf, sizeof(writeBuf))
                < (int)(PRV_KEY_SIZE + PUB_KEY_SIZE)) {
            OPS_LOG("Mesh", "_saveIdentityBackups: writeTo failed");
            return;
        }

        // Assemble Stream format: pub[32] + prv[64] + name[32]
        uint8_t buf[128];
        memcpy(buf,                self_id.pub_key, PUB_KEY_SIZE);   // bytes 0-31
        memcpy(buf + PUB_KEY_SIZE, writeBuf,        PRV_KEY_SIZE);   // bytes 32-95
        memset(buf + 96, 0, 32);
        strncpy((char*)(buf + 96), _callsign, 31);

        // Also write to LittleFS if mounted (keeps it in sync for IdentityStore).
        if (LittleFS.begin(false)) {
            if (!LittleFS.exists("/mesh")) LittleFS.mkdir("/mesh");
            File f = LittleFS.open("/mesh/self.id", "w", true);
            if (f) { f.write(buf, sizeof(buf)); f.close(); }
        }

        Preferences idPrefs;
        if (idPrefs.begin("opsMesh", /*readOnly=*/false)) {
            idPrefs.putBytes("selfId", buf, sizeof(buf));
            idPrefs.end();
        }
        if (ops::sdcard::isMounted())
            ops::idbackup::write(buf);
        memset(writeBuf, 0, sizeof(writeBuf));
        OPS_LOG("Mesh", "Identity backups updated (%02X%02X%02X%02X...)",
                buf[0], buf[1], buf[2], buf[3]);
        memset(buf, 0, sizeof(buf));
    }

    bool identityLocked() const { return _identityLocked; }

    // Opens the locked SD backup with `pw`. On success the backup's password
    // and salt become this device's (so secrets sealed before the wipe open
    // again) and the identity is written to LittleFS + NVS, where the next
    // boot loads it — the caller restarts.
    bool unlockIdentity(const char* pw) {
        if (!_identityLocked) return false;
        uint8_t buf[ops::idbackup::PLAIN_LEN];
        uint8_t fileSalt[ops::crypto::SALT_LEN];
        if (!ops::idbackup::unlock(pw, buf, fileSalt)) return false;
        bool ok = ops::crypto::adopt(pw, fileSalt);
        if (ok) {
            if (LittleFS.begin(false)) {
                if (!LittleFS.exists("/mesh")) LittleFS.mkdir("/mesh");
                File f = LittleFS.open("/mesh/self.id", "w", true);
                if (f) { f.write(buf, sizeof(buf)); f.close(); }
            }
            Preferences idPrefs;
            if (idPrefs.begin("opsMesh", false)) {
                ok = idPrefs.putBytes("selfId", buf, sizeof(buf)) == sizeof(buf);
                idPrefs.end();
            }
        }
        memset(buf, 0, sizeof(buf));
        OPS_LOG("Mesh", "Identity unlock %s", ok ? "succeeded - restart to use it" : "FAILED to persist");
        return ok;
    }

    // Gives up on the locked backup: moves it aside and keeps the temporary
    // identity as this node's real one from now on.
    bool discardLockedIdentity() {
        if (!_identityLocked) return false;
        if (!ops::idbackup::setAside()) return false;
        _identityLocked = false;
        if (LittleFS.begin(false)) {
            IdentityStore store(LittleFS, "/mesh");
            store.begin();
            store.save("self", self_id, _callsign);
        }
        _saveIdentityBackups();
        return true;
    }

    // Re-seals the SD backup, e.g. after the storage password changed.
    void rewriteIdentityBackup() { _saveIdentityBackups(); }
};

static OMSMesh the_mesh;

// ── MeshService utilities ─────────────────────────────────────────
void MeshService::deriveChannelPsk(const char* name, char* psk_out, int psk_size)
{
    if (psk_size < 25) return;
    char hashInput[34];  // '#' + up to 31-char name + '\0'
    snprintf(hashInput, sizeof(hashInput), "#%s", name);
    uint8_t hash[16];
    mesh::Utils::sha256(hash, 16, (const uint8_t*)hashInput, (int)strlen(hashInput));
    static const char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int b = 0, j = 0; b < 15; b += 3, j += 4) {
        uint32_t v = ((uint32_t)hash[b] << 16)
                   | ((uint32_t)hash[b+1] << 8)
                   | hash[b+2];
        psk_out[j+0] = kB64[(v >> 18) & 63];
        psk_out[j+1] = kB64[(v >> 12) & 63];
        psk_out[j+2] = kB64[(v >>  6) & 63];
        psk_out[j+3] = kB64[v & 63];
    }
    uint32_t v = (uint32_t)hash[15] << 16;
    psk_out[20] = kB64[(v >> 18) & 63];
    psk_out[21] = kB64[(v >> 12) & 63];
    psk_out[22] = '='; psk_out[23] = '='; psk_out[24] = '\0';
}

void MeshService::syncChannel(int chIdx) {
    if (_initialized) the_mesh.syncChannelSlot(chIdx);
}

void MeshService::normalizePsk(const char* in, char* out, int outSize)
{
    if (!in || !out || outSize < 25) return;
    // Detect 32-char hex string (share-link format = 16 raw bytes in hex)
    int inLen = (int)strlen(in);
    if (inLen == 32) {
        bool isHex = true;
        for (int i = 0; i < 32 && isHex; i++) {
            char c = in[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                isHex = false;
        }
        if (isHex) {
            auto nibble = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
                if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
                return (uint8_t)(c - 'A' + 10);
            };
            uint8_t bytes[16];
            for (int i = 0; i < 16; i++)
                bytes[i] = (uint8_t)((nibble(in[i*2]) << 4) | nibble(in[i*2+1]));
            static const char kB64[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (int b = 0, j = 0; b < 15; b += 3, j += 4) {
                uint32_t v = ((uint32_t)bytes[b] << 16)
                           | ((uint32_t)bytes[b+1] << 8)
                           | bytes[b+2];
                out[j+0] = kB64[(v >> 18) & 63];
                out[j+1] = kB64[(v >> 12) & 63];
                out[j+2] = kB64[(v >>  6) & 63];
                out[j+3] = kB64[v & 63];
            }
            uint32_t v = (uint32_t)bytes[15] << 16;
            out[20] = kB64[(v >> 18) & 63];
            out[21] = kB64[(v >> 12) & 63];
            out[22] = '='; out[23] = '='; out[24] = '\0';
            return;
        }
    }
    strncpy(out, in, outSize - 1);
    out[outSize - 1] = '\0';
}

// ── FHSS state ──────────────────────────────────────────────────────
// See src/mesh/Fhss.h and docs/FHSS.md. The hop channel is a pure function
// of (networkKey, frameNumber, region); nothing here is negotiated over the
// air, so every node must compute it identically or the mesh partitions.
static const ops::fhss::Region* s_fhssRegion   = nullptr;
static uint8_t  s_fhssKey[32]  = {};
static char     s_fhssKeyPsk[28] = {};   // PSK the cached key was derived from
static bool     s_fhssKeyReady = false;
static bool     s_fhssActive   = false;   // true while we are actually hopping
static uint8_t  s_fhssChannel  = 0xFF;    // 0xFF = not on a hop channel
static uint32_t s_fhssFrame    = 0;
static uint32_t s_fhssHopCount = 0;
static bool     s_fhssTxClamped    = false;  // TX power reduced for band limit
static bool     s_fhssNoPlanWarned = false;
static bool     s_fhssNoClockWarned = false;

// The fixed (non-hopping) frequency this node uses outside FHSS mode —
// same resolution order as MeshService::getFreqMHz().
static float _fixedFreqMHz()
{
    const auto& cfg = ops::config::get();
    if (cfg.radioCustom && cfg.freqMHz > 0.0f) return cfg.freqMHz;
    static const float kFreqs[14] = {
        915.800f, 916.575f, 869.618f, 869.525f, 869.525f,
        869.525f, 433.650f, 917.375f, 917.375f, 433.375f,
        869.618f, 869.618f, 910.525f, 920.250f,
    };
    uint8_t p = cfg.radioProfile;
    return kFreqs[p < 14 ? p : 2];
}

// Hop-sequence key = SHA-256(public channel PSK). Mirrors the slot-0 PSK
// resolution in syncChannelSlot() so the key follows the channel the user
// actually configured — nodes on the same public channel hop together, and
// two meshes with different PSKs hop independently with no extra setup.
// Resolve the effective public-channel PSK — same order as syncChannelSlot().
static void _fhssEffectivePsk(char* out, size_t outSize)
{
    const auto& cfg = ops::config::get();
    if (cfg.channels[0].psk[0]) {
        strncpy(out, cfg.channels[0].psk, outSize - 1);
    } else {
        strncpy(out, PUBLIC_GROUP_PSK, outSize - 1);
    }
    out[outSize - 1] = '\0';
}

// Rebuilds s_fhssKey if the effective PSK has changed since last time.
// Editing the public channel must change the hop sequence — a cached key
// would leave this node hopping on the old mesh's sequence, silently.
static bool _fhssSyncKey()
{
    char psk64[28] = {};
    _fhssEffectivePsk(psk64, sizeof(psk64));
    if (s_fhssKeyReady && strcmp(psk64, s_fhssKeyPsk) == 0) return false;
    ops::fhss::deriveNetworkKey(psk64, s_fhssKey);
    strncpy(s_fhssKeyPsk, psk64, sizeof(s_fhssKeyPsk) - 1);
    s_fhssKeyPsk[sizeof(s_fhssKeyPsk) - 1] = '\0';
    s_fhssKeyReady = true;
    return true;   // key changed — caller must force a retune
}

// The TX power the user configured, before any FHSS clamp.
static int8_t _configuredTxDbm()
{
    const auto& cfg = ops::config::get();
    if (cfg.radioCustom && cfg.radioTX != 0) return cfg.radioTX;
    return 22;   // SX1262 default used when no override is set
}

// Hop plans can land in a sub-band with a lower legal power limit than the
// node's normal fixed frequency. EU is the sharp case: the EU profiles run at
// 869.525/869.618 (869.4-869.65 = 500 mW / 10 % duty), while the EU868 hop
// plan sits at 868.1-868.5 (868.0-868.6 = 25 mW / 1 % duty). Carrying 22 dBm
// across that boundary would transmit ~8 dB over the limit, so clamp to the
// region's maxTxPowerDbm for as long as we are hopping.
static void _fhssApplyTxLimit(const ops::fhss::Region* region)
{
    int8_t want = _configuredTxDbm();
    int8_t lim  = region->maxTxPowerDbm;
    if (want > lim) {
        sx1262.setOutputPower(lim);
        if (!s_fhssTxClamped) {
            OPS_LOG("Mesh", "FHSS: TX clamped %d -> %d dBm for %s band limit",
                    want, lim, region->name);
            s_fhssTxClamped = true;
        }
    }
}

static void _fhssRestoreFixedFreq(const char* why)
{
    s_fhssActive  = false;
    s_fhssChannel = 0xFF;
    if (s_fhssTxClamped) {
        sx1262.setOutputPower(_configuredTxDbm());
        s_fhssTxClamped = false;
        OPS_LOG("Mesh", "FHSS: TX restored to %d dBm", _configuredTxDbm());
    }
    sx1262.setFrequency(_fixedFreqMHz());
    sx1262.startReceive();
    OPS_LOG("Mesh", "FHSS off (%s) — fixed %.3f MHz", why, (double)_fixedFreqMHz());
}

static void _tickFhss()
{
    const auto& cfg = ops::config::get();

    if (cfg.radioPowerMode != RADIO_POWER_FHSS) {
        if (s_fhssActive) _fhssRestoreFixedFreq("mode changed");
        return;
    }

    // FHSS is not offered on every profile: the 433 MHz bands have no hop
    // plan, and the EU 868 profiles decline the one that exists (see the
    // rationale on fhss::availability()). Either way — stay on fixed.
    const ops::fhss::Region* region = ops::fhss::regionForProfile(cfg.radioProfile);
    if (!region) {
        if (s_fhssActive) _fhssRestoreFixedFreq("not offered on this profile");
        if (!s_fhssNoPlanWarned) {
            OPS_LOG("Mesh", "FHSS unavailable on profile %u (%s)", cfg.radioProfile,
                    ops::fhss::availability(cfg.radioProfile) ==
                        ops::fhss::Unavailable::EuSubBandTradeoff
                        ? "EU 868 — sub-band/power trade not worth it"
                        : "no hop plan for this band");
            s_fhssNoPlanWarned = true;
        }
        return;
    }

    // The frame number comes from UTC (see the DEVIATION note in Fhss.h), so
    // an unset clock means we cannot know which channel the rest of the mesh
    // is on. Hopping on a guessed frame number is strictly worse than not
    // hopping: it puts us on a channel nobody is listening to, silently.
    uint32_t utc = (uint32_t)rtc_clock.getCurrentTime();
    if (!ops::fhss::clockIsValid(utc)) {
        if (s_fhssActive) _fhssRestoreFixedFreq("clock lost");
        if (!s_fhssNoClockWarned) {
            OPS_LOG("Mesh", "FHSS waiting for clock (GPS/RTC not set yet)");
            s_fhssNoClockWarned = true;
        }
        return;
    }
    s_fhssNoClockWarned = false;

    s_fhssNoPlanWarned = false;

    // A changed PSK or a changed region means the sequence we were following
    // is no longer the right one — force a retune rather than waiting for the
    // next natural channel change.
    if (_fhssSyncKey() || region != s_fhssRegion) {
        s_fhssRegion  = region;
        s_fhssChannel = 0xFF;
    }

    uint32_t frame = ops::fhss::frameNumberForUtc(utc);
    if (s_fhssActive && frame == s_fhssFrame) return;   // still inside this frame

    uint8_t ch = ops::fhss::hopSequence(s_fhssKey, frame, region);
    if (s_fhssActive && ch == s_fhssChannel) {
        s_fhssFrame = frame;    // new frame, same channel — nothing to retune
        return;
    }

    // Never retune underneath an in-flight transmission.
    if (digitalRead(P_LORA_BUSY) == HIGH) return;   // retry on the next tick

    float f = ops::fhss::channelToFreqMHz(ch, region);
    if (f <= 0.0f) return;

    // Clamp power before the first hop puts us in the new sub-band, not after.
    _fhssApplyTxLimit(region);

    sx1262.setFrequency(f);
    sx1262.startReceive();
    s_fhssFrame   = frame;
    s_fhssChannel = ch;
    s_fhssHopCount++;
    if (!s_fhssActive) {
        s_fhssActive = true;
        OPS_LOG("Mesh", "FHSS engaged: %s, %u ch, %.3f-%.3f MHz, frame %u",
                region->name, region->numChannels,
                (double)ops::fhss::channelToFreqMHz(0, region),
                (double)ops::fhss::channelToFreqMHz((uint8_t)(region->numChannels - 1), region),
                frame);
    }
}

static void _tickDutyCycle() {
    const auto& cfg = config::get();

    // Detect received packets — MeshCore calls startReceive() after each one,
    // which exits duty cycle mode on the hardware side.
    uint32_t recvNow = radio_driver.getPacketsRecv();
    uint32_t sentNow = radio_driver.getPacketsSent();

    if (recvNow != s_dcLastRecvCount) {
        s_dcLastRecvCount = recvNow;
        s_dcLastPacketMs  = millis();
        s_dcApplied       = false;  // MeshCore called startReceive(); hardware back to continuous RX
    }
    if (sentNow != s_dcLastSentCount) {
        s_dcLastSentCount = sentNow;
        s_dcLastPacketMs  = millis();  // reset idle timer so we don't re-arm immediately after TX
        s_dcApplied       = false;     // TX finished; MeshCore will call startReceive() shortly
    }

    // Duty cycle and FHSS are mutually exclusive strategies — the hardware
    // RX duty cycle parks the radio asleep on one frequency, which is exactly
    // what a hopping node must not do.
    if (cfg.radioPowerMode != RADIO_POWER_DUTY_CYCLE || s_dcSuspended) {
        if (s_dcApplied) {
            sx1262.startReceive();
            s_dcApplied = false;
            OPS_LOG("Mesh", "LoRa duty cycle disarmed");
        }
        return;
    }

    // Arm after DC_ARM_MS of no TX/RX activity.
    if (!s_dcApplied && radio_driver.isInRecvMode()) {
        if (millis() - s_dcLastPacketMs >= DC_ARM_MS) {
            sx1262.startReceiveDutyCycle(DC_RX_US, DC_SLP_US);
            s_dcApplied = true;
            OPS_LOG("Mesh", "LoRa duty cycle armed (250ms/250ms)");
        }
    }
}

// ── MeshService singleton ──────────────────────────────────────────
static MeshService s_instance;

MeshService& MeshService::instance() { return s_instance; }

void MeshService::init() {
    OPS_LOG("Mesh", "Initialising MeshCore stack");

    // SPI bus shared with TFT (same physical pins SCK=40 MISO=38 MOSI=41,
    // separate CS lines: TFT=12, LoRa=9)
    lora_spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);

    if (!sx1262.std_init(&lora_spi)) {
        OPS_LOG("Mesh", "SX1262 init failed — mesh disabled");
        return;
    }
    OPS_LOG("Mesh", "SX1262 OK %.1f MHz SF%d BW%d", (double)LORA_FREQ, LORA_SF, LORA_BW);

    const auto& cfg = ops::config::get();
    the_mesh.begin_mesh(cfg.callsign[0] ? cfg.callsign : "OMS-NODE");

    applyLoraProfile(cfg.radioProfile);
    applyRadioOverrides();
    // Config is the source of truth: std_init() switched boost on from the
    // build flag, so apply the saved choice explicitly in both directions.
    sx1262.setRxBoostedGainMode(cfg.rxBoost);
    the_mesh.sendSelfAdvert(random(500, 2500), false);  // zero-hop on boot

    _initialized = true;
    OPS_LOG("Mesh", "MeshCore ready");
}

void MeshService::tick() {
    if (!_initialized) return;
    if (s_sigGenActive) return;  // mesh suspended while signal generator is active
    the_mesh.loop();
    the_mesh.serviceDmRetries();
    the_mesh.serviceOwnFloods();
    the_mesh.checkSerialInterface();
    the_mesh.restoreDeferredPaths();   // no-op unless deferred at boot
    _tickFhss();
    _tickDutyCycle();
}

void MeshService::suspendDutyCycle(bool suspend)
{
    s_dcSuspended = suspend;
}

bool MeshService::sendChannel(int chIdx, const char* text) {
    return _initialized && the_mesh.sendChannelMsg(chIdx, text);
}

bool MeshService::sendDirect(const uint8_t* pubKeyPrefix4, const char* text) {
    return _initialized && the_mesh.sendDirectMsg(pubKeyPrefix4, text);
}

bool MeshService::sendAdvert(int delayMs, bool flood) {
    return _initialized && the_mesh.sendSelfAdvert(delayMs, flood);
}

bool MeshService::dequeueMessage(RxMessage& out) {
    return _initialized && the_mesh.dequeueRx(out);
}

int MeshService::messageCount() const {
    return _initialized ? the_mesh.rxCount() : 0;
}

void MeshService::setPcapCapture(bool enable) {
    if (_initialized) the_mesh.enablePcapCapture(enable);
}

bool MeshService::dequeueCapturedPacket(CapturedPacket& out) {
    return _initialized && the_mesh.dequeuePcap(out);
}

int MeshService::peerCount() const {
    return _initialized ? the_mesh.numPeers() : 0;
}

uint32_t MeshService::peerSerial() const {
    return _initialized ? the_mesh.numPeerSerial() : 0;
}

void MeshService::clearPeers() {
    if (_initialized) the_mesh.clearPeerInfo();
}

bool MeshService::getPeer(int idx, PeerInfo& out) const {
    return _initialized && the_mesh.getPeerInfo(idx, out);
}

bool MeshService::findPeerByKey(const uint8_t* prefix4, PeerInfo& out) const {
    if (!_initialized || !prefix4) return false;
    int n = peerCount();
    for (int i = 0; i < n; i++)
        if (the_mesh.getPeerInfo(i, out) && memcmp(out.pubKeyPrefix, prefix4, 4) == 0)
            return true;
    return false;
}

bool MeshService::pollAck(uint32_t& acked_crc) {
    return _initialized && the_mesh.pollAck(acked_crc);
}

bool MeshService::pollDmFailed(uint32_t& id) {
    return _initialized && the_mesh.pollDmFailed(id);
}

int MeshService::discoverRegions() {
    return _initialized ? the_mesh.discoverRegions() : 0;
}

uint32_t MeshService::regionScanMs() {
    return _initialized ? the_mesh.regionScanMs() : 0;
}

int MeshService::regionAskedCount() {
    return _initialized ? the_mesh.regionAskedCount() : 0;
}

bool MeshService::regionScanning() {
    return _initialized && the_mesh.regionScanning();
}

int MeshService::retryRegions() {
    return _initialized ? the_mesh.retryRegions() : 0;
}

uint32_t MeshService::regionWaitMs(int n) {
    return _initialized ? the_mesh.regionWaitMs(n) : 0;
}

int MeshService::regionUnanswered(char names[][32], int max) {
    return _initialized ? the_mesh.regionUnanswered(names, max) : 0;
}

bool MeshService::pollRegionReply(RegionReply& out) {
    return _initialized && the_mesh.pollRegionReply(out);
}

bool MeshService::identityLocked() const {
    return _initialized && the_mesh.identityLocked();
}

bool MeshService::unlockIdentity(const char* pw) {
    return _initialized && the_mesh.unlockIdentity(pw);
}

bool MeshService::discardLockedIdentity() {
    return _initialized && the_mesh.discardLockedIdentity();
}

void MeshService::rewriteIdentityBackup() {
    if (_initialized) the_mesh.rewriteIdentityBackup();
}

bool MeshService::sendTrace(const uint8_t* pubKeyPrefix4, uint32_t& out_tag, int& out_hops) {
    return _initialized && the_mesh.sendTraceToContact(pubKeyPrefix4, out_tag, out_hops);
}

bool MeshService::pollTraceResult(TraceResult& out) {
    return _initialized && the_mesh.pollTraceResult(out);
}

bool MeshService::hasPathTo(const uint8_t* pubKeyPrefix4) const {
    return _initialized && the_mesh.hasPathToContact(pubKeyPrefix4);
}

uint32_t MeshService::lastChannelFloodId() const {
    return _initialized ? the_mesh.lastChannelFloodId() : 0;
}

bool MeshService::pollFloodRepeat(uint32_t& id, uint8_t& count) {
    return _initialized && the_mesh.pollFloodRepeat(id, count);
}

uint32_t MeshService::lastExpectedAck() const {
    return _initialized ? the_mesh.lastExpectedAck() : 0;
}

bool MeshService::sendRepeatersStatus(int timeoutSecs) {
    return _initialized && the_mesh.sendRepeatersStatusReq(timeoutSecs);
}

bool MeshService::sendRepeaterLogin(const uint8_t* prefix4, const char* password) {
    return _initialized && the_mesh.sendRepeaterLoginReq(prefix4, password);
}

bool MeshService::sendAdminCommand(const uint8_t* prefix4, const char* command) {
    return _initialized && the_mesh.sendAdminCommandTo(prefix4, command);
}

bool MeshService::sendRepeaterStatusReq(const uint8_t* prefix4) {
    return _initialized && the_mesh.sendSingleRepeaterStatus(prefix4);
}

bool MeshService::sendRepeaterNeighboursReq(const uint8_t* prefix4) {
    return _initialized && the_mesh.sendSingleRepeaterNeighbours(prefix4);
}

bool MeshService::pollContactResponse(char* out, int outMax) {
    return _initialized && the_mesh.dequeueResp(out, outMax);
}

bool MeshService::pollLoginResult(bool& ok) {
    return _initialized && the_mesh.dequeueLoginResult(ok);
}

FhssStatus MeshService::fhssStatus() const {
    const auto& cfg = ops::config::get();
    FhssStatus s{};
    s.enabled       = (cfg.radioPowerMode == RADIO_POWER_FHSS);
    s.hopping       = s_fhssActive;
    s.clockValid    = _initialized &&
                      ops::fhss::clockIsValid((uint32_t)rtc_clock.getCurrentTime());
    const ops::fhss::Region* r = ops::fhss::regionForProfile(cfg.radioProfile);
    s.planAvailable = (r != nullptr);
    s.regionName    = r ? r->name : "";
    s.numChannels   = r ? r->numChannels : 0;
    s.channel       = s_fhssChannel;
    // Report the fixed frequency here even while hopping: the dialog uses it
    // to tell the user which frequency FHSS would move them *off* of.
    s.freqMHz       = _fixedFreqMHz();
    s.hopLoMHz      = r ? ops::fhss::channelToFreqMHz(0, r) : 0.0f;
    s.hopHiMHz      = r ? ops::fhss::channelToFreqMHz((uint8_t)(r->numChannels - 1), r) : 0.0f;
    s.maxTxDbm      = r ? r->maxTxPowerDbm : 0;
    s.txClamped     = s_fhssTxClamped;
    s.frameNumber   = s_fhssFrame;
    s.hopCount      = s_fhssHopCount;
    return s;
}

MeshService::RadioDiag MeshService::radioDiag() const {
    RadioDiag d{ -1, 0, 0.0f, 0 };
    if (!_initialized) return d;
    uint8_t reg = 0;
    if (sx1262.readRegister(RADIOLIB_SX126X_REG_RX_GAIN, &reg, 1) == RADIOLIB_ERR_NONE)
        d.rxGainReg = reg;
    // RadioLib's cached modem settings (public under RADIOLIB_GODMODE).
    d.sf    = sx1262.spreadingFactor;
    d.bwKhz = sx1262.bandwidthKhz;
    d.cr    = sx1262.codingRate + 4;   // stored as cr - 4 (short interleave)
    return d;
}

RadioStats MeshService::radioStats() const {
    if (!_initialized) return RadioStats{};
    return the_mesh.getStats();
}

void MeshService::setActive(bool active) {
    if (_initialized) the_mesh.setRadioActive(active);
}

bool MeshService::isActive() const {
    return _initialized && the_mesh.isRadioActive();
}

void MeshService::getSelfPubKeyPrefix(uint8_t out[4]) const {
    if (_initialized) { the_mesh.getSelfPubKeyPrefix(out); return; }
    memset(out, 0, 4);
}

void MeshService::getSelfPubKey(uint8_t out[32]) const {
    if (_initialized) { the_mesh.getSelfPubKey(out); return; }
    memset(out, 0, 32);
}

void MeshService::getSelfPrvKey(uint8_t out[64]) const {
    if (_initialized) { the_mesh.getSelfPrvKey(out); return; }
    memset(out, 0, 64);
}

bool MeshService::getContactPath(const uint8_t* prefix4, PathInfo& out) const {
    if (!_initialized) { out = {}; return false; }
    return the_mesh.getContactPathInfo(prefix4, out);
}

void MeshService::resetContactPath(const uint8_t* prefix4) {
    if (_initialized) the_mesh.resetContactPath(prefix4);
}

void MeshService::resetAllContactPaths() {
    if (_initialized) the_mesh.resetAllContactPaths();
}

bool MeshService::lightSleep(uint32_t maxMs) {
    if (_initialized) {
        // Only sleep while quietly listening. Mid-TX, MeshCore waits for the
        // TX-done edge on DIO1 (which sleep would swallow); queued packets —
        // ACKs, relays, delayed RX processing — must not wait on the timer.
        if (!radio_driver.isInRecvMode() || isTxBusy()) return false;
        int outTotal = pkt_mgr.getOutboundTotal();
        int inbound  = PKT_POOL_SIZE - pkt_mgr.getFreeCount() - outTotal;
        if (outTotal > 0 || inbound > 0) return false;
    }

    // Cap each sleep at half the airtime of the shortest packet on the current
    // profile: two packets can't both complete within one slice, so the
    // SX1262's single RX buffer is always read before it can be overwritten —
    // even if the DIO1 wake below doesn't fire on this hardware.
    if (_initialized) {
        uint32_t slice = radio_driver.getEstAirtimeFor(16) / 2;
        if (slice < 20)  slice = 20;
        if (slice > 250) slice = 250;
        if (maxMs > slice) maxMs = slice;
    }

    const gpio_num_t dio1 = (gpio_num_t)P_LORA_DIO_1;
    if (_initialized) {
        // Level wake with the CPU interrupt masked: wakes on a received packet
        // without a level-triggered ISR storm.
        gpio_intr_disable(dio1);
        gpio_wakeup_enable(dio1, GPIO_INTR_HIGH_LEVEL);
    }
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup((uint64_t)maxMs * 1000ULL);
    esp_light_sleep_start();

    s_sleepStats.sleeps++;
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_GPIO:
            if (_initialized && digitalRead(P_LORA_DIO_1) == HIGH) s_sleepStats.wakeRadio++;
            else                                                 s_sleepStats.wakeOther++;
            break;
        case ESP_SLEEP_WAKEUP_TIMER: s_sleepStats.wakeTimer++; break;
        default:                     s_sleepStats.wakeOther++; break;
    }

    if (_initialized) {
        gpio_wakeup_disable(dio1);
        gpio_set_intr_type(dio1, GPIO_INTR_POSEDGE);
        // Drop any status latched by the level type so the ISR doesn't fire for
        // this event too — recvRaw() picks it up via s_rxMissedInSleep instead.
        REG_WRITE(GPIO_STATUS1_W1TC_REG, 1UL << (P_LORA_DIO_1 - 32));
        // While DIO1 stays high no new rising edge can occur, so there is no
        // race with the ISR: the next edge only follows our read clearing it.
        if (digitalRead(P_LORA_DIO_1) == HIGH) s_rxMissedInSleep = true;
        gpio_intr_enable(dio1);
    }
    return true;
}

MeshService::SleepStats MeshService::sleepStats() const {
    return s_sleepStats;
}

bool MeshService::isTxBusy() const {
    if (!_initialized) return false;
    return digitalRead(P_LORA_BUSY) == HIGH;
}

float MeshService::getFreqMHz() const {
    // While FHSS is hopping, the radio is genuinely on the current hop
    // channel — report that rather than the configured fixed frequency, so
    // the Signal/PCAP/scan screens show where we are actually listening.
    if (s_fhssActive && s_fhssRegion) {
        float f = ops::fhss::channelToFreqMHz(s_fhssChannel, s_fhssRegion);
        if (f > 0.0f) return f;
    }
    return _fixedFreqMHz();
}

void MeshService::setFreqMHz(float mhz) {
    if (_initialized) sx1262.setFrequency(mhz);
}

void MeshService::setSpreadingFactor(uint8_t sf) {
    if (_initialized) sx1262.setSpreadingFactor(sf);
}

void MeshService::setBandwidth(float bw_khz) {
    if (_initialized) sx1262.setBandwidth(bw_khz);
}

void MeshService::setCodingRate(uint8_t cr) {
    if (_initialized) sx1262.setCodingRate(cr);
}

void MeshService::setTxPower(int8_t dbm) {
    if (_initialized) sx1262.setOutputPower(dbm);
}

void MeshService::setRxBoost(bool boost) {
    if (_initialized) sx1262.setRxBoostedGainMode(boost);
}

void MeshService::applyRadioOverrides() {
    if (!_initialized) return;
    const auto& cfg = ops::config::get();
    if (!cfg.radioCustom) return;
    if (cfg.freqMHz > 0.0f)                        sx1262.setFrequency(cfg.freqMHz);
    if (cfg.radioSF >= 7 && cfg.radioSF <= 12)     sx1262.setSpreadingFactor(cfg.radioSF);
    static const float kBW[] = { 0.0f, 62.5f, 125.0f, 250.0f };
    if (cfg.radioBW >= 1 && cfg.radioBW <= 3)      sx1262.setBandwidth(kBW[cfg.radioBW]);
    if (cfg.radioCR >= 5 && cfg.radioCR <= 8)      sx1262.setCodingRate(cfg.radioCR);
    if (cfg.radioTX != 0)                           sx1262.setOutputPower(cfg.radioTX);
    OPS_LOG("Mesh", "Radio overrides applied");
}

void MeshService::setCallsign(const char* cs) {
    if (_initialized && cs && cs[0]) the_mesh.updateCallsign(cs);
}

void MeshService::regenerateIdentity() {
    if (_initialized) the_mesh.regenerateIdentity();
}

void MeshService::preloadContact(const uint8_t* pubKey32, const char* name) {
    if (_initialized) the_mesh.preloadOne(pubKey32, name);
}

bool MeshService::setContactPath(const uint8_t* prefix4, const uint8_t* pathBytes,
                                  uint8_t numHops, uint8_t hashSz) {
    return _initialized && the_mesh.setContactPathBytes(prefix4, pathBytes, numHops, hashSz);
}

void MeshService::applyLoraProfile(uint8_t profile) {
    struct ProfileEntry { float freq; float bw; uint8_t sf; uint8_t cr; };
    static const ProfileEntry kProfiles[14] = {
        { 915.800f, 250.0f, 10, 5 },  // 0  Australia
        { 916.575f,  62.5f,  7, 8 },  // 1  Australia Victoria
        { 869.618f,  62.5f,  8, 8 },  // 2  EU/UK Narrow (recommended)
        { 869.525f, 250.0f, 11, 5 },  // 3  EU/UK Long Range
        { 869.525f, 250.0f, 10, 5 },  // 4  EU/UK Medium Range
        { 869.525f,  62.5f,  7, 5 },  // 5  Czech Republic Narrow
        { 433.650f, 250.0f, 11, 5 },  // 6  EU 433MHz Long Range
        { 917.375f, 250.0f, 11, 5 },  // 7  New Zealand
        { 917.375f,  62.5f,  7, 5 },  // 8  New Zealand Narrow
        { 433.375f,  62.5f,  9, 6 },  // 9  Portugal 433
        { 869.618f,  62.5f,  7, 6 },  // 10 Portugal 868
        { 869.618f,  62.5f,  8, 8 },  // 11 Switzerland
        { 910.525f,  62.5f,  7, 5 },  // 12 USA/Canada
        { 920.250f, 250.0f, 11, 5 },  // 13 Vietnam
    };
    if (profile >= 14) profile = 2;  // default EU/UK Narrow
    const ProfileEntry& p = kProfiles[profile];
    sx1262.setFrequency(p.freq);
    sx1262.setBandwidth(p.bw);
    sx1262.setSpreadingFactor(p.sf);
    sx1262.setCodingRate(p.cr);
    OPS_LOG("Mesh", "Profile %d: %.3f MHz SF%d BW%.1f CR%d",
            profile, (double)p.freq, p.sf, (double)p.bw, p.cr);
}

// ── BT companion bridge ─────────────────────────────────────────────

void MeshService::startCompanionBLE()
{
    const auto& cfg = ops::config::get();
    ops::BTCompanionService::instance().init(
        cfg.callsign[0] ? cfg.callsign : "OMS-NODE", ops::btpin::get());
    if (_initialized)
        the_mesh.startCompanionInterface(
            ops::BTCompanionService::instance().getInterface());
}

void MeshService::stopCompanionBLE()
{
    if (_initialized) the_mesh.stopCompanionInterface();
    ops::BTCompanionService::instance().stop();
}

bool MeshService::isBLERunning() const
{
    return ops::BTCompanionService::instance().isRunning();
}

bool MeshService::isBLEConnected() const
{
    return ops::BTCompanionService::instance().isConnected();
}

bool MeshService::sendDiscoverReq(uint8_t typeFilter) {
    return _initialized && the_mesh.sendDiscoverReqMsg(typeFilter);
}

bool MeshService::pollDiscoverResult(DiscoverEntry& out) {
    return _initialized && the_mesh.pollDiscoverResult(out);
}

bool MeshService::spectrumScan(float startMHz, float stepMHz, int count, float* rssiOut)
{
    if (!_initialized || count <= 0 || !rssiOut) return false;

    // Guard: skip scan if radio is not in RX mode — it may be mid-TX.
    // Changing frequency during an active transmission corrupts the packet
    // and leaves the radio in an undefined state.
    if (!radio_driver.isInRecvMode()) return false;

    // Sweep frequencies using GetRssiInst (SX1262 §9.8: technically FSK/FS/STDBY only,
    // but confirmed to return valid instantaneous RSSI in LoRa RX on this silicon).
    // setFrequency() auto-calibrates image rejection on jumps ≥ 20 MHz (RadioLib).
    // Steps < 20 MHz write the PLL register in-place while in RX — confirmed working.
    for (int i = 0; i < count; i++) {
        float f = startMHz + (float)i * stepMHz;
        if (f < 150.0f) f = 150.0f;
        if (f > 960.0f) f = 960.0f;
        sx1262.setFrequency(f);
        delayMicroseconds(75);           // PLL re-lock (44 µs typical per datasheet)
        rssiOut[i] = sx1262.getRSSI(false);
    }

    // Restore mesh frequency and re-enter continuous RX.
    sx1262.setFrequency(getFreqMHz());
    sx1262.startReceive();
    return true;
}

bool MeshService::cadCheck()
{
    if (!_initialized) return false;
    sx1262.setFrequency(getFreqMHz());
    int16_t result = sx1262.scanChannel();  // blocks ~4 ms (1 symbol at SF8/BW62.5)
    sx1262.startReceive();
    return (result == RADIOLIB_LORA_DETECTED);
}

bool MeshService::cadSweepChannels(const float* freqs, int count, bool* detectedOut)
{
    if (!_initialized || count <= 0 || !freqs || !detectedOut) return false;
    if (s_sigGenActive) return false;
    if (!radio_driver.isInRecvMode()) return false;

    for (int i = 0; i < count; i++) {
        float f = freqs[i];
        if (f < 150.0f || f > 960.0f) { detectedOut[i] = false; continue; }
        sx1262.setFrequency(f);
        detectedOut[i] = (sx1262.scanChannel() == RADIOLIB_LORA_DETECTED);
    }
    sx1262.setFrequency(getFreqMHz());
    sx1262.startReceive();
    return true;
}

// ── Signal generator ─────────────────────────────────────────────────────────

bool MeshService::sigGenStart(float freqMHz, int8_t powerDbm, bool loraMode)
{
    if (!_initialized || s_sigGenActive) return false;
    if (powerDbm < -9) powerDbm = -9;
    if (powerDbm > 22) powerDbm = 22;
    sx1262.standby();
    sx1262.setFrequency(freqMHz);
    sx1262.setOutputPower(powerDbm);
    // transmitDirect(0) sets the RF switch to TX mode and issues SetTxContinuousWave.
    sx1262.transmitDirect(0);
    if (loraMode) {
        // Override CW with SetTxInfinitePreamble (0xD2) — outputs LoRa chirp symbols
        // indefinitely.  RF switch is already in TX from transmitDirect().
        sx1262.getMod()->SPIwriteStream(
            RADIOLIB_SX126X_CMD_SET_TX_INFINITE_PREAMBLE, nullptr, 0);
    }
    s_sigGenActive = true;
    OPS_LOG("SigGen", "start %.3f MHz %s %+d dBm",
            (double)freqMHz, loraMode ? "preamble" : "CW", (int)powerDbm);
    return true;
}

void MeshService::sigGenStop()
{
    if (!_initialized) return;
    s_sigGenActive = false;
    sx1262.standby();
    sx1262.setOutputPower(LORA_TX_POWER);
    sx1262.setFrequency(getFreqMHz());
    sx1262.startReceive();
    OPS_LOG("SigGen", "stopped");
}

bool MeshService::sigGenActive() const { return s_sigGenActive; }

}  // namespace ops

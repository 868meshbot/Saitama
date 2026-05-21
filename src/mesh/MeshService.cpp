// Saitama — MeshService.cpp
// Copyright 2026 Saitama — MIT License
//
// Bridges the MeshCore library to the rest of Saitama.
// Owns the SX1262 radio, identity, and the BaseChatMesh loop.

#include "MeshService.h"
#include "../bt/BTCompanionService.h"
#include "../version.h"
#include "../hardware/Board.h"
#include "../utils/Config.h"
#include "../utils/Contacts.h"
#include "../utils/Repeaters.h"
#include "../utils/SDCard.h"
#include "../utils/Log.h"

#include <Arduino.h>
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
#include <helpers/BaseChatMesh.h>
#include <helpers/TransportKeyStore.h>
#include <helpers/ESP32Board.h>
#include <helpers/radiolib/CustomSX1262.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>

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
class OPSBoard : public ESP32Board {
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
static OPSBoard                  ops_board;
static SPIClass                  lora_spi(FSPI);
static CustomSX1262              sx1262(new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, lora_spi));
static CustomSX1262Wrapper       radio_driver(sx1262, ops_board);
static ArduinoMillis             ms_clock;
static StdRNG                    std_rng;
static ESP32RTCClock             rtc_clock;
static StaticPoolPacketManager   pkt_mgr(32);
static SimpleMeshTables          mesh_tables;

// ── LoRa duty cycle state ─────────────────────────────────────────
// Declared before OPSMesh so getStats() can reference s_dcApplied inline.
// SX1262 hardware duty cycle: radio sleeps most of the time and wakes on
// preamble detection via DIO1 interrupt — MeshCore's STATE_RX is never cleared.
// RX window 50 ms covers the 33 ms 8-symbol preamble at SF8/BW62.5 with margin.
// Duty cycle ~10 % → ~0.6 mA average idle vs ~6 mA continuous RX.
static uint32_t s_dcLastPacketMs  = 0;
static uint32_t s_dcLastRecvCount = 0;
static uint32_t s_dcLastSentCount = 0;
static bool     s_dcApplied       = false;
static constexpr uint32_t DC_RX_US  = 100000;  // 100 ms RX window
static constexpr uint32_t DC_SLP_US = 450000;  // 450 ms sleep

// ── OPSMesh ────────────────────────────────────────────────────────
class OPSMesh : public BaseChatMesh {
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

    uint32_t _lastExpectedAck = 0;
    uint32_t _lastAckedCrc    = 0;
    bool     _hasNewAck       = false;
    // First 4 bytes of the contact whose direct send is in-flight; all-zero = none / flood pending.
    uint8_t  _pendingDirectKey[4] = {};

    TraceResult _traceResult{};
    bool        _hasTraceResult = false;

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

    void _upsertPeer(const ContactInfo& contact) {
        for (int i = 0; i < _peerCount; i++) {
            if (memcmp(_peers[i].pubKeyPrefix, contact.id.pub_key, 4) == 0) {
                strncpy(_peers[i].name, contact.name, 31);
                memcpy(_peers[i].pubKey, contact.id.pub_key, 32);
                _peers[i].lastSeen = getRTCClock()->getCurrentTime();
                _peers[i].lastRssi = radio_driver.getLastRSSI();
                _peers[i].lat      = contact.gps_lat;
                _peers[i].lon      = contact.gps_lon;
                _peerSerial++;
                // Update position in stored contact/repeater without triggering
                // a full NVS save — position persists on the next natural save().
                int idx = -1;
                if (contact.type == 2) {
                    if (ops::repeaters::findByKey(contact.id.pub_key, &idx))
                        ops::repeaters::setPosition(idx, contact.gps_lat, contact.gps_lon);
                } else {
                    if (ops::contacts::findByKey(contact.id.pub_key, &idx))
                        ops::contacts::setPosition(idx, contact.gps_lat, contact.gps_lon);
                }
                return;
            }
        }
        if (_peerCount < MAX_PEERS) {
            PeerInfo& p = _peers[_peerCount++];
            strncpy(p.name, contact.name, 31);
            p.name[31] = '\0';
            memcpy(p.pubKeyPrefix, contact.id.pub_key, 4);
            memcpy(p.pubKey,       contact.id.pub_key, 32);
            p.type     = contact.type;
            p.lastSeen = getRTCClock()->getCurrentTime();
            p.lastRssi = radio_driver.getLastRSSI();
            p.lat      = contact.gps_lat;
            p.lon      = contact.gps_lon;
            _peerSerial++;
            _autoAddPeer(p);
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
            const char* name = _callsign[0] ? _callsign : "OPS-NODE";
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
                    if (expected_ack) _lastExpectedAck = expected_ack;
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
            if (pkt) { sendFlood(pkt, (uint32_t)0); _compWriteOK(); }
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
        _upsertPeer(contact);
        // Record reverse of the inbound advert path so trace (and sendDirect) can
        // route to this node without needing a prior bidirectional DM exchange.
        if (contact.out_path_len == OUT_PATH_UNKNOWN) {
            if (path_len == 0) {
                contact.out_path_len = 0;   // direct neighbour, empty path
            } else {
                _applyReverseOutPath(contact, path, path_len);
            }
            _persistContactPath(contact);
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
        if (crc != 0 && crc == _lastExpectedAck) {
            _lastAckedCrc = crc;
            _hasNewAck    = true;
            memset(_pendingDirectKey, 0, 4);  // direct send succeeded — stop tracking
            OPS_LOG("Mesh", "ACK crc=%08X", crc);
        }
        return nullptr;
    }

    void onContactPathUpdated(const ContactInfo& contact) override {
        _upsertPeer(contact);
        _persistContactPath(contact);
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
        // Fill target prefix from the last hash in the path
        uint8_t last_hash_off = (num_hops - 1) * hash_sz;
        for (int b = 0; b < 4 && b < hash_sz; b++)
            res.targetPubKeyPrefix[b] = path_hashes[last_hash_off + b];

        _traceResult    = res;
        _hasTraceResult = true;

        OPS_LOG("Trace", "onTraceRecv tag=%08X hops=%d snrs=%d",
                tag, num_hops, num_snrs);
    }

    void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp, const char* text) override {
        _upsertPeer(from);
        // Opportunistically record the return path from a flood DM.
        if (from.out_path_len == OUT_PATH_UNKNOWN && pkt->isRouteFlood()) {
            ContactInfo* ci = lookupContactByPubKey(from.id.pub_key, PUB_KEY_SIZE);
            if (ci) {
                if (pkt->path_len == 0) ci->out_path_len = 0;
                else _applyReverseOutPath(*ci, pkt->path, pkt->path_len);
            }
        }
        RxMessage msg{};
        msg.timestamp = sender_timestamp;
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

            _discoverTail = (_discoverTail + 1) % DISCOVER_QUEUE_SIZE;
            _discoverCount++;
            OPS_LOG("Finder", "RESP %02X%02X type=%d rssi=%.0f",
                    pubKey[0], pubKey[1], nodeType, (double)e.rssi);

            // Add to in-memory routing table as a direct neighbor (path_len=0)
            if (!lookupContactByPubKey(pubKey, 4)) {
                ContactInfo ci{};
                ci.id = mesh::Identity(pubKey);
                if (e.name[0]) strncpy(ci.name, e.name, 31);
                ci.type        = nodeType;
                ci.out_path_len = 0;
                addContact(ci);
            }

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

    void onSendTimeout() override {
        bool wasDirect = _pendingDirectKey[0] || _pendingDirectKey[1] ||
                         _pendingDirectKey[2] || _pendingDirectKey[3];
        if (wasDirect) {
            ContactInfo* ci = lookupContactByPubKey(_pendingDirectKey, 4);
            if (ci) {
                OPS_LOG("Mesh", "Direct timeout for '%s' — resetting path, next send will flood",
                        ci->name);
                resetPathTo(*ci);
                // Also clear the persisted path in the NVS/SD stores so a reboot does not
                // reload the stale path and immediately time out again.
                int idx;
                if (ops::contacts::findByKey(_pendingDirectKey, &idx))
                    ops::contacts::clearPath(idx);
                if (ops::repeaters::findByKey(_pendingDirectKey, &idx))
                    ops::repeaters::clearPath(idx);
            } else {
                OPS_LOG("Mesh", "Direct timeout — contact no longer in table");
            }
            memset(_pendingDirectKey, 0, 4);
        } else {
            OPS_LOG("Mesh", "Flood timeout — no ACK received");
        }
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

    void onContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len) override
    {
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
            snprintf(buf, sizeof(buf),
                "[%s] Batt:%dmV  RSSI:%d  SNR:%d.%02d  Noise:%d",
                contact.name, (int)batt_mv, (int)rssi, snr_i, snr_f, (int)noise);
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

    bool pollAck(uint32_t& crc) {
        if (!_hasNewAck) return false;
        crc = _lastAckedCrc;
        _hasNewAck = false;
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
        OPSMesh* self = const_cast<OPSMesh*>(this);
        ContactInfo* ci = self->lookupContactByPubKey(prefix4, 4);
        return ci && ci->out_path_len != OUT_PATH_UNKNOWN;
    }

    // Sends TRACE (0x09) along the known path to a contact.
    // The path ends with the contact's own hash byte(s) so the retransmitted
    // packet triggers onTraceRecv on any node that receives it after the final
    // hop — including the initiator if it is in RF range.
    bool sendTraceToContact(const uint8_t* prefix4, uint32_t& out_tag) {
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
        uint8_t path_sz = hash_sz - 1;                   // 0 for 1-byte, 1 for 2-byte
        uint8_t flags   = path_sz;

        uint8_t byte_count = hash_sz * hop_count;

        // Build combined path: intermediate hops + contact's own pub_key prefix
        static uint8_t combined[MAX_PATH_SIZE + 3];
        if (byte_count + hash_sz > sizeof(combined)) return false;
        memcpy(combined, ci->out_path, byte_count);
        memcpy(combined + byte_count, ci->id.pub_key, hash_sz);
        uint8_t total_bytes = byte_count + hash_sz;

        uint32_t tag  = (uint32_t)std_rng.nextInt(1, 0x7FFFFFFF);
        uint32_t auth = (uint32_t)std_rng.nextInt(1, 0x7FFFFFFF);

        mesh::Packet* pkt = createTrace(tag, auth, flags);
        if (!pkt) return false;
        sendDirect(pkt, combined, total_bytes, 0);

        out_tag = tag;

        // Log path bytes so the user can verify the path is correct
        char pathHex[64] = {};
        for (int b = 0; b < total_bytes && (b * 3 + 2) < (int)sizeof(pathHex); b++)
            snprintf(pathHex + b * 3, 4, "%02X ", combined[b]);
        OPS_LOG("Trace", "Sent trace to %02X%02X hops=%d flags=%02X path=[%s]tag=%08X",
                prefix4[0], prefix4[1], hop_count + 1, flags, pathHex, tag);
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

    OPSMesh()
        : BaseChatMesh(radio_driver, ms_clock, std_rng, rtc_clock, pkt_mgr, mesh_tables)
    {}

    void begin_mesh(const char* callsign) {
        strncpy(_callsign, callsign, 31);
        _callsign[31] = '\0';

        BaseChatMesh::begin();

        uint32_t seed = esp_random() ^ (uint32_t)millis();
        std_rng.begin((long)seed);

        if (!LittleFS.begin(true)) {
            OPS_LOG("Mesh", "LittleFS mount failed");
            return;
        }

        if (!LittleFS.exists("/mesh")) LittleFS.mkdir("/mesh");

        // Load identity: LittleFS first, then NVS, then SD.
        // Only restore from backup when LittleFS is missing the identity — never
        // overwrite unconditionally, which could replace a good identity with a stale one.
        IdentityStore store(LittleFS, "/mesh");
        store.begin();

        char stored_name[32] = {};
        bool loaded = store.load("self", self_id, stored_name, sizeof(stored_name));

        if (!loaded) {
            // NVS backup survives LittleFS.begin(true) format events
            Preferences idPrefs;
            if (idPrefs.begin("opsMesh", /*readOnly=*/true)) {
                size_t blen = idPrefs.getBytesLength("selfId");
                if (blen > 0 && blen <= 256) {
                    uint8_t buf[256];
                    if (idPrefs.getBytes("selfId", buf, blen) == blen) {
                        File idFile = LittleFS.open("/mesh/self.id", "w", true);
                        if (idFile) { idFile.write(buf, blen); idFile.close(); }
                        loaded = store.load("self", self_id, stored_name, sizeof(stored_name));
                        if (loaded) OPS_LOG("Mesh", "Identity restored from NVS (%d bytes)", (int)blen);
                    }
                }
                idPrefs.end();
            }
        }

        if (!loaded && ops::sdcard::isMounted() && ops::sdcard::hasFile("/ops/identity.bin")) {
            uint8_t buf[256];
            size_t  len = 0;
            if (ops::sdcard::readFile("/ops/identity.bin", buf, sizeof(buf), &len) && len > 0) {
                File idFile = LittleFS.open("/mesh/self.id", "w", true);
                if (idFile) { idFile.write(buf, len); idFile.close(); }
                loaded = store.load("self", self_id, stored_name, sizeof(stored_name));
                if (loaded) OPS_LOG("Mesh", "Identity restored from SD (%d bytes)", (int)len);
            }
        }

        if (!loaded) {
            OPS_LOG("Mesh", "Generating new identity for '%s'", _callsign);
            self_id = mesh::LocalIdentity(getRNG());
            for (int i = 0; i < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF); i++)
                self_id = mesh::LocalIdentity(getRNG());
            store.save("self", self_id, _callsign);
        } else if (stored_name[0] != '\0') {
            if (_callsign[0] != '\0' && strcmp(_callsign, stored_name) != 0) {
                store.save("self", self_id, _callsign);
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
        preloadNvsContacts();
        preloadNvsRepeaters();
        OPS_LOG("Mesh", "Ready as '%s'", _callsign);
    }

    void preloadNvsContacts() {
        int n = ops::contacts::count();
        for (int i = 0; i < n; i++) {
            ops::Contact c;
            if (!ops::contacts::get(i, c)) continue;
            bool hasKey = false;
            for (int b = 0; b < 32; b++) if (c.pubKey[b]) { hasKey = true; break; }
            if (!hasKey) { OPS_LOG("Mesh", "Contact '%s' has no key — skipped", c.name); continue; }
            if (lookupContactByPubKey(c.pubKey, 4)) continue;
            ContactInfo ci{};
            ci.id = mesh::Identity(c.pubKey);
            strncpy(ci.name, c.name, 31);
            if (c.outPathValid && c.outPathLen != 0xFF) {
                ci.out_path_len = c.outPathLen;
                memcpy(ci.out_path, c.outPath, MAX_PATH_SIZE);
                OPS_LOG("Mesh", "Preloaded contact: %s (path len=%d)", c.name, c.outPathLen);
            } else {
                ci.out_path_len = OUT_PATH_UNKNOWN;
                OPS_LOG("Mesh", "Preloaded contact: %s (path unknown)", c.name);
            }
            addContact(ci);
        }
    }

    void preloadNvsRepeaters() {
        int n = ops::repeaters::count();
        for (int i = 0; i < n; i++) {
            ops::Repeater r;
            if (!ops::repeaters::get(i, r)) continue;
            bool hasKey = false;
            for (int b = 0; b < 32; b++) if (r.pubKey[b]) { hasKey = true; break; }
            if (!hasKey) { OPS_LOG("Mesh", "Repeater '%s' has no key — skipped", r.name); continue; }
            if (lookupContactByPubKey(r.pubKey, 4)) continue;
            ContactInfo ci{};
            ci.id = mesh::Identity(r.pubKey);
            strncpy(ci.name, r.name, 31);
            ci.type = ADV_TYPE_REPEATER;
            if (r.outPathValid && r.outPathLen != 0xFF) {
                ci.out_path_len = r.outPathLen;
                memcpy(ci.out_path, r.outPath, MAX_PATH_SIZE);
                OPS_LOG("Mesh", "Preloaded repeater: %s (path len=%d)", r.name, r.outPathLen);
            } else {
                ci.out_path_len = OUT_PATH_UNKNOWN;
                OPS_LOG("Mesh", "Preloaded repeater: %s (path unknown)", r.name);
            }
            addContact(ci);
        }
    }

    void _persistContactPath(const ContactInfo& ci) {
        if (ci.out_path_len == OUT_PATH_UNKNOWN) return;
        int idx;
        if (ops::repeaters::findByKey(ci.id.pub_key, &idx))
            ops::repeaters::setPath(idx, ci.out_path_len, ci.out_path);
        if (ops::contacts::findByKey(ci.id.pub_key, &idx))
            ops::contacts::setPath(idx, ci.out_path_len, ci.out_path);
    }

    void updateCallsign(const char* cs) {
        strncpy(_callsign, cs, 31);
        _callsign[31] = '\0';
        IdentityStore store(LittleFS, "/mesh");
        store.begin();
        store.save("self", self_id, _callsign);
        _saveIdentityBackups();
        OPS_LOG("Mesh", "Callsign updated to '%s'", _callsign);
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
            // Already registered — update secret and recompute hash in place
            memset(_channels[cfgSlot]->channel.secret, 0, sizeof(_channels[cfgSlot]->channel.secret));
            int len = _b64decode(psk64, _channels[cfgSlot]->channel.secret,
                                 (int)sizeof(_channels[cfgSlot]->channel.secret));
            if (len == 16 || len == 32) {
                mesh::Utils::sha256(_channels[cfgSlot]->channel.hash,
                                    sizeof(_channels[cfgSlot]->channel.hash),
                                    _channels[cfgSlot]->channel.secret, len);
                OPS_LOG("Mesh", "Channel %d synced in place (len=%d)", cfgSlot, len);
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

    void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis = 0) override {
        // Find the config slot that owns this channel object
        const auto& cfg = config::get();
        for (int i = 0; i < 10; i++) {
            if (!_channels[i]) continue;
            if (&_channels[i]->channel != &channel) continue;
            // Matched — apply scope if configured for this slot
            const char* scope = cfg.channels[i].scope;
            if (scope && scope[0]) {
                TransportKey tk;
                TransportKeyStore tks;
                tks.getAutoKeyFor(0, scope, tk);
                uint16_t codes[2] = { tk.calcTransportCode(pkt), 0 };
                sendFlood(pkt, codes, delay_millis);
                return;
            }
            break;
        }
        sendFlood(pkt, delay_millis);
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
        uint32_t expected_ack = 0, est_timeout = 0;
        int r = sendMessage(*ci, getRTCClock()->getCurrentTime(), 0, text, expected_ack, est_timeout);
        if (r == MSG_SEND_FAILED) {
            OPS_LOG("Mesh", "sendDirect: MSG_SEND_FAILED for %s", ci->name);
            memset(_pendingDirectKey, 0, 4);
        } else if (r == MSG_SEND_SENT_DIRECT) {
            // Track so onSendTimeout() can auto-reset if no ACK arrives.
            memcpy(_pendingDirectKey, ci->id.pub_key, 4);
            _lastExpectedAck = expected_ack;
        } else {
            // MSG_SEND_SENT_FLOOD — path was unknown, flooding; nothing to reset on timeout.
            memset(_pendingDirectKey, 0, 4);
            _lastExpectedAck = expected_ack;
        }
        return r != MSG_SEND_FAILED;
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
            sendFlood(pkt, delay_ms);
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

    int      rxCount()      const { return _rxCount;   }
    int      numPeers()     const { return _peerCount;  }
    uint32_t numPeerSerial() const { return _peerSerial; }

    bool getPeerInfo(int idx, PeerInfo& out) const {
        if (idx < 0 || idx >= _peerCount) return false;
        out = _peers[idx];
        return true;
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

    bool getContactPathInfo(const uint8_t* prefix4, PathInfo& out) const {
        OPSMesh* self = const_cast<OPSMesh*>(this);
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

    void resetContactPath(const uint8_t* prefix4) {
        ContactInfo* ci = lookupContactByPubKey(prefix4, 4);
        if (ci) ci->out_path_len = OUT_PATH_UNKNOWN;
    }

    void resetAllContactPaths() {
        for (int i = 0; i < _peerCount; i++) {
            ContactInfo* ci = lookupContactByPubKey(_peers[i].pubKeyPrefix, 4);
            if (ci) ci->out_path_len = OUT_PATH_UNKNOWN;
        }
    }

    // Reads /mesh/self.id from LittleFS and writes it to NVS ("opsMesh"/"selfId")
    // and SD (/ops/identity.bin).  Call after every identity load or save.
    void _saveIdentityBackups() {
        if (!LittleFS.exists("/mesh/self.id")) return;
        File f = LittleFS.open("/mesh/self.id", "r");
        if (!f) return;
        uint8_t buf[256];
        size_t  len = f.read(buf, sizeof(buf));
        f.close();
        if (len == 0) return;
        Preferences idPrefs;
        if (idPrefs.begin("opsMesh", /*readOnly=*/false)) {
            idPrefs.putBytes("selfId", buf, len);
            idPrefs.end();
        }
        if (ops::sdcard::isMounted())
            ops::sdcard::writeFile("/ops/identity.bin", buf, len);
        OPS_LOG("Mesh", "Identity backups updated (NVS+SD)");
    }
};

static OPSMesh the_mesh;

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
        s_dcApplied       = false;  // TX finished; MeshCore will call startReceive() shortly
    }

    if (!cfg.loraDutyCycle) {
        if (s_dcApplied) {
            // Restore continuous RX without touching MeshCore's state variable.
            sx1262.startReceive();
            s_dcApplied = false;
            OPS_LOG("Mesh", "LoRa duty cycle disarmed");
        }
        return;
    }

    // Arm duty cycle after 3 s of no received packets (3-hop flood repeat window).
    if (!s_dcApplied && radio_driver.isInRecvMode()) {
        if (millis() - s_dcLastPacketMs >= 3000UL) {
            sx1262.startReceiveDutyCycle(DC_RX_US, DC_SLP_US);
            s_dcApplied = true;
            OPS_LOG("Mesh", "LoRa duty cycle armed (Rx:100ms Sleep:450ms)");
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
    the_mesh.begin_mesh(cfg.callsign[0] ? cfg.callsign : "OPS-NODE");

    applyLoraProfile(cfg.radioProfile);
    applyRadioOverrides();
    if (cfg.rxBoost) sx1262.setRxBoostedGainMode(true);
    the_mesh.sendSelfAdvert(random(500, 2500));

    _initialized = true;
    OPS_LOG("Mesh", "MeshCore ready");
}

void MeshService::tick() {
    if (!_initialized) return;
    the_mesh.loop();
    the_mesh.checkSerialInterface();
    _tickDutyCycle();
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

int MeshService::peerCount() const {
    return _initialized ? the_mesh.numPeers() : 0;
}

uint32_t MeshService::peerSerial() const {
    return _initialized ? the_mesh.numPeerSerial() : 0;
}

bool MeshService::getPeer(int idx, PeerInfo& out) const {
    return _initialized && the_mesh.getPeerInfo(idx, out);
}

bool MeshService::pollAck(uint32_t& acked_crc) {
    return _initialized && the_mesh.pollAck(acked_crc);
}

bool MeshService::sendTrace(const uint8_t* pubKeyPrefix4, uint32_t& out_tag) {
    return _initialized && the_mesh.sendTraceToContact(pubKeyPrefix4, out_tag);
}

bool MeshService::pollTraceResult(TraceResult& out) {
    return _initialized && the_mesh.pollTraceResult(out);
}

bool MeshService::hasPathTo(const uint8_t* pubKeyPrefix4) const {
    return _initialized && the_mesh.hasPathToContact(pubKeyPrefix4);
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

float MeshService::getFreqMHz() const {
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
        cfg.callsign[0] ? cfg.callsign : "OPS-NODE", 123456);
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

}  // namespace ops

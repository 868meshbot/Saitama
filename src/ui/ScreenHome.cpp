// Saitama — ScreenHome.cpp
// Copyright 2026 Saitama — MIT License
//
// Chat app — two modes within one class:
//
//   LIST mode  (320 x 240):
//   ┌──────────────────────────────────────┐
//   │ [⌂]  Channels                        │  header 30 px
//   ├──────────────────────────────────────┤
//   │ ● Public                       [≡]  │
//   │   Mesh                         [≡]  │  channel rows 34 px each
//   │   ...                               │
//   │   Direct Messages                   │
//   └──────────────────────────────────────┘
//
//   CHAT mode  (320 x 240):
//   ┌──────────────────────────────────────┐
//   │ [<]  Channel Name                    │  header 28 px
//   ├──────────────────────────────────────┤
//   │                                      │  176 px scrollable bubbles
//   ├──────────────────────────────────────┤
//   │ [ Type a message...          ] [➤]  │  input bar 36 px
//   └──────────────────────────────────────┘

#include "ScreenHome.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "emoji/emoji_data.h"
#include "../utils/Config.h"
#include "../utils/Contacts.h"
#include "../utils/Log.h"
#include "../utils/SDCard.h"
#include "../utils/Sound.h"
#include "../mesh/MeshService.h"
#include <cstring>
#include <cstdlib>
#include <esp_system.h>
#include <time.h>

namespace ops { namespace ui {

// ── Static member definitions ─────────────────────────────────────────

ScreenHome::Mode ScreenHome::s_mode        = ScreenHome::MODE_LIST;
int              ScreenHome::s_activeChIdx = 0;
bool             ScreenHome::s_chUnread[10] = {};
lv_obj_t*        ScreenHome::s_rowDots[10]  = {};

int      ScreenHome::s_sendMode    = 0;
uint8_t  ScreenHome::s_dmPubKey[4] = {};
char     ScreenHome::s_dmName[32]  = {};

lv_obj_t* ScreenHome::s_listScreen = nullptr;
lv_obj_t* ScreenHome::_screen      = nullptr;
lv_obj_t* ScreenHome::_msgArea     = nullptr;
lv_obj_t* ScreenHome::_textarea    = nullptr;
lv_obj_t* ScreenHome::_sendBtn     = nullptr;
lv_obj_t* ScreenHome::_emojiPanel  = nullptr;

ScreenHome::MsgEntry ScreenHome::s_history[ScreenHome::HISTORY_MAX];
int                  ScreenHome::s_histCount = 0;
lv_obj_t*            ScreenHome::s_metaLabels[ScreenHome::HISTORY_MAX] = {};

char ScreenHome::s_loadedTags[10][32] = {};
int  ScreenHome::s_loadedTagCnt = 0;
bool ScreenHome::s_loadingFromSD = false;

// File-scope statics for overlays / dialog state
static lv_obj_t* s_actionOverlay    = nullptr;
static lv_obj_t* s_addOverlay       = nullptr;
static lv_obj_t* s_addNameTa        = nullptr;
static lv_obj_t* s_addPskTa         = nullptr;
static lv_obj_t* s_addScopeTa       = nullptr;
static lv_obj_t* s_dmPickerOverlay  = nullptr;
static lv_obj_t* s_addContactOverlay = nullptr;
// Captured when the user taps a received bubble; consumed by _onAddContactSave.
static char     s_pendingContactName[32] = {};
static uint8_t  s_pendingContactKey[4]   = {};

// ── Msg JSON helpers (no heap — hand-rolled for hot path) ─────────────

static void _jsonEscape(const char* src, char* dst, int dstMax)
{
    int j = 0;
    for (int i = 0; src[i] && j < dstMax - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { if (j < dstMax - 3) dst[j++] = '\\'; }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

static void _serializeMsgLine(char* out, size_t outSize,
                               bool sent, const char* name, const char* text,
                               uint8_t hops, uint32_t ts, int rssi, uint32_t ack)
{
    char eName[66] = {}, eText[330] = {};
    _jsonEscape(name ? name : "", eName,  sizeof(eName));
    _jsonEscape(text ? text : "", eText,  sizeof(eText));
    snprintf(out, outSize,
             "{\"s\":%d,\"n\":\"%s\",\"t\":\"%s\",\"h\":%u,\"ts\":%lu,\"r\":%d,\"a\":%lu}",
             (int)sent, eName, eText, (unsigned)hops,
             (unsigned long)ts, rssi, (unsigned long)ack);
}

static long _jsonGetLong(const char* json, const char* key)
{
    char search[12];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(json, search);
    if (!p) return 0;
    return strtol(p + strlen(search), nullptr, 10);
}

static int _jsonGetStr(const char* json, const char* key, char* out, int outSize)
{
    char search[12];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char* p = strstr(json, search);
    if (!p) { out[0] = '\0'; return 0; }
    p += strlen(search);
    int n = 0;
    while (*p && n < outSize - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            char c = (*p == 'n') ? '\n' : *p;
            out[n++] = c;
        } else if (*p == '"') { break; }
        else { out[n++] = *p; }
        p++;
    }
    out[n] = '\0';
    return n;
}

// Replace 3-byte UTF-8 smart quotes/dashes with ASCII equivalents in-place.
// Needed because the Montserrat font only covers U+0020-007F and U+00A0-00FF.
static void _sanitizeSmartPunct(char* s)
{
    unsigned char* p = (unsigned char*)s;
    unsigned char* w = p;
    while (*p) {
        if (p[0] == 0xE2 && p[1] == 0x80) {
            unsigned char sub = p[2];
            if (sub == 0x98 || sub == 0x99) { *w++ = '\''; p += 3; continue; }
            if (sub == 0x9C || sub == 0x9D) { *w++ = '"';  p += 3; continue; }
            if (sub == 0x93 || sub == 0x94) { *w++ = '-';  p += 3; continue; }
        }
        *w++ = *p++;
    }
    *w = '\0';
}

// ── History ring buffer ───────────────────────────────────────────────

void ScreenHome::_historyAdd(bool           sent,
                              const char*    sender,
                              const char*    text,
                              uint8_t        hops,
                              uint32_t       ts,
                              float          rssi,
                              uint32_t       expectedAck,
                              const char*    channelTag,
                              const char*    pathStr,
                              const uint8_t* pubKeyPrefix)
{
    if (s_histCount == HISTORY_MAX) {
        memmove(s_history,    s_history    + 1, (HISTORY_MAX - 1) * sizeof(MsgEntry));
        memmove(s_metaLabels, s_metaLabels + 1, (HISTORY_MAX - 1) * sizeof(lv_obj_t*));
        s_histCount--;
    }
    MsgEntry& e = s_history[s_histCount];
    e.sent = sent;
    strncpy(e.senderName, sender ? sender : "", sizeof(e.senderName) - 1);
    e.senderName[sizeof(e.senderName) - 1] = '\0';
    strncpy(e.text, text ? text : "", sizeof(e.text) - 1);
    e.text[sizeof(e.text) - 1] = '\0';
    _sanitizeSmartPunct(e.text);
    strncpy(e.channelTag, channelTag ? channelTag : "Public", sizeof(e.channelTag) - 1);
    e.channelTag[sizeof(e.channelTag) - 1] = '\0';
    strncpy(e.pathStr, pathStr ? pathStr : "", sizeof(e.pathStr) - 1);
    e.pathStr[sizeof(e.pathStr) - 1] = '\0';
    if (pubKeyPrefix)
        memcpy(e.pubKeyPrefix, pubKeyPrefix, 4);
    else
        memset(e.pubKeyPrefix, 0, 4);
    e.hops        = hops;
    e.ts          = ts;
    e.rssi        = rssi;
    e.expectedAck = expectedAck;
    e.isAcked     = false;
    s_metaLabels[s_histCount] = nullptr;
    s_histCount++;

    if (s_loadingFromSD) {
        // replaying from SD — skip write
    } else if (!ops::config::get().saveMsgs) {
        OPS_LOG("Chat", "saveMsgs off — not saving '%s'", e.channelTag);
    } else if (!ops::sdcard::isMounted()) {
        OPS_LOG("Chat", "SD not mounted — dropping msg for '%s'", e.channelTag);
    } else {
        char line[480];
        _serializeMsgLine(line, sizeof(line), sent, sender, text,
                          hops, ts, (int)rssi, expectedAck);
        if (!ops::sdcard::appendMsgLine(e.channelTag, line))
            OPS_LOG("Chat", "appendMsgLine failed for '%s'", e.channelTag);
    }
}

// ── _getViewTag() ─────────────────────────────────────────────────────

const char* ScreenHome::_getViewTag()
{
    if (s_sendMode == 10) {
        static char dmTag[16];
        snprintf(dmTag, sizeof(dmTag), "DM_%.11s", s_dmName);
        return dmTag;
    }
    const auto& cfg = ops::config::get();
    if (s_sendMode == 0) {
        return cfg.channels[0].name[0] ? cfg.channels[0].name : "Public";
    }
    const char* n = cfg.channels[s_sendMode].name;
    return (n && n[0]) ? n : "Public";
}

// ── _tagToSlot() ─────────────────────────────────────────────────────
// Returns channel slot index 0-9, or -1 for DM/unknown.

int ScreenHome::_tagToSlot(const char* tag)
{
    if (!tag || !tag[0]) return -1;
    const auto& cfg = ops::config::get();
    const char* pub = cfg.channels[0].name[0] ? cfg.channels[0].name : "Public";
    if (strcmp(tag, pub) == 0 || strcmp(tag, "Public") == 0) return 0;
    for (int i = 1; i < 10; i++) {
        if (cfg.channels[i].name[0] && strcmp(cfg.channels[i].name, tag) == 0) return i;
    }
    return -1;
}

// ── _rebuildMsgArea() ─────────────────────────────────────────────────

void ScreenHome::_rebuildMsgArea()
{
    if (!_msgArea) return;
    for (int i = 0; i < HISTORY_MAX; i++) s_metaLabels[i] = nullptr;
    lv_obj_clean(_msgArea);

    const char* viewTag = _getViewTag();
    for (int i = 0; i < s_histCount; i++) {
        const MsgEntry& e = s_history[i];
        const char* tag = e.channelTag[0] ? e.channelTag : "Public";
        if (strcmp(tag, viewTag) != 0) continue;
        _addBubble(i, e.sent, e.senderName, e.text, e.hops, e.ts, e.rssi, e.isAcked, e.pathStr);
    }
}

// ── Time formatter ────────────────────────────────────────────────────

void ScreenHome::_fmtTime(char* buf, size_t len, uint32_t ts)
{
    if (ts == 0) { snprintf(buf, len, "--:--"); return; }
    time_t t = (time_t)ts;
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, len, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

// ── Bubble helper ──────────────────────────────────────────────────────

void ScreenHome::_addBubble(int         histIdx,
                             bool        sent,
                             const char* senderName,
                             const char* text,
                             uint8_t     hops,
                             uint32_t    ts,
                             float       rssi,
                             bool        isAcked,
                             const char* pathStr)
{
    if (!_msgArea) return;
    const auto& cfg = ops::config::get();

    lv_obj_t* row = lv_obj_create(_msgArea);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_row(row, 0, 0);
    lv_obj_set_style_pad_column(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        sent ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, LV_PCT(68));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 6, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_shadow_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 5, 0);
    lv_obj_set_style_pad_row(bubble, 2, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bubble,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(bubble,
        sent ? theme::PRIMARY : theme::BG_CARD, 0);

    if (!sent) {
        // Header row: sender name (if known) + action button (▾) pinned to top-right.
        lv_obj_t* hdr = lv_obj_create(bubble);
        lv_obj_set_width(hdr, LV_PCT(100));
        lv_obj_set_height(hdr, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hdr, 0, 0);
        lv_obj_set_style_pad_all(hdr, 0, 0);
        lv_obj_set_style_pad_column(hdr, 3, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* nameLbl = lv_label_create(hdr);
        lv_label_set_text(nameLbl, (senderName && senderName[0]) ? senderName : "");
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_flex_grow(nameLbl, 1);
        lv_obj_set_style_min_width(nameLbl, 0, 0);
        lv_obj_set_style_text_color(nameLbl, theme::ACCENT, 0);
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_12, 0);

        lv_obj_t* btn = lv_btn_create(hdr);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 18, 18);
        lv_obj_set_style_bg_color(btn, theme::BG, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, theme::BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 3, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, _onBubbleClick, LV_EVENT_CLICKED,
                            (void*)(intptr_t)histIdx);
        lv_obj_t* ico = lv_label_create(btn);
        lv_label_set_text(ico, LV_SYMBOL_DOWN);
        lv_obj_set_style_text_font(ico, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(ico, theme::ACCENT, 0);
        lv_obj_center(ico);
    }

    lv_obj_t* msgLbl = lv_label_create(bubble);
    lv_label_set_text(msgLbl, text ? text : "");
    lv_label_set_long_mode(msgLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msgLbl, LV_PCT(100));
    lv_obj_set_style_text_font(msgLbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_line_space(msgLbl, 3, 0);
    lv_obj_set_style_text_color(msgLbl, sent ? theme::TEXT : theme::GREEN, 0);

    char timeBuf[8];
    _fmtTime(timeBuf, sizeof(timeBuf), ts);

    char meta[48] = {};
    if (sent) {
        uint32_t expAck = (histIdx >= 0 && histIdx < HISTORY_MAX)
                          ? s_history[histIdx].expectedAck : 1;
        if (isAcked || expAck == 0) {
            // green tick for DM ACK; grey tick for channel (no ACK possible)
            snprintf(meta, sizeof(meta), "%s " LV_SYMBOL_OK, timeBuf);
        } else {
            snprintf(meta, sizeof(meta), "%s", timeBuf);  // DM awaiting ACK
        }
    } else {
        if (cfg.showRssi)
            snprintf(meta, sizeof(meta), "%ddBm  %s", (int)rssi, timeBuf);
        else
            snprintf(meta, sizeof(meta), "%s", timeBuf);
    }

    lv_obj_t* metaLbl = lv_label_create(bubble);
    lv_obj_set_width(metaLbl, LV_PCT(100));
    lv_obj_set_style_text_color(metaLbl, (sent && isAcked) ? theme::GREEN : theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(metaLbl, &lv_font_montserrat_10, 0);
    lv_label_set_text(metaLbl, meta);
    if (sent) lv_obj_set_style_text_align(metaLbl, LV_TEXT_ALIGN_RIGHT, 0);

    if (histIdx >= 0 && histIdx < HISTORY_MAX)
        s_metaLabels[histIdx] = metaLbl;

    // Hop info row — separate line below the bubble, left-aligned
    if (!sent && cfg.showHops) {
        lv_obj_t* hopRow = lv_obj_create(_msgArea);
        lv_obj_set_width(hopRow, LV_PCT(100));
        lv_obj_set_height(hopRow, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(hopRow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hopRow, 0, 0);
        lv_obj_set_style_pad_top(hopRow, 0, 0);
        lv_obj_set_style_pad_bottom(hopRow, 3, 0);
        lv_obj_set_style_pad_left(hopRow, 6, 0);
        lv_obj_set_style_pad_right(hopRow, 0, 0);
        lv_obj_clear_flag(hopRow, LV_OBJ_FLAG_SCROLLABLE);

        char hopBuf[56];
        const bool hasPath = pathStr && pathStr[0];
        if (hops == 0)
            snprintf(hopBuf, sizeof(hopBuf), "Direct");
        else if (hops == 1)
            snprintf(hopBuf, sizeof(hopBuf), hasPath ? "1 hop  %s" : "1 hop", pathStr);
        else
            snprintf(hopBuf, sizeof(hopBuf), hasPath ? "%u hops  %s" : "%u hops",
                     (unsigned)hops, pathStr);

        lv_obj_t* hopLbl = lv_label_create(hopRow);
        lv_label_set_text(hopLbl, hopBuf);
        lv_obj_set_width(hopLbl, LV_PCT(100));
        lv_label_set_long_mode(hopLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(hopLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(hopLbl, theme::TEXT_MUTED, 0);
    }

    lv_obj_scroll_to_y(_msgArea, LV_COORD_MAX, LV_ANIM_OFF);
}

// ── _loadChannelHistory() ─────────────────────────────────────────────

void ScreenHome::_loadChannelHistory(const char* tag)
{
    if (!ops::sdcard::isMounted()) return;
    for (int i = 0; i < s_loadedTagCnt; i++)
        if (strncmp(s_loadedTags[i], tag, 31) == 0) return;

    if (s_loadedTagCnt < 10) {
        strncpy(s_loadedTags[s_loadedTagCnt], tag, 31);
        s_loadedTags[s_loadedTagCnt][31] = '\0';
        s_loadedTagCnt++;
    }

    constexpr size_t BUFSIZE = 8192;
    char* buf = (char*)ps_malloc(BUFSIZE);
    if (!buf) { OPS_LOG("Chat", "ps_malloc failed for msg log"); return; }

    size_t n = ops::sdcard::readMsgLog(tag, buf, BUFSIZE);
    if (n == 0) { free(buf); return; }

    s_loadingFromSD = true;

    char* line = buf;
    if (n == BUFSIZE - 1 && buf[0] != '{') {
        char* nl = strchr(buf, '\n');
        if (nl) line = nl + 1;
    }

    int loaded = 0;
    while (*line) {
        char* end = strchr(line, '\n');
        if (end) { *end = '\0'; }
        if (*line == '{') {
            bool sent      = (bool)_jsonGetLong(line, "s");
            uint8_t hops   = (uint8_t)_jsonGetLong(line, "h");
            uint32_t ts    = (uint32_t)_jsonGetLong(line, "ts");
            float rssi     = (float)_jsonGetLong(line, "r");
            uint32_t ack   = (uint32_t)_jsonGetLong(line, "a");
            char name[32] = {}, text[160] = {};
            _jsonGetStr(line, "n", name, sizeof(name));
            _jsonGetStr(line, "t", text, sizeof(text));
            _historyAdd(sent, name[0] ? name : nullptr, text,
                        hops, ts, rssi, ack, tag);
            loaded++;
        }
        if (!end) break;
        line = end + 1;
    }

    s_loadingFromSD = false;
    free(buf);
    OPS_LOG("Chat", "Loaded %d msgs for '%s'", loaded, tag);
}

// ── _clearChannelMessages() ───────────────────────────────────────────

void ScreenHome::_clearChannelMessages(int chIdx)
{
    const auto& cfg = ops::config::get();
    const char* tag;
    if (chIdx == 0) {
        tag = cfg.channels[0].name[0] ? cfg.channels[0].name : "Public";
    } else {
        tag = cfg.channels[chIdx].name;
    }
    if (!tag || !tag[0]) return;

    ops::sdcard::deleteMsgLog(tag);

    // Remove from loadedTags so history can be reloaded if needed
    for (int i = 0; i < s_loadedTagCnt; i++) {
        if (strncmp(s_loadedTags[i], tag, 31) == 0) {
            for (int j = i; j < s_loadedTagCnt - 1; j++)
                memcpy(s_loadedTags[j], s_loadedTags[j + 1], 32);
            s_loadedTagCnt--;
            break;
        }
    }

    // Compact history in place, removing entries for this tag
    int write = 0;
    for (int i = 0; i < s_histCount; i++) {
        const char* hTag = s_history[i].channelTag[0] ? s_history[i].channelTag : "Public";
        if (strcmp(hTag, tag) != 0) {
            if (write != i) {
                s_history[write] = s_history[i];
                s_metaLabels[write] = nullptr;
            }
            write++;
        }
    }
    s_histCount = write;

    // If currently showing this channel in chat, rebuild
    if (s_mode == MODE_CHAT && s_activeChIdx == chIdx && _msgArea)
        _rebuildMsgArea();

    OPS_LOG("Chat", "Cleared ch%d (%s)", chIdx, tag);
}

// ── _deleteChannel() ──────────────────────────────────────────────────

void ScreenHome::_deleteChannel(int chIdx)
{
    if (chIdx <= 0) return;  // can't delete public (slot 0)
    _clearChannelMessages(chIdx);
    ops::config::setChannel(chIdx, "", "", "");
    ops::config::save();
    OPS_LOG("Chat", "Deleted ch%d", chIdx);

    if (s_mode == MODE_CHAT && s_activeChIdx == chIdx) {
        show();
    } else {
        _showList();
    }
}

// ── openChannel() ─────────────────────────────────────────────────────

void ScreenHome::openChannel(int chIdx)
{
    if (chIdx >= 0 && chIdx <= 9) {
        s_sendMode    = chIdx;
        s_activeChIdx = chIdx;
        s_chUnread[chIdx] = false;
    }
    s_mode = MODE_CHAT;
    _showChat();
}

// ── openDM() ──────────────────────────────────────────────────────────

void ScreenHome::openDM(const uint8_t* pubKeyPrefix4, const char* name)
{
    memcpy(s_dmPubKey, pubKeyPrefix4, 4);
    strncpy(s_dmName, name, sizeof(s_dmName) - 1);
    s_dmName[sizeof(s_dmName) - 1] = '\0';
    s_sendMode    = 10;
    s_activeChIdx = -1;
    s_mode        = MODE_CHAT;
    _showChat();
}

// ── show() ────────────────────────────────────────────────────────────

void ScreenHome::show()
{
    s_mode = MODE_LIST;
    _showList();
}

// ── appendMessage() ───────────────────────────────────────────────────

void ScreenHome::appendMessage(const RxMessage& msg)
{
    char tagBuf[16] = {};
    const char* tag;
    if (msg.isDirect) {
        snprintf(tagBuf, sizeof(tagBuf), "DM_%.11s", msg.senderName);
        tag = tagBuf;
    } else if (msg.channelName[0]) {
        tag = msg.channelName;
    } else {
        tag = "Public";
    }

    _historyAdd(false, msg.senderName, msg.text, msg.hops, msg.timestamp, msg.rssi, 0, tag,
                msg.pathStr, msg.pubKeyPrefix);

    int slot = _tagToSlot(tag);
    if (slot >= 0) {
        bool isViewing = (s_mode == MODE_CHAT && s_activeChIdx == slot &&
                          _screen && lv_scr_act() == _screen);
        if (!isViewing) {
            s_chUnread[slot] = true;
            if (s_mode == MODE_LIST && s_rowDots[slot]) {
                lv_obj_clear_flag(s_rowDots[slot], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (ops::config::get().channels[slot].notify)
            ops::sound::playNotification();
    }

    const char* viewTag = _getViewTag();
    OPS_LOG("Chat", "appendMessage: tag='%s' viewTag='%s' mode=%d", tag, viewTag, (int)s_mode);
    if (s_mode == MODE_CHAT && strcmp(tag, viewTag) == 0) {
        _addBubble(s_histCount - 1, false,
                   msg.senderName, msg.text, msg.hops, msg.timestamp, msg.rssi, false,
                   msg.pathStr);
    }
}

// ── appendSent() ──────────────────────────────────────────────────────

void ScreenHome::appendSent(const char* text, uint32_t ts, uint32_t expectedAck)
{
    _historyAdd(true, nullptr, text, 0, ts, 0.0f, expectedAck, _getViewTag());
    _addBubble(s_histCount - 1, true, nullptr, text, 0, ts, 0.0f, false);
}

// ── checkPendingAck() ─────────────────────────────────────────────────

void ScreenHome::checkPendingAck()
{
    uint32_t acked_crc;
    if (!ops::MeshService::instance().pollAck(acked_crc)) return;

    for (int i = 0; i < s_histCount; i++) {
        MsgEntry& e = s_history[i];
        if (e.sent && !e.isAcked && e.expectedAck != 0 && e.expectedAck == acked_crc) {
            e.isAcked = true;
            if (s_metaLabels[i]) {
                char timeBuf[8];
                _fmtTime(timeBuf, sizeof(timeBuf), e.ts);
                char meta[32];
                snprintf(meta, sizeof(meta), "%s " LV_SYMBOL_OK, timeBuf);
                lv_label_set_text(s_metaLabels[i], meta);
                lv_obj_set_style_text_color(s_metaLabels[i], theme::GREEN, 0);
            }
            OPS_LOG("Chat", "ACK displayed for msg %d", i);
        }
    }
}

// ── _showList() ───────────────────────────────────────────────────────

void ScreenHome::_showList()
{
    OPS_LOG("UI", "Showing channel list");

    // Nullify overlay pointers — their objects will be deleted with the old screen
    s_actionOverlay     = nullptr;
    s_addOverlay        = nullptr;
    s_addNameTa         = nullptr;
    s_addPskTa          = nullptr;
    s_addScopeTa        = nullptr;
    s_addContactOverlay = nullptr;

    // Capture old screens — delete them AFTER loading the new one (safe LVGL pattern)
    lv_obj_t* oldChat = _screen;
    lv_obj_t* oldList = s_listScreen;
    s_listScreen = nullptr;
    _screen      = nullptr;
    _msgArea     = nullptr;
    _textarea    = nullptr;
    _sendBtn     = nullptr;
    _emojiPanel  = nullptr;
    for (int i = 0; i < HISTORY_MAX; i++) s_metaLabels[i] = nullptr;
    for (int i = 0; i < 10; i++) s_rowDots[i] = nullptr;

    s_listScreen = lv_obj_create(nullptr);
    lv_obj_set_size(s_listScreen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(s_listScreen, theme::BG, 0);
    lv_obj_set_style_pad_all(s_listScreen, 0, 0);
    lv_obj_clear_flag(s_listScreen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ──────────────────────────────────────────────────────
    lv_obj_t* hdr = lv_obj_create(s_listScreen);
    lv_obj_set_size(hdr, OPS_SCREEN_W, 30);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(hdr, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 4, 0);
    lv_obj_set_style_pad_ver(hdr, 2, 0);
    lv_obj_set_style_pad_column(hdr, 6, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* homeBtn = lv_btn_create(hdr);
    lv_obj_set_height(homeBtn, 22);
    lv_obj_set_style_bg_color(homeBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(homeBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(homeBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(homeBtn, 1, 0);
    lv_obj_set_style_radius(homeBtn, 4, 0);
    lv_obj_set_style_shadow_width(homeBtn, 0, 0);
    lv_obj_set_style_pad_hor(homeBtn, 5, 0);
    lv_obj_add_event_cb(homeBtn, _onListBack, LV_EVENT_CLICKED, nullptr);
    lv_group_remove_obj(homeBtn);

    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    lv_obj_t* titleLbl = lv_label_create(hdr);
    lv_label_set_text(titleLbl, "Channels");
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_12, 0);

    // ── Scrollable channel list ──────────────────────────────────────
    lv_obj_t* listBody = lv_obj_create(s_listScreen);
    lv_obj_set_size(listBody, OPS_SCREEN_W, OPS_SCREEN_H - 30);
    lv_obj_align(listBody, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_set_style_bg_color(listBody, theme::BG, 0);
    lv_obj_set_style_border_width(listBody, 0, 0);
    lv_obj_set_style_radius(listBody, 0, 0);
    lv_obj_set_style_pad_all(listBody, 0, 0);
    lv_obj_set_style_pad_row(listBody, 1, 0);
    lv_obj_set_flex_flow(listBody, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(listBody, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(listBody, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(listBody, LV_SCROLLBAR_MODE_AUTO);

    // Register with LVGL group so ESC key reaches _onListKey
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, listBody);
        lv_group_focus_obj(listBody);
    }
    lv_obj_add_event_cb(listBody, _onListKey, LV_EVENT_KEY, nullptr);

    const auto& cfg = ops::config::get();
    int rowIdx = 0;

    // Row width breakdown (OPS_SCREEN_W=320, pad_hor=4 each side):
    //   content = 312 px; dot-cont=14, gap=4, name=224, gap=4, action-btn=30 → 276
    //   action-btn right edge = pad_left(4)+14+4+224+4+30 = 280 px
    //   DM row (no action btn): dot-cont=14, gap=4, name=294 → 312

    auto addRow = [&](int chIdx, const char* name, bool hasDot, bool hasAction) {
        bool unread = (hasDot && chIdx >= 0 && chIdx < 10) ? s_chUnread[chIdx] : false;

        lv_obj_t* row = lv_btn_create(listBody);
        lv_obj_set_size(row, OPS_SCREEN_W, 34);
        lv_obj_set_style_bg_color(row, (rowIdx & 1) ? theme::BG_CARD : theme::BG, 0);
        lv_obj_set_style_bg_color(row, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 4, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_set_style_pad_column(row, 4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_group_remove_obj(row);

        if (chIdx >= 0) {
            lv_obj_add_event_cb(row, _onChannelRow, LV_EVENT_CLICKED, (void*)(intptr_t)chIdx);
        } else {
            lv_obj_add_event_cb(row, _onDMRow, LV_EVENT_CLICKED, nullptr);
        }

        // Unread dot container (non-clickable, 14px wide)
        lv_obj_t* dotCont = lv_obj_create(row);
        lv_obj_set_size(dotCont, 14, 30);
        lv_obj_set_style_bg_opa(dotCont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(dotCont, 0, 0);
        lv_obj_set_style_pad_all(dotCont, 0, 0);
        lv_obj_clear_flag(dotCont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* dot = lv_obj_create(dotCont);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_bg_color(dot, theme::RED, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_shadow_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(dot);
        if (!unread) lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

        if (hasDot && chIdx >= 0 && chIdx < 10) s_rowDots[chIdx] = dot;

        // Channel name label (fixed width)
        lv_obj_t* nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, name);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nameLbl, hasAction ? 224 : 294);
        lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_12, 0);

        // Action button (≡) for channel rows
        if (hasAction) {
            lv_obj_t* actBtn = lv_btn_create(row);
            lv_obj_set_size(actBtn, 30, 28);
            lv_obj_set_style_bg_color(actBtn, theme::BG_CARD, 0);
            lv_obj_set_style_bg_color(actBtn, theme::ACCENT, LV_STATE_PRESSED);
            lv_obj_set_style_border_color(actBtn, theme::BORDER, 0);
            lv_obj_set_style_border_width(actBtn, 1, 0);
            lv_obj_set_style_radius(actBtn, 4, 0);
            lv_obj_set_style_shadow_width(actBtn, 0, 0);
            lv_obj_set_style_pad_all(actBtn, 2, 0);
            lv_group_remove_obj(actBtn);
            lv_obj_add_event_cb(actBtn, _onActionBtn, LV_EVENT_CLICKED,
                                (void*)(intptr_t)chIdx);

            lv_obj_t* actLbl = lv_label_create(actBtn);
            lv_label_set_text(actLbl, LV_SYMBOL_EDIT);
            lv_obj_set_style_text_color(actLbl, theme::TEXT_MUTED, 0);
            lv_obj_set_style_text_font(actLbl, &lv_font_montserrat_10, 0);
            lv_obj_center(actLbl);
        }

        rowIdx++;
    };

    // Slot 0 (Public) — always shown
    {
        const char* name = cfg.channels[0].name[0] ? cfg.channels[0].name : "Public";
        addRow(0, name, true, true);
    }

    // Slots 1-9 — shown if name is configured
    for (int i = 1; i < 10; i++) {
        if (!cfg.channels[i].name[0]) continue;
        addRow(i, cfg.channels[i].name, true, true);
    }

    // Direct Messages entry — always shown at bottom
    addRow(-1, LV_SYMBOL_CALL "  Direct Messages", false, false);

    lv_scr_load(s_listScreen);
    if (oldChat) lv_obj_del(oldChat);
    if (oldList) lv_obj_del(oldList);
}

// ── _showChat() ───────────────────────────────────────────────────────

void ScreenHome::_showChat()
{
    OPS_LOG("UI", "Showing chat (mode=%d)", s_sendMode);

    // Nullify overlay pointers — their objects are about to be deleted with the old screen
    s_actionOverlay     = nullptr;
    s_addOverlay        = nullptr;
    s_addNameTa         = nullptr;
    s_addPskTa          = nullptr;
    s_addScopeTa        = nullptr;
    s_dmPickerOverlay   = nullptr;
    s_addContactOverlay = nullptr;

    // Capture old screens — delete them AFTER loading the new one (safe LVGL pattern)
    lv_obj_t* oldList = s_listScreen;
    lv_obj_t* oldChat = _screen;
    if (oldList) for (int i = 0; i < 10; i++) s_rowDots[i] = nullptr;
    if (oldChat) for (int i = 0; i < HISTORY_MAX; i++) s_metaLabels[i] = nullptr;
    s_listScreen = nullptr;
    _screen      = nullptr;
    _msgArea     = nullptr;
    _textarea    = nullptr;
    _sendBtn     = nullptr;
    _emojiPanel  = nullptr;

    const auto& cfg = ops::config::get();

    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header bar ────────────────────────────────────────────────────
    lv_obj_t* hdr = lv_obj_create(_screen);
    lv_obj_set_size(hdr, OPS_SCREEN_W, 28);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(hdr, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 4, 0);
    lv_obj_set_style_pad_ver(hdr, 2, 0);
    lv_obj_set_style_pad_column(hdr, 6, 0);
    lv_obj_set_scrollbar_mode(hdr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Back button → channel list
    lv_obj_t* backBtn = lv_btn_create(hdr);
    lv_obj_set_height(backBtn, 22);
    lv_obj_set_style_bg_color(backBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(backBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(backBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(backBtn, 1, 0);
    lv_obj_set_style_radius(backBtn, 4, 0);
    lv_obj_set_style_shadow_width(backBtn, 0, 0);
    lv_obj_set_style_pad_hor(backBtn, 5, 0);
    lv_obj_add_event_cb(backBtn, _onChatBack, LV_EVENT_CLICKED, nullptr);
    lv_group_remove_obj(backBtn);

    lv_obj_t* backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(backLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(backLbl);

    // Channel/DM title
    char titleBuf[40] = {};
    if (s_sendMode == 10) {
        snprintf(titleBuf, sizeof(titleBuf), "DM: %.30s", s_dmName);
    } else if (s_sendMode == 0) {
        const char* n = cfg.channels[0].name[0] ? cfg.channels[0].name : "Public";
        snprintf(titleBuf, sizeof(titleBuf), "%s", n);
    } else {
        const char* n = cfg.channels[s_sendMode].name;
        snprintf(titleBuf, sizeof(titleBuf), "%s", n[0] ? n : "Channel");
    }

    lv_obj_t* titleLbl = lv_label_create(hdr);
    lv_label_set_text(titleLbl, titleBuf);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(titleLbl, OPS_SCREEN_W - 70);
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_12, 0);

    // ── Message area ──────────────────────────────────────────────────
    _msgArea = lv_obj_create(_screen);
    lv_obj_set_size(_msgArea, OPS_SCREEN_W, OPS_SCREEN_H - 28 - 36);
    lv_obj_align(_msgArea, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_bg_color(_msgArea, theme::BG, 0);
    lv_obj_set_style_border_width(_msgArea, 0, 0);
    lv_obj_set_style_pad_hor(_msgArea, 4, 0);
    lv_obj_set_style_pad_ver(_msgArea, 4, 0);
    lv_obj_set_style_pad_row(_msgArea, 4, 0);
    lv_obj_set_flex_flow(_msgArea, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_msgArea,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // ── Input bar ─────────────────────────────────────────────────────
    lv_obj_t* inputBar = lv_obj_create(_screen);
    lv_obj_set_size(inputBar, OPS_SCREEN_W, 36);
    lv_obj_align(inputBar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(inputBar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(inputBar, 0, 0);
    lv_obj_set_style_radius(inputBar, 0, 0);
    lv_obj_set_style_pad_all(inputBar, 2, 0);
    lv_obj_clear_flag(inputBar, LV_OBJ_FLAG_SCROLLABLE);

    _textarea = lv_textarea_create(inputBar);
    lv_obj_set_size(_textarea, OPS_SCREEN_W - 80, 30);
    lv_obj_align(_textarea, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(_textarea, theme::BG, 0);
    lv_obj_set_style_text_color(_textarea, theme::TEXT, 0);
    lv_obj_set_style_border_color(_textarea, theme::BORDER, 0);
    lv_obj_set_style_border_color(_textarea, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(_textarea, &lv_font_montserrat_10, 0);
    lv_textarea_set_placeholder_text(_textarea, "Type a message...");
    lv_textarea_set_one_line(_textarea, true);
    lv_obj_add_event_cb(_textarea, _onSend,    LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(_textarea, _onChatKey, LV_EVENT_KEY,   nullptr);

    lv_obj_t* emojiToggle = lv_btn_create(inputBar);
    lv_obj_set_size(emojiToggle, 34, 30);
    lv_obj_align(emojiToggle, LV_ALIGN_RIGHT_MID, -40, 0);
    lv_obj_set_style_bg_color(emojiToggle, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(emojiToggle, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(emojiToggle, 4, 0);
    lv_obj_set_style_shadow_width(emojiToggle, 0, 0);
    lv_obj_set_style_border_width(emojiToggle, 1, 0);
    lv_obj_set_style_border_color(emojiToggle, theme::BORDER, 0);
    lv_obj_add_event_cb(emojiToggle, _onEmojiToggle, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* emojiToggleLbl = lv_label_create(emojiToggle);
    // 😊 U+1F60A in UTF-8
    lv_label_set_text(emojiToggleLbl, "\xF0\x9F\x98\x8A");
    lv_obj_set_style_text_font(emojiToggleLbl, &lv_font_montserrat_12, 0);
    lv_obj_center(emojiToggleLbl);

    _sendBtn = lv_btn_create(inputBar);
    lv_obj_set_size(_sendBtn, 36, 30);
    lv_obj_align(_sendBtn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(_sendBtn, theme::PRIMARY, 0);
    lv_obj_set_style_bg_color(_sendBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(_sendBtn, 4, 0);
    lv_obj_set_style_shadow_width(_sendBtn, 0, 0);
    lv_obj_add_event_cb(_sendBtn, _onSend,    LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(_sendBtn, _onChatKey, LV_EVENT_KEY,     nullptr);

    lv_obj_t* sendLbl = lv_label_create(_sendBtn);
    lv_label_set_text(sendLbl, LV_SYMBOL_RIGHT);
    lv_obj_center(sendLbl);

    lv_group_t* grp = lv_group_get_default();
    if (grp) {
        lv_group_add_obj(grp, _textarea);
        lv_group_add_obj(grp, _sendBtn);
        lv_group_focus_obj(_textarea);
    }

    // Clear unread for this channel
    if (s_sendMode >= 0 && s_sendMode < 10)
        s_chUnread[s_sendMode] = false;

    lv_scr_load(_screen);
    if (oldList) lv_obj_del(oldList);
    if (oldChat) lv_obj_del(oldChat);

    _loadChannelHistory(_getViewTag());
    _rebuildMsgArea();
}

// ── _openActionPopup() ────────────────────────────────────────────────

void ScreenHome::_openActionPopup(int chIdx)
{
    if (s_actionOverlay) { lv_obj_del(s_actionOverlay); s_actionOverlay = nullptr; }

    const auto& cfg = ops::config::get();
    const char* chName = (chIdx == 0)
        ? (cfg.channels[0].name[0] ? cfg.channels[0].name : "Public")
        : cfg.channels[chIdx].name;

    s_actionOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_actionOverlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_actionOverlay, 0, 0);
    lv_obj_set_style_bg_color(s_actionOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_actionOverlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_actionOverlay, 0, 0);
    lv_obj_clear_flag(s_actionOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(s_actionOverlay);
    lv_obj_set_size(box, 220, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);  // absorbs clicks, prevents overlay close
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title
    char titleBuf[28];
    snprintf(titleBuf, sizeof(titleBuf), "%.24s", chName);
    lv_obj_t* tLbl = lv_label_create(box);
    lv_label_set_text(tLbl, titleBuf);
    lv_obj_set_style_text_color(tLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(tLbl, &lv_font_montserrat_12, 0);
    lv_obj_set_width(tLbl, LV_PCT(100));

    auto mkBtn = [&](const char* label, lv_color_t bg, lv_event_cb_t cb, void* ud) {
        lv_obj_t* btn = lv_btn_create(box);
        lv_obj_set_size(btn, 200, 30);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_group_remove_obj(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_center(lbl);
    };

    mkBtn("Add Channel",    theme::PRIMARY, _onActionAdd,    (void*)(intptr_t)chIdx);
    mkBtn("Clear Messages", theme::ORANGE,  _onActionClear,  (void*)(intptr_t)chIdx);
    {
        bool notifyOn = cfg.channels[chIdx].notify;
        char notifyLabel[20];
        snprintf(notifyLabel, sizeof(notifyLabel), "Notify: %s", notifyOn ? "On" : "Off");
        mkBtn(notifyLabel, notifyOn ? theme::GREEN : theme::BG, _onActionNotify, (void*)(intptr_t)chIdx);
    }
    if (chIdx != 0)
        mkBtn("Delete Channel", theme::RED, _onActionDelete, (void*)(intptr_t)chIdx);
    mkBtn("Close",          theme::BG,      _onActionClose,  nullptr);
}

// ── Add-channel dialog helpers ────────────────────────────────────────

static void _randomPsk(char* out, int size)
{
    if (size < 25) return;
    uint8_t raw[16];
    for (int i = 0; i < 4; i++) {
        uint32_t r = esp_random();
        raw[i*4+0] = (uint8_t)(r >> 24);
        raw[i*4+1] = (uint8_t)(r >> 16);
        raw[i*4+2] = (uint8_t)(r >>  8);
        raw[i*4+3] = (uint8_t)(r);
    }
    static const char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int b = 0, j = 0; b < 15; b += 3, j += 4) {
        uint32_t v = ((uint32_t)raw[b] << 16) | ((uint32_t)raw[b+1] << 8) | raw[b+2];
        out[j+0] = kB64[(v >> 18) & 63];
        out[j+1] = kB64[(v >> 12) & 63];
        out[j+2] = kB64[(v >>  6) & 63];
        out[j+3] = kB64[v & 63];
    }
    uint32_t v = (uint32_t)raw[15] << 16;
    out[20] = kB64[(v >> 18) & 63];
    out[21] = kB64[(v >> 12) & 63];
    out[22] = '='; out[23] = '='; out[24] = '\0';
}

// Auto-fill PSK field when channel name changes:
//   name starts '#' → derive standard PSK (deriveChannelPsk prepends '#', so pass name+1)
//   name doesn't start '#' → clear field (will get a random PSK on save)
static void _onAddNameChanged(lv_event_t* /*e*/)
{
    if (!s_addNameTa || !s_addPskTa) return;
    const char* name = lv_textarea_get_text(s_addNameTa);
    if (name && name[0] == '#' && name[1]) {
        char psk[28] = {};
        ops::MeshService::instance().deriveChannelPsk(name + 1, psk, sizeof(psk));
        lv_textarea_set_text(s_addPskTa, psk);
    } else {
        lv_textarea_set_text(s_addPskTa, "");
    }
}

// ── _openAddChannelDialog() ───────────────────────────────────────────

void ScreenHome::_openAddChannelDialog()
{
    if (s_addOverlay) { lv_obj_del(s_addOverlay); s_addOverlay = nullptr; }
    s_addNameTa = s_addPskTa = s_addScopeTa = nullptr;

    s_addOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_addOverlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_addOverlay, 0, 0);
    lv_obj_set_style_bg_color(s_addOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_addOverlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_addOverlay, 0, 0);
    lv_obj_clear_flag(s_addOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(s_addOverlay);
    lv_obj_set_size(box, 260, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* titleLbl = lv_label_create(box);
    lv_label_set_text(titleLbl, "Add Channel");
    lv_obj_set_style_text_color(titleLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_12, 0);

    auto mkLabel = [&](const char* text) {
        lv_obj_t* lbl = lv_label_create(box);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    };

    auto mkTa = [&](int maxLen) -> lv_obj_t* {
        lv_obj_t* ta = lv_textarea_create(box);
        lv_obj_set_size(ta, 238, 28);
        lv_obj_set_style_bg_color(ta, theme::BG, 0);
        lv_obj_set_style_text_color(ta, theme::TEXT, 0);
        lv_obj_set_style_border_color(ta, theme::BORDER, 0);
        lv_obj_set_style_border_color(ta, theme::ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_text_font(ta, &lv_font_montserrat_10, 0);
        lv_textarea_set_one_line(ta, true);
        if (maxLen > 0) lv_textarea_set_max_length(ta, (uint32_t)maxLen);
        return ta;
    };

    mkLabel("Name (start with # for standard PSK):");
    s_addNameTa = mkTa(31);
    lv_textarea_set_placeholder_text(s_addNameTa, "e.g. #Mesh or MyGroup");
    lv_obj_add_event_cb(s_addNameTa, _onAddNameChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    mkLabel("PSK (auto-filled or edit to override):");
    s_addPskTa = mkTa(32);
    lv_textarea_set_placeholder_text(s_addPskTa, "auto-random if blank");

    mkLabel("Scope (optional, e.g. AU):");
    s_addScopeTa = mkTa(15);
    lv_textarea_set_placeholder_text(s_addScopeTa, "leave blank for no scope");

    // Button row
    lv_obj_t* btnRow = lv_obj_create(box);
    lv_obj_set_size(btnRow, 238, 34);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_style_pad_column(btnRow, 8, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, 110, 30);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, 0);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_border_width(saveBtn, 0, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_group_remove_obj(saveBtn);
    lv_obj_add_event_cb(saveBtn, _onAddSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, "Save");
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(saveLbl, theme::TEXT, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* cancelBtn = lv_btn_create(btnRow);
    lv_obj_set_size(cancelBtn, 110, 30);
    lv_obj_set_style_bg_color(cancelBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(cancelBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(cancelBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(cancelBtn, 1, 0);
    lv_obj_set_style_radius(cancelBtn, 4, 0);
    lv_obj_set_style_shadow_width(cancelBtn, 0, 0);
    lv_group_remove_obj(cancelBtn);
    lv_obj_add_event_cb(cancelBtn, _onAddCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
    lv_label_set_text(cancelLbl, "Cancel");
    lv_obj_set_style_text_font(cancelLbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cancelLbl, theme::TEXT, 0);
    lv_obj_center(cancelLbl);

    // Focus the name field
    lv_group_t* grp = lv_group_get_default();
    if (grp) {
        lv_group_add_obj(grp, s_addNameTa);
        lv_group_add_obj(grp, s_addPskTa);
        lv_group_add_obj(grp, s_addScopeTa);
        lv_group_focus_obj(s_addNameTa);
    }
}

// ── _openDMPicker() ───────────────────────────────────────────────────

void ScreenHome::_openDMPicker()
{
    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, _onDMPickerClose, LV_EVENT_CLICKED, overlay);
    s_dmPickerOverlay = overlay;

    lv_obj_t* box = lv_obj_create(overlay);
    lv_obj_set_size(box, 260, 190);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hdr = lv_label_create(box);
    lv_label_set_text(hdr, "Select DM recipient");
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hdr, theme::ACCENT, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t* list = lv_obj_create(box);
    lv_obj_set_size(list, 258, 162);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 1, 24);
    lv_obj_set_style_bg_color(list, theme::BG, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    int rowIdx = 0;
    auto addPickerRow = [&](const char* name, const char* addr, int encoded) {
        lv_obj_t* row = lv_btn_create(list);
        lv_group_remove_obj(row);
        lv_obj_set_size(row, 258, 26);
        lv_obj_set_style_bg_color(row, (rowIdx & 1) ? theme::BG_CARD : theme::BG, 0);
        lv_obj_set_style_bg_color(row, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 6, 0);
        lv_obj_set_style_pad_right(row, 4, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, _onDMPickerRow, LV_EVENT_CLICKED, (void*)(intptr_t)encoded);

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, name);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nameLbl, 190);
        lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* addrLbl = lv_label_create(row);
        lv_label_set_text(addrLbl, addr);
        lv_obj_set_width(addrLbl, 56);
        lv_obj_set_style_text_color(addrLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(addrLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_align(addrLbl, LV_TEXT_ALIGN_RIGHT, 0);
        rowIdx++;
    };

    int cCnt = ops::contacts::count();
    for (int i = 0; i < cCnt; i++) {
        ops::Contact c;
        if (!ops::contacts::get(i, c)) continue;
        char addr[8];
        snprintf(addr, sizeof(addr), "%02X%02X%02X",
                 c.pubKeyPrefix[0], c.pubKeyPrefix[1], c.pubKeyPrefix[2]);
        addPickerRow(c.name, addr, i);
    }

    int pCnt = ops::MeshService::instance().peerCount();
    for (int i = 0; i < pCnt; i++) {
        ops::PeerInfo p;
        if (!ops::MeshService::instance().getPeer(i, p)) continue;
        if (p.type == 2) continue;
        int dummy;
        if (ops::contacts::findByKey(p.pubKeyPrefix, &dummy)) continue;
        char addr[8];
        snprintf(addr, sizeof(addr), "%02X%02X%02X",
                 p.pubKeyPrefix[0], p.pubKeyPrefix[1], p.pubKeyPrefix[2]);
        char peerName[36];
        snprintf(peerName, sizeof(peerName), "%s *", p.name);
        addPickerRow(peerName, addr, -(i + 1));
    }

    if (rowIdx == 0) {
        lv_obj_t* empty = lv_label_create(list);
        lv_label_set_text(empty,
            "No contacts or heard peers.\n"
            "Go to Heard to discover nodes.");
        lv_obj_set_style_text_color(empty, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_10, 0);
        lv_obj_set_style_pad_all(empty, 8, 0);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, 240);
    }
}

// ── _onSend() ─────────────────────────────────────────────────────────

void ScreenHome::_onSend(lv_event_t* /*e*/)
{
    if (!_textarea || !_msgArea) return;
    const char* txt = lv_textarea_get_text(_textarea);
    if (!txt || txt[0] == '\0') return;

    if (!ops::MeshService::instance().initialized()) {
        OPS_LOG("Chat", "Send failed: radio not ready");
        lv_obj_t* row = lv_obj_create(_msgArea);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Radio not ready - not sent");
        lv_obj_set_style_text_color(lbl, theme::RED, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_scroll_to_y(_msgArea, LV_COORD_MAX, LV_ANIM_OFF);
        return;
    }

    uint32_t ts = (uint32_t)time(nullptr);
    bool ok = false;
    uint32_t expectedAck = 0;

    if (s_sendMode == 10) {
        bool hasTarget = s_dmPubKey[0] || s_dmPubKey[1] || s_dmPubKey[2] || s_dmPubKey[3];
        if (hasTarget) {
            ok = ops::MeshService::instance().sendDirect(s_dmPubKey, txt);
            if (ok) expectedAck = ops::MeshService::instance().lastExpectedAck();
        } else {
            OPS_LOG("Chat", "DM mode but no target");
        }
    } else {
        ok = ops::MeshService::instance().sendChannel(s_sendMode, txt);
    }
    if (!ok) {
        OPS_LOG("Chat", "Send returned false");
        lv_obj_t* row = lv_obj_create(_msgArea);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lbl = lv_label_create(row);
        const char* errMsg = (s_sendMode == 10)
            ? "Contact not reachable - not sent"
            : "Send failed - not sent";
        lv_label_set_text(lbl, errMsg);
        lv_obj_set_style_text_color(lbl, theme::RED, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_scroll_to_y(_msgArea, LV_COORD_MAX, LV_ANIM_OFF);
        return;
    }

    appendSent(txt, ts, expectedAck);
    lv_textarea_set_text(_textarea, "");
}

// ── Navigation callbacks ──────────────────────────────────────────────

void ScreenHome::_onListBack(lv_event_t* /*e*/)
{
    ScreenLauncher::show();
}

void ScreenHome::_onChatBack(lv_event_t* /*e*/)
{
    s_mode = MODE_LIST;
    _showList();
}

void ScreenHome::_onListKey(lv_event_t* e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC) ScreenLauncher::show();
}

void ScreenHome::_onChatKey(lv_event_t* e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC) {
        s_mode = MODE_LIST;
        _showList();
    }
}

// ── List row callbacks ────────────────────────────────────────────────

void ScreenHome::_onChannelRow(lv_event_t* e)
{
    int chIdx = (int)(intptr_t)lv_event_get_user_data(e);
    s_sendMode    = chIdx;
    s_activeChIdx = chIdx;
    s_mode        = MODE_CHAT;
    s_chUnread[chIdx] = false;
    _showChat();
}

void ScreenHome::_onDMRow(lv_event_t* /*e*/)
{
    _openDMPicker();
}

void ScreenHome::_onActionBtn(lv_event_t* e)
{
    int chIdx = (int)(intptr_t)lv_event_get_user_data(e);
    _openActionPopup(chIdx);
}

// ── Action popup callbacks ────────────────────────────────────────────

void ScreenHome::_onActionAdd(lv_event_t* /*e*/)
{
    if (s_actionOverlay) { lv_obj_del(s_actionOverlay); s_actionOverlay = nullptr; }
    _openAddChannelDialog();
}

void ScreenHome::_onActionClear(lv_event_t* e)
{
    int chIdx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_actionOverlay) { lv_obj_del(s_actionOverlay); s_actionOverlay = nullptr; }
    _clearChannelMessages(chIdx);
    if (s_mode == MODE_LIST) _showList();
}

void ScreenHome::_onActionDelete(lv_event_t* e)
{
    int chIdx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_actionOverlay) { lv_obj_del(s_actionOverlay); s_actionOverlay = nullptr; }
    _deleteChannel(chIdx);  // calls _showList() or show() internally
}

void ScreenHome::_onActionNotify(lv_event_t* e)
{
    int chIdx = (int)(intptr_t)lv_event_get_user_data(e);
    bool newState = !ops::config::get().channels[chIdx].notify;
    ops::config::setChannelNotify(chIdx, newState);
    // Update the button label and colour in-place
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_label_set_text(lbl, newState ? "Notify: On" : "Notify: Off");
    lv_obj_set_style_bg_color(btn, newState ? theme::GREEN : theme::BG, 0);
}

void ScreenHome::_onActionClose(lv_event_t* /*e*/)
{
    if (s_actionOverlay) { lv_obj_del(s_actionOverlay); s_actionOverlay = nullptr; }
}

// ── Add channel dialog callbacks ──────────────────────────────────────

void ScreenHome::_onAddSave(lv_event_t* /*e*/)
{
    if (!s_addNameTa || !s_addPskTa) return;
    const char* name       = lv_textarea_get_text(s_addNameTa);
    const char* pskField   = lv_textarea_get_text(s_addPskTa);
    const char* scopeField = s_addScopeTa ? lv_textarea_get_text(s_addScopeTa) : "";

    if (!name || !name[0]) {
        if (s_addOverlay) { lv_obj_del(s_addOverlay); s_addOverlay = nullptr; }
        s_addNameTa = s_addPskTa = s_addScopeTa = nullptr;
        return;
    }

    int slot = -1;
    const auto& cfg = ops::config::get();
    for (int i = 1; i < 10; i++) {
        if (!cfg.channels[i].name[0]) { slot = i; break; }
    }

    if (slot >= 0) {
        // Strip leading '#' — stored name is plain (consistent with Settings)
        const char* nameBase = (name[0] == '#' && name[1]) ? name + 1 : name;

        char psk[28] = {};
        if (pskField && pskField[0]) {
            ops::MeshService::normalizePsk(pskField, psk, sizeof(psk));
        } else if (name[0] == '#' && name[1]) {
            ops::MeshService::deriveChannelPsk(nameBase, psk, sizeof(psk));
        } else {
            _randomPsk(psk, sizeof(psk));
        }

        char sh[6] = {};
        strncpy(sh, nameBase, 5);

        ops::config::setChannel(slot, nameBase, psk, sh, scopeField ? scopeField : "");
        ops::MeshService::instance().syncChannel(slot);
        OPS_LOG("Chat", "Added channel '%s' psk=%.6s... scope='%s' slot %d",
                nameBase, psk, scopeField ? scopeField : "", slot);
    } else {
        OPS_LOG("Chat", "No empty channel slot available");
    }

    if (s_addOverlay) { lv_obj_del(s_addOverlay); s_addOverlay = nullptr; }
    s_addNameTa = s_addPskTa = s_addScopeTa = nullptr;
    _showList();
}

void ScreenHome::_onAddCancel(lv_event_t* /*e*/)
{
    if (s_addOverlay) { lv_obj_del(s_addOverlay); s_addOverlay = nullptr; }
    s_addNameTa = s_addPskTa = s_addScopeTa = nullptr;
}

// ── DM picker callbacks ───────────────────────────────────────────────

void ScreenHome::_onDMPickerRow(lv_event_t* e)
{
    int encoded = (int)(intptr_t)lv_event_get_user_data(e);
    bool found  = false;

    if (encoded >= 0) {
        ops::Contact c;
        if (ops::contacts::get(encoded, c)) {
            memcpy(s_dmPubKey, c.pubKeyPrefix, 4);
            strncpy(s_dmName, c.name, sizeof(s_dmName) - 1);
            s_dmName[sizeof(s_dmName) - 1] = '\0';
            found = true;
        }
    } else {
        int pIdx = -(encoded + 1);
        ops::PeerInfo p;
        if (ops::MeshService::instance().getPeer(pIdx, p)) {
            memcpy(s_dmPubKey, p.pubKeyPrefix, 4);
            strncpy(s_dmName, p.name, sizeof(s_dmName) - 1);
            s_dmName[sizeof(s_dmName) - 1] = '\0';
            found = true;
        }
    }

    if (s_dmPickerOverlay) {
        lv_obj_del(s_dmPickerOverlay);
        s_dmPickerOverlay = nullptr;
    }

    if (found) {
        s_sendMode    = 10;
        s_activeChIdx = -1;
        s_mode        = MODE_CHAT;
        _showChat();
        OPS_LOG("Chat", "DM target: %s", s_dmName);
    }
}

void ScreenHome::_onDMPickerClose(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    s_dmPickerOverlay = nullptr;
}

// ── Bubble action menu (Reply / Add Contact) ─────────────────────────

void ScreenHome::_onBubbleClick(lv_event_t* e)
{
    int histIdx = (int)(intptr_t)lv_event_get_user_data(e);
    if (histIdx < 0 || histIdx >= s_histCount) return;
    const MsgEntry& entry = s_history[histIdx];
    if (entry.sent) return;

    strncpy(s_pendingContactName, entry.senderName, sizeof(s_pendingContactName) - 1);
    s_pendingContactName[sizeof(s_pendingContactName) - 1] = '\0';
    memcpy(s_pendingContactKey, entry.pubKeyPrefix, 4);
    _openBubbleActionMenu();
}

void ScreenHome::_openBubbleActionMenu()
{
    if (s_addContactOverlay) { lv_obj_del(s_addContactOverlay); s_addContactOverlay = nullptr; }

    bool hasKey = s_pendingContactKey[0] || s_pendingContactKey[1] ||
                  s_pendingContactKey[2] || s_pendingContactKey[3];
    bool alreadySaved = hasKey && ops::contacts::findByKey(s_pendingContactKey, nullptr);

    s_addContactOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_addContactOverlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_addContactOverlay, 0, 0);
    lv_obj_set_style_bg_color(s_addContactOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_addContactOverlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_addContactOverlay, 0, 0);
    lv_obj_clear_flag(s_addContactOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(s_addContactOverlay);
    lv_obj_set_size(box, 220, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Sender name + key hint
    lv_obj_t* nameLbl = lv_label_create(box);
    lv_label_set_text(nameLbl, s_pendingContactName);
    lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(nameLbl, 200);

    char keyHint[12];
    snprintf(keyHint, sizeof(keyHint), "%02X%02X%02X%02X",
             s_pendingContactKey[0], s_pendingContactKey[1],
             s_pendingContactKey[2], s_pendingContactKey[3]);
    lv_obj_t* keyLbl = lv_label_create(box);
    lv_label_set_text(keyLbl, keyHint);
    lv_obj_set_style_text_color(keyLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(keyLbl, &lv_font_montserrat_10, 0);

    auto mkBtn = [&](const char* label, lv_color_t bg, lv_event_cb_t cb, bool disabled) {
        lv_obj_t* btn = lv_btn_create(box);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 200, 30);
        lv_obj_set_style_bg_color(btn, disabled ? theme::BG : bg, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, theme::BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        if (!disabled)
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        else
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);  // keep clickable but no cb
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, disabled ? theme::TEXT_MUTED : theme::TEXT, 0);
        lv_obj_center(lbl);
    };

    mkBtn(LV_SYMBOL_LEFT " Reply", theme::PRIMARY, _onBubbleReply, false);
    if (hasKey)
        mkBtn(alreadySaved ? LV_SYMBOL_OK " Already in Contacts" : LV_SYMBOL_PLUS " Add Contact",
              alreadySaved ? theme::BG : theme::PRIMARY,
              _onBubbleAddContact,
              alreadySaved);
    mkBtn("Close", theme::BG, _onAddContactCancel, false);
}

void ScreenHome::_onBubbleReply(lv_event_t* /*e*/)
{
    if (s_addContactOverlay) { lv_obj_del(s_addContactOverlay); s_addContactOverlay = nullptr; }
    if (!_textarea) return;
    char prefix[36];
    snprintf(prefix, sizeof(prefix), "@%s ", s_pendingContactName);
    lv_textarea_set_text(_textarea, prefix);
    // Move cursor to end so the user types after the mention.
    lv_textarea_set_cursor_pos(_textarea, LV_TEXTAREA_CURSOR_LAST);
}

void ScreenHome::_onBubbleAddContact(lv_event_t* /*e*/)
{
    _openAddContactPopup();
}

void ScreenHome::_openAddContactPopup()
{
    if (s_addContactOverlay) { lv_obj_del(s_addContactOverlay); s_addContactOverlay = nullptr; }

    // Check whether already saved
    int existIdx = -1;
    bool alreadySaved = ops::contacts::findByKey(s_pendingContactKey, &existIdx);

    s_addContactOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_addContactOverlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_addContactOverlay, 0, 0);
    lv_obj_set_style_bg_color(s_addContactOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_addContactOverlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_addContactOverlay, 0, 0);
    lv_obj_clear_flag(s_addContactOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(s_addContactOverlay);
    lv_obj_set_size(box, 240, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* titleLbl = lv_label_create(box);
    lv_label_set_text(titleLbl, alreadySaved
        ? LV_SYMBOL_OK " Already in Contacts"
        : LV_SYMBOL_PLUS " Add to Contacts");
    lv_obj_set_style_text_color(titleLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_12, 0);

    lv_obj_t* nameLbl = lv_label_create(box);
    lv_label_set_text(nameLbl, s_pendingContactName);
    lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_14, 0);

    // Key hint
    char keyHint[12];
    snprintf(keyHint, sizeof(keyHint), "%02X%02X%02X%02X",
             s_pendingContactKey[0], s_pendingContactKey[1],
             s_pendingContactKey[2], s_pendingContactKey[3]);
    lv_obj_t* keyLbl = lv_label_create(box);
    lv_label_set_text(keyLbl, keyHint);
    lv_obj_set_style_text_color(keyLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(keyLbl, &lv_font_montserrat_10, 0);

    // Button row
    lv_obj_t* btnRow = lv_obj_create(box);
    lv_obj_set_size(btnRow, 220, 34);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_style_pad_column(btnRow, 8, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        alreadySaved ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_SPACE_BETWEEN,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto mkBtn = [&](const char* label, lv_color_t bg, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_btn_create(btnRow);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 100, 30);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, theme::BORDER, 0);
        lv_obj_set_style_border_width(btn, bg.full == theme::BG.full ? 1 : 0, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_center(lbl);
    };

    if (!alreadySaved)
        mkBtn("Add", theme::PRIMARY, _onAddContactSave);
    mkBtn("Close", theme::BG, _onAddContactCancel);
}

void ScreenHome::_onAddContactSave(lv_event_t* /*e*/)
{
    // Guard: double-check it isn't already saved (could have arrived between tap and confirm).
    int dummy;
    if (!ops::contacts::findByKey(s_pendingContactKey, &dummy))
    {
        ops::Contact c = {};
        strncpy(c.name, s_pendingContactName, sizeof(c.name) - 1);
        memcpy(c.pubKeyPrefix, s_pendingContactKey, 4);
        c.lastSeen  = (uint32_t)time(nullptr);
        c.outPathLen = 0xFF;   // unknown until an advert is heard
        ops::contacts::add(c);
        ops::contacts::save();
        OPS_LOG("Chat", "Added contact '%s' %02X%02X%02X%02X from bubble tap",
                c.name,
                c.pubKeyPrefix[0], c.pubKeyPrefix[1],
                c.pubKeyPrefix[2], c.pubKeyPrefix[3]);
    }
    if (s_addContactOverlay) { lv_obj_del(s_addContactOverlay); s_addContactOverlay = nullptr; }
}

void ScreenHome::_onAddContactCancel(lv_event_t* /*e*/)
{
    if (s_addContactOverlay) { lv_obj_del(s_addContactOverlay); s_addContactOverlay = nullptr; }
}

// ── Emoji picker ───────────────────────────────────────────────────────

static void _codeToUtf8(uint32_t cp, char* buf)
{
    if (cp <= 0x7Fu) {
        buf[0] = (char)cp; buf[1] = '\0';
    } else if (cp <= 0x7FFu) {
        buf[0] = (char)(0xC0u | (cp >> 6));
        buf[1] = (char)(0x80u | (cp & 0x3Fu));
        buf[2] = '\0';
    } else if (cp <= 0xFFFFu) {
        buf[0] = (char)(0xE0u | (cp >> 12));
        buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (char)(0x80u | (cp & 0x3Fu));
        buf[3] = '\0';
    } else {
        buf[0] = (char)(0xF0u | (cp >> 18));
        buf[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        buf[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[3] = (char)(0x80u | (cp & 0x3Fu));
        buf[4] = '\0';
    }
}

void ScreenHome::_onEmojiToggle(lv_event_t* /*e*/)
{
    if (_emojiPanel && lv_obj_is_valid(_emojiPanel)) {
        lv_obj_del(_emojiPanel);
        _emojiPanel = nullptr;
        return;
    }

    _emojiPanel = lv_obj_create(_screen);
    lv_obj_set_size(_emojiPanel, OPS_SCREEN_W, 90);
    lv_obj_align(_emojiPanel, LV_ALIGN_BOTTOM_LEFT, 0, -36);
    lv_obj_set_style_bg_color(_emojiPanel, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(_emojiPanel, 0, 0);
    lv_obj_set_style_radius(_emojiPanel, 0, 0);
    lv_obj_set_style_pad_all(_emojiPanel, 2, 0);
    lv_obj_set_style_pad_row(_emojiPanel, 2, 0);
    lv_obj_set_style_pad_column(_emojiPanel, 2, 0);
    lv_obj_clear_flag(_emojiPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_emojiPanel, LV_FLEX_FLOW_ROW_WRAP);

    for (int i = 0; i < kOpsEmojiCount; i++) {
        lv_obj_t* btn = lv_btn_create(_emojiPanel);
        lv_obj_set_size(btn, 60, 26);
        lv_obj_set_style_bg_color(btn, theme::BG, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 2, 0);
        lv_obj_add_event_cb(btn, _onEmojiInsert, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(
                                static_cast<uintptr_t>(kOpsEmoji[i].codepoint)));

        char utf8[5] = {};
        _codeToUtf8(kOpsEmoji[i].codepoint, utf8);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, utf8);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
    }
}

void ScreenHome::_onEmojiInsert(lv_event_t* e)
{
    uint32_t cp = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    char utf8[5] = {};
    _codeToUtf8(cp, utf8);
    if (_textarea && lv_obj_is_valid(_textarea))
        lv_textarea_add_text(_textarea, utf8);
    if (_emojiPanel && lv_obj_is_valid(_emojiPanel)) {
        lv_obj_del(_emojiPanel);
        _emojiPanel = nullptr;
    }
}

}}  // namespace ops::ui

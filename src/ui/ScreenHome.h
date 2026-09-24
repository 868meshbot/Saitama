// Saitama — ScreenHome.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once

#include <lvgl.h>
#include "../mesh/MeshService.h"

namespace ops { namespace ui {

class ScreenHome {
public:
    // Show the channel list (LIST mode).
    static void show();

    // Navigate directly to a channel (0-9) in CHAT mode.
    static void openChannel(int chIdx);

    // Navigate directly to DM mode for the given contact.
    static void openDM(const uint8_t* pubKeyPrefix4, const char* name);

    // Called by UIScreen::tick for every received mesh message.
    static void appendMessage(const RxMessage& msg);

    // Called by _onSend for outgoing messages.
    // expectedAck: CRC the mesh stack will ACK; 0 for channel (flood) messages.
    static void appendSent(const char* text, uint32_t ts, uint32_t expectedAck = 0);

    // Called each tick to update sent-bubble check indicator if ACK has arrived.
    static void checkPendingAck();

private:
    enum Mode { MODE_LIST, MODE_CHAT };

    struct MsgEntry {
        bool     sent;
        char     senderName[32];
        char     text[160];
        char     channelTag[32];  // "Public", channel name, "DM_<name>", etc.
        char     pathStr[36];     // repeater codes, e.g. "AA>B1" — empty for sent/SD-loaded
        uint8_t  pubKeyPrefix[4]; // sender's first 4 key bytes; zeros for channel msgs/SD-loaded
        uint8_t  hops;
        uint32_t ts;
        float    rssi;
        uint32_t expectedAck;
        bool     isAcked;
    };

    // 200 slots shared across all channels/DMs, ps_malloc'd in PSRAM on first
    // use. Eviction is per-tag (see _historyAdd) so a busy channel/DM can only
    // push out its own oldest messages, never another conversation's.
    static constexpr int HISTORY_MAX = 200;
    static MsgEntry*  s_history;    // allocated by _ensureHistoryAlloc()
    static int        s_histCount;
    static lv_obj_t*  s_metaLabels[HISTORY_MAX];

    // ── Mode state ────────────────────────────────────────────────────
    static Mode     s_mode;
    static int      s_activeChIdx;   // 0-9 when CHAT channel, -1 for DM

    // ── List-row unread/summary state ───────────────────────────────────
    static uint16_t  s_chUnreadCount[10]; // per-channel unread message count
    static uint16_t  s_dmUnreadCount;     // aggregate unread count across all DMs
    static lv_obj_t* s_rowBlob[10];       // unread-count blob container, per channel row
    static lv_obj_t* s_rowBlobLbl[10];    // count label inside the blob, per channel row
    static lv_obj_t* s_rowSubtitle[10];   // "last message" subtitle label, per channel row
    static lv_obj_t* s_dmRowBlob;         // DM row unread-count blob container
    static lv_obj_t* s_dmRowBlobLbl;      // DM row count label
    static lv_obj_t* s_dmRowSubtitle;     // DM row "last message" subtitle label

    // ── Send mode ─────────────────────────────────────────────────────
    // 0-9 = channel slot, 10 = DM to s_dmPubKey/s_dmName
    static int      s_sendMode;
    static uint8_t  s_dmPubKey[4];
    static char     s_dmName[32];

    // ── LVGL objects ──────────────────────────────────────────────────
    static lv_obj_t* s_listScreen;
    static lv_obj_t* _screen;      // chat screen
    static lv_obj_t* _msgArea;
    static lv_obj_t* _textarea;
    static lv_obj_t* _sendBtn;
    static lv_obj_t* _emojiPanel;  // emoji picker overlay (nullptr = hidden)

    // ── SD log state ──────────────────────────────────────────────────
    static char s_loadedTags[10][32];
    static int  s_loadedTagCnt;
    static bool s_loadingFromSD;

    // ── History helpers ───────────────────────────────────────────────
    static bool _ensureHistoryAlloc();
    static void _historyAdd(bool sent, const char* sender, const char* text,
                            uint8_t hops, uint32_t ts, float rssi,
                            uint32_t expectedAck = 0,
                            const char* channelTag = nullptr,
                            const char* pathStr = nullptr,
                            const uint8_t* pubKeyPrefix = nullptr);
    static const char* _getViewTag();
    static int         _tagToSlot(const char* tag);
    static void        _rebuildMsgArea();
    static void        _addBubble(int histIdx, bool sent, const char* senderName,
                                  const char* text, uint8_t hops, uint32_t ts,
                                  float rssi, bool isAcked,
                                  const char* pathStr = nullptr);
    static void        _fmtTime(char* buf, size_t len, uint32_t ts);
    static void        _loadChannelHistory(const char* tag);

    // ── Screen builders ───────────────────────────────────────────────
    static void _showList();
    static void _showChat();

    // ── List-row live update helpers ────────────────────────────────────
    // Update a channel row's subtitle + unread blob in place (list must be showing).
    static void _updateChannelRowSummary(int chIdx, const char* sender, uint32_t ts);
    // Update the aggregate DM row's subtitle + unread blob in place.
    static void _updateDMRowSummary(const char* sender, uint32_t ts);

    // ── Action popup + dialogs ────────────────────────────────────────
    static void _openActionPopup    (int chIdx);
    static void _openAddChannelDialog();
    static void _clearChannelMessages(int chIdx);
    static void _deleteChannel      (int chIdx);
    static void _openDMPicker();
    static void _openBubbleActionMenu();
    static void _openAddContactPopup();
    static void _openPathView();
    static void _openIconPicker(int chIdx);   // avatar tapped → emoji/initials picker

    // ── Event callbacks ───────────────────────────────────────────────
    static void _onListBack    (lv_event_t* e);   // list  → launcher
    static void _onChatBack    (lv_event_t* e);   // chat  → list
    static void _onListKey     (lv_event_t* e);   // ESC on list
    static void _onChatKey     (lv_event_t* e);   // ESC on chat
    static void _onChannelRow  (lv_event_t* e);   // channel row tapped
    static void _onDMRow       (lv_event_t* e);   // DM row tapped
    static void _onActionBtn   (lv_event_t* e);   // ≡ button tapped
    static void _onAvatarClick (lv_event_t* e);   // channel avatar tapped → icon picker
    static void _onIconInitialsSave(lv_event_t* e); // icon picker → initials Save
    static void _onIconEmojiOpen   (lv_event_t* e); // icon picker → Choose Emoji
    static void _onIconEmojiPick   (lv_event_t* e); // emoji grid → emoji selected
    static void _onIconPickerClose (lv_event_t* e); // icon picker → Close/Cancel

    static void _onActionAdd   (lv_event_t* e);   // popup → Add
    static void _onActionClear (lv_event_t* e);   // popup → Clear
    static void _onActionDelete(lv_event_t* e);   // popup → Delete
    static void _onActionNotify(lv_event_t* e);   // popup → Notify toggle
    static void _onActionClose (lv_event_t* e);   // popup → Close

    static void _onAddSave     (lv_event_t* e);   // add dialog → Save
    static void _onAddCancel   (lv_event_t* e);   // add dialog → Cancel

    static void _onSend        (lv_event_t* e);
    static void _onDMPickerRow     (lv_event_t* e);
    static void _onDMPickerClose   (lv_event_t* e);
    static void _onBubbleClick      (lv_event_t* e);
    static void _onBubbleReply      (lv_event_t* e);
    static void _onBubbleAddContact (lv_event_t* e);
    static void _onBubbleShowPath   (lv_event_t* e);
    static void _onPathClose        (lv_event_t* e);
    static void _onPathKey          (lv_event_t* e);
    static void _onAddContactSave   (lv_event_t* e);
    static void _onAddContactCancel (lv_event_t* e);
    static void _onEmojiToggle     (lv_event_t* e);
    static void _onEmojiInsert     (lv_event_t* e);
};

}}  // namespace ops::ui

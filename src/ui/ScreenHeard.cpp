// Saitama — ScreenHeard.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenHeard.h"
#include "../mesh/MeshService.h"
#include "../utils/Contacts.h"
#include "../utils/Log.h"
#include "../utils/Repeaters.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include <cstdio>
#include <cstring>
#include <time.h>

namespace ops {
namespace ui {

lv_obj_t *ScreenHeard::_screen = nullptr;
bool ScreenHeard::_isVisible = false;

static constexpr int TOP_H = 28;

static int s_pendingPeer = -1;
// Key of the peer the open popup is about — its list position can change
// while the popup is up (the oldest peer is replaced when the list is full).
static uint8_t s_pendingKey[4] = {};
static bool s_dialogOpen = false;

// ── Time formatter ────────────────────────────────────────────────────
static void fmtTime(uint32_t ts, char *buf, size_t len) {
  if (!ts) {
    snprintf(buf, len, "--");
    return;
  }
  time_t t = (time_t)ts;
  time_t now = time(nullptr);
  struct tm lt, nt;
  localtime_r(&t, &lt);
  localtime_r(&now, &nt);
  static const char *kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (lt.tm_year == nt.tm_year && lt.tm_yday == nt.tm_yday)
    snprintf(buf, len, "%02d:%02d", lt.tm_hour, lt.tm_min);
  else
    snprintf(buf, len, "%d %s", lt.tm_mday, kMon[lt.tm_mon]);
}

// ── show() ───────────────────────────────────────────────────────────
void ScreenHeard::show() {
  lv_obj_t *old = _screen;
  _screen = nullptr;
  _isVisible = true;
  _build(); // creates _screen, calls lv_scr_load
  if (old)
    lv_obj_del(old);
}

void ScreenHeard::onPeersUpdated() {
  if (!_isVisible)
    return;
  if (s_dialogOpen)
    return; // don't rebuild while a save dialog is visible
  // If a screensaver or other overlay is covering this screen, skip the rebuild.
  // Deleting _screen while the screensaver's s_prevScreen still points to it
  // causes a LoadProhibited crash when the screensaver tries to restore it.
  if (_screen && lv_scr_act() != _screen)
    return;
  lv_obj_t *old = _screen;
  _screen = nullptr;
  _build(); // creates _screen, calls lv_scr_load
  // lv_obj_del_async defers deletion so LVGL can finish dispatching any
  // in-flight click event on the old screen before its objects are freed.
  if (old)
    lv_obj_del_async(old);
}

// ── _build() ─────────────────────────────────────────────────────────
void ScreenHeard::_build() {
  int peerCount = MeshService::instance().peerCount();

  _screen = lv_obj_create(nullptr);
  lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
  lv_obj_set_style_bg_color(_screen, theme::BG, 0);
  lv_obj_set_style_pad_all(_screen, 0, 0);
  lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

  // ── Top bar ───────────────────────────────────────────────────────
  lv_obj_t *bar = lv_obj_create(_screen);
  lv_obj_set_size(bar, OPS_SCREEN_W, TOP_H);
  lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_hor(bar, 4, 0);
  lv_obj_set_style_pad_ver(bar, 2, 0);
  lv_obj_set_style_pad_column(bar, 6, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *homeBtn = lv_btn_create(bar);
  lv_group_remove_obj(homeBtn);
  lv_obj_set_height(homeBtn, TOP_H - 6);
  lv_obj_set_style_bg_color(homeBtn, theme::BG, 0);
  lv_obj_set_style_bg_color(homeBtn, theme::PRIMARY, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(homeBtn, theme::BORDER, 0);
  lv_obj_set_style_border_width(homeBtn, 1, 0);
  lv_obj_set_style_radius(homeBtn, 4, 0);
  lv_obj_set_style_shadow_width(homeBtn, 0, 0);
  lv_obj_set_style_pad_hor(homeBtn, 5, 0);
  lv_obj_add_event_cb(homeBtn, _onHomeClick, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *homeLbl = lv_label_create(homeBtn);
  lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
  lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
  lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
  lv_obj_center(homeLbl);

  char title[24];
  snprintf(title, sizeof(title), "Heard (%d)", peerCount);
  lv_obj_t *titleLbl = lv_label_create(bar);
  lv_label_set_text(titleLbl, title);
  lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
  lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

  // Right side: Clear List, then the close X at the top-right corner.
  lv_obj_t *spacer = lv_obj_create(bar);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

  auto barBtn = [bar](const char *text, lv_color_t col, lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(bar);
    lv_group_remove_obj(b);
    lv_obj_set_height(b, TOP_H - 6);
    lv_obj_set_style_bg_color(b, theme::BG, 0);
    lv_obj_set_style_bg_color(b, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, col, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_center(l);
  };
  barBtn("Clear List", theme::ORANGE, _onClearList);
  barBtn(LV_SYMBOL_CLOSE, theme::TEXT_MUTED, _onHomeClick);

  // ── Peer list ─────────────────────────────────────────────────────
  lv_obj_t *list = lv_obj_create(_screen);
  lv_obj_set_size(list, OPS_SCREEN_W, OPS_SCREEN_H - TOP_H);
  lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, TOP_H);
  lv_obj_set_style_bg_color(list, theme::BG, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_radius(list, 0, 0);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_group_add_obj(lv_group_get_default(), list);
  lv_group_focus_obj(list);
  lv_obj_add_event_cb(list, _onKey, LV_EVENT_KEY, nullptr);

  if (peerCount == 0) {
    lv_obj_t *empty = lv_label_create(list);
    lv_label_set_text(empty, "Nothing heard yet - waiting for mesh traffic");
    lv_obj_set_style_text_color(empty, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_10, 0);
    lv_obj_set_style_pad_all(empty, 8, 0);
    lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(empty, OPS_SCREEN_W - 16);
  }

  for (int i = 0; i < peerCount; i++) {
    PeerInfo peer;
    if (!MeshService::instance().getPeer(i, peer))
      continue;
    theme::sanitizeText(peer.name);

    bool isRepeater = (peer.type == 2);

    // Row container
    lv_obj_t *row = lv_btn_create(list);
    lv_group_remove_obj(row);
    lv_obj_set_size(row, OPS_SCREEN_W, 28);
    lv_obj_set_style_bg_color(row, (i & 1) ? theme::BG_CARD : theme::BG, 0);
    lv_obj_set_style_bg_color(row, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, _onRowClick, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);

    // Type badge: "C" for Companion/Chat, "R" for Repeater
    lv_obj_t *typeLbl = lv_label_create(row);
    lv_label_set_text(typeLbl, isRepeater ? "R" : "C");
    lv_obj_set_width(typeLbl, 16);
    lv_obj_set_style_text_color(typeLbl,
                                isRepeater ? theme::GREEN : theme::ACCENT, 0);
    lv_obj_set_style_text_font(typeLbl, &lv_font_montserrat_10, 0);

    // Name
    lv_obj_t *nameLbl = lv_label_create(row);
    lv_label_set_text(nameLbl, peer.name);
    lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nameLbl, 160);
    lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(nameLbl, theme::bodyFont10(), 0);

    // Signal bars (last-heard RSSI), as in Contacts and Repeaters. A fixed
    // cell keeps the time column aligned; the bars sit at its right edge.
    lv_obj_t *sigCell = lv_obj_create(row);
    lv_obj_set_size(sigCell, 40, 16);
    lv_obj_set_style_bg_opa(sigCell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sigCell, 0, 0);
    lv_obj_set_style_pad_all(sigCell, 0, 0);
    lv_obj_clear_flag(sigCell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    theme::addSignalBars(sigCell, peer.lastRssi);

    // Time
    char timeBuf[10];
    fmtTime(peer.lastSeen, timeBuf, sizeof(timeBuf));
    lv_obj_t *timeLbl = lv_label_create(row);
    lv_label_set_text(timeLbl, timeBuf);
    lv_obj_set_width(timeLbl, 62);
    lv_obj_set_style_text_color(timeLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(timeLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(timeLbl, LV_TEXT_ALIGN_RIGHT, 0);
  }

  lv_scr_load(_screen);
  OPS_LOG("UI", "Heard shown (%d peers)", peerCount);
}

// ── _onHomeClick() ────────────────────────────────────────────────────
void ScreenHeard::_onHomeClick(lv_event_t * /*e*/) {
  _isVisible = false;
  ScreenLauncher::show();
}

// ── Peer details window ──────────────────────────────────────────────
// Name / Identity / RSSI (/ Owner for repeaters) with Save-or-Update and
// Cancel. A repeater's owner info is asked for over the air and filled in
// when it answers (lv_timer polls MeshService; the timer dies with the box).
static lv_obj_t *s_ownerLbl = nullptr;
static lv_timer_t *s_ownerTimer = nullptr;
static uint32_t s_ownerAskedMs = 0;
static constexpr uint32_t OWNER_TIMEOUT_MS = 20000;

static void _onOwnerPoll(lv_timer_t * /*t*/) {
  if (!s_ownerLbl) return;
  char txt[124];
  if (MeshService::instance().pollOwnerInfo(txt, sizeof(txt))) {
    theme::sanitizeText(txt);
    lv_label_set_text(s_ownerLbl, txt[0] ? txt : "(not set)");
    lv_obj_set_style_text_color(s_ownerLbl, theme::TEXT, 0);
  } else if (millis() - s_ownerAskedMs < OWNER_TIMEOUT_MS) {
    return;
  } else {
    lv_label_set_text(s_ownerLbl, "no answer");
  }
  lv_timer_del(s_ownerTimer);
  s_ownerTimer = nullptr;
}

static void _onDialogDeleted(lv_event_t * /*e*/) {
  if (s_ownerTimer) lv_timer_del(s_ownerTimer);
  s_ownerTimer = nullptr;
  s_ownerLbl = nullptr;
}

// One "Key:  value" line; returns the value label.
static lv_obj_t *_addField(lv_obj_t *box, const char *key, const char *value,
                           lv_color_t col) {
  lv_obj_t *row = lv_obj_create(box);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *k = lv_label_create(row);
  lv_label_set_text(k, key);
  lv_obj_set_style_text_color(k, theme::TEXT_MUTED, 0);
  lv_obj_set_style_text_font(k, &lv_font_montserrat_10, 0);

  lv_obj_t *v = lv_label_create(row);
  lv_label_set_text(v, value);
  lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(v, 200);
  lv_obj_set_pos(v, 56, 0);
  lv_obj_set_style_text_color(v, col, 0);
  lv_obj_set_style_text_font(v, theme::bodyFont10(), 0);
  return v;
}

static void _buildPeerDialog(const PeerInfo &peer, bool isRepeater,
                             bool already, lv_event_cb_t saveCb) {
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(overlay, _onDialogDeleted, LV_EVENT_DELETE, nullptr);

  lv_obj_t *box = lv_obj_create(overlay);
  lv_obj_set_width(box, 280);
  lv_obj_set_height(box, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(box, OPS_SCREEN_H - 8, 0);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
  lv_obj_set_style_border_color(box, theme::ACCENT, 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_radius(box, 6, 0);
  lv_obj_set_style_pad_all(box, 8, 0);
  lv_obj_set_style_pad_row(box, 5, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(box, LV_DIR_VER);

  lv_obj_t *title = lv_label_create(box);
  lv_label_set_text(title, isRepeater ? "Repeater" : "Contact");
  lv_obj_set_style_text_color(title, theme::ACCENT, 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

  _addField(box, "Name:", peer.name, theme::TEXT);

  // Full public key when the advert carried it, else the 4-byte prefix.
  static const uint8_t kZero[32] = {};
  char idBuf[65];
  bool fullKey = memcmp(peer.pubKey, kZero, 32) != 0;
  int n = fullKey ? 32 : 4;
  const uint8_t *k = fullKey ? peer.pubKey : peer.pubKeyPrefix;
  for (int i = 0; i < n; i++) snprintf(&idBuf[i * 2], 3, "%02X", k[i]);
  _addField(box, "Identity:", idBuf, theme::TEXT_MUTED);

  char rssiBuf[40];
  if (peer.hops == 0)
    snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm (direct)", (int)peer.lastRssi);
  else if (peer.hops == 0xFF)
    snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm (last hop)", (int)peer.lastRssi);
  else
    snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm (last hop, %u hops)",
             (int)peer.lastRssi, (unsigned)peer.hops);
  lv_color_t rc = peer.lastRssi >= -80.0f    ? lv_color_hex(0x3FB950)
                  : peer.lastRssi >= -100.0f ? lv_color_hex(0xE3B341)
                                             : lv_color_hex(0xF85149);
  _addField(box, "RSSI:", rssiBuf, rc);

  if (isRepeater) {
    bool asked = MeshService::instance().requestOwnerInfo(peer.pubKey, peer.name,
                                                         peer.hops == 0);
    s_ownerLbl = _addField(box, "Owner:", asked ? "asking..." : "needs a direct route",
                           theme::TEXT_MUTED);
    if (asked) {
      s_ownerAskedMs = millis();
      s_ownerTimer = lv_timer_create(_onOwnerPoll, 250, nullptr);
    }
  }

  // Save/Update + Cancel, two columns (fat-finger popup style).
  lv_obj_t *btnRow = lv_obj_create(box);
  lv_obj_set_size(btnRow, LV_PCT(100), 34);
  lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btnRow, 0, 0);
  lv_obj_set_style_pad_all(btnRow, 0, 0);
  lv_obj_set_style_pad_top(btnRow, 4, 0);
  lv_obj_set_style_pad_column(btnRow, 8, 0);
  lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);

  auto mkBtn = [&](const char *text, lv_color_t bg, lv_color_t fg,
                   lv_color_t border, lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(btnRow);
    lv_group_remove_obj(b);
    lv_obj_set_height(b, 30);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_color(b, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, border, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);
  };
  mkBtn(already ? "Update" : "Save", theme::PRIMARY, theme::TEXT, theme::PRIMARY, saveCb);
  mkBtn("Cancel", theme::BG, theme::TEXT_MUTED, theme::BORDER, ScreenHeard::_onSaveCancel);

  s_dialogOpen = true;
}

// ── _onKey() — ESC/backspace → back to launcher ───────────────────────
void ScreenHeard::_onKey(lv_event_t *e) {
  uint32_t *key = (uint32_t *)lv_event_get_param(e);
  if (key && *key == LV_KEY_ESC) {
    _isVisible = false;
    ScreenLauncher::show();
  }
}

// ── _onRowClick() — branch on peer type ──────────────────────────────
void ScreenHeard::_onRowClick(lv_event_t *e) {
  s_pendingPeer = (int)(intptr_t)lv_event_get_user_data(e);

  PeerInfo peer;
  if (!MeshService::instance().getPeer(s_pendingPeer, peer))
    return;
  memcpy(s_pendingKey, peer.pubKeyPrefix, 4);
  theme::sanitizeText(peer.name);

  if (peer.type == 2) {
    bool already = repeaters::findByKey(peer.pubKeyPrefix);
    _buildPeerDialog(peer, true, already, _onSaveRepConfirm);
  } else {
    // Client / Companion / Room — saved to contacts
    bool already = contacts::findByKey(peer.pubKeyPrefix);
    _buildPeerDialog(peer, false, already, _onSaveConfirm);
  }
}

// ── _onSaveConfirm() — save to contacts ──────────────────────────────
void ScreenHeard::_onSaveConfirm(lv_event_t *e) {
  lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);

  PeerInfo peer;
  if (s_pendingPeer >= 0 &&
      MeshService::instance().findPeerByKey(s_pendingKey, peer)) {
    theme::sanitizeText(peer.name);
    Contact c{};
    strncpy(c.name, peer.name, sizeof(c.name) - 1);
    memcpy(c.pubKeyPrefix, peer.pubKeyPrefix, 4);
    memcpy(c.pubKey, peer.pubKey, 32);
    c.lastSeen = peer.lastSeen;
    c.lastRssi = peer.lastRssi;
    c.hasUnread = false;
    contacts::add(c);
    OPS_LOG("Heard", "Saved contact: %s", peer.name);
  }

  s_dialogOpen = false;
  lv_obj_del_async(overlay);   // this handler is on a child of overlay
  s_pendingPeer = -1;
}

// ── _onSaveRepConfirm() — save to repeaters ───────────────────────────
void ScreenHeard::_onSaveRepConfirm(lv_event_t *e) {
  lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);

  PeerInfo peer;
  if (s_pendingPeer >= 0 &&
      MeshService::instance().findPeerByKey(s_pendingKey, peer)) {
    theme::sanitizeText(peer.name);
    Repeater r{};
    strncpy(r.name, peer.name, sizeof(r.name) - 1);
    memcpy(r.pubKeyPrefix, peer.pubKeyPrefix, 4);
    memcpy(r.pubKey, peer.pubKey, 32);
    r.lastSeen = peer.lastSeen;
    r.lastRssi = peer.lastRssi;
    repeaters::add(r);
    OPS_LOG("Heard", "Saved repeater: %s", peer.name);
  }

  s_dialogOpen = false;
  lv_obj_del_async(overlay);   // this handler is on a child of overlay
  s_pendingPeer = -1;
}

// ── _onSaveCancel() ───────────────────────────────────────────────────
void ScreenHeard::_onSaveCancel(lv_event_t *e) {
  lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
  s_dialogOpen = false;
  lv_obj_del_async(overlay);   // this handler is on a child of overlay
  s_pendingPeer = -1;
}

// ── _onClearList() — wipe the in-memory Heard list (top-bar button) ──
void ScreenHeard::_onClearList(lv_event_t * /*e*/) {
  MeshService::instance().clearPeers();
  OPS_LOG("Heard", "Cleared heard list");
  s_dialogOpen = false;
  s_pendingPeer = -1;
  onPeersUpdated();  // rebuilds now; the old screen is deleted async (the
                     // click is still being dispatched on its button)
}

} // namespace ui
} // namespace ops

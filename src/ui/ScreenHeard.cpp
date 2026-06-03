// Saitama — ScreenHeard.cpp
// Copyright 2026 Saitama — MIT License

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
    lv_obj_set_width(nameLbl, 132);
    lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);

    // RSSI
    char rssiBuf[12];
    snprintf(rssiBuf, sizeof(rssiBuf), "%ddBm", (int)peer.lastRssi);
    lv_obj_t *rssiLbl = lv_label_create(row);
    lv_label_set_text(rssiLbl, rssiBuf);
    lv_obj_set_width(rssiLbl, 68);
    lv_obj_set_style_text_color(rssiLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(rssiLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(rssiLbl, LV_TEXT_ALIGN_RIGHT, 0);

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

// ── _buildSaveDialog() — shared overlay builder ───────────────────────
// Returns the box so caller can attach a save button with the right callback.
static lv_obj_t *_buildSaveDialog(const char *msg, lv_event_cb_t saveCb,
                                  const char *saveLblText) {
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *box = lv_obj_create(overlay);
  lv_obj_set_size(box, 240, 90);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
  lv_obj_set_style_border_color(box, theme::ACCENT, 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_radius(box, 6, 0);
  lv_obj_set_style_pad_all(box, 8, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lbl = lv_label_create(box);
  lv_label_set_text(lbl, msg);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, 220);
  lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *saveBtn = lv_btn_create(box);
  lv_group_remove_obj(saveBtn);
  lv_obj_set_size(saveBtn, 95, 26);
  lv_obj_align(saveBtn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, 0);
  lv_obj_set_style_bg_color(saveBtn, theme::ACCENT, LV_STATE_PRESSED);
  lv_obj_set_style_radius(saveBtn, 4, 0);
  lv_obj_set_style_shadow_width(saveBtn, 0, 0);
  lv_obj_add_event_cb(saveBtn, saveCb, LV_EVENT_CLICKED, overlay);
  lv_obj_t *saveTxt = lv_label_create(saveBtn);
  lv_label_set_text(saveTxt, saveLblText);
  lv_obj_set_style_text_color(saveTxt, theme::TEXT, 0);
  lv_obj_set_style_text_font(saveTxt, &lv_font_montserrat_10, 0);
  lv_obj_center(saveTxt);

  lv_obj_t *cancelBtn = lv_btn_create(box);
  lv_group_remove_obj(cancelBtn);
  lv_obj_set_size(cancelBtn, 95, 26);
  lv_obj_align(cancelBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(cancelBtn, theme::BG, 0);
  lv_obj_set_style_bg_color(cancelBtn, theme::RED, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(cancelBtn, theme::BORDER, 0);
  lv_obj_set_style_border_width(cancelBtn, 1, 0);
  lv_obj_set_style_radius(cancelBtn, 4, 0);
  lv_obj_set_style_shadow_width(cancelBtn, 0, 0);
  lv_obj_add_event_cb(cancelBtn, ScreenHeard::_onSaveCancel, LV_EVENT_CLICKED,
                      overlay);
  lv_obj_t *cancelTxt = lv_label_create(cancelBtn);
  lv_label_set_text(cancelTxt, "Cancel");
  lv_obj_set_style_text_color(cancelTxt, theme::TEXT_MUTED, 0);
  lv_obj_set_style_text_font(cancelTxt, &lv_font_montserrat_10, 0);
  lv_obj_center(cancelTxt);

  s_dialogOpen = true;
  return overlay;
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

  char msg[72];
  if (peer.type == 2) {
    // Repeater
    bool already = repeaters::findByKey(peer.pubKeyPrefix);
    snprintf(msg, sizeof(msg),
             already ? "Update repeater '%s'?" : "Save '%s' to repeaters?",
             peer.name);
    _buildSaveDialog(msg, _onSaveRepConfirm, already ? "Update" : "Save");
  } else {
    // Client / Companion / Room — save to contacts
    bool already = contacts::findByKey(peer.pubKeyPrefix);
    snprintf(msg, sizeof(msg),
             already ? "Update contact '%s'?" : "Save '%s' to contacts?",
             peer.name);
    _buildSaveDialog(msg, _onSaveConfirm, already ? "Update" : "Save");
  }
}

// ── _onSaveConfirm() — save to contacts ──────────────────────────────
void ScreenHeard::_onSaveConfirm(lv_event_t *e) {
  lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);

  PeerInfo peer;
  if (s_pendingPeer >= 0 &&
      MeshService::instance().getPeer(s_pendingPeer, peer)) {
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
  lv_obj_del(overlay);
  s_pendingPeer = -1;
}

// ── _onSaveRepConfirm() — save to repeaters ───────────────────────────
void ScreenHeard::_onSaveRepConfirm(lv_event_t *e) {
  lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);

  PeerInfo peer;
  if (s_pendingPeer >= 0 &&
      MeshService::instance().getPeer(s_pendingPeer, peer)) {
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
  lv_obj_del(overlay);
  s_pendingPeer = -1;
}

// ── _onSaveCancel() ───────────────────────────────────────────────────
void ScreenHeard::_onSaveCancel(lv_event_t *e) {
  lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
  s_dialogOpen = false;
  lv_obj_del(overlay);
  s_pendingPeer = -1;
}

} // namespace ui
} // namespace ops

// Saitama — ScreenTrace.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenTrace.h"
#include "../mesh/MeshService.h"
#include "../utils/Contacts.h"
#include "../utils/Log.h"
#include "../utils/Repeaters.h"
#include "ScreenLauncher.h"
#include "Theme.h"

#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lvgl.h>

namespace ops {
namespace ui {

// ── Static state ────────────────────────────���────────────────────────
lv_obj_t *ScreenTrace::_screen = nullptr;
lv_obj_t *ScreenTrace::_dropdown = nullptr;
lv_obj_t *ScreenTrace::_traceBtn = nullptr;
lv_obj_t *ScreenTrace::_statusLbl = nullptr;
lv_obj_t *ScreenTrace::_hopList = nullptr;

static constexpr int TOP_H = 28;

// ── Target registry ───────────────────────────────────────────────────
// Holds a merged, alphabetically sorted list of contacts + repeaters.
static constexpr int MAX_TARGETS = 100;

struct TraceTarget {
  char name[32];
  uint8_t pubKeyPrefix[4];
  bool isRepeater;
};

static TraceTarget s_targets[MAX_TARGETS];
static int s_numTargets = 0;

// Currently selected dropdown index (0-based; -1 = nothing).
static int s_selIdx = -1;
// Set by showFor(): the target to preselect when the screen is built.
static bool    s_wantSel = false;
static uint8_t s_wantPrefix[4] = {};

// Pending trace tracking.
static uint32_t s_pendingTag = 0;
static uint32_t s_pendingUntilMs = 0; // timeout ms (millis())
static bool s_traceInFlight = false;
static bool s_pendingIsDirect =
    false; // true = 0-hop (direct RF); false = multi-hop

// Stored trace result for display.
static ops::TraceResult s_result{};
static bool s_hasResult = false;

// ── Build target list ────────────────────────────��────────────────────
static void _buildTargetList() {
  s_numTargets = 0;

  // Add contacts (type 0 = companion/client, etc.)
  int nc = ops::contacts::count();
  for (int i = 0; i < nc && s_numTargets < MAX_TARGETS; i++) {
    ops::Contact c;
    if (!ops::contacts::get(i, c))
      continue;
    if (!c.name[0])
      continue;
    TraceTarget &t = s_targets[s_numTargets++];
    strncpy(t.name, c.name, 31);
    t.name[31] = '\0';
    memcpy(t.pubKeyPrefix, c.pubKeyPrefix, 4);
    t.isRepeater = false;
  }

  // Add repeaters
  int nr = ops::repeaters::count();
  for (int i = 0; i < nr && s_numTargets < MAX_TARGETS; i++) {
    ops::Repeater r;
    if (!ops::repeaters::get(i, r))
      continue;
    if (!r.name[0])
      continue;
    // Skip duplicates already in contacts list
    bool dup = false;
    for (int j = 0; j < s_numTargets; j++) {
      if (memcmp(s_targets[j].pubKeyPrefix, r.pubKeyPrefix, 4) == 0) {
        dup = true;
        break;
      }
    }
    if (dup)
      continue;
    TraceTarget &t = s_targets[s_numTargets++];
    strncpy(t.name, r.name, 31);
    t.name[31] = '\0';
    memcpy(t.pubKeyPrefix, r.pubKeyPrefix, 4);
    t.isRepeater = true;
  }

  // Sort alphabetically (insertion sort — n ≤ 100)
  for (int i = 1; i < s_numTargets; i++) {
    TraceTarget key = s_targets[i];
    int j = i - 1;
    while (j >= 0 && strcasecmp(s_targets[j].name, key.name) > 0) {
      s_targets[j + 1] = s_targets[j];
      j--;
    }
    s_targets[j + 1] = key;
  }
}

// ── Dropdown options string ─────────────────────────��─────────────────
// lv_dropdown requires a single "\n"-separated string of options.
static char *_buildDropOptions() {
  static char buf[MAX_TARGETS * 40];
  buf[0] = '\0';
  for (int i = 0; i < s_numTargets; i++) {
    if (i > 0)
      strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
    char entry[36];
    snprintf(entry, sizeof(entry), "%c %s", s_targets[i].isRepeater ? 'R' : 'C',
             s_targets[i].name);
    strncat(buf, entry, sizeof(buf) - strlen(buf) - 1);
  }
  return buf;
}

// ── Helpers ───────────────────────────────���───────────────────────────
static const char *_lookupNodeName(const uint8_t *hashBytes, uint8_t hashSz,
                                   char *fallback, size_t fblen) {
  // Try contacts first.
  int nc = ops::contacts::count();
  for (int i = 0; i < nc; i++) {
    ops::Contact c;
    if (!ops::contacts::get(i, c))
      continue;
    bool match = true;
    for (int b = 0; b < (int)hashSz && b < 4; b++)
      if (c.pubKeyPrefix[b] != hashBytes[b]) {
        match = false;
        break;
      }
    if (match) {
      snprintf(fallback, fblen, "%s", c.name);
      return fallback;
    }
  }
  // Try repeaters.
  int nr = ops::repeaters::count();
  for (int i = 0; i < nr; i++) {
    ops::Repeater r;
    if (!ops::repeaters::get(i, r))
      continue;
    bool match = true;
    for (int b = 0; b < (int)hashSz && b < 4; b++)
      if (r.pubKeyPrefix[b] != hashBytes[b]) {
        match = false;
        break;
      }
    if (match) {
      snprintf(fallback, fblen, "%s", r.name);
      return fallback;
    }
  }
  // Unknown — show hash as uppercase hex.
  char *p = fallback;
  size_t rem = fblen;
  for (int b = 0; b < (int)hashSz && rem > 2; b++, rem -= 2)
    snprintf(p + (hashSz - rem / 2) * 2 - (hashSz - rem / 2) * 2, rem, "%02X",
             hashBytes[b]);
  // simpler:
  fallback[0] = '\0';
  for (int b = 0; b < (int)hashSz && (size_t)(b * 2 + 2) < fblen; b++)
    snprintf(fallback + b * 2, 3, "%02X", hashBytes[b]);
  return fallback;
}

static float _snrFromRaw(int8_t raw) { return raw / 4.0f; }

// ── Hop row ───────────────────────────────────────────────────────────
// One result row: "N.  AB  Name        +6.5 dB". snrRaw < -128 = unknown.
static void _addHopRow(lv_obj_t *list, const char *num, const char *abbr,
                       const char *name, bool highlight, int snrRaw) {
  lv_obj_t *row = lv_obj_create(list);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(row, theme::BG_CARD, 0);
  lv_obj_set_style_border_width(row, highlight ? 1 : 0, 0);
  lv_obj_set_style_border_color(row, theme::ACCENT, 0);
  lv_obj_set_style_radius(row, 4, 0);
  lv_obj_set_style_pad_hor(row, 6, 0);
  lv_obj_set_style_pad_ver(row, 3, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *hopLbl = lv_label_create(row);
  lv_label_set_text(hopLbl, num);
  lv_obj_set_style_text_color(hopLbl, theme::TEXT_MUTED, 0);
  lv_obj_set_style_text_font(hopLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_width(hopLbl, 22);

  lv_obj_t *addrLbl = lv_label_create(row);
  lv_label_set_text(addrLbl, abbr);
  lv_obj_set_style_text_color(addrLbl, theme::ACCENT, 0);
  lv_obj_set_style_text_font(addrLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_width(addrLbl, 26);

  lv_obj_t *nameLbl = lv_label_create(row);
  lv_label_set_text(nameLbl, name);
  lv_obj_set_style_text_color(nameLbl, highlight ? theme::ACCENT : theme::TEXT, 0);
  lv_obj_set_style_text_font(nameLbl, theme::bodyFont12(), 0);
  lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);
  lv_obj_set_flex_grow(nameLbl, 1);

  char snrBuf[16] = "?";
  lv_color_t snrCol = theme::TEXT_MUTED;
  if (snrRaw >= -128) {
    float snr = _snrFromRaw((int8_t)snrRaw);
    snprintf(snrBuf, sizeof(snrBuf), "%+.1f dB", (double)snr);
    snrCol = (snr >= 5.0f) ? theme::GREEN : (snr >= -2.5f) ? theme::ORANGE : theme::RED;
  }
  lv_obj_t *snrLbl = lv_label_create(row);
  lv_label_set_text(snrLbl, snrBuf);
  lv_obj_set_style_text_font(snrLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(snrLbl, snrCol, 0);
  lv_obj_set_width(snrLbl, 66);
  lv_obj_set_style_text_align(snrLbl, LV_TEXT_ALIGN_RIGHT, 0);
}

// ── _rebuildHopList() ─────────────────────────────────────────────────
// The route is out and back, so it's symmetric: the middle entry is the
// turnaround (the target, or its last relay). Each row's SNR is what that
// node received the trace at; the final "You" row is our own receive SNR.
void ScreenTrace::_rebuildHopList() {
  if (!_hopList)
    return;
  lv_obj_clean(_hopList);

  if (!s_hasResult)
    return;

  const ops::TraceResult &r = s_result;
  uint8_t hashSz = r.hashSz ? r.hashSz : 1;
  uint8_t numHops = r.numHops;
  int turn = numHops / 2;

  {
    lv_obj_t *hdr = lv_label_create(_hopList);
    char buf[48];
    snprintf(buf, sizeof(buf), "Out and back: %d node%s", numHops,
             numHops == 1 ? "" : "s");
    lv_label_set_text(hdr, buf);
    lv_obj_set_style_text_color(hdr, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_width(hdr, lv_pct(100));
  }

  for (int i = 0; i < numHops; i++) {
    const uint8_t *hashBytes = r.hashes + (i * hashSz);
    char unknown[12];
    const char *nodeName =
        _lookupNodeName(hashBytes, hashSz, unknown, sizeof(unknown));
    char num[8];
    snprintf(num, sizeof(num), "%d.", i + 1);
    char abbr[8];
    snprintf(abbr, sizeof(abbr), "%02X", hashBytes[0]);
    _addHopRow(_hopList, num, abbr, nodeName, i == turn,
               i < r.numSnrs ? (int)r.snrs[i] : -129);
  }
  _addHopRow(_hopList, LV_SYMBOL_HOME, "", "You", false, (int)r.rxSnr);
}

// ── Route status text ─────────────────────────────────────────────────
// Describes how a trace to t will run. Returns false if there is no path.
static bool _routeStatus(const TraceTarget &t, char *buf, size_t len,
                         lv_color_t &col) {
  auto &mesh = ops::MeshService::instance();
  if (!mesh.hasPathTo(t.pubKeyPrefix)) {
    snprintf(buf, len, "No path known yet - wait for advert");
    col = theme::ORANGE;
    return false;
  }
  ops::PathInfo pi{};
  mesh.getContactPath(t.pubKeyPrefix, pi);
  if (pi.direct)
    snprintf(buf, len, "Direct - target must forward to answer");
  else if (t.isRepeater)
    snprintf(buf, len, "Via %d relay(s), out and back", (int)pi.hopCount);
  else
    snprintf(buf, len, "Via %d relay(s), turns at last relay", (int)pi.hopCount);
  col = theme::ACCENT;
  return true;
}

// ── _setStatus() ──────────────────────────────────────────────────────
void ScreenTrace::_setStatus(const char *msg, lv_color_t col) {
  if (!_statusLbl)
    return;
  lv_label_set_text(_statusLbl, msg);
  lv_obj_set_style_text_color(_statusLbl, col, 0);
}

// ── tick() ────────────────────────────────��───────────────────────────
void ScreenTrace::tick() {
  if (!_screen || lv_scr_act() != _screen)
    return;

  // Check for arrived trace result.
  ops::TraceResult res;
  if (ops::MeshService::instance().pollTraceResult(res)) {
    // A trace from someone else can end within earshot too — while ours is
    // in flight, only ours counts. (Idle, accept any: the Terminal's
    // "trace" command shows its result here.)
    if (s_traceInFlight && res.tag != s_pendingTag)
      return;
    s_result = res;
    s_hasResult = true;
    s_traceInFlight = false;
    _setStatus("Result received", theme::GREEN);
    _rebuildHopList();
    return;
  }

  // Timeout guard.
  if (s_traceInFlight) {
    if (millis() > s_pendingUntilMs) {
      s_traceInFlight = false;
      if (s_pendingIsDirect) {
        _setStatus("No response - target has forwarding off", theme::ORANGE);
      } else {
        _setStatus("No response - a node on the route didn't forward "
                   "(route may be stale)",
                   theme::ORANGE);
      }
    }
  }
}

// ── _build() ────────────────────���─────────────────────────────���──────
void ScreenTrace::_build() {
  _buildTargetList();

  _screen = lv_obj_create(nullptr);
  lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
  lv_obj_set_style_bg_color(_screen, theme::BG, 0);
  lv_obj_set_style_pad_all(_screen, 0, 0);
  lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

  // ── Top bar ─────────────────────────���────────────────────���────────
  lv_obj_t *bar = lv_obj_create(_screen);
  lv_obj_set_size(bar, OPS_SCREEN_W, TOP_H);
  lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_hor(bar, 4, 0);
  lv_obj_set_style_pad_ver(bar, 2, 0);
  lv_obj_set_style_pad_column(bar, 6, 0);
  lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // Home button
  lv_obj_t *backBtn = lv_btn_create(bar);
  lv_group_remove_obj(backBtn);
  lv_obj_set_height(backBtn, TOP_H - 6);
  lv_obj_set_style_bg_color(backBtn, theme::BG, 0);
  lv_obj_set_style_bg_color(backBtn, theme::PRIMARY, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(backBtn, theme::BORDER, 0);
  lv_obj_set_style_border_width(backBtn, 1, 0);
  lv_obj_set_style_radius(backBtn, 4, 0);
  lv_obj_set_style_shadow_width(backBtn, 0, 0);
  lv_obj_set_style_pad_hor(backBtn, 5, 0);
  lv_obj_add_event_cb(backBtn, _onHomeClick, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLbl = lv_label_create(backBtn);
  lv_label_set_text(backLbl, LV_SYMBOL_HOME);
  lv_obj_set_style_text_color(backLbl, theme::ACCENT, 0);
  lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_10, 0);
  lv_obj_center(backLbl);

  // Title
  lv_obj_t *titleLbl = lv_label_create(bar);
  lv_label_set_text(titleLbl, "Trace Route");
  lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
  lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_14, 0);

  // ── Body: one column below the top bar ────────────────────────────
  // Target label, dropdown (own line), Trace button (own line), status,
  // then the hop list filling the rest. Flex layout keeps every row at its
  // content height, so nothing clips or overlaps.
  lv_obj_t *body = lv_obj_create(_screen);
  lv_obj_set_size(body, OPS_SCREEN_W, OPS_SCREEN_H - TOP_H);
  lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, TOP_H);
  lv_obj_set_style_bg_color(body, theme::BG, 0);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_obj_set_style_radius(body, 0, 0);
  lv_obj_set_style_pad_hor(body, 6, 0);
  lv_obj_set_style_pad_top(body, 4, 0);
  lv_obj_set_style_pad_bottom(body, 2, 0);
  lv_obj_set_style_pad_row(body, 4, 0);
  lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  // "Target" label
  lv_obj_t *tgtLbl = lv_label_create(body);
  lv_label_set_text(tgtLbl, "Target");
  lv_obj_set_style_text_color(tgtLbl, theme::TEXT_MUTED, 0);
  lv_obj_set_style_text_font(tgtLbl, &lv_font_montserrat_12, 0);

  // Dropdown — full width on its own line
  _dropdown = lv_dropdown_create(body);
  lv_obj_set_width(_dropdown, lv_pct(100));
  lv_obj_set_height(_dropdown, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(_dropdown, theme::BG_CARD, 0);
  lv_obj_set_style_border_color(_dropdown, theme::BORDER, 0);
  lv_obj_set_style_border_color(_dropdown, theme::ACCENT, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(_dropdown, 1, 0);
  lv_obj_set_style_radius(_dropdown, 4, 0);
  lv_obj_set_style_text_color(_dropdown, theme::TEXT, 0);
  lv_obj_set_style_text_font(_dropdown, &lv_font_montserrat_14, 0);
  lv_obj_set_style_pad_hor(_dropdown, 6, 0);
  lv_obj_set_style_pad_ver(_dropdown, 6, 0);

  if (s_numTargets > 0) {
    lv_dropdown_set_options(_dropdown, _buildDropOptions());
    s_selIdx = 0;
    if (s_wantSel) {
      for (int i = 0; i < s_numTargets; i++)
        if (memcmp(s_targets[i].pubKeyPrefix, s_wantPrefix, 4) == 0) { s_selIdx = i; break; }
      s_wantSel = false;
    }
    lv_dropdown_set_selected(_dropdown, (uint16_t)s_selIdx);
  } else {
    lv_dropdown_set_options(_dropdown, "(no contacts)");
    s_selIdx = -1;
  }
  lv_obj_add_event_cb(_dropdown, _onDropChange, LV_EVENT_VALUE_CHANGED,
                      nullptr);

  // Style the list that opens below the dropdown
  lv_obj_t *dropList = lv_dropdown_get_list(_dropdown);
  lv_obj_set_style_bg_color(dropList, theme::BG_CARD, 0);
  lv_obj_set_style_border_color(dropList, theme::BORDER, 0);
  lv_obj_set_style_text_color(dropList, theme::TEXT, 0);
  lv_obj_set_style_text_font(dropList, &lv_font_montserrat_14, 0);
  lv_obj_set_style_max_height(dropList, 150, 0);

  // "Trace" button — full width on its own line
  _traceBtn = lv_btn_create(body);
  lv_obj_set_width(_traceBtn, lv_pct(100));
  lv_obj_set_height(_traceBtn, 30);
  lv_obj_set_style_bg_color(_traceBtn, theme::PRIMARY, 0);
  lv_obj_set_style_bg_color(_traceBtn, theme::ACCENT, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(_traceBtn, theme::ACCENT, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(_traceBtn, 0, 0);
  lv_obj_set_style_border_width(_traceBtn, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_bg_opa(_traceBtn, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_set_style_radius(_traceBtn, 4, 0);
  lv_obj_set_style_shadow_width(_traceBtn, 0, 0);
  lv_obj_add_event_cb(_traceBtn, _onTraceClick, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *tBtnLbl = lv_label_create(_traceBtn);
  lv_label_set_text(tBtnLbl, LV_SYMBOL_LOOP " Trace");
  lv_obj_set_style_text_color(tBtnLbl, theme::TEXT, 0);
  lv_obj_set_style_text_font(tBtnLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(tBtnLbl);

  // Disable trace btn if no targets or no path known; build initial status
  // text.
  char initStatus[56] = "No contacts or repeaters saved";
  lv_color_t initCol = theme::TEXT_MUTED;
  if (s_selIdx < 0 ||
      !_routeStatus(s_targets[s_selIdx], initStatus, sizeof(initStatus), initCol))
    lv_obj_add_state(_traceBtn, LV_STATE_DISABLED);

  // ── Status line ───────────────────────────────────────────────────
  _statusLbl = lv_label_create(body);
  lv_obj_set_width(_statusLbl, lv_pct(100));
  lv_label_set_long_mode(_statusLbl, LV_LABEL_LONG_WRAP);
  lv_label_set_text(_statusLbl, initStatus);
  lv_obj_set_style_text_color(_statusLbl, initCol, 0);
  lv_obj_set_style_text_font(_statusLbl, &lv_font_montserrat_12, 0);

  // ── Hop list (scrollable, fills the rest) ─────────────────────────
  _hopList = lv_obj_create(body);
  lv_obj_set_width(_hopList, lv_pct(100));
  lv_obj_set_flex_grow(_hopList, 1);
  lv_obj_set_style_bg_color(_hopList, theme::BG, 0);
  lv_obj_set_style_border_width(_hopList, 0, 0);
  lv_obj_set_style_radius(_hopList, 0, 0);
  lv_obj_set_style_pad_all(_hopList, 0, 0);
  lv_obj_set_style_pad_row(_hopList, 2, 0);
  lv_obj_set_scrollbar_mode(_hopList, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(_hopList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(_hopList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  if (s_hasResult)
    _rebuildHopList();

  lv_scr_load(_screen);
}

// ── show() ──────────────────────────────────────────────────────���────
void ScreenTrace::showFor(const uint8_t pubKeyPrefix4[4]) {
  memcpy(s_wantPrefix, pubKeyPrefix4, 4);
  s_wantSel = true;
  show();
}

void ScreenTrace::show() {
  lv_obj_t *old = _screen;
  _screen = nullptr;
  _dropdown = nullptr;
  _traceBtn = nullptr;
  _statusLbl = nullptr;
  _hopList = nullptr;
  _build();
  if (old)
    lv_obj_del(old);
}

// ── Callbacks ─────────────────────────────��──────────────────────────��
void ScreenTrace::_onHomeClick(lv_event_t * /*e*/) { ScreenLauncher::show(); }

void ScreenTrace::_onDropChange(lv_event_t *e) {
  lv_obj_t *dd = static_cast<lv_obj_t *>(lv_event_get_target(e));
  int idx = (int)lv_dropdown_get_selected(dd);

  s_selIdx = (idx >= 0 && idx < s_numTargets) ? idx : -1;

  if (!_traceBtn)
    return;

  if (s_selIdx < 0) {
    lv_obj_add_state(_traceBtn, LV_STATE_DISABLED);
    _setStatus("No target selected", theme::TEXT_MUTED);
    return;
  }

  char msg[56];
  lv_color_t col;
  if (_routeStatus(s_targets[s_selIdx], msg, sizeof(msg), col))
    lv_obj_clear_state(_traceBtn, LV_STATE_DISABLED);
  else
    lv_obj_add_state(_traceBtn, LV_STATE_DISABLED);
  _setStatus(msg, col);
}

void ScreenTrace::_onTraceClick(lv_event_t * /*e*/) {
  if (s_selIdx < 0 || s_selIdx >= s_numTargets)
    return;

  const TraceTarget &tgt = s_targets[s_selIdx];
  uint32_t tag = 0;
  int nodes = 1;
  bool ok = ops::MeshService::instance().sendTrace(tgt.pubKeyPrefix, tag, nodes);
  if (!ok) {
    _setStatus("Send failed (no path?)", theme::RED);
    return;
  }

  ops::PathInfo pi{};
  ops::MeshService::instance().getContactPath(tgt.pubKeyPrefix, pi);
  s_pendingTag = tag;
  s_traceInFlight = true;
  // Each node waits a short random delay before retransmitting; allow
  // generously per node on the out-and-back route.
  s_pendingUntilMs = millis() + 8000 + 3000 * (uint32_t)nodes;
  s_pendingIsDirect = pi.direct;
  s_hasResult = false;
  if (_hopList)
    lv_obj_clean(_hopList);

  char buf[40];
  snprintf(buf, sizeof(buf), "Trace sent to %s...", tgt.name);
  _setStatus(buf, theme::ACCENT);

  OPS_LOG("Trace", "Trace → %s tag=%08X", tgt.name, tag);
}

} // namespace ui
} // namespace ops

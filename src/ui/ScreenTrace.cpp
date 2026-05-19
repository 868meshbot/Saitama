// Saitama — ScreenTrace.cpp
// Copyright 2026 Saitama — MIT License

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

// ── _rebuildHopList() ─────────────────────────��───────────────────────
void ScreenTrace::_rebuildHopList() {
  if (!_hopList)
    return;
  lv_obj_clean(_hopList);

  if (!s_hasResult)
    return;

  const ops::TraceResult &r = s_result;
  uint8_t hashSz = r.hashSz ? r.hashSz : 1;
  uint8_t numHops = r.numHops;

  // Header: tag + hop count
  {
    lv_obj_t *hdr = lv_label_create(_hopList);
    char buf[48];
    snprintf(buf, sizeof(buf), "Tag %08X  %d hop%s", r.tag, numHops,
             numHops == 1 ? "" : "s");
    lv_label_set_text(hdr, buf);
    lv_obj_set_style_text_color(hdr, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_10, 0);
    lv_obj_set_width(hdr, lv_pct(100));
  }

  // Hop rows: one row per path hash.
  // path_hashes layout: [hash_0][hash_1]...[hash_N-1]
  // path_snrs[i] = SNR accumulated by the i-th forwarding node.
  // The final hash (index numHops-1) is typically the contact/target.
  for (int i = 0; i < numHops; i++) {
    const uint8_t *hashBytes = r.hashes + (i * hashSz);
    char unknown[12];
    const char *nodeName =
        _lookupNodeName(hashBytes, hashSz, unknown, sizeof(unknown));

    // SNR for this hop: snrs[i] is the SNR the i-th node RECEIVED with.
    // numSnrs may be numHops-1 (intermediate hops only) or numHops.
    char snrBuf[16] = "?";
    if (i < r.numSnrs) {
      float snr = _snrFromRaw(r.snrs[i]);
      snprintf(snrBuf, sizeof(snrBuf), "%+.1f dB", (double)snr);
    }

    // Hash abbreviation: first 2 hex chars (1st byte).
    char hashAbbr[8];
    snprintf(hashAbbr, sizeof(hashAbbr), "%02X", hashBytes[0]);

    // Build row object.
    lv_obj_t *row = lv_obj_create(_hopList);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_set_style_pad_ver(row, 3, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Hop number
    lv_obj_t *hopLbl = lv_label_create(row);
    char hopBuf[8];
    snprintf(hopBuf, sizeof(hopBuf), "%d.", i + 1);
    lv_label_set_text(hopLbl, hopBuf);
    lv_obj_set_style_text_color(hopLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hopLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(hopLbl, 18);

    // Hash abbreviation (2 hex chars, accent colour)
    lv_obj_t *addrLbl = lv_label_create(row);
    lv_label_set_text(addrLbl, hashAbbr);
    lv_obj_set_style_text_color(addrLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(addrLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(addrLbl, 22);

    // Node name (truncated)
    lv_obj_t *nameLbl = lv_label_create(row);
    lv_label_set_text(nameLbl, nodeName);
    lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_flex_grow(nameLbl, 1);

    // SNR value (right-aligned via grow spacer before it)
    lv_obj_t *snrLbl = lv_label_create(row);
    lv_label_set_text(snrLbl, snrBuf);
    lv_obj_set_style_text_font(snrLbl, &lv_font_montserrat_10, 0);
    // Colour by signal quality
    lv_color_t snrCol = theme::TEXT_MUTED;
    if (i < r.numSnrs) {
      float snr = _snrFromRaw(r.snrs[i]);
      snrCol = (snr >= 5.0f)    ? theme::GREEN
               : (snr >= -2.5f) ? theme::ORANGE
                                : theme::RED;
    }
    lv_obj_set_style_text_color(snrLbl, snrCol, 0);
    lv_obj_set_width(snrLbl, 54);
  }
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
        _setStatus("No response - target forwarding disabled", theme::ORANGE);
      } else {
        _setStatus("No response - target not in direct RF range",
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

  // ── Target row (below top bar) ─────────────────────────────────────
  static constexpr int TARGET_ROW_H = 32;
  lv_obj_t *targetRow = lv_obj_create(_screen);
  lv_obj_set_size(targetRow, OPS_SCREEN_W, TARGET_ROW_H);
  lv_obj_align(targetRow, LV_ALIGN_TOP_LEFT, 0, TOP_H + 2);
  lv_obj_set_style_bg_color(targetRow, theme::BG, 0);
  lv_obj_set_style_border_width(targetRow, 0, 0);
  lv_obj_set_style_pad_hor(targetRow, 4, 0);
  lv_obj_set_style_pad_ver(targetRow, 2, 0);
  lv_obj_set_style_pad_column(targetRow, 6, 0);
  lv_obj_set_scrollbar_mode(targetRow, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(targetRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(targetRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(targetRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // "Target:" label
  lv_obj_t *tgtLbl = lv_label_create(targetRow);
  lv_label_set_text(tgtLbl, "Target:");
  lv_obj_set_style_text_color(tgtLbl, theme::TEXT_MUTED, 0);
  lv_obj_set_style_text_font(tgtLbl, &lv_font_montserrat_10, 0);

  // Dropdown — grows to fill available width minus trace button
  _dropdown = lv_dropdown_create(targetRow);
  lv_obj_set_flex_grow(_dropdown, 1);
  lv_obj_set_height(_dropdown, TARGET_ROW_H - 6);
  lv_obj_set_style_bg_color(_dropdown, theme::BG_CARD, 0);
  lv_obj_set_style_border_color(_dropdown, theme::BORDER, 0);
  lv_obj_set_style_border_width(_dropdown, 1, 0);
  lv_obj_set_style_text_color(_dropdown, theme::TEXT, 0);
  lv_obj_set_style_text_font(_dropdown, &lv_font_montserrat_10, 0);
  lv_obj_set_style_pad_hor(_dropdown, 4, 0);
  lv_obj_set_style_pad_ver(_dropdown, 2, 0);

  if (s_numTargets > 0) {
    lv_dropdown_set_options(_dropdown, _buildDropOptions());
    lv_dropdown_set_selected(_dropdown, 0);
    s_selIdx = 0;
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
  lv_obj_set_style_text_font(dropList, &lv_font_montserrat_10, 0);
  lv_obj_set_style_max_height(dropList, 140, 0);

  // "Trace" button
  _traceBtn = lv_btn_create(targetRow);
  lv_obj_set_size(_traceBtn, 58, TARGET_ROW_H - 6);
  lv_obj_set_style_bg_color(_traceBtn, theme::PRIMARY, 0);
  lv_obj_set_style_bg_color(_traceBtn, theme::ACCENT, LV_STATE_PRESSED);
  lv_obj_set_style_radius(_traceBtn, 4, 0);
  lv_obj_set_style_border_width(_traceBtn, 0, 0);
  lv_obj_set_style_shadow_width(_traceBtn, 0, 0);
  lv_obj_set_style_pad_hor(_traceBtn, 4, 0);
  lv_obj_set_style_pad_ver(_traceBtn, 2, 0);
  lv_obj_add_event_cb(_traceBtn, _onTraceClick, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *tBtnLbl = lv_label_create(_traceBtn);
  lv_label_set_text(tBtnLbl, LV_SYMBOL_LOOP " Trace");
  lv_obj_set_style_text_color(tBtnLbl, theme::TEXT, 0);
  lv_obj_set_style_text_font(tBtnLbl, &lv_font_montserrat_10, 0);
  lv_obj_center(tBtnLbl);

  // Disable trace btn if no targets or no path known; build initial status
  // text.
  char initStatus[56] = "No contacts or repeaters saved";
  lv_color_t initCol = theme::TEXT_MUTED;
  if (s_selIdx >= 0) {
    auto &mesh0 = ops::MeshService::instance();
    bool pathOk0 = mesh0.hasPathTo(s_targets[s_selIdx].pubKeyPrefix);
    if (!pathOk0) {
      lv_obj_add_state(_traceBtn, LV_STATE_DISABLED);
      strncpy(initStatus, "No path known yet - wait for advert",
              sizeof(initStatus) - 1);
      initCol = theme::ORANGE;
    } else {
      ops::PathInfo pi0{};
      mesh0.getContactPath(s_targets[s_selIdx].pubKeyPrefix, pi0);
      if (pi0.direct) {
        strncpy(initStatus, "Direct - trace needs target forwarding",
                sizeof(initStatus) - 1);
      } else {
        snprintf(initStatus, sizeof(initStatus),
                 "Via %d relay(s) - may not return to you", (int)pi0.hopCount);
      }
      initCol = theme::ACCENT;
    }
  } else {
    lv_obj_add_state(_traceBtn, LV_STATE_DISABLED);
  }

  // ── Status bar ────────────────────────────────────────────────────
  static constexpr int STATUS_H = 18;
  lv_obj_t *statusRow = lv_obj_create(_screen);
  lv_obj_set_size(statusRow, OPS_SCREEN_W, STATUS_H);
  lv_obj_align(statusRow, LV_ALIGN_TOP_LEFT, 0, TOP_H + 2 + TARGET_ROW_H + 2);
  lv_obj_set_style_bg_color(statusRow, theme::BG, 0);
  lv_obj_set_style_border_width(statusRow, 0, 0);
  lv_obj_set_style_pad_hor(statusRow, 6, 0);
  lv_obj_set_style_pad_ver(statusRow, 1, 0);
  lv_obj_clear_flag(statusRow, LV_OBJ_FLAG_SCROLLABLE);

  _statusLbl = lv_label_create(statusRow);
  lv_label_set_text(_statusLbl, initStatus);
  lv_obj_set_style_text_color(_statusLbl, initCol, 0);
  lv_obj_set_style_text_font(_statusLbl, &lv_font_montserrat_10, 0);

  // ── Hop list (scrollable) ────────────────────────────���────────────
  int hopListY = TOP_H + 2 + TARGET_ROW_H + 2 + STATUS_H + 2;
  int hopListH = OPS_SCREEN_H - hopListY - 2;

  _hopList = lv_obj_create(_screen);
  lv_obj_set_size(_hopList, OPS_SCREEN_W - 4, hopListH);
  lv_obj_align(_hopList, LV_ALIGN_TOP_LEFT, 2, hopListY);
  lv_obj_set_style_bg_color(_hopList, theme::BG, 0);
  lv_obj_set_style_border_width(_hopList, 0, 0);
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

  auto &mesh = ops::MeshService::instance();
  bool pathOk = mesh.hasPathTo(s_targets[s_selIdx].pubKeyPrefix);
  if (pathOk) {
    lv_obj_clear_state(_traceBtn, LV_STATE_DISABLED);
    ops::PathInfo pi{};
    mesh.getContactPath(s_targets[s_selIdx].pubKeyPrefix, pi);
    char msg[48];
    if (pi.direct) {
      snprintf(msg, sizeof(msg), "Direct - trace needs target forwarding");
    } else {
      snprintf(msg, sizeof(msg), "Via %d relay(s) — may not return to you",
               (int)pi.hopCount);
    }
    _setStatus(msg, theme::ACCENT);
  } else {
    lv_obj_add_state(_traceBtn, LV_STATE_DISABLED);
    _setStatus("No path known yet - wait for advert", theme::ORANGE);
  }
}

void ScreenTrace::_onTraceClick(lv_event_t * /*e*/) {
  if (s_selIdx < 0 || s_selIdx >= s_numTargets)
    return;

  const TraceTarget &tgt = s_targets[s_selIdx];
  uint32_t tag = 0;
  bool ok = ops::MeshService::instance().sendTrace(tgt.pubKeyPrefix, tag);
  if (!ok) {
    _setStatus("Send failed (no path?)", theme::RED);
    return;
  }

  ops::PathInfo pi{};
  ops::MeshService::instance().getContactPath(tgt.pubKeyPrefix, pi);
  s_pendingTag = tag;
  s_traceInFlight = true;
  s_pendingUntilMs = millis() + 15000;
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

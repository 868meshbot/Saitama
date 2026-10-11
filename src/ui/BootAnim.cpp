// Saitama — BootAnim.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "BootAnim.h"
#include "../version.h"
#include <Arduino.h>
#include <esp_random.h>
#include <math.h>

namespace ops {
namespace bootanim {

// ── Layout / timing ──────────────────────────────────────────────────
static constexpr int      MAX_NODES   = 9;
static constexpr int      MAX_EDGES   = 24;
static constexpr int      AREA_X0     = 20;    // node area (wordmark sits below)
static constexpr int      AREA_X1     = OPS_SCREEN_W - 20;
static constexpr int      AREA_Y0     = 20;
static constexpr int      AREA_Y1     = OPS_SCREEN_H - 52;
static constexpr int      NODE_SZ     = 8;
static constexpr int      PKT_SZ      = 24;    // packet halo
static constexpr int      TRAIL_N     = 4;
static constexpr int      HIST_N      = TRAIL_N * 3 + 1;
static constexpr uint32_t FRAME_MS    = 33;
static constexpr uint32_t PAUSE_MS    = 520;
static constexpr uint32_t PULSE_MS    = 650;
static constexpr uint32_t HOT_FADE_MS = 1100;  // visited node fades amber → teal
static constexpr uint32_t INTRO_STEP  = 110;   // node n appears at n * INTRO_STEP
static constexpr uint32_t INTRO_FADE  = 300;
static constexpr float    SPEED_PX_MS = 0.11f;

// ── Palette (fixed: the boot screen is always on black) ─────────────
static lv_color_t _c(uint32_t rgb) { return lv_color_hex(rgb); }
#define COL_EDGE_IDLE   _c(0x1E3A46)
#define COL_EDGE_ACTIVE _c(0x3FA7C4)
#define COL_NODE_FILL   _c(0x0E1A20)
#define COL_NODE_RING   _c(0x3FA7C4)
#define COL_PACKET      _c(0xFFB000)
#define COL_WORDMARK    _c(0x7FA3B5)
#define COL_VERSION     _c(0x41545E)

struct Node
{
    float     bx, by;          // base position
    float     ax, ay;          // drift amplitude
    float     wx, wy;          // drift angular speed (rad/ms)
    float     phx, phy;        // drift phase
    float     x, y;            // current position
    int       ix, iy;          // last drawn position
    uint32_t  hotAt;           // packet left this node at (0 = never)
    bool      hot;
    lv_obj_t* obj;
};

struct Edge
{
    uint8_t    a, b;
    lv_point_t pts[2];
    lv_obj_t*  line;
};

static Node       s_nodes[MAX_NODES];
static Edge       s_edges[MAX_EDGES];
static int        s_nNodes  = 0;
static int        s_nEdges  = 0;
static lv_timer_t* s_timer  = nullptr;
static lv_obj_t*  s_pkt     = nullptr;
static lv_obj_t*  s_pulse   = nullptr;
static lv_obj_t*  s_trail[TRAIL_N] = {};
static lv_point_t s_sPts[17];
static lv_point_t s_hist[HIST_N];
static int        s_histHead = 0;

static uint32_t   s_startMs   = 0;
static bool       s_intro     = true;
static bool       s_travel    = false;
static int        s_from      = 0;
static int        s_to        = 0;
static int        s_prev      = -1;
static int        s_edgeIdx   = -1;
static uint32_t   s_phaseAt   = 0;
static uint32_t   s_travelMs  = 0;
static uint32_t   s_arrivedAt = 0;

// ── Helpers ──────────────────────────────────────────────────────────
static float _rand01()
{
    return (float)(esp_random() & 0xFFFF) / 65535.0f;
}

static float _dist(float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0;
    return sqrtf(dx * dx + dy * dy);
}

static int _findEdge(int a, int b)
{
    for (int i = 0; i < s_nEdges; i++) {
        if ((s_edges[i].a == a && s_edges[i].b == b) ||
            (s_edges[i].a == b && s_edges[i].b == a)) return i;
    }
    return -1;
}

static void _addEdge(int a, int b)
{
    if (a == b || s_nEdges >= MAX_EDGES || _findEdge(a, b) >= 0) return;
    s_edges[s_nEdges].a    = (uint8_t)a;
    s_edges[s_nEdges].b    = (uint8_t)b;
    s_edges[s_nEdges].line = nullptr;
    s_nEdges++;
}

static int _ufFind(int* p, int i)
{
    while (p[i] != i) i = p[i] = p[p[i]];
    return i;
}

// Sparse organic layout: spaced-out random nodes, each linked to its two
// nearest neighbours, then any separate clusters joined by their closest pair.
static void _buildGraph()
{
    s_nNodes = 7 + (int)(esp_random() % 3);   // 7..9
    float minD = 70.0f;
    for (int i = 0; i < s_nNodes; i++) {
        Node& n = s_nodes[i];
        for (int tries = 0; ; tries++) {
            n.bx = AREA_X0 + _rand01() * (AREA_X1 - AREA_X0);
            n.by = AREA_Y0 + _rand01() * (AREA_Y1 - AREA_Y0);
            bool ok = true;
            for (int j = 0; j < i && ok; j++) {
                ok = _dist(n.bx, n.by, s_nodes[j].bx, s_nodes[j].by) >= minD;
            }
            if (ok) break;
            if (tries > 60) { minD *= 0.85f; tries = 0; }
        }
        n.ax  = 5.0f + _rand01() * 5.0f;
        n.ay  = 4.0f + _rand01() * 5.0f;
        n.wx  = 0.00045f + _rand01() * 0.00035f;   // 8-14 s periods
        n.wy  = 0.00040f + _rand01() * 0.00035f;
        n.phx = _rand01() * 6.2832f;
        n.phy = _rand01() * 6.2832f;
        n.x   = n.bx;
        n.y   = n.by;
        n.ix  = n.iy = -1000;
        n.hotAt = 0;
        n.hot   = false;
        n.obj   = nullptr;
    }

    s_nEdges = 0;
    for (int i = 0; i < s_nNodes; i++) {
        int best[2] = { -1, -1 };
        float bd[2] = { 1e9f, 1e9f };
        for (int j = 0; j < s_nNodes; j++) {
            if (j == i) continue;
            float d = _dist(s_nodes[i].bx, s_nodes[i].by, s_nodes[j].bx, s_nodes[j].by);
            if (d < bd[0])      { bd[1] = bd[0]; best[1] = best[0]; bd[0] = d; best[0] = j; }
            else if (d < bd[1]) { bd[1] = d; best[1] = j; }
        }
        if (best[0] >= 0) _addEdge(i, best[0]);
        if (best[1] >= 0) _addEdge(i, best[1]);
    }

    int parent[MAX_NODES];
    for (int i = 0; i < s_nNodes; i++) parent[i] = i;
    for (int e = 0; e < s_nEdges; e++) {
        parent[_ufFind(parent, s_edges[e].a)] = _ufFind(parent, s_edges[e].b);
    }
    for (;;) {
        int ba = -1, bb = -1;
        float bd = 1e9f;
        for (int i = 0; i < s_nNodes; i++) {
            for (int j = i + 1; j < s_nNodes; j++) {
                if (_ufFind(parent, i) == _ufFind(parent, j)) continue;
                float d = _dist(s_nodes[i].bx, s_nodes[i].by, s_nodes[j].bx, s_nodes[j].by);
                if (d < bd) { bd = d; ba = i; bb = j; }
            }
        }
        if (ba < 0) break;   // one cluster
        _addEdge(ba, bb);
        parent[_ufFind(parent, ba)] = _ufFind(parent, bb);
    }
}

// The packet: a bold S from two 240° arcs, drawn as one rounded polyline.
static void _buildSPoints()
{
    const float r  = 4.5f;
    const float cx = PKT_SZ / 2.0f, cy = PKT_SZ / 2.0f;
    int k = 0;
    for (int i = 0; i <= 8; i++) {   // upper bowl: top-right, over the top, to centre
        float a = (-30.0f - 240.0f * i / 8.0f) * 0.017453f;
        s_sPts[k].x = (lv_coord_t)lroundf(cx + r * cosf(a));
        s_sPts[k].y = (lv_coord_t)lroundf(cy - r + r * sinf(a));
        k++;
    }
    for (int i = 1; i <= 8; i++) {   // lower bowl: centre, round the right, to bottom-left
        float a = (-90.0f + 240.0f * i / 8.0f) * 0.017453f;
        s_sPts[k].x = (lv_coord_t)lroundf(cx + r * cosf(a));
        s_sPts[k].y = (lv_coord_t)lroundf(cy + r + r * sinf(a));
        k++;
    }
}

static lv_obj_t* _plainObj(lv_obj_t* parent)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return o;
}

static void _placeEdge(Edge& e)
{
    const Node& a = s_nodes[e.a];
    const Node& b = s_nodes[e.b];
    int x0 = a.ix < b.ix ? a.ix : b.ix;
    int y0 = a.iy < b.iy ? a.iy : b.iy;
    e.pts[0].x = (lv_coord_t)(a.ix - x0 + 1);
    e.pts[0].y = (lv_coord_t)(a.iy - y0 + 1);
    e.pts[1].x = (lv_coord_t)(b.ix - x0 + 1);
    e.pts[1].y = (lv_coord_t)(b.iy - y0 + 1);
    lv_obj_set_pos(e.line, (lv_coord_t)(x0 - 1), (lv_coord_t)(y0 - 1));
    lv_line_set_points(e.line, e.pts, 2);   // re-sizes and invalidates
}

static void _setEdgeActive(int idx, bool on)
{
    if (idx < 0) return;
    lv_obj_set_style_line_color(s_edges[idx].line, on ? COL_EDGE_ACTIVE : COL_EDGE_IDLE, 0);
}

// Moves nodes (and their links) only when a whole pixel changes — the drift
// is slow, so most frames redraw nothing but the packet.
static void _driftNodes(uint32_t t)
{
    bool moved[MAX_NODES] = {};
    for (int i = 0; i < s_nNodes; i++) {
        Node& n = s_nodes[i];
        n.x = n.bx + n.ax * sinf(t * n.wx + n.phx);
        n.y = n.by + n.ay * cosf(t * n.wy + n.phy);
        int ix = (int)lroundf(n.x), iy = (int)lroundf(n.y);
        if (ix != n.ix || iy != n.iy) {
            n.ix = ix;
            n.iy = iy;
            lv_obj_set_pos(n.obj, (lv_coord_t)(ix - NODE_SZ / 2), (lv_coord_t)(iy - NODE_SZ / 2));
            moved[i] = true;
        }
    }
    for (int e = 0; e < s_nEdges; e++) {
        if (moved[s_edges[e].a] || moved[s_edges[e].b]) _placeEdge(s_edges[e]);
    }
}

static void _fadeHotNodes(uint32_t now)
{
    for (int i = 0; i < s_nNodes; i++) {
        Node& n = s_nodes[i];
        if (!n.hot) continue;
        uint32_t e = now - n.hotAt;
        if (e >= HOT_FADE_MS) {
            n.hot = false;
            lv_obj_set_style_border_color(n.obj, COL_NODE_RING, 0);
            continue;
        }
        lv_opa_t mix = (lv_opa_t)(255 - (e * 255) / HOT_FADE_MS);
        lv_obj_set_style_border_color(n.obj, lv_color_mix(COL_PACKET, COL_NODE_RING, mix), 0);
    }
}

static void _intro(uint32_t el)
{
    bool done = true;
    for (int i = 0; i < s_nNodes; i++) {
        uint32_t at = (uint32_t)i * INTRO_STEP;
        lv_opa_t o = LV_OPA_TRANSP;
        if (el >= at + INTRO_FADE) o = LV_OPA_COVER;
        else if (el > at)          o = (lv_opa_t)(((el - at) * 255) / INTRO_FADE);
        if (o != LV_OPA_COVER) done = false;
        lv_obj_set_style_opa(s_nodes[i].obj, o, 0);
    }
    for (int e = 0; e < s_nEdges; e++) {
        // A link fades in once its later endpoint has appeared.
        int last = s_edges[e].a > s_edges[e].b ? s_edges[e].a : s_edges[e].b;
        uint32_t at = (uint32_t)last * INTRO_STEP + INTRO_FADE / 2;
        lv_opa_t o = LV_OPA_TRANSP;
        if (el >= at + INTRO_FADE) o = LV_OPA_COVER;
        else if (el > at)          o = (lv_opa_t)(((el - at) * 255) / INTRO_FADE);
        if (o != LV_OPA_COVER) done = false;
        lv_obj_set_style_opa(s_edges[e].line, o, 0);
    }
    if (done) s_intro = false;
}

static void _placePacket(float x, float y)
{
    lv_obj_set_pos(s_pkt, (lv_coord_t)lroundf(x) - PKT_SZ / 2, (lv_coord_t)lroundf(y) - PKT_SZ / 2);
    s_histHead = (s_histHead + 1) % HIST_N;
    s_hist[s_histHead].x = (lv_coord_t)lroundf(x);
    s_hist[s_histHead].y = (lv_coord_t)lroundf(y);
    for (int k = 0; k < TRAIL_N; k++) {
        const lv_point_t& p = s_hist[(s_histHead - (k + 1) * 3 + HIST_N * 2) % HIST_N];
        lv_coord_t sz = lv_obj_get_width(s_trail[k]);
        lv_obj_set_pos(s_trail[k], p.x - sz / 2, p.y - sz / 2);
    }
}

static void _resetTrail(float x, float y)
{
    for (int i = 0; i < HIST_N; i++) {
        s_hist[i].x = (lv_coord_t)lroundf(x);
        s_hist[i].y = (lv_coord_t)lroundf(y);
    }
}

static void _startTravel(uint32_t now)
{
    int cand[MAX_NODES];
    int nc = 0;
    for (int e = 0; e < s_nEdges; e++) {
        int other = -1;
        if (s_edges[e].a == s_from) other = s_edges[e].b;
        else if (s_edges[e].b == s_from) other = s_edges[e].a;
        if (other >= 0 && other != s_prev) cand[nc++] = other;
    }
    if (nc == 0) cand[nc++] = s_prev;   // dead end: bounce back
    s_to       = cand[esp_random() % nc];
    s_edgeIdx  = _findEdge(s_from, s_to);
    float d    = _dist(s_nodes[s_from].x, s_nodes[s_from].y, s_nodes[s_to].x, s_nodes[s_to].y);
    s_travelMs = (uint32_t)(d / SPEED_PX_MS);
    if (s_travelMs < 450)  s_travelMs = 450;
    if (s_travelMs > 1600) s_travelMs = 1600;
    s_travel   = true;
    s_phaseAt  = now;
    _setEdgeActive(s_edgeIdx, true);
    s_nodes[s_from].hot   = true;      // leaves an amber afterglow
    s_nodes[s_from].hotAt = now;
}

static void _arrive(uint32_t now)
{
    _setEdgeActive(s_edgeIdx, false);
    s_prev      = s_from;
    s_from      = s_to;
    s_travel    = false;
    s_phaseAt   = now;
    s_arrivedAt = now;
    lv_obj_clear_flag(s_pulse, LV_OBJ_FLAG_HIDDEN);
}

static void _updatePulse(uint32_t now)
{
    if (lv_obj_has_flag(s_pulse, LV_OBJ_FLAG_HIDDEN)) return;
    uint32_t e = now - s_arrivedAt;
    if (e >= PULSE_MS) {
        lv_obj_add_flag(s_pulse, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_coord_t sz = (lv_coord_t)(12 + (e * 26) / PULSE_MS);
    const Node& n = s_nodes[s_from];
    lv_obj_set_size(s_pulse, sz, sz);
    lv_obj_set_pos(s_pulse, (lv_coord_t)(n.ix - sz / 2), (lv_coord_t)(n.iy - sz / 2));
    lv_obj_set_style_border_opa(s_pulse, (lv_opa_t)(200 - (e * 200) / PULSE_MS), 0);
}

static void _onTimer(lv_timer_t* /*t*/)
{
    uint32_t now = millis();
    uint32_t el  = now - s_startMs;

    _driftNodes(now);
    if (s_intro) {
        _intro(el);
        if (s_intro) return;
        // Intro done: the packet appears on its first node.
        lv_obj_clear_flag(s_pkt, LV_OBJ_FLAG_HIDDEN);
        for (int k = 0; k < TRAIL_N; k++) lv_obj_clear_flag(s_trail[k], LV_OBJ_FLAG_HIDDEN);
        _resetTrail(s_nodes[s_from].x, s_nodes[s_from].y);
        s_phaseAt = s_arrivedAt = now;
        lv_obj_clear_flag(s_pulse, LV_OBJ_FLAG_HIDDEN);
    }

    _fadeHotNodes(now);
    _updatePulse(now);

    if (s_travel) {
        float p = (float)(now - s_phaseAt) / (float)s_travelMs;
        if (p >= 1.0f) {
            _placePacket(s_nodes[s_to].x, s_nodes[s_to].y);
            _arrive(now);
            return;
        }
        float s = p * p * (3.0f - 2.0f * p);   // ease in/out
        const Node& a = s_nodes[s_from];
        const Node& b = s_nodes[s_to];
        _placePacket(a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s);
    } else {
        _placePacket(s_nodes[s_from].x, s_nodes[s_from].y);   // ride the drifting node
        if (now - s_phaseAt >= PAUSE_MS) _startTravel(now);
    }
}

// ── Public ───────────────────────────────────────────────────────────
void create(lv_obj_t* parent)
{
    destroy();
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    _buildGraph();
    _buildSPoints();

    // Links first so nodes draw over them.
    for (int e = 0; e < s_nEdges; e++) {
        lv_obj_t* l = lv_line_create(parent);
        lv_obj_clear_flag(l, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_set_style_line_width(l, 1, 0);
        lv_obj_set_style_line_color(l, COL_EDGE_IDLE, 0);
        lv_obj_set_style_opa(l, LV_OPA_TRANSP, 0);
        s_edges[e].line = l;
    }
    for (int i = 0; i < s_nNodes; i++) {
        lv_obj_t* o = _plainObj(parent);
        lv_obj_set_size(o, NODE_SZ, NODE_SZ);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(o, COL_NODE_FILL, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(o, COL_NODE_RING, 0);
        lv_obj_set_style_border_width(o, 2, 0);
        lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
        s_nodes[i].obj = o;
    }
    _driftNodes(millis());

    s_pulse = _plainObj(parent);
    lv_obj_set_style_radius(s_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_pulse, COL_PACKET, 0);
    lv_obj_set_style_border_width(s_pulse, 1, 0);
    lv_obj_add_flag(s_pulse, LV_OBJ_FLAG_HIDDEN);

    static const lv_coord_t kTrailSz[TRAIL_N]  = { 5, 4, 3, 3 };
    static const lv_opa_t   kTrailOpa[TRAIL_N] = { 150, 105, 70, 40 };
    for (int k = 0; k < TRAIL_N; k++) {
        lv_obj_t* d = _plainObj(parent);
        lv_obj_set_size(d, kTrailSz[k], kTrailSz[k]);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, COL_PACKET, 0);
        lv_obj_set_style_bg_opa(d, kTrailOpa[k], 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        s_trail[k] = d;
    }

    s_pkt = _plainObj(parent);
    lv_obj_set_size(s_pkt, PKT_SZ, PKT_SZ);
    lv_obj_set_style_radius(s_pkt, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pkt, COL_PACKET, 0);
    lv_obj_set_style_bg_opa(s_pkt, 55, 0);
    lv_obj_add_flag(s_pkt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* sLine = lv_line_create(s_pkt);
    lv_obj_clear_flag(sLine, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(sLine, s_sPts, 17);
    lv_obj_set_style_line_width(sLine, 4, 0);
    lv_obj_set_style_line_rounded(sLine, true, 0);
    lv_obj_set_style_line_color(sLine, COL_PACKET, 0);

    lv_obj_t* word = lv_label_create(parent);
    lv_label_set_text(word, "SAITAMA");
    lv_obj_set_style_text_font(word, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(word, COL_WORDMARK, 0);
    lv_obj_set_style_text_letter_space(word, 6, 0);
    lv_obj_align(word, LV_ALIGN_BOTTOM_MID, 3, -18);   // +3: letter-space trails

    lv_obj_t* ver = lv_label_create(parent);
    lv_label_set_text(ver, "v" OPS_VERSION_STRING);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(ver, COL_VERSION, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -4);

    s_from     = (int)(esp_random() % s_nNodes);
    s_prev     = -1;
    s_edgeIdx  = -1;
    s_travel   = false;
    s_intro    = true;
    s_startMs  = millis();
    s_timer    = lv_timer_create(_onTimer, FRAME_MS, nullptr);
}

void destroy()
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    s_pkt   = nullptr;
    s_pulse = nullptr;
    for (int k = 0; k < TRAIL_N; k++) s_trail[k] = nullptr;
    for (int i = 0; i < s_nNodes; i++) s_nodes[i].obj = nullptr;
    for (int e = 0; e < s_nEdges; e++) s_edges[e].line = nullptr;
    s_nNodes = 0;
    s_nEdges = 0;
}

}  // namespace bootanim
}  // namespace ops

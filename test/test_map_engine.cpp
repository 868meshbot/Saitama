// Saitama — MapEngine unit tests (host-side, no Arduino)
// Copyright 2026 Saitama — MIT License
//
// Pure math tests for coordinate conversion. Runs on host (x86).
// Coordinate math is duplicated from MapEngine.cpp since we can't
// include Arduino.h on the host. These MUST stay in sync with source.

#include <cmath>
#include <cstdio>
#include <cstdlib>

static void latLngToTile(float lat, float lng, int z, int& tx, int& ty) {
    float n = powf(2.0f, z);
    tx = (int)((lng + 180.0f) / 360.0f * n);
    float latRad = lat * M_PI / 180.0f;
    ty = (int)((1.0f - logf(tanf(latRad) + 1.0f / cosf(latRad)) / M_PI) / 2.0f * n);
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tx >= (int)n) tx = (int)n - 1;
    if (ty >= (int)n) ty = (int)n - 1;
}

static void tileToLatLng(int tx, int ty, int z, float& lat, float& lng) {
    float n = powf(2.0f, z);
    lng = tx / n * 360.0f - 180.0f;
    float latRad = atanf(sinhf(M_PI * (1.0f - 2.0f * ty / n)));
    lat = latRad * 180.0f / M_PI;
}

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) == (b)) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL: %s — expected %d, got %d\n", msg, (int)(b), (int)(a)); } \
} while(0)

#define ASSERT_NEAR(a, b, eps, msg) do { \
    tests_run++; \
    if (fabs((a) - (b)) < (eps)) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL: %s — expected %.6f, got %.6f (eps %.6f)\n", msg, (double)(b), (double)(a), (double)(eps)); } \
} while(0)

// ── lat/lng → tile ──────────────────────────────────────────────────

void test_zoom0_center() {
    int x, y;
    latLngToTile(0.0f, 0.0f, 0, x, y);
    ASSERT_EQ(x, 0, "zoom0 center x");
    ASSERT_EQ(y, 0, "zoom0 center y");
}

void test_zoom0_corners() {
    int x, y;
    latLngToTile(85.05f, -180.0f, 0, x, y);
    ASSERT_EQ(x, 0, "zoom0 top-left x");
    ASSERT_EQ(y, 0, "zoom0 top-left y");

    latLngToTile(-85.05f, 180.0f, 0, x, y);
    ASSERT_EQ(x, 0, "zoom0 bottom-right x");
    ASSERT_EQ(y, 0, "zoom0 bottom-right y");
}

void test_zoom10_luxembourg() {
    int x, y;
    latLngToTile(49.6117f, 6.1300f, 10, x, y);
    ASSERT_EQ(x, 529, "zoom10 lux x");
    ASSERT_EQ(y, 348, "zoom10 lux y");
}

void test_zoom14_luxembourg() {
    int x, y;
    latLngToTile(49.6117f, 6.1300f, 14, x, y);
    ASSERT_EQ(x, 8470, "zoom14 lux x");
    ASSERT_EQ(y, 5583, "zoom14 lux y");
}

void test_zoom10_berlin() {
    int x, y;
    latLngToTile(52.5200f, 13.4050f, 10, x, y);
    ASSERT_EQ(x, 550, "zoom10 berlin x");
    ASSERT_EQ(y, 335, "zoom10 berlin y");
}

void test_zoom10_paris() {
    int x, y;
    latLngToTile(48.8566f, 2.3522f, 10, x, y);
    ASSERT_EQ(x, 518, "zoom10 paris x");
    ASSERT_EQ(y, 352, "zoom10 paris y");
}

void test_zoom10_nyc() {
    int x, y;
    latLngToTile(40.7128f, -74.0060f, 10, x, y);
    ASSERT_EQ(x, 301, "zoom10 nyc x");
    ASSERT_EQ(y, 385, "zoom10 nyc y");
}

void test_clamp_beyond_range() {
    int x, y;
    latLngToTile(89.0f, 200.0f, 10, x, y);
    ASSERT_EQ(x, 1023, "zoom10 clamped x (200 lng)");
    ASSERT_EQ(y, 0, "zoom10 clamped y (89 lat)");

    latLngToTile(-89.0f, -200.0f, 10, x, y);
    ASSERT_EQ(x, 0, "zoom10 clamped x (-200 lng)");
    ASSERT_EQ(y, 1023, "zoom10 clamped y (-89 lat)");
}

void test_equator_zero_longitude() {
    int x, y;
    latLngToTile(0.0f, 0.0f, 5, x, y);
    ASSERT_EQ(x, 16, "zoom5 equator x");
    ASSERT_EQ(y, 16, "zoom5 equator y");
}

void test_zoom18_high_zoom() {
    int x, y;
    latLngToTile(49.6117f, 6.1300f, 18, x, y);
    ASSERT_EQ(x > 0, true, "zoom18 x positive");
    ASSERT_EQ(y > 0, true, "zoom18 y positive");
    ASSERT_EQ(x < (1 << 18), true, "zoom18 x in range");
    ASSERT_EQ(y < (1 << 18), true, "zoom18 y in range");
}

// ── tile → lat/lng ──────────────────────────────────────────────────

void test_roundtrip_zoom10() {
    int orig_x = 529, orig_y = 348, zoom = 10;
    float lat, lng;
    tileToLatLng(orig_x, orig_y, zoom, lat, lng);

    int back_x, back_y;
    latLngToTile(lat, lng, zoom, back_x, back_y);
    ASSERT_EQ(back_x, orig_x, "roundtrip zoom10 x");
    // float32 precision: ±1 tile at any zoom level
    ASSERT_EQ(back_y >= orig_y - 1 && back_y <= orig_y + 1, true, "roundtrip zoom10 y (±1)");
}

void test_roundtrip_zoom14() {
    int orig_x = 8470, orig_y = 5583, zoom = 14;
    float lat, lng;
    tileToLatLng(orig_x, orig_y, zoom, lat, lng);

    int back_x, back_y;
    latLngToTile(lat, lng, zoom, back_x, back_y);
    ASSERT_EQ(back_x, orig_x, "roundtrip zoom14 x");
    // float32 loses precision at zoom 14 — allow ±1 tile drift
    ASSERT_EQ(back_y >= orig_y - 1 && back_y <= orig_y + 1, true, "roundtrip zoom14 y (±1)");
}

void test_tile_to_lat_lng_zoom0() {
    float lat, lng;
    tileToLatLng(0, 0, 0, lat, lng);
    ASSERT_NEAR(lat, 85.05, 1.0, "zoom0 tile lat");
    ASSERT_NEAR(lng, -180.0, 1.0, "zoom0 tile lng");
}

void test_tile_to_lat_lng_lux_zoom10() {
    float lat, lng;
    tileToLatLng(529, 348, 10, lat, lng);
    // Top-left corner of tile (529, 348) should be near Luxembourg
    ASSERT_NEAR(lat, 49.65, 0.5, "zoom10 lux tile lat");
    ASSERT_NEAR(lng, 5.97, 1.0, "zoom10 lux tile lng");
}

// ── Main ─────────────────────────────────────────────────────────────

int main() {
    printf("Saitama MapEngine Tests\n");
    printf("==========================\n\n");

    printf("lat/lng to tile:\n");
    test_zoom0_center();
    test_zoom0_corners();
    test_zoom10_luxembourg();
    test_zoom14_luxembourg();
    test_zoom10_berlin();
    test_zoom10_paris();
    test_zoom10_nyc();
    test_clamp_beyond_range();
    test_equator_zero_longitude();
    test_zoom18_high_zoom();

    printf("\ntile to lat/lng:\n");
    test_roundtrip_zoom10();
    test_roundtrip_zoom14();
    test_tile_to_lat_lng_zoom0();
    test_tile_to_lat_lng_lux_zoom10();

    printf("\n==========================\n");
    printf("Results: %d/%d passed, %d failed\n", tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
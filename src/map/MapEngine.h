// Saitama — MapEngine.h
// Copyright 2026 Saitama — MIT License
//
// Offline tile-based map renderer for ESP32.
// Tiles stored on SD card in /map/ directory structure:
//   /map/{z}/{x}/{y}.png
// Using OpenStreetMap tile coordinates (slippy map).

#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace ops {

struct MapNode {
    float lat;
    float lng;
    char  name[16];
    int   rssi;           // signal strength
    bool  isRepeater;
};

class MapEngine {
public:
    void init();
    void tick();

    // Pan/zoom
    void setCenter(float lat, float lng);
    void setZoom(int z);         // z = 0..18
    void pan(int dxPx, int dyPx);
    void zoomIn();
    void zoomOut();

    // Nodes
    void addNode(const MapNode& node);
    void clearNodes();
    uint16_t nodeCount() const;

    // Tile management
    bool hasTileDir() const;      // SD card present with /maps/
    bool tileExists(int z, int x, int y) const;

    // Rendering — called from LVGL tick
    // Returns true if a new tile was loaded this frame
    bool renderFrame();

    // Coordinate conversion
    static void latLngToTile(float lat, float lng, int z, int& tx, int& ty);
    static void tileToLatLng(int tx, int ty, int z, float& lat, float& lng);

    // Returns fractional tile coordinates (integer part = tile index, fraction = pixel offset / 256)
    static void latLngToTileFrac(float lat, float lng, int z, float& tx_f, float& ty_f);

    // Read raw PNG bytes for a tile into buf. Returns bytes read (0 = not found/SD unavailable).
    size_t readTile(int z, int x, int y, uint8_t* buf, size_t bufMax) const;

private:
    float  _centerLat = 49.6117f;   // Luxembourg :)
    float  _centerLng = 6.1300f;
    int    _zoom = 10;                // country-level default
    MapNode _nodes[64];
    uint16_t _nodeCount = 0;
    bool   _sdPresent = false;
};

}  // namespace oms
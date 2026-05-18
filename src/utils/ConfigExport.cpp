// Saitama -- ConfigExport.cpp
// Copyright 2026 Saitama — MIT License
//
// Export/import config + identity to/from SD card.
// MeshCore-compatible format on SD card:
//   /oms/config.json    -- Saitama settings (JSON, same format as SPIFFS)
//   /oms/identity.id    -- MeshCore local identity (binary, IdentityStore format)
//   /oms/regions.bin    -- MeshCore region map (binary, SimpleMeshTables format)
//   /oms/contacts/      -- Exported contacts as binary advert packets

#include "ConfigExport.h"
#include "Config.h"
#include "Log.h"
#include <SPIFFS.h>
#include <SD.h>

namespace ops {

static const char* OPS_DIR       = "/ops";
static const char* OPS_CONFIG   = "/ops/config.json";
static const char* OPS_IDENTITY = "/ops/identity.id";
static const char* OPS_REGIONS  = "/ops/regions.bin";
static const char* OPS_CONTACTS = "/ops/contacts";

// ── Helpers ─────────────────────────────────────────────────────────

static bool ensureOMSDir() {
    if (!SD.exists(OPS_DIR)) {
        if (!SD.mkdir(OPS_DIR)) {
            OPS_LOG("ConfigExport", "Failed to create %s on SD", OPS_DIR);
            return false;
        }
    }
    return true;
}

// Copy a file from src filesystem to dst filesystem
// fsSrc/fsDst are SPIFFS or SD, paths include full path
static bool copyFile(fs::FS& fsSrc, const char* srcPath,
                     fs::FS& fsDst, const char* dstPath) {
    File src = fsSrc.open(srcPath, "r");
    if (!src) return false;

    File dst = fsDst.open(dstPath, "w");
    if (!dst) { src.close(); return false; }

    uint8_t buf[512];
    while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        if (n > 0) dst.write(buf, n);
    }
    src.close();
    dst.close();
    return true;
}

// ── SD Config Check ─────────────────────────────────────────────────

bool hasSDConfig() {
    if (!SD.exists(OPS_DIR)) return false;
    return SD.exists(OPS_CONFIG) || SD.exists(OPS_IDENTITY);
}

// ── Export ───────────────────────────────────────────────────────────

bool configExportToSD() {
    OPS_LOG("ConfigExport", "Exporting config to SD card");

    if (!ensureOMSDir()) return false;

    bool ok = true;

    // 1) Export Saitama config.json (from SPIFFS to SD)
    {
        // Re-generate JSON config to SD (same format as SPIFFS)
        File f = SD.open(OPS_CONFIG, "w");
        if (f) {
            const Config& c = config::get();
            f.printf(
                "{\n"
                "  \"radioRegion\": \"%s\",\n"
                "  \"callsign\": \"%s\",\n"
                "  \"channel\": %d,\n"
                "  \"brightness\": %d,\n"
                "  \"screenTimeoutSec\": %d,\n"
                "  \"notifySound\": %s,\n"
                "  \"mapTileDir\": \"%s\",\n"
                "  \"theme\": %d\n"
                "}\n",
                c.radioRegion, c.callsign, c.activeChannel,
                c.brightness, c.screenTimeoutSec,
                c.notifySound ? "true" : "false",
                c.mapTileDir, c.theme
            );
            f.close();
            OPS_LOG("ConfigExport", "Wrote %s", OPS_CONFIG);
        } else {
            OPS_LOG("ConfigExport", "Failed to write %s", OPS_CONFIG);
            ok = false;
        }
    }

    // 2) Export MeshCore identity (copy from SPIFFS to SD)
    {
        if (SPIFFS.exists("/identity.id")) {
            if (copyFile(SPIFFS, "/identity.id", SD, OPS_IDENTITY)) {
                OPS_LOG("ConfigExport", "Wrote %s", OPS_IDENTITY);
            } else {
                OPS_LOG("ConfigExport", "Failed to export identity");
                ok = false;
            }
        } else {
            OPS_LOG("ConfigExport", "No identity file to export");
        }
    }

    // 3) Export MeshCore region map (copy from SPIFFS to SD)
    {
        if (SPIFFS.exists("/regions.bin")) {
            if (copyFile(SPIFFS, "/regions.bin", SD, OPS_REGIONS)) {
                OPS_LOG("ConfigExport", "Wrote %s", OPS_REGIONS);
            } else {
                OPS_LOG("ConfigExport", "Failed to export regions");
                ok = false;
            }
        }
    }

    if (ok) {
        OPS_LOG("ConfigExport", "Export complete");
    }
    return ok;
}

// ── Import ───────────────────────────────────────────────────────────

bool configImportFromSD() {
    OPS_LOG("ConfigExport", "Importing config from SD card");

    if (!SD.exists(OPS_DIR)) {
        OPS_LOG("ConfigExport", "No /oms directory on SD");
        return false;
    }

    bool ok = true;

    // 1) Import Saitama config.json (from SD to SPIFFS, then reload)
    {
        if (SD.exists(OPS_CONFIG)) {
            // Read SD config and write to SPIFFS
            File src = SD.open(OPS_CONFIG, "r");
            if (src) {
                File dst = SPIFFS.open("/ops.cfg", "w");
                if (dst) {
                    uint8_t buf[512];
                    while (src.available()) {
                        size_t n = src.read(buf, sizeof(buf));
                        if (n > 0) dst.write(buf, n);
                    }
                    dst.close();
                    OPS_LOG("ConfigExport", "Imported config.json -> /ops.cfg");
                } else {
                    OPS_LOG("ConfigExport", "Failed to write /ops.cfg");
                    ok = false;
                }
                src.close();
            } else {
                OPS_LOG("ConfigExport", "Failed to read %s", OPS_CONFIG);
                ok = false;
            }
        }
    }

    // 2) Import MeshCore identity (from SD to SPIFFS)
    {
        if (SD.exists(OPS_IDENTITY)) {
            if (copyFile(SD, OPS_IDENTITY, SPIFFS, "/identity.id")) {
                OPS_LOG("ConfigExport", "Imported identity.id");
            } else {
                OPS_LOG("ConfigExport", "Failed to import identity");
                ok = false;
            }
        }
    }

    // 3) Import MeshCore region map (from SD to SPIFFS)
    {
        if (SD.exists(OPS_REGIONS)) {
            if (copyFile(SD, OPS_REGIONS, SPIFFS, "/regions.bin")) {
                OPS_LOG("ConfigExport", "Imported regions.bin");
            } else {
                OPS_LOG("ConfigExport", "Failed to import regions");
                ok = false;
            }
        }
    }

    // Reload config from the newly-imported SPIFFS file
    if (ok) {
        config::init();  // re-read /ops.cfg into memory
        OPS_LOG("ConfigExport", "Import complete, config reloaded");
    }

    return ok;
}

// ── Identity-only export/import ──────────────────────────────────────

bool exportIdentityToSD(const char* filename) {
    if (!ensureOMSDir()) return false;

    const char* destPath;
    char pathBuf[64];

    if (filename) {
        snprintf(pathBuf, sizeof(pathBuf), "%s/%s", OPS_CONTACTS, filename);
        destPath = pathBuf;
        // Create contacts dir if needed
        if (!SD.exists(OPS_CONTACTS)) {
            SD.mkdir(OPS_CONTACTS);
        }
    } else {
        destPath = OPS_IDENTITY;
    }

    if (!SPIFFS.exists("/identity.id")) {
        OPS_LOG("ConfigExport", "No identity to export");
        return false;
    }

    return copyFile(SPIFFS, "/identity.id", SD, destPath);
}

bool importIdentityFromSD(const char* filename) {
    const char* srcPath;
    char pathBuf[64];

    if (filename) {
        snprintf(pathBuf, sizeof(pathBuf), "%s/%s", OPS_CONTACTS, filename);
        srcPath = pathBuf;
    } else {
        srcPath = OPS_IDENTITY;
    }

    if (!SD.exists(srcPath)) {
        OPS_LOG("ConfigExport", "Identity file not found: %s", srcPath);
        return false;
    }

    return copyFile(SD, srcPath, SPIFFS, "/identity.id");
}

}  // namespace oms
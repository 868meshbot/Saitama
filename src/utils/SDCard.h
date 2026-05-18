// Saitama — SDCard.h
// Copyright 2026 Saitama — MIT License

#pragma once
#include <Arduino.h>
#include <stddef.h>

namespace ops {
namespace sdcard {
    // Mount SD on shared SPI bus (CS=39, SCK=40, MISO=38, MOSI=41).
    // Creates /oms/ directory.  Silent no-op if no card is present.
    void init();

    bool isMounted();

    // Attempt to mount without re-initialising the SPI bus (use after a failed init()).
    // Returns true if the card is now mounted. No-op if already mounted.
    bool tryMount();

    // Binary file helpers (used for /oms/identity.bin).
    bool writeFile(const char* path, const uint8_t* data, size_t len);
    bool readFile(const char* path, uint8_t* buf, size_t maxLen, size_t* outLen);

    // Message log helpers — /oms/msgs/<tag>.log, one JSON line per message.
    bool   appendMsgLine(const char* tag, const char* json);
    size_t readMsgLog   (const char* tag, char* buf, size_t bufSize);
    bool   deleteMsgLog (const char* tag);  // removes the .log file for tag

    // Directory listing — newline-separated entries in buf; dirs wrapped in [].
    size_t   listDir(const char* path, char* buf, size_t bufSize);
    // Delete all files in /oms/msgs/.
    void     clearMsgLogs();
    // Disk capacity helpers.
    uint64_t totalMB();
    uint64_t freeMB();
    // Returns SD.exists(path), or false if not mounted.
    bool     hasFile(const char* path);
    // True when identity.bin + settings.json + contacts.json + repeaters.json all exist.
    // Use this as the gate for "SD is source of truth" boot restore.
    bool     hasCompleteBackup();
}
}

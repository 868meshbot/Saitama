// Saitama — SDCard.cpp
// Copyright 2026 Saitama — MIT License

#include "SDCard.h"
#include "Log.h"
#include <SPI.h>
#include <SD.h>

// T-Deck Plus: SD CS = GPIO 39; shared SPI bus SCK=40 MISO=38 MOSI=41
static constexpr int SD_CS = 39;

namespace ops {

static bool     s_mounted = false;
static SPIClass s_spi(FSPI);

bool sdcard::isMounted() { return s_mounted; }

void sdcard::init() {
    s_spi.begin(40, 38, 41, -1);   // SCK, MISO, MOSI, SS (CS handled by SD lib)
    if (!SD.begin(SD_CS, s_spi)) {
        OPS_LOG("SD", "No card or mount failed");
        return;
    }
    s_mounted = true;
    if (!SD.exists("/ops")) SD.mkdir("/ops");
    uint64_t free_mb = (SD.totalBytes() - SD.usedBytes()) >> 20;
    OPS_LOG("SD", "Mounted, %llu MB free", (unsigned long long)free_mb);
    OPS_LOG("SD", "Backups: id=%d contacts=%d repeaters=%d",
            SD.exists("/ops/identity.bin")  ? 1 : 0,
            SD.exists("/ops/contacts.json") ? 1 : 0,
            SD.exists("/ops/repeaters.json")? 1 : 0);
}

bool sdcard::tryMount()
{
    if (s_mounted) return true;
    if (!SD.begin(SD_CS, s_spi)) {
        OPS_LOG("SD", "Remount failed — no card?");
        return false;
    }
    s_mounted = true;
    if (!SD.exists("/ops")) SD.mkdir("/ops");
    uint64_t free_mb = (SD.totalBytes() - SD.usedBytes()) >> 20;
    OPS_LOG("SD", "Remounted, %llu MB free", (unsigned long long)free_mb);
    return true;
}

bool sdcard::writeFile(const char* path, const uint8_t* data, size_t len) {
    if (!s_mounted) return false;
    File f = SD.open(path, FILE_WRITE);
    if (!f) { OPS_LOG("SD", "Write open failed: %s", path); return false; }
    size_t written = f.write(data, len);
    f.close();
    return written == len;
}

bool sdcard::readFile(const char* path, uint8_t* buf, size_t maxLen, size_t* outLen) {
    if (!s_mounted) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t n = f.read(buf, maxLen);
    f.close();
    if (outLen) *outLen = n;
    return n > 0;
}

// ── Message log helpers ───────────────────────────────────────────────

static void _buildMsgPath(const char* tag, char* path, size_t pathSize)
{
    char safe[13] = {};
    int j = 0;
    for (int i = 0; tag[i] && j < 12; i++) {
        char c = tag[i];
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' ||
            c == '"' || c == '<'  || c == '>' || c == '|') c = '_';
        safe[j++] = c;
    }
    snprintf(path, pathSize, "/ops/msgs/%s.log", safe);
}

bool sdcard::appendMsgLine(const char* tag, const char* json)
{
    if (!s_mounted) return false;
    if (!SD.exists("/ops/msgs")) SD.mkdir("/ops/msgs");
    char path[40];
    _buildMsgPath(tag, path, sizeof(path));
    File f = SD.open(path, FILE_APPEND);
    if (!f) { OPS_LOG("SD", "appendMsgLine failed: %s", path); return false; }
    f.println(json);
    f.close();
    return true;
}

size_t sdcard::readMsgLog(const char* tag, char* buf, size_t bufSize)
{
    if (!s_mounted || !buf || bufSize < 2) return 0;
    char path[40];
    _buildMsgPath(tag, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    size_t fileSize = f.size();
    if (fileSize > bufSize - 1) f.seek(fileSize - (bufSize - 1));
    size_t n = f.read((uint8_t*)buf, bufSize - 1);
    buf[n] = '\0';
    f.close();
    return n;
}

bool sdcard::deleteMsgLog(const char* tag)
{
    if (!s_mounted) return false;
    char path[40];
    _buildMsgPath(tag, path, sizeof(path));
    if (!SD.exists(path)) return true;
    bool ok = SD.remove(path);
    OPS_LOG("SD", "deleteMsgLog '%s': %s", tag, ok ? "ok" : "fail");
    return ok;
}

size_t sdcard::listDir(const char* path, char* buf, size_t bufSize)
{
    if (!s_mounted || !buf || bufSize < 2) return 0;
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) return 0;
    size_t pos = 0;
    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        const char* full = entry.name();
        const char* base = strrchr(full, '/');
        base = base ? base + 1 : full;
        size_t blen = strlen(base);
        if (pos + blen + 4 < bufSize) {
            if (entry.isDirectory()) buf[pos++] = '[';
            memcpy(buf + pos, base, blen); pos += blen;
            if (entry.isDirectory()) buf[pos++] = ']';
            buf[pos++] = '\n';
        }
        entry.close();
    }
    buf[pos] = '\0';
    dir.close();
    return pos;
}

void sdcard::clearMsgLogs()
{
    if (!s_mounted) return;
    static char paths[20][40];
    int count = 0;
    File dir = SD.open("/ops/msgs");
    if (dir && dir.isDirectory()) {
        while (count < 20) {
            File entry = dir.openNextFile();
            if (!entry) break;
            if (!entry.isDirectory()) {
                const char* full = entry.name();
                const char* base = strrchr(full, '/');
                base = base ? base + 1 : full;
                snprintf(paths[count], sizeof(paths[count]), "/ops/msgs/%s", base);
                count++;
            }
            entry.close();
        }
        dir.close();
    }
    for (int i = 0; i < count; i++) SD.remove(paths[i]);
    OPS_LOG("SD", "clearMsgLogs: removed %d file(s)", count);
}

uint64_t sdcard::totalMB() { return s_mounted ? SD.totalBytes() >> 20 : 0; }
uint64_t sdcard::freeMB()  { return s_mounted ? (SD.totalBytes() - SD.usedBytes()) >> 20 : 0; }
bool sdcard::hasFile(const char* path) { return s_mounted && SD.exists(path); }

bool sdcard::hasCompleteBackup()
{
    if (!s_mounted) return false;
    return SD.exists("/ops/identity.bin")
        && SD.exists("/ops/settings.json")
        && SD.exists("/ops/contacts.json")
        && SD.exists("/ops/repeaters.json");
}

}  // namespace oms

// Saitama — Regions.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "Regions.h"
#include "Log.h"
#include <Preferences.h>
#include <cstring>

namespace ops {
namespace regions {

static constexpr const char* NVS_NS  = "opsreg";
static constexpr const char* NVS_KEY = "list";   // comma-separated names

static char s_names[MAX_REGIONS][NAME_LEN] = {};
static int  s_count = 0;

static void _save()
{
    char buf[MAX_REGIONS * NAME_LEN] = {};
    for (int i = 0; i < s_count; i++) {
        if (i) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, s_names[i], sizeof(buf) - strlen(buf) - 1);
    }
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putString(NVS_KEY, buf);
    p.end();
}

// Trims a leading '#'/spaces and trailing spaces; false if not a valid name.
static bool _clean(const char*& name, size_t& len)
{
    while (len && (*name == '#' || *name == ' ')) { name++; len--; }
    while (len && name[len - 1] == ' ') len--;
    return !(len == 0 || len >= NAME_LEN || name[0] == '*' || name[0] == '$');
}

static int _find(const char* name, size_t len)
{
    for (int i = 0; i < s_count; i++)
        if (strlen(s_names[i]) == len && memcmp(s_names[i], name, len) == 0) return i;
    return -1;
}

// Appends without saving; true if it was new. Full = ignored.
static bool _add(const char* name, size_t len)
{
    if (!_clean(name, len) || _find(name, len) >= 0) return false;
    if (s_count == MAX_REGIONS) return false;
    memcpy(s_names[s_count], name, len);
    s_names[s_count][len] = '\0';
    s_count++;
    return true;
}

static int _addCsv(const char* csv)
{
    int added = 0;
    while (csv && *csv) {
        const char* end = strchr(csv, ',');
        size_t len = end ? (size_t)(end - csv) : strlen(csv);
        if (_add(csv, len)) added++;
        csv = end ? end + 1 : nullptr;
    }
    return added;
}

void init()
{
    s_count = 0;
    Preferences p;
    if (!p.begin(NVS_NS, true)) return;
    char buf[16 * NAME_LEN] = {};   // older builds kept up to 16
    p.getString(NVS_KEY, buf, sizeof(buf));
    p.end();
    // Keep the newest MAX_REGIONS (the list is stored oldest first).
    int total = 0;
    for (const char* c = buf; *c; c++) if (*c == ',') total++;
    if (buf[0]) total++;
    const char* start = buf;
    for (int skip = total - MAX_REGIONS; skip > 0 && start; skip--) {
        start = strchr(start, ',');
        if (start) start++;
    }
    _addCsv(start ? start : "");
    if (total > MAX_REGIONS) _save();
}

int count() { return s_count; }

const char* get(int i) { return (i >= 0 && i < s_count) ? s_names[i] : nullptr; }

bool add(const char* name)
{
    if (!name || !_add(name, strlen(name))) return false;
    _save();
    OPS_LOG("Regions", "Catalog + %s (%d)", s_names[s_count - 1], s_count);
    return true;
}

bool set(int i, const char* name)
{
    if (!name || i < 0 || i > s_count || i >= MAX_REGIONS) return false;
    size_t len = strlen(name);
    if (!_clean(name, len)) return false;
    int at = _find(name, len);
    if (at >= 0 && at != i) return false;   // already saved in another slot
    memcpy(s_names[i], name, len);
    s_names[i][len] = '\0';
    if (i == s_count) s_count++;
    _save();
    OPS_LOG("Regions", "Catalog [%d] = %s", i, s_names[i]);
    return true;
}

bool remove(int i)
{
    if (i < 0 || i >= s_count) return false;
    OPS_LOG("Regions", "Catalog - %s", s_names[i]);
    memmove(s_names[i], s_names[i + 1], (size_t)(s_count - i - 1) * NAME_LEN);
    s_count--;
    s_names[s_count][0] = '\0';
    _save();
    return true;
}

int addList(const char* csv)
{
    int added = _addCsv(csv);
    if (added) _save();
    return added;
}

}  // namespace regions
}  // namespace ops

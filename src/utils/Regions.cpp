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

// Appends without saving; true if it was new.
static bool _add(const char* name, size_t len)
{
    while (len && (*name == '#' || *name == ' ')) { name++; len--; }
    while (len && name[len - 1] == ' ') len--;
    if (len == 0 || len >= NAME_LEN || name[0] == '*' || name[0] == '$') return false;
    for (int i = 0; i < s_count; i++)
        if (strlen(s_names[i]) == len && memcmp(s_names[i], name, len) == 0) return false;
    if (s_count == MAX_REGIONS) {   // drop the oldest
        memmove(s_names[0], s_names[1], (MAX_REGIONS - 1) * NAME_LEN);
        s_count--;
    }
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
    char buf[MAX_REGIONS * NAME_LEN] = {};
    p.getString(NVS_KEY, buf, sizeof(buf));
    p.end();
    _addCsv(buf);
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

int addList(const char* csv)
{
    int added = _addCsv(csv);
    if (added) _save();
    return added;
}

}  // namespace regions
}  // namespace ops

// Saitama — g2048_core.h
// Copyright 2026 Saitama — GPL-3.0-or-later
// 2048 game logic, Arduino-free (host-testable).
// Caller owns s_seed (LCG state) and passes it to spawnTile().

#pragma once
#include <stdint.h>

namespace g2048 {

struct Board {
    uint32_t c[4][4];  // 0 = empty
    uint32_t score;
    bool     won;      // set once when any tile reaches 2048
};

inline void reset(Board& b)
{
    for (int r = 0; r < 4; r++)
        for (int cc = 0; cc < 4; cc++)
            b.c[r][cc] = 0;
    b.score = 0;
    b.won   = false;
}

// Place a 2 (90%) or 4 (10%) in a random empty cell using caller's LCG seed.
inline void spawnTile(Board& b, uint32_t& seed)
{
    uint8_t emp[16];
    int cnt = 0;
    for (int r = 0; r < 4; r++)
        for (int cc = 0; cc < 4; cc++)
            if (!b.c[r][cc]) emp[cnt++] = (uint8_t)(r * 4 + cc);
    if (!cnt) return;
    seed = seed * 1664525u + 1013904223u;
    int idx  = (int)((seed >> 16) % (uint32_t)cnt);
    int cell = emp[idx];
    seed = seed * 1664525u + 1013904223u;
    b.c[cell / 4][cell % 4] = ((seed >> 16) % 10u == 0u) ? 4u : 2u;
}

inline bool hasLost(const Board& b)
{
    for (int r = 0; r < 4; r++)
        for (int cc = 0; cc < 4; cc++) {
            if (!b.c[r][cc]) return false;
            if (r  < 3 && b.c[r][cc] == b.c[r+1][cc]) return false;
            if (cc < 3 && b.c[r][cc] == b.c[r][cc+1]) return false;
        }
    return true;
}

namespace detail {

// Merge 4-cell line leftward in-place; returns score gained.
inline uint32_t mergeLine(uint32_t* a)
{
    uint32_t t[4] = {};
    int n = 0;
    for (int i = 0; i < 4; i++) if (a[i]) t[n++] = a[i];
    uint32_t sc = 0;
    for (int i = 0; i < 3; i++) {
        if (t[i] && t[i] == t[i+1]) { t[i] *= 2; sc += t[i]; t[i+1] = 0; }
    }
    n = 0;
    for (int i = 0; i < 4; i++) if (t[i]) a[n++] = t[i];
    while (n < 4) a[n++] = 0;
    return sc;
}

} // namespace detail

// dir: 0=left 1=right 2=up 3=down
// Returns true if the board changed.
inline bool move(Board& b, int dir)
{
    uint32_t line[4];
    bool changed = false;

    for (int i = 0; i < 4; i++) {
        if      (dir == 0) for (int j = 0; j < 4; j++) line[j] = b.c[i][j];
        else if (dir == 1) for (int j = 0; j < 4; j++) line[j] = b.c[i][3-j];
        else if (dir == 2) for (int j = 0; j < 4; j++) line[j] = b.c[j][i];
        else               for (int j = 0; j < 4; j++) line[j] = b.c[3-j][i];

        uint32_t orig[4] = { line[0], line[1], line[2], line[3] };
        b.score += detail::mergeLine(line);
        for (int j = 0; j < 4; j++) if (orig[j] != line[j]) changed = true;

        if      (dir == 0) for (int j = 0; j < 4; j++) b.c[i][j]   = line[j];
        else if (dir == 1) for (int j = 0; j < 4; j++) b.c[i][3-j] = line[j];
        else if (dir == 2) for (int j = 0; j < 4; j++) b.c[j][i]   = line[j];
        else               for (int j = 0; j < 4; j++) b.c[3-j][i] = line[j];
    }

    if (changed && !b.won)
        for (int r = 0; r < 4; r++)
            for (int cc = 0; cc < 4; cc++)
                if (b.c[r][cc] >= 2048) b.won = true;

    return changed;
}

} // namespace g2048

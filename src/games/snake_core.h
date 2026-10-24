// Saitama — snake_core.h
// Copyright 2026 Saitama — GPL-3.0-or-later
// Snake game logic, Arduino-free (host-testable).
// Caller owns the LCG seed and passes it to reset() and step().

#pragma once
#include <stdint.h>

namespace snake {

static constexpr int COLS  = 30;
static constexpr int ROWS  = 20;
static constexpr int CELLS = COLS * ROWS;
static constexpr uint16_t NO_CELL = 0xFFFF;

enum Dir : uint8_t { UP = 0, DOWN = 1, LEFT = 2, RIGHT = 3 };

enum Step : uint8_t {
    STEP_MOVED = 0,  // moved one cell
    STEP_ATE   = 1,  // moved onto the food and grew
    STEP_DEAD  = 2,  // hit a wall or itself
    STEP_WON   = 3,  // filled the whole field
};

struct Game {
    uint16_t body[CELLS];  // ring buffer of cell indexes; body[head] is the head
    uint8_t  occ[CELLS];   // 1 = cell holds a snake segment
    uint16_t head;         // ring index of the head
    uint16_t len;
    uint16_t food;         // cell index, NO_CELL when the field is full
    uint16_t vacated;      // cell the tail left on the last step, or NO_CELL
    uint8_t  dir;
    uint8_t  queue[2];     // pending turns, so two quick inputs between steps both count
    uint8_t  qlen;
    uint16_t eaten;
    uint32_t score;
    bool     dead;
};

inline int cellX(uint16_t c) { return c % COLS; }
inline int cellY(uint16_t c) { return c / COLS; }

inline bool isOpposite(uint8_t a, uint8_t b)
{
    return (a == UP && b == DOWN) || (a == DOWN && b == UP) ||
           (a == LEFT && b == RIGHT) || (a == RIGHT && b == LEFT);
}

inline uint16_t tailCell(const Game& g)
{
    return g.body[(g.head + CELLS - (g.len - 1)) % CELLS];
}

// Place food on a random empty cell; NO_CELL if there is none.
inline void spawnFood(Game& g, uint32_t& seed)
{
    int empty = CELLS - g.len;
    if (empty <= 0) { g.food = NO_CELL; return; }
    seed = seed * 1664525u + 1013904223u;
    int k = (int)((seed >> 16) % (uint32_t)empty);
    for (int c = 0; c < CELLS; c++) {
        if (g.occ[c]) continue;
        if (k-- == 0) { g.food = (uint16_t)c; return; }
    }
    g.food = NO_CELL;
}

// Three-segment snake in the middle of the field, heading right.
inline void reset(Game& g, uint32_t& seed)
{
    for (int c = 0; c < CELLS; c++) g.occ[c] = 0;
    const int y = ROWS / 2;
    const int x = COLS / 2 - 2;
    g.len  = 3;
    g.head = 2;
    for (int i = 0; i < 3; i++) {
        uint16_t c = (uint16_t)(y * COLS + x + i);
        g.body[i] = c;
        g.occ[c]  = 1;
    }
    g.dir     = RIGHT;
    g.qlen    = 0;
    g.eaten   = 0;
    g.score   = 0;
    g.dead    = false;
    g.vacated = NO_CELL;
    spawnFood(g, seed);
}

// Queue a turn. Turning back on yourself, or repeating the current
// heading, is ignored. Returns true if the turn was queued.
inline bool turn(Game& g, uint8_t d)
{
    uint8_t last = g.qlen ? g.queue[g.qlen - 1] : g.dir;
    if (d == last || isOpposite(d, last)) return false;
    if (g.qlen >= 2) return false;
    g.queue[g.qlen++] = d;
    return true;
}

// Advance one cell.
inline Step step(Game& g, uint32_t& seed)
{
    g.vacated = NO_CELL;
    if (g.dead) return STEP_DEAD;

    if (g.qlen) {
        g.dir = g.queue[0];
        g.queue[0] = g.queue[1];
        g.qlen--;
    }

    uint16_t h = g.body[g.head];
    int x = cellX(h);
    int y = cellY(h);
    if      (g.dir == UP)    y--;
    else if (g.dir == DOWN)  y++;
    else if (g.dir == LEFT)  x--;
    else                     x++;
    if (x < 0 || x >= COLS || y < 0 || y >= ROWS) { g.dead = true; return STEP_DEAD; }

    uint16_t next = (uint16_t)(y * COLS + x);
    bool ate = (next == g.food);

    // The tail moves away this step unless we grow, so the head may
    // follow straight into the cell the tail is leaving.
    if (!ate) {
        uint16_t t = tailCell(g);
        g.occ[t] = 0;
        g.len--;
        g.vacated = t;
    }
    if (g.occ[next]) {
        if (!ate) { g.occ[g.vacated] = 1; g.len++; g.vacated = NO_CELL; }  // keep the body drawn as it was
        g.dead = true;
        return STEP_DEAD;
    }

    g.head = (uint16_t)((g.head + 1) % CELLS);
    g.body[g.head] = next;
    g.occ[next] = 1;
    g.len++;

    if (!ate) return STEP_MOVED;

    g.eaten++;
    g.score += 10;
    spawnFood(g, seed);
    return (g.food == NO_CELL) ? STEP_WON : STEP_ATE;
}

} // namespace snake

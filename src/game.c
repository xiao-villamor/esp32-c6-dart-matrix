/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smart Dartboard — Hit handler
 *
 * In BLE mode the board is a "dumb" peripheral: it detects dart hits,
 * sends GranBoard-format BLE notifications (done in matrix.c before
 * calling here), then marks the contact as seated so subsequent scans
 * skip it.  Seated contacts are cleared once all darts are removed.
 *
 * All game logic (scoring, turns, players) runs on the connected
 * phone app — this module only provides scoring helpers for logging.
 *
 * Target: ESP32-C6-DevKitC-1 (N4)
 */

#include "dartboard.h"

LOG_MODULE_REGISTER(game, LOG_LEVEL_INF);

/* ═══════════════════════════════════════════════════════════════════════════
 * SHARED STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

bool seated_darts[NUM_ROWS][NUM_COLS];

K_MUTEX_DEFINE(game_mutex);

/* ═══════════════════════════════════════════════════════════════════════════
 * SCORING HELPERS (used for log readability)
 * ═══════════════════════════════════════════════════════════════════════════ */

int get_points(const char *hit)
{
    if (strcmp(hit, "BULL") == 0)  return 25;
    if (strcmp(hit, "DBULL") == 0) return 50;
    if (hit[0] == '?' || strlen(hit) < 2) return 0;

    int number = atoi(hit + 1);
    char p = hit[0];
    if (p == 'T') return number * 3;
    if (p == 'D') return number * 2;
    if (p == 'S') return number;
    return 0;
}

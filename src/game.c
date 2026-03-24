/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smart Dartboard — Hit handler
 *
 * In BLE mode the board is a "dumb" peripheral: it detects dart hits,
 * sends GranBoard-format BLE notifications (done in matrix.c before
 * calling here), then waits for the dart to be removed before resuming
 * the matrix scan.
 *
 * All game logic (scoring, turns, players) runs on the connected
 * phone app — this module only provides logging and dart-settle handling.
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

static void get_hit_label(const char *hit, char *buf, int buf_size)
{
    if (strcmp(hit, "BULL") == 0)  { snprintf(buf, buf_size, "Bull (25)");  return; }
    if (strcmp(hit, "DBULL") == 0) { snprintf(buf, buf_size, "D-Bull (50)"); return; }
    if (hit[0] == '?')            { snprintf(buf, buf_size, "Miss");        return; }
    if (strlen(hit) < 2)          { snprintf(buf, buf_size, "%s", hit);     return; }

    int pts    = get_points(hit);
    int number = atoi(hit + 1);
    char prefix = hit[0];

    if (prefix == 'T')      snprintf(buf, buf_size, "Triple %d (%d)", number, pts);
    else if (prefix == 'D') snprintf(buf, buf_size, "Double %d (%d)", number, pts);
    else                    snprintf(buf, buf_size, "%d (%d)", number, pts);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HIT HANDLER
 *
 * NOTE: Caller must hold game_mutex before calling this function.
 *
 * After logging the hit, releases the mutex while waiting for all darts
 * to be removed (so BLE stack remains responsive), then re-acquires the
 * mutex and clears seated state.
 * ═══════════════════════════════════════════════════════════════════════════ */

void handle_hit(const char *hit, int row, int col)
{
    char label[28];
    get_hit_label(hit, label, sizeof(label));

    LOG_INF("HIT: %s  [row=%d col=%d]", label, row, col);

    /* Release mutex during the blocking wait so the BLE stack
     * (running in Zephyr's system workqueue) stays responsive. */
    k_mutex_unlock(&game_mutex);
    wait_for_full_release();
    k_mutex_lock(&game_mutex, K_FOREVER);

    clear_seated_darts();
}

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smart Dartboard — Common header
 * Zephyr RTOS on ESP32-C6-DevKitC-1 (N4)
 *
 * BLE mode: Emulates a GranBoard peripheral so existing dart apps
 * (DaDartboard, gran-app, etc.) can connect and receive hit events.
 * All game logic runs on the phone app.
 */

#ifndef DARTBOARD_H
#define DARTBOARD_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CALIBRATION_MODE  0

/* ═══════════════════════════════════════════════════════════════════════════
 * MATRIX DEFINITION
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NUM_ROWS  8
#define NUM_COLS  8

/* ═══════════════════════════════════════════════════════════════════════════
 * SHARED STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

extern bool seated_darts[NUM_ROWS][NUM_COLS];

/* Mutex protecting seated_darts and serializing hit handling */
extern struct k_mutex game_mutex;

/* ═══════════════════════════════════════════════════════════════════════════
 * MATRIX MODULE (matrix.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

int  matrix_init(void);
void matrix_scan_loop(void);
int  count_active_contacts(void);
bool is_matrix_quiet(void);
void wait_for_full_release(void);
void wait_for_settle(void);
void clear_seated_darts(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * HIT HANDLER (game.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

int  get_points(const char *hit);
void handle_hit(const char *hit, int row, int col);

/* ═══════════════════════════════════════════════════════════════════════════
 * BLE MODULE (ble.c) — GranBoard-compatible peripheral
 * ═══════════════════════════════════════════════════════════════════════════ */

int  ble_init(void);
void ble_notify_hit(const char *segment);
bool ble_is_connected(void);

#endif /* DARTBOARD_H */

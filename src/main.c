/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smart Dartboard — Main entry point
 * Zephyr RTOS application for ESP32-C6-DevKitC-1 (N4)
 *
 * Architecture:
 *   - Main thread: BLE initialization (GranBoard-compatible peripheral)
 *   - Matrix scan thread: Dedicated thread for continuous 8x8 matrix scanning
 *
 * The board acts as a "dumb" BLE peripheral — it detects dart hits via
 * the contact matrix and sends GranBoard-format notifications over BLE.
 * Game logic runs on the connected phone app.
 */

#include "dartboard.h"

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_INF);

/* ═══════════════════════════════════════════════════════════════════════════
 * MATRIX SCAN THREAD
 *
 * Runs continuously, scanning the 8x8 contact matrix and processing hits.
 * Separated from the main thread to avoid blocking BLE handling.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MATRIX_STACK_SIZE  4096
#define MATRIX_PRIORITY    7      /* Cooperative priority */

static void matrix_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Matrix scan thread started");

    while (1) {
        matrix_scan_loop();
        k_msleep(5);  /* 5 ms between scans (same as original) */
    }
}

K_THREAD_DEFINE(matrix_thread, MATRIX_STACK_SIZE,
                matrix_thread_entry, NULL, NULL, NULL,
                MATRIX_PRIORITY, 0, -1);  /* -1 = don't auto-start */

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    LOG_INF("=== Smart Dartboard (BLE) ===");
    LOG_INF("Platform: ESP32-C6-DevKitC-1 (N4)");
    LOG_INF("RTOS: Zephyr %s", STRINGIFY(BUILD_VERSION));

    /* Initialize the 8x8 contact matrix GPIO */
    int ret = matrix_init();
    if (ret) {
        LOG_ERR("Matrix init failed: %d", ret);
        return ret;
    }

    if (CALIBRATION_MODE) {
        LOG_INF("=== CALIBRATION MODE ===");
        LOG_INF("Touch each segment. Note PAx + PEy.");
        /* Start matrix thread — no BLE needed in calibration */
        k_thread_start(matrix_thread);
        return 0;  /* main returns, matrix thread keeps running */
    }

    /* Initialize BLE (GranBoard-compatible peripheral) */
    ret = ble_init();
    if (ret) {
        LOG_ERR("BLE init failed: %d", ret);
        /* Continue anyway — matrix scanning still works for logging */
    }

    /* Start the matrix scan thread */
    k_thread_start(matrix_thread);
    LOG_INF("System ready — matrix scanning active, BLE advertising");

    /* Main thread has nothing more to do.
     * The matrix scan thread handles dartboard scanning,
     * and Zephyr's BLE stack runs in its own context. */
    return 0;
}

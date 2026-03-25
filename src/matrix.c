/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smart Dartboard — Matrix scanning module
 * 8x8 row-column contact matrix scan using Zephyr GPIO API
 * Target: ESP32-C6-DevKitC-1 (N4)
 */

#include "dartboard.h"

LOG_MODULE_REGISTER(matrix, LOG_LEVEL_INF);

/* ═══════════════════════════════════════════════════════════════════════════
 * GPIO PIN DEFINITIONS
 *
 * Row outputs:  GPIO 0,1,2,3,4,5,6,7    (active LOW scan)
 * Col inputs:   GPIO 20,21,10,11,12,13,15,18  (INPUT_PULLUP, read LOW = contact)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const int row_pins[NUM_ROWS] = {0, 1, 2, 3, 4, 5, 6, 7};
static const int col_pins[NUM_COLS] = {20, 21, 10, 11, 12, 13, 15, 18};

static const struct device *gpio_dev;

/* ═══════════════════════════════════════════════════════════════════════════
 * SEGMENT MAP
 *
 * segmentos[row][col] — maps each matrix intersection to a dartboard segment.
 * Prefixes: S=Single, D=Double, T=Triple, BULL=25, DBULL=50, ??=unmapped
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *segments[NUM_ROWS][NUM_COLS] = {
    /* PA0 */ {"S12", "BULL", "T12", "T5",  "S5",  "D5",  "D12", "??"},
    /* PA1 */ {"S9",  "DBULL","T9",  "T20", "S20", "D20", "??",  "D9"},
    /* PA2 */ {"S14", "S11",  "S8",  "S16", "S7",  "S19", "S3",  "S17"},
    /* PA3 */ {"D14", "D11",  "D8",  "D16", "D7",  "D19", "D3",  "D17"},
    /* PA4 */ {"T14", "T11",  "T8",  "T16", "T7",  "T19", "T3",  "T17"},
    /* PA5 */ {"T1",  "T18",  "T4",  "T13", "T6",  "T10", "T15", "T2"},
    /* PA6 */ {"S1",  "S18",  "S4",  "S13", "S6",  "S10", "S15", "S2"},
    /* PA7 */ {"D1",  "D18",  "D4",  "D13", "D6",  "D10", "D15", "D2"},
};

/* ═══════════════════════════════════════════════════════════════════════════
 * GPIO HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline void row_set(int row, int value)
{
    gpio_pin_set(gpio_dev, row_pins[row], value);
}

static inline int col_read(int col)
{
    return gpio_pin_get(gpio_dev, col_pins[col]);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MATRIX INIT
 * ═══════════════════════════════════════════════════════════════════════════ */

int matrix_init(void)
{
    gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_dev)) {
        LOG_ERR("GPIO device not ready");
        return -ENODEV;
    }

    /* Configure row pins as outputs, default HIGH (inactive) */
    for (int i = 0; i < NUM_ROWS; i++) {
        int ret = gpio_pin_configure(gpio_dev, row_pins[i],
                                     GPIO_OUTPUT_ACTIVE | GPIO_ACTIVE_HIGH);
        if (ret < 0) {
            LOG_ERR("Failed to configure row pin %d: %d", row_pins[i], ret);
            return ret;
        }
        gpio_pin_set(gpio_dev, row_pins[i], 1);  /* HIGH = inactive */
    }

    /* Configure column pins as inputs with pull-up */
    for (int j = 0; j < NUM_COLS; j++) {
        int ret = gpio_pin_configure(gpio_dev, col_pins[j],
                                     GPIO_INPUT | GPIO_PULL_UP);
        if (ret < 0) {
            LOG_ERR("Failed to configure col pin %d: %d", col_pins[j], ret);
            return ret;
        }
    }

    clear_seated_darts();
    LOG_INF("Matrix initialized: %d rows x %d cols", NUM_ROWS, NUM_COLS);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MATRIX UTILITIES
 * ═══════════════════════════════════════════════════════════════════════════ */

void clear_seated_darts(void)
{
    memset(seated_darts, 0, sizeof(seated_darts));
}

int count_active_contacts(void)
{
    int count = 0;

    for (int i = 0; i < NUM_ROWS; i++) {
        row_set(i, 0);          /* Drive row LOW */
        k_busy_wait(50);        /* 50 us settle */

        for (int j = 0; j < NUM_COLS; j++) {
            /* Skip unmapped cells (hardware noise) */
            if (segments[i][j][0] == '?') {
                continue;
            }
            if (col_read(j) == 0) {  /* Active-low with GPIO_PULL_UP: 0 = pressed */
                count++;
            }
        }
        row_set(i, 1);          /* Row back HIGH */
    }
    return count;
}

bool is_matrix_quiet(void)
{
    return count_active_contacts() == 0;
}

void wait_for_full_release(void)
{
    k_msleep(50);

    /* All rows HIGH */
    for (int i = 0; i < NUM_ROWS; i++) {
        row_set(i, 1);
    }

    /* Wait up to 10 seconds for all darts to be removed */
    int64_t t0 = k_uptime_get();
    while (!is_matrix_quiet() && (k_uptime_get() - t0) < 10000) {
        k_msleep(20);
    }

    k_msleep(150);
}

void wait_for_settle(void)
{
    /* All rows HIGH — stop driving during settle */
    for (int i = 0; i < NUM_ROWS; i++) {
        row_set(i, 1);
    }

    /* Wait for matrix to go quiet (bounce stops), capped at 300ms */
    int64_t t0 = k_uptime_get();
    while ((k_uptime_get() - t0) < 300) {
        k_msleep(10);
        if (is_matrix_quiet()) {
            break;
        }
    }

    /* Extra settling margin */
    k_msleep(60);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MATRIX SCAN LOOP
 *
 * Called from the dedicated matrix-scan thread. Performs one full scan
 * of the 8x8 matrix and processes any detected hits.
 *
 * Darts that are physically lodged in the board are tracked in the
 * seated_darts[][] array so they don't re-fire.  After a hit is detected
 * and notified over BLE, a short settle delay allows contact bounce to
 * subside, then scanning resumes immediately — there is no blocking wait
 * for dart removal.  This lets the board detect the next dart as soon as
 * it lands, even while previous darts are still in the board.
 *
 * The seated_darts array is cleared when the board goes fully quiet
 * (all darts removed) for QUIET_CLEAR_MS.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Time the matrix must be quiet before clearing seated darts (ms) */
#define QUIET_CLEAR_MS  500

static int64_t last_quiet_check;

void matrix_scan_loop(void)
{
    bool found_hit = false;

    for (int f = 0; f < NUM_ROWS; f++) {
        row_set(f, 0);          /* Drive row LOW */
        k_busy_wait(50);        /* 50 us settle */

        for (int c = 0; c < NUM_COLS; c++) {
            if (col_read(c) == 0 && !seated_darts[f][c]) {
                row_set(f, 1);  /* Row back HIGH before processing */

                const char *seg = segments[f][c];

                if (CALIBRATION_MODE) {
                    printk("PA%d + PE%d  ->  %s\n", f, c,
                           seg[0] == '?' ? "???" : seg);
                    wait_for_full_release();
                    return;     /* Exit scan — one hit at a time in cal mode */
                }

                /* Mark as seated immediately to prevent re-triggering */
                seated_darts[f][c] = true;

                /* Silently suppress unmapped cells */
                if (seg[0] == '?') {
                    return;
                }

                /* Send BLE notification (outside mutex — non-blocking) */
                ble_notify_hit(seg);

                /* Log the hit */
                k_mutex_lock(&game_mutex, K_FOREVER);
                {
                    char label[28];
                    int pts = get_points(seg);
                    LOG_INF("HIT: %s (%d pts)  [row=%d col=%d]", seg, pts, f, c);
                }
                k_mutex_unlock(&game_mutex);

                found_hit = true;

                /* Short settle delay after a hit to let contact bounce subside,
                 * then resume scanning so the next dart can be detected while
                 * previous darts remain in the board. */
                wait_for_settle();
                return;
            }
        }
        row_set(f, 1);          /* Row back HIGH */
    }

    /* If no new hit was found, check whether the board is fully quiet
     * (all darts removed) and clear seated state after a grace period. */
    if (!found_hit && is_matrix_quiet()) {
        int64_t now = k_uptime_get();
        if (last_quiet_check == 0) {
            last_quiet_check = now;
        } else if ((now - last_quiet_check) >= QUIET_CLEAR_MS) {
            k_mutex_lock(&game_mutex, K_FOREVER);
            clear_seated_darts();
            k_mutex_unlock(&game_mutex);
            last_quiet_check = 0;
        }
    } else {
        last_quiet_check = 0;
    }
}

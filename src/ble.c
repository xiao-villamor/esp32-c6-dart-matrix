/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smart Dartboard — BLE module (GranBoard-compatible)
 *
 * Emulates a GranBoard BLE peripheral so existing apps
 * (DaDartboard, gran-app, etc.) can connect and receive
 * dart hit notifications.
 *
 * Protocol (reverse-engineered from sobassy/gran-app):
 *   Service UUID: 442f1570-8a00-9a28-cbe1-e1d4212d53eb
 *   Notify characteristic: sends ASCII "row.col@" on each hit
 *   where @ = 0x40 terminator byte.
 *
 * Target: ESP32-C6-DevKitC-1 (N4)
 */

#include "dartboard.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

/* ═══════════════════════════════════════════════════════════════════════════
 * GRANBOARD BLE PROTOCOL CONSTANTS
 *
 * The GranBoard service UUID and the characteristic used for hit
 * notifications. Apps filter BLE scan results by this service UUID.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* GranBoard primary service UUID: 442f1570-8a00-9a28-cbe1-e1d4212d53eb */
static struct bt_uuid_128 granboard_svc_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x442f1570, 0x8a00, 0x9a28, 0xcbe1, 0xe1d4212d53eb));

/* Notify characteristic UUID (+1): board → app, dart hit events */
static struct bt_uuid_128 granboard_notify_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x442f1571, 0x8a00, 0x9a28, 0xcbe1, 0xe1d4212d53eb));

/* Write characteristic UUID (+2): app → board, LED commands / handshake */
static struct bt_uuid_128 granboard_write_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x442f1572, 0x8a00, 0x9a28, 0xcbe1, 0xe1d4212d53eb));

/* Info characteristic UUID (+3): board info / capability read */
static struct bt_uuid_128 granboard_info_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x442f1573, 0x8a00, 0x9a28, 0xcbe1, 0xe1d4212d53eb));

/* ═══════════════════════════════════════════════════════════════════════════
 * GRANBOARD SEGMENT CODE TABLE
 *
 * Maps our dartboard segment strings (e.g., "S20", "T19", "BULL") to
 * the GranBoard BLE notification byte sequences.
 *
 * GranBoard sends ASCII "row.col@" where @ = 0x40 = 64 decimal.
 * The bytes in the open-source mapping are decimal ASCII codes:
 *   e.g., "50-46-51-64" means bytes { '2', '.', '3', '@' } = "2.3@"
 *
 * We store the raw byte arrays to send over BLE notify.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct ble_segment_code {
    const char *segment;    /* Our segment string, e.g. "S1", "T20", "BULL" */
    uint8_t     data[6];    /* Bytes to send (max 5 + NUL) */
    uint8_t     len;        /* Number of bytes to send */
};

/* Table built from the gran-app SEGMENT_MAPPING reverse lookup.
 * GranBoard has INNER (=Single), TRP (=Triple), OUTER (=Single outer),
 * DBL (=Double) for each number. Our board uses S (=Single), T (=Triple),
 * D (=Double). GranBoard distinguishes inner/outer single; our board does
 * not — we map our S to INNER.
 *
 * Byte format: ASCII chars of "row.col" followed by 0x40 ('@'). */

static const struct ble_segment_code segment_codes[] = {
    /* Number 1:  INNER "2.3@", TRIPLE "2.4@", OUTER "2.5@", DOUBLE "2.6@" */
    {"S1",    {50, 46, 51, 64},          4},
    {"T1",    {50, 46, 52, 64},          4},
    {"OS1",   {50, 46, 53, 64},          4},
    {"D1",    {50, 46, 54, 64},          4},
    /* Number 2:  INNER "9.1@", TRIPLE "9.0@", OUTER "9.2@", DOUBLE "8.2@" */
    {"S2",    {57, 46, 49, 64},          4},
    {"T2",    {57, 46, 48, 64},          4},
    {"OS2",   {57, 46, 50, 64},          4},
    {"D2",    {56, 46, 50, 64},          4},
    /* Number 3:  INNER "7.1@", TRIPLE "7.0@", OUTER "7.2@", DOUBLE "8.4@" */
    {"S3",    {55, 46, 49, 64},          4},
    {"T3",    {55, 46, 48, 64},          4},
    {"OS3",   {55, 46, 50, 64},          4},
    {"D3",    {56, 46, 52, 64},          4},
    /* Number 4:  INNER "0.1@", TRIPLE "0.3@", OUTER "0.5@", DOUBLE "0.6@" */
    {"S4",    {48, 46, 49, 64},          4},
    {"T4",    {48, 46, 51, 64},          4},
    {"OS4",   {48, 46, 53, 64},          4},
    {"D4",    {48, 46, 54, 64},          4},
    /* Number 5:  INNER "5.1@", TRIPLE "5.2@", OUTER "5.4@", DOUBLE "4.6@" */
    {"S5",    {53, 46, 49, 64},          4},
    {"T5",    {53, 46, 50, 64},          4},
    {"OS5",   {53, 46, 52, 64},          4},
    {"D5",    {52, 46, 54, 64},          4},
    /* Number 6:  INNER "1.0@", TRIPLE "1.1@", OUTER "1.3@", DOUBLE "4.4@" */
    {"S6",    {49, 46, 48, 64},          4},
    {"T6",    {49, 46, 49, 64},          4},
    {"OS6",   {49, 46, 51, 64},          4},
    {"D6",    {52, 46, 52, 64},          4},
    /* Number 7:  INNER "11.1@", TRIPLE "11.2@", OUTER "11.4@", DOUBLE "8.6@" */
    {"S7",    {49, 49, 46, 49, 64},      5},
    {"T7",    {49, 49, 46, 50, 64},      5},
    {"OS7",   {49, 49, 46, 52, 64},      5},
    {"D7",    {56, 46, 54, 64},          4},
    /* Number 8:  INNER "6.2@", TRIPLE "6.4@", OUTER "6.5@", DOUBLE "6.6@" */
    {"S8",    {54, 46, 50, 64},          4},
    {"T8",    {54, 46, 52, 64},          4},
    {"OS8",   {54, 46, 53, 64},          4},
    {"D8",    {54, 46, 54, 64},          4},
    /* Number 9:  INNER "9.3@", TRIPLE "9.4@", OUTER "9.5@", DOUBLE "9.6@" */
    {"S9",    {57, 46, 51, 64},          4},
    {"T9",    {57, 46, 52, 64},          4},
    {"OS9",   {57, 46, 53, 64},          4},
    {"D9",    {57, 46, 54, 64},          4},
    /* Number 10: INNER "2.0@", TRIPLE "2.1@", OUTER "2.2@", DOUBLE "4.3@" */
    {"S10",   {50, 46, 48, 64},          4},
    {"T10",   {50, 46, 49, 64},          4},
    {"OS10",  {50, 46, 50, 64},          4},
    {"D10",   {52, 46, 51, 64},          4},
    /* Number 11: INNER "7.3@", TRIPLE "7.4@", OUTER "7.5@", DOUBLE "7.6@" */
    {"S11",   {55, 46, 51, 64},          4},
    {"T11",   {55, 46, 52, 64},          4},
    {"OS11",  {55, 46, 53, 64},          4},
    {"D11",   {55, 46, 54, 64},          4},
    /* Number 12: INNER "5.0@", TRIPLE "5.3@", OUTER "5.5@", DOUBLE "5.6@" */
    {"S12",   {53, 46, 48, 64},          4},
    {"T12",   {53, 46, 51, 64},          4},
    {"OS12",  {53, 46, 53, 64},          4},
    {"D12",   {53, 46, 54, 64},          4},
    /* Number 13: INNER "0.0@", TRIPLE "0.2@", OUTER "0.4@", DOUBLE "4.5@" */
    {"S13",   {48, 46, 48, 64},          4},
    {"T13",   {48, 46, 50, 64},          4},
    {"OS13",  {48, 46, 52, 64},          4},
    {"D13",   {52, 46, 53, 64},          4},
    /* Number 14: INNER "10.3@", TRIPLE "10.4@", OUTER "10.5@", DOUBLE "10.6@" */
    {"S14",   {49, 48, 46, 51, 64},      5},
    {"T14",   {49, 48, 46, 52, 64},      5},
    {"OS14",  {49, 48, 46, 53, 64},      5},
    {"D14",   {49, 48, 46, 54, 64},      5},
    /* Number 15: INNER "3.0@", TRIPLE "3.1@", OUTER "3.2@", DOUBLE "4.2@" */
    {"S15",   {51, 46, 48, 64},          4},
    {"T15",   {51, 46, 49, 64},          4},
    {"OS15",  {51, 46, 50, 64},          4},
    {"D15",   {52, 46, 50, 64},          4},
    /* Number 16: INNER "11.0@", TRIPLE "11.3@", OUTER "11.5@", DOUBLE "11.6@" */
    {"S16",   {49, 49, 46, 48, 64},      5},
    {"T16",   {49, 49, 46, 51, 64},      5},
    {"OS16",  {49, 49, 46, 53, 64},      5},
    {"D16",   {49, 49, 46, 54, 64},      5},
    /* Number 17: INNER "10.1@", TRIPLE "10.0@", OUTER "10.2@", DOUBLE "8.3@" */
    {"S17",   {49, 48, 46, 49, 64},      5},
    {"T17",   {49, 48, 46, 48, 64},      5},
    {"OS17",  {49, 48, 46, 50, 64},      5},
    {"D17",   {56, 46, 51, 64},          4},
    /* Number 18: INNER "1.2@", TRIPLE "1.4@", OUTER "1.5@", DOUBLE "1.6@" */
    {"S18",   {49, 46, 50, 64},          4},
    {"T18",   {49, 46, 52, 64},          4},
    {"OS18",  {49, 46, 53, 64},          4},
    {"D18",   {49, 46, 54, 64},          4},
    /* Number 19: INNER "6.1@", TRIPLE "6.0@", OUTER "6.3@", DOUBLE "8.5@" */
    {"S19",   {54, 46, 49, 64},          4},
    {"T19",   {54, 46, 48, 64},          4},
    {"OS19",  {54, 46, 51, 64},          4},
    {"D19",   {56, 46, 53, 64},          4},
    /* Number 20: INNER "3.3@", TRIPLE "3.4@", OUTER "3.5@", DOUBLE "3.6@" */
    {"S20",   {51, 46, 51, 64},          4},
    {"T20",   {51, 46, 52, 64},          4},
    {"OS20",  {51, 46, 53, 64},          4},
    {"D20",   {51, 46, 54, 64},          4},
    /* Bull */
    {"BULL",  {56, 46, 48, 64},          4},  /* BULL:     "8.0@" */
    {"DBULL", {52, 46, 48, 64},          4},  /* DBL_BULL: "4.0@" */
    /* Reset button */
    {"BTN",   {66, 84, 78, 64},          4},  /* RESET:    "BTN@" */
};

#define NUM_SEGMENT_CODES  ARRAY_SIZE(segment_codes)

/* ═══════════════════════════════════════════════════════════════════════════
 * BLE STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool ble_connected;
static bool ble_notifications_enabled;
static struct bt_conn *current_conn;

/* Delayable work item for restarting advertising after disconnect.
 * bt_le_adv_start called directly in the disconnected callback can fail with
 * -ENOMEM because BLE buffers haven't been fully released yet. */
static struct k_work_delayable adv_restart_work;

/* Static board info response — real GranBoard returns a capability/version
 * byte here. We return a plausible value (0x01) and log what the app reads.
 * Adjust once we know what value the official app expects. */
static uint8_t board_info_val[4] = {0x01, 0x00, 0x00, 0x00};

/* ═══════════════════════════════════════════════════════════════════════════
 * GATT SERVICE DEFINITION
 * ═══════════════════════════════════════════════════════════════════════════ */

static void notify_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ble_notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("BLE notifications %s", ble_notifications_enabled ? "enabled" : "disabled");
}

/* Write handler for 442f1572 (LED command characteristic).
 * The official app writes LED commands here after connecting.
 * We just log the bytes — no response needed. */
static ssize_t write_led_cmd(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len,
                              uint16_t offset, uint8_t flags)
{
    const uint8_t *data = buf;
    LOG_INF("LED cmd (%u bytes): %02x %02x %02x %02x ...",
            len,
            len > 0 ? data[0] : 0,
            len > 1 ? data[1] : 0,
            len > 2 ? data[2] : 0,
            len > 3 ? data[3] : 0);
    return len;
}

/* Read handler for 442f1573 (board info characteristic).
 * Returns board_info_val — adjust once we know what the app expects. */
static ssize_t read_board_info(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    LOG_INF("Board info read (offset %u)", offset);
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             board_info_val, sizeof(board_info_val));
}

/* GATT service table:
 *   - Primary Service: GranBoard UUID
 *   - Characteristic 1571: Notify (hit events) + CCC
 *   - Characteristic 1572: Write (LED commands from app)
 *   - Characteristic 1573: Read (board info) */
BT_GATT_SERVICE_DEFINE(granboard_svc,
    BT_GATT_PRIMARY_SERVICE(&granboard_svc_uuid),
    /* 442f1571 — notify (board → app) */
    BT_GATT_CHARACTERISTIC(&granboard_notify_uuid.uuid,
                           BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE,
                           NULL, NULL, NULL),
    BT_GATT_CCC(notify_ccc_changed,
                 BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    /* 442f1572 — write (app → board, LED commands) */
    BT_GATT_CHARACTERISTIC(&granboard_write_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE,
                           NULL, write_led_cmd, NULL),
    /* 442f1573 — read (board info / capability) */
    BT_GATT_CHARACTERISTIC(&granboard_info_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ,
                           read_board_info, NULL, board_info_val),
);

/* ═══════════════════════════════════════════════════════════════════════════
 * ADVERTISING DATA
 *
 * We advertise the GranBoard service UUID in the main packet so apps
 * can discover us by UUID filtering (passive or active scan).
 * The device name ("GranBoard") is in the scan response only — this keeps
 * the main advertising packet within the 31-byte BLE limit.
 * (FLAGS=3 + UUID128=18 = 21 bytes; adding the 11-byte name would exceed 31.)
 *
 * BT_LE_ADV_OPT_USE_IDENTITY forces use of the public/static address
 * (A0:F2:62:45:CE:BE) — no RPA rotation. This is required for Android
 * to connect without a warning icon.
 * ═══════════════════════════════════════════════════════════════════════════ */

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS,
                  (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL,
                  BT_UUID_128_ENCODE(0x442f1570, 0x8a00, 0x9a28,
                                     0xcbe1, 0xe1d4212d53eb)),
};

/* Scan response carries the device name for active-scan clients.
 * The UUID is in the main ad[] because apps filter by service UUID. */
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Advertising parameters: connectable, use public/identity address.
 * BT_LE_ADV_OPT_USE_IDENTITY prevents RPA rotation so Android connects
 * cleanly without a "cannot connect" warning. */
static const struct bt_le_adv_param adv_params =
    BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
                         BT_GAP_ADV_FAST_INT_MIN_2,
                         BT_GAP_ADV_FAST_INT_MAX_2,
                         NULL);

/* ═══════════════════════════════════════════════════════════════════════════
 * CONNECTION CALLBACKS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("BLE connection failed (err %u)", err);
        return;
    }

    LOG_INF("BLE connected");
    current_conn = bt_conn_ref(conn);
    ble_connected = true;
}

static void adv_restart_handler(struct k_work *work)
{
    int ret = bt_le_adv_start(&adv_params,
                              ad, ARRAY_SIZE(ad),
                              sd, ARRAY_SIZE(sd));
    if (ret) {
        LOG_ERR("Failed to restart advertising (err %d) — retrying", ret);
        /* Retry after another 500ms if it still fails */
        k_work_schedule(&adv_restart_work, K_MSEC(500));
    } else {
        LOG_INF("BLE advertising restarted after disconnect");
    }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("BLE disconnected (reason %u)", reason);

    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    ble_connected = false;
    ble_notifications_enabled = false;

    /* Schedule advertising restart after 500ms so the BLE stack has time
     * to release connection resources (immediate restart can fail -ENOMEM). */
    k_work_schedule(&adv_restart_work, K_MSEC(500));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════ */

int ble_init(void)
{
    int err;

    LOG_INF("Initializing BLE (GranBoard-compatible)");

    k_work_init_delayable(&adv_restart_work, adv_restart_handler);

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return err;
    }

    LOG_INF("Bluetooth initialized");

    /* Start advertising */
    err = bt_le_adv_start(&adv_params,
                          ad, ARRAY_SIZE(ad),
                          sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("Advertising failed to start (err %d)", err);
        return err;
    }

    LOG_INF("BLE advertising started — waiting for connection...");
    return 0;
}

void ble_notify_hit(const char *segment)
{
    if (!ble_connected || !ble_notifications_enabled || !current_conn) {
        LOG_INF("BLE notify skipped: conn=%d notif=%d ptr=%p",
                ble_connected, ble_notifications_enabled, current_conn);
        return;
    }

    /* Look up the GranBoard byte code for this segment */
    for (int i = 0; i < (int)NUM_SEGMENT_CODES; i++) {
        if (strcmp(segment_codes[i].segment, segment) == 0) {
            /*
             * Append a monotonic sequence counter byte to every notification
             * so that each packet is byte-unique.  Chrome's Web Bluetooth
             * implementation deduplicates consecutive GATT notifications
             * with identical payloads — the counter guarantees uniqueness
             * even when the same segment is hit repeatedly.
             *
             * Format:  [original GranBoard bytes...] [seqno]
             *
             * The web app strips or ignores trailing bytes after the 0x40
             * ('@') terminator when doing segment lookup.
             *
             * This replaces the previous {0x00} separator + 20ms delay
             * approach which blocked the calling thread and may have
             * contributed to BLE supervision timeouts.
             */
            static uint8_t seqno;
            uint8_t buf[7]; /* max 5 data bytes + 1 seqno */
            uint8_t len = segment_codes[i].len;

            memcpy(buf, segment_codes[i].data, len);
            buf[len] = ++seqno; /* wraps 0→255 naturally */
            len++;

            int err = bt_gatt_notify(current_conn,
                                     &granboard_svc.attrs[2],
                                     buf, len);
            if (err) {
                LOG_WRN("BLE notify FAILED for %s (err %d)", segment, err);
            } else {
                LOG_INF("BLE notify OK: %s seq=%u (%u bytes)",
                        segment, seqno, len);
            }
            return;
        }
    }

    LOG_WRN("No GranBoard code for segment: %s", segment);
}

bool ble_is_connected(void)
{
    return ble_connected;
}

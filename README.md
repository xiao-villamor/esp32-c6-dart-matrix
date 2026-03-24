# Zephyr Smart Dartboard

Firmware for a DIY smart electronic dartboard built on the **ESP32-C6** microcontroller using the **Zephyr RTOS**. The device detects dart hits via a GPIO contact matrix and emulates a **GranBoard BLE peripheral**, making it compatible with existing dart-scoring apps like [gran-app](https://github.com/sobassy/gran-app) and DaDartboard without any modifications.

---

## Overview

The hardware consists of an 8×8 contact matrix (64 intersections) wired to the ESP32-C6's GPIO pins. When a dart lands, it closes a circuit between a row and column pin. The firmware detects which cell was hit, maps it to a dartboard segment (Single/Double/Triple/Bullseye), and transmits a BLE notification in the GranBoard proprietary format. All game logic — scoring, turns, players, game modes — is handled by the connected phone app. The firmware is intentionally a "dumb" peripheral.

---

## Features

- **8×8 GPIO matrix scanning** with debounce and settle detection
- **Seated dart tracking** — prevents a lodged dart from re-firing on every scan pass
- **GranBoard BLE protocol emulation** — reverse-engineered from the GranBoard open-source ecosystem; compatible with existing scorekeeping apps
- **Auto-restart advertising** after disconnect (500 ms delay)
- **Calibration mode** — compile-time flag to print raw pin-to-segment mappings over UART for physical wiring verification
- **Zephyr RTOS** — preemptive multithreading, mutex-protected shared state, logging subsystem

---

## Hardware

| Component | Details |
|---|---|
| Microcontroller | Espressif ESP32-C6-DevKitC-1 (N4) |
| Architecture | RISC-V (single-core) |
| Wireless | Bluetooth 5.3 (BLE peripheral role) |
| Matrix | 8 row outputs × 8 column inputs = 64 segments |
| Interface | USB serial (UART console for logs and calibration) |

### GPIO Pin Assignment

| Role | Pins |
|---|---|
| Row outputs (driven LOW during scan) | GPIO 0–7 |
| Column inputs (pulled up, read LOW on hit) | GPIO 20, 21, 10, 11, 12, 13, 15, 18 |

Exact row/column-to-segment mapping is defined in the `segments[8][8]` lookup table in `src/matrix.c`.

---

## Project Structure

```
zephyr-dartboard/
├── CMakeLists.txt              # Zephyr CMake build entry point
├── prj.conf                    # Zephyr Kconfig configuration
├── boards/
│   └── esp32c6_devkitc.overlay # Devicetree overlay (GPIO pin assignments)
└── src/
    ├── main.c                  # Application entry point + thread definitions
    ├── dartboard.h             # Shared header: types, constants, function prototypes
    ├── matrix.c                # GPIO matrix scanning, debounce, segment map
    ├── game.c                  # Hit handler, scoring helpers, shared state
    └── ble.c                   # BLE peripheral (GranBoard-compatible GATT service)
```

### Module Responsibilities

| File | Role |
|---|---|
| `src/main.c` | Initializes GPIO matrix and BLE, starts the matrix scan thread |
| `src/dartboard.h` | Central shared header — all public types and function prototypes |
| `src/matrix.c` | Scans rows/columns, debounces contacts, maps (row, col) → segment string |
| `src/game.c` | Handles hits, logs scores, manages seated-dart state with a mutex |
| `src/ble.c` | GATT service, BLE advertising, connection management, hit notifications |

### Call Flow

```
main()
  └── matrix_init()
  └── ble_init()
  └── k_thread_start(matrix_thread)
        └── matrix_scan_loop()
              └── ble_notify_hit(segment)
              └── handle_hit(segment, row, col)
```

---

## BLE Interface

The device advertises as **`GranBoard`** and exposes a custom GATT primary service.

**Service UUID:** `442f1570-8a00-9a28-cbe1-e1d4212d53eb`

| Characteristic UUID | Direction | Purpose |
|---|---|---|
| `442f1571` | Board → App (Notify) | Dart hit events as ASCII `"row.col@"` (e.g., `"3.4@"`) |
| `442f1572` | App → Board (Write) | LED control commands from the phone app (logged, no LED hardware required) |
| `442f1573` | App → Board (Read) | Board capability / version info byte |

Hit payloads are ASCII strings in the format `"row.col@"` where `@` is the `0x40` terminator byte. The mapping from segment strings (e.g., `"T20"`, `"DBULL"`) to GranBoard coordinate pairs is defined in the `segment_codes[]` table in `src/ble.c`.

---

## Getting Started

### Prerequisites

- [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html) installed
- [`west`](https://docs.zephyrproject.org/latest/develop/west/index.html) meta-tool installed
- Espressif HAL module in your Zephyr workspace (`west update`)
- `ZEPHYR_BASE` environment variable pointing to your Zephyr kernel directory

### Build

```bash
west build -b esp32c6_devkitc/esp32c6/hpcore
```

### Flash

Connect the ESP32-C6 via USB, then:

```bash
west flash
```

Or use `esptool.py` directly:

```bash
esptool.py --chip esp32c6 write_flash 0x0 build/zephyr/zephyr.bin
```

### Monitor Serial Output

```bash
west espressif monitor
# or
minicom -D /dev/ttyUSB0 -b 115200
```

---

## Calibration Mode

To verify your physical wiring and segment map, enable calibration mode before building:

In `src/dartboard.h` (or as a CMake flag), set:

```c
#define CALIBRATION_MODE 1
```

In this mode, BLE initialization is skipped and every detected hit is printed over UART as:

```
PA<row> + PE<col> → <segment>
```

This lets you confirm each matrix intersection resolves to the correct dartboard segment before using the device with a scoring app.

---

## Segment Naming Convention

| Prefix | Meaning | Example |
|---|---|---|
| `S` | Single | `S20` = Single 20 |
| `D` | Double | `D20` = Double 20 (outer ring) |
| `T` | Triple | `T20` = Triple 20 (inner ring) |
| `BULL` | Outer bull | 25 points |
| `DBULL` | Double bull (bullseye) | 50 points |
| `??` | Unmapped / noisy cell | Ignored |

---

## Configuration

Key Zephyr Kconfig options in `prj.conf`:

| Option | Value | Description |
|---|---|---|
| `CONFIG_BT_DEVICE_NAME` | `"GranBoard"` | Advertised BLE device name |
| `CONFIG_BT_MAX_CONN` | `1` | Max simultaneous BLE connections |
| `CONFIG_BT_MAX_PAIRED` | `1` | Max paired devices |
| `CONFIG_MAIN_STACK_SIZE` | `8192` | Main thread stack size (bytes) |
| `CONFIG_HEAP_MEM_POOL_SIZE` | `196608` | Heap pool size (192 KB) |
| `CONFIG_LOG_DEFAULT_LEVEL` | `3` (INFO) | Logging verbosity |

---

## Compatibility

This firmware is designed to be a drop-in replacement for a GranBoard hardware device. Any app that supports GranBoard hardware over BLE should work, including:

- [gran-app](https://github.com/sobassy/gran-app) (open source, used as protocol reference)
- DaDartboard
- Other GranBoard-compatible scoring apps

The GranBoard BLE protocol was reverse-engineered from the `sobassy/gran-app` open-source project.

---

## Contributing

Contributions are welcome! Whether it's fixing a bug, improving the segment map, adding new features, or porting to a different board — feel free to open an issue or submit a pull request.

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-feature`)
3. Commit your changes (`git commit -m 'Add my feature'`)
4. Push to the branch (`git push origin feature/my-feature`)
5. Open a pull request

Please open an issue first for larger changes so the approach can be discussed before investing time in implementation.

---

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

You are free to use, modify, distribute, and build on this project, including for commercial purposes, as long as the original copyright notice is retained.

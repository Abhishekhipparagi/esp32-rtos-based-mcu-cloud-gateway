# ESP32 RTOS-Based MCU Cloud Gateway

An ESP32 firmware built on FreeRTOS that receives sensor data over UART, parses fixed-format packets, and uploads readings to a cloud endpoint over HTTPS. WiFi credentials are configured through a captive portal hosted on the ESP32 itself — no hardcoded network settings required.

## How It Works

The firmware runs five concurrent FreeRTOS tasks that communicate through queues, mutexes, semaphores, and task notifications:

```
                         ┌──────────────┐
  External MCU ──UART──► │  UART RX     │
                         │  (priority 7)│
                         └──────┬───────┘
                                │
                    ┌───────────┼────────────────┐
                    │ queue     │                 │ task notify
                    ▼           │                 ▼
             ┌──────────┐      │       ┌───────────────────┐
             │  Parser  │      │       │ Provision Monitor  │
             │ (pri 6)  │      │       │     (pri 5)        │
             └────┬─────┘      │       └────────┬──────────┘
                  │ mutex      │                │
                  ▼            │                ▼
           ┌─────────────┐    │      ┌────────────────────┐
           │ Shared Data │    │      │  AP + Web Server   │
           │ (sensor +   │    │      │  (192.168.4.1)     │
           │  alerts)    │    │      │  SSID/Pass config   │
           └──────┬──────┘    │      └────────────────────┘
                  │ semaphore  │
                  ▼            │
           ┌──────────────┐   │
           │ HTTP Upload  │   │
           │   (pri 5)    │───┘
           └──────────────┘
                  │
                  ▼
            Cloud Server
             (HTTPS GET)
```

**UART RX Task** — Reads bytes from UART1 at 38400 baud. Validates the header byte (`X`), checks for the `WEB` command, and forwards packets to the parser via a FreeRTOS queue.

**Parser Task** — Pulls packets from the queue, extracts five 3-digit sensor values and five single-bit alert flags, then writes the results into shared memory under a mutex. Signals the upload task with a binary semaphore.

**HTTP Upload Task** — Waits for new data, snapshots the shared state under the mutex, builds a URL with query parameters, and performs an HTTPS GET using the ESP-IDF HTTP client with TLS certificate bundle verification. Keeps the connection alive across requests.

**Provision Monitor Task** — Sleeps until notified by the UART RX task (triggered by the `WEB` command). Starts the ESP32 in Access Point mode and launches an HTTP server hosting a WiFi configuration page. Credentials are saved to NVS flash and persist across reboots.

## UART Packet Format

The external MCU sends 24-byte fixed-format ASCII packets:

```
Position:  [0]  [1-3]  [4-6] [7-9] [10-12] [13-15] [16-18]  [19] [20] [21] [22] [23]
Content:    X    CMD   val_a  val_b  val_c   val_d   val_e    a_a  a_b  a_c  a_d  a_e
Example:    X    WEB    042    035    078     012     099       1    0    1    1    0
```

| Field | Bytes | Description |
|-------|-------|-------------|
| Header | `[0]` | Always `X` — used for frame sync |
| Command | `[1-3]` | `WEB` = start provisioning AP; anything else = data only |
| Sensor values | `[4-18]` | Five 3-digit zero-padded integers (000–999) |
| Alert flags | `[19-23]` | Five single characters, `1` = active, `0` = inactive |

**Example packets:**
- `XWEB04203507801209910110` — Start the WiFi config portal and parse sensor data
- `XYYY04203507801209910110` — Parse sensor data only (command field ignored)

## WiFi Provisioning

When the ESP32 receives a `XWEB...` packet:

1. It starts a WiFi Access Point with SSID `ESP32_SETUP` and password `admin123`
2. A web server on `http://192.168.4.1` serves a configuration page
3. The user enters their WiFi SSID and password through the form
4. Credentials are saved to NVS flash
5. The AP shuts down and the ESP32 connects to the configured network

On subsequent boots, stored credentials are loaded automatically — no re-provisioning needed unless the `WEB` command is sent again.

## Project Structure

```
esp32-rtos-sensor-cloud-gateway/
├── main/
│   ├── esp32-rtos-sensor-cloud-gateway.c    # Application source
│   └── CMakeLists.txt                       # Component build file
├── CMakeLists.txt                           # Top-level project build file
└── sdkconfig                                # ESP-IDF configuration
```

## Building and Flashing

**Prerequisites:** ESP-IDF v5.x installed and configured.

```bash
# Set up the ESP-IDF environment
. $HOME/esp/esp-idf/export.sh

# Build
idf.py build

# Flash (replace PORT with your serial port)
idf.py -p PORT flash

# Monitor serial output
idf.py -p PORT monitor
```

## Configuration

Key parameters are defined as macros at the top of the source file:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `UART_PORT` | `UART_NUM_1` | UART peripheral |
| `UART_TX_PIN` / `UART_RX_PIN` | 17 / 16 | GPIO pins |
| `UART_BAUD` | 38400 | Baud rate |
| `AP_SSID` / `AP_PASS` | `ESP32_SETUP` / `admin123` | Provisioning AP credentials |
| `UPLOAD_URL_FMT` | `https://example.com/...` | Cloud endpoint URL template |
| `HTTP_TIMEOUT_MS` | 10000 | HTTP request timeout |

Update `UPLOAD_URL_FMT` with your actual cloud endpoint before building.

## FreeRTOS Resources

| Resource | Type | Purpose |
|----------|------|---------|
| `g_data_mutex` | Mutex | Protects shared sensor/alert data |
| `g_new_data_sem` | Binary semaphore | Signals new data available for upload |
| `g_uart_queue` | Queue (depth 5) | Passes raw UART packets to the parser |
| `g_event_group` | Event group | Tracks WiFi connected / AP running state |
| `g_provision_task` | Task notification | Triggers AP provisioning on `WEB` command |

## Dependencies

- ESP-IDF v5.x
- FreeRTOS (included with ESP-IDF)
- ESP-IDF components: `esp_wifi`, `esp_http_server`, `esp_http_client`, `esp_crt_bundle`, `nvs_flash`, `driver` (UART)

## License

This project is licensed under the [MIT License].
